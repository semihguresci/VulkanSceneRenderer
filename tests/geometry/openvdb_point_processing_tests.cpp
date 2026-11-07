#include "Container/geometry/OpenVdbPointProcessing.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/points/PointDataGrid.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;
TEST(OpenVdbPointProcessingAvailability, DisabledApisFailBeforePaths) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP() << "OpenVDB is enabled";
#else
  try { (void)vdb::ScatterPoints("missing.vdb", "missing-dir/output.vdb"); FAIL(); }
  catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"), std::string::npos);
  }
  EXPECT_THROW((void)vdb::WriteAdvectedPoints("missing.vdb", "velocity.vdb", "missing-dir/output.vdb"), std::runtime_error);
  EXPECT_THROW((void)vdb::WriteSampledPoints("missing.vdb", "field.vdb", "missing-dir/output.vdb"), std::runtime_error);
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbPointProcessing : public ::testing::Test {
 protected:
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{0};
    directory = std::filesystem::temp_directory_path() / ("container-point-processing-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
    options.gridName = "points"; options.voxelSize = .1;
  }
  void TearDown() override {
    std::error_code ignored;
    for (const auto& item : std::filesystem::directory_iterator(directory)) std::filesystem::remove(item.path(), ignored);
    std::filesystem::remove(directory, ignored);
  }
  auto file(const char* name) const { return directory / name; }
  void direct(const char* name, openvdb::GridPtrVec grids) {
    openvdb::io::File output(file(name).string()); output.write(grids); output.close();
  }
  openvdb::GridBase::Ptr read(const char* name, const char* grid = "points") {
    openvdb::io::File input(file(name).string()); input.open(false); auto result = input.readGrid(grid); input.close(); return result;
  }
  auto records(const char* name) { return vdb::ReadPoints(file(name), {}, options); }
  static std::string bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary); return {std::istreambuf_iterator<char>(input), {}};
  }
  template<class Grid = openvdb::FloatGrid> auto fog() {
    auto grid = Grid::create(0); grid->setName("fog"); grid->setGridClass(openvdb::GRID_FOG_VOLUME);
    grid->tree().setValue(openvdb::Coord(0,0,0), 1); grid->tree().setValue(openvdb::Coord(4,0,0), 3);
    grid->insertMeta("artist", openvdb::StringMetadata("Ada")); return grid;
  }
  void pointSource(const std::vector<vdb::PointRecord>& values = {{{.2,.3,.4}, {7,8,9}, .25}}) {
    (void)vdb::WritePoints(values, file("points.vdb"), options);
    auto grid = read("points.vdb"); grid->insertMeta("artist", openvdb::StringMetadata("Grace"));
    direct("input.vdb", {grid});
  }
  template<class Grid = openvdb::Vec3DGrid> void constantVelocity(double x = 1) {
    auto grid = Grid::create(typename Grid::ValueType(x, 0, 0)); grid->setName("velocity");
    grid->setTransform(openvdb::math::Transform::createLinearTransform(.2));
    direct("velocity.vdb", {grid});
  }
  void unchanged(const std::string& source, const char* path) { EXPECT_EQ(source, bytes(file(path))); }
  static void same(const auto& a, const auto& b) {
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
      EXPECT_EQ(a[i].position, b[i].position); EXPECT_EQ(a[i].velocity, b[i].velocity); EXPECT_EQ(a[i].radius, b[i].radius);
    }
  }
  std::filesystem::path directory;
  vdb::ProcessingOptions options;
};

