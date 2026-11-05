#include <Container/geometry/GltfModelLoader.h>

#include <Container/geometry/Mesh.h>

#include <tiny_gltf.h>

extern "C" {
#include <mikktspace.h>
}

#include <glm/geometric.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace container::geometry {
namespace gltf {
namespace {

bool isBinaryGltf(const std::string& path) {
  const auto dotPos = path.find_last_of('.');
  if (dotPos == std::string::npos || dotPos + 1 >= path.size()) return false;

  std::string ext = path.substr(dotPos + 1);
  for (char& c : ext) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return ext == "glb";
}

size_t componentSizeBytes(int componentType) {
  const int size = tinygltf::GetComponentSizeInBytes(componentType);
  if (size <= 0) {
    throw std::runtime_error("Unsupported glTF component type");
  }
  return static_cast<size_t>(size);
}

template <typename T>
T readUnalignedScalar(const uint8_t* data) {
  T value{};
  std::memcpy(&value, data, sizeof(T));
  return value;
}

[[nodiscard]] std::runtime_error gltfError(std::string_view label,
                                           std::string_view message) {
  return std::runtime_error(std::string(label) + ": " + std::string(message));
}

const tinygltf::Accessor& checkedAccessor(const tinygltf::Model& model,
                                          int accessorIndex,
                                          std::string_view label) {
  if (accessorIndex < 0 ||
      static_cast<size_t>(accessorIndex) >= model.accessors.size()) {
    throw gltfError(label, "accessor index is out of range");
  }
  const auto& accessor = model.accessors[static_cast<size_t>(accessorIndex)];
  if (accessor.sparse.isSparse) {
    throw gltfError(label, "sparse accessors are not supported");
  }
  return accessor;
}

const tinygltf::BufferView& checkedBufferView(const tinygltf::Model& model,
                                              const tinygltf::Accessor& accessor,
                                              std::string_view label) {
  if (accessor.bufferView < 0 ||
      static_cast<size_t>(accessor.bufferView) >= model.bufferViews.size()) {
    throw gltfError(label, "accessor is missing a valid buffer view");
  }
  return model.bufferViews[static_cast<size_t>(accessor.bufferView)];
}

const tinygltf::Buffer& checkedBuffer(const tinygltf::Model& model,
                                      const tinygltf::BufferView& view,
                                      std::string_view label) {
  if (view.buffer < 0 || static_cast<size_t>(view.buffer) >= model.buffers.size()) {
    throw gltfError(label, "buffer view is missing a valid buffer");
  }
  return model.buffers[static_cast<size_t>(view.buffer)];
}

size_t accessorElementSizeBytes(const tinygltf::Accessor& accessor,
                                std::string_view label) {
  const int componentCount = tinygltf::GetNumComponentsInType(accessor.type);
  if (componentCount <= 0) {
    throw gltfError(label, "unsupported accessor type");
  }
  return static_cast<size_t>(componentCount) *
         componentSizeBytes(accessor.componentType);
}

size_t accessorStrideBytes(const tinygltf::Accessor& accessor,
                           const tinygltf::BufferView& view,
                           std::string_view label) {
  const int stride = accessor.ByteStride(view);
  if (stride < 0) {
    throw gltfError(label, "invalid accessor byte stride");
  }
  const size_t elementSize = accessorElementSizeBytes(accessor, label);
  const size_t strideBytes = stride == 0 ? elementSize : static_cast<size_t>(stride);
  if (strideBytes < elementSize) {
    throw gltfError(label, "accessor byte stride is smaller than element size");
  }
  return strideBytes;
}

void validateAccessorRange(const tinygltf::Accessor& accessor,
                           const tinygltf::BufferView& view,
                           const tinygltf::Buffer& buffer,
                           std::string_view label) {
  if (view.byteOffset > buffer.data.size()) {
    throw gltfError(label, "buffer view offset is outside the buffer");
  }
  if (view.byteLength > buffer.data.size() - view.byteOffset) {
    throw gltfError(label, "buffer view range is outside the buffer");
  }
  if (accessor.byteOffset > view.byteLength) {
    throw gltfError(label, "accessor offset is outside the buffer view");
  }

  const size_t elementSize = accessorElementSizeBytes(accessor, label);
  const size_t stride = accessorStrideBytes(accessor, view, label);
  if (accessor.count == 0) return;

  const size_t maxOffset = std::numeric_limits<size_t>::max();
  const size_t lastIndex = accessor.count - 1;
  if (lastIndex > (maxOffset - accessor.byteOffset) / stride) {
    throw gltfError(label, "accessor byte range overflows size_t");
  }

  const size_t lastElementOffset = accessor.byteOffset + lastIndex * stride;
  if (lastElementOffset > view.byteLength ||
      elementSize > view.byteLength - lastElementOffset) {
    throw gltfError(label, "accessor byte range is outside the buffer view");
  }
}

struct AccessorReadView {
  const tinygltf::Accessor& accessor;
  const tinygltf::BufferView& view;
  const tinygltf::Buffer& buffer;
};

AccessorReadView checkedAccessorReadView(const tinygltf::Model& model,
                                         int accessorIndex,
                                         std::string_view label) {
  const auto& accessor = checkedAccessor(model, accessorIndex, label);
  const auto& view = checkedBufferView(model, accessor, label);
  const auto& buffer = checkedBuffer(model, view, label);
  validateAccessorRange(accessor, view, buffer, label);
  return {accessor, view, buffer};
}

const uint8_t* accessorElementData(const tinygltf::Accessor& accessor,
                                   const tinygltf::BufferView& view,
                                   const tinygltf::Buffer& buffer,
                                   size_t index) {
  if (index >= accessor.count) {
    throw std::runtime_error("glTF accessor read index is out of range");
  }
  const size_t stride = accessorStrideBytes(accessor, view, "accessor read");
  const auto* dataStart = buffer.data.data() + view.byteOffset + accessor.byteOffset;
  return dataStart + index * stride;
}

float readComponentAsFloat(const uint8_t* data, int componentType,
                           bool normalized) {
  switch (componentType) {
    case TINYGLTF_COMPONENT_TYPE_FLOAT:
      return readUnalignedScalar<float>(data);
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: {
      const auto value = static_cast<float>(readUnalignedScalar<uint8_t>(data));
      return normalized ? value / 255.0f : value;
    }
    case TINYGLTF_COMPONENT_TYPE_BYTE: {
      const auto value = static_cast<float>(readUnalignedScalar<int8_t>(data));
      return normalized ? std::max(value / 127.0f, -1.0f) : value;
    }
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
      const auto value =
          static_cast<float>(readUnalignedScalar<uint16_t>(data));
      return normalized ? value / 65535.0f : value;
    }
    case TINYGLTF_COMPONENT_TYPE_SHORT: {
      const auto value = static_cast<float>(readUnalignedScalar<int16_t>(data));
      return normalized ? std::max(value / 32767.0f, -1.0f) : value;
    }
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
      return static_cast<float>(readUnalignedScalar<uint32_t>(data));
    default:
      throw std::runtime_error("Unsupported glTF component type");
  }
}

glm::vec2 readVec2f(const tinygltf::Accessor& accessor,
                    const tinygltf::BufferView& view,
                    const tinygltf::Buffer& buffer, size_t index) {
  const uint8_t* element = accessorElementData(accessor, view, buffer, index);
  const size_t componentSize = componentSizeBytes(accessor.componentType);
  return glm::vec2(readComponentAsFloat(element, accessor.componentType,
                                        accessor.normalized),
                   readComponentAsFloat(element + componentSize,
                                        accessor.componentType,
                                        accessor.normalized));
}

glm::vec3 readVec3f(const tinygltf::Accessor& accessor,
                    const tinygltf::BufferView& view,
                    const tinygltf::Buffer& buffer, size_t index) {
  const uint8_t* element = accessorElementData(accessor, view, buffer, index);
  const size_t componentSize = componentSizeBytes(accessor.componentType);
  return glm::vec3(
      readComponentAsFloat(element, accessor.componentType, accessor.normalized),
      readComponentAsFloat(element + componentSize, accessor.componentType,
                           accessor.normalized),
      readComponentAsFloat(element + componentSize * 2,
                           accessor.componentType, accessor.normalized));
}

glm::vec4 readVec4f(const tinygltf::Accessor& accessor,
                    const tinygltf::BufferView& view,
                    const tinygltf::Buffer& buffer, size_t index) {
  const uint8_t* element = accessorElementData(accessor, view, buffer, index);
  const size_t componentSize = componentSizeBytes(accessor.componentType);
  return glm::vec4(
      readComponentAsFloat(element, accessor.componentType, accessor.normalized),
      readComponentAsFloat(element + componentSize, accessor.componentType,
                           accessor.normalized),
      readComponentAsFloat(element + componentSize * 2,
                           accessor.componentType, accessor.normalized),
      readComponentAsFloat(element + componentSize * 3,
                           accessor.componentType, accessor.normalized));
}

bool isColorComponentTypeSupported(const tinygltf::Accessor& accessor) {
  switch (accessor.componentType) {
    case TINYGLTF_COMPONENT_TYPE_FLOAT:
      return true;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
      return accessor.normalized;
    default:
      return false;
  }
}

bool isTexCoordComponentTypeSupported(const tinygltf::Accessor& accessor) {
  switch (accessor.componentType) {
    case TINYGLTF_COMPONENT_TYPE_FLOAT:
      return true;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
      return accessor.normalized;
    default:
      return false;
  }
}

bool isNormalComponentTypeSupported(const tinygltf::Accessor& accessor) {
  if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
    return true;
  }
  switch (accessor.componentType) {
    case TINYGLTF_COMPONENT_TYPE_BYTE:
    case TINYGLTF_COMPONENT_TYPE_SHORT:
      return accessor.normalized;
    default:
      return false;
  }
}

