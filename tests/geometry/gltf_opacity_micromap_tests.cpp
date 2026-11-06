#include <Container/geometry/GltfOpacityMicromaps.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <initializer_list>
#include <string>
#include <utility>

namespace {

namespace gltf = container::geometry::gltf;
using Value = tinygltf::Value;
using Object = Value::Object;
using Usage = gltf::GltfOpacityMicromapUsage;
const std::string extension(gltf::kGltfOpacityMicromapExtension);

Value integerArray(std::initializer_list<int> values) {
  Value::Array result;
  for (const int value : values) result.emplace_back(value);
  return Value(std::move(result));
}

struct Fixture {
  tinygltf::Model source;
  size_t stride;

  explicit Fixture(size_t triangleStride = 8) : stride(triangleStride) {
    source.buffers.resize(1);
    const auto data = view(3);
    source.buffers[0].data[data] = 0;
    source.buffers[0].data[data + 1] = 0xe4;
    source.buffers[0].data[data + 2] = 1;
    (void)view(stride * 3, stride == 8 ? 0 : stride);
    triangle(0, 0, 0, 1);
    triangle(1, 1, 1, 2);
    triangle(2, 2, 0, 1);
    source.extensionsUsed = {extension};
    source.extensions[extension] = Value(Object{{"micromaps", Value(Value::Array{Value(Object{
        {"data", Value(0)}, {"triangles", Value(1)}, {"usageCounts", integerArray({1, 1, 1})},
        {"usageLevels", integerArray({0, 1, 0})}, {"usageFormats", integerArray({1, 2, 1})}
    })})}});
    source.meshes.resize(2);
  }

  size_t view(size_t size, size_t byteStride = 0) {
    auto& bytes = source.buffers[0].data;
    const size_t offset = (bytes.size() + 3) & ~size_t(3);
    bytes.resize(offset + size);
    tinygltf::BufferView buffer;
    buffer.buffer = 0;
    buffer.byteOffset = offset;
    buffer.byteLength = size;
    buffer.byteStride = byteStride;
    source.bufferViews.push_back(buffer);
    return offset;
  }

  void word(size_t offset, uint32_t value, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i)
      source.buffers[0].data[offset + i] = static_cast<unsigned char>(value >> (8 * i));
  }

  void triangle(size_t index, uint32_t offset, uint16_t level, uint16_t format) {
    const auto record = source.bufferViews[1].byteOffset + index * stride;
    word(record, offset, 4);
    word(record + 4, level, 2);
    word(record + 6, format, 2);
  }

  int accessor(int type, int component, size_t count, int viewIndex = -1) {
    tinygltf::Accessor value;
    value.type = type;
    value.componentType = component;
    value.count = count;
    value.bufferView = viewIndex;
    source.accessors.push_back(value);
    return int(source.accessors.size() - 1);
  }

  int lookup(std::initializer_list<uint32_t> values, int component, size_t width, size_t lookupStride = 0) {
    const size_t step = lookupStride ? lookupStride : width;
    size_t offset = view(step * values.size(), lookupStride);
    for (const auto value : values) {
      word(offset, value, width);
      offset += step;
    }
    return accessor(TINYGLTF_TYPE_SCALAR, component, values.size(), int(source.bufferViews.size() - 1));
  }

  tinygltf::Primitive& primitive(size_t count, int base = 0, int lookupAccessor = -1, int map = 0) {
    auto& value = source.meshes[0].primitives.emplace_back();
    value.mode = TINYGLTF_MODE_TRIANGLES;
    value.attributes["POSITION"] = accessor(TINYGLTF_TYPE_VEC3, TINYGLTF_COMPONENT_TYPE_FLOAT, count * 3);
    Object binding{{"micromap", Value(map)}, {"micromapBaseTriangle", Value(base)}};
    if (lookupAccessor >= 0) binding["micromapIndices"] = Value(lookupAccessor);
    value.extensions[extension] = Value(std::move(binding));
    return value;
  }

  Value::Array& maps() {
    return source.extensions.at(extension).Get<Object>().at("micromaps").Get<Value::Array>();
  }
};

void expectFallback(const Fixture& fixture, std::string_view detail) {
  const auto parsed = gltf::ParseGltfOpacityMicromaps(fixture.source);
  EXPECT_FALSE(parsed.find(0, 0));
  ASSERT_FALSE(parsed.diagnostics.empty());
  EXPECT_NE(parsed.diagnostics.front().message.find(detail), std::string::npos)
      << parsed.diagnostics.front().context << " " << parsed.diagnostics.front().message;
}