TEST_F(OpenVdbPointProcessing, ScatterIsSeededDensityWeightedAndPreservesMetadata) {
  direct("fog.vdb", {fog()}); const auto original = bytes(file("fog.vdb"));
  vdb::ScatterOptions scatter{4096, 934, .17};
  const auto result = vdb::ScatterPoints(file("fog.vdb"), file("a.vdb"), {}, scatter, options);
  EXPECT_EQ(result.pointCount, 4096u);
  (void)vdb::ScatterPoints(file("fog.vdb"), file("b.vdb"), {}, scatter, options);
  const auto a = records("a.vdb"), b = records("b.vdb"); same(a,b);
  size_t left = 0;
  for (const auto& point : a) {
    if (point.position[0] < 1) { ++left; EXPECT_GE(point.position[0], -.500001); EXPECT_LE(point.position[0], .500001); }
    else { EXPECT_GE(point.position[0], 3.499999); EXPECT_LE(point.position[0], 4.500001); }
    EXPECT_LE(std::abs(point.position[1]), .500001); EXPECT_LE(std::abs(point.position[2]), .500001);
    EXPECT_EQ(point.velocity, (std::array<double,3>{})); EXPECT_DOUBLE_EQ(point.radius, .17);
  }
  EXPECT_GT(left, 900u); EXPECT_LT(left, 1150u);
  scatter.seed++;
  (void)vdb::ScatterPoints(file("fog.vdb"), file("c.vdb"), {}, scatter, options);
  EXPECT_NE(a.front().position, records("c.vdb").front().position);
  const auto grid = read("a.vdb"); EXPECT_EQ(grid->metaValue<std::string>("artist"), "Ada");
  EXPECT_EQ(grid->getGridClass(), openvdb::GRID_UNKNOWN); unchanged(original, "fog.vdb");
}

TEST_F(OpenVdbPointProcessing, DoubleFogTilesAndAffineTransformRemainSparse) {
  auto grid = openvdb::DoubleGrid::create(0); grid->setName("fog"); grid->setGridClass(openvdb::GRID_FOG_VOLUME);
  grid->tree().addTile(1, openvdb::Coord(0), 2., true);
  auto transform = openvdb::math::Transform::createLinearTransform(1.);
  transform->postScale(openvdb::Vec3d(2,3,4)); transform->postTranslate(openvdb::Vec3d(10,-20,30));
  grid->setTransform(transform); direct("fog.vdb", {grid});
  const auto before = bytes(file("fog.vdb"));
  (void)vdb::ScatterPoints(file("fog.vdb"), file("tile.vdb"), {}, {200, 42, 1}, options);
  double minimum = 100, maximum = -100;
  for (const auto& point : records("tile.vdb")) {
    const auto index = transform->worldToIndex(openvdb::Vec3d(point.position[0], point.position[1], point.position[2]));
    for (size_t axis = 0; axis < 3; ++axis) { EXPECT_GE(index[axis], -.500001); EXPECT_LE(index[axis], 7.500001); }
    minimum = std::min(minimum, index.x()); maximum = std::max(maximum, index.x());
  }
  EXPECT_LT(minimum, 0.); EXPECT_GT(maximum, 7.); unchanged(before, "fog.vdb");
}

TEST_F(OpenVdbPointProcessing, EmptyScatterAndFogValidationAreExplicit) {
  auto grid = openvdb::FloatGrid::create(0); grid->setName("fog"); grid->setGridClass(openvdb::GRID_FOG_VOLUME);
  direct("empty.vdb", {grid});
  EXPECT_EQ(vdb::ScatterPoints(file("empty.vdb"), file("zero.vdb"), {}, {0}, options).pointCount, 0u);
  EXPECT_TRUE(records("zero.vdb").empty());
  EXPECT_THROW((void)vdb::ScatterPoints(file("empty.vdb"), file("fail.vdb"), {}, {}, options), std::invalid_argument);
  grid->tree().setValue(openvdb::Coord(0), -1); direct("negative.vdb", {grid});
  EXPECT_THROW((void)vdb::ScatterPoints(file("negative.vdb"), file("negative-out.vdb"), {}, {}, options), std::invalid_argument);
  grid->tree().setValue(openvdb::Coord(0), std::numeric_limits<float>::infinity()); direct("inf.vdb", {grid});
  EXPECT_THROW((void)vdb::ScatterPoints(file("inf.vdb"), file("inf-out.vdb"), {}, {}, options), std::invalid_argument);
  grid->tree().setValueOff(openvdb::Coord(0), 1); direct("inactive.vdb", {grid});
  EXPECT_THROW((void)vdb::ScatterPoints(file("inactive.vdb"), file("inactive-out.vdb"), {}, {}, options), std::invalid_argument);
}

