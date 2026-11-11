#include "Container/geometry/Model.h"
#include "Container/geometry/GltfLinkedAssets.h"
#include "Container/geometry/GltfModelLoader.h"
#include "Container/geometry/GltfIesProfiles.h"
#include "Container/geometry/GltfNodeFlags.h"
#include "Container/utility/ModelImportCancellation.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <glm/geometric.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

void appendFloat(std::vector<char>& bytes, float value) {
  const auto* raw = reinterpret_cast<const char*>(&value);
  bytes.insert(bytes.end(), raw, raw + sizeof(float));
}

void appendUint16(std::vector<char>& bytes, uint16_t value) {
  const auto* raw = reinterpret_cast<const char*>(&value);
  bytes.insert(bytes.end(), raw, raw + sizeof(uint16_t));
}

std::filesystem::path writeTriangleGltf(
    std::string_view name,
    const std::array<uint16_t, 3>& triangleIndices) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "container_gltf_loader_tests";
  std::filesystem::create_directories(dir);

  const std::filesystem::path gltfPath = dir / (std::string(name) + ".gltf");
  const std::filesystem::path binPath = dir / (std::string(name) + ".bin");

  std::vector<char> bytes;
  bytes.reserve(78);

  for (const float value : {
           0.0f, 0.0f, 0.0f,
           0.0f, 1.0f, 0.0f,
           1.0f, 0.0f, 0.0f,
       }) {
    appendFloat(bytes, value);
  }
  for (const float value : {
           0.0f, 0.0f, 1.0f,
           0.0f, 0.0f, 1.0f,
           0.0f, 0.0f, 1.0f,
       }) {
    appendFloat(bytes, value);
  }
  for (const uint16_t index : triangleIndices) {
    appendUint16(bytes, index);
  }

  {
    std::ofstream bin(binPath, std::ios::binary);
    bin.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  const std::string binName = binPath.filename().generic_string();
  std::ofstream gltf(gltfPath);
  gltf
      << "{\n"
      << "  \"asset\": {\"version\": \"2.0\"},\n"
      << "  \"buffers\": [{\"uri\": \"" << binName
      << "\", \"byteLength\": " << bytes.size() << "}],\n"
      << "  \"bufferViews\": [\n"
      << "    {\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 36},\n"
      << "    {\"buffer\": 0, \"byteOffset\": 36, \"byteLength\": 36},\n"
      << "    {\"buffer\": 0, \"byteOffset\": 72, \"byteLength\": 6}\n"
      << "  ],\n"
      << "  \"accessors\": [\n"
      << "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": 3, "
         "\"type\": \"VEC3\", \"min\": [0, 0, 0], \"max\": [1, 1, 0]},\n"
      << "    {\"bufferView\": 1, \"componentType\": 5126, \"count\": 3, "
         "\"type\": \"VEC3\"},\n"
      << "    {\"bufferView\": 2, \"componentType\": 5123, \"count\": 3, "
         "\"type\": \"SCALAR\"}\n"
      << "  ],\n"
      << "  \"materials\": [{}],\n"
      << "  \"meshes\": [{\"primitives\": [{\"attributes\": {\"POSITION\": 0, "
         "\"NORMAL\": 1}, \"indices\": 2, \"mode\": 4, \"material\": 0}]}],\n"
      << "  \"nodes\": [{\"mesh\": 0}],\n"
      << "  \"scenes\": [{\"nodes\": [0]}],\n"
      << "  \"scene\": 0\n"
      << "}\n";

  return gltfPath;
}

std::filesystem::path writeMixedWindingQuadGltf(std::string_view name) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "container_gltf_loader_tests";
  std::filesystem::create_directories(dir);

  const std::filesystem::path gltfPath = dir / (std::string(name) + ".gltf");
  const std::filesystem::path binPath = dir / (std::string(name) + ".bin");

  std::vector<char> bytes;
  bytes.reserve(108);

  for (const float value : {
           0.0f, 0.0f, 0.0f,
           0.0f, 1.0f, 0.0f,
           1.0f, 0.0f, 0.0f,
           1.0f, 1.0f, 0.0f,
       }) {
    appendFloat(bytes, value);
  }
  for (const float value : {
           0.0f, 0.0f, 1.0f,
           0.0f, 0.0f, 1.0f,
           0.0f, 0.0f, 1.0f,
           0.0f, 0.0f, 1.0f,
       }) {
    appendFloat(bytes, value);
  }
  for (const uint16_t index : {0, 1, 2, 0, 2, 3}) {
    appendUint16(bytes, index);
  }

  {
    std::ofstream bin(binPath, std::ios::binary);
    bin.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  const std::string binName = binPath.filename().generic_string();
  std::ofstream gltf(gltfPath);
  gltf
      << "{\n"
      << "  \"asset\": {\"version\": \"2.0\"},\n"
      << "  \"buffers\": [{\"uri\": \"" << binName
      << "\", \"byteLength\": " << bytes.size() << "}],\n"
      << "  \"bufferViews\": [\n"
      << "    {\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 48},\n"
      << "    {\"buffer\": 0, \"byteOffset\": 48, \"byteLength\": 48},\n"
      << "    {\"buffer\": 0, \"byteOffset\": 96, \"byteLength\": 12}\n"
      << "  ],\n"
      << "  \"accessors\": [\n"
      << "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": 4, "
         "\"type\": \"VEC3\", \"min\": [0, 0, 0], \"max\": [1, 1, 0]},\n"
      << "    {\"bufferView\": 1, \"componentType\": 5126, \"count\": 4, "
         "\"type\": \"VEC3\"},\n"
      << "    {\"bufferView\": 2, \"componentType\": 5123, \"count\": 6, "
         "\"type\": \"SCALAR\"}\n"
      << "  ],\n"
      << "  \"materials\": [{}],\n"
      << "  \"meshes\": [{\"primitives\": [{\"attributes\": {\"POSITION\": 0, "
         "\"NORMAL\": 1}, \"indices\": 2, \"mode\": 4, \"material\": 0}]}],\n"
      << "  \"nodes\": [{\"mesh\": 0}],\n"
      << "  \"scenes\": [{\"nodes\": [0]}],\n"
      << "  \"scene\": 0\n"
      << "}\n";

  return gltfPath;
}

std::filesystem::path writeMostlyConsistentWindingGltf(std::string_view name) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "container_gltf_loader_tests";
  std::filesystem::create_directories(dir);

  const std::filesystem::path gltfPath = dir / (std::string(name) + ".gltf");
  const std::filesystem::path binPath = dir / (std::string(name) + ".bin");

  std::vector<char> bytes;
  bytes.reserve(450);

  for (int triangle = 0; triangle < 5; ++triangle) {
    const float x = static_cast<float>(triangle) * 2.0f;
    for (const float value : {
             x, 0.0f, 0.0f,
             x + 1.0f, 0.0f, 0.0f,
             x, 1.0f, 0.0f,
         }) {
      appendFloat(bytes, value);
    }
  }
  for (int vertex = 0; vertex < 15; ++vertex) {
    for (const float value : {0.0f, 0.0f, 1.0f}) {
      appendFloat(bytes, value);
    }
  }
  for (const uint16_t index : {
           0, 1, 2,
           3, 4, 5,
           6, 7, 8,
           9, 10, 11,
           12, 14, 13,
       }) {
    appendUint16(bytes, index);
  }

  {
    std::ofstream bin(binPath, std::ios::binary);
    bin.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  const std::string binName = binPath.filename().generic_string();
  std::ofstream gltf(gltfPath);
  gltf
      << "{\n"
      << "  \"asset\": {\"version\": \"2.0\"},\n"
      << "  \"buffers\": [{\"uri\": \"" << binName
      << "\", \"byteLength\": " << bytes.size() << "}],\n"
      << "  \"bufferViews\": [\n"
      << "    {\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 180},\n"
      << "    {\"buffer\": 0, \"byteOffset\": 180, \"byteLength\": 180},\n"
      << "    {\"buffer\": 0, \"byteOffset\": 360, \"byteLength\": 30}\n"
      << "  ],\n"
      << "  \"accessors\": [\n"
      << "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": 15, "
         "\"type\": \"VEC3\", \"min\": [0, 0, 0], \"max\": [9, 1, 0]},\n"
      << "    {\"bufferView\": 1, \"componentType\": 5126, \"count\": 15, "
         "\"type\": \"VEC3\"},\n"
      << "    {\"bufferView\": 2, \"componentType\": 5123, \"count\": 15, "
         "\"type\": \"SCALAR\"}\n"
      << "  ],\n"
      << "  \"materials\": [{}],\n"
      << "  \"meshes\": [{\"primitives\": [{\"attributes\": {\"POSITION\": 0, "
         "\"NORMAL\": 1}, \"indices\": 2, \"mode\": 4, \"material\": 0}]}],\n"
      << "  \"nodes\": [{\"mesh\": 0}],\n"
      << "  \"scenes\": [{\"nodes\": [0]}],\n"
      << "  \"scene\": 0\n"
      << "}\n";

  return gltfPath;
}

