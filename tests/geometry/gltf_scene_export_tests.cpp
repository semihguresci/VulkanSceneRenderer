#include <Container/geometry/GltfModelLoader.h>
#include <Container/geometry/GltfSceneExport.h>
#include <Container/geometry/GltfLinkedAssets.h>
#include <Container/geometry/GltfMaterialExport.h>
#include <Container/geometry/GltfNodeExportCompaction.h>
#include <Container/geometry/GltfMaterialExportCompaction.h>
#include <Container/geometry/GltfAnimation.h>
#include <Container/geometry/GltfInteractivity.h>
#include <Container/geometry/GltfDeformation.h>
#include <Container/geometry/GltfDeformationFrames.h>
#include <Container/geometry/GltfAccessor.h>
#include <Container/geometry/GltfMaterialVariants.h>
#include <Container/geometry/GltfNodeFlags.h>
#include <Container/geometry/GltfImageSources.h>
#include <Container/geometry/GltfXmpMetadata.h>
#include <Container/utility/GltfSceneInstancing.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>
#include <utility>

namespace {

using container::geometry::Model;
using container::geometry::gltf::buildExportModel;
using container::geometry::gltf::LoadModelWithSource;
using container::geometry::gltf::SceneExportSnapshot;
using container::geometry::gltf::writeSceneExport;
using container::geometry::gltf::writeExportModel;
using container::scene::SceneGraph;

class ExportDirectory {
 public:
  ExportDirectory() {
    static std::atomic<uint64_t> sequence{0};
    root = std::filesystem::temp_directory_path() /
        ("container-scene-export-" + std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         '-' + std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(root);
  }
  ~ExportDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
  std::filesystem::path root;
};

std::string readText(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void writeText(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream stream(path, std::ios::binary);
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(stream.good());
}

void expectMatrix(const tinygltf::Node& node, const glm::mat4& matrix) {
  ASSERT_EQ(node.matrix.size(), 16u);
  for (int col = 0; col < 4; ++col)
    for (int row = 0; row < 4; ++row)
      EXPECT_NEAR(node.matrix[static_cast<size_t>(col * 4 + row)],
                  matrix[col][row], 1e-6);
}

SceneGraph cubeGraph() {
  SceneGraph graph;
  const auto root = graph.createNode(glm::mat4(1), 0, false,
                                      SceneGraph::kInvalidNode, "Source group");
  const auto cube = graph.createNode(glm::mat4(1), 0, true, 0, "Source cube");
  graph.setParent(cube, root);
  return graph;
}

void scalarBehavior(tinygltf::Model& model, const std::vector<std::string>& paths) {
  using namespace container::geometry::gltf;
  using V = tinygltf::Value;
  using O = V::Object;
  using A = V::Array;
  A nodes{V(O{{"declaration",V(0)},{"flows",V(O{{"out",V(O{{"node",V(1)}})}})}})};
  for (size_t i = 0; i < paths.size(); ++i) {
    O node{{"declaration",V(1)},{"configuration",V(O{
        {"pointer",V(O{{"value",V(A{V(paths[i])})}})},
        {"type",V(O{{"value",V(A{V(0)})}})}})},
        {"values",V(O{{"value",V(O{{"type",V(0)},{"value",V(A{V(.75)})}})},
            {"node",V(O{{"type",V(2)},{"value",V(A{V(2)})}})}})}};
    if (i + 1 < paths.size()) node["flows"] = V(O{{"out",V(O{{"node",V(int(i + 2))}})}});
    nodes.emplace_back(std::move(node));
  }
  V graph(O{{"types",V(A{V(O{{"signature",V(std::string("float"))}}),
      V(O{{"signature",V(std::string("ref"))}}),V(O{{"signature",V(std::string("int"))}})})},
      {"declarations",V(A{V(O{{"op",V(std::string("event/onStart"))}}),
          V(O{{"op",V(std::string("pointer/set"))}})})},
      {"nodes",V(std::move(nodes))},
      {"extras",V(O{{"opaqueIndex",V(917)}})}});
  model.extensions[kGltfInteractivityExtension] = V(O{{"graphs",V(A{graph})},
      {"extras",V(O{{"opaqueReference",V(std::string("/nodes/917"))}})}});
  if (std::ranges::find(model.extensionsUsed,kGltfInteractivityExtension) == model.extensionsUsed.end())
    model.extensionsUsed.push_back(kGltfInteractivityExtension);
}

TEST(GltfSceneExport, BehaviorGraphTargetsRetainedNodeAndRuntimeMaterialAfterReload) {
  using namespace container::geometry::gltf;
  using Value = tinygltf::Value;
  using Object = Value::Object;
  using Array = Value::Array;
  const auto cube = Model::MakeCube();
  auto source = buildExportModel({cubeGraph(), cube});
  source.nodes[0].matrix.clear();
  source.nodes[1].matrix.clear();
  source.nodes[1].translation = {0, 0, 0};
  const auto accessor = [&](const std::vector<float>& values, int type, size_t count) {
    auto& data = source.buffers.front().data;
    const size_t offset = (data.size() + 3) & ~size_t{3};
    const size_t bytes = values.size() * sizeof(float);
    data.resize(offset + bytes);
    std::memcpy(data.data() + offset, values.data(), bytes);
    tinygltf::BufferView view;
    view.buffer = 0;
    view.byteOffset = offset;
    view.byteLength = bytes;
    tinygltf::Accessor result;
    result.bufferView = int(source.bufferViews.size());
    result.type = type;
    result.count = count;
    result.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    source.bufferViews.push_back(view);
    source.accessors.push_back(result);
    return int(source.accessors.size() - 1);
  };
  const int times = accessor({0, 1}, TINYGLTF_TYPE_SCALAR, 2);
  source.accessors[times].minValues = {0};
  source.accessors[times].maxValues = {1};
  tinygltf::Animation motion, color;
  tinygltf::AnimationSampler motionSamples, colorSamples;
  motionSamples.input = colorSamples.input = times;
  motionSamples.output = accessor({0, 0, 0, 1, 0, 0}, TINYGLTF_TYPE_VEC3, 2);
  colorSamples.output = accessor({1, 1, 1, 1, 0, 0, 1, 1}, TINYGLTF_TYPE_VEC4, 2);
  motion.samplers.push_back(motionSamples);
  color.samplers.push_back(colorSamples);
  tinygltf::AnimationChannel motionChannel, colorChannel;
  motionChannel.sampler = colorChannel.sampler = 0;
  motionChannel.target_node = 1;
  motionChannel.target_path = "translation";
  colorChannel.target_path = "pointer";
  colorChannel.target_extensions["KHR_animation_pointer"] = Value(Object{
      {"pointer", Value(std::string("/materials/0/pbrMetallicRoughness/baseColorFactor"))}});
  motion.channels.push_back(motionChannel);
  color.channels.push_back(colorChannel);
  source.animations = {motion, color};
  source.extensionsUsed.push_back("KHR_animation_pointer");
  const auto graphPointer = [](const char* path, int type, Array value) {
    return Value(Object{
      {"declaration", Value(1)},
      {"configuration", Value(Object{
        {"pointer", Value(Object{{"value", Value(Array{Value(std::string(path))})}})},
        {"type", Value(Object{{"value", Value(Array{Value(type)})}})}})},
      {"values", Value(Object{{"value", Value(Object{{"type", Value(type)}, {"value", Value(std::move(value))}})}})},
    });
  };
  auto first = graphPointer("/nodes/0/translation", 0, {Value(2.), Value(0.), Value(0.)});
  first.Get<Object>()["flows"] = Value(Object{{"out", Value(Object{{"node", Value(2)}})}});
  auto recolor = graphPointer("/materials/0/pbrMetallicRoughness/baseColorFactor", 1,
      {Value(0.), Value(1.), Value(0.), Value(1.)});
  recolor.Get<Object>()["flows"] = Value(Object{{"out", Value(Object{{"node", Value(3)}})}});
  const Value startClip(Object{{"declaration", Value(2)}, {"values", Value(Object{{"animation",
    Value(Object{{"type", Value(2)}, {"value", Value(Array{Value(std::string("/animations/0"))})}})}})}});
  const Value start(Object{{"declaration", Value(0)},
    {"flows", Value(Object{{"out", Value(Object{{"node", Value(1)}})}})}});
  const Value behaviorGraph(Object{
    {"declarations", Value(Array{
      Value(Object{{"op", Value(std::string("event/onStart"))}}),
      Value(Object{{"op", Value(std::string("pointer/set"))}}),
      Value(Object{{"op", Value(std::string("animation/start"))}})})},
    {"types", Value(Array{
      Value(Object{{"signature", Value(std::string("float3"))}}),
      Value(Object{{"signature", Value(std::string("float4"))}}),
      Value(Object{{"signature", Value(std::string("ref"))}})})},
    {"nodes", Value(Array{start, first, recolor, startClip})},
  });
  source.extensions[kGltfInteractivityExtension] = Value(Object{{"graphs", Value(Array{behaviorGraph})}});
  source.extensionsUsed.push_back(kGltfInteractivityExtension);
  source.extensionsRequired.push_back(kGltfInteractivityExtension);

  SceneGraph runtime;
  const auto editor = runtime.createNode(glm::mat4(1), 0, false);
  const auto group = runtime.createNode(glm::mat4(1), 0, false);
  const auto drawable = runtime.createNode(glm::mat4(1), 1, true, 0);
  ASSERT_TRUE(runtime.setParent(group, editor));
  ASSERT_TRUE(runtime.setParent(drawable, group));
  const std::array<int32_t, 3> nodeMap{-1, 0, 1};
  const std::array<int32_t, 1> materialMap{1};
  std::vector<tinygltf::Material> materials(2);
  const SceneExportSnapshot snapshot{runtime, cube, &source, {}, nodeMap, materials, materialMap, &source};
  ExportDirectory temp;
  const auto path = temp.root / "behavior.glb";
  ASSERT_NO_THROW(writeSceneExport(snapshot, path));
  auto reloaded = LoadModelWithSource(path.string()).gltfModel;
  ASSERT_TRUE(reloaded.nodes[group].matrix.empty());
  const auto animations = ParseAnimations(reloaded);
  ASSERT_EQ(animations.clips.size(), 2u);
  EXPECT_EQ(animations.clips[0].tracks.size(), 1u);
  EXPECT_EQ(animations.clips[1].tracks.size(), 2u);
  GltfInteractivity behavior(reloaded);
  const auto dirty = behavior.advance(reloaded, animations, 0);
  EXPECT_EQ(dirty.nodes, std::vector<size_t>{group});
  EXPECT_EQ(dirty.materials, std::vector<size_t>{1});
  EXPECT_EQ(reloaded.nodes[group].translation, (std::vector<double>{2, 0, 0}));
  EXPECT_EQ(reloaded.materials[1].pbrMetallicRoughness.baseColorFactor, (std::vector<double>{0, 1, 0, 1}));

  auto compacted = snapshot;
  const std::array<uint8_t, 2> activeMaterials{0, 1};
  compacted.sourceResourceMasks.materials = activeMaterials;
  EXPECT_NO_THROW((void)buildExportModel(compacted));
}

tinygltf::Model behaviorExportSource(const Model& cube) {
  auto source = buildExportModel({cubeGraph(),cube});
  source.nodes.insert(source.nodes.begin(),tinygltf::Node{});
  source.nodes[0].name = "Unused retired node";
  source.nodes[1].children = {2};
  source.nodes[2].matrix.clear();
  source.nodes[2].translation = {0,0,0};
  source.scenes[0].nodes = {0,1};
  source.materials.resize(3);
  source.meshes[0].primitives[0].material = 2;
  source.materials[2].pbrMetallicRoughness.baseColorTexture.index = 1;
  source.images.resize(2);
  source.textures.resize(2);
  source.samplers.resize(2);
  for (size_t i = 0; i < 2; ++i) {
    auto& image = source.images[i];
    image.width = image.height = 1;
    image.component = 4;
    image.bits = 8;
    image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image.image = {uint8_t(i ? 255 : 0),0,0,255};
    source.textures[i].source = source.textures[i].sampler = int(i);
  }
  source.lights.resize(1);
  source.lights[0].type = "point";
  source.lights[0].intensity = 4;
  source.nodes[2].light = 0;
  source.extensionsUsed.push_back("KHR_lights_punctual");
  return source;
}

TEST(GltfSceneExport, BehaviorSurvivesUnrelatedNodeMaterialAndTextureRetirementAcrossBothFormats) {
  using namespace container::geometry::gltf;
  const auto cube = Model::MakeCube();
  auto source = behaviorExportSource(cube);
  scalarBehavior(source,{"/nodes/2/translation/0","/materials/2/pbrMetallicRoughness/metallicFactor",
      "/extensions/KHR_lights_punctual/lights/0/intensity"});
  const auto original = source;
  SceneGraph graph;
  graph.createNode(glm::mat4(1),0,false,SceneGraph::kInvalidNode,"Unused retired node");
  const auto group = graph.createNode(glm::mat4(1),0,false,SceneGraph::kInvalidNode,"Source group");
  const auto renderable = graph.createNode(glm::mat4(1),2,true,0,"Source cube");
  ASSERT_TRUE(graph.setParent(renderable,group));
  auto retirement = graph.prepareRetireSubtree(0);
  ASSERT_TRUE(graph.publishRetirement(retirement));
  const std::array<int32_t,3> identities{0,1,2};
  const std::array<uint8_t,2> resourcePhases{0,1};
  const std::array<uint8_t,3> materialPhases{0,1,1};
  SceneExportSnapshot snapshot{graph,cube,&source,{},identities};
  snapshot.sourceResourceMasks = {resourcePhases,resourcePhases,resourcePhases,materialPhases};
  auto packed = buildExportModel(snapshot);
  ASSERT_EQ(packed.images.size(),1u);
  ASSERT_EQ(packed.textures.size(),1u);
  ASSERT_EQ(packed.samplers.size(),1u);
  auto compacted = CompactGltfExportNodes(packed,std::array<int32_t,1>{0});
  const auto retainedNode = compacted.oldToNewNodes[renderable];
  auto& output = compacted.model;
  const auto materials = PlanGltfMaterialExportCompaction(output,std::array<int32_t,1>{0});
  ApplyGltfMaterialExportCompaction(output,materials);
  output.animations.emplace_back().name = "Retired clip";
  ExportDirectory directory;
  for (const auto* suffix : {".gltf",".glb"}) {
    const auto path = directory.root / (std::string("behavior-retirement") + suffix);
    writeExportModel(output,path);
    auto loaded = LoadModelWithSource(path.string()).gltfModel;
    EXPECT_TRUE(loaded.animations.empty());
    EXPECT_EQ(output.animations.size(), 1u);
    const auto& extension = loaded.extensions.at(kGltfInteractivityExtension);
    EXPECT_EQ(extension.Get("extras").Get("opaqueReference").Get<std::string>(),"/nodes/917");
    GltfInteractivity behavior(loaded);
    const auto dirty = behavior.advance(loaded,ParseAnimations(loaded),0);
    EXPECT_EQ(dirty.nodes,std::vector<size_t>{size_t(retainedNode)});
    EXPECT_EQ(dirty.materials,std::vector<size_t>{1});
    EXPECT_EQ(dirty.lights,std::vector<size_t>{0});
    EXPECT_DOUBLE_EQ(loaded.nodes[size_t(retainedNode)].translation[0],.75);
    EXPECT_DOUBLE_EQ(loaded.materials[1].pbrMetallicRoughness.metallicFactor,.75);
    EXPECT_DOUBLE_EQ(loaded.lights[0].intensity,.75);
    EXPECT_EQ(loaded.materials[1].pbrMetallicRoughness.baseColorTexture.index,0);
  }
  EXPECT_EQ(source,original);
}

TEST(GltfSceneExport, BehaviorCompactionRejectsDeletedReferencesAndChangedDynamicDomains) {
  using namespace container::geometry::gltf;
  const auto cube = Model::MakeCube();
  for (const std::string path : {"/nodes/0/translation/0","/nodes/{node}/translation/0"}) {
    auto source = behaviorExportSource(cube);
    scalarBehavior(source,{path});
    const auto original = source;
    EXPECT_THROW((void)CompactGltfExportNodes(source,std::array<int32_t,1>{0}),std::exception);
    EXPECT_EQ(source,original);
  }
  for (const std::string path : {"/materials/0/pbrMetallicRoughness/metallicFactor",
                                "/materials/{node}/pbrMetallicRoughness/metallicFactor"}) {
    auto source = behaviorExportSource(cube);
    scalarBehavior(source,{path});
    const auto original = source;
    EXPECT_THROW((void)PlanGltfMaterialExportCompaction(source,std::array<int32_t,1>{0}),std::exception);
    EXPECT_EQ(source,original);
  }
}

std::filesystem::path texturedSource(const std::filesystem::path& directory,
                                    const Model& cube) {
  std::filesystem::create_directories(directory);
  auto graph = cubeGraph();
  auto source = buildExportModel({graph, cube});
  source.buffers.front().uri = "source.bin";
  tinygltf::Image image;
  image.name = "External PNG";
  image.uri = "albedo.png";
  source.images.push_back(image);
  tinygltf::Texture texture;
  texture.source = 0;
  texture.sampler = 0;
  source.textures.push_back(texture);
  tinygltf::Sampler sampler;
  sampler.wrapS = TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE;
  sampler.wrapT = TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT;
  source.samplers.push_back(sampler);
  source.materials[0].name = "Paint";
  source.materials[0].pbrMetallicRoughness.baseColorTexture.index = 0;
  source.materials[0].pbrMetallicRoughness.baseColorTexture.texCoord = 1;
  source.materials[0].pbrMetallicRoughness.roughnessFactor = 0.73;
  tinygltf::Value::Object transform;
  transform["rotation"] = tinygltf::Value(0.5);
  source.materials[0].pbrMetallicRoughness.baseColorTexture.extensions[
      "KHR_texture_transform"] = tinygltf::Value(std::move(transform));
  source.extensionsUsed.push_back("KHR_texture_transform");
  source.extensionsUsed.push_back("KHR_lights_punctual");
  tinygltf::Light light;
  light.name = "Source lamp";
  light.type = "point";
  light.intensity = 23.0;
  light.color = {0.25, 0.5, 1.0};
  source.lights.push_back(light);
  source.nodes[0].light = 0;
  tinygltf::Camera camera;
  camera.name = "Source camera";
  camera.type = "perspective";
  camera.perspective.yfov = 0.75;
  camera.perspective.znear = 0.1;
  source.cameras.push_back(camera);
  source.nodes[0].camera = 0;
  tinygltf::Value::Object metadata;
  metadata["authoringNote"] = tinygltf::Value(std::string("retain this note"));
  source.nodes[0].extras = tinygltf::Value(std::move(metadata));

  // Valid one-pixel PNG, kept external in the imported asset.
  std::vector<unsigned char> png;
  std::string mime;
  if (!tinygltf::DecodeDataURI(&png, mime,
      "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwC"
      "AAAAC0lEQVR42mP8/x8AAwMCAO+aP1sAAAAASUVORK5CYII=", 0, false))
    throw std::runtime_error("test PNG decoding failed");
  std::ofstream imageFile(directory / "albedo.png", std::ios::binary);
  imageFile.write(reinterpret_cast<const char*>(png.data()),
                  static_cast<std::streamsize>(png.size()));
  imageFile.close();
  const auto path = directory / "source.gltf";
  tinygltf::TinyGLTF writer;
  if (!writer.WriteGltfSceneToFile(&source, path.string(), false, false, true, false))
    throw std::runtime_error("test source writing failed");
  return path;
}

template <typename T>
int sourceAccessor(tinygltf::Model& model, const std::vector<T>& values,
                    int type, int component, bool normalized = false) {
  if (model.buffers.empty()) model.buffers.emplace_back();
  auto& bytes = model.buffers[0].data;
  while (bytes.size() % 4) bytes.push_back(0);
  tinygltf::BufferView view;
  view.buffer = 0;
  view.byteOffset = bytes.size();
  view.byteLength = values.size() * sizeof(T);
  const auto* begin = reinterpret_cast<const unsigned char*>(values.data());
  bytes.insert(bytes.end(), begin, begin + view.byteLength);
  tinygltf::Accessor accessor;
  accessor.bufferView = static_cast<int>(model.bufferViews.size());
  accessor.type = type;
  accessor.componentType = component;
  accessor.normalized = normalized;
  accessor.count = values.size() / size_t(tinygltf::GetNumComponentsInType(type));
  if (type == TINYGLTF_TYPE_SCALAR && !values.empty()) {
    const auto [minimum, maximum] = std::minmax_element(values.begin(), values.end());
    accessor.minValues = {double(*minimum)};
    accessor.maxValues = {double(*maximum)};
  }
  model.bufferViews.push_back(view);
  model.accessors.push_back(accessor);
  return static_cast<int>(model.accessors.size() - 1);
}

void sourceTrack(tinygltf::Model& source, int node, const std::string& property,
                  int type, std::vector<float> values, const std::string& pointer = {},
                  size_t clip = 0) {
  if (source.animations.size() <= clip) source.animations.resize(clip + 1);
  auto& animation = source.animations[clip];
  tinygltf::AnimationSampler sampler;
  sampler.input = sourceAccessor(source, std::vector<float>{0, 2}, TINYGLTF_TYPE_SCALAR,
                                 TINYGLTF_COMPONENT_TYPE_FLOAT);
  sampler.output = sourceAccessor(source, values, type, TINYGLTF_COMPONENT_TYPE_FLOAT);
  sampler.interpolation = "LINEAR";
  sampler.extras = tinygltf::Value(tinygltf::Value::Object{{"retainSampler", tinygltf::Value(true)}});
  tinygltf::AnimationChannel channel;
  channel.sampler = static_cast<int>(animation.samplers.size());
  channel.target_node = node;
  channel.target_path = property;
  if (!pointer.empty()) {
    channel.target_node = -1;
    channel.target_path = "pointer";
    channel.target_extensions["KHR_animation_pointer"] = tinygltf::Value(
        tinygltf::Value::Object{{"pointer", tinygltf::Value(pointer)}});
  }
  animation.samplers.push_back(sampler);
  animation.channels.push_back(channel);
}

glm::mat4 nodeMatrix(const tinygltf::Node& node) {
  glm::mat4 matrix(1);
  if (node.matrix.size() == 16) {
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r) matrix[c][r] = float(node.matrix[size_t(c * 4 + r)]);
  } else {
    if (!node.translation.empty()) matrix = glm::translate(matrix,
        glm::vec3(node.translation[0], node.translation[1], node.translation[2]));
    if (!node.rotation.empty()) matrix *= glm::mat4_cast(glm::quat(float(node.rotation[3]),
        float(node.rotation[0]), float(node.rotation[1]), float(node.rotation[2])));
    if (!node.scale.empty()) matrix = glm::scale(matrix,
        glm::vec3(node.scale[0], node.scale[1], node.scale[2]));
  }
  return matrix;
}

std::vector<glm::mat4> worldMatrices(const tinygltf::Model& model) {
  std::vector<glm::mat4> worlds(model.nodes.size(), glm::mat4(1));
  std::function<void(int, const glm::mat4&)> visit = [&](int index, const glm::mat4& parent) {
    worlds[size_t(index)] = parent * nodeMatrix(model.nodes[size_t(index)]);
    for (const auto child : model.nodes[size_t(index)].children) visit(child, worlds[size_t(index)]);
  };
  for (const auto root : model.scenes[size_t(model.defaultScene)].nodes) visit(root, glm::mat4(1));
  return worlds;
}

struct AnimatedExportFixture {
  Model geometry;
  tinygltf::Model source;
  SceneGraph graph;
  std::vector<int32_t> nodes{-1, 0, -1, -1, 1, -1, -1, 2, 3};
  std::vector<tinygltf::Material> materials{3};
  std::vector<int32_t> materialMapping{2};
  glm::mat4 correction{1};