TEST_F(OpenVdbPointProcessing, DensityDynamicRangeAndScatterLimitsRejectBeforePublication) {
  auto grid = fog<openvdb::DoubleGrid>(); grid->tree().setValue(openvdb::Coord(0), 1e300);
  grid->tree().setValue(openvdb::Coord(4,0,0), 1e-300); direct("fog.vdb", {grid});
  EXPECT_THROW((void)vdb::ScatterPoints(file("fog.vdb"), file("range.vdb"), {}, {}, options), std::invalid_argument);
  EXPECT_THROW((void)vdb::ScatterPoints(file("fog.vdb"), file("count.vdb"), {}, {11}, options, {10}), std::invalid_argument);
  EXPECT_THROW((void)vdb::ScatterPoints(file("fog.vdb"), file("radius.vdb"), {}, {1,0,-1}, options), std::invalid_argument);
  auto small = options; small.maxInputBytes = 1024;
  EXPECT_THROW((void)vdb::ScatterPoints(file("fog.vdb"), file("memory.vdb"), {}, {10}, small), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("range.vdb"))); EXPECT_FALSE(std::filesystem::exists(file("memory.vdb")));
}

TEST_F(OpenVdbPointProcessing, ConstantVelocitySignedTimeAndAttributesArePreserved) {
  pointSource(); constantVelocity<openvdb::Vec3SGrid>();
  const auto original = records("input.vdb"); const auto source = bytes(file("input.vdb"));
  const auto field = bytes(file("velocity.vdb"));
  for (const auto& [dt,name] : std::array<std::pair<double,const char*>,2>{{{.5,"forward.vdb"},{-.5,"backward.vdb"}}}) {
    vdb::PointAdvectionOptions a; a.timeStep = dt; a.steps = 2; a.updateVelocity = false;
    (void)vdb::WriteAdvectedPoints(file("input.vdb"), file("velocity.vdb"), file(name), {}, {}, a, options);
    const auto result = records(name); ASSERT_EQ(result.size(),1u);
    EXPECT_NEAR(result[0].position[0], original[0].position[0] + dt, 1e-7);
    EXPECT_NEAR(result[0].position[1], original[0].position[1], 1e-7);
    EXPECT_EQ(result[0].velocity, original[0].velocity); EXPECT_EQ(result[0].radius, original[0].radius);
    EXPECT_EQ(read(name)->metaValue<std::string>("artist"), "Grace");
  }
  vdb::PointAdvectionOptions a; a.timeStep = .5;
  (void)vdb::WriteAdvectedPoints(file("input.vdb"), file("velocity.vdb"), file("updated.vdb"), {}, {}, a, options);
  EXPECT_EQ(records("updated.vdb")[0].velocity, (std::array<double,3>{1,0,0}));
  unchanged(source,"input.vdb"); unchanged(field,"velocity.vdb");
}

TEST_F(OpenVdbPointProcessing, Rk4IsMoreAccurateForLinearFieldAndIndependentTransform) {
  pointSource({{{.2,.1,.1}, {}, .3}});
  auto field = openvdb::Vec3DGrid::create(openvdb::Vec3d(0)); field->setName("velocity");
  auto transform = openvdb::math::Transform::createLinearTransform(.2);
  transform->postTranslate(openvdb::Vec3d(.03,-.07,.11)); field->setTransform(transform);
  for (int x = -2; x <= 8; ++x) for (int y = -2; y <= 3; ++y) for (int z = -2; z <= 3; ++z) {
    const openvdb::Coord coordinate(x,y,z); field->tree().setValue(coordinate, openvdb::Vec3d(field->indexToWorld(coordinate.asVec3d()).x(),0,0));
  }
  direct("velocity.vdb", {field});
  vdb::PointAdvectionOptions a; a.timeStep = .5; a.steps = 1;
  (void)vdb::WriteAdvectedPoints(file("input.vdb"), file("velocity.vdb"), file("rk2.vdb"), {}, {}, a, options);
  a.integration = vdb::PointIntegration::RK4;
  (void)vdb::WriteAdvectedPoints(file("input.vdb"), file("velocity.vdb"), file("rk4.vdb"), {}, {}, a, options);
  const double expected = records("input.vdb")[0].position[0] * std::exp(.5);
  const double error2 = std::abs(records("rk2.vdb")[0].position[0]-expected);
  const double error4 = std::abs(records("rk4.vdb")[0].position[0]-expected);
  EXPECT_GT(error2, .004); EXPECT_LT(error4, .0001); EXPECT_LT(error4, error2 / 30);
  EXPECT_NEAR(records("rk4.vdb")[0].velocity[0], records("rk4.vdb")[0].position[0], 1e-7);
}

