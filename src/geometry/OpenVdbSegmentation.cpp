#include "Container/geometry/OpenVdbSegmentation.h"
#include "OpenVdbProvenance.h"
#include "Container/geometry/OpenVdbGridSelection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/tools/LevelSetUtil.h>
#include <tbb/task_arena.h>

#include <iomanip>
#include <mutex>
#include <sstream>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error(
      "OpenVDB processing is unavailable in this build. Install the OpenVDB "
      "dependencies and configure with -DENABLE_OPENVDB=ON.");
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using Tree = openvdb::FloatTree;
using Leaf = Tree::LeafNodeType;
using Upper = Tree::RootNodeType::ChildNodeType;
using Lower = Upper::ChildNodeType;
using BoolTree = Tree::ValueConverter<bool>::Type;
using BoolLeaf = BoolTree::LeafNodeType;
using BoolUpper = BoolTree::RootNodeType::ChildNodeType;
using BoolLower = BoolUpper::ChildNodeType;

void validateOptions(const ProcessingOptions& options, const SegmentationOptions& segmentation) {
  if (options.maxVoxelCount == 0 || options.maxInputBytes == 0)
    throw std::invalid_argument("segmentation budgets must be positive");
  if (segmentation.maxComponents == 0 || segmentation.maxComponents > 1024)
    throw std::invalid_argument("max components must be in [1,1024]");
  if (options.gridName.empty() || options.gridName.size() > 256)
    throw std::invalid_argument("output segmentation grid name must contain 1..256 bytes");
}

void validateOutput(const std::filesystem::path& output) {
  if (output.empty()) throw std::invalid_argument("output path must not be empty");
  if (std::filesystem::exists(output))
    throw std::invalid_argument("output already exists; choose a new .vdb path");
  if (!output.parent_path().empty() && !std::filesystem::is_directory(output.parent_path()))
    throw std::invalid_argument("output directory does not exist");
}

uint64_t boxCount(const openvdb::CoordBBox& bounds, uint64_t dimension = 1) {
  auto block = [dimension](int32_t value) {
    const int64_t v = value, dim = static_cast<int64_t>(dimension);
    return v >= 0 ? v / dim : (v + 1) / dim - 1;
  };
  uint64_t count = 1;
  for (size_t axis = 0; axis < 3; ++axis) {
    const auto extent = static_cast<uint64_t>(block(bounds.max()[axis]) - block(bounds.min()[axis]) + 1);
    if (extent > std::numeric_limits<uint64_t>::max() / count)
      throw std::invalid_argument("segmentation bounds exceed the voxel-count range");
    count *= extent;
  }
  return count;
}

struct Topology {
  openvdb::CoordBBox bounds;
  long double floatBytes{0}, boolBytes{0};
  long double metadataBytes{0};
  uint64_t leafVoxels{0};
};

long double metadataBytes(const openvdb::MetaMap& metadata) {
  long double bytes = sizeof(openvdb::MetaMap);
  for (auto it = metadata.beginMeta(); it != metadata.endMeta(); ++it) {
    // Grid::memUsage() only measures its tree. insertMeta deep-copies fields
    // for every segment, and the writer copies metadata for file statistics.
    // Account string capacity and map/shared-ownership allocation overhead
    // without allocating a textual representation of the value.
    bytes += 2.0L * (it->first.size() + it->second->typeName().size()) +
             2.0L * it->second->size() + 256;
  }
  return bytes;
}

