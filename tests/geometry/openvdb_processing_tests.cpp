#include "Container/geometry/DotBimLoader.h"
#include "Container/geometry/OpenVdbLoader.h"
#include "Container/geometry/OpenVdbProcessing.h"
#include "Container/geometry/OpenVdbMeshConversion.h"

#include <gtest/gtest.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <nlohmann/json.hpp>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/File.h>
#include <openvdb/openvdb.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;

class OpenVdbProcessing : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB is disabled";
    static std::atomic_uint64_t serial{0};
    directory = std::filesystem::temp_directory_path() /
        ("container-vdb-processing-" + std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
    options.voxelSize = 0.2;
    options.maxVoxelCount = 1'000'000;
  }
  void TearDown() override {
    if (directory.empty()) return;
    std::error_code ignored;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
      std::filesystem::remove(entry.path(), ignored);
    std::filesystem::remove(directory, ignored);
  }
  std::filesystem::path file(const char* name) { return directory / name; }

  std::filesystem::path gltfCube(bool binary = false, bool cycle = false) {
    using Json = nlohmann::json;
    const std::array<float, 24> positions{
        -1,-1,-1, 1,-1,-1, 1,1,-1, -1,1,-1,
        -1,-1,1, 1,-1,1, 1,1,1, -1,1,1};
    const std::array<uint16_t, 36> indices{
        0,2,1,0,3,2,4,5,6,4,6,7,0,1,5,0,5,4,
        3,7,6,3,6,2,0,4,7,0,7,3,1,2,6,1,6,5};
    std::vector<char> bytes(sizeof(positions) + sizeof(indices));
    std::memcpy(bytes.data(), positions.data(), sizeof(positions));
    std::memcpy(bytes.data() + sizeof(positions), indices.data(), sizeof(indices));
    Json json{
        {"asset", {{"version", "2.0"}}},
        {"buffers", Json::array({{{"byteLength", bytes.size()}}})},
        {"bufferViews", Json::array({
            {{"buffer", 0}, {"byteOffset", 0}, {"byteLength", sizeof(positions)}},
            {{"buffer", 0}, {"byteOffset", sizeof(positions)}, {"byteLength", sizeof(indices)}}})},
        {"accessors", Json::array({
            {{"bufferView", 0}, {"componentType", 5126}, {"count", 8}, {"type", "VEC3"},
             {"min", {-1,-1,-1}}, {"max", {1,1,1}}},
            {{"bufferView", 1}, {"componentType", 5123}, {"count", 36}, {"type", "SCALAR"}}})},
        {"meshes", Json::array({{{"primitives", Json::array({
            {{"attributes", {{"POSITION", 0}}}, {"indices", 1}, {"mode", 4}}})}}})},
        {"nodes", Json::array({
            {{"translation", {3,0,0}}, {"children", {1}}},
            {{"translation", {0,2,0}}, {"scale", {0.5,1,1}}, {"mesh", 0}},
            {{"translation", {-3,0,0}}, {"mesh", 0}},
            {{"translation", {20,0,0}}, {"mesh", 0}}})},
        {"scenes", Json::array({{{"nodes", {0,2}}}, {{"nodes", {3}}}})},
        {"scene", 0}};
    if (cycle) json["nodes"][1]["children"] = {0};
    const auto path = file(binary ? "cube.glb" : "cube.gltf");
    if (!binary) {
      json["buffers"][0]["uri"] = "cube.bin";
      std::ofstream payload(file("cube.bin"), std::ios::binary);
      payload.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      std::ofstream(path) << json.dump();
    } else {
      auto text = json.dump();
      while (text.size() % 4) text.push_back(' ');
      while (bytes.size() % 4) bytes.push_back(0);
      std::ofstream output(path, std::ios::binary);
      auto word = [&](uint32_t value) {
        output.write(reinterpret_cast<const char*>(&value), sizeof(value));
      };
      word(0x46546c67); word(2);
      word(static_cast<uint32_t>(12 + 8 + text.size() + 8 + bytes.size()));
      word(static_cast<uint32_t>(text.size())); word(0x4e4f534a);
      output.write(text.data(), static_cast<std::streamsize>(text.size()));
      word(static_cast<uint32_t>(bytes.size())); word(0x004e4942);
      output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    return path;
  }
  std::filesystem::path directory;
  vdb::ProcessingOptions options;
};