TEST_F(OpenVdbPointProcessing, ZeroTimeEmptyPointsAndOutputRepartitionAreExplicit) {
  pointSource(); constantVelocity(); auto different = options; different.voxelSize = .2;
  vdb::PointAdvectionOptions a; a.timeStep = 0; a.updateVelocity = false;
  (void)vdb::WriteAdvectedPoints(file("input.vdb"), file("velocity.vdb"), file("zero.vdb"), {}, {}, a, different);
  const auto original = records("input.vdb"), zero = records("zero.vdb");
  EXPECT_NEAR(original[0].position[0], zero[0].position[0], 1e-8); EXPECT_EQ(original[0].velocity, zero[0].velocity);
  EXPECT_DOUBLE_EQ(read("zero.vdb")->voxelSize().x(), .2);
  (void)vdb::WritePoints({}, file("empty.vdb"), options);
  EXPECT_EQ(vdb::WriteAdvectedPoints(file("empty.vdb"), file("velocity.vdb"), file("empty-out.vdb"), {}, {}, {}, options).pointCount,0u);
}

TEST_F(OpenVdbPointProcessing, AdvectionValidatesOptionsDisplacementAndFieldSemantics) {
  pointSource(); constantVelocity();
  vdb::PointAdvectionOptions a; a.steps = 0;
  EXPECT_THROW((void)vdb::WriteAdvectedPoints(file("input.vdb"),file("velocity.vdb"),file("steps.vdb"),{},{},a,options),std::invalid_argument);
  a.steps = 65;
  EXPECT_THROW((void)vdb::WriteAdvectedPoints(file("input.vdb"),file("velocity.vdb"),file("steps65.vdb"),{},{},a,options),std::invalid_argument);
  a.steps = 1; a.timeStep = std::numeric_limits<double>::quiet_NaN();
  EXPECT_THROW((void)vdb::WriteAdvectedPoints(file("input.vdb"),file("velocity.vdb"),file("nan.vdb"),{},{},a,options),std::invalid_argument);
  a.timeStep = 2;
  EXPECT_THROW((void)vdb::WriteAdvectedPoints(file("input.vdb"),file("velocity.vdb"),file("step-travel.vdb"),{},{},a,options),std::invalid_argument);
  a.timeStep = 30; a.steps = 64;
  EXPECT_THROW((void)vdb::WriteAdvectedPoints(file("input.vdb"),file("velocity.vdb"),file("total-travel.vdb"),{},{},a,options),std::invalid_argument);
  auto field = openvdb::Vec3DGrid::create(openvdb::Vec3d(0)); field->setName("velocity"); field->setIsInWorldSpace(false);
  direct("local.vdb", {field});
  EXPECT_THROW((void)vdb::WriteAdvectedPoints(file("input.vdb"),file("local.vdb"),file("local-out.vdb"),{},{},{},options),std::invalid_argument);
  field->setIsInWorldSpace(true); field->setGridClass(openvdb::GRID_STAGGERED); direct("staggered.vdb", {field});
  EXPECT_THROW((void)vdb::WriteAdvectedPoints(file("input.vdb"),file("staggered.vdb"),file("staggered-out.vdb"),{},{},{},options),std::invalid_argument);
}