bool isTangentComponentTypeSupported(const tinygltf::Accessor& accessor) {
  return isNormalComponentTypeSupported(accessor);
}

glm::vec3 fallbackTangentForNormal(const glm::vec3& normal) {
  glm::vec3 axis = std::abs(normal.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f)
                                               : glm::vec3(1.0f, 0.0f, 0.0f);
  glm::vec3 tangent = glm::cross(axis, normal);
  if (glm::dot(tangent, tangent) < 1e-8f) {
    tangent = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), normal);
  }
  if (glm::dot(tangent, tangent) < 1e-8f) {
    tangent = glm::vec3(1.0f, 0.0f, 0.0f);
  }
  return glm::normalize(tangent);
}

bool isFiniteVec3(const glm::vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

bool isFiniteVec2(const glm::vec2& value) {
  return std::isfinite(value.x) && std::isfinite(value.y);
}

bool isFiniteVec4(const glm::vec4& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z) && std::isfinite(value.w);
}

glm::vec3 sanitizeNormal(const glm::vec3& normal) {
  if (!isFiniteVec3(normal) || glm::dot(normal, normal) < 1e-8f) {
    return glm::vec3(0.0f, 1.0f, 0.0f);
  }
  return glm::normalize(normal);
}

glm::vec4 orthonormalizeTangent(const glm::vec3& normal,
                                const glm::vec4& tangentValue) {
  const glm::vec3 safeNormal = sanitizeNormal(normal);

  glm::vec3 tangent(tangentValue);
  tangent -= safeNormal * glm::dot(safeNormal, tangent);
  if (!isFiniteVec3(tangent) || glm::dot(tangent, tangent) < 1e-8f) {
    tangent = fallbackTangentForNormal(safeNormal);
  } else {
    tangent = glm::normalize(tangent);
  }

  float handedness = std::isfinite(tangentValue.w) ? tangentValue.w : 1.0f;
  handedness = handedness < 0.0f ? -1.0f : 1.0f;
  return glm::vec4(tangent, handedness);
}

