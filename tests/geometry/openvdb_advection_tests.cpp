#include "Container/geometry/OpenVdbAdvection.h"
#include "Container/geometry/OpenVdbLoader.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <cstdlib>
#include <limits>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "../../src/geometry/OpenVdbFileUtils.h"
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;

TEST(OpenVdbAdvectionAvailability, DisabledBuildExplainsFeatureOption) {
  if (vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB is enabled";
  EXPECT_THROW((void)vdb::WriteConstantVelocity("disabled-advection.vdb", {1,0,0}), std::runtime_error);
  try {
    (void)vdb::WriteAdvectedFog("missing.vdb", "missing-velocity.vdb", "disabled-advection.vdb");
    FAIL() << "disabled advection must throw";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists("disabled-advection.vdb"));
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbAdvection : public ::testing::Test {
protected:
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{0};
    directory = std::filesystem::temp_directory_path() /
        ("container-vdb-advection-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
    options.voxelSize = 0.25;
    options.gridName = "density";
  }
  void TearDown() override {
    if (directory.empty()) return;
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  std::filesystem::path file(const char* name) const { return directory / name; }
  static void write(const std::filesystem::path& path, const openvdb::GridPtrVec& grids) {
    openvdb::io::File output(path.string());
    output.write(grids);
    output.close();
  }
  static openvdb::GridBase::Ptr read(const std::filesystem::path& path, const char* name) {
    openvdb::io::File input(path.string());
    input.open(false);
    auto grid = input.readGrid(name);
    input.close();
    return grid;
  }
  openvdb::FloatGrid::Ptr fog(float density = 1.0f) {
    auto grid = openvdb::FloatGrid::create(0.0f);
    grid->setName("smoke");
    grid->setGridClass(openvdb::GRID_FOG_VOLUME);
    grid->setTransform(openvdb::math::Transform::createLinearTransform(0.25));
    grid->tree().setValue(openvdb::Coord(0), density);
    grid->insertMeta("artist", openvdb::StringMetadata("original fog"));
    return grid;
  }
  openvdb::Vec3DGrid::Ptr velocity(openvdb::Vec3d vector = {1,0,0}) {
    auto grid = openvdb::Vec3DGrid::create(vector);
    grid->setName("wind");
    grid->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
    grid->setIsInWorldSpace(true);
    return grid;
  }
  float sample(const char* name, openvdb::Coord coordinate) {
    return openvdb::gridPtrCast<openvdb::FloatGrid>(read(file(name), "density"))->tree().getValue(coordinate);
  }
  std::filesystem::path directory;
  vdb::ProcessingOptions options;
};

TEST_F(OpenVdbAdvection, ConstantWorldVelocityUsesSparseBackgroundAndProtectsOutput) {
  const auto result = vdb::WriteConstantVelocity(file("wind.vdb"), {1,-2,3}, options);
  EXPECT_EQ(result.gridClass, "vector field");
  EXPECT_EQ(result.activeVoxelCount, 0u);
  const auto field = openvdb::gridPtrCast<openvdb::Vec3DGrid>(read(file("wind.vdb"), "density"));
  EXPECT_EQ(field->background(), openvdb::Vec3d(1,-2,3));
  EXPECT_EQ(field->getVectorType(), openvdb::VEC_CONTRAVARIANT_RELATIVE);
  EXPECT_TRUE(field->isInWorldSpace());
  EXPECT_NEAR(field->voxelSize().x(), 0.25, 1e-12);
  EXPECT_THROW((void)vdb::WriteConstantVelocity(file("wind.vdb"), {0,0,0}, options), std::invalid_argument);
  auto tiny = options;
  tiny.voxelSize = 1e-6;
  EXPECT_THROW((void)vdb::WriteConstantVelocity(file("too-small.vdb"), {1,0,0}, tiny), std::invalid_argument);
  tiny.voxelSize = 1.5e-5;
  EXPECT_NO_THROW((void)vdb::WriteConstantVelocity(file("smallest.vdb"), {1,0,0}, tiny));
}

TEST_F(OpenVdbAdvection, UnicodeVelocityFilenameAndParentPublishExclusivelyWithNativeReadback) {
  const auto parent=directory/std::filesystem::path(u8"移流-é");
  ASSERT_TRUE(std::filesystem::create_directory(parent));
  const auto output=parent/std::filesystem::path(u8"速度.vdb");
  options.gridName="\xe6\xb5\x81\xe3\x82\x8c";
  const auto result=vdb::WriteConstantVelocity(output,{.5,-.25,.125},options);
  EXPECT_EQ(result.gridName,options.gridName);ASSERT_TRUE(std::filesystem::is_regular_file(output));
  const auto bytes=[](const auto& path){std::ifstream input(path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(input),{});};
  const auto original=bytes(output);EXPECT_FALSE(original.empty());
  std::filesystem::copy_file(output,file("unicode-readback.vdb"));
  const auto decoded=openvdb::gridPtrCast<openvdb::Vec3DGrid>(read(file("unicode-readback.vdb"),options.gridName.c_str()));
  ASSERT_TRUE(decoded);EXPECT_EQ(decoded->background(),openvdb::Vec3d(.5,-.25,.125));
  EXPECT_EQ(decoded->getVectorType(),openvdb::VEC_CONTRAVARIANT_RELATIVE);EXPECT_TRUE(decoded->isInWorldSpace());
  EXPECT_EQ(decoded->transform().baseMap()->applyJacobian({1,0,0}),openvdb::Vec3d(.25,0,0));
  EXPECT_THROW((void)vdb::WriteConstantVelocity(output,{1,0,0},options),std::invalid_argument);
  EXPECT_EQ(bytes(output),original);
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(parent),std::filesystem::directory_iterator{}),1);
}

TEST_F(OpenVdbAdvection, SharedExclusiveWriterCleansAsciiScratchAfterEncodedBudgetFailure) {
  const auto scratch=[] {
    std::set<std::filesystem::path> paths;
    for(const auto& base:{std::filesystem::temp_directory_path(),std::filesystem::current_path()})
      for(const auto& entry:std::filesystem::directory_iterator(base))
        if(entry.path().filename().u8string().starts_with(u8".container-vdb-"))paths.insert(entry.path());
    return paths;
  };
  const auto before=scratch();
  const auto parent=directory/std::filesystem::path(u8"中断");ASSERT_TRUE(std::filesystem::create_directory(parent));
  const auto output=parent/std::filesystem::path(u8"失敗.vdb");
  EXPECT_THROW(vdb::detail::WriteFileExclusive(output,{velocity()},1),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(output));EXPECT_TRUE(std::filesystem::is_empty(parent));
  EXPECT_EQ(scratch(),before);
}

#ifdef _WIN32
TEST_F(OpenVdbAdvection, UnsupportedUnicodeTempAndWorkingDirectoriesFailBeforePublication) {
  const auto parent=directory/std::filesystem::path(u8"一時");ASSERT_TRUE(std::filesystem::create_directory(parent));
  struct Restore {
    std::filesystem::path working=std::filesystem::current_path();
    std::wstring tmp=_wgetenv(L"TMP")?_wgetenv(L"TMP"):L"",temp=_wgetenv(L"TEMP")?_wgetenv(L"TEMP"):L"";
    ~Restore(){std::filesystem::current_path(working);_wputenv_s(L"TMP",tmp.c_str());_wputenv_s(L"TEMP",temp.c_str());}
  } restore;
  ASSERT_EQ(_wputenv_s(L"TMP",parent.c_str()),0);ASSERT_EQ(_wputenv_s(L"TEMP",parent.c_str()),0);
  std::filesystem::current_path(parent);
  const auto output=parent/L"out.vdb";
  try{(void)vdb::WriteConstantVelocity(output,{1,0,0},options);FAIL();}
  catch(const std::runtime_error& error){EXPECT_NE(std::string(error.what()).find("ASCII writable temporary directory"),std::string::npos);}
  EXPECT_FALSE(std::filesystem::exists(output));EXPECT_TRUE(std::filesystem::is_empty(parent));
}
#endif

TEST_F(OpenVdbAdvection, PositiveAndNegativeTimePreserveWorldTransformMetadataAndInput) {
  const auto source = fog();
  source->transform().postTranslate(openvdb::Vec3d(2,-1,0.5));
  write(file("source.vdb"), {source});
  write(file("wind.vdb"), {velocity()});
  (void)vdb::WriteAdvectedFog(file("source.vdb"), file("wind.vdb"), file("forward.vdb"),
                             {.timeStep = 0.5}, "smoke", "wind", options);
  (void)vdb::WriteAdvectedFog(file("source.vdb"), file("wind.vdb"), file("backward.vdb"),
                             {.timeStep = -0.5}, "smoke", "wind", options);
  EXPECT_FLOAT_EQ(sample("forward.vdb", {2,0,0}), 1.0f);
  EXPECT_FLOAT_EQ(sample("backward.vdb", {-2,0,0}), 1.0f);
  EXPECT_FLOAT_EQ(sample("forward.vdb", {0,0,0}), 0.0f);
  const auto result = read(file("forward.vdb"), "density");
  EXPECT_EQ(result->transform(), source->transform());
  EXPECT_EQ(result->metaValue<std::string>("artist"), "original fog");
  const auto unchanged = openvdb::gridPtrCast<openvdb::FloatGrid>(read(file("source.vdb"), "smoke"));
  EXPECT_FLOAT_EQ(unchanged->tree().getValue(openvdb::Coord(0)), 1.0f);
  EXPECT_FLOAT_EQ(unchanged->tree().getValue(openvdb::Coord(2,0,0)), 0.0f);
}

TEST_F(OpenVdbAdvection, DiagonalLeadingMaterialIsNotClippedByFaceNeighbourDilation) {
  write(file("source.vdb"), {fog(0.8f)});
  write(file("wind.vdb"), {velocity({0.25,0.25,0.25})});
  (void)vdb::WriteAdvectedFog(file("source.vdb"), file("wind.vdb"), file("diagonal.vdb"),
                             {.timeStep = 1}, {}, {}, options);
  EXPECT_FLOAT_EQ(sample("diagonal.vdb", {1,1,1}), 0.8f);
  EXPECT_FLOAT_EQ(sample("diagonal.vdb", {0,0,0}), 0.0f);
}

TEST_F(OpenVdbAdvection, FloatVelocityUsesItsOwnTranslatedScaledSamplingTransform) {
  auto source = fog();
  source->transform().postTranslate(openvdb::Vec3d(10,0,0));
  auto wind = openvdb::Vec3SGrid::create(openvdb::Vec3f(0));
  wind->setName("wind");
  wind->setIsInWorldSpace(true);
  wind->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
  wind->setTransform(openvdb::math::Transform::createLinearTransform(0.5));
  wind->transform().postTranslate(openvdb::Vec3d(10,0,0));
  wind->tree().fill(openvdb::CoordBBox(openvdb::Coord(-3), openvdb::Coord(3)),
                    openvdb::Vec3f(0.5f,0,0), true);
  write(file("source.vdb"), {source});
  write(file("wind.vdb"), {wind});
  (void)vdb::WriteAdvectedFog(file("source.vdb"), file("wind.vdb"), file("moved.vdb"),
                             {.timeStep = 0.5}, {}, {}, options);
  EXPECT_NEAR(sample("moved.vdb", {1,0,0}), 1.0f, 1e-6f);
}

TEST_F(OpenVdbAdvection, FloatVelocityPromotionPreservesActiveAndInactiveTilesVoxelsAndBackground) {
  auto source = fog();
  // A density gradient makes every velocity category observable, including
  // inactive interior tiles/voxels that a flat density would conceal.
  for (int x = -2; x <= 18; ++x)
    source->tree().fill(openvdb::CoordBBox(openvdb::Coord(x,-1,-1), openvdb::Coord(x,1,1)),
                        static_cast<float>(x + 3) / 24.0f, true);
  source->tree().setValue(openvdb::Coord(0), 1.0f);
  source->transform().postTranslate(openvdb::Vec3d(10,-2,3));
  auto floats = openvdb::Vec3SGrid::create(openvdb::Vec3f(0.125f,0,0));
  auto doubles = openvdb::Vec3DGrid::create(openvdb::Vec3d(0.125,0,0));
  for (const auto& grid : openvdb::GridPtrVec{floats, doubles}) {
    grid->setName("wind");
    grid->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
    grid->setIsInWorldSpace(true);
    grid->setTransform(source->transform().copy());
    grid->insertMeta("wind-author", openvdb::StringMetadata("sparse mixed topology"));
  }
  floats->tree().addTile(1, openvdb::Coord(0), openvdb::Vec3f(0.25f,0,0), true);
  floats->tree().addTile(1, openvdb::Coord(8,0,0), openvdb::Vec3f(0.5f,0,0), false);
  floats->tree().setValue(openvdb::Coord(16,0,0), openvdb::Vec3f(0.75f,0,0));
  floats->tree().setValueOff(openvdb::Coord(17,0,0), openvdb::Vec3f(-0.25f,0,0));
  doubles->tree().addTile(1, openvdb::Coord(0), openvdb::Vec3d(0.25,0,0), true);
  doubles->tree().addTile(1, openvdb::Coord(8,0,0), openvdb::Vec3d(0.5,0,0), false);
  doubles->tree().setValue(openvdb::Coord(16,0,0), openvdb::Vec3d(0.75,0,0));
  doubles->tree().setValueOff(openvdb::Coord(17,0,0), openvdb::Vec3d(-0.25,0,0));
  write(file("source.vdb"), {source});
  write(file("floats.vdb"), {floats});
  write(file("doubles.vdb"), {doubles});
  (void)vdb::WriteAdvectedFog(file("source.vdb"), file("floats.vdb"), file("float-out.vdb"),
                             {.timeStep = 0.25}, {}, {}, options);
  (void)vdb::WriteAdvectedFog(file("source.vdb"), file("doubles.vdb"), file("double-out.vdb"),
                             {.timeStep = 0.25}, {}, {}, options);
  const auto promoted = openvdb::gridPtrCast<openvdb::FloatGrid>(read(file("float-out.vdb"), "density"));
  const auto reference = openvdb::gridPtrCast<openvdb::FloatGrid>(read(file("double-out.vdb"), "density"));
  EXPECT_EQ(promoted->activeVoxelCount(), reference->activeVoxelCount());
  for (int x = -5; x <= 22; ++x)
    for (int y = -4; y <= 4; ++y)
      for (int z = -4; z <= 4; ++z) {
        const openvdb::Coord coordinate(x,y,z);
        EXPECT_EQ(promoted->tree().getValue(coordinate), reference->tree().getValue(coordinate));
      }
  const auto unchanged = openvdb::gridPtrCast<openvdb::Vec3SGrid>(read(file("floats.vdb"), "wind"));
  EXPECT_EQ(unchanged->tree().leafCount(), floats->tree().leafCount());
  EXPECT_EQ(unchanged->activeVoxelCount(), floats->activeVoxelCount());
  EXPECT_EQ(unchanged->metaValue<std::string>("wind-author"), "sparse mixed topology");
}

TEST_F(OpenVdbAdvection, DiluteAndSubnormalFogRetainTheirDensityUnits) {
  write(file("wind.vdb"), {velocity({0.25,0,0})});
  size_t index = 0;
  for (float density : {1e-7f, std::numeric_limits<float>::denorm_min() * 8.0f}) {
    write(file("source.vdb"), {fog(density)});
    const auto output = directory / ("dilute-" + std::to_string(index++) + ".vdb");
    (void)vdb::WriteAdvectedFog(file("source.vdb"), file("wind.vdb"), output,
                               {.timeStep = 1}, {}, {}, options);
    const auto result = openvdb::gridPtrCast<openvdb::FloatGrid>(read(output, "density"));
    EXPECT_EQ(result->tree().getValue(openvdb::Coord(1,0,0)), density);
  }
}

TEST_F(OpenVdbAdvection, ZeroTimeAndZeroSpeedAreExactCopies) {
  const auto source = fog(1e-7f);
  write(file("source.vdb"), {source});
  write(file("wind.vdb"), {velocity()});
  (void)vdb::WriteAdvectedFog(file("source.vdb"), file("wind.vdb"), file("time0.vdb"),
                             {.timeStep = 0}, {}, {}, options);
  write(file("calm.vdb"), {velocity({0,0,0})});
  (void)vdb::WriteAdvectedFog(file("source.vdb"), file("calm.vdb"), file("speed0.vdb"),
                             {.timeStep = 1}, {}, {}, options);
  EXPECT_EQ(sample("time0.vdb", {0,0,0}), 1e-7f);
  EXPECT_EQ(sample("speed0.vdb", {0,0,0}), 1e-7f);
  EXPECT_EQ(read(file("time0.vdb"), "density")->activeVoxelCount(), source->activeVoxelCount());
}

TEST_F(OpenVdbAdvection, RotatedReflectedFogMovesInWorldSpaceAndEmptyFogStaysEmpty) {
  auto source = fog();
  source->transform().postRotate(0.7, openvdb::math::Z_AXIS);
  source->transform().postScale(openvdb::Vec3d(-1,1,1));
  const auto worldVelocity = source->indexToWorld(openvdb::Coord(1,0,0)) -
                             source->indexToWorld(openvdb::Coord(0));
  write(file("source.vdb"), {source});
  write(file("wind.vdb"), {velocity(worldVelocity)});
  (void)vdb::WriteAdvectedFog(file("source.vdb"), file("wind.vdb"), file("rotated.vdb"),
                             {.timeStep = 1}, {}, {}, options);
  EXPECT_NEAR(sample("rotated.vdb", {1,0,0}), 1.0f, 1e-6f);
  source->tree().clear();
  write(file("empty.vdb"), {source});
  const auto result = vdb::WriteAdvectedFog(file("empty.vdb"), file("wind.vdb"), file("empty-out.vdb"),
                                           {.timeStep = 1}, {}, {}, options);
  EXPECT_EQ(result.activeVoxelCount, 0u);
}

TEST_F(OpenVdbAdvection, InactiveVelocitySamplesContributeToTheDisplacementBound) {
  write(file("source.vdb"), {fog()});
  auto wind = velocity({0,0,0});
  wind->tree().setValueOff(openvdb::Coord(0), openvdb::Vec3d(1,0,0));
  write(file("wind.vdb"), {wind});
  try {
    (void)vdb::WriteAdvectedFog(file("source.vdb"), file("wind.vdb"), file("bad.vdb"),
                               {.timeStep = 100, .steps = 64}, {}, {}, options);
    FAIL() << "inactive nonzero velocity still participates in sampling";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("displacement"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbAdvection, FractionalMotionRejectsInsufficientWorldCoordinatePrecision) {
  auto source = fog();
  source->setTransform(openvdb::math::Transform::createLinearTransform(1.0));
  source->transform().postTranslate(openvdb::Vec3d(1e15,0,0));
  // A unit voxel is exactly representable here, but the world's 0.125 ULP
  // would quantize the requested 0.1 displacement and its RK2 midpoint.
  EXPECT_EQ(source->indexToWorld(openvdb::Coord(1,0,0)).x() -
            source->indexToWorld(openvdb::Coord(0)).x(), 1.0);
  write(file("distant.vdb"), {source});
  auto wind = velocity({0.1,0,0});
  wind->transform().postTranslate(openvdb::Vec3d(1e15,0,0));
  write(file("wind.vdb"), {wind});
  try {
    (void)vdb::WriteAdvectedFog(file("distant.vdb"), file("wind.vdb"), file("bad.vdb"),
                               {.timeStep = 1}, {}, {}, options);
    FAIL() << "subvoxel motion must be representable before integration";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("precision"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbAdvection, InvalidTimeVectorSemanticsAndSourceFieldsFailWithoutOutput) {
  write(file("source.vdb"), {fog()});
  write(file("wind.vdb"), {velocity()});
  for (const vdb::AdvectionOptions invalid : {vdb::AdvectionOptions{NAN,1}, {3601,1}, {1,0}, {1,65}, {100,64}, {5,1}})
    EXPECT_THROW((void)vdb::WriteAdvectedFog(file("source.vdb"), file("wind.vdb"), file("bad.vdb"),
                                            invalid, {}, {}, options), std::invalid_argument);
  auto invalidWind = velocity();
  invalidWind->setIsInWorldSpace(false);
  write(file("invalid-wind.vdb"), {invalidWind});
  EXPECT_THROW((void)vdb::WriteAdvectedFog(file("source.vdb"), file("invalid-wind.vdb"), file("bad.vdb"),
                                          {}, {}, {}, options), std::invalid_argument);
  invalidWind->setIsInWorldSpace(true);
  for (const auto type : {openvdb::VEC_COVARIANT, openvdb::VEC_COVARIANT_NORMALIZE, openvdb::VEC_CONTRAVARIANT_ABSOLUTE}) {
    invalidWind->setVectorType(type);
    write(file("invalid-wind.vdb"), {invalidWind});
    EXPECT_THROW((void)vdb::WriteAdvectedFog(file("source.vdb"), file("invalid-wind.vdb"), file("bad.vdb"),
                                            {}, {}, {}, options), std::invalid_argument);
  }
  invalidWind->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
  invalidWind->setGridClass(openvdb::GRID_STAGGERED);
  write(file("staggered.vdb"), {invalidWind});
  EXPECT_THROW((void)vdb::WriteAdvectedFog(file("source.vdb"), file("staggered.vdb"), file("bad.vdb"),
                                          {}, {}, {}, options), std::invalid_argument);
  auto invalidFog = fog();
  invalidFog->tree().setValueOff(openvdb::Coord(1,0,0), 0.5f);
  write(file("invalid-fog.vdb"), {invalidFog});
  EXPECT_THROW((void)vdb::WriteAdvectedFog(file("invalid-fog.vdb"), file("wind.vdb"), file("bad.vdb"),
                                          {}, {}, {}, options), std::invalid_argument);
  invalidFog = fog();
  invalidFog->transform().postScale(openvdb::Vec3d(1,2,1));
  write(file("anisotropic.vdb"), {invalidFog});
  EXPECT_THROW((void)vdb::WriteAdvectedFog(file("anisotropic.vdb"), file("wind.vdb"), file("bad.vdb"),
                                          {}, {}, {}, options), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbAdvection, ExpansionWorkspaceAndSamplingCoordinatesArePreflighted) {
  const auto source = fog();
  const auto wind = velocity({0.25,0,0});
  write(file("source.vdb"), {source});
  write(file("wind.vdb"), {wind});
  auto bounded = options;
  bounded.maxVoxelCount = 512;
  EXPECT_THROW((void)vdb::WriteAdvectedFog(file("source.vdb"), file("wind.vdb"), file("bad.vdb"),
                                          {.timeStep = 1}, {}, {}, bounded), std::invalid_argument);
  bounded = options;
  bounded.maxInputBytes = source->memUsage() + wind->memUsage() + 4096;
  EXPECT_THROW((void)vdb::WriteAdvectedFog(file("source.vdb"), file("wind.vdb"), file("bad.vdb"),
                                          {.timeStep = 1}, {}, {}, bounded), std::invalid_argument);
  source->tree().clear();
  source->tree().setValue(openvdb::Coord(std::numeric_limits<int32_t>::max() - 3,0,0), 1.0f);
  write(file("edge.vdb"), {source});
  EXPECT_THROW((void)vdb::WriteAdvectedFog(file("edge.vdb"), file("wind.vdb"), file("bad.vdb"),
                                          {.timeStep = 1}, {}, {}, options), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}
TEST_F(OpenVdbAdvection, UnstableUpstreamTransformCopiesRejectInsteadOfShiftingSampling) {
  auto source=fog(); source->setTransform(openvdb::math::Transform::createLinearTransform(.1));
  source->transform().postTranslate({1e8,0,0}); write(file("source.vdb"),{source});
  write(file("velocity.vdb"),{velocity()});
  try {
    (void)vdb::WriteAdvectedFog(file("source.vdb"),file("velocity.vdb"),file("bad.vdb"),{}, {}, {},options); FAIL();
  } catch(const std::invalid_argument &e) {
    EXPECT_NE(std::string(e.what()).find("recenter"),std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}
#endif
} // namespace