  AnimatedExportFixture() {
    const auto cube = Model::MakeCube();
    auto originalGraph = cubeGraph();
    source = buildExportModel({originalGraph, cube});
    source.nodes.assign(4, tinygltf::Node{});
    source.scenes[0].nodes = {0, 1, 2, 3};
    source.meshes[0].primitives.push_back(source.meshes[0].primitives[0]);
    source.meshes[0].weights = {0.1};
    const auto count = cube.meshes()[0].vertices().size();
    std::vector<uint8_t> joints(count * 4, 0), weights(count * 4, 0);
    for (size_t i = 0; i < count; ++i) weights[i * 4] = 255;
    const auto jointAccessor = sourceAccessor(source, joints, TINYGLTF_TYPE_VEC4,
                                               TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE);
    const auto weightAccessor = sourceAccessor(source, weights, TINYGLTF_TYPE_VEC4,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE, true);
    std::vector<float> delta(count * 3, 0);
    for (size_t i = 0; i < count; ++i) delta[i * 3 + 2] = 2;
    const auto morphAccessor = sourceAccessor(source, delta, TINYGLTF_TYPE_VEC3,
                                               TINYGLTF_COMPONENT_TYPE_FLOAT);
    source.accessors[size_t(morphAccessor)].minValues = {0, 0, 2};
    source.accessors[size_t(morphAccessor)].maxValues = {0, 0, 2};
    for (auto& primitive : source.meshes[0].primitives) {
      primitive.attributes["JOINTS_0"] = jointAccessor;
      primitive.attributes["WEIGHTS_0"] = weightAccessor;
      primitive.targets = {{{"POSITION", morphAccessor}}};
    }
    const auto inverseBind = sourceAccessor(source,
        std::vector<float>{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},
        TINYGLTF_TYPE_MAT4, TINYGLTF_COMPONENT_TYPE_FLOAT);
    for (int instance = 0; instance < 2; ++instance) {
      tinygltf::Skin skin;
      skin.name = "Skin " + std::to_string(instance);
      skin.joints = {instance + 2};
      skin.skeleton = instance + 2;
      skin.inverseBindMatrices = inverseBind;
      source.skins.push_back(skin);
      source.nodes[size_t(instance)].mesh = 0;
      source.nodes[size_t(instance)].skin = instance;
      source.nodes[size_t(instance)].weights = {instance == 0 ? 0.25 : 0.75};
    }
    source.nodes[0].translation = {0, 0.5, 0};
    source.nodes[1].translation = {3, 0, 0};
    source.nodes[2].translation = {0.5, 0, 0};
    source.nodes[3].translation = {2, 0, 0};
    source.lights.emplace_back();
    source.lights[0].type = "point";
    source.lights[0].intensity = 4;
    source.lights[0].range = 10;
    source.nodes[0].light = 0;
    source.cameras.emplace_back();
    source.cameras[0].type = "perspective";
    source.cameras[0].perspective.znear = 0.1;
    source.cameras[0].perspective.yfov = 0.75;
    source.nodes[1].camera = 0;
    sourceTrack(source, 0, "translation", TINYGLTF_TYPE_VEC3, {0,0,0, 0,2,0});
    sourceTrack(source, 2, "translation", TINYGLTF_TYPE_VEC3, {0,0,0, 2,0,0});
    sourceTrack(source, 0, "weights", TINYGLTF_TYPE_SCALAR, {0, 1});
    sourceTrack(source, -1, "pointer", TINYGLTF_TYPE_SCALAR, {0.5f, 1.5f}, "/nodes/1/weights/0");
    sourceTrack(source, -1, "pointer", TINYGLTF_TYPE_SCALAR, {0.2f, 0.8f}, "/materials/0/pbrMetallicRoughness/roughnessFactor");
    sourceTrack(source, -1, "pointer", TINYGLTF_TYPE_SCALAR, {2, 10}, "/extensions/KHR_lights_punctual/lights/0/intensity");
    sourceTrack(source, -1, "pointer", TINYGLTF_TYPE_SCALAR, {0.5f, 1.5f}, "/cameras/0/perspective/yfov");
    source.animations[0].samplers[0].interpolation = "CUBICSPLINE";
    source.animations[0].samplers[0].output = sourceAccessor(source,
        std::vector<float>{0,0,0, 0,0,0, 0,0,0, 0,0,0, 0,2,0, 0,0,0},
        TINYGLTF_TYPE_VEC3, TINYGLTF_COMPONENT_TYPE_FLOAT);
    source.animations[0].samplers[1].interpolation = "STEP";
    source.animations[0].name = "Preserved clip";
    source.animations[0].extras = tinygltf::Value(tinygltf::Value::Object{{"authoringNote", tinygltf::Value(true)}});
    source.extensionsUsed = {"KHR_animation_pointer", "KHR_lights_punctual"};
    auto meshes = cube.meshes();
    meshes.push_back(cube.meshes()[0]);
    geometry = Model::FromMeshes(std::move(meshes));
    materials[2] = source.materials[0];
    materials[2].pbrMetallicRoughness.roughnessFactor = 0.35;
    correction[1][0] = 0.3f;
    graph.createNode(glm::mat4(1), 2, false, SceneGraph::kInvalidNode, "Editor root");
    for (int instance = 0; instance < 2; ++instance) {
      const auto owner = graph.createNode(nodeMatrix(source.nodes[size_t(instance)]), 2,
                                           false, SceneGraph::kInvalidNode, "Instance " + std::to_string(instance));
      graph.setParent(owner, 0);
      graph.getNode(owner)->sourceNodeIndex = instance;
      if (instance == 0) graph.setLocalTransform(owner, correction * graph.getNode(owner)->localTransform);
      for (uint32_t primitive = 0; primitive < 2; ++primitive) {
        const auto child = graph.createNode(glm::mat4(1), 2, true, primitive, "Primitive");
        graph.getNode(child)->meshOwnerGraphNode = owner;
        graph.setParent(child, owner);
      }
    }
    for (int joint = 2; joint < 4; ++joint) {
      const auto node = graph.createNode(nodeMatrix(source.nodes[size_t(joint)]), 2,
                                          false, SceneGraph::kInvalidNode, "Joint");
      graph.getNode(node)->sourceNodeIndex = joint;
      graph.setParent(node, 0);
    }
    graph.updateWorldTransforms();
  }

  SceneExportSnapshot snapshot() const {
    return {graph, geometry, &source, {}, nodes, materials, materialMapping};
  }
};

struct LinkedSkinFixture {
  ExportDirectory directory;
  std::filesystem::path input{directory.root / "linked"};
  std::filesystem::path leaf{input / "leaf.gltf"};
  std::filesystem::path primary{input / "primary.gltf"};
  Model geometry{Model::MakeCube()};
  tinygltf::Model child{buildExportModel({cubeGraph(), geometry})};
  size_t primaryAccessors{};

  LinkedSkinFixture() {
    using Json = nlohmann::json;
    std::filesystem::create_directories(input / "group");
    child.nodes.assign(5, tinygltf::Node{});
    child.nodes[0].name = "Inactive leaf node";
    child.nodes[1].name = "Leaf rig";
    child.nodes[1].translation = {.25, .1, 0};
    child.nodes[1].children = {2, 3};
    child.nodes[2].name = "Leaf mesh";
    child.nodes[2].mesh = 0;
    child.nodes[2].skin = 0;
    child.nodes[2].weights = {.25};
    child.nodes[3].name = "Leaf root joint";
    child.nodes[3].children = {4};
    child.nodes[4].name = "Leaf child joint";
    child.nodes[4].translation = {0, .5, 0};
    child.scenes.resize(2);
    child.scenes[0].nodes = {0};
    child.scenes[1].nodes = {1};
    child.defaultScene = 1;
    child.materials[0].name = "Linked material";
    child.materials[0].pbrMetallicRoughness.roughnessFactor = .35;
    auto& primitive = child.meshes[0].primitives[0];
    const size_t vertices = geometry.meshes()[0].vertices().size();
    std::vector<uint8_t> joints(vertices * 4), weights(vertices * 4);
    std::vector<float> positions(vertices * 3);
    for (size_t i = 0; i < vertices; ++i) {
      joints[i * 4 + 1] = 1;
      weights[i * 4] = 192;
      weights[i * 4 + 1] = 63;
      positions[i * 3 + 2] = 1.25f;
    }
    primitive.attributes["JOINTS_0"] = sourceAccessor(child, joints,
        TINYGLTF_TYPE_VEC4, TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE);
    primitive.attributes["WEIGHTS_0"] = sourceAccessor(child, weights,
        TINYGLTF_TYPE_VEC4, TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE, true);
    const int morph = sourceAccessor(child, positions, TINYGLTF_TYPE_VEC3, TINYGLTF_COMPONENT_TYPE_FLOAT);
    child.accessors[size_t(morph)].minValues = {0, 0, 1.25};
    child.accessors[size_t(morph)].maxValues = {0, 0, 1.25};
    primitive.targets = {{{"POSITION", morph}}};
    child.meshes[0].weights = {.1};
    tinygltf::Skin skin;
    skin.name = "Leaf skin";
    skin.skeleton = 3;
    skin.joints = {3, 4};
    skin.inverseBindMatrices = sourceAccessor(child,
        std::vector<float>{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1,
                           1,0,0,0, 0,1,0,0, 0,0,1,0, 0,-.5f,0,1},
        TINYGLTF_TYPE_MAT4, TINYGLTF_COMPONENT_TYPE_FLOAT);
    child.skins.push_back(std::move(skin));
    const auto turn = glm::angleAxis(1.25f, glm::vec3(0, 0, 1));
    sourceTrack(child, 3, "rotation", TINYGLTF_TYPE_VEC4,
        {0, 0, 0, 1, turn.x, turn.y, turn.z, turn.w});
    sourceTrack(child, 2, "weights", TINYGLTF_TYPE_SCALAR, {.1f, .9f});
    sourceTrack(child, -1, "pointer", TINYGLTF_TYPE_SCALAR, {.5f, 1.25f}, "/nodes/4/translation/1");
    sourceTrack(child, -1, "pointer", TINYGLTF_TYPE_SCALAR, {.2f, .8f},
        "/materials/0/pbrMetallicRoughness/roughnessFactor");
    child.animations[0].name = "Linked gesture";
    child.extensionsUsed.push_back("KHR_animation_pointer");
    writeExportModel(child, leaf);

    writeText(input / "group" / "nested.gltf", R"({"asset":{"version":"2.1"},
      "files":[{"uri":"../leaf.gltf","mimeType":"model/gltf+json"},
               {"uri":"missing-inactive.gltf","mimeType":"model/gltf+json"}],
      "externalAssets":[{"file":0},{"file":1}],
      "nodes":[{"externalAsset":1,"name":"Inactive nested placement"},
               {"externalAsset":0,"translation":[0,0,-2]}],
      "scenes":[{"nodes":[0]},{"nodes":[1]}],"scene":1})");
    auto base = buildExportModel({cubeGraph(), geometry});
    base.materials[0].name = "Primary material";
    base.materials[0].pbrMetallicRoughness.roughnessFactor = .93;
    primaryAccessors = base.accessors.size();
    writeExportModel(base, primary);
    auto manifest = Json::parse(readText(primary));
    manifest["asset"]["version"] = "2.1";
    manifest["files"] = Json::array({{{"uri", "group/nested.gltf"}, {"mimeType", "model/gltf+json"}},
                                    {{"uri", "leaf.gltf"}, {"mimeType", "model/gltf+json"}}});
    manifest["externalAssets"] = Json::array({{{"file", 0}}, {{"file", 1}}});
    for (const auto& [name, link, translation] :
         std::array{std::tuple{"Left rig", 0, std::array{-3, 0, 0}},
                    std::tuple{"Right rig", 0, std::array{3, 0, 0}},
                    std::tuple{"Direct rig", 1, std::array{0, 4, 0}}}) {
      manifest["scenes"][0]["nodes"].push_back(manifest["nodes"].size());
      manifest["nodes"].push_back({{"name", name}, {"externalAsset", link}, {"translation", translation}});
    }
    writeText(primary, manifest.dump());
  }
};

}  // namespace

TEST(GltfSceneExport, RoundTripsCurrentHierarchyTransformsVisibilityAndAppendedGeometry) {
  ExportDirectory directory;
  const auto cube = Model::MakeCube();
  const auto sphere = Model::MakeSphere(8, 12);
  auto meshes = cube.meshes();
  meshes.insert(meshes.end(), sphere.meshes().begin(), sphere.meshes().end());
  const auto geometry = Model::FromMeshes(std::move(meshes));
  auto graph = cubeGraph();
  const auto parentTransform = glm::translate(glm::mat4(1), glm::vec3(2, 3, -4)) *
      glm::rotate(glm::mat4(1), 0.3f, glm::vec3(0, 1, 0));
  const auto childTransform = glm::translate(glm::mat4(1), glm::vec3(-1, 2, 3)) *
      glm::scale(glm::mat4(1), glm::vec3(-2, 3, 4));
  graph.setLocalTransform(0, parentTransform);
  graph.setLocalTransform(1, childTransform);
  graph.setName(0, "Edited parent");
  graph.setName(1, "Edited cube");
  graph.setVisible(0, false);
  const auto appended = graph.createNode(glm::mat4(1), 0, true, 1, "Added sphere");
  ASSERT_TRUE(graph.setParent(appended, 1));
  graph.updateWorldTransforms();
  const auto revision = graph.revision();
  const auto identity = graph.identity();

  for (const auto* suffix : {".gltf", ".glb"}) {
    const auto path = directory.root / (std::string("edited") + suffix);
    ASSERT_NO_THROW(writeSceneExport({graph, geometry}, path));
    const auto loaded = LoadModelWithSource(path.string());
    const auto& output = loaded.gltfModel;
    ASSERT_EQ(output.nodes.size(), 3u);
    EXPECT_EQ(output.nodes[0].name, "Edited parent");
    EXPECT_EQ(output.nodes[1].name, "Edited cube");
    EXPECT_EQ(output.nodes[2].name, "Added sphere");
    EXPECT_EQ(output.nodes[0].children, std::vector<int>{1});
    EXPECT_EQ(output.nodes[1].children, std::vector<int>{2});
    EXPECT_EQ(output.scenes[0].nodes, std::vector<int>{0});
    expectMatrix(output.nodes[0], parentTransform);
    expectMatrix(output.nodes[1], childTransform);
    ASSERT_TRUE(output.nodes[0].extensions.contains("KHR_node_visibility"));
    EXPECT_FALSE(output.nodes[0].extensions.at("KHR_node_visibility")
                     .Get("visible").Get<bool>());
    EXPECT_FALSE(output.nodes[1].extensions.contains("KHR_node_visibility"));
    EXPECT_EQ(loaded.model.meshes().size(), 2u);
    EXPECT_EQ(loaded.model.vertices().size(), geometry.vertices().size());
    EXPECT_EQ(loaded.model.indices().size(), geometry.indices().size());
  }
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(graph.identity(), identity);
  EXPECT_FALSE(graph.getNode(0)->visible);
  EXPECT_EQ(graph.getNode(1)->worldTransform, parentTransform * childTransform);
}

TEST(GltfSceneExport, XmpPacketsRetainDistinctContextsAndLosslessValuesWithVariants) {
  using namespace container::geometry::gltf;
  using Json = nlohmann::json;
  ExportDirectory directory;
  const auto input = directory.root / "xmp-source.gltf";
  const auto cube = Model::MakeCube();
  const auto graph = cubeGraph();
  writeSceneExport({graph, cube}, input);
  auto manifest = Json::parse(readText(input));
  const auto variants = "KHR_materials_variants";
  manifest["extensionsUsed"] = Json::array({kGltfXmpExtension, variants});
  manifest["extensions"][variants] = Json::parse(R"({"variants":[{"name":"Paint"}]})");
  manifest["meshes"][0]["primitives"][0]["extensions"][variants] =
      Json::parse(R"({"mappings":[{"material":0,"variants":[0]}]})");
  manifest["extensions"]["VENDOR_unrelated"] = {{"extra", true}};
  const auto package = Json::parse(R"({"packets":[
    {"@context":{"dc":"http://purl.org/dc/elements/1.1/"},"dc:title":"Whole asset",
     "dc:creator":{"@list":["A","B"]},"dc:identifier":18446744073709551614},
    {"@context":{"dc":"http://purl.org/dc/elements/1.1/","rdf":"http://www.w3.org/1999/02/22-rdf-syntax-ns#"},
     "dc:title":{"@type":"rdf:Alt","rdf:_1":{"@language":"fr","@value":"Nœud"}}},
    {"@context":{"dc":"http://purl.org/dc/elements/1.1/"},"dc:title":"Mesh",
     "dc:contributor":{"@set":[]},"dc:description":null},
    {"@context":{"dc":"http://purl.org/dc/elements/1.1/"},"dc:title":"Paint"},
    {"@context":{"dc":"http://purl.org/dc/elements/1.1/"},"dc:title":"Scene"}
  ]})");
  manifest["extensions"][kGltfXmpExtension] = package;
  const auto bind = [](Json& record, Json packet) {record["extensions"][kGltfXmpExtension] = {{"packet", packet}};};
  bind(manifest["asset"], 0);
  bind(manifest["nodes"][1], 1.0);
  bind(manifest["meshes"][0], 2.0);
  bind(manifest["materials"][0], 3);
  manifest["materials"][0]["extensions"][kGltfXmpExtension]["extras"] =
      {{"previewTexture", {{"index", 42}}}};
  bind(manifest["scenes"][0], 4);
  writeText(input, manifest.dump());
  const auto check = [&](const tinygltf::Model& model) {
    const auto retained = ReadGltfXmpPackage(model);
    ASSERT_TRUE(retained);
    EXPECT_EQ(*retained, package);
    ASSERT_EQ(model.nodes.size(), 2u);
    ASSERT_EQ(model.meshes.size(), 1u);
    ASSERT_EQ(model.materials.size(), 1u);
    ASSERT_EQ(model.scenes.size(), 1u);
    const std::array contexts{&model.asset.extensions, &model.nodes[1].extensions,
        &model.meshes[0].extensions, &model.materials[0].extensions, &model.scenes[0].extensions};
    for (size_t i = 0; i < contexts.size(); ++i) {
      const auto packet = ResolveGltfXmpPacket(*retained, *contexts[i]);
      ASSERT_NE(packet.packet, nullptr);
      EXPECT_EQ(*packet.packet, package["packets"][i]);
    }
    EXPECT_TRUE(model.extensions.contains("VENDOR_unrelated"));
    EXPECT_TRUE(model.meshes[0].primitives[0].extensions.contains(variants));
    using Value = tinygltf::Value;
    const Value extras(Value::Object{{"previewTexture", Value(Value::Object{{"index", Value(42)}})}});
    EXPECT_EQ(model.materials[0].extensions.at(kGltfXmpExtension).Get("extras"), extras);
    EXPECT_EQ(ResolveGltfXmpPacket(*retained, model.nodes[0].extensions).message,
              "No XMP packet is attached.");
  };
  const auto source = LoadModelWithSource(input.string());
  EXPECT_EQ(std::ranges::count(source.capabilityDiagnostics, "xmp_metadata", &CapabilityDiagnostic::code), 0);
  check(source.gltfModel);
  tinygltf::Model archive;
  const auto materials = appendExportResources(archive, source.gltfModel, input);
  ASSERT_EQ(materials.size(), 1u);
  EXPECT_EQ(materials[0].extensions.at(kGltfXmpExtension),
            source.gltfModel.materials[0].extensions.at(kGltfXmpExtension));
  const std::array<int32_t, 2> nodeIdentities{0, 1};
  for (const auto* format : {".gltf", ".glb"}) {
    const auto output = directory.root / (std::string("xmp-round-trip") + format);
    writeSceneExport({graph, source.model, &source.gltfModel, input, nodeIdentities}, output);
    check(LoadModelWithSource(output.string()).gltfModel);
  }
}

TEST(GltfSceneExport, MalformedXmpReportsWarningsAndKeepsPacketIndicesStable) {
  using namespace container::geometry::gltf;
  using Json = nlohmann::json;
  ExportDirectory directory;
  const auto path = directory.root / "xmp-malformed.gltf";
  writeSceneExport({cubeGraph(), Model::MakeCube()}, path);
  auto manifest = Json::parse(readText(path));
  manifest["extensionsUsed"] = Json::array({kGltfXmpExtension});
  manifest["extensions"][kGltfXmpExtension] = Json::parse(R"({"packets":[null,{"@context":{},"xmp:Label":"second slot"}]})");
  manifest["asset"]["extensions"][kGltfXmpExtension] = {{"packet", 0}};
  manifest["nodes"][0]["extensions"][kGltfXmpExtension] = {{"packet", 0.5}};
  manifest["nodes"][1]["extensions"][kGltfXmpExtension] = {{"packet", uint64_t(4294967297)}};
  manifest["meshes"][0]["extensions"][kGltfXmpExtension] = {{"packet", 1}};
  manifest["materials"][0]["extensions"][kGltfXmpExtension] = {{"packet", 9}};
  manifest["scenes"][0]["extensions"][kGltfXmpExtension] = false;
  writeText(path, manifest.dump());
  const auto result = LoadModelWithSource(path.string());
  const auto& model = result.gltfModel;
  const auto package = ReadGltfXmpPackage(model);
  ASSERT_TRUE(package);
  EXPECT_EQ(std::ranges::count(result.capabilityDiagnostics, "xmp_metadata", &CapabilityDiagnostic::code), 6);
  const auto mesh = ResolveGltfXmpPacket(*package, model.meshes[0].extensions);
  ASSERT_NE(mesh.packet, nullptr);
  EXPECT_EQ(mesh.packet->at("xmp:Label"), "second slot");
  EXPECT_TRUE(ResolveGltfXmpPacket(*package, model.asset.extensions).message.contains("@context"));
  EXPECT_TRUE(ResolveGltfXmpPacket(*package, model.nodes[0].extensions).message.contains("integer"));
  EXPECT_TRUE(ResolveGltfXmpPacket(*package, model.nodes[1].extensions).message.contains("outside"));
  EXPECT_TRUE(ResolveGltfXmpPacket(*package, model.materials[0].extensions).message.contains("outside"));
  EXPECT_TRUE(ResolveGltfXmpPacket(*package, model.scenes[0].extensions).message.contains("integer"));
  manifest.erase("extensions");
  writeText(path, manifest.dump());
  const auto withoutRoot = LoadModelWithSource(path.string());
  EXPECT_FALSE(ReadGltfXmpPackage(withoutRoot.gltfModel));
  EXPECT_TRUE(ResolveGltfXmpPacket({}, withoutRoot.gltfModel.meshes[0].extensions).message.contains("root extension"));
}

