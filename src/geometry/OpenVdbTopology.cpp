#include "Container/geometry/OpenVdbTopology.h"
#include "OpenVdbProvenance.h"
#include "Container/geometry/OpenVdbMaskedValues.h"
#include "Container/geometry/OpenVdbSdfBackground.h"
#include "Container/geometry/OpenVdbSdfFilters.h"
#include "Container/geometry/OpenVdbGridSelection.h"
#include <cmath>
#include <bit>
#include <charconv>
#include <type_traits>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <openvdb/openvdb.h>
#include <openvdb/tools/Morphology.h>
#include <openvdb/tools/ChangeBackground.h>
#include <openvdb/tools/SignedFloodFill.h>
#include <openvdb/tools/LevelSetFilter.h>
#include <tuple>
#include <set>
#include <tbb/task_arena.h>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB topology masks are unavailable; configure "
                           "with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
constexpr uintmax_t Fixed = 4ull * 1024 * 1024;
void add(uintmax_t &a, uintmax_t b, uintmax_t cap, const char *why) {
  if (a > cap || b > cap - a)
    throw std::invalid_argument(why);
  a += b;
}
uintmax_t multiply(uintmax_t a, uintmax_t b, uintmax_t cap, const char *why) {
  if (b && a > cap / b)
    throw std::invalid_argument(why);
  return a * b;
}
struct Budget {
  const ProcessingOptions &p;
  uint64_t maxWork;
  uintmax_t encoded{0}, resident{0}, metadata{0}, active{0}, slots{0}, work{0};
  void check(uintmax_t prospective = 0, uintmax_t catalog = 0) const {
    const auto why = "combined topology input/mask/morphology/writer workspace "
                     "exceeds maxInputBytes";
    uintmax_t bytes = 0;
    add(bytes, Fixed, p.maxInputBytes, why);
    add(bytes, encoded, p.maxInputBytes, why);
    add(bytes, catalog, p.maxInputBytes, why);
    // Includes retained source topology, conversion, two-phase mask/scratch
    // copies, node masks/leaf manager and writer buffers. Independent of the
    // prospective active region, so remote inactive nodes remain charged.
    for (int n = 0; n < 16; ++n)
      add(bytes, resident, p.maxInputBytes, why);
    for (int n = 0; n < 8; ++n) {
      add(bytes, prospective, p.maxInputBytes, why);
      add(bytes, metadata, p.maxInputBytes, why);
    }
  }
  void items(uintmax_t count) {
    add(work, count, maxWork, "topology operation exceeds maxWorkItems");
  }
  void meta(const openvdb::MetaMap &map) {
    for (auto it = map.beginMeta(); it != map.endMeta(); ++it) {
      if (!it->second)
        throw std::invalid_argument("null topology input metadata");
      add(metadata, it->first.size() + it->second->typeName().size() + 256,
          p.maxInputBytes, "topology metadata exceeds maxInputBytes");
      add(metadata, it->second->size(), p.maxInputBytes,
          "topology metadata exceeds maxInputBytes");
    }
    check();
  }
};
void validate(const ProcessingOptions &p, uint64_t maxWork) {
  if (!p.maxVoxelCount || !p.maxInputBytes || !maxWork ||
      maxWork > 10'000'000'000ull || p.gridName.empty() ||
      p.gridName.size() > 256 || p.gridName.find('\0') != std::string::npos)
    throw std::invalid_argument(
        "invalid topology byte/voxel/work/output-name limits");
}
void validate(TopologyOperation operation, const TopologyOptions &o,
              const ProcessingOptions &p) {
  validate(p, o.maxWorkItems);
  if (operation != TopologyOperation::ActiveMask &&
      operation != TopologyOperation::Dilate &&
      operation != TopologyOperation::Erode &&
      operation != TopologyOperation::Open &&
      operation != TopologyOperation::Close)
    throw std::invalid_argument("invalid topology operation");
  if (!o.iterations || o.iterations > 32 ||
      (o.connectivity != TopologyConnectivity::Faces6 &&
       o.connectivity != TopologyConnectivity::FacesEdges18 &&
       o.connectivity != TopologyConnectivity::Full26) ||
      (o.tilePolicy != TopologyTilePolicy::Preserve &&
       o.tilePolicy != TopologyTilePolicy::Expand &&
       o.tilePolicy != TopologyTilePolicy::Ignore))
    throw std::invalid_argument(
        "invalid topology iterations/connectivity/tile policy");
  if (operation == TopologyOperation::ActiveMask &&
      (o.iterations != 1 || o.connectivity != TopologyConnectivity::Faces6 ||
       o.tilePolicy != TopologyTilePolicy::Preserve))
    throw std::invalid_argument(
        "ActiveMask does not accept morphology iterations/connectivity/tile "
        "policy overrides");
}
void outputPath(const std::filesystem::path &path) {
  if (path.empty() || std::filesystem::exists(path) ||
      (path.has_parent_path() &&
       !std::filesystem::is_directory(path.parent_path())))
    throw std::invalid_argument(
        "topology output must be a new path in an existing directory");
}
struct Temporary {
  std::filesystem::path directory;
  explicit Temporary(const std::filesystem::path &output) {
    static std::atomic_uint64_t sequence{0};
    const auto parent = output.has_parent_path() ? output.parent_path()
                                                 : std::filesystem::path(".");
    for (unsigned n = 0; n < 64; ++n) {
      const auto candidate =
          parent /
          (".container-topology-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(++sequence));
      if (std::filesystem::create_directory(candidate)) {
        directory = candidate;
        return;
      }
    }
    throw std::runtime_error("cannot reserve topology snapshot directory");
  }
  ~Temporary() {
    std::error_code ignored;
    std::filesystem::remove(directory / "a.vdb", ignored);
    std::filesystem::remove(directory / "b.vdb", ignored);
    std::filesystem::remove(directory, ignored);
  }
};
uintmax_t inputBytes(const std::filesystem::path &input, Budget &budget) {
  if (!std::filesystem::is_regular_file(input))
    throw std::invalid_argument("topology input must be a regular file");
  const auto bytes = std::filesystem::file_size(input);
  add(budget.encoded, bytes, budget.p.maxInputBytes,
      "encoded topology inputs exceed maxInputBytes");
  budget.check();
  return bytes;
}
void snapshot(const std::filesystem::path &input,
              const std::filesystem::path &output, uintmax_t bytes) {
  std::ifstream source(input, std::ios::binary);
  std::ofstream target(output,
                       std::ios::binary | std::ios::out | std::ios::noreplace);
  if (!source || !target)
    throw std::runtime_error("cannot open topology snapshot");
  std::array<char, 64 * 1024> buffer{};
  while (bytes) {
    const auto n = std::streamsize(std::min<uintmax_t>(bytes, buffer.size()));
    source.read(buffer.data(), n);
    if (source.gcount() != n)
      throw std::runtime_error(
          "topology source changed length during snapshot");
    target.write(buffer.data(), n);
    if (!target)
      throw std::runtime_error("topology snapshot write failed");
    bytes -= uintmax_t(n);
  }
  target.close();
  if (!target || source.bad() || source.peek() != std::char_traits<char>::eof())
    throw std::runtime_error(
        "topology source changed length or snapshot failed");
}
template <class Function>
auto visit(const openvdb::GridBase &grid, Function function)
    -> decltype(function(static_cast<const openvdb::FloatGrid &>(grid))) {
#define TYPE(T)                                                                \
  if (grid.isType<openvdb::T>())                                               \
  return function(static_cast<const openvdb::T &>(grid))
  TYPE(FloatGrid);
  TYPE(DoubleGrid);
  TYPE(BoolGrid);
  TYPE(MaskGrid);
  TYPE(Int32Grid);
  TYPE(Int64Grid);
  TYPE(Vec3IGrid);
  TYPE(Vec3SGrid);
  TYPE(Vec3DGrid);
#undef TYPE
  throw std::invalid_argument("topology masks support "
                              "Float/Double/Bool/Mask/Int32/Int64/Vec3I/Vec3S/"
                              "Vec3D; use point voxel masks for PointDataGrid");
}
bool supported(const openvdb::GridBase &grid) {
  return grid.isType<openvdb::FloatGrid>() ||
         grid.isType<openvdb::DoubleGrid>() ||
         grid.isType<openvdb::BoolGrid>() || grid.isType<openvdb::MaskGrid>() ||
         grid.isType<openvdb::Int32Grid>() ||
         grid.isType<openvdb::Int64Grid>() ||
         grid.isType<openvdb::Vec3IGrid>() ||
         grid.isType<openvdb::Vec3SGrid>() || grid.isType<openvdb::Vec3DGrid>();
}
openvdb::GridBase::Ptr load(const std::filesystem::path &input,
                            const std::string &selection, Budget &budget,
                            openvdb::MetaMap::Ptr *fileMetadata = nullptr,
                            bool valuesOnly = false, bool sdfOnly = false, bool realOnly = false) {
  detail::RequireSeekableFile(input);
  openvdb::io::File file(input.string());
  file.setCopyMaxBytes(0);
  file.open(true);
  const auto fileMeta = file.getMetadata();
  budget.meta(*fileMeta);
  if (fileMetadata) *fileMetadata = fileMeta;
  std::set<std::string> descriptors;
  for (auto it = file.beginName(); it != file.endName(); ++it) {
    const auto descriptor = it.gridName();
    add(budget.metadata, 2 * descriptor.size() + 128, budget.p.maxInputBytes,
        "topology descriptor catalog exceeds byte budget");
    budget.check();
    if (!descriptors.insert(descriptor).second)
      throw std::invalid_argument(
          "topology input contains colliding descriptors; rename grids first");
  }
  bool found = false;
  std::string name;
  for (auto it = file.beginName(); it != file.endName(); ++it) {
    const auto meta = file.readGridMetadata(it.gridName());
    budget.meta(*meta);
    if (!GridSelectionMatches(selection, it.gridName(), meta->getName()))
      continue;
    const bool sdfType=(meta->isType<openvdb::FloatGrid>() || meta->isType<openvdb::DoubleGrid>()) &&
                       meta->getGridClass()==openvdb::GRID_LEVEL_SET;
    const bool realType=meta->isType<openvdb::FloatGrid>() || meta->isType<openvdb::DoubleGrid>();
    if (selection.empty() && (!supported(*meta) || (valuesOnly && meta->isType<openvdb::MaskGrid>()) || (sdfOnly && !sdfType) || (realOnly && !realType)))
      continue;
    if (found)
      throw std::invalid_argument(
          "ambiguous topology selector; use descriptor:<exact descriptor>");
    if (!supported(*meta))
      (void)visit(*meta, [](const auto &) { return false; });
    if (valuesOnly && meta->isType<openvdb::MaskGrid>())
      throw std::invalid_argument("MaskGrid has no independent stored leaf values; use topology masks");
    if (sdfOnly && !sdfType)
      throw std::invalid_argument("SDF background tools require native Float/Double level sets");
    if(realOnly && !realType)throw std::invalid_argument("SDF alpha masks require native Float/Double scalar grids");
    const auto memory = meta->getMetadata<openvdb::Int64Metadata>(
        openvdb::GridBase::META_FILE_MEM_BYTES);
    const auto count = meta->getMetadata<openvdb::Int64Metadata>(
        openvdb::GridBase::META_FILE_VOXEL_COUNT);
    if (!memory || !count || memory->value() < 0 || count->value() < 0 ||
        uint64_t(count->value()) > budget.p.maxVoxelCount)
      throw std::invalid_argument("topology masks require valid recorded "
                                  "memory/voxel statistics within limits");
    budget.check(uintmax_t(memory->value()));
    name = it.gridName();
    found = true;
    if (selection.empty())
      break;
  }
  if (!found)
    throw std::invalid_argument("no supported grid matches topology selector");
  auto grid = file.readGrid(name);
  const auto bytes = visit(*grid, [](const auto &g) {
    return openvdb::tools::memUsageIfLoaded(g.tree(), false);
  });
  add(budget.resident, bytes, budget.p.maxInputBytes,
      "decoded topology trees exceed byte budget");
  add(budget.active, grid->activeVoxelCount(), budget.p.maxVoxelCount,
      "aggregate topology active voxels exceed maxVoxelCount");
  const auto leaves = grid->baseTree().leafCount();
  if (leaves > budget.p.maxVoxelCount / 512)
    throw std::invalid_argument(
        "stored topology leaf slots exceed maxVoxelCount");
  add(budget.slots, leaves * 512, budget.p.maxVoxelCount,
      "aggregate topology leaf slots exceed maxVoxelCount");
  budget.check();
  grid->readNonresidentBuffers();
  if (grid->memUsage() > bytes)
    throw std::runtime_error(
        "decoded topology source exceeded preflight estimate");
  // Worst default 5/4/3 internal slots plus leaf slots: count sparse node
  // visits conservatively before mask construction, independent of values.
  budget.items(multiply(grid->baseTree().nonLeafCount(), 36'864, budget.maxWork,
                        "topology node work exceeds maxWorkItems"));
  budget.items(leaves * 512);
  file.close();
  return grid;
}
double affine(const openvdb::GridBase &grid) {
  if (!grid.transform().isLinear())
    throw std::invalid_argument(
        "topology masks require finite nonsingular affine maps");
  const auto &map = *grid.transform().baseMap();
  const auto origin = map.applyMap(openvdb::Vec3d(0));
  for (size_t n = 0; n < 3; ++n)
    if (!std::isfinite(origin[n]))
      throw std::invalid_argument("nonfinite topology map origin");
  std::array<openvdb::Vec3d, 3> unit;
  double first = 0;
  for (size_t axis = 0; axis < 3; ++axis) {
    openvdb::Vec3d e(0);
    e[axis] = 1;
    const auto j = map.applyJacobian(e), inverse = map.applyInverseJacobian(e);
    for (size_t n = 0; n < 3; ++n)
      if (!std::isfinite(j[n]) || !std::isfinite(inverse[n]))
        throw std::invalid_argument("nonfinite/singular topology map");
    const auto h = std::hypot(j.x(), j.y(), j.z());
    if (!(h > 0) || !std::isfinite(h))
      throw std::invalid_argument("invalid topology affine axis");
    unit[axis] = j / h;
    if (!axis)
      first = h;
  }
  if (const auto determinant = unit[0].dot(unit[1].cross(unit[2]));
      !std::isfinite(determinant) || determinant == 0)
    throw std::invalid_argument(
        "topology affine map is singular or numerically unresolved");
  return first;
}
void compatible(const openvdb::GridBase &a, const openvdb::GridBase &b) {
  const auto &x = *a.transform().baseMap(), &y = *b.transform().baseMap();
  const auto equal = [](const auto &u, const auto &v) {
    return u.x() == v.x() && u.y() == v.y() && u.z() == v.z();
  };
  if (!equal(x.applyMap(openvdb::Vec3d(0)), y.applyMap(openvdb::Vec3d(0))))
    throw std::invalid_argument(
        "topology Boolean transforms must match exactly; resample explicitly");
  for (size_t axis = 0; axis < 3; ++axis) {
    openvdb::Vec3d e(0);
    e[axis] = 1;
    if (!equal(x.applyJacobian(e), y.applyJacobian(e)))
      throw std::invalid_argument("topology Boolean transforms must match "
                                  "exactly; resample explicitly");
  }
}
openvdb::MaskGrid::Ptr mask(const openvdb::GridBase &source) {
  return visit(source, [](const auto &grid) {
    auto output = openvdb::MaskGrid::create(false);
    output->setTree(std::make_shared<openvdb::MaskTree>(
        grid.tree(), false, true, openvdb::TopologyCopy()));
    return output;
  });
}
int64_t block(int64_t coordinate, int64_t size) {
  const auto quotient = coordinate / size;
  return quotient - (coordinate % size < 0);
}
struct Candidates {
  std::set<openvdb::Coord> leaves, lower, upper;
  Budget &budget;
  uintmax_t unchangedTileVoxels{0};
  uintmax_t treeBytes() const {
    using Leaf = openvdb::MaskTree::LeafNodeType;
    using Lower = openvdb::MaskTree::RootNodeType::ChildNodeType::ChildNodeType;
    using Upper = openvdb::MaskTree::RootNodeType::ChildNodeType;
    return uintmax_t(leaves.size()) * sizeof(Leaf) +
           uintmax_t(lower.size()) * sizeof(Lower) +
           uintmax_t(upper.size()) * (sizeof(Upper) + 128) +
           sizeof(openvdb::MaskTree);
  }
  uintmax_t catalogBytes() const {
    return uintmax_t(leaves.size() + lower.size() + upper.size()) * 128;
  }
  void insert(const openvdb::Coord &coordinate) {
    budget.items(1);
    if (leaves.contains(coordinate))
      return;
    const auto cap = budget.p.maxVoxelCount;
    if (leaves.size() >= cap / 512)
      throw std::invalid_argument(
          "prospective topology morphology leaf slots exceed maxVoxelCount");
    const openvdb::Coord l(int32_t(block(coordinate.x(), 128) * 128),
                           int32_t(block(coordinate.y(), 128) * 128),
                           int32_t(block(coordinate.z(), 128) * 128));
    const openvdb::Coord u(int32_t(block(coordinate.x(), 4096) * 4096),
                           int32_t(block(coordinate.y(), 4096) * 4096),
                           int32_t(block(coordinate.z(), 4096) * 4096));
    using Leaf = openvdb::MaskTree::LeafNodeType;
    using Lower = openvdb::MaskTree::RootNodeType::ChildNodeType::ChildNodeType;
    using Upper = openvdb::MaskTree::RootNodeType::ChildNodeType;
    const auto prospective = treeBytes() + sizeof(Leaf) +
                             (lower.contains(l) ? 0 : sizeof(Lower)) +
                             (upper.contains(u) ? 0 : sizeof(Upper) + 128);
    budget.check(prospective, catalogBytes() + 384);
    leaves.insert(coordinate);
    lower.insert(l);
    upper.insert(u);
  }
  void box(const openvdb::CoordBBox &box, uint32_t radius) {
    std::array<int64_t, 3> lo, hi;
    for (size_t n = 0; n < 3; ++n) {
      const int64_t minimum = int64_t(box.min()[n]) - radius,
                    maximum = int64_t(box.max()[n]) + radius;
      if (minimum < INT32_MIN + 8ll || maximum > INT32_MAX - 8ll)
        throw std::invalid_argument(
            "topology morphology exceeds safe index-coordinate bounds");
      lo[n] = block(minimum, 8);
      hi[n] = block(maximum, 8);
    }
    uintmax_t slots = 1;
    for (size_t n = 0; n < 3; ++n)
      slots = multiply(
          slots, uintmax_t(hi[n] - lo[n] + 1), budget.p.maxVoxelCount / 512,
          "prospective topology tile expansion exceeds maxVoxelCount");
    (void)slots;
    for (auto z = lo[2]; z <= hi[2]; ++z)
      for (auto y = lo[1]; y <= hi[1]; ++y)
        for (auto x = lo[0]; x <= hi[0]; ++x)
          insert(
              openvdb::Coord(int32_t(x * 8), int32_t(y * 8), int32_t(z * 8)));
  }
};
Candidates prepare(const openvdb::GridBase &source, TopologyOperation operation,
                   const TopologyOptions &o, Budget &budget) {
  Candidates candidates{{}, {}, {}, budget};
  const uint32_t phases = (operation == TopologyOperation::Open ||
                           operation == TopologyOperation::Close)
                              ? 2
                              : 1;
  // Erosion never expands beyond the input. Open may redilate eroded leaves;
  // Close expands once then erodes. A phase-sum radius also covers native
  // scratch allocation at intermediate steps and newly pruned tiles.
  const bool erosion = operation == TopologyOperation::Erode ||
                       operation == TopologyOperation::Open ||
                       operation == TopologyOperation::Close;
  // Native 18/26-neighbor erosion constructs an inverse-dilated wavefront
  // with one initial dilation before its requested iterations. Reserve that
  // additional scratch halo as well as both phases' ordinary reach.
  const uint32_t radius =
      o.iterations * phases +
      (erosion && o.connectivity != TopologyConnectivity::Faces6 ? 1 : 0);
  visit(source, [&](const auto &grid) {
    for (auto leaf = grid.tree().cbeginLeaf(); leaf; ++leaf)
      candidates.box(leaf->getNodeBoundingBox(),
                     leaf->onVoxelCount() ? radius : 0);
    for (auto value = grid.cbeginValueOn(); value; ++value) {
      budget.items(1);
      if (value.isVoxelValue())
        continue;
      if (o.tilePolicy == TopologyTilePolicy::Ignore)
        add(candidates.unchangedTileVoxels, value.getVoxelCount(),
            budget.p.maxVoxelCount,
            "unchanged topology active tiles exceed voxel limit");
      else {
        openvdb::CoordBBox bounds;
        value.getBoundingBox(bounds);
        candidates.box(bounds, radius);
      }
    }
    return true;
  });
  uintmax_t possible = candidates.unchangedTileVoxels;
  add(possible,
      multiply(candidates.leaves.size(), 512, budget.p.maxVoxelCount,
               "prospective topology slots exceed maxVoxelCount"),
      budget.p.maxVoxelCount,
      "prospective topology active voxels exceed maxVoxelCount");
  const uint64_t neighbors =
      o.connectivity == TopologyConnectivity::Faces6         ? 6
      : o.connectivity == TopologyConnectivity::FacesEdges18 ? 18
                                                             : 26;
  auto work = multiply(candidates.leaves.size(), 512, budget.maxWork,
                       "topology morphology work exceeds maxWorkItems");
  for (const auto factor :
       {neighbors, uint64_t(o.iterations), uint64_t(phases), uint64_t(4)})
    work = multiply(work, factor, budget.maxWork,
                    "topology morphology work exceeds maxWorkItems");
  budget.items(work);
  budget.check(candidates.treeBytes(), candidates.catalogBytes());
  return candidates;
}
void metadata(openvdb::MaskGrid &output, const openvdb::GridBase &source,
              const std::string &operation, const ProcessingOptions &p) {
  output.insertMeta(source);
  detail::RemoveBoundaryProjectionMetadata(output);
  output.setTransform(detail::CopyTransformExact(source.transform()));
  for (const auto key :
       {openvdb::GridBase::META_GRID_CLASS, openvdb::GridBase::META_VECTOR_TYPE,
        openvdb::GridBase::META_IS_LOCAL_SPACE,
        openvdb::GridBase::META_GRID_NAME,
        openvdb::GridBase::META_FILE_BBOX_MIN,
        openvdb::GridBase::META_FILE_BBOX_MAX,
        openvdb::GridBase::META_FILE_MEM_BYTES,
        openvdb::GridBase::META_FILE_VOXEL_COUNT})
    output.removeMeta(key);
  for (const auto key :
       {"container:mac_layout", "container:velocity_units",
        "container:projection_dt", "container:pressure_gauge",
        "container:pressure_units", "container:point_count",
        "container:point_empty_manifest_v1",
        "container:point_empty_descriptor_v1",
        "container:point_empty_leaf_flags_v1", "container:topology_operation",
        "container:topology_source_grid", "container:topology_occupancy"})
    output.removeMeta(key);
  output.setName(p.gridName);
  output.setGridClass(openvdb::GRID_UNKNOWN);
  output.setVectorType(openvdb::VEC_INVARIANT);
  output.setIsInWorldSpace(true);
  output.setSaveFloatAsHalf(false);
  output.insertMeta("container:topology_operation",
                    openvdb::StringMetadata(operation));
  output.insertMeta("container:topology_source_grid",
                    openvdb::StringMetadata(source.getName()));
  output.insertMeta("container:topology_occupancy",
                    openvdb::StringMetadata(
                        "active-state-only; stored values/background ignored"));
}
TopologyResult publish(const std::filesystem::path &output,
                       openvdb::MaskGrid::Ptr result,
                       const openvdb::GridBase &a, const openvdb::GridBase *b,
                       const std::string &operation, Budget &budget, double h) {
  if (result->activeVoxelCount() > budget.p.maxVoxelCount ||
      result->tree().leafCount() > budget.p.maxVoxelCount / 512)
    throw std::runtime_error(
        "topology output exceeded preflight voxel/leaf limits");
  metadata(*result, a, operation, budget.p);
  // Mask leaves derive values from their activity mask, but internal/root
  // tiles retain a separate Boolean value. Topology operators only promise
  // active states, so restore the documented value=activity invariant without
  // voxelizing compact tiles or changing their active states.
  for (auto value = result->beginValueAll(); value; ++value) {
    budget.items(1);
    if (!value.isVoxelValue())
      value.setValue(value.isValueOn());
  }
  TopologyResult report;
  report.grid = {result->getName(), "unknown", result->activeVoxelCount(), h};
  report.inputActiveVoxelCountA = a.activeVoxelCount();
  report.inputActiveVoxelCountB = b ? b->activeVoxelCount() : 0;
  report.storedLeafCount = result->tree().leafCount();
  for (auto value = result->cbeginValueOn(); value; ++value) {
    budget.items(1);
    if (!value.isVoxelValue())
      ++report.activeTileCount;
  }
  report.workItems = budget.work;
  report.diagnostics.emplace_back(
      "mask occupancy uses active states only; stored values, background and "
      "SDF interior signs are ignored");
  report.diagnostics.emplace_back(
      "source A custom metadata retained; native semantic/statistical tags "
      "regenerated for MaskGrid");
  if (b)
    report.diagnostics.emplace_back(
        "source B metadata is not merged; affine coefficients match exactly");
  budget.check();
  detail::WriteFileExclusive(output, {result}, budget.p.maxInputBytes,
                             openvdb::io::COMPRESS_ZIP);
  return report;
}
void initialize() {
#ifndef OPENVDB_USE_ZLIB
  throw std::invalid_argument(
      "topology masks require an OpenVDB build with ZIP/Zlib compression");
#endif
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });
  if (!openvdb::io::Archive::isDelayedLoadingEnabled())
    throw std::invalid_argument("bounded topology masks require delayed "
                                "loading; unset OPENVDB_DISABLE_DELAYED_LOAD");
}
const char *name(TopologyOperation operation) {
  switch (operation) {
  case TopologyOperation::ActiveMask:
    return "active-mask";
  case TopologyOperation::Dilate:
    return "dilate";
  case TopologyOperation::Erode:
    return "erode";
  case TopologyOperation::Open:
    return "open";
  case TopologyOperation::Close:
    return "close";
  }
  return "unknown";
}
#include "OpenVdbValueTopology.inl"
#include "OpenVdbMaskedValues.inl"
#include "OpenVdbSdfBackground.inl"
#include "OpenVdbSdfFilters.inl"
#endif
} // namespace

