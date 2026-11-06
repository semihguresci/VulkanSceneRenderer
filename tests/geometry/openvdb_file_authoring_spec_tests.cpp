#include "Container/geometry/OpenVdbFileAuthoringSpec.h"

#include <atomic>
#include <bit>
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/File.h>
#include <openvdb/openvdb.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;
TEST(OpenVdbFileAuthoringSpecAvailability,
     DisabledPrecedesPathsOptionsAndParsing) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::FileAuthoringSpecOptions options;
  options.maxSpecBytes = 0;
  try {
    (void)vdb::WriteAuthoredVdbSpec("missing/spec.json", {}, options,
                                    {.maxInputBytes = 0});
    FAIL();
  } catch (const std::runtime_error &error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
  }
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbFileAuthoringSpec : public ::testing::Test {
protected:
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{0};
    directory =
        std::filesystem::temp_directory_path() /
        ("container-author-spec-test-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    writeGrid("input.vdb", {field("density")});
  }
  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  auto path(const std::string &name) const { return directory / name; }
  static auto field(const std::string &name, float value = 7.25f) {
    auto grid = openvdb::FloatGrid::create(0);
    grid->setName(name);
    grid->tree().setValue({0, 0, 0}, value);
    grid->insertMeta("artist", openvdb::StringMetadata("original"));
    return grid;
  }
  void writeGrid(const std::string &name, const openvdb::GridPtrVec &grids,
                 const openvdb::MetaMap &metadata = {}) {
    openvdb::io::File file(path(name).string());
    file.setCompression(openvdb::io::COMPRESS_ZIP);
    file.write(grids, metadata);
  }
  auto spec(const std::string &text, const std::string &name = "spec.json") {
    std::ofstream file(path(name), std::ios::binary);
    file.write(text.data(), std::streamsize(text.size()));
    file.close();
    return path(name);
  }
  static std::string bytes(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
  }
  auto read(const std::string &file, const std::string &name = "density") {
    openvdb::io::File input(path(file).string());
    input.open(false);
    auto grid = openvdb::gridPtrCast<openvdb::FloatGrid>(input.readGrid(name));
    input.close();
    return grid;
  }
  void failure(const std::string &text,
               const vdb::FileAuthoringSpecOptions &options = {},
               const vdb::ProcessingOptions &processing = {}) {
    const auto specification = spec(text);
    const auto output = path("failure-" + std::to_string(++serial) + ".vdb");
    EXPECT_THROW((void)vdb::WriteAuthoredVdbSpec(specification, output, options,
                                                 processing),
                 std::exception)
        << text;
    EXPECT_FALSE(std::filesystem::exists(output));
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(directory))
      EXPECT_FALSE(entry.path().filename().string().starts_with(
          ".container-author-vdb-"));
  }
  static std::string document(const std::string &metadata) {
    return R"({"version":1,"inputs":[{"path":"input.vdb"}],"metadata":)" +
           metadata + "}";
  }
  std::filesystem::path directory;
  size_t serial{0};
};