TEST(GltfSceneExport, LinkedDraftSharesNestedSourceAndAliasResourcesAndFlattensPortably) {
  using namespace container::geometry::gltf;
  using Json = nlohmann::json;
  ExportDirectory directory;
  const auto input = directory.root / "linked";
  std::filesystem::create_directories(input / "leaf");
  std::filesystem::create_directories(input / "group");
  const auto leaf = input / "leaf" / "Mesh part.gltf";
  writeSceneExport({cubeGraph(), Model::MakeCube()}, leaf);
  auto leafJson = Json::parse(readText(leaf));
  leafJson["asset"]["version"] = "2.1";
  leafJson["asset"]["minVersion"] = "2.1";
  leafJson["extensionsUsed"].push_back("KHR_interactivity");
  leafJson["extensionsUsed"].push_back("VENDOR_optional");
  leafJson["extensionsUsed"].push_back(kGltfXmpExtension);
  leafJson["extensions"]["KHR_interactivity"] = Json::parse(R"({
    "graphs":[{"declarations":[{"op":"event/onStart"}],"nodes":[{"declaration":0}]}]
  })");
  leafJson["meshes"][0]["primitives"][0]["extensions"]["VENDOR_optional"] = {{"accessor", 500}};
  const auto leafPackets = Json::parse(R"([
    {"@context":{},"label":"Linked asset"},
    {"@context":{},"label":"Linked scene"},
    {"@context":{},"label":"Linked mesh","id":18446744073709551614}])");
  leafJson["extensions"][kGltfXmpExtension] = {{"packets", leafPackets}};
  leafJson["asset"]["extensions"][kGltfXmpExtension] = {{"packet", 0}};
  leafJson["scenes"][0]["extensions"][kGltfXmpExtension] = {{"packet", 1}};
  leafJson["meshes"][0]["extensions"][kGltfXmpExtension] = {{"packet", 2}};
  std::filesystem::copy_file(leaf.parent_path() / leafJson["buffers"][0]["uri"].get<std::string>(),
      input / "Shared mesh.bin");
  leafJson["buffers"][0]["uri"] = "missing%20geometry.bin";
  writeText(leaf, leafJson.dump());

  const auto gltfFile = [](std::string uri) { return Json{{"uri", std::move(uri)}, {"mimeType", "model/gltf+json"}}; };
  auto nestedFile = gltfFile("../leaf/Mesh%20part.gltf");
  nestedFile["aliases"] = Json::array({{{"alias", "missing%20geometry.bin"}, {"file", 1}}});
  const Json nested = {{"asset", {{"version", "2.1"}, {"minVersion", "2.1"}}},
      {"files", Json::array({nestedFile, {{"uri", "../Shared%20mesh.bin"}, {"mimeType", "application/octet-stream"}}})},
      {"externalAssets", Json::array({{{"file", 0}}})},
      {"nodes", Json::array({{{"externalAsset", 0}, {"translation", {0, 0, -2}}}})},
      {"scenes", Json::array({{{"nodes", {0}}}})}, {"scene", 0}};
  writeText(input / "group" / "nested.gltf", nested.dump());

  auto direct = gltfFile("leaf/Mesh%20part.gltf");
  direct["aliases"] = Json::array({{{"alias", "./missing%20geometry.bin"}, {"file", 2}}});
  Json primary = {{"asset", {{"version", "2.1"}}},
      {"files", Json::array({gltfFile("group/nested.gltf"), direct,
          {{"uri", "Shared%20mesh.bin"}, {"mimeType", "application/octet-stream"}}})},
      {"externalAssets", Json::array({{{"file", 0}}, {{"file", 1}}})},
      {"nodes", Json::array({{{"externalAsset", 0}, {"translation", {-3, 0, 0}}},
          {{"externalAsset", 0}, {"translation", {3, 0, 0}}},
          {{"externalAsset", 1}, {"translation", {0, 5, 0}}}})},
      {"scenes", Json::array({{{"nodes", {0, 1, 2}}}})}, {"scene", 0}};
  const auto primaryPackets = Json::parse(R"([
    {"@context":{},"label":"Primary asset"},{"@context":{},"label":"Primary placement"}])");
  primary["extensionsUsed"] = {kGltfXmpExtension};
  primary["extensions"][kGltfXmpExtension] = {{"packets", primaryPackets}};
  primary["asset"]["extensions"][kGltfXmpExtension] = {{"packet", 0}};
  primary["nodes"][0]["extensions"][kGltfXmpExtension] = {{"packet", 1}};
  const auto path = input / "primary.gltf";
  writeText(path, primary.dump());
  EXPECT_THROW((void)LoadModelWithSource(path.string()), std::runtime_error);
  LoadOptions options;
  options.enableLinkedAssets = true;
  const auto result = LoadModelWithSource(path.string(), options);
  const auto& source = result.gltfModel;
  ASSERT_EQ(source.nodes.size(), 17u);
  ASSERT_EQ(source.meshes.size(), 1u);
  ASSERT_EQ(source.buffers.size(), 1u);
  ASSERT_EQ(result.model.meshes().size(), 1u);
  ASSERT_EQ(source.scenes.size(), 1u);
  EXPECT_EQ(source.asset.version, "2.0");
  EXPECT_FALSE(source.extensions.contains("KHR_interactivity"));
  EXPECT_FALSE(source.meshes[0].primitives[0].extensions.contains("VENDOR_optional"));
  for (const std::string_view feature : {"KHR_interactivity", "VENDOR_optional"}) {
    EXPECT_TRUE(std::ranges::any_of(result.capabilityDiagnostics, [&](const auto& diagnostic) {
      return std::filesystem::path(diagnostic.assetPath).filename() == leaf.filename() &&
          diagnostic.code == "unsupported_optional_extension" && diagnostic.message.contains(feature);
    }));
  }
  const std::vector<glm::vec3> expected{{-3, 0, -2}, {3, 0, -2}, {0, 5, 0}};
  const auto check = [&](const tinygltf::Model& model) {
    const auto matrices = worldMatrices(model);
    const auto package = ReadGltfXmpPackage(model);
    ASSERT_TRUE(package);
    auto packets = primaryPackets;
    for (const auto& packet : leafPackets) packets.push_back(packet);
    EXPECT_EQ(package->at("packets"), packets);
    const auto primaryAsset = ResolveGltfXmpPacket(*package, model.asset.extensions);
    ASSERT_NE(primaryAsset.packet, nullptr);
    EXPECT_EQ(*primaryAsset.packet, primaryPackets[0]);
    const auto primaryPlacement = ResolveGltfXmpPacket(*package, model.nodes[0].extensions);
    ASSERT_NE(primaryPlacement.packet, nullptr);
    EXPECT_EQ(*primaryPlacement.packet, primaryPackets[1]);
    const auto linkedMesh = ResolveGltfXmpPacket(*package, model.meshes[0].extensions);
    ASSERT_NE(linkedMesh.packet, nullptr);
    EXPECT_EQ(*linkedMesh.packet, leafPackets[2]);
    std::array<size_t, 2> metadataCounts{};
    for (const auto& node : model.nodes) {
      if (!node.extras.Has("containerLinkedSource")) continue;
      const auto& origin = node.extras.Get("containerLinkedSource");
      if (!origin.Has("scope")) continue;
      const auto scope = origin.Get("scope").Get<std::string>();
      ASSERT_TRUE(scope == "asset" || scope == "scene");
      const auto index = scope == "asset" ? 0u : 1u;
      const auto packet = ResolveGltfXmpPacket(*package, node.extensions);
      ASSERT_NE(packet.packet, nullptr);
      EXPECT_EQ(*packet.packet, leafPackets[index]);
      ++metadataCounts[index];
    }
    EXPECT_EQ(metadataCounts, (std::array<size_t, 2>{3, 3}));
    size_t draw = 0;
    for (size_t i = 0; i < model.nodes.size(); ++i) if (model.nodes[i].mesh >= 0) {
      ASSERT_LT(draw, expected.size());
      EXPECT_EQ(model.nodes[i].mesh, 0);
      EXPECT_EQ(glm::vec3(matrices[i][3]), expected[draw++]);
      ASSERT_TRUE(model.nodes[i].extras.Has("containerLinkedSource"));
      const auto& origin = model.nodes[i].extras.Get("containerLinkedSource");
      EXPECT_EQ(origin.Get("uri").Get<std::string>(), "leaf/Mesh part.gltf");
      EXPECT_EQ(origin.Get("node").Get<int>(), 1);
    }
    EXPECT_EQ(draw, expected.size());
  };
  check(source);
  ASSERT_TRUE(result.linkedComposition);
  const auto& ownership = *result.linkedComposition;
  EXPECT_EQ(ownership.primaryNodes, 3u);
  EXPECT_EQ(ownership.placements, (std::vector<int>{0, 1, 2}));
  ASSERT_EQ(ownership.nodePlacements.size(), source.nodes.size());
  ASSERT_EQ(ownership.placementRecords.size(), 5u);
  ASSERT_EQ(ownership.sourceContexts.size(), 2u);
  ASSERT_EQ(source.nodes[0].children.size(), 1u);
  ASSERT_EQ(source.nodes[1].children.size(), 1u);
  const std::array carriers{0, 1, 2, source.nodes[0].children[0], source.nodes[1].children[0]};
  std::array<int32_t, 5> placementIndices{};
  size_t ownedNodes = 0;
  for (size_t i = 0; i < carriers.size(); ++i) {
    const auto placement = std::ranges::find(ownership.placementRecords, carriers[i],
        &GltfLinkedPlacementOwnership::carrierNode);
    ASSERT_NE(placement, ownership.placementRecords.end());
    placementIndices[i] = int32_t(placement - ownership.placementRecords.begin());
    EXPECT_EQ(ownership.nodePlacements.at(size_t(carriers[i])), placement->parentPlacement);
    auto payload = source.nodes.at(size_t(carriers[i])).children;
    ASSERT_FALSE(payload.empty());
    while (!payload.empty()) {
      const int child = payload.back();
      payload.pop_back();
      EXPECT_EQ(ownership.nodePlacements.at(size_t(child)), placementIndices[i]);
      ++ownedNodes;
      // A nested carrier belongs to this placement, but its descendants belong to the nested placement.
      if (std::ranges::find(carriers, child) != carriers.end()) continue;
      const auto& children = source.nodes.at(size_t(child)).children;
      payload.insert(payload.end(), children.begin(), children.end());
    }
  }
  EXPECT_EQ(ownedNodes, source.nodes.size() - ownership.primaryNodes);
  const auto& outer = ownership.placementRecords[size_t(placementIndices[0])];
  const auto& otherOuter = ownership.placementRecords[size_t(placementIndices[1])];
  const auto& directLeaf = ownership.placementRecords[size_t(placementIndices[2])];
  const auto& nestedLeaf = ownership.placementRecords[size_t(placementIndices[3])];
  const auto& otherNestedLeaf = ownership.placementRecords[size_t(placementIndices[4])];
  EXPECT_EQ(outer.parentPlacement, -1);
  EXPECT_EQ(otherOuter.parentPlacement, -1);
  EXPECT_EQ(directLeaf.parentPlacement, -1);
  EXPECT_EQ(nestedLeaf.parentPlacement, placementIndices[0]);
  EXPECT_EQ(otherNestedLeaf.parentPlacement, placementIndices[1]);
  EXPECT_EQ(outer.sourceContext, otherOuter.sourceContext);
  EXPECT_NE(outer.sourceContext, directLeaf.sourceContext);
  EXPECT_EQ(nestedLeaf.sourceContext, directLeaf.sourceContext);
  EXPECT_EQ(otherNestedLeaf.sourceContext, directLeaf.sourceContext);
  const auto& sharedLeaf = ownership.sourceContexts.at(directLeaf.sourceContext);
  EXPECT_EQ(sharedLeaf.source, std::filesystem::canonical(leaf));
  ASSERT_EQ(sharedLeaf.aliases.size(), 1u);
  EXPECT_EQ(sharedLeaf.aliases.at("missing geometry.bin"), std::filesystem::canonical(input / "Shared mesh.bin"));
  EXPECT_EQ(sharedLeaf.meshes.count, 1u);
  EXPECT_TRUE(sharedLeaf.meshes.contains(0));
  EXPECT_EQ(sharedLeaf.buffers.count, 1u);
  EXPECT_TRUE(sharedLeaf.buffers.contains(0));

  // The same leaf and buffer bytes still need separate ownership when the resolved alias differs.
  const auto alternateBuffer = input / "Alternate mesh.bin";
  std::filesystem::copy_file(input / "Shared mesh.bin", alternateBuffer);
  auto alternateLeaf = direct;
  alternateLeaf["aliases"][0]["file"] = 4;
  auto alternateManifest = primary;
  alternateManifest["files"].push_back(alternateLeaf);
  alternateManifest["files"].push_back({{"uri", "Alternate%20mesh.bin"}, {"mimeType", "application/octet-stream"}});
  alternateManifest["externalAssets"].push_back({{"file", 3}});
  alternateManifest["nodes"].push_back({{"externalAsset", 2}});
  alternateManifest["scenes"][0]["nodes"].push_back(3);
  const auto alternatePath = input / "alternate.gltf";
  writeText(alternatePath, alternateManifest.dump());
  const auto distinct = LoadModelWithSource(alternatePath.string(), options);
  ASSERT_TRUE(distinct.linkedComposition);
  ASSERT_EQ(distinct.linkedComposition->sourceContexts.size(), 3u);
  EXPECT_EQ(distinct.gltfModel.meshes.size(), 2u);
  const auto contextForCarrier = [&](int carrier) {
    const auto& records = distinct.linkedComposition->placementRecords;
    const auto entry = std::ranges::find(records, carrier, &GltfLinkedPlacementOwnership::carrierNode);
    return entry == records.end() ? distinct.linkedComposition->sourceContexts.size() : entry->sourceContext;
  };
  const auto originalContext = contextForCarrier(2), alternateContext = contextForCarrier(3);
  ASSERT_LT(originalContext, distinct.linkedComposition->sourceContexts.size());
  ASSERT_LT(alternateContext, distinct.linkedComposition->sourceContexts.size());
  EXPECT_NE(originalContext, alternateContext);
  const auto& originalOwner = distinct.linkedComposition->sourceContexts[originalContext];
  const auto& alternateOwner = distinct.linkedComposition->sourceContexts[alternateContext];
  EXPECT_EQ(originalOwner.source, alternateOwner.source);
  EXPECT_EQ(alternateOwner.aliases.at("missing geometry.bin"), std::filesystem::canonical(alternateBuffer));
  EXPECT_NE(originalOwner.meshes.first, alternateOwner.meshes.first);

  const auto linked = input / "saved-linked.gltf";
  writeLinkedSceneManifest(*result.linkedComposition, {}, 0, linked);
  const auto originalManifest = Json::parse(readText(path));
  const auto companion = Json::parse(readText(linked));
  EXPECT_EQ(companion, originalManifest);
  EXPECT_FALSE(companion.contains("meshes"));
  const auto linkedReload = LoadModelWithSource(linked.string(), options);
  check(linkedReload.gltfModel);
  EXPECT_EQ(linkedReload.gltfModel.meshes.size(), 1u);

  const std::array edits{GltfLinkedPlacementEdit{0, "Moved placement",
      std::array<double, 16>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -4, 0, 0, 1}}};
  writeLinkedSceneManifest(*result.linkedComposition, edits, 0, linked);
  const auto edited = Json::parse(readText(linked));
  EXPECT_EQ(edited["files"], originalManifest["files"]);
  EXPECT_EQ(edited["externalAssets"], originalManifest["externalAssets"]);
  EXPECT_EQ(edited["nodes"][0]["name"], "Moved placement");
  EXPECT_EQ(edited["nodes"][0]["externalAsset"], 0);
  EXPECT_FALSE(edited["nodes"][0].contains("translation"));
  EXPECT_EQ(Json::parse(readText(path)), originalManifest);
  const auto moved = LoadModelWithSource(linked.string(), options);
  const auto matrices = worldMatrices(moved.gltfModel);
  EXPECT_EQ(glm::vec3(matrices.at(0)[3]), glm::vec3(-4, 0, 0));

  const auto savedText = readText(linked);
  EXPECT_NO_THROW(ValidateGltfLinkedExportDestination(*result.linkedComposition, linked));
  EXPECT_THROW(writeLinkedSceneManifest(*result.linkedComposition, {}, 0, leaf), std::runtime_error);
  EXPECT_THROW(writeLinkedSceneManifest(*result.linkedComposition, {}, 0, path), std::runtime_error);
  EXPECT_THROW(writeLinkedSceneManifest(*result.linkedComposition, {}, 0, directory.root / "different.gltf"), std::runtime_error);
  const auto alias = input / "source-hard-link.gltf";
  std::error_code aliasError;
  std::filesystem::create_hard_link(path, alias, aliasError);
  if (!aliasError) EXPECT_THROW(ValidateGltfLinkedExportDestination(*result.linkedComposition, alias), std::runtime_error);
  const auto originalDependency = readText(input / "Shared mesh.bin");
  auto changedDependency = originalDependency;
  ASSERT_FALSE(changedDependency.empty());
  changedDependency[0] ^= 1;
  writeText(input / "Shared mesh.bin", changedDependency);
  EXPECT_THROW(writeLinkedSceneManifest(*result.linkedComposition, {}, 0, linked), std::runtime_error);
  EXPECT_EQ(readText(linked), savedText);
  writeText(input / "Shared mesh.bin", originalDependency);

  SceneGraph graph;
  std::vector<int32_t> identities;
  for (size_t i = 0; i < source.nodes.size(); ++i) {
    const auto& node = source.nodes[i];
    graph.createNode(nodeMatrix(node), 0, node.mesh >= 0,
        node.mesh >= 0 ? 0 : SceneGraph::kInvalidNode, node.name);
    identities.push_back(int32_t(i));
  }
  for (size_t i = 0; i < source.nodes.size(); ++i)
    for (const auto child : source.nodes[i].children) graph.setParent(uint32_t(child), uint32_t(i));
  graph.updateWorldTransforms();
  const auto flattened = directory.root / "flattened";
  std::filesystem::create_directories(flattened);
  for (const auto* format : {".gltf", ".glb"})
    writeSceneExport({graph, result.model, &source, path, identities},
        flattened / (std::string("scene") + format));
  std::filesystem::rename(input, directory.root / "moved-inputs");
  for (const auto* format : {".gltf", ".glb"}) {
    const auto portablePath = (flattened / (std::string("scene") + format)).string();
    const auto reloaded = LoadModelWithSource(portablePath);
    check(reloaded.gltfModel);
    EXPECT_EQ(reloaded.gltfModel.asset.version, "2.0");
    EXPECT_FALSE(reloaded.linkedComposition);
    const auto optedIn = LoadModelWithSource(portablePath, options);
    check(optedIn.gltfModel);
    EXPECT_FALSE(optedIn.linkedComposition);
  }
}

TEST(GltfSceneExport, HybridLinkedManifestValidatesOnlyRetainedDependencies) {
  using namespace container::geometry::gltf;
  using Json = nlohmann::json;
  ExportDirectory directory;
  const auto aPath = directory.root / "A.gltf";
  const auto bPath = directory.root / "B.gltf";
  writeSceneExport({cubeGraph(), Model::MakeCube()}, aPath);
  writeSceneExport({cubeGraph(), Model::MakeCube()}, bPath);
  auto b = Json::parse(readText(bPath));
  const auto bBufferUri = b["buffers"][0]["uri"].get<std::string>();
  const auto bBuffer = directory.root / bBufferUri;
  b["buffers"][0]["uri"] = "virtual%20B.bin";
  writeText(bPath, b.dump());

  const auto primary = directory.root / "primary.gltf";
  const Json manifest = {{"asset", {{"version", "2.1"}, {"minVersion", "2.1"}}},
      {"files", Json::array({{{"uri", "A.gltf"}, {"mimeType", "model/gltf+json"}},
          {{"uri", "B.gltf"}, {"mimeType", "model/gltf+json"},
              {"aliases", Json::array({{{"alias", "virtual%20B.bin"}, {"file", 2}}})}},
          {{"uri", bBufferUri}, {"mimeType", "application/octet-stream"}}})},
      {"externalAssets", Json::array({{{"file", 0}}, {{"file", 1}}})},
      {"nodes", Json::array({{{"externalAsset", 0}}, {{"externalAsset", 0}}, {{"externalAsset", 1}}})},
      {"scenes", Json::array({{{"nodes", {0, 1, 2}}}})}, {"scene", 0}};
  writeText(primary, manifest.dump());
  LoadOptions options;
  options.enableLinkedAssets = true;
  const auto loaded = LoadModelWithSource(primary.string(), options);
  ASSERT_TRUE(loaded.linkedComposition);
  ASSERT_TRUE(std::filesystem::remove(aPath));

  const auto destination = directory.root / "hybrid.gltf";
  const std::array retained{GltfLinkedHybridCarrier{2, 1}};
  const auto patch = BuildGltfLinkedHybridManifest(*loaded.linkedComposition, retained, destination);
  Json output = {{"asset", {{"version", "2.0"}}},
      {"nodes", Json::array({{{"name", "Inline A"}}, {{"name", "Linked B"}}})}};
  ApplyGltfLinkedHybridManifest(output, patch);
  EXPECT_EQ(output["asset"]["version"], "2.1");
  EXPECT_FALSE(output["nodes"][0].contains("externalAsset"));
  ASSERT_EQ(output["externalAssets"].size(), 1u);
  ASSERT_EQ(output["files"].size(), 2u);
  const auto& file = output["files"].at(output["externalAssets"].at(
      output["nodes"][1].at("externalAsset").get<size_t>()).at("file").get<size_t>());
  EXPECT_EQ(file["uri"], "B.gltf");
  const auto& alias = file.at("aliases").at(0);
  EXPECT_EQ(alias["alias"], "virtual%20B.bin");
  EXPECT_EQ(output["files"].at(alias.at("file").get<size_t>())["uri"], bBufferUri);

  const auto originalBytes = readText(bBuffer);
  auto changedBytes = originalBytes;
  ASSERT_FALSE(changedBytes.empty());
  changedBytes.front() ^= 1;
  writeText(destination, "preexisting companion");
  const auto rejectChangedDependency = [&] {
    try {
      (void)BuildGltfLinkedHybridManifest(*loaded.linkedComposition, retained, destination);
      ADD_FAILURE() << "Expected the retained B alias target to be verified";
    } catch (const std::runtime_error& error) {
      EXPECT_TRUE(std::string_view(error.what()).contains("changed after import")) << error.what();
    }
    EXPECT_EQ(readText(destination), "preexisting companion");
  };
  writeText(bBuffer, changedBytes);
  rejectChangedDependency();
  writeText(bBuffer, originalBytes);
  ASSERT_TRUE(std::filesystem::remove(bBuffer));
  rejectChangedDependency();
}

TEST(GltfSceneExport, LinkedSkinsAndAnimationsShareResourcesAcrossSelectedNestedPlacements) {
  using namespace container::geometry::gltf;
  const LinkedSkinFixture fixture;
  LoadOptions options;
  options.enableLinkedAssets = true;
  const auto composed = LoadModelWithSource(fixture.primary.string(), options);
  const auto& source = composed.gltfModel;
  ASSERT_EQ(source.meshes.size(), 2u);
  EXPECT_EQ(composed.model.meshes().size(), 2u);
  EXPECT_EQ(source.accessors.size(), fixture.primaryAccessors + fixture.child.accessors.size());
  ASSERT_EQ(source.animations.size(), 1u);
  const auto& clip = source.animations[0];
  ASSERT_EQ(clip.samplers.size(), 4u);
  std::array<int, 4> uses{};
  for (const auto& channel : clip.channels) {
    ASSERT_GE(channel.sampler, 0);
    ASSERT_LT(size_t(channel.sampler), uses.size());
    ++uses[size_t(channel.sampler)];
  }
  std::ranges::sort(uses);
  EXPECT_EQ(uses, (std::array{1, 3, 3, 3}));

  const auto referenceAnimation = ParseAnimations(fixture.child);
  ASSERT_TRUE(referenceAnimation.diagnostics.empty());
  const auto referenceDeformation = LoadDeformationData(fixture.child, fixture.geometry.meshes());
  const auto inspect = [&](const container::geometry::GltfLoadResult& loaded) {
    const auto& model = loaded.gltfModel;
    ASSERT_TRUE(loaded.deformation);
    ASSERT_EQ(model.skins.size(), 3u);
    EXPECT_FALSE(std::ranges::any_of(model.nodes, [](const auto& node) { return node.name.starts_with("Inactive"); }));
    ASSERT_EQ(model.materials.size(), 2u);
    EXPECT_EQ(model.materials[0].name, "Primary material");
    EXPECT_EQ(model.materials[1].name, "Linked material");
    const auto animation = ParseAnimations(model);
    ASSERT_TRUE(animation.diagnostics.empty());
    ASSERT_EQ(animation.clips.size(), 1u);
    EXPECT_EQ(animation.clips[0].name, "Linked gesture");
    ASSERT_EQ(animation.clips[0].tracks.size(), 10u);
    EXPECT_EQ(std::ranges::count_if(animation.clips[0].tracks, [](const auto& track) {
      return track.target.kind == AnimationTargetKind::Material && track.target.index == 1;
    }), 1);

    const std::array placements{std::pair{"Left rig", glm::vec3(-3, 0, -2)},
                                std::pair{"Right rig", glm::vec3(3, 0, -2)},
                                std::pair{"Direct rig", glm::vec3(0, 4, 0)}};
    std::array<int, 3> placedMeshes{};
    std::set<int> skinIds, jointIds;
    int sharedInverseBind = -1;
    for (size_t placement = 0; placement < placements.size(); ++placement) {
      SCOPED_TRACE(placements[placement].first);
      const auto root = std::ranges::find_if(model.nodes, [&](const auto& node) {
        return node.name == placements[placement].first;
      });
      ASSERT_NE(root, model.nodes.end());
      std::vector<int> subtree{int(root - model.nodes.begin())};
      std::vector<int> meshes;
      for (size_t i = 0; i < subtree.size(); ++i) {
        const auto& node = model.nodes.at(size_t(subtree[i]));
        if (node.mesh >= 0 && node.skin >= 0) meshes.push_back(subtree[i]);
        subtree.insert(subtree.end(), node.children.begin(), node.children.end());
      }
      ASSERT_EQ(meshes.size(), 1u);
      placedMeshes[placement] = meshes[0];
      const auto& mesh = model.nodes.at(size_t(meshes[0]));
      EXPECT_TRUE(skinIds.insert(mesh.skin).second);
      EXPECT_EQ(model.meshes.at(size_t(mesh.mesh)).primitives[0].material, 1);
      const auto& skin = model.skins.at(size_t(mesh.skin));
      ASSERT_EQ(skin.joints.size(), 2u);
      EXPECT_EQ(skin.skeleton, skin.joints[0]);
      EXPECT_EQ(model.nodes.at(size_t(skin.joints[0])).name, "Leaf root joint");
      EXPECT_EQ(model.nodes.at(size_t(skin.joints[1])).name, "Leaf child joint");
      ASSERT_GE(skin.inverseBindMatrices, 0);
      if (sharedInverseBind < 0) sharedInverseBind = skin.inverseBindMatrices;
      EXPECT_EQ(skin.inverseBindMatrices, sharedInverseBind);
      for (const int joint : skin.joints) {
        EXPECT_TRUE(std::ranges::find(subtree, joint) != subtree.end());
        EXPECT_TRUE(jointIds.insert(joint).second);
      }
    }

    for (double time : {.35, .8, 1.65}) {
      SCOPED_TRACE(time);
      auto expectedModel = fixture.child;
      auto actualModel = model;
      ApplyAnimationPose(expectedModel, SampleAnimation(referenceAnimation.clips[0], time));
      ApplyAnimationPose(actualModel, SampleAnimation(animation.clips[0], time));
      EXPECT_DOUBLE_EQ(actualModel.materials[0].pbrMetallicRoughness.roughnessFactor, .93);
      EXPECT_NEAR(actualModel.materials[1].pbrMetallicRoughness.roughnessFactor,
          expectedModel.materials[0].pbrMetallicRoughness.roughnessFactor, 1e-7);
      const auto expectedWorlds = worldMatrices(expectedModel);
      const auto actualWorlds = worldMatrices(actualModel);
      const auto expectedPalette = ComputeSkinPalette(referenceDeformation.skins[0], expectedWorlds[2], expectedWorlds);
      const auto expected = EvaluatePrimitive(referenceDeformation.primitives[0],
          std::array{float(expectedModel.nodes[2].weights.at(0))}, expectedPalette);
      for (size_t placement = 0; placement < placements.size(); ++placement) {
        SCOPED_TRACE(placements[placement].first);
        const size_t index = size_t(placedMeshes[placement]);
        const auto& node = actualModel.nodes[index];
        const auto palette = ComputeSkinPalette(loaded.deformation->skins.at(size_t(node.skin)),
            actualWorlds[index], actualWorlds);
        const auto actual = EvaluatePrimitive(loaded.deformation->primitives.at(size_t(node.mesh)),
            std::array{float(node.weights.at(0))}, palette);
        ASSERT_EQ(actual.size(), expected.size());
        for (size_t i = 0; i < actual.size(); ++i) {
          const auto a = glm::vec3(actualWorlds[index] * glm::vec4(actual[i].position, 1));
          const auto b = placements[placement].second + glm::vec3(expectedWorlds[2] * glm::vec4(expected[i].position, 1));
          EXPECT_NEAR(glm::length(a - b), 0, 2e-5);
        }
      }
    }
  };
  inspect(composed);

  std::vector<uint32_t> materials;
  for (const auto& mesh : composed.model.meshes()) {
    ASSERT_GE(mesh.materialIndex(), 0);
    materials.push_back(uint32_t(mesh.materialIndex()));
  }
  auto graph = container::scene::BuildGltfSceneHierarchy(source, composed.instancing, materials);
  std::vector<int32_t> identities;
  for (uint32_t i = 0; i < graph.nodeCount(); ++i) identities.push_back(graph.getNode(i)->sourceNodeIndex);
  for (const auto* suffix : {".gltf", ".glb"})
    writeSceneExport({graph, composed.model, &source, fixture.primary, identities},
        fixture.directory.root / (std::string("portable") + suffix));
  std::filesystem::rename(fixture.input, fixture.directory.root / "relocated-linked-inputs");
  for (const auto* suffix : {".gltf", ".glb"}) {
    SCOPED_TRACE(suffix);
    inspect(LoadModelWithSource((fixture.directory.root / (std::string("portable") + suffix)).string()));
  }
}