TopologyResult WriteTopologyMask(const std::filesystem::path &input,
                                 const std::filesystem::path &output,
                                 TopologyOperation operation,
                                 const TopologyOptions &o,
                                 const std::string &selection,
                                 const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validate(operation, o, p);
  outputPath(output);
  initialize();
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument(
      "bounded topology masks require delayed-loading support");
#else
  tbb::task_arena arena(1);
  return arena.execute([&] {
    Budget budget{p, o.maxWorkItems};
    const auto bytes = inputBytes(input, budget);
    Temporary temporary(output);
    snapshot(input, temporary.directory / "a.vdb", bytes);
    auto source = load(temporary.directory / "a.vdb", selection, budget);
    const auto h = affine(*source);
    // Candidate sets live through morphology; their allocation is charged
    // before each insertion and before any native topology expansion.
    std::optional<Candidates> candidates;
    if (operation != TopologyOperation::ActiveMask)
      candidates.emplace(prepare(*source, operation, o, budget));
    budget.check();
    auto result = mask(*source);
    if (operation != TopologyOperation::ActiveMask) {
      const auto nn = o.connectivity == TopologyConnectivity::Faces6
                          ? openvdb::tools::NN_FACE
                      : o.connectivity == TopologyConnectivity::FacesEdges18
                          ? openvdb::tools::NN_FACE_EDGE
                          : openvdb::tools::NN_FACE_EDGE_VERTEX;
      const auto tiles = o.tilePolicy == TopologyTilePolicy::Preserve
                             ? openvdb::tools::PRESERVE_TILES
                         : o.tilePolicy == TopologyTilePolicy::Expand
                             ? openvdb::tools::EXPAND_TILES
                             : openvdb::tools::IGNORE_TILES;
      const auto dilate = [&] {
        openvdb::tools::dilateActiveValues(result->tree(), int(o.iterations),
                                           nn, tiles, false);
      };
      const auto erode = [&] {
        openvdb::tools::erodeActiveValues(result->tree(), int(o.iterations), nn,
                                          tiles, false);
      };
      if (operation == TopologyOperation::Dilate)
        dilate();
      else if (operation == TopologyOperation::Erode)
        erode();
      else if (operation == TopologyOperation::Open) {
        erode();
        dilate();
      } else {
        dilate();
        erode();
      }
    }
    return publish(output, result, *source, nullptr, name(operation), budget,
                   h);
  });
#endif
#else
  (void)input;
  (void)output;
  (void)operation;
  (void)o;
  (void)selection;
  (void)p;
  unavailable();
#endif
}
TopologyResult WriteTopologyBoolean(
    const std::filesystem::path &inputA, const std::filesystem::path &inputB,
    const std::filesystem::path &output, TopologyBoolean operation,
    const std::string &gridA, const std::string &gridB,
    const ProcessingOptions &p, uint64_t maxWork) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validate(p, maxWork);
  if (operation != TopologyBoolean::Union &&
      operation != TopologyBoolean::Intersection &&
      operation != TopologyBoolean::Difference)
    throw std::invalid_argument("invalid topology Boolean operation");
  outputPath(output);
  initialize();
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument(
      "bounded topology masks require delayed-loading support");