Topology validateGrid(const openvdb::FloatGrid& grid, const ProcessingOptions& options) {
  if (grid.getGridClass() != openvdb::GRID_LEVEL_SET ||
      !std::isfinite(grid.background()) || grid.background() <= 0)
    throw std::invalid_argument("segmentation requires a FloatGrid level set with positive finite background");
  if (!grid.transform().isLinear())
    throw std::invalid_argument("segmentation requires an affine grid transform");
  const auto origin = grid.indexToWorld(openvdb::Vec3d(0));
  std::array<openvdb::Vec3d, 3> basis{
      grid.indexToWorld(openvdb::Vec3d(1,0,0)) - origin,
      grid.indexToWorld(openvdb::Vec3d(0,1,0)) - origin,
      grid.indexToWorld(openvdb::Vec3d(0,0,1)) - origin};
  std::array<double, 3> lengths{};
  for (size_t axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(origin[axis]))
      throw std::invalid_argument("segmentation requires a finite affine transform");
    lengths[axis] = std::hypot(basis[axis].x(), basis[axis].y(), basis[axis].z());
    if (!std::isfinite(lengths[axis]) || lengths[axis] <= 0)
      throw std::invalid_argument("segmentation requires a finite nonsingular transform");
    basis[axis] /= lengths[axis];
  }
  constexpr double tolerance = 1e-6;
  if (!grid.hasUniformVoxels() || std::abs(lengths[1] / lengths[0] - 1) > tolerance ||
      std::abs(lengths[2] / lengths[0] - 1) > tolerance ||
      std::abs(basis[0].dot(basis[1])) > tolerance ||
      std::abs(basis[0].dot(basis[2])) > tolerance ||
      std::abs(basis[1].dot(basis[2])) > tolerance)
    throw std::invalid_argument("segmentation requires uniform voxels; resample the input first");
  const double dx = grid.voxelSize().x();
  if (!std::isfinite(dx) || dx < 1e-6 || dx > 1e6)
    throw std::invalid_argument("segmentation requires source voxel size in [1e-6,1e6] world units");
  const double halfWidth = double(grid.background()) / dx;
  if (!std::isfinite(halfWidth) || halfWidth < 3 - tolerance || halfWidth > 64 + tolerance)
    throw std::invalid_argument("segmentation requires a band half width in [3,64] voxels");
  if (grid.empty()) throw std::invalid_argument("cannot segment an empty level set");
  if (grid.tree().hasActiveTiles())
    throw std::invalid_argument("segmentation requires a voxelized narrow band without active tiles");
  const auto leaves = uint64_t(grid.tree().leafCount());
  const auto maxLeaves = options.maxVoxelCount / Leaf::NUM_VALUES +
      (options.maxVoxelCount % Leaf::NUM_VALUES != 0);
  const auto metadataMemory = metadataBytes(grid);
  if (grid.activeVoxelCount() > options.maxVoxelCount || leaves > maxLeaves ||
      static_cast<long double>(grid.memUsage()) + metadataMemory > options.maxInputBytes)
    throw std::invalid_argument("input segmentation topology exceeds the voxel or input-memory budget");
  auto bounds = grid.evalActiveVoxelBoundingBox();
  for (auto value = grid.cbeginValueAll(); value; ++value) {
    if (!std::isfinite(*value)) throw std::invalid_argument("level set contains a non-finite scalar value");
    if (std::abs(double(*value)) > double(grid.background()) * (1 + tolerance))
      throw std::invalid_argument("segmentation requires stored distances within the narrow band");
    if (*value != grid.background()) bounds.expand(value.getBoundingBox());
  }
  for (auto leaf = grid.tree().cbeginLeaf(); leaf; ++leaf) {
    const auto box = leaf->getNodeBoundingBox();
    for (size_t axis = 0; axis < 3; ++axis)
      if (int64_t(box.min()[axis]) < int64_t(std::numeric_limits<int32_t>::min()) + 16 ||
          int64_t(box.max()[axis]) > int64_t(std::numeric_limits<int32_t>::max()) - 16)
        throw std::invalid_argument("segmentation topology exceeds safe OpenVDB coordinate limits");
  }
  if (bounds.empty() || boxCount(bounds) > options.maxVoxelCount)
    throw std::invalid_argument("segmentation bounds exceed the voxel budget");
  const auto leafCount = boxCount(bounds, Leaf::DIM);
  const auto lowerCount = boxCount(bounds, Lower::DIM);
  const auto upperCount = boxCount(bounds, Upper::DIM);
  Topology result;
  result.bounds = bounds;
  result.metadataBytes = metadataMemory;
  result.leafVoxels = leaves * Leaf::NUM_VALUES;
  result.floatBytes = leafCount * (sizeof(Leaf) + Leaf::NUM_VALUES * sizeof(float) + 128.0L) +
      lowerCount * (sizeof(Lower) + 128.0L) + upperCount * (sizeof(Upper) + 384.0L);
  result.boolBytes = leafCount * (sizeof(BoolLeaf) + BoolLeaf::NUM_VALUES / 8 + 128.0L) +
      lowerCount * (sizeof(BoolLower) + 128.0L) + upperCount * (sizeof(BoolUpper) + 384.0L);
  return result;
}