TEST(GltfSceneExport, LinkedSkinRejectsJointOutsideSelectedDefaultScene) {
  using namespace container::geometry::gltf;
  LinkedSkinFixture fixture;
  fixture.child.nodes[3].children.clear();
  fixture.child.scenes[0].nodes.push_back(4);
  writeExportModel(fixture.child, fixture.leaf);
  LoadOptions options;
  options.enableLinkedAssets = true;
  try {
    (void)LoadModelWithSource(fixture.primary.string(), options);
    ADD_FAILURE() << "Expected the linked skin with an out-of-scene joint to fail";
  } catch (const std::runtime_error& error) {
    const std::string_view message(error.what());
    EXPECT_TRUE(message.contains("leaf.gltf") && message.contains("joint") && message.contains("scene")) << message;
  }
}

TEST(GltfSceneExport, LinkedDraftRejectsUnsafeAndUnresolvableReferencesWithContext) {
  using namespace container::geometry::gltf;
  using Json = nlohmann::json;
  ExportDirectory directory;
  const auto root = directory.root / "sandbox";
  std::filesystem::create_directories(root);
  const auto input = root / "root.gltf";
  const auto document = [](std::string uri) -> Json {
    return {{"asset", {{"version", "2.1"}}},
        {"files", Json::array({{{"uri", std::move(uri)}, {"mimeType", "model/gltf+json"}}})},
        {"externalAssets", Json::array({{{"file", 0}}})},
        {"nodes", Json::array({{{"externalAsset", 0}}})},
        {"scenes", Json::array({{{"nodes", {0}}}})}, {"scene", 0}};
  };
  LoadOptions options;
  options.enableLinkedAssets = true;
  const auto rejected = [&](const Json& source, std::string_view diagnostic) {
    writeText(input, source.dump());
    try {
      (void)LoadModelWithSource(input.string(), options);
      FAIL() << "Expected linked import to fail";
    } catch (const std::runtime_error& error) {
      EXPECT_TRUE(std::string_view(error.what()).contains(diagnostic)) << error.what();
    }
  };
  rejected(document("missing.gltf"), "missing linked file 'missing.gltf'");
  rejected(document("data:model/gltf+json;base64,e30="), "embedded/data-URI");
  rejected(document("DATA:model/gltf+json;base64,e30="), "embedded/data-URI");
  auto embedded = document("unused.gltf");
  embedded["files"][0].erase("uri");
  embedded["files"][0]["bufferView"] = 0;
  rejected(embedded, "embedded linked files are unsupported");
  rejected(document("http://localhost/model.gltf"), "relative file URI");
  rejected(document("root.gltf?revision=1"), "without a query or fragment");
  rejected(document("root.gltf#scene"), "without a query or fragment");
  rejected(document("root%zz.gltf"), "malformed percent escape");
  writeText(directory.root / "outside.gltf", document("none.gltf").dump());
  rejected(document("../outside.gltf"), "escapes the primary asset directory");
  const auto externalSource = directory.root / "escaped-resource.gltf";
  writeSceneExport({cubeGraph(), Model::MakeCube()}, externalSource);
  auto externalResource = Json::parse(readText(externalSource));
  externalResource["buffers"][0]["uri"] = "../" + externalResource["buffers"][0]["uri"].get<std::string>();
  writeText(root / "child.gltf", externalResource.dump());
  rejected(document("child.gltf"), "escapes the primary asset directory");
  const auto primaryTables = document("child.gltf");
  externalResource["asset"]["version"] = "2.1";
  externalResource["files"] = primaryTables["files"];
  externalResource["externalAssets"] = primaryTables["externalAssets"];
  externalResource["nodes"].push_back({{"externalAsset", 0}});
  externalResource["scenes"][0]["nodes"].push_back(externalResource["nodes"].size() - 1);
  rejected(externalResource, "escapes the primary asset directory");
  writeText(root / "inner.gltf", document("./root.gltf").dump());
  rejected(document("inner.gltf"), "reference cycle");
  writeText(root / "behavior.gltf", R"json({
    "asset":{"version":"2.0"},"nodes":[{}],"scenes":[{"nodes":[0]}],"scene":0,
    "extensionsUsed":["KHR_interactivity"],"extensionsRequired":["KHR_interactivity"],
    "extensions":{"KHR_interactivity":{"graphs":[{"declarations":[{"op":"event/onStart"}],
      "nodes":[{"declaration":0}]}]}}
  })json");
  rejected(document("behavior.gltf"), "Required extension 'KHR_interactivity' is unavailable");
  auto wrongVersion = document("missing.gltf");
  wrongVersion["asset"]["version"] = "2.2";
  rejected(wrongVersion, "pinned glTF 2.1");
  auto duplicateAlias = document("inner.gltf");
  duplicateAlias["files"][0]["aliases"] = Json::array({
      {{"alias", "buffer.bin"}, {"file", 0}}, {{"alias", "./buffer.bin"}, {"file", 0}}});
  rejected(duplicateAlias, "duplicate normalized alias");
  for (int i = 1; i <= 17; ++i)
    writeText(root / (std::to_string(i) + ".gltf"), document(std::to_string(i + 1) + ".gltf").dump());
  rejected(document("1.gltf"), "nesting exceeds 16 levels");
}

TEST(GltfSceneExport, LinkedDraftAcceptsIntegralDecimalAndExponentIndices) {
  using namespace container::geometry::gltf;
  using Json = nlohmann::json;
  ExportDirectory directory;
  const auto leaf = directory.root / "leaf.gltf";
  writeSceneExport({cubeGraph(), Model::MakeCube()}, leaf);
  auto child = Json::parse(readText(leaf));
  const auto buffer = child["buffers"][0]["uri"].get<std::string>();
  child["buffers"][0]["uri"] = "aliased.bin";
  writeText(leaf, child.dump());
  const auto primary = directory.root / "primary.gltf";
  const std::string manifest = R"({
    "asset":{"version":"2.1"},
    "files":[{"uri":"leaf.gltf","mimeType":"model/gltf+json",
              "aliases":[{"alias":"aliased.bin","file":1e0}]},
             {"uri":)" + Json(buffer).dump() + R"(,"mimeType":"application/octet-stream"}],
    "externalAssets":[{"file":0.0}],"nodes":[{"externalAsset":0e0}],
    "scenes":[{"nodes":[0]}],"scene":0
  })";
  writeText(primary, manifest);
  LoadOptions options;
  options.enableLinkedAssets = true;
  const auto linked = LoadModelWithSource(primary.string(), options);
  ASSERT_TRUE(linked.linkedComposition);
  ASSERT_EQ(linked.gltfModel.meshes.size(), 1u);
  ASSERT_EQ(linked.linkedComposition->placementRecords.size(), 1u);
  const auto companion = directory.root / "companion.gltf";
  writeLinkedSceneManifest(*linked.linkedComposition, {}, 0, companion);
  const auto reopened = LoadModelWithSource(companion.string(), options);
  ASSERT_EQ(reopened.gltfModel.buffers.size(), 1u);
  EXPECT_EQ(reopened.gltfModel.buffers[0].data, linked.gltfModel.buffers[0].data);
  const std::array carriers{GltfLinkedHybridCarrier{0, 0}};
  const auto hybrid = BuildGltfLinkedHybridManifest(*linked.linkedComposition, carriers,
      directory.root / "hybrid.gltf");
  EXPECT_EQ(hybrid.at("files").at(0).at("aliases").at(0).at("file"), 1);
  EXPECT_EQ(hybrid.at("externalAssets").at(0).at("file"), 0);
  EXPECT_EQ(hybrid.at("nodeExternalAssets").at(0).at("externalAsset"), 0);

  const auto rejected = [&](const char* field, Json replacement, std::string_view context) {
    auto invalid = Json::parse(manifest);
    invalid[Json::json_pointer(field)] = std::move(replacement);
    writeText(primary, invalid.dump());
    try {
      (void)LoadModelWithSource(primary.string(), options);
      ADD_FAILURE() << "Expected invalid linked index at " << field;
    } catch (const std::runtime_error& error) {
      EXPECT_TRUE(std::string_view(error.what()).contains(context)) << error.what();
      EXPECT_TRUE(std::string_view(error.what()).contains("index is outside its source table")) << error.what();
    }
  };
  rejected("/files/0/aliases/0/file", 1.5, "files[0]");
  rejected("/externalAssets/0/file", uint64_t(4294967296), "externalAssets[0]");
  rejected("/nodes/0/externalAsset", -1.0, "nodes[0].externalAsset");
}

TEST(GltfSceneExport, RoundTripsRgbaVertexColorsThroughPortableGltfAndGlb) {
  ExportDirectory directory;
  std::vector<container::geometry::Vertex> vertices(3);
  vertices[0].position = {0, 0, 0};
  vertices[1].position = {1, 0, 0};
  vertices[2].position = {0, 1, 0};
  vertices[0].color = {.25f, .5f, .75f};
  vertices[1].color = {1, 0, .5f};
  vertices[2].color = {.5f, 1, 0};
  vertices[0].colorAlpha = 0;
  vertices[1].colorAlpha = .375f;
  vertices[2].colorAlpha = 1;
  const auto geometry = Model::FromMeshes({container::geometry::Mesh(vertices, {0, 1, 2})});
  auto graph = cubeGraph();
  for (const auto* suffix : {".gltf", ".glb"}) {
    const auto path = directory.root / (std::string("rgba") + suffix);
    writeSceneExport({graph, geometry}, path);
    const auto loaded = LoadModelWithSource(path.string());
    const auto& primitive = loaded.gltfModel.meshes[0].primitives[0];
    const auto& accessor = loaded.gltfModel.accessors[primitive.attributes.at("COLOR_0")];
    EXPECT_EQ(accessor.type, TINYGLTF_TYPE_VEC4);
    EXPECT_EQ(accessor.componentType, TINYGLTF_COMPONENT_TYPE_FLOAT);
    ASSERT_EQ(loaded.model.vertices().size(), vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
      EXPECT_EQ(loaded.model.vertices()[i].color, vertices[i].color);
      EXPECT_FLOAT_EQ(loaded.model.vertices()[i].colorAlpha, vertices[i].colorAlpha);
    }
  }
}

TEST(GltfSceneExport, PreservesMaterialsTexturesLightsCamerasAndPortableResources) {
  ExportDirectory directory;
  const auto cube = Model::MakeCube();
  const auto sourceDirectory = directory.root / "source";
  const auto path = texturedSource(sourceDirectory, cube);
  const auto loaded = LoadModelWithSource(path.string());
  auto graph = cubeGraph();
  graph.setName(0, "Moved source group");
  const std::vector<int32_t> mapping{0, 1};
  auto materials = loaded.gltfModel.materials;
  materials[0].pbrMetallicRoughness.metallicFactor = 0.31;
  materials[0].doubleSided = true;
  const auto outputDirectory = directory.root / "export";
  std::filesystem::create_directories(outputDirectory);
  const SceneExportSnapshot snapshot{graph, loaded.model, &loaded.gltfModel,
                                      path, mapping, materials};
  for (const auto* suffix : {".gltf", ".glb"})
    ASSERT_NO_THROW(writeSceneExport(snapshot, outputDirectory /
        (std::string("portable") + suffix)));
  std::filesystem::remove_all(sourceDirectory);

  for (const auto* suffix : {".gltf", ".glb"}) {
    const auto exported = outputDirectory / (std::string("portable") + suffix);
    const auto result = LoadModelWithSource(exported.string());
    const auto& output = result.gltfModel;
    ASSERT_EQ(output.images.size(), 1u);
    EXPECT_EQ(output.images[0].width, 1);
    EXPECT_EQ(output.images[0].height, 1);
    EXPECT_TRUE(output.images[0].uri.empty());
    EXPECT_GE(output.images[0].bufferView, 0);
    EXPECT_EQ(output.materials[0].name, "Paint");
    EXPECT_DOUBLE_EQ(output.materials[0].pbrMetallicRoughness.metallicFactor, 0.31);
    EXPECT_DOUBLE_EQ(output.materials[0].pbrMetallicRoughness.roughnessFactor, 0.73);
    EXPECT_TRUE(output.materials[0].doubleSided);
    EXPECT_EQ(output.materials[0].pbrMetallicRoughness.baseColorTexture.index, 0);
    EXPECT_EQ(output.materials[0].pbrMetallicRoughness.baseColorTexture.texCoord, 1);
    EXPECT_TRUE(output.materials[0].pbrMetallicRoughness.baseColorTexture.extensions
                    .contains("KHR_texture_transform"));
    EXPECT_EQ(output.samplers[0].wrapS, TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE);
    EXPECT_EQ(output.samplers[0].wrapT, TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT);
    ASSERT_EQ(output.lights.size(), 1u);
    EXPECT_DOUBLE_EQ(output.lights[0].intensity, 23.0);
    EXPECT_EQ(output.nodes[0].light, 0);
    EXPECT_EQ(output.nodes[0].camera, 0);
    EXPECT_EQ(output.cameras[0].name, "Source camera");
    EXPECT_EQ(output.nodes[0].extras.Get("authoringNote").Get<std::string>(),
                "retain this note");
    ASSERT_EQ(output.buffers.size(), 1u);
    if (std::string(suffix) == ".glb") {
      EXPECT_TRUE(output.buffers[0].uri.empty());
    } else {
      EXPECT_TRUE(output.buffers[0].uri.starts_with("container-"));
      EXPECT_TRUE(std::filesystem::exists(outputDirectory / output.buffers[0].uri));
    }
  }
}

TEST(GltfSceneExport, ExportsImageDataUrisAndImagesFromSourceGlbBufferViews) {
  ExportDirectory directory;
  const auto cube = Model::MakeCube();
  auto graph = cubeGraph();
  auto source = buildExportModel({graph, cube});
  tinygltf::Image image;
  image.uri = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwC"
              "AAAAC0lEQVR42mP8/x8AAwMCAO+aP1sAAAAASUVORK5CYII=";
  source.images.push_back(image);
  const auto first = directory.root / "first.glb";
  ASSERT_NO_THROW(writeSceneExport({graph, cube, &source}, first));
  const auto fromGlb = LoadModelWithSource(first.string());
  ASSERT_GE(fromGlb.gltfModel.images[0].bufferView, 0);
  const auto second = directory.root / "second.gltf";
  ASSERT_NO_THROW(writeSceneExport({graph, fromGlb.model, &fromGlb.gltfModel, first}, second));
  std::filesystem::remove(first);
  const auto reloaded = LoadModelWithSource(second.string());
  EXPECT_EQ(reloaded.gltfModel.images[0].width, 1);
  EXPECT_EQ(reloaded.gltfModel.images[0].height, 1);
}

TEST(GltfSceneExport, FailedWriteRetainsDestinationDependenciesAndLiveState) {
  ExportDirectory directory;
  const auto cube = Model::MakeCube();
  auto graph = cubeGraph();
  const auto destination = directory.root / "scene.gltf";
  writeSceneExport({graph, cube}, destination);
  const auto original = readText(destination);
  const auto loaded = LoadModelWithSource(destination.string());
  const auto dependency = directory.root / loaded.gltfModel.buffers[0].uri;
  const auto originalDependency = readText(dependency);
  const auto revision = graph.revision();
  const auto identity = graph.identity();
  auto unavailableResource = loaded.gltfModel;
  tinygltf::Image image;
  image.uri = "missing.png";
  unavailableResource.images.push_back(image);
  EXPECT_THROW(writeSceneExport({graph, cube, &unavailableResource,
                                 directory.root / "missing-source.gltf"}, destination),
                 std::runtime_error);
  EXPECT_EQ(readText(destination), original);
  EXPECT_EQ(readText(dependency), originalDependency);
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(graph.identity(), identity);
  EXPECT_EQ(graph.nodeCount(), 2u);
  for (const auto& entry : std::filesystem::directory_iterator(directory.root))
    EXPECT_FALSE(entry.path().filename().string().starts_with(".container-export-"));
}

TEST(GltfSceneExport, SuccessfulOverwriteKeepsEarlierExportDependenciesIntact) {
  ExportDirectory directory;
  const auto cube = Model::MakeCube();
  const auto sphere = Model::MakeSphere(8, 12);
  auto graph = cubeGraph();
  const auto destination = directory.root / "scene.gltf";
  writeSceneExport({graph, cube}, destination);
  const auto first = LoadModelWithSource(destination.string());
  const auto dependency = directory.root / first.gltfModel.buffers[0].uri;
  const auto originalDependency = readText(dependency);
  graph.setName(1, "Sphere replacement");
  writeSceneExport({graph, sphere}, destination);
  const auto second = LoadModelWithSource(destination.string());
  EXPECT_EQ(second.gltfModel.nodes[1].name, "Sphere replacement");
  EXPECT_EQ(second.model.indices().size(), sphere.indices().size());
  EXPECT_NE(first.gltfModel.buffers[0].uri, second.gltfModel.buffers[0].uri);
  EXPECT_EQ(readText(dependency), originalDependency);
}

TEST(GltfSceneExport, ConcurrentSavesReuseTheCompletePortableDependency) {
  ExportDirectory directory;
  const auto geometry = Model::MakeSphere(16, 32);
  const auto graph = cubeGraph();
  const auto packed = buildExportModel({graph, geometry});
  ASSERT_EQ(packed.buffers.size(), 1u);
  constexpr size_t saveCount = 4;
  std::array<std::filesystem::path, saveCount> destinations;
  std::array<std::future<void>, saveCount> saves;
  for (size_t i = 0; i < destinations.size(); ++i) {
    destinations[i] = directory.root / ("scene-" + std::to_string(i) + ".gltf");
    saves[i] = std::async(std::launch::async, [&, destination = destinations[i]] {
      writeExportModel(packed, destination);
    });
  }
  for (auto& save : saves) ASSERT_NO_THROW(save.get());

  std::set<std::string> dependencies;
  for (const auto& destination : destinations) {
    const auto loaded = LoadModelWithSource(destination.string());
    ASSERT_EQ(loaded.gltfModel.buffers.size(), 1u);
    EXPECT_EQ(loaded.gltfModel.buffers[0].data, packed.buffers[0].data);
    dependencies.insert(loaded.gltfModel.buffers[0].uri);
  }
  ASSERT_EQ(dependencies.size(), 1u);
  EXPECT_TRUE(std::filesystem::is_regular_file(directory.root / *dependencies.begin()));
  for (const auto& entry : std::filesystem::directory_iterator(directory.root))
    EXPECT_FALSE(entry.path().filename().string().starts_with(".container-export-"));
}

TEST(GltfSceneExport, ConflictingDependencyFailsAfterStagingWithoutReplacingDestination) {
  ExportDirectory directory;
  const auto cube = Model::MakeCube();
  auto graph = cubeGraph();
  const auto probe = directory.root / "probe.gltf";
  writeSceneExport({graph, cube}, probe);
  const auto loaded = LoadModelWithSource(probe.string());
  const auto outputDirectory = directory.root / "conflicting-output";
  std::filesystem::create_directories(outputDirectory);
  const auto destination = outputDirectory / "scene.gltf";
  const auto dependency = outputDirectory / loaded.gltfModel.buffers[0].uri;
  writeText(destination, "existing scene must survive");
  writeText(dependency, "existing unrelated content must survive");
  EXPECT_THROW(writeSceneExport({graph, cube}, destination), std::runtime_error);
  EXPECT_EQ(readText(destination), "existing scene must survive");
  EXPECT_EQ(readText(dependency), "existing unrelated content must survive");
  for (const auto& entry : std::filesystem::directory_iterator(outputDirectory))
    EXPECT_FALSE(entry.path().filename().string().starts_with(".container-export-"));
}

TEST(GltfSceneExport, WritesDetachedCurrentLightDefinitionsWithoutChangingPackedModel) {
  ExportDirectory directory;
  const auto cube = Model::MakeCube();
  auto graph = cubeGraph();
  auto model = buildExportModel({graph, cube});
  tinygltf::Light light;
  light.type = "spot";
  light.intensity = 49;
  light.range = 12;
  light.color = {0.2, 0.4, 0.8};
  light.spot.innerConeAngle = 0.1;
  light.spot.outerConeAngle = 0.4;
  model.lights.push_back(light);
  model.extensionsUsed.push_back("KHR_lights_punctual");
  tinygltf::Node node;
  node.name = "Edited manual light";
  node.light = 0;
  node.translation = {2, 3, 4};
  model.scenes[0].nodes.push_back(static_cast<int>(model.nodes.size()));
  model.nodes.push_back(node);
  const auto originalBytes = model.buffers[0].data;
  const auto destination = directory.root / "lights.glb";
  writeExportModel(model, destination);
  const auto loaded = LoadModelWithSource(destination.string());
  ASSERT_EQ(loaded.gltfModel.lights.size(), 1u);
  EXPECT_DOUBLE_EQ(loaded.gltfModel.lights[0].intensity, 49);
  EXPECT_DOUBLE_EQ(loaded.gltfModel.lights[0].spot.outerConeAngle, 0.4);
  EXPECT_EQ(loaded.gltfModel.nodes.back().translation, node.translation);
  EXPECT_EQ(loaded.gltfModel.nodes.back().light, 0);
  EXPECT_EQ(model.buffers[0].data, originalBytes);
  EXPECT_TRUE(model.buffers[0].uri.empty());
}

