#include "Container/geometry/OpenVdbVectorPreview.h"
#include "Container/geometry/OpenVdbGridSelection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <functional>
#include <mutex>
#include <numbers>
#include <openvdb/openvdb.h>
#include <openvdb/tools/Interpolation.h>
#include <set>
#include <tbb/task_arena.h>
#endif

namespace container::geometry::vdb {
namespace {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using V = openvdb::Vec3d;
constexpr long double Fixed = 1024 * 1024;
double length(const V &v) { return std::hypot(v.x(), v.y(), v.z()); }
bool finite(const V &v) {
  return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
}
V normalized(const V &v) {
  const double n = length(v);
  return V(v.x() / n, v.y() / n, v.z() / n);
}
double gap(double x) {
  return std::max(std::nextafter(x, INFINITY) - x, x - std::nextafter(x, -INFINITY));
}
long double metadata(const openvdb::GridBase &g) {
  long double bytes = sizeof(openvdb::MetaMap);
  for (auto i = g.beginMeta(); i != g.endMeta(); ++i)
    bytes += 4.0L * (i->first.size() + i->second->typeName().size() + i->second->size()) + 256;
  return bytes;
}
void budget(long double bytes, const VectorPreviewOptions &o) {
  if (!std::isfinite(bytes) || bytes > o.maxInputBytes)
    throw std::invalid_argument("vector preview input/geometry workspace exceeds memory budget");
}
void validate(float scale, const VectorPreviewOptions &o) {
  if (!std::isfinite(scale) || scale <= 0)
    throw std::invalid_argument("vector preview import scale must be finite positive");
  if (o.mode != VectorPreviewMode::Glyphs && o.mode != VectorPreviewMode::Streamlines)
    throw std::invalid_argument("unknown vector preview mode");
  for (double x : {o.displaySeconds, o.maxGlyphLength, o.worldRadius, o.worldStep})
    if (!std::isfinite(x) || x < 1e-6 || x > 1e6)
      throw std::invalid_argument("vector preview lengths/seconds must be in [1e-6,1e6]");
  if (!o.maxSeeds || o.maxSeeds > 4096 || !o.maxTriangles || !o.maxActiveVoxels ||
      !o.maxInputBytes || !o.maxWork || o.maxWork > 1'000'000'000 || !o.maxSteps ||
      o.maxSteps > 4096)
    throw std::invalid_argument("invalid vector preview seed/triangle/topology/work/step budgets");
  uint64_t count = 1;
  for (uint32_t n : o.seedCounts) {
    if (!n || n > 256)
      throw std::invalid_argument("vector preview seed axes must be in [1,256]");
    count *= n;
  }
  if (count > o.maxSeeds || count > 4096)
    throw std::invalid_argument("vector preview seed product exceeds maxSeeds");
  if (o.colorMode != PointPreviewColor::Uniform &&
      o.colorMode != PointPreviewColor::VelocityDirection &&
      o.colorMode != PointPreviewColor::VelocityMagnitude)
    throw std::invalid_argument("unknown vector preview color mode");
  for (float x : o.uniformColor)
    if (!std::isfinite(x) || x < 0 || x > 1)
      throw std::invalid_argument("invalid vector preview uniform color");
  if (!std::isfinite(o.velocityScale) || o.velocityScale <= 0)
    throw std::invalid_argument("vector preview velocity color scale must be positive finite");
  if (o.hasBounds)
    for (size_t a = 0; a < 3; ++a)
      if (!std::isfinite(o.boundsMin[a]) || !std::isfinite(o.boundsMax[a]) ||
          o.boundsMax[a] <= o.boundsMin[a])
        throw std::invalid_argument(
            "vector preview bounds must be finite ordered source-world bounds");
}
bool isMac(const openvdb::GridBase &g) {
  const auto layout = g.getMetadata<openvdb::StringMetadata>("container:mac_layout");
  const auto units = g.getMetadata<openvdb::StringMetadata>("container:velocity_units");
  return g.getGridClass() == openvdb::GRID_STAGGERED && !g.isInWorldSpace() &&
         g.getVectorType() == openvdb::VEC_CONTRAVARIANT_RELATIVE && layout &&
         layout->value() == "negative_faces" && units && units->value() == "voxels_per_second";
}
openvdb::GridBase::Ptr read(openvdb::io::File &file, const std::string &selector, bool vector,
                            long double resident, const VectorPreviewOptions &o,
                            uint64_t priorActive = 0, uint64_t priorLeaves = 0) {
  std::string descriptor;
  bool found = false;
  for (auto it = file.beginName(); it != file.endName(); ++it) {
    const auto g = file.readGridMetadata(it.gridName());
    if (!GridSelectionMatches(selector, it.gridName(), g->getName()))
      continue;
    const bool type = vector ? (g->isType<openvdb::Vec3SGrid>() || g->isType<openvdb::Vec3DGrid>())
                             : (g->isType<openvdb::BoolGrid>() || g->isType<openvdb::FloatGrid>() ||
                                g->isType<openvdb::DoubleGrid>());
    if (!type) {
      if (!selector.empty())
        throw std::invalid_argument("selected vector preview grid has unsupported type");
      continue;
    }
    if (found)
      throw std::invalid_argument(
          "vector preview grid selection is ambiguous; use descriptor:<exact name>");
    const auto count =
        g->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_VOXEL_COUNT);
    const auto memory =
        g->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_MEM_BYTES);
    if (!count || !memory || count->value() < 0 || memory->value() < 0 ||
        uint64_t(count->value()) > o.maxActiveVoxels - priorActive)
      throw std::invalid_argument(
          "vector preview requires recorded topology/memory statistics within budget");
    budget(resident + memory->value() + metadata(*g) + Fixed, o);
    descriptor = it.gridName();
    found = true;
    if (selector.empty())
      break;
  }
  if (!found)
    throw std::invalid_argument(vector ? "no matching Vec3S/Vec3D vector grid"
                                       : "no matching scalar/bool fluid mask");
  auto grid = file.readGrid(descriptor);
  if (grid->activeVoxelCount() > o.maxActiveVoxels - priorActive ||
      grid->baseTree().leafCount() + priorLeaves >
          o.maxActiveVoxels / 512 + (o.maxActiveVoxels % 512 != 0))
    throw std::invalid_argument("vector preview stored topology exceeds budget");
  budget(resident + grid->memUsage() + metadata(*grid) + Fixed, o);
  return grid;
}
double transform(const openvdb::GridBase &grid, bool mac) {
  if (!grid.transform().isLinear())
    throw std::invalid_argument("vector preview requires finite affine transforms");
  const auto map = grid.transform().baseMap();
  const auto origin = map->applyMap(V(0));
  if (!finite(origin))
    throw std::invalid_argument("vector preview transform origin is nonfinite");
  std::array<V, 3> basis, inverse;
  std::array<double, 3> sizes;
  double maximum = 0;
  for (size_t a = 0; a < 3; ++a) {
    V axis(0);
    axis[a] = 1;
    basis[a] = map->applyJacobian(axis);
    sizes[a] = length(basis[a]);
    inverse[a] = map->applyInverseJacobian(axis);
    if (!finite(basis[a]) || !finite(inverse[a]) || sizes[a] < 1e-6 || sizes[a] > 1e6)
      throw std::invalid_argument("vector preview grid axes must span [1e-6,1e6] world units");
    basis[a] /= sizes[a];
    for (size_t k = 0; k < 3; ++k)
      if (std::abs(inverse[a][k]) > 1e8)
        throw std::invalid_argument("ill-conditioned vector preview affine map");
  }
  if (std::abs(basis[0].dot(basis[1].cross(basis[2]))) < 1e-8)
    throw std::invalid_argument("singular vector preview affine map");
  for (size_t a = 0; a < 3; ++a) {
    maximum = std::max(maximum, std::hypot(inverse[0][a], inverse[1][a], inverse[2][a]));
    if (mac)
      for (size_t b = a + 1; b < 3; ++b)
        if (std::abs(sizes[a] / sizes[b] - 1) > 1e-9 || std::abs(basis[a].dot(basis[b])) > 1e-9)
          throw std::invalid_argument("native MAC preview requires uniform orthogonal voxels");
  }
  return .5 / maximum;
}
struct Context {
  const VectorPreviewOptions &options;
  float scale;
  const openvdb::GridBase &grid;
  long double resident;
  dotbim::Model model;
  V minimum, maximum;
  double step;
  uint64_t work = 0, zeros = 0, shortened = 0, clipped = 0, cycles = 0, exhausted = 0, outside = 0;
  std::function<bool(const openvdb::Coord &)> fluid;
  V lo{INFINITY}, hi{-INFINITY};
  void use() {
    if (work == options.maxWork)
      throw std::invalid_argument("vector preview work budget exceeded");
    ++work;
  }
  V index(const V &p) {
    const auto map = grid.transform().baseMap();
    const auto q = map->applyInverseJacobian(p - map->applyMap(V(0)));
    if (!finite(q))
      throw std::invalid_argument("vector preview sample index is nonfinite");
    double resolution = 0;
    for (size_t a = 0; a < 3; ++a) {
      if (q[a] < double(INT32_MIN) + 16 || q[a] > double(INT32_MAX) - 16)
        throw std::invalid_argument("vector preview sample exceeds stencil index range");
      V axis(0);
      axis[a] = 1;
      resolution += gap(q[a]) * length(map->applyJacobian(axis));
    }
    if (resolution > std::min(step, options.worldRadius) * 1e-4)
      throw std::invalid_argument(
          "vector preview source-index precision cannot resolve sampling; recenter the grid");
    return q;
  }
  static openvdb::Coord cell(const V &p) {
    return {int32_t(std::floor(p.x() + .5)), int32_t(std::floor(p.y() + .5)),
            int32_t(std::floor(p.z() + .5))};
  }
  bool active(const openvdb::Coord &c) {
    use();
    return fluid(c);
  }
  bool inside(const V &p) {
    if (!finite(p))
      throw std::invalid_argument("vector preview integration produced nonfinite coordinates");
    for (size_t a = 0; a < 3; ++a)
      if (p[a] < minimum[a] || p[a] > maximum[a])
        return false;
    return !fluid || active(cell(index(p)));
  }
  double wall(const V &start, const V &end) {
    if (!fluid)
      return 1;
    const V delta = end - start;
    auto c = cell(start);
    if (!active(c))
      return 0;
    std::array<double, 3> next, increment;
    for (size_t a = 0; a < 3; ++a) {
      if (delta[a] == 0)
        next[a] = increment[a] = INFINITY;
      else {
        next[a] = std::max(0.0, (double(c[a]) + (delta[a] > 0 ? .5 : -.5) - start[a]) / delta[a]);
        increment[a] = 1 / std::abs(delta[a]);
      }
    }
    for (unsigned crossing = 0; crossing < 64; ++crossing) {
      use();
      const double t = std::min({next[0], next[1], next[2]});
      if (t > 1 || !std::isfinite(t))
        return 1;
      unsigned axes = 0;
      for (size_t a = 0; a < 3; ++a)
        if (next[a] <= t)
          axes |= 1u << a;
      for (unsigned subset = axes; subset; subset = (subset - 1) & axes) {
        auto n = c;
        for (size_t a = 0; a < 3; ++a)
          if (subset & (1u << a))
            n[a] += delta[a] > 0 ? 1 : -1;
        if (!active(n))
          return t;
      }
      for (size_t a = 0; a < 3; ++a)
        if (next[a] <= t) {
          c[a] += delta[a] > 0 ? 1 : -1;
          next[a] += increment[a];
        }
    }
    throw std::invalid_argument("vector preview wall traversal exceeds bounded crossings");
  }
  bool visible(const V &p, const V &q) { return wall(p, q) >= 1; }
  V clip(const V &a, const V &b) {
    const V delta = b - a;
    double t = 1;
    for (size_t axis = 0; axis < 3; ++axis) {
      if (delta[axis] > 0)
        t = std::min(t, (maximum[axis] - a[axis]) / delta[axis]);
      if (delta[axis] < 0)
        t = std::min(t, (minimum[axis] - a[axis]) / delta[axis]);
    }
    const V boxed = a + delta * std::clamp(t, 0.0, 1.0);
    if (fluid)
      t *= wall(index(a), index(boxed));
    if (t < 1)
      ++clipped;
    V result = a + delta * (t < 1 ? std::nextafter(std::max(0.0, t), 0.0) : t);
    if (fluid)
      for (unsigned n = 0; n < 4 && !inside(result); ++n)
        for (size_t axis = 0; axis < 3; ++axis)
          result[axis] = std::nextafter(result[axis], a[axis]);
    return inside(result) ? result : a;
  }
  glm::vec3 color(const V &v) const {
    const auto &o = options;
    if (o.colorMode == PointPreviewColor::Uniform)
      return {o.uniformColor[0], o.uniformColor[1], o.uniformColor[2]};
    const double maximum = std::max({std::abs(v.x()), std::abs(v.y()), std::abs(v.z())});
    if (maximum == 0)
      return o.colorMode == PointPreviewColor::VelocityDirection ? glm::vec3(.5f)
                                                                 : glm::vec3(.1f, .25f, 1);
    const V relative = v / maximum;
    const double n = length(relative);
    if (o.colorMode == PointPreviewColor::VelocityDirection)
      return {float(.5 + .5 * relative.x() / n), float(.5 + .5 * relative.y() / n),
              float(.5 + .5 * relative.z() / n)};
    const double t =
        maximum >= o.velocityScale / n ? 1 : std::clamp((maximum / o.velocityScale) * n, 0.0, 1.0);
    return {float(std::lerp(.1, 1., t)), float(std::lerp(.25, .2, t)),
            float(std::lerp(1., .05, t))};
  }
  void triangle(const V &a, const V &b, const V &c, const glm::vec3 &tint, double feature) {
    use();
    if (model.indices.size() / 3 >= options.maxTriangles || model.vertices.size() > UINT32_MAX - 3)
      throw std::invalid_argument("vector preview triangle/index budget exceeded");
    const size_t needed = model.vertices.size() + 3;
    if (needed > model.vertices.capacity() || needed > model.indices.capacity()) {
      const size_t cap = std::max(needed, std::max<size_t>(256, model.vertices.capacity() * 2));
      budget(resident + Fixed +
                 (cap + model.vertices.capacity()) * static_cast<long double>(sizeof(Vertex)) +
                 (cap + model.indices.capacity()) * sizeof(uint32_t) +
                 (options.maxSteps + 1) * sizeof(V),
             options);
      model.vertices.reserve(cap);
      model.indices.reserve(cap);
    }
    const V cross = (b - a).cross(c - a);
    if (!finite(cross) || length(cross) == 0)
      throw std::invalid_argument("degenerate vector preview geometry; increase radius/length");
    const V normal = normalized(cross);
    for (const auto &p : {a, b, c}) {
      Vertex vertex;
      vertex.normal = {float(normal.x()), float(normal.y()), float(normal.z())};
      vertex.color = tint;
      for (size_t axis = 0; axis < 3; ++axis) {
        const double value = p[axis] * scale;
        const float represented = float(value);
        if (!std::isfinite(value) || !std::isfinite(represented) ||
            std::abs(double(represented) - value) > feature * scale * .001)
          throw std::invalid_argument(
              "vector preview geometry loses Float32 offsets; recenter/rescale input");
        vertex.position[axis] = represented;
        lo[axis] = std::min(lo[axis], double(represented));
        hi[axis] = std::max(hi[axis], double(represented));
      }
      const auto reference =
          std::abs(vertex.normal.z) < .9f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
      vertex.tangent = glm::vec4(glm::normalize(glm::cross(reference, vertex.normal)), 1);
      model.indices.push_back(static_cast<uint32_t>(model.vertices.size()));
      model.vertices.push_back(vertex);
    }
  }
  void segment(const V &a, const V &b, const V &velocity, bool arrow) {
    const double n = length(b - a);
    if (n == 0) {
      ++zeros;
      return;
    }
    const V direction = (b - a) / n;
    const V tangent =
        normalized(direction.cross(std::abs(direction.z()) < .9 ? V(0, 0, 1) : V(0, 1, 0)));
    const V bitangent = direction.cross(tangent);
    const double radius = options.worldRadius;
    const double feature = std::min(radius,n);
    const V shaftEnd = arrow ? a + (b - a) * .7 : b;
    std::array<V, 6> ringA, ringB, head;
    for (size_t i = 0; i < 6; ++i) {
      const double angle = 2 * std::numbers::pi * double(i) / 6;
      const V offset = tangent * std::cos(angle) + bitangent * std::sin(angle);
      ringA[i] = a + offset * radius;
      ringB[i] = shaftEnd + offset * radius;
      head[i] = shaftEnd + offset * (radius * 2);
    }
    const auto tint = color(velocity);
    for (size_t i = 0; i < 6; ++i) {
      const size_t next = (i + 1) % 6;
      triangle(ringA[i], ringB[next], ringB[i], tint, feature);
      triangle(ringA[i], ringA[next], ringB[next], tint, feature);
      triangle(a, ringA[next], ringA[i], tint, feature);
      triangle(shaftEnd, ringB[i], ringB[next], tint, feature);
      if (arrow) {
        triangle(head[i], head[next], b, tint, feature);
        triangle(shaftEnd, head[next], head[i], tint, feature);
      }
    }
  }
};