void generateMissingNormals(std::vector<Vertex>& vertices,
                            const std::vector<uint32_t>& indices) {
  for (auto& vertex : vertices) {
    vertex.normal = glm::vec3(0.0f);
  }

  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    const uint32_t i0 = indices[i];
    const uint32_t i1 = indices[i + 1];
    const uint32_t i2 = indices[i + 2];
    if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
      continue;
    }

    const glm::vec3 edge01 = vertices[i1].position - vertices[i0].position;
    const glm::vec3 edge02 = vertices[i2].position - vertices[i0].position;
    const glm::vec3 faceNormal = glm::cross(edge01, edge02);
    if (glm::dot(faceNormal, faceNormal) < 1e-8f) {
      continue;
    }

    vertices[i0].normal += faceNormal;
    vertices[i1].normal += faceNormal;
    vertices[i2].normal += faceNormal;
  }

  for (auto& vertex : vertices) {
    if (glm::dot(vertex.normal, vertex.normal) < 1e-8f) {
      vertex.normal = glm::vec3(0.0f, 1.0f, 0.0f);
      continue;
    }
    vertex.normal = glm::normalize(vertex.normal);
  }
}

void normalizeLoadedNormals(std::vector<Vertex>& vertices) {
  for (auto& vertex : vertices) {
    vertex.normal = sanitizeNormal(vertex.normal);
  }
}

bool triangleNormalAgreement(
    const std::vector<Vertex>& vertices,
    uint32_t i0,
    uint32_t i1,
    uint32_t i2,
    float& agreement) {
  if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
    return false;
  }

  const glm::vec3 edge01 = vertices[i1].position - vertices[i0].position;
  const glm::vec3 edge02 = vertices[i2].position - vertices[i0].position;
  const glm::vec3 faceNormal = glm::cross(edge01, edge02);
  if (!isFiniteVec3(faceNormal) || glm::dot(faceNormal, faceNormal) < 1e-12f) {
    return false;
  }

  const glm::vec3 vertexNormal =
      vertices[i0].normal + vertices[i1].normal + vertices[i2].normal;
  if (!isFiniteVec3(vertexNormal) ||
      glm::dot(vertexNormal, vertexNormal) < 1e-12f) {
    return false;
  }

  agreement = glm::dot(glm::normalize(faceNormal),
                       glm::normalize(vertexNormal));
  return true;
}

struct WindingRepairResult {
  size_t repairedTriangleCount{0};
  bool disableBackfaceCulling{false};
};

uint64_t edgeKey(uint32_t a, uint32_t b) {
  if (a > b) {
    std::swap(a, b);
  }
  return (static_cast<uint64_t>(a) << 32u) | static_cast<uint64_t>(b);
}

