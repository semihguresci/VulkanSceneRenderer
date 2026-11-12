#include <Container/geometry/GltfInteractivity.h>
#include <Container/geometry/GltfMerge.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <string_view>

namespace {
using namespace container::geometry::gltf;
using Json = nlohmann::json;

tinygltf::Value convert(const Json& input) {
  if (input.is_boolean()) return tinygltf::Value(input.get<bool>());
  if (input.is_number_integer()) return tinygltf::Value(input.get<int>());
  if (input.is_number()) return tinygltf::Value(input.get<double>());
  if (input.is_string()) return tinygltf::Value(input.get<std::string>());
  if (input.is_array()) {
    tinygltf::Value::Array values;
    for (const auto& item : input) values.push_back(convert(item));
    return tinygltf::Value(std::move(values));
  }
  if (input.is_object()) {
    tinygltf::Value::Object values;
    for (const auto& [key, item] : input.items()) values.emplace(key, convert(item));
    return tinygltf::Value(std::move(values));
  }
  return {};
}

tinygltf::Model asset(Json graph) {
  tinygltf::Model model;
  model.nodes.resize(2);
  model.materials.resize(1);
  model.extensions[kGltfInteractivityExtension] = convert(
      Json{{"graph", 0}, {"graphs", Json::array({std::move(graph)})}});
  return model;
}

void defineNodeFlags(tinygltf::Node& node) {
  node.extensions["KHR_node_visibility"] = convert(Json::parse(R"({"extras":{"retain":true},"future":9})"));
  node.extensions["KHR_node_selectability"] = convert(Json{{"selectable", true}});
  node.extensions["KHR_node_hoverability"] = convert(Json{{"hoverable", true}});
}

TEST(GltfInteractivity, BooleanPointersReadWriteAndRestoreAuthoredDefaults) {
  auto model = asset(Json::parse(R"json({
    "types":[{"signature":"bool"}],
    "declarations":[{"op":"event/onStart"},{"op":"pointer/get"},{"op":"flow/branch"},{"op":"pointer/set"}],
    "nodes":[
      {"declaration":0,"flows":{"out":{"node":2}}},
      {"declaration":1,"configuration":{"pointer":{"value":["/nodes/0/extensions/KHR_node_visibility/visible"]},"type":{"value":[0]}}},
      {"declaration":2,"values":{"condition":{"node":1}},"flows":{"true":{"node":3}}},
      {"declaration":3,"configuration":{"pointer":{"value":["/nodes/0/extensions/KHR_node_visibility/visible"]},"type":{"value":[0]}},
        "values":{"value":{"type":0,"value":[false]}},"flows":{"out":{"node":5}}},
      {"declaration":1,"configuration":{"pointer":{"value":["/nodes/0/extensions/KHR_node_visibility/visible"]},"type":{"value":[0]}}},
      {"declaration":3,"configuration":{"pointer":{"value":["/nodes/0/extensions/KHR_node_selectability/selectable"]},"type":{"value":[0]}},
        "values":{"value":{"node":4}},"flows":{"out":{"node":7}}},
      {"declaration":1,"configuration":{"pointer":{"value":["/nodes/0/extensions/KHR_node_selectability/selectable"]},"type":{"value":[0]}}},
      {"declaration":3,"configuration":{"pointer":{"value":["/nodes/0/extensions/KHR_node_hoverability/hoverable"]},"type":{"value":[0]}},
        "values":{"value":{"node":6}}}
    ]})json"));
  defineNodeFlags(model.nodes[0]);
  const auto authored = model.nodes[0].extensions;
  GltfInteractivity player(model);
  const auto dirty = player.advance(model, {}, 0);
  ASSERT_TRUE(player.diagnostic().empty()) << player.diagnostic();
  ASSERT_EQ(dirty.nodeFlags.size(), 1u);
  EXPECT_EQ(dirty.nodeFlags[0].node, 0u);
  const auto mask = uint8_t(GltfNodeFlag::Visibility) | uint8_t(GltfNodeFlag::Selectability) | uint8_t(GltfNodeFlag::Hoverability);
  EXPECT_EQ(dirty.nodeFlags[0].mask, mask);
  EXPECT_TRUE(dirty.nodes.empty());
  const auto& flags = model.nodes[0].extensions;
  EXPECT_FALSE(flags.at("KHR_node_visibility").Get("visible").Get<bool>());
  EXPECT_FALSE(flags.at("KHR_node_selectability").Get("selectable").Get<bool>());
  EXPECT_FALSE(flags.at("KHR_node_hoverability").Get("hoverable").Get<bool>());
  EXPECT_EQ(flags.at("KHR_node_visibility").Get("future").Get<int>(), 9);
  const auto restored = player.reset(model);
  ASSERT_EQ(restored.nodeFlags.size(), 1u);
  EXPECT_EQ(restored.nodeFlags[0].mask, mask);
  EXPECT_EQ(flags, authored);
  EXPECT_FALSE(flags.at("KHR_node_visibility").Has("visible"));
  EXPECT_FALSE(player.started());
}

TEST(GltfInteractivity, BooleanPointersUseEditedFlagsAndHistoryRestoresMetadata) {
  auto model = asset(Json::parse(R"json({
    "types":[{"signature":"bool"}],
    "declarations":[{"op":"event/onStart"},{"op":"pointer/get"},{"op":"flow/branch"},{"op":"pointer/set"}],
    "nodes":[
      {"declaration":0,"flows":{"out":{"node":2}}},
      {"declaration":1,"configuration":{"pointer":{"value":["/nodes/0/extensions/KHR_node_selectability/selectable"]},"type":{"value":[0]}}},
      {"declaration":2,"values":{"condition":{"node":1}},"flows":{"false":{"node":3}}},
      {"declaration":3,"configuration":{"pointer":{"value":["/nodes/0/extensions/KHR_node_visibility/visible"]},"type":{"value":[0]}},
        "values":{"value":{"type":0,"value":[true]}}}
    ]})json"));
  auto& node = model.nodes.front();
  defineNodeFlags(node);
  node.extensions.erase("KHR_node_hoverability");
  const auto archive = node;
  const auto edit = PrepareGltfNodeFlagEdit(node, archive, {false, false, false});
  ApplyGltfNodeFlagEdit(node, edit.after);
  EXPECT_FALSE(node.extensions.at("KHR_node_visibility").Get("visible").Get<bool>());
  EXPECT_FALSE(node.extensions.at("KHR_node_selectability").Get("selectable").Get<bool>());
  EXPECT_FALSE(node.extensions.contains("KHR_node_hoverability"));

  GltfInteractivity player(model);
  const auto dirty = player.advance(model, {}, 0);
  EXPECT_TRUE(player.diagnostic().empty()) << player.diagnostic();
  ASSERT_EQ(dirty.nodeFlags.size(), 1u);
  EXPECT_EQ(dirty.nodeFlags.front().node, 0u);
  EXPECT_EQ(dirty.nodeFlags.front().mask, uint8_t(GltfNodeFlag::Visibility));
  EXPECT_TRUE(node.extensions.at("KHR_node_visibility").Get("visible").Get<bool>());

  ApplyGltfNodeFlagEdit(node, edit.before);
  EXPECT_EQ(node.extensions, archive.extensions);
  ApplyGltfNodeFlagEdit(node, edit.after);
  const auto restore = PrepareGltfNodeFlagEdit(node, archive, {true, true, std::nullopt});
  ApplyGltfNodeFlagEdit(node, restore.after);
  EXPECT_EQ(node.extensions, archive.extensions);
  ApplyGltfNodeFlagEdit(node, restore.before);
  EXPECT_FALSE(node.extensions.at("KHR_node_visibility").Get("visible").Get<bool>());
  EXPECT_FALSE(node.extensions.at("KHR_node_selectability").Get("selectable").Get<bool>());
  EXPECT_EQ(node.extensions.at("KHR_node_visibility").Get("extras"),
            archive.extensions.at("KHR_node_visibility").Get("extras"));
}

TEST(GltfInteractivity, BooleanPointerTypeMismatchesUseInvalidReadAndErrorFlow) {
  auto model = asset(Json::parse(R"json({
    "types":[{"signature":"bool"},{"signature":"float"}],
    "declarations":[{"op":"event/onStart"},{"op":"pointer/get"},{"op":"flow/branch"},{"op":"pointer/set"}],
    "nodes":[
      {"declaration":0,"flows":{"out":{"node":2}}},
      {"declaration":1,"configuration":{"pointer":{"value":["/nodes/0/extensions/KHR_node_visibility/visible"]},"type":{"value":[1]}}},
      {"declaration":2,"values":{"condition":{"node":1,"socket":"isValid"}},"flows":{"false":{"node":4}}},
      {"declaration":1,"configuration":{"pointer":{"value":["/nodes/0/translation/0"]},"type":{"value":[0]}}},
      {"declaration":3,"configuration":{"pointer":{"value":["/nodes/0/extensions/KHR_node_selectability/selectable"]},"type":{"value":[0]}},
        "values":{"value":{"node":3}},"flows":{"out":{"node":5}}},
      {"declaration":3,"configuration":{"pointer":{"value":["/nodes/0/extensions/KHR_node_visibility/visible"]},"type":{"value":[0]}},
        "values":{"value":{"type":1,"value":[0]}},"flows":{"err":{"node":6}}},
      {"declaration":3,"configuration":{"pointer":{"value":["/nodes/0/extensions/KHR_node_hoverability/hoverable"]},"type":{"value":[0]}},
        "values":{"value":{"type":0,"value":[false]}}}
    ]})json"));
  defineNodeFlags(model.nodes[0]);
  model.nodes[0].translation = {2, 0, 0};
  GltfInteractivity player(model);
  const auto dirty = player.advance(model, {}, 0);
  EXPECT_TRUE(player.playing());
  EXPECT_TRUE(player.diagnostic().empty()) << player.diagnostic();
  ASSERT_EQ(dirty.nodeFlags.size(), 1u);
  EXPECT_EQ(dirty.nodeFlags[0].mask, uint8_t(GltfNodeFlag::Selectability) | uint8_t(GltfNodeFlag::Hoverability));
  const auto& flags = model.nodes[0].extensions;
  EXPECT_FALSE(flags.at("KHR_node_visibility").Has("visible"));
  EXPECT_FALSE(flags.at("KHR_node_selectability").Get("selectable").Get<bool>());
  EXPECT_FALSE(flags.at("KHR_node_hoverability").Get("hoverable").Get<bool>());
}

TEST(GltfInteractivity, BooleanPointersDiagnoseMissingParentAndRejectInterpolation) {
  auto graph = Json::parse(R"json({
    "types":[{"signature":"bool"},{"signature":"float"}],
    "declarations":[{"op":"event/onStart"},{"op":"pointer/set"}],
    "nodes":[
      {"declaration":0,"flows":{"out":{"node":1}}},
      {"declaration":1,"configuration":{"pointer":{"value":["/nodes/0/extensions/KHR_node_visibility/visible"]},"type":{"value":[0]}},
        "values":{"value":{"type":0,"value":[false]}}}
    ]})json");
  auto model = asset(graph);
  GltfInteractivity player(model);
  EXPECT_TRUE(player.advance(model, {}, 0).empty());
  EXPECT_FALSE(player.playing());
  EXPECT_TRUE(std::string_view(player.diagnostic()).contains("pointer/set")) << player.diagnostic();
  EXPECT_TRUE(std::string_view(player.diagnostic()).contains("parent object")) << player.diagnostic();
  EXPECT_TRUE(model.nodes[0].extensions.empty());