TEST_F(OpenVdbPointProcessing, DescriptorMetadataAndHiddenFlagsRejectWithoutLoss) {
  pointSource(); constantVelocity();
  auto grid = openvdb::gridPtrCast<openvdb::points::PointDataGrid>(read("input.vdb"));
  for (auto leaf = grid->tree().beginLeaf(); leaf; ++leaf)
    leaf->attributeSet().descriptorPtr()->getMetadata().insertMeta("custom", openvdb::StringMetadata("keep"));
  direct("custom.vdb", {grid});
  EXPECT_THROW((void)vdb::WriteAdvectedPoints(file("custom.vdb"),file("velocity.vdb"),file("custom-out.vdb"),{},{},{},options),std::invalid_argument);
  grid = openvdb::gridPtrCast<openvdb::points::PointDataGrid>(read("input.vdb"));
  for (auto leaf = grid->tree().beginLeaf(); leaf; ++leaf) leaf->attributeArray("v").setHidden(true);
  direct("hidden.vdb", {grid});
  EXPECT_THROW((void)vdb::WriteAdvectedPoints(file("hidden.vdb"),file("velocity.vdb"),file("hidden-out.vdb"),{},{},{},options),std::invalid_argument);
  grid = openvdb::gridPtrCast<openvdb::points::PointDataGrid>(read("input.vdb"));
  for (auto leaf = grid->tree().beginLeaf(); leaf; ++leaf) {
    const auto point = leaf->beginIndexOn(); if (point) leaf->setValueOff(point.getCoord());
  }
  direct("inactive-points.vdb", {grid});
  EXPECT_THROW((void)vdb::WriteAdvectedPoints(file("inactive-points.vdb"),file("velocity.vdb"),file("inactive-points-out.vdb"),{},{},{},options),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("custom-out.vdb"))); EXPECT_FALSE(std::filesystem::exists(file("hidden-out.vdb")));
}

TEST_F(OpenVdbPointProcessing, MemoryBudgetsCollidingDescriptorsAndExistingOutputsAreProtected) {
  auto grid = fog(); direct("fog.vdb", {grid});
  auto a = fog(), b = fog(), c = fog(); a->setName("field"); b->setName("field"); c->setName("field[0]");
  direct("collision.vdb", {a,b,c});
  EXPECT_THROW((void)vdb::ScatterPoints(file("collision.vdb"),file("collision-out.vdb"),{}, {1},options),std::invalid_argument);
  EXPECT_THROW((void)vdb::ScatterPoints(file("collision.vdb"),file("selected-out.vdb"),"descriptor:field[0]",{1},options),std::invalid_argument);
  std::ofstream(file("protected.vdb")) << "keep";
  EXPECT_THROW((void)vdb::ScatterPoints(file("fog.vdb"),file("protected.vdb"),{},{1},options),std::invalid_argument);
  EXPECT_EQ(bytes(file("protected.vdb")),"keep");
  pointSource(); constantVelocity(); auto low = options; low.maxInputBytes = 1024;
  EXPECT_THROW((void)vdb::WriteAdvectedPoints(file("input.vdb"),file("velocity.vdb"),file("memory.vdb"),{},{},{},low),std::invalid_argument);
  for (const auto& item : std::filesystem::directory_iterator(directory)) EXPECT_FALSE(item.is_directory());
}

TEST_F(OpenVdbPointProcessing, MetadataCopiesAndTinyWorldDisplacementsAreBudgeted) {
  auto grid = fog(); direct("base.vdb", {grid});
  auto bounded = options; bounded.maxInputBytes = 20ull * 1024 * 1024;
  EXPECT_EQ(vdb::ScatterPoints(file("base.vdb"),file("base-out.vdb"),{},{1},bounded).pointCount,1u);
  grid->insertMeta("large-custom",openvdb::StringMetadata(std::string(2*1024*1024,'x')));
  direct("metadata.vdb", {grid});
  EXPECT_THROW((void)vdb::ScatterPoints(file("metadata.vdb"),file("metadata-out.vdb"),{},{1},bounded),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("metadata-out.vdb")));
  pointSource({{{1e7,.1,.1},{},.2}}); constantVelocity();
  vdb::PointAdvectionOptions a; a.timeStep = 1e-12;
  try {
    (void)vdb::WriteAdvectedPoints(file("input.vdb"),file("velocity.vdb"),file("tiny.vdb"),{},{},a,options); FAIL();
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("precision"),std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(file("tiny.vdb")));
}

