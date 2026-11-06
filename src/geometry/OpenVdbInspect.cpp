#include "Container/geometry/OpenVdbInspect.h"
#include "Container/geometry/OpenVdbGridSelection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbExactStatistics.h"
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/io/GridDescriptor.h>
#include <openvdb/io/io.h>
#include <fstream>
#include <mutex>
#endif

namespace container::geometry::vdb {
namespace {

[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB inspection is unavailable in this build. Configure with -DENABLE_OPENVDB=ON.");
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class MetadataArchive : public openvdb::io::Archive {
public:
  void begin(std::istream& stream, openvdb::io::StreamMetadata::Ptr& metadata) {
    readHeader(stream);
    if (!inputHasGridOffsets())
      throw std::invalid_argument("inspection requires random-access grid offsets; rewrite the streaming VDB as a seekable file");
    if (fileVersion() < openvdb::OPENVDB_FILE_VERSION_GRID_INSTANCING ||
        fileVersion() > openvdb::OPENVDB_FILE_VERSION)
      throw std::invalid_argument("inspection requires a supported VDB format with metadata-readable transforms; rewrite with a current OpenVDB version");
    openvdb::io::setStreamMetadataPtr(stream, metadata, false);
    setFormatVersion(stream);
    setLibraryVersion(stream);
    setDataCompression(stream);
  }
  int32_t count(std::istream& stream) { return readGridCount(stream); }
  openvdb::GridBase::Ptr readMetadata(std::istream& stream,
      openvdb::io::GridDescriptor& descriptor, uintmax_t fileBytes) {
    auto grid = descriptor.read(stream);
    const auto afterDescriptor = static_cast<int64_t>(stream.tellg());
    const auto start = descriptor.getGridPos(), end = descriptor.getEndPos();
    const auto blocks = descriptor.getBlockPos();
    if (!grid || start < afterDescriptor || end <= start ||
        static_cast<uintmax_t>(end) > fileBytes ||
        (!descriptor.isInstance() && (blocks < start || blocks > end)))
      throw std::invalid_argument("inspection grid offsets are outside their encoded file range");
    descriptor.seekToGrid(stream);
    readGridCompression(stream);
    grid->readMeta(stream);
    grid->readTransform(stream);
    const auto afterTransform = static_cast<int64_t>(stream.tellg());
    if (afterTransform > (descriptor.isInstance() ? end : blocks))
      throw std::invalid_argument("inspection metadata/transform crosses the grid payload boundary");
    grid->removeMeta(openvdb::GridBase::META_FILE_DELAYED_LOAD);
    descriptor.seekToEnd(stream);
    return grid;
  }
};

class MetadataBudget {
public:
  explicit MetadataBudget(size_t limit) : remaining_(limit) {}
  void charge(uintmax_t bytes) {
    if (bytes > remaining_)
      throw std::invalid_argument("inspection metadata/report strings exceed maxMetadataBytes");
    remaining_ -= static_cast<size_t>(bytes);
  }
  void charge(const std::string& text) { charge(text.size()); }
private:
  size_t remaining_;
};

[[noreturn]] void gridError(const std::string& descriptor, const std::string& reason) {
  throw std::invalid_argument("Grid '" + descriptor + "': " + reason);
}

std::optional<uint64_t> recordedInteger(const openvdb::GridBase& grid, const char* name) {
  const auto value = grid.getMetadata<openvdb::Int64Metadata>(name);
  if (!value || value->value() < 0) return {};
  return static_cast<uint64_t>(value->value());
}

std::optional<InspectionIndexBounds> recordedBounds(const openvdb::GridBase& grid) {
  const auto low = grid.getMetadata<openvdb::Vec3IMetadata>(openvdb::GridBase::META_FILE_BBOX_MIN);
  const auto high = grid.getMetadata<openvdb::Vec3IMetadata>(openvdb::GridBase::META_FILE_BBOX_MAX);
  if (!low || !high) return {};
  InspectionIndexBounds bounds;
  for (size_t axis = 0; axis < 3; ++axis) {
    bounds.minimum[axis] = low->value()[axis];
    bounds.maximum[axis] = high->value()[axis];
    if (bounds.minimum[axis] > bounds.maximum[axis]) return {};
  }
  return bounds;
}

InspectionIndexBounds indexBounds(const openvdb::CoordBBox& source) {
  InspectionIndexBounds bounds;
  for (size_t axis = 0; axis < 3; ++axis) {
    bounds.minimum[axis] = source.min()[axis];
    bounds.maximum[axis] = source.max()[axis];
  }
  return bounds;
}

std::optional<InspectionWorldBounds> worldBounds(
    const std::optional<InspectionIndexBounds>& bounds,
    const std::optional<std::array<double, 16>>& matrix) {
  if (!bounds || !matrix) return {};
  InspectionWorldBounds result;
  result.minimum.fill(std::numeric_limits<double>::infinity());
  result.maximum.fill(-std::numeric_limits<double>::infinity());
  for (unsigned corner = 0; corner < 8; ++corner) {
    std::array<double, 3> index;
    for (size_t axis = 0; axis < 3; ++axis)
      index[axis] = (corner & (1u << axis)) ? double(bounds->maximum[axis]) + .5
                                           : double(bounds->minimum[axis]) - .5;
    for (size_t axis = 0; axis < 3; ++axis) {
      const double world = (*matrix)[4 * axis] * index[0] +
          (*matrix)[4 * axis + 1] * index[1] + (*matrix)[4 * axis + 2] * index[2] +
          (*matrix)[4 * axis + 3];
      if (!std::isfinite(world)) return {};
      result.minimum[axis] = std::min(result.minimum[axis], world);
      result.maximum[axis] = std::max(result.maximum[axis], world);
    }
  }
  return result;
}

void describeTransform(const openvdb::GridBase& grid, GridInspection& result) {
  result.affine = grid.transform().isLinear();
  if (!result.affine) {
    const auto voxel = grid.voxelSize();
    for (size_t axis = 0; axis < 3; ++axis) {
      result.voxelSize[axis] = voxel[axis];
      if (!std::isfinite(voxel[axis]))
        gridError(result.descriptorName, "transform has non-finite voxel sizes");
    }
    result.transformDiagnostic = "nonlinear transform: affine matrix and cell-world bounds unavailable";
    return;
  }
  std::array<double, 16> matrix{};
  try {
    const auto map=grid.transform().baseMap();
    if(map->isType<openvdb::math::AffineMap>()) {
      const auto source=static_cast<const openvdb::math::AffineMap&>(*map).getMat4();
      if(source(0,3)!=0 || source(1,3)!=0 || source(2,3)!=0 || source(3,3)!=1) {
        result.transformDiagnostic="non-affine homogeneous record: matrix and cell-world bounds unavailable";
        return;
      }
      for(size_t row=0;row<4;++row)for(size_t column=0;column<4;++column)
        matrix[4*row+column]=source(column,row);
    } else {
      // Creating an AffineMap imposes an absolute determinant threshold which
      // rejects well-conditioned persisted scale maps, e.g. uniform 1e-6.
      // Their direct Jacobian/origin supply the same coefficients exactly.
      const auto origin=map->applyMap(openvdb::Vec3d(0));
      for(size_t column=0;column<3;++column) {
        openvdb::Vec3d unit(0);unit[column]=1;
        const auto axis=map->applyJacobian(unit);
        for(size_t row=0;row<3;++row) {
          matrix[4*row+column]=axis[row];matrix[4*row+3]=origin[row];
        }
      }
      matrix[15]=1;
    }
  } catch (const std::exception&) {
    result.transformDiagnostic = "invalid affine transform: matrix and cell-world bounds unavailable";
    return;
  }
  std::array<openvdb::Vec3d, 3> axes;
  bool singular=false;
  for (size_t axis = 0; axis < 3; ++axis) {
    axes[axis] = openvdb::Vec3d(matrix[axis], matrix[4 + axis], matrix[8 + axis]);
    const double length = std::hypot(axes[axis].x(), axes[axis].y(), axes[axis].z());
    // Derive sizes from the linear coefficients, avoiding translated-point
    // subtraction in AffineMap's cached voxelSize() at distant world origins.
    result.voxelSize[axis] = length;
    if(!std::isfinite(length))gridError(result.descriptorName,"transform has non-finite voxel sizes");
    if(length==0)singular=true;else axes[axis]/=length;
  }
  for(const auto value:matrix)if(!std::isfinite(value)) {
    result.transformDiagnostic="non-finite affine transform: matrix and cell-world bounds unavailable";
    return;
  }
  if(singular) {
    result.transformDiagnostic="singular affine transform: matrix and cell-world bounds unavailable";
    return;
  }
  const double determinant = axes[0].dot(axes[1].cross(axes[2]));
  if (!std::isfinite(determinant) || determinant == 0.0) {
    result.transformDiagnostic = "singular affine transform: matrix and cell-world bounds unavailable";
    return;
  }
  result.indexToWorld = matrix;
}

GridInspection describe(const openvdb::GridBase& grid, const std::string& descriptor,
                        MetadataBudget& budget) {
  GridInspection result;
  result.descriptorName = descriptor;
  result.name = grid.getName();
  result.valueType = grid.valueType();
  result.gridClass = openvdb::GridBase::gridClassToString(grid.getGridClass());
  result.vectorType = openvdb::GridBase::vecTypeToString(grid.getVectorType());
  result.inWorldSpace = grid.isInWorldSpace();
  describeTransform(grid, result);
  result.activeVoxelCount = recordedInteger(grid, openvdb::GridBase::META_FILE_VOXEL_COUNT);
  result.memoryBytes = recordedInteger(grid, openvdb::GridBase::META_FILE_MEM_BYTES);
  if (result.activeVoxelCount != std::optional<uint64_t>(0))
    result.indexBounds = recordedBounds(grid);
  result.worldBounds = worldBounds(result.indexBounds, result.indexToWorld);
  if (result.indexBounds && result.indexToWorld && !result.worldBounds)
    result.transformDiagnostic = "active cell-world envelope exceeds finite double coordinates";
  for (const auto* text : {&result.descriptorName, &result.name, &result.valueType,
                          &result.gridClass, &result.vectorType, &result.transformDiagnostic})
    budget.charge(*text);
  for (auto metadata = grid.beginMeta(); metadata != grid.endMeta(); ++metadata) {
    if (!metadata->second) continue;
    budget.charge(sizeof(InspectionMetadata));
    budget.charge(metadata->first);
    budget.charge(metadata->second->size()); // Before textual copies of long strings.
    InspectionMetadata field{metadata->first, metadata->second->typeName(), {}};
    budget.charge(field.type);
    field.value = metadata->second->str();
    budget.charge(field.value);
    result.metadata.push_back(std::move(field));
  }
  return result;
}

template<class Grid>
ScalarStatistics statistics(const Grid& grid, GridInspection& information,
                             const InspectionOptions& options) {
  ScalarStatistics result;
  result.background = static_cast<double>(grid.background());
  if (!std::isfinite(result.background)) gridError(information.descriptorName, "scalar background is not finite");
  constexpr uint64_t leafVoxels = Grid::TreeType::LeafNodeType::NUM_VALUES;
  const uint64_t leafBudget = options.maxActiveVoxels / leafVoxels +
      (options.maxActiveVoxels % leafVoxels != 0);
  result.actualMemoryBytes = grid.memUsage();
  if (grid.activeVoxelCount() > options.maxActiveVoxels ||
      uint64_t(grid.tree().leafCount()) > leafBudget || result.actualMemoryBytes > options.maxInputBytes)
    gridError(information.descriptorName, "loaded scalar active/stored topology or memory exceeds inspection budgets");
  exact_statistics_detail::Moment moments;
  for (auto value = grid.cbeginValueAll(); value; ++value) {
    const double scalar = static_cast<double>(*value);
    if (!std::isfinite(scalar)) gridError(information.descriptorName, "stored scalar value is not finite");
    if (!value.isValueOn()) continue;
    const uint64_t count = value.getVoxelCount();
    if (count > options.maxActiveVoxels - result.count)
      gridError(information.descriptorName, "active scalar count exceeds inspection budget");
    result.count += count;
    result.minimum = result.minimum ? std::min(*result.minimum, scalar) : scalar;
    result.maximum = result.maximum ? std::max(*result.maximum, scalar) : scalar;
    moments.add(scalar, count);
  }
  try {
    result.sum = moments.total();
    if (result.count) {
      const auto [mean,deviation] = moments.finish(result.count);
      result.mean = mean;
      result.populationStdDev = deviation;
      result.rootMeanSquare = moments.rootMeanSquare(result.count);
    }
  } catch (const std::runtime_error& error) {
    gridError(information.descriptorName, error.what());
  }
  if (options.histogramBins) {
    ScalarHistogram histogram;
    histogram.bins.resize(options.histogramBins,0);
    histogram.minimum = options.histogramRange ? (*options.histogramRange)[0] : result.minimum;
    histogram.maximum = options.histogramRange ? (*options.histogramRange)[1] : result.maximum;
    if (result.count) {
      const double low = *histogram.minimum, high = *histogram.maximum;
      exact_statistics_detail::Histogram boundaries(low,high,options.histogramBins);
      for (auto value = grid.cbeginValueOn(); value; ++value) {
        const double scalar = static_cast<double>(*value);
        const auto count = value.getVoxelCount();
        if (scalar < low) histogram.belowRange += count;
        else if (scalar > high) histogram.aboveRange += count;
        else histogram.bins[boundaries.index(scalar)] += count;
      }
    }
    result.histogram = std::move(histogram);
  }
  const auto bounds = grid.evalActiveVoxelBoundingBox();
  if (!bounds.empty()) result.indexBounds = indexBounds(bounds);
  result.worldBounds = worldBounds(result.indexBounds, information.indexToWorld);
  if (result.indexBounds && information.indexToWorld && !result.worldBounds)
    information.transformDiagnostic = "active cell-world envelope exceeds finite double coordinates";
  return result;
}
#endif
} // namespace

FileInspection InspectFile(const std::filesystem::path& input, const InspectionOptions& options) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  if (options.maxGrids == 0 || options.maxGrids > 4096 || !options.maxMetadataBytes ||
      !options.maxInputBytes || !options.maxActiveVoxels)
    throw std::invalid_argument("inspection requires maxGrids in [1,4096] and positive byte/voxel budgets");
  if (options.histogramBins > 4096 ||
      (options.histogramBins && !options.loadStatistics) ||
      (options.histogramRange && (!options.histogramBins ||
        !std::isfinite((*options.histogramRange)[0]) || !std::isfinite((*options.histogramRange)[1]) ||
        (*options.histogramRange)[0] >= (*options.histogramRange)[1])))
    throw std::invalid_argument("inspection histogram requires statistics, bins in [1,4096] and a finite increasing optional range");
  if (!std::filesystem::is_regular_file(input))
    throw std::invalid_argument("inspection input is not a regular file");
  FileInspection result;
  result.fileBytes = std::filesystem::file_size(input);
  if (result.fileBytes > options.maxInputBytes)
    throw std::invalid_argument("inspection input exceeds maxInputBytes");
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });
  if (result.fileBytes > static_cast<uintmax_t>(std::numeric_limits<int64_t>::max()))
    throw std::invalid_argument("inspection input exceeds seekable stream offset range");
  // File::readGridMetadata copies its result through Transform(MapBase), which
  // simplifies the map and can change distant scales or drop small shears.
  // Read the SDK's metadata/transform records directly, without reading trees.
  // The shared pointer object must outlive the stream that refers to it.
  auto streamMetadata = std::make_shared<openvdb::io::StreamMetadata>();
  streamMetadata->setSeekable(true);
  std::ifstream stream(input, std::ios::binary);
  stream.exceptions(std::ios::badbit | std::ios::failbit);
  MetadataArchive archive;
  archive.begin(stream, streamMetadata);
  MetadataBudget budget(options.maxMetadataBytes);
  openvdb::MetaMap fileMetadata;
  fileMetadata.readMeta(stream);
  for (auto metadata = fileMetadata.beginMeta(); metadata != fileMetadata.endMeta(); ++metadata) {
    if (!metadata->second) continue;
    budget.charge(sizeof(InspectionMetadata));
    budget.charge(metadata->first);
    budget.charge(metadata->second->typeName());
    budget.charge(metadata->second->size());
  }
  const auto count = archive.count(stream);
  if (count < 0 || static_cast<size_t>(count) > options.maxGrids)
    throw std::invalid_argument("inspection descriptor count exceeds maxGrids or is negative");
  struct DescribedGrid { std::string sortName; GridInspection information; };
  budget.charge(static_cast<uintmax_t>(count) * sizeof(DescribedGrid));
  std::vector<DescribedGrid> selected;
  selected.reserve(static_cast<size_t>(count));
  std::set<std::string> uniqueDescriptors;
  for (int32_t index = 0; index < count; ++index) {
    openvdb::io::GridDescriptor encoded;
    const auto grid = archive.readMetadata(stream, encoded, result.fileBytes);
    const auto descriptor = openvdb::io::GridDescriptor::nameAsString(encoded.uniqueName());
    budget.charge(descriptor);
    if (!uniqueDescriptors.insert(descriptor).second)
      throw std::invalid_argument("inspection descriptor names collide; rename bracket-suffixed grids before inspecting this file");
    auto information = describe(*grid, descriptor, budget);
    if (!GridSelectionMatches(options.gridName, descriptor, grid->getName())) continue;
    if (options.loadStatistics && !grid->isType<openvdb::FloatGrid>() && !grid->isType<openvdb::DoubleGrid>())
      information.statisticsUnavailableReason = "statistics support only FloatGrid and DoubleGrid scalar trees";
    budget.charge(encoded.gridName());
    selected.push_back({encoded.gridName(), std::move(information)});
  }
  stream.close();
  // Match File's multimap order, retaining file order among duplicate names.
  // Sorting the printable descriptor would put name[10] before name[2].
  std::stable_sort(selected.begin(), selected.end(), [](const auto& a, const auto& b) {
    return a.sortName < b.sortName;
  });
  result.grids.reserve(selected.size());
  for (auto& entry : selected) result.grids.push_back(std::move(entry.information));
  if (!options.gridName.empty() && result.grids.empty())
    throw std::invalid_argument("inspection found no grid matching the requested name");
  if (!options.gridName.empty() && result.grids.size() > 1)
    throw std::invalid_argument("inspection grid name is ambiguous; use descriptor:<unique descriptor name>");
  if (options.loadStatistics) {
    for (const auto& information : result.grids) {
      if (!information.statisticsUnavailableReason.empty()) continue;
      if (!information.activeVoxelCount || !information.memoryBytes)
        gridError(information.descriptorName,
            "statistics require valid recorded file_voxel_count and file_mem_bytes metadata; rewrite with OpenVDB grid statistics enabled");
      if (*information.activeVoxelCount > options.maxActiveVoxels || *information.memoryBytes > options.maxInputBytes)
        gridError(information.descriptorName, "recorded scalar count or memory exceeds inspection budgets before loading");
    }
    openvdb::io::File file(input.string());
    file.open(false);
    if (file.getUniqueTag() != archive.getUniqueTag())
      throw std::invalid_argument("inspection input archive changed before statistics loading");
    for (auto& information : result.grids) {
      if (!information.statisticsUnavailableReason.empty()) {
        if (!options.gridName.empty()) gridError(information.descriptorName, information.statisticsUnavailableReason);
        budget.charge(information.statisticsUnavailableReason);
        continue;
      }
      const size_t diagnosticBytes = information.transformDiagnostic.size();
      if (options.histogramBins)
        budget.charge(sizeof(ScalarHistogram) + uintmax_t(options.histogramBins)*sizeof(uint64_t));
      const auto grid = file.readGrid(information.descriptorName);
      if (grid->isType<openvdb::FloatGrid>())
        information.statistics = statistics(*openvdb::gridPtrCast<openvdb::FloatGrid>(grid), information, options);
      else if (grid->isType<openvdb::DoubleGrid>())
        information.statistics = statistics(*openvdb::gridPtrCast<openvdb::DoubleGrid>(grid), information, options);
      else gridError(information.descriptorName, "scalar grid type changed while inspecting the file");
      if (information.transformDiagnostic.size() > diagnosticBytes)
        budget.charge(information.transformDiagnostic.size() - diagnosticBytes);
    }
    file.close();
  }
  return result;
#else
  (void)input; (void)options;
  unavailable();
#endif
}

} // namespace container::geometry::vdb
