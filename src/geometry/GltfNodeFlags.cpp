#include <Container/geometry/GltfNodeFlags.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace container::geometry::gltf {
namespace {

struct FlagProperty {
  const char* extension;
  const char* property;
  bool GltfNodeFlags::*value;
  GltfNodeFlag flag;
};
constexpr std::array kFlags{
    FlagProperty{"KHR_node_visibility", "visible", &GltfNodeFlags::visible, GltfNodeFlag::Visibility},
    FlagProperty{"KHR_node_selectability", "selectable", &GltfNodeFlags::selectable, GltfNodeFlag::Selectability},
    FlagProperty{"KHR_node_hoverability", "hoverable", &GltfNodeFlags::hoverable, GltfNodeFlag::Hoverability},
};

[[noreturn]] void fail(std::string_view context, std::string_view message) {
  throw std::runtime_error("glTF node flags " + std::string(context) + ": " + std::string(message));
}

GltfNodeFlagSummary checkedSummary(size_t count, const GltfNodeFlagLimits& limits) {
  if (!limits.maxNodes || limits.maxNodes > INT_MAX || !limits.maxRetainedBytes ||
      limits.maxRetainedBytes == UINT64_MAX)
    fail("limits", "node and retained-byte limits must be finite and positive, within int identity range");
  if (count > limits.maxNodes) fail("nodes", "source node count exceeds its limit");
  if (count > std::min<uint64_t>(limits.maxRetainedBytes, SIZE_MAX) / sizeof(GltfNodeFlags))
    fail("nodes", "local flag storage exceeds its retained-byte budget");
  return {uint64_t(count), 0, 0, uint64_t(count) * sizeof(GltfNodeFlags)};
}

std::string nodeLabel(size_t index) {
  return "node[" + std::to_string(index) + "]";
}

std::string propertyLabel(size_t index, const FlagProperty& flag) {
  return nodeLabel(index) + ".extensions." + flag.extension;
}

void authored(GltfNodeFlags& result, const FlagProperty& flag, bool propertyPresent) {
  result.authoredExtensions |= static_cast<uint8_t>(flag.flag);
  if (propertyPresent) result.authoredProperties |= static_cast<uint8_t>(flag.flag);
}

void countAuthored(GltfNodeFlagSummary& summary, const GltfNodeFlags& flags) {
  if (flags.authoredExtensions) ++summary.authoredNodes;
  summary.authoredExtensions += std::popcount(static_cast<unsigned>(flags.authoredExtensions));
}

const tinygltf::Value::Object& flagFields(const tinygltf::Value& extension, const FlagProperty& flag) {
  if (!extension.IsObject()) fail(flag.extension, "extension must be an object");
  return extension.Get<tinygltf::Value::Object>();
}

} // namespace

GltfNodeFlagSummary ValidateGltfNodeFlagsJson(const nlohmann::json& source,
                                           const GltfNodeFlagLimits& limits) {
  if (!source.is_object()) fail("asset", "source must be an object");
  const auto nodes = source.find("nodes");
  if (nodes != source.end() && !nodes->is_array()) fail("nodes", "must be an array");
  auto summary = checkedSummary(nodes == source.end() ? 0 : nodes->size(), limits);
  const auto used = source.find("extensionsUsed");
  if (used != source.end()) {
    if (!used->is_array()) fail("extensionsUsed", "must be an array");
    for (const auto& name : *used)
      if (!name.is_string()) fail("extensionsUsed", "extension names must be strings");
  }
  std::array<bool, kFlags.size()> declared{};
  if (used != source.end()) for (size_t i = 0; i < kFlags.size(); ++i)
    declared[i] = std::ranges::any_of(*used, [&](const auto& value) {
      return value.template get_ref<const std::string&>() == kFlags[i].extension;
    });
  for (size_t index = 0; index < summary.nodes; ++index) {
    const auto& node = nodes->at(index);
    if (!node.is_object()) fail(nodeLabel(index), "must be an object");
    const auto extensions = node.find("extensions");
    if (extensions == node.end()) continue;
    if (!extensions->is_object()) fail(nodeLabel(index) + ".extensions", "must be an object");
    GltfNodeFlags flags;
    for (size_t i = 0; i < kFlags.size(); ++i) {
      const auto& flag = kFlags[i];
      const auto extension = extensions->find(flag.extension);
      if (extension == extensions->end()) continue;
      const auto context = propertyLabel(index, flag);
      if (!declared[i]) fail(context, "extension must be declared in extensionsUsed");
      if (!extension->is_object()) fail(context, "extension must be an object");
      const auto property = extension->find(flag.property);
      if (property != extension->end()) {
        if (!property->is_boolean()) fail(context + "." + flag.property, "must be a boolean");
        flags.*(flag.value) = property->get<bool>();
      }
      authored(flags, flag, property != extension->end());
    }
    countAuthored(summary, flags);
  }
  return summary;
}

