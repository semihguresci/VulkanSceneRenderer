#include "GltfXmpInspector.h"

#include <Container/geometry/GltfXmpMetadata.h>
#include <Container/renderer/bim/BimMetadata.h>
#include <Container/utility/GuiManager.h>
#include <Container/utility/SceneGraph.h>

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <initializer_list>
#include <string_view>
#include <utility>

namespace {

using container::geometry::gltf::kGltfXmpExtension;
using container::scene::SceneGraph;
using container::ui::ResourceCatalogDomain;
using container::ui::ResourceCatalogKind;
using container::ui::ResourceCatalogTarget;
using container::ui::detail::GltfXmpInspector;
using Json = nlohmann::json;
using Value = tinygltf::Value;

Json packet(std::string description) {
  return {{"@context", {{"dc", "http://purl.org/dc/elements/1.1/"}}},
          {"dc:description", std::move(description)}};
}

void setPackets(tinygltf::Model& model, Json packets) {
  model.extensions[kGltfXmpExtension] = Value(Value::Object{});
  model.extensions_json_string = Json{{kGltfXmpExtension, {{"packets", std::move(packets)}}}}.dump();
}

void attach(tinygltf::ExtensionMap& extensions, int index) {
  extensions[kGltfXmpExtension] = Value(Value::Object{{"packet", Value(index)}});
}

void expectText(const std::string& log, std::initializer_list<std::string_view> expected) {
  for (const auto value : expected)
    EXPECT_TRUE(log.contains(value)) << value;
}

class GltfXmpInspectorWidgets : public testing::Test {
 protected:
  void SetUp() override {
    previous_ = ImGui::GetCurrentContext();
    context_ = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1100, 1350};
    io.DeltaTime = 1.f / 60;
    unsigned char* pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto& platform = ImGui::GetPlatformIO();
    platform.Platform_ClipboardUserData = &clipboard_;
    platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
      *static_cast<std::string*>(ImGui::GetPlatformIO().Platform_ClipboardUserData) = text;
    };
  }

  void TearDown() override {
    ImGui::DestroyContext(context_);
    ImGui::SetCurrentContext(previous_);
  }

  template<class Draw>
  std::string frame(Draw draw, bool log = true) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({1090, 1340});
    ImGui::Begin("XMP widget test", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
    if (log) ImGui::LogToBuffer(0);
    draw();
    std::string rendered;
    if (log) {
      rendered = context_->LogBuffer.c_str();
      ImGui::LogFinish();
    }
    ImGui::End();
    ImGui::Render();
    return rendered;
  }

  static void openTree(const char* label) {
    ImGui::GetStateStorage()->SetInt(ImGui::GetID(label), 1);
  }

  ImGuiContext* previous_{};
  ImGuiContext* context_{};
  std::string clipboard_;
  GltfXmpInspector inspector_;
};

TEST_F(GltfXmpInspectorWidgets, DisplaysAssetAndSelectedSceneWithoutGeometryAndRefreshesSourceGeneration) {
  tinygltf::Model model;
  model.scenes.resize(2);
  attach(model.asset.extensions, 0);
  attach(model.scenes[0].extensions, 1);
  attach(model.scenes[1].extensions, 2);
  setPackets(model, Json::array({packet("Asset packet"), packet("First scene packet"), packet("Second scene packet")}));
  inspector_.setSource(&model, 17, 4);
  const auto draw = [&](int scene) {
    return frame([&] {
      openTree("glTF asset metadata");
      openTree("glTF scene metadata");
      inspector_.drawAsset();
      inspector_.drawScene(scene);
    });
  };
  auto log = draw(0);
  expectText(log, {"glTF asset metadata", "Asset packet", "glTF scene metadata", "First scene packet"});
  EXPECT_FALSE(log.contains("Second scene packet"));
  log = draw(1);
  expectText(log, {"Asset packet", "Second scene packet"});
  EXPECT_FALSE(log.contains("First scene packet"));

  setPackets(model, Json::array({packet("Replacement asset"), packet("Replacement scene"), packet("Next scene")}));
  inspector_.setSource(&model, 17, 5);
  log = draw(0);
  expectText(log, {"Replacement asset", "Replacement scene"});
  EXPECT_FALSE(log.contains("Asset packet"));
  EXPECT_FALSE(log.contains("First scene packet"));
}

