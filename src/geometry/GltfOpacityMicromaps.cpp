#include <Container/geometry/GltfOpacityMicromaps.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>
#include <utility>

namespace container::geometry::gltf {
namespace {

using Value = tinygltf::Value;
using Attachment = GltfOpacityMicromapAttachment;
using Micromap = GltfOpacityMicromap;
using Triangle = GltfOpacityMicromapTriangle;
using Usage = GltfOpacityMicromapUsage;
using Json = nlohmann::json;
constexpr uint32_t maxDraftLevel = 12;
constexpr uint32_t firstSpecialIndex = UINT32_MAX - 3;
constexpr size_t bucketCount = 2 * (maxDraftLevel + 1);
using Histogram = std::array<uint32_t, bucketCount>;

class InvalidMicromap : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

[[noreturn]] void reject(std::string message) { throw InvalidMicromap(std::move(message)); }

struct Budget {
  size_t records, geometry, bytes;

  static void take(size_t& remaining, size_t count, const char* label) {
    if (count > remaining) reject(std::string(label) + " exceeds the aggregate micromap budget");
    remaining -= count;
  }

  void payload(size_t count, size_t width) {
    if (count > bytes / width) reject("Micromap payload exceeds the aggregate byte budget");
    bytes -= count * width;
  }
};

const Value* member(const Value& object, const char* name) {
  if (!object.IsObject()) reject("Expected an extension object");
  const auto& fields = object.Get<Value::Object>();
  const auto found = fields.find(name);
  return found != fields.end() ? &found->second : nullptr;
}

const Value& required(const Value& object, const char* name) {
  if (const auto* value = member(object, name)) return *value;
  reject(std::string("Missing ") + name);
}

const Value::Array& array(const Value& value, const char* name) {
  if (!value.IsArray()) reject(std::string(name) + " must be an array");
  return value.Get<Value::Array>();
}

uint32_t integer(const Value& value, const char* name) {
  if (!value.IsInt() || value.Get<int>() < 0)
    reject(std::string(name) + " must be an exact nonnegative integer");
  return static_cast<uint32_t>(value.Get<int>());
}

struct View {
  const tinygltf::BufferView& record;
  std::span<const std::byte> bytes;
};

View bufferView(const tinygltf::Model& source, size_t index, const char* name) {
  if (index >= source.bufferViews.size()) reject(std::string(name) + " bufferView is out of range");
  const auto& view = source.bufferViews[index];
  if (view.buffer < 0 || size_t(view.buffer) >= source.buffers.size())
    reject(std::string(name) + " bufferView references a missing buffer");
  const auto bytes = std::as_bytes(std::span(source.buffers[size_t(view.buffer)].data));
  if (view.byteLength == 0 || view.byteOffset > bytes.size() || view.byteLength > bytes.size() - view.byteOffset)
    reject(std::string(name) + " bufferView extent is empty or exceeds its buffer");
  return {view, bytes.subspan(view.byteOffset, view.byteLength)};
}

uint32_t littleEndian(std::span<const std::byte> data) {
  uint32_t value{};
  for (size_t byte = 0; byte < data.size(); ++byte)
    value |= std::to_integer<uint32_t>(data[byte]) << (8 * byte);
  return value;
}

size_t bucket(uint32_t level, uint32_t format) {
  return (format - 1) * (maxDraftLevel + 1) + level;
}

std::vector<Usage> usage(const Histogram& histogram, Budget& budget) {
  const auto count = std::ranges::count_if(histogram, [](uint32_t value) { return value != 0; });
  budget.payload(size_t(count), sizeof(Usage));
  std::vector<Usage> result;
  result.reserve(size_t(count));
  for (uint32_t format = 1; format <= 2; ++format)
    for (uint32_t level = 0; level <= maxDraftLevel; ++level)
      if (const auto value = histogram[bucket(level, format)]; value != 0)
        result.push_back({value, level, format});
  return result;
}

std::shared_ptr<const Micromap> parseMap(const tinygltf::Model& source, const Value& record,
    const GltfOpacityMicromapLimits& limits, Budget& budget) {
  const auto data = bufferView(source, integer(required(record, "data"), "data"), "data");
  const auto triangles = bufferView(source, integer(required(record, "triangles"), "triangles"), "triangles");
  if (data.record.byteStride != 0) reject("Packed opacity data cannot have a byteStride");
  const auto& counts = array(required(record, "usageCounts"), "usageCounts");
  const auto& levels = array(required(record, "usageLevels"), "usageLevels");
  const auto& formats = array(required(record, "usageFormats"), "usageFormats");
  if (counts.empty() || counts.size() != levels.size() || counts.size() != formats.size())
    reject("Usage arrays must have equal nonzero lengths");
  if (counts.size() > budget.records) reject("Usage arrays exceed the remaining triangle record budget");

  Histogram authored{};
  size_t triangleCount = 0;
  for (size_t index = 0; index < counts.size(); ++index) {
    const uint32_t count = integer(counts[index], "usageCounts entry");
    const uint32_t level = integer(levels[index], "usageLevels entry");
    const uint32_t format = integer(formats[index], "usageFormats entry");
    if (count == 0 || level > limits.maxSubdivisionLevel || (format != 1 && format != 2))
      reject("Usage entries require a positive count, a supported subdivision level, and format 1 or 2");
    Budget::take(budget.records, count, "Micromap triangle records");
    triangleCount += count;
    // The pinned profile permits duplicate buckets, whose effective counts
    // are the sums of the positive entries.
    authored[bucket(level, format)] += count;
  }

  const size_t stride = triangles.record.byteStride ? triangles.record.byteStride : sizeof(Triangle);
  if (stride < sizeof(Triangle) || stride % 4 != 0)
    reject("Triangle byteStride must be at least eight and divisible by four");
  if (triangles.bytes.size() < sizeof(Triangle) ||
      (triangles.bytes.size() - sizeof(Triangle)) / stride + 1 != triangleCount ||
      triangles.bytes.size() - (triangleCount - 1) * stride > stride)
    reject("Triangle bufferView does not contain the number of records declared by usageCounts");

  budget.payload(data.bytes.size(), 1);
  budget.payload(triangleCount, sizeof(Triangle) + sizeof(std::pair<size_t, size_t>));
  auto map = std::make_shared<Micromap>();
  map->triangles.reserve(triangleCount);
  std::vector<std::pair<size_t, size_t>> ranges;
  ranges.reserve(triangleCount);
  Histogram observed{};
  for (size_t index = 0; index < triangleCount; ++index) {
    const auto bytes = triangles.bytes.subspan(index * stride, sizeof(Triangle));
    const uint32_t offset = littleEndian(bytes.first(4));
    const uint32_t level = littleEndian(bytes.subspan(4, 2));
    const uint32_t format = littleEndian(bytes.subspan(6, 2));
    if (level > limits.maxSubdivisionLevel || (format != 1 && format != 2))
      reject("Triangle record " + std::to_string(index) + " has an unsupported subdivision level or format");
    const size_t packedSize = ((size_t{1} << (2 * level)) * format + 7) / 8;
    if (offset > data.bytes.size() || packedSize > data.bytes.size() - offset)
      reject("Triangle record " + std::to_string(index) + " exceeds the packed opacity data");
    ranges.emplace_back(offset, size_t(offset) + packedSize);
    map->triangles.push_back({offset, static_cast<uint16_t>(level), static_cast<uint16_t>(format)});
    ++observed[bucket(level, format)];
  }
  if (observed != authored) reject("Usage histogram differs from the triangle records");
  std::ranges::sort(ranges);
  for (size_t index = 1; index < ranges.size(); ++index)
    if (ranges[index].first < ranges[index - 1].second) reject("Packed opacity data ranges overlap");

  map->data.assign(data.bytes.begin(), data.bytes.end());
  map->usage = usage(observed, budget);
  return map;
}

const tinygltf::Accessor& accessor(const tinygltf::Model& source, size_t index, const char* name) {
  if (index >= source.accessors.size()) reject(std::string(name) + " accessor is out of range");
  return source.accessors[index];
}

struct Encoding { size_t width; bool signedValue; };

Encoding indexEncoding(int component) {
  switch (component) {
    case TINYGLTF_COMPONENT_TYPE_BYTE: return {1, true};
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: return {1, false};
    case TINYGLTF_COMPONENT_TYPE_SHORT: return {2, true};
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return {2, false};
    case TINYGLTF_COMPONENT_TYPE_INT: return {4, true};
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: return {4, false};
    default: reject("micromapIndices requires an 8-, 16-, or 32-bit integer component type");
  }
}

size_t geometryTriangleCount(const tinygltf::Model& source, const tinygltf::Primitive& primitive) {
  // TinyGLTF's default-constructed primitive uses -1 for unspecified mode.
  if (primitive.mode != TINYGLTF_MODE_TRIANGLES && primitive.mode != -1)
    reject("Only TRIANGLES primitives can use opacity micromaps");
  const auto position = primitive.attributes.find("POSITION");
  if (position == primitive.attributes.end() || position->second < 0)
    reject("Triangle primitive has no POSITION accessor");
  const auto& positions = accessor(source, size_t(position->second), "POSITION");
  if (positions.type != TINYGLTF_TYPE_VEC3) reject("POSITION accessor must be VEC3");
  size_t vertices = positions.count;
  if (primitive.indices < -1) reject("Geometry indices accessor is out of range");
  if (primitive.indices >= 0) {
    const auto& indices = accessor(source, size_t(primitive.indices), "Geometry indices");
    if (indices.type != TINYGLTF_TYPE_SCALAR) reject("Geometry indices accessor must be SCALAR");
    vertices = indices.count;
  }
  if (vertices == 0 || vertices % 3 != 0) reject("Primitive does not contain complete triangles");
  return vertices / 3;
}

uint32_t normalizedIndex(std::span<const std::byte> bytes, Encoding encoding) {
  const uint32_t value = littleEndian(bytes);
  const auto bits = encoding.width * 8;
  const uint32_t maximum = UINT32_MAX >> (32 - bits);
  if (value >= maximum - 3) return UINT32_MAX - (maximum - value);
  if (encoding.signedValue && (value & (uint32_t{1} << (bits - 1))) != 0)
    reject("micromapIndices contains a negative value outside -1 through -4");
  return value;
}

std::shared_ptr<const Attachment> parseAttachment(const tinygltf::Model& source,
    const tinygltf::Primitive& primitive, const Value& record,
    std::span<const std::shared_ptr<const Micromap>> maps, Budget& budget) {
  const uint32_t mapIndex = integer(required(record, "micromap"), "micromap");
  if (mapIndex >= maps.size() || !maps[mapIndex]) reject("micromap references an unavailable root entry");
  const auto* base = member(record, "micromapBaseTriangle");
  const uint32_t baseTriangle = base ? integer(*base, "micromapBaseTriangle") : 0;
  const size_t triangleCount = geometryTriangleCount(source, primitive);
  Budget::take(budget.geometry, triangleCount, "Micromap geometry lookup count");
  budget.payload(triangleCount, sizeof(uint32_t));

  std::span<const std::byte> lookups;
  Encoding encoding{};
  size_t stride{};
  if (const auto* index = member(record, "micromapIndices")) {
    const auto& indices = accessor(source, integer(*index, "micromapIndices"), "micromapIndices");
    if (indices.type != TINYGLTF_TYPE_SCALAR || indices.normalized || indices.count != triangleCount)
      reject("micromapIndices must contain one non-normalized SCALAR per geometry triangle");
    if (indices.sparse.isSparse || indices.bufferView < 0)
      reject("micromapIndices requires a resident, non-sparse bufferView");
    encoding = indexEncoding(indices.componentType);
    const auto view = bufferView(source, size_t(indices.bufferView), "micromapIndices");
    stride = view.record.byteStride ? view.record.byteStride : encoding.width;
    if (stride < encoding.width || stride % encoding.width != 0 ||
        indices.byteOffset % encoding.width != 0 || view.record.byteOffset % encoding.width != 0)
      reject("micromapIndices has a misaligned byteOffset or byteStride");
    if (indices.byteOffset > view.bytes.size()) reject("micromapIndices byteOffset exceeds its bufferView");
    lookups = view.bytes.subspan(indices.byteOffset);
    if (lookups.size() < encoding.width || triangleCount - 1 > (lookups.size() - encoding.width) / stride)
      reject("micromapIndices data exceeds its bufferView");
  }

  auto attachment = std::make_shared<Attachment>();
  attachment->map = maps[mapIndex];
  attachment->indices.reserve(triangleCount);
  Histogram observed{};
  for (size_t index = 0; index < triangleCount; ++index) {
    const uint32_t lookup = lookups.empty() ? static_cast<uint32_t>(index) :
        normalizedIndex(lookups.subspan(index * stride, encoding.width), encoding);
    if (lookup >= firstSpecialIndex) {
      attachment->indices.push_back(lookup);
      continue;
    }
    const uint64_t absolute = uint64_t(lookup) + baseTriangle;
    if (absolute >= attachment->map->triangles.size())
      reject("Micromap lookup " + std::to_string(index) + " plus baseTriangle exceeds the triangle table");
    attachment->indices.push_back(static_cast<uint32_t>(absolute));
    const auto& triangle = attachment->map->triangles[size_t(absolute)];
    ++observed[bucket(triangle.subdivisionLevel, triangle.format)];
  }
  attachment->usage = usage(observed, budget);
  return attachment;
}

void validateLimits(const GltfOpacityMicromapLimits& limits) {
  if (!limits.maxMaps || !limits.maxPrimitiveSlots || !limits.maxTriangleRecords ||
      !limits.maxGeometryTriangles || !limits.maxPayloadBytes ||
      limits.maxTriangleRecords >= firstSpecialIndex || limits.maxGeometryTriangles >= firstSpecialIndex ||
      limits.maxSubdivisionLevel > maxDraftLevel)
    throw std::invalid_argument("Opacity micromap limits are outside the supported range");
}

const Json* jsonField(const Json& parent, std::string_view name) {
  if (!parent.is_object()) return nullptr;
  const auto found = parent.find(name);
  return found != parent.end() ? &*found : nullptr;
}

const Json* jsonExtension(const Json& owner) {
  const auto* extensions = jsonField(owner, "extensions");
  return extensions ? jsonField(*extensions, kGltfOpacityMicromapExtension) : nullptr;
}

bool isExactJsonInteger(const Json& value) {
  if (value.is_number_unsigned()) return value.get<uint64_t>() <= INT32_MAX;
  if (value.is_number_integer()) {
    const auto integer = value.get<int64_t>();
    return integer >= INT32_MIN && integer <= INT32_MAX;
  }
  return false;
}

void requireExactJsonInteger(const Json& record, const char* field, bool optional = false) {
  const auto* value = jsonField(record, field);
  if (!value && optional) return;
  if (!value || !isExactJsonInteger(*value))
    reject(std::string(field) + " must be an integer representable before TinyGLTF extension conversion");
}

void checkRawMap(const Json& record, size_t& remainingEntries) {
  if (!record.is_object()) reject("Root micromap array entry must be an object");
  for (const auto* field : {"data", "triangles"}) requireExactJsonInteger(record, field);
  size_t entryCount = 0;
  for (const auto* field : {"usageCounts", "usageLevels", "usageFormats"}) {
    const auto* entries = jsonField(record, field);
    if (!entries || !entries->is_array() || entries->empty())
      reject(std::string(field) + " must be a nonempty array of exact integers");
    if (entries->size() > remainingEntries) reject("Raw micromap usage entries exceed the aggregate budget");
    entryCount = std::max(entryCount, entries->size());
    if (!std::ranges::all_of(*entries, isExactJsonInteger))
      reject(std::string(field) + " must contain exact integers representable before TinyGLTF extension conversion");
  }
  Budget::take(remainingEntries, entryCount, "Raw micromap usage entries");
}

} // namespace

std::shared_ptr<const GltfOpacityMicromapAttachment> GltfOpacityMicromapAssets::find(
    size_t mesh, size_t primitive) const noexcept {
  if (mesh >= primitives.size() || primitive >= primitives[mesh].size()) return {};
  return primitives[mesh][primitive];
}

GltfOpacityMicromapAssets ParseGltfOpacityMicromaps(
    const tinygltf::Model& source, const GltfOpacityMicromapLimits& limits) {
  validateLimits(limits);
  GltfOpacityMicromapAssets result;
  const auto root = source.extensions.find(kGltfOpacityMicromapExtension.data());
  const bool declared = std::ranges::find(source.extensionsUsed, kGltfOpacityMicromapExtension) != source.extensionsUsed.end();
  if (root == source.extensions.end() && !declared) return result;
  const std::string extension(kGltfOpacityMicromapExtension);
  const auto report = [&](std::string context, std::string message) {
    if (result.diagnostics.size() < limits.maxDiagnostics)
      result.diagnostics.push_back({std::move(context), std::move(message)});
    else
      ++result.diagnosticsOmitted;
  };
  const std::string rootContext = "extensions." + extension;
  if (!declared || root == source.extensions.end()) {
    report(rootContext, !declared ? "Extension must be declared in extensionsUsed" : "Root micromaps table is missing");
    return result;
  }
  Budget budget{limits.maxTriangleRecords, limits.maxGeometryTriangles, limits.maxPayloadBytes};
  std::vector<std::shared_ptr<const Micromap>> maps;
  try {
    const auto& records = array(required(root->second, "micromaps"), "micromaps");
    if (records.empty() || records.size() > limits.maxMaps) reject("Root micromaps count is empty or exceeds its limit");
    budget.payload(records.size(), sizeof(maps[0]));
    maps.resize(records.size());
    for (size_t index = 0; index < records.size(); ++index) {
      try {
        maps[index] = parseMap(source, records[index], limits, budget);
      } catch (const InvalidMicromap& error) {
        report(rootContext + ".micromaps[" + std::to_string(index) + "]", error.what());
      }
    }
    if (source.meshes.size() > limits.maxPrimitiveSlots) reject("Source mesh count exceeds the micromap primitive slot limit");
    budget.payload(source.meshes.size(), sizeof(result.primitives[0]));
    result.primitives.resize(source.meshes.size());
  } catch (const InvalidMicromap& error) {
    report(rootContext, error.what());
    return result;
  }

  size_t slots = limits.maxPrimitiveSlots;
  for (size_t mesh = 0; mesh < source.meshes.size(); ++mesh) {
    const auto& primitives = source.meshes[mesh].primitives;
    if (primitives.size() > slots) {
      report(rootContext, "Source primitive count exceeds the micromap primitive slot limit");
      break;
    }
    slots -= primitives.size();
    auto& output = result.primitives[mesh];
    for (size_t primitive = 0; primitive < primitives.size(); ++primitive) {
      const auto& sourcePrimitive = primitives[primitive];
      const auto found = sourcePrimitive.extensions.find(extension);
      if (found == sourcePrimitive.extensions.end()) continue;
      try {
        if (output.empty()) {
          budget.payload(primitives.size(), sizeof(output[0]));
          output.resize(primitives.size());
        }
        output[primitive] = parseAttachment(source, sourcePrimitive, found->second, maps, budget);
      } catch (const InvalidMicromap& error) {
        report("meshes[" + std::to_string(mesh) + "].primitives[" + std::to_string(primitive) +
            "].extensions." + extension, error.what());
      }
    }
  }
  return result;
}

std::optional<GltfOpacityMicromapDiagnostic> ValidateGltfOpacityMicromapSourceJson(
    const Json& source, const GltfOpacityMicromapLimits& limits) {
  validateLimits(limits);
  const auto* root = jsonExtension(source);
  if (!root) return std::nullopt;
  const std::string extension(kGltfOpacityMicromapExtension);
  const std::string rootContext = "extensions." + extension;
  std::string context = rootContext;
  try {
    const auto* maps = jsonField(*root, "micromaps");
    if (!maps || !maps->is_array() || maps->empty() || maps->size() > limits.maxMaps)
      reject("Root micromaps must be a nonempty array within its count limit");
    size_t entries = limits.maxTriangleRecords;
    for (size_t index = 0; index < maps->size(); ++index) {
      context = rootContext + ".micromaps[" + std::to_string(index) + "]";
      checkRawMap((*maps)[index], entries);
    }
    context = rootContext;
    if (const auto* meshes = jsonField(source, "meshes"); meshes && meshes->is_array()) {
      if (meshes->size() > limits.maxPrimitiveSlots) reject("Source mesh count exceeds the micromap primitive slot limit");
      size_t slots = limits.maxPrimitiveSlots;
      for (size_t mesh = 0; mesh < meshes->size(); ++mesh) {
        const auto* primitives = jsonField((*meshes)[mesh], "primitives");
        if (!primitives || !primitives->is_array()) continue;
        Budget::take(slots, primitives->size(), "Raw micromap primitive slots");
        for (size_t primitive = 0; primitive < primitives->size(); ++primitive) {
          const auto* link = jsonExtension((*primitives)[primitive]);
          if (!link) continue;
          context = "meshes[" + std::to_string(mesh) + "].primitives[" + std::to_string(primitive) +
              "].extensions." + extension;
          requireExactJsonInteger(*link, "micromap");
          for (const auto* field : {"micromapIndices", "micromapBaseTriangle"})
            requireExactJsonInteger(*link, field, true);
        }
      }
    }
  } catch (const InvalidMicromap& error) {
    return GltfOpacityMicromapDiagnostic{std::move(context), error.what()};
  }
  return std::nullopt;
}

} // namespace container::geometry::gltf
