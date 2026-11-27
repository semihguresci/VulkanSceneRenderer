#include "BimAssetCacheCodec.h"

#include "Container/geometry/DotBimLoader.h"
#include "Container/robotics/BimAssetCache.h"
#include "Container/utility/AtomicFile.h"

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>
#include <miniz/miniz.h>
#include <tiny_gltf.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_map>

namespace container::robotics {
namespace {

namespace bim = geometry::dotbim;
using material::AlphaMode;
using asset_cache::Reader;
using asset_cache::Writer;

constexpr uint32_t noMaterial = std::numeric_limits<uint32_t>::max();
constexpr size_t vertexBytes = 18 * sizeof(uint32_t);
constexpr size_t verticesPerBlock = (1024u * 1024u) / vertexBytes;
constexpr size_t maxRenderVertices = 16u * 1024u * 1024u;

void requireRender(bool condition, const char* message) {
  if (!condition) throw std::invalid_argument(std::string("BIM render asset: ") + message);
}

template<class Vector>
void writeVector(Writer& writer, const Vector& value) {
  for (glm::length_t i = 0; i < Vector::length(); ++i) writer.f32(value[i]);
}

template<class Vector>
Vector readVector(Reader& reader) {
  Vector value(0.0f);
  for (glm::length_t i = 0; i < Vector::length(); ++i) value[i] = reader.f32();
  return value;
}

template<class Vertex, class Visit>
void visitVertexFields(Vertex& vertex, Visit visit) {
  const auto vector = [&](auto& value) {
    for (glm::length_t i = 0; i < std::remove_cvref_t<decltype(value)>::length(); ++i)
      visit(value[i]);
  };
  vector(vertex.position);
  vector(vertex.color);
  vector(vertex.texCoord);
  vector(vertex.texCoord1);
  vector(vertex.normal);
  vector(vertex.tangent);
  visit(vertex.colorAlpha);
}

size_t vertexBlockCount(size_t count) {
  return (count + verticesPerBlock - 1) / verticesPerBlock;
}

void writeVertexBlocks(Writer& writer, std::span<const geometry::Vertex> vertices) {
  requireRender(vertices.size() <= maxRenderVertices, "vertex count exceeds the cache limit");
  writer.count(vertices.size());
  writer.count(vertexBlockCount(vertices.size()));
  if (vertices.empty()) return;

  std::string block;
  std::string compressed(mz_compressBound(verticesPerBlock * vertexBytes), '\0');
  for (size_t offset = 0; offset < vertices.size(); offset += verticesPerBlock) {
    const size_t count = std::min(verticesPerBlock, vertices.size() - offset);
    block.resize(count * vertexBytes);
    // Keep each attribute's bytes in separate lanes so repeated float bits
    // compress even when adjacent vertices have different positions.
    for (size_t i = 0; i < count; ++i) {
      size_t lane = 0;
      visitVertexFields(vertices[offset + i], [&](float value) {
        requireRender(std::isfinite(value), "vertex attribute is nonfinite");
        const auto bits = std::bit_cast<uint32_t>(value);
        for (unsigned shift = 0; shift < 32; shift += 8)
          block[lane++ * count + i] = static_cast<char>(static_cast<unsigned char>(bits >> shift));
      });
    }

    mz_ulong bytes = static_cast<mz_ulong>(compressed.size());
    const auto status = mz_compress2(reinterpret_cast<unsigned char*>(compressed.data()), &bytes,
                                     reinterpret_cast<const unsigned char*>(block.data()),
                                     static_cast<mz_ulong>(block.size()), MZ_BEST_SPEED);
    requireRender(status == MZ_OK, "unable to compress vertex block");
    // A block at the decoded length contains raw lanes; a shorter block is zlib.
    const std::string_view stored = bytes < block.size()
        ? std::string_view(compressed.data(), bytes) : std::string_view(block);
    writer.count(block.size());
    writer.count(stored.size());
    requireRender(stored.size() <= asset_cache::maxPayloadBytes - writer.data.size(), "vertex blocks exceed the cache payload limit");
    writer.data.append(stored);
  }
}

std::vector<geometry::Vertex> readVertexBlocks(Reader& reader) {
  const size_t total = reader.u32();
  requireRender(total <= maxRenderVertices, "vertex count exceeds the cache limit");
  const size_t blockCount = reader.count(2 * sizeof(uint32_t));
  requireRender(blockCount == vertexBlockCount(total), "incorrect vertex block count");

  // Check the complete block table and its wire lengths before reserving the
  // decoded vertex array, which can be much larger than the stored payload.
  std::vector<std::string_view> blocks;
  blocks.reserve(blockCount);
  for (size_t i = 0; i < blockCount; ++i) {
    const size_t count = std::min(verticesPerBlock, total - i * verticesPerBlock);
    const size_t decodedBytes = reader.u32();
    const size_t storedBytes = reader.u32();
    requireRender(decodedBytes == count * vertexBytes && storedBytes > 0 && storedBytes <= decodedBytes,
                  "invalid vertex block sizes");
    blocks.push_back(reader.view(storedBytes));
  }

  std::vector<geometry::Vertex> vertices;
  vertices.reserve(total);
  std::string decoded;
  for (const auto stored : blocks) {
    const size_t count = std::min(verticesPerBlock, total - vertices.size());
    std::string_view raw = stored;
    if (stored.size() < count * vertexBytes) {
      decoded.resize(count * vertexBytes);
      mz_ulong bytes = static_cast<mz_ulong>(decoded.size());
      const auto status = mz_uncompress(reinterpret_cast<unsigned char*>(decoded.data()), &bytes,
                                        reinterpret_cast<const unsigned char*>(stored.data()),
                                        static_cast<mz_ulong>(stored.size()));
      requireRender(status == MZ_OK && bytes == decoded.size(), "invalid compressed vertex block");
      raw = decoded;
    }
    for (size_t i = 0; i < count; ++i) {
      size_t lane = 0;
      visitVertexFields(vertices.emplace_back(), [&](float& value) {
        uint32_t bits = 0;
        for (unsigned shift = 0; shift < 32; shift += 8)
          bits |= uint32_t(static_cast<unsigned char>(raw[lane++ * count + i])) << shift;
        value = std::bit_cast<float>(bits);
        requireRender(std::isfinite(value), "vertex attribute is nonfinite");
      });
    }
  }
  return vertices;
}

bool readFlag(Reader& reader) {
  const auto flag = reader.u32();
  requireRender(flag <= 1, "invalid boolean field");
  return flag != 0;
}

template<class Range>
void writeRanges(Writer& writer, const std::vector<Range>& ranges) {
  writer.count(ranges.size());
  for (const auto& range : ranges) {
    writer.u32(range.meshId);
    writer.u32(range.firstIndex);
    writer.u32(range.indexCount);
    writeVector(writer, range.boundsCenter);
    writer.f32(range.boundsRadius);
  }
}

template<class Range>
std::vector<Range> readRanges(Reader& reader) {
  std::vector<Range> ranges(reader.count(7 * sizeof(uint32_t)));
  for (auto& range : ranges) {
    range.meshId = reader.u32();
    range.firstIndex = reader.u32();
    range.indexCount = reader.u32();
    range.boundsCenter = readVector<glm::vec3>(reader);
    range.boundsRadius = reader.f32();
  }
  return ranges;
}

struct DrawRange {
  uint32_t firstIndex;
  uint32_t indexCount;
  int mode;
};

using DrawRangeMap = std::unordered_map<uint32_t, DrawRange>;

template<class Range>
DrawRangeMap indexRanges(const std::vector<Range>& ranges, size_t indexCount, int mode, uint32_t width) {
  DrawRangeMap result;
  result.reserve(ranges.size());
  for (const auto& range : ranges) {
    requireRender(range.firstIndex <= indexCount && range.indexCount <= indexCount - range.firstIndex &&
                      range.indexCount % width == 0 && range.indexCount != 0,
                  "out-of-bounds or incomplete primitive range");
    requireRender(std::isfinite(range.boundsCenter.x) && std::isfinite(range.boundsCenter.y) &&
                      std::isfinite(range.boundsCenter.z) && std::isfinite(range.boundsRadius) && range.boundsRadius >= 0,
                  "invalid primitive bounds");
    requireRender(result.try_emplace(range.meshId, DrawRange{range.firstIndex, range.indexCount, mode}).second,
                  "duplicate primitive range ID");
  }
  return result;
}

struct DrawRanges {
  explicit DrawRanges(const bim::Model& model)
      : triangles(indexRanges(model.meshRanges, model.indices.size(), TINYGLTF_MODE_TRIANGLES, 3)),
        points(indexRanges(model.nativePointRanges, model.indices.size(), TINYGLTF_MODE_POINTS, 1)),
        lines(indexRanges(model.nativeCurveRanges, model.indices.size(), TINYGLTF_MODE_LINE, 2)) {}