TEST(GltfSceneExport, ValidatesSnapshotAndRejectsInvalidAnimations) {
  const auto cube = Model::MakeCube();
  auto graph = cubeGraph();
  graph.getNode(1)->primitiveIndex = 500;
  EXPECT_THROW(buildExportModel({graph, cube}), std::runtime_error);
  graph.getNode(1)->primitiveIndex = 0;
  graph.getNode(1)->materialIndex = 500;
  EXPECT_THROW(buildExportModel({graph, cube}), std::runtime_error);
  graph.getNode(1)->materialIndex = 0;
  graph.getNode(1)->localTransform[0][0] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW(buildExportModel({graph, cube}), std::runtime_error);
  graph.getNode(1)->localTransform = glm::mat4(1);
  graph.getNode(0)->children.push_back(1);
  EXPECT_THROW(buildExportModel({graph, cube}), std::runtime_error);
  graph.getNode(0)->children.pop_back();
  auto source = buildExportModel({graph, cube});
  source.animations.emplace_back();
  source.animations[0].channels.emplace_back();
  EXPECT_THROW(buildExportModel({graph, cube, &source}), std::runtime_error);
}

TEST(GltfSceneExport, RoundTripsAnimatedSkinsMorphInstancesPointersAndEditorShear) {
  ExportDirectory directory;
  const AnimatedExportFixture fixture;
  const auto sourceDeformation = container::geometry::gltf::LoadDeformationData(
      fixture.source, fixture.geometry.meshes());
  const auto sourceAnimations = container::geometry::gltf::ParseAnimations(fixture.source);
  const auto revision = fixture.graph.revision();
  for (const auto* suffix : {".gltf", ".glb"}) {
    const auto path = directory.root / (std::string("animated") + suffix);
    ASSERT_NO_THROW(writeSceneExport(fixture.snapshot(), path));
    auto loaded = LoadModelWithSource(path.string());
    ASSERT_TRUE(loaded.deformation.has_value());
    auto& output = loaded.gltfModel;
    ASSERT_EQ(output.nodes.size(), 10u);
    ASSERT_EQ(output.skins.size(), 2u);
    EXPECT_EQ(output.skins[0].joints, std::vector<int>{7});
    EXPECT_EQ(output.skins[1].joints, std::vector<int>{8});
    EXPECT_EQ(output.skins[0].skeleton, 7);
    EXPECT_EQ(output.nodes[2].skin, 0);
    EXPECT_EQ(output.nodes[3].skin, 0);
    EXPECT_EQ(output.nodes[5].skin, 1);
    EXPECT_EQ(output.nodes[6].skin, 1);
    EXPECT_EQ(output.nodes[2].weights, std::vector<double>{0.25});
    EXPECT_EQ(output.nodes[5].weights, std::vector<double>{0.75});
    EXPECT_TRUE(output.nodes[1].matrix.empty());
    EXPECT_TRUE(output.nodes[7].matrix.empty());
    EXPECT_EQ(output.nodes[9].children, std::vector<int>{1});
    expectMatrix(output.nodes[9], fixture.correction);
    ASSERT_EQ(output.animations.size(), 1u);
    EXPECT_EQ(output.animations[0].name, "Preserved clip");
    EXPECT_TRUE(output.animations[0].extras.Get("authoringNote").Get<bool>());
    ASSERT_EQ(output.animations[0].channels.size(), 9u);
    for (const auto& sampler : output.animations[0].samplers)
      EXPECT_TRUE(sampler.extras.Get("retainSampler").Get<bool>());
    EXPECT_EQ(output.animations[0].samplers[0].interpolation, "CUBICSPLINE");
    EXPECT_EQ(output.animations[0].samplers[1].interpolation, "STEP");
    for (const auto& mesh : output.meshes) {
      EXPECT_EQ(mesh.weights, std::vector<double>{0.1});
      ASSERT_EQ(mesh.primitives.size(), 1u);
      const auto& primitive = mesh.primitives[0];
      EXPECT_TRUE(primitive.attributes.contains("JOINTS_0"));
      EXPECT_TRUE(primitive.attributes.contains("WEIGHTS_0"));
      EXPECT_TRUE(output.accessors[size_t(primitive.attributes.at("WEIGHTS_0"))].normalized);
      ASSERT_EQ(primitive.targets.size(), 1u);
    }
    const auto animations = container::geometry::gltf::ParseAnimations(output);
    EXPECT_TRUE(animations.diagnostics.empty());
    ASSERT_EQ(animations.clips[0].tracks.size(), 9u);
    for (const auto time : {0.0, 0.5, 1.0, 2.0}) {
      auto reference = fixture.source;
      container::geometry::gltf::ApplyAnimationPose(reference,
          container::geometry::gltf::SampleAnimation(sourceAnimations.clips[0], time));
      container::geometry::gltf::ApplyAnimationPose(output,
          container::geometry::gltf::SampleAnimation(animations.clips[0], time));
      const auto referenceWorlds = worldMatrices(reference);
      const auto outputWorlds = worldMatrices(output);
      EXPECT_NEAR(output.materials[2].pbrMetallicRoughness.roughnessFactor,
                  reference.materials[0].pbrMetallicRoughness.roughnessFactor, 1e-6);
      EXPECT_DOUBLE_EQ(output.lights[0].intensity, reference.lights[0].intensity);
      EXPECT_DOUBLE_EQ(output.cameras[0].perspective.yfov, reference.cameras[0].perspective.yfov);
      for (const auto [sourceNode, emittedNode] : std::array{std::pair{0, 2}, std::pair{1, 5}}) {
        const auto& referenceSkin = sourceDeformation.skins[size_t(sourceNode)];
        const auto referencePalette = container::geometry::gltf::ComputeSkinPalette(
            referenceSkin, referenceWorlds[size_t(sourceNode)], referenceWorlds);
        std::vector<float> referenceWeights(reference.nodes[size_t(sourceNode)].weights.begin(),
                                             reference.nodes[size_t(sourceNode)].weights.end());
        const auto expected = container::geometry::gltf::EvaluatePrimitive(
            sourceDeformation.primitives[0], referenceWeights, referencePalette);
        const auto& emitted = output.nodes[size_t(emittedNode)];
        std::vector<float> emittedWeights(emitted.weights.begin(), emitted.weights.end());
        const auto palette = container::geometry::gltf::ComputeSkinPalette(
            loaded.deformation->skins[size_t(emitted.skin)], outputWorlds[size_t(emittedNode)], outputWorlds);
        const auto actual = container::geometry::gltf::EvaluatePrimitive(
            loaded.deformation->primitives[size_t(emitted.mesh)], emittedWeights, palette);
        ASSERT_EQ(actual.size(), expected.size());
        for (size_t vertex = 0; vertex < actual.size(); ++vertex) {
          const auto expectedWorld = glm::vec3(referenceWorlds[size_t(sourceNode)] * glm::vec4(expected[vertex].position, 1));
          const auto actualWorld = glm::vec3(outputWorlds[size_t(emittedNode)] * glm::vec4(actual[vertex].position, 1));
          EXPECT_NEAR(actualWorld.x, expectedWorld.x, 1e-5);
          EXPECT_NEAR(actualWorld.y, expectedWorld.y, 1e-5);
          EXPECT_NEAR(actualWorld.z, expectedWorld.z, 1e-5);
        }
      }
      for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
          EXPECT_NEAR(outputWorlds[1][c][r], (fixture.correction * referenceWorlds[0])[c][r], 1e-5);
    }
  }
  EXPECT_EQ(fixture.graph.revision(), revision);
}

namespace {

container::geometry::Vertex toWorldFrame(container::geometry::Vertex vertex,
                                         const glm::mat4& world) {
  const glm::mat3 linear(world);
  vertex.position = glm::vec3(world * glm::vec4(vertex.position, 1));
  vertex.normal = glm::normalize(glm::transpose(glm::inverse(linear)) * vertex.normal);
  const auto tangent = linear * glm::vec3(vertex.tangent);
  vertex.tangent = glm::vec4(glm::normalize(tangent - glm::dot(tangent, vertex.normal) * vertex.normal),
      vertex.tangent.w * (glm::determinant(linear) < 0 ? -1.0f : 1.0f));
  return vertex;
}

int exportedSkinMesh(const tinygltf::Model& model, uint32_t carrier) {
  const auto& node = model.nodes.at(carrier);
  if (node.mesh >= 0) return int(carrier);
  const auto mesh = std::ranges::find_if(node.children,
      [&](int child) { return model.nodes.at(size_t(child)).mesh >= 0; });
  return mesh == node.children.end() ? -1 : *mesh;
}

void expectPortableSkinTransforms(const tinygltf::Model& model, int mesh) {
  std::vector<int> parents(model.nodes.size(), -1);
  for (size_t i = 0; i < model.nodes.size(); ++i)
    for (int child : model.nodes[i].children) parents.at(size_t(child)) = int(i);
  const auto checkPath = [&](int node) {
    for (; node >= 0; node = parents[size_t(node)]) {
      SCOPED_TRACE(node);
      const glm::mat3 linear(nodeMatrix(model.nodes[size_t(node)]));
      const auto x = glm::normalize(linear[0]), y = glm::normalize(linear[1]), z = glm::normalize(linear[2]);
      EXPECT_NEAR(glm::dot(x, y), 0, 1e-6);
      EXPECT_NEAR(glm::dot(x, z), 0, 1e-6);
      EXPECT_NEAR(glm::dot(y, z), 0, 1e-6);
    }
  };
  checkPath(mesh);
  for (const int joint : model.skins.at(size_t(model.nodes.at(size_t(mesh)).skin)).joints)
    checkPath(joint);
}

void expectPostSkinEditRoundTrip(bool reparent, bool animateOwner = false) {
  using namespace container::geometry::gltf;
  constexpr uint32_t owner = 4, edited = 5, sibling = 6, sourceOwner = 1;
  ExportDirectory directory;
  AnimatedExportFixture fixture;
  const auto rotation = glm::angleAxis(1.1f, glm::normalize(glm::vec3(1, 2, 3)));
  sourceTrack(fixture.source, 3, "rotation", TINYGLTF_TYPE_VEC4,
      {0, 0, 0, 1, rotation.x, rotation.y, rotation.z, rotation.w});
  sourceTrack(fixture.source, 3, "translation", TINYGLTF_TYPE_VEC3, {2, 0, 0, -1, 1, 2});
  sourceTrack(fixture.source, 3, "translation", TINYGLTF_TYPE_VEC3, {2, 0, 0, 3, -1, .5f}, {}, 1);
  sourceTrack(fixture.source, sourceOwner, "weights", TINYGLTF_TYPE_SCALAR, {.8f, .2f}, {}, 1);
  fixture.source.animations[1].name = "Joint rotation rest";
  if (animateOwner) {
    const auto turn = glm::angleAxis(.9f, glm::normalize(glm::vec3(3, 1, 2)));
    sourceTrack(fixture.source, sourceOwner, "translation", TINYGLTF_TYPE_VEC3, {3, 0, 0, 2, 1, -.4f});
    sourceTrack(fixture.source, sourceOwner, "rotation", TINYGLTF_TYPE_VEC4,
        {0, 0, 0, 1, turn.x, turn.y, turn.z, turn.w});
    sourceTrack(fixture.source, sourceOwner, "translation", TINYGLTF_TYPE_VEC3, {3, 0, 0, 1, -1, .5f}, {}, 1);
  }
  fixture.source.skins[1].inverseBindMatrices = sourceAccessor(fixture.source,
      std::vector<float>{1,0,0,0, 0,1,0,0, 0,0,1,0, -2,0,0,1},
      TINYGLTF_TYPE_MAT4, TINYGLTF_COMPONENT_TYPE_FLOAT);
  std::vector<float> normalDelta;
  for (const auto& vertex : fixture.geometry.meshes()[0].vertices()) {
    const auto delta = .2f * glm::cross(vertex.normal, glm::vec3(vertex.tangent));
    normalDelta.insert(normalDelta.end(), {delta.x, delta.y, delta.z});
  }
  const int normalAccessor = sourceAccessor(fixture.source, normalDelta,
      TINYGLTF_TYPE_VEC3, TINYGLTF_COMPONENT_TYPE_FLOAT);
  for (auto& primitive : fixture.source.meshes[0].primitives)
    primitive.targets[0]["NORMAL"] = normalAccessor;

  const auto rest = fixture.source;
  const auto deformation = LoadDeformationData(rest, fixture.geometry.meshes());
  AnimationPlayer live(fixture.source);
  live.selectClip(fixture.source, 0);
  live.seek(fixture.source, .65);
  fixture.materials[2] = fixture.source.materials[0];
  for (uint32_t node = 0; node < fixture.nodes.size(); ++node) {
    const int source = fixture.nodes[node];
    if (source >= 0) {
      const auto local = nodeMatrix(fixture.source.nodes[size_t(source)]);
      fixture.graph.setLocalTransform(node, source == 0 ? fixture.correction * local : local);
    }
  }
  fixture.graph.updateWorldTransforms();

  auto affine = glm::translate(glm::mat4(1), glm::vec3(1, -.5f, 2));
  affine[0][0] = reparent ? -1.3f : 1.3f;
  affine[1][0] = .3f;
  affine[1][1] = .7f;
  affine[2][1] = -.2f;
  if (!reparent && !animateOwner) affine[3][3] = std::nextafter(1.0f, 2.0f);
  if (reparent) {
    const auto destination = fixture.graph.createNode(glm::mat4(1), 2, false,
        SceneGraph::kInvalidNode, "New primitive parent");
    fixture.nodes.push_back(-1);
    ASSERT_TRUE(fixture.graph.setParent(destination, 0));
    ASSERT_TRUE(fixture.graph.setParentPreserveWorldTransform(edited, destination));
    fixture.graph.setLocalTransform(destination, affine);
  } else {
    fixture.graph.setLocalTransform(edited, affine);
  }
  fixture.graph.updateWorldTransforms();
  ASSERT_EQ(fixture.graph.getNode(edited)->meshOwnerGraphNode, owner);
  auto snapshot = fixture.snapshot();
  snapshot.animationRestSource = &rest;
  const auto revision = fixture.graph.revision();

  for (const auto* suffix : {".gltf", ".glb"}) {
    SCOPED_TRACE(suffix);
    const auto path = directory.root / (std::string("post-skin") + suffix);
    ASSERT_NO_THROW(writeSceneExport(snapshot, path));
    auto loaded = LoadModelWithSource(path.string());
    ASSERT_TRUE(loaded.deformation);
    auto& output = loaded.gltfModel;
    const auto editedParent = fixture.graph.getNode(edited)->parent;
    ASSERT_LT(editedParent, output.nodes.size());
    EXPECT_TRUE(std::ranges::any_of(output.nodes[editedParent].children, [&](int child) {
      return child == int(edited) || output.nodes.at(size_t(child)).children == std::vector<int>{int(edited)};
    }));
    const int editedMesh = exportedSkinMesh(output, edited);
    ASSERT_GE(editedMesh, 0);
    ASSERT_GE(output.nodes[size_t(editedMesh)].skin, 0);
    expectPortableSkinTransforms(output, editedMesh);
    const auto comparePose = [&](const tinygltf::Model& reference) {
      const auto referenceWorlds = worldMatrices(reference);
      const auto actualWorlds = worldMatrices(output);
      const auto ownerWorld = referenceWorlds[sourceOwner];
      const auto palette = ComputeSkinPalette(deformation.skins[1], ownerWorld, referenceWorlds);
      const auto& weights = reference.nodes[sourceOwner].weights;
      for (const auto graphNode : {edited, sibling}) {
        SCOPED_TRACE(graphNode);
        const auto expectedWorld = graphNode == sibling ? ownerWorld :
            reparent ? affine * ownerWorld : ownerWorld * affine;
        const auto expected = EvaluatePrimitive(
            deformation.primitives[fixture.graph.getNode(graphNode)->primitiveIndex],
            std::vector<float>(weights.begin(), weights.end()), palette);
        const int mesh = exportedSkinMesh(output, graphNode);
        ASSERT_GE(mesh, 0);
        const auto& node = output.nodes[size_t(mesh)];
        ASSERT_GE(node.skin, 0);
        const auto& actualWorld = actualWorlds[size_t(mesh)];
        for (int column = 0; column < 4; ++column)
          EXPECT_NEAR(glm::length(actualWorld[column] - expectedWorld[column]), 0, 3e-5);
        const auto actualPalette = ComputeSkinPalette(loaded.deformation->skins.at(size_t(node.skin)),
            actualWorld, actualWorlds);
        const auto actual = EvaluatePrimitive(loaded.deformation->primitives.at(size_t(node.mesh)),
            std::vector<float>(node.weights.begin(), node.weights.end()), actualPalette);
        ASSERT_EQ(actual.size(), expected.size());
        for (size_t vertex = 0; vertex < actual.size(); ++vertex) {
          SCOPED_TRACE(vertex);
          const auto a = toWorldFrame(actual[vertex], actualWorld);
          const auto b = toWorldFrame(expected[vertex], expectedWorld);
          EXPECT_NEAR(glm::length(a.position - b.position), 0, 3e-5);
          EXPECT_NEAR(glm::length(a.normal - b.normal), 0, 3e-5);
          EXPECT_NEAR(glm::length(glm::vec3(a.tangent - b.tangent)), 0, 3e-5);
          EXPECT_FLOAT_EQ(a.tangent.w, b.tangent.w);
        }
      }
    };
    {
      SCOPED_TRACE("saved pose");
      comparePose(fixture.source);
    }
    AnimationPlayer replay(output);
    auto reference = rest;
    AnimationPlayer authored(reference);
    ASSERT_EQ(replay.library().clips.size(), 2u);
    for (size_t clip : {0u, 1u, 0u}) {
      SCOPED_TRACE(clip);
      replay.selectClip(output, clip);
      authored.selectClip(reference, clip);
      for (const double time : {.15, .65, 1.4, 2.0}) {
        SCOPED_TRACE(time);
        replay.seek(output, time);
        authored.seek(reference, time);
        comparePose(reference);
      }
    }
  }
  EXPECT_EQ(fixture.graph.revision(), revision);
}

}  // namespace

TEST(GltfSceneExport, PostSkinAffineEditRetainsAnimatedWorldPositionsAndFrames) {
  expectPostSkinEditRoundTrip(false);
}

TEST(GltfSceneExport, PostSkinReparentRetainsAnimatedWorldPositionsAndFrames) {
  expectPostSkinEditRoundTrip(true);
}

TEST(GltfSceneExport, PostSkinEditFollowsAnimatedOwnerWithoutFreezingCorrection) {
  expectPostSkinEditRoundTrip(false, true);
}

TEST(GltfSceneExport, RejectsContinuousInverseScaleForPostSkinEditWithoutReplacingDestination) {
  using namespace container::geometry::gltf;
  ExportDirectory directory;
  AnimatedExportFixture fixture;
  const auto path = directory.root / "retained.glb";
  writeSceneExport(fixture.snapshot(), path);
  const auto saved = readText(path);
  sourceTrack(fixture.source, 1, "scale", TINYGLTF_TYPE_VEC3, {1, 1, 1, 2, .5f, 1});
  const auto rest = fixture.source;
  AnimationPlayer live(fixture.source);
  live.selectClip(fixture.source, 0);
  live.seek(fixture.source, .65);
  fixture.graph.setLocalTransform(1, fixture.correction * nodeMatrix(fixture.source.nodes[0]));
  fixture.graph.setLocalTransform(4, nodeMatrix(fixture.source.nodes[1]));
  fixture.graph.setLocalTransform(7, nodeMatrix(fixture.source.nodes[2]));
  auto shear = glm::mat4(1);
  shear[1][0] = .35f;
  shear[0][1] = .25f;
  fixture.graph.setLocalTransform(5, shear);
  fixture.graph.updateWorldTransforms();
  auto snapshot = fixture.snapshot();
  snapshot.animationRestSource = &rest;
  EXPECT_THROW(writeSceneExport(snapshot, path), std::runtime_error);
  EXPECT_EQ(readText(path), saved);
}

TEST(GltfSceneExport, RejectsSingularEditedPrimitiveWorldWithoutReplacingDestination) {
  ExportDirectory directory;
  AnimatedExportFixture fixture;
  const auto path = directory.root / "retained.glb";
  writeSceneExport(fixture.snapshot(), path);
  const auto saved = readText(path);
  const auto parent = fixture.graph.createNode(glm::scale(glm::mat4(1), glm::vec3(1, 0, 1)),
      2, false, SceneGraph::kInvalidNode, "Singular parent");
  fixture.nodes.push_back(-1);
  ASSERT_TRUE(fixture.graph.setParent(parent, 0));
  ASSERT_TRUE(fixture.graph.setParent(5, parent));
  fixture.graph.updateWorldTransforms();
  EXPECT_FLOAT_EQ(glm::determinant(glm::mat3(fixture.graph.getNode(5)->localTransform)), 1);
  EXPECT_FLOAT_EQ(glm::determinant(glm::mat3(fixture.graph.getNode(5)->worldTransform)), 0);
  try {
    writeSceneExport(fixture.snapshot(), path);
    ADD_FAILURE() << "Expected the edited primitive's singular world transform to fail export";
  } catch (const std::runtime_error& error) {
    EXPECT_TRUE(std::string_view(error.what()).contains("singular or unrepresentable world transform"));
  }
  EXPECT_EQ(readText(path), saved);
}

TEST(GltfSceneExport, RejectsMissingMappingsWithoutReplacingDestination) {
  ExportDirectory directory;
  AnimatedExportFixture fixture;
  const auto path = directory.root / "retained.glb";
  writeSceneExport(fixture.snapshot(), path);
  const auto saved = readText(path);
  fixture.nodes[7] = -1;
  EXPECT_THROW(writeSceneExport(fixture.snapshot(), path), std::runtime_error);
  EXPECT_EQ(readText(path), saved);
  fixture.nodes[7] = 2;
  fixture.materialMapping[0] = 500;
  EXPECT_THROW(writeSceneExport(fixture.snapshot(), path), std::runtime_error);
  EXPECT_EQ(readText(path), saved);
}

TEST(GltfSceneExport, ExplicitPrimitiveMapPreservesSourceMetadataAcrossProceduralGeometry) {
  AnimatedExportFixture fixture;
  auto& sourcePrimitives = fixture.source.meshes[0].primitives;
  sourcePrimitives[0].extras = tinygltf::Value(tinygltf::Value::Object{
      {"sourceIdentity", tinygltf::Value(std::string("A"))}});
  sourcePrimitives[1].extras = tinygltf::Value(tinygltf::Value::Object{
      {"sourceIdentity", tinygltf::Value(std::string("B"))}});
  fixture.source.meshes[0].extras = tinygltf::Value(tinygltf::Value::Object{
      {"sourceMesh", tinygltf::Value(true)}});
  const auto sphere = Model::MakeSphere(4, 6);
  fixture.geometry = Model::FromMeshes({fixture.geometry.meshes()[0], sphere.meshes()[0],
                                       fixture.geometry.meshes()[1]});
  fixture.graph.getNode(3)->primitiveIndex = 2;
  fixture.graph.getNode(6)->primitiveIndex = 2;
  const auto procedural = fixture.graph.createNode(glm::mat4(1), 0, true, 1, "Procedural sphere");
  ASSERT_TRUE(fixture.graph.setParent(procedural, 0));
  fixture.nodes.push_back(-1);
  fixture.graph.updateWorldTransforms();
  const std::array<int32_t, 3> mapping{0, -1, 1};
  auto snapshot = fixture.snapshot(); snapshot.runtimePrimitiveToSourceIndices = mapping;
  const auto revision = fixture.graph.revision();
  const auto output = buildExportModel(snapshot);
  for (const auto [graphNode, identity, sourceSkin, weight] :
       {std::tuple{2, "A", 0, .25}, std::tuple{3, "B", 0, .25},
        std::tuple{5, "A", 1, .75}, std::tuple{6, "B", 1, .75}}) {
    const auto& node = output.nodes[size_t(graphNode)];
    ASSERT_GE(node.mesh, 0);
    const auto& mesh = output.meshes[size_t(node.mesh)];
    ASSERT_EQ(mesh.primitives.size(), 1u);
    const auto& primitive = mesh.primitives[0];
    EXPECT_EQ(primitive.extras.Get("sourceIdentity").Get<std::string>(), identity);
    EXPECT_TRUE(mesh.extras.Get("sourceMesh").Get<bool>());
    EXPECT_TRUE(primitive.attributes.contains("JOINTS_0"));
    EXPECT_TRUE(primitive.attributes.contains("WEIGHTS_0"));
    ASSERT_EQ(primitive.targets.size(), 1u);
    EXPECT_TRUE(primitive.targets[0].contains("POSITION"));
    EXPECT_EQ(node.skin, sourceSkin);
    EXPECT_EQ(node.weights, (std::vector<double>{weight}));
  }
  const auto& plainNode = output.nodes[procedural];
  ASSERT_GE(plainNode.mesh, 0);
  const auto& plainMesh = output.meshes[size_t(plainNode.mesh)];
  const auto& plainPrimitive = plainMesh.primitives[0];
  EXPECT_FALSE(plainMesh.extras.Has("sourceMesh"));
  EXPECT_FALSE(plainPrimitive.extras.Has("sourceIdentity"));
  EXPECT_FALSE(plainPrimitive.attributes.contains("JOINTS_0"));
  EXPECT_FALSE(plainPrimitive.attributes.contains("WEIGHTS_0"));
  EXPECT_TRUE(plainPrimitive.targets.empty());
  EXPECT_EQ(plainNode.skin, -1);
  EXPECT_TRUE(plainNode.weights.empty());
  EXPECT_EQ(output.accessors[size_t(plainPrimitive.attributes.at("POSITION"))].count,
            sphere.meshes()[0].vertices().size());
  EXPECT_EQ(fixture.graph.revision(), revision);
  EXPECT_EQ(fixture.source.meshes[0].primitives[1].extras.Get("sourceIdentity").Get<std::string>(), "B");
}

