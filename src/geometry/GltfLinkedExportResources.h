#pragma once

#include <Container/geometry/GltfAnimation.h>
#include <Container/geometry/GltfLinkedAssets.h>
#include <Container/geometry/GltfMaterialExportCompaction.h>
#include <Container/geometry/GltfResourceExportCompaction.h>
#include <Container/geometry/GltfXmpMetadata.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace container::geometry::gltf::detail {
namespace linked_export {

using Value = tinygltf::Value;
using Contexts = std::span<const GltfLinkedSourceOwnership>;
constexpr auto kLimits = GltfResourceExportCompactionLimits{};
constexpr char kPunctual[] = "KHR_lights_punctual";
constexpr char kVariants[] = "KHR_materials_variants";

[[noreturn]] inline void invalid(std::string_view reason) {
  throw std::runtime_error("Linked scene export resources: " + std::string(reason));
}

inline size_t checkedCount(size_t size) {
  if (size > kLimits.maxTableEntries) invalid("table exceeds its finite capacity");
  return size;
}

class Projection {
public:
  explicit Projection(size_t size, bool keep = true) : retained(checkedCount(size), uint8_t(keep)) {}

  void omit(GltfLinkedIndexRange range) {
    if (!range.count) return;
    const auto first = checked(range.first);
    if (range.count > retained.size() - first) invalid("linked ownership exceeds the projected table");
    std::fill_n(retained.begin() + first, range.count, uint8_t(0));
  }

  void omit(int id) { retained[checked(id)] = 0; }
  void include(int id) { if (id != -1) retained[checked(id)] = 1; }
  bool keeps(size_t id) const { return retained.at(id) != 0; }
  bool changed() const { return std::ranges::find(retained, uint8_t(0)) != retained.end(); }

  std::vector<int32_t> omitted() const {
    std::vector<int32_t> ids;
    for (size_t id = 0; id < retained.size(); ++id)
      if (!keeps(id)) ids.push_back(int32_t(id));
    return ids;
  }

  void seal() {
    indices.resize(retained.size(), -1);
    int32_t next = 0;
    for (size_t id = 0; id < retained.size(); ++id)
      if (keeps(id)) indices[id] = next++;
  }

  int mapped(int id) const {
    if (id == -1) return id;
    const auto index = indices.at(checked(id));
    if (index < 0) invalid("surviving reference targets a removed row");
    return index;
  }

  template<class T> void compact(std::vector<T>& table) const {
    if (table.size() != retained.size()) invalid("table changed during projection");
    size_t next = 0;
    for (size_t id = 0; id < table.size(); ++id) {
      if (!keeps(id)) continue;
      if (next != id) table[next] = std::move(table[id]);
      ++next;
    }
    table.resize(next);
  }