openvdb::FloatGrid::Ptr readGrid(const std::filesystem::path& input,
                               const std::string& selection, const ProcessingOptions& options,
                               uintmax_t& encodedBytes) {
  if (!std::filesystem::is_regular_file(input)) throw std::invalid_argument("input is not a regular file");
  encodedBytes = std::filesystem::file_size(input);
  if (encodedBytes > options.maxInputBytes)
    throw std::invalid_argument("segmentation input exceeds the file-size budget");
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });
  openvdb::io::File file(input.string());
  file.open(false);
  std::string descriptor;
  bool found = false;
  for (auto it = file.beginName(); it != file.endName(); ++it) {
    const auto metadata = file.readGridMetadata(it.gridName());
    if (!GridSelectionMatches(selection, it.gridName(), metadata->getName()) ||
        !metadata->isType<openvdb::FloatGrid>() || metadata->getGridClass() != openvdb::GRID_LEVEL_SET)
      continue;
    if (found) throw std::invalid_argument("input grid name is ambiguous; use descriptor:<exact descriptor name>");
    if (const auto count = metadata->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_VOXEL_COUNT);
        count && (count->value() < 0 || uint64_t(count->value()) > options.maxVoxelCount))
      throw std::invalid_argument("segmentation input metadata exceeds the voxel budget");
    if (const auto bytes = metadata->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_MEM_BYTES);
        bytes && (bytes->value() < 0 ||
            static_cast<long double>(encodedBytes) + bytes->value() > options.maxInputBytes))
      throw std::invalid_argument("segmentation input metadata exceeds the input-memory budget");
    descriptor = it.gridName();
    found = true;
    if (selection.empty()) break;
  }
  if (!found) throw std::invalid_argument("no matching FloatGrid level set found in input");
  auto grid = openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid(descriptor));
  file.close();
  return grid;
}

struct MaskStats {
  uint64_t voxels{0}, fragments{0}, squaredFragments{0};
  uint32_t components{0};
};

uint64_t localFragments(const BoolLeaf& leaf) {
  auto remaining = leaf.getValueMask();
  std::array<openvdb::Index, BoolLeaf::NUM_VALUES> queue{};
  uint64_t fragments = 0;
  while (!remaining.isOff()) {
    ++fragments;
    size_t queued = 0;
    const auto first = remaining.findFirstOn();
    remaining.setOff(first);
    queue[queued++] = first;
    while (queued) {
      const auto offset = queue[--queued];
      const auto coordinate = BoolLeaf::offsetToLocalCoord(offset);
      for (size_t axis = 0; axis < 3; ++axis) {
        for (int step : {-1, 1}) {
          auto next = coordinate;
          next[axis] += step;
          if (next[axis] < 0 || next[axis] >= int32_t(BoolLeaf::DIM)) continue;
          const auto nextOffset = BoolLeaf::coordToOffset(next);
          if (!remaining.isOn(nextOffset)) continue;
          remaining.setOff(nextOffset);
          queue[queued++] = nextOffset;
        }
      }
    }
  }
  return fragments;
}

MaskStats countMask(BoolTree& mask, const SegmentationOptions& options) {
  MaskStats stats;
  stats.voxels = mask.activeVoxelCount();
  if (stats.voxels == 0) throw std::invalid_argument("level set has no zero-crossing surface to segment");
  for (auto leaf = mask.cbeginLeaf(); leaf; ++leaf) {
    const auto fragments = localFragments(*leaf);
    stats.fragments += fragments;
    stats.squaredFragments += fragments * fragments;
  }
  if (stats.voxels > std::numeric_limits<size_t>::max() / sizeof(openvdb::Coord))
    throw std::invalid_argument("segmentation traversal exceeds the allocation-size range");
  std::vector<openvdb::Coord> queue;
  queue.reserve(static_cast<size_t>(stats.voxels));
  openvdb::tree::ValueAccessor<BoolTree> access(mask);
  for (auto leaf = mask.beginLeaf(); leaf; ++leaf) {
    for (auto value = leaf->beginValueOn(); value; ++value) {
      if (++stats.components > options.maxComponents)
        throw std::invalid_argument("level-set surface count exceeds --max-components");
      const auto first = value.getCoord();
      access.setValueOff(first, false);
      queue.push_back(first);
      while (!queue.empty()) {
        const auto coordinate = queue.back();
        queue.pop_back();
        for (size_t axis = 0; axis < 3; ++axis) {
          for (int step : {-1, 1}) {
            auto next = coordinate;
            next[axis] += step;
            if (!access.isValueOn(next)) continue;
            access.setValueOff(next, false);
            queue.push_back(next);
          }
        }
      }
    }
  }
  return stats;
}