template <class Grid> struct DoubleTreeView {
  using ValueType = V;
  const Grid &grid;
  V getValue(const openvdb::Coord &coordinate) const { return V(grid.tree().getValue(coordinate)); }
};

template <class Grid> V sample(Context &c, const Grid &grid, const V &world, bool mac) {
  c.use();
  const V p = c.index(world);
  V result;
  if (!c.fluid) {
    // Promote fetches before interpolation: Float32's staged interpolation can
    // turn a valid tiny vector into zero, changing a streamline's direction.
    const DoubleTreeView<Grid> view{grid};
    result = mac ? openvdb::tools::StaggeredBoxSampler::sample(view, p)
                 : openvdb::tools::BoxSampler::sample(view, p);
  } else {
    // Renormalize only locally visible fluid stencil weights. Global component
    // labels alone would leak through a solid corner joined by a remote path.
    for (size_t axis = 0; axis < 3; ++axis) {
      V q = p;
      if (mac)
        q[axis] += .5;
      const openvdb::Coord base(int32_t(std::floor(q.x())), int32_t(std::floor(q.y())),
                                int32_t(std::floor(q.z())));
      const V f = q - base.asVec3d();
      double sum = 0, weights = 0;
      for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 2; ++y)
          for (int z = 0; z < 2; ++z) {
            const auto coord = base + openvdb::Coord(x, y, z);
            const double w =
                (x ? f.x() : 1 - f.x()) * (y ? f.y() : 1 - f.y()) * (z ? f.z() : 1 - f.z());
            if (w == 0)
              continue;
            V location = coord.asVec3d();
            bool valid = c.active(coord), interior = valid;
            if (mac) {
              location[axis] -= .5;
              auto previous = coord;
              --previous[axis];
              const bool before = c.active(previous);
              valid |= before;
              interior &= before;
            }
            if (valid && c.visible(p, location)) {
              sum += (mac && !interior ? 0 : double(grid.tree().getValue(coord)[axis])) * w;
              weights += w;
            }
          }
      result[axis] = weights > 0 ? sum / weights : 0;
    }
  }
  V velocity(double(result.x()), double(result.y()), double(result.z()));
  if (mac) {
    velocity = grid.transform().baseMap()->applyJacobian(velocity);
    if (velocity == V(0) && result != V(0))
      throw std::invalid_argument("native MAC world conversion underflows; rescale input");
  }
  if (!finite(velocity))
    throw std::invalid_argument("vector preview sampled nonfinite world velocity");
  return velocity;
}