  graph["declarations"][1]["op"] = "pointer/interpolate";
  graph["nodes"][1]["values"]["duration"] = {{"type", 1}, {"value", {1}}};
  auto interpolation = asset(graph);
  defineNodeFlags(interpolation.nodes[0]);
  EXPECT_THROW(ValidateGltfInteractivity(interpolation), std::invalid_argument);
}

TEST(GltfInteractivity, StartMovesAndRecolorsUsingSynchronousFlowOrder) {
  auto model = asset(Json::parse(R"json({
    "types":[{"signature":"float3"},{"signature":"float4"}],
    "declarations":[{"op":"event/onStart"},{"op":"flow/sequence"},{"op":"pointer/get"},
      {"op":"pointer/set"},{"op":"math/add"}],
    "nodes":[
      {"declaration":0,"flows":{"out":{"node":1}}},
      {"declaration":1,"flows":{"b":{"node":5},"a":{"node":2}}},
      {"declaration":3,"configuration":{"pointer":{"value":["/nodes/0/translation"]},"type":{"value":[0]}},
        "values":{"value":{"type":0,"value":[1,2,3]}}},
      {"declaration":2,"configuration":{"pointer":{"value":["/nodes/0/translation"]},"type":{"value":[0]}}},
      {"declaration":4,"values":{"a":{"node":3},"b":{"type":0,"value":[1,1,1]}}},
      {"declaration":3,"configuration":{"pointer":{"value":["/nodes/1/translation"]},"type":{"value":[0]}},
        "values":{"value":{"node":4}},"flows":{"out":{"node":6}}},
      {"declaration":3,"configuration":{"pointer":{"value":["/materials/0/pbrMetallicRoughness/baseColorFactor"]},
        "type":{"value":[1]}},"values":{"value":{"type":1,"value":[0,1,0,1]}}}
    ]})json"));
  GltfInteractivity player(model);
  ASSERT_TRUE(player.available());
  const auto dirty = player.advance(model, {}, 10);
  EXPECT_EQ(dirty.nodes, (std::vector<size_t>{0, 1}));
  EXPECT_EQ(dirty.materials, (std::vector<size_t>{0}));
  EXPECT_EQ(model.nodes[1].translation, (std::vector<double>{2, 3, 4}));
  EXPECT_EQ(model.materials[0].pbrMetallicRoughness.baseColorFactor, (std::vector<double>{0, 1, 0, 1}));
  EXPECT_EQ(player.time(), 0);
  EXPECT_TRUE(player.diagnostic().empty());
}