TEST(OpenVdbProcessingAvailability, DisabledBuildHasActionableError) {
  if (vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB is enabled";
  try {
    (void)vdb::WriteSphere("disabled-do-not-create.vdb");
    FAIL() << "disabled processing should throw";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists("disabled-do-not-create.vdb"));
  EXPECT_THROW((void)vdb::WriteOffset("missing.vdb", "disabled-do-not-create.vdb", 0.1),
               std::runtime_error);
  EXPECT_THROW((void)vdb::WriteSmoothed("missing.vdb", "disabled-do-not-create.vdb",
                                       vdb::SmoothingOperation::Gaussian), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists("disabled-do-not-create.vdb"));
}

TEST_F(OpenVdbProcessing, SphereRoundTripsToSurfaceAndHasSignedInterior) {
  const auto result = vdb::WriteSphere(file("sphere.vdb"), {}, options);
  EXPECT_EQ(result.gridClass, "level set");
  EXPECT_EQ(result.gridName, "surface");
  EXPECT_GT(result.activeVoxelCount, 0u);
  EXPECT_NEAR(result.voxelSize, 0.2, 1e-6);
  EXPECT_LT(vdb::SampleScalar(file("sphere.vdb"), {0, 0, 0}), 0.0);
  EXPECT_GT(vdb::SampleScalar(file("sphere.vdb"), {2, 0, 0}), 0.0);
  const auto model = vdb::LoadFromFile(file("sphere.vdb"));
  EXPECT_FALSE(model.meshRanges.empty());
  EXPECT_FALSE(model.indices.empty());
}

TEST_F(OpenVdbProcessing, FogHasUnitInteriorAndZeroExterior) {
  (void)vdb::WriteSphere(file("sphere.vdb"), {}, options);
  options.gridName = "density";
  const auto result = vdb::WriteFog(file("sphere.vdb"), file("fog.vdb"), "surface", options);
  EXPECT_EQ(result.gridClass, "fog volume");
  EXPECT_DOUBLE_EQ(vdb::SampleScalar(file("fog.vdb"), {0, 0, 0}, "density"), 1.0);
  EXPECT_DOUBLE_EQ(vdb::SampleScalar(file("fog.vdb"), {2, 0, 0}, "density"), 0.0);
  EXPECT_THROW((void)vdb::WriteFog(file("fog.vdb"), file("bad.vdb")), std::invalid_argument);
}

TEST_F(OpenVdbProcessing, CsgPreservesExpectedInsideOutsideSigns) {
  (void)vdb::WriteSphere(file("a.vdb"), {}, options);
  (void)vdb::WriteSphere(file("b.vdb"), {.radius = 1.0, .center = {1.0, 0, 0}}, options);
  (void)vdb::WriteCsg(file("a.vdb"), file("b.vdb"), file("union.vdb"), vdb::CsgOperation::Union);
  (void)vdb::WriteCsg(file("a.vdb"), file("b.vdb"), file("intersect.vdb"), vdb::CsgOperation::Intersection);
  (void)vdb::WriteCsg(file("a.vdb"), file("b.vdb"), file("difference.vdb"), vdb::CsgOperation::Difference);
  EXPECT_LT(vdb::SampleScalar(file("union.vdb"), {1.4, 0, 0}), 0.0);
  EXPECT_GT(vdb::SampleScalar(file("intersect.vdb"), {1.4, 0, 0}), 0.0);
  EXPECT_LT(vdb::SampleScalar(file("intersect.vdb"), {0.4, 0, 0}), 0.0);
  EXPECT_LT(vdb::SampleScalar(file("difference.vdb"), {-0.4, 0, 0}), 0.0);
  EXPECT_GT(vdb::SampleScalar(file("difference.vdb"), {0.6, 0, 0}), 0.0);
  // Compositing must leave the files that supplied the two SDFs untouched.
  EXPECT_LT(vdb::SampleScalar(file("a.vdb"), {0.6, 0, 0}), 0.0);
}

TEST_F(OpenVdbProcessing, MeshVoxelizationAppliesElementTransforms) {
  constexpr auto json = R"({"schema_version":"1.1.0","meshes":[{
    "mesh_id":0,"coordinates":[-1,-1,-1,1,-1,-1,1,1,-1,-1,1,-1,
    -1,-1,1,1,-1,1,1,1,1,-1,1,1],"indices":[0,2,1,0,3,2,
    4,5,6,4,6,7,0,1,5,0,5,4,3,7,6,3,6,2,0,4,7,0,7,3,1,2,6,1,6,5]}],
    "elements":[{"mesh_id":0,"guid":"cube","type":"cube",
    "vector":{"x":3,"y":0,"z":0},"rotation":{"qx":0,"qy":0,"qz":0,"qw":1},
    "color":{"r":255,"g":255,"b":255,"a":255}}]})";
  const auto model = container::geometry::dotbim::LoadFromJson(json);
  const auto result = vdb::WriteMeshLevelSet(model, file("cube.vdb"), options);
  EXPECT_GT(result.activeVoxelCount, 0u);
  EXPECT_LT(vdb::SampleScalar(file("cube.vdb"), {3, 0, 0}), 0.0);
  EXPECT_GT(vdb::SampleScalar(file("cube.vdb"), {0, 0, 0}), 0.0);
}

