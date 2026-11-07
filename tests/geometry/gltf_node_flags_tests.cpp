#include <Container/geometry/GltfNodeFlags.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <climits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace container::geometry::gltf;
using Json = nlohmann::json;

constexpr std::array kExtensions{
    "KHR_node_visibility", "KHR_node_selectability", "KHR_node_hoverability"};
constexpr std::array kProperties{"visible", "selectable", "hoverable"};
constexpr std::array kKinds{
    GltfNodeFlag::Visibility, GltfNodeFlag::Selectability, GltfNodeFlag::Hoverability};

Json validJson() {
  Json source{{"asset", {{"version", "2.0"}}}, {"extensionsUsed", kExtensions},
              {"nodes", Json::array({Json::object(), Json::object(), Json::object(), Json::object()})}};
  for (size_t flag = 0; flag < kExtensions.size(); ++flag) {
    source["nodes"][1]["extensions"][kExtensions[flag]] = Json::object();
    source["nodes"][2]["extensions"][kExtensions[flag]][kProperties[flag]] = true;
    source["nodes"][3]["extensions"][kExtensions[flag]][kProperties[flag]] = false;
  }
  return source;
}

tinygltf::Model validModel() {
  tinygltf::Model source;
  source.nodes.resize(4);
  source.extensionsUsed.assign(kExtensions.begin(), kExtensions.end());
  for (size_t flag = 0; flag < kExtensions.size(); ++flag) {
    source.nodes[1].extensions[kExtensions[flag]] = tinygltf::Value(tinygltf::Value::Object{});
    source.nodes[2].extensions[kExtensions[flag]] = tinygltf::Value(tinygltf::Value::Object{
        {kProperties[flag], tinygltf::Value(true)}});
    source.nodes[3].extensions[kExtensions[flag]] = tinygltf::Value(tinygltf::Value::Object{
        {kProperties[flag], tinygltf::Value(false)}});
  }
  return source;
}

template <typename Operation>
void expectError(Operation&& operation, std::string_view expected) {
  try {
    operation();
    FAIL() << "Expected a node flag metadata error";
  } catch (const std::runtime_error& error) {
    const std::string message(error.what());
    EXPECT_NE(message.find("glTF node flags"), std::string::npos) << message;
    EXPECT_NE(message.find(expected), std::string::npos) << message;
  }
}

TEST(GltfNodeFlags, DefaultsApplyToAbsentAndEmptyExtensionProperties) {
  const auto source = validModel();
  const auto loaded = ParseGltfNodeFlags(source);
  ASSERT_EQ(loaded.nodes.size(), 4u);
  EXPECT_EQ(loaded.nodes[0], GltfNodeFlags{});
  for (const auto kind : kKinds) {
    EXPECT_FALSE(loaded.nodes[0].hasExtension(kind));
    EXPECT_FALSE(loaded.nodes[0].hasProperty(kind));
    EXPECT_TRUE(loaded.nodes[1].hasExtension(kind));
    EXPECT_FALSE(loaded.nodes[1].hasProperty(kind));
    EXPECT_TRUE(loaded.nodes[2].hasExtension(kind));
    EXPECT_TRUE(loaded.nodes[2].hasProperty(kind));
    EXPECT_TRUE(loaded.nodes[3].hasProperty(kind));
  }
  for (const auto node : {1u, 2u}) {
    EXPECT_TRUE(loaded.nodes[node].visible);
    EXPECT_TRUE(loaded.nodes[node].selectable);
    EXPECT_TRUE(loaded.nodes[node].hoverable);
  }
  EXPECT_FALSE(loaded.nodes[3].visible);
  EXPECT_FALSE(loaded.nodes[3].selectable);
  EXPECT_FALSE(loaded.nodes[3].hoverable);
  EXPECT_EQ(loaded.summary.nodes, 4u);
  EXPECT_EQ(loaded.summary.authoredNodes, 3u);
  EXPECT_EQ(loaded.summary.authoredExtensions, 9u);
  EXPECT_EQ(loaded.summary.retainedBytes, 4 * sizeof(GltfNodeFlags));
  EXPECT_EQ(ValidateGltfNodeFlagsJson(validJson()), loaded.summary);
  EXPECT_TRUE(ParseGltfNodeFlags(tinygltf::Model{}).nodes.empty());
  EXPECT_EQ(ValidateGltfNodeFlagsJson(Json::object()).nodes, 0u);
}

