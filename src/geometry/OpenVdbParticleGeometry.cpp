#include "Container/geometry/OpenVdbParticleGeometry.h"
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <numeric>
#include <openvdb/tools/ParticlesToLevelSet.h>
#include <sstream>
#include <streambuf>
#include <tbb/task_arena.h>
#include <tuple>
#include <type_traits>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error(
      "OpenVDB particle geometry is unavailable; configure "
      "with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using V = openvdb::Vec3d;
constexpr uint64_t Fixed = 4ull * 1024 * 1024;
constexpr uint64_t MaxBytes = 16ull << 30;
void require(bool condition, const char *message) {
  if (!condition)
    throw std::invalid_argument(message);
}
uint64_t add(uint64_t a, uint64_t b) {
  require(b <= UINT64_MAX - a, "particle geometry UInt64 addition overflow");
  return a + b;
}
uint64_t mul(uint64_t a, uint64_t b) {
  require(!a || b <= UINT64_MAX / a,
          "particle geometry UInt64 product overflow");
  return a * b;
}
void bytes(uint64_t n, const ProcessingOptions &p) {
  require(n <= p.maxInputBytes,
          "particle geometry joint workspace exceeds maxInputBytes");
}
void work(uint64_t n, const ParticleGeometryOptions &o) {
  require(n <= o.maxWork,
          "particle geometry work exceeds maxWork before native allocation");
}
bool sdf(const ParticleGeometryOptions &o) {
  return o.output != ParticleGeometryOutput::Mask;
}
bool mask(const ParticleGeometryOptions &o) {
  return o.output != ParticleGeometryOutput::LevelSet;
}
bool finite(const std::array<double, 3> &v) {
  return std::all_of(v.begin(), v.end(),
                     [](double x) { return std::isfinite(x); });
}
void name(const std::string &value) {
  require(!value.empty() && value.size() <= 256 &&
              value.find('\0') == std::string::npos,
          "particle geometry names require1..256 native bytes without NUL");
}
void validate(const ParticleGeometryOptions &o, const ProcessingOptions &p) {
  require(o.output == ParticleGeometryOutput::LevelSet ||
              o.output == ParticleGeometryOutput::Mask ||
              o.output == ParticleGeometryOutput::Both,
          "unknown particle geometry output mode");
  require(std::isfinite(p.voxelSize) && p.voxelSize >= 1e-6 &&
              p.voxelSize <= 1e6,
          "particle geometry voxelSize requires[1e-6,1e6] WORLD units");
  require(!sdf(o) || (std::isfinite(p.halfWidth) && p.halfWidth >= 3 &&
                      p.halfWidth <= 64),
          "particle geometry SDF halfWidth requires[3,64] voxels");
  require(std::isfinite(o.timeStep) && std::isfinite(o.spacing) &&
              o.spacing >= .25 && o.spacing <= 4,
          "particle geometry time must be finite; spacing requires[0.25,4]");
  require(std::isfinite(o.minRadiusVoxels) &&
              std::isfinite(o.maxRadiusVoxels) && o.minRadiusVoxels >= 1.5 &&
              o.maxRadiusVoxels >= o.minRadiusVoxels &&
              o.maxRadiusVoxels <= 1024,
          "particle geometry radius policy requires1.5<=min<=max<=1024 voxels");
  require(!o.originWorld || finite(*o.originWorld),
          "particle geometry origin must be finite");
  require(o.maxParticles && o.maxParticles < UINT32_MAX &&
              o.maxTrailInstances && o.maxTrailInstances <= 16'000'000 &&
              o.maxNodes && o.maxNodes <= 1'000'000 && o.maxWork &&
              o.maxWork <= 10'000'000'000ull && p.maxVoxelCount &&
              p.maxInputBytes && p.maxInputBytes <= MaxBytes &&
              o.maxOutputBytes && o.maxOutputBytes <= MaxBytes &&
              o.maxMetadataBytes && o.maxMetadataBytes <= (16ull << 20),
          "invalid particle geometry count/node/work/byte/metadata limits");
  require(std::isfinite(o.maxTrailLengthVoxels) && o.maxTrailLengthVoxels > 0 &&
              o.maxTrailLengthVoxels <= 1e8,
          "particle geometry maximum trail length requires(0,1e8] voxels");
  if (sdf(o))
    name(p.gridName);
  if (mask(o))
    name(o.maskGridName);
  require(o.output != ParticleGeometryOutput::Both ||
              p.gridName != o.maskGridName,
          "particle geometry SDF and mask names must differ");
}
void outputPath(const std::filesystem::path &output) {
  require(!output.empty() && !std::filesystem::exists(output),
          "particle geometry output must be new");
  require(std::filesystem::is_directory(
              output.has_parent_path() ? output.parent_path() : "."),
          "particle geometry output parent must exist");
}
double spacing(double x) {
  return std::max(std::nextafter(x, INFINITY) - x,
                  x - std::nextafter(x, -INFINITY));
}
double product(double a, double b) {
  const double result = a * b;
  require(std::isfinite(result) && (a == 0 || b == 0 || result != 0),
          "particle geometry displacement multiplication overflows or loses a "
          "nonzero component");
  return result;
}
double difference(double a, double b, double h, double &error) {
  const double d = a - b;
  require(std::isfinite(d), "particle geometry relative coordinate overflows");
  const double z = d - a;
  const double residual = (a - (d - z)) + (-b - z);
  error = std::max(error, std::abs(residual / h));
  return d;
}
double quotient(double a, double h, double &error) {
  const double q = a / h;
  require(std::isfinite(q) && (a == 0 || q != 0),
          "particle geometry division overflows or loses a nonzero component");
  error = std::max(error, std::abs(std::fma(-q, h, a) / h));
  return q;
}
struct Prepared {
  V position, displacement;
  double radius{}, speed{};
  uint64_t instances{}, upper{};
};
struct Box {
  std::array<int64_t, 3> lo{INT64_MAX, INT64_MAX, INT64_MAX},
      hi{INT64_MIN, INT64_MIN, INT64_MIN};
  void include(const V &p, const V &tail, double padding) {
    for (int axis = 0; axis < 3; ++axis) {
      const double low = std::floor(std::min(p[axis], tail[axis]) - padding);
      const double high = std::ceil(std::max(p[axis], tail[axis]) + padding);
      require(std::isfinite(low) && std::isfinite(high) &&
                  low >= double(INT32_MIN) + 8 && high <= double(INT32_MAX) - 8,
              "particle geometry swept bounds exceed safe Int32 native "
              "coordinates");
      lo[axis] = std::min(lo[axis], int64_t(low));
      hi[axis] = std::max(hi[axis], int64_t(high));
    }
  }
  uint64_t count(int64_t block = 1) const {
    if (lo[0] == INT64_MAX)
      return 0;
    const auto floor = [=](int64_t n) {
      return n >= 0 ? n / block : -((-n + block - 1) / block);
    };
    uint64_t n = 1;
    for (int axis = 0; axis < 3; ++axis)
      n = mul(n, uint64_t(floor(hi[axis]) - floor(lo[axis]) + 1));
    return n;
  }
};
struct Shape {
  uint64_t leaf{}, lower{}, upper{};
  uint64_t nodes() const { return add(1, add(upper, add(leaf, lower))); }
  uint64_t slots() const {
    return add(upper,
               add(mul(leaf, 512), add(mul(lower, 4096), mul(upper, 32768))));
  }
};
template <class Tree> uint64_t treeBytes(const Shape &s) {
  using Leaf = typename Tree::LeafNodeType;
  using Upper = typename Tree::RootNodeType::ChildNodeType;
  using Lower = typename Upper::ChildNodeType;
  return add(
      8192,
      add(mul(s.upper, 256),
          add(mul(s.leaf,
                  sizeof(Leaf) + 512 * sizeof(typename Tree::ValueType) + 512),
              add(mul(s.lower, sizeof(Lower) +
                                   Lower::NUM_VALUES *
                                       sizeof(typename Lower::UnionType) +
                                   1024),
                  mul(s.upper, sizeof(Upper) +
                                   Upper::NUM_VALUES *
                                       sizeof(typename Upper::UnionType) +
                                   8192)))));
}
struct Stamp {
  V position;
  double radius{};
};
template <class Tree> Shape actualShape(const Tree &tree) {
  const auto n = tree.nodeCount();
  return {n[0], n[1], n[2]};
}
uint64_t trace(const Prepared &record, const ParticleGeometryOptions &o,
               std::vector<Stamp> *stamps) {
  if (!record.speed) {
    if (stamps)
      stamps->push_back({record.position, record.radius});
    return 1;
  }
  const double inverse = 1 / record.speed;
  require(std::isfinite(inverse),
          "particle geometry native inverse trail speed overflows");
  const V direction = -record.displacement * inverse;
  for (int axis = 0; axis < 3; ++axis)
    require(record.displacement[axis] == 0 || direction[axis] != 0,
            "particle geometry native normalized trail loses a weak component");
  V p = record.position;
  double radius = record.radius, d = 0;
  uint64_t n = 0;
  while (d <= record.speed) {
    require(++n <= record.upper,
            "particle geometry recurrence exceeds its finite instance bound");
    require(std::isfinite(radius) &&
                radius >= o.minRadiusVoxels * (1 - 1e-12) &&
                radius <= record.radius * (1 + 1e-12),
            "particle geometry native taper radius loses range");
    if (stamps)
      stamps->push_back({p, radius});
    const V step = .5 * o.spacing * radius * direction;
    const V next = p + step;
    for (int axis = 0; axis < 3; ++axis) {
      require(std::isfinite(next[axis]) &&
                  (direction[axis] == 0 || step[axis] != 0),
              "particle geometry trail step overflows or underflows a weak "
              "component");
      require(step[axis] == 0 || next[axis] != p[axis],
              "particle geometry trail update loses a weak component");
    }
    const double nextDistance = (next - record.position).length();
    require(
        std::isfinite(nextDistance) && nextDistance > d,
        "particle geometry native recurrence does not make finite progress");
    p = next;
    d = nextDistance;
    radius = record.radius - (record.radius - o.minRadiusVoxels) * d * inverse;
    require(std::isfinite(radius),
            "particle geometry native taper arithmetic overflows");
  }
  return n;
}
struct Plan {
  std::vector<Prepared> records;
  std::vector<size_t> spheres, trails;
  std::vector<Stamp> stamps;
  std::vector<openvdb::Coord> centerCandidates;
  ParticleGeometryResult result;
  Shape shape;
  uint64_t metadata{}, encoded{};
};
Plan plan(std::span<const GeometryParticle> particles,
          const ParticleGeometryOptions &o, const ProcessingOptions &p) {
  require(
      particles.size() <= o.maxParticles && particles.size() < UINT32_MAX,
      "particle geometry particle count exceeds maxParticles/native UInt32");
  const uint64_t count = particles.size();
  work(mul(count, 2048), o);
  uint64_t arrays =
      mul(count, 3 * sizeof(GeometryParticle) + 3 * sizeof(Prepared) + 64);
  const uint64_t meta =
      add(16384, mul(32, add(sdf(o) ? p.gridName.size() : 0,
                             mask(o) ? o.maskGridName.size() : 0)));
  require(meta <= o.maxMetadataBytes,
          "particle geometry producer/name metadata exceeds cap");
  bytes(add(Fixed, add(arrays, mul(meta, 16))), p);
  std::array<double, 3> low{INFINITY, INFINITY, INFINITY},
      high{-INFINITY, -INFINITY, -INFINITY};
  double error = 0;
  for (const auto &particle : particles) {
    require(finite(particle.position) && finite(particle.velocity) &&
                std::isfinite(particle.radius) && particle.radius > 0,
            "particle geometry positions/velocities must be finite and radius "
            "positive finite");
    for (int axis = 0; axis < 3; ++axis) {
      require(spacing(particle.position[axis]) <= p.voxelSize * 1e-4,
              "particle geometry WORLD position loses1e-4 voxel precision");
      const double displacement = product(particle.velocity[axis], o.timeStep);
      const double tail = particle.position[axis] - displacement;
      require(std::isfinite(tail) &&
                  (displacement == 0 || tail != particle.position[axis]),
              "particle geometry WORLD endpoint overflows or loses a weak "
              "displacement component");
      require(spacing(tail) <= p.voxelSize * 1e-4,
              "particle geometry WORLD endpoint loses1e-4 voxel precision");
      error = std::max(error, std::abs(std::fma(particle.velocity[axis],
                                                o.timeStep, -displacement) /
                                       p.voxelSize));
      low[axis] = std::min({low[axis], particle.position[axis], tail});
      high[axis] = std::max({high[axis], particle.position[axis], tail});
    }
  }
  Plan a;
  a.metadata = meta;
  a.result.particleCount = count;
  for (int axis = 0; axis < 3; ++axis) {
    a.result.originWorld[axis] =
        o.originWorld ? (*o.originWorld)[axis]
                      : (count ? std::midpoint(low[axis], high[axis]) : 0.);
    require(spacing(a.result.originWorld[axis]) <= p.voxelSize * 1e-4,
            "particle geometry lattice origin loses1e-4 voxel precision");
  }
  a.records.reserve(size_t(count));
  a.spheres.reserve(size_t(count));
  a.trails.reserve(size_t(count));
  Box bounds;
  uint64_t sphereWork = 0, preliminary = mul(count, 2048);
  for (const auto &particle : particles) {
    Prepared r;
    r.radius = quotient(particle.radius, p.voxelSize, error);
    require(r.radius >= o.minRadiusVoxels && r.radius <= o.maxRadiusVoxels,
            "particle geometry head radius lies outside explicit native "
            "cutoffs; no particle was ignored");
    for (int axis = 0; axis < 3; ++axis) {
      r.position[axis] =
          quotient(difference(particle.position[axis],
                              a.result.originWorld[axis], p.voxelSize, error),
                   p.voxelSize, error);
      r.displacement[axis] = quotient(
          product(particle.velocity[axis], o.timeStep), p.voxelSize, error);
      error = std::max(error, std::abs(std::fma(r.position[axis], p.voxelSize,
                                                a.result.originWorld[axis]) -
                                       particle.position[axis]) /
                                  p.voxelSize);
      require(spacing(r.position[axis]) <= 1e-4,
              "particle geometry local position loses1e-4 voxel precision");
    }
    const bool moving = r.displacement[0] != 0 || r.displacement[1] != 0 ||
                        r.displacement[2] != 0;
    r.speed = r.displacement.length();
    require(std::isfinite(r.speed) && (!moving || r.speed > 0) &&
                r.speed <= o.maxTrailLengthVoxels,
            "particle geometry native trail length overflows/underflows or "
            "exceeds maxTrailLengthVoxels");
    r.upper = moving ? uint64_t(std::ceil(4 * r.speed /
                                          (o.spacing * o.minRadiusVoxels))) +
                           4
                     : 1;
    a.result.instanceUpperBound = add(a.result.instanceUpperBound, r.upper);
    require(a.result.instanceUpperBound <= o.maxTrailInstances,
            "particle geometry conservative trail instances exceed cap BEFORE "
            "recurrence simulation");
    const double side =
        2 * std::ceil(r.radius + (sdf(o) ? p.halfWidth : 0)) + 3;
    const uint64_t loop =
        mul(mul(uint64_t(side), uint64_t(side)), uint64_t(side));
    sphereWork = add(
        sphereWork,
        mul(r.upper,
            mul(loop, o.output == ParticleGeometryOutput::Both ? 128 : 64)));
    preliminary = add(mul(count, 2048),
                      add(sphereWork, mul(a.result.instanceUpperBound, 256)));
    work(preliminary, o);
    const V tail = r.position - r.displacement;
    for (int axis = 0; axis < 3; ++axis)
      require(r.displacement[axis] == 0 || tail[axis] != r.position[axis],
              "particle geometry local endpoint loses a weak component");
    bounds.include(r.position, tail, r.radius + (sdf(o) ? p.halfWidth : 0) + 2);
    const auto index = a.records.size();
    a.records.push_back(r);
    (moving ? a.trails : a.spheres).push_back(index);
    a.result.maximumTrailLengthWorld = std::max(
        a.result.maximumTrailLengthWorld, product(r.speed, p.voxelSize));
  }
  require(std::isfinite(error) && error <= 1e-4,
          "particle geometry displacement/relative map arithmetic exceeds1e-4 "
          "voxel precision");
  a.result.maximumPositionErrorVoxels = error;
  a.result.candidateVoxelUpperBound = bounds.count();
  a.shape = {bounds.count(8), bounds.count(128), bounds.count(4096)};
  const uint64_t liveTrees = sdf(o) ? (mask(o) ? 3 : 2) : 1;
  a.result.storedLeafVoxelUpperBound = mul(mul(a.shape.leaf, 512), liveTrees);
  a.result.nodeUpperBound = mul(a.shape.nodes(), liveTrees);
  require(a.result.candidateVoxelUpperBound <= p.maxVoxelCount &&
              a.result.storedLeafVoxelUpperBound <= p.maxVoxelCount &&
              a.result.nodeUpperBound <= o.maxNodes,
          "particle geometry conservative combined swept box/native trees "
          "exceed voxel/leaf/node caps BEFORE allocation");
  const bool repair = sdf(o) && o.minRadiusVoxels < p.halfWidth;
  if (repair) {
    // Complete stamp vector, candidate Coord vector and sorting/allocator
    // slack.
    arrays = add(arrays, mul(a.result.instanceUpperBound, 192));
    preliminary =
        add(preliminary, mul(mul(a.result.instanceUpperBound,
                                 std::bit_width(a.result.instanceUpperBound)),
                             128));
  }
  uint64_t native = 0, outputs = 0;
  if (sdf(o)) {
    native = add(native, treeBytes<openvdb::DoubleTree>(a.shape));
    outputs = add(outputs, treeBytes<openvdb::FloatTree>(a.shape));
  }
  if (mask(o))
    outputs = add(outputs, treeBytes<openvdb::MaskTree>(a.shape));
  native = add(native, outputs);
  a.encoded = add(Fixed, add(mul(outputs, 2), mul(meta, 8)));
  require(
      a.encoded <= o.maxOutputBytes,
      "particle geometry conservative encoded output exceeds maxOutputBytes");
  a.result.workspaceUpperBound = add(
      Fixed, add(arrays, add(mul(native, 4), add(mul(meta, 16), a.encoded))));
  bytes(a.result.workspaceUpperBound, p);
  a.result.workUpperBound =
      add(preliminary,
          add(mul(mul(a.shape.slots(), liveTrees), 256),
              add(mul(a.result.workspaceUpperBound, 2), mul(a.encoded, 2))));
  work(a.result.workUpperBound, o);
  if (repair)
    a.stamps.reserve(size_t(a.result.instanceUpperBound));
  for (auto &r : a.records) {
    r.instances = trace(r, o, repair ? &a.stamps : nullptr);
    a.result.sphereInstances = add(a.result.sphereInstances, r.instances);
  }
  a.result.sphereParticleCount = a.spheres.size();
  a.result.trailParticleCount = a.trails.size();
  auto &candidates = a.centerCandidates;
  candidates.reserve(a.stamps.size());
  for (const auto &stamp : a.stamps) {
    if (stamp.radius >= p.halfWidth)
      continue;
    openvdb::Coord q;
    for (int axis = 0; axis < 3; ++axis) {
      const auto lower = std::floor(stamp.position[axis]);
      q[axis] = int32_t(lower + (stamp.position[axis] - lower >= .5 ? 1 : 0));
    }
    if ((q.asVec3d() - stamp.position).lengthSqr() == 0)
      candidates.push_back(q);
  }
  const auto lex = [](const openvdb::Coord &a, const openvdb::Coord &b) {
    return std::tuple(a.x(), a.y(), a.z()) < std::tuple(b.x(), b.y(), b.z());
  };
  std::sort(candidates.begin(), candidates.end(), lex);
  candidates.erase(std::unique(candidates.begin(), candidates.end()),
                   candidates.end());
  a.result.centerCandidateCount = candidates.size();
  a.result.centerRepairPairUpperBound = mul(candidates.size(), a.stamps.size());
  a.result.workUpperBound = add(a.result.workUpperBound,
                                mul(a.result.centerRepairPairUpperBound, 64));
  require(a.result.workUpperBound <= o.maxWork,
          "particle geometry deduplicated center*stamp repair work exceeds "
          "maxWork BEFORE native allocation; reduce particles/trails or raise "
          "the bounded work cap");
  return a;
}
void repairCenters(openvdb::DoubleGrid &grid, Plan &a) {
  const double band = grid.background();
  auto accessor = grid.getAccessor();
  for (const auto &q : a.centerCandidates) {
    double value = band;
    for (const auto &stamp : a.stamps) {
      value = std::min(value,
                       (q.asVec3d() - stamp.position).length() - stamp.radius);
      if (value <= -band) {
        value = -band;
        break;
      }
    }
    if (value != accessor.getValue(q)) {
      if (value > -band && value < band)
        accessor.setValue(q, value);
      else
        accessor.setValueOff(q, value);
      ++a.result.repairedNativeCenterValues;
    }
  }
  const auto shape = actualShape(grid.tree());
  require(shape.leaf <= a.shape.leaf && shape.lower <= a.shape.lower &&
              shape.upper <= a.shape.upper,
          "particle geometry repaired center topology exceeds preflight");
}
struct List {
  using PosType = V;
  const std::vector<Prepared> &records;
  const std::vector<size_t> &indices;
  size_t size() const { return indices.size(); }
  void getPos(size_t n, V &p) const { p = records[indices[n]].position; }
  void getPosRad(size_t n, V &p, double &r) const {
    const auto &v = records[indices[n]];
    p = v.position;
    r = v.radius;
  }
  void getPosRadVel(size_t n, V &p, double &r, V &v) const {
    getPosRad(n, p, r);
    v = records[indices[n]].displacement;
  }
};
template <class Grid>
void rasterize(Grid &grid, const Plan &a, const ParticleGeometryOptions &o) {
  openvdb::tools::ParticlesToLevelSet<Grid> raster(grid);
  raster.setGrainSize(0);
  raster.setRmin(o.minRadiusVoxels);
  raster.setRmax(o.maxRadiusVoxels);
  if (!a.spheres.empty()) {
    raster.rasterizeSpheres(List{a.records, a.spheres});
    require(
        !raster.ignoredParticles(),
        "native particle sphere unexpectedly ignored explicit radius policy");
  }
  if (!a.trails.empty()) {
    raster.rasterizeTrails(List{a.records, a.trails}, o.spacing);
    require(
        !raster.ignoredParticles(),
        "native particle trail unexpectedly ignored explicit radius policy");
  }
  raster.finalize(true);
  const auto shape = actualShape(grid.tree());
  require(shape.leaf <= a.shape.leaf && shape.lower <= a.shape.lower &&
              shape.upper <= a.shape.upper,
          "native particle geometry topology exceeds its preflight");
}
openvdb::math::Transform::Ptr transform(double h,
                                        const std::array<double, 3> &origin) {
  std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
  stream.exceptions(std::ios::badbit | std::ios::failbit);
  openvdb::writeString(stream,
                       openvdb::math::UniformScaleTranslateMap::mapType());
  V(origin[0], origin[1], origin[2]).write(stream);
  for (double value : {h, h, 1 / h, 1 / (h * h), .5 / h})
    V(value).write(stream);
  stream.seekg(0);
  openvdb::io::setCurrentVersion(stream);
  auto result = std::make_shared<openvdb::math::Transform>();
  result->read(stream);
  return result;
}
template <class Grid> class ExactGrid final : public Grid {
public:
  explicit ExactGrid(typename Grid::TreePtrType tree) : Grid(tree) {}
  openvdb::GridBase::ConstPtr copyGrid() const override {
    return this->copyReplacingMetadataAndTransform(
        *this, detail::CopyTransformExact(this->transform()));
  }
  openvdb::GridBase::Ptr copyGrid() override {
    return openvdb::ConstPtrCast<openvdb::GridBase>(
        static_cast<const ExactGrid &>(*this).copyGrid());
  }
};
void metadata(openvdb::GridBase &g, const ParticleGeometryOptions &o,
              const ParticleGeometryResult &r) {
  g.insertMeta("container:particle_geometry_version",
               openvdb::Int32Metadata(1));
  g.insertMeta("container:particle_count",
               openvdb::Int64Metadata(int64_t(r.particleCount)));
  g.insertMeta("container:particle_stamp_count",
               openvdb::Int64Metadata(int64_t(r.sphereInstances)));
  g.insertMeta("container:particle_repaired_centers",
               openvdb::Int64Metadata(int64_t(r.repairedNativeCenterValues)));
  g.insertMeta("container:particle_time_step",
               openvdb::DoubleMetadata(o.timeStep));
  g.insertMeta("container:particle_spacing",
               openvdb::DoubleMetadata(o.spacing));
  g.insertMeta("container:particle_min_radius_voxels",
               openvdb::DoubleMetadata(o.minRadiusVoxels));
  g.insertMeta("container:particle_max_radius_voxels",
               openvdb::DoubleMetadata(o.maxRadiusVoxels));
  g.insertMeta(
      "container:particle_geometry_method",
      openvdb::StringMetadata(
          "native sampled tapered sphere union; not a continuous capsule"));
  g.insertMeta("container:particle_origin",
               openvdb::Vec3DMetadata(
                   V(r.originWorld[0], r.originWorld[1], r.originWorld[2])));
  g.insertMeta("container:particle_mask_policy",
               openvdb::StringMetadata("voxel centers strictly inside sampled "
                                       "spheres; no cell-intersection claim"));
  g.setIsInWorldSpace(true);
  g.setVectorType(openvdb::VEC_INVARIANT);
  g.setSaveFloatAsHalf(false);
}
float distance(double value, ParticleGeometryResult &result) {
  require(std::isfinite(value) &&
              std::abs(value) <= std::numeric_limits<float>::max(),
          "particle geometry WORLD distance exceeds Float range");
  const float f = float(value);
  require(value == 0 || f != 0,
          "particle geometry nonzero WORLD distance underflows Float storage");
  if (double(f) != value) {
    ++result.roundedDistanceValues;
    result.maximumDistanceAbsoluteError = std::max(
        result.maximumDistanceAbsoluteError, std::abs(double(f) - value));
  }
  return f;
}
ParticleGeometryResult execute(std::span<const GeometryParticle> input,
                               const std::filesystem::path &output,
                               const ParticleGeometryOptions &o,
                               const ProcessingOptions &p) {
  auto a = plan(input, o, p);
  auto &result = a.result;
  openvdb::GridPtrVec grids;
  openvdb::DoubleGrid::Ptr nativeSdf;
  if (sdf(o)) {
    nativeSdf = openvdb::DoubleGrid::create(p.halfWidth);
    nativeSdf->setGridClass(openvdb::GRID_LEVEL_SET);
    rasterize(*nativeSdf, a, o);
    repairCenters(*nativeSdf, a);
    const float background =
        distance(product(nativeSdf->background(), p.voxelSize), result);
    auto tree = std::make_shared<openvdb::FloatTree>(
        nativeSdf->tree(), background, openvdb::TopologyCopy());
    auto source = nativeSdf->tree().cbeginValueAll();
    auto target = tree->beginValueAll();
    for (; source && target; ++source, ++target) {
      require(source.getCoord() == target.getCoord() &&
                  source.getLevel() == target.getLevel(),
              "particle geometry Float conversion topology mismatch");
      target.setValue(distance(product(*source, p.voxelSize), result));
    }
    require(!source && !target,
            "particle geometry Float conversion iterator mismatch");
    auto grid = std::make_shared<ExactGrid<openvdb::FloatGrid>>(tree);
    grid->setTransform(transform(p.voxelSize, result.originWorld));
    grid->setName(p.gridName);
    grid->setGridClass(openvdb::GRID_LEVEL_SET);
    metadata(*grid, o, result);
    result.effectiveExteriorBand = grid->background();
    result.levelSet =
        ProcessingResult{p.gridName, "level set",
                         uint64_t(grid->activeVoxelCount()), p.voxelSize};
    grids.push_back(grid);
  }
  if (mask(o)) {
    auto nativeMask = openvdb::MaskGrid::create(false);
    rasterize(*nativeMask, a, o);
    auto grid =
        std::make_shared<ExactGrid<openvdb::MaskGrid>>(nativeMask->treePtr());
    grid->setTransform(transform(p.voxelSize, result.originWorld));
    grid->setName(o.maskGridName);
    grid->setGridClass(openvdb::GRID_UNKNOWN);
    metadata(*grid, o, result);
    result.mask =
        ProcessingResult{o.maskGridName, "unknown",
                         uint64_t(grid->activeVoxelCount()), p.voxelSize};
    grids.push_back(grid);
  }
  result.diagnostics = {
      "Native sampled sphere unions taper opposite velocity*timeStep. Last "
      "sample need not reach the requested endpoint; no continuous capsule, "
      "conserved mass or ODE trajectory is claimed.",
      "Mask activity tests voxel CENTERS strictly inside sampled spheres; SDF "
      "narrow-band activity is different. All selected output trees share "
      "preflight budgets.",
      "The reported origin and diagonal voxel size define the output lattice. "
      "Unit Double native geometry precedes audited Float WORLD distances; "
      "native approximation remains.",
      "Source ordinal IDs and attribute transfer remain a separate variant: "
      "native blind comparisons/pruning and mask overwrite ownership do not "
      "provide a universal tie contract.",
      "Native zero-squared-distance center saturation for radius<band is "
      "repaired against the complete sampled stamp union; exact deduplicated "
      "center*stamp work and changed values are reported.",
      "Logical joint encoded/native/partition/conversion/writer limits are "
      "conservative, including the complete combined swept box, and are not an "
      "RSS sandbox."};
  detail::WriteFileExclusive(output, grids, o.maxOutputBytes);
  static_assert(std::is_nothrow_move_constructible_v<ParticleGeometryResult>);
  return std::move(a.result);
}

