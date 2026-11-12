#include <Container/geometry/GltfXmpMetadata.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace container::geometry::gltf {
namespace {

using Json = nlohmann::json;

constexpr auto kInvalidReference = "XMP packet reference must be an object with a nonnegative integer packet index";
constexpr auto kInvalidTable = "XMP root extension must contain a nonempty packets array";
constexpr auto kInvalidPacket = "XMP packet must be an object with an @context object";
constexpr int kInvalidPacketIndex = std::numeric_limits<int>::max();

std::optional<int> sourcePacketIndex(const Json& value) {
  if (value.is_number_unsigned()) {
    const auto number = value.get<uint64_t>();
    return number < kInvalidPacketIndex ? static_cast<int>(number) : kInvalidPacketIndex;
  } else if (value.is_number_integer()) {
    const auto number = value.get<int64_t>();
    if (number >= 0) return number < kInvalidPacketIndex ? static_cast<int>(number) : kInvalidPacketIndex;
  } else if (value.is_number_float()) {
    // glTF allows integral values encoded with a decimal or exponent.
    const auto number = value.get<double>();
    if (number >= 0 && std::isfinite(number) && std::trunc(number) == number)
      return number < kInvalidPacketIndex ? static_cast<int>(number) : kInvalidPacketIndex;
  }
  return std::nullopt;
}

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

void checkDepth(const Json& value, uint32_t remaining) {
  if (value.empty() || !value.is_structured()) return;
  if (!remaining) throw std::runtime_error("glTF merge: XMP package exceeds the metadata nesting limit");
  for (const auto& child : value) checkDepth(child, remaining - 1);
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
    const auto index = id == extension->end() ? std::nullopt : sourcePacketIndex(*id);
    if (!index) {
      warn(context, kInvalidReference);
      // An empty extension remains visible as malformed in the inspector.
      *extension = Json::object();
      rewritten_ = true;
      return;
    }
    if (!packets) {
      warn(context, kInvalidTable);
      *id = kInvalidPacketIndex;
      rewritten_ = true;
    } else if (*index == kInvalidPacketIndex || static_cast<size_t>(*index) >= packets->size()) {
      warn(context, "XMP packet index is outside the packets array");
      *id = kInvalidPacketIndex;
      rewritten_ = true;
    } else {
      if (id->is_number_float()) {
        *id = *index;
        rewritten_ = true;
      }
      if (!isPacket((*packets)[static_cast<size_t>(*index)])) warn(context, kInvalidPacket);
    }
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

GltfXmpComposition ComposeGltfXmpPackets(const tinygltf::Model& destination,
    const tinygltf::Model& incoming, uint32_t maxMetadataDepth) {
  auto base = ReadGltfXmpPackage(destination);
  auto added = ReadGltfXmpPackage(incoming);
  GltfXmpComposition result;
  if (!base && !added) return result;

  Json package = Json::object();
  Json packets = Json::array();
  const auto append = [&](std::optional<Json>& source) -> size_t {
    if (!source) return 0;
    checkDepth(*source, maxMetadataDepth);
    if (!source->is_object()) return 0;
    size_t count = 0;
    for (auto entry = source->begin(); entry != source->end(); ++entry) {
      if (entry.key() == "packets") {
        if (!entry->is_array()) continue;
        count = entry->size();
        if (count > size_t(kInvalidPacketIndex) - packets.size())
          throw std::runtime_error("glTF merge: XMP packet index limit exceeded");
        for (auto& packet : *entry) packets.push_back(std::move(packet));
      } else if (const auto previous = package.find(entry.key()); previous == package.end()) {
        package[entry.key()] = std::move(*entry);
      } else if (*previous != *entry) {
        throw std::runtime_error("glTF merge: conflicting XMP package metadata: " + entry.key());
      }
    }
    return count;
  };
  result.destinationPackets = append(base);
  result.incomingPackets = append(added);
  if (!packets.empty()) package["packets"] = std::move(packets);
  result.rootExtensionsJson = Json{{kGltfXmpExtension, std::move(package)}}.dump();
  return result;
}

bool RemapGltfXmpAttachment(tinygltf::ExtensionMap& extensions, size_t sourcePackets, size_t offset) {
  const auto xmp = extensions.find(kGltfXmpExtension);
  if (xmp == extensions.end() || !xmp->second.IsObject()) return false;
  auto& fields = xmp->second.Get<tinygltf::Value::Object>();
  const auto packet = fields.find("packet");
  if (packet == fields.end() || !packet->second.IsInt() || packet->second.Get<int>() < 0) return false;
  const int index = packet->second.Get<int>();
  const int remapped = size_t(index) < sourcePackets ? int(offset + size_t(index)) : kInvalidPacketIndex;
  if (remapped == index) return false;
  packet->second = tinygltf::Value(remapped);
  return true;
}

}  // namespace container::geometry::gltf