template <class Grid>
dotbim::Model run(const Grid &grid, const openvdb::GridBase::Ptr &domain,
                  const std::string &selector, float scale, const VectorPreviewOptions &o,
                  long double resident) {
  const bool mac = isMac(grid);
  if (!mac && (!grid.isInWorldSpace() || grid.getGridClass() == openvdb::GRID_STAGGERED))
    throw std::invalid_argument(
        "vector preview requires collocated WORLD vectors or tagged native M04/M05 MAC");
  const double mapStep = transform(grid, mac);
  const auto validValue = [&](const auto &v) {
    for (size_t a = 0; a < 3; ++a)
      if (!std::isfinite(double(v[a])) || std::abs(double(v[a])) > (mac ? 1e24 : 1e12))
        return false;
    return true;
  };
  if (!validValue(grid.background()))
    throw std::invalid_argument("vector preview background exceeds finite component bounds");
  for (auto v = grid.cbeginValueAll(); v; ++v)
    if (!validValue(*v))
      throw std::invalid_argument("vector preview stored values exceed finite component bounds");
  Context c{o, scale, grid, resident};
  c.step = std::min(o.worldStep, mapStep);
  if (o.hasBounds) {
    c.minimum = V(o.boundsMin.data());
    c.maximum = V(o.boundsMax.data());
  } else {
    if (grid.empty() || grid.background() != typename Grid::ValueType(0))
      throw std::invalid_argument(
          "constant/inactive-background vector fields require explicit preview bounds");
    const auto b = grid.evalActiveVoxelBoundingBox();
    c.minimum = V(INFINITY);
    c.maximum = V(-INFINITY);
    for (unsigned corner = 0; corner < 8; ++corner) {
      V index;
      for (size_t a = 0; a < 3; ++a)
        index[a] = corner & (1u << a) ? double(b.max()[a]) + .5 : double(b.min()[a]) - .5;
      const V p = grid.indexToWorld(index);
      for (size_t a = 0; a < 3; ++a) {
        c.minimum[a] = std::min(c.minimum[a], p[a]);
        c.maximum[a] = std::max(c.maximum[a], p[a]);
      }
    }
  }
  for (size_t a = 0; a < 3; ++a)
    if (!std::isfinite(c.minimum[a]) || !std::isfinite(c.maximum[a]) ||
        c.maximum[a] <= c.minimum[a] ||
        std::max(gap(c.minimum[a]), gap(c.maximum[a])) > std::min(c.step, o.worldRadius) * 1e-4)
      throw std::invalid_argument(
          "vector preview world bounds lack sampling precision; recenter/rescale input");
  if (domain) {
    if (domain->transform() != grid.transform())
      throw std::invalid_argument(
          "vector preview fluid mask must share the exact vector transform");
    const auto prepare = [&]<class Mask>(typename Mask::Ptr mask) {
      if (mask->background() != 0)
        throw std::invalid_argument("vector preview fluid mask background must be zero");
      for (auto value = mask->cbeginValueAll(); value; ++value)
        if (!std::isfinite(double(*value)) || double(*value) < 0)
          throw std::invalid_argument(
              "vector preview fluid mask values must be finite nonnegative");
      c.fluid = [mask](const openvdb::Coord &q) {
        return mask->tree().isValueOn(q) && double(mask->tree().getValue(q)) > 0;
      };
    };
    if (auto g = openvdb::gridPtrCast<openvdb::BoolGrid>(domain))
      prepare.template operator()<openvdb::BoolGrid>(g);
    else if (auto g = openvdb::gridPtrCast<openvdb::FloatGrid>(domain))
      prepare.template operator()<openvdb::FloatGrid>(g);
    else
      prepare.template operator()<openvdb::DoubleGrid>(
          openvdb::gridPtrCast<openvdb::DoubleGrid>(domain));
  }
  for (uint32_t x = 0; x < o.seedCounts[0]; ++x)
    for (uint32_t y = 0; y < o.seedCounts[1]; ++y)
      for (uint32_t z = 0; z < o.seedCounts[2]; ++z) {
        V seed;
        const std::array<uint32_t, 3> coord{x, y, z};
        for (size_t a = 0; a < 3; ++a)
          seed[a] = c.minimum[a] +
                    (c.maximum[a] - c.minimum[a]) * (double(coord[a]) + .5) / o.seedCounts[a];
        if (!c.inside(seed)) {
          ++c.outside;
          continue;
        }
        const V velocity = sample(c, grid, seed, mac);
        const double speed = length(velocity);
        if (speed == 0) {
          ++c.zeros;
          continue;
        }
        if (o.mode == VectorPreviewMode::Glyphs) {
          const double desired = speed * o.displaySeconds;
          if (desired == 0)
            throw std::invalid_argument("vector glyph displacement underflows; rescale input");
          const double extent = std::min(desired, o.maxGlyphLength);
          if (desired > o.maxGlyphLength)
            ++c.shortened;
          c.segment(seed, seed + normalized(velocity) * extent, velocity, true);
          continue;
        }
        for (int sign : o.bothDirections ? std::vector<int>{-1, 1} : std::vector<int>{1}) {
          V point = seed;
          bool stopped = false;
          for (uint32_t step = 0; step < o.maxSteps; ++step) {
            const auto tangent = [&](const V &p) {
              const V v = sample(c, grid, p, mac);
              return length(v) == 0 ? V(0) : normalized(v) * double(sign);
            };
            const V k1 = tangent(point);
            if (length(k1) == 0) {
              ++c.zeros;
              stopped = true;
              break;
            }
            V target = point + k1 * c.step;
            bool stageClip = false, stagnant = false;
            V k2, k3, k4;
            const auto stage = [&](const V &p, V &k) {
              if (!c.inside(p)) {
                stageClip = true;
                return;
              }
              k = tangent(p);
              if (length(k) == 0)
                stagnant = true;
            };
            stage(point + k1 * (c.step * .5), k2);
            if (!stageClip && !stagnant)
              stage(point + k2 * (c.step * .5), k3);
            if (!stageClip && !stagnant)
              stage(point + k3 * c.step, k4);
            if (stagnant) {
              ++c.zeros;
              stopped = true;
              break;
            }
            if (!stageClip)
              target = point + (k1 + k2 * 2 + k3 * 2 + k4) * (c.step / 6);
            const auto priorClipped = c.clipped;
            const V next = c.clip(point, target);
            if (stageClip && priorClipped == c.clipped)
              ++c.clipped;
            if (length(next - point) <= c.step * 1e-10) {
              ++c.zeros;
              stopped = true;
              break;
            }
            c.segment(point, next, sample(c, grid, point, mac), false);
            if (stageClip || length(next - target) > c.step * 1e-10) {
              stopped = true;
              break;
            }
            if (step >= 4 && length(next - seed) < c.step * .5) {
              ++c.cycles;
              stopped = true;
              break;
            }
            point = next;
          }
          if (!stopped)
            ++c.exhausted;
        }
      }
  auto &model = c.model;
  model.importReport.sourceFormat = "OpenVDB";
  model.importReport.sourceItemName = "vector fields";
  model.importReport.sourceProductCount = 1;
  const auto warn = [&](const char *key, uint64_t count, const char *reason) {
    if (count) {
      model.importReport.representationWarnings[key] = static_cast<size_t>(count);
      model.importReport.diagnostics.push_back(
          {key, 0, 0, "vdb-vector:" + selector,
           std::string(reason) + ": " + std::to_string(count)});
    }
  };
  warn("OpenVDB.VectorZero", c.zeros, "Zero/stagnant vectors or traces omitted");
  warn("OpenVDB.VectorGlyphShortened", c.shortened, "Glyphs shortened to explicit maxGlyphLength");
  warn("OpenVDB.VectorClipped", c.clipped, "Traces clipped to bounds/solid walls");
  warn("OpenVDB.VectorCycle", c.cycles, "Return-to-seed cycles stopped");
  warn("OpenVDB.VectorStepLimit", c.exhausted, "Traces stopped at maxSteps");
  warn("OpenVDB.VectorOutsideDomain", c.outside, "Seed points outside fluid domain omitted");
  if (c.step < o.worldStep)
    warn("OpenVDB.VectorStepReduced", 1, "World step reduced to half a source index-cell travel");
  model.unitMetadata.hasSourceUnits = true;
  model.unitMetadata.sourceUnits = "OpenVDB world units";
  model.unitMetadata.hasImportScale = true;
  model.unitMetadata.importScale = scale;
  model.unitMetadata.hasEffectiveImportScale = true;
  model.unitMetadata.effectiveImportScale = scale;
  if (!model.vertices.empty()) {
    dotbim::Material material;
    material.pbr.baseColor = glm::vec4(1);
    material.pbr.roughnessFactor = .75f;
    model.materials.push_back(material);
    const V center = (c.lo + c.hi) * .5;
    const glm::vec3 mid(float(center.x()), float(center.y()), float(center.z()));
    const V displayCenter(double(mid.x), double(mid.y), double(mid.z));
    V extent;
    for (size_t a = 0; a < 3; ++a)
      extent[a] =
          std::max(std::abs(c.lo[a] - displayCenter[a]), std::abs(c.hi[a] - displayCenter[a]));
    const double exactRadius = length(extent);
    float radius = float(exactRadius);
    if (radius < exactRadius)
      radius = std::nextafter(radius, INFINITY);
    if (!std::isfinite(radius))
      throw std::invalid_argument("vector preview display bounds exceed Float32 range");
    model.meshRanges.push_back({.meshId = 0,
                                .firstIndex = 0,
                                .indexCount = static_cast<uint32_t>(model.indices.size()),
                                .boundsCenter = mid,
                                .boundsRadius = radius});
    dotbim::Element element;
    element.meshId = 0;
    element.geometryKind = dotbim::GeometryKind::Mesh;
    element.guid = "vdb-vector:" + selector;
    element.type =
        o.mode == VectorPreviewMode::Glyphs ? "OpenVDB Vector Glyphs" : "OpenVDB Streamlines";
    element.sourceId = selector;
    element.displayName = grid.getName();
    element.color = glm::vec4(1);
    element.materialIndex = 0;
    element.doubleSided = false;
    element.properties = {
        {"OpenVDB", "Preview",
         o.mode == VectorPreviewMode::Glyphs
             ? "World velocity times displaySeconds, capped explicitly"
             : "Instantaneous world field lines; not particle trajectories",
         ""},
        {"OpenVDB", "Frame",
         mac ? "Native negative-face MAC: Jacobian converts voxels/second to world"
             : "Collocated world vector components",
         ""},
        {"OpenVDB", "Work", std::to_string(c.work), ""},
        {"OpenVDB", "Triangles", std::to_string(model.indices.size() / 3), ""},
        {"OpenVDB", "Clipped", std::to_string(c.clipped), ""},
        {"OpenVDB", "Cycles", std::to_string(c.cycles), ""}};
    model.elements.push_back(std::move(element));
    model.importReport.importedProductCount = 1;
  } else
    model.importReport.skippedProductCount = 1;
  model.importReport.completeness = model.importReport.diagnostics.empty()
                                        ? ImportCompleteness::Complete
                                        : ImportCompleteness::Partial;
  return std::move(model);
}
#endif
} // namespace