TEST_F(GltfXmpInspectorWidgets, ResolvesObjectAndResourceSelectionsInTheirOwnScopes) {
  tinygltf::Model model;
  model.nodes.resize(1);
  model.meshes.resize(1);
  model.materials.resize(2);
  model.animations.resize(1);
  model.images.resize(1);
  std::array<tinygltf::Image, 1> archive;
  model.nodes[0].mesh = 0;
  attach(model.nodes[0].extensions, 0);
  attach(model.meshes[0].extensions, 1);
  attach(model.materials[0].extensions, 2);
  attach(model.materials[1].extensions, 3);
  attach(model.animations[0].extensions, 4);
  attach(model.images[0].extensions, 5);
  attach(archive[0].extensions, 6);
  setPackets(model, Json::array({packet("Node attachment"), packet("Mesh attachment"), packet("Surface attachment"),
      packet("Catalog attachment"), packet("Animation attachment"), packet("Evaluated image"), packet("Archive image")}));
  SceneGraph graph;
  const auto owner = graph.createNode(glm::mat4{1}, 0);
  const auto primitive = graph.createNode(glm::mat4{1}, 0, true, 0);
  ASSERT_TRUE(graph.setParent(primitive, owner));
  graph.getNode(owner)->sourceNodeIndex = 0;
  graph.getNode(primitive)->meshOwnerGraphNode = owner;
  graph.getNode(primitive)->sourceMeshIndex = 0;
  inspector_.setSource(&model, graph.identity(), 0);

  ResourceCatalogTarget image;
  image.row = 0;
  image.kind = ResourceCatalogKind::Image;
  image.domain = ResourceCatalogDomain::Archive;
  auto log = frame([&] {
    for (const auto* label : {"glTF node metadata", "glTF mesh metadata", "glTF surface material metadata",
                              "glTF catalog material metadata", "glTF animation metadata", "glTF image metadata"})
      openTree(label);
    inspector_.drawSelection(graph, primitive, false);
    inspector_.drawSurfaceMaterial(0);
    inspector_.drawCatalogMaterial(1);
    inspector_.drawAnimation(0);
    inspector_.drawImage(image, archive);
  });
  expectText(log, {"glTF node metadata", "Node attachment", "glTF mesh metadata", "Mesh attachment",
                   "glTF surface material metadata", "Surface attachment", "glTF catalog material metadata", "Catalog attachment",
                   "glTF animation metadata", "Animation attachment", "glTF image metadata", "Archive image"});
  EXPECT_FALSE(log.contains("Evaluated image"));

  image.domain = ResourceCatalogDomain::Evaluated;
  log = frame([&] {
    inspector_.drawSelection(graph, owner, false);
    inspector_.drawSurfaceMaterial(0);
    inspector_.drawCatalogMaterial(0);
    inspector_.drawAnimation(-1);
    inspector_.drawImage(image, archive);
  });
  expectText(log, {"Node attachment", "Mesh attachment", "Surface attachment", "Evaluated image"});
  EXPECT_TRUE(log.contains("glTF catalog material metadata"));
  EXPECT_FALSE(log.contains("Catalog attachment"));
  EXPECT_FALSE(log.contains("glTF animation metadata"));
  EXPECT_FALSE(log.contains("Archive image"));
}