  std::vector<uint8_t> retained;

private:
  size_t checked(int id) const {
    if (id < 0 || size_t(id) >= retained.size()) invalid("reference exceeds the projected table");
    return size_t(id);
  }
  std::vector<int32_t> indices;
};

inline Value* extension(tinygltf::ExtensionMap& extensions, const char* name) {
  const auto found = extensions.find(name);
  return found == extensions.end() ? nullptr : &found->second;
}

inline Value* field(Value& value, const char* name) {
  if (!value.IsObject()) invalid("extension record must be an object");
  auto& object = value.Get<Value::Object>();
  const auto found = object.find(name);
  return found == object.end() ? nullptr : &found->second;
}

inline Value& requireField(Value& value, const char* name) {
  const auto found = field(value, name);
  if (!found) invalid("extension record lacks a required field");
  return *found;
}

inline Value::Array& array(Value& value) {
  if (!value.IsArray()) invalid("extension table must be an array");
  checkedCount(value.ArrayLen());
  return value.Get<Value::Array>();
}

inline int& index(Value& value) {
  if (!value.IsInt() || value.Get<int>() < 0) invalid("extension reference must be a nonnegative integer");
  return value.Get<int>();
}

template<class Visit> inline void forResourcePointers(tinygltf::Model& model, Visit visit) {
  for (auto& clip : model.animations) for (auto& channel : clip.channels) {
    if (channel.target_path != "pointer") continue;
    auto* extensionValue = extension(channel.target_extensions, "KHR_animation_pointer");
    if (!extensionValue) invalid("animation pointer extension is missing");
    auto& value = requireField(*extensionValue, "pointer");
    if (!value.IsString()) invalid("animation pointer must be a string");
    auto& path = value.Get<std::string>();
    if (path.starts_with("/nodes/")) continue; // Already projected by node compaction.
    const auto target = ResolveAnimationPointer(model, path);
    if (!target) invalid("unsupported animation resource pointer");
    visit(channel, path, *target);
  }
}

inline void remapPointer(tinygltf::AnimationChannel& channel, std::string& path, std::string_view prefix, int id) {
  const auto suffix = path.find('/', prefix.size());
  if (!path.starts_with(prefix) || suffix == std::string::npos) invalid("unsupported animation pointer index");
  path.replace(prefix.size(), suffix - prefix.size(), std::to_string(id));
  channel.target_extensions_json_string.clear();
}

template<class Visit> inline void forVariantMappings(tinygltf::Model& model, Visit visit) {
  for (auto& mesh : model.meshes) for (auto& primitive : mesh.primitives) {
    if (auto* value = extension(primitive.extensions, kVariants))
      for (auto& mapping : array(requireField(*value, "mappings"))) visit(primitive, mapping);
  }
}

template<class Include> inline void compactVariants(tinygltf::Model& model, Contexts contexts, Include includeMaterial) {
  auto* root = extension(model.extensions, kVariants);
  if (!root) return;
  auto& rows = array(requireField(*root, "variants"));
  Projection variants(rows.size());
  for (const auto& context : contexts) variants.omit(context.variants);
  forVariantMappings(model, [&](auto&, auto& mapping) {
    includeMaterial(index(requireField(mapping, "material")));
    for (auto& value : array(requireField(mapping, "variants"))) variants.include(index(value));
  });
  if (!variants.changed()) return;
  variants.seal();
  forVariantMappings(model, [&](auto& primitive, auto& mapping) {
    for (auto& value : array(requireField(mapping, "variants"))) {
      auto& id = index(value);
      const auto mapped = variants.mapped(id);
      if (id != mapped) primitive.extensions_json_string.clear();
      id = mapped;
    }
  });
  variants.compact(rows);
  if (rows.empty()) {
    model.extensions.erase(kVariants);
    std::erase(model.extensionsUsed, kVariants);
    std::erase(model.extensionsRequired, kVariants);
  }
}

inline void compactIndexedLights(tinygltf::Model& model, Contexts contexts) {
  for (const auto* name : {"KHR_lights_area", "EXT_lights_area", "WEBGI_lights_area", "WEBGI_area_light"}) {
    auto* root = extension(model.extensions, name);
    if (!root) continue;
    auto& lights = array(requireField(*root, "lights"));
    Projection projection(lights.size());
    for (const auto& context : contexts)
      if (const auto found = context.extensionLights.find(name); found != context.extensionLights.end())
        projection.omit(found->second);
    const auto visitBindings = [&](auto visit) {
      for (auto& node : model.nodes)
        if (auto* binding = extension(node.extensions, name))
          if (auto* id = field(*binding, "light")) visit(node, index(*id));
    };
    visitBindings([&](auto&, int id) { projection.include(id); });
    if (!projection.changed()) continue;
    projection.seal();
    visitBindings([&](auto& node, int& id) {
      const auto mapped = projection.mapped(id);
      if (id != mapped) node.extensions_json_string.clear();
      id = mapped;
    });
    projection.compact(lights);
    if (lights.empty()) model.extensions.erase(name);
  }
}

template<class Visit> inline void forXmpAttachments(tinygltf::Model& model, Visit visit) {
  const auto record = [&](auto& value) {
    auto* attachment = extension(value.extensions, kGltfXmpExtension);
    if (!attachment) return;
    auto* packet = attachment->IsObject() ? field(*attachment, "packet") : nullptr;
    visit(value, packet && packet->IsInt() ? &packet->template Get<int>() : nullptr);
  };
  const auto table = [&](auto& values) { for (auto& value : values) record(value); };
  record(model.asset);
  table(model.nodes); table(model.scenes); table(model.meshes); table(model.materials);
  table(model.images); table(model.animations);
}

inline void compactXmp(tinygltf::Model& model, Contexts contexts) {
  if (std::ranges::none_of(contexts, [](const auto& context) { return context.xmpPackets.count != 0; })) return;
  if (model.extensions_json_string.size() > kLimits.references.maxMetadataBytes)
    invalid("retained XMP package exceeds its finite capacity");
  auto extensions = nlohmann::json::parse(model.extensions_json_string, nullptr, false);
  if (!extensions.is_object() || !extensions.contains(kGltfXmpExtension)) invalid("linked XMP packet table is unavailable");
  auto& package = extensions[kGltfXmpExtension];
  if (!package.is_object() || !package.contains("packets") || !package["packets"].is_array())
    invalid("linked XMP packet table is unavailable");
  auto& packets = package["packets"].get_ref<nlohmann::json::array_t&>();
  Projection projection(packets.size());
  for (const auto& context : contexts) projection.omit(context.xmpPackets);
  const auto valid = [&](int id) { return id >= 0 && size_t(id) < packets.size(); };
  bool hasAttachment = false;
  forXmpAttachments(model, [&](auto&, const int* id) {
    hasAttachment = true;
    if (id && valid(*id)) projection.include(*id);
  });
  if (!projection.changed()) return;
  projection.seal();
  forXmpAttachments(model, [&](auto& record, int* id) {
    if (!id || !valid(*id)) return;
    const auto mapped = projection.mapped(*id);
    if (*id != mapped) record.extensions_json_string.clear();
    *id = mapped;
  });
  projection.compact(packets);
  if (packets.empty()) package.erase("packets");
  if (package.empty() && !hasAttachment) {
    extensions.erase(kGltfXmpExtension);
    model.extensions.erase(kGltfXmpExtension);
    std::erase(model.extensionsUsed, kGltfXmpExtension);
    std::erase(model.extensionsRequired, kGltfXmpExtension);
  }
  model.extensions_json_string = extensions.empty() ? std::string{} : extensions.dump();
}

inline void compactTextures(tinygltf::Model& model, Contexts contexts) {
  Projection images(model.images.size()), textures(model.textures.size()), samplers(model.samplers.size());
  for (const auto& context : contexts) {
    images.omit(context.images);
    textures.omit(context.textures);
    samplers.omit(context.samplers);
  }
  if (!images.changed() && !textures.changed() && !samplers.changed()) return;
  const auto references = ExtractGltfTextureResourceReferences(model);
  for (size_t material = 0; material < model.materials.size(); ++material)
    for (const auto texture : references.materialTextures(uint32_t(material))) textures.include(int(texture));
  for (const auto& reference : references.sourceReferences()) {
    switch (reference.kind) {
      case GltfTextureResourceReferenceKind::Texture: textures.include(int(reference.index)); break;
      case GltfTextureResourceReferenceKind::Image: images.include(int(reference.index)); break;
      case GltfTextureResourceReferenceKind::Sampler: samplers.include(int(reference.index)); break;
    }
  }
  for (size_t texture = 0; texture < model.textures.size(); ++texture) {
    if (!textures.keeps(texture)) continue;
    for (const auto image : references.textureImages(uint32_t(texture))) images.include(int(image));
    for (const auto sampler : references.textureSamplers(uint32_t(texture))) samplers.include(int(sampler));
  }
  if (!images.changed() && !textures.changed() && !samplers.changed()) return;
  const auto plan = PlanGltfResourceExportCompaction(model, {images.retained, textures.retained, samplers.retained});
  ApplyGltfResourceExportCompaction(model, plan);
}

inline void repackViews(tinygltf::Model& model) {
  if (model.bufferViews.empty()) { model.buffers.clear(); return; }
  if (model.buffers.size() != 1 || model.buffers.front().data.size() > kLimits.maxPayloadBytes)
    invalid("expected one bounded resident export buffer");
  auto& source = model.buffers.front().data;
  std::vector<size_t> order(model.bufferViews.size());
  std::iota(order.begin(), order.end(), size_t(0));
  for (const auto& view : model.bufferViews)
    if (view.buffer != 0 || view.byteOffset > source.size() || view.byteLength > source.size() - view.byteOffset)
      invalid("buffer view exceeds its resident payload");
  std::ranges::sort(order, {}, [&](size_t id) { return model.bufferViews[id].byteOffset; });
  std::vector<unsigned char> payload;
  for (size_t cursor = 0; cursor < order.size();) {
    const size_t first = cursor;
    const auto& view = model.bufferViews[order[cursor++]];
    const size_t begin = view.byteOffset;
    size_t end = begin + view.byteLength;
    while (cursor < order.size() && model.bufferViews[order[cursor]].byteOffset <= end) {
      const auto& next = model.bufferViews[order[cursor++]];
      end = std::max(end, next.byteOffset + next.byteLength);
    }
    // Keep every retained byte offset congruent modulo eight. Overlapping
    // views share one copied interval and preserve their original alignment.
    payload.resize(payload.size() + (begin % 8 + 8 - payload.size() % 8) % 8);
    const size_t destination = payload.size();
    if (destination > begin) invalid("repacked buffer exceeds its original extent");
    payload.insert(payload.end(), source.begin() + begin, source.begin() + end);
    for (size_t i = first; i < cursor; ++i)
      model.bufferViews[order[i]].byteOffset = destination + (model.bufferViews[order[i]].byteOffset - begin);
  }
  source = std::move(payload);
}

inline void compactPayload(tinygltf::Model& model) {
  Projection accessors(model.accessors.size(), false), views(model.bufferViews.size(), false);
  const auto visitAccessors = [&](auto visit) {
    for (auto& mesh : model.meshes) for (auto& primitive : mesh.primitives) {
      visit(primitive.indices);
      for (auto& [_, id] : primitive.attributes) visit(id);
      for (auto& target : primitive.targets) for (auto& [_, id] : target) visit(id);
    }
    for (auto& skin : model.skins) visit(skin.inverseBindMatrices);
    for (auto& animation : model.animations) for (auto& sampler : animation.samplers) {
      visit(sampler.input);
      visit(sampler.output);
    }
    for (auto& node : model.nodes)
      if (auto* instances = extension(node.extensions, "EXT_mesh_gpu_instancing")) {
        auto& attributes = requireField(*instances, "attributes");
        if (!attributes.IsObject()) invalid("instancing attributes must be an object");
        for (auto& [_, id] : attributes.Get<Value::Object>()) visit(index(id));
        node.extensions_json_string.clear();
      }
  };
  for (auto& animation : model.animations) {
    Projection samplers(animation.samplers.size(), false);
    for (const auto& channel : animation.channels) samplers.include(channel.sampler);
    samplers.seal();
    for (auto& channel : animation.channels) channel.sampler = samplers.mapped(channel.sampler);
    samplers.compact(animation.samplers);
  }
  visitAccessors([&](int id) { accessors.include(id); });
  accessors.seal();
  visitAccessors([&](int& id) { id = accessors.mapped(id); });
  accessors.compact(model.accessors);

  const auto visitViews = [&](auto visit) {
    for (auto& accessor : model.accessors) {
      visit(accessor.bufferView);
      if (accessor.sparse.isSparse) {
        visit(accessor.sparse.indices.bufferView);
        visit(accessor.sparse.values.bufferView);
      }
    }
    for (auto& image : model.images) visit(image.bufferView);
  };
  visitViews([&](int id) { views.include(id); });
  views.seal();
  visitViews([&](int& id) { id = views.mapped(id); });
  views.compact(model.bufferViews);
  repackViews(model);
}

} // namespace linked_export