TEST_F(OpenVdbFileAuthoringSpec,
       RelativeUtf8PathsOriginalDirectoryAndExactInt64TypedEdits) {
  std::filesystem::create_directory(path("specs"));
  std::filesystem::create_directory(path("specs/data"));
  openvdb::MetaMap first, second;
  first.insertMeta("source", openvdb::Int32Metadata(1));
  second.insertMeta("source", openvdb::Int32Metadata(2));
  writeGrid("first.vdb", {field("density"), field("remove")}, first);
  std::filesystem::rename(path("first.vdb"),
                          path("specs/data") /
                              std::filesystem::path(u8"\u03b1.vdb"));
  writeGrid("other.vdb", {field("other", 9)}, second);
  const std::string text = R"({
    "compression":"none","metadataConflict":"preferLast","version":1,
    "inputs":[{"edits":[{"metadata":[
      {"value":9223372036854775807,"type":"int64","operation":"set","name":"maximum"},
      {"name":"minimum","operation":"set","type":"int64","value":-9223372036854775808},
      {"name":"artist","operation":"remove"},
      {"name":"v32","operation":"set","type":"vec3s","value":[1,2.5,-3]},
      {"name":"v64","operation":"set","type":"vec3d","value":[-1e200,1e-200,0]},
      {"name":"bool","operation":"set","type":"bool","value":true},
      {"name":"float","operation":"set","type":"float","value":1e-45},
      {"name":"text","operation":"set","type":"string","value":"a\u0000b"}],
      "newName":"fog","selector":"density"}],"path":"data/\u03b1.vdb",
      "select":["density","remove"],"remove":["remove"]},{"path":"../other.vdb"}],
    "metadata":[{"name":"count","operation":"set","type":"int32","value":-2147483648},
      {"name":"time","operation":"set","type":"double","value":0.1234567891234567}]
  })";
  const auto specification = spec(text, "specs/spec.json");
  const auto original = bytes(specification);
  const auto result =
      vdb::WriteAuthoredVdbSpec(specification, path("authored.vdb"));
  EXPECT_EQ(result.outputGridCount, 2u);
  EXPECT_EQ(result.removedGridCount, 1u);
  EXPECT_TRUE(result.diagnostics.empty());
  auto grid = read("authored.vdb", "fog");
  EXPECT_FLOAT_EQ(grid->tree().getValue({0, 0, 0}), 7.25f);
  EXPECT_EQ(grid->metaValue<int64_t>("maximum"), INT64_MAX);
  EXPECT_EQ(grid->metaValue<int64_t>("minimum"), INT64_MIN);
  EXPECT_FALSE((*grid)["artist"]);
  EXPECT_EQ(grid->metaValue<openvdb::Vec3f>("v32"),
            openvdb::Vec3f(1, 2.5f, -3));
  EXPECT_EQ(grid->metaValue<openvdb::Vec3d>("v64"),
            openvdb::Vec3d(-1e200, 1e-200, 0));
  EXPECT_EQ(std::bit_cast<uint32_t>(grid->metaValue<float>("float")), 1u);
  EXPECT_EQ(grid->metaValue<std::string>("text"), std::string("a\0b", 3));
  EXPECT_TRUE(grid->metaValue<bool>("bool"));
  openvdb::io::File file(path("authored.vdb").string());
  file.open(false);
  EXPECT_EQ(file.getMetadata()->metaValue<int32_t>("source"), 2);
  EXPECT_EQ(file.getMetadata()->metaValue<int32_t>("count"), INT32_MIN);
  EXPECT_DOUBLE_EQ(file.getMetadata()->metaValue<double>("time"),
                   .1234567891234567);
  EXPECT_EQ(bytes(specification), original);
}

TEST_F(OpenVdbFileAuthoringSpec,
       LosslessDefaultsAndExplicitLossFlagsReachNativeWriter) {
  auto grid = field("density", 1.234567f);
  grid->tree().touchLeaf({16, 0, 0})->setValueOff({16, 0, 0}, -0.0f);
  writeGrid("input.vdb", {grid});
  const auto before = bytes(path("input.vdb"));
  const auto defaultSpec =
      spec(R"({"version":1,"inputs":[{"path":"input.vdb"}]})");
  const auto result = vdb::WriteAuthoredVdbSpec(defaultSpec, path("copy.vdb"));
  EXPECT_TRUE(result.diagnostics.empty());
  EXPECT_TRUE(std::signbit(read("copy.vdb")->tree().getValue({16, 0, 0})));
  const auto lossSpec = spec(
      R"({"version":1,"inputs":[{"path":"input.vdb"}],"activeMaskCompression":true,"saveFloatAsHalf":true})",
      "loss.json");
  EXPECT_EQ(
      vdb::WriteAuthoredVdbSpec(lossSpec, path("loss.vdb")).diagnostics.size(),
      2u);
  EXPECT_NE(read("loss.vdb")->tree().getValue({0, 0, 0}), 1.234567f);
  EXPECT_EQ(bytes(path("input.vdb")), before);
}