bool isDenseOpenReliefTopology(const std::vector<uint32_t>& indices) {
  const size_t triangleCount = indices.size() / 3u;
  if (triangleCount < 1024u) {
    return false;
  }

  std::vector<uint64_t> edges;
  edges.reserve(triangleCount * 3u);
  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    edges.push_back(edgeKey(indices[i], indices[i + 1]));
    edges.push_back(edgeKey(indices[i + 1], indices[i + 2]));
    edges.push_back(edgeKey(indices[i + 2], indices[i]));
  }
  if (edges.empty()) {
    return false;
  }

  std::sort(edges.begin(), edges.end());
  size_t uniqueEdgeCount = 0;
  size_t boundaryEdgeCount = 0;
  for (size_t i = 0; i < edges.size();) {
    const uint64_t key = edges[i];
    size_t runCount = 0;
    while (i < edges.size() && edges[i] == key) {
      ++i;
      ++runCount;
    }
    ++uniqueEdgeCount;
    if (runCount == 1u) {
      ++boundaryEdgeCount;
    }
  }

  if (uniqueEdgeCount == 0u) {
    return false;
  }

  const double boundaryRatio =
      static_cast<double>(boundaryEdgeCount) /
      static_cast<double>(uniqueEdgeCount);
  // Dense relief decals in sample assets can be authored as open embossed
  // sheets mounted just in front of another primitive. They need no-cull
  // rendering, but broad open-mesh detection would incorrectly disable culling
  // for simple one-sided test planes.
  return boundaryRatio >= 0.075 && boundaryRatio <= 0.10;
}

WindingRepairResult repairTriangleWindingFromNormals(
    const std::vector<Vertex>& vertices,
    std::vector<uint32_t>& indices) {
  size_t agreeingTriangleCount = 0;
  size_t disagreeingTriangleCount = 0;

  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    float agreement = 0.0f;
    if (!triangleNormalAgreement(vertices, indices[i], indices[i + 1],
                                 indices[i + 2], agreement)) {
      continue;
    }
    if (agreement > 0.25f) {
      ++agreeingTriangleCount;
    } else if (agreement < -0.25f) {
      ++disagreeingTriangleCount;
    }
  }

  const size_t evidenceTriangleCount =
      agreeingTriangleCount + disagreeingTriangleCount;
  if (evidenceTriangleCount == 0 || disagreeingTriangleCount == 0) {
    return {};
  }

  if (disagreeingTriangleCount * 4 < evidenceTriangleCount * 3) {
    return {0, agreeingTriangleCount > 0};
  }

  WindingRepairResult result{};
  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    std::swap(indices[i + 1], indices[i + 2]);
    ++result.repairedTriangleCount;
  }
  return result;
}

void sanitizeTangents(std::vector<Vertex>& vertices) {
  for (auto& vertex : vertices) {
    vertex.normal = sanitizeNormal(vertex.normal);
    vertex.tangent = orthonormalizeTangent(vertex.normal, vertex.tangent);
  }
}

bool hasTrustworthyTangent(const Vertex& vertex) {
  if (!isFiniteVec4(vertex.tangent) || std::abs(vertex.tangent.w) < 1e-8f) {
    return false;
  }

  const glm::vec3 normal = sanitizeNormal(vertex.normal);
  glm::vec3 tangent(vertex.tangent);
  tangent -= normal * glm::dot(normal, tangent);
  const float projectedLengthSq = glm::dot(tangent, tangent);
  return isFiniteVec3(tangent) && std::isfinite(projectedLengthSq) &&
         projectedLengthSq >= 1e-8f;
}

struct TangentBasis {
  glm::vec3 tangent{0.0f};
  glm::vec3 bitangent{0.0f};
};

struct MikkTangentContext {
  std::vector<Vertex>* vertices{nullptr};
  const std::vector<uint32_t>* indices{nullptr};
  std::vector<uint8_t>* tangentWritten{nullptr};
};

uint32_t mikkVertexIndex(const SMikkTSpaceContext* context, int faceIndex,
                         int vertexIndex) {
  const auto* data =
      reinterpret_cast<const MikkTangentContext*>(context->m_pUserData);
  const size_t indexOffset =
      static_cast<size_t>(std::max(faceIndex, 0)) * 3u +
      static_cast<size_t>(std::clamp(vertexIndex, 0, 2));
  if (data == nullptr || data->indices == nullptr ||
      indexOffset >= data->indices->size()) {
    return 0;
  }
  return (*data->indices)[indexOffset];
}

const Vertex& mikkVertex(const SMikkTSpaceContext* context, int faceIndex,
                         int vertexIndex) {
  const auto* data =
      reinterpret_cast<const MikkTangentContext*>(context->m_pUserData);
  if (data == nullptr || data->vertices == nullptr || data->vertices->empty()) {
    static const Vertex kFallbackVertex{};
    return kFallbackVertex;
  }
  const uint32_t index = mikkVertexIndex(context, faceIndex, vertexIndex);
  return (*data->vertices)[std::min<size_t>(index, data->vertices->size() - 1u)];
}