std::filesystem::path writeDenseOpenReliefGltf(std::string_view name) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "container_gltf_loader_tests";
  std::filesystem::create_directories(dir);

  const std::filesystem::path gltfPath = dir / (std::string(name) + ".gltf");
  const std::filesystem::path binPath = dir / (std::string(name) + ".bin");

  constexpr uint16_t kColumns = 64;
  constexpr uint16_t kRows = 8;
  constexpr uint32_t kVertexCount =
      static_cast<uint32_t>(kColumns + 1u) * static_cast<uint32_t>(kRows + 1u);
  constexpr uint32_t kIndexCount =
      static_cast<uint32_t>(kColumns) * static_cast<uint32_t>(kRows) * 6u;
  constexpr size_t kPositionBytes = kVertexCount * 3u * sizeof(float);
  constexpr size_t kNormalBytes = kVertexCount * 3u * sizeof(float);
  constexpr size_t kIndexBytes = kIndexCount * sizeof(uint16_t);

  std::vector<char> bytes;
  bytes.reserve(kPositionBytes + kNormalBytes + kIndexBytes);

  for (uint16_t y = 0; y <= kRows; ++y) {
    for (uint16_t x = 0; x <= kColumns; ++x) {
      appendFloat(bytes, static_cast<float>(x));
      appendFloat(bytes, static_cast<float>(y));
      appendFloat(bytes, 0.0f);
    }
  }
  for (uint32_t vertex = 0; vertex < kVertexCount; ++vertex) {
    appendFloat(bytes, 0.0f);
    appendFloat(bytes, 0.0f);
    appendFloat(bytes, 1.0f);
  }
  for (uint16_t y = 0; y < kRows; ++y) {
    for (uint16_t x = 0; x < kColumns; ++x) {
      const uint16_t row0 = static_cast<uint16_t>(y * (kColumns + 1u));
      const uint16_t row1 = static_cast<uint16_t>((y + 1u) * (kColumns + 1u));
      const uint16_t v00 = static_cast<uint16_t>(row0 + x);
      const uint16_t v10 = static_cast<uint16_t>(row0 + x + 1u);
      const uint16_t v01 = static_cast<uint16_t>(row1 + x);
      const uint16_t v11 = static_cast<uint16_t>(row1 + x + 1u);
      appendUint16(bytes, v00);
      appendUint16(bytes, v10);
      appendUint16(bytes, v11);
      appendUint16(bytes, v00);
      appendUint16(bytes, v11);
      appendUint16(bytes, v01);
    }
  }

  {
    std::ofstream bin(binPath, std::ios::binary);
    bin.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  const std::string binName = binPath.filename().generic_string();
  std::ofstream gltf(gltfPath);
  gltf
      << "{\n"
      << "  \"asset\": {\"version\": \"2.0\"},\n"
      << "  \"buffers\": [{\"uri\": \"" << binName
      << "\", \"byteLength\": " << bytes.size() << "}],\n"
      << "  \"bufferViews\": [\n"
      << "    {\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": "
      << kPositionBytes << "},\n"
      << "    {\"buffer\": 0, \"byteOffset\": " << kPositionBytes
      << ", \"byteLength\": " << kNormalBytes << "},\n"
      << "    {\"buffer\": 0, \"byteOffset\": " << (kPositionBytes + kNormalBytes)
      << ", \"byteLength\": " << kIndexBytes << "}\n"
      << "  ],\n"
      << "  \"accessors\": [\n"
      << "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": "
      << kVertexCount
      << ", \"type\": \"VEC3\", \"min\": [0, 0, 0], \"max\": ["
      << kColumns << ", " << kRows << ", 0]},\n"
      << "    {\"bufferView\": 1, \"componentType\": 5126, \"count\": "
      << kVertexCount << ", \"type\": \"VEC3\"},\n"
      << "    {\"bufferView\": 2, \"componentType\": 5123, \"count\": "
      << kIndexCount << ", \"type\": \"SCALAR\"}\n"
      << "  ],\n"
      << "  \"materials\": [{}],\n"
      << "  \"meshes\": [{\"primitives\": [{\"attributes\": {\"POSITION\": 0, "
         "\"NORMAL\": 1}, \"indices\": 2, \"mode\": 4, \"material\": 0}]}],\n"
      << "  \"nodes\": [{\"mesh\": 0}],\n"
      << "  \"scenes\": [{\"nodes\": [0]}],\n"
      << "  \"scene\": 0\n"
      << "}\n";

  return gltfPath;
}

std::filesystem::path writeTangentTriangleGltf(
    std::string_view name,
    const std::array<glm::vec4, 3>& tangents,
    bool includeNormals = true) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "container_gltf_loader_tests";
  std::filesystem::create_directories(dir);

  const std::filesystem::path gltfPath = dir / (std::string(name) + ".gltf");
  const std::filesystem::path binPath = dir / (std::string(name) + ".bin");

  std::vector<char> bytes;
  bytes.reserve(150);

  for (const float value : {
           0.0f, 0.0f, 0.0f,
           1.0f, 0.0f, 0.0f,
           0.0f, 1.0f, 0.0f,
       }) {
    appendFloat(bytes, value);
  }
  for (const float value : {
           0.0f, 0.0f, 1.0f,
           0.0f, 0.0f, 1.0f,
           0.0f, 0.0f, 1.0f,
       }) {
    appendFloat(bytes, value);
  }
  for (const float value : {
           0.0f, 0.0f,
           0.0f, 1.0f,
           1.0f, 0.0f,
       }) {
    appendFloat(bytes, value);
  }
  for (const glm::vec4& tangent : tangents) {
    appendFloat(bytes, tangent.x);
    appendFloat(bytes, tangent.y);
    appendFloat(bytes, tangent.z);
    appendFloat(bytes, tangent.w);
  }
  for (const uint16_t index : {0, 1, 2}) {
    appendUint16(bytes, index);
  }

  {
    std::ofstream bin(binPath, std::ios::binary);
    bin.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  const std::string binName = binPath.filename().generic_string();
  std::ofstream gltf(gltfPath);
  gltf
      << "{\n"
      << "  \"asset\": {\"version\": \"2.0\"},\n"
      << "  \"buffers\": [{\"uri\": \"" << binName
      << "\", \"byteLength\": " << bytes.size() << "}],\n"
      << "  \"bufferViews\": [\n"
      << "    {\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 36},\n"
      << "    {\"buffer\": 0, \"byteOffset\": 36, \"byteLength\": 36},\n"
      << "    {\"buffer\": 0, \"byteOffset\": 72, \"byteLength\": 24},\n"
      << "    {\"buffer\": 0, \"byteOffset\": 96, \"byteLength\": 48},\n"
      << "    {\"buffer\": 0, \"byteOffset\": 144, \"byteLength\": 6}\n"
      << "  ],\n"
      << "  \"accessors\": [\n"
      << "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": 3, "
         "\"type\": \"VEC3\", \"min\": [0, 0, 0], \"max\": [1, 1, 0]},\n"
      << "    {\"bufferView\": 1, \"componentType\": 5126, \"count\": 3, "
         "\"type\": \"VEC3\"},\n"
      << "    {\"bufferView\": 2, \"componentType\": 5126, \"count\": 3, "
         "\"type\": \"VEC2\"},\n"
      << "    {\"bufferView\": 3, \"componentType\": 5126, \"count\": 3, "
         "\"type\": \"VEC4\"},\n"
      << "    {\"bufferView\": 4, \"componentType\": 5123, \"count\": 3, "
         "\"type\": \"SCALAR\"}\n"
      << "  ],\n"
      << "  \"materials\": [{}],\n"
      << "  \"meshes\": [{\"primitives\": [{\"attributes\": {\"POSITION\": 0, "
      << (includeNormals ? "\"NORMAL\": 1, " : "")
      << "\"TEXCOORD_0\": 2, \"TANGENT\": 3}, "
         "\"indices\": 4, \"mode\": 4, \"material\": 0}]}],\n"
      << "  \"nodes\": [{\"mesh\": 0}],\n"
      << "  \"scenes\": [{\"nodes\": [0]}],\n"
      << "  \"scene\": 0\n"
      << "}\n";

  return gltfPath;
}

