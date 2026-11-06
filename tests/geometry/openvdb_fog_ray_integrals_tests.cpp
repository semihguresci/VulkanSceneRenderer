#include "Container/geometry/OpenVdbFogRayIntegrals.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/File.h>
#include <openvdb/io/io.h>
#include <openvdb/openvdb.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;
using Status = vdb::FogTransmissionStatus;
using Policy = vdb::FogTransmissionPolicy;

TEST(OpenVdbFogRayIntegralsAvailability, DisabledBeforeSourceOptionsAndRays) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::FogRayIntegralOptions options;
  options.maxCellSegments = options.maxCellSegmentsPerRay = 0;
  options.limits.maxWorkItems = 0;
  options.extinction = -1;
  vdb::ProcessingOptions processing;
  processing.maxInputBytes = 0;
  const std::vector<vdb::WorldRay> rays{{{}, {}, -1, 0}};
  try {
    (void)vdb::IntegrateFogRays("missing.vdb", rays, options, {}, processing);
    FAIL();
  } catch (const std::runtime_error &error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
template <class G> class ExactFogFixture : public G {
public:
  explicit ExactFogFixture(const G &grid)
      : G(openvdb::ConstPtrCast<typename G::TreeType>(grid.constTreePtr())) {
    this->insertMeta(grid);
    this->setTransform(
        std::make_shared<openvdb::math::Transform>(grid.transform()));
  }
  openvdb::GridBase::ConstPtr copyGrid() const override {
    return this->copyReplacingMetadataAndTransform(
        *this, std::make_shared<openvdb::math::Transform>(this->transform()));
  }
  openvdb::GridBase::Ptr copyGrid() override {
    return openvdb::ConstPtrCast<openvdb::GridBase>(
        static_cast<const ExactFogFixture &>(*this).copyGrid());
  }
};

class OpenVdbFogRayIntegrals : public ::testing::Test {
protected:
  using C = openvdb::Coord;
  using V = openvdb::Vec3d;
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{0};
    directory =
        std::filesystem::temp_directory_path() /
        ("container-fog-integrals-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
    options.limits.maxWorkItems = 10'000'000'000ull;
  }
  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  std::filesystem::path path(const std::string &name) const {
    return directory / name;
  }
  static std::string bytes(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
  }
  static void uniformMap(openvdb::GridBase &grid, double h) {
    if (h >= 1e-4) {
      grid.setTransform(openvdb::math::Transform::createLinearTransform(h));
      return;
    }
    std::stringstream record(std::ios::in | std::ios::out | std::ios::binary);
    openvdb::writeString(record, openvdb::math::UniformScaleMap::mapType());
    for (const double value : {h, h, 1 / h, 1 / (h * h), .5 / h})
      V(value).write(record);
    record.seekg(0);
    openvdb::io::setCurrentVersion(record);
    grid.transform().read(record);
  }
  template <class G = openvdb::DoubleGrid>
  static typename G::Ptr field(double background = 0) {
    auto grid = G::create(typename G::ValueType(background));
    grid->setName("density");
    grid->setGridClass(openvdb::GRID_FOG_VOLUME);
    grid->setVectorType(openvdb::VEC_INVARIANT);
    grid->setIsInWorldSpace(true);
    return grid;
  }
  void write(const std::string &name, const openvdb::GridPtrVec &grids,
             const openvdb::MetaMap &metadata = {}, bool statistics = true) {
    openvdb::GridPtrVec exact;
    for (const auto &grid : grids)
      if (grid->isType<openvdb::FloatGrid>())
        exact.push_back(std::make_shared<ExactFogFixture<openvdb::FloatGrid>>(
            *openvdb::gridPtrCast<openvdb::FloatGrid>(grid)));
      else if (grid->isType<openvdb::DoubleGrid>())
        exact.push_back(std::make_shared<ExactFogFixture<openvdb::DoubleGrid>>(
            *openvdb::gridPtrCast<openvdb::DoubleGrid>(grid)));
      else
        exact.push_back(grid);
    openvdb::io::File file(path(name).string());
    file.setCompression(openvdb::io::COMPRESS_ZIP);
    file.setGridStatsMetadataEnabled(statistics);
    file.write(exact, metadata);
  }
  static vdb::WorldRay ray(V origin = V(-1, 0, 0), V direction = V(7, 0, 0),
                           double maximum = 2, double minimum = 0) {
    return {{origin[0], origin[1], origin[2]},
            {direction[0], direction[1], direction[2]},
            minimum,
            maximum};
  }
  vdb::FogRayIntegralBatch integrate(const std::vector<vdb::WorldRay> &rays,
                                     const std::string &name = "input.vdb",
                                     const std::string &selector = {}) {
    return vdb::IntegrateFogRays(path(name), rays, options, selector,
                                 processing);
  }
  void structure(const vdb::FogRayIntegralBatch &batch, size_t count) {
    ASSERT_EQ(batch.rays.size(), count);
    EXPECT_GT(batch.workItems, 0u);
    EXPECT_LE(batch.workItems, options.limits.maxWorkItems);
    EXPECT_LE(batch.cellSegments, batch.cellSegmentUpperBound);
    EXPECT_LE(batch.cellSegmentUpperBound, options.maxCellSegments);
    uint64_t actual = 0;
    for (const auto &value : batch.rays) {
      actual += value.cellSegments;
      EXPECT_LE(value.cellSegments, options.maxCellSegmentsPerRay);
      EXPECT_NEAR(std::hypot(value.normalizedDirection[0],
                             value.normalizedDirection[1],
                             value.normalizedDirection[2]),
                  1, 1e-13);
      EXPECT_TRUE(std::isfinite(value.densityIntegral));
      EXPECT_GE(value.densityIntegral, 0);
      EXPECT_TRUE(std::isfinite(value.opticalDepth));
      EXPECT_GE(value.opticalDepth, 0);
      EXPECT_GE(value.transmittance, 0);
      EXPECT_LE(value.transmittance, 1);
      EXPECT_GE(value.absorbedFraction, 0);
      EXPECT_LE(value.absorbedFraction, 1);
    }
    EXPECT_EQ(actual, batch.cellSegments);
  }
  static void near(double actual, double expected, double relative = 2e-12) {
    EXPECT_NEAR(actual, expected,
                std::max(std::abs(expected) * relative,
                         8 * std::numeric_limits<double>::denorm_min()));
  }
  std::filesystem::path directory;
  vdb::FogRayIntegralOptions options;
  vdb::ProcessingOptions processing;
};

TEST_F(OpenVdbFogRayIntegrals,
       ActualDefaultSingletonIntegratesConstantBackground) {
  write("input.vdb", {field(.5)});
  const std::vector<vdb::WorldRay> rays{ray(V(0), V(1, 0, 0), 1)};
  const auto batch = vdb::IntegrateFogRays(path("input.vdb"), rays);
  ASSERT_EQ(batch.rays.size(), 1u);
  EXPECT_EQ(batch.backgroundDensity, .5);
  EXPECT_TRUE(batch.numericallyConstant);
  EXPECT_TRUE(batch.source.empty);
  EXPECT_EQ(batch.rays[0].densityIntegral, .5);
  near(batch.rays[0].transmittance, std::exp(-.5));
  EXPECT_LE(batch.workItems, vdb::FogRayIntegralOptions{}.limits.maxWorkItems);
}

TEST_F(OpenVdbFogRayIntegrals,
       FloatDoubleNodalTentsUseValuesIndependentlyOfActivity) {
  for (bool nativeFloat : {false, true})
    for (bool active : {false, true}) {
      SCOPED_TRACE(nativeFloat);
      SCOPED_TRACE(active);
      auto grid = nativeFloat
                      ? openvdb::GridBase::Ptr(field<openvdb::FloatGrid>())
                      : openvdb::GridBase::Ptr(field());
      if (nativeFloat) {
        auto typed = openvdb::gridPtrCast<openvdb::FloatGrid>(grid);
        typed->tree().setValueOnly(C(0), 3.f);
        typed->tree().setActiveState(C(0), active);
      } else {
        auto typed = openvdb::gridPtrCast<openvdb::DoubleGrid>(grid);
        typed->tree().setValueOnly(C(0), 3.);
        typed->tree().setActiveState(C(0), active);
      }
      write("input.vdb", {grid});
      const auto before = bytes(path("input.vdb"));
      const auto batch = integrate({ray(), ray(V(1, 0, 0), V(-2, 0, 0))});
      structure(batch, 2);
      for (const auto &value : batch.rays) {
        near(value.densityIntegral, 3);
        near(value.opticalDepth, 3);
        near(value.transmittance, std::exp(-3.));
      }
      EXPECT_FALSE(batch.numericallyConstant);
      EXPECT_EQ(batch.source.inputActiveVoxelCount, active ? 1u : 0u);
      EXPECT_EQ(batch.source.empty, !active);
      EXPECT_EQ(bytes(path("input.vdb")), before);
    }
}

TEST_F(OpenVdbFogRayIntegrals,
       CubicDiagonalAndMultilinearPolynomialHaveAnalyticIntegrals) {
  for (bool addOne : {false, true}) {
    auto grid = field();
    for (int x = 0; x <= 1; ++x)
      for (int y = 0; y <= 1; ++y)
        for (int z = 0; z <= 1; ++z)
          grid->tree().setValue(C(x, y, z), (addOne ? 1. : 0.) + x * y * z);
    write("input.vdb", {grid});
    const double length = std::sqrt(3.);
    const auto batch =
        integrate({ray(V(0), V(1), length), ray(V(1), V(-1), length)});
    structure(batch, 2);
    const double expected = length * (addOne ? 1.25 : .25);
    near(batch.rays[0].densityIntegral, expected);
    near(batch.rays[1].densityIntegral, expected);
  }
  auto grid = field();
  // Along the diagonal: 2+3s+9s^2+8s^3, whose mean on[0,1] is8.5.
  for (int x = 0; x <= 1; ++x)
    for (int y = 0; y <= 1; ++y)
      for (int z = 0; z <= 1; ++z)
        grid->tree().setValue(C(x, y, z), 2. + x + y + z + 2 * x * y +
                                              3 * x * z + 4 * y * z +
                                              8 * x * y * z);
  write("input.vdb", {grid});
  near(integrate({ray(V(0), V(1), std::sqrt(3.))}).rays[0].densityIntegral,
       8.5 * std::sqrt(3.));
}

TEST_F(OpenVdbFogRayIntegrals,
       PositiveBackgroundAndInactiveZeroHoleIncludeOuterIntervals) {
  auto grid = field(2);
  write("input.vdb", {grid});
  auto batch = integrate({ray(V(-2, 0, 0), V(1, 0, 0), 4)});
  structure(batch, 1);
  EXPECT_TRUE(batch.numericallyConstant);
  EXPECT_TRUE(batch.source.empty);
  EXPECT_EQ(batch.rays[0].densityIntegral, 8);
  EXPECT_EQ(batch.cellSegments, 0u);
  grid->tree().touchLeaf(C(0));
  grid->tree().setValueOff(C(0), 0);
  write("input.vdb", {grid});
  batch = integrate({ray(V(-2, 0, 0), V(1, 0, 0), 4)});
  structure(batch, 1);
  EXPECT_TRUE(batch.source.empty);
  EXPECT_FALSE(batch.numericallyConstant);
  near(batch.rays[0].densityIntegral, 6);
  EXPECT_GT(batch.cellSegments, 0u);
}

TEST_F(OpenVdbFogRayIntegrals,
       TinyNegativeCoordinateRetainsIndependentLeftComplement) {
  auto grid = field();
  grid->tree().setValue(C(-1, 0, 0), std::ldexp(1., 19));
  write("input.vdb", {grid});
  const auto batch = integrate({ray(
      V(-std::numeric_limits<double>::denorm_min(), 0, 0), V(0, 1, 0), .5)});
  structure(batch, 1);
  const double expected = std::ldexp(3., -1058);
  EXPECT_GT(batch.rays[0].densityIntegral, 0);
  near(batch.rays[0].densityIntegral, expected);
  EXPECT_EQ(batch.rays[0].transmittance, 1);
  near(batch.rays[0].absorbedFraction, expected);
}

TEST_F(OpenVdbFogRayIntegrals,
       TinyWeightProductRecoversBeforeRoundingToDouble) {
  auto grid = field();
  grid->tree().setValue(C(1), std::ldexp(1., 20));
  write("input.vdb", {grid});
  const auto batch = integrate(
      {ray(V(0, std::ldexp(1., -537), std::ldexp(1., -538)), V(1, 0, 0), 1)});
  structure(batch, 1);
  EXPECT_GT(batch.rays[0].densityIntegral, 0);
  near(batch.rays[0].densityIntegral, std::ldexp(1., -1056));
}

TEST_F(OpenVdbFogRayIntegrals,
       ReverseSplitAndNonunitDirectionsAgreeAcrossLatticePlanes) {
  auto grid = field();
  for (int x = -2; x <= 2; ++x)
    for (int y = -2; y <= 2; ++y)
      for (int z = -2; z <= 2; ++z)
        grid->tree().setValue(C(x, y, z), 2. + .25 * x + .125 * y + .0625 * z);
  write("input.vdb", {grid});
  const V start(-1.5, -1.5, -1.5), finish(1.5, 1.5, 1.5);
  const double length = (finish - start).length(), split = .37 * length;
  const auto batch =
      integrate({ray(start, V(1), length), ray(finish, V(-3), length),
                 ray(start, V(7), split), ray(start, V(2), length, split)});
  structure(batch, 4);
  near(batch.rays[0].densityIntegral, 2 * length);
  near(batch.rays[1].densityIntegral, 2 * length);
  near(batch.rays[2].densityIntegral + batch.rays[3].densityIntegral,
       2 * length);
  EXPECT_GT(batch.rays[0].cellSegments, 1u);
  // A two-axis tie and negative zero components must not lose a cell or loop.
  const auto planar =
      integrate({ray(V(-1, -1, 0), V(1, 1, -0.), 2 * std::sqrt(2.))});
  near(planar.rays[0].densityIntegral, 4 * std::sqrt(2.));
}

TEST_F(OpenVdbFogRayIntegrals,
       PositiveFinalSliverIsNotDroppedByNativeMarchEpsilon) {
  auto grid = field(1e6);
  // Keep a nonconstant interior so this exercises a lattice-cell interval.
  grid->tree().setValue(C(0), 2e6);
  write("input.vdb", {grid});
  const double length = std::ldexp(1., -40);
  const auto batch = integrate({ray(V(0), V(1, 0, 0), length)});
  structure(batch, 1);
  near(batch.rays[0].densityIntegral, 2e6 * length - .5e6 * length * length);
  EXPECT_GT(batch.rays[0].densityIntegral, 0);
}

TEST_F(OpenVdbFogRayIntegrals,
       WorldScaleRotationReflectionAndRemoteIndicesPreserveIntegral) {
  for (double h : {1e-6, 1., 1e6}) {
    auto grid = field();
    grid->tree().setValue(C(0), 2);
    uniformMap(*grid, h);
    write("input.vdb", {grid});
    options.extinction = 1 / h;
    const auto batch = integrate({ray(V(-h, 0, 0), V(1, 0, 0), 2 * h)});
    structure(batch, 1);
    near(batch.rays[0].densityIntegral, 2 * h);
    near(batch.rays[0].opticalDepth, 2);
  }
  const C offset(1048576, -8192, 4096);
  auto grid = field();
  grid->tree().setValue(offset, 3);
  uniformMap(*grid, 2);
  grid->transform().postScale(V(-1, 1, 1));
  grid->transform().postRotate(.37, openvdb::math::Z_AXIS);
  grid->transform().postTranslate(V(100.125, -50, 7));
  write("input.vdb", {grid});
  const V origin = grid->indexToWorld(offset.asVec3d() + V(-1, 0, 0));
  const V finish = grid->indexToWorld(offset.asVec3d() + V(1, 0, 0));
  options.extinction = 1;
  const auto batch = integrate({ray(origin, finish - origin, 4)});
  structure(batch, 1);
  near(batch.rays[0].densityIntegral, 6, 2e-9);
  for (int row = 0; row < 3; ++row) {
    const auto zero = grid->indexToWorld(V(0));
    EXPECT_EQ(batch.source.indexToWorld[4 * row + 3], zero[row]);
    for (int column = 0; column < 3; ++column) {
      V axis(0);
      axis[column] = 1;
      near(batch.source.indexToWorld[4 * row + column],
           grid->transform().baseMap()->applyJacobian(axis)[row]);
    }
  }
}

TEST_F(OpenVdbFogRayIntegrals,
       BeerThinAbsorptionZeroExtinctionAndExplicitOpaquePolicy) {
  write("input.vdb", {field(.125)});
  options.extinction = 2;
  auto batch = integrate({ray()});
  structure(batch, 1);
  EXPECT_EQ(batch.rays[0].densityIntegral, .25);
  EXPECT_EQ(batch.rays[0].opticalDepth, .5);
  near(batch.rays[0].transmittance, std::exp(-.5));
  near(batch.rays[0].absorbedFraction, -std::expm1(-.5));
  write("input.vdb", {field(std::ldexp(1., -1060))});
  options.extinction = 1;
  batch = integrate({ray(V(0), V(1, 0, 0), 1)});
  EXPECT_EQ(batch.rays[0].transmittance, 1);
  EXPECT_GT(batch.rays[0].absorbedFraction, 0);
  near(batch.rays[0].absorbedFraction, std::ldexp(1., -1060));
  write("input.vdb", {field(1000)});
  EXPECT_THROW((void)integrate({ray(V(0), V(1, 0, 0), 1)}), std::exception);
  options.transmissionPolicy = Policy::AllowOpaqueUnderflow;
  batch = integrate({ray(V(0), V(1, 0, 0), 1)});
  EXPECT_EQ(batch.rays[0].transmittance, 0);
  EXPECT_EQ(batch.rays[0].absorbedFraction, 1);
  EXPECT_EQ(batch.rays[0].transmissionStatus, Status::OpaqueUnderflow);
  options.extinction = 0;
  batch = integrate({ray(V(0), V(1, 0, 0), 1)});
  EXPECT_EQ(batch.rays[0].densityIntegral, 1000);
  EXPECT_EQ(batch.rays[0].opticalDepth, 0);
  EXPECT_EQ(batch.rays[0].transmittance, 1);
  EXPECT_EQ(batch.rays[0].absorbedFraction, 0);
  EXPECT_EQ(batch.rays[0].transmissionStatus, Status::Represented);
}

TEST_F(OpenVdbFogRayIntegrals,
       FinalIntegralAndOpticalRangeCannotBeHiddenByOtherFactors) {
  options.transmissionPolicy = Policy::AllowOpaqueUnderflow;
  write("input.vdb", {field(std::numeric_limits<double>::max())});
  options.extinction = 0;
  EXPECT_THROW((void)integrate({ray()}), std::exception);
  auto batch = integrate({ray(V(0), V(1, 0, 0), .25)});
  near(batch.rays[0].densityIntegral, std::numeric_limits<double>::max() * .25);
  EXPECT_EQ(batch.rays[0].transmittance, 1);
  options.extinction = 8;
  EXPECT_THROW((void)integrate({ray(V(0), V(1, 0, 0), .25)}), std::exception);
  write("input.vdb", {field(std::numeric_limits<double>::denorm_min())});
  options.extinction = std::numeric_limits<double>::max();
  EXPECT_THROW((void)integrate({ray(V(0), V(1, 0, 0), .25)}), std::exception);
  options.extinction = .25;
  EXPECT_THROW((void)integrate({ray(V(0), V(1, 0, 0), 1)}), std::exception);
  options.extinction = 0;
  batch = integrate({ray(V(0), V(1, 0, 0), 1)});
  EXPECT_EQ(batch.rays[0].densityIntegral,
            std::numeric_limits<double>::denorm_min());
  // Mathematical .75*denorm is below the positive Double range even though
  // round-to-nearest would produce denorm rather than zero.
  EXPECT_THROW((void)integrate({ray(V(0), V(1, 0, 0), .75)}), std::exception);
  const double maximum = std::numeric_limits<double>::max();
  auto justOver = field(maximum * .5);
  justOver->tree().setValue(C(0, 1, 0), maximum);
  write("just-over.vdb", {justOver});
  const auto before = bytes(path("just-over.vdb"));
  // Constant BG contributes MAX; the positive knot adds MAX*2^-57. The
  // exact integral lies above MAX although nearest rounding returns MAX.
  EXPECT_THROW(
      (void)integrate({ray(V(-1, std::ldexp(1., -56), 0), V(1, 0, 0), 2)},
                      "just-over.vdb"),
      std::exception);
  EXPECT_EQ(bytes(path("just-over.vdb")), before);
}

TEST_F(OpenVdbFogRayIntegrals,
       InactivePositiveTileIsIntegratedWithoutVoxelExpansion) {
  auto grid = field();
  grid->tree().addTile(2, C(0), 2, false);
  write("input.vdb", {grid});
  processing.maxVoxelCount = 1;
  const auto batch = integrate({ray(V(-1, 64, 64), V(1, 0, 0), 129)});
  structure(batch, 1);
  EXPECT_TRUE(batch.source.empty);
  EXPECT_EQ(batch.source.inputStoredLeafCount, 0u);
  near(batch.rays[0].densityIntegral, 256);
  processing = {};
  grid = field();
  grid->tree().addTile(1, C(0), 1, true);
  write("input.vdb", {grid});
  processing.maxVoxelCount = 511;
  EXPECT_THROW((void)integrate({}), std::exception);
}

TEST_F(OpenVdbFogRayIntegrals,
       SparseGapBoundsAndRemoteInactiveLeavesRespectSharedCaps) {
  auto grid = field();
  grid->tree().setValue(C(0), 1);
  grid->tree().setValueOff(C(1000000, 0, 0), 1);
  write("input.vdb", {grid});
  options.maxCellSegmentsPerRay = 16;
  options.maxCellSegments = 16;
  EXPECT_THROW((void)integrate({ray(V(-1, 0, 0), V(1, 0, 0), 1000002)}),
               std::exception);
  near(integrate({ray()}).rays[0].densityIntegral, 1);
  grid = field();
  grid->tree().setValue(C(0), 1);
  grid->tree().touchLeaf(C(1000000, 0, 0));
  grid->tree().setValueOff(C(1000000, 0, 0), 0);
  write("input.vdb", {grid}, {}, false);
  processing.maxVoxelCount = 512;
  EXPECT_THROW((void)integrate({}), std::exception);
  processing.maxVoxelCount = 1024;
  const auto batch = integrate({ray()});
  EXPECT_EQ(batch.source.inputStoredLeafCount, 2u);
  near(batch.rays[0].densityIntegral, 1);
}

TEST_F(OpenVdbFogRayIntegrals,
       MetadataUnicodeDescriptorsMissingStatisticsAndEmptyBatchAreReadOnly) {
  auto first = field();
  first->tree().setValue(C(0), 1);
  first->insertMeta("artist",
                    openvdb::StringMetadata("retained density source"));
  first->insertMeta("exact_integer",
                    openvdb::Int64Metadata(INT64_C(9007199254740993)));
  auto second = field();
  second->tree().setValue(C(0), 2);
  openvdb::MetaMap metadata;
  metadata.insertMeta("scene",
                      openvdb::StringMetadata("file density metadata"));
  write("input.vdb", {first, second}, metadata, false);
  const auto before = bytes(path("input.vdb"));
  EXPECT_THROW((void)integrate({}, "input.vdb", "density"), std::exception);
  const auto empty = integrate({}, "input.vdb", "descriptor:density[0]");
  structure(empty, 0);
  EXPECT_EQ(empty.source.descriptorName, "density[0]");
  bool artist = false, integer = false, scene = false;
  for (const auto &item : empty.source.metadata) {
    if (item.name == "artist") {
      artist = true;
      EXPECT_EQ(item.value, "retained density source");
    }
    if (item.name == "exact_integer") {
      integer = true;
      EXPECT_EQ(item.value, "9007199254740993");
    }
  }
  for (const auto &item : empty.source.fileMetadata)
    if (item.name == "scene") {
      scene = true;
      EXPECT_EQ(item.value, "file density metadata");
    }
  EXPECT_TRUE(artist);
  EXPECT_TRUE(integer);
  EXPECT_TRUE(scene);
  near(integrate({ray()}, "input.vdb", "descriptor:density[1]")
           .rays[0]
           .densityIntegral,
       2);
  const auto unicode = directory / std::filesystem::path(u"density_球.vdb");
  std::filesystem::copy_file(path("input.vdb"), unicode);
  const std::vector<vdb::WorldRay> rays{ray()};
  const auto copied = vdb::IntegrateFogRays(
      unicode, rays, options, "descriptor:density[0]", processing);
  near(copied.rays[0].densityIntegral, 1);
  first->setName("descriptor:literal");
  write("literal.vdb", {first});
  near(integrate({ray()}, "literal.vdb", "descriptor:descriptor:literal")
           .rays[0]
           .densityIntegral,
       1);
  EXPECT_THROW((void)integrate({ray()}, "input.vdb", "descriptor:missing"),
               std::exception);
  EXPECT_EQ(bytes(path("input.vdb")), before);
  EXPECT_EQ(bytes(unicode), before);
}

TEST_F(OpenVdbFogRayIntegrals,
       UnicodeTemporaryDirectoryUsesCheckedAsciiNativeScratch) {
#if defined(_WIN32)
  auto grid = field();
  grid->tree().setValue(C(0), 1);
  write("input.vdb", {grid});
  const auto source = path("input.vdb");
  const auto before = bytes(source);
  const auto temporary = directory / std::filesystem::path(u"一時_é");
  std::filesystem::create_directory(temporary);
  struct RestoreEnvironment {
    std::wstring temporary = _wgetenv(L"TEMP") ? _wgetenv(L"TEMP") : L"";
    std::wstring scratch = _wgetenv(L"TMP") ? _wgetenv(L"TMP") : L"";
    ~RestoreEnvironment() {
      _wputenv_s(L"TEMP", temporary.c_str());
      _wputenv_s(L"TMP", scratch.c_str());
    }
  } restore;
  ASSERT_EQ(_wputenv_s(L"TEMP", temporary.c_str()), 0);
  ASSERT_EQ(_wputenv_s(L"TMP", temporary.c_str()), 0);
  // GetTempPath may append a separator. Compare the native directory identity;
  // a failed GTest path printer would itself narrow the Unicode path.
  ASSERT_TRUE(std::filesystem::equivalent(
      std::filesystem::temp_directory_path(), temporary));
  const auto result = integrate({ray()});
  structure(result, 1);
  near(result.rays[0].densityIntegral, 1);
  EXPECT_EQ(bytes(source), before);
  EXPECT_TRUE(std::filesystem::is_empty(temporary));
#else
  GTEST_SKIP() << "Windows native SDK narrow-path fallback regression";
#endif
}

TEST_F(OpenVdbFogRayIntegrals,
       MissingHintsAreAllowedButMalformedAndStaleHintsReject) {
  auto grid = field();
  grid->tree().setValue(C(0), 1);
  write("input.vdb", {grid}, {}, false);
  near(integrate({ray()}).rays[0].densityIntegral, 1);
  grid->insertMeta(openvdb::GridBase::META_FILE_MEM_BYTES,
                   openvdb::Int64Metadata(-1));
  write("negative.vdb", {grid}, {}, false);
  EXPECT_THROW((void)integrate({}, "negative.vdb"), std::exception);
  grid->removeMeta(openvdb::GridBase::META_FILE_MEM_BYTES);
  grid->insertMeta(openvdb::GridBase::META_FILE_MEM_BYTES,
                   openvdb::StringMetadata("not bytes"));
  write("wrongtype.vdb", {grid}, {}, false);
  EXPECT_THROW((void)integrate({}, "wrongtype.vdb"), std::exception);
  grid->removeMeta(openvdb::GridBase::META_FILE_MEM_BYTES);
  grid->insertMeta(openvdb::GridBase::META_FILE_VOXEL_COUNT,
                   openvdb::Int64Metadata(2));
  write("stale.vdb", {grid}, {}, false);
  EXPECT_THROW((void)integrate({}, "stale.vdb"), std::exception);
}

TEST_F(OpenVdbFogRayIntegrals,
       NonfiniteNegativeAndSemanticSourcesRejectEvenEmptyQueries) {
  for (int failure = 0; failure < 13; ++failure) {
    SCOPED_TRACE(failure);
    auto grid = field();
    grid->tree().setValue(C(0), 1);
    if (failure == 0)
      grid->tree().setValue(C(0), -1);
    if (failure == 1)
      grid->tree().setValueOff(C(1000000, 0, 0), -1);
    if (failure == 2)
      grid->tree().setValueOff(C(1000000, 0, 0),
                               std::numeric_limits<double>::quiet_NaN());
    if (failure == 3)
      grid->tree().root().setBackground(-1, false);
    if (failure == 4)
      grid->tree().root().setBackground(std::numeric_limits<double>::infinity(),
                                        false);
    if (failure == 5)
      grid->setGridClass(openvdb::GRID_LEVEL_SET);
    if (failure == 6) {
      grid->removeMeta(openvdb::GridBase::META_GRID_CLASS);
      grid->insertMeta(openvdb::GridBase::META_GRID_CLASS,
                       openvdb::StringMetadata("unrecognized"));
    }
    if (failure == 7) {
      grid->removeMeta(openvdb::GridBase::META_GRID_CLASS);
      grid->insertMeta(openvdb::GridBase::META_GRID_CLASS,
                       openvdb::Int32Metadata(7));
    }
    if (failure == 8)
      grid->setVectorType(openvdb::VEC_COVARIANT);
    if (failure == 9) {
      grid->removeMeta(openvdb::GridBase::META_VECTOR_TYPE);
      grid->insertMeta(openvdb::GridBase::META_VECTOR_TYPE,
                       openvdb::Int32Metadata(7));
    }
    if (failure == 10)
      grid->setIsInWorldSpace(false);
    if (failure == 11) {
      grid->removeMeta(openvdb::GridBase::META_IS_LOCAL_SPACE);
      grid->insertMeta(openvdb::GridBase::META_IS_LOCAL_SPACE,
                       openvdb::StringMetadata("false"));
    }
    if (failure == 12)
      grid->tree().setValue(C(0), std::numeric_limits<double>::infinity());
    write("input.vdb", {grid});
    const auto before = bytes(path("input.vdb"));
    options.extinction = 0;
    EXPECT_THROW((void)integrate({}), std::exception);
    EXPECT_EQ(bytes(path("input.vdb")), before);
  }
  auto integer = openvdb::Int32Grid::create(0);
  write("integer.vdb", {integer});
  EXPECT_THROW((void)integrate({}, "integer.vdb"), std::exception);
  auto unknown = field();
  unknown->setGridClass(openvdb::GRID_UNKNOWN);
  unknown->tree().setValue(C(0), 1);
  write("unknown.vdb", {unknown});
  near(integrate({ray()}, "unknown.vdb").rays[0].densityIntegral, 1);
}

TEST_F(OpenVdbFogRayIntegrals, InvalidRaysMapsAndInterpolationExtentsReject) {
  auto grid = field();
  grid->tree().setValue(C(0), 1);
  write("input.vdb", {grid});
  for (const auto &invalid :
       {ray(V(0), V(0)), ray(V(0), V(1, 0, 0), 1, -1),
        ray(V(0), V(1, 0, 0), 1, 1), ray(V(1e13, 0, 0), V(1, 0, 0), 1),
        ray(V(0), V(std::numeric_limits<double>::infinity(), 0, 0))})
    EXPECT_THROW((void)integrate({invalid}), std::exception);
  grid->transform().postScale(V(1, 2, 1));
  write("input.vdb", {grid});
  EXPECT_THROW((void)integrate({ray()}), std::exception);
  grid = field();
  grid->tree().setValue(C(INT32_MAX, 0, 0), 1);
  write("input.vdb", {grid});
  EXPECT_THROW((void)integrate({}), std::exception);
  grid = field();
  grid->tree().setValue(C(0), 1);
  grid->transform().postTranslate(V(1e16, 0, 0));
  write("input.vdb", {grid});
  EXPECT_THROW((void)integrate({ray(V(1e16, 0, 0), V(1, 0, 0), 1)}),
               std::exception);
}

TEST_F(OpenVdbFogRayIntegrals,
       ExactAffineInverseCannotEraseARescuableNonzeroOffDiagonal) {
  constexpr double h = 1e6, epsilon = 1e-314;
  auto grid = field();
  grid->tree().setValue(C(0), 1);
  auto matrix = openvdb::math::Mat4d::identity();
  for (int axis = 0; axis < 3; ++axis)
    matrix(axis, axis) = h;
  // SDK matrices act on row vectors, so these preserve the WORLD-column
  // Jacobian [[h,-epsilon,0],[epsilon,h,0],[0,0,h]]. Convenience factories
  // may simplify this near-diagonal map and hide the original coefficients.
  matrix(0, 1) = epsilon;
  matrix(1, 0) = -epsilon;
  std::stringstream record(std::ios::in | std::ios::out | std::ios::binary);
  openvdb::writeString(record, openvdb::math::AffineMap::mapType());
  matrix.write(record);
  record.seekg(0);
  openvdb::io::setCurrentVersion(record);
  grid->transform().read(record);
  write("input.vdb", {grid});
  {
    openvdb::io::File file(path("input.vdb").string());
    file.open(false);
    const auto persisted = file.readGrid("density");
    const auto map = persisted->transform().baseMap();
    EXPECT_EQ(map->applyJacobian(V(1, 0, 0))[1], epsilon);
    EXPECT_EQ(map->applyJacobian(V(0, 1, 0))[0], -epsilon);
    EXPECT_EQ(map->applyInverseJacobian(V(1, 0, 0))[1], 0);
    // A WORLD vector1e12 can rescue the mathematically nonzero inverse term;
    // zero in the cache therefore cannot certify a truly diagonal map.
    EXPECT_GT((epsilon * (1e12 / h)) / h, 0);
  }
  const auto before = bytes(path("input.vdb"));
  try {
    (void)integrate({});
    FAIL() << "Loss of a nonzero inverse coefficient must reject before rays";
  } catch (const std::invalid_argument &error) {
    EXPECT_NE(std::string(error.what()).find("cofactor"), std::string::npos);
  }
  EXPECT_EQ(bytes(path("input.vdb")), before);
}

TEST_F(OpenVdbFogRayIntegrals,
       ExactMappedCoordinateRetainsAnInteriorComplementAfterIntegerRounding) {
  constexpr double epsilon = 1e-314;
  auto grid = field();
  grid->tree().setValue(C(1, 0, 0), std::ldexp(1., 20));
  auto matrix = openvdb::math::Mat4d::identity();
  matrix(0, 1) = epsilon;
  matrix(1, 0) = -epsilon;
  std::stringstream record(std::ios::in | std::ios::out | std::ios::binary);
  openvdb::writeString(record, openvdb::math::AffineMap::mapType());
  matrix.write(record);
  record.seekg(0);
  openvdb::io::setCurrentVersion(record);
  grid->transform().read(record);
  write("input.vdb", {grid});
  {
    openvdb::io::File file(path("input.vdb").string());
    file.open(false);
    const auto persisted = file.readGrid("density");
    const auto map = persisted->transform().baseMap();
    ASSERT_EQ(map->applyJacobian(V(1, 0, 0))[1], epsilon);
    ASSERT_EQ(map->applyJacobian(V(0, 1, 0))[0], -epsilon);
    ASSERT_EQ(map->applyInverseJacobian(V(1, 0, 0))[1], -epsilon);
    ASSERT_NE(map->applyInverseJacobian(V(1, 0, 0))[1], 0);
    // SDK Double coordinate evaluation erases q=(1+epsilon,1-epsilon,0).
    EXPECT_EQ(map->applyInverseMap(V(1, 1, 0)), V(1, 1, 0));
  }
  const auto before = bytes(path("input.vdb"));
  const auto batch = integrate({ray(V(1, 1, 0), V(0, 0, 1), .5)});
  structure(batch, 1);
  // Exact field integral is (3/8)*2^20*epsilon*(1-epsilon). The epsilon^2
  // correction is far below one Double denorm ulp; this scaled oracle does not
  // reconstruct or reuse the implementation's exact-coordinate machinery.
  const double expected = std::ldexp(epsilon, 17) * 3;
  EXPECT_GT(batch.rays[0].densityIntegral, 0);
  EXPECT_NEAR(batch.rays[0].densityIntegral, expected,
              8 * std::numeric_limits<double>::denorm_min());
  EXPECT_EQ(bytes(path("input.vdb")), before);
}

TEST_F(OpenVdbFogRayIntegrals,
       SharedWorkBytesMetadataQueryAndCellCapsAreAuthoritative) {
  auto grid = field();
  grid->tree().setValue(C(0), 1);
  write("input.vdb", {grid});
  const auto before = bytes(path("input.vdb"));
  const auto good = options;
  options.limits.maxRays = 1;
  EXPECT_THROW((void)integrate({ray(), ray()}), std::exception);
  options = good;
  options.limits.maxWorkItems = 1;
  EXPECT_THROW((void)integrate({}), std::exception);
  options = good;
  options.limits.maxMetadataBytes = 1;
  EXPECT_THROW((void)integrate({}), std::exception);
  options = good;
  processing.maxInputBytes = 1;
  EXPECT_THROW((void)integrate({}), std::exception);
  processing = {};
  options.maxCellSegmentsPerRay = 1;
  EXPECT_THROW((void)integrate({ray()}), std::exception);
  options = good;
  options.maxCellSegments = 1;
  EXPECT_THROW((void)integrate({ray()}), std::exception);
  options = good;
  for (int bad = 0; bad < 5; ++bad) {
    options = good;
    if (bad == 0)
      options.maxCellSegmentsPerRay = 1000001;
    if (bad == 1)
      options.maxCellSegments = 0;
    if (bad == 2)
      options.extinction = -1;
    if (bad == 3)
      options.extinction = std::numeric_limits<double>::quiet_NaN();
    if (bad == 4)
      options.transmissionPolicy = static_cast<Policy>(99);
    EXPECT_THROW((void)integrate({}), std::exception);
  }
  options = good;
  auto other = field();
  other->setName("other");
  write("two.vdb", {grid, other});
  options.limits.maxGrids = 1;
  EXPECT_THROW((void)integrate({}, "two.vdb", "descriptor:density"),
               std::exception);
  options = good;
  processing.voxelSize = processing.halfWidth = 0;
  processing.maxTriangles = 0;
  processing.gridName = std::string("\xff", 1);
  near(integrate({ray()}).rays[0].densityIntegral, 1);
  EXPECT_EQ(bytes(path("input.vdb")), before);
  std::ofstream corrupt(path("corrupt.vdb"), std::ios::binary);
  corrupt << "not a VDB archive";
  corrupt.close();
  EXPECT_THROW((void)integrate({}, "corrupt.vdb"), std::exception);
}
#endif
} // namespace