TEST_F(OpenVdbPointProcessing, SamplesScalarAndVectorFieldsInIndependentWorldTransform) {
  pointSource({{{.2,.3,.4},{7,8,9},.25},{{.5,.1,.2},{4,5,6},.75}});
  auto scalar = openvdb::DoubleGrid::create(1); scalar->setName("radius");
  auto vector = openvdb::Vec3SGrid::create(openvdb::Vec3f(0)); vector->setName("velocity");
  auto transform = openvdb::math::Transform::createLinearTransform(.2);
  transform->postTranslate(openvdb::Vec3d(.03,-.07,.11));
  scalar->setTransform(transform); vector->setTransform(transform->copy());
  for (int x = -1; x <= 4; ++x) for (int y = -1; y <= 3; ++y) for (int z = -1; z <= 3; ++z) {
    const openvdb::Coord cell(x,y,z); const auto world = transform->indexToWorld(cell.asVec3d());
    scalar->tree().setValue(cell, 2+world.x()+2*world.y()+3*world.z());
    vector->tree().setValue(cell, openvdb::Vec3f(world.x(),2*world.y(),3*world.z()));
  }
  direct("fields.vdb",{scalar,vector}); const auto fieldBytes = bytes(file("fields.vdb")), inputBytes = bytes(file("input.vdb"));
  (void)vdb::WriteSampledPoints(file("input.vdb"),file("fields.vdb"),file("radius.vdb"),vdb::PointAttribute::Radius,{},"radius",options);
  (void)vdb::WriteSampledPoints(file("input.vdb"),file("fields.vdb"),file("vector.vdb"),vdb::PointAttribute::Velocity,{},"velocity",options);
  const auto original = records("input.vdb"), radii = records("radius.vdb"), velocities = records("vector.vdb");
  ASSERT_EQ(original.size(),2u);
  for (size_t i = 0; i < original.size(); ++i) {
    const auto& p = original[i].position;
    EXPECT_NEAR(radii[i].radius,2+p[0]+2*p[1]+3*p[2],1e-10); EXPECT_EQ(radii[i].velocity,original[i].velocity);
    EXPECT_NEAR(velocities[i].velocity[0],p[0],1e-7); EXPECT_NEAR(velocities[i].velocity[1],2*p[1],1e-7);
    EXPECT_NEAR(velocities[i].velocity[2],3*p[2],1e-7); EXPECT_EQ(velocities[i].radius,original[i].radius);
    for (size_t axis = 0; axis < 3; ++axis) {
      EXPECT_NEAR(radii[i].position[axis],p[axis],1e-8); EXPECT_NEAR(velocities[i].position[axis],p[axis],1e-8);
    }
  }
  EXPECT_EQ(read("radius.vdb")->metaValue<std::string>("artist"),"Grace");
  unchanged(fieldBytes,"fields.vdb"); unchanged(inputBytes,"input.vdb");
}

TEST_F(OpenVdbPointProcessing, RadiusInterpolationPromotesFloatArithmeticAndRejectsInvalidSamples) {
  pointSource({{{.25,.25,.25},{1,2,3},.5}});
  auto grid = openvdb::FloatGrid::create(std::numeric_limits<float>::max()); grid->setName("radius");
  direct("large.vdb",{grid});
  (void)vdb::WriteSampledPoints(file("input.vdb"),file("large.vdb"),file("large-out.vdb"),vdb::PointAttribute::Radius,{},{},options);
  EXPECT_DOUBLE_EQ(records("large-out.vdb")[0].radius,double(std::numeric_limits<float>::max()));
  auto negative = openvdb::DoubleGrid::create(-1); negative->setName("radius"); direct("negative.vdb",{negative});
  EXPECT_THROW((void)vdb::WriteSampledPoints(file("input.vdb"),file("negative.vdb"),file("negative-out.vdb"),vdb::PointAttribute::Radius,{},{},options),std::invalid_argument);
  negative->tree().setValue(openvdb::Coord(10),std::numeric_limits<double>::infinity()); direct("nonfinite.vdb",{negative});
  EXPECT_THROW((void)vdb::WriteSampledPoints(file("input.vdb"),file("nonfinite.vdb"),file("nonfinite-out.vdb"),vdb::PointAttribute::Radius,{},{},options),std::invalid_argument);
  EXPECT_THROW((void)vdb::WriteSampledPoints(file("input.vdb"),file("large.vdb"),file("wrong-type.vdb"),vdb::PointAttribute::Velocity,{},{},options),std::invalid_argument);
  EXPECT_THROW((void)vdb::WriteSampledPoints(file("input.vdb"),file("large.vdb"),file("bad-target.vdb"),static_cast<vdb::PointAttribute>(9),{},{},options),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("negative-out.vdb"))); EXPECT_FALSE(std::filesystem::exists(file("nonfinite-out.vdb")));
}