TEST_F(OpenVdbProcessing, RejectsInvalidGeometryOptionsAndBudgetsBeforeWriting) {
  options.voxelSize = std::numeric_limits<double>::quiet_NaN();
  EXPECT_THROW((void)vdb::WriteSphere(file("bad.vdb"), {}, options), std::invalid_argument);
  options.voxelSize = 0.2;
  EXPECT_THROW((void)vdb::WriteSphere(file("bad.vdb"), {.radius = -1}, options), std::invalid_argument);
  options.maxVoxelCount = 8;
  EXPECT_THROW((void)vdb::WriteSphere(file("bad.vdb"), {}, options), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbProcessing, RejectsCsgTransformMismatchAndOutputOverwrite) {
  (void)vdb::WriteSphere(file("a.vdb"), {}, options);
  options.voxelSize = 0.25;
  (void)vdb::WriteSphere(file("b.vdb"), {}, options);
  EXPECT_THROW((void)vdb::WriteCsg(file("a.vdb"), file("b.vdb"), file("bad.vdb"),
                                 vdb::CsgOperation::Union), std::invalid_argument);
  EXPECT_THROW((void)vdb::WriteSphere(file("a.vdb"), {}, options), std::invalid_argument);
  EXPECT_LT(vdb::SampleScalar(file("a.vdb"), {0, 0, 0}), 0.0);
}

TEST_F(OpenVdbProcessing, RejectsInvalidMeshIndicesAndTriangleBudget) {
  container::geometry::dotbim::Model model;
  model.vertices.resize(1);
  model.indices = {0, 1, 2};
  model.meshRanges.push_back({.meshId = 0, .firstIndex = 0, .indexCount = 3});
  model.elements.push_back({.meshId = 0});
  EXPECT_THROW((void)vdb::WriteMeshLevelSet(model, file("bad.vdb"), options),
               std::invalid_argument);
  options.maxTriangles = 1;
  model.indices = {0, 0, 0, 0, 0, 0};
  model.meshRanges[0].indexCount = 6;
  EXPECT_THROW((void)vdb::WriteMeshLevelSet(model, file("bad.vdb"), options),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbProcessing, RejectsMissingGridAndInputFileBudget) {
  (void)vdb::WriteSphere(file("sphere.vdb"), {}, options);
  EXPECT_THROW((void)vdb::SampleScalar(file("sphere.vdb"), {0, 0, 0}, "missing"),
               std::invalid_argument);
  options.maxInputBytes = 1;
  EXPECT_THROW((void)vdb::WriteFog(file("sphere.vdb"), file("bad.vdb"), {}, options),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbProcessing, RejectsIncompleteImportsAndNonMeshOmissions) {
  container::geometry::dotbim::Model model;
  model.importReport.completeness = container::geometry::ImportCompleteness::Partial;
  model.importReport.skippedProductCount = 1;
  model.importReport.diagnostics.push_back({.reason = "missing closed solid"});
  try {
    (void)vdb::WriteMeshLevelSet(model, file("partial.vdb"), options);
    FAIL();
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("missing closed solid"), std::string::npos);
  }
  model.importReport = {};
  model.elements.push_back({.geometryKind = container::geometry::dotbim::GeometryKind::Curves});
  EXPECT_THROW((void)vdb::WriteMeshLevelSet(model, file("curves.vdb"), options),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("partial.vdb")));
  EXPECT_FALSE(std::filesystem::exists(file("curves.vdb")));
}

TEST_F(OpenVdbProcessing, GltfAndGlbVoxelizeNestedActiveSceneWorldInstances) {
  for (const bool binary : {false, true}) {
    const auto output = file(binary ? "glb.vdb" : "gltf.vdb");
    const auto result = vdb::VoxelizeModel(gltfCube(binary), output, 2.0f, options);
    EXPECT_GT(result.activeVoxelCount, 0u);
    EXPECT_LT(vdb::SampleScalar(output, {6,4,0}), 0.0);
    EXPECT_LT(vdb::SampleScalar(output, {-6,0,0}), 0.0);
    EXPECT_GT(vdb::SampleScalar(output, {0,0,0}), 0.0);
    EXPECT_GT(vdb::SampleScalar(output, {7.4,4,0}), 0.0);
    EXPECT_GT(vdb::SampleScalar(output, {40,0,0}), 0.0);
  }
}

TEST_F(OpenVdbProcessing, GltfRejectsCyclesAndCountsEveryTriangleInstance) {
  const auto invalid=gltfCube(false,true);
  try {
    (void)vdb::VoxelizeModel(invalid,file("cycle.vdb"),1.0f,options);
    FAIL();
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("cycle"),std::string::npos);
    EXPECT_NE(std::string(error.what()).find("cube.gltf"),std::string::npos);
  }
  options.maxTriangles = 12;
  EXPECT_THROW((void)vdb::VoxelizeModel(gltfCube(), file("budget.vdb"), 1.0f, options),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("cycle.vdb")));
  EXPECT_FALSE(std::filesystem::exists(file("budget.vdb")));
}

TEST_F(OpenVdbProcessing, UnsignedGltfRejectsLossOfDoubleInstanceTranslationBeforeConversion) {
  const auto input=gltfCube();
  nlohmann::json source;
  { std::ifstream stream(input); stream>>source; }
  source["scenes"][0]["nodes"]={0};
  source["nodes"][0]["translation"]={1000000.123456789,0,0};
  { std::ofstream stream(input); stream<<source.dump(); }
  options.voxelSize=.1;
  try {
    (void)vdb::VoxelizeModelDistance(input,file("rounded.vdb"),1,options);
    FAIL();
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("Float transform"),std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(file("rounded.vdb")));
  // An exactly representable translation remains available.
  source["nodes"][0]["translation"]={1000000,0,0};
  { std::ofstream stream(input); stream<<source.dump(); }
  EXPECT_GT(vdb::VoxelizeModelDistance(input,file("exact.vdb"),1,options).distance.activeVoxelCount,0u);
}