TEST(GltfOpacityMicromaps, SharesValidatedMapsAndNormalizesAbsoluteLookupIndices) {
  Fixture fixture(12);
  const auto lookup = fixture.lookup({0, 0, 0, 1, 0xffff, 0xfffe, 0xfffd, 0xfffc},
      TINYGLTF_COMPONENT_TYPE_SHORT, 2, 4);
  fixture.primitive(8, 1, lookup);
  fixture.primitive(2, 1);
  const auto parsed = gltf::ParseGltfOpacityMicromaps(fixture.source);
  ASSERT_TRUE(parsed.diagnostics.empty());
  const auto first = parsed.find(0, 0);
  const auto second = parsed.find(0, 1);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(first->map, second->map);
  EXPECT_EQ(first->indices, (std::vector<uint32_t>{1, 1, 1, 2, UINT32_MAX, UINT32_MAX-1, UINT32_MAX-2, UINT32_MAX-3}));
  EXPECT_EQ(first->usage, (std::vector<Usage>{{1, 0, 1}, {3, 1, 2}}));
  EXPECT_EQ(second->indices, (std::vector<uint32_t>{1, 2}));
  EXPECT_EQ(second->usage, (std::vector<Usage>{{1, 0, 1}, {1, 1, 2}}));
  EXPECT_EQ(first->map->usage, (std::vector<Usage>{{2, 0, 1}, {1, 1, 2}}));
  ASSERT_EQ(first->map->triangles.size(), 3u);
  EXPECT_EQ(first->map->triangles[1].dataOffset, 1u);
  EXPECT_EQ(first->map->triangles[1].subdivisionLevel, 1u);
  EXPECT_EQ(first->map->triangles[1].format, 2u);
  EXPECT_EQ(first->map->data, (std::vector<std::byte>{std::byte(0), std::byte(0xe4), std::byte(1)}));
  EXPECT_FALSE(parsed.find(1, 0));
  EXPECT_FALSE(parsed.find(0, 99));
}

TEST(GltfOpacityMicromaps, AllSpecialIndicesIgnoreTheBaseAndHaveNoGeometryUsage) {
  Fixture fixture;
  const auto lookup = fixture.lookup({0xff, 0xfe, 0xfd, 0xfc}, TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE, 1);
  fixture.primitive(4, INT32_MAX, lookup);
  const auto parsed = gltf::ParseGltfOpacityMicromaps(fixture.source);
  const auto attachment = parsed.find(0, 0);
  ASSERT_TRUE(attachment);
  EXPECT_EQ(attachment->indices, (std::vector<uint32_t>{UINT32_MAX, UINT32_MAX-1, UINT32_MAX-2, UINT32_MAX-3}));
  EXPECT_TRUE(attachment->usage.empty());
}

TEST(GltfOpacityMicromaps, RejectsMismatchedHistogramOverlapsAndTruncatedBufferViews) {
  Fixture histogram;
  histogram.primitive(1);
  histogram.maps()[0].Get<Object>()["usageLevels"] = integerArray({0, 0, 0});
  expectFallback(histogram, "Usage histogram");

  Fixture overlap;
  overlap.primitive(1);
  overlap.triangle(2, 1, 0, 1);
  expectFallback(overlap, "overlap");

  Fixture extent;
  extent.primitive(1);
  extent.triangle(1, UINT32_MAX, 1, 2);
  expectFallback(extent, "packed opacity data");

  Fixture malformed;
  malformed.primitive(1);
  malformed.source.bufferViews[1].byteOffset = SIZE_MAX;
  expectFallback(malformed, "exceeds its buffer");
}

TEST(GltfOpacityMicromaps, RejectsInvalidPrimitiveLinkageWithoutRejectingOtherAttachments) {
  Fixture fixture;
  fixture.primitive(1, 3);
  const auto badIndex = fixture.lookup({0xfb}, TINYGLTF_COMPONENT_TYPE_BYTE, 1);
  fixture.primitive(1, 0, badIndex);
  fixture.primitive(2);
  const auto parsed = gltf::ParseGltfOpacityMicromaps(fixture.source);
  EXPECT_FALSE(parsed.find(0, 0));
  EXPECT_FALSE(parsed.find(0, 1));
  EXPECT_TRUE(parsed.find(0, 2));
  ASSERT_EQ(parsed.diagnostics.size(), 2u);
  EXPECT_NE(parsed.diagnostics[0].context.find("meshes[0].primitives[0]"), std::string::npos);
  EXPECT_NE(parsed.diagnostics[0].message.find("baseTriangle"), std::string::npos);
  EXPECT_NE(parsed.diagnostics[1].message.find("negative value"), std::string::npos);
}