struct Snapshot {
  std::filesystem::path directory, file;
  uint64_t encoded{};
  Snapshot(const std::filesystem::path &input, const ProcessingOptions &p,
           const ParticleGeometryOptions &o) {
    require(std::filesystem::is_regular_file(input),
            "particle geometry JSON input must be a regular file");
    encoded = std::filesystem::file_size(input);
    // Reserve the entire encoded copy + lexical pass before either begins.
    bytes(add(Fixed, mul(encoded, 3)), p);
    work(mul(encoded, 8), o);
    try {
      directory = detail::CreateAsciiVdbTemporaryDirectory();
      file = directory / "particles.json";
      std::ifstream source(input, std::ios::binary);
      std::ofstream target(file, std::ios::binary | std::ios::out |
                                     std::ios::noreplace);
      require(bool(source) && bool(target),
              "cannot open immutable particle geometry JSON snapshot");
      std::array<char, 65536> buffer{};
      uint64_t left = encoded;
      while (left) {
        const auto n = std::streamsize(std::min<uint64_t>(left, buffer.size()));
        source.read(buffer.data(), n);
        require(source.gcount() == n,
                "particle geometry JSON changed length during snapshot");
        target.write(buffer.data(), n);
        left -= uint64_t(n);
      }
      target.close();
      require(
          bool(target) && !source.bad() &&
              source.peek() == std::char_traits<char>::eof(),
          "particle geometry JSON snapshot failed or source changed length");
    } catch (...) {
      cleanup();
      throw;
    }
  }
  void cleanup() noexcept {
    std::error_code e;
    std::filesystem::remove(file, e);
    std::filesystem::remove(directory, e);
  }
  ~Snapshot() { cleanup(); }
};
class TokenStream : public std::streambuf {
public:
  TokenStream(std::ifstream &input, uint64_t size)
      : input_(input), left_(size) {
    setg(buffer_, buffer_, buffer_);
  }
  bool exceeded() const { return exceeded_; }

protected:
  int_type underflow() override {
    if (gptr() < egptr())
      return traits_type::to_int_type(*gptr());
    if (!left_)
      return traits_type::eof();
    input_.read(buffer_,
                std::streamsize(std::min<uint64_t>(left_, sizeof(buffer_))));
    const auto n = input_.gcount();
    left_ -= uint64_t(n);
    if (!n)
      return traits_type::eof();
    for (std::streamsize i = 0; i < n; ++i) {
      const char c = buffer_[i];
      if (quoted_) {
        ++token_;
        if (!escaped_ && c == '"')
          quoted_ = false;
        if (!escaped_ && c == '\\')
          escaped_ = true;
        else
          escaped_ = false;
      } else if (c == '"') {
        quoted_ = true;
        escaped_ = false;
        token_ = 1;
      } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '[' ||
                 c == ']' || c == '{' || c == '}' || c == ':' || c == ',')
        token_ = 0;
      else
        ++token_;
      if (token_ > 128) {
        exceeded_ = true;
        return traits_type::eof();
      }
    }
    setg(buffer_, buffer_, buffer_ + n);
    return traits_type::to_int_type(*gptr());
  }