TEST(GltfInteractivity, CustomEventEasesPointerAndResetRestoresBeforePlayback) {
  auto model = asset(Json::parse(R"json({
    "types":[{"signature":"float"},{"signature":"float3"}],
    "events":[{"id":"move","values":{"position":{"type":1,"value":[2,0,0]}}}],
    "declarations":[{"op":"event/receive"},{"op":"pointer/interpolate"},{"op":"pointer/set"}],
    "nodes":[
      {"declaration":0,"configuration":{"event":{"value":[0]}},"flows":{"out":{"node":1}}},
      {"declaration":1,"configuration":{"pointer":{"value":["/nodes/0/translation"]},"type":{"value":[1]}},
        "values":{"value":{"node":0,"socket":"position"},"duration":{"type":0,"value":[2]}},"flows":{"done":{"node":2}}},
      {"declaration":2,"configuration":{"pointer":{"value":["/nodes/1/translation"]},"type":{"value":[1]}},
        "values":{"value":{"type":1,"value":[3,0,0]}}}
    ]})json"));
  GltfInteractivity player(model);
  ASSERT_TRUE(player.queueCustomEvent("move"));
  player.advance(model, {}, 1);
  player.advance(model, {}, 1);
  ASSERT_EQ(model.nodes[0].translation.size(), 3);
  EXPECT_NEAR(model.nodes[0].translation[0], 1, 1e-6);
  EXPECT_TRUE(model.nodes[1].translation.empty());
  player.setPlaying(false);
  EXPECT_TRUE(player.advance(model, {}, 100).empty());
  EXPECT_FALSE(player.queueCustomEvent("move"));
  player.setPlaying(true);
  player.advance(model, {}, 1);
  EXPECT_EQ(model.nodes[0].translation[0], 2);
  ASSERT_EQ(model.nodes[1].translation.size(), 3);
  EXPECT_EQ(model.nodes[1].translation[0], 3);
  EXPECT_EQ(player.reset(model).nodes, (std::vector<size_t>{0, 1}));
  EXPECT_EQ(model.nodes[0].translation[0], 0);
  EXPECT_EQ(model.nodes[1].translation[0], 0);
  EXPECT_FALSE(player.started());
}

TEST(GltfInteractivity, LastDelayCancelsOnlyTheLatestActivation) {
  auto graph = Json::parse(R"json({
    "types":[{"signature":"float"}],"events":[{"id":"schedule"},{"id":"cancel-one"}],
    "declarations":[{"op":"event/receive"},{"op":"flow/setDelay"},{"op":"flow/cancelDelay"},
      {"op":"pointer/get"},{"op":"math/add"},{"op":"pointer/set"}],
    "nodes":[
      {"declaration":0,"configuration":{"event":{"value":[0]}},"flows":{"out":{"node":2}}},
      {"declaration":0,"configuration":{"event":{"value":[1]}},"flows":{"out":{"node":3}}},
      {"declaration":1,"values":{"duration":{"type":0,"value":[1]}},"flows":{"done":{"node":6}}},
      {"declaration":2,"values":{"delay":{"node":2,"socket":"lastDelay"}}},
      {"declaration":3,"configuration":{"pointer":{"value":["/nodes/0/translation/0"]},"type":{"value":[0]}}},
      {"declaration":4,"values":{"a":{"node":4},"b":{"type":0,"value":[1]}}},
      {"declaration":5,"configuration":{"pointer":{"value":["/nodes/0/translation/0"]},"type":{"value":[0]}},
        "values":{"value":{"node":5}}}
    ]})json");
  for (const char* socket : {"lastDelay", "delay"}) {
    SCOPED_TRACE(socket);
    graph["nodes"][3]["values"]["delay"]["socket"] = socket;
    auto model = asset(graph);
    model.nodes[0].translation = {0, 0, 0};
    GltfInteractivity player(model);
    ASSERT_TRUE(player.queueCustomEvent("cancel-one")); // Null reference before scheduling.
    ASSERT_TRUE(player.queueCustomEvent("schedule"));
    ASSERT_TRUE(player.queueCustomEvent("schedule"));
    ASSERT_TRUE(player.queueCustomEvent("cancel-one"));
    EXPECT_TRUE(player.advance(model, {}, 0).empty());
    EXPECT_EQ(player.advance(model, {}, 1).nodes, (std::vector<size_t>{0}));
    EXPECT_EQ(model.nodes[0].translation[0], 1);
    ASSERT_TRUE(player.queueCustomEvent("cancel-one")); // Already cancelled reference.
    EXPECT_TRUE(player.advance(model, {}, 1).empty());
    EXPECT_TRUE(player.diagnostic().empty()) << player.diagnostic();
  }
}