  [[nodiscard]] const DrawRange& forElement(const bim::Element& element) const {
    if (element.geometryKind == bim::GeometryKind::Points) {
      if (const auto range = points.find(element.meshId); range != points.end()) return range->second;
    } else if (element.geometryKind == bim::GeometryKind::Curves && !element.hasAuthoredCurveWidth) {
      if (const auto range = lines.find(element.meshId); range != lines.end()) return range->second;
    }
    const auto triangle = triangles.find(element.meshId);
    requireRender(triangle != triangles.end(), "drawable references a missing primitive range");
    return triangle->second;
  }

  DrawRangeMap triangles;
  DrawRangeMap points;
  DrawRangeMap lines;
};

void validateDraws(const bim::Model& model, const DrawRanges& ranges) {
  for (const auto index : model.indices)
    requireRender(index < model.vertices.size(), "index references a missing vertex");
  for (const auto& element : model.elements) {
    requireRender(element.geometryKind == bim::GeometryKind::Mesh || element.geometryKind == bim::GeometryKind::Points ||
                      element.geometryKind == bim::GeometryKind::Curves, "invalid geometry kind");
    requireRender(element.materialIndex == noMaterial || element.materialIndex < model.materials.size(),
                  "drawable references a missing material");
    (void)ranges.forElement(element);
    for (glm::length_t column = 0; column < 4; ++column) {
      requireRender(element.transform[column][3] == (column == 3 ? 1.0f : 0.0f), "drawable transform is nonaffine");
      requireRender(std::isfinite(element.color[column]), "drawable color is nonfinite");
      for (glm::length_t row = 0; row < 4; ++row)
        requireRender(std::isfinite(element.transform[column][row]), "drawable transform is nonfinite");
    }
    const auto determinant = glm::determinant(glm::dmat3(element.transform));
    requireRender(std::isfinite(determinant) && determinant != 0.0, "drawable transform is singular");
  }
}

void writeMaterial(Writer& writer, const material::Material& pbr) {
  requireRender(static_cast<uint32_t>(pbr.alphaMode) <= static_cast<uint32_t>(AlphaMode::Blend), "invalid material alpha mode");
  writeVector(writer, pbr.baseColor);
  writeVector(writer, pbr.emissiveColor);
  writer.f32(pbr.emissiveStrength);
  writer.f32(pbr.metallicFactor);
  writer.f32(pbr.roughnessFactor);
  writer.f32(pbr.alphaCutoff);
  writer.f32(pbr.opacityFactor);
  writer.u32(static_cast<uint32_t>(pbr.alphaMode));
  writer.u32(pbr.doubleSided);
  writer.u32(pbr.unlit);
}

material::Material readMaterial(Reader& reader) {
  material::Material pbr;
  pbr.baseColor = readVector<glm::vec4>(reader);
  pbr.emissiveColor = readVector<glm::vec3>(reader);
  pbr.emissiveStrength = reader.f32();
  pbr.metallicFactor = reader.f32();
  pbr.roughnessFactor = reader.f32();
  pbr.alphaCutoff = reader.f32();
  pbr.opacityFactor = reader.f32();
  const auto alpha = reader.u32();
  requireRender(alpha <= static_cast<uint32_t>(AlphaMode::Blend), "invalid material alpha mode");
  pbr.alphaMode = static_cast<AlphaMode>(alpha);
  pbr.doubleSided = readFlag(reader);
  pbr.unlit = readFlag(reader);
  return pbr;
}

void writeElement(Writer& writer, const bim::Element& element) {
  writer.u32(element.meshId);
  writer.u32(static_cast<uint32_t>(element.geometryKind));
  writer.u32(element.materialIndex);
  for (glm::length_t column = 0; column < 4; ++column) writeVector(writer, element.transform[column]);
  writeVector(writer, element.color);
  writer.text(element.guid);
  writer.text(element.type);
  writer.text(element.displayName);
  writer.text(element.sourceId);
  writer.text(element.semanticSourceId);
  writer.u32(element.doubleSided);
  writer.u32(element.hasAuthoredCurveWidth);
  writer.u32(element.isRepresentationOnly);
}

bim::Element readElement(Reader& reader) {
  bim::Element element;
  element.meshId = reader.u32();
  const auto kind = reader.u32();
  requireRender(kind <= static_cast<uint32_t>(bim::GeometryKind::Curves), "invalid geometry kind");
  element.geometryKind = static_cast<bim::GeometryKind>(kind);
  element.materialIndex = reader.u32();
  for (glm::length_t column = 0; column < 4; ++column) element.transform[column] = readVector<glm::vec4>(reader);
  element.color = readVector<glm::vec4>(reader);
  element.guid = reader.text();
  element.type = reader.text();
  element.displayName = reader.text();
  element.sourceId = reader.text();
  element.semanticSourceId = reader.text();
  element.doubleSided = readFlag(reader);
  element.hasAuthoredCurveWidth = readFlag(reader);
  element.isRepresentationOnly = readFlag(reader);
  return element;
}

} // namespace

namespace asset_cache {

void writeRender(Writer& writer, const bim::Model& model) {
  validateDraws(model, DrawRanges(model));
  writeVertexBlocks(writer, model.vertices);
  writer.count(model.indices.size());
  for (const auto index : model.indices) writer.u32(index);
  writeRanges(writer, model.meshRanges);
  writeRanges(writer, model.nativePointRanges);
  writeRanges(writer, model.nativeCurveRanges);
  writer.count(model.materials.size());
  for (const auto& material : model.materials) writeMaterial(writer, material.pbr);
  writer.count(model.elements.size());
  for (const auto& element : model.elements) writeElement(writer, element);
}

bim::Model readRender(Reader& reader) {
  bim::Model model;
  model.vertices = readVertexBlocks(reader);
  model.indices.resize(reader.count(sizeof(uint32_t)));
  for (auto& index : model.indices) index = reader.u32();
  model.meshRanges = readRanges<bim::MeshRange>(reader);
  model.nativePointRanges = readRanges<bim::NativePrimitiveRange>(reader);
  model.nativeCurveRanges = readRanges<bim::NativePrimitiveRange>(reader);
  model.materials.resize(reader.count(15 * sizeof(uint32_t)));
  for (auto& material : model.materials) material.pbr = readMaterial(reader);
  model.elements.resize(reader.count(26 * sizeof(uint32_t)));
  for (auto& element : model.elements) element = readElement(reader);
  validateDraws(model, DrawRanges(model));
  return model;
}

} // namespace asset_cache

namespace {

int gltfIndex(size_t index) {
  requireRender(index <= static_cast<size_t>(std::numeric_limits<int>::max()), "preview exceeds glTF index limits");
  return static_cast<int>(index);
}

template<class Value>
int appendAccessor(tinygltf::Model& gltf, const std::vector<Value>& values,
                   int type, int componentType, int target, int components) {
  auto& bytes = gltf.buffers.front().data;
  constexpr size_t maxBuffer = std::numeric_limits<uint32_t>::max() - 32u;
  requireRender(values.size() <= (maxBuffer - bytes.size()) / 4, "preview exceeds GLB buffer limits");
  tinygltf::BufferView view;
  view.buffer = 0;
  view.byteOffset = bytes.size();
  view.byteLength = values.size() * 4;
  view.target = target;
  for (const auto value : values) {
    if constexpr (std::is_floating_point_v<Value>)
      requireRender(std::isfinite(value), "preview attribute is nonfinite");
    const auto bits = std::bit_cast<uint32_t>(value);
    for (unsigned shift = 0; shift < 32; shift += 8)
      bytes.push_back(static_cast<unsigned char>(bits >> shift));
  }
  tinygltf::Accessor accessor;
  accessor.bufferView = gltfIndex(gltf.bufferViews.size());
  accessor.count = values.size() / components;
  accessor.type = type;
  accessor.componentType = componentType;
  const auto index = gltfIndex(gltf.accessors.size());
  gltf.bufferViews.push_back(std::move(view));
  gltf.accessors.push_back(std::move(accessor));
  return index;
}

bool hasShear(const glm::dmat4& transform) {
  const auto x = glm::normalize(glm::dvec3(transform[0]));
  const auto y = glm::normalize(glm::dvec3(transform[1]));
  const auto z = glm::normalize(glm::dvec3(transform[2]));
  return std::abs(glm::dot(x, y)) > 1e-6 || std::abs(glm::dot(y, z)) > 1e-6 ||
         std::abs(glm::dot(x, z)) > 1e-6;
}

tinygltf::Primitive previewPrimitive(tinygltf::Model& gltf, const bim::Model& model,
                                    const DrawRange& range, const glm::dmat4& transform) {
  const bool triangles = range.mode == TINYGLTF_MODE_TRIANGLES;
  const auto linear = glm::dmat3(transform);
  const auto normalTransform = triangles ? glm::transpose(glm::inverse(linear)) : glm::dmat3(1.0);
  const bool reversed = triangles && glm::determinant(linear) < 0.0;
  std::unordered_map<uint32_t, uint32_t> localIndices;
  std::vector<uint32_t> indices;
  std::vector<float> positions, colors, normals;
  indices.reserve(range.indexCount);
  glm::dvec3 minimum(std::numeric_limits<double>::infinity());
  glm::dvec3 maximum(-std::numeric_limits<double>::infinity());
  for (size_t i = 0; i < range.indexCount; ++i) {
    const auto index = model.indices[size_t(range.firstIndex) + i];
    const auto [local, inserted] = localIndices.try_emplace(index, static_cast<uint32_t>(localIndices.size()));
    indices.push_back(local->second);
    if (!inserted) continue;
    const auto& vertex = model.vertices[index];
    const glm::vec3 position(glm::dvec3(transform * glm::dvec4(vertex.position, 1.0)));
    requireRender(std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z),
                  "preview position is not representable in glTF");
    minimum = glm::min(minimum, glm::dvec3(position));
    maximum = glm::max(maximum, glm::dvec3(position));
    positions.insert(positions.end(), {position.x, position.y, position.z});
    const auto color = glm::clamp(glm::vec4(vertex.color, vertex.colorAlpha), 0.0f, 1.0f);
    colors.insert(colors.end(), {color.r, color.g, color.b, color.a});
    if (triangles) {
      auto normal = normalTransform * glm::dvec3(vertex.normal);
      const auto length = glm::length(normal);
      normal = length > 0.0 && std::isfinite(length) ? normal / length : glm::dvec3(0.0, 1.0, 0.0);
      normals.insert(normals.end(), {static_cast<float>(normal.x), static_cast<float>(normal.y), static_cast<float>(normal.z)});
    }
  }
  if (reversed)
    for (size_t i = 0; i < indices.size(); i += 3) std::swap(indices[i + 1], indices[i + 2]);