private:
  std::ifstream &input_;
  uint64_t left_{};
  size_t token_{};
  bool quoted_{}, escaped_{}, exceeded_{};
  char buffer_[4096]{};
};
bool exactDoubleInteger(uint64_t n) {
  const auto bits = std::bit_width(n);
  return bits <= 53 || (n & ((uint64_t(1) << (bits - 53)) - 1)) == 0;
}
struct JsonParticles : nlohmann::json_sax<nlohmann::json> {
  enum class State {
    Start,
    Root,
    Array,
    Particle,
    Coordinates,
    Done
  } state{State::Start};
  enum class Field {
    None,
    Version,
    Particles,
    Position,
    Radius,
    Velocity
  } field{},
      vectorField{};
  const ProcessingOptions &processing;
  const ParticleGeometryOptions &options;
  uint64_t encoded{};
  std::vector<GeometryParticle> particles;
  GeometryParticle current;
  unsigned rootSeen{}, particleSeen{}, coordinate{};
  bool versionComplete{}, particlesComplete{};
  std::string error;
  JsonParticles(const ProcessingOptions &p, const ParticleGeometryOptions &o,
                uint64_t bytes)
      : processing(p), options(o), encoded(bytes) {}
  bool fail(const char *message) {
    error = message;
    return false;
  }
  bool null() override {
    return fail("particle geometry JSON null is unsupported");
  }
  bool boolean(bool) override {
    return fail("particle geometry JSON expects numeric geometry, not Boolean");
  }
  bool string(string_t &) override {
    return fail("particle geometry JSON string values are unsupported");
  }
  bool binary(binary_t &) override {
    return fail("particle geometry JSON binary values are unsupported");
  }
  bool number_integer(number_integer_t n) override {
    if (state == State::Root && field == Field::Version)
      return version(n == 1);
    const auto magnitude = n < 0 ? uint64_t(-(n + 1)) + 1 : uint64_t(n);
    if (!exactDoubleInteger(magnitude))
      return fail(
          "particle geometry integer cannot be represented exactly as Double");
    return number(double(n));
  }
  bool number_unsigned(number_unsigned_t n) override {
    if (state == State::Root && field == Field::Version)
      return version(n == 1);
    if (!exactDoubleInteger(n))
      return fail(
          "particle geometry integer cannot be represented exactly as Double");
    return number(double(n));
  }
  bool number_float(number_float_t n, const string_t &token) override {
    if (token.find_first_of(".eE") == string_t::npos)
      return fail("particle geometry JSON integer literal exceeds "
                  "Int64/UInt64; exact conversion cannot be guaranteed");
    if (n == 0) {
      // strtod-like lexical conversion can silently underflow a nonzero
      // velocity to zero before displacement validation can see it.
      const auto exponent = token.find_first_of("eE");
      const auto end = exponent == string_t::npos ? token.size() : exponent;
      for (size_t i = 0; i < end; ++i)
        if (token[i] >= '1' && token[i] <= '9')
          return fail(
              "particle geometry JSON nonzero number underflows Double");
    }
    return number(n);
  }
  bool version(bool valid) {
    if (!valid)
      return fail("particle geometry JSON version must be integer1");
    versionComplete = true;
    field = Field::None;
    return true;
  }
  bool number(double n) {
    if (!std::isfinite(n))
      return fail("particle geometry JSON numbers must be finite Double");
    if (state == State::Coordinates && coordinate < 3) {
      (vectorField == Field::Position ? current.position
                                      : current.velocity)[coordinate++] = n;
      return true;
    }
    if (state == State::Particle && field == Field::Radius) {
      current.radius = n;
      field = Field::None;
      return true;
    }
    return fail("particle geometry JSON expects integer version, radius or "
                "exactly3 coordinates");
  }
  bool start_object(std::size_t) override {
    if (state == State::Start) {
      state = State::Root;
      return true;
    }
    if (state != State::Array)
      return fail("particle geometry JSON has unexpected nested object");
    if (particles.size() >= options.maxParticles ||
        particles.size() + 1 >= UINT32_MAX)
      return fail("particle geometry JSON exceeds maxParticles/native UInt32");
    if (add(mul(encoded, 8), mul(particles.size() + 1, 512)) > options.maxWork)
      return fail("particle geometry JSON parsing exceeds shared work cap "
                  "before record allocation");
    if (particles.size() == particles.capacity()) {
      const uint64_t grown = std::min<uint64_t>(
          options.maxParticles,
          std::max<uint64_t>(8, mul(particles.capacity(), 2)));
      const uint64_t reserve =
          add(Fixed, add(mul(encoded, 3),
                         mul(mul(grown, sizeof(GeometryParticle)), 3)));
      if (reserve > processing.maxInputBytes)
        return fail("particle geometry decoded JSON vector exceeds joint byte "
                    "cap before growth");
      particles.reserve(size_t(grown));
      bytes(
          add(Fixed,
              add(mul(encoded, 3),
                  mul(mul(particles.capacity(), sizeof(GeometryParticle)), 3))),
          processing);
    }
    current = {};
    particleSeen = 0;
    field = Field::None;
    state = State::Particle;
    return true;
  }
  bool key(string_t &key) override {
    if (field != Field::None)
      return fail("particle geometry JSON key has incomplete prior value");
    if (state == State::Root) {
      const unsigned bit = key == "version" ? 1 : key == "particles" ? 2 : 0;
      if (!bit || (rootSeen & bit))
        return fail("unknown or duplicate particle geometry JSON root field");
      rootSeen |= bit;
      field = bit == 1 ? Field::Version : Field::Particles;
      return true;
    }
    if (state == State::Particle) {
      const unsigned bit = key == "position"   ? 1
                           : key == "radius"   ? 2
                           : key == "velocity" ? 4
                                               : 0;
      if (!bit || (particleSeen & bit))
        return fail(
            "unknown or duplicate particle geometry JSON particle field");
      particleSeen |= bit;
      field = bit == 1   ? Field::Position
              : bit == 2 ? Field::Radius
                         : Field::Velocity;
      return true;
    }
    return fail("particle geometry JSON object key is out of schema");
  }
  bool end_object() override {
    if (state == State::Particle && field == Field::None &&
        (particleSeen & 3) == 3) {
      particles.push_back(current);
      state = State::Array;
      return true;
    }
    if (state == State::Root && field == Field::None && versionComplete &&
        particlesComplete) {
      state = State::Done;
      return true;
    }
    return fail(
        "particle geometry JSON has missing or incomplete required fields");
  }
  bool start_array(std::size_t) override {
    if (state == State::Root && field == Field::Particles) {
      state = State::Array;
      field = Field::None;
      return true;
    }
    if (state == State::Particle &&
        (field == Field::Position || field == Field::Velocity)) {
      vectorField = field;
      coordinate = 0;
      state = State::Coordinates;
      return true;
    }
    return fail("particle geometry JSON array is out of schema");
  }
  bool end_array() override {
    if (state == State::Coordinates && coordinate == 3) {
      state = State::Particle;
      field = Field::None;
      return true;
    }
    if (state == State::Array) {
      state = State::Root;
      particlesComplete = true;
      return true;
    }
    return fail(
        "particle geometry JSON vectors require exactly3 numeric components");
  }
  bool parse_error(std::size_t, const string_t &,
                   const nlohmann::detail::exception &) override {
    return fail("particle geometry JSON syntax/UTF-8/numeric range is invalid");
  }
};
#endif
} // namespace