TEST_F(OpenVdbFileAuthoringSpec,
       DuplicateUnknownMissingVersionAndWrongShapesFailStrictly) {
  for (
      const auto &text :
      {"[]",
       "{}",
       R"({"version":2,"inputs":[{"path":"input.vdb"}]})",
       R"({"version":1.0,"inputs":[{"path":"input.vdb"}]})",
       R"({"version":1,"version":1,"inputs":[{"path":"input.vdb"}]})",
       R"({"version":1,"inputs":[]})",
       R"({"version":1,"inputs":[{}]})",
       R"({"version":1,"inputs":[{"path":"input.vdb","path":"input.vdb"}]})",
       R"({"version":1,"inputs":[{"path":"input.vdb","extra":true}]})",
       R"({"version":1,"inputs":[{"path":"input.vdb"}],"maxInputBytes":999999999})",
       R"({"version":1,"inputs":[{"path":"input.vdb","select":"density"}]})",
       R"({"version":1,"inputs":[{"path":"input.vdb","select":[null]}]})",
       R"({"version":1,"inputs":[{"path":"input.vdb","edits":[{"newName":"x"}]}]})",
       R"({"version":1,"inputs":[{"path":"input.vdb","edits":[{"selector":"density","selector":"density"}]}]})",
       R"({"version":1,"inputs":[{"path":"input.vdb"}],"compression":"gzip"})",
       R"({"version":1,"inputs":[{"path":"input.vdb"}],"metadataConflict":"replace"})",
       R"({"version":1,"inputs":[{"path":"input.vdb"}],"saveFloatAsHalf":1})",
       R"({"version":1,"inputs":[{"path":"input.vdb"}],"activeMaskCompression":"true"})",
       R"({"version":1,"inputs":[{"path":"input.vdb"}]} trailing)",
       R"({"version":1,"inputs":[{"path":"input.vdb"}],})"})
    failure(text);
}

TEST_F(OpenVdbFileAuthoringSpec,
       ExactIntegerRangeAndTypedMetadataAreNeverCoercedSilently) {
  for (
      const auto &entry :
      {R"({"name":"x","operation":"set","type":"int64","value":9223372036854775808})",
       R"({"name":"x","operation":"set","type":"int64","value":-9223372036854775809})",
       R"({"name":"x","operation":"set","type":"int64","value":1.0})",
       R"({"name":"x","operation":"set","type":"int64","value":"1"})",
       R"({"name":"x","operation":"set","type":"int32","value":2147483648})",
       R"({"name":"x","operation":"set","type":"float","value":1e-99})",
       R"({"name":"x","operation":"set","type":"float","value":1e99})",
       R"({"name":"x","operation":"set","type":"double","value":1e-999})",
       R"({"name":"x","operation":"set","type":"double","value":1e999})",
       R"({"name":"x","operation":"set","type":"bool","value":1})",
       R"({"name":"x","operation":"set","type":"string","value":false})",
       R"({"name":"x","operation":"set","type":"vec3s","value":[1,2]})",
       R"({"name":"x","operation":"set","type":"vec3d","value":[1,2,3,4]})",
       R"({"name":"x","operation":"set","type":"vec3s","value":[1,false,3]})",
       R"({"name":"x","operation":"set","type":"vec3f","value":[1,2,3]})",
       R"({"name":"x","operation":"set","type":"double"})",
       R"({"name":"x","operation":"set","type":"double","type":"float","value":1})",
       R"({"name":"x","operation":"remove","value":1})",
       R"({"name":"x","operation":"remove","type":"int32"})",
       R"({"name":"x","operation":"erase"})"})
    failure(document("[" + std::string(entry) + "]"));
  failure(document(
      "[{\"name\":\"x\",\"operation\":\"set\",\"type\":\"double\",\"value\":" +
      std::string(129, '1') + "}]"));
}