TEST_F(OpenVdbProcessing, GltfSkipsTextureDecodingAndPreflightsBufferAllocations) {
  const auto input = gltfCube();
  nlohmann::json json;
  { std::ifstream stream(input); stream >> json; }
  // A geometry-only task does not need to decode this deliberately non-image
  // payload; the same malformed image would make the default image loader fail.
  json["images"] = nlohmann::json::array({
      {{"uri", "data:image/png;base64,bm90YW5pbWFnZQ=="}}});
  { std::ofstream stream(input); stream << json.dump(); }
  EXPECT_GT(vdb::VoxelizeModel(input, file("no-textures.vdb"), 1.0f, options).activeVoxelCount, 0u);

  json["buffers"][0]["byteLength"] = options.maxInputBytes + 1;
  { std::ofstream stream(input); stream << json.dump(); }
  try {
    (void)vdb::VoxelizeModel(input, file("too-large.vdb"), 1.0f, options);
    FAIL();
  } catch (const std::exception& error) {
    // The glTF loader attaches source context to preflight failures. Keep the
    // budget diagnostic contract independent of its exception wrapper type.
    EXPECT_NE(std::string(error.what()).find("budget"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(file("too-large.vdb")));

  json["buffers"][0]["byteLength"] = 168;
  { std::ofstream stream(input); stream << json.dump(); }
  { std::ofstream stream(file("cube.bin"), std::ios::binary | std::ios::app);
    const std::string oversized(8192, '\0'); stream.write(oversized.data(), oversized.size()); }
  options.maxInputBytes = 4096;
  EXPECT_THROW((void)vdb::VoxelizeModel(input, file("external-too-large.vdb"), 1.0f, options),
               std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(file("external-too-large.vdb")));
}

TEST_F(OpenVdbProcessing, ResamplingRebuildsWorldDistancesAndEnablesMismatchedCsg) {
  (void)vdb::WriteSphere(file("a.vdb"), {}, options);
  auto other = options;
  other.voxelSize = 0.3;
  other.halfWidth = 4;
  (void)vdb::WriteSphere(file("b.vdb"), {.center = {0.6,0,0}}, other);
  auto target = options;
  target.voxelSize = 0.1;
  target.halfWidth = 3;
  const auto a = vdb::WriteResampled(file("a.vdb"), file("a-match.vdb"), {}, target);
  const auto b = vdb::WriteResampled(file("b.vdb"), file("b-match.vdb"), {}, target);
  EXPECT_NEAR(a.voxelSize, 0.1, 1e-8);
  EXPECT_NEAR(b.voxelSize, 0.1, 1e-8);
  EXPECT_NEAR(vdb::SampleScalar(file("a-match.vdb"), {1.2,0,0}), 0.2, 0.06);
  EXPECT_NEAR(vdb::SampleScalar(file("a-match.vdb"), {0.8,0,0}), -0.2, 0.06);
  (void)vdb::WriteCsg(file("a-match.vdb"), file("b-match.vdb"), file("matched.vdb"),
                     vdb::CsgOperation::Union, {}, {}, target);
  EXPECT_LT(vdb::SampleScalar(file("matched.vdb"), {1.4,0,0}), 0.0);
  EXPECT_GT(vdb::SampleScalar(file("matched.vdb"), {-1.4,0,0}), 0.0);
  EXPECT_THROW((void)vdb::WriteResampled(file("a.vdb"), file("a-match.vdb"), {}, target),
               std::invalid_argument);
  target.voxelSize = 0.0001;
  EXPECT_THROW((void)vdb::WriteResampled(file("a.vdb"), file("too-fine.vdb"), {}, target),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("too-fine.vdb")));
}

TEST_F(OpenVdbProcessing, FogResamplingPreservesDensityMagnitudeAndExterior) {
  (void)vdb::WriteSphere(file("sphere.vdb"), {}, options);
  options.gridName = "density";
  (void)vdb::WriteFog(file("sphere.vdb"), file("fog.vdb"), {}, options);
  options.voxelSize = 0.1;
  const auto result = vdb::WriteResampled(file("fog.vdb"), file("fog-match.vdb"), {}, options);
  EXPECT_EQ(result.gridClass, "fog volume");
  EXPECT_EQ(result.gridName, "density");
  EXPECT_NEAR(vdb::SampleScalar(file("fog-match.vdb"), {0,0,0}), 1.0, 1e-6);
  EXPECT_DOUBLE_EQ(vdb::SampleScalar(file("fog-match.vdb"), {2,0,0}), 0.0);
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
openvdb::FloatGrid::Ptr readProcessingFixture(const std::filesystem::path& path,
                                            const std::string& name = "surface") {
  openvdb::io::File input(path.string());
  input.open(false);
  auto grid = openvdb::gridPtrCast<openvdb::FloatGrid>(input.readGrid(name));
  input.close();
  return grid;
}

void writeProcessingFixture(const std::filesystem::path& path,
                            const openvdb::GridBase::Ptr& grid) {
  openvdb::io::File output(path.string());
  output.write(openvdb::GridPtrVec{grid});
  output.close();
}

openvdb::math::Transform::Ptr serializedScaleFixture(const openvdb::Vec3d& scale) {
  // Scale-map constructors reject tiny determinants. Serialized uniform maps
  // can still round-trip through File, while copying a tiny general ScaleMap
  // promotes it to a rejected AffineMap. Use the correct map type for each
  // coherent serialized state to reach the production guard through real IO.
  std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
  openvdb::io::setVersion(stream,
      openvdb::VersionId(OPENVDB_LIBRARY_MAJOR_VERSION_NUMBER, OPENVDB_LIBRARY_MINOR_VERSION_NUMBER),
      openvdb::OPENVDB_FILE_VERSION);
  const bool uniform = scale.x() == scale.y() && scale.x() == scale.z();
  openvdb::writeString(stream, uniform ? openvdb::math::UniformScaleMap::mapType()
                                      : openvdb::math::ScaleMap::mapType());
  const openvdb::Vec3d inverse(1.0 / scale.x(), 1.0 / scale.y(), 1.0 / scale.z());
  scale.write(stream);
  openvdb::Vec3d(std::abs(scale.x()), std::abs(scale.y()), std::abs(scale.z())).write(stream);
  inverse.write(stream);
  (inverse * inverse).write(stream);
  (inverse / 2.0).write(stream);
  stream.seekg(0);
  auto transform = openvdb::math::Transform::Ptr(new openvdb::math::Transform());
  transform->read(stream);
  return transform;
}

TEST_F(OpenVdbProcessing, OffsetUsesWorldDistanceAndPreservesTranslatedBandAndMetadata) {
  options.halfWidth = 4;
  (void)vdb::WriteSphere(file("source.vdb"), {}, options);
  const auto source = readProcessingFixture(file("source.vdb"));
  source->transform().postTranslate(openvdb::Vec3d(3, -2, 1));
  source->insertMeta("artist", openvdb::StringMetadata("original metadata"));
  writeProcessingFixture(file("source.vdb"), source);
  auto outputOptions = options;
  outputOptions.gridName = "edited";
  // Generation settings must not change the input's transform or band.
  outputOptions.voxelSize = 0.75;
  outputOptions.halfWidth = 9;
  const auto expanded = vdb::WriteOffset(file("source.vdb"), file("expanded.vdb"),
                                         0.4, "surface", outputOptions);
  (void)vdb::WriteOffset(file("source.vdb"), file("eroded.vdb"), -0.4, {}, outputOptions);
  (void)vdb::WriteOffset(file("source.vdb"), file("identity.vdb"), 0.0, {}, outputOptions);
  (void)vdb::WriteOffset(file("source.vdb"), file("boundary.vdb"),
                         0.001 * source->voxelSize().x(), {}, outputOptions);
  EXPECT_NEAR(expanded.voxelSize, 0.2, 1e-6);
  EXPECT_NEAR(vdb::SampleScalar(file("expanded.vdb"), {4.4,-2,1}, "edited"), 0.0, 0.07);
  EXPECT_NEAR(vdb::SampleScalar(file("eroded.vdb"), {3.6,-2,1}, "edited"), 0.0, 0.07);
  EXPECT_LT(vdb::SampleScalar(file("expanded.vdb"), {4.2,-2,1}, "edited"), -0.1);
  EXPECT_GT(vdb::SampleScalar(file("eroded.vdb"), {3.8,-2,1}, "edited"), 0.1);
  EXPECT_LT(vdb::SampleScalar(file("boundary.vdb"), {4,-2,1}, "edited"), -1e-5);
  EXPECT_NEAR(vdb::SampleScalar(file("source.vdb"), {4.4,-2,1}), 0.4, 1e-5);
  const auto result = readProcessingFixture(file("expanded.vdb"), "edited");
  EXPECT_EQ(result->transform(), source->transform());
  EXPECT_EQ(result->background(), source->background());
  ASSERT_TRUE(result->getMetadata<openvdb::StringMetadata>("artist"));
  EXPECT_EQ(result->metaValue<std::string>("artist"), "original metadata");
  EXPECT_DOUBLE_EQ(vdb::SampleScalar(file("identity.vdb"), {4.2,-2,1}, "edited"),
                   vdb::SampleScalar(file("source.vdb"), {4.2,-2,1}));
  EXPECT_THROW((void)vdb::WriteOffset(file("source.vdb"), file("expanded.vdb"), 0.2),
               std::invalid_argument);
}

TEST_F(OpenVdbProcessing, EveryTrackedSmoothingFilterReducesAnIsolatedSurfaceDefect) {
  (void)vdb::WriteSphere(file("smooth-source.vdb"), {}, options);
  const auto source = readProcessingFixture(file("smooth-source.vdb"));
  source->tree().setValue(openvdb::Coord(5, 0, 0), -0.2f);
  source->insertMeta("artist", openvdb::StringMetadata("surface defect fixture"));
  writeProcessingFixture(file("smooth-source.vdb"), source);
  size_t index = 0;
  for (const auto operation : {vdb::SmoothingOperation::Gaussian,
                                vdb::SmoothingOperation::Mean,
                                vdb::SmoothingOperation::Median}) {
    const auto output = directory / ("smoothed-" + std::to_string(index++) + ".vdb");
    const auto result = vdb::WriteSmoothed(file("smooth-source.vdb"), output, operation);
    EXPECT_EQ(result.gridClass, "level set");
    EXPECT_GT(result.activeVoxelCount, 0u);
    const double filtered = vdb::SampleScalar(output, {1,0,0});
    EXPECT_LT(std::abs(filtered), 0.19) << "filter " << index;
    EXPECT_GT(filtered, -0.15) << "filter " << index;
    const auto grid = readProcessingFixture(output);
    EXPECT_EQ(grid->transform(), source->transform());
    EXPECT_EQ(grid->background(), source->background());
    EXPECT_EQ(grid->metaValue<std::string>("artist"), "surface defect fixture");
    EXPECT_THROW((void)vdb::WriteSmoothed(file("smooth-source.vdb"), output, operation),
                 std::invalid_argument);
  }
  EXPECT_NEAR(vdb::SampleScalar(file("smooth-source.vdb"), {1,0,0}), -0.2, 1e-6);
}

TEST_F(OpenVdbProcessing, EditingRejectsUnsupportedGridsAndInvalidParameters) {
  (void)vdb::WriteSphere(file("source.vdb"), {}, options);
  (void)vdb::WriteFog(file("source.vdb"), file("fog.vdb"), {}, options);
  for (const double distance : {std::numeric_limits<double>::quiet_NaN(),
                                std::numeric_limits<double>::infinity(),
                                1e300, 100.0, 1e-12})
    EXPECT_THROW((void)vdb::WriteOffset(file("source.vdb"), file("bad.vdb"), distance),
                 std::invalid_argument);
  EXPECT_THROW((void)vdb::WriteOffset(file("fog.vdb"), file("bad.vdb"), 0.2),
               std::invalid_argument);
  EXPECT_THROW((void)vdb::WriteSmoothed(file("fog.vdb"), file("bad.vdb"),
                                       vdb::SmoothingOperation::Mean), std::invalid_argument);
  for (const vdb::SmoothingOptions smoothing : {vdb::SmoothingOptions{0,1}, {5,1}, {1,0}, {1,33}})
    EXPECT_THROW((void)vdb::WriteSmoothed(file("source.vdb"), file("bad.vdb"),
                                         vdb::SmoothingOperation::Gaussian, smoothing),
                 std::invalid_argument);
  EXPECT_THROW((void)vdb::WriteSmoothed(file("source.vdb"), file("bad.vdb"),
                                       static_cast<vdb::SmoothingOperation>(999)),
               std::invalid_argument);
  auto anisotropic = readProcessingFixture(file("source.vdb"));
  anisotropic->transform().postScale(openvdb::Vec3d(1,2,1));
  writeProcessingFixture(file("anisotropic.vdb"), anisotropic);
  EXPECT_THROW((void)vdb::WriteOffset(file("anisotropic.vdb"), file("bad.vdb"), 0.2),
               std::invalid_argument);
  auto relativeAnisotropic = openvdb::FloatGrid::create(3e-4f);
  relativeAnisotropic->setName("surface");
  relativeAnisotropic->setGridClass(openvdb::GRID_LEVEL_SET);
  // All axes lie inside the supported tracking range. The 2e-7 absolute
  // difference passes OpenVDB 12 ScaleMap's 5e-7 tolerance but is a 0.2%
  // relative anisotropy, so the separate scale-range guard cannot mask this.
  ASSERT_NO_THROW(relativeAnisotropic->setTransform(
      serializedScaleFixture(openvdb::Vec3d(1e-4, 1.002e-4, 1e-4))));
  relativeAnisotropic->tree().setValue(openvdb::Coord(0), 0.0f);
  ASSERT_NO_THROW(writeProcessingFixture(file("relative-anisotropic.vdb"), relativeAnisotropic));
  try {
    (void)vdb::WriteOffset(file("relative-anisotropic.vdb"), file("bad.vdb"), 2e-5);
    FAIL() << "supported scales still require relative uniformity";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("uniform"), std::string::npos);
  }
  for (const double voxelSize : {1e-8, 1e7}) {
    SCOPED_TRACE(::testing::Message() << "numeric scale fixture dx=" << voxelSize);
    auto extreme = openvdb::FloatGrid::create(static_cast<float>(3 * voxelSize));
    extreme->setName("surface");
    extreme->setGridClass(openvdb::GRID_LEVEL_SET);
    ASSERT_NO_THROW(extreme->setTransform(serializedScaleFixture(openvdb::Vec3d(voxelSize))));
    extreme->tree().setValue(openvdb::Coord(0), 0.0f);
    ASSERT_NO_THROW(writeProcessingFixture(file("extreme.vdb"), extreme));
    try {
      (void)vdb::WriteSmoothed(file("extreme.vdb"), file("bad.vdb"), vdb::SmoothingOperation::Mean);
      FAIL() << "unsupported Float32 tracking scale must fail before normalization";
    } catch (const std::invalid_argument& error) {
      EXPECT_NE(std::string(error.what()).find("Float32"), std::string::npos);
    }
  }
  auto outsideBand = readProcessingFixture(file("source.vdb"));
  outsideBand->tree().setValue(openvdb::Coord(5,0,0), 1e30f);
  writeProcessingFixture(file("outside-band.vdb"), outsideBand);
  EXPECT_THROW((void)vdb::WriteSmoothed(file("outside-band.vdb"), file("bad.vdb"),
                                       vdb::SmoothingOperation::Median), std::invalid_argument);
  options.halfWidth = 2;
  (void)vdb::WriteSphere(file("narrow.vdb"), {}, options);
  EXPECT_THROW((void)vdb::WriteSmoothed(file("narrow.vdb"), file("bad.vdb"),
                                       vdb::SmoothingOperation::Median), std::invalid_argument);
  auto doubles = openvdb::DoubleGrid::create(0.6);
  doubles->setGridClass(openvdb::GRID_LEVEL_SET);
  doubles->setName("surface");
  doubles->tree().setValue(openvdb::Coord(0), 0.0);
  writeProcessingFixture(file("double.vdb"), doubles);
  EXPECT_THROW((void)vdb::WriteOffset(file("double.vdb"), file("bad.vdb"), 0.2),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbProcessing, EditingPreflightsTransientTopologyAndWorkspaceBeforeWriting) {
  (void)vdb::WriteSphere(file("source.vdb"), {}, options);
  const auto source = readProcessingFixture(file("source.vdb"));
  auto bounded = options;
  bounded.maxInputBytes = std::max<uintmax_t>(source->memUsage(),
      std::filesystem::file_size(file("source.vdb"))) + 4096;
  try {
    (void)vdb::WriteSmoothed(file("source.vdb"), file("bad.vdb"),
                            vdb::SmoothingOperation::Gaussian, {}, {}, bounded);
    FAIL() << "workspace must be bounded separately from the loaded source";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("workspace"), std::string::npos);
  }
  auto remoteLeaves = openvdb::FloatGrid::create(0.6f);
  remoteLeaves->setTransform(openvdb::math::Transform::createLinearTransform(0.2));
  remoteLeaves->setGridClass(openvdb::GRID_LEVEL_SET);
  remoteLeaves->setName("surface");
  remoteLeaves->tree().setValue(openvdb::Coord(0), 0.0f);
  for (int leaf = 0; leaf < 1500; ++leaf)
    remoteLeaves->tree().touchLeaf(openvdb::Coord(128 + leaf * 8, 0, 0));
  writeProcessingFixture(file("remote-background-leaves.vdb"), remoteLeaves);
  bounded = options;
  bounded.maxVoxelCount = 2'000'000;
  bounded.maxInputBytes = remoteLeaves->memUsage() + 6ull * 1024 * 1024;
  try {
    (void)vdb::WriteOffset(file("remote-background-leaves.vdb"), file("bad.vdb"), 0.1, {}, bounded);
    FAIL() << "remote inactive leaves also require filter auxiliary buffers";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("workspace"), std::string::npos);
  }
  auto compact = openvdb::FloatGrid::create(0.6f);
  compact->setTransform(openvdb::math::Transform::createLinearTransform(0.2));
  compact->setGridClass(openvdb::GRID_LEVEL_SET);
  compact->setName("surface");
  compact->tree().fill(openvdb::CoordBBox(openvdb::Coord(0), openvdb::Coord(7)), 0.0f, true);
  writeProcessingFixture(file("compact.vdb"), compact);
  bounded = options;
  bounded.maxVoxelCount = 512;
  try {
    (void)vdb::WriteOffset(file("compact.vdb"), file("bad.vdb"), 0.1, {}, bounded);
    FAIL() << "the first tracked dilation would exceed the input's exact leaf budget";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("expansion"), std::string::npos);
  }
  compact->tree().clear();
  compact->tree().setValue(openvdb::Coord(std::numeric_limits<int32_t>::max() - 2, 0, 0), 0.0f);
  writeProcessingFixture(file("edge.vdb"), compact);
  EXPECT_THROW((void)vdb::WriteOffset(file("edge.vdb"), file("bad.vdb"), 0.1),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbProcessing, ExplicitDescriptorSelectsFirstDuplicateGrid) {
  openvdb::initialize();
  auto first = openvdb::FloatGrid::create(0.0f);
  first->setName("duplicate");
  first->tree().setValue(openvdb::Coord(0), 1.0f);
  auto second = first->deepCopy();
  second->tree().setValue(openvdb::Coord(0), 2.0f);
  openvdb::io::File output(file("duplicate.vdb").string());
  output.write(openvdb::GridPtrVec{first, second});
  output.close();
  EXPECT_THROW((void)vdb::SampleScalar(file("duplicate.vdb"), {0,0,0}, "duplicate"),
               std::invalid_argument);
  openvdb::io::File source(file("duplicate.vdb").string());
  source.open(false);
  std::vector<std::string> descriptorNames;
  for (auto it = source.beginName(); it != source.endName(); ++it)
    descriptorNames.push_back(it.gridName());
  source.close();
  ASSERT_EQ(descriptorNames.size(), 2u);
  EXPECT_DOUBLE_EQ(vdb::SampleScalar(file("duplicate.vdb"), {0,0,0},
                                     "descriptor:" + descriptorNames[0]), 1.0);
  EXPECT_DOUBLE_EQ(vdb::SampleScalar(file("duplicate.vdb"), {0,0,0},
                                     "descriptor:" + descriptorNames[1]), 2.0);
}