dotbim::Model LoadVectorPreviewFromFile(const std::filesystem::path &input,
                                        const std::string &selection, float importScale,
                                        const VectorPreviewOptions &options) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validate(importScale, options);
  if (!std::filesystem::is_regular_file(input))
    throw std::invalid_argument("vector preview input is not a regular file");
  const auto encoded = std::filesystem::file_size(input);
  budget(encoded + Fixed, options);
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });
  tbb::task_arena arena(1);
  return arena.execute([&] {
    detail::RequireSeekableFile(input);
    openvdb::io::File file(input.string());
    file.open(false);
    {
      std::set<std::string> names;
      long double bytes = encoded + Fixed;
      for (auto it = file.beginName(); it != file.endName(); ++it) {
        const auto name = it.gridName();
        bytes += name.size() * 2.0L + 128;
        budget(bytes, options);
        if (names.size() >= 4096 || !names.insert(name).second)
          throw std::invalid_argument(
              "too many or colliding vector preview descriptors; rename grids");
      }
    }
    auto grid = read(file, selection, true, encoded, options);
    long double resident = encoded + grid->memUsage() + metadata(*grid);
    openvdb::GridBase::Ptr domain;
    if (!options.domainGrid.empty()) {
      domain = read(file, options.domainGrid, false, resident, options, grid->activeVoxelCount(),
                    grid->baseTree().leafCount());
      resident += domain->memUsage() + metadata(*domain);
    }
    file.close();
    budget(resident + Fixed, options);
    if (auto floats = openvdb::gridPtrCast<openvdb::Vec3SGrid>(grid))
      return run(*floats, domain, selection, importScale, options, resident);
    return run(*openvdb::gridPtrCast<openvdb::Vec3DGrid>(grid), domain, selection, importScale,
               options, resident);
  });
#else
  (void)input;
  (void)selection;
  (void)importScale;
  (void)options;
  throw std::runtime_error(
      "OpenVDB vector preview is unavailable; configure with -DENABLE_OPENVDB=ON.");
#endif
}
} // namespace container::geometry::vdb