TEST_F(OpenVdbFileAuthoringSpec,
       IntegerToFloatUsesOneCorrectlyRoundedConversion) {
  // Each integer lies one unit above a binary32 midpoint. A Double
  // intermediary erases that unit and incorrectly chooses the even low Float.
  const auto specification = spec(document(R"([
    {"name":"scalar","operation":"set","type":"float","value":1152921573326323713},
    {"name":"vector","operation":"set","type":"vec3s","value":[1152921573326323713,-1152921573326323713,9223372586610589697]}
  ])"));
  (void)vdb::WriteAuthoredVdbSpec(specification, path("rounded.vdb"));
  openvdb::io::File file(path("rounded.vdb").string());
  file.open(false);
  const auto metadata = file.getMetadata();
  EXPECT_EQ(std::bit_cast<uint32_t>(metadata->metaValue<float>("scalar")),
            0x5d800001u);
  const auto vector = metadata->metaValue<openvdb::Vec3f>("vector");
  EXPECT_EQ(std::bit_cast<uint32_t>(vector.x()), 0x5d800001u);
  EXPECT_EQ(std::bit_cast<uint32_t>(vector.y()), 0xdd800001u);
  EXPECT_EQ(std::bit_cast<uint32_t>(vector.z()), 0x5f000001u);
}

TEST_F(OpenVdbFileAuthoringSpec,
       AuthoritativeLimitsBoundParserAndNativeWorkTogether) {
  const std::string text = R"({"version":1,"inputs":[{"path":"input.vdb"}]})";
  vdb::FileAuthoringSpecOptions options;
  options.maxSpecBytes = 8;
  failure(text, options);
  options = {};
  options.maxDepth = 2;
  failure(text, options);
  options = {};
  options.maxMetadataBytes = 32;
  failure(text, options);
  options = {};
  options.maxEdits = 1;
  failure(
      R"({"version":1,"inputs":[{"path":"input.vdb","select":["density","density"]}]})",
      options);
  writeGrid("two.vdb", {field("a"), field("b")});
  options = {};
  options.maxGrids = 1;
  failure(R"({"version":1,"inputs":[{"path":"two.vdb","select":["a"]}]})",
          options);
  failure(text, {}, {.maxInputBytes = 1024});
  auto grid = field("density");
  vdb::ProcessingOptions processing;
  processing.maxInputBytes =
      4 * 1024 * 1024 + 4 * grid->memUsage() + 512 * 1024;
  vdb::FileAuthoringOptions native;
  native.inputs = {{path("input.vdb")}};
  EXPECT_NO_THROW(
      (void)vdb::WriteAuthoredVdb(path("native.vdb"), native, processing));
  // The parser and its still-live native model must reserve space in addition
  // to a native operation that fits on its own.
  failure(text, {}, processing);
}

TEST_F(OpenVdbFileAuthoringSpec,
       SemanticNativeFailuresAndExistingOutputPreserveAllInputs) {
  const auto before = bytes(path("input.vdb"));
  failure(
      R"({"version":1,"inputs":[{"path":"input.vdb","select":["missing"]}]})");
  failure(
      R"({"version":1,"inputs":[{"path":"input.vdb","edits":[{"selector":"density","metadata":[{"name":"class","operation":"set","type":"string","value":"fake"}]}]}]})");
  const auto specification =
      spec(R"({"version":1,"inputs":[{"path":"input.vdb"}]})");
  EXPECT_THROW(
      (void)vdb::WriteAuthoredVdbSpec(specification, path("input.vdb")),
      std::exception);
  EXPECT_EQ(bytes(path("input.vdb")), before);
  std::string invalidUtf8 = R"({"version":1,"inputs":[{"path":")";
  invalidUtf8.push_back(char(0xff));
  invalidUtf8 += R"("}]})";
  failure(invalidUtf8);
}
#endif
} // namespace