TEST_F(OpenVdbProcessing, RejectsNonFiniteInactiveSdfValuesBeforeProcessing) {
  openvdb::initialize();
  auto grid = openvdb::FloatGrid::create(0.6f);
  grid->setName("surface");
  grid->setGridClass(openvdb::GRID_LEVEL_SET);
  grid->setTransform(openvdb::math::Transform::createLinearTransform(0.2));
  grid->tree().setValue(openvdb::Coord(4, 0, 0), 0.0f);
  grid->tree().setValueOff(openvdb::Coord(0, 0, 0),
                          std::numeric_limits<float>::quiet_NaN());
  openvdb::io::File output(file("invalid.vdb").string());
  output.write(openvdb::GridPtrVec{grid});
  output.close();
  (void)vdb::WriteSphere(file("sphere.vdb"), {}, options);
  EXPECT_THROW((void)vdb::SampleScalar(file("invalid.vdb"), {0, 0, 0}),
               std::invalid_argument);
  EXPECT_THROW((void)vdb::WriteFog(file("invalid.vdb"), file("bad-fog.vdb")),
               std::invalid_argument);
  EXPECT_THROW((void)vdb::WriteCsg(file("sphere.vdb"), file("invalid.vdb"),
                                 file("bad-csg.vdb"), vdb::CsgOperation::Union),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("bad-fog.vdb")));
  EXPECT_FALSE(std::filesystem::exists(file("bad-csg.vdb")));
}