TEST(GltfNodeFlags, LocalFlagsStayIndependentAndRetainInactiveSourceNodeIdentity) {
  auto source = validModel();
  source.nodes[0].children = {2};
  source.nodes[0].extensions[kExtensions[0]] = tinygltf::Value(tinygltf::Value::Object{
      {"visible", tinygltf::Value(false)}});
  source.nodes[0].extensions[kExtensions[2]] = tinygltf::Value(tinygltf::Value::Object{
      {"hoverable", tinygltf::Value(false)}});
  source.scenes.resize(2);
  source.scenes[0].nodes = {1};
  source.scenes[1].nodes = {0, 3};
  source.defaultScene = 0;
  const auto loaded = ParseGltfNodeFlags(source);
  ASSERT_EQ(loaded.nodes.size(), source.nodes.size());
  EXPECT_FALSE(loaded.nodes[0].visible);
  EXPECT_TRUE(loaded.nodes[0].selectable);
  EXPECT_FALSE(loaded.nodes[0].hoverable);
  // A later runtime inheritance pass applies the parent opt-outs; parsing
  // must preserve the child's explicitly authored true values for export/edit.
  EXPECT_TRUE(loaded.nodes[2].visible);
  EXPECT_TRUE(loaded.nodes[2].selectable);
  EXPECT_TRUE(loaded.nodes[2].hoverable);
  EXPECT_FALSE(loaded.nodes[3].visible);
  EXPECT_EQ(source.defaultScene, 0);
  EXPECT_EQ(source.nodes[0].children, std::vector<int>{2});
  EXPECT_TRUE(source.nodes[2].extensions.at(kExtensions[0]).Get("visible").Get<bool>());
}

TEST(GltfNodeFlags, JsonRejectsCoercedBooleansAndInvalidExtensionShapes) {
  const std::vector<Json> badProperties{
      nullptr, 0, 1, -1, 0.0, 1.0, "true", "false", Json::array(), Json::object()};
  const std::vector<Json> badExtensions{nullptr, 0, 1.0, "bad", true, Json::array()};
  for (size_t flag = 0; flag < kExtensions.size(); ++flag) {
    for (const auto& bad : badProperties) {
      SCOPED_TRACE(std::string(kExtensions[flag]) + " " + bad.dump());
      auto source = validJson();
      source["nodes"][2]["extensions"][kExtensions[flag]][kProperties[flag]] = bad;
      expectError([&] { (void)ValidateGltfNodeFlagsJson(source); },
                  "node[2].extensions." + std::string(kExtensions[flag]) + "." + kProperties[flag]);
    }
    for (const auto& bad : badExtensions) {
      auto source = validJson();
      source["nodes"][2]["extensions"][kExtensions[flag]] = bad;
      expectError([&] { (void)ValidateGltfNodeFlagsJson(source); }, "extension must be an object");
    }
  }
}

TEST(GltfNodeFlags, ModelRejectsCoercedBooleansAndInvalidExtensionShapes) {
  const std::vector<tinygltf::Value> badProperties{
      tinygltf::Value{}, tinygltf::Value(0), tinygltf::Value(1), tinygltf::Value(0.0),
      tinygltf::Value(std::string("true")), tinygltf::Value(tinygltf::Value::Array{}),
      tinygltf::Value(tinygltf::Value::Object{})};
  for (size_t flag = 0; flag < kExtensions.size(); ++flag) {
    for (const auto& bad : badProperties) {
      auto source = validModel();
      source.nodes[2].extensions[kExtensions[flag]] = tinygltf::Value(tinygltf::Value::Object{
          {kProperties[flag], bad}});
      expectError([&] { (void)ParseGltfNodeFlags(source); },
                  "node[2].extensions." + std::string(kExtensions[flag]) + "." + kProperties[flag]);
    }
    auto source = validModel();
    source.nodes[2].extensions[kExtensions[flag]] = tinygltf::Value(false);
    expectError([&] { (void)ParseGltfNodeFlags(source); }, "extension must be an object");
  }
}

