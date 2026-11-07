#include <Container/geometry/GltfDracoProtocol.h>

#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace container::geometry::gltf {

uint32_t DracoScalarByteWidth(DracoScalarType type) {
  switch (type) {
  case DracoScalarType::Int8: case DracoScalarType::UInt8: case DracoScalarType::Bool: return 1;
  case DracoScalarType::Int16: case DracoScalarType::UInt16: return 2;
  case DracoScalarType::Int32: case DracoScalarType::UInt32: case DracoScalarType::Float32: return 4;
  case DracoScalarType::Int64: case DracoScalarType::UInt64: case DracoScalarType::Float64: return 8;
  }
  throw std::runtime_error("Draco worker protocol: unknown scalar type");
}

namespace draco_worker {
namespace {
constexpr uint32_t kMagic = 0x31435244; // DRC1, explicitly little-endian.
constexpr uint32_t kRequest = 1, kResponse = 2;
constexpr uint32_t kMaximumErrorBytes = 1024;

[[noreturn]] void fail(std::string_view reason) {
  throw std::runtime_error("Draco worker protocol: " + std::string(reason));
}
uint64_t add(uint64_t a, uint64_t b) {
  if (b > std::numeric_limits<uint64_t>::max() - a) fail("byte count overflow");
  return a + b;
}
uint64_t multiply(uint64_t a, uint64_t b) {
  if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) fail("byte count overflow");
  return a * b;
}
size_t count(uint64_t size) {
  if (size > std::numeric_limits<size_t>::max() ||
      size > uint64_t(std::numeric_limits<std::streamsize>::max())) fail("byte count exceeds host range");
  return static_cast<size_t>(size);
}
void validateAttributes(std::span<const DracoAttributeRequest> attributes) {
  if (attributes.empty() || attributes.size() > kMaximumAttributes) fail("attribute count is out of bounds");
  for (size_t i = 0; i < attributes.size(); ++i) {
    if (attributes[i].componentCount == 0 || attributes[i].componentCount > 16)
      fail("attribute component count is out of bounds");
    for (size_t j = 0; j < i; ++j)
      if (attributes[j].uniqueId == attributes[i].uniqueId) fail("duplicate attribute unique ID");
  }
}
class Writer {
public:
  explicit Writer(const std::filesystem::path& path) : stream_(path, std::ios::binary | std::ios::trunc) {
    if (!stream_) fail("cannot open output file");
  }
  void bytes(const void* data, uint64_t size) {
    if (size) stream_.write(static_cast<const char*>(data), static_cast<std::streamsize>(count(size)));
    if (!stream_) fail("cannot write output file");
  }
  void u32(uint32_t value) {
    std::array<unsigned char, 4> encoded{};
    for (uint32_t i = 0; i < 4; ++i) encoded[i] = static_cast<unsigned char>(value >> (8 * i));
    bytes(encoded.data(), encoded.size());
  }
  void u64(uint64_t value) {
    std::array<unsigned char, 8> encoded{};
    for (uint32_t i = 0; i < 8; ++i) encoded[i] = static_cast<unsigned char>(value >> (8 * i));
    bytes(encoded.data(), encoded.size());
  }
  void finish() { stream_.flush(); if (!stream_) fail("cannot flush output file"); }
private:
  std::ofstream stream_;
};
class Reader {
public:
  Reader(const std::filesystem::path& path, uint64_t maximum)
      : stream_(path, std::ios::binary) {
    if (!stream_ || !std::filesystem::is_regular_file(path)) fail("cannot open regular input file");
    remaining_ = std::filesystem::file_size(path);
    if (remaining_ > maximum) fail("input file exceeds byte budget");
    total_ = remaining_;
  }
  void bytes(void* destination, uint64_t size) {
    if (size > remaining_) fail("truncated frame");
    if (size) stream_.read(static_cast<char*>(destination), static_cast<std::streamsize>(count(size)));
    if (!stream_) fail("truncated frame");
    remaining_ -= size;
  }
  uint32_t u32() {
    std::array<unsigned char, 4> encoded{}; bytes(encoded.data(), encoded.size());
    uint32_t value{};
    for (uint32_t i = 0; i < 4; ++i) value |= uint32_t(encoded[i]) << (8 * i);
    return value;
  }
  uint64_t u64() {
    std::array<unsigned char, 8> encoded{}; bytes(encoded.data(), encoded.size());
    uint64_t value{};
    for (uint32_t i = 0; i < 8; ++i) value |= uint64_t(encoded[i]) << (8 * i);
    return value;
  }
  uint64_t remaining() const { return remaining_; }
  uint64_t total() const { return total_; }
private:
  std::ifstream stream_;
  uint64_t remaining_{}, total_{};
};
void writePrefix(Writer& writer, uint32_t kind) {
  writer.u32(kMagic); writer.u32(kProtocolVersion); writer.u32(kind);
}
void readPrefix(Reader& reader, uint32_t kind) {
  if (reader.u32() != kMagic || reader.u32() != kProtocolVersion || reader.u32() != kind)
    fail("incompatible magic, version or message kind");
}
void writeResponseHeader(Writer& writer, uint32_t status, uint64_t bytes,
                         uint64_t points, uint64_t faces, uint32_t attributes, uint32_t errorBytes) {
  writePrefix(writer, kResponse); writer.u32(status); writer.u64(bytes);
  writer.u64(points); writer.u64(faces); writer.u32(attributes); writer.u32(errorBytes);
}
} // namespace