template <class T>
int appendUvTriangleAccessor(tinygltf::Model& source, const std::vector<T>& values,
                              int type, int component) {
  auto& bytes = source.buffers[0].data;
  while (bytes.size() % 4) bytes.push_back(0);
  tinygltf::BufferView view;
  view.buffer = 0;
  view.byteOffset = bytes.size();
  view.byteLength = values.size() * sizeof(T);
  const auto* begin = reinterpret_cast<const unsigned char*>(values.data());
  bytes.insert(bytes.end(), begin, begin + view.byteLength);
  tinygltf::Accessor accessor;
  accessor.bufferView = static_cast<int>(source.bufferViews.size());
  accessor.type = type;
  accessor.componentType = component;
  accessor.count = values.size() / static_cast<size_t>(tinygltf::GetNumComponentsInType(type));
  source.bufferViews.push_back(view);
  source.accessors.push_back(accessor);
  return static_cast<int>(source.accessors.size() - 1);
}

std::filesystem::path writeNormalMapUvTriangleGltf(
    std::string_view name, int texCoord, std::optional<int> transformTexCoord,
    const std::optional<std::array<glm::vec4,3>>& tangents = {}, bool skinned = false) {
  const std::array<glm::vec4,3> absent{};
  const auto path = writeTangentTriangleGltf(name, tangents ? *tangents : absent);
  auto source = container::geometry::gltf::LoadSourceModel(path.string());
  auto& primitive = source.meshes[0].primitives[0];
  if (!tangents) primitive.attributes.erase("TANGENT");
  // UV0 gives T=+Y, w=-1. UV1 reverses U, giving T=-Y, w=+1.
  // Position/normal/index order stays identical in both parameterizations.
  primitive.attributes["TEXCOORD_1"] = appendUvTriangleAccessor(source,
      std::vector<float>{0,0, 0,1, -1,0}, TINYGLTF_TYPE_VEC2, TINYGLTF_COMPONENT_TYPE_FLOAT);
  tinygltf::Image image;
  image.name = "Neutral normal";
  image.uri = "neutral_normal.png";
  image.mimeType = "image/png";
  image.width = image.height = 1;
  image.component = 4;
  image.bits = 8;
  image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
  image.image = {128,128,255,255};
  source.images.push_back(std::move(image));
  source.textures.emplace_back();
  source.textures[0].source = 0;
  auto& normal = source.materials[0].normalTexture;
  normal.index = 0;
  normal.texCoord = texCoord;
  if (transformTexCoord) {
    normal.extensions["KHR_texture_transform"] = tinygltf::Value(
        tinygltf::Value::Object{{"texCoord", tinygltf::Value(*transformTexCoord)}});
    source.extensionsUsed.push_back("KHR_texture_transform");
  }
  if (skinned) {
    primitive.attributes["JOINTS_0"] = appendUvTriangleAccessor(source,
        std::vector<uint8_t>(12,0), TINYGLTF_TYPE_VEC4, TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE);
    primitive.attributes["WEIGHTS_0"] = appendUvTriangleAccessor(source,
        std::vector<float>{1,0,0,0, 1,0,0,0, 1,0,0,0},
        TINYGLTF_TYPE_VEC4, TINYGLTF_COMPONENT_TYPE_FLOAT);
    tinygltf::Skin skin;
    skin.joints = {1};
    source.skins.push_back(skin);
    source.nodes[0].skin = 0;
    source.nodes.emplace_back();
    source.scenes[0].nodes.push_back(1);
  }
  tinygltf::TinyGLTF writer;
  if (!writer.WriteGltfSceneToFile(&source, path.string(), true, true, true, false))
    throw std::runtime_error("Could not write normal-map UV triangle fixture");
  return path;
}

glm::vec3 firstTriangleFaceNormal(const container::geometry::Model& model) {
  const auto& vertices = model.vertices();
  const auto& indices = model.indices();
  const glm::vec3& p0 = vertices[indices[0]].position;
  const glm::vec3& p1 = vertices[indices[1]].position;
  const glm::vec3& p2 = vertices[indices[2]].position;
  return glm::normalize(glm::cross(p1 - p0, p2 - p0));
}

void expectTangentNear(const glm::vec4& actual, const glm::vec4& expected) {
  EXPECT_NEAR(actual.x, expected.x, 1e-5f);
  EXPECT_NEAR(actual.y, expected.y, 1e-5f);
  EXPECT_NEAR(actual.z, expected.z, 1e-5f);
  EXPECT_NEAR(actual.w, expected.w, 1e-5f);
}

void expectFiniteOrthonormalTangent(
    const container::geometry::Vertex& vertex) {
  EXPECT_TRUE(std::isfinite(vertex.tangent.x));
  EXPECT_TRUE(std::isfinite(vertex.tangent.y));
  EXPECT_TRUE(std::isfinite(vertex.tangent.z));
  EXPECT_TRUE(std::isfinite(vertex.tangent.w));
  EXPECT_NEAR(glm::length(glm::vec3(vertex.tangent)), 1.0f, 1e-5f);
  EXPECT_NEAR(glm::dot(glm::vec3(vertex.tangent), vertex.normal), 0.0f,
              1e-5f);
  EXPECT_TRUE(vertex.tangent.w == 1.0f || vertex.tangent.w == -1.0f);
}

std::filesystem::path findSampleAsset(std::string_view relativePath) {
  const std::filesystem::path relative{std::string(relativePath)};
  if (std::filesystem::exists(relative)) {
    return std::filesystem::absolute(relative);
  }

  std::vector<std::filesystem::path> roots;
  auto addRoot = [&](std::filesystem::path root) {
    if (!root.empty()) {
      roots.push_back(std::filesystem::absolute(std::move(root)));
    }
  };

  addRoot(std::filesystem::current_path());
  addRoot(std::filesystem::absolute(__FILE__).parent_path().parent_path());

  for (std::filesystem::path cursor = std::filesystem::current_path();
       !cursor.empty(); cursor = cursor.parent_path()) {
    addRoot(cursor);
    if (cursor == cursor.parent_path()) {
      break;
    }
  }

  for (const auto& root : roots) {
    const std::array candidates = {
        root / relative,
        root / "out" / "build" / "windows-debug" / relative,
        root / "out" / "build" / "windows-release" / relative,
    };
    for (const auto& candidate : candidates) {
      if (std::filesystem::exists(candidate)) {
        return std::filesystem::absolute(candidate);
      }
    }
  }

  return {};
}

}  // namespace