TEST(GltfInteractivity, DelayCancelSocketClearsOnlyTheTargetOwnerAndItsReference) {
  auto graph = Json::parse(R"json({
    "types":[{"signature":"float"},{"signature":"ref"}],
    "events":[{"id":"schedule"},{"id":"cancel-owner"},{"id":"observe"}],
    "declarations":[{"op":"event/onStart"},{"op":"event/receive"},{"op":"flow/setDelay"},
      {"op":"ref/eq"},{"op":"pointer/get"},{"op":"math/add"},{"op":"pointer/set"},{"op":"flow/branch"}],
    "nodes":[
      {"declaration":0,"flows":{"out":{"node":5}}},
      {"declaration":1,"configuration":{"event":{"value":[0]}},"flows":{"out":{"node":4}}},
      {"declaration":1,"configuration":{"event":{"value":[1]}},"flows":{"out":{"node":4,"socket":"cancel"}}},
      {"declaration":1,"configuration":{"event":{"value":[2]}},"flows":{"out":{"node":10}}},
      {"declaration":2,"values":{"duration":{"type":0,"value":[1]}},"flows":{"out":{"node":9},"done":{"node":11}}},
      {"declaration":2,"values":{"duration":{"type":0,"value":[1]}},"flows":{"done":{"node":14}}},
      {"declaration":3,"values":{"a":{"node":4,"socket":"lastDelay"},"b":{"type":1,"value":[null]}}},
      {"declaration":4,"configuration":{"pointer":{"value":["/nodes/0/translation/0"]},"type":{"value":[0]}}},
      {"declaration":5,"values":{"a":{"node":7},"b":{"type":0,"value":[1]}}},
      {"declaration":6,"configuration":{"pointer":{"value":["/nodes/0/translation/0"]},"type":{"value":[0]}},
        "values":{"value":{"node":8}}},
      {"declaration":7,"values":{"condition":{"node":6}},"flows":{"true":{"node":12},"false":{"node":13}}},
      {"declaration":6,"configuration":{"pointer":{"value":["/nodes/0/translation/1"]},"type":{"value":[0]}},
        "values":{"value":{"type":0,"value":[9]}}},
      {"declaration":6,"configuration":{"pointer":{"value":["/nodes/0/translation/2"]},"type":{"value":[0]}},
        "values":{"value":{"type":0,"value":[1]}}},
      {"declaration":6,"configuration":{"pointer":{"value":["/nodes/0/translation/2"]},"type":{"value":[0]}},
        "values":{"value":{"type":0,"value":[-1]}}},
      {"declaration":6,"configuration":{"pointer":{"value":["/nodes/1/translation/0"]},"type":{"value":[0]}},
        "values":{"value":{"type":0,"value":[6]}}}
    ]})json");
  auto model = asset(graph);
  model.nodes[0].translation = {0, 0, 0};
  model.nodes[1].translation = {0, 0, 0};
  GltfInteractivity player(model);
  ASSERT_TRUE(player.queueCustomEvent("observe"));
  player.advance(model, {}, 0);
  EXPECT_EQ(model.nodes[0].translation[2], 1);
  ASSERT_TRUE(player.queueCustomEvent("schedule"));
  ASSERT_TRUE(player.queueCustomEvent("schedule"));
  ASSERT_TRUE(player.queueCustomEvent("observe"));
  player.advance(model, {}, 0);
  EXPECT_EQ(model.nodes[0].translation, (std::vector<double>{2, 0, -1}));
  ASSERT_TRUE(player.queueCustomEvent("cancel-owner"));
  ASSERT_TRUE(player.queueCustomEvent("observe"));
  player.advance(model, {}, 0);
  EXPECT_EQ(model.nodes[0].translation, (std::vector<double>{2, 0, 1}));
  EXPECT_EQ(player.advance(model, {}, 1).nodes, (std::vector<size_t>{1}));
  EXPECT_EQ(model.nodes[0].translation, (std::vector<double>{2, 0, 1}));
  EXPECT_EQ(model.nodes[1].translation[0], 6);
  EXPECT_TRUE(player.diagnostic().empty()) << player.diagnostic();

  graph["nodes"][2]["flows"]["out"]["node"] = 11; // A pointer setter has no cancel socket.
  EXPECT_THROW(ValidateGltfInteractivity(asset(graph)), std::invalid_argument);
}