void preflightMask(const openvdb::FloatGrid& grid, const Topology& topology,
                   uintmax_t encodedBytes, const ProcessingOptions& options) {
  const long double workspace = static_cast<long double>(encodedBytes) + grid.memUsage() +
      topology.metadataBytes +
      4 * topology.boolBytes + static_cast<long double>(topology.leafVoxels) * sizeof(openvdb::Coord) +
      4.0L * 1024 * 1024;
  if (workspace > options.maxInputBytes)
    throw std::invalid_argument("segmentation mask/traversal workspace exceeds the input-memory budget");
}

void preflightSegmentation(const openvdb::FloatGrid& grid, const Topology& topology,
                          const MaskStats& stats, uintmax_t encodedBytes,
                          const ProcessingOptions& options) {
  // The upstream graph retains one fragment per locally connected leaf mask,
  // reserves per-leaf connection vectors, and reserves all local fragments in
  // every global component group. Account these real input-dependent terms.
  // 384 bytes per fragment covers its bit mask, origin, connection vector,
  // shared ownership and allocator bookkeeping without using a private type.
  const long double graphBytes = static_cast<long double>(stats.fragments) * 384 +
      static_cast<long double>(stats.squaredFragments) * sizeof(void*) +
      static_cast<long double>(stats.components) * stats.fragments * sizeof(void*) +
      static_cast<long double>(stats.voxels) * (6 * 64 + sizeof(openvdb::Coord));
  const long double workspace = static_cast<long double>(encodedBytes) + grid.memUsage() +
      topology.metadataBytes + 4 * topology.boolBytes + graphBytes +
      stats.components * (2 * topology.floatBytes + 3 * topology.boolBytes +
                          2 * topology.metadataBytes + 8.0L * (options.gridName.size() + 16)) +
      4.0L * 1024 * 1024;
  if (workspace > options.maxInputBytes)
    throw std::invalid_argument("segmentation component/graph workspace exceeds the input-memory budget");
}

std::array<int32_t, 6> boxKey(const openvdb::FloatGrid& grid) {
  const auto box = grid.evalActiveVoxelBoundingBox();
  return {box.min().x(), box.min().y(), box.min().z(), box.max().x(), box.max().y(), box.max().z()};
}

void validatePositiveExterior(const openvdb::FloatGrid& grid) {
  const auto active = grid.evalActiveVoxelBoundingBox();
  for (auto value = grid.cbeginValueOff(); value; ++value) {
    if (*value >= 0) continue;
    const auto stored = value.getBoundingBox();
    for (size_t axis = 0; axis < 3; ++axis)
      if (stored.min()[axis] < active.min()[axis] || stored.max()[axis] > active.max()[axis])
        throw std::invalid_argument(
            "segmentation cannot preserve a negative-exterior component, such as a nested cavity surface; "
            "isolate or rebuild it as a closed positive-exterior level set first");
  }
}

bool contentLess(const openvdb::FloatGrid& left, const openvdb::FloatGrid& right) {
  auto a = left.cbeginValueAll(), b = right.cbeginValueAll();
  for (; a && b; ++a, ++b) {
    const auto ca = a.getCoord(), cb = b.getCoord();
    for (size_t axis = 0; axis < 3; ++axis)
      if (ca[axis] != cb[axis]) return ca[axis] < cb[axis];
    if (a.getLevel() != b.getLevel()) return a.getLevel() < b.getLevel();
    if (a.isValueOn() != b.isValueOn()) return a.isValueOn() < b.isValueOn();
    if (*a != *b) return *a < *b;
  }
  return !a && bool(b);
}
#endif
}  // namespace