ParticleGeometryResult
WriteParticleGeometry(std::span<const GeometryParticle> particles,
                      const std::filesystem::path &output,
                      const ParticleGeometryOptions &options,
                      const ProcessingOptions &processing) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validate(options, processing);
  outputPath(output);
  openvdb::initialize();
  tbb::task_arena arena(1);
  return arena.execute(
      [&] { return execute(particles, output, options, processing); });
#else
  (void)particles;
  (void)output;
  (void)options;
  (void)processing;
  unavailable();
#endif
}

ParticleGeometryResult
WriteParticleGeometryFile(const std::filesystem::path &input,
                          const std::filesystem::path &output,
                          const ParticleGeometryOptions &options,
                          const ProcessingOptions &processing) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validate(options, processing);
  outputPath(output);
  Snapshot snapshot(input, processing, options);
  std::ifstream source(snapshot.file, std::ios::binary);
  require(bool(source), "cannot read private particle geometry JSON snapshot");
  TokenStream buffer(source, snapshot.encoded);
  std::istream stream(&buffer);
  JsonParticles parser(processing, options, snapshot.encoded);
  const bool parsed = nlohmann::json::sax_parse(stream, &parser);
  require(!buffer.exceeded(),
          "particle geometry JSON raw token exceeds128 bytes");
  if (!parsed || parser.state != JsonParticles::State::Done)
    throw std::invalid_argument(parser.error.empty()
                                    ? "particle geometry JSON is incomplete"
                                    : parser.error);
  require(!source.bad(), "particle geometry JSON private snapshot read failed");
  const uint64_t retained = add(
      Fixed,
      add(mul(snapshot.encoded, 3),
          mul(mul(parser.particles.capacity(), sizeof(GeometryParticle)), 3)));
  const uint64_t parsingWork =
      add(mul(snapshot.encoded, 8), mul(parser.particles.size(), 512));
  require(retained < processing.maxInputBytes && parsingWork < options.maxWork,
          "particle geometry JSON retained buffers/parsing leave no shared "
          "native budget");
  ProcessingOptions nativeProcessing;
  nativeProcessing.voxelSize = processing.voxelSize;
  nativeProcessing.halfWidth = processing.halfWidth;
  if (sdf(options))
    nativeProcessing.gridName = processing.gridName;
  nativeProcessing.maxVoxelCount = processing.maxVoxelCount;
  nativeProcessing.maxInputBytes = processing.maxInputBytes - retained;
  // Do not copy ignored arbitrarily large names. All copied names were bounded
  // before parsing, and their storage is included in the native metadata term.
  ParticleGeometryOptions nativeOptions;
  nativeOptions.output = options.output;
  nativeOptions.timeStep = options.timeStep;
  nativeOptions.spacing = options.spacing;
  nativeOptions.minRadiusVoxels = options.minRadiusVoxels;
  nativeOptions.maxRadiusVoxels = options.maxRadiusVoxels;
  nativeOptions.originWorld = options.originWorld;
  if (mask(options))
    nativeOptions.maskGridName = options.maskGridName;
  nativeOptions.maxParticles = options.maxParticles;
  nativeOptions.maxTrailInstances = options.maxTrailInstances;
  nativeOptions.maxTrailLengthVoxels = options.maxTrailLengthVoxels;
  nativeOptions.maxNodes = options.maxNodes;
  nativeOptions.maxWork = options.maxWork - parsingWork;
  nativeOptions.maxMetadataBytes = options.maxMetadataBytes;
  nativeOptions.maxOutputBytes = options.maxOutputBytes;
  auto result = WriteParticleGeometry(parser.particles, output, nativeOptions,
                                      nativeProcessing);
  // Reduced caps proved both sums <= the original finite caps before
  // publication.
  result.workspaceUpperBound += retained;
  result.workUpperBound += parsingWork;
  return result;
#else
  (void)input;
  (void)output;
  (void)options;
  (void)processing;
  unavailable();
#endif
}

} // namespace container::geometry::vdb
