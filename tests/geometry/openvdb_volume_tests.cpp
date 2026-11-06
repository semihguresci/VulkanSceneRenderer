#include "Container/geometry/OpenVdbVolume.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#if defined(CONTAINER_HAS_NANOVDB) && CONTAINER_HAS_NANOVDB
#include <nanovdb/GridHandle.h>
#include <nanovdb/math/SampleFromVoxels.h>
#endif
#endif

using container::geometry::loadOpenVdbVolume;
using container::geometry::OpenVdbVolumeOptions;

TEST(OpenVdbVolume, RejectsInvalidPreviewOptionsBeforeReading) {
  OpenVdbVolumeOptions options;
  options.maxResolution = 257;
  EXPECT_THROW((void)loadOpenVdbVolume("missing.vdb", options), std::invalid_argument);
  options.maxResolution = 16;
  options.importScale = std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW((void)loadOpenVdbVolume("missing.vdb", options), std::invalid_argument);
  options.importScale = 1.0f;
  options.maxActiveVoxels = 0;
  EXPECT_THROW((void)loadOpenVdbVolume("missing.vdb", options), std::invalid_argument);
}

#if !defined(CONTAINER_HAS_NANOVDB) || !CONTAINER_HAS_NANOVDB
TEST(OpenVdbVolume, NativeSparseUnavailableHasActionableDiagnostic) {
  try {
    (void)loadOpenVdbVolume("missing.vdb", {.sparse = true});
    FAIL() << "Native sparse preview must reject a build without NanoVDB";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("NANOVDB_INCLUDE_DIR"), std::string::npos);
  }
}
#endif

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
namespace {
class VolumeFile {
public:
  explicit VolumeFile(const openvdb::GridPtrVec& grids) {
    path = std::filesystem::temp_directory_path() /
        ("container_vdb_volume_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".vdb");
    openvdb::io::File file(path.string());
    file.write(grids);
  }
  ~VolumeFile() { std::error_code error; std::filesystem::remove(path, error); }
  std::filesystem::path path;
};

openvdb::FloatGrid::Ptr fog(const std::string& name = "density") {
  openvdb::initialize();
  auto grid = openvdb::FloatGrid::create(0.0f);
  grid->setName(name);
  grid->setGridClass(openvdb::GRID_FOG_VOLUME);
  grid->fill(openvdb::CoordBBox(openvdb::Coord(-2, -3, -4),
                               openvdb::Coord(2, 3, 4)), 1.0f, true);
  return grid;
}

float temperatureAt(const container::geometry::OpenVdbVolume& volume,
                    const std::array<float, 3>& world) {
  std::array<uint32_t, 3> index;
  for (int axis = 0; axis < 3; ++axis) {
    const auto fraction = (world[axis] - volume.boundsMin[axis]) /
        (volume.boundsMax[axis] - volume.boundsMin[axis]);
    index[axis] = std::min(uint32_t(std::max(0.0f, fraction) * volume.dimensions[axis]),
                           volume.dimensions[axis] - 1);
  }
  return volume.temperature[(size_t(index[2]) * volume.dimensions[1] + index[1]) *
      volume.dimensions[0] + index[0]];
}
}

TEST(OpenVdbVolume, SamplesReflectedTranslatedAndScaledWorldBounds) {
  auto grid = fog();
  auto transform = openvdb::math::Transform::createLinearTransform(0.5);
  transform->preScale(openvdb::Vec3d(-1.0, 1.0, 1.0));
  transform->postTranslate(openvdb::Vec3d(10.0, 20.0, 30.0));
  grid->setTransform(transform);
  VolumeFile file({grid});
  auto volume = loadOpenVdbVolume(file.path, {.maxResolution = 32, .importScale = 2.0f});
  EXPECT_TRUE(container::geometry::openVdbVolumeAvailable());
  EXPECT_EQ(volume.dimensions, (std::array<uint32_t, 3>{18, 25, 32}));
  EXPECT_EQ(volume.sourceActiveVoxels, 5u * 7u * 9u);
  EXPECT_FLOAT_EQ(volume.boundsMin[0], 17.5f);
  EXPECT_FLOAT_EQ(volume.boundsMax[0], 22.5f);
  EXPECT_FLOAT_EQ(volume.boundsMin[1], 36.5f);
  EXPECT_FLOAT_EQ(volume.boundsMax[2], 64.5f);
  EXPECT_EQ(volume.density.size(), 18u * 25u * 32u);
  EXPECT_FLOAT_EQ(volume.maxDensity, 1.0f);
  EXPECT_TRUE(std::ranges::all_of(volume.density, [](float value) {
    return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
  }));
}

TEST(OpenVdbVolume, PrefersDensityAndAcceptsUnnamedScalarGrid) {
  auto temperature = fog("temperature");
  auto density = fog("density");
  VolumeFile named({temperature, density});
  EXPECT_EQ(loadOpenVdbVolume(named.path).gridName, "density");
  auto unnamed = fog("");
  VolumeFile unnamedFile({unnamed});
  EXPECT_GT(loadOpenVdbVolume(unnamedFile.path, {.maxResolution = 16}).maxDensity, 0.0f);
}

TEST(OpenVdbVolume, RejectsMissingAmbiguousUnsupportedAndLevelSetGrids) {
  auto first = fog("same");
  auto second = fog("same");
  auto mask = openvdb::BoolGrid::create(false);
  mask->setName("mask");
  auto sdf = fog("sdf");
  sdf->setGridClass(openvdb::GRID_LEVEL_SET);
  VolumeFile file({first, second, mask, sdf});
  EXPECT_THROW((void)loadOpenVdbVolume(file.path, {.gridName = "missing"}), std::runtime_error);
  EXPECT_THROW((void)loadOpenVdbVolume(file.path, {.gridName = "same"}), std::runtime_error);
  EXPECT_THROW((void)loadOpenVdbVolume(file.path, {.gridName = "mask"}), std::runtime_error);
  EXPECT_THROW((void)loadOpenVdbVolume(file.path, {.gridName = "sdf"}), std::runtime_error);
}

TEST(OpenVdbVolume, EnforcesSparseInputAndFileBudgets) {
  VolumeFile file({fog()});
  EXPECT_THROW((void)loadOpenVdbVolume(file.path, {.maxActiveVoxels = 100}), std::runtime_error);
  EXPECT_THROW((void)loadOpenVdbVolume(file.path, {.maxInputBytes = 16}), std::runtime_error);
}

TEST(OpenVdbVolume, RejectsNonfiniteValuesAndEmptyDensity) {
  auto grid = fog();
  grid->getAccessor().setValue(openvdb::Coord(0), std::numeric_limits<float>::quiet_NaN());
  VolumeFile nonfinite({grid});
  EXPECT_THROW((void)loadOpenVdbVolume(nonfinite.path), std::runtime_error);
  auto empty = openvdb::FloatGrid::create(0.0f);
  empty->setName("density");
  VolumeFile emptyFile({empty});
  EXPECT_THROW((void)loadOpenVdbVolume(emptyFile.path), std::runtime_error);
}

TEST(OpenVdbVolume, SupportsDoubleDensityWithoutNormalizingMagnitude) {
  openvdb::initialize();
  auto grid = openvdb::DoubleGrid::create(0.0);
  grid->setName("density");
  grid->fill(openvdb::CoordBBox(openvdb::Coord(-1), openvdb::Coord(1)), 3.0, true);
  VolumeFile file({grid});
  const auto volume = loadOpenVdbVolume(file.path, {.maxResolution = 16});
  EXPECT_FLOAT_EQ(volume.maxDensity, 3.0f);
}

TEST(OpenVdbVolume, TemperatureIsOptInAndUsesItsOwnAffineTransform) {
  auto density = fog();
  auto temperature = openvdb::FloatGrid::create(900.0f);
  temperature->setName("temperature");
  temperature->fill(openvdb::CoordBBox(openvdb::Coord(-1), openvdb::Coord(1)), 1500.0f, true);
  auto transform = openvdb::math::Transform::createLinearTransform(0.5);
  transform->preScale(openvdb::Vec3d(-1.0, 2.0, 1.0));
  transform->postTranslate(openvdb::Vec3d(1.0, 0.0, 0.0));
  temperature->setTransform(transform);
  VolumeFile file({density, temperature});
  const auto ordinary = loadOpenVdbVolume(file.path, {.maxResolution = 32, .importScale = 2.0f});
  EXPECT_TRUE(ordinary.temperature.empty());
  EXPECT_TRUE(ordinary.temperatureGridName.empty());
  const auto emissive = loadOpenVdbVolume(file.path,
      {.maxResolution = 32, .importScale = 2.0f, .temperatureGridName = "temperature"});
  EXPECT_EQ(emissive.temperatureGridName, "temperature");
  EXPECT_EQ(emissive.dimensions, ordinary.dimensions);
  EXPECT_EQ(emissive.boundsMin, ordinary.boundsMin);
  EXPECT_EQ(emissive.boundsMax, ordinary.boundsMax);
  EXPECT_EQ(emissive.density, ordinary.density);
  ASSERT_EQ(emissive.temperature.size(), emissive.density.size());
  EXPECT_NEAR(temperatureAt(emissive, {2.0f, 0.0f, 0.0f}), 1500.0f, 0.01f);
  // A positive temperature background must not leak beyond its active bounds.
  EXPECT_FLOAT_EQ(temperatureAt(emissive, {-4.0f, 0.0f, 0.0f}), 0.0f);
}

TEST(OpenVdbVolume, SupportsDoubleTemperatureAndClampsKelvinRange) {
  auto temperature = openvdb::DoubleGrid::create(0.0);
  temperature->setName("heat");
  temperature->fill(openvdb::CoordBBox(openvdb::Coord(-10), openvdb::Coord(10)), 2.0e6, true);
  VolumeFile high({fog(), temperature});
  const auto hot = loadOpenVdbVolume(high.path, {.maxResolution = 16, .temperatureGridName = "heat"});
  EXPECT_TRUE(std::ranges::all_of(hot.temperature, [](float value) { return value == 1.0e6f; }));
  temperature->fill(openvdb::CoordBBox(openvdb::Coord(-10), openvdb::Coord(10)), -200.0, true);
  VolumeFile low({fog(), temperature});
  const auto cold = loadOpenVdbVolume(low.path, {.maxResolution = 16, .temperatureGridName = "heat"});
  EXPECT_TRUE(std::ranges::all_of(cold.temperature, [](float value) { return value == 0.0f; }));
}

TEST(OpenVdbVolume, EmptyTemperatureProducesZeroKelvin) {
  auto temperature = openvdb::FloatGrid::create(5000.0f);
  temperature->setName("temperature");
  VolumeFile file({fog(), temperature});
  const auto volume = loadOpenVdbVolume(file.path,
      {.maxResolution = 16, .temperatureGridName = "temperature"});
  ASSERT_EQ(volume.temperature.size(), volume.density.size());
  EXPECT_TRUE(std::ranges::all_of(volume.temperature, [](float value) { return value == 0.0f; }));
}

TEST(OpenVdbVolume, RejectsMissingAmbiguousUnsupportedAndSdfTemperature) {
  auto mask = openvdb::BoolGrid::create(false);
  mask->setName("mask");
  auto sdf = fog("sdf");
  sdf->setGridClass(openvdb::GRID_LEVEL_SET);
  VolumeFile file({fog(), fog("same"), fog("same"), mask, sdf});
  for (const auto& name : {"missing", "same", "mask", "sdf"})
    EXPECT_THROW((void)loadOpenVdbVolume(file.path,
        {.gridName = "density", .maxResolution = 16, .temperatureGridName = name}), std::runtime_error);
  openvdb::io::File source(file.path.string());
  source.open(false);
  std::string uniqueName;
  for (auto descriptor = source.beginName(); descriptor != source.endName(); ++descriptor)
    if (source.readGridMetadata(descriptor.gridName())->getName() == "same") {
      uniqueName = descriptor.gridName();
      break;
    }
  source.close();
  ASSERT_FALSE(uniqueName.empty());
  EXPECT_NO_THROW((void)loadOpenVdbVolume(file.path,
      {.maxResolution = 16, .temperatureGridName = "descriptor:" + uniqueName}));
}

TEST(OpenVdbVolume, ExplicitDescriptorSelectsFirstDuplicateDensityAndTemperature) {
  auto first = fog("duplicate");
  auto second = fog("duplicate");
  second->fill(openvdb::CoordBBox(openvdb::Coord(-2, -3, -4),
                                 openvdb::Coord(2, 3, 4)), 2.0f, true);
  VolumeFile file({fog(), first, second});
  openvdb::io::File source(file.path.string());
  source.open(false);
  std::string firstDescriptor;
  for (auto it = source.beginName(); it != source.endName(); ++it)
    if (source.readGridMetadata(it.gridName())->getName() == "duplicate") {
      firstDescriptor = it.gridName();
      break;
    }
  source.close();
  ASSERT_FALSE(firstDescriptor.empty());
  EXPECT_THROW((void)loadOpenVdbVolume(file.path,
      {.gridName = "duplicate", .maxResolution = 16}), std::runtime_error);
  const auto density = loadOpenVdbVolume(file.path,
      {.gridName = "descriptor:" + firstDescriptor, .maxResolution = 16});
  EXPECT_EQ(density.gridName, firstDescriptor);
  EXPECT_FLOAT_EQ(density.maxDensity, 1.0f);
  EXPECT_THROW((void)loadOpenVdbVolume(file.path,
      {.maxResolution = 16, .temperatureGridName = "duplicate"}), std::runtime_error);
  const auto temperature = loadOpenVdbVolume(file.path,
      {.maxResolution = 16, .temperatureGridName = "descriptor:" + firstDescriptor});
  EXPECT_EQ(temperature.temperatureGridName, firstDescriptor);
  ASSERT_EQ(temperature.temperature.size(), temperature.density.size());
  EXPECT_FLOAT_EQ(*std::max_element(temperature.temperature.begin(), temperature.temperature.end()), 1.0f);
}

TEST(OpenVdbVolume, RejectsNonfiniteTemperatureIncludingInactiveValuesAndBackground) {
  auto temperature = fog("temperature");
  temperature->getAccessor().setValueOff(openvdb::Coord(20), std::numeric_limits<float>::quiet_NaN());
  VolumeFile inactive({fog(), temperature});
  EXPECT_NO_THROW((void)loadOpenVdbVolume(inactive.path, {.maxResolution = 16}));
  EXPECT_THROW((void)loadOpenVdbVolume(inactive.path,
      {.maxResolution = 16, .temperatureGridName = "temperature"}), std::runtime_error);
  auto invalidBackground = openvdb::FloatGrid::create(std::numeric_limits<float>::infinity());
  invalidBackground->setName("temperature");
  VolumeFile background({fog(), invalidBackground});
  EXPECT_THROW((void)loadOpenVdbVolume(background.path,
      {.maxResolution = 16, .temperatureGridName = "temperature"}), std::runtime_error);
}

TEST(OpenVdbVolume, DensityAndTemperatureShareActiveVoxelAndMemoryBudgets) {
  auto density = openvdb::FloatGrid::create(0.0f);
  density->setName("density");
  density->fill(openvdb::CoordBBox(openvdb::Coord(0), openvdb::Coord(7)), 1.0f, true);
  auto temperature = density->deepCopy();
  temperature->setName("temperature");
  VolumeFile file({density, temperature});
  EXPECT_NO_THROW((void)loadOpenVdbVolume(file.path, {.maxResolution = 16, .maxActiveVoxels = 700}));
  EXPECT_THROW((void)loadOpenVdbVolume(file.path,
      {.maxResolution = 16, .maxActiveVoxels = 700, .temperatureGridName = "temperature"}), std::runtime_error);
  const uint64_t memoryLimit = uint64_t(density->memUsage()) + uint64_t(density->memUsage()) / 2;
  ASSERT_LT(std::filesystem::file_size(file.path), memoryLimit);
  EXPECT_NO_THROW((void)loadOpenVdbVolume(file.path, {.maxResolution = 16, .maxInputBytes = memoryLimit}));
  EXPECT_THROW((void)loadOpenVdbVolume(file.path,
      {.maxResolution = 16, .maxInputBytes = memoryLimit, .temperatureGridName = "temperature"}), std::runtime_error);
  // Selecting the same source for both channels only loads/counts it once.
  EXPECT_NO_THROW((void)loadOpenVdbVolume(file.path,
      {.maxResolution = 16, .maxActiveVoxels = 700, .temperatureGridName = "density"}));
}

TEST(OpenVdbVolume, DensityAndTemperatureShareStoredTopologyBudget) {
  auto density = openvdb::FloatGrid::create(0.0f);
  density->setName("density");
  density->getAccessor().setValue(openvdb::Coord(0), 1.0f);
  density->tree().touchLeaf(openvdb::Coord(512, 0, 0));
  auto temperature = density->deepCopy();
  temperature->setName("temperature");
  VolumeFile file({density, temperature});
  EXPECT_NO_THROW((void)loadOpenVdbVolume(file.path, {.maxResolution = 16, .maxActiveVoxels = 600}));
  EXPECT_THROW((void)loadOpenVdbVolume(file.path,
      {.maxResolution = 16, .maxActiveVoxels = 600, .temperatureGridName = "temperature"}), std::runtime_error);
}

#if defined(CONTAINER_HAS_NANOVDB) && CONTAINER_HAS_NANOVDB
namespace {
float nativeSample(const std::vector<uint32_t>& words, const std::array<double, 3>& world,
                   float importScale) {
  // The public payload is raw words; NanoVDB's C++ view requires its aligned
  // allocation, unlike the GPU byte-address accessor.
  auto buffer = nanovdb::HostBuffer::create(words.size() * sizeof(uint32_t));
  std::memcpy(buffer.data(), words.data(), words.size() * sizeof(uint32_t));
  nanovdb::GridHandle<nanovdb::HostBuffer> handle(std::move(buffer));
  const auto* grid = handle.grid<float>();
  if (!grid || !grid->isValid()) throw std::runtime_error("Invalid native Float32 payload");
  if (!grid->activeVoxelCount()) return 0.0f;
  const auto index = grid->worldToIndex(nanovdb::Vec3d(
      world[0] / importScale, world[1] / importScale, world[2] / importScale));
  const auto& bounds = grid->indexBBox();
  for (int axis = 0; axis < 3; ++axis)
    if (index[axis] < double(bounds.min()[axis]) - 0.5 ||
        index[axis] > double(bounds.max()[axis]) + 0.5) return 0.0f;
  auto accessor = grid->getAccessor();
  auto sampler = nanovdb::math::createSampler<1>(accessor);
  return sampler(index);
}
}

TEST(OpenVdbVolume, NativeSparseMatchesDenseSamplesWithIndependentAffineMap) {
  auto grid = fog();
  auto transform = openvdb::math::Transform::createLinearTransform(0.5);
  transform->preScale(openvdb::Vec3d(-2.0, 1.0, 0.5));
  transform->postTranslate(openvdb::Vec3d(10.0, 20.0, 30.0));
  grid->setTransform(transform);
  VolumeFile file({grid});
  const auto dense = loadOpenVdbVolume(file.path, {.maxResolution = 16, .importScale = 3.0f});
  const auto sparse = loadOpenVdbVolume(file.path,
      {.maxResolution = 16, .importScale = 3.0f, .sparse = true});
  EXPECT_TRUE(sparse.density.empty());
  EXPECT_TRUE(sparse.temperature.empty());
  EXPECT_FALSE(sparse.sparseDensity.empty());
  EXPECT_TRUE(sparse.sparseTemperature.empty());
  EXPECT_EQ(sparse.dimensions, (std::array<uint32_t, 3>{1, 1, 1}));
  EXPECT_EQ(sparse.boundsMin, dense.boundsMin);
  EXPECT_EQ(sparse.boundsMax, dense.boundsMax);
  EXPECT_FLOAT_EQ(sparse.sparseImportScale, 3.0f);
  EXPECT_FLOAT_EQ(sparse.sparseStepSize, 0.5625f);
  EXPECT_FLOAT_EQ(sparse.maxDensity, 1.0f);
  for (uint32_t z = 0; z < dense.dimensions[2]; z += 3)
    for (uint32_t y = 0; y < dense.dimensions[1]; y += 3)
      for (uint32_t x = 0; x < dense.dimensions[0]; x += 3) {
        std::array<double, 3> world;
        const std::array<uint32_t, 3> index{x, y, z};
        for (int axis = 0; axis < 3; ++axis)
          world[axis] = double(dense.boundsMin[axis]) +
              (double(index[axis]) + 0.5) / dense.dimensions[axis] *
              (double(dense.boundsMax[axis]) - dense.boundsMin[axis]);
        const float expected = dense.density[(size_t(z) * dense.dimensions[1] + y) * dense.dimensions[0] + x];
        EXPECT_NEAR(nativeSample(sparse.sparseDensity, world, sparse.sparseImportScale), expected, 2.0e-4f);
      }
}

TEST(OpenVdbVolume, NativeSparsePreservesIsolatedVoxelsAcrossLargeEmptyBounds) {
  auto grid = openvdb::FloatGrid::create(0.0f);
  grid->setName("density");
  grid->getAccessor().setValue(openvdb::Coord(0), 1.0f);
  grid->getAccessor().setValue(openvdb::Coord(1000000, 0, 0), 3.0f);
  VolumeFile file({grid});
  EXPECT_THROW((void)loadOpenVdbVolume(file.path, {.maxResolution = 16}), std::runtime_error);
  const auto sparse = loadOpenVdbVolume(file.path, {.maxResolution = 16, .sparse = true});
  EXPECT_FLOAT_EQ(nativeSample(sparse.sparseDensity, {0, 0, 0}, 1.0f), 1.0f);
  EXPECT_FLOAT_EQ(nativeSample(sparse.sparseDensity, {1000000, 0, 0}, 1.0f), 3.0f);
  EXPECT_FLOAT_EQ(nativeSample(sparse.sparseDensity, {500000, 0, 0}, 1.0f), 0.0f);
  EXPECT_FLOAT_EQ(sparse.maxDensity, 3.0f);
}

TEST(OpenVdbVolume, NativeSparseConvertsDoubleAndIndependentTemperatureWithClamping) {
  auto density = openvdb::DoubleGrid::create(-200.0);
  density->setName("density");
  density->fill(openvdb::CoordBBox(openvdb::Coord(-2), openvdb::Coord(2)), 2.0e6, true);
  auto temperature = openvdb::DoubleGrid::create(900.0);
  temperature->setName("temperature");
  temperature->fill(openvdb::CoordBBox(openvdb::Coord(-2), openvdb::Coord(2)), 1200.0, true);
  temperature->getAccessor().setValue(openvdb::Coord(0), -100.0);
  auto transform = openvdb::math::Transform::createLinearTransform(0.5);
  transform->postTranslate(openvdb::Vec3d(1.0, 0.0, 0.0));
  temperature->setTransform(transform);
  VolumeFile file({density, temperature});
  const auto sparse = loadOpenVdbVolume(file.path,
      {.maxResolution = 16, .temperatureGridName = "temperature", .sparse = true});
  EXPECT_TRUE(sparse.density.empty());
  EXPECT_TRUE(sparse.temperature.empty());
  ASSERT_FALSE(sparse.sparseDensity.empty());
  ASSERT_FALSE(sparse.sparseTemperature.empty());
  EXPECT_FLOAT_EQ(sparse.maxDensity, 1.0e6f);
  EXPECT_FLOAT_EQ(nativeSample(sparse.sparseDensity, {0, 0, 0}, 1.0f), 1.0e6f);
  EXPECT_FLOAT_EQ(nativeSample(sparse.sparseTemperature, {1, 0, 0}, 1.0f), 0.0f);
  EXPECT_FLOAT_EQ(nativeSample(sparse.sparseTemperature, {1.5, 0, 0}, 1.0f), 1200.0f);
  EXPECT_FLOAT_EQ(nativeSample(sparse.sparseTemperature, {-100, 0, 0}, 1.0f), 0.0f);
}

TEST(OpenVdbVolume, NativeSparseChannelsShareSerializedByteBudgetAndSourceGuards) {
  auto density = fog();
  VolumeFile file({density});
  const auto sparse = loadOpenVdbVolume(file.path, {.maxResolution = 16, .sparse = true});
  const uint64_t bytes = sparse.sparseDensity.size() * uint64_t(sizeof(uint32_t));
  const uint64_t byteLimit = std::max({uint64_t(std::filesystem::file_size(file.path)),
      uint64_t(density->memUsage()), bytes}) + 4096;
  ASSERT_LT(byteLimit, 2 * bytes);
  EXPECT_NO_THROW((void)loadOpenVdbVolume(file.path,
      {.maxResolution = 16, .maxInputBytes = byteLimit, .sparse = true}));
  EXPECT_THROW((void)loadOpenVdbVolume(file.path,
      {.maxResolution = 16, .maxInputBytes = byteLimit, .temperatureGridName = "density", .sparse = true}), std::runtime_error);
  EXPECT_THROW((void)loadOpenVdbVolume(file.path,
      {.maxResolution = 16, .maxActiveVoxels = 1, .sparse = true}), std::runtime_error);
  density->getAccessor().setValueOff(openvdb::Coord(20), std::numeric_limits<float>::quiet_NaN());
  VolumeFile nonfinite({density});
  EXPECT_THROW((void)loadOpenVdbVolume(nonfinite.path,
      {.maxResolution = 16, .sparse = true}), std::runtime_error);
}

TEST(OpenVdbVolume, NativeSparseEmptyTemperatureRemainsZeroDespitePositiveBackground) {
  auto temperature = openvdb::FloatGrid::create(5000.0f);
  temperature->setName("temperature");
  VolumeFile file({fog(), temperature});
  const auto sparse = loadOpenVdbVolume(file.path,
      {.maxResolution = 16, .temperatureGridName = "temperature", .sparse = true});
  ASSERT_FALSE(sparse.sparseTemperature.empty());
  EXPECT_FLOAT_EQ(nativeSample(sparse.sparseTemperature, {0, 0, 0}, 1.0f), 0.0f);
}
#endif
#else
TEST(OpenVdbVolume, DisabledBuildReportsUnavailable) {
  EXPECT_FALSE(container::geometry::openVdbVolumeAvailable());
  EXPECT_THROW((void)loadOpenVdbVolume("missing.vdb"), std::runtime_error);
}
#endif