TEST(GltfInteractivity, PointerSetStopsOnlyTheMatchingInterpolation) {
  auto model = asset(Json::parse(R"json({
    "types":[{"signature":"float"},{"signature":"float3"},{"signature":"int"}],
    "events":[{"id":"move"},{"id":"override"},{"id":"invalid"}],
    "declarations":[{"op":"event/receive"},{"op":"flow/sequence"},{"op":"pointer/interpolate"},{"op":"pointer/set"}],
    "nodes":[
      {"declaration":0,"configuration":{"event":{"value":[0]}},"flows":{"out":{"node":1}}},
      {"declaration":1,"flows":{"a":{"node":2},"b":{"node":3}}},
      {"declaration":2,"configuration":{"pointer":{"value":["/nodes/0/translation"]},"type":{"value":[1]}},
        "values":{"value":{"type":1,"value":[4,0,0]},"duration":{"type":0,"value":[2]}},"flows":{"done":{"node":8}}},
      {"declaration":2,"configuration":{"pointer":{"value":["/nodes/1/translation"]},"type":{"value":[1]}},
        "values":{"value":{"type":1,"value":[0,6,0]},"duration":{"type":0,"value":[2]}},"flows":{"done":{"node":9}}},
      {"declaration":0,"configuration":{"event":{"value":[1]}},"flows":{"out":{"node":5}}},
      {"declaration":3,"configuration":{"pointer":{"value":["/nodes/{node}/translation"]},"type":{"value":[1]}},
        "values":{"node":{"type":2,"value":[0]},"value":{"type":1,"value":[9,0,0]}}},
      {"declaration":0,"configuration":{"event":{"value":[2]}},"flows":{"out":{"node":7}}},
      {"declaration":3,"configuration":{"pointer":{"value":["/nodes/0/translation"]},"type":{"value":[1]}},
        "values":{"value":{"type":0,"value":[3]}},"flows":{"err":{"node":10}}},
      {"declaration":3,"configuration":{"pointer":{"value":["/materials/0/pbrMetallicRoughness/metallicFactor"]},"type":{"value":[0]}},
        "values":{"value":{"type":0,"value":[0.5]}}},
      {"declaration":3,"configuration":{"pointer":{"value":["/materials/0/pbrMetallicRoughness/roughnessFactor"]},"type":{"value":[0]}},
        "values":{"value":{"type":0,"value":[0.25]}}},
      {"declaration":3,"configuration":{"pointer":{"value":["/materials/0/pbrMetallicRoughness/metallicFactor"]},"type":{"value":[0]}},
        "values":{"value":{"type":0,"value":[0.75]}}}
    ]})json"));
  GltfInteractivity player(model);
  auto& material = model.materials[0].pbrMetallicRoughness;
  ASSERT_TRUE(player.queueCustomEvent("move"));
  player.advance(model, {}, 0);
  player.advance(model, {}, .5);
  ASSERT_EQ(model.nodes[0].translation.size(), 3u);
  EXPECT_NEAR(model.nodes[0].translation[0], 1, 1e-6);
  ASSERT_TRUE(player.queueCustomEvent("invalid"));
  player.advance(model, {}, .5);
  EXPECT_EQ(material.metallicFactor, .75);
  EXPECT_NEAR(model.nodes[0].translation[0], 2, 1e-6);
  ASSERT_TRUE(player.queueCustomEvent("override"));
  player.advance(model, {}, .25);
  EXPECT_EQ(model.nodes[0].translation[0], 9);
  player.advance(model, {}, 1.75);
  EXPECT_EQ(model.nodes[0].translation[0], 9);
  EXPECT_EQ(model.nodes[1].translation[1], 6);
  EXPECT_EQ(material.metallicFactor, .75); // Only the unrelated interpolation completes.
  EXPECT_EQ(material.roughnessFactor, .25);
  EXPECT_TRUE(player.diagnostic().empty()) << player.diagnostic();
}

TEST(GltfInteractivity, VariableSetStopsInterpolationForEveryWrittenVariable) {
  auto model = asset(Json::parse(R"json({
    "types":[{"signature":"float"}],"variables":[{"type":0,"value":[0]},{"type":0,"value":[10]}],
    "events":[{"id":"start"},{"id":"override"}],
    "declarations":[{"op":"event/receive"},{"op":"flow/sequence"},{"op":"variable/interpolate"},
      {"op":"variable/set"},{"op":"variable/get"},{"op":"event/onTick"},{"op":"pointer/set"}],
    "nodes":[
      {"declaration":0,"configuration":{"event":{"value":[0]}},"flows":{"out":{"node":1}}},
      {"declaration":1,"flows":{"a":{"node":2},"b":{"node":3}}},
      {"declaration":2,"configuration":{"variable":{"value":[0]}},
        "values":{"value":{"type":0,"value":[4]},"duration":{"type":0,"value":[2]}},"flows":{"done":{"node":9}}},
      {"declaration":2,"configuration":{"variable":{"value":[1]}},
        "values":{"value":{"type":0,"value":[14]},"duration":{"type":0,"value":[2]}},"flows":{"done":{"node":10}}},
      {"declaration":0,"configuration":{"event":{"value":[1]}},"flows":{"out":{"node":5}}},
      {"declaration":3,"configuration":{"variables":{"value":[0,1]}},
        "values":{"0":{"type":0,"value":[9]},"1":{"type":0,"value":[20]}}},
      {"declaration":4,"configuration":{"variable":{"value":[0]}}},
      {"declaration":4,"configuration":{"variable":{"value":[1]}}},
      {"declaration":5,"flows":{"out":{"node":11}}},
      {"declaration":6,"configuration":{"pointer":{"value":["/materials/0/pbrMetallicRoughness/metallicFactor"]},"type":{"value":[0]}},
        "values":{"value":{"type":0,"value":[0.5]}}},
      {"declaration":6,"configuration":{"pointer":{"value":["/materials/0/pbrMetallicRoughness/roughnessFactor"]},"type":{"value":[0]}},
        "values":{"value":{"type":0,"value":[0.5]}}},
      {"declaration":6,"configuration":{"pointer":{"value":["/nodes/0/translation/0"]},"type":{"value":[0]}},
        "values":{"value":{"node":6}},"flows":{"out":{"node":12}}},
      {"declaration":6,"configuration":{"pointer":{"value":["/nodes/0/translation/1"]},"type":{"value":[0]}},
        "values":{"value":{"node":7}}}
    ]})json"));
  GltfInteractivity player(model);
  ASSERT_TRUE(player.queueCustomEvent("start"));
  player.advance(model, {}, 0);
  player.advance(model, {}, 1);
  ASSERT_EQ(model.nodes[0].translation.size(), 3u);
  EXPECT_NEAR(model.nodes[0].translation[0], 2, 1e-6);
  EXPECT_NEAR(model.nodes[0].translation[1], 12, 1e-6);
  ASSERT_TRUE(player.queueCustomEvent("override"));
  player.advance(model, {}, .25);
  EXPECT_EQ(model.nodes[0].translation, (std::vector<double>{9, 20, 0}));
  EXPECT_TRUE(player.advance(model, {}, 2).materials.empty());
  EXPECT_EQ(model.nodes[0].translation, (std::vector<double>{9, 20, 0}));
  EXPECT_TRUE(player.diagnostic().empty()) << player.diagnostic();
}

