#include "Container/geometry/OpenVdbVolume.h"
#include "Container/geometry/OpenVdbGridSelection.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/tools/Interpolation.h>
#if defined(CONTAINER_HAS_NANOVDB) && CONTAINER_HAS_NANOVDB
#ifndef NANOVDB_USE_OPENVDB
#define NANOVDB_USE_OPENVDB
#endif
#include "Container/geometry/detail/OpenVdbNanoCompatibility.h"
#include <nanovdb/tools/CreateNanoGrid.h>
#endif
#endif

namespace container::geometry {

bool openVdbVolumeAvailable() noexcept {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  return true;
#else
  return false;
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
namespace {
void initializeOpenVdbVolume() {
  static std::once_flag initialized;
  std::call_once(initialized, [] { openvdb::initialize(); });
}
bool scalarGrid(const openvdb::GridBase& grid) {
  return grid.isType<openvdb::FloatGrid>() ||
         grid.isType<openvdb::DoubleGrid>();
}

struct InputBudget {
  uint64_t activeVoxels{0}, bytes{0}, leaves{0};
};

void consumeBudget(uint64_t value, uint64_t limit, uint64_t& used,
                   const char* message) {
  if (value > limit - used)
    throw std::runtime_error(message);
  used += value;
}

template<class Grid>
void validateGrid(const Grid& grid, const OpenVdbVolumeOptions& options,
                  InputBudget& budget) {
  consumeBudget(grid.activeVoxelCount(), options.maxActiveVoxels,
                budget.activeVoxels, "OpenVDB volume grids exceed the combined active voxel budget");
  // Sparse files can contain large inactive leaf arrays with few active
  // voxels. All selected source grids share topology and memory budgets.
  constexpr uint64_t leafVoxelCount = uint64_t(Grid::TreeType::LeafNodeType::NUM_VALUES);
  consumeBudget(uint64_t(grid.tree().leafCount()), options.maxActiveVoxels / leafVoxelCount + 1,
                budget.leaves, "OpenVDB volume topology exceeds the combined preview input budget");
  consumeBudget(uint64_t(grid.memUsage()), options.maxInputBytes, budget.bytes,
                "OpenVDB volume grids exceed the combined preview memory budget");
  if (!std::isfinite(double(grid.background())))
    throw std::runtime_error("OpenVDB volume background is nonfinite");
  for (auto value = grid.cbeginValueAll(); value; ++value)
    if (!std::isfinite(double(*value)))
      throw std::runtime_error("OpenVDB volume contains a nonfinite scalar value");
  if (!grid.transform().isLinear())
    throw std::runtime_error("OpenVDB volume preview requires a linear grid transform");
  const auto origin = grid.indexToWorld(openvdb::Vec3d(0));
  const auto xAxis = grid.indexToWorld(openvdb::Vec3d(1, 0, 0)) - origin;
  const auto yAxis = grid.indexToWorld(openvdb::Vec3d(0, 1, 0)) - origin;
  const auto zAxis = grid.indexToWorld(openvdb::Vec3d(0, 0, 1)) - origin;
  const double determinant = xAxis.cross(yAxis).dot(zAxis);
  if (!std::isfinite(determinant) || determinant == 0.0)
    throw std::runtime_error("OpenVDB volume transform is nonfinite or singular");
}

void validateGrid(const openvdb::GridBase::Ptr& grid,
                  const OpenVdbVolumeOptions& options, InputBudget& budget) {
  if (grid->isType<openvdb::FloatGrid>())
    validateGrid(*openvdb::gridConstPtrCast<openvdb::FloatGrid>(grid), options, budget);
  else
    validateGrid(*openvdb::gridConstPtrCast<openvdb::DoubleGrid>(grid), options, budget);
}

template<class Grid>
void sampleValues(const Grid& grid, const OpenVdbVolumeOptions& options,
                  const OpenVdbVolume& volume,
                  std::vector<float>& values) {
  const auto& size = volume.dimensions;
  values.resize(size_t(size[0]) * size[1] * size[2]);
  if (grid.empty())
    return;
  const auto bbox = grid.evalActiveVoxelBoundingBox();
  const openvdb::Vec3d minimum(volume.boundsMin[0], volume.boundsMin[1], volume.boundsMin[2]);
  const openvdb::Vec3d maximum(volume.boundsMax[0], volume.boundsMax[1], volume.boundsMax[2]);
  const auto extent = maximum - minimum;
  auto accessor = grid.getConstAccessor();
  openvdb::tools::GridSampler<decltype(accessor), openvdb::tools::BoxSampler>
      sampler(accessor, grid.transform());
  for (uint32_t z = 0; z < size[2]; ++z)
    for (uint32_t y = 0; y < size[1]; ++y)
      for (uint32_t x = 0; x < size[0]; ++x) {
        const openvdb::Vec3d uv((x + 0.5) / size[0], (y + 0.5) / size[1],
                                (z + 0.5) / size[2]);
        openvdb::Vec3d world;
        for (int axis = 0; axis < 3; ++axis)
          world[axis] = (minimum[axis] + uv[axis] * extent[axis]) / options.importScale;
        const auto index = grid.worldToIndex(world);
        bool inside = true;
        for (int axis = 0; axis < 3; ++axis) {
          if (!std::isfinite(index[axis]))
            throw std::runtime_error("OpenVDB volume inverse transform is nonfinite");
          inside &= index[axis] >= double(bbox.min()[axis]) - 0.5 &&
                    index[axis] <= double(bbox.max()[axis]) + 0.5;
        }
        const double value = inside ? double(sampler.wsSample(world)) : 0.0;
        if (!std::isfinite(value))
          throw std::runtime_error("OpenVDB volume sampling produced a nonfinite value");
        values[(size_t(z) * size[1] + y) * size[0] + x] =
            static_cast<float>(std::clamp(value, 0.0, 1.0e6));
      }
}

template<class Grid>
OpenVdbVolume describeGrid(const Grid& grid, const OpenVdbVolumeOptions& options) {
  if (grid.empty())
    throw std::runtime_error("OpenVDB density grid is empty");
  const auto bbox = grid.evalActiveVoxelBoundingBox();
  openvdb::Vec3d minimum(std::numeric_limits<double>::max());
  openvdb::Vec3d maximum(std::numeric_limits<double>::lowest());
  for (int corner = 0; corner < 8; ++corner) {
    openvdb::Vec3d index;
    for (int axis = 0; axis < 3; ++axis)
      index[axis] = (corner & (1 << axis)) ? double(bbox.max()[axis]) + 0.5
                                          : double(bbox.min()[axis]) - 0.5;
    const auto world = grid.indexToWorld(index) * double(options.importScale);
    for (int axis = 0; axis < 3; ++axis) {
      if (!std::isfinite(world[axis]))
        throw std::runtime_error("OpenVDB density transform produces nonfinite world bounds");
      minimum[axis] = std::min(minimum[axis], world[axis]);
      maximum[axis] = std::max(maximum[axis], world[axis]);
    }
  }
  const auto extent = maximum - minimum;
  const double longest = std::max({extent[0], extent[1], extent[2]});
  if (!std::isfinite(longest) || longest <= 0.0)
    throw std::runtime_error("OpenVDB density grid has invalid world bounds");
  OpenVdbVolume result;
  result.gridName = grid.getName();
  result.sourceActiveVoxels = grid.activeVoxelCount();
  for (int axis = 0; axis < 3; ++axis) {
    result.boundsMin[axis] = static_cast<float>(minimum[axis]);
    result.boundsMax[axis] = static_cast<float>(maximum[axis]);
    if (!std::isfinite(result.boundsMin[axis]) ||
        !std::isfinite(result.boundsMax[axis]) ||
        result.boundsMax[axis] <= result.boundsMin[axis])
      throw std::runtime_error("OpenVDB density bounds exceed float precision");
    result.dimensions[axis] = std::clamp(
        static_cast<uint32_t>(std::ceil(options.maxResolution * extent[axis] / longest)),
        1u, options.maxResolution);
  }
  if (options.sparse) {
    result.dimensions = {1, 1, 1};
    result.sparseImportScale = options.importScale;
    const auto origin = grid.indexToWorld(openvdb::Vec3d(0));
    double shortestAxis = std::numeric_limits<double>::max();
    for (int axis = 0; axis < 3; ++axis) {
      openvdb::Vec3d unit(0);
      unit[axis] = 1.0;
      shortestAxis = std::min(shortestAxis, (grid.indexToWorld(unit) - origin).length());
    }
    result.sparseStepSize = static_cast<float>(0.75 * shortestAxis * options.importScale);
    if (!std::isfinite(result.sparseStepSize) || result.sparseStepSize <= 0.0f)
      throw std::runtime_error("OpenVDB native voxel size exceeds preview step precision");
  }
  return result;
}

#if defined(CONTAINER_HAS_NANOVDB) && CONTAINER_HAS_NANOVDB
template<class Grid>
float clampSparseValues(Grid& grid) {
  using Value = typename Grid::ValueType;
  const auto clamped = [](Value value) { return Value(std::clamp(double(value), 0.0, 1.0e6)); };
  float maximum = float(clamped(grid.background()));
  for (auto value = grid.beginValueAll(); value; ++value) {
    const Value bounded = clamped(*value);
    value.setValue(bounded);
    maximum = std::max(maximum, float(bounded));
  }
  grid.tree().root().setBackground(clamped(grid.background()), false);
  return maximum;
}

template<class Grid>
void serializeSparseGrid(Grid& grid, const OpenVdbVolumeOptions& options,
                     uint64_t& serializedBytes, std::vector<uint32_t>& words,
                     float* maximum = nullptr) {
  const float valueMaximum = clampSparseValues(grid);
  // Conversion runs only after encoded/metadata/loaded topology checks. Its
  // temporary allocation precedes this post-conversion serialized-byte check.
  auto handle = nanovdb::tools::createNanoGrid<Grid, float>(grid,
      nanovdb::tools::StatsMode::BBox, nanovdb::CheckMode::Disable);
  const uint64_t bytes = handle.bufferSize();
  if (!bytes || bytes >= std::numeric_limits<uint32_t>::max() || bytes % sizeof(uint32_t) ||
      !handle.template grid<float>())
    throw std::runtime_error("OpenVDB native grid exceeds 32-bit NanoVDB buffer offsets");
  consumeBudget(bytes, std::min(options.maxInputBytes, uint64_t(std::numeric_limits<uint32_t>::max()) - 1), serializedBytes,
                "OpenVDB native grids exceed the combined serialized byte budget");
  words.resize(size_t(bytes / sizeof(uint32_t)));
  std::memcpy(words.data(), handle.data(), size_t(bytes));
  if (maximum) *maximum = valueMaximum;
}

void serializeSparse(const openvdb::GridBase::Ptr& grid, const OpenVdbVolumeOptions& options,
                     uint64_t& serializedBytes, std::vector<uint32_t>& words,
                     float* maximum = nullptr) {
  if (grid->isType<openvdb::FloatGrid>())
    serializeSparseGrid(*openvdb::gridPtrCast<openvdb::FloatGrid>(grid), options, serializedBytes, words, maximum);
  else
    serializeSparseGrid(*openvdb::gridPtrCast<openvdb::DoubleGrid>(grid), options, serializedBytes, words, maximum);
}
#endif
} // namespace
#endif

OpenVdbVolume loadOpenVdbVolume(const std::filesystem::path& path,
                               const OpenVdbVolumeOptions& options) {
  if (options.maxResolution < 16 || options.maxResolution > 256)
    throw std::invalid_argument("OpenVDB volume resolution must be in [16,256]");
  if (!std::isfinite(options.importScale) || options.importScale <= 0.0f)
    throw std::invalid_argument("OpenVDB volume import scale must be positive and finite");
  if (!options.maxActiveVoxels || !options.maxInputBytes)
    throw std::invalid_argument("OpenVDB volume input budgets must be positive");
#if !defined(CONTAINER_HAS_NANOVDB) || !CONTAINER_HAS_NANOVDB
  if (options.sparse)
    throw std::runtime_error("OpenVDB native sparse loading requires NanoVDB headers; configure with -DENABLE_OPENVDB=ON and NANOVDB_INCLUDE_DIR pointing to NanoVDB");
#endif
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  if (std::filesystem::file_size(path) > options.maxInputBytes)
    throw std::runtime_error("OpenVDB file exceeds the volume preview input byte budget");
  initializeOpenVdbVolume();
  openvdb::io::File file(path.string());
  file.open(false);
  std::optional<std::string> selected;
  openvdb::GridBase::Ptr selectedMetadata;
  std::optional<std::string> selectedTemperature;
  openvdb::GridBase::Ptr temperatureMetadata;
  for (auto descriptor = file.beginName(); descriptor != file.endName(); ++descriptor) {
    const auto name = descriptor.gridName();
    const auto grid = file.readGridMetadata(name);
    if (!options.temperatureGridName.empty() &&
        vdb::GridSelectionMatches(options.temperatureGridName, name, grid->getName())) {
      if (selectedTemperature)
        throw std::runtime_error("OpenVDB temperature grid name is ambiguous; use descriptor:<raw descriptor name>");
      if (!scalarGrid(*grid))
        throw std::runtime_error("OpenVDB temperature supports FloatGrid and DoubleGrid Kelvin values");
      if (grid->getGridClass() == openvdb::GRID_LEVEL_SET)
        throw std::runtime_error("OpenVDB temperature grid is signed distance, not Kelvin temperature");
      selectedTemperature = name;
      temperatureMetadata = grid;
    }
    if (!options.gridName.empty()) {
      if (vdb::GridSelectionMatches(options.gridName, name, grid->getName())) {
        if (selected)
          throw std::runtime_error("OpenVDB grid name is ambiguous; use descriptor:<raw descriptor name>");
        selected = name;
        selectedMetadata = grid;
        if (!scalarGrid(*grid))
          throw std::runtime_error("OpenVDB volume preview supports FloatGrid and DoubleGrid density");
      }
    } else if (scalarGrid(*grid) && grid->getGridClass() != openvdb::GRID_LEVEL_SET) {
      if (!selected || (grid->getName() == "density" && selectedMetadata->getName() != "density")) {
        selected = name;
        selectedMetadata = grid;
      }
    }
  }
  if (!selected)
    throw std::runtime_error(options.gridName.empty()
        ? "OpenVDB file has no scalar fog grid; convert a level set to fog with OpenVdbTool"
        : "OpenVDB density grid not found: " + options.gridName);
  if (!options.temperatureGridName.empty() && !selectedTemperature)
    throw std::runtime_error("OpenVDB temperature grid not found: " + options.temperatureGridName);
  if (selectedMetadata->getGridClass() == openvdb::GRID_LEVEL_SET)
    throw std::runtime_error("OpenVDB level set is signed distance, not density; convert it to fog with OpenVdbTool");
  InputBudget metadataBudget;
  const auto checkMetadata = [&](const openvdb::GridBase::Ptr& metadata) {
    const auto count = metadata->getMetadata<openvdb::Int64Metadata>(
        openvdb::GridBase::META_FILE_VOXEL_COUNT);
    if (count && count->value() >= 0)
      consumeBudget(uint64_t(count->value()), options.maxActiveVoxels, metadataBudget.activeVoxels,
                    "OpenVDB volume metadata exceeds the combined active voxel budget");
    const auto bytes = metadata->getMetadata<openvdb::Int64Metadata>(
        openvdb::GridBase::META_FILE_MEM_BYTES);
    if (bytes && bytes->value() >= 0)
      consumeBudget(uint64_t(bytes->value()), options.maxInputBytes, metadataBudget.bytes,
                    "OpenVDB volume metadata exceeds the combined preview memory budget");
  };
  checkMetadata(selectedMetadata);
  if (selectedTemperature && selectedTemperature != selected)
    checkMetadata(temperatureMetadata);
  InputBudget inputBudget;
  const auto grid = file.readGrid(*selected);
  validateGrid(grid, options, inputBudget);
  openvdb::GridBase::Ptr temperature;
  if (selectedTemperature) {
    temperature = selectedTemperature == selected ? grid : file.readGrid(*selectedTemperature);
    if (temperature != grid)
      validateGrid(temperature, options, inputBudget);
  }
  file.close();
  auto result = grid->isType<openvdb::FloatGrid>()
      ? describeGrid(*openvdb::gridConstPtrCast<openvdb::FloatGrid>(grid), options)
      : describeGrid(*openvdb::gridConstPtrCast<openvdb::DoubleGrid>(grid), options);
  result.gridName = *selected;
  if (options.sparse) {
#if defined(CONTAINER_HAS_NANOVDB) && CONTAINER_HAS_NANOVDB
    uint64_t serializedBytes = 0;
    serializeSparse(grid, options, serializedBytes, result.sparseDensity, &result.maxDensity);
    if (result.maxDensity <= 0.0f)
      throw std::runtime_error("OpenVDB native preview has no positive density; select a fog grid");
    if (temperature) {
      result.temperatureGridName = *selectedTemperature;
      if (temperature == grid) {
        consumeBudget(result.sparseDensity.size() * uint64_t(sizeof(uint32_t)),
            std::min(options.maxInputBytes, uint64_t(std::numeric_limits<uint32_t>::max()) - 1),
            serializedBytes, "OpenVDB native grids exceed the combined serialized byte budget");
        result.sparseTemperature = result.sparseDensity;
      } else {
        serializeSparse(temperature, options, serializedBytes, result.sparseTemperature);
      }
    }
#endif
    return result;
  }
  if (grid->isType<openvdb::FloatGrid>())
    sampleValues(*openvdb::gridConstPtrCast<openvdb::FloatGrid>(grid), options, result, result.density);
  else
    sampleValues(*openvdb::gridConstPtrCast<openvdb::DoubleGrid>(grid), options, result, result.density);
  result.maxDensity = *std::max_element(result.density.begin(), result.density.end());
  if (result.maxDensity <= 0.0f)
    throw std::runtime_error("OpenVDB preview has no positive density; increase --vdb-resolution or select a fog grid");
  if (temperature) {
    result.temperatureGridName = *selectedTemperature;
    if (temperature->isType<openvdb::FloatGrid>())
      sampleValues(*openvdb::gridConstPtrCast<openvdb::FloatGrid>(temperature), options,
                   result, result.temperature);
    else
      sampleValues(*openvdb::gridConstPtrCast<openvdb::DoubleGrid>(temperature), options,
                   result, result.temperature);
  }
  return result;
#else
  (void)path;
  throw std::runtime_error("OpenVDB volume loading is disabled; configure with -DENABLE_OPENVDB=ON and install OpenVDB");
#endif
}

} // namespace container::geometry