TEST(GltfModelLoader, CancelledRequestsFailBeforeSourceIoAndAllowAReplacement) {
  using namespace container::geometry::gltf;
  const auto path = writeTriangleGltf("cancelled_before_read", {0, 1, 2});
  std::stop_source cancelled;
  cancelled.request_stop();
  LoadOptions options;
  options.stop = cancelled.get_token();
  const auto absent = path.parent_path() / "cancelled_source_does_not_exist.gltf";
  EXPECT_THROW((void)LoadSourceModel(absent.string(), options),
               container::util::ModelImportCancelled);
  EXPECT_THROW((void)LoadModelWithSource(path.string(), options),
               container::util::ModelImportCancelled);

  std::stop_source replacement;
  options.stop = replacement.get_token();
  const auto loaded = LoadModelWithSource(path.string(), options);
  ASSERT_EQ(loaded.model.meshes().size(), 1);
  EXPECT_EQ(loaded.model.meshes()[0].vertices().size(), 3);
}

TEST(GltfModelLoader, LinkedCompositionStopsAfterReadingAnActiveChild) {
  using namespace container::geometry::gltf;
  const auto parent = writeTriangleGltf("cancelled_link_parent", {0, 1, 2});
  const auto child = writeTriangleGltf("cancelled_link_child", {0, 1, 2});
  const auto read = [&](const GltfLinkedReadContext& context) {
    LoadOptions options;
    options.stop = context.stop;
    GltfLinkedDocument document{LoadSourceModel(context.source.string(), options), {}};
    if (!context.child) {
      document.model.nodes[0].mesh = -1;
      document.manifest.nodeFiles = {0};
      document.manifest.files.push_back({child.filename(), "model/gltf+json", {}});
    }
    return document;
  };

  std::stop_source source;
  size_t documents = 0;
  GltfLinkedCompositionInfo cancelled;
  EXPECT_THROW((void)LoadGltfLinkedComposition(parent, [&](const auto& context) {
    auto document = read(context);
    ++documents;
    if (context.child) source.request_stop();
    return document;
  }, &cancelled, source.get_token()), container::util::ModelImportCancelled);
  EXPECT_EQ(documents, 2);
  EXPECT_TRUE(cancelled.placementRecords.empty());

  GltfLinkedCompositionInfo completed;
  const auto model = LoadGltfLinkedComposition(parent, read, &completed);
  ASSERT_EQ(model.nodes.size(), 2);
  EXPECT_EQ(completed.placementRecords.size(), 1);
}

TEST(GltfModelLoader, RepairsTriangleWindingOppositeImportedNormals) {
  const auto gltfPath = writeTriangleGltf("bad_winding", {0, 1, 2});

  const auto model =
      container::geometry::Model::LoadFromGltf(gltfPath.string());

  ASSERT_EQ(model.indices().size(), 3u);
  ASSERT_EQ(model.primitiveRanges().size(), 1u);
  EXPECT_EQ(model.indices()[0], 0u);
  EXPECT_EQ(model.indices()[1], 2u);
  EXPECT_EQ(model.indices()[2], 1u);
  EXPECT_FALSE(model.primitiveRanges()[0].disableBackfaceCulling);
  EXPECT_GT(firstTriangleFaceNormal(model).z, 0.99f);
}

TEST(GltfModelLoader, KeepsTriangleWindingAlignedWithImportedNormals) {
  const auto gltfPath = writeTriangleGltf("good_winding", {0, 2, 1});

  const auto model =
      container::geometry::Model::LoadFromGltf(gltfPath.string());

  ASSERT_EQ(model.indices().size(), 3u);
  ASSERT_EQ(model.primitiveRanges().size(), 1u);
  EXPECT_EQ(model.indices()[0], 0u);
  EXPECT_EQ(model.indices()[1], 2u);
  EXPECT_EQ(model.indices()[2], 1u);
  EXPECT_FALSE(model.primitiveRanges()[0].disableBackfaceCulling);
  EXPECT_GT(firstTriangleFaceNormal(model).z, 0.99f);
}

TEST(GltfModelLoader, DoesNotLocallyFlipMixedEvidencePrimitive) {
  const auto gltfPath = writeMixedWindingQuadGltf("mixed_winding");

  const auto model =
      container::geometry::Model::LoadFromGltf(gltfPath.string());

  ASSERT_EQ(model.indices().size(), 6u);
  ASSERT_EQ(model.primitiveRanges().size(), 1u);
  EXPECT_EQ(model.indices()[0], 0u);
  EXPECT_EQ(model.indices()[1], 1u);
  EXPECT_EQ(model.indices()[2], 2u);
  EXPECT_EQ(model.indices()[3], 0u);
  EXPECT_EQ(model.indices()[4], 2u);
  EXPECT_EQ(model.indices()[5], 3u);
  EXPECT_TRUE(model.primitiveRanges()[0].disableBackfaceCulling);
}

TEST(GltfModelLoader, KeepsCullDisabledForIsolatedWindingOutliers) {
  const auto gltfPath =
      writeMostlyConsistentWindingGltf("mostly_consistent_winding");

  const auto model =
      container::geometry::Model::LoadFromGltf(gltfPath.string());

  ASSERT_EQ(model.indices().size(), 15u);
  ASSERT_EQ(model.primitiveRanges().size(), 1u);
  EXPECT_EQ(model.indices()[12], 12u);
  EXPECT_EQ(model.indices()[13], 14u);
  EXPECT_EQ(model.indices()[14], 13u);
  EXPECT_TRUE(model.primitiveRanges()[0].disableBackfaceCulling);
}

TEST(GltfModelLoader, DisablesCullForDenseOpenReliefTopology) {
  const auto gltfPath = writeDenseOpenReliefGltf("dense_open_relief");

  const auto model =
      container::geometry::Model::LoadFromGltf(gltfPath.string());

  ASSERT_EQ(model.primitiveRanges().size(), 1u);
  EXPECT_TRUE(model.primitiveRanges()[0].disableBackfaceCulling);
}

TEST(GltfModelLoader, SampleTextureSettingsSingleSidedPrimitiveKeepsCulling) {
  const auto gltfPath = findSampleAsset(
      "models/glTF-Sample-Models/2.0/TextureSettingsTest/glTF/"
      "TextureSettingsTest.gltf");
  if (gltfPath.empty()) {
    GTEST_SKIP() << "TextureSettingsTest sample asset is not available";
  }

  const auto model =
      container::geometry::Model::LoadFromGltf(gltfPath.string());

  ASSERT_GT(model.primitiveRanges().size(), 8u);
  const auto singleSided = std::ranges::find_if(
      model.primitiveRanges(), [](const container::geometry::PrimitiveRange& p) {
        return p.materialIndex == 8;
      });
  ASSERT_NE(singleSided, model.primitiveRanges().end())
      << "SingleSidedMaterial primitive was not found";
  EXPECT_FALSE(singleSided->disableBackfaceCulling)
      << "TextureSettings single-sided is an open glTF plane, but it must keep "
         "normal back-face culling unless the material is doubleSided.";
}

TEST(GltfModelLoader, SampleSponzaLionReliefPrimitivesUseNoCullException) {
  const auto gltfPath = findSampleAsset(
      "models/glTF-Sample-Models/2.0/Sponza/glTF/Sponza.gltf");
  if (gltfPath.empty()) {
    GTEST_SKIP() << "Sponza sample asset is not available";
  }

  const auto model =
      container::geometry::Model::LoadFromGltf(gltfPath.string());

  size_t lionReliefCount = 0;
  for (const auto& primitive : model.primitiveRanges()) {
    if (primitive.materialIndex != 23 || primitive.indexCount < 1024u * 3u) {
      continue;
    }
    ++lionReliefCount;
    EXPECT_TRUE(primitive.disableBackfaceCulling)
        << "Sponza lion relief material 23 is a dense open embossed surface; "
           "it must stay in the no-cull exception bucket.";
  }
  EXPECT_GE(lionReliefCount, 2u)
      << "Expected the two large Sponza lion relief primitives to be present.";
}