TEST(GltfInteractivity, ClipStartsSamplesAndStopsWithoutRestoringUntilReset) {
  auto model = asset(Json::parse(R"json({
    "types":[{"signature":"ref"}],"events":[{"id":"stop"}],
    "declarations":[{"op":"event/onStart"},{"op":"animation/start"},{"op":"event/receive"},{"op":"animation/stop"}],
    "nodes":[
      {"declaration":0,"flows":{"out":{"node":1}}},
      {"declaration":1,"values":{"animation":{"type":0,"value":["/animations/0"]}}},
      {"declaration":2,"configuration":{"event":{"value":[0]}},"flows":{"out":{"node":3}}},
      {"declaration":3,"values":{"animation":{"type":0,"value":["/animations/0"]}}}
    ]})json"));
  const AnimationTarget translation{AnimationTargetKind::NodeTranslation, 0, "translation", 3};
  AnimationLibrary library;
  library.clips.push_back({"move", 2.0, {{translation, AnimationInterpolation::Linear, {0, 2}, {0, 0, 0, 4, 0, 0}}}});
  GltfInteractivity player(model);
  player.advance(model, library, 0);
  EXPECT_EQ(player.activeClips(), 1);
  player.advance(model, library, .5);
  ASSERT_EQ(model.nodes[0].translation.size(), 3);
  EXPECT_EQ(model.nodes[0].translation[0], 1);
  ASSERT_TRUE(player.queueCustomEvent("stop"));
  EXPECT_TRUE(player.advance(model, library, .5).empty());
  EXPECT_EQ(player.activeClips(), 0);
  EXPECT_EQ(model.nodes[0].translation[0], 1);
  EXPECT_EQ(player.reset(model).nodes, (std::vector<size_t>{0}));
  EXPECT_EQ(model.nodes[0].translation[0], 0);
}

TEST(GltfInteractivity, RestartedClipAppliesAfterOtherActiveClips) {
  auto model = asset(Json::parse(R"json({
    "types":[{"signature":"ref"}],"events":[{"id":"restart"}],
    "declarations":[{"op":"event/onStart"},{"op":"animation/start"},{"op":"event/receive"}],
    "nodes":[
      {"declaration":0,"flows":{"out":{"node":1}}},
      {"declaration":1,"values":{"animation":{"type":0,"value":["/animations/0"]}},"flows":{"out":{"node":2}}},
      {"declaration":1,"values":{"animation":{"type":0,"value":["/animations/1"]}}},
      {"declaration":2,"configuration":{"event":{"value":[0]}},"flows":{"out":{"node":4}}},
      {"declaration":1,"values":{"animation":{"type":0,"value":["/animations/0"]}}}
    ]})json"));
  const AnimationTarget translation{AnimationTargetKind::NodeTranslation, 0, "translation", 3};
  AnimationLibrary library;
  library.clips.push_back({"first", 2.0, {{translation, AnimationInterpolation::Linear, {0, 2}, {1, 0, 0, 1, 0, 0}}}});
  library.clips.push_back({"second", 2.0, {{translation, AnimationInterpolation::Linear, {0, 2}, {2, 0, 0, 2, 0, 0}}}});
  GltfInteractivity player(model);

  player.advance(model, library, 0);
  ASSERT_TRUE(player.diagnostic().empty()) << player.diagnostic();
  ASSERT_EQ(player.activeClips(), 2);
  ASSERT_EQ(model.nodes[0].translation, (std::vector<double>{2, 0, 0}));
  ASSERT_TRUE(player.queueCustomEvent("restart"));
  player.advance(model, library, .25);
  EXPECT_TRUE(player.diagnostic().empty()) << player.diagnostic();
  EXPECT_EQ(player.activeClips(), 2);
  EXPECT_EQ(model.nodes[0].translation, (std::vector<double>{1, 0, 0}));
}

