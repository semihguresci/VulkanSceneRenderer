#include "GltfXmpInspector.h"

#include <Container/geometry/GltfXmpMetadata.h>
#include <Container/utility/SceneGraph.h>

#include <imgui.h>

#include <algorithm>
#include <string_view>

namespace container::ui::detail {
namespace {

constexpr size_t kMaxValueBytes = 2048;
constexpr size_t kMaxRawPreviewBytes = 16 * 1024;
constexpr size_t kMaxSummaryRows = 128;
constexpr size_t kMaxSummaryDepth = 8;

std::string_view xmpPreview(std::string_view value, size_t maxBytes = kMaxValueBytes) {
  size_t end = std::min(maxBytes, value.size());
  while (end < value.size() && (static_cast<unsigned char>(value[end]) & 0xc0) == 0x80)
    --end;
  return value.substr(0, end);
}

void drawXmpValue(std::string_view label, const nlohmann::json& value, size_t depth, size_t& rows) {
  --rows;
  auto line = std::string(xmpPreview(label));
  const auto* display = &value;
  bool ordered = false;
  if (const auto literal = value.find("@value"); literal != value.end()) {
    const auto language = value.find("@language");
    if (language != value.end() && language->is_string())
      line += " [" + std::string(xmpPreview(language->get_ref<const std::string&>())) + "]";
    display = &*literal;
  } else {
    for (const auto keyword : {std::string_view("@list"), std::string_view("@set")}) {
      if (const auto entries = value.find(keyword); entries != value.end() && entries->is_array()) {
        display = &*entries;
        ordered = keyword == "@list";
        break;
      }
    }
  }

  if (!display->is_structured() || display->empty()) {
    const auto encoded = display->is_string() ? std::string{} : display->dump();
    const std::string_view scalar = display->is_string() ? display->get_ref<const std::string&>() : encoded;
    line += "   ";
    line += xmpPreview(scalar);
    if (scalar.size() > kMaxValueBytes) line += " ...";
  }
  ImGui::PushTextWrapPos();
  ImGui::TextUnformatted(line.data(), line.data() + line.size());
  ImGui::PopTextWrapPos();
  if (!display->is_structured() || display->empty()) return;
  if (depth == kMaxSummaryDepth) {
    ImGui::TextDisabled("Nested values are available in the raw JSON.");
    return;
  }
  ImGui::Indent();
  size_t index = 0;
  for (auto entry = display->begin(); entry != display->end() && rows; ++entry) {
    if (display->is_array()) {
      const auto item = ordered ? std::to_string(++index) + "." : "-";
      drawXmpValue(item, *entry, depth + 1, rows);
    } else if (entry.key() != "@context") {
      drawXmpValue(entry.key(), *entry, depth + 1, rows);
    }
  }
  ImGui::Unindent();
}

} // namespace

void GltfXmpInspector::setSource(const tinygltf::Model* model, uint64_t graphIdentity, uint64_t generation) {
  // The source model lives at a stable address across scene publications.
  if (model_ == model && graphIdentity_ == graphIdentity && generation_ == generation) return;
  previews_ = {};
  package_.reset();
  model_ = model;
  graphIdentity_ = graphIdentity;
  generation_ = generation;
}

void GltfXmpInspector::drawAsset() {
  using geometry::gltf::kGltfXmpExtension;
  if (model_ && (!model_->scenes.empty() || !model_->nodes.empty() || !model_->meshes.empty() ||
                 model_->extensions.contains(kGltfXmpExtension) || model_->asset.extensions.contains(kGltfXmpExtension)))
    draw(Scope::Asset, model_->asset.extensions);
}

void GltfXmpInspector::drawScene(int selectedScene) {
  if (model_ && selectedScene >= 0 && size_t(selectedScene) < model_->scenes.size())
    draw(Scope::Scene, model_->scenes[size_t(selectedScene)].extensions);
}

void GltfXmpInspector::drawSelection(const scene::SceneGraph& graph, uint32_t selectedNode, bool hasBimSelection) {
  if (!model_ || hasBimSelection) return;
  const auto* selected = graph.getNode(selectedNode);
  if (!selected || selected->retired) return;
  const auto* owner = selected->sourceNodeIndex >= 0 ? selected : graph.getNode(selected->meshOwnerGraphNode);
  int sourceMesh = selected->sourceMeshIndex;
  if (owner && !owner->retired && owner->sourceNodeIndex >= 0 && size_t(owner->sourceNodeIndex) < model_->nodes.size()) {
    const auto& sourceNode = model_->nodes[size_t(owner->sourceNodeIndex)];
    draw(Scope::Node, sourceNode.extensions);
    if (sourceMesh < 0) sourceMesh = sourceNode.mesh;
  }
  if (sourceMesh >= 0 && size_t(sourceMesh) < model_->meshes.size())
    draw(Scope::Mesh, model_->meshes[size_t(sourceMesh)].extensions);
}

void GltfXmpInspector::drawSurfaceMaterial(int sourceMaterial) {
  if (model_ && sourceMaterial >= 0 && size_t(sourceMaterial) < model_->materials.size())
    draw(Scope::SurfaceMaterial, model_->materials[size_t(sourceMaterial)].extensions);
}

void GltfXmpInspector::drawCatalogMaterial(int sourceMaterial) {
  if (model_ && sourceMaterial >= 0 && size_t(sourceMaterial) < model_->materials.size())
    draw(Scope::CatalogMaterial, model_->materials[size_t(sourceMaterial)].extensions);
}

void GltfXmpInspector::drawAnimation(int selectedClip) {
  if (model_ && selectedClip >= 0 && size_t(selectedClip) < model_->animations.size())
    draw(Scope::Animation, model_->animations[size_t(selectedClip)].extensions);
}

void GltfXmpInspector::drawImage(const std::optional<ResourceCatalogTarget>& selected,
                                 std::span<const tinygltf::Image> archiveImages) {
  if (!model_ || !selected || selected->kind != ResourceCatalogKind::Image) return;
  if (selected->domain == ResourceCatalogDomain::Archive && selected->row < archiveImages.size())
    draw(Scope::Image, archiveImages[selected->row].extensions);
  else if (selected->domain == ResourceCatalogDomain::Evaluated && selected->row < model_->images.size())
    draw(Scope::Image, model_->images[selected->row].extensions);
}

void GltfXmpInspector::draw(Scope scope, const tinygltf::ExtensionMap& extensions) {
  static constexpr std::array labels{
      "glTF asset metadata", "glTF scene metadata", "glTF node metadata", "glTF mesh metadata",
      "glTF surface material metadata", "glTF catalog material metadata", "glTF animation metadata", "glTF image metadata"};
  const auto index = static_cast<size_t>(scope);
  if (!ImGui::TreeNode(labels[index])) return;

  using namespace geometry::gltf;
  if (!package_ && extensions.contains(kGltfXmpExtension))
    package_.emplace(ReadGltfXmpPackage(*model_).value_or(nlohmann::json{}));
  static const nlohmann::json emptyPackage;
  const auto resolved = ResolveGltfXmpPacket(package_ ? *package_ : emptyPackage, extensions);
  if (const auto* packet = resolved.packet) {
    size_t rows = kMaxSummaryRows;
    for (auto entry = packet->begin(); entry != packet->end() && rows; ++entry)
      if (entry.key() != "@context") drawXmpValue(entry.key(), *entry, 0, rows);
    if (rows == kMaxSummaryRows) ImGui::TextDisabled("This packet contains only the JSON-LD context.");
    if (!rows) ImGui::TextDisabled("Additional values are available in the raw JSON.");
    if (ImGui::TreeNode("Raw packet JSON")) {
      if (ImGui::Button("Copy JSON")) ImGui::SetClipboardText(packet->dump(2).c_str());
      auto& preview = previews_[index];
      if (preview.packet != packet) {
        const auto raw = packet->dump(2);
        const auto prefix = xmpPreview(raw, kMaxRawPreviewBytes);
        preview.text.assign(prefix);
        preview.shortened = prefix.size() < raw.size();
        preview.packet = packet;
      }
      if (ImGui::BeginChild("##RawPacket", ImVec2(0.0f, 12 * ImGui::GetTextLineHeightWithSpacing()),
                            ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar))
        ImGui::TextUnformatted(preview.text.c_str());
      ImGui::EndChild();
      if (preview.shortened) ImGui::TextDisabled("Preview shortened. Copy JSON includes the full packet.");
      ImGui::TreePop();
    }
  } else {
    ImGui::TextWrapped("%.*s", int(resolved.message.size()), resolved.message.data());
  }
  ImGui::TreePop();
}

} // namespace container::ui::detail
