#include <Container/geometry/GltfCompression.h>
#include <Container/utility/ModelImportCancellation.h>
#if defined(CONTAINER_HAS_DRACO)
#include <Container/geometry/GltfDracoWorker.h>
#endif
#if defined(CONTAINER_HAS_MESHOPT)
#include <meshoptimizer.h>
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace container::geometry::gltf {
namespace {
using Json = nlohmann::json;
using container::util::checkModelImportCancellation;
using Bytes = std::vector<unsigned char>;
constexpr const char* kDraco = "KHR_draco_mesh_compression";
constexpr const char* kExt = "EXT_meshopt_compression";
constexpr const char* kKhr = "KHR_meshopt_compression";

[[noreturn]] void fail(const std::string& message) {
  throw std::runtime_error("glTF compression: " + message);
}
uint64_t integer(const Json& value, const char* name) {
  if ((!value.is_number_unsigned() && !value.is_number_integer()) ||
      (value.is_number_integer() && !value.is_number_unsigned() && value.get<int64_t>() < 0))
    fail(std::string(name) + " must be a nonnegative integer");
  const auto result = value.get<uint64_t>();
  if (result > uint64_t(std::numeric_limits<size_t>::max()) || result > 9007199254740991ull)
    fail(std::string(name) + " exceeds the exact supported integer range");
  return result;
}
uint64_t required(const Json& object, const char* name) {
  if (!object.is_object() || !object.contains(name)) fail(std::string("missing ") + name);
  return integer(object.at(name), name);
}
uint64_t optional(const Json& object, const char* name, uint64_t fallback = 0) {
  return object.contains(name) ? integer(object.at(name),name) : fallback;
}
std::string string(const Json& object, const char* name, const char* fallback = nullptr) {
  if (!object.contains(name)) {
    if (fallback) return fallback;
    fail(std::string("missing ") + name);
  }
  if (!object.at(name).is_string()) fail(std::string(name) + " must be a string");
  return object.at(name).get<std::string>();
}
void range(uint64_t offset, uint64_t length, uint64_t size, const char* label) {
  if (offset > size || length > size-offset) fail(std::string(label) + " range exceeds its buffer");
}
uint64_t product(uint64_t count, uint64_t width) {
  if (width && count > std::numeric_limits<uint64_t>::max()/width) fail("decoded size overflows");
  return count*width;
}
void charge(uint64_t& total, uint64_t bytes, const CompressionLimits& limits) {
  if (total > limits.maxDecodedBytes || bytes > limits.maxDecodedBytes-total)
    fail("decoded geometry exceeds the output-memory budget");
  total += bytes;
}
bool listed(const Json& model, const char* list, const char* name) {
  if (!model.contains(list)) return false;
  if (!model.at(list).is_array()) fail(std::string(list) + " must be an array");
  return std::ranges::find(model.at(list), Json(name)) != model.at(list).end();
}
void declaration(const Json& model, const char* name, bool enabled) {
  if (!listed(model,"extensionsUsed",name)) fail(std::string(name) + " is missing from extensionsUsed");
  if (!enabled && listed(model,"extensionsRequired",name))
    fail(std::string(name) + " is required but its bounded decoder is disabled or unavailable");
}
const Json* extension(const Json& object, const char* name) {
  if (!object.contains("extensions")) return nullptr;
  const auto& extensions = object.at("extensions");
  if (!extensions.is_object()) fail("extensions must be an object");
  return extensions.contains(name) ? &extensions.at(name) : nullptr;
}
const Json& at(const Json& array, uint64_t index, const char* label) {
  if (!array.is_array() || index >= array.size()) fail(std::string(label) + " index is out of range");
  return array.at(size_t(index));
}
const Json& table(const Json& model, const char* name) {
  static const Json empty = Json::array();
  if (!model.contains(name)) return empty;
  if (!model.at(name).is_array()) fail(std::string(name) + " must be an array");
  return model.at(name);
}
struct Meshopt {
  const char* name{};
  uint64_t buffer{}, offset{}, length{}, count{}, stride{}, decoded{};
  std::string mode, filter;
};
Meshopt meshopt(const Json& view, const Json& buffers) {
  const auto* ext = extension(view,kExt);
  const auto* khr = extension(view,kKhr);
  if (ext && khr) fail("a buffer view cannot use both KHR and EXT meshopt compression");
  Meshopt result;
  result.name = khr ? kKhr : ext ? kExt : nullptr;
  if (!result.name) return result;
  const auto& value = khr ? *khr : *ext;
  result.buffer = required(value,"buffer");
  result.offset = optional(value,"byteOffset");
  result.length = required(value,"byteLength");
  result.count = required(value,"count");
  result.stride = required(value,"byteStride");
  result.mode = string(value,"mode");
  result.filter = string(value,"filter","NONE");
  if (!result.length || !result.count) fail("meshopt lengths/count must be positive");
  if (result.mode == "ATTRIBUTES") {
    if (result.stride < 4 || result.stride > 256 || result.stride%4)
      fail("meshopt attribute stride must be a multiple of four in [4,256]");
  } else if (result.mode == "TRIANGLES" || result.mode == "INDICES") {
    if (result.stride != 2 && result.stride != 4) fail("meshopt index stride must be two or four");
    if (result.mode == "TRIANGLES" && result.count%3) fail("meshopt triangle count must be divisible by three");
    if (result.filter != "NONE") fail("meshopt indices cannot use attribute filters");
  } else fail("unknown meshopt mode");
  if (result.filter == "OCTAHEDRAL" || result.filter == "COLOR") {
    if (result.stride != 4 && result.stride != 8) fail("meshopt filter stride must be four or eight");
    if (result.filter == "COLOR" && !khr) fail("COLOR requires KHR_meshopt_compression");
  } else if (result.filter == "QUATERNION") {
    if (result.stride != 8) fail("meshopt quaternion stride must be eight");
  } else if (result.filter == "EXPONENTIAL") {
    if (result.stride%4) fail("meshopt exponential stride must be divisible by four");
  } else if (result.filter != "NONE") fail("unknown meshopt filter");
  result.decoded = product(result.count,result.stride);
  if (required(view,"byteLength") != result.decoded) fail("meshopt output size differs from parent buffer view");
  if (ext && view.contains("byteStride") && required(view,"byteStride") != result.stride)
    fail("EXT meshopt stride differs from parent buffer view");
  range(result.offset,result.length,required(at(buffers,result.buffer,"compressed buffer"),"byteLength"),"meshopt encoded");
  return result;
}
uint64_t width(uint64_t component) {
  switch(component) {
    case TINYGLTF_COMPONENT_TYPE_BYTE: case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: return 1;
    case TINYGLTF_COMPONENT_TYPE_SHORT: case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return 2;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: case TINYGLTF_COMPONENT_TYPE_FLOAT: return 4;
    case TINYGLTF_COMPONENT_TYPE_DOUBLE: return 8;
    default: fail("unsupported Draco accessor component type");
  }
}
uint64_t components(const Json& accessor) {
  const auto type = string(accessor,"type");
  if (type == "SCALAR") return 1;
  if (type == "VEC2") return 2;
  if (type == "VEC3") return 3;
  if (type == "VEC4") return 4;
  fail("Draco vertex attributes must be scalar or vectors");
}
uint64_t dracoBudget(const Json& primitive, const Json& accessors) {
  const auto* ext = extension(primitive,kDraco);
  if (!ext || !ext->is_object() || !ext->contains("attributes") ||
      !ext->at("attributes").is_object() || ext->at("attributes").empty()) fail("invalid Draco attribute map");
  const auto& attrs = primitive.at("attributes");
  const auto points = required(at(accessors,required(attrs,"POSITION"),"POSITION accessor"),"count");
  if (!points || points > UINT32_MAX) fail("Draco point count is outside the supported range");
  uint64_t count = points;
  uint64_t indexWidth = 4;
  if (primitive.contains("indices")) {
    const auto& accessor = at(accessors,required(primitive,"indices"),"index accessor");
    if (string(accessor,"type") != "SCALAR" || accessor.value("normalized",false)) fail("invalid Draco index accessor");
    const auto component = required(accessor,"componentType");
    if (component != 5121 && component != 5123 && component != 5125) fail("Draco indices must be unsigned integers");
    indexWidth = width(component);
    count = required(accessor,"count");
  }
  if (!count || count%3) fail("Draco triangle index count must be divisible by three");
  // The worker may return any raw scalar up to64 bits. Reserve that worst case
  // before decoding; conversion to the declared glTF width is range checked.
  const auto faceBytes = product(count,4);
  if (faceBytes > UINT64_MAX-48) fail("Draco response size overflows");
  uint64_t wire = 48 + faceBytes;
  std::map<uint64_t,uint64_t> ids;
  for (const auto& [semantic,id] : ext->at("attributes").items()) {
    const auto unique = integer(id,"Draco unique attribute ID");
    if (unique > UINT32_MAX) fail("excessive Draco unique attribute ID");
    if (!attrs.contains(semantic)) fail("Draco attribute is absent from primitive attributes");
    const auto& accessor = at(accessors,integer(attrs.at(semantic),"attribute accessor"),"attribute accessor");
    if (required(accessor,"count") != points) fail("Draco attribute counts disagree");
    (void)width(required(accessor,"componentType"));
    const auto shape = components(accessor);
    const auto [alias,inserted] = ids.emplace(unique,shape);
    if (!inserted && alias->second != shape) fail("aliased Draco attribute ID has incompatible component counts");
    const auto bytes = product(product(points,shape),8);
    if (wire > UINT64_MAX-24 || bytes > UINT64_MAX-wire-24) fail("Draco response size overflows");
    wire += 24 + bytes;
  }
  if (optional(primitive,"mode",4) != 4) fail("Draco import currently requires triangle-list primitives");
  const auto scratch = product(wire,3);
  const auto indices = product(count,indexWidth);
  if (indices > UINT64_MAX-scratch) fail("Draco decoded size overflows");
  return scratch + indices;
}
Json jsonValue(const tinygltf::Value& value) {
  if (value.IsObject()) {
    Json result = Json::object();
    for (const auto& [name,child] : value.Get<tinygltf::Value::Object>()) result[name] = jsonValue(child);
    return result;
  }
  if (value.IsArray()) {
    Json result = Json::array();
    for (const auto& child : value.Get<tinygltf::Value::Array>()) result.push_back(jsonValue(child));
    return result;
  }
  if (value.IsInt()) return value.Get<int>();
  if (value.IsNumber()) {
    const auto number = value.GetNumberAsDouble();
    // TinyGLTF stores large JSON integers as doubles. Only exact values are
    // usable; the pre-parse planner already validates the original JSON type.
    if (std::isfinite(number) && number > INT32_MAX && number <= 9007199254740991.0 && std::floor(number) == number)
      return uint64_t(number);
    return number;
  }
  if (value.IsBool()) return value.Get<bool>();
  if (value.IsString()) return value.Get<std::string>();
  return nullptr;
}
Json extensions(const tinygltf::ExtensionMap& values) {
  Json result = Json::object();
  for (const auto* name : {kExt,kKhr,kDraco})
    if (const auto found=values.find(name); found!=values.end()) result[name] = jsonValue(found->second);
  return result;
}
Json metadata(const tinygltf::Model& model, const std::stop_token& stop) {
  Json result{{"extensionsUsed",model.extensionsUsed},{"extensionsRequired",model.extensionsRequired},
      {"buffers",Json::array()},{"bufferViews",Json::array()},{"accessors",Json::array()},{"meshes",Json::array()}};
  for (const auto& buffer : model.buffers) {
    checkModelImportCancellation(stop);
    Json item{{"byteLength",buffer.data.size()},{"extensions",extensions(buffer.extensions)}};
    if (!buffer.uri.empty()) item["uri"] = "resident:";
    // Resident byte arrays supplied directly by CPU callers have no URI.
    // A synthetic marker distinguishes them from unresolved placeholders.
    else if (!buffer.data.empty()) item["uri"] = "resident:";
    result["buffers"].push_back(std::move(item));
  }
  for (const auto& view : model.bufferViews) {
    checkModelImportCancellation(stop);
    Json item{{"buffer",view.buffer},{"byteOffset",view.byteOffset},{"byteLength",view.byteLength},
        {"extensions",extensions(view.extensions)}};
    if (view.byteStride) item["byteStride"] = view.byteStride;
    result["bufferViews"].push_back(std::move(item));
  }
  for (const auto& accessor : model.accessors) {
    checkModelImportCancellation(stop);
    const char* type = accessor.type == TINYGLTF_TYPE_SCALAR ? "SCALAR" : accessor.type == TINYGLTF_TYPE_VEC2 ? "VEC2" :
        accessor.type == TINYGLTF_TYPE_VEC3 ? "VEC3" : accessor.type == TINYGLTF_TYPE_VEC4 ? "VEC4" : "MATRIX";
    result["accessors"].push_back(Json{{"count",accessor.count},{"componentType",accessor.componentType},
        {"type",type},{"normalized",accessor.normalized}});
  }
  for (const auto& mesh : model.meshes) {
    Json primitives = Json::array();
    for (const auto& primitive : mesh.primitives) {
      checkModelImportCancellation(stop);
      Json item{{"attributes",primitive.attributes},{"mode",primitive.mode < 0 ? 4 : primitive.mode},
          {"extensions",extensions(primitive.extensions)}};
      if (primitive.indices >= 0) item["indices"] = primitive.indices;
      primitives.push_back(std::move(item));
    }
    result["meshes"].push_back(Json{{"primitives",std::move(primitives)}});
  }
  return result;
}
void remove(tinygltf::ExtensionMap& map, const char* name, std::string& cached) {
  if (map.erase(name)) cached.clear();
}
int storage(tinygltf::Model& model, Bytes bytes) {
  if (model.buffers.size() >= size_t(INT32_MAX)) fail("too many decoded buffers");
  tinygltf::Buffer buffer;
  buffer.data = std::move(bytes);
  model.buffers.push_back(std::move(buffer));
  return int(model.buffers.size()-1);
}
Bytes decodeMeshopt(const tinygltf::Model& model, const Meshopt& request) {
#if defined(CONTAINER_HAS_MESHOPT)
  if (request.buffer >= model.buffers.size()) fail("meshopt encoded buffer is missing");
  const auto& source = model.buffers[size_t(request.buffer)].data;
  range(request.offset,request.length,source.size(),"meshopt encoded");
  const auto* bytes = source.data()+size_t(request.offset);
  const int version = request.mode == "ATTRIBUTES" ? meshopt_decodeVertexVersion(bytes,size_t(request.length)) :
      meshopt_decodeIndexVersion(bytes,size_t(request.length));
  if (version < 0 || version > 1 || (request.name == kExt && request.mode == "ATTRIBUTES" && version != 0))
    fail("meshopt bitstream version is invalid for this extension");
  Bytes output(size_t(request.decoded));
  int status;
  if (request.mode == "ATTRIBUTES")
    status = meshopt_decodeVertexBuffer(output.data(),size_t(request.count),size_t(request.stride),bytes,size_t(request.length));
  else if (request.mode == "TRIANGLES")
    status = meshopt_decodeIndexBuffer(output.data(),size_t(request.count),size_t(request.stride),bytes,size_t(request.length));
  else status = meshopt_decodeIndexSequence(output.data(),size_t(request.count),size_t(request.stride),bytes,size_t(request.length));
  if (status) fail("malformed meshopt bitstream (decoder status " + std::to_string(status) + ')');
  if (request.filter == "OCTAHEDRAL") meshopt_decodeFilterOct(output.data(),size_t(request.count),size_t(request.stride));
  else if (request.filter == "QUATERNION") meshopt_decodeFilterQuat(output.data(),size_t(request.count),size_t(request.stride));
  else if (request.filter == "EXPONENTIAL") meshopt_decodeFilterExp(output.data(),size_t(request.count),size_t(request.stride));
  else if (request.filter == "COLOR") meshopt_decodeFilterColor(output.data(),size_t(request.count),size_t(request.stride));
  return output;
#else
  (void)model; (void)request;
  fail("meshopt decoder is disabled");
#endif
}

#if defined(CONTAINER_HAS_DRACO)
template<class T> T little(const std::byte* bytes) {
  std::array<std::byte,sizeof(T)> raw;
  std::memcpy(raw.data(),bytes,sizeof(T));
  if constexpr (std::endian::native == std::endian::big) std::reverse(raw.begin(),raw.end());
  return std::bit_cast<T>(raw);
}
template<class T> void append(Bytes& bytes,T value) {
  auto raw = std::bit_cast<std::array<unsigned char,sizeof(T)>>(value);
  if constexpr (std::endian::native == std::endian::big) std::reverse(raw.begin(),raw.end());
  bytes.insert(bytes.end(),raw.begin(),raw.end());
}
Bytes convert(const DracoDecodedAttribute& attribute,const tinygltf::Accessor& accessor,uint32_t points,
              const std::stop_token& stop) {
  const auto sourceWidth = DracoScalarByteWidth(attribute.scalarType);
  const auto count = product(points,attribute.componentCount);
  if (attribute.values.size() != product(count,sourceWidth)) fail("Draco attribute payload size disagrees");
  Bytes output;
  output.reserve(size_t(product(count,width(uint64_t(accessor.componentType)))));
  for (uint64_t i=0;i<count;++i) {
    if (i % 1024 == 0) checkModelImportCancellation(stop);
    const auto* bytes = attribute.values.data()+size_t(i*sourceWidth);
    const auto encode = [&]<class T>(T value) {
      const auto integer = [&]<class D>() {
        if constexpr (!std::is_integral_v<T>) fail("Draco float attribute cannot supply integer accessor storage");
        else {
          if (!std::in_range<D>(value)) fail("Draco integer attribute is outside declared component range");
          append(output,static_cast<D>(value));
        }
      };
      const auto floating = [&]<class D>() {
        const auto converted = static_cast<D>(value);
        if (!std::isfinite(converted)) fail("Draco attribute is outside finite declared floating-point range");
        append(output,converted);
      };
      switch(accessor.componentType) {
        case 5120: integer.template operator()<int8_t>(); break;
        case 5121: integer.template operator()<uint8_t>(); break;
        case 5122: integer.template operator()<int16_t>(); break;
        case 5123: integer.template operator()<uint16_t>(); break;
        case 5125: integer.template operator()<uint32_t>(); break;
        case 5126: floating.template operator()<float>(); break;
        case TINYGLTF_COMPONENT_TYPE_DOUBLE: floating.template operator()<double>(); break;
        default: fail("unsupported Draco accessor component type");
      }
    };
    switch(attribute.scalarType) {
      case DracoScalarType::Int8: encode(little<int8_t>(bytes)); break;
      case DracoScalarType::UInt8: encode(little<uint8_t>(bytes)); break;
      case DracoScalarType::Int16: encode(little<int16_t>(bytes)); break;
      case DracoScalarType::UInt16: encode(little<uint16_t>(bytes)); break;
      case DracoScalarType::Int32: encode(little<int32_t>(bytes)); break;
      case DracoScalarType::UInt32: encode(little<uint32_t>(bytes)); break;
      case DracoScalarType::Int64: encode(little<int64_t>(bytes)); break;
      case DracoScalarType::UInt64: encode(little<uint64_t>(bytes)); break;
      case DracoScalarType::Float32: encode(little<float>(bytes)); break;
      case DracoScalarType::Float64: encode(little<double>(bytes)); break;
      default: fail("Draco boolean attributes cannot supply glTF numeric accessors");
    }
  }
  return output;
}
void decodeDraco(tinygltf::Model& model,tinygltf::Primitive& primitive,const Json& description,const Json& accessorDescriptions,
                 const std::vector<tinygltf::Accessor>& originals,const CompressionLimits& limits,
                 const std::stop_token& stop) {
  const auto& dracoExtension = *extension(description,kDraco);
  const auto viewIndex = required(dracoExtension,"bufferView");
  if (viewIndex >= model.bufferViews.size()) fail("Draco buffer view is missing");
  const auto& view = model.bufferViews[size_t(viewIndex)];
  if (view.buffer < 0 || size_t(view.buffer) >= model.buffers.size()) fail("Draco encoded buffer is missing");
  const auto& bytes = model.buffers[size_t(view.buffer)].data;
  range(view.byteOffset,view.byteLength,bytes.size(),"Draco encoded");
  const auto position = required(description.at("attributes"),"POSITION");
  const auto points = originals[size_t(position)].count;
  const auto indexCount = primitive.indices >= 0 ? originals[size_t(primitive.indices)].count : points;
  std::vector<DracoAttributeRequest> requests;
  for (const auto& [semantic,id] : dracoExtension.at("attributes").items()) {
    const auto unique=uint32_t(integer(id,"Draco attribute ID"));
    const auto shape=uint32_t(tinygltf::GetNumComponentsInType(originals[size_t(primitive.attributes.at(semantic))].type));
    const auto existing=std::ranges::find(requests,unique,&DracoAttributeRequest::uniqueId);
    if (existing==requests.end()) requests.push_back({unique,shape});
    else if (existing->componentCount!=shape) fail("aliased Draco attribute ID has incompatible component counts");
  }
  const auto charged = dracoBudget(description,accessorDescriptions);
  const auto indexBytes = product(indexCount,primitive.indices >= 0 ? width(uint64_t(originals[size_t(primitive.indices)].componentType)) : 4);
  DracoDecodeLimits workerLimits;
  workerLimits.maxInputBytes = view.byteLength;
  workerLimits.maxOutputBytes = (charged-indexBytes)/3;
  workerLimits.maxPoints = points;
  workerLimits.maxFaces = indexCount/3;
  workerLimits.processMemoryBytes = limits.dracoProcessMemoryBytes;
  workerLimits.timeoutMs = limits.dracoTimeoutMs;
  const auto decoded = DecodeDracoInWorker(
      {reinterpret_cast<const std::byte*>(bytes.data()+view.byteOffset),view.byteLength},requests,workerLimits,
      {limits.dracoWorkerExecutable,limits.dracoTemporaryRoot},stop);
  if (decoded.pointCount != points || decoded.faceIndices.size() != indexCount || decoded.attributes.size() != requests.size())
    fail("Draco decoded counts disagree with source accessors");
  // Append per-primitive accessor copies. Shared original metadata can describe
  // different Draco streams, and must not let one primitive overwrite another.
  const auto materialize = [&](tinygltf::Accessor accessor,Bytes data,int target) {
    tinygltf::BufferView output;
    if (target == TINYGLTF_TARGET_ARRAY_BUFFER) {
      const auto element = size_t(width(uint64_t(accessor.componentType))) * size_t(tinygltf::GetNumComponentsInType(accessor.type));
      const auto stride = (element+3)&~size_t(3);
      if (stride != element) {
        Bytes padded(size_t(product(accessor.count,stride)),0);
        for (size_t point=0;point<accessor.count;++point) {
          if (point % 1024 == 0) checkModelImportCancellation(stop);
          std::memcpy(padded.data()+point*stride,data.data()+point*element,element);
        }
        data = std::move(padded);
        output.byteStride = stride;
      }
    }
    output.buffer = storage(model,std::move(data));
    output.byteLength = model.buffers.back().data.size();
    output.target = target;
    model.bufferViews.push_back(output);
    accessor.bufferView = int(model.bufferViews.size()-1);
    accessor.byteOffset = 0;
    model.accessors.push_back(std::move(accessor));
    return int(model.accessors.size()-1);
  };
  for (const auto& [semantic,id] : dracoExtension.at("attributes").items()) {
    const auto unique = uint32_t(integer(id,"Draco attribute ID"));
    const auto found = std::ranges::find(decoded.attributes,unique,&DracoDecodedAttribute::uniqueId);
    if (found == decoded.attributes.end()) fail("Draco unique attribute ID is absent from decoded mesh");
    const auto& accessor = originals[size_t(primitive.attributes.at(semantic))];
    if (found->componentCount != uint32_t(tinygltf::GetNumComponentsInType(accessor.type))) fail("Draco attribute shape disagrees");
    primitive.attributes[semantic] = materialize(accessor,convert(*found,accessor,decoded.pointCount,stop),TINYGLTF_TARGET_ARRAY_BUFFER);
  }
  tinygltf::Accessor indices;
  if (primitive.indices >= 0) indices = originals[size_t(primitive.indices)];
  else { indices.type = TINYGLTF_TYPE_SCALAR; indices.componentType = 5125; indices.count = indexCount; }
  Bytes output;
  for (size_t i=0;i<decoded.faceIndices.size();++i) {
    if (i % 1024 == 0) checkModelImportCancellation(stop);
    const auto index = decoded.faceIndices[i];
    if (index >= points) fail("Draco face index exceeds point count");
    if (indices.componentType == 5121) {
      if (index > UINT8_MAX) fail("Draco index exceeds declared UBYTE range");
      append(output,uint8_t(index));
    } else if (indices.componentType == 5123) {
      if (index > UINT16_MAX) fail("Draco index exceeds declared USHORT range");
      append(output,uint16_t(index));
    } else append(output,index);
  }
  primitive.indices = materialize(indices,std::move(output),TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
}
#endif
} // namespace

bool HasMeshoptDecoder() noexcept {
#if defined(CONTAINER_HAS_MESHOPT)
  return true;
#else
  return false;
#endif
}
bool HasDracoDecoder() noexcept {
#if defined(CONTAINER_HAS_DRACO)
  return DracoWorkerPlatformSupported();
#else
  return false;
#endif
}
bool HasAvailableDracoDecoder(const std::filesystem::path& workerExecutable) noexcept {
#if defined(CONTAINER_HAS_DRACO)
  return HasDracoDecoder() && DracoWorkerExecutableAvailable(workerExecutable);
#else
  (void)workerExecutable;
  return false;
#endif
}
bool IsGeometryCompressionExtension(std::string_view name) noexcept {
  return name == kDraco || name == kExt || name == kKhr;
}

namespace {
CompressionSourcePlan inspectCompressedGltfSource(const Json& source, bool binary,
    const CompressionLimits& limits, const std::stop_token& stop,
    std::optional<bool> preflightDracoAvailability) {
  checkModelImportCancellation(stop);
  try {
    CompressionSourcePlan plan;
    const bool dracoRequired = listed(source,"extensionsRequired",kDraco);
    plan.dracoAvailable = (dracoRequired || listed(source,"extensionsUsed",kDraco)) &&
        (preflightDracoAvailability ? (*preflightDracoAvailability && HasDracoDecoder()) :
         HasAvailableDracoDecoder(limits.dracoWorkerExecutable));
    // Keep the required layout checks even when a direct caller lacks a worker.
    // File imports already check worker availability in the capability gate.
    const bool useDraco = dracoRequired ? HasDracoDecoder() : plan.dracoAvailable;
    for (const auto* name : {kExt,kKhr,kDraco})
      if (listed(source,"extensionsRequired",name))
        declaration(source,name,name == kDraco ? useDraco : HasMeshoptDecoder());
    const auto& buffers = table(source,"buffers");
    const auto& views = table(source,"bufferViews");
    std::vector<size_t> references(buffers.size()),compressed(buffers.size()),extReferences(buffers.size()),khrReferences(buffers.size());
    std::vector<bool> requiredReferences(buffers.size(),true);
    std::set<size_t> encodedSources;
    for (const auto& view : views) {
      checkModelImportCancellation(stop);
      const auto buffer = required(view,"buffer");
      const auto size = required(at(buffers,buffer,"buffer"),"byteLength");
      range(optional(view,"byteOffset"),required(view,"byteLength"),size,"parent buffer view");
      ++references[size_t(buffer)];
      const auto* ext = extension(view,kExt);
      const auto* khr = extension(view,kKhr);
      if (ext && khr) fail("a buffer view cannot use both meshopt extensions");
      if (!ext && !khr) continue;
      const auto* name = khr ? kKhr : kExt;
      declaration(source,name,HasMeshoptDecoder());
      const auto request = meshopt(view,buffers);
      encodedSources.insert(size_t(request.buffer));
      ++compressed[size_t(buffer)];
      ++(khr ? khrReferences[size_t(buffer)] : extReferences[size_t(buffer)]);
      requiredReferences[size_t(buffer)] = requiredReferences[size_t(buffer)] && listed(source,"extensionsRequired",name);
      if (HasMeshoptDecoder()) {
        charge(plan.decodedBytes,request.decoded,limits);
        ++plan.meshoptViews;
      }
    }
    for (size_t i=0;i<buffers.size();++i) {
      checkModelImportCancellation(stop);
      const auto& buffer = buffers[i];
      const auto* ext = extension(buffer,kExt);
      const auto* khr = extension(buffer,kKhr);
      if (ext && khr) fail("a buffer cannot use both meshopt fallback tags");
      if ((ext && khrReferences[i]) || (khr && extReferences[i])) fail("meshopt fallback tag differs from its referencing views");
      if (ext) declaration(source,kExt,HasMeshoptDecoder());
      if (khr) declaration(source,kKhr,HasMeshoptDecoder());
      for (const auto* fallback : {ext,khr})
        if (fallback && (!fallback->is_object() || !fallback->contains("fallback") ||
                         !fallback->at("fallback").is_boolean() || !fallback->at("fallback").get<bool>()))
          fail("meshopt buffer fallback tag must be true");
      const bool placeholder = references[i] &&
          (!buffer.contains("uri") || (buffer.at("uri").is_string() && buffer.at("uri").get<std::string>().empty())) && !(binary && i==0);
      if ((ext || khr || placeholder) && (!references[i] || references[i] != compressed[i] || encodedSources.contains(i)))
        fail("meshopt placeholder/fallback buffer has unsupported or noncompressed references");
      if (placeholder) {
        // A placeholder is valid only when the decoder is required; otherwise
        // an ordinary loader would have no usable fallback representation.
        if (!HasMeshoptDecoder() || !requiredReferences[i])
          fail("URI-less meshopt placeholder requires its compression extension");
        plan.placeholders.push_back({i,required(buffer,"byteLength")});
      }
    }
    for (const auto& mesh : table(source,"meshes"))
      for (const auto& primitive : table(mesh,"primitives")) {
        checkModelImportCancellation(stop);
        if (const auto* draco = extension(primitive,kDraco)) {
          declaration(source,kDraco,useDraco);
          if (!useDraco) continue;
          const auto view = required(*draco,"bufferView");
          (void)at(views,view,"Draco buffer view");
          charge(plan.decodedBytes,dracoBudget(primitive,table(source,"accessors")),limits);
          ++plan.dracoPrimitives;
        }
      }
    if (dracoRequired && !plan.dracoAvailable)
      fail(std::string(kDraco) + " is required but the bounded Draco worker executable is unavailable");
    return plan;
  } catch(const Json::exception& error) { fail(std::string("invalid compression metadata: ")+error.what()); }
}
} // namespace

CompressionSourcePlan InspectCompressedGltfSource(const Json& source, bool binary,
    const CompressionLimits& limits, const std::stop_token& stop) {
  return inspectCompressedGltfSource(source, binary, limits, stop, std::nullopt);
}

tinygltf::Model DecodeCompressedGltf(tinygltf::Model source,const CompressionLimits& limits,CompressionStatistics* statistics,
                                    std::span<const CompressionPlaceholder> placeholders, const std::stop_token& stop,
                                    std::optional<bool> preflightDracoAvailability) {
  checkModelImportCancellation(stop);
  const auto hasMap=[&stop](const tinygltf::ExtensionMap& map) {
    checkModelImportCancellation(stop);
    return map.contains(kExt) || map.contains(kKhr) || map.contains(kDraco);
  };
  bool compressed=!placeholders.empty() || std::ranges::any_of(source.extensionsUsed,IsGeometryCompressionExtension) ||
      std::ranges::any_of(source.extensionsRequired,IsGeometryCompressionExtension);
  if (!compressed) {
    compressed=std::ranges::any_of(source.buffers,[&](const auto& buffer){return hasMap(buffer.extensions);}) ||
        std::ranges::any_of(source.bufferViews,[&](const auto& view){return hasMap(view.extensions);});
    for (const auto& mesh:source.meshes)
      compressed=compressed || std::ranges::any_of(mesh.primitives,[&](const auto& primitive){return hasMap(primitive.extensions);});
  }
  if (!compressed) {
    if (statistics) *statistics={};
    return source;
  }
  auto description = metadata(source, stop);
  std::set<size_t> placeholderIndices;
  for (const auto& placeholder : placeholders) {
    checkModelImportCancellation(stop);
    if (placeholder.bufferIndex >= source.buffers.size() || !placeholderIndices.insert(placeholder.bufferIndex).second)
      fail("invalid or duplicate stand-in placeholder buffer");
    auto& buffer = description.at("buffers").at(placeholder.bufferIndex);
    buffer["byteLength"] = placeholder.byteLength;
    buffer.erase("uri");
  }
  const auto plan = inspectCompressedGltfSource(description, false, limits, stop, preflightDracoAvailability);
  if (plan.placeholders.size() != placeholders.size())
    fail("unresolved placeholder buffer was not supplied by source preflight");
  CompressionStatistics completed;
  completed.decodedBytes = plan.decodedBytes;
  const auto originalAccessors = source.accessors;
  for (size_t i=0;i<source.bufferViews.size();++i) {
    checkModelImportCancellation(stop);
    auto& view = source.bufferViews[i];
    const auto& item = description.at("bufferViews").at(i);
    if (!extension(item,kExt) && !extension(item,kKhr)) continue;
    if (HasMeshoptDecoder()) {
      const auto request = meshopt(item,description.at("buffers"));
      auto decoded = decodeMeshopt(source,request);
      view.buffer = storage(source,std::move(decoded));
      view.byteOffset = 0;
      ++completed.meshoptViews;
    } else ++completed.fallbackViews;
    remove(view.extensions,kExt,view.extensions_json_string);
    remove(view.extensions,kKhr,view.extensions_json_string);
  }
  const bool useDraco = plan.dracoPrimitives > 0;
  for (size_t mesh=0;mesh<source.meshes.size();++mesh)
    for (size_t p=0;p<source.meshes[mesh].primitives.size();++p) {
      checkModelImportCancellation(stop);
      auto& primitive = source.meshes[mesh].primitives[p];
      if (!primitive.extensions.contains(kDraco)) continue;
      if (useDraco) {
#if defined(CONTAINER_HAS_DRACO)
        decodeDraco(source,primitive,description.at("meshes").at(mesh).at("primitives").at(p),description.at("accessors"),originalAccessors,limits,stop);
        ++completed.dracoPrimitives;
#endif
      } else ++completed.fallbackPrimitives;
      remove(primitive.extensions,kDraco,primitive.extensions_json_string);
    }
  for (auto& buffer : source.buffers) {
    checkModelImportCancellation(stop);
    remove(buffer.extensions,kExt,buffer.extensions_json_string);
    remove(buffer.extensions,kKhr,buffer.extensions_json_string);
  }
  for (const auto index : placeholderIndices) {
    // No decoded view continues to reference synthetic stand-in bytes. The
    // portable exporter packs referenced storage and omits this empty buffer.
    source.buffers[index].data.clear();
    source.buffers[index].uri.clear();
  }
  std::erase_if(source.extensionsUsed,IsGeometryCompressionExtension);
  std::erase_if(source.extensionsRequired,IsGeometryCompressionExtension);
  checkModelImportCancellation(stop);
  if (statistics) *statistics = completed;
  return source;
}

} // namespace container::geometry::gltf