TEST_F(GltfXmpInspectorWidgets, ReportsAttachmentErrorsWhileBimPropertiesStayAccessible) {
  tinygltf::Model model;
  model.scenes.resize(1);
  model.nodes.resize(1);
  model.meshes.resize(1);
  model.materials.resize(1);
  model.nodes[0].mesh = 0;
  model.nodes[0].extensions[kGltfXmpExtension] = Value(Value::Object{});
  attach(model.meshes[0].extensions, 7);
  attach(model.materials[0].extensions, 0);
  setPackets(model, Json::array({Json{{"dc:description", "Missing namespace context"}}}));
  SceneGraph graph;
  const auto node = graph.createNode(glm::mat4{1}, 0);
  graph.getNode(node)->sourceNodeIndex = 0;
  inspector_.setSource(&model, graph.identity(), 1);

  container::ui::GuiManager gui;
  std::array<container::renderer::BimElementMetadata, 1> elements;
  elements[0].objectIndex = 42;
  elements[0].guid = "BIM GUID 42";
  elements[0].type = "IfcWall";
  elements[0].displayName = "BIM property row";
  container::ui::BimInspectionState bim;
  bim.hasScene = true;
  bim.metadataSource = elements.data();
  bim.metadataRevision = 1;
  bim.elements = std::span<const container::renderer::BimElementMetadata>(elements);
  const auto draw = [&] {
    return frame([&] {
      for (const auto* label : {"glTF asset metadata", "glTF node metadata", "glTF mesh metadata",
                                "glTF surface material metadata", "BIM Elements"})
        openTree(label);
      inspector_.drawAsset();
      inspector_.drawSelection(graph, node, bim.hasSelection);
      inspector_.drawSurfaceMaterial(0);
      gui.drawBimElementBrowser(bim);
    });
  };
  const auto log = draw();
  expectText(log, {"No XMP packet is attached.", "XMP packet reference must be an object with a nonnegative integer packet index",
                   "XMP packet index is outside the packets array", "XMP packet must be an object with an @context object",
                   "BIM Elements", "1 / 1 elements", "IfcWall", "BIM property row"});
  bim.hasSelection = true;
  bim.selectedObjectIndex = 42;
  const auto bimLog = draw();
  expectText(bimLog, {"glTF asset metadata", "BIM Elements", "BIM property row"});
  EXPECT_FALSE(bimLog.contains("glTF node metadata"));
  EXPECT_FALSE(bimLog.contains("glTF mesh metadata"));
}

TEST_F(GltfXmpInspectorWidgets, ShowsLanguageOrderedListsAndCopiesFullRawPacket) {
  tinygltf::Model model;
  auto original = packet("Scalar description");
  original["dc:title"] = {{"@value", "Bonjour"}, {"@language", "fr-FR"}};
  original["dc:creator"] = {{"@list", Json::array({"First creator", "Second creator"})}};
  original["dc:subject"] = {{"@set", Json::array({"Subject one", "Subject two"})}};
  original["dc:source"] = {{"@id", "https://example.test/original"}};
  original["dc:extent"] = 18446744073709551615ull;
  original["dc:emptyList"] = Json::array();
  original["dc:emptyObject"] = Json::object();
  original["zz:payload"] = std::string(20'000, 'x') + "unshortened tail";
  attach(model.asset.extensions, 0);
  setPackets(model, Json::array({original}));
  inspector_.setSource(&model, 1, 1);
  ImGuiID copyId{};
  const auto draw = [&] {
    openTree("glTF asset metadata");
    ImGui::PushID("glTF asset metadata");
    openTree("Raw packet JSON");
    ImGui::PushID("Raw packet JSON");
    copyId = ImGui::GetID("Copy JSON");
    ImGui::PopID();
    ImGui::PopID();
    inspector_.drawAsset();
  };
  const auto log = frame(draw);
  expectText(log, {"Scalar description", "dc:title [fr-FR]   Bonjour", "1.   First creator", "2.   Second creator",
                   "-   Subject one", "-   Subject two", "@id   https://example.test/original", "18446744073709551615",
                   "dc:emptyList   []", "dc:emptyObject   {}",
                   "Raw packet JSON", "@context", "Preview shortened. Copy JSON includes the full packet."});
  EXPECT_FALSE(log.contains("unshortened tail"));

  auto& io = ImGui::GetIO();
  for (float y = 35; y < 1100 && context_->HoveredId != copyId; y += 4) {
    io.AddMousePosEvent(60, y);
    frame(draw, false);
  }
  ASSERT_EQ(context_->HoveredId, copyId);
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
  frame(draw, false);
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
  frame(draw, false);
  ASSERT_FALSE(clipboard_.empty());
  EXPECT_EQ(Json::parse(clipboard_), original);
}

} // namespace