void ValidateLimits(const DracoDecodeLimits& limits) {
  if (!limits.maxInputBytes || limits.maxInputBytes == std::numeric_limits<uint64_t>::max() ||
      limits.maxOutputBytes < kResponseHeaderBytes || limits.maxOutputBytes == std::numeric_limits<uint64_t>::max() ||
      !limits.maxPoints || limits.maxPoints > std::numeric_limits<uint32_t>::max() ||
      !limits.maxFaces || limits.maxFaces > std::numeric_limits<uint32_t>::max() ||
      !limits.processMemoryBytes || limits.processMemoryBytes > std::numeric_limits<size_t>::max() ||
      limits.processMemoryBytes == std::numeric_limits<size_t>::max() ||
      !limits.timeoutMs || limits.timeoutMs == std::numeric_limits<uint32_t>::max())
    fail("finite nonzero decode limits are required");
  (void)count(limits.maxInputBytes); (void)count(limits.maxOutputBytes);
}

void ValidateRequest(std::span<const std::byte> compressed,
                     std::span<const DracoAttributeRequest> attributes,
                     const DracoDecodeLimits& limits) {
  ValidateLimits(limits); validateAttributes(attributes);
  if (compressed.empty() || compressed.size() > limits.maxInputBytes) fail("compressed input exceeds byte budget");
}

uint64_t DecodedResponseBytes(const DracoDecodedMesh& mesh) {
  uint64_t bytes = add(kResponseHeaderBytes, multiply(mesh.faceIndices.size(), 4));
  for (const auto& attribute : mesh.attributes) bytes = add(bytes, add(kAttributeHeaderBytes, attribute.values.size()));
  return bytes;
}

void WriteRequest(const std::filesystem::path& path,
                  std::span<const std::byte> compressed,
                  std::span<const DracoAttributeRequest> attributes,
                  const DracoDecodeLimits& limits) {
  ValidateRequest(compressed, attributes, limits);
  Writer writer(path); writePrefix(writer, kRequest); writer.u32(static_cast<uint32_t>(attributes.size()));
  writer.u64(compressed.size()); writer.u64(limits.maxInputBytes); writer.u64(limits.maxOutputBytes);
  writer.u64(limits.maxPoints); writer.u64(limits.maxFaces); writer.u64(limits.processMemoryBytes);
  writer.u32(limits.timeoutMs); writer.u32(0);
  for (const auto& attribute : attributes) { writer.u32(attribute.uniqueId); writer.u32(attribute.componentCount); }
  writer.bytes(compressed.data(), compressed.size()); writer.finish();
}

Request ReadRequest(const std::filesystem::path& path) {
  // Read only the fixed header before trusting the declared input budget.
  Reader reader(path, std::numeric_limits<uint64_t>::max());
  readPrefix(reader, kRequest);
  const uint32_t attributes = reader.u32();
  const uint64_t compressedBytes = reader.u64();
  Request request;
  request.limits.maxInputBytes = reader.u64(); request.limits.maxOutputBytes = reader.u64();
  request.limits.maxPoints = reader.u64(); request.limits.maxFaces = reader.u64();
  request.limits.processMemoryBytes = reader.u64(); request.limits.timeoutMs = reader.u32();
  if (reader.u32() != 0) fail("unknown request flags");
  ValidateLimits(request.limits);
  if (!attributes || attributes > kMaximumAttributes || !compressedBytes ||
      compressedBytes > request.limits.maxInputBytes ||
      add(multiply(attributes, 8), compressedBytes) != reader.remaining()) fail("invalid request lengths");
  request.attributes.resize(attributes);
  for (auto& attribute : request.attributes) { attribute.uniqueId = reader.u32(); attribute.componentCount = reader.u32(); }
  validateAttributes(request.attributes);
  request.compressed.resize(count(compressedBytes));
  reader.bytes(request.compressed.data(), compressedBytes);
  return request;
}

