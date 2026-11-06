#include <Container/geometry/GltfInteractivity.h>
#include <Container/geometry/GltfMerge.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

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