TEST(GltfModelLoader, PreservesValidImportedTangents) {
  const glm::vec4 expectedTangent(0.0f, 1.0f, 0.0f, -1.0f);
  const auto gltfPath = writeTangentTriangleGltf(
      "valid_tangents",
      {expectedTangent, expectedTangent, expectedTangent});

  const auto model =
      container::geometry::Model::LoadFromGltf(gltfPath.string());

  ASSERT_EQ(model.vertices().size(), 3u);
  for (const auto& vertex : model.vertices()) {
    expectFiniteOrthonormalTangent(vertex);
    expectTangentNear(vertex.tangent, expectedTangent);
  }
}

TEST(GltfModelLoader, IgnoresSuppliedTangentsWhenBaseNormalsAreMissing) {
  const glm::vec4 wrongTangent(1, 0, 0, 1);
  const auto path = writeTangentTriangleGltf(
      "missing_normal_authored_tangent", {wrongTangent, wrongTangent, wrongTangent}, false);
  const auto model = container::geometry::Model::LoadFromGltf(path.string());
  ASSERT_EQ(model.vertices().size(), 3u);
  for (const auto& vertex : model.vertices()) {
    expectFiniteOrthonormalTangent(vertex);
    EXPECT_GT(vertex.normal.z, .99f);
    expectTangentNear(vertex.tangent, glm::vec4(0, 1, 0, -1));
  }
}

TEST(GltfModelLoader, RepairsInvalidImportedTangentsFromGeometry) {
  const glm::vec4 expectedTangent(0.0f, 1.0f, 0.0f, -1.0f);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const auto gltfPath = writeTangentTriangleGltf(
      "invalid_tangents",
      {expectedTangent, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
       glm::vec4(nan, 0.0f, 0.0f, 1.0f)});

  const auto model =
      container::geometry::Model::LoadFromGltf(gltfPath.string());

  ASSERT_EQ(model.vertices().size(), 3u);
  for (const auto& vertex : model.vertices()) {
    expectFiniteOrthonormalTangent(vertex);
    expectTangentNear(vertex.tangent, expectedTangent);
  }
}

TEST(GltfModelLoader, GeneratesMissingTangentsFromNormalTextureUvSelection) {
  for (bool skinned : {false, true}) {
    for (int selected : {0,1}) {
      const auto path = writeNormalMapUvTriangleGltf(
          std::string(skinned ? "skin" : "static") + "_normal_uv" + std::to_string(selected),
          selected, {}, {}, skinned);
      const auto loaded = container::geometry::gltf::LoadModelWithSource(path.string());
      ASSERT_EQ(loaded.model.vertices().size(), 3u);
      EXPECT_EQ(loaded.deformation.has_value(), skinned);
      const auto expected = selected == 0 ? glm::vec4(0,1,0,-1) : glm::vec4(0,-1,0,1);
      for (const auto& vertex : loaded.model.vertices()) {
        expectFiniteOrthonormalTangent(vertex);
        expectTangentNear(vertex.tangent, expected);
      }
      if (skinned) {
        ASSERT_EQ(loaded.deformation->primitives.size(), 1u);
        EXPECT_EQ(loaded.deformation->primitives[0].normalTextureTexCoord, selected);
        EXPECT_TRUE(loaded.deformation->primitives[0].authoredBaseNormal);
        EXPECT_FALSE(loaded.deformation->primitives[0].authoredBaseTangent);
      }
    }
  }
}

TEST(GltfModelLoader, NormalTextureTransformUvOverrideControlsGeneratedTangents) {
  for (int selected : {0,1}) {
    const auto path = writeNormalMapUvTriangleGltf(
        "normal_texture_override_uv" + std::to_string(selected), 1-selected, selected, {}, true);
    const auto loaded = container::geometry::gltf::LoadModelWithSource(path.string());
    ASSERT_EQ(loaded.model.vertices().size(), 3u);
    ASSERT_TRUE(loaded.deformation.has_value());
    EXPECT_EQ(loaded.deformation->primitives[0].normalTextureTexCoord, selected);
    const auto expected = selected == 0 ? glm::vec4(0,1,0,-1) : glm::vec4(0,-1,0,1);
    for (const auto& vertex : loaded.model.vertices()) expectTangentNear(vertex.tangent, expected);
  }
}

TEST(GltfModelLoader, UvSelectionPreservesAuthoredTangentsAndRepairsOnlyInvalidVertices) {
  const glm::vec4 authored(1,0,0,-1);
  const auto validPath = writeNormalMapUvTriangleGltf("authored_normal_uv1", 1, {},
      std::array{authored,authored,authored});
  const auto valid = container::geometry::gltf::LoadModelWithSource(validPath.string());
  ASSERT_EQ(valid.model.vertices().size(), 3u);
  for (const auto& vertex : valid.model.vertices()) expectTangentNear(vertex.tangent, authored);

  const auto repairedPath = writeNormalMapUvTriangleGltf("invalid_normal_uv1", 0, 1,
      std::array{authored,glm::vec4(0),glm::vec4(0)}, true);
  const auto repaired = container::geometry::gltf::LoadModelWithSource(repairedPath.string());
  ASSERT_EQ(repaired.model.vertices().size(), 3u);
  expectTangentNear(repaired.model.vertices()[0].tangent, authored);
  for (size_t i : {1u,2u}) {
    expectFiniteOrthonormalTangent(repaired.model.vertices()[i]);
    expectTangentNear(repaired.model.vertices()[i].tangent, glm::vec4(0,-1,0,1));
  }
}

TEST(GltfModelLoader, InstancingUsesSharedGeometryAndRejectsBadMetadataBeforeBufferIo) {
  namespace gltf=container::geometry::gltf;
  const auto path=writeTriangleGltf("instanced-shared",{0,1,2});
  nlohmann::json source; {std::ifstream file(path);file>>source;}
  source["extensionsUsed"]=source["extensionsRequired"]={"EXT_mesh_gpu_instancing"};
  source["nodes"][0]["extensions"]["EXT_mesh_gpu_instancing"]["attributes"]={{"TRANSLATION",0}};
  {std::ofstream file(path);file<<source.dump();}
  const auto loaded=gltf::LoadModelWithSource(path.string());
  ASSERT_EQ(loaded.instancing.nodes.size(),1u);
  EXPECT_EQ(loaded.instancing.summary.instances,3u); EXPECT_EQ(loaded.model.vertices().size(),3u);
  EXPECT_EQ(loaded.model.primitiveRanges().size(),1u);
  source["buffers"][0]["uri"]="must-not-be-opened.bin";
  source["nodes"][0]["extensions"]["EXT_mesh_gpu_instancing"]["attributes"]["TRANSLATION"]=4294967297ull;
  {std::ofstream file(path);file<<source.dump();}
  try { (void)gltf::LoadModelWithSource(path.string()); FAIL()<<"Expected checked metadata rejection"; }
  catch(const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("EXT_mesh_gpu_instancing"),std::string::npos);
    EXPECT_NE(std::string(error.what()).find("TRANSLATION"),std::string::npos);
  }
}