TEST(GltfOpacityMicromaps, RequiresTriangleModeAndExactResidentIndexAccessor) {
  Fixture mode;
  mode.primitive(1).mode = TINYGLTF_MODE_TRIANGLE_STRIP;
  expectFallback(mode, "Only TRIANGLES");

  Fixture normalized;
  const auto index = normalized.lookup({0}, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, 4);
  normalized.primitive(1, 0, index);
  normalized.source.accessors[size_t(index)].normalized = true;
  expectFallback(normalized, "non-normalized SCALAR");
  normalized.source.accessors[size_t(index)].normalized = false;
  normalized.source.accessors[size_t(index)].sparse.isSparse = true;
  expectFallback(normalized, "non-sparse");
  normalized.source.accessors[size_t(index)].sparse.isSparse = false;
  normalized.source.bufferViews[2].byteLength = 3;
  expectFallback(normalized, "exceeds its bufferView");
}

TEST(GltfOpacityMicromaps, BoundsRetainedLookupsAndDiagnostics) {
  Fixture fixture;
  fixture.primitive(2);
  fixture.primitive(1);
  fixture.primitive(1);
  gltf::GltfOpacityMicromapLimits limits;
  limits.maxGeometryTriangles = 2;
  limits.maxDiagnostics = 1;
  const auto parsed = gltf::ParseGltfOpacityMicromaps(fixture.source, limits);
  EXPECT_TRUE(parsed.find(0, 0));
  EXPECT_FALSE(parsed.find(0, 1));
  EXPECT_FALSE(parsed.find(0, 2));
  ASSERT_EQ(parsed.diagnostics.size(), 1u);
  EXPECT_NE(parsed.diagnostics[0].message.find("budget"), std::string::npos);
  EXPECT_EQ(parsed.diagnosticsOmitted, 1u);
}

TEST(GltfOpacityMicromaps, HandlesAbsentUndeclaredAndBrokenRootEntries) {
  const auto empty = gltf::ParseGltfOpacityMicromaps(tinygltf::Model{});
  EXPECT_TRUE(empty.primitives.empty());
  EXPECT_TRUE(empty.diagnostics.empty());

  Fixture undeclared;
  undeclared.primitive(1);
  undeclared.source.extensionsUsed.clear();
  expectFallback(undeclared, "extensionsUsed");

  Fixture malformed;
  malformed.maps().emplace_back(Object{{"data", Value(9)}});
  malformed.primitive(1, 0, -1, 1);
  malformed.primitive(1);
  const auto parsed = gltf::ParseGltfOpacityMicromaps(malformed.source);
  EXPECT_FALSE(parsed.find(0, 0));
  EXPECT_TRUE(parsed.find(0, 1));
  ASSERT_EQ(parsed.diagnostics.size(), 2u);
  EXPECT_NE(parsed.diagnostics[0].context.find("micromaps[1]"), std::string::npos);
  EXPECT_NE(parsed.diagnostics[1].message.find("unavailable root entry"), std::string::npos);
}

TEST(GltfOpacityMicromaps, RawManifestRejectsIntegerNarrowingAndIndexShifts) {
  const auto valid = nlohmann::json::parse(R"({
    "extensions":{"EXT_mesh_opacity_micromap":{"micromaps":[{
      "data":0,"triangles":1,"usageCounts":[1],"usageLevels":[0],"usageFormats":[1]
    }]}},
    "meshes":[{"primitives":[{"extensions":{"EXT_mesh_opacity_micromap":{"micromap":0}}}]}]
  })");
  EXPECT_FALSE(gltf::ValidateGltfOpacityMicromapSourceJson(valid));

  auto narrowed = valid;
  narrowed["extensions"][extension]["micromaps"][0]["data"] = uint64_t{1} << 32;
  auto diagnostic = gltf::ValidateGltfOpacityMicromapSourceJson(narrowed);
  ASSERT_TRUE(diagnostic);
  EXPECT_NE(diagnostic->context.find("micromaps[0]"), std::string::npos);
  EXPECT_NE(diagnostic->message.find("data"), std::string::npos);

  auto shifted = valid;
  shifted["extensions"][extension]["micromaps"][0]["usageLevels"].insert(
      shifted["extensions"][extension]["micromaps"][0]["usageLevels"].begin(), nullptr);
  diagnostic = gltf::ValidateGltfOpacityMicromapSourceJson(shifted);
  ASSERT_TRUE(diagnostic);
  EXPECT_NE(diagnostic->message.find("usageLevels"), std::string::npos);

  auto implicit = valid;
  implicit["meshes"][0]["primitives"][0]["extensions"][extension]["micromapIndices"] = nullptr;
  diagnostic = gltf::ValidateGltfOpacityMicromapSourceJson(implicit);
  ASSERT_TRUE(diagnostic);
  EXPECT_NE(diagnostic->context.find("primitives[0]"), std::string::npos);
  EXPECT_NE(diagnostic->message.find("micromapIndices"), std::string::npos);
}

} // namespace