TEST(GltfSceneExport, InvalidPrimitiveMapsPreserveGraphSourceAndExistingDestination) {
  ExportDirectory directory;
  AnimatedExportFixture fixture;
  const auto path = directory.root / "retained-primitive-map.glb";
  writeSceneExport(fixture.snapshot(), path);
  const auto saved = readText(path);
  const auto identity = fixture.graph.identity();
  const auto revision = fixture.graph.revision();
  const auto hierarchy = fixture.graph.hierarchyRevision();
  const auto nodes = fixture.graph.nodeCount();
  const auto sourceBytes = fixture.source.buffers[0].data;
  for (const auto& mapping : std::vector<std::vector<int32_t>>{
           {0}, {0, 1, -1}, {0, 0}, {0, -1}, {0, 2}, {0, -2}}) {
    auto snapshot = fixture.snapshot(); snapshot.runtimePrimitiveToSourceIndices = mapping;
    EXPECT_THROW(writeSceneExport(snapshot, path), std::runtime_error);
    EXPECT_EQ(readText(path), saved);
    EXPECT_EQ(fixture.graph.identity(), identity);
    EXPECT_EQ(fixture.graph.revision(), revision);
    EXPECT_EQ(fixture.graph.hierarchyRevision(), hierarchy);
    EXPECT_EQ(fixture.graph.nodeCount(), nodes);
    EXPECT_EQ(fixture.source.buffers[0].data, sourceBytes);
    EXPECT_EQ(fixture.source.meshes[0].primitives.size(), 2u);
  }
}

TEST(GltfSceneExport, PreservesAnimatedTextureTransformParentsAtSchemaDefaults) {
  ExportDirectory directory;
  const auto cube = Model::MakeCube();
  const auto sourcePath = texturedSource(directory.root / "source", cube);
  auto loaded = LoadModelWithSource(sourcePath.string());
  auto& source = loaded.gltfModel;
  tinygltf::Value::Object textureExtensions{
      {"KHR_texture_transform", tinygltf::Value(tinygltf::Value::Object{})}};
  tinygltf::Value::Object textureInfo{{"index", tinygltf::Value(0)},
      {"extensions", tinygltf::Value(std::move(textureExtensions))}};
  source.materials[0].extensions["KHR_materials_specular"] = tinygltf::Value(
      tinygltf::Value::Object{{"specularColorTexture", tinygltf::Value(std::move(textureInfo))}});
  source.extensionsUsed.push_back("KHR_materials_specular");
  source.extensionsUsed.push_back("KHR_animation_pointer");
  sourceTrack(source, -1, "pointer", TINYGLTF_TYPE_SCALAR, {0, 2},
      "/materials/0/pbrMetallicRoughness/baseColorTexture/extensions/KHR_texture_transform/rotation");
  sourceTrack(source, -1, "pointer", TINYGLTF_TYPE_VEC2, {0, 0, 2, 4},
      "/materials/0/extensions/KHR_materials_specular/specularColorTexture/extensions/KHR_texture_transform/offset");
  std::vector<tinygltf::Material> materials(3);
  materials[2] = source.materials[0];
  materials[2].pbrMetallicRoughness.baseColorTexture.extensions.erase("KHR_texture_transform");
  auto& nestedTexture = materials[2].extensions["KHR_materials_specular"]
      .Get<tinygltf::Value::Object>()["specularColorTexture"].Get<tinygltf::Value::Object>();
  nestedTexture.erase("extensions");
  auto graph = cubeGraph();
  graph.getNode(1)->materialIndex = 2;
  const std::vector<int32_t> nodes{0, 1}, materialMapping{2};
  for (const auto* suffix : {".gltf", ".glb"}) {
    const auto path = directory.root / (std::string("texture-defaults") + suffix);
    writeSceneExport({graph, loaded.model, &source, sourcePath, nodes, materials, materialMapping}, path);
    auto output = LoadModelWithSource(path.string()).gltfModel;
    const auto animations = container::geometry::gltf::ParseAnimations(output);
    EXPECT_TRUE(animations.diagnostics.empty());
    ASSERT_EQ(animations.clips[0].tracks.size(), 2u);
    container::geometry::gltf::ApplyAnimationPose(output,
        container::geometry::gltf::SampleAnimation(animations.clips[0], 1));
    EXPECT_DOUBLE_EQ(output.materials[2].pbrMetallicRoughness.baseColorTexture.extensions
        .at("KHR_texture_transform").Get("rotation").GetNumberAsDouble(), 1);
    const auto& offset = output.materials[2].extensions.at("KHR_materials_specular")
        .Get("specularColorTexture").Get("extensions").Get("KHR_texture_transform").Get("offset");
    EXPECT_DOUBLE_EQ(offset.Get(size_t{0}).GetNumberAsDouble(), 1);
    EXPECT_DOUBLE_EQ(offset.Get(size_t{1}).GetNumberAsDouble(), 2);
  }
}

TEST(GltfSceneExport, PreservesSavedPoseAndCrossClipRestWithPartialVectorCoverage) {
  using namespace container::geometry::gltf;
  ExportDirectory directory;
  const auto cube = Model::MakeCube();
  auto graph = cubeGraph();
  auto rest = buildExportModel({graph, cube});
  rest.nodes[1].matrix.clear();
  rest.nodes[1].translation = {0, 0, 0};
  rest.materials[0].pbrMetallicRoughness.baseColorFactor = {0.2, 0.3, 0.4, 0.5};
  rest.materials[0].pbrMetallicRoughness.roughnessFactor = 0.65;
  sourceTrack(rest, 1, "translation", TINYGLTF_TYPE_VEC3, {0,0,0, 2,4,6});
  sourceTrack(rest, -1, "pointer", TINYGLTF_TYPE_VEC4, {0.2f,0.3f,0.4f,0.5f, 0.8f,0.9f,1,0.5f},
      "/materials/0/pbrMetallicRoughness/baseColorFactor");
  sourceTrack(rest, -1, "pointer", TINYGLTF_TYPE_SCALAR, {0.1f,0.9f},
      "/materials/0/pbrMetallicRoughness/roughnessFactor", 1);
  sourceTrack(rest, -1, "pointer", TINYGLTF_TYPE_SCALAR, {0,8}, "/nodes/1/translation/1", 2);
  sourceTrack(rest, -1, "pointer", TINYGLTF_TYPE_SCALAR, {0.3f,0.7f},
      "/materials/0/pbrMetallicRoughness/baseColorFactor/1", 2);
  sourceTrack(rest, -1, "pointer", TINYGLTF_TYPE_SCALAR, {0,10}, "/nodes/1/translation/2", 3);
  sourceTrack(rest, -1, "pointer", TINYGLTF_TYPE_SCALAR, {0.4f,0.6f},
      "/materials/0/pbrMetallicRoughness/baseColorFactor/2", 3);
  rest.extensionsUsed.push_back("KHR_animation_pointer");
  for (size_t i = 0; i < rest.animations.size(); ++i) {
    rest.animations[i].name = "Clip " + std::to_string(i);
    rest.animations[i].extras = tinygltf::Value(
        tinygltf::Value::Object{{"authored", tinygltf::Value(true)}});
  }
  auto source = rest;
  AnimationPlayer sourcePlayer(source);
  sourcePlayer.selectClip(source, 0);
  sourcePlayer.seek(source, 0.65);
  graph.setLocalTransform(1, nodeMatrix(source.nodes[1]));
  graph.getNode(1)->materialIndex = 2;
  const std::vector<int32_t> nodes{0,1}, materialMapping{2};
  std::vector<tinygltf::Material> materials(3);
  materials[2] = source.materials[0];
  for (const auto* suffix : {".gltf", ".glb"}) {
    const auto path = directory.root / (std::string("cross-clip") + suffix);
    writeSceneExport({graph, cube, &source, {}, nodes, materials, materialMapping, &rest}, path);
    auto output = LoadModelWithSource(path.string()).gltfModel;
    EXPECT_EQ(output.nodes[1].translation, source.nodes[1].translation);
    EXPECT_EQ(output.materials[2].pbrMetallicRoughness.baseColorFactor,
              source.materials[0].pbrMetallicRoughness.baseColorFactor);
    ASSERT_EQ(output.animations.size(), rest.animations.size());
    for (size_t clip = 0; clip < rest.animations.size(); ++clip) {
      EXPECT_EQ(output.animations[clip].name, rest.animations[clip].name);
      EXPECT_TRUE(output.animations[clip].extras.Get("authored").Get<bool>());
      ASSERT_GE(output.animations[clip].channels.size(), rest.animations[clip].channels.size());
      for (size_t sampler = 0; sampler < rest.animations[clip].samplers.size(); ++sampler) {
        EXPECT_EQ(output.animations[clip].samplers[sampler].input,
                  rest.animations[clip].samplers[sampler].input);
        EXPECT_EQ(output.animations[clip].samplers[sampler].output,
                  rest.animations[clip].samplers[sampler].output);
        EXPECT_TRUE(output.animations[clip].samplers[sampler].extras.Get("retainSampler").Get<bool>());
      }
    }
    AnimationPlayer replay(output);
    auto reference = rest;
    AnimationPlayer authored(reference);
    for (size_t clip : {1u, 2u, 0u, 3u, 1u}) {
      replay.selectClip(output, clip);
      authored.selectClip(reference, clip);
      for (double time : {0.0, 0.5, 1.4, 2.0}) {
        replay.seek(output, time);
        authored.seek(reference, time);
        ASSERT_EQ(output.nodes[1].translation.size(), 3u);
        for (size_t c = 0; c < 3; ++c)
          EXPECT_NEAR(output.nodes[1].translation[c], reference.nodes[1].translation[c], 1e-6);
        for (size_t c = 0; c < 4; ++c)
          EXPECT_NEAR(output.materials[2].pbrMetallicRoughness.baseColorFactor[c],
                      reference.materials[0].pbrMetallicRoughness.baseColorFactor[c], 1e-6);
        EXPECT_NEAR(output.materials[2].pbrMetallicRoughness.roughnessFactor,
                    reference.materials[0].pbrMetallicRoughness.roughnessFactor, 1e-6);
      }
    }
    replay.clearClip(output);
    EXPECT_EQ(output.nodes[1].translation, source.nodes[1].translation);
    EXPECT_EQ(output.materials[2].pbrMetallicRoughness.baseColorFactor,
              source.materials[0].pbrMetallicRoughness.baseColorFactor);
  }
}

TEST(GltfSceneExport, ExtendedPbrDefaultRestSurvivesPortableCrossClipMaterialRemapping) {
  using namespace container::geometry::gltf;
  using Value = tinygltf::Value;
  using Object = Value::Object;
  ExportDirectory directory;
  const auto cube = Model::MakeCube();
  auto graph = cubeGraph();
  auto rest = buildExportModel({graph,cube});
  rest.nodes[1].matrix.clear();
  const Value note(Object{{"sourceIdentity",Value(92)}});
  rest.materials[0].extensions["KHR_materials_anisotropy"] = Value(Object{{"extras",note}});
  rest.materials[0].extensions["KHR_materials_diffuse_transmission"] = Value(Object{{"extras",note}});
  rest.extensionsUsed = {"KHR_materials_anisotropy","KHR_materials_diffuse_transmission","KHR_animation_pointer"};
  sourceTrack(rest,-1,"pointer",TINYGLTF_TYPE_SCALAR,{0,1},
      "/materials/0/extensions/KHR_materials_anisotropy/anisotropyStrength");
  sourceTrack(rest,-1,"pointer",TINYGLTF_TYPE_SCALAR,{-1,2},
      "/materials/0/extensions/KHR_materials_anisotropy/anisotropyRotation");
  sourceTrack(rest,-1,"pointer",TINYGLTF_TYPE_SCALAR,{0,.8f},
      "/materials/0/extensions/KHR_materials_diffuse_transmission/diffuseTransmissionFactor");
  sourceTrack(rest,-1,"pointer",TINYGLTF_TYPE_VEC3,{0,0,0, .25f,.5f,1},
      "/materials/0/extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorFactor");
  sourceTrack(rest,1,"translation",TINYGLTF_TYPE_VEC3,{0,0,0, 1,0,0},{},1);
  sourceTrack(rest,-1,"pointer",TINYGLTF_TYPE_SCALAR,{0,.2f},
      "/materials/0/extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorFactor/1",2);
  auto source = rest;
  AnimationPlayer current(source); current.selectClip(source,0); current.seek(source,.65);
  graph.getNode(1)->materialIndex = 2;
  const std::vector<int32_t> sourceNodes{0,1}, materialMapping{2};
  std::vector<tinygltf::Material> materials(3); materials[2] = source.materials[0];
  const auto values = [](const tinygltf::Material& material) {
    const auto& anisotropy = material.extensions.at("KHR_materials_anisotropy");
    const auto& diffuse = material.extensions.at("KHR_materials_diffuse_transmission");
    const auto scalar = [](const Value& object,const char* name) {
      return object.Has(name) ? object.Get(name).GetNumberAsDouble() : 0.0;
    };
    std::array<double,6> result{scalar(anisotropy,"anisotropyStrength"),scalar(anisotropy,"anisotropyRotation"),
        scalar(diffuse,"diffuseTransmissionFactor"),1,1,1};
    if (diffuse.Has("diffuseTransmissionColorFactor"))
      for (size_t i = 0; i < 3; ++i) result[i+3] = diffuse.Get("diffuseTransmissionColorFactor").Get(i).GetNumberAsDouble();
    return result;
  };
  for (const auto* suffix : {".gltf",".glb"}) {
    const auto path = directory.root/(std::string("extended-pbr-cross-clip")+suffix);
    writeSceneExport({graph,cube,&source,{},sourceNodes,materials,materialMapping,&rest},path);
    auto output = LoadModelWithSource(path.string()).gltfModel;
    EXPECT_EQ(values(output.materials[2]),values(source.materials[0]));
    for (const auto* extension : {"KHR_materials_anisotropy","KHR_materials_diffuse_transmission"})
      EXPECT_EQ(output.materials[2].extensions.at(extension).Get("extras"),note);
    AnimationPlayer replay(output);
    ASSERT_TRUE(replay.library().diagnostics.empty());
    auto reference = rest; AnimationPlayer authored(reference);
    for (size_t clip : {1u,2u,0u,1u}) {
      replay.selectClip(output,clip); authored.selectClip(reference,clip);
      for (double time : {0.0,.65,2.0}) {
        replay.seek(output,time); authored.seek(reference,time);
        const auto actual = values(output.materials[2]), expected = values(reference.materials[0]);
        for (size_t i = 0; i < actual.size(); ++i) EXPECT_NEAR(actual[i],expected[i],1e-6);
      }
    }
    replay.clearClip(output);
    EXPECT_EQ(values(output.materials[2]),values(source.materials[0]));
  }
}

TEST(GltfSceneExport, NonfiniteAuthoredRestoreFailsWithoutReplacingSavedScene) {
  ExportDirectory directory;
  const auto cube = Model::MakeCube();
  auto graph = cubeGraph();
  auto source = buildExportModel({graph, cube});
  source.nodes[1].matrix.clear();
  source.materials[0].extensions["KHR_materials_volume"] = tinygltf::Value(
      tinygltf::Value::Object{{"attenuationDistance", tinygltf::Value(4.0)}});
  source.extensionsUsed = {"KHR_materials_volume", "KHR_animation_pointer"};
  sourceTrack(source, -1, "pointer", TINYGLTF_TYPE_SCALAR, {1,5},
      "/materials/0/extensions/KHR_materials_volume/attenuationDistance");
  sourceTrack(source, 1, "translation", TINYGLTF_TYPE_VEC3, {0,0,0, 1,0,0}, {}, 1);
  auto rest = source;
  // Omission means +infinity in the volume schema. glTF FLOAT animation
  // accessors cannot express that restore value without changing semantics.
  rest.materials[0].extensions["KHR_materials_volume"] = tinygltf::Value(tinygltf::Value::Object{});
  const std::vector<int32_t> nodes{0,1};
  const auto path = directory.root / "retained.glb";
  writeText(path, "previous scene");
  const auto revision = graph.revision();
  EXPECT_THROW(writeSceneExport({graph, cube, &source, {}, nodes, {}, {}, &rest}, path), std::runtime_error);
  EXPECT_EQ(readText(path), "previous scene");
  EXPECT_EQ(graph.revision(), revision);
  sourceTrack(source, -1, "pointer", TINYGLTF_TYPE_SCALAR, {2,6},
      "/materials/0/extensions/KHR_materials_volume/attenuationDistance", 1);
  // No restore is necessary when every clip already animates the property.
  EXPECT_NO_THROW(writeSceneExport({graph, cube, &source, {}, nodes, {}, {}, &rest}, path));
}

TEST(GltfSceneExport, PreservesMissingBaseFramesAndRegeneratedDeformationNormals) {
  using namespace container::geometry::gltf;
  ExportDirectory directory;
  for (bool authoredNormal : {false, true}) {
    AnimatedExportFixture fixture;
    const auto count = fixture.geometry.meshes()[0].vertices().size();
    std::vector<float> deltas(count * 3, 0);
    deltas[2] = 2;
    deltas[4] = 1;
    const auto delta = sourceAccessor(fixture.source, deltas,
        TINYGLTF_TYPE_VEC3, TINYGLTF_COMPONENT_TYPE_FLOAT);
    fixture.source.accessors[size_t(delta)].minValues = {0,0,0};
    fixture.source.accessors[size_t(delta)].maxValues = {0,1,2};
    for (auto& primitive : fixture.source.meshes[0].primitives) {
      primitive.targets[0]["POSITION"] = delta;
      if (authoredNormal) primitive.attributes.erase("TANGENT");
      else primitive.attributes.erase("NORMAL"); // Even a supplied tangent must be ignored here.
    }
    const auto sourcePath = directory.root / (authoredNormal ? "no-tangent.glb" : "no-normal.glb");
    tinygltf::TinyGLTF writer;
    ASSERT_TRUE(writer.WriteGltfSceneToFile(&fixture.source, sourcePath.string(), true, true, true, true));
    const auto original = LoadModelWithSource(sourcePath.string());
    ASSERT_TRUE(original.deformation.has_value());
    const auto referenceWorlds = worldMatrices(original.gltfModel);
    const auto& referencePrimitive = original.deformation->primitives[0];
    EXPECT_EQ(referencePrimitive.authoredBaseNormal, authoredNormal);
    const auto referencePrepared = PrepareDeformationFrames(referencePrimitive,
        original.model.meshes()[0].indices(), {referencePrimitive.authoredBaseNormal,
            referencePrimitive.authoredBaseTangent, referencePrimitive.normalTextureTexCoord});
    for (const auto* suffix : {".gltf", ".glb"}) {
      const auto path = directory.root / ((authoredNormal ? std::string("tangent-output") :
          std::string("normal-output")) + suffix);
      writeSceneExport({fixture.graph, original.model, &original.gltfModel, sourcePath,
          fixture.nodes, fixture.materials, fixture.materialMapping}, path);
      const auto loaded = LoadModelWithSource(path.string());
      ASSERT_TRUE(loaded.deformation.has_value());
      const auto& output = loaded.gltfModel;
      const auto outputWorlds = worldMatrices(output);
      for (const auto& mesh : output.meshes) {
        const auto& primitive = mesh.primitives[0];
        EXPECT_EQ(primitive.attributes.contains("NORMAL"), authoredNormal);
        EXPECT_FALSE(primitive.attributes.contains("TANGENT"));
        EXPECT_TRUE(primitive.attributes.contains("JOINTS_0"));
        EXPECT_TRUE(primitive.attributes.contains("WEIGHTS_0"));
        EXPECT_EQ(primitive.targets[0].at("POSITION"), delta);
      }
      for (const auto [sourceNode, emittedNode] : std::array{std::pair{0,2}, std::pair{1,5}}) {
        const auto& node = output.nodes[size_t(emittedNode)];
        const auto& primitive = loaded.deformation->primitives[size_t(node.mesh)];
        EXPECT_EQ(primitive.authoredBaseNormal, authoredNormal);
        EXPECT_FALSE(primitive.authoredBaseTangent);
        const auto prepared = PrepareDeformationFrames(primitive,
            loaded.model.meshes()[size_t(node.mesh)].indices(),
            {primitive.authoredBaseNormal, primitive.authoredBaseTangent, primitive.normalTextureTexCoord});
        const auto referencePalette = ComputeSkinPalette(original.deformation->skins[size_t(sourceNode)],
            referenceWorlds[size_t(sourceNode)], referenceWorlds);
        const auto palette = ComputeSkinPalette(loaded.deformation->skins[size_t(node.skin)],
            outputWorlds[size_t(emittedNode)], outputWorlds);
        for (float weight : {0.0f, 0.25f, 1.0f}) {
          const std::array weights{weight};
          const auto expected = EvaluateDeformationFrames(referencePrepared, weights, referencePalette);
          const auto actual = EvaluateDeformationFrames(prepared, weights, palette);
          ASSERT_EQ(actual.size(), expected.size());
          const auto referenceNormal = glm::transpose(glm::inverse(glm::mat3(referenceWorlds[size_t(sourceNode)])));
          const auto outputNormal = glm::transpose(glm::inverse(glm::mat3(outputWorlds[size_t(emittedNode)])));
          for (size_t vertex = 0; vertex < actual.size(); ++vertex) {
            const auto expectedWorld = glm::vec3(referenceWorlds[size_t(sourceNode)] * glm::vec4(expected[vertex].position,1));
            const auto actualWorld = glm::vec3(outputWorlds[size_t(emittedNode)] * glm::vec4(actual[vertex].position,1));
            const auto expectedNormal = glm::normalize(referenceNormal * expected[vertex].normal);
            const auto actualNormal = glm::normalize(outputNormal * actual[vertex].normal);
            for (int axis = 0; axis < 3; ++axis) {
              EXPECT_NEAR(actualWorld[axis], expectedWorld[axis], 1e-5);
              EXPECT_NEAR(actualNormal[axis], expectedNormal[axis], 1e-5);
            }
          }
        }
      }
    }
  }
}