TEST_F(OpenVdbProcessing, RejectsInactiveStoredTopologyDespiteSmallActiveCount) {
  openvdb::initialize();
  auto grid = openvdb::FloatGrid::create(0.6f);
  grid->setName("surface");
  grid->setGridClass(openvdb::GRID_LEVEL_SET);
  grid->tree().setValue(openvdb::Coord(0), 0.0f);
  for (int leaf = 0; leaf < 300; ++leaf)
    grid->tree().setValueOff(openvdb::Coord(128 + leaf * 8, 0, 0), -0.6f);
  openvdb::io::File output(file("inactive.vdb").string());
  output.write(openvdb::GridPtrVec{grid});
  output.close();
  options.maxVoxelCount = 100'000;
  ASSERT_LT(grid->activeVoxelCount(), options.maxVoxelCount);
  EXPECT_THROW((void)vdb::SampleScalar(file("inactive.vdb"), {0,0,0}, {}, options),
               std::invalid_argument);
  EXPECT_THROW((void)vdb::WriteFog(file("inactive.vdb"), file("bad.vdb"), {}, options),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(file("bad.vdb")));
}

TEST_F(OpenVdbProcessing, BoundsInactiveSdfTilesBeforeFogActivatesTheirInterior) {
  openvdb::initialize();
  auto grid = openvdb::FloatGrid::create(0.6f);
  grid->setName("surface");
  grid->setGridClass(openvdb::GRID_LEVEL_SET);
  grid->tree().setValue(openvdb::Coord(0), 0.0f);
  grid->tree().addTile(3, openvdb::Coord(4096, 0, 0), -0.6f, false);
  openvdb::io::File output(file("inactive-tile.vdb").string());
  output.write(openvdb::GridPtrVec{grid});
  output.close();
  ASSERT_EQ(grid->activeVoxelCount(), 1u);
  try {
    (void)vdb::WriteFog(file("inactive-tile.vdb"), file("huge-fog.vdb"), {}, options);
    FAIL();
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("bounds"), std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(file("huge-fog.vdb")));
}