// Run after CompactGltfExportNodes has validated and projected the detached
// model and the clips supplied by live links have been removed. Ownership uses
// stable packed archive indices and the supplied source-to-export material map.
inline void compactLinkedExportResources(tinygltf::Model& model, std::span<const GltfLinkedSourceOwnership> contexts,
    std::span<const int32_t> sourceMaterialToExport,
    std::span<const int32_t> retiredMaterials = {}, std::span<const int32_t> animatedLinkedMaterials = {}) {
  using namespace linked_export;
  checkedCount(contexts.size());
  Projection meshes(model.meshes.size(), false), skins(model.skins.size(), false);
  Projection sourceMaterials(sourceMaterialToExport.size());
  Projection materials(model.materials.size()), cameras(model.cameras.size()), lights(model.lights.size());
  for (const auto& context : contexts) {
    const auto& range = context.materials;
    if (range.count) {
      sourceMaterials.omit(range);
      for (const auto id : sourceMaterialToExport.subspan(size_t(range.first), range.count)) materials.omit(id);
    }
    cameras.omit(context.cameras);
    lights.omit(context.lights);
  }
  for (size_t id = 0; id < sourceMaterialToExport.size(); ++id)
    if (sourceMaterials.keeps(id)) materials.include(sourceMaterialToExport[id]);
  // The raw root carries original typed lights. Portable light authoring can
  // append replacement rows, which survive only when still bound or animated.
  auto* punctual = extension(model.extensions, kPunctual);
  auto* rawLights = punctual ? &array(requireField(*punctual, "lights")) : nullptr;
  if (rawLights) {
    if (rawLights->size() > model.lights.size()) invalid("punctual light tables disagree");
    for (size_t id = rawLights->size(); id < model.lights.size(); ++id) lights.omit(int(id));
  }
  for (const auto& node : model.nodes) {
    meshes.include(node.mesh);
    skins.include(node.skin);
    cameras.include(node.camera);
    lights.include(node.light);
  }
  forResourcePointers(model, [&](auto&, auto&, const AnimationTarget& target) {
    if (target.kind == AnimationTargetKind::Material) materials.include(int(target.index));
    else if (target.kind == AnimationTargetKind::Camera) cameras.include(int(target.index));
    else if (target.kind == AnimationTargetKind::Light) lights.include(int(target.index));
  });
  meshes.seal(); skins.seal(); cameras.seal(); lights.seal();
  for (auto& node : model.nodes) {
    const int oldLight = node.light;
    node.mesh = meshes.mapped(node.mesh);
    node.skin = skins.mapped(node.skin);
    node.camera = cameras.mapped(node.camera);
    node.light = lights.mapped(node.light);
    if (auto* value = extension(node.extensions, kPunctual)) {
      auto& id = index(requireField(*value, "light"));
      if (id != oldLight) invalid("punctual light node bindings disagree");
      if (id != node.light) node.extensions_json_string.clear();
      id = node.light;
    }
  }
  forResourcePointers(model, [&](auto& channel, auto& path, const AnimationTarget& target) {
    if (target.kind == AnimationTargetKind::Camera)
      remapPointer(channel, path, "/cameras/", cameras.mapped(int(target.index)));
    else if (target.kind == AnimationTargetKind::Light)
      remapPointer(channel, path, "/extensions/KHR_lights_punctual/lights/", lights.mapped(int(target.index)));
  });
  meshes.compact(model.meshes);
  skins.compact(model.skins);
  cameras.compact(model.cameras);
  if (rawLights) {
    Projection raw(rawLights->size(), false);
    for (size_t id = 0; id < rawLights->size(); ++id) if (lights.keeps(id)) raw.include(int(id));
    raw.compact(*rawLights);
  }
  lights.compact(model.lights);
  if (model.lights.empty()) {
    model.extensions.erase(kPunctual);
    std::erase(model.extensionsUsed, kPunctual);
    std::erase(model.extensionsRequired, kPunctual);
  }
  compactIndexedLights(model, contexts);
  Projection animatedMaterials(model.materials.size(), false);
  for (const auto id : animatedLinkedMaterials) animatedMaterials.include(id);
  const auto includeInlineMaterial = [&](int id) {
    materials.include(id);
    if (id >= 0 && animatedMaterials.keeps(size_t(id)))
      invalid("an inline mesh shares a material animated by a linked asset");
  };
  for (const auto& mesh : model.meshes) for (const auto& primitive : mesh.primitives) includeInlineMaterial(primitive.material);
  compactVariants(model, contexts, includeInlineMaterial);
  for (const auto id : retiredMaterials) materials.omit(id);
  if (materials.changed()) {
    const auto plan = PlanGltfMaterialExportCompaction(model, materials.omitted());
    ApplyGltfMaterialExportCompaction(model, plan);
  }
  compactTextures(model, contexts);
  compactXmp(model, contexts);
  compactPayload(model);
}

} // namespace container::geometry::gltf::detail