#else
  tbb::task_arena arena(1);
  return arena.execute([&] {
    Budget budget{p, maxWork};
    const auto aBytes = inputBytes(inputA, budget),
               bBytes = inputBytes(inputB, budget);
    Temporary temporary(output);
    snapshot(inputA, temporary.directory / "a.vdb", aBytes);
    snapshot(inputB, temporary.directory / "b.vdb", bBytes);
    auto a = load(temporary.directory / "a.vdb", gridA, budget),
         b = load(temporary.directory / "b.vdb", gridB, budget);
    const auto h = affine(*a);
    (void)affine(*b);
    compatible(*a, *b);
    budget.items(budget.work);
    budget.check();
    auto result = mask(*a);
    auto other = mask(*b);
    const char *operationName;
    if (operation == TopologyBoolean::Union) {
      result->tree().topologyUnion(other->tree());
      operationName = "union";
    } else if (operation == TopologyBoolean::Intersection) {
      result->tree().topologyIntersection(other->tree());
      operationName = "intersection";
    } else {
      result->tree().topologyDifference(other->tree());
      operationName = "difference";
    }
    return publish(output, result, *a, b.get(), operationName, budget, h);
  });
#endif
#else
  (void)inputA;
  (void)inputB;
  (void)output;
  (void)operation;
  (void)gridA;
  (void)gridB;
  (void)p;
  (void)maxWork;
  unavailable();