TEST_F(OpenVdbProcessing, ScalarSamplingPreservesDoublePrecisionAndMixedGridSelection) {
  openvdb::initialize();
  auto precise = openvdb::DoubleGrid::create(1.2345678901234567); precise->setName("precise");
  precise->getAccessor().setValue(openvdb::Coord(2,0,0), 3.141592653589793);
  auto single = openvdb::FloatGrid::create(7); single->setName("single");
  const auto write = [&](const char* name, const openvdb::GridPtrVec& grids) {
    openvdb::io::File output(file(name).string()); output.write(grids); output.close();
  };
  write("double.vdb", {precise});
  EXPECT_DOUBLE_EQ(vdb::SampleScalar(file("double.vdb"), {2,0,0}), 3.141592653589793);
  EXPECT_DOUBLE_EQ(vdb::SampleScalar(file("double.vdb"), {20,0,0}), precise->background());
  auto queryOnly = options;
  queryOnly.voxelSize = std::numeric_limits<double>::quiet_NaN();
  queryOnly.halfWidth = 0; queryOnly.gridName.clear(); queryOnly.maxTriangles = 0;
  EXPECT_DOUBLE_EQ(vdb::SampleScalar(file("double.vdb"), {2,0,0}, {}, queryOnly), 3.141592653589793);
  write("mixed.vdb", {precise,single});
  EXPECT_DOUBLE_EQ(vdb::SampleScalar(file("mixed.vdb"), {2,0,0}, "precise"), 3.141592653589793);
  EXPECT_DOUBLE_EQ(vdb::SampleScalar(file("mixed.vdb"), {2,0,0}, "single"), 7);
  single->setName("precise"); write("duplicate.vdb", {precise,single});
  EXPECT_THROW((void)vdb::SampleScalar(file("duplicate.vdb"), {0,0,0}, "precise"), std::invalid_argument);
  openvdb::io::File duplicate(file("duplicate.vdb").string()); duplicate.open(false);
  const auto descriptor = duplicate.beginName().gridName();
  const auto selected = duplicate.readGrid(descriptor); duplicate.close();
  const double expected = selected->isType<openvdb::DoubleGrid>() ? precise->background() : 7;
  EXPECT_DOUBLE_EQ(vdb::SampleScalar(file("duplicate.vdb"), {20,0,0}, "descriptor:" + descriptor), expected);
}

