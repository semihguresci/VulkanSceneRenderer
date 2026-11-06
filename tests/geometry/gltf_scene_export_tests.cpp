#include <Container/geometry/GltfModelLoader.h>
#include <Container/geometry/GltfSceneExport.h>
#include <Container/geometry/GltfNodeExportCompaction.h>
#include <Container/geometry/GltfAnimation.h>
#include <Container/geometry/GltfDeformation.h>
#include <Container/geometry/GltfDeformationFrames.h>
#include <Container/geometry/GltfAccessor.h>
#include <Container/geometry/GltfMaterialVariants.h>
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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
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

TEST(GltfSceneExport, XmpPacketsRetainDistinctContextsAndLosslessValuesAcrossFormats) {
  using namespace container::geometry::gltf;
  using Json = nlohmann::json;
  ExportDirectory directory;
  const auto input = directory.root / "xmp-source.gltf";
  const auto cube = Model::MakeCube();
  const auto graph = cubeGraph();
  writeSceneExport({graph, cube}, input);
  auto manifest = Json::parse(readText(input));
  manifest["extensionsUsed"] = Json::array({kGltfXmpExtension});
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
  const auto bind = [](Json& record, int packet) {record["extensions"][kGltfXmpExtension] = {{"packet", packet}};};
  bind(manifest["asset"], 0);
  bind(manifest["nodes"][1], 1);
  bind(manifest["meshes"][0], 2);
  bind(manifest["materials"][0], 3);
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
    EXPECT_EQ(ResolveGltfXmpPacket(*retained, model.nodes[0].extensions).message,
              "No XMP packet is attached.");
  };
  const auto source = LoadModelWithSource(input.string());
  check(source.gltfModel);
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
  manifest["nodes"][1]["extensions"][kGltfXmpExtension] = {{"packet", uint64_t(4294967297)}};
  manifest["meshes"][0]["extensions"][kGltfXmpExtension] = {{"packet", 1}};
  manifest["materials"][0]["extensions"][kGltfXmpExtension] = {{"packet", 9}};
  manifest["scenes"][0]["extensions"][kGltfXmpExtension] = false;
  writeText(path, manifest.dump());
  const auto result = LoadModelWithSource(path.string());
  const auto& model = result.gltfModel;
  const auto package = ReadGltfXmpPackage(model);
  ASSERT_TRUE(package);
  EXPECT_EQ(std::ranges::count(result.capabilityDiagnostics, "xmp_metadata", &CapabilityDiagnostic::code), 5);
  const auto mesh = ResolveGltfXmpPacket(*package, model.meshes[0].extensions);
  ASSERT_NE(mesh.packet, nullptr);
  EXPECT_EQ(mesh.packet->at("xmp:Label"), "second slot");
  EXPECT_TRUE(ResolveGltfXmpPacket(*package, model.asset.extensions).message.contains("@context"));
  EXPECT_TRUE(ResolveGltfXmpPacket(*package, model.nodes[1].extensions).message.contains("integer"));
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
  const Json primary = {{"asset", {{"version", "2.1"}}},
      {"files", Json::array({gltfFile("group/nested.gltf"), direct,
          {{"uri", "Shared%20mesh.bin"}, {"mimeType", "application/octet-stream"}}})},
      {"externalAssets", Json::array({{{"file", 0}}, {{"file", 1}}})},
      {"nodes", Json::array({{{"externalAsset", 0}, {"translation", {-3, 0, 0}}},
          {{"externalAsset", 0}, {"translation", {3, 0, 0}}},
          {{"externalAsset", 1}, {"translation", {0, 5, 0}}}})},
      {"scenes", Json::array({{{"nodes", {0, 1, 2}}}})}, {"scene", 0}};
  const auto path = input / "primary.gltf";
  writeText(path, primary.dump());
  EXPECT_THROW((void)LoadModelWithSource(path.string()), std::runtime_error);
  LoadOptions options;
  options.enableLinkedAssets = true;
  const auto result = LoadModelWithSource(path.string(), options);
  const auto& source = result.gltfModel;
  ASSERT_EQ(source.nodes.size(), 11u);
  ASSERT_EQ(source.meshes.size(), 1u);
  ASSERT_EQ(source.buffers.size(), 1u);
  ASSERT_EQ(result.model.meshes().size(), 1u);
  ASSERT_EQ(source.scenes.size(), 1u);
  EXPECT_EQ(source.asset.version, "2.0");
  const std::vector<glm::vec3> expected{{-3, 0, -2}, {3, 0, -2}, {0, 5, 0}};
  const auto check = [&](const tinygltf::Model& model) {
    const auto matrices = worldMatrices(model);
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
    const auto reloaded = LoadModelWithSource((flattened / (std::string("scene") + format)).string());
    check(reloaded.gltfModel);
    EXPECT_EQ(reloaded.gltfModel.asset.version, "2.0");
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
  rejected(document("http://localhost/model.gltf"), "relative file URI");
  rejected(document("root%zz.gltf"), "malformed percent escape");
  writeText(directory.root / "outside.gltf", document("none.gltf").dump());
  rejected(document("../outside.gltf"), "escapes the primary asset directory");
  writeText(root / "inner.gltf", document("./root.gltf").dump());
  rejected(document("inner.gltf"), "reference cycle");
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

TEST(GltfSceneExport, RejectsPostSkinEditsAndMissingMappingsWithoutReplacingDestination) {
  ExportDirectory directory;
  AnimatedExportFixture fixture;
  const auto path = directory.root / "retained.glb";
  writeSceneExport(fixture.snapshot(), path);
  const auto saved = readText(path);
  fixture.graph.setLocalTransform(2, glm::translate(glm::mat4(1), glm::vec3(1, 0, 0)));
  EXPECT_THROW(writeSceneExport(fixture.snapshot(), path), std::runtime_error);
  EXPECT_EQ(readText(path), saved);
  fixture.graph.setLocalTransform(2, glm::mat4(1));
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
  const auto output=buildExportModel({graph,f.geometry,&f.source,{},sourceNodes});
  EXPECT_FALSE(output.nodes[0].extensions.contains("EXT_mesh_gpu_instancing"));
  ExportDirectory directory;
  for(const auto* suffix:{".glb",".gltf"}) {
    const auto path=directory.root/(std::string("instanced-skin")+suffix);
    writeExportModel(output,path);
    const auto loaded=LoadModelWithSource(path.string());
    ASSERT_TRUE(loaded.deformation);
    ASSERT_EQ(loaded.instancing.nodes.size(),4u);
    EXPECT_EQ(loaded.model.vertices().size(),f.geometry.vertices().size());
    const auto worlds=worldMatrices(loaded.gltfModel);
    for(const auto& instances:loaded.instancing.nodes) {
      ASSERT_EQ(instances.localMatrices.size(),1u);
      const auto& runtime=*graph.getNode(instances.sourceNode);
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
        const auto b=glm::vec3(sourceWorlds[0]*instancing.nodes[0].localMatrices[which]*glm::vec4(expected[i].position,1));
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
  EXPECT_EQ(f.source.buffers[0].data,originalBytes);
  const auto first=graph.renderableNodes()[0];
  graph.setLocalTransform(first,glm::translate(graph.getNode(first)->localTransform,{.1f,0,0}));
  EXPECT_THROW((void)buildExportModel({graph,f.geometry,&f.source,{},sourceNodes}),std::runtime_error);
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
  // A post-skin correction needs baking while live. Once its whole owner is
  // retired, it must not prevent saving or create correction-wrapper orphans.
  fixture.graph.setLocalTransform(2,glm::translate(glm::mat4(1),{1,0,0}));
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
  EXPECT_EQ(compact.droppedChannels,3u);
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