#endif
}
TopologyResult WriteValueTopology(
    const std::filesystem::path &input, const std::filesystem::path &output,
    ValueTopologyOperation operation, const ValueTopologyOptions &o,
    const std::string &selection, const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateValueTopology(operation, o, p);
  outputPath(output);
  initialize();
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument("bounded value topology requires delayed-loading support");
#else
  tbb::task_arena arena(1);
  return arena.execute([&] {
    Budget budget{p, o.maxWorkItems};
    const auto bytes = inputBytes(input, budget);
    Temporary temporary(std::filesystem::temp_directory_path() / "unused");
    snapshot(input, temporary.directory / "a.vdb", bytes);
    openvdb::MetaMap::Ptr fileMetadata;
    const auto source = load(temporary.directory / "a.vdb", selection, budget, &fileMetadata, true);
    const auto h = affine(*source);
    return visit(*source, [&](const auto &grid) -> TopologyResult {
      using Grid = std::decay_t<decltype(grid)>;
      if constexpr (std::is_same_v<Grid, openvdb::MaskGrid>)
        throw std::invalid_argument("MaskGrid has no independent stored leaf values; use topology masks");
      else
        return editValues(grid, *fileMetadata, output, operation, o, budget, h, temporary.directory / "b.vdb");
    });
  });
#endif
#else
  (void)input; (void)output; (void)operation; (void)o; (void)selection; (void)p;
  unavailable();
#endif
}
TopologyResult WriteMaskedValues(
    const std::filesystem::path &input, const std::filesystem::path &maskInput,
    const std::filesystem::path &output, MaskedValuesOperation operation,
    const std::string &inputGrid, const std::string &maskGrid,
    const ProcessingOptions &p, uint64_t maxWorkItems) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validate(p,maxWorkItems);
  if(operation<MaskedValuesOperation::Activate || operation>MaskedValuesOperation::ClipOutside)
    throw std::invalid_argument("invalid native masked value operation");
  outputPath(output); initialize();
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument("bounded masked values require delayed-loading support");
#else
  tbb::task_arena arena(1);
  return arena.execute([&] {
    Budget budget{p,maxWorkItems};
    const auto aBytes=inputBytes(input,budget),bBytes=inputBytes(maskInput,budget);
    Temporary temporary(std::filesystem::temp_directory_path()/"unused");
    Temporary destination(std::filesystem::temp_directory_path()/"unused");
    snapshot(input,temporary.directory/"a.vdb",aBytes);
    snapshot(maskInput,temporary.directory/"b.vdb",bBytes);
    openvdb::MetaMap::Ptr fileMeta;
    auto a=load(temporary.directory/"a.vdb",inputGrid,budget,&fileMeta,true);
    auto b=load(temporary.directory/"b.vdb",maskGrid,budget);
    const auto h=affine(*a); (void)affine(*b); compatible(*a,*b);
    return visit(*a,[&](const auto &source)->TopologyResult {
      using Grid=std::decay_t<decltype(source)>;
      if constexpr(std::is_same_v<Grid,openvdb::MaskGrid>)
        throw std::invalid_argument("MaskGrid has no independent stored leaf values");
      else return maskedValues(source,*b,*fileMeta,output,operation,budget,h,destination.directory/"b.vdb");
    });
  });