TEST(GltfInteractivity, ScheduledClipStopsUseDirectionalIntervalsAndCompletionFlows) {
  const auto graph = Json::parse(R"json({
    "types":[{"signature":"ref"},{"signature":"float"},{"signature":"float3"}],"events":[{"id":"stopAt"}],
    "declarations":[{"op":"event/onStart"},{"op":"animation/start"},{"op":"event/receive"},
      {"op":"animation/stopAt"},{"op":"pointer/set"}],
    "nodes":[
      {"declaration":0,"flows":{"out":{"node":1}}},
      {"declaration":1,"values":{"animation":{"type":0,"value":["/animations/0"]},
        "startTime":{"type":1,"value":[0]},"endTime":{"type":1,"value":[0]}},"flows":{"done":{"node":4}}},
      {"declaration":2,"configuration":{"event":{"value":[0]}},"flows":{"out":{"node":3}}},
      {"declaration":3,"values":{"animation":{"type":0,"value":["/animations/0"]},
        "stopTime":{"type":1,"value":[0]}},"flows":{"done":{"node":5}}},
      {"declaration":4,"configuration":{"pointer":{"value":["/nodes/1/translation"]},"type":{"value":[2]}},
        "values":{"value":{"type":2,"value":[10,0,0]}}},
      {"declaration":4,"configuration":{"pointer":{"value":["/nodes/1/translation"]},"type":{"value":[2]}},
        "values":{"value":{"type":2,"value":[20,0,0]}}}
    ]})json");
  const AnimationTarget translation{AnimationTargetKind::NodeTranslation, 0, "translation", 3};
  AnimationLibrary library;
  library.clips.push_back({"move", 2.0, {{translation, AnimationInterpolation::Linear, {0, 2}, {0, 0, 0, 4, 0, 0}}}});

  struct Scenario {
    const char* label;
    double start, end, stop, elapsed;
    bool scheduled;
  };
  const Scenario cases[]{
      {"forward before start", .5, 1.5, .25, 0, false},
      {"forward at start", .5, 1.5, .5, 0, true},
      {"forward within interval", .5, 1.5, 1, 0, true},
      {"forward past scheduled stop", .5, 1.5, 1, .75, true},
      {"forward at natural end", .5, 1.5, 1.5, 0, false},
      {"forward beyond natural end", .5, 1.5, 1.75, 0, false},
      {"reverse before start", 1.5, .5, 1.75, 0, false},
      {"reverse at start", 1.5, .5, 1.5, 0, true},
      {"reverse within interval", 1.5, .5, 1, 0, true},
      {"reverse past scheduled stop", 1.5, .5, 1, .75, true},
      {"reverse at natural end", 1.5, .5, .5, 0, false},
      {"reverse beyond natural end", 1.5, .5, .25, 0, false},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.label);
    auto fixture = graph;
    fixture["nodes"][1]["values"]["startTime"]["value"][0] = test.start;
    fixture["nodes"][1]["values"]["endTime"]["value"][0] = test.end;
    fixture["nodes"][3]["values"]["stopTime"]["value"][0] = test.stop;
    auto model = asset(std::move(fixture));
    GltfInteractivity player(model);
    player.advance(model, library, 0);
    player.advance(model, library, test.elapsed);
    ASSERT_EQ(player.activeClips(), 1);

    ASSERT_TRUE(player.queueCustomEvent("stopAt"));
    player.advance(model, library, 0);
    const bool alreadyReached = test.scheduled && test.elapsed >= std::abs(test.stop - test.start);
    EXPECT_EQ(player.activeClips(), alreadyReached ? 0u : 1u);
    if (!alreadyReached) EXPECT_TRUE(model.nodes[1].translation.empty());

    // A frame can cross both a scheduled stop and the natural end.
    player.advance(model, library, 2);
    EXPECT_TRUE(player.diagnostic().empty()) << player.diagnostic();
    EXPECT_EQ(player.activeClips(), 0);
    EXPECT_EQ(model.nodes[0].translation, (std::vector<double>{2 * (test.scheduled ? test.stop : test.end), 0, 0}));
    EXPECT_EQ(model.nodes[1].translation, (std::vector<double>{test.scheduled ? 20.0 : 10.0, 0, 0}));
  }
}

TEST(GltfInteractivity, SelectionAndHoverUseProvidedSourceChainAndEventValues) {
  auto model = asset(Json::parse(R"json({
    "types":[{"signature":"float3"}],
    "declarations":[{"op":"event/onSelect","extension":"KHR_node_selectability"},
      {"op":"event/onHoverIn","extension":"KHR_node_hoverability"},{"op":"pointer/set"}],
    "nodes":[
      {"declaration":0,"configuration":{"nodeIndex":{"value":[0]}},"flows":{"out":{"node":1}}},
      {"declaration":2,"configuration":{"pointer":{"value":["/nodes/0/translation"]},"type":{"value":[0]}},
        "values":{"value":{"node":0,"socket":"selectionPoint"}}},
      {"declaration":1,"configuration":{"nodeIndex":{"value":[1]}},"flows":{"out":{"node":3}}},
      {"declaration":2,"configuration":{"pointer":{"value":["/nodes/1/translation"]},"type":{"value":[0]}},
        "values":{"value":{"type":0,"value":[5,0,0]}}}
    ]})json"));
  GltfInteractivity player(model);
  ASSERT_TRUE(player.queueNodeEvent(GltfInteractivity::NodeEvent::Select, {1, 0}, GltfInteractivity::Point{1, 2, 3}));
  ASSERT_TRUE(player.queueNodeEvent(GltfInteractivity::NodeEvent::HoverIn, {1}));
  EXPECT_EQ(player.advance(model, {}, 0).nodes, (std::vector<size_t>{0, 1}));
  EXPECT_EQ(model.nodes[0].translation, (std::vector<double>{1, 2, 3}));
  EXPECT_EQ(model.nodes[1].translation, (std::vector<double>{5, 0, 0}));
}

TEST(GltfInteractivity, RejectsUnknownOperationsAndBackwardFlowAndBoundsDeepDispatch) {
  auto graph = Json::parse(R"json({
    "declarations":[{"op":"event/onStart"},{"op":"flow/sequence"}],
    "nodes":[{"declaration":0,"flows":{"out":{"node":1}}},{"declaration":1,"flows":{"out":{"node":0}}}]
  })json");
  EXPECT_THROW(ValidateGltfInteractivity(asset(graph)), std::invalid_argument);
  graph["declarations"][1]["op"] = "unsupported/operation";
  graph["nodes"][1].erase("flows");
  EXPECT_THROW(ValidateGltfInteractivity(asset(graph)), std::invalid_argument);

  graph["declarations"][1]["op"] = "flow/sequence";
  for (size_t i = 1; i < 110; ++i)
    graph["nodes"][i] = {{"declaration", 1}, {"flows", {{"out", {{"node", i + 1}}}}}};
  graph["nodes"].push_back({{"declaration", 1}});
  auto model = asset(graph);
  GltfInteractivity player(model);
  EXPECT_TRUE(player.advance(model, {}, 0).empty());
  EXPECT_FALSE(player.playing());
  EXPECT_NE(player.diagnostic().find("work budget"), std::string::npos);
}