namespace {

nlohmann::json readGltfJson(const std::filesystem::path& path) {
  nlohmann::json source;
  std::ifstream file(path);
  file >> source;
  return source;
}

void writeGltfJson(const std::filesystem::path& path, const nlohmann::json& source) {
  std::ofstream file(path);
  file << source.dump();
}

void expectSceneRejectionBeforeBufferIo(const std::filesystem::path& path,
                                      nlohmann::json source,
                                      std::string_view expected) {
  source["buffers"][0]["uri"] = "scene-metadata-must-not-be-opened.bin";
  writeGltfJson(path, source);
  try {
    (void)container::geometry::gltf::LoadSourceModel(path.string());
    FAIL() << "Expected scene metadata rejection";
  } catch (const std::runtime_error& error) {
    const std::string message(error.what());
    EXPECT_NE(message.find("glTF scene hierarchy:"), std::string::npos) << message;
    EXPECT_NE(message.find(expected), std::string::npos) << message;
    EXPECT_EQ(message.find("scene-metadata-must-not-be-opened.bin"), std::string::npos)
        << "Scene metadata must reject before opening an external buffer";
  }
}

}  // namespace

TEST(GltfModelLoader, SceneMetadataPreservesSharedRootsDuplicateNamesAndEmptyScenes) {
  const auto path = writeTriangleGltf("scene-metadata-valid", {0, 1, 2});
  auto source = readGltfJson(path);
  source["nodes"][0]["children"] = {1};
  source["nodes"].push_back({{"name", "Shared child"}});
  source["scenes"] = {
      {{"name", "Repeated name"}, {"nodes", {0}}},
      {{"name", "Repeated name"}, {"nodes", {0}}},
      {{"name", "Empty"}},
      {{"nodes", nlohmann::json::array()}}};
  source["scene"] = 1;
  writeGltfJson(path, source);
  const auto loaded = container::geometry::gltf::LoadModelWithSource(path.string());
  EXPECT_EQ(loaded.model.vertices().size(), 3u);
  ASSERT_EQ(loaded.gltfModel.scenes.size(), 4u);
  EXPECT_EQ(loaded.gltfModel.defaultScene, 1);
  EXPECT_EQ(loaded.gltfModel.scenes[0].nodes, loaded.gltfModel.scenes[1].nodes);
  EXPECT_EQ(loaded.gltfModel.scenes[0].name, loaded.gltfModel.scenes[1].name);
  EXPECT_TRUE(loaded.gltfModel.scenes[2].nodes.empty());
  EXPECT_TRUE(loaded.gltfModel.scenes[3].nodes.empty());

  source["scene"] = 2;
  writeGltfJson(path, source);
  EXPECT_EQ(container::geometry::gltf::LoadSourceModel(path.string()).defaultScene, 2);
  source.erase("scene");
  writeGltfJson(path, source);
  EXPECT_EQ(container::geometry::gltf::LoadSourceModel(path.string()).defaultScene, -1);
  source.erase("scenes");
  writeGltfJson(path, source);
  const auto implicit = container::geometry::gltf::LoadSourceModel(path.string());
  EXPECT_EQ(implicit.defaultScene, -1);
  EXPECT_TRUE(implicit.scenes.empty());
  EXPECT_EQ(implicit.nodes[0].children, std::vector<int>{1});
}

TEST(GltfModelLoader, SceneMetadataRejectsNarrowingAndNonIntegerIndicesBeforeResourceIo) {
  const auto path = writeTriangleGltf("scene-metadata-indices", {0, 1, 2});
  const auto valid = readGltfJson(path);
  const std::vector<nlohmann::json> invalid = {
      -1, 1, 4294967296ull, 4294967297ull, UINT64_MAX,
      0.0, 0.5, "0", nullptr, true, nlohmann::json::object(),
      nlohmann::json::array()};
  for (const auto& index : invalid) {
    SCOPED_TRACE(index.dump());
    auto source = valid;
    source["scene"] = index;
    expectSceneRejectionBeforeBufferIo(path, source, "scene");
    source = valid;
    source["scenes"][0]["nodes"] = nlohmann::json::array({index});
    expectSceneRejectionBeforeBufferIo(path, source, "scenes[0].nodes");
    source = valid;
    source["nodes"][0]["children"] = nlohmann::json::array({index});
    expectSceneRejectionBeforeBufferIo(path, source, "nodes[0].children");
  }
  auto source = valid;
  source.erase("scenes");
  expectSceneRejectionBeforeBufferIo(path, source, "scene");
}

TEST(GltfModelLoader, SceneMetadataValidatesGlobalForestIncludingUnusedComponents) {
  const auto path = writeTriangleGltf("scene-metadata-forest", {0, 1, 2});
  auto valid = readGltfJson(path);
  valid["nodes"].push_back(nlohmann::json::object());
  valid["nodes"].push_back(nlohmann::json::object());
  auto source = valid;
  source["nodes"][0]["children"] = {1, 1};
  expectSceneRejectionBeforeBufferIo(path, source, "repeats a node");
  source = valid;
  source["nodes"][0]["children"] = {2};
  source["nodes"][1]["children"] = {2};
  expectSceneRejectionBeforeBufferIo(path, source, "multiple parents");
  source = valid;
  source["nodes"][1]["children"] = {2};
  source["nodes"][2]["children"] = {1};
  expectSceneRejectionBeforeBufferIo(path, source, "cycle");
  source = valid;
  source["nodes"][1]["children"] = {1};
  expectSceneRejectionBeforeBufferIo(path, source, "cycle");
  source = valid;
  source["nodes"][1]["children"] = {0};
  expectSceneRejectionBeforeBufferIo(path, source, "root is also a node child");
  source = valid;
  source["scenes"][0]["nodes"] = {0, 0};
  expectSceneRejectionBeforeBufferIo(path, source, "repeats a scene root");
}

TEST(GltfModelLoader, SceneMetadataRejectsInvalidTableAndReferenceTypesBeforeResourceIo) {
  const auto path = writeTriangleGltf("scene-metadata-types", {0, 1, 2});
  const auto valid = readGltfJson(path);
  for (const auto& value : std::vector<nlohmann::json>{nullptr, 0, "bad", true,
                                                     nlohmann::json::object()}) {
    SCOPED_TRACE(value.dump());
    auto source = valid;
    source["nodes"] = value;
    expectSceneRejectionBeforeBufferIo(path, source, "nodes must be an array");
    source = valid;
    source["scenes"] = value;
    expectSceneRejectionBeforeBufferIo(path, source, "scenes must be an array");
    source = valid;
    source["nodes"][0]["children"] = value;
    expectSceneRejectionBeforeBufferIo(path, source, "children must be an array");
    source = valid;
    source["scenes"][0]["nodes"] = value;
    expectSceneRejectionBeforeBufferIo(path, source, ".nodes must be an array");
  }
  auto source = valid;
  source["nodes"][0] = nullptr;
  expectSceneRejectionBeforeBufferIo(path, source, "nodes[0] must be an object");
  source = valid;
  source["scenes"][0] = nullptr;
  expectSceneRejectionBeforeBufferIo(path, source, "scenes[0] must be an object");
}

TEST(GltfModelLoader, SceneMetadataBoundsTablesAndRootReferencesBeforeResourceIo) {
  const auto path = writeTriangleGltf("scene-metadata-limits", {0, 1, 2});
  const auto valid = readGltfJson(path);
  auto source = valid;
  source["scenes"] = std::vector<nlohmann::json>(65537, nlohmann::json::object());
  expectSceneRejectionBeforeBufferIo(path, std::move(source), "scene table exceeds");
  source = valid;
  source["nodes"] = std::vector<nlohmann::json>(1'000'001, nlohmann::json::object());
  expectSceneRejectionBeforeBufferIo(path, std::move(source), "node table exceeds");
  source = valid;
  source["scenes"][0]["nodes"] = std::vector<uint32_t>(1'000'001, 0);
  expectSceneRejectionBeforeBufferIo(path, std::move(source), "aggregate reference limit");
}

