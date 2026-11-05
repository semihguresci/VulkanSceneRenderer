// Encoder-only helper for independently authored native compression fixtures.
// No renderer, compression-helper, or decoder API is called here.
#include <draco/compression/encode.h>
#include <draco/mesh/mesh.h>
#include <meshoptimizer.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Json = nlohmann::json;
using Bytes = std::vector<unsigned char>;
void require(bool valid, const std::string& message) {
  if (!valid) throw std::runtime_error(message);
}
Bytes read(const std::filesystem::path& path) {
  const auto length = std::filesystem::file_size(path);
  require(length > 0 && length <= 16 * 1024 * 1024, "Fixture input size is invalid");
  Bytes bytes(static_cast<size_t>(length));
  std::ifstream input(path, std::ios::binary);
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  require(input.good(), "Cannot read fixture input: " + path.string());
  return bytes;
}
void write(const std::filesystem::path& path, const unsigned char* data, size_t size) {
  require(size > 0, "Official encoder produced an empty stream");
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
  require(output.good(), "Cannot write encoded fixture: " + path.string());
}
size_t bounded(const Json& value, const char* name, size_t maximum) {
  const auto number = value.at(name).get<uint64_t>();
  require(number > 0 && number <= maximum, std::string("Invalid fixture ") + name);
  return static_cast<size_t>(number);
}
std::vector<unsigned int> indices(const Bytes& bytes, size_t count, size_t stride) {
  require((stride == 2 || stride == 4) && bytes.size() == count * stride, "Invalid index fixture");
  std::vector<unsigned int> result(count);
  for (size_t i = 0; i < count; ++i) {
    result[i] = 0;
    for (size_t byte = 0; byte < stride; ++byte)
      result[i] |= static_cast<unsigned int>(bytes[i * stride + byte]) << (byte * 8);
  }
  return result;
}
void meshopt(const Json& job) {
  const auto count = bounded(job, "count", 4096);
  const auto stride = bounded(job, "stride", 256);
  const auto input = read(job.at("input").get<std::string>());
  require(input.size() == count * stride, "Meshopt source byte count differs");
  const auto mode = job.at("mode").get<std::string>();
  Bytes encoded;
  size_t length = 0;
  if (mode == "ATTRIBUTES") {
    require(stride % 4 == 0, "Vertex source is not four-byte aligned");
    const int version = job.at("version").get<int>();
    require(version == 0 || version == 1, "Unsupported fixture vertex version");
    meshopt_encodeVertexVersion(version);
    encoded.resize(meshopt_encodeVertexBufferBound(count, stride));
    length = meshopt_encodeVertexBuffer(encoded.data(), encoded.size(), input.data(), count, stride);
  } else {
    const auto source = indices(input, count, stride);
    const size_t vertices = *std::max_element(source.begin(), source.end()) + size_t(1);
    require(vertices <= 4096, "Fixture vertex index exceeds its bound");
    if (mode == "TRIANGLES") {
      require(count % 3 == 0, "Triangle source has an incomplete face");
      encoded.resize(meshopt_encodeIndexBufferBound(count, vertices));
      length = meshopt_encodeIndexBuffer(encoded.data(), encoded.size(), source.data(), count);
    } else {
      require(mode == "INDICES", "Unknown meshopt fixture mode");
      encoded.resize(meshopt_encodeIndexSequenceBound(count, vertices));
      length = meshopt_encodeIndexSequence(encoded.data(), encoded.size(), source.data(), count);
    }
  }
  write(job.at("output").get<std::string>(), encoded.data(), length);
}
struct Scalar { draco::DataType type; size_t bytes; };
Scalar scalar(int component) {
  switch (component) {
    case 5120: return {draco::DT_INT8, 1};
    case 5121: return {draco::DT_UINT8, 1};
    case 5122: return {draco::DT_INT16, 2};
    case 5123: return {draco::DT_UINT16, 2};
    case 5125: return {draco::DT_UINT32, 4};
    case 5126: return {draco::DT_FLOAT32, 4};
    default: throw std::runtime_error("Unknown fixture component type");
  }
}
draco::GeometryAttribute::Type semantic(const std::string& name) {
  if (name == "POSITION") return draco::GeometryAttribute::POSITION;
  if (name == "NORMAL") return draco::GeometryAttribute::NORMAL;
  if (name == "TEXCOORD_0") return draco::GeometryAttribute::TEX_COORD;
  if (name == "COLOR_0") return draco::GeometryAttribute::COLOR;
  return draco::GeometryAttribute::GENERIC;
}
void dracoMesh(const Json& job) {
  const auto count = bounded(job, "count", 4096);
  const auto faces = bounded(job, "faces", 4096);
  const auto faceIndices = indices(read(job.at("indices").get<std::string>()), faces * 3, 4);
  draco::Mesh mesh;
  mesh.set_num_points(static_cast<uint32_t>(count));
  mesh.SetNumFaces(faces);
  for (size_t face = 0; face < faces; ++face) {
    for (size_t corner = 0; corner < 3; ++corner)
      require(faceIndices[face * 3 + corner] < count, "Draco fixture index out of range");
    mesh.SetFace(draco::FaceIndex(static_cast<uint32_t>(face)),
        {draco::PointIndex(faceIndices[face * 3]), draco::PointIndex(faceIndices[face * 3 + 1]),
         draco::PointIndex(faceIndices[face * 3 + 2])});
  }
  const auto& attributes = job.at("attributes");
  require(attributes.is_array() && !attributes.empty() && attributes.size() <= 8, "Invalid fixture attributes");
  for (const auto& value : attributes) {
    const auto components = bounded(value, "components", 4);
    const auto storage = scalar(value.at("component").get<int>());
    const auto raw = read(value.at("input").get<std::string>());
    const auto stride = storage.bytes * components;
    require(raw.size() == count * stride, "Draco source attribute size differs");
    draco::GeometryAttribute description;
    description.Init(semantic(value.at("semantic").get<std::string>()), nullptr,
        static_cast<int8_t>(components), storage.type, value.value("normalized", false),
        static_cast<int64_t>(stride), 0);
    const int attribute = mesh.AddAttribute(description, true, static_cast<uint32_t>(count));
    require(attribute >= 0, "Cannot allocate Draco fixture attribute");
    mesh.attribute(attribute)->set_unique_id(value.at("id").get<uint32_t>());
    for (uint32_t point = 0; point < count; ++point)
      mesh.attribute(attribute)->SetAttributeValue(draco::AttributeValueIndex(point), raw.data() + point * stride);
  }
  draco::Encoder encoder;
  encoder.SetSpeedOptions(10, 10);
  // Sequential encoding preserves the authored point order of untouched
  // tangent/morph/skin arrays. Zero quantization retains exact float payloads.
  encoder.SetEncodingMethod(draco::MESH_SEQUENTIAL_ENCODING);
  for (const auto type : {draco::GeometryAttribute::POSITION, draco::GeometryAttribute::NORMAL,
                         draco::GeometryAttribute::TEX_COORD, draco::GeometryAttribute::COLOR,
                         draco::GeometryAttribute::GENERIC}) {
    encoder.SetAttributeQuantization(type, 0);
  }
  draco::EncoderBuffer encoded;
  const auto status = encoder.EncodeMeshToBuffer(mesh, &encoded);
  require(status.ok(), status.error_msg_string());
  write(job.at("output").get<std::string>(),
      reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size());
}
} // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 2, "Usage: GltfCompressionFixtureGenerator <jobs.json>");
    std::ifstream manifest(argv[1]);
    Json jobs;
    manifest >> jobs;
    require(jobs.is_array() && !jobs.empty() && jobs.size() <= 128, "Invalid fixture job manifest");
    for (const auto& job : jobs) {
      if (job.at("codec") == "meshopt") meshopt(job);
      else { require(job.at("codec") == "draco", "Unknown fixture codec"); dracoMesh(job); }
    }
    std::cout << "PASS: " << jobs.size() << " official encoder jobs; no decoder called\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