void WriteResponse(const std::filesystem::path& path, const DracoDecodedMesh& mesh,
                   const DracoDecodeLimits& limits) {
  ValidateLimits(limits);
  const auto bytes = DecodedResponseBytes(mesh);
  if (!mesh.pointCount || mesh.pointCount > limits.maxPoints || mesh.faceIndices.empty() ||
      mesh.faceIndices.size() % 3 || mesh.faceIndices.size() / 3 > limits.maxFaces ||
      mesh.attributes.empty() || mesh.attributes.size() > kMaximumAttributes || bytes > limits.maxOutputBytes)
    fail("decoded mesh exceeds output/count budget");
  Writer writer(path);
  writeResponseHeader(writer, 0, bytes, mesh.pointCount, mesh.faceIndices.size() / 3,
                       static_cast<uint32_t>(mesh.attributes.size()), 0);
  for (const auto index : mesh.faceIndices) { if (index >= mesh.pointCount) fail("face index is out of range"); writer.u32(index); }
  for (const auto& attribute : mesh.attributes) {
    if (!attribute.componentCount || attribute.componentCount > 16 || attribute.values.size() !=
        multiply(mesh.pointCount, multiply(attribute.componentCount, DracoScalarByteWidth(attribute.scalarType))))
      fail("invalid attribute output length");
    writer.u32(attribute.uniqueId); writer.u32(static_cast<uint32_t>(attribute.scalarType));
    writer.u32(attribute.componentCount); writer.u32(attribute.normalized ? 1 : 0); writer.u64(attribute.values.size());
    writer.bytes(attribute.values.data(), attribute.values.size());
  }
  writer.finish();
}

void WriteFailure(const std::filesystem::path& path, std::string_view message, uint64_t maxOutputBytes) {
  if (maxOutputBytes < kResponseHeaderBytes) fail("error response exceeds output budget");
  const auto size = static_cast<uint32_t>(std::min<uint64_t>({message.size(), kMaximumErrorBytes, maxOutputBytes - kResponseHeaderBytes}));
  Writer writer(path); writeResponseHeader(writer, 1, kResponseHeaderBytes + size, 0, 0, 0, size);
  writer.bytes(message.data(), size); writer.finish();
}

DracoDecodedMesh ReadResponse(const std::filesystem::path& path,
    std::span<const DracoAttributeRequest> attributes, const DracoDecodeLimits& limits) {
  ValidateLimits(limits); validateAttributes(attributes);
  Reader reader(path, limits.maxOutputBytes); readPrefix(reader, kResponse);
  const uint32_t status = reader.u32();
  const uint64_t total = reader.u64(), points = reader.u64(), faces = reader.u64();
  const uint32_t attributeCount = reader.u32(), errorBytes = reader.u32();
  if (total != reader.total()) fail("response length does not match file");
  if (status != 0) {
    if (status != 1 || points || faces || attributeCount || errorBytes > kMaximumErrorBytes ||
        errorBytes != reader.remaining()) fail("invalid failure response");
    std::string message(errorBytes, '\0'); reader.bytes(message.data(), errorBytes);
    throw std::runtime_error("Draco worker decode failed: " + message);
  }
  const uint64_t indexCount = multiply(faces, 3), indexBytes = multiply(indexCount, 4);
  if (!points || points > limits.maxPoints || !faces || faces > limits.maxFaces ||
      attributeCount != attributes.size() || errorBytes ||
      add(indexBytes, multiply(attributeCount, kAttributeHeaderBytes)) > reader.remaining())
    fail("response counts exceed request budget");
  DracoDecodedMesh mesh; mesh.pointCount = static_cast<uint32_t>(points);
  mesh.faceIndices.resize(count(indexCount));
  for (auto& index : mesh.faceIndices) { index = reader.u32(); if (index >= points) fail("decoded face index is out of range"); }
  mesh.attributes.reserve(attributeCount);
  for (uint32_t i = 0; i < attributeCount; ++i) {
    DracoDecodedAttribute attribute;
    attribute.uniqueId = reader.u32(); attribute.scalarType = static_cast<DracoScalarType>(reader.u32());
    attribute.componentCount = reader.u32(); const uint32_t normalized = reader.u32();
    const uint64_t valueBytes = reader.u64();
    const auto expected = std::find_if(attributes.begin(), attributes.end(), [&](const auto& value) { return value.uniqueId == attribute.uniqueId; });
    if (expected == attributes.end() || expected->componentCount != attribute.componentCount || normalized > 1 ||
        std::any_of(mesh.attributes.begin(), mesh.attributes.end(), [&](const auto& value) { return value.uniqueId == attribute.uniqueId; }) ||
        valueBytes != multiply(points, multiply(attribute.componentCount, DracoScalarByteWidth(attribute.scalarType))) ||
        valueBytes > reader.remaining()) fail("invalid decoded attribute metadata");
    attribute.normalized = normalized != 0;
    attribute.values.resize(count(valueBytes)); reader.bytes(attribute.values.data(), valueBytes);
    mesh.attributes.push_back(std::move(attribute));
  }
  if (reader.remaining()) fail("unexpected trailing response bytes");
  return mesh;
}

} // namespace draco_worker
} // namespace container::geometry::gltf