TEST(GltfSceneExport, AlignsRetainedDoubleAnimationMorphAndSkinBuffersAfterFourBytePrefix) {
  using namespace container::geometry::gltf;
  ExportDirectory directory;
  AnimatedExportFixture fixture;
  auto& source = fixture.source;
  const auto doubles = [&](std::vector<double> values, int type) {
    while (source.buffers[0].data.size() % 8) source.buffers[0].data.push_back(0);
    return sourceAccessor(source, values, type, TINYGLTF_COMPONENT_TYPE_DOUBLE);
  };
  constexpr double preciseEnd = 2.0000000001;
  const auto times = doubles({0,preciseEnd}, TINYGLTF_TYPE_SCALAR);
  const auto translation = doubles({0,0,0, 0,0,0, 0,0,0,
      0,0,0, 0,preciseEnd,0, 0,0,0}, TINYGLTF_TYPE_VEC3);
  source.animations[0].samplers[0].input = times;
  source.animations[0].samplers[0].output = translation;
  std::vector<double> deltas(fixture.geometry.meshes()[0].vertices().size() * 3, 0);
  for (size_t i = 2; i < deltas.size(); i += 3) deltas[i] = preciseEnd;
  const auto morph = doubles(std::move(deltas), TINYGLTF_TYPE_VEC3);
  source.accessors[size_t(morph)].minValues = {0,0,preciseEnd};
  source.accessors[size_t(morph)].maxValues = {0,0,preciseEnd};
  for (auto& primitive : source.meshes[0].primitives) primitive.targets[0]["POSITION"] = morph;
  constexpr double preciseBindTranslation = 0.12500000001;
  const auto inverseBind = doubles({1,0,0,0, 0,1,0,0, 0,0,1,0,
      0,preciseBindTranslation,0,1}, TINYGLTF_TYPE_MAT4);
  for (auto& skin : source.skins) skin.inverseBindMatrices = inverseBind;
  source.extensionsUsed.push_back("KHR_accessor_float64");
  source.extensionsRequired.push_back("KHR_accessor_float64");
  tinygltf::Buffer prefix;
  prefix.name = "Four byte prefix";
  prefix.data = {1,2,3,4};
  source.buffers.insert(source.buffers.begin(), prefix);
  for (auto& view : source.bufferViews) ++view.buffer;
  ASSERT_EQ(source.buffers[0].data.size() % 8, 4u);
  const auto originalBytes = source.buffers[1].data;
  const auto referenceAnimations = ParseAnimations(source);
  const auto referenceDeformation = LoadDeformationData(source, fixture.geometry.meshes());
  for (const auto* suffix : {".gltf", ".glb"}) {
    const auto path = directory.root / (std::string("double-streams") + suffix);
    ASSERT_NO_THROW(writeSceneExport(fixture.snapshot(), path));
    auto loaded = LoadModelWithSource(path.string());
    ASSERT_TRUE(loaded.deformation.has_value());
    auto& output = loaded.gltfModel;
    for (const auto index : {times,translation,morph,inverseBind}) {
      const auto& accessor = output.accessors[size_t(index)];
      EXPECT_EQ(accessor.componentType, TINYGLTF_COMPONENT_TYPE_DOUBLE);
      const auto& view = output.bufferViews[size_t(accessor.bufferView)];
      EXPECT_EQ((view.byteOffset + accessor.byteOffset) % 8, 0u);
    }
    std::array<double,1> time{};
    CheckedAccessor(output, times, "exported double time", {.allowFloat64=true}).readDoubles(1,time);
    EXPECT_DOUBLE_EQ(time[0], preciseEnd);
    std::array<double,16> bind{};
    CheckedAccessor(output, inverseBind, "exported double inverse bind", {.allowFloat64=true}).readDoubles(0,bind);
    EXPECT_DOUBLE_EQ(bind[13], preciseBindTranslation);
    std::array<double,3> delta{};
    CheckedAccessor(output, morph, "exported double morph", {.allowFloat64=true}).readDoubles(0,delta);
    EXPECT_DOUBLE_EQ(delta[2], preciseEnd);
    const auto animations = ParseAnimations(output);
    EXPECT_EQ(animations.clips[0].tracks[0].times, referenceAnimations.clips[0].tracks[0].times);
    EXPECT_EQ(animations.clips[0].tracks[0].values, referenceAnimations.clips[0].tracks[0].values);
    auto reference = source;
    const auto sampleTime = preciseEnd / 2;
    ApplyAnimationPose(reference, SampleAnimation(referenceAnimations.clips[0], sampleTime));
    ApplyAnimationPose(output, SampleAnimation(animations.clips[0], sampleTime));
    const auto referenceWorlds = worldMatrices(reference);
    const auto outputWorlds = worldMatrices(output);
    for (const auto [sourceNode, emittedNode] : std::array{std::pair{0,2},std::pair{1,5}}) {
      const auto& node = output.nodes[size_t(emittedNode)];
      const auto referencePalette = ComputeSkinPalette(referenceDeformation.skins[size_t(sourceNode)],
          referenceWorlds[size_t(sourceNode)], referenceWorlds);
      const auto palette = ComputeSkinPalette(loaded.deformation->skins[size_t(node.skin)],
          outputWorlds[size_t(emittedNode)], outputWorlds);
      const std::vector<float> referenceWeights(reference.nodes[size_t(sourceNode)].weights.begin(),
                                                  reference.nodes[size_t(sourceNode)].weights.end());
      const std::vector<float> weights(node.weights.begin(),node.weights.end());
      const auto expected = EvaluatePrimitive(referenceDeformation.primitives[0], referenceWeights, referencePalette);
      const auto actual = EvaluatePrimitive(loaded.deformation->primitives[size_t(node.mesh)], weights, palette);
      ASSERT_EQ(actual.size(), expected.size());
      for (size_t i = 0; i < actual.size(); ++i) {
        const auto a = glm::vec3(outputWorlds[size_t(emittedNode)] * glm::vec4(actual[i].position,1));
        const auto b = glm::vec3(referenceWorlds[size_t(sourceNode)] * glm::vec4(expected[i].position,1));
        for (int axis = 0; axis < 3; ++axis) EXPECT_NEAR(a[axis],b[axis],1e-5);
      }
    }
  }
  EXPECT_EQ(source.buffers[0].data, prefix.data);
  EXPECT_EQ(source.buffers[1].data, originalBytes);
}

TEST(GltfSceneExport, RemapsAlternateVariantMaterialsWhileSavingCurrentDefaultAndSharedInstances) {
  const auto cube=Model::MakeCube();
  auto graph=cubeGraph();
  auto source=buildExportModel({graph,cube});
  source.materials.resize(3);
  source.extensionsUsed.push_back("KHR_materials_variants");
  tinygltf::Value::Object variant; variant["name"]=tinygltf::Value(std::string("Alternate"));
  tinygltf::Value::Object root; root["variants"]=tinygltf::Value(tinygltf::Value::Array{tinygltf::Value(variant)});
  root["extras"]=tinygltf::Value(tinygltf::Value::Object{{"keep",tinygltf::Value(true)}});
  source.extensions["KHR_materials_variants"]=tinygltf::Value(root);
  tinygltf::Value::Object mapping;
  mapping["material"]=tinygltf::Value(2);
  mapping["variants"]=tinygltf::Value(tinygltf::Value::Array{tinygltf::Value(0)});
  mapping["extras"]=tinygltf::Value(tinygltf::Value::Object{{"note",tinygltf::Value(std::string("keep"))}});
  tinygltf::Value::Object extension;
  extension["mappings"]=tinygltf::Value(tinygltf::Value::Array{tinygltf::Value(mapping)});
  source.meshes[0].primitives[0].extensions["KHR_materials_variants"]=tinygltf::Value(extension);
  graph.setMaterialIndex(1,4);
  const auto instance=graph.createNode(glm::translate(glm::mat4(1),glm::vec3(3,0,0)),5,true,0,"Second instance");
  graph.setParent(instance,0);
  std::vector<tinygltf::Material> materials(6);
  materials[5].name="Edited alternate even when undrawn";
  const std::array<int32_t,3> materialMap{3,4,5};
  const SceneExportSnapshot snapshot{graph,cube,&source,{}, {},materials,materialMap};
  const auto exported=buildExportModel(snapshot);
  ASSERT_EQ(exported.meshes.size(),2u);
  EXPECT_EQ(exported.meshes[0].primitives[0].material,4);
  EXPECT_EQ(exported.meshes[1].primitives[0].material,5);
  for(const auto& mesh:exported.meshes) {
    const auto& ext=mesh.primitives[0].extensions.at("KHR_materials_variants");
    EXPECT_EQ(ext.Get("mappings").Get(0).Get("material").Get<int>(),5);
    EXPECT_EQ(ext.Get("mappings").Get(0).Get("extras").Get("note").Get<std::string>(),"keep");
  }
  EXPECT_EQ(exported.materials[5].name,"Edited alternate even when undrawn");
  ExportDirectory directory;
  for (const auto* name:{"variants.gltf","variants.glb"}) {
    const auto path=directory.root/name;
    writeExportModel(exported,path);
    const auto loaded=LoadModelWithSource(path.string());
    const auto variants=container::geometry::gltf::ParseMaterialVariants(loaded.gltfModel);
    ASSERT_EQ(variants.names().size(),1u);
    const auto selected=container::geometry::gltf::planMaterialVariantSelection(variants,0);
    ASSERT_EQ(selected.assignments.size(),2u);
    EXPECT_EQ(selected.assignments[0].sourceMaterial,5); EXPECT_EQ(selected.assignments[1].sourceMaterial,5);
    const auto defaults=container::geometry::gltf::planMaterialVariantSelection(variants,std::nullopt);
    EXPECT_EQ(defaults.assignments[0].sourceMaterial,4); EXPECT_EQ(defaults.assignments[1].sourceMaterial,5);
    EXPECT_TRUE(loaded.gltfModel.extensions.at("KHR_materials_variants").Get("extras").Get("keep").Get<bool>());
  }
  EXPECT_EQ(source.meshes[0].primitives[0].material,0);
}

TEST(GltfSceneExport, RetainsEncodedImagesWhenExternalFileIsMissingAndRecognizesDdsAndKtx1) {
  const auto cube=Model::MakeCube(); const auto graph=cubeGraph();
  const std::vector<std::pair<std::vector<unsigned char>,std::string>> cases{
    {{0x89,'P','N','G',13,10,26,10,0,0,16,255},"image/png"},
    {{'D','D','S',' ',124,0,0,0,255,127},"image/vnd-ms.dds"},
    {{0xab,0x4b,0x54,0x58,0x20,0x31,0x31,0xbb,0x0d,0x0a,0x1a,0x0a,255,127},"image/ktx"},
    {{'#','?','R','A','D','I','A','N','C','E',10,255,127,63,132},"image/vnd.radiance"},
    {{'#','?','R','G','B','E',10,255,127,63,132},"image/vnd.radiance"}};
  for(const auto& [encoded,mime]:cases) {
    auto source=buildExportModel({graph,cube});
    tinygltf::Image image; image.uri="file-was-removed-after-loading.bin";
    image.as_is=true; image.image=encoded; source.images.push_back(image);
    const auto output=buildExportModel({graph,cube,&source});
    ASSERT_EQ(output.images.size(),1u); EXPECT_EQ(output.images[0].mimeType,mime);
    const auto& view=output.bufferViews[size_t(output.images[0].bufferView)];
    const auto& bytes=output.buffers[size_t(view.buffer)].data;
    EXPECT_EQ((std::vector<unsigned char>(bytes.begin()+view.byteOffset,bytes.begin()+view.byteOffset+view.byteLength)),encoded);
    EXPECT_EQ(source.images[0].image,encoded); EXPECT_FALSE(source.images[0].uri.empty());
  }
}

TEST(GltfSceneExport, DecodedSixteenBitImagesExportPortablePngWithoutLosingAdjacentValues) {
  ExportDirectory directory;
  const auto cube = Model::MakeCube(); const auto graph = cubeGraph();
  for (int components = 1; components <= 4; ++components) {
    auto source = buildExportModel({graph,cube});
    tinygltf::Image image;
    image.width = 301; image.height = 29; image.component = components; image.bits = 16;
    image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
    const size_t count = size_t(image.width) * image.height * components;
    std::vector<uint16_t> words(count);
    for (size_t i = 0; i < count; ++i) words[i] = static_cast<uint16_t>(0x1234 + i % 257);
    image.image.resize(count * 2); std::memcpy(image.image.data(),words.data(),image.image.size());
    source.images.push_back(image); source.textures.emplace_back(); source.textures[0].source = 0;
    const auto output = buildExportModel({graph,cube,&source});
    ASSERT_EQ(output.images.size(),1u); EXPECT_EQ(output.images[0].mimeType,"image/png");
    const auto& view = output.bufferViews.at(output.images[0].bufferView);
    const auto& encoded = output.buffers.at(view.buffer).data;
    EXPECT_EQ(encoded.at(view.byteOffset + 24),16); // PNG IHDR bit depth.
    for (const auto* suffix : {".glb",".gltf"}) {
      const auto path = directory.root / ("wide-" + std::to_string(components) + suffix);
      writeExportModel(output,path);
      const auto loaded = container::geometry::gltf::LoadSourceModel(path.string());
      ASSERT_EQ(loaded.images[0].bits,16);
      const auto decoded = container::geometry::gltf::DecodeSelectedTextureImage(loaded,
          container::geometry::gltf::SelectTextureImageSource(loaded,0),
          container::geometry::gltf::TextureImageRole::Data);
      ASSERT_EQ(decoded.format,VK_FORMAT_R16G16B16A16_UNORM);
      ASSERT_EQ(decoded.mips[0].bytes.size(), size_t(image.width) * image.height * 8);
      std::vector<uint16_t> actual(decoded.mips[0].bytes.size()/2);
      std::memcpy(actual.data(), decoded.mips[0].bytes.data(), decoded.mips[0].bytes.size());
      for (size_t p = 0; p < size_t(image.width) * image.height; ++p) {
        const auto expected = [&](int c) {
          if (components <= 2) return c == 3 ? (components == 2 ? words[p*2+1] : uint16_t{65535}) : words[p*components];
          return c < components ? words[p*components+c] : uint16_t{65535};
        };
        for (int c=0;c<4;++c) ASSERT_EQ(actual[p*4+c], expected(c)) << components << " channels, pixel " << p;
      }
    }
    EXPECT_EQ(source.images[0].image,image.image);
  }
}

TEST(GltfSceneExport, ManuallyImportedTgaPixelsReloadFromPngWithoutSourceFile) {
  ExportDirectory directory;
  const auto cube = Model::MakeCube(); const auto graph = cubeGraph();
  std::vector<std::byte> tga(18);
  tga[2]=std::byte{2}; tga[12]=tga[14]=std::byte{1}; tga[16]=std::byte{32}; tga[17]=std::byte{0x28};
  for (unsigned char b : {53,29,17,149}) tga.push_back(std::byte{b});
  auto source=buildExportModel({graph,cube});
  source.images.push_back(container::geometry::gltf::RetainTextureImageForExport(tga));
  const auto output=buildExportModel({graph,cube,&source});
  EXPECT_EQ(output.images[0].mimeType,"image/png");
  const auto path=directory.root/"manual.glb"; writeExportModel(output,path);
  const auto loaded=container::geometry::gltf::LoadSourceModel(path.string());
  EXPECT_EQ(loaded.images[0].image,(std::vector<unsigned char>{17,29,53,149}));
}

TEST(GltfSceneExport, InstancedSkinMorphExportKeepsTransformOrderAndExactCustomAttributes) {
  namespace gltf=container::geometry::gltf;
  AnimatedExportFixture f;
  const auto translation=sourceAccessor(f.source,std::vector<float>{-.6f,0,0, .7f,0,0},TINYGLTF_TYPE_VEC3,TINYGLTF_COMPONENT_TYPE_FLOAT);
  const auto scale=sourceAccessor(f.source,std::vector<float>{1,1,1, -1,2,1},TINYGLTF_TYPE_VEC3,TINYGLTF_COMPONENT_TYPE_FLOAT);
  const auto ids=sourceAccessor(f.source,std::vector<uint32_t>{16777217,4294967295u},TINYGLTF_TYPE_SCALAR,TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT);
  const auto signedCodes=sourceAccessor(f.source,std::vector<int8_t>{-128,-127},TINYGLTF_TYPE_SCALAR,TINYGLTF_COMPONENT_TYPE_BYTE,true);
  using Value=tinygltf::Value;
  f.source.nodes[0].extensions["EXT_mesh_gpu_instancing"]=Value(Value::Object{{"attributes",Value(Value::Object{
      {"TRANSLATION",Value(translation)},{"SCALE",Value(scale)},{"_ID",Value(ids)},{"_CODE",Value(signedCodes)}})},
      {"extras",Value(Value::Object{{"keep",Value(true)}})}});
  f.source.extensionsUsed.push_back("EXT_mesh_gpu_instancing");
  f.source.extensionsRequired.push_back("EXT_mesh_gpu_instancing");
  const auto instancing=gltf::ParseMeshGpuInstancing(f.source);
  const std::array<uint32_t,2> materials{0,0};
  auto graph=container::scene::BuildGltfSceneHierarchy(f.source,instancing,materials);
  std::vector<int32_t> sourceNodes;
  for(uint32_t i=0;i<graph.nodeCount();++i) sourceNodes.push_back(graph.getNode(i)->sourceNodeIndex);
  const auto originalBytes=f.source.buffers[0].data;
  const auto sourceDeformation=gltf::LoadDeformationData(f.source,f.geometry.meshes());
  const auto sourceWorlds=worldMatrices(f.source);
  ExportDirectory directory;
  const auto first=graph.renderableNodes()[0];
  const auto check=[&](const std::string& name) {
    graph.updateWorldTransforms();
    const auto output=buildExportModel({graph,f.geometry,&f.source,{},sourceNodes});
    EXPECT_FALSE(output.nodes[0].extensions.contains("EXT_mesh_gpu_instancing"));
    for(const auto* suffix:{".glb",".gltf"}) {
      const auto path=directory.root/(name+suffix);
      writeExportModel(output,path);
      const auto loaded=LoadModelWithSource(path.string());
      ASSERT_TRUE(loaded.deformation);
      ASSERT_EQ(loaded.instancing.nodes.size(),4u);
      EXPECT_EQ(loaded.model.vertices().size(),f.geometry.vertices().size());
      const auto worlds=worldMatrices(loaded.gltfModel);
      for(const auto& instances:loaded.instancing.nodes) {
        ASSERT_EQ(instances.localMatrices.size(),1u);
        const auto runtimeNode=instances.sourceNode<graph.nodeCount()?instances.sourceNode:first;
        ASSERT_EQ(exportedSkinMesh(loaded.gltfModel,runtimeNode),int(instances.sourceNode));
        const auto& runtime=*graph.getNode(runtimeNode);
        ASSERT_GE(runtime.sourceInstanceIndex,0);
        const auto which=uint32_t(runtime.sourceInstanceIndex);
        const auto& node=loaded.gltfModel.nodes[instances.sourceNode];
        EXPECT_GE(node.skin,0);
        const auto referencePalette=gltf::ComputeSkinPalette(sourceDeformation.skins[0],sourceWorlds[0],sourceWorlds);
        const auto palette=gltf::ComputeSkinPalette(loaded.deformation->skins[size_t(node.skin)],worlds[instances.sourceNode],worlds);
        const auto expected=gltf::EvaluatePrimitive(sourceDeformation.primitives[runtime.primitiveIndex],std::array<float,1>{.25f},referencePalette);
        const auto actual=gltf::EvaluatePrimitive(loaded.deformation->primitives[size_t(node.mesh)],std::array<float,1>{.25f},palette);
        ASSERT_EQ(actual.size(),expected.size());
        for(size_t i=0;i<actual.size();++i) {
          const auto a=glm::vec3(worlds[instances.sourceNode]*instances.localMatrices[0]*glm::vec4(actual[i].position,1));
          const auto b=glm::vec3(runtime.worldTransform*glm::vec4(expected[i].position,1));
          for(int axis=0;axis<3;++axis) EXPECT_NEAR(a[axis],b[axis],1e-5);
        }
        for(const auto& attribute:instances.attributes) {
          const gltf::CheckedAccessor reader(loaded.gltfModel,attribute.sourceAccessor,"singleton");
          EXPECT_EQ(reader.count(),1u);
          if(attribute.semantic=="_ID") {
            std::array<uint32_t,1> value{};reader.readUnsigned(0,value);
            EXPECT_EQ(value[0],which?4294967295u:16777217u);
          }
          if(attribute.semantic=="_CODE") {
            std::array<unsigned char,1> value{};reader.readEncodedElement(0,value);
            EXPECT_EQ(value[0],which?129:128);
          }
        }
        EXPECT_TRUE(node.extensions.at("EXT_mesh_gpu_instancing").Get("extras").Get("keep").Get<bool>());
      }
    }
  };
  check("instanced-skin");
  graph.setLocalTransform(first,glm::translate(graph.getNode(first)->localTransform,{.1f,0,0}));
  check("instanced-skin-edit");
  EXPECT_EQ(f.source.buffers[0].data,originalBytes);
}

TEST(GltfSceneExport, PreservesAllScenesSharedRootsInactiveEditsAndAnimationThroughImportPrefixes) {
  namespace gltf = container::geometry::gltf;
  using Value = tinygltf::Value;
  const auto cube = Model::MakeCube();
  auto initial = cubeGraph();
  auto source = buildExportModel({initial, cube});
  source.nodes.assign(2, tinygltf::Node{});
  source.nodes[0].mesh = source.nodes[1].mesh = 0;
  source.nodes[0].translation = {.5, 0, 0};
  source.nodes[1].translation = {0, 1, 0};
  source.scenes.resize(3);
  source.scenes[0].nodes = {0}; source.scenes[1].nodes = {0, 1}; source.scenes[2].nodes.clear();
  for (size_t i = 0; i < source.scenes.size(); ++i) {
    source.scenes[i].name = "Authored scene " + std::to_string(i);
    source.scenes[i].extras = Value(Value::Object{{"catalogID", Value(int(i))}});
    source.scenes[i].extensions["VENDOR_scene_note"] = Value(Value::Object{{"retain", Value(true)}});
  }
  source.defaultScene = 0;
  sourceTrack(source, 1, "translation", TINYGLTF_TYPE_VEC3, {0,1,0, 0,3,0});
  sourceTrack(source, -1, "pointer", TINYGLTF_TYPE_SCALAR, {.2f, .8f},
      "/materials/0/pbrMetallicRoughness/roughnessFactor");
  source.extensionsUsed = {"KHR_animation_pointer", "VENDOR_scene_note"};
  const auto sourceBytes = source.buffers[0].data;
  SceneGraph graph;
  const auto editor = graph.createNode(glm::translate(glm::mat4(1), {3,-1,2}), 1, false,
      SceneGraph::kInvalidNode, "Common Scene Root");
  const auto scale = graph.createNode(glm::scale(glm::mat4(1), {2,2,2}), 1, false,
      SceneGraph::kInvalidNode, "Import scale");
  ASSERT_TRUE(graph.setParent(scale, editor));
  const auto shared = graph.createNode(nodeMatrix(source.nodes[0]), 1, false, SceneGraph::kInvalidNode, "Shared root");
  ASSERT_TRUE(graph.setParent(shared, scale));
  const auto sharedPrimitive = graph.createNode(glm::mat4(1), 1, true, 0, "Shared mesh");
  ASSERT_TRUE(graph.setParent(sharedPrimitive, shared));
  const auto inactive = graph.createNode(glm::translate(glm::mat4(1), {0,5,0}), 1, false,
      SceneGraph::kInvalidNode, "Edited inactive root");
  ASSERT_TRUE(graph.setParent(inactive, scale));
  const auto inactivePrimitive = graph.createNode(glm::mat4(1), 2, true, 0, "Inactive mesh");
  ASSERT_TRUE(graph.setParent(inactivePrimitive, inactive));
  graph.setActiveRoots(std::array{shared});
  graph.updateWorldTransforms();
  ASSERT_EQ(graph.renderableNodes().size(), 1u);
  const std::vector<int32_t> sourceNodes{-1,-1,0,-1,1,-1};
  const std::vector<std::vector<uint32_t>> scenes{{shared},{shared,inactive},{}};
  std::vector<tinygltf::Material> materials(3);
  materials[1] = source.materials[0]; materials[1].pbrMetallicRoughness.roughnessFactor = .27;
  materials[2] = source.materials[0]; materials[2].name = "Inactive material edit";
  materials[2].pbrMetallicRoughness.roughnessFactor = .63;
  const std::array<int32_t,1> materialMapping{1};
  SceneExportSnapshot snapshot{graph,cube,&source,{},sourceNodes,materials,materialMapping};
  snapshot.runtimeSceneRoots = scenes; snapshot.defaultSceneIndex = 1;
  const auto output = buildExportModel(snapshot);
  ASSERT_EQ(output.scenes.size(), 3u); EXPECT_EQ(output.defaultScene, 1);
  EXPECT_EQ(output.scenes[0].nodes[0], output.scenes[1].nodes[0]);
  EXPECT_NE(output.scenes[1].nodes[0], output.scenes[1].nodes[1]);
  EXPECT_TRUE(output.scenes[2].nodes.empty());
  // One correction node and exactly two shared prefix placement nodes.
  EXPECT_EQ(output.nodes.size(), graph.nodeCount() + 3);
  EXPECT_TRUE(output.nodes[scale].children.empty());
  for (size_t i = 0; i < source.scenes.size(); ++i) {
    EXPECT_EQ(output.scenes[i].name, source.scenes[i].name);
    EXPECT_EQ(output.scenes[i].extras, source.scenes[i].extras);
    EXPECT_EQ(output.scenes[i].extensions, source.scenes[i].extensions);
  }
  std::vector<size_t> incoming(output.nodes.size());
  for (const auto& node : output.nodes) for (const auto child : node.children) ++incoming.at(child);
  for (const auto parents : incoming) EXPECT_LE(parents, 1u);
  for (const auto& scene : output.scenes) for (const auto root : scene.nodes) EXPECT_EQ(incoming.at(root), 0u);
  const auto savedWorlds = worldMatrices(output);
  for (const auto node : {shared,inactive,inactivePrimitive})
    for (int c=0;c<4;++c) for (int r=0;r<4;++r)
      EXPECT_NEAR(savedWorlds[node][c][r],graph.getNode(node)->worldTransform[c][r],1e-6);
  EXPECT_EQ(output.animations[0].channels[0].target_node, int(inactive));
  ExportDirectory directory;
  for (const auto* suffix : {".glb", ".gltf"}) {
    const auto path = directory.root / (std::string("all-scenes") + suffix);
    writeExportModel(output, path);
    auto loaded = LoadModelWithSource(path.string()).gltfModel;
    ASSERT_EQ(loaded.scenes.size(), 3u); EXPECT_EQ(loaded.defaultScene, 1);
    EXPECT_EQ(loaded.scenes[0].nodes, output.scenes[0].nodes);
    EXPECT_EQ(loaded.scenes[1].nodes, output.scenes[1].nodes);
    EXPECT_TRUE(loaded.scenes[2].nodes.empty());
    for (size_t i=0;i<loaded.scenes.size();++i) {
      EXPECT_EQ(loaded.scenes[i].name,source.scenes[i].name);
      EXPECT_EQ(loaded.scenes[i].extras,source.scenes[i].extras);
      EXPECT_EQ(loaded.scenes[i].extensions,source.scenes[i].extensions);
    }
    EXPECT_DOUBLE_EQ(loaded.materials[1].pbrMetallicRoughness.roughnessFactor, .27);
    EXPECT_EQ(loaded.materials[2].name,"Inactive material edit");
    EXPECT_DOUBLE_EQ(loaded.materials[2].pbrMetallicRoughness.roughnessFactor, .63);
    ASSERT_GE(loaded.nodes[inactivePrimitive].mesh,0);
    EXPECT_EQ(loaded.meshes[loaded.nodes[inactivePrimitive].mesh].primitives[0].material,2);
    const auto animations = gltf::ParseAnimations(loaded);
    ASSERT_TRUE(animations.diagnostics.empty()); ASSERT_EQ(animations.clips.size(), 1u);
    gltf::ApplyAnimationPose(loaded, gltf::SampleAnimation(animations.clips[0], 1));
    const auto animatedWorlds = worldMatrices(loaded);
    EXPECT_NEAR(animatedWorlds[inactive][3].x, 3, 1e-6);
    EXPECT_NEAR(animatedWorlds[inactive][3].y, 11, 1e-6);
    EXPECT_NEAR(animatedWorlds[inactive][3].z, 2, 1e-6);
    EXPECT_NEAR(loaded.materials[1].pbrMetallicRoughness.roughnessFactor, .5, 1e-6);
  }
  EXPECT_EQ(source.buffers[0].data, sourceBytes);
  EXPECT_EQ(graph.getNode(scale)->children, (std::vector<uint32_t>{shared,inactive}));
  EXPECT_EQ(graph.renderableNodes(), (std::vector<uint32_t>{sharedPrimitive}));
}