int mikkGetNumFaces(const SMikkTSpaceContext* context) {
  const auto* data =
      reinterpret_cast<const MikkTangentContext*>(context->m_pUserData);
  if (data == nullptr || data->indices == nullptr) {
    return 0;
  }
  return static_cast<int>(data->indices->size() / 3u);
}

int mikkGetNumVerticesOfFace(const SMikkTSpaceContext*, int) {
  return 3;
}

void mikkGetPosition(const SMikkTSpaceContext* context, float position[3],
                     int faceIndex, int vertexIndex) {
  const glm::vec3& value = mikkVertex(context, faceIndex, vertexIndex).position;
  position[0] = value.x;
  position[1] = value.y;
  position[2] = value.z;
}

void mikkGetNormal(const SMikkTSpaceContext* context, float normal[3],
                   int faceIndex, int vertexIndex) {
  const glm::vec3 value =
      sanitizeNormal(mikkVertex(context, faceIndex, vertexIndex).normal);
  normal[0] = value.x;
  normal[1] = value.y;
  normal[2] = value.z;
}

void mikkGetTexCoord(const SMikkTSpaceContext* context, float texCoord[2],
                     int faceIndex, int vertexIndex) {
  const glm::vec2& value = mikkVertex(context, faceIndex, vertexIndex).texCoord;
  texCoord[0] = std::isfinite(value.x) ? value.x : 0.0f;
  texCoord[1] = std::isfinite(value.y) ? value.y : 0.0f;
}

void mikkSetTangent(const SMikkTSpaceContext* context, const float tangent[3],
                    float sign, int faceIndex, int vertexIndex) {
  auto* data = reinterpret_cast<MikkTangentContext*>(context->m_pUserData);
  if (data == nullptr || data->vertices == nullptr ||
      data->tangentWritten == nullptr) {
    return;
  }

  const uint32_t index = mikkVertexIndex(context, faceIndex, vertexIndex);
  if (index >= data->vertices->size()) {
    return;
  }

  Vertex& vertex = (*data->vertices)[index];
  vertex.tangent = orthonormalizeTangent(
      vertex.normal, glm::vec4(tangent[0], tangent[1], tangent[2], sign));
  (*data->tangentWritten)[index] = 1u;
}

std::vector<TangentBasis> accumulateGeometryTangents(
    const std::vector<Vertex>& vertices,
    const std::vector<uint32_t>& indices) {
  std::vector<TangentBasis> bases(vertices.size());

  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    const uint32_t i0 = indices[i];
    const uint32_t i1 = indices[i + 1];
    const uint32_t i2 = indices[i + 2];
    if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
      continue;
    }

    const glm::vec3& p0 = vertices[i0].position;
    const glm::vec3& p1 = vertices[i1].position;
    const glm::vec3& p2 = vertices[i2].position;
    const glm::vec2& uv0 = vertices[i0].texCoord;
    const glm::vec2& uv1 = vertices[i1].texCoord;
    const glm::vec2& uv2 = vertices[i2].texCoord;

    const glm::vec3 edge01 = p1 - p0;
    const glm::vec3 edge02 = p2 - p0;
    const glm::vec2 deltaUv01 = uv1 - uv0;
    const glm::vec2 deltaUv02 = uv2 - uv0;
    if (!isFiniteVec3(edge01) || !isFiniteVec3(edge02) ||
        !isFiniteVec2(deltaUv01) || !isFiniteVec2(deltaUv02)) {
      continue;
    }

    const float determinant =
        deltaUv01.x * deltaUv02.y - deltaUv01.y * deltaUv02.x;
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-8f) {
      continue;
    }

    const float invDeterminant = 1.0f / determinant;
    const glm::vec3 triangleTangent =
        (edge01 * deltaUv02.y - edge02 * deltaUv01.y) * invDeterminant;
    const glm::vec3 triangleBitangent =
        (edge02 * deltaUv01.x - edge01 * deltaUv02.x) * invDeterminant;

    if (!isFiniteVec3(triangleTangent) || !isFiniteVec3(triangleBitangent)) {
      continue;
    }

    bases[i0].tangent += triangleTangent;
    bases[i1].tangent += triangleTangent;
    bases[i2].tangent += triangleTangent;
    bases[i0].bitangent += triangleBitangent;
    bases[i1].bitangent += triangleBitangent;
    bases[i2].bitangent += triangleBitangent;
  }

  return bases;
}

