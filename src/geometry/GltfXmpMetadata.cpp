#include <Container/geometry/GltfXmpMetadata.h>

#include <nlohmann/json.hpp>

#include <limits>
#include <utility>

namespace container::geometry::gltf {
namespace {

using Json = nlohmann::json;

constexpr auto kInvalidReference = "XMP packet reference must be an object with a nonnegative integer packet index";
constexpr auto kInvalidTable = "XMP root extension must contain a nonempty packets array";
constexpr auto kInvalidPacket = "XMP packet must be an object with an @context object";

Json* findXmp(Json& record) {
  auto extensions = record.find("extensions");
  if (extensions == record.end() || !extensions->is_object()) return nullptr;
  auto xmp = extensions->find(kGltfXmpExtension);
  return xmp == extensions->end() ? nullptr : &*xmp;
}

const Json* packetsIn(const Json& package) {
  const auto packets = package.find("packets");
  return packets != package.end() && packets->is_array() && !packets->empty() ? &*packets : nullptr;
}

bool isPacket(const Json& packet) {
  const auto context = packet.find("@context");
  return context != packet.end() && context->is_object();
}

}  // namespace

GltfXmpImport::GltfXmpImport(Json& manifest) {
  constexpr size_t maxWarnings = 32;
  size_t omittedWarnings = 0;
  const auto warn = [&](std::string context, std::string_view message) {
    if (warnings_.size() < maxWarnings)
      warnings_.push_back(std::move(context) + ": " + std::string(message));
    else
      ++omittedWarnings;
  };

  Json package;
  const bool hasRoot = findXmp(manifest) != nullptr;
  if (auto* root = findXmp(manifest)) {
    package = std::exchange(*root, Json::object());
    rewritten_ = true;
  }
  const auto* packets = packetsIn(package);
  if (hasRoot && !packets) warn("root", kInvalidTable);
  if (packets)
    for (size_t i = 0; i < packets->size(); ++i)
      if (!isPacket((*packets)[i])) warn("packets[" + std::to_string(i) + "]", kInvalidPacket);

  const auto checkReference = [&](Json& record, const std::string& context) {
    auto* extension = findXmp(record);
    if (!extension) return;
    const auto id = extension->find("packet");
    if (id == extension->end() || !id->is_number_integer() ||
        *id < 0 || *id > std::numeric_limits<int>::max()) {
      warn(context, kInvalidReference);
      // An empty extension remains visible as malformed in the inspector.
      *extension = Json::object();
      rewritten_ = true;
      return;
    }
    const auto index = id->get<size_t>();
    if (!packets)
      warn(context, kInvalidTable);
    else if (index >= packets->size())
      warn(context, "XMP packet index is outside the packets array");
    else if (!isPacket((*packets)[index]))
      warn(context, kInvalidPacket);
  };

  if (auto asset = manifest.find("asset"); asset != manifest.end()) checkReference(*asset, "asset");
  for (const auto* table : {"scenes", "nodes", "meshes", "materials", "images", "animations"}) {
    auto records = manifest.find(table);
    if (records == manifest.end() || !records->is_array()) continue;
    for (size_t i = 0; i < records->size(); ++i) {
      auto& record = (*records)[i];
      if (findXmp(record)) checkReference(record, std::string(table) + "[" + std::to_string(i) + "]");
    }
  }

  if (omittedWarnings)
    warnings_.push_back(std::to_string(omittedWarnings) + " additional XMP diagnostics omitted");
  if (hasRoot) rootExtensionsJson_ = Json{{kGltfXmpExtension, std::move(package)}}.dump();
}

void GltfXmpImport::retain(tinygltf::Model& model) && {
  if (!rootExtensionsJson_.empty()) model.extensions_json_string = std::move(rootExtensionsJson_);
}

std::optional<Json> ReadGltfXmpPackage(const tinygltf::Model& model) {
  if (!model.extensions.contains(kGltfXmpExtension) || model.extensions_json_string.empty()) return {};
  auto extensions = Json::parse(model.extensions_json_string, nullptr, false);
  auto xmp = extensions.find(kGltfXmpExtension);
  if (xmp == extensions.end()) return {};
  return std::move(*xmp);
}

GltfXmpPacketView ResolveGltfXmpPacket(const Json& package, const tinygltf::ExtensionMap& extensions) {
  const auto found = extensions.find(kGltfXmpExtension);
  if (found == extensions.end()) return {nullptr, "No XMP packet is attached."};
  if (!found->second.IsObject()) return {nullptr, kInvalidReference};
  const auto& properties = found->second.Get<tinygltf::Value::Object>();
  const auto id = properties.find("packet");
  if (id == properties.end() || !id->second.IsInt() || id->second.Get<int>() < 0)
    return {nullptr, kInvalidReference};
  const auto* packets = packetsIn(package);
  if (!packets) return {nullptr, kInvalidTable};
  const auto index = size_t(id->second.Get<int>());
  if (index >= packets->size()) return {nullptr, "XMP packet index is outside the packets array"};
  const auto& packet = (*packets)[index];
  return isPacket(packet) ? GltfXmpPacketView{&packet, {}} : GltfXmpPacketView{nullptr, kInvalidPacket};
}

}  // namespace container::geometry::gltf