  tinygltf::Primitive primitive;
  primitive.mode = range.mode;
  const auto position = appendAccessor(gltf, positions, TINYGLTF_TYPE_VEC3, TINYGLTF_COMPONENT_TYPE_FLOAT,
                                       TINYGLTF_TARGET_ARRAY_BUFFER, 3);
  gltf.accessors[position].minValues = {minimum.x, minimum.y, minimum.z};
  gltf.accessors[position].maxValues = {maximum.x, maximum.y, maximum.z};
  primitive.attributes.emplace("POSITION", position);
  primitive.attributes.emplace("COLOR_0", appendAccessor(gltf, colors, TINYGLTF_TYPE_VEC4,
      TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TARGET_ARRAY_BUFFER, 4));
  if (triangles)
    primitive.attributes.emplace("NORMAL", appendAccessor(gltf, normals, TINYGLTF_TYPE_VEC3,
        TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TARGET_ARRAY_BUFFER, 3));
  primitive.indices = appendAccessor(gltf, indices, TINYGLTF_TYPE_SCALAR, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,
                                     TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER, 1);
  return primitive;
}

tinygltf::Material previewMaterial(const material::Material& pbr, bool doubleSided, bool unlit) {
  tinygltf::Material result;
  for (const float value : {pbr.baseColor.r, pbr.baseColor.g, pbr.baseColor.b, pbr.baseColor.a,
                            pbr.opacityFactor, pbr.metallicFactor, pbr.roughnessFactor, pbr.alphaCutoff,
                            pbr.emissiveColor.r, pbr.emissiveColor.g, pbr.emissiveColor.b, pbr.emissiveStrength})
    requireRender(std::isfinite(value), "preview material is nonfinite");
  const auto color = glm::clamp(pbr.baseColor, 0.0f, 1.0f);
  result.pbrMetallicRoughness.baseColorFactor = {color.r, color.g, color.b, std::clamp(color.a * pbr.opacityFactor, 0.0f, 1.0f)};
  result.pbrMetallicRoughness.metallicFactor = std::clamp(pbr.metallicFactor, 0.0f, 1.0f);
  result.pbrMetallicRoughness.roughnessFactor = std::clamp(pbr.roughnessFactor, 0.0f, 1.0f);
  const auto emission = glm::clamp(pbr.emissiveColor * pbr.emissiveStrength, 0.0f, 1.0f);
  result.emissiveFactor = {emission.r, emission.g, emission.b};
  result.doubleSided = doubleSided || pbr.doubleSided;
  if (pbr.alphaMode == AlphaMode::Mask) {
    result.alphaMode = "MASK";
    result.alphaCutoff = std::clamp(pbr.alphaCutoff, 0.0f, 1.0f);
  } else if (pbr.alphaMode == AlphaMode::Blend) {
    result.alphaMode = "BLEND";
  }
  if (unlit || pbr.unlit)
    result.extensions.emplace("KHR_materials_unlit", tinygltf::Value(tinygltf::Value::Object{}));
  return result;
}

} // namespace