TEST(GltfNodeFlags, AuthoredExtensionsRequireDeclarationsInBothRepresentations) {
  auto json = validJson();
  json.erase("extensionsUsed");
  expectError([&] { (void)ValidateGltfNodeFlagsJson(json); }, "extensionsUsed");
  auto model = validModel();
  model.extensionsUsed.clear();
  expectError([&] { (void)ParseGltfNodeFlags(model); }, "extensionsUsed");
  json = validJson();
  json["extensionsUsed"] = Json::object();
  expectError([&] { (void)ValidateGltfNodeFlagsJson(json); }, "must be an array");
  json = validJson();
  json["extensionsUsed"].push_back(3);
  expectError([&] { (void)ValidateGltfNodeFlagsJson(json); }, "names must be strings");
}

TEST(GltfNodeFlags, JsonRejectsInvalidNodeAndExtensionContainers) {
  for (const auto& bad : std::vector<Json>{nullptr, 1, "bad", true, Json::array()}) {
    expectError([&] { (void)ValidateGltfNodeFlagsJson(bad); }, "source must be an object");
    auto source = validJson();
    source["nodes"][0] = bad;
    expectError([&] { (void)ValidateGltfNodeFlagsJson(source); }, "node[0]: must be an object");
    source = validJson();
    source["nodes"][0]["extensions"] = bad;
    expectError([&] { (void)ValidateGltfNodeFlagsJson(source); }, "extensions: must be an object");
  }
  auto source = validJson();
  source["nodes"] = Json::object();
  expectError([&] { (void)ValidateGltfNodeFlagsJson(source); }, "nodes: must be an array");
}

TEST(GltfNodeFlags, LimitsRejectCountsAndStorageBeforeInspectingNodePayloads) {
  const auto source = validModel();
  const auto json = validJson();
  GltfNodeFlagLimits limits;
  limits.maxNodes = 3;
  expectError([&] { (void)ParseGltfNodeFlags(source, limits); }, "node count");
  expectError([&] { (void)ValidateGltfNodeFlagsJson(json, limits); }, "node count");
  limits.maxNodes = 4;
  limits.maxRetainedBytes = 4 * sizeof(GltfNodeFlags) - 1;
  expectError([&] { (void)ParseGltfNodeFlags(source, limits); }, "retained-byte budget");
  expectError([&] { (void)ValidateGltfNodeFlagsJson(json, limits); }, "retained-byte budget");
  limits.maxRetainedBytes += 1;
  EXPECT_EQ(ParseGltfNodeFlags(source, limits).nodes.size(), 4u);
  EXPECT_EQ(ValidateGltfNodeFlagsJson(json, limits).retainedBytes, limits.maxRetainedBytes);
  for (const auto invalid : std::array<GltfNodeFlagLimits, 4>{
           GltfNodeFlagLimits{0, 100}, GltfNodeFlagLimits{uint64_t(INT_MAX) + 1, 100},
           GltfNodeFlagLimits{10, 0}, GltfNodeFlagLimits{10, UINT64_MAX}}) {
    expectError([&] { (void)ParseGltfNodeFlags(source, invalid); }, "limits");
    expectError([&] { (void)ValidateGltfNodeFlagsJson(json, invalid); }, "limits");
  }
}

TEST(GltfNodeFlags, UnknownPropertiesAndMetadataStayOpaqueAndUnmodified) {
  auto source = validJson();
  source["nodes"][1]["extensions"][kExtensions[0]]["extras"] = {{"custom", {1, 2, 3}}};
  source["nodes"][1]["extensions"][kExtensions[0]]["extensions"] = {{"VENDOR_flag", Json::object()}};
  source["nodes"][1]["extensions"][kExtensions[0]]["futureProperty"] = "opaque";
  source["nodes"][1]["extensions"]["VENDOR_other"] = {{"value", 9}};
  const auto original = source;
  const auto summary = ValidateGltfNodeFlagsJson(source);
  EXPECT_EQ(summary.authoredExtensions, 9u);
  EXPECT_EQ(source, original);
  auto model = validModel();
  model.nodes[1].extensions[kExtensions[0]] = tinygltf::Value(tinygltf::Value::Object{
      {"extras", tinygltf::Value(std::string("metadata"))},
      {"futureProperty", tinygltf::Value(42)}});
  const auto flags = ParseGltfNodeFlags(model);
  EXPECT_TRUE(flags.nodes[1].visible);
  EXPECT_FALSE(flags.nodes[1].hasProperty(GltfNodeFlag::Visibility));
  EXPECT_EQ(model.nodes[1].extensions.at(kExtensions[0]).Get("extras").Get<std::string>(), "metadata");
}

} // namespace