TEST(GltfInteractivity, RemapsBindingsAndTypedReferencesWithoutEditingAnnotations) {
  auto model = asset(Json::parse(R"json({
    "types":[{"signature":"ref"},{"signature":"float3"}],
    "variables":[{"type":0,"value":["/materials/0"]}],
    "events":[{"id":"demo","values":{"node":{"type":0,"value":["/nodes/1"]}}}],
    "declarations":[{"op":"event/onSelect","extension":"KHR_node_selectability"},{"op":"pointer/set"},{"op":"animation/start"}],
    "nodes":[
      {"declaration":0,"configuration":{"nodeIndex":{"value":[1]}},"flows":{"out":{"node":1}}},
      {"declaration":1,"configuration":{"pointer":{"value":["/nodes/0/translation/0"]},"type":{"value":[1]}},
        "values":{"value":{"type":1,"value":[1,2,3]}},"extras":{"label":"/nodes/0"}},
      {"declaration":2,"values":{"animation":{"type":0,"value":["/animations/0"]}}}
    ]})json"));
  model.animations.resize(1);
  const std::array<int32_t, 2> nodes{4, 7};
  const std::array<int32_t, 1> materials{6}, animations{8};
  const GltfInteractivityMappings maps{{
    {nodes, 9}, {nodes, 9}, {materials, 9}, {{}, 0}, {{}, 0}, {animations, 9},
  }};
  const auto& original = model.extensions.at(kGltfInteractivityExtension);
  const auto remapped = RemapGltfInteractivity(model, maps);
  const auto& graph = remapped.Get("graphs").Get(size_t{0});
  EXPECT_EQ(graph.Get("variables").Get(size_t{0}).Get("value").Get(size_t{0}).Get<std::string>(), "/materials/6");
  EXPECT_EQ(graph.Get("events").Get(size_t{0}).Get("values").Get("node").Get("value").Get(size_t{0}).Get<std::string>(),
      "/nodes/7");
  const auto& entries = graph.Get("nodes");
  EXPECT_EQ(entries.Get(size_t{0}).Get("configuration").Get("nodeIndex").Get("value").Get(size_t{0}).Get<int>(), 7);
  EXPECT_EQ(entries.Get(size_t{1}).Get("configuration").Get("pointer").Get("value").Get(size_t{0}).Get<std::string>(),
      "/nodes/4/translation/0");
  EXPECT_EQ(entries.Get(size_t{1}).Get("extras").Get("label").Get<std::string>(), "/nodes/0");
  EXPECT_EQ(entries.Get(size_t{2}).Get("values").Get("animation").Get("value").Get(size_t{0}).Get<std::string>(),
      "/animations/8");
  EXPECT_EQ(original.Get("graphs").Get(size_t{0}).Get("nodes").Get(size_t{0}).Get("configuration")
      .Get("nodeIndex").Get("value").Get(size_t{0}).Get<int>(), 1);
}

TEST(GltfInteractivity, DynamicObjectIdsRequireUnchangedCompleteDomain) {
  const auto model = asset(Json::parse(R"json({
    "types":[{"signature":"float3"},{"signature":"int"}],
    "declarations":[{"op":"pointer/get"}],
    "nodes":[{"declaration":0,"configuration":{"pointer":{"value":["/nodes/{target}/translation"]},"type":{"value":[0]}},
      "values":{"target":{"type":1,"value":[0]}}}]
  })json"));
  GltfInteractivityMappings maps{};
  maps[size_t(InteractivityIndexDomain::Node)].destinationSize = model.nodes.size();
  EXPECT_EQ(RemapGltfInteractivity(model, maps), model.extensions.at(kGltfInteractivityExtension));
  ++maps[size_t(InteractivityIndexDomain::Node)].destinationSize;
  EXPECT_THROW(RemapGltfInteractivity(model, maps), std::invalid_argument);
  const std::array<int32_t, 2> shifted{1, 0};
  maps[size_t(InteractivityIndexDomain::Node)] = {shifted, model.nodes.size()};
  EXPECT_THROW(RemapGltfInteractivity(model, maps), std::invalid_argument);

  const auto crossDomain = asset(Json::parse(R"json({
    "types":[{"signature":"float3"},{"signature":"ref"}],
    "declarations":[{"op":"pointer/get"}],
    "nodes":[{"declaration":0,"configuration":{"pointer":{"value":["/nodes/{target}/translation"]},"type":{"value":[0]}},
      "values":{"target":{"type":1,"value":["/materials/0"]}}}]
  })json"));
  const std::array<int32_t, 1> shiftedMaterials{2};
  maps[size_t(InteractivityIndexDomain::Node)] = {{}, crossDomain.nodes.size()};
  maps[size_t(InteractivityIndexDomain::Material)] = {shiftedMaterials, 3};
  EXPECT_THROW(RemapGltfInteractivity(crossDomain, maps), std::invalid_argument);
}

TEST(GltfInteractivity, MergeAdoptsPlacedGraphAndRejectsIndependentOrMissingTargets) {
  auto incoming = asset(Json::parse(R"json({
    "types":[{"signature":"float3"}],
    "declarations":[{"op":"event/onStart"},{"op":"pointer/set"}],
    "nodes":[
      {"declaration":0,"flows":{"out":{"node":1}}},
      {"declaration":1,"configuration":{"pointer":{"value":["/nodes/1/translation"]},"type":{"value":[0]}},
        "values":{"value":{"type":0,"value":[1,2,3]}}}
    ]})json"));
  incoming.asset.version = "2.0";
  incoming.extensionsUsed = {kGltfInteractivityExtension};
  incoming.extensionsRequired = incoming.extensionsUsed;
  tinygltf::Scene both;
  both.nodes = {0, 1};
  tinygltf::Scene missingTarget;
  missingTarget.nodes = {0};
  incoming.scenes = {both, missingTarget};
  incoming.defaultScene = 0;
  tinygltf::Model base;
  base.asset.version = "2.0";
  base.nodes.resize(1);
  auto plan = PlanGltfMerge(base, incoming);
  GltfInteractivity graph(plan.model);
  const auto placement = size_t(plan.placement.nodes[1]);
  EXPECT_EQ(graph.advance(plan.model, {}, 0).nodes, std::vector<size_t>{placement});
  EXPECT_TRUE(plan.model.nodes[size_t(plan.imported.nodes[1])].translation.empty());
  EXPECT_EQ(plan.model.nodes[placement].translation, (std::vector<double>{1, 2, 3}));
  EXPECT_TRUE(incoming.nodes[1].translation.empty());
  EXPECT_THROW((void)PlanGltfMerge(incoming, incoming), std::runtime_error);
  EXPECT_THROW((void)PlanGltfMerge(base, incoming, {.incomingScene = 1}), std::runtime_error);
  auto retained = PlanGltfMerge(incoming, base);
  EXPECT_EQ(retained.model.extensions.at(kGltfInteractivityExtension), incoming.extensions.at(kGltfInteractivityExtension));
}
} // namespace