GltfNodeFlagData ParseGltfNodeFlags(const tinygltf::Model& source,
                                 const GltfNodeFlagLimits& limits) {
  GltfNodeFlagData result;
  result.summary = checkedSummary(source.nodes.size(), limits);
  std::array<bool, kFlags.size()> declared{};
  for (size_t i = 0; i < kFlags.size(); ++i)
    declared[i] = std::ranges::find(source.extensionsUsed, kFlags[i].extension) != source.extensionsUsed.end();
  result.nodes.resize(source.nodes.size());
  for (size_t index = 0; index < source.nodes.size(); ++index) {
    auto& flags = result.nodes[index];
    for (size_t i = 0; i < kFlags.size(); ++i) {
      const auto& flag = kFlags[i];
      const auto extension = source.nodes[index].extensions.find(flag.extension);
      if (extension == source.nodes[index].extensions.end()) continue;
      const auto context = propertyLabel(index, flag);
      if (!declared[i]) fail(context, "extension must be declared in extensionsUsed");
      const auto& value = extension->second;
      if (!value.IsObject()) fail(context, "extension must be an object");
      const bool present = value.Has(flag.property);
      if (present) {
        const auto& property = value.Get(flag.property);
        if (!property.IsBool()) fail(context + "." + flag.property, "must be a boolean");
        flags.*(flag.value) = property.Get<bool>();
      }
      authored(flags, flag, present);
    }
    countAuthored(result.summary, flags);
  }
  return result;
}

GltfNodeFlagEdit PrepareGltfNodeFlagEdit(const tinygltf::Node& evaluated, const tinygltf::Node& archived,
                                       const std::array<std::optional<bool>, 3>& changes) {
  GltfNodeFlagEdit edit;
  for (size_t i = 0; i < kFlags.size(); ++i) {
    if (!changes[i]) continue;
    const auto& flag = kFlags[i];
    const auto extension = evaluated.extensions.find(flag.extension);
    if (extension == evaluated.extensions.end()) continue;
    const auto& fields = flagFields(extension->second, flag);
    const auto existing = fields.find(flag.property);
    if (existing != fields.end() && !existing->second.IsBool())
      fail(flag.extension, "flag property must be a boolean");
    authored(edit.before, flag, existing != fields.end());
    edit.before.*(flag.value) = existing == fields.end() || existing->second.Get<bool>();

    const auto original = archived.extensions.find(flag.extension);
    const bool explicitValue = original != archived.extensions.end() &&
        flagFields(original->second, flag).contains(flag.property);
    authored(edit.after, flag, explicitValue || !*changes[i]);
    edit.after.*(flag.value) = *changes[i];
  }
  return edit;
}

void ApplyGltfNodeFlagEdit(tinygltf::Node& evaluated, const GltfNodeFlags& flags) {
  using Fields = tinygltf::Value::Object;
  std::array<Fields*, kFlags.size()> fields{};
  std::array<Fields::iterator, kFlags.size()> existing{};
  std::array<Fields::node_type, kFlags.size()> insertions;
  Fields prepared;
  for (size_t i = 0; i < kFlags.size(); ++i) {
    const auto& flag = kFlags[i];
    if (!flags.hasExtension(flag.flag)) continue;
    const auto extension = evaluated.extensions.find(flag.extension);
    if (extension == evaluated.extensions.end() || !extension->second.IsObject())
      fail(flag.extension, "edited extension object is unavailable");
    fields[i] = &extension->second.Get<Fields>();
    existing[i] = fields[i]->find(flag.property);
    if (existing[i] != fields[i]->end()) {
      if (!existing[i]->second.IsBool()) fail(flag.extension, "flag property must be a boolean");
    } else if (flags.hasProperty(flag.flag)) {
      // Prepare every allocation before modifying any flag in a grouped edit.
      const auto entry = prepared.emplace(flag.property, tinygltf::Value(flags.*(flag.value))).first;
      insertions[i] = prepared.extract(entry);
    }
  }
  bool changed = false;
  for (size_t i = 0; i < kFlags.size(); ++i) {
    if (!fields[i]) continue;
    if (!insertions[i].empty()) {
      fields[i]->insert(std::move(insertions[i]));
    } else if (existing[i] == fields[i]->end()) {
      continue;
    } else if (!flags.hasProperty(kFlags[i].flag)) {
      fields[i]->erase(existing[i]);
    } else {
      auto& value = existing[i]->second.Get<bool>();
      const bool next = flags.*(kFlags[i].value);
      if (value == next) continue;
      value = next;
    }
    changed = true;
  }
  if (changed) evaluated.extensions_json_string.clear();
}

} // namespace container::geometry::gltf