TEST(GltfModelLoader, SceneMetadataAcceptsDeepImplicitForestWithoutRecursiveTraversal) {
  const auto path = writeTriangleGltf("scene-metadata-deep", {0, 1, 2});
  auto source = readGltfJson(path);
  constexpr size_t depth = 10000;
  source["nodes"] = std::vector<nlohmann::json>(depth, nlohmann::json::object());
  for (size_t i = 0; i + 1 < depth; ++i) source["nodes"][i]["children"] = {i + 1};
  source["nodes"][depth - 1]["mesh"] = 0;
  source.erase("scenes");
  source.erase("scene");
  writeGltfJson(path, source);
  const auto loaded = container::geometry::gltf::LoadSourceModel(path.string());
  ASSERT_EQ(loaded.nodes.size(), depth);
  EXPECT_EQ(loaded.nodes.front().children, std::vector<int>{1});
  EXPECT_EQ(loaded.nodes.back().mesh, 0);
}

TEST(GltfModelLoader, AuthoredNodeFlagsSurviveRequiredExtensionLoadingWithoutFlattening) {
  namespace gltf = container::geometry::gltf;
  const auto path = writeTriangleGltf("node-flags-valid", {0, 1, 2});
  auto source = readGltfJson(path);
  source["extensionsUsed"] = source["extensionsRequired"] = {
      "KHR_node_visibility", "KHR_node_selectability", "KHR_node_hoverability"};
  source["nodes"][0]["extensions"] = {
      {"KHR_node_visibility", {{"visible", false}, {"extras", {{"marker", "retained"}}}}},
      {"KHR_node_selectability", {{"selectable", true}}},
      {"KHR_node_hoverability", {{"hoverable", false}}}};
  source["nodes"][0]["children"] = {1};
  source["nodes"].push_back({{"extensions", {
      {"KHR_node_visibility", {{"visible", true}}},
      {"KHR_node_selectability", nlohmann::json::object()}}}});
  writeGltfJson(path, source);
  const auto loaded = gltf::LoadModelWithSource(path.string());
  EXPECT_EQ(loaded.model.vertices().size(), 3u);
  const auto flags = gltf::ParseGltfNodeFlags(loaded.gltfModel);
  ASSERT_EQ(flags.nodes.size(), 2u);
  EXPECT_FALSE(flags.nodes[0].visible);
  EXPECT_TRUE(flags.nodes[0].selectable);
  EXPECT_FALSE(flags.nodes[0].hoverable);
  EXPECT_TRUE(flags.nodes[1].visible);
  EXPECT_TRUE(flags.nodes[1].selectable);
  EXPECT_TRUE(flags.nodes[1].hasExtension(gltf::GltfNodeFlag::Selectability));
  EXPECT_FALSE(flags.nodes[1].hasProperty(gltf::GltfNodeFlag::Selectability));
  EXPECT_EQ(loaded.gltfModel.nodes[0].extensions.at("KHR_node_visibility")
      .Get("extras").Get("marker").Get<std::string>(), "retained");
  const auto sourceOnly = gltf::LoadSourceModel(path.string(), {false, 65536});
  EXPECT_EQ(gltf::ParseGltfNodeFlags(sourceOnly).nodes, flags.nodes);
  EXPECT_EQ(gltf::LoadModelFromFile(path.string()).vertices().size(), 3u);
  EXPECT_EQ(container::geometry::Model::LoadFromGltf(path.string()).vertices().size(), 3u);
}

TEST(GltfModelLoader, NodeFlagSchemaRejectsBeforeBufferAndImageIoAtEveryEntryPoint) {
  namespace gltf = container::geometry::gltf;
  const auto path = writeTriangleGltf("node-flags-before-io", {0, 1, 2});
  auto valid = readGltfJson(path);
  valid["buffers"][0]["uri"] = "node-flags-must-not-be-opened.bin";
  valid["images"] = {{{"uri", "node-flags-must-not-be-decoded.png"}}};
  const std::array extensions{
      "KHR_node_visibility", "KHR_node_selectability", "KHR_node_hoverability"};
  const std::array properties{"visible", "selectable", "hoverable"};
  valid["extensionsUsed"] = valid["extensionsRequired"] = extensions;
  for (size_t i = 0; i < extensions.size(); ++i) {
    for (const auto& malformed : std::vector<nlohmann::json>{
             nullptr, 0, 1, 1.0, "true", nlohmann::json::array(), nlohmann::json::object()}) {
      SCOPED_TRACE(std::string(extensions[i]) + " " + malformed.dump());
      auto source = valid;
      source["nodes"][0]["extensions"][extensions[i]][properties[i]] = malformed;
      writeGltfJson(path, source);
      const auto check = [&](auto load) {
        try {
          load();
          FAIL() << "Expected node flag metadata rejection";
        } catch (const std::runtime_error& error) {
          const std::string message(error.what());
          EXPECT_NE(message.find("glTF node flags node[0].extensions." +
              std::string(extensions[i]) + "." + properties[i]), std::string::npos) << message;
          EXPECT_EQ(message.find("node-flags-must-not-be-opened.bin"), std::string::npos);
          EXPECT_EQ(message.find("node-flags-must-not-be-decoded.png"), std::string::npos);
        }
      };
      check([&] { (void)gltf::LoadModelWithSource(path.string()); });
      check([&] { (void)gltf::LoadModelWithSource(path.string(), {false, 65536}); });
      check([&] { (void)gltf::LoadSourceModel(path.string()); });
      check([&] { (void)gltf::LoadModelFromFile(path.string()); });
      check([&] { (void)container::geometry::Model::LoadFromGltf(path.string()); });
    }
  }
}

TEST(GltfModelLoader, NodeFlagStorageCountsInactiveNodesAgainstAggregateInputBudget) {
  namespace gltf = container::geometry::gltf;
  const auto path = writeTriangleGltf("node-flags-input-budget", {0, 1, 2});
  auto source = readGltfJson(path);
  source["nodes"] = std::vector<nlohmann::json>(1000, nlohmann::json::object());
  source["nodes"][0]["mesh"] = 0;
  writeGltfJson(path, source);
  const auto sourceBytes = std::filesystem::file_size(path);
  try {
    // This covers the source, triangle buffers/conversion and a small margin,
    // but cannot also retain local flags for all 1,000 authored source nodes.
    (void)gltf::LoadSourceModel(path.string(), {false, sourceBytes + 1024});
    FAIL() << "Expected aggregate local-flag storage rejection";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("input-memory budget"), std::string::npos);
  }
  EXPECT_EQ(gltf::LoadSourceModel(path.string(), {false, sourceBytes + 10000}).nodes.size(), 1000u);
}

TEST(GltfModelLoader, ExtendedMaterialSchemaFailsBeforeExternalResourceIo) {
  namespace gltf = container::geometry::gltf;
  const auto path = writeTriangleGltf("extended-materials-before-io", {0, 1, 2});
  auto valid = readGltfJson(path);
  valid["buffers"][0]["uri"] = "extended-materials-must-not-be-opened.bin";
  valid["images"] = {{{"uri", "extended-materials-must-not-be-decoded.png"}}};
  const std::array extensions{"KHR_materials_anisotropy", "KHR_materials_diffuse_transmission"};
  const std::array properties{"anisotropyStrength", "diffuseTransmissionFactor"};
  valid["extensionsUsed"] = extensions;
  for (size_t i = 0; i < extensions.size(); ++i) {
    for (const auto& malformed : std::vector<nlohmann::json>{nullptr, -1, 1.1, true, "0", nlohmann::json::array()}) {
      auto source = valid;
      source["materials"][0]["extensions"][extensions[i]][properties[i]] = malformed;
      writeGltfJson(path, source);
      const auto check = [&](auto load) {
        try { load(); FAIL() << "Expected advanced material validation failure"; }
        catch (const std::runtime_error& error) {
          const std::string message(error.what());
          EXPECT_NE(message.find("glTF extended materials"), std::string::npos) << message;
          EXPECT_NE(message.find(properties[i]), std::string::npos) << message;
          EXPECT_EQ(message.find("must-not-be-opened.bin"), std::string::npos);
          EXPECT_EQ(message.find("must-not-be-decoded.png"), std::string::npos);
        }
      };
      check([&] { (void)gltf::LoadModelWithSource(path.string()); });
      check([&] { (void)gltf::LoadModelWithSource(path.string(), {false, 65536}); });
      check([&] { (void)gltf::LoadSourceModel(path.string()); });
      check([&] { (void)gltf::LoadModelFromFile(path.string()); });
      check([&] { (void)container::geometry::Model::LoadFromGltf(path.string()); });
    }
  }
}