glm::vec4 makeGeneratedTangent(const Vertex& vertex,
                               const TangentBasis& basis) {
  const glm::vec3 normal = sanitizeNormal(vertex.normal);

  glm::vec3 tangent = basis.tangent - normal * glm::dot(normal, basis.tangent);
  const float tangentLengthSq = glm::dot(tangent, tangent);
  if (!isFiniteVec3(tangent) || !std::isfinite(tangentLengthSq) ||
      tangentLengthSq < 1e-8f) {
    tangent = fallbackTangentForNormal(normal);
  } else {
    tangent = glm::normalize(tangent);
  }

  float handedness = 1.0f;
  const float bitangentLengthSq = glm::dot(basis.bitangent, basis.bitangent);
  if (isFiniteVec3(basis.bitangent) && std::isfinite(bitangentLengthSq) &&
      bitangentLengthSq >= 1e-8f) {
    handedness =
        glm::dot(glm::cross(normal, tangent), basis.bitangent) < 0.0f ? -1.0f
                                                                      : 1.0f;
  }

  return orthonormalizeTangent(normal, glm::vec4(tangent, handedness));
}

void generateTangentsFromGeometry(std::vector<Vertex>& vertices,
                                  const std::vector<uint32_t>& indices) {
  std::vector<uint8_t> tangentWritten(vertices.size(), 0u);
  MikkTangentContext tangentContext{
      &vertices,
      &indices,
      &tangentWritten,
  };
  SMikkTSpaceInterface mikkInterface{};
  mikkInterface.m_getNumFaces = mikkGetNumFaces;
  mikkInterface.m_getNumVerticesOfFace = mikkGetNumVerticesOfFace;
  mikkInterface.m_getPosition = mikkGetPosition;
  mikkInterface.m_getNormal = mikkGetNormal;
  mikkInterface.m_getTexCoord = mikkGetTexCoord;
  mikkInterface.m_setTSpaceBasic = mikkSetTangent;
  SMikkTSpaceContext mikkContext{};
  mikkContext.m_pInterface = &mikkInterface;
  mikkContext.m_pUserData = &tangentContext;

  const bool generatedByMikk =
      !vertices.empty() && indices.size() >= 3u &&
      genTangSpaceDefault(&mikkContext) != 0;

  const auto bases = accumulateGeometryTangents(vertices, indices);
  for (size_t i = 0; i < vertices.size(); ++i) {
    vertices[i].normal = sanitizeNormal(vertices[i].normal);
    if (!generatedByMikk || tangentWritten[i] == 0u ||
        !hasTrustworthyTangent(vertices[i])) {
      vertices[i].tangent = makeGeneratedTangent(vertices[i], bases[i]);
    } else {
      vertices[i].tangent =
          orthonormalizeTangent(vertices[i].normal, vertices[i].tangent);
    }
  }
}

void repairInvalidTangents(std::vector<Vertex>& vertices,
                           const std::vector<uint32_t>& indices) {
  bool hasInvalidTangent = false;

  for (size_t i = 0; i < vertices.size(); ++i) {
    vertices[i].normal = sanitizeNormal(vertices[i].normal);
    if (!hasTrustworthyTangent(vertices[i])) {
      hasInvalidTangent = true;
    }
  }

  if (!hasInvalidTangent) {
    sanitizeTangents(vertices);
    return;
  }

  generateTangentsFromGeometry(vertices, indices);
}

struct PrimitiveVertexData {
  std::vector<Vertex> vertices{};
  bool hasNormals{false};
  bool hasTangents{false};
};

std::vector<uint32_t> readIndices(const tinygltf::Model& model,
                                  const tinygltf::Primitive& primitive,
                                  size_t vertexCount) {
  if (primitive.mode != TINYGLTF_MODE_TRIANGLES) {
    throw std::runtime_error("Only glTF triangle primitives are supported");
  }

  if (primitive.indices < 0) {
    if (vertexCount % 3 != 0) {
      throw std::runtime_error("Non-indexed glTF primitive must contain triangles");
    }
    std::vector<uint32_t> sequential(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
      sequential[i] = static_cast<uint32_t>(i);
    }
    return sequential;
  }

  const auto indicesData =
      checkedAccessorReadView(model, primitive.indices, "indices");
  const auto& accessor = indicesData.accessor;
  if (accessor.type != TINYGLTF_TYPE_SCALAR) {
    throw std::runtime_error("glTF indices accessor must be scalar");
  }
  const auto& view = indicesData.view;
  const auto& buffer = indicesData.buffer;
  const auto* dataStart = buffer.data.data() + view.byteOffset + accessor.byteOffset;
  const size_t stride = accessorStrideBytes(accessor, view, "indices");

  std::vector<uint32_t> indices(accessor.count);
  for (size_t i = 0; i < accessor.count; ++i) {
    const auto* element = dataStart + i * stride;
    switch (accessor.componentType) {
      case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        indices[i] =
            static_cast<uint32_t>(readUnalignedScalar<uint16_t>(element));
        break;
      case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        indices[i] = readUnalignedScalar<uint32_t>(element);
        break;
      case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        indices[i] =
            static_cast<uint32_t>(readUnalignedScalar<uint8_t>(element));
        break;
      default:
        throw std::runtime_error("Unsupported glTF index component type");
    }
  }
  return indices;
}

