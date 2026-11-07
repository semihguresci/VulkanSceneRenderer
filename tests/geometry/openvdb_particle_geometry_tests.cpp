#include "Container/geometry/OpenVdbParticleGeometry.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <sstream>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/File.h>
#include <openvdb/openvdb.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;
TEST(OpenVdbParticleGeometryAvailability,
     DisabledBeforeOptionsSpansPathsAndOutput) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::ParticleGeometryOptions o;
  o.maxParticles = 0;
  vdb::ProcessingOptions p;
  p.maxInputBytes = 0;
  const auto check = [](auto call) {
    try {
      call();
      FAIL();
    } catch (const std::runtime_error &e) {
      EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),
                std::string::npos);
    }
  };
  check([&] { (void)vdb::WriteParticleGeometry({}, "missing", o, p); });
  check([&] {
    (void)vdb::WriteParticleGeometryFile("missing", "missing", o, p);
  });
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbParticleGeometry : public ::testing::Test {
protected:
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t sequence{0};
    directory =
        std::filesystem::current_path() /
        (".particle-geometry-test-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++sequence));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    p.voxelSize = 1;
    p.gridName = "sdf";
    o.originWorld = std::array<double, 3>{0, 0, 0};
  }
  void TearDown() override {
    // Remove only this fixture's checked, independently created directory.
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
  std::filesystem::path path(const std::string &name) const {
    return directory / name;
  }
  static std::string contents(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
  }
  static void text(const std::filesystem::path &path,
                   const std::string &value) {
    std::ofstream stream(path, std::ios::binary);
    stream << value;
    ASSERT_TRUE(bool(stream));
  }
  openvdb::GridBase::Ptr read(const std::filesystem::path &input,
                              const std::string &name) {
    // SDK filenames are narrow: read an exact native-path copy in the fixture's
    // ASCII directory. The production writer itself receives the Unicode path.
    const auto copy = path("readback.vdb");
    std::filesystem::copy_file(
        input, copy, std::filesystem::copy_options::overwrite_existing);
    openvdb::io::File file(copy.string());
    file.open(false);
    auto grid = file.readGrid(name);
    file.close();
    return grid;
  }
  vdb::ParticleGeometryResult
  write(const std::vector<vdb::GeometryParticle> &records,
        const std::string &name = "out.vdb") {
    return vdb::WriteParticleGeometry(records, path(name), o, p);
  }
  template <class Callback>
  void rejects(Callback call, const std::string &name = "out.vdb") {
    EXPECT_ANY_THROW(call());
    EXPECT_FALSE(std::filesystem::exists(path(name)));
  }
  static double sphere(const openvdb::Coord &q,
                       const std::array<double, 3> &center, double radius) {
    return std::hypot(double(q.x()) - center[0], double(q.y()) - center[1],
                      double(q.z()) - center[2]) -
           radius;
  }
  std::filesystem::path directory;
  vdb::ProcessingOptions p;
  vdb::ParticleGeometryOptions o;
};

TEST_F(OpenVdbParticleGeometry,
       SphereUnionSignedDistanceAndStrictCenterMaskOracle) {
  o.output = vdb::ParticleGeometryOutput::Both;
  const auto result = write({{{0, 0, 0}, {}, 2}, {{2, 0, 0}, {}, 2}});
  ASSERT_TRUE(result.levelSet);
  ASSERT_TRUE(result.mask);
  EXPECT_EQ(result.mask->gridClass, "unknown");
  EXPECT_EQ(result.particleCount, 2);
  EXPECT_EQ(result.sphereParticleCount, 2);
  EXPECT_EQ(result.trailParticleCount, 0);
  EXPECT_EQ(result.sphereInstances, 2);
  EXPECT_EQ(result.repairedNativeCenterValues, 2);
  EXPECT_EQ(result.centerCandidateCount, 2);
  EXPECT_EQ(result.centerRepairPairUpperBound, 4);
  auto sdf =
      openvdb::gridPtrCast<openvdb::FloatGrid>(read(path("out.vdb"), "sdf"));
  auto mask = openvdb::gridPtrCast<openvdb::MaskGrid>(
      read(path("out.vdb"), "particle_mask"));
  ASSERT_TRUE(sdf);
  ASSERT_TRUE(mask);
  EXPECT_EQ(sdf->getGridClass(), openvdb::GRID_LEVEL_SET);
  EXPECT_EQ(mask->getGridClass(), openvdb::GRID_UNKNOWN);
  EXPECT_EQ(sdf->background(), 3);
  EXPECT_FALSE(mask->background());
  for (int x = -6; x <= 8; ++x)
    for (int y = -5; y <= 5; ++y)
      for (int z = -5; z <= 5; ++z) {
        const openvdb::Coord q(x, y, z);
        const auto distance =
            std::min(sphere(q, {0, 0, 0}, 2), sphere(q, {2, 0, 0}, 2));
        EXPECT_NEAR(sdf->tree().getValue(q), std::clamp(distance, -3., 3.),
                    4e-7)
            << q;
        EXPECT_EQ(mask->tree().isValueOn(q), distance < 0) << q;
      }
  EXPECT_FALSE(mask->tree().isValueOn(
      openvdb::Coord(-2, 0, 0))); // exact surface excluded
  EXPECT_EQ(sdf->metaValue<int64_t>("container:particle_count"), 2);
  EXPECT_EQ(sdf->metaValue<int64_t>("container:particle_stamp_count"), 2);
  EXPECT_LE(result.workUpperBound, o.maxWork);
  EXPECT_LE(result.workspaceUpperBound, p.maxInputBytes);
}

TEST_F(OpenVdbParticleGeometry,
       TaperedTrailIndependentDyadicStampsAndLastSamplePolicy) {
  o.output = vdb::ParticleGeometryOutput::Both;
  o.minRadiusVoxels = 2;
  const auto result = write({{{0, 0, 0}, {4, 0, 0}, 4}});
  // Independent exact recurrence: step R/2, R=4-d/2 gives three stamps.
  const std::array<double, 3> centers{0, -2, -3.5}, radii{4, 3, 2.25};
  ASSERT_EQ(result.sphereInstances, 3);
  EXPECT_EQ(result.trailParticleCount, 1);
  auto sdf =
      openvdb::gridPtrCast<openvdb::FloatGrid>(read(path("out.vdb"), "sdf"));
  auto mask = openvdb::gridPtrCast<openvdb::MaskGrid>(
      read(path("out.vdb"), "particle_mask"));
  for (int x = -8; x <= 7; ++x)
    for (int y = -6; y <= 6; ++y)
      for (int z = -2; z <= 2; ++z) {
        const openvdb::Coord q(x, y, z);
        double expected = 3;
        for (size_t i = 0; i < centers.size(); ++i)
          expected =
              std::min(expected, sphere(q, {centers[i], 0, 0}, radii[i]));
        EXPECT_NEAR(sdf->tree().getValue(q), std::max(expected, -3.), 4e-7)
            << q;
        EXPECT_EQ(mask->tree().isValueOn(q), expected < 0) << q;
      }
  EXPECT_FLOAT_EQ(sdf->tree().getValue(openvdb::Coord(-6, 0, 0)), .25f);
  EXPECT_FALSE(mask->tree().isValueOn(openvdb::Coord(-6, 0, 0)));
  EXPECT_EQ(result.maximumTrailLengthWorld, 4);
  EXPECT_GT(result.instanceUpperBound, result.sphereInstances);
}

TEST_F(OpenVdbParticleGeometry,
       CenterRepairUsesGlobalUnionAndSquaredUnderflowCandidates) {
  const auto result = write({{{0, 0, 0}, {}, 2}, {{1, 0, 0}, {}, 3.25}});
  auto grid =
      openvdb::gridPtrCast<openvdb::FloatGrid>(read(path("out.vdb"), "sdf"));
  // The larger overlapping sphere wins: restoring just the small radius would
  // incorrectly write -2 instead of the global minimum -2.25.
  EXPECT_FLOAT_EQ(grid->tree().getValue(openvdb::Coord(0)), -2.25f);
  EXPECT_EQ(result.repairedNativeCenterValues, 1);
  EXPECT_EQ(result.centerRepairPairUpperBound, 2);
  const auto tiny =
      write({{{1e-200, 0, 0}, {}, 2}, {{-1e-200, 0, 0}, {}, 2}}, "tiny.vdb");
  grid =
      openvdb::gridPtrCast<openvdb::FloatGrid>(read(path("tiny.vdb"), "sdf"));
  EXPECT_EQ(tiny.centerCandidateCount, 1); // deduplicated, nonintegral centers
  EXPECT_EQ(tiny.centerRepairPairUpperBound, 2);
  EXPECT_EQ(tiny.repairedNativeCenterValues, 1);
  EXPECT_FLOAT_EQ(grid->tree().getValue(openvdb::Coord(0)), -2);
}

TEST_F(OpenVdbParticleGeometry,
       DeduplicatedCenterPairWorkIsBoundedBeforeNativeAllocation) {
  const std::vector<vdb::GeometryParticle> repeated(4, {{0, 0, 0}, {}, 2});
  const auto result = write(repeated, "baseline.vdb");
  EXPECT_EQ(result.centerCandidateCount, 1);
  EXPECT_EQ(result.centerRepairPairUpperBound, 4);
  o.maxWork = result.workUpperBound - 1;
  try {
    (void)write(repeated);
    FAIL();
  } catch (const std::invalid_argument &error) {
    EXPECT_NE(std::string(error.what()).find("center*stamp repair work"),
              std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
}

TEST_F(OpenVdbParticleGeometry, SignedTimeZeroVelocityAndDeterministicOrder) {
  o.minRadiusVoxels = 2;
  o.timeStep = -1;
  const auto a =
      write({{{0, 0, 0}, {4, 0, 0}, 4}, {{0, 4, 0}, {}, 2}}, "a.vdb");
  o.timeStep = 1;
  const auto b =
      write({{{0, 4, 0}, {}, 2}, {{0, 0, 0}, {-4, 0, 0}, 4}}, "b.vdb");
  auto ga =
      openvdb::gridPtrCast<openvdb::FloatGrid>(read(path("a.vdb"), "sdf"));
  auto gb =
      openvdb::gridPtrCast<openvdb::FloatGrid>(read(path("b.vdb"), "sdf"));
  EXPECT_EQ(a.sphereInstances, b.sphereInstances);
  for (int x = -6; x <= 9; ++x)
    for (int y = -6; y <= 9; ++y)
      EXPECT_EQ(ga->tree().getValue(openvdb::Coord(x, y, 0)),
                gb->tree().getValue(openvdb::Coord(x, y, 0)));
  o.timeStep = 0;
  const auto zero = write({{{0, 0, 0}, {1e308, -1e308, 1e308}, 2}}, "zero.vdb");
  EXPECT_EQ(zero.sphereInstances, 1);
  EXPECT_EQ(zero.trailParticleCount, 0);
}

TEST_F(OpenVdbParticleGeometry,
       WorldDistanceRoundingAndExactTinyLargeRemoteMaps) {
  for (const double h : {1e-6, .07, .125, 1e6}) {
    p.voxelSize = h;
    const std::array<double, 3> origin =
        h == 1e6 ? std::array<double, 3>{1e12 + .125, -2e12, 3e12}
                 : std::array<double, 3>{123.125, -22.25, 44.5};
    o.originWorld = origin;
    const auto filename = "map-" + std::to_string(h) + ".vdb";
    const auto result = write({{origin, {}, 2 * h}}, filename);
    auto grid =
        openvdb::gridPtrCast<openvdb::FloatGrid>(read(path(filename), "sdf"));
    const auto map = grid->transform().baseMap();
    for (int axis = 0; axis < 3; ++axis) {
      openvdb::Vec3d unit(0);
      unit[axis] = 1;
      const auto column = map->applyJacobian(unit);
      EXPECT_EQ(column[axis], h);
      EXPECT_EQ(map->applyMap(openvdb::Vec3d(0))[axis], origin[axis]);
    }
    EXPECT_EQ(result.originWorld, origin);
    EXPECT_FLOAT_EQ(grid->tree().getValue(openvdb::Coord(1, 0, 0)), float(-h));
    EXPECT_FLOAT_EQ(grid->background(), float(3 * h));
    if (h == .07)
      EXPECT_GT(result.roundedDistanceValues, 0);
    EXPECT_LE(result.maximumPositionErrorVoxels, 1e-4);
  }
}

TEST_F(OpenVdbParticleGeometry,
       AutomaticMidpointOriginAndExplicitLatticePhase) {
  o.originWorld.reset();
  const std::array<double, 3> center{1e6 + .125, 2e6 + .25, -3e6 - .5};
  auto result = write({{center, {}, 2}});
  EXPECT_EQ(result.originWorld, center);
  auto grid =
      openvdb::gridPtrCast<openvdb::FloatGrid>(read(path("out.vdb"), "sdf"));
  EXPECT_FLOAT_EQ(grid->tree().getValue(openvdb::Coord(0)), -2);
  o.originWorld = std::array<double, 3>{center[0] + .25, center[1], center[2]};
  result = write({{center, {}, 2}}, "phase.vdb");
  grid =
      openvdb::gridPtrCast<openvdb::FloatGrid>(read(path("phase.vdb"), "sdf"));
  EXPECT_FLOAT_EQ(grid->tree().getValue(openvdb::Coord(0)), -1.75);
}

TEST_F(OpenVdbParticleGeometry,
       EmptyOutputsMaskOnlyAndIrrelevantGenerationFields) {
  o.output = vdb::ParticleGeometryOutput::Both;
  const auto result = write({});
  EXPECT_EQ(result.particleCount, 0);
  EXPECT_EQ(result.sphereInstances, 0);
  EXPECT_TRUE(result.levelSet);
  EXPECT_TRUE(result.mask);
  EXPECT_EQ(read(path("out.vdb"), "sdf")->activeVoxelCount(), 0);
  EXPECT_EQ(read(path("out.vdb"), "particle_mask")->activeVoxelCount(), 0);
  o.output = vdb::ParticleGeometryOutput::Mask;
  p.halfWidth = std::numeric_limits<double>::quiet_NaN();
  p.gridName = "";
  p.maxTriangles = 0;
  const auto mask = write({{{0, 0, 0}, {}, 2}}, "mask.vdb");
  EXPECT_FALSE(mask.levelSet);
  EXPECT_TRUE(mask.mask);
  EXPECT_EQ(mask.effectiveExteriorBand, 0);
}

TEST_F(OpenVdbParticleGeometry,
       InvalidOptionsRadiusCutoffsCoordinatesAndWeakComponents) {
  const std::vector<vdb::GeometryParticle> good{{{0, 0, 0}, {}, 2}};
  for (const double radius : {0., -1., 1.49, 100.01, double(INFINITY),
                              std::numeric_limits<double>::quiet_NaN()})
    rejects([&] { (void)write({{{0, 0, 0}, {}, radius}}); });
  for (const double spacing : {0., .249, 4.001, double(INFINITY)}) {
    o.spacing = spacing;
    rejects([&] { (void)write(good); });
  }
  o.spacing = 1;
  o.output = vdb::ParticleGeometryOutput::Both;
  o.maskGridName = p.gridName;
  rejects([&] { (void)write(good); });
  o.maskGridName = "mask";
  o.output = vdb::ParticleGeometryOutput::LevelSet;
  rejects([&] { (void)write({{{NAN, 0, 0}, {}, 2}}); });
  rejects([&] { (void)write({{{0, 0, 0}, {INFINITY, 0, 0}, 2}}); });
  rejects([&] { (void)write({{{1e15, 0, 0}, {}, 2}}); });
  rejects([&] { (void)write({{{1e6, 0, 0}, {1e-20, 0, 0}, 2}}); });
  rejects([&] {
    (void)write(
        {{{0, 0, 0}, {2, std::numeric_limits<double>::denorm_min(), 0}, 2}});
  });
  o.timeStep = .25;
  rejects([&] {
    (void)write(
        {{{0, 0, 0}, {std::numeric_limits<double>::denorm_min(), 0, 0}, 2}});
  });
  o.timeStep = 2;
  rejects([&] {
    (void)write({{{0, 0, 0}, {std::numeric_limits<double>::max(), 0, 0}, 2}});
  });
  o.timeStep = 1;
  rejects([&] { (void)write({{{double(INT32_MAX) - 2, 0, 0}, {}, 2}}); });
}

TEST_F(OpenVdbParticleGeometry,
       JointBudgetsFailBeforePublicationAndSourceIsBorrowed) {
  const std::vector<vdb::GeometryParticle> records{{{0, 0, 0}, {4, 0, 0}, 4}};
  const auto original = records;
  o.maxParticles = 0;
  rejects([&] { (void)write(records); });
  o.maxParticles = 1;
  o.maxTrailInstances = 2;
  rejects([&] { (void)write(records); });
  o.maxTrailInstances = 1'000'000;
  o.maxTrailLengthVoxels = 1;
  rejects([&] { (void)write(records); });
  o.maxTrailLengthVoxels = 100;
  o.maxNodes = 1;
  rejects([&] { (void)write(records); });
  o.maxNodes = 1'000'000;
  o.maxMetadataBytes = 1;
  rejects([&] { (void)write(records); });
  o.maxMetadataBytes = 1024 * 1024;
  o.maxOutputBytes = 1024;
  rejects([&] { (void)write(records); });
  o.maxOutputBytes = 512ull << 20;
  p.maxInputBytes = 4ull << 20;
  rejects([&] { (void)write(records); });
  p.maxInputBytes = 512ull << 20;
  p.maxVoxelCount = 512;
  rejects([&] { (void)write(records); });
  p.maxVoxelCount = 16'000'000;
  o.maxWork = 512;
  rejects([&] { (void)write(records); });
  EXPECT_EQ(records[0].position, original[0].position);
  EXPECT_EQ(records[0].velocity, original[0].velocity);
  EXPECT_EQ(records[0].radius, original[0].radius);
}

TEST_F(OpenVdbParticleGeometry,
       UnicodeExclusiveOutputLiteralNamesAndSourceProtection) {
  const auto unicode = directory / std::filesystem::path(u8"粒子-ş");
  ASSERT_TRUE(std::filesystem::create_directory(unicode));
  const auto destination = unicode / std::filesystem::path(u8"軌跡.vdb");
  const std::vector<vdb::GeometryParticle> records{{{0, 0, 0}, {}, 2}};
  (void)vdb::WriteParticleGeometry(records, destination, o, p);
  const auto before = contents(destination);
  EXPECT_ANY_THROW(
      (void)vdb::WriteParticleGeometry(records, destination, o, p));
  EXPECT_EQ(before, contents(destination));
  const auto foreign = unicode / "foreign.vdb";
  text(foreign, "foreign bytes");
  EXPECT_ANY_THROW((void)vdb::WriteParticleGeometry(records, foreign, o, p));
  EXPECT_EQ(contents(foreign), "foreign bytes");
  (void)vdb::WriteParticleGeometry(records, unicode / "-h", o, p);
  EXPECT_EQ(read(unicode / "-h", "sdf")->activeVoxelCount(),
            read(destination, "sdf")->activeVoxelCount());
}

TEST_F(OpenVdbParticleGeometry,
       StrictJsonRoundTripOptionalVelocityAndImmutableNativePaths) {
  const auto unicode = directory / std::filesystem::path(u8"入力-ç");
  ASSERT_TRUE(std::filesystem::create_directory(unicode));
  const auto input = unicode / std::filesystem::path(u8"粒子.json");
  const std::string json =
      R"({"version":1,"particles":[{"position":[0,0,0],"radius":4,"velocity":[4,0,0]},{"position":[0,4,0],"radius":2}]})";
  text(input, json);
  o.minRadiusVoxels = 2;
  const auto fromFile =
      vdb::WriteParticleGeometryFile(input, unicode / "out.vdb", o, p);
  const auto fromSpan = write({{{0, 0, 0}, {4, 0, 0}, 4}, {{0, 4, 0}, {}, 2}});
  EXPECT_EQ(fromFile.sphereInstances, fromSpan.sphereInstances);
  EXPECT_GT(fromFile.workspaceUpperBound, fromSpan.workspaceUpperBound);
  EXPECT_GT(fromFile.workUpperBound, fromSpan.workUpperBound);
  auto a = openvdb::gridPtrCast<openvdb::FloatGrid>(
      read(unicode / "out.vdb", "sdf"));
  auto b =
      openvdb::gridPtrCast<openvdb::FloatGrid>(read(path("out.vdb"), "sdf"));
  for (int x = -8; x <= 8; ++x)
    for (int y = -6; y <= 8; ++y)
      EXPECT_EQ(a->tree().getValue(openvdb::Coord(x, y, 0)),
                b->tree().getValue(openvdb::Coord(x, y, 0)));
  EXPECT_EQ(contents(input), json);
  EXPECT_ANY_THROW((void)vdb::WriteParticleGeometryFile(input, input, o, p));
  EXPECT_EQ(contents(input), json);
}

TEST_F(OpenVdbParticleGeometry,
       StrictJsonSchemaTokensExactIntegersAndUnderflow) {
  const std::vector<std::string> invalid{
      "[]",
      "{}",
      R"({"version":1.0,"particles":[]})",
      R"({"version":2,"particles":[]})",
      R"({"version":1,"particles":[],"particles":[]})",
      R"({"version":1,"particles":[],"extra":0})",
      R"({"version":1,"particles":[{"position":[0,0],"radius":2}]})",
      R"({"version":1,"particles":[{"position":[0,0,0,0],"radius":2}]})",
      R"({"version":1,"particles":[{"position":[0,0,0],"radius":2,"radius":3}]})",
      R"({"version":1,"particles":[{"position":[0,0,0],"radius":2,"velocity":[0,false,0]}]})",
      R"({"version":1,"particles":[{"position":[0,0,0],"radius":2,"velocity":[0,0]}]})",
      R"({"version":1,"particles":[{"position":[9007199254740993,0,0],"radius":2}]})",
      R"({"version":1,"particles":[{"position":[0,0,0],"radius":2,"velocity":[18446744073709551617,0,0]}]})",
      R"({"version":1,"particles":[{"position":[0,0,0],"radius":2,"velocity":[1e-999,0,0]}]})",
      R"({"version":1,"particles":[{"position":[0,0,0],"radius":2,"extra":{}}]})",
      R"({"version":1,"particles":[{"position":[0,0,0],"radius":2e999}]})",
      std::string("{\"") + std::string(129, 'x') + "\":0}",
      std::string("{\"version\":1,\"particles\":[],\"") + char(0xff) + "\":0}"};
  for (const auto &json : invalid) {
    text(path("in.json"), json);
    rejects([&] {
      (void)vdb::WriteParticleGeometryFile(path("in.json"), path("out.vdb"), o,
                                           p);
    });
    EXPECT_EQ(contents(path("in.json")), json);
  }
  text(path("in.json"), R"({"version":1,"particles":[]})");
  const auto empty =
      vdb::WriteParticleGeometryFile(path("in.json"), path("empty.vdb"), o, p);
  EXPECT_EQ(empty.particleCount, 0);
}

TEST_F(OpenVdbParticleGeometry, JsonSharedEncodedDecodedCountAndWorkLimits) {
  const std::string json =
      R"({"version":1,"particles":[{"position":[0,0,0],"radius":2},{"position":[1,0,0],"radius":2}]})";
  text(path("in.json"), json);
  o.maxParticles = 1;
  rejects([&] {
    (void)vdb::WriteParticleGeometryFile(path("in.json"), path("out.vdb"), o,
                                         p);
  });
  o.maxParticles = 2;
  o.maxWork = json.size() * 8 - 1;
  rejects([&] {
    (void)vdb::WriteParticleGeometryFile(path("in.json"), path("out.vdb"), o,
                                         p);
  });
  o.maxWork = json.size() * 8 + 511;
  rejects([&] {
    (void)vdb::WriteParticleGeometryFile(path("in.json"), path("out.vdb"), o,
                                         p);
  });
  o.maxWork = 2'000'000'000;
  p.maxInputBytes =
      (4ull << 20) + json.size() * 3 + sizeof(vdb::GeometryParticle);
  rejects([&] {
    (void)vdb::WriteParticleGeometryFile(path("in.json"), path("out.vdb"), o,
                                         p);
  });
  EXPECT_EQ(contents(path("in.json")), json);
}
#endif
} // namespace