TEST(GltfSceneExport, SceneCatalogReusesIdentityRootsAndPreservesHiddenSyntheticPrefixes) {
  const auto cube = Model::MakeCube();
  auto graph = cubeGraph();
  auto source = buildExportModel({graph,cube});
  source.scenes.resize(3);
  const std::vector<std::vector<uint32_t>> scenes{{1},{1},{}};
  SceneExportSnapshot snapshot{graph,cube,&source};
  snapshot.runtimeSceneRoots=scenes; snapshot.defaultSceneIndex=2;
  const auto identity = buildExportModel(snapshot);
  EXPECT_EQ(identity.nodes.size(),graph.nodeCount());
  EXPECT_EQ(identity.scenes[0].nodes,(std::vector<int>{1}));
  EXPECT_EQ(identity.scenes[0].nodes,identity.scenes[1].nodes);
  EXPECT_TRUE(identity.nodes[0].children.empty()); EXPECT_EQ(identity.defaultScene,2);
  graph.setLocalTransform(0,glm::translate(glm::mat4(1),{1e-7f,0,0}));
  const auto tiny=buildExportModel(snapshot);
  EXPECT_EQ(tiny.nodes.size(),graph.nodeCount()+1);
  EXPECT_NEAR(tiny.nodes[tiny.scenes[0].nodes[0]].matrix[12],1e-7,1e-14);
  graph.setLocalTransform(0,glm::mat4(1));
  graph.setVisible(0,false);
  graph.setSelectable(0,false);
  graph.setHoverable(0,false);
  const auto hidden=buildExportModel(snapshot);
  EXPECT_EQ(hidden.nodes.size(),graph.nodeCount()+1);
  EXPECT_EQ(hidden.scenes[0].nodes,hidden.scenes[1].nodes);
  const auto root=hidden.scenes[0].nodes[0];
  EXPECT_EQ(hidden.nodes[root].children,(std::vector<int>{1}));
  EXPECT_FALSE(hidden.nodes[root].extensions.at("KHR_node_visibility").Get("visible").Get<bool>());
  EXPECT_FALSE(hidden.nodes[root].extensions.at("KHR_node_selectability").Get("selectable").Get<bool>());
  EXPECT_FALSE(hidden.nodes[root].extensions.at("KHR_node_hoverability").Get("hoverable").Get<bool>());
  EXPECT_FALSE(hidden.nodes[1].extensions.contains("KHR_node_selectability"));
  EXPECT_FALSE(hidden.nodes[1].extensions.contains("KHR_node_hoverability"));
  for (const auto* name:{"KHR_node_visibility","KHR_node_selectability","KHR_node_hoverability"})
    EXPECT_EQ(std::ranges::count(hidden.extensionsUsed,std::string(name)),1);
}

TEST(GltfSceneExport, PortablePlacementKeepsBlockedAuthoredRootAndItsEdits) {
  namespace gltf = container::geometry::gltf;
  using V = tinygltf::Value;
  const auto cube = Model::MakeCube();
  auto source = buildExportModel({cubeGraph(), cube});
  auto& authored = source.nodes.at(0);
  authored.matrix.clear();
  authored.translation = {0, 0, 0};
  authored.extensions["KHR_node_selectability"] = V(V::Object{{"selectable", V(false)}});
  authored.extensions["KHR_node_hoverability"] = V(V::Object{{"hoverable", V(false)}});
  source.extensionsUsed = {"KHR_node_selectability", "KHR_node_hoverability"};
  sourceTrack(source, 0, "translation", TINYGLTF_TYPE_VEC3, {0, 0, 0, 0, 1, 0});
  const std::array<uint32_t, 1> materials{0};
  const auto scale = glm::scale(glm::mat4(1), glm::vec3(2));
  const auto findNode = [](const SceneGraph& graph, int sourceIndex) {
    for (uint32_t i = 0; i < graph.nodeCount(); ++i)
      if (graph.getNode(i)->sourceNodeIndex == sourceIndex) return i;
    return SceneGraph::kInvalidNode;
  };
  const auto exportGraph = [&](const tinygltf::Model& current, const SceneGraph& graph,
                               const Model& geometry) {
    std::vector<int32_t> sourceNodes;
    for (uint32_t i = 0; i < graph.nodeCount(); ++i)
      sourceNodes.push_back(graph.getNode(i)->sourceNodeIndex);
    const std::vector<std::vector<uint32_t>> scenes{{findNode(graph, current.scenes.at(0).nodes.at(0))}};
    SceneExportSnapshot snapshot{graph, geometry, &current, {}, sourceNodes, current.materials};
    snapshot.runtimeSceneRoots = scenes;
    snapshot.defaultSceneIndex = 0;
    return buildExportModel(snapshot);
  };

  auto original = container::scene::BuildGltfSceneHierarchy(source, {}, materials, scale);
  const auto root = findNode(original, 0);
  ASSERT_NE(root, SceneGraph::kInvalidNode);
  original.setActiveRoots(std::array{root});
  // This edit requires an animated correction between the placement and root.
  original.setLocalTransform(root, glm::translate(glm::mat4(1), glm::vec3(1, 0, 0)));
  original.updateWorldTransforms();
  ASSERT_EQ(original.renderableNodes().size(), 1u);
  const auto originalHit = original.renderableNodes().front();
  EXPECT_EQ(original.nearestSelectableAncestor(originalHit), SceneGraph::kInvalidNode);
  EXPECT_EQ(original.nearestHoverableAncestor(originalHit), SceneGraph::kInvalidNode);
  const auto output = exportGraph(source, original, cube);
  const int placement = output.scenes.at(0).nodes.at(0);
  const int correction = output.nodes.at(size_t(placement)).children.at(0);
  const int restoredRoot = output.nodes.at(size_t(correction)).children.at(0);
  EXPECT_EQ(output.nodes.at(size_t(restoredRoot)).name, authored.name);
  for (const auto index : {placement, correction})
    EXPECT_TRUE(output.nodes.at(size_t(index)).extras.Get(gltf::kScenePlacementBoundaryExtra).Get<bool>());

  ExportDirectory directory;
  for (const auto* suffix : {".glb", ".gltf"}) {
    const auto path = directory.root / (std::string("interaction-placement") + suffix);
    writeExportModel(output, path);
    const auto loaded = LoadModelWithSource(path.string());
    auto graph = container::scene::BuildGltfSceneHierarchy(loaded.gltfModel, {}, materials);
    const auto placementNode = findNode(graph, placement);
    const auto correctionNode = findNode(graph, correction);
    const auto authoredNode = findNode(graph, restoredRoot);
    ASSERT_NE(placementNode, SceneGraph::kInvalidNode);
    ASSERT_NE(correctionNode, SceneGraph::kInvalidNode);
    ASSERT_NE(authoredNode, SceneGraph::kInvalidNode);
    EXPECT_TRUE(graph.getNode(placementNode)->interactionRootBoundary);
    EXPECT_TRUE(graph.getNode(correctionNode)->interactionRootBoundary);
    EXPECT_FALSE(graph.getNode(authoredNode)->interactionRootBoundary);
    ASSERT_EQ(graph.renderableNodes().size(), 1u);
    const auto hit = graph.renderableNodes().front();
    EXPECT_EQ(graph.getNode(hit)->worldTransform, original.getNode(originalHit)->worldTransform);
    EXPECT_EQ(graph.nearestSelectableAncestor(hit), SceneGraph::kInvalidNode);
    EXPECT_EQ(graph.nearestHoverableAncestor(hit), SceneGraph::kInvalidNode);
    EXPECT_THROW((void)graph.prepareRetireSubtree(placementNode), std::invalid_argument);
    EXPECT_NO_THROW((void)graph.prepareRetireSubtree(authoredNode));
    graph.setSelectable(authoredNode, true);
    graph.setHoverable(authoredNode, true);
    EXPECT_EQ(graph.nearestSelectableAncestor(hit), hit);
    EXPECT_EQ(graph.nearestHoverableAncestor(hit), hit);

    const auto scaledAgain = container::scene::BuildGltfSceneHierarchy(loaded.gltfModel, {}, materials, scale);
    const auto repeated = exportGraph(loaded.gltfModel, scaledAgain, loaded.model);
    const auto reloaded = container::scene::BuildGltfSceneHierarchy(repeated, {}, materials);
    ASSERT_EQ(reloaded.renderableNodes().size(), 1u);
    EXPECT_EQ(reloaded.nearestSelectableAncestor(reloaded.renderableNodes().front()), SceneGraph::kInvalidNode);
    EXPECT_EQ(reloaded.nearestHoverableAncestor(reloaded.renderableNodes().front()), SceneGraph::kInvalidNode);
  }
}

TEST(GltfSceneExport, InvalidSceneCatalogFailsBeforeDestinationOrLiveStateChanges) {
  ExportDirectory directory;
  const auto path=directory.root/"retain.glb";
  writeText(path,"existing scene must survive");
  const auto cube=Model::MakeCube(); auto graph=cubeGraph();
  auto source=buildExportModel({graph,cube}); source.scenes.resize(2);
  std::vector<std::vector<uint32_t>> scenes{{1},{1}};
  SceneExportSnapshot snapshot{graph,cube,&source}; snapshot.runtimeSceneRoots=scenes;
  const auto originalChildren=graph.getNode(0)->children;
  const auto reject=[&] {
    EXPECT_THROW(writeSceneExport(snapshot,path),std::runtime_error);
    EXPECT_EQ(readText(path),"existing scene must survive");
    EXPECT_EQ(graph.getNode(0)->children,originalChildren);
  };
  scenes[0]={1,1}; reject();
  scenes[0]={999}; reject();
  scenes[0]={1}; snapshot.defaultSceneIndex=-1; reject();
  snapshot.defaultSceneIndex=2; reject(); snapshot.defaultSceneIndex=0;
  snapshot.runtimeSceneRoots=std::span<const std::vector<uint32_t>>(scenes.data(),1); reject();
  snapshot.runtimeSceneRoots=scenes;
  graph.getNode(0)->sourceNodeIndex=0; reject(); // Authored ancestry cannot be silently flattened.
  graph.getNode(0)->sourceNodeIndex=-1;
  graph.getNode(0)->localTransform[0][0]=std::numeric_limits<float>::max();
  graph.getNode(0)->localTransform[1][1]=std::numeric_limits<float>::infinity(); reject();
}

TEST(GltfSceneExport, NodeFlagEditsRetainAuthoredObjectsAndMetadataWithoutCopyingOntoSyntheticChildren) {
  using Value=tinygltf::Value;
  using Object=Value::Object;
  const auto cube=Model::MakeCube(); auto graph=cubeGraph();
  auto source=buildExportModel({graph,cube});
  const Value metadata(Object{{"author",Value(std::string("retain node flag metadata"))}});
  source.nodes[0].extensions["KHR_node_visibility"]=Value(Object{
      {"visible",Value(false)},{"extras",metadata},{"VENDOR_property",Value(7)},
      {"extensions",Value(Object{{"VENDOR_node_note",metadata}})}});
  source.nodes[0].extensions["KHR_node_selectability"]=Value(Object{{"extras",metadata}});
  source.nodes[0].extensions["KHR_node_hoverability"]=Value(Object{{"hoverable",Value(true)},{"extras",metadata}});
  source.extensionsUsed={"KHR_node_visibility","KHR_node_selectability","KHR_node_hoverability"};
  const auto original=source.nodes[0].extensions;
  graph.setVisible(0,true); graph.setSelectable(0,false); graph.setHoverable(0,true);
  const std::array<int32_t,2> sourceNodes{0,-1};
  const auto output=buildExportModel({graph,cube,&source,{},sourceNodes});
  const auto inspect=[&](const tinygltf::Model& model) {
    const auto& extensions=model.nodes[0].extensions;
    EXPECT_TRUE(extensions.at("KHR_node_visibility").Get("visible").Get<bool>());
    EXPECT_FALSE(extensions.at("KHR_node_selectability").Get("selectable").Get<bool>());
    EXPECT_TRUE(extensions.at("KHR_node_hoverability").Get("hoverable").Get<bool>());
    for (const auto* name:{"KHR_node_visibility","KHR_node_selectability","KHR_node_hoverability"}) {
      EXPECT_EQ(extensions.at(name).Get("extras"),metadata);
      EXPECT_FALSE(model.nodes[1].extensions.contains(name));
      EXPECT_EQ(std::ranges::count(model.extensionsUsed,std::string(name)),1);
    }
    EXPECT_EQ(extensions.at("KHR_node_visibility").Get("VENDOR_property").Get<int>(),7);
    EXPECT_EQ(extensions.at("KHR_node_visibility").Get("extensions").Get("VENDOR_node_note"),metadata);
  };
  inspect(output);
  ExportDirectory directory;
  for (const auto* suffix:{".gltf",".glb"}) {
    const auto path=directory.root/(std::string("edited-node-flags")+suffix);
    writeExportModel(output,path);
    inspect(LoadModelWithSource(path.string()).gltfModel);
  }
  EXPECT_EQ(source.nodes[0].extensions,original);
  EXPECT_TRUE(graph.getNode(1)->visible && graph.getNode(1)->selectable && graph.getNode(1)->hoverable);
}

TEST(GltfSceneExport, NodeFlagDefaultPresenceAndNewLocalOverridesRoundTripIndependently) {
  using Value=tinygltf::Value;
  using Object=Value::Object;
  const auto cube=Model::MakeCube(); auto graph=cubeGraph();
  auto source=buildExportModel({graph,cube});
  source.nodes[0].extensions["KHR_node_visibility"]=Value(Object{});
  source.nodes[0].extensions["KHR_node_selectability"]=Value(Object{{"selectable",Value(true)}});
  source.nodes[0].extensions["KHR_node_hoverability"]=Value(Object{{"extras",Value(Object{{"keep",Value(true)}})}});
  source.extensionsUsed={"KHR_node_visibility","KHR_node_selectability","KHR_node_hoverability"};
  const auto original=source.nodes[0].extensions;
  const auto manual=graph.createNode(glm::mat4(1),0,false,SceneGraph::kInvalidNode,"Unauthored default flags");
  ASSERT_TRUE(graph.setParent(manual,0));
  graph.setVisible(1,false); graph.setSelectable(1,false); graph.setHoverable(1,false);
  const std::array<int32_t,3> sourceNodes{0,-1,-1};
  const auto output=buildExportModel({graph,cube,&source,{},sourceNodes});
  const auto inspect=[&](const tinygltf::Model& model) {
    EXPECT_EQ(model.nodes[0].extensions,original); // Empty/defaulted properties remain absent.
    for (const auto& [name,property]:{std::pair{"KHR_node_visibility","visible"},
        std::pair{"KHR_node_selectability","selectable"},std::pair{"KHR_node_hoverability","hoverable"}}) {
      EXPECT_FALSE(model.nodes[1].extensions.at(name).Get(property).Get<bool>());
      EXPECT_EQ(model.nodes[1].extensions.at(name).Get<Object>().size(),1u);
      EXPECT_FALSE(model.nodes[manual].extensions.contains(name));
    }
  };
  inspect(output);
  ExportDirectory directory;
  for (const auto* suffix:{".gltf",".glb"}) {
    const auto path=directory.root/(std::string("default-node-flags")+suffix);
    writeExportModel(output,path);
    inspect(LoadModelWithSource(path.string()).gltfModel);
  }
  EXPECT_EQ(source.nodes[0].extensions,original);
}

TEST(GltfSceneExport, RetiredAnimatedSubtreeKeepsRawIdentityUntilFinalOverlayCompaction) {
  namespace gltf=container::geometry::gltf;
  AnimatedExportFixture fixture;
  // Inverting the continuous owner scale for an affine leaf edit is unsupported
  // while live. Retiring the whole owner must avoid generating any correction.
  sourceTrack(fixture.source, 0, "scale", TINYGLTF_TYPE_VEC3, {1, 1, 1, 2, .5f, 1});
  auto shear = glm::mat4(1);
  shear[1][0] = .35f;
  fixture.graph.setLocalTransform(2, shear);
  auto retire=fixture.graph.prepareRetireSubtree(1);ASSERT_TRUE(retire);
  ASSERT_TRUE(fixture.graph.publishRetirement(retire));
  const auto raw=buildExportModel(fixture.snapshot());
  ASSERT_EQ(raw.nodes.size(),fixture.graph.nodeCount());
  EXPECT_EQ(raw.nodes[1].name,fixture.graph.getNode(1)->name);
  EXPECT_EQ(raw.nodes[2].skin,0); // Raw identities still serve the light overlay.
  EXPECT_EQ(raw.nodes[4].camera,0);
  auto overlaid=raw;
  tinygltf::Node manual;manual.name="Live manual light overlay";
  tinygltf::Light light;light.type="point";light.intensity=13;
  manual.light=int(overlaid.lights.size());overlaid.lights.push_back(light);
  const auto manualId=int(overlaid.nodes.size());overlaid.nodes.push_back(manual);
  overlaid.scenes[0].nodes.push_back(manualId);
  const auto compact=gltf::CompactGltfExportNodes(overlaid,std::array<int32_t,3>{1,2,3});
  EXPECT_EQ(compact.droppedChannels,4u);
  EXPECT_EQ(compact.oldToNewNodes[1],-1);EXPECT_EQ(compact.oldToNewNodes[2],-1);
  EXPECT_EQ(compact.oldToNewNodes[3],-1);
  EXPECT_EQ(compact.model.nodes[size_t(compact.oldToNewNodes[manualId])].light,1);
  EXPECT_EQ(compact.model.nodes[size_t(compact.oldToNewNodes[4])].camera,0);
  EXPECT_EQ(compact.model.lights.size(),2u);
  EXPECT_EQ(compact.model.animations[0].samplers,overlaid.animations[0].samplers);
  ExportDirectory directory;
  for(const auto* suffix:{".gltf",".glb"}) {
    const auto path=directory.root/(std::string("retired-animated")+suffix);
    writeExportModel(compact.model,path);
    auto loaded=LoadModelWithSource(path.string());
    ASSERT_TRUE(loaded.deformation);EXPECT_EQ(loaded.gltfModel.nodes.size(),compact.model.nodes.size());
    const auto animation=gltf::ParseAnimations(loaded.gltfModel);ASSERT_TRUE(animation.diagnostics.empty());
    gltf::ApplyAnimationPose(loaded.gltfModel,gltf::SampleAnimation(animation.clips[0],1));
    EXPECT_DOUBLE_EQ(loaded.gltfModel.nodes[size_t(compact.oldToNewNodes[5])].weights[0],1.0);
    EXPECT_DOUBLE_EQ(loaded.gltfModel.lights[1].intensity,13);
    EXPECT_EQ(loaded.gltfModel.nodes[size_t(compact.oldToNewNodes[manualId])].name,"Live manual light overlay");
  }
}

TEST(GltfSceneExport, RetiredSceneRootsDoNotAcquireSurvivingPlacementWrappers) {
  namespace gltf=container::geometry::gltf;
  const auto geometry=Model::MakeCube();auto initial=cubeGraph();
  auto source=buildExportModel({initial,geometry});
  source.nodes.assign(2,tinygltf::Node{});source.nodes[0].mesh=source.nodes[1].mesh=0;
  source.nodes[1].translation={0,2,0};source.scenes.resize(2);
  source.scenes[0].name="Retired scene";source.scenes[0].nodes={0};
  source.scenes[1].name="Shared scene";source.scenes[1].nodes={0,1};
  SceneGraph graph;
  const auto prefix=graph.createNode(glm::scale(glm::mat4(1),{2,2,2}),0,false,SceneGraph::kInvalidNode,"Prefix");
  graph.getNode(prefix)->editorSceneRoot=true;
  const auto a=graph.createNode(glm::mat4(1),0,false,SceneGraph::kInvalidNode,"Deleted root");
  graph.setParent(a,prefix);graph.getNode(a)->sourceNodeIndex=0;
  const auto ap=graph.createNode(glm::mat4(1),0,true,0,"Deleted primitive");graph.setParent(ap,a);
  const auto b=graph.createNode(nodeMatrix(source.nodes[1]),0,false,SceneGraph::kInvalidNode,"Live root");
  graph.setParent(b,prefix);graph.getNode(b)->sourceNodeIndex=1;
  const auto bp=graph.createNode(glm::mat4(1),0,true,0,"Live primitive");graph.setParent(bp,b);
  graph.updateWorldTransforms();
  auto retire=graph.prepareRetireSubtree(a);ASSERT_TRUE(retire);ASSERT_TRUE(graph.publishRetirement(retire));
  const std::array<int32_t,5> identities{-1,0,-1,1,-1};
  const std::vector<std::vector<uint32_t>> roots{{a},{a,b}};
  SceneExportSnapshot snapshot{graph,geometry,&source,{},identities};
  snapshot.runtimeSceneRoots=roots;snapshot.defaultSceneIndex=1;
  const auto raw=buildExportModel(snapshot);
  EXPECT_EQ(raw.nodes.size(),graph.nodeCount()+1); // Only the live root needs a prefix wrapper.
  EXPECT_EQ(raw.scenes[0].nodes,(std::vector<int>{int(a)}));
  const auto compact=gltf::CompactGltfExportNodes(raw,std::array<int32_t,2>{int32_t(a),int32_t(ap)});
  EXPECT_TRUE(compact.model.scenes[0].nodes.empty());ASSERT_EQ(compact.model.scenes[1].nodes.size(),1u);
  const auto worlds=worldMatrices(compact.model);
  const auto survivor=compact.oldToNewNodes[bp];ASSERT_GE(survivor,0);
  for(int c=0;c<4;++c) for(int r=0;r<4;++r)
    EXPECT_NEAR(worlds[size_t(survivor)][c][r],graph.getNode(bp)->worldTransform[c][r],1e-6);
  ExportDirectory directory;
  for(const auto* suffix:{".glb",".gltf"}) {
    const auto path=directory.root/(std::string("retired-scenes")+suffix);
    writeExportModel(compact.model,path);
    const auto loaded=LoadModelWithSource(path.string()).gltfModel;
    EXPECT_EQ(loaded.scenes.size(),2u);EXPECT_TRUE(loaded.scenes[0].nodes.empty());
    EXPECT_EQ(loaded.scenes[1].nodes,compact.model.scenes[1].nodes);
    EXPECT_EQ(loaded.defaultScene,1);EXPECT_EQ(loaded.scenes[0].name,"Retired scene");
  }
}