PrimitiveVertexData mergeAttributes(const tinygltf::Model& model,
                                    const tinygltf::Primitive& primitive) {
  auto positionIt = primitive.attributes.find("POSITION");
  if (positionIt == primitive.attributes.end()) {
    throw std::runtime_error("glTF primitive is missing POSITION attribute");
  }

  const auto positionData =
      checkedAccessorReadView(model, positionIt->second, "POSITION");
  const auto& positionAccessor = positionData.accessor;
  if (positionAccessor.type != TINYGLTF_TYPE_VEC3 ||
      positionAccessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
    throw std::runtime_error("POSITION must be a VEC3 float attribute");
  }

  const auto& positionView = positionData.view;
  const auto& positionBuffer = positionData.buffer;

  PrimitiveVertexData primitiveData{};
  primitiveData.vertices.resize(positionAccessor.count);
  for (size_t i = 0; i < positionAccessor.count; ++i) {
    primitiveData.vertices[i].position =
        readVec3f(positionAccessor, positionView, positionBuffer, i);
  }

  auto colorIt = primitive.attributes.find("COLOR_0");
  if (colorIt != primitive.attributes.end()) {
    const auto colorData =
        checkedAccessorReadView(model, colorIt->second, "COLOR_0");
    const auto& colorAccessor = colorData.accessor;
    if ((colorAccessor.type != TINYGLTF_TYPE_VEC3 &&
         colorAccessor.type != TINYGLTF_TYPE_VEC4) ||
        !isColorComponentTypeSupported(colorAccessor)) {
      throw std::runtime_error(
          "COLOR_0 must be VEC3/VEC4 float or normalized unsigned integer");
    }
    const auto& colorView = colorData.view;
    const auto& colorBuffer = colorData.buffer;
    if (colorAccessor.count < primitiveData.vertices.size()) {
      throw std::runtime_error("COLOR_0 attribute count is smaller than POSITION count");
    }

    for (size_t i = 0; i < primitiveData.vertices.size(); ++i) {
      if (colorAccessor.type == TINYGLTF_TYPE_VEC3) {
        primitiveData.vertices[i].color =
            readVec3f(colorAccessor, colorView, colorBuffer, i);
      } else {
        const auto rgba = readVec4f(colorAccessor, colorView, colorBuffer, i);
        primitiveData.vertices[i].color = glm::vec3(rgba);
        primitiveData.vertices[i].colorAlpha = std::clamp(rgba.a, 0.0f, 1.0f);
      }
    }
  }

  auto texCoordIt = primitive.attributes.find("TEXCOORD_0");
  if (texCoordIt != primitive.attributes.end()) {
    const auto texCoordData =
        checkedAccessorReadView(model, texCoordIt->second, "TEXCOORD_0");
    const auto& texCoordAccessor = texCoordData.accessor;
    if (texCoordAccessor.type != TINYGLTF_TYPE_VEC2 ||
        !isTexCoordComponentTypeSupported(texCoordAccessor)) {
      throw std::runtime_error(
          "TEXCOORD_0 must be a VEC2 float or normalized unsigned integer");
    }

    const auto& texCoordView = texCoordData.view;
    const auto& texCoordBuffer = texCoordData.buffer;
    if (texCoordAccessor.count < primitiveData.vertices.size()) {
      throw std::runtime_error("TEXCOORD_0 attribute count is smaller than POSITION count");
    }

    for (size_t i = 0; i < primitiveData.vertices.size(); ++i) {
      primitiveData.vertices[i].texCoord =
          readVec2f(texCoordAccessor, texCoordView, texCoordBuffer, i);
      primitiveData.vertices[i].texCoord1 = primitiveData.vertices[i].texCoord;
    }
  }

  auto texCoord1It = primitive.attributes.find("TEXCOORD_1");
  if (texCoord1It != primitive.attributes.end()) {
    const auto texCoordData =
        checkedAccessorReadView(model, texCoord1It->second, "TEXCOORD_1");
    const auto& texCoordAccessor = texCoordData.accessor;
    if (texCoordAccessor.type != TINYGLTF_TYPE_VEC2 ||
        !isTexCoordComponentTypeSupported(texCoordAccessor)) {
      throw std::runtime_error(
          "TEXCOORD_1 must be a VEC2 float or normalized unsigned integer");
    }

    const auto& texCoordView = texCoordData.view;
    const auto& texCoordBuffer = texCoordData.buffer;
    if (texCoordAccessor.count < primitiveData.vertices.size()) {
      throw std::runtime_error("TEXCOORD_1 attribute count is smaller than POSITION count");
    }

    for (size_t i = 0; i < primitiveData.vertices.size(); ++i) {
      primitiveData.vertices[i].texCoord1 =
          readVec2f(texCoordAccessor, texCoordView, texCoordBuffer, i);
    }
  }

  auto normalIt = primitive.attributes.find("NORMAL");
  if (normalIt != primitive.attributes.end()) {
    const auto normalData =
        checkedAccessorReadView(model, normalIt->second, "NORMAL");
    const auto& normalAccessor = normalData.accessor;
    if (normalAccessor.type != TINYGLTF_TYPE_VEC3 ||
        !isNormalComponentTypeSupported(normalAccessor)) {
      throw std::runtime_error(
          "NORMAL must be a VEC3 float or normalized signed integer attribute");
    }
    const auto& normalView = normalData.view;
    const auto& normalBuffer = normalData.buffer;
    if (normalAccessor.count < primitiveData.vertices.size()) {
      throw std::runtime_error("NORMAL attribute count is smaller than POSITION count");
    }

    primitiveData.hasNormals = true;
    for (size_t i = 0; i < primitiveData.vertices.size(); ++i) {
      primitiveData.vertices[i].normal =
          readVec3f(normalAccessor, normalView, normalBuffer, i);
    }
  }

  auto tangentIt = primitive.attributes.find("TANGENT");
  if (tangentIt != primitive.attributes.end()) {
    const auto tangentData =
        checkedAccessorReadView(model, tangentIt->second, "TANGENT");
    const auto& tangentAccessor = tangentData.accessor;
    if (tangentAccessor.type != TINYGLTF_TYPE_VEC4 ||
        !isTangentComponentTypeSupported(tangentAccessor)) {
      throw std::runtime_error(
          "TANGENT must be a VEC4 float or normalized signed integer attribute");
    }
    const auto& tangentView = tangentData.view;
    const auto& tangentBuffer = tangentData.buffer;
    if (tangentAccessor.count < primitiveData.vertices.size()) {
      throw std::runtime_error("TANGENT attribute count is smaller than POSITION count");
    }

    primitiveData.hasTangents = true;
    for (size_t i = 0; i < primitiveData.vertices.size(); ++i) {
      primitiveData.vertices[i].tangent =
          readVec4f(tangentAccessor, tangentView, tangentBuffer, i);
    }
  }

  return primitiveData;
}