TEST_F(OpenVdbPointProcessing, RadiusInterpolationAvoidsOppositeDoubleEndpointOverflow) {
  pointSource({{{.75,.25,.25},{1,2,3},.5}});
  const double maximum = std::numeric_limits<double>::max();
  auto field = openvdb::DoubleGrid::create(-maximum); field->setName("radius");
  for (int y = 0; y < 2; ++y) for (int z = 0; z < 2; ++z)
    field->tree().setValue(openvdb::Coord(1,y,z),maximum);
  direct("extreme.vdb",{field});
  (void)vdb::WriteSampledPoints(file("input.vdb"),file("extreme.vdb"),file("finite.vdb"),vdb::PointAttribute::Radius,{},{},options);
  const double sampled = records("finite.vdb")[0].radius;
  EXPECT_TRUE(std::isfinite(sampled)); EXPECT_DOUBLE_EQ(sampled,maximum * .5);
}

TEST_F(OpenVdbPointProcessing, AlreadyWorldCovariantFieldsSupportSamplingAndAdvection) {
  pointSource(); const auto source = records("input.vdb");
  auto field = openvdb::Vec3DGrid::create(openvdb::Vec3d(2,3,4)); field->setName("gradient");
  field->setIsInWorldSpace(true); field->setVectorType(openvdb::VEC_COVARIANT);
  direct("gradient.vdb",{field});
  (void)vdb::WriteSampledPoints(file("input.vdb"),file("gradient.vdb"),file("sampled.vdb"),vdb::PointAttribute::Velocity,{},{},options);
  EXPECT_EQ(records("sampled.vdb")[0].velocity,(std::array<double,3>{2,3,4}));
  vdb::PointAdvectionOptions a; a.timeStep = .01;
  (void)vdb::WriteAdvectedPoints(file("input.vdb"),file("gradient.vdb"),file("moved.vdb"),{},{},a,options);
  const auto moved = records("moved.vdb");
  for (size_t axis = 0; axis < 3; ++axis)
    EXPECT_NEAR(moved[0].position[axis],source[0].position[axis]+.01*(axis+2),1e-7);
}

TEST_F(OpenVdbPointProcessing, RemoteFieldIndexOriginsRejectLossyInterpolation) {
  pointSource({{{.5,.25,.25},{},.2}});
  auto vector = openvdb::Vec3DGrid::create(openvdb::Vec3d(0)); vector->setName("velocity");
  auto scalar = openvdb::DoubleGrid::create(0); scalar->setName("radius");
  auto transform = openvdb::math::Transform::createLinearTransform(1e6);
  transform->postTranslate(openvdb::Vec3d(1e15,0,0));
  vector->setTransform(transform); scalar->setTransform(transform->copy());
  for (int y = 0; y < 2; ++y) for (int z = 0; z < 2; ++z) {
    vector->tree().setValue(openvdb::Coord(-999999999,y,z),openvdb::Vec3d(1e12,0,0));
    scalar->tree().setValue(openvdb::Coord(-999999999,y,z),1e12);
  }
  direct("remote.vdb",{vector,scalar});
  const auto rejectsPrecision = [&](auto action) {
    try { action(); FAIL(); }
    catch (const std::invalid_argument& error) { EXPECT_NE(std::string(error.what()).find("precision"),std::string::npos); }
  };
  rejectsPrecision([&] { (void)vdb::WriteSampledPoints(file("input.vdb"),file("remote.vdb"),file("vector-out.vdb"),vdb::PointAttribute::Velocity,{},"velocity",options); });
  rejectsPrecision([&] { (void)vdb::WriteSampledPoints(file("input.vdb"),file("remote.vdb"),file("scalar-out.vdb"),vdb::PointAttribute::Radius,{},"radius",options); });
  vdb::PointAdvectionOptions a; a.timeStep = 1e-14;
  rejectsPrecision([&] { (void)vdb::WriteAdvectedPoints(file("input.vdb"),file("remote.vdb"),file("advected.vdb"),{},"velocity",a,options); });
  EXPECT_FALSE(std::filesystem::exists(file("vector-out.vdb"))); EXPECT_FALSE(std::filesystem::exists(file("scalar-out.vdb")));
  EXPECT_FALSE(std::filesystem::exists(file("advected.vdb")));
}
#endif
} // namespace