std::vector<ProcessingResult> WriteSegmentedLevelSet(
    const std::filesystem::path& input, const std::filesystem::path& output,
    const std::string& inputGrid, const ProcessingOptions& options,
    const SegmentationOptions& segmentation) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(options, segmentation);
  validateOutput(output);
  tbb::task_arena arena(1);
  return arena.execute([&]() -> std::vector<ProcessingResult> {
    uintmax_t encodedBytes = 0;
    auto grid = readGrid(input, inputGrid, options, encodedBytes);
    const auto topology = validateGrid(*grid, options);
    preflightMask(*grid, topology, encodedBytes, options);
    MaskStats stats;
    {
      auto mask = openvdb::tools::extractIsosurfaceMask(grid->tree(), 0.0f);
      stats = countMask(*mask, segmentation);
    }
    preflightSegmentation(*grid, topology, stats, encodedBytes, options);
    std::vector<openvdb::FloatGrid::Ptr> segments;
    openvdb::tools::segmentSDF(*grid, segments);
    if (segments.size() != stats.components || segments.size() > segmentation.maxComponents)
      throw std::runtime_error("OpenVDB segmentation component count disagrees with the bounded surface prepass");
    uint64_t totalVoxels = 0, totalLeaves = 0;
    long double totalBytes = static_cast<long double>(encodedBytes) + grid->memUsage() + topology.metadataBytes;
    for (const auto& segment : segments) {
      validatePositiveExterior(*segment);
      (void)validateGrid(*segment, options);
      const auto active = uint64_t(segment->activeVoxelCount());
      if (active > options.maxVoxelCount - totalVoxels)
        throw std::invalid_argument("summed segmentation output exceeds the voxel budget");
      totalVoxels += active;
      totalLeaves += segment->tree().leafCount();
      totalBytes += segment->memUsage() + metadataBytes(*segment);
    }
    const auto maxLeaves = options.maxVoxelCount / Leaf::NUM_VALUES +
        (options.maxVoxelCount % Leaf::NUM_VALUES != 0);
    if (totalLeaves > maxLeaves || totalBytes > options.maxInputBytes)
      throw std::invalid_argument("summed segmentation output topology exceeds the voxel or input-memory budget");
    std::sort(segments.begin(), segments.end(), [](const auto& a, const auto& b) {
      const auto ac = a->activeVoxelCount(), bc = b->activeVoxelCount();
      if (ac != bc) return ac > bc;
      const auto ab = boxKey(*a), bb = boxKey(*b);
      if (ab != bb) return ab < bb;
      return contentLess(*a, *b);
    });
    std::vector<ProcessingResult> results;
    openvdb::GridPtrVec outputGrids;
    for (size_t index = 0; index < segments.size(); ++index) {
      std::ostringstream name;
      name << options.gridName << '_' << std::setw(3) << std::setfill('0') << index;
      segments[index]->setName(name.str());
      detail::RemoveBoundaryProjectionMetadata(*segments[index]);
      results.push_back({name.str(), "level set", uint64_t(segments[index]->activeVoxelCount()),
                         segments[index]->voxelSize().x()});
      outputGrids.push_back(segments[index]);
    }
    // Recheck actual renamed metadata and returned name strings before IO.
    totalBytes = static_cast<long double>(encodedBytes) + grid->memUsage() + topology.metadataBytes;
    for (size_t index = 0; index < segments.size(); ++index)
      totalBytes += segments[index]->memUsage() + metadataBytes(*segments[index]) +
                    2.0L * results[index].gridName.size();
    if (totalBytes > options.maxInputBytes)
      throw std::invalid_argument("summed segmentation output metadata exceeds the input-memory budget");
    try {
      openvdb::io::File file(output.string());
      file.write(outputGrids);
      file.close();
    } catch (...) {
      std::error_code ignored;
      std::filesystem::remove(output, ignored);
      throw;
    }
    return results;
  });
#else
  (void)input; (void)output; (void)inputGrid; (void)options; (void)segmentation;
  unavailable();
#endif
}

}  // namespace container::geometry::vdb
