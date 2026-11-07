#include "Container/geometry/Model.h"

#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
template <typename T>
void append(std::vector<char>& data, T value) {
  const auto* begin = reinterpret_cast<const char*>(&value);
  data.insert(data.end(), begin, begin + sizeof(value));
}

std::filesystem::path writeColors(const std::string& name, int component,
                                  bool rgba) {
  const auto dir = std::filesystem::temp_directory_path() /
                   "container_gltf_vertex_alpha_tests";
  std::filesystem::create_directories(dir);
  const auto path = dir / (name + ".gltf");
  std::vector<char> data;
  for (float position : {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f})
    append(data, position);
  const auto colorStart = data.size();
  for (uint32_t corner = 0; corner < 3; ++corner) {
    const std::array<float, 4> values{1.f, 0.f, 1.f, corner * .5f};
    for (uint32_t channel = 0; channel < (rgba ? 4u : 3u); ++channel) {
      if (component == 5121)
        append(data, static_cast<uint8_t>(values[channel] * 255.f + .5f));
      else if (component == 5123)
        append(data, static_cast<uint16_t>(values[channel] * 65535.f + .5f));
      else
        append(data, values[channel]);
    }
  }
  const auto colorBytes = data.size() - colorStart;
  const auto indexStart = data.size();
  for (uint16_t index : {0, 1, 2}) append(data, index);
  std::ofstream bin(dir / (name + ".bin"), std::ios::binary);
  bin.write(data.data(), static_cast<std::streamsize>(data.size()));
  bin.close();
  std::ofstream json(path);
  json << "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\""
       << name << ".bin\",\"byteLength\":" << data.size()
       << "}],\"bufferViews\":[{\"buffer\":0,\"byteLength\":36},"
          "{\"buffer\":0,\"byteOffset\":" << colorStart
       << ",\"byteLength\":" << colorBytes
       << "},{\"buffer\":0,\"byteOffset\":" << indexStart
       << ",\"byteLength\":6}],\"accessors\":["
          "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
          "\"min\":[0,0,0],\"max\":[1,1,0]},"
          "{\"bufferView\":1,\"componentType\":" << component
       << ",\"count\":3,\"type\":\"" << (rgba ? "VEC4" : "VEC3")
       << "\",\"normalized\":" << (component == 5126 ? "false" : "true")
       << "},{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
          "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"COLOR_0\":1},"
          "\"indices\":2}]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
  return path;
}

void expectColors(const std::string& name, int component, bool rgba) {
  const auto model = container::geometry::Model::LoadFromGltf(
      writeColors(name, component, rgba).string());
  ASSERT_EQ(model.vertices().size(), 3u);
  for (size_t corner = 0; corner < 3; ++corner) {
    const auto& vertex = model.vertices()[corner];
    EXPECT_EQ(vertex.color, glm::vec3(1, 0, 1));
    const float middle = component == 5121 ? 128.f / 255.f :
                         component == 5123 ? 32768.f / 65535.f : .5f;
    const float expected = rgba ? (corner == 1 ? middle : float(corner / 2)) : 1.f;
    EXPECT_FLOAT_EQ(vertex.colorAlpha, expected) << corner;
  }
}
} // namespace

TEST(GltfVertexAlpha, FloatRgbaPreservesZeroHalfAndOne) {
  expectColors("float-rgba", 5126, true);
}
TEST(GltfVertexAlpha, NormalizedByteRgbaPreservesQuantizedAlpha) {
  expectColors("byte-rgba", 5121, true);
}
TEST(GltfVertexAlpha, NormalizedShortRgbaPreservesQuantizedAlpha) {
  expectColors("short-rgba", 5123, true);
}
TEST(GltfVertexAlpha, RgbDefaultsToOpaque) {
  expectColors("float-rgb", 5126, false);
}
TEST(GltfVertexAlpha, ProceduralMeshesDefaultToOpaque) {
  const auto model = container::geometry::Model::MakeCube();
  ASSERT_FALSE(model.vertices().empty());
  for (const auto& vertex : model.vertices()) EXPECT_FLOAT_EQ(vertex.colorAlpha, 1);
}