#endif
#else
  (void)input; (void)maskInput; (void)output; (void)operation; (void)inputGrid;
  (void)maskGrid; (void)p; (void)maxWorkItems; unavailable();
#endif
}
TopologyResult WriteSdfBackground(
    const std::filesystem::path &input,const std::filesystem::path &output,
    SdfBackgroundOperation operation,const SdfBackgroundOptions &o,
    const std::string &inputGrid,const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validate(p,o.maxWorkItems);
  if(operation!=SdfBackgroundOperation::BackgroundOnly && operation!=SdfBackgroundOperation::FloodFill)
    throw std::invalid_argument("invalid SDF background operation");
  for(const auto width:{o.outsideWidth,o.insideWidth})
    if(width && (!std::isfinite(*width) || !(*width>0)))
      throw std::invalid_argument("SDF background widths must be finite positive world distances");
  outputPath(output); initialize();
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument("bounded SDF background tools require delayed-loading support");
#else
  tbb::task_arena arena(1);
  return arena.execute([&] {
    Budget budget{p,o.maxWorkItems};
    const auto size=inputBytes(input,budget);
    Temporary temporary(std::filesystem::temp_directory_path()/"unused");
    snapshot(input,temporary.directory/"a.vdb",size);
    openvdb::MetaMap::Ptr fileMeta;
    auto source=load(temporary.directory/"a.vdb",inputGrid,budget,&fileMeta,true,true);
    const auto h=affine(*source);
    if(source->isType<openvdb::FloatGrid>())
      return sdfBackground(*openvdb::gridPtrCast<openvdb::FloatGrid>(source),*fileMeta,output,operation,o,budget,h,temporary.directory/"b.vdb");
    if(source->isType<openvdb::DoubleGrid>())
      return sdfBackground(*openvdb::gridPtrCast<openvdb::DoubleGrid>(source),*fileMeta,output,operation,o,budget,h,temporary.directory/"b.vdb");
    throw std::invalid_argument("SDF background tools require native Float/Double level sets");
  });
#endif
#else
  (void)input; (void)output; (void)operation; (void)o; (void)inputGrid; (void)p; unavailable();
#endif
}
SdfFilterResult WriteSdfFiltered(
    const std::filesystem::path& input,const std::filesystem::path& output,
    SdfFilterOperation operation,const SdfFilterOptions& o,
    const std::string& inputGrid,const ProcessingOptions& p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateSdfFilter(operation,o,p);outputPath(output);initialize();
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument("bounded SDF filters require delayed-loading support");
#else
  tbb::task_arena arena(1);
  return arena.execute([&] {
    Budget budget{p,o.maxWorkItems};
    const auto bytes=inputBytes(input,budget);
    const auto maskPath=o.maskInput.empty()?o.normalizationMaskInput:o.maskInput;
    const auto maskBytes=maskPath.empty()?0:inputBytes(maskPath,budget);
    Temporary temporary(std::filesystem::temp_directory_path()/"unused");
    Temporary destination(std::filesystem::temp_directory_path()/"unused");
    snapshot(input,temporary.directory/"a.vdb",bytes);
    openvdb::MetaMap::Ptr fileMetadata;
    const auto source=load(temporary.directory/"a.vdb",inputGrid,budget,&fileMetadata,true,true);
    const double h=affine(*source);
    openvdb::GridBase::Ptr mask;
    if(!maskPath.empty()) {
      snapshot(maskPath,temporary.directory/"b.vdb",maskBytes);
      const bool alpha=o.normalizationMaskInput.empty();
      mask=load(temporary.directory/"b.vdb",alpha?o.maskGrid:o.normalizationMaskGrid,budget,nullptr,alpha,false,alpha);
      if(alpha && !mask->isType<openvdb::FloatGrid>() && !mask->isType<openvdb::DoubleGrid>())
        throw std::invalid_argument("SDF alpha masks require native Float/Double scalar grids");
      (void)affine(*mask);compatible(*source,*mask);
      if(alpha) {
        if(mask->isType<openvdb::FloatGrid>())sdfValidateAlpha(static_cast<const openvdb::FloatGrid&>(*mask),o,budget);
        else sdfValidateAlpha(static_cast<const openvdb::DoubleGrid&>(*mask),o,budget);
      }
    }
    if(source->isType<openvdb::FloatGrid>())
      return sdfFiltered(*openvdb::gridPtrCast<openvdb::FloatGrid>(source),mask.get(),*fileMetadata,
          output,operation,o,budget,h,destination.directory/"b.vdb");
    if(source->isType<openvdb::DoubleGrid>())
      return sdfFiltered(*openvdb::gridPtrCast<openvdb::DoubleGrid>(source),mask.get(),*fileMetadata,
          output,operation,o,budget,h,destination.directory/"b.vdb");
    throw std::invalid_argument("SDF filters require native Float/Double level sets");
  });
#endif
#else
  (void)input;(void)output;(void)operation;(void)o;(void)inputGrid;(void)p;unavailable();
#endif
}
} // namespace container::geometry::vdb