void writeBimCachedRenderPreview(const std::filesystem::path& destination, const bim::Model& model,
                                const RoboticsScene& scene) {
  auto extension = destination.extension().string();
  std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  requireRender(extension == ".glb", "preview destination must have a .glb extension");
  const auto scale = scene.frame.metersPerRendererUnit;
  requireRender(scale && std::isfinite(*scale) && *scale > 0.0, "preview requires known metric units");
  const DrawRanges ranges(model);
  validateDraws(model, ranges);

  std::unordered_map<std::string_view, const SemanticEntity*> sourceEntities, guidEntities;
  for (const auto& entity : scene.entities) {
    if (!entity.sourceId.empty()) sourceEntities.try_emplace(entity.sourceId, &entity);
    if (!entity.guid.empty() && !guidEntities.try_emplace(entity.guid, &entity).second)
      guidEntities[entity.guid] = nullptr;
  }
  tinygltf::Model gltf;
  gltf.asset.version = "2.0";
  gltf.asset.generator = "Container BIM Robotics";
  gltf.asset.extras = tinygltf::Value(tinygltf::Value::Object{
      {"sourceNamespace", tinygltf::Value(scene.sourceNamespace)}, {"unit", tinygltf::Value(std::string("metre"))}});
  gltf.defaultScene = 0;
  gltf.scenes.emplace_back();
  if (!model.elements.empty()) gltf.buffers.emplace_back();
  using MaterialKey = std::tuple<uint32_t, std::array<float, 4>, bool, bool>;
  std::map<MaterialKey, int> materials;
  std::map<std::pair<int, uint32_t>, tinygltf::Primitive> primitives;
  std::map<std::tuple<int, uint32_t, int>, int> meshes;
  bool usesUnlit = false;
  for (const auto& element : model.elements) {
    const auto& range = ranges.forElement(element);
    const bool unlit = range.mode != TINYGLTF_MODE_TRIANGLES;
    const auto color = element.materialIndex == noMaterial ? element.color : glm::vec4(1.0f);
    const auto [material, inserted] = materials.try_emplace(
        MaterialKey{element.materialIndex, {color.r, color.g, color.b, color.a}, element.doubleSided, unlit},
        gltfIndex(gltf.materials.size()));
    if (inserted) {
      container::material::Material pbr;
      if (element.materialIndex != noMaterial) {
        pbr = model.materials[element.materialIndex].pbr;
      } else {
        pbr.baseColor = color;
        if (color.a < 1.0f) pbr.alphaMode = AlphaMode::Blend;
      }
      usesUnlit = usesUnlit || unlit || pbr.unlit;
      gltf.materials.push_back(previewMaterial(pbr, element.doubleSided, unlit));
    }

    const glm::dmat4 sourceTransform(element.transform);
    auto transform = sourceTransform;
    for (glm::length_t column = 0; column < 4; ++column)
      for (glm::length_t row = 0; row < 3; ++row) {
        transform[column][row] *= *scale;
        requireRender(std::abs(transform[column][row]) <= std::numeric_limits<float>::max(),
                      "metric preview transform is not representable in glTF");
      }
    tinygltf::Node node;
    node.name = element.displayName.empty() ? element.sourceId : element.displayName;
    // glTF nodes require a TRS-decomposable matrix; bake only sheared occurrences.
    if (hasShear(sourceTransform)) {
      auto primitive = previewPrimitive(gltf, model, range, transform);
      primitive.material = material->second;
      node.mesh = gltfIndex(gltf.meshes.size());
      gltf.meshes.emplace_back().primitives.push_back(std::move(primitive));
    } else {
      const auto [mesh, newMesh] = meshes.try_emplace(std::tuple{range.mode, element.meshId, material->second},
                                                     gltfIndex(gltf.meshes.size()));
      node.mesh = mesh->second;
      if (newMesh) {
        const auto [preview, newPrimitive] = primitives.try_emplace(std::pair{range.mode, element.meshId});
        if (newPrimitive) preview->second = previewPrimitive(gltf, model, range, glm::dmat4(1.0));
        auto primitive = preview->second;
        primitive.material = material->second;
        gltf.meshes.emplace_back().primitives.push_back(std::move(primitive));
      }
      for (glm::length_t column = 0; column < 4; ++column)
        for (glm::length_t row = 0; row < 4; ++row) node.matrix.push_back(transform[column][row]);
    }

    const auto& sourceId = element.semanticSourceId.empty() ? element.sourceId : element.semanticSourceId;
    const SemanticEntity* entity = nullptr;
    if (!element.isRepresentationOnly) {
      if (const auto owner = sourceEntities.find(sourceId); owner != sourceEntities.end()) entity = owner->second;
      if (!entity && sourceId.empty())
        if (const auto owner = guidEntities.find(element.guid); owner != guidEntities.end()) entity = owner->second;
    }
    tinygltf::Value::Object extras{
        {"representationId", tinygltf::Value(element.sourceId)}, {"semanticSourceId", tinygltf::Value(sourceId)},
        {"sourceMeshId", tinygltf::Value(static_cast<double>(element.meshId))}};
    if (entity) {
      extras.emplace("entityId", tinygltf::Value(entity->id));
      if (!entity->guid.empty()) extras.emplace("guid", tinygltf::Value(entity->guid));
    }
    node.extras = tinygltf::Value(std::move(extras));
    gltf.scenes.front().nodes.push_back(gltfIndex(gltf.nodes.size()));
    gltf.nodes.push_back(std::move(node));
  }
  if (usesUnlit) gltf.extensionsUsed.emplace_back("KHR_materials_unlit");
  // The preview contains one packed buffer and no external assets. Use the
  // same glTF serializer without pulling the editor's scene graph into the
  // CPU asset cache target.
  tinygltf::TinyGLTF writer;
  std::ostringstream stream(std::ios::binary);
  if (!writer.WriteGltfSceneToStream(&gltf, stream, false, true) || !stream)
    throw std::runtime_error("Could not serialize the BIM render preview");
  util::writeFileAtomically(destination, stream.view());
}

} // namespace container::robotics