TEST_F(OpenVdbProcessing, ScalarSamplingCentersAffineInverseAndChecksDoubleInputBudgetsAndValues) {
  openvdb::initialize();
  auto grid = openvdb::DoubleGrid::create(0); grid->setName("field");
  openvdb::math::Mat4d matrix; matrix.setIdentity();
  matrix[0][0]=.1; matrix[0][1]=.1; matrix[1][1]=.1;
  matrix[3][0]=1e15; matrix[3][1]=-2e15;
  grid->setTransform(openvdb::math::Transform::createLinearTransform(matrix));
  grid->getAccessor().setValue(openvdb::Coord(4,1,0), 17.123456789012345);
  const auto write = [&](const char* name) { openvdb::io::File output(file(name).string()); output.write(openvdb::GridPtrVec{grid}); output.close(); };
  write("translated-double.vdb");
  EXPECT_DOUBLE_EQ(vdb::SampleScalar(file("translated-double.vdb"), {1e15+.375,-2e15+.5,0}), 17.123456789012345);
  auto bounded=options; bounded.maxVoxelCount=1;
  // One stored leaf is permitted by the rounded storage cap, but a second
  // active value must still fail the actual active-count check.
  grid->getAccessor().setValue(openvdb::Coord(5,1,0), 5); write("bounded-double.vdb");
  EXPECT_THROW((void)vdb::SampleScalar(file("bounded-double.vdb"), {1e15+.375,-2e15+.5,0}, {}, bounded), std::invalid_argument);
  bounded=options; bounded.maxInputBytes=1;
  EXPECT_THROW((void)vdb::SampleScalar(file("translated-double.vdb"), {0,0,0}, {}, bounded), std::invalid_argument);
  grid->getAccessor().setValueOff(openvdb::Coord(6,1,0), std::numeric_limits<double>::infinity()); write("nonfinite-double.vdb");
  EXPECT_THROW((void)vdb::SampleScalar(file("nonfinite-double.vdb"), {1e15+.375,-2e15+.5,0}), std::invalid_argument);
}
#endif
}  // namespace