TEST(GltfModelLoader, ExtendedMaterialStorageIncludesInactiveMaterialsInInputBudget) {
  namespace gltf = container::geometry::gltf;
  const auto path = writeTriangleGltf("extended-materials-input-budget", {0, 1, 2});
  auto source = readGltfJson(path);
  source["materials"] = std::vector<nlohmann::json>(1000, nlohmann::json::object());
  writeGltfJson(path, source);
  const auto bytes = std::filesystem::file_size(path);
  try {
    (void)gltf::LoadSourceModel(path.string(), {false, bytes + 1024});
    FAIL() << "Expected aggregate local-material storage rejection";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("input-memory budget"), std::string::npos);
  }
  EXPECT_EQ(gltf::LoadSourceModel(path.string(), {false, bytes + 1024 * 1024}).materials.size(), 1000u);
}

namespace {
constexpr std::string_view kAsymmetricIes =
    "IESNA:LM-63-2002\r\n[TEST] Fixture\r\nTILT=NONE\r\n"
    "1 -1 2 3 2 1 2 0 0 0\r\n0.5 1 12\r\n"
    "0 40 90\r\n0 90\r\n10 30 50\r\n20 50 90\r\n";
}

TEST(GltfIesProfiles, RetainsCandelaAndAsymmetryAcrossMeasuredAngles) {
  namespace gltf = container::geometry::gltf;
  const auto profile = gltf::ParseIesProfile(kAsymmetricIes);
  EXPECT_EQ(profile.symmetry, gltf::IesHorizontalSymmetry::Quadrant);
  EXPECT_FLOAT_EQ(profile.sample(20.0f, 45.0f), 27.5f);
  EXPECT_FLOAT_EQ(profile.sample(40.0f, -90.0f), 50.0f);
  EXPECT_FLOAT_EQ(profile.sample(90.0f, 180.0f), 50.0f);
  EXPECT_FLOAT_EQ(profile.sample(90.1f, 0.0f), 0.0f);
  EXPECT_EQ(profile.packedForGpu(), (std::vector<float>{3, 2, 1, 0, 0, 40, 90, 0, 90, 10, 30, 50, 20, 50, 90}));
}

TEST(GltfIesProfiles, RejectsUnsupportedTiltTypeAndInvalidTables) {
  namespace gltf = container::geometry::gltf;
  const auto edited = [](std::string_view before, std::string_view after) {
    std::string text(kAsymmetricIes);
    text.replace(text.find(before), before.size(), after);
    return text;
  };
  EXPECT_THROW(static_cast<void>(gltf::ParseIesProfile(edited("TILT=NONE", "TILT=fixture.ies"))), std::invalid_argument);
  EXPECT_THROW(static_cast<void>(gltf::ParseIesProfile(edited("3 2 1 2", "3 2 2 2"))), std::invalid_argument);
  EXPECT_THROW(static_cast<void>(gltf::ParseIesProfile(edited("0 40 90", "0 40 40"))), std::invalid_argument);
  EXPECT_THROW(static_cast<void>(gltf::ParseIesProfile(edited("10 30 50", "10 30 nan"))), std::invalid_argument);
  EXPECT_THROW(static_cast<void>(gltf::ParseIesProfile(edited("20 50 90", "20 50"))), std::invalid_argument);
}

TEST(GltfIesProfiles, EmbedsValidResourcesAndKeepsOptionalPunctualFallback) {
  namespace gltf = container::geometry::gltf;
  const auto path = writeTriangleGltf("ies-profile", {0, 1, 2});
  std::ofstream(path.parent_path() / "luminaire sample.ies", std::ios::binary) << kAsymmetricIes;
  auto source = readGltfJson(path);
  source["extensionsUsed"] = {gltf::kGltfIesExtension, "KHR_lights_punctual"};
  source["extensions"][gltf::kGltfIesExtension]["lights"] = {
      {{"uri", "missing-profile.ies"}}, {{"uri", "luminaire%20sample.ies"}}};
  source["extensions"]["KHR_lights_punctual"]["lights"] = {{{"type", "point"}}};
  source["nodes"][0]["extensions"] = {{gltf::kGltfIesExtension, {{"light", 1}, {"multiplier", 2}}}};
  source["nodes"].push_back({{"extensions", {{gltf::kGltfIesExtension, {{"light", 0}}},
      {"KHR_lights_punctual", {{"light", 0}}}}}});
  source["scenes"][0]["nodes"].push_back(1);
  writeGltfJson(path, source);

  const auto loaded = gltf::LoadModelWithSource(path.string());
  const auto& model = loaded.gltfModel;
  const auto* uri = gltf::GltfIesEmbeddedProfileUri(model, 0);
  ASSERT_NE(uri, nullptr);
  EXPECT_EQ(gltf::DecodeGltfIesProfileUri(*uri), kAsymmetricIes);
  EXPECT_EQ(gltf::ParseGltfIesBinding(model.nodes[0].extensions.at(gltf::kGltfIesExtension), 1).multiplier, 2.0f);
  EXPECT_FALSE(model.nodes[1].extensions.contains(gltf::kGltfIesExtension));
  EXPECT_EQ(model.nodes[1].light, 0);
  EXPECT_TRUE(std::ranges::any_of(loaded.capabilityDiagnostics,
      [](const auto& diagnostic) { return diagnostic.code == "unsupported_ies_profile"; }));

  source["extensionsRequired"] = {gltf::kGltfIesExtension};
  writeGltfJson(path, source);
  EXPECT_THROW(gltf::LoadSourceModel(path.string()), std::runtime_error);
}

TEST(GltfIesProfiles, ExternalProfileReadConsumesTheLoaderInputBudget) {
  namespace gltf = container::geometry::gltf;
  const auto path = writeTriangleGltf("ies-profile-budget", {0, 1, 2});
  std::string profile(kAsymmetricIes);
  profile.insert(profile.find("TILT=NONE"), "[MORE] " + std::string(1024 * 1024, 'a') + "\r\n");
  std::ofstream(path.parent_path() / "ies-profile-budget.ies", std::ios::binary) << profile;
  auto source = readGltfJson(path);
  source["extensionsUsed"] = {gltf::kGltfIesExtension};
  source["extensions"][gltf::kGltfIesExtension]["lights"] = {{{"uri", "ies-profile-budget.ies"}}};
  source["nodes"][0]["extensions"] = {{gltf::kGltfIesExtension, {{"light", 0}}}};
  writeGltfJson(path, source);

  gltf::LoadOptions options;
  options.maxInputBytes = 512 * 1024;
  const auto loaded = gltf::LoadModelWithSource(path.string(), options);
  EXPECT_FALSE(loaded.gltfModel.nodes[0].extensions.contains(gltf::kGltfIesExtension));
  EXPECT_TRUE(std::ranges::any_of(loaded.capabilityDiagnostics, [](const auto& diagnostic) {
    return diagnostic.code == "unsupported_ies_profile" && diagnostic.message.contains("remaining glTF input byte budget");
  }));
  options.maxInputBytes = 2 * 1024 * 1024;
  const auto permitted = gltf::LoadModelWithSource(path.string(), options);
  EXPECT_TRUE(permitted.gltfModel.nodes[0].extensions.contains(gltf::kGltfIesExtension));
}