std::vector<Mesh> parseMeshes(const tinygltf::Model& model) {
  std::vector<Mesh> meshes;
  for (const auto& mesh : model.meshes) {
    for (const auto& primitive : mesh.primitives) {
      auto primitiveData = mergeAttributes(model, primitive);
      auto indices = readIndices(model, primitive, primitiveData.vertices.size());
      WindingRepairResult windingRepair{};
      if (!primitiveData.hasNormals) {
        generateMissingNormals(primitiveData.vertices, indices);
      } else {
        normalizeLoadedNormals(primitiveData.vertices);
        windingRepair =
            repairTriangleWindingFromNormals(primitiveData.vertices, indices);
      }
      const bool disableBackfaceCulling =
          windingRepair.disableBackfaceCulling ||
          isDenseOpenReliefTopology(indices);
      if (!primitiveData.hasTangents ||
          windingRepair.repairedTriangleCount > 0) {
        generateTangentsFromGeometry(primitiveData.vertices, indices);
      } else {
        repairInvalidTangents(primitiveData.vertices, indices);
      }
      meshes.emplace_back(std::move(primitiveData.vertices), std::move(indices),
                          primitive.material, disableBackfaceCulling);
    }
  }
  return meshes;
}

tinygltf::Model loadGltfModel(const std::string& path) {
  tinygltf::TinyGLTF loader;
  tinygltf::Model model;
  std::string err;
  std::string warn;

  bool loaded = false;
  if (isBinaryGltf(path)) {
    loaded = loader.LoadBinaryFromFile(&model, &err, &warn, path);
  } else {
    loaded = loader.LoadASCIIFromFile(&model, &err, &warn, path);
  }

  if (!warn.empty()) {
    std::clog << "glTF warning: " << warn << std::endl;
  }
  if (!loaded) {
    throw std::runtime_error("Failed to load glTF file: " + err);
  }

  return model;
}

}  // namespace

Model LoadModelFromFile(const std::string& path) {
  auto model = loadGltfModel(path);
  auto meshes = parseMeshes(model);
  if (meshes.empty()) {
    throw std::runtime_error("No renderable primitives found in glTF file");
  }

  return Model{std::move(meshes)};
}

GltfLoadResult LoadModelWithSource(const std::string& path) {
  auto gltfModel = loadGltfModel(path);

  auto meshes = parseMeshes(gltfModel);
  if (meshes.empty()) {
    throw std::runtime_error("No renderable primitives found in glTF file");
  }

  return GltfLoadResult{Model{std::move(meshes)}, std::move(gltfModel)};
}

}  // namespace gltf
}  // namespace container::geometry
