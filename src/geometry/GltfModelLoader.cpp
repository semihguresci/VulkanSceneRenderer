#include <Container/geometry/GltfModelLoader.h>
#include <Container/geometry/GltfAccessor.h>
#include <Container/geometry/GltfImageSources.h>
#include <Container/geometry/GltfNodeFlags.h>
#include <Container/geometry/GltfExtendedMaterials.h>
#include <Container/geometry/GltfIesProfiles.h>
#include <Container/geometry/GltfLinkedAssets.h>
#include <Container/geometry/GltfInteractivity.h>
#include <Container/geometry/GltfMerge.h>
#include <Container/geometry/GltfXmpMetadata.h>

#include <Container/geometry/Mesh.h>
#include <Container/utility/ModelImportCancellation.h>

#include <tiny_gltf.h>
#include <stb_image.h>

extern "C" {
#include <mikktspace.h>
}

#include <glm/geometric.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <climits>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

namespace container::geometry {
namespace gltf {
namespace {

using container::util::checkModelImportCancellation;
using container::util::ModelImportCancelled;

void checkImportIteration(const std::stop_token& stop, size_t iteration) {
  if (iteration % 1024 == 0) checkModelImportCancellation(stop);
}

bool isBinaryGltf(const std::string& path) {
  const auto dotPos = path.find_last_of('.');
  if (dotPos == std::string::npos || dotPos + 1 >= path.size()) return false;

  std::string ext = path.substr(dotPos + 1);
  for (char& c : ext) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return ext == "glb";
}

[[nodiscard]] std::runtime_error gltfError(std::string_view label,
                                           std::string_view message) {
  return std::runtime_error(std::string(label) + ": " + std::string(message));
}
struct AccessorReadView {
  CheckedAccessor reader;
  const tinygltf::Accessor& accessor;
};
AccessorReadView checkedAccessorReadView(const tinygltf::Model& model,
                                         int index, std::string_view label,
                                         const std::stop_token& stop,
                                         bool repairFloatFrame = false) {
  CheckedAccessor reader(model,index,label,{AccessorUsage::VertexAttribute,
    HasRequiredExtension(model,"KHR_accessor_float64"),repairFloatFrame},stop);
  const auto& accessor = reader.accessor();
  return {std::move(reader),accessor};
}
glm::vec2 readVec2f(const AccessorReadView& data,size_t index) {
  std::array<float,2> values{}; data.reader.readFloats(index,values);
  return {values[0],values[1]};
}
glm::vec3 readVec3f(const AccessorReadView& data,size_t index) {
  std::array<float,3> values{}; data.reader.readFloats(index,values);
  return {values[0],values[1],values[2]};
}
glm::vec4 readVec4f(const AccessorReadView& data,size_t index) {
  std::array<float,4> values{}; data.reader.readFloats(index,values);
  return {values[0],values[1],values[2],values[3]};
}
bool floatingComponent(const tinygltf::Model& model,const tinygltf::Accessor& accessor) {
  return !accessor.normalized && (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT ||
    (accessor.componentType == TINYGLTF_COMPONENT_TYPE_DOUBLE && HasRequiredExtension(model,"KHR_accessor_float64")));
}
bool integer8or16(const tinygltf::Accessor& accessor) {
  return accessor.componentType == TINYGLTF_COMPONENT_TYPE_BYTE || accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
         accessor.componentType == TINYGLTF_COMPONENT_TYPE_SHORT || accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
}
bool isColorComponentTypeSupported(const tinygltf::Model& model,const tinygltf::Accessor& accessor) {
  return floatingComponent(model,accessor) || (accessor.normalized &&
    (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE || accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT));
}
bool isTexCoordComponentTypeSupported(const tinygltf::Model& model,const tinygltf::Accessor& accessor) {
  return isColorComponentTypeSupported(model,accessor) ||
    (HasRequiredExtension(model,"KHR_mesh_quantization") && integer8or16(accessor));
}
bool isNormalComponentTypeSupported(const tinygltf::Model& model,const tinygltf::Accessor& accessor) {
  // Retain existing normalized signed normal/tangent compatibility. New
  // integer POSITION/UV/morph layouts require the quantization declaration.
  return floatingComponent(model,accessor) || (accessor.normalized &&
    (accessor.componentType == TINYGLTF_COMPONENT_TYPE_BYTE || accessor.componentType == TINYGLTF_COMPONENT_TYPE_SHORT));
}
bool isTangentComponentTypeSupported(const tinygltf::Model& model,const tinygltf::Accessor& accessor) {
  return isNormalComponentTypeSupported(model,accessor);
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
                            const std::vector<uint32_t>& indices,
                            const std::stop_token& stop) {
  for (size_t i = 0; i < vertices.size(); ++i) {
    checkImportIteration(stop, i);
    vertices[i].normal = glm::vec3(0.0f);
  }

  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    checkImportIteration(stop, i);
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

  for (size_t i = 0; i < vertices.size(); ++i) {
    checkImportIteration(stop, i);
    auto& vertex = vertices[i];
    if (glm::dot(vertex.normal, vertex.normal) < 1e-8f) {
      vertex.normal = glm::vec3(0.0f, 1.0f, 0.0f);
      continue;
    }
    vertex.normal = glm::normalize(vertex.normal);
  }
}

void normalizeLoadedNormals(std::vector<Vertex>& vertices, const std::stop_token& stop) {
  for (size_t i = 0; i < vertices.size(); ++i) {
    checkImportIteration(stop, i);
    vertices[i].normal = sanitizeNormal(vertices[i].normal);
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

bool isDenseOpenReliefTopology(const std::vector<uint32_t>& indices, const std::stop_token& stop) {
  const size_t triangleCount = indices.size() / 3u;
  if (triangleCount < 1024u) {
    return false;
  }

  std::vector<uint64_t> edges;
  edges.reserve(triangleCount * 3u);
  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    checkImportIteration(stop, i);
    edges.push_back(edgeKey(indices[i], indices[i + 1]));
    edges.push_back(edgeKey(indices[i + 1], indices[i + 2]));
    edges.push_back(edgeKey(indices[i + 2], indices[i]));
  }
  if (edges.empty()) {
    return false;
  }

  if (stop.stop_possible()) {
    size_t comparisons = 0;
    std::sort(edges.begin(), edges.end(), [&](uint64_t a, uint64_t b) {
      checkImportIteration(stop, comparisons++);
      return a < b;
    });
  } else {
    std::sort(edges.begin(), edges.end());
  }
  size_t uniqueEdgeCount = 0;
  size_t boundaryEdgeCount = 0;
  for (size_t i = 0; i < edges.size();) {
    const uint64_t key = edges[i];
    size_t runCount = 0;
    while (i < edges.size() && edges[i] == key) {
      checkImportIteration(stop, i);
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
    std::vector<uint32_t>& indices, const std::stop_token& stop) {
  size_t agreeingTriangleCount = 0;
  size_t disagreeingTriangleCount = 0;

  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    checkImportIteration(stop, i);
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
    checkImportIteration(stop, i);
    std::swap(indices[i + 1], indices[i + 2]);
    ++result.repairedTriangleCount;
  }
  return result;
}

void sanitizeTangents(std::vector<Vertex>& vertices, const std::stop_token& stop) {
  for (size_t i = 0; i < vertices.size(); ++i) {
    checkImportIteration(stop, i);
    auto& vertex = vertices[i];
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
  uint32_t texCoord{0};
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
  const auto* data = reinterpret_cast<const MikkTangentContext*>(context->m_pUserData);
  const auto& vertex = mikkVertex(context, faceIndex, vertexIndex);
  const glm::vec2& value = data && data->texCoord == 1 ? vertex.texCoord1 : vertex.texCoord;
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
    const std::vector<uint32_t>& indices, uint32_t texCoord, const std::stop_token& stop) {
  std::vector<TangentBasis> bases(vertices.size());

  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    checkImportIteration(stop, i);
    const uint32_t i0 = indices[i];
    const uint32_t i1 = indices[i + 1];
    const uint32_t i2 = indices[i + 2];
    if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
      continue;
    }

    const glm::vec3& p0 = vertices[i0].position;
    const glm::vec3& p1 = vertices[i1].position;
    const glm::vec3& p2 = vertices[i2].position;
    const glm::vec2& uv0 = texCoord == 1 ? vertices[i0].texCoord1 : vertices[i0].texCoord;
    const glm::vec2& uv1 = texCoord == 1 ? vertices[i1].texCoord1 : vertices[i1].texCoord;
    const glm::vec2& uv2 = texCoord == 1 ? vertices[i2].texCoord1 : vertices[i2].texCoord;

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
                                  const std::vector<uint32_t>& indices,
                                  uint32_t texCoord, const std::stop_token& stop = {}) {
  if (texCoord > 1)
    throw gltfError("normal texture", "tangent generation supports UV0 and UV1");
  std::vector<uint8_t> tangentWritten(vertices.size(), 0u);
  MikkTangentContext tangentContext{
      &vertices,
      &indices,
      &tangentWritten,
      texCoord,
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

  checkModelImportCancellation(stop);
  const bool generatedByMikk =
      !vertices.empty() && indices.size() >= 3u &&
      genTangSpaceDefault(&mikkContext) != 0;
  checkModelImportCancellation(stop);

  const auto bases = accumulateGeometryTangents(vertices, indices, texCoord, stop);
  for (size_t i = 0; i < vertices.size(); ++i) {
    checkImportIteration(stop, i);
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
                           const std::vector<uint32_t>& indices,
                           uint32_t texCoord, const std::stop_token& stop) {
  bool hasInvalidTangent = false;

  for (size_t i = 0; i < vertices.size(); ++i) {
    checkImportIteration(stop, i);
    vertices[i].normal = sanitizeNormal(vertices[i].normal);
    if (!hasTrustworthyTangent(vertices[i])) {
      hasInvalidTangent = true;
    }
  }

  if (!hasInvalidTangent) {
    sanitizeTangents(vertices, stop);
    return;
  }

  // A malformed neighbor does not invalidate a valid authored tangent. Keep
  // those values while repairing only vertices that need the selected UV set.
  std::vector<glm::vec4> authored;
  authored.reserve(vertices.size());
  for (size_t i = 0; i < vertices.size(); ++i) {
    checkImportIteration(stop, i);
    const auto& vertex = vertices[i];
    authored.push_back(hasTrustworthyTangent(vertex) ? vertex.tangent : glm::vec4(0));
  }
  generateTangentsFromGeometry(vertices, indices, texCoord, stop);
  for (size_t i = 0; i < vertices.size(); ++i) {
    checkImportIteration(stop, i);
    if (authored[i].w != 0)
      vertices[i].tangent = orthonormalizeTangent(vertices[i].normal, authored[i]);
  }
}

struct PrimitiveVertexData {
  std::vector<Vertex> vertices{};
  bool hasNormals{false};
  bool hasTangents{false};
};

std::vector<uint32_t> readIndices(const tinygltf::Model& model,
                                  const tinygltf::Primitive& primitive,
                                  size_t vertexCount, const std::stop_token& stop) {
  if (primitive.mode != TINYGLTF_MODE_TRIANGLES) {
    throw std::runtime_error("Only glTF triangle primitives are supported");
  }

  if (primitive.indices < 0) {
    if (vertexCount % 3 != 0) {
      throw std::runtime_error("Non-indexed glTF primitive must contain triangles");
    }
    std::vector<uint32_t> sequential(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
      checkImportIteration(stop, i);
      sequential[i] = static_cast<uint32_t>(i);
    }
    return sequential;
  }

  CheckedAccessor reader(model,primitive.indices,"indices",{AccessorUsage::Indices},stop);
  if (reader.accessor().type != TINYGLTF_TYPE_SCALAR) throw gltfError("indices","accessor must be scalar");
  std::vector<uint32_t> indices(reader.count());
  for (size_t i = 0; i < indices.size(); ++i) {
    checkImportIteration(stop, i);
    reader.readUnsigned(i,std::span(indices).subspan(i,1));
    if (indices[i] >= vertexCount) throw gltfError("indices","vertex index is out of range");
  }
  return indices;
}

PrimitiveVertexData mergeAttributes(const tinygltf::Model& model,
                                    const tinygltf::Primitive& primitive,
                                    const std::stop_token& stop) {
  auto positionIt = primitive.attributes.find("POSITION");
  if (positionIt == primitive.attributes.end()) {
    throw std::runtime_error("glTF primitive is missing POSITION attribute");
  }

  const auto positionData =
      checkedAccessorReadView(model, positionIt->second, "POSITION", stop);
  const auto& positionAccessor = positionData.accessor;
  if (positionAccessor.type != TINYGLTF_TYPE_VEC3 ||
      !(floatingComponent(model,positionAccessor) || (HasRequiredExtension(model,"KHR_mesh_quantization") && integer8or16(positionAccessor)))) {
    throw std::runtime_error("POSITION must be VEC3 FLOAT, declared DOUBLE, or declared quantized 8/16-bit integer");
  }


  PrimitiveVertexData primitiveData{};
  if (positionAccessor.count > std::numeric_limits<uint32_t>::max())
    throw gltfError("POSITION","vertex count exceeds the runtime index range");
  primitiveData.vertices.resize(positionAccessor.count);
  for (size_t i = 0; i < positionAccessor.count; ++i) {
    checkImportIteration(stop, i);
    primitiveData.vertices[i].position =
        readVec3f(positionData, i);
  }

  auto colorIt = primitive.attributes.find("COLOR_0");
  if (colorIt != primitive.attributes.end()) {
    const auto colorData =
        checkedAccessorReadView(model, colorIt->second, "COLOR_0", stop);
    const auto& colorAccessor = colorData.accessor;
    if ((colorAccessor.type != TINYGLTF_TYPE_VEC3 &&
         colorAccessor.type != TINYGLTF_TYPE_VEC4) ||
        !isColorComponentTypeSupported(model,colorAccessor)) {
      throw std::runtime_error(
          "COLOR_0 must be VEC3/VEC4 float or normalized unsigned integer");
    }
    if (colorAccessor.count < primitiveData.vertices.size()) {
      throw std::runtime_error("COLOR_0 attribute count is smaller than POSITION count");
    }

    for (size_t i = 0; i < primitiveData.vertices.size(); ++i) {
      checkImportIteration(stop, i);
      if (colorAccessor.type == TINYGLTF_TYPE_VEC3) {
        primitiveData.vertices[i].color = glm::clamp(readVec3f(colorData,i),0.0f,1.0f);
        primitiveData.vertices[i].colorAlpha = 1;
      } else {
        const auto rgba = readVec4f(colorData, i);
        primitiveData.vertices[i].color = glm::clamp(glm::vec3(rgba),0.0f,1.0f);
        primitiveData.vertices[i].colorAlpha = std::clamp(rgba.a,0.0f,1.0f);
      }
    }
  }

  auto texCoordIt = primitive.attributes.find("TEXCOORD_0");
  if (texCoordIt != primitive.attributes.end()) {
    const auto texCoordData =
        checkedAccessorReadView(model, texCoordIt->second, "TEXCOORD_0", stop);
    const auto& texCoordAccessor = texCoordData.accessor;
    if (texCoordAccessor.type != TINYGLTF_TYPE_VEC2 ||
        !isTexCoordComponentTypeSupported(model,texCoordAccessor)) {
      throw std::runtime_error(
          "TEXCOORD_0 must be a VEC2 float or normalized unsigned integer");
    }

    if (texCoordAccessor.count < primitiveData.vertices.size()) {
      throw std::runtime_error("TEXCOORD_0 attribute count is smaller than POSITION count");
    }

    for (size_t i = 0; i < primitiveData.vertices.size(); ++i) {
      checkImportIteration(stop, i);
      primitiveData.vertices[i].texCoord =
          readVec2f(texCoordData, i);
      primitiveData.vertices[i].texCoord1 = primitiveData.vertices[i].texCoord;
    }
  }

  auto texCoord1It = primitive.attributes.find("TEXCOORD_1");
  if (texCoord1It != primitive.attributes.end()) {
    const auto texCoordData =
        checkedAccessorReadView(model, texCoord1It->second, "TEXCOORD_1", stop);
    const auto& texCoordAccessor = texCoordData.accessor;
    if (texCoordAccessor.type != TINYGLTF_TYPE_VEC2 ||
        !isTexCoordComponentTypeSupported(model,texCoordAccessor)) {
      throw std::runtime_error(
          "TEXCOORD_1 must be a VEC2 float or normalized unsigned integer");
    }

    if (texCoordAccessor.count < primitiveData.vertices.size()) {
      throw std::runtime_error("TEXCOORD_1 attribute count is smaller than POSITION count");
    }

    for (size_t i = 0; i < primitiveData.vertices.size(); ++i) {
      checkImportIteration(stop, i);
      primitiveData.vertices[i].texCoord1 =
          readVec2f(texCoordData, i);
    }
  }

  auto normalIt = primitive.attributes.find("NORMAL");
  if (normalIt != primitive.attributes.end()) {
    const auto normalData =
        checkedAccessorReadView(model, normalIt->second, "NORMAL", stop, true);
    const auto& normalAccessor = normalData.accessor;
    if (normalAccessor.type != TINYGLTF_TYPE_VEC3 ||
        !isNormalComponentTypeSupported(model,normalAccessor)) {
      throw std::runtime_error(
          "NORMAL must be a VEC3 float or normalized signed integer attribute");
    }
    if (normalAccessor.count < primitiveData.vertices.size()) {
      throw std::runtime_error("NORMAL attribute count is smaller than POSITION count");
    }

    primitiveData.hasNormals = true;
    for (size_t i = 0; i < primitiveData.vertices.size(); ++i) {
      checkImportIteration(stop, i);
      primitiveData.vertices[i].normal =
          readVec3f(normalData, i);
    }
  }

  auto tangentIt = primitive.attributes.find("TANGENT");
  if (tangentIt != primitive.attributes.end()) {
    const auto tangentData =
        checkedAccessorReadView(model, tangentIt->second, "TANGENT", stop, true);
    const auto& tangentAccessor = tangentData.accessor;
    if (tangentAccessor.type != TINYGLTF_TYPE_VEC4 ||
        !isTangentComponentTypeSupported(model,tangentAccessor)) {
      throw std::runtime_error(
          "TANGENT must be a VEC4 float or normalized signed integer attribute");
    }
    if (tangentAccessor.count < primitiveData.vertices.size()) {
      throw std::runtime_error("TANGENT attribute count is smaller than POSITION count");
    }

    primitiveData.hasTangents = true;
    for (size_t i = 0; i < primitiveData.vertices.size(); ++i) {
      checkImportIteration(stop, i);
      primitiveData.vertices[i].tangent =
          readVec4f(tangentData, i);
    }
  }

  return primitiveData;
}

uint32_t normalTextureTexCoord(const tinygltf::Model& model,
                               const tinygltf::Primitive& primitive) {
  if (primitive.material < 0 || size_t(primitive.material) >= model.materials.size()) return 0;
  const auto& normal = model.materials[size_t(primitive.material)].normalTexture;
  if (normal.index < 0) return 0;
  int selected = normal.texCoord;
  if (const auto transform = normal.extensions.find("KHR_texture_transform");
      transform != normal.extensions.end() && transform->second.IsObject() && transform->second.Has("texCoord")) {
    const auto& override = transform->second.Get("texCoord");
    if (!override.IsInt()) throw gltfError("normal texture", "texture-transform texCoord must be an integer");
    selected = override.Get<int>();
  }
  if (selected < 0) throw gltfError("normal texture", "texCoord must be nonnegative");
  return static_cast<uint32_t>(selected);
}

std::vector<Mesh> parseMeshes(const tinygltf::Model& model, GltfOpacityMicromapAssets& micromaps,
                             const std::stop_token& stop) {
  std::vector<Mesh> meshes;
  for (size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex) {
    const auto& mesh = model.meshes[meshIndex];
    for (size_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size(); ++primitiveIndex) {
      checkModelImportCancellation(stop);
      const auto& primitive = mesh.primitives[primitiveIndex];
      auto primitiveData = mergeAttributes(model, primitive, stop);
      auto indices = readIndices(model, primitive, primitiveData.vertices.size(), stop);
      WindingRepairResult windingRepair{};
      if (!primitiveData.hasNormals) {
        generateMissingNormals(primitiveData.vertices, indices, stop);
      } else {
        normalizeLoadedNormals(primitiveData.vertices, stop);
        windingRepair =
            repairTriangleWindingFromNormals(primitiveData.vertices, indices, stop);
      }
      const bool disableBackfaceCulling =
          windingRepair.disableBackfaceCulling ||
          isDenseOpenReliefTopology(indices, stop);
      // Micromap cells are addressed in the source triangle's ordered
      // barycentric frame. A winding repair swaps two corners of that frame.
      if (windingRepair.repairedTriangleCount && micromaps.find(meshIndex, primitiveIndex)) {
        micromaps.primitives[meshIndex][primitiveIndex].reset();
        if (micromaps.diagnostics.size() < GltfOpacityMicromapLimits{}.maxDiagnostics)
          micromaps.diagnostics.push_back({"mesh " + std::to_string(meshIndex) + "/" + std::to_string(primitiveIndex),
              "triangle winding was repaired; using sampled alpha"});
        else
          ++micromaps.diagnosticsOmitted;
      }
      const auto tangentTexCoord = normalTextureTexCoord(model, primitive);
      if (!primitiveData.hasNormals || !primitiveData.hasTangents ||
          windingRepair.repairedTriangleCount > 0) {
        generateTangentsFromGeometry(primitiveData.vertices, indices, tangentTexCoord, stop);
      } else {
        repairInvalidTangents(primitiveData.vertices, indices, tangentTexCoord, stop);
      }
      meshes.emplace_back(std::move(primitiveData.vertices), std::move(indices),
                          primitive.material, disableBackfaceCulling);
    }
  }
  return meshes;
}

nlohmann::json parseSourceJson(const std::vector<unsigned char>& bytes, bool binary,
                               const std::stop_token& stop) {
  size_t begin = 0, length = bytes.size();
  if (binary) {
    auto word = [&](size_t offset) {
      return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) |
             (uint32_t(bytes[offset + 2]) << 16) | (uint32_t(bytes[offset + 3]) << 24);
    };
    if (bytes.size() < 20 || word(0) != 0x46546c67 || word(4) != 2 ||
        word(8) != bytes.size() || word(16) != 0x4e4f534a ||
        word(12) > bytes.size() - 20)
      throw std::invalid_argument("invalid GLB header or JSON chunk");
    begin = 20;
    length = word(12);
  }
  nlohmann::json::parser_callback_t checkpoint;
  if (stop.stop_possible()) {
    checkpoint = [&stop, events = size_t{}](int, nlohmann::json::parse_event_t,
                                         nlohmann::json&) mutable {
      checkImportIteration(stop, events++);
      return true;
    };
  }
  auto document = nlohmann::json::parse(bytes.begin() + begin, bytes.begin() + begin + length,
                                       std::move(checkpoint));
  checkModelImportCancellation(stop);
  return document;
}

// TinyGLTF treats every URI-less GLB buffer as BIN and rejects URI-less ASCII
// buffers. Meshopt permits these as destination placeholders. Rewrite only the
// parser's detached JSON to private one-byte reads, preserving later GLB chunks.
// The decoder receives original logical sizes separately; no destination-sized
// zero allocation is necessary before decoded views replace these references.
void replaceSourceJson(std::vector<unsigned char>& bytes,
                       const nlohmann::json& json, bool binary) {
  const auto text = json.dump();
  if (!binary) {
    if (text.size() > std::numeric_limits<unsigned int>::max())
      throw std::invalid_argument("rewritten glTF JSON exceeds the parser size limit");
    bytes.assign(text.begin(), text.end());
    return;
  }
  const uint32_t oldLength = uint32_t(bytes[12]) | (uint32_t(bytes[13]) << 8) |
      (uint32_t(bytes[14]) << 16) | (uint32_t(bytes[15]) << 24);
  const size_t padded = (text.size() + 3u) & ~size_t(3);
  const size_t suffix = 20u + oldLength;
  const uint64_t total = uint64_t(20) + padded + (bytes.size() - suffix);
  if (total > std::numeric_limits<unsigned int>::max())
    throw std::invalid_argument("rewritten GLB exceeds the parser size limit");
  std::vector<unsigned char> rewritten(static_cast<size_t>(total), ' ');
  auto word = [&](size_t offset, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
      rewritten[offset + shift / 8] = static_cast<unsigned char>(value >> shift);
  };
  word(0, 0x46546c67); word(4, 2); word(8, static_cast<uint32_t>(total));
  word(12, static_cast<uint32_t>(padded)); word(16, 0x4e4f534a);
  std::copy(text.begin(), text.end(), rewritten.begin() + 20);
  std::copy(bytes.begin() + suffix, bytes.end(), rewritten.begin() + 20 + padded);
  bytes.swap(rewritten);
}

AssetRequirements sourceRequirements(const nlohmann::json& json) {
  AssetRequirements requirements;
  const auto asset = json.find("asset");
  if (asset == json.end() || !asset->is_object())
    throw std::invalid_argument("invalid_version: asset must be an object with an asset.version string");
  const auto version = asset->find("version");
  if (version == asset->end() || !version->is_string())
    throw std::invalid_argument("invalid_version: asset.version must be a major.minor string");
  requirements.version = version->get<std::string>();
  if (const auto minimum = asset->find("minVersion"); minimum != asset->end()) {
    if (!minimum->is_string())
      throw std::invalid_argument("invalid_min_version: asset.minVersion must be a major.minor string");
    requirements.minVersion = minimum->get<std::string>();
  }
  auto extensionNames = [&](const char* field) {
    const auto names = json.find(field);
    if (names == json.end()) return std::vector<std::string>{};
    if (!names->is_array() || !std::ranges::all_of(*names, &nlohmann::json::is_string))
      throw std::invalid_argument(std::string("invalid_extension_declaration: ") + field +
                                  " must be an array of extension name strings");
    return names->get<std::vector<std::string>>();
  };
  requirements.extensionsUsed = extensionNames("extensionsUsed");
  requirements.extensionsRequired = extensionNames("extensionsRequired");
  return requirements;
}

bool omitUnavailableOptionalExtensions(nlohmann::json& document,
    AssetRequirements& requirements, std::span<const std::string> unavailable,
    bool composing, const std::stop_token& stop) {
  if (!composing && unavailable.empty()) return false;
  const std::set<std::string_view> required(requirements.extensionsRequired.begin(),
                                             requirements.extensionsRequired.end());
  std::set<std::string> omitted;
  for (const auto& name : requirements.extensionsUsed) {
    if (required.contains(name)) continue;
    const bool unavailableInConsumer = std::ranges::find(unavailable, name) != unavailable.end();
    const bool opaqueCompositionPayload = composing && !GltfMergeSupportsExtension(name) &&
        !IsGeometryCompressionExtension(name);
    if (unavailableInConsumer || opaqueCompositionPayload) omitted.insert(name);
  }
  if (omitted.empty()) return false;

  std::erase_if(requirements.extensionsUsed, [&](const auto& name) { return omitted.contains(name); });
  document["extensionsUsed"] = std::move(requirements.extensionsUsed);

  // Work on the detached source JSON, not the archive. Remove only the
  // declared extensions; extras may contain arbitrary application metadata.
  std::vector<nlohmann::json*> pending{&document};
  size_t iteration = 0;
  while (!pending.empty()) {
    checkImportIteration(stop, iteration++);
    auto& value = *pending.back();
    pending.pop_back();
    if (value.is_object()) {
      if (const auto extensions = value.find("extensions"); extensions != value.end() && extensions->is_object()) {
        for (auto entry = extensions->begin(); entry != extensions->end();) {
          if (omitted.contains(entry.key())) entry = extensions->erase(entry);
          else ++entry;
        }
      }
      for (auto entry = value.begin(); entry != value.end(); ++entry)
        if (entry.key() != "extras" && entry->is_structured()) pending.push_back(&*entry);
    } else if (value.is_array()) {
      for (auto& entry : value)
        if (entry.is_structured()) pending.push_back(&entry);
    }
  }
  return true;
}

// Keep hierarchy indices in their original JSON representation until they are
// checked. TinyGLTF stores them as int, so validating its model alone cannot
// detect an oversized source index that narrowed into an otherwise valid one.
// This runs while reading the source document, before any resource callback.
void validateSceneHierarchyJson(const nlohmann::json& json, const std::stop_token& stop) {
  constexpr size_t maxScenes = 65536;
  constexpr size_t maxNodes = 1'000'000;
  constexpr size_t maxRootReferences = 1'000'000;
  const auto fail = [](const std::string& message) {
    throw std::invalid_argument("glTF scene hierarchy: " + message);
  };
  const auto nodes = json.find("nodes");
  const auto scenes = json.find("scenes");
  if (nodes != json.end() && !nodes->is_array()) fail("nodes must be an array");
  if (scenes != json.end() && !scenes->is_array()) fail("scenes must be an array");
  const size_t nodeCount = nodes == json.end() ? 0 : nodes->size();
  const size_t sceneCount = scenes == json.end() ? 0 : scenes->size();
  if (nodeCount > maxNodes) fail("node table exceeds its count limit");
  if (sceneCount > maxScenes) fail("scene table exceeds its count limit");
  const auto checkedIndex = [&](const nlohmann::json& value, size_t count,
                                const std::string& label) -> uint32_t {
    if (!value.is_number_integer() ||
        (!value.is_number_unsigned() && value.get<int64_t>() < 0))
      fail(label + " must be a nonnegative integer index");
    const auto index = value.get<uint64_t>();
    if (index > INT_MAX || index >= count)
      fail(label + " references a missing entry or exceeds the int index limit");
    return static_cast<uint32_t>(index);
  };
  if (const auto selected = json.find("scene"); selected != json.end())
    (void)checkedIndex(*selected, sceneCount, "scene");

  std::vector<int32_t> parents(nodeCount, -1);
  size_t references = 0;
  for (size_t i = 0; i < nodeCount; ++i) {
    checkImportIteration(stop, i);
    const auto& node = nodes->at(i);
    const auto label = "nodes[" + std::to_string(i) + "]";
    if (!node.is_object()) fail(label + " must be an object");
    const auto children = node.find("children");
    if (children == node.end()) continue;
    if (!children->is_array()) fail(label + ".children must be an array");
    if (children->size() > nodeCount)
      fail(label + ".children exceeds the node count");
    for (const auto& child : *children) {
      checkImportIteration(stop, references++);
      const auto index = checkedIndex(child, nodeCount, label + ".children");
      if (parents[index] >= 0)
        fail(label + ".children repeats a node or gives it multiple parents");
      parents[index] = static_cast<int32_t>(i);
    }
  }

  // A strict forest has at most one incoming edge per node. Visiting every
  // globally parentless root therefore visits each acyclic node once. Any
  // unvisited component is a cycle, including components unused by all scenes.
  // Iteration keeps even a maximum-depth asset off the native call stack.
  std::vector<uint32_t> pending;
  pending.reserve(nodeCount);
  for (size_t i = 0; i < nodeCount; ++i) {
    checkImportIteration(stop, i);
    if (parents[i] < 0) pending.push_back(static_cast<uint32_t>(i));
  }
  for (size_t cursor = 0; cursor < pending.size(); ++cursor) {
    checkImportIteration(stop, cursor);
    const auto& node = nodes->at(pending[cursor]);
    if (const auto children = node.find("children"); children != node.end())
      for (const auto& child : *children) {
        checkImportIteration(stop, references++);
        pending.push_back(child.get<uint32_t>());
      }
  }
  if (pending.size() != nodeCount) fail("node hierarchy contains a cycle");

  size_t rootReferences = 0;
  std::vector<uint32_t> rootScene(nodeCount, UINT32_MAX);
  for (size_t i = 0; i < sceneCount; ++i) {
    checkImportIteration(stop, i);
    const auto& scene = scenes->at(i);
    const auto label = "scenes[" + std::to_string(i) + "]";
    if (!scene.is_object()) fail(label + " must be an object");
    const auto roots = scene.find("nodes");
    if (roots == scene.end()) continue;
    if (!roots->is_array()) fail(label + ".nodes must be an array");
    if (roots->size() > maxRootReferences - rootReferences)
      fail("scene roots exceed the aggregate reference limit");
    rootReferences += roots->size();
    for (const auto& root : *roots) {
      checkImportIteration(stop, references++);
      const auto index = checkedIndex(root, nodeCount, label + ".nodes");
      if (parents[index] >= 0) fail(label + ".nodes root is also a node child");
      if (rootScene[index] == i) fail(label + ".nodes repeats a scene root");
      rootScene[index] = static_cast<uint32_t>(i);
    }
  }
}

// TinyGLTF narrows JSON integers. glTF permits integral decimals and
// exponents, so normalize those before TinyGLTF interprets the fields.
std::optional<int> normalizeSourceInteger(nlohmann::json& value, bool& rewritten) {
  if (value.is_number_unsigned()) {
    const auto integer = value.get<uint64_t>();
    if (integer <= INT_MAX) return static_cast<int>(integer);
  } else if (value.is_number_integer()) {
    const auto integer = value.get<int64_t>();
    if (integer >= INT_MIN && integer <= INT_MAX) return static_cast<int>(integer);
  } else if (value.is_number_float()) {
    const auto number = value.get<double>();
    if (number >= INT_MIN && number <= INT_MAX && std::trunc(number) == number) {
      const auto integer = static_cast<int>(number);
      value = integer;
      rewritten = true;
      return integer;
    }
  }
  return std::nullopt;
}

bool normalizeTextureSamplerJson(nlohmann::json& json, const std::stop_token& stop) {
  bool rewritten = false;
  const auto samplers = json.find("samplers");
  if (samplers != json.end() && !samplers->is_array())
    throw std::invalid_argument("samplers must be an array");
  const size_t samplerCount = samplers == json.end() ? 0 : samplers->size();
  for (size_t i = 0; i < samplerCount; ++i) {
    checkImportIteration(stop, i);
    auto& sampler = samplers->at(i);
    if (!sampler.is_object())
      throw std::invalid_argument("samplers[" + std::to_string(i) + "] must be an object");
    for (const char* field : {"minFilter", "magFilter", "wrapS", "wrapT"}) {
      const auto value = sampler.find(field);
      if (value != sampler.end() && !normalizeSourceInteger(*value, rewritten))
        throw std::invalid_argument("samplers[" + std::to_string(i) + "]." + field +
                                    " must be an integer in the int range");
    }
  }
  // The image-source preflight already validated the texture array and rows.
  const auto textures = json.find("textures");
  const size_t textureCount = textures == json.end() ? 0 : textures->size();
  for (size_t i = 0; i < textureCount; ++i) {
    checkImportIteration(stop, i);
    auto& texture = textures->at(i);
    const auto sampler = texture.find("sampler");
    if (sampler == texture.end()) continue;
    const auto index = normalizeSourceInteger(*sampler, rewritten);
    if (!index || *index < 0 || static_cast<size_t>(*index) >= samplerCount)
      throw std::invalid_argument("textures[" + std::to_string(i) +
                                  "].sampler must reference an existing sampler with a nonnegative int index");
  }
  return rewritten;
}

// Validate the source extension before TinyGLTF can narrow its indices or
// drop nulls and empty containers from extension arrays and objects.
bool normalizeMaterialVariantsJson(nlohmann::json& json, const std::stop_token& stop) {
  constexpr auto extensionName = "KHR_materials_variants";
  const auto fail = [](const std::string& context, const std::string& message) {
    throw std::invalid_argument("glTF material variants: " + context + ": " + message);
  };
  const auto declares = [&](const char* field) {
    const auto names = json.find(field);
    return names != json.end() && std::ranges::any_of(*names, [&](const auto& name) {
      return name == extensionName;
    });
  };
  const auto findExtension = [&](nlohmann::json& property) -> nlohmann::json* {
    const auto extensions = property.find("extensions");
    if (extensions == property.end()) return nullptr;
    const auto extension = extensions->find(extensionName);
    return extension == extensions->end() ? nullptr : &*extension;
  };
  const auto checkProperty = [&](const nlohmann::json& property, const std::string& context) {
    if (!property.is_object()) fail(context, "must be an object");
    if (const auto extensions = property.find("extensions");
        extensions != property.end() && !extensions->is_object())
      fail(context, "extensions must be an object");
  };
  const auto requiredArray = [&](nlohmann::json& property, const char* field,
                                 const std::string& context) -> nlohmann::json& {
    const auto array = property.find(field);
    if (array == property.end() || !array->is_array() || array->empty())
      fail(context, std::string(field) + " must be a nonempty array");
    return *array;
  };
  bool rewritten = false;
  const auto checkIndex = [&](nlohmann::json& value, size_t count, const std::string& context) {
    const auto index = normalizeSourceInteger(value, rewritten);
    if (!index || *index < 0 || static_cast<size_t>(*index) >= count)
      fail(context, "must reference an existing entry with a nonnegative int index");
    return *index;
  };

  const bool declared = declares("extensionsUsed");
  auto* root = findExtension(json);
  size_t variantCount = 0;
  if (root) {
    if (!declared) fail("root", "extension is missing from extensionsUsed");
    checkProperty(*root, "root");
    const auto& variants = requiredArray(*root, "variants", "root");
    variantCount = variants.size();
    for (size_t i = 0; i < variantCount; ++i) {
      checkImportIteration(stop, i);
      const auto& variant = variants[i];
      const auto context = "variant " + std::to_string(i);
      checkProperty(variant, context);
      const auto name = variant.find("name");
      if (name == variant.end() || !name->is_string()) fail(context, "name must be a string");
    }
  }

  const auto materials = json.find("materials");
  const size_t materialCount = materials != json.end() && materials->is_array() ? materials->size() : 0;
  const auto meshes = json.find("meshes");
  const size_t meshCount = meshes != json.end() && meshes->is_array() ? meshes->size() : 0;
  for (size_t meshIndex = 0; meshIndex < meshCount; ++meshIndex) {
    checkImportIteration(stop, meshIndex);
    auto& mesh = (*meshes)[meshIndex];
    const auto primitives = mesh.find("primitives");
    if (primitives == mesh.end() || !primitives->is_array()) continue;
    for (size_t primitiveIndex = 0; primitiveIndex < primitives->size(); ++primitiveIndex) {
      checkImportIteration(stop, primitiveIndex);
      auto& primitive = (*primitives)[primitiveIndex];
      auto* extension = findExtension(primitive);
      if (!root && !extension) continue;
      const auto context = "mesh " + std::to_string(meshIndex) + " primitive " + std::to_string(primitiveIndex);
      if (const auto material = primitive.find("material"); material != primitive.end())
        checkIndex(*material, materialCount, context + " default material");
      if (!extension) continue;
      if (!declared) fail(context, "extension is missing from extensionsUsed");
      if (!root) fail(context, "primitive mappings require a root variants table");
      checkProperty(*extension, context);
      auto& mappings = requiredArray(*extension, "mappings", context);
      std::set<int> seen;
      for (size_t mappingIndex = 0; mappingIndex < mappings.size(); ++mappingIndex) {
        checkImportIteration(stop, mappingIndex);
        auto& mapping = mappings[mappingIndex];
        const auto mappingContext = context + " mapping " + std::to_string(mappingIndex);
        checkProperty(mapping, mappingContext);
        if (const auto name = mapping.find("name"); name != mapping.end() && !name->is_string())
          fail(mappingContext, "name must be a string");
        const auto material = mapping.find("material");
        if (material == mapping.end()) fail(mappingContext, "material is required");
        checkIndex(*material, materialCount, mappingContext + " material");
        auto& variants = requiredArray(mapping, "variants", mappingContext);
        for (size_t i = 0; i < variants.size(); ++i) {
          checkImportIteration(stop, i);
          const auto variant = checkIndex(variants[i], variantCount, mappingContext + " variant " + std::to_string(i));
          if (!seen.insert(variant).second)
            fail(mappingContext, "variant " + std::to_string(variant) + " is already mapped by this primitive");
        }
      }
    }
  }
  if (!root && (declared || declares("extensionsRequired")))
    fail("root", "declared extension requires a root variants table");
  return rewritten;
}

struct ParserImageReference {
  int bufferView{-1};
  std::string mimeType;
};

// Validate image views before TinyGLTF forms its unchecked buffer span. Views
// awaiting meshopt decode use private one-byte URI reads only in parser JSON.
std::vector<ParserImageReference> prepareImageReferences(
    nlohmann::json& json, std::unordered_map<std::string, uint64_t>& synthetic,
    const std::string& prefix, bool& rewritten) {
  const auto images = json.find("images");
  if (images == json.end()) return {};
  if (!images->is_array()) throw std::invalid_argument("images must be an array");
  std::vector<ParserImageReference> references(images->size());
  const auto views = json.find("bufferViews");
  for (size_t imageIndex = 0; imageIndex < images->size(); ++imageIndex) {
    auto& image = (*images)[imageIndex];
    const auto label = "image[" + std::to_string(imageIndex) + "]";
    if (!image.is_object()) throw std::invalid_argument(label + " must be an object");
    if (auto uri = image.find("uri"); uri != image.end() && uri->is_string()) {
      const auto& encodedUri = uri->get_ref<const std::string&>();
      // TinyGLTF recognizes a fixed MIME allowlist in IsDataURI. New image
      // containers must reach the same bounded image callback, with '+' bytes
      // intact, rather than being URI-decoded as external filenames.
      if (encodedUri.starts_with("data:") && !tinygltf::IsDataURI(encodedUri)) {
        const auto comma = encodedUri.find(',');
        if (comma == std::string::npos ||
            !std::string_view(encodedUri).substr(0, comma).ends_with(";base64"))
          throw std::invalid_argument(label + " embedded image URI requires base64 encoding");
        const auto payload = std::string_view(encodedUri).substr(comma + 1);
        const auto decodedUpperBound = uint64_t(payload.size() / 4) * 3 +
            (payload.size() % 4 ? 3 : 0);
        if (decodedUpperBound > container::material::TextureDecodeOptions{}.maxEncodedBytes + 2)
          throw std::invalid_argument(label + " embedded image exceeds its encoded byte budget");
        *uri = "data:application/octet-stream;base64," + std::string(payload);
        rewritten = true;
      }
    }
    const auto viewIndex = image.find("bufferView");
    if (viewIndex == image.end()) continue;
    if (image.contains("uri")) throw std::invalid_argument(label + " cannot specify both URI and bufferView");
    if (!viewIndex->is_number_integer() || (!viewIndex->is_number_unsigned() && viewIndex->get<int64_t>() < 0))
      throw std::invalid_argument(label + ".bufferView must be a nonnegative int index");
    const uint64_t number = viewIndex->get<uint64_t>();
    if (number > INT_MAX || views == json.end() || !views->is_array() || number >= views->size())
      throw std::invalid_argument(label + ".bufferView references a missing view");
    if (!image.contains("mimeType") || !image["mimeType"].is_string() || image["mimeType"].get<std::string>().empty())
      throw std::invalid_argument(label + " bufferView requires a MIME type");
    const auto& view = views->at(size_t(number));
    const auto nonnegative = [&](const nlohmann::json& value, std::string_view field) {
      if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0))
        throw std::invalid_argument(label + " " + std::string(field) + " must be a nonnegative integer");
      return value.get<uint64_t>();
    };
    const auto buffers = json.find("buffers");
    const auto buffer = nonnegative(view.at("buffer"), "view.buffer");
    if (buffer > INT_MAX || buffers == json.end() || !buffers->is_array() || buffer >= buffers->size())
      throw std::invalid_argument(label + " view references a missing buffer");
    const auto declared = nonnegative(buffers->at(size_t(buffer)).at("byteLength"), "buffer.byteLength");
    const auto offset = view.contains("byteOffset") ? nonnegative(view["byteOffset"], "view.byteOffset") : 0;
    const auto bytes = nonnegative(view.at("byteLength"), "view.byteLength");
    if (offset > declared || bytes > declared - offset)
      throw std::invalid_argument(label + " view byte range exceeds its declared buffer");
    if (!bytes || bytes > INT_MAX)
      throw std::invalid_argument(label + " encoded image exceeds the parser's positive int span limit");
    const auto extensions = view.find("extensions");
    const bool compressed = HasMeshoptDecoder() && extensions != view.end() && extensions->is_object() &&
        (extensions->contains("EXT_meshopt_compression") || extensions->contains("KHR_meshopt_compression"));
    if (!compressed) continue;
    references[imageIndex] = {int(number), image["mimeType"].get<std::string>()};
    const auto uri = prefix + "image_" + std::to_string(imageIndex) + ".bin";
    synthetic.emplace(uri, 1);
    image.erase("bufferView");
    image["uri"] = uri;
    rewritten = true;
  }
  return references;
}

uintmax_t preflightBoundedGltf(const nlohmann::json& json, uintmax_t sourceBytes,
                             uintmax_t budget, uint64_t decoderBytes) {
  uintmax_t total = sourceBytes;
  auto add = [&](uintmax_t count, uintmax_t elementSize) {
    if (count > (budget - total) / elementSize)
      throw std::invalid_argument("glTF buffers and geometry exceed the input-memory budget");
    total += count * elementSize;
  };
  add(decoderBytes, 1);
  if (json.contains("buffers"))
    for (const auto& buffer : json.at("buffers"))
      add(buffer.at("byteLength").get<uintmax_t>(), 1);
  bool hasDeformation = json.contains("skins") && !json.at("skins").empty();
  if (json.contains("meshes"))
    for (const auto& mesh : json.at("meshes"))
      for (const auto& primitive : mesh.at("primitives"))
        hasDeformation |= primitive.contains("targets") && !primitive.at("targets").empty();
  if (json.contains("meshes")) {
    const auto& accessors = json.at("accessors");
    for (const auto& mesh : json.at("meshes"))
      for (const auto& primitive : mesh.at("primitives")) {
        const auto position = primitive.at("attributes").at("POSITION").get<size_t>();
        const auto count = accessors.at(position).at("count").get<uintmax_t>();
        add(count, sizeof(Vertex));
        const auto indexCount = primitive.contains("indices")
            ? accessors.at(primitive.at("indices").get<size_t>()).at("count").get<uintmax_t>()
            : count;
        add(indexCount, sizeof(uint32_t));
        if (hasDeformation) {
          // The deformation reference retains bind vertices independently of
          // the flattened render geometry. Bound its decoded arrays too.
          add(count, sizeof(Vertex));
          add(1, sizeof(DeformationPrimitive));
          const auto& attributes = primitive.at("attributes");
          if (attributes.contains("JOINTS_0")) {
            add(count, sizeof(VertexSkinInfluences));
            for (const auto& [name, _] : attributes.items())
              if (name.starts_with("JOINTS_"))
                add(count, 4 * sizeof(JointInfluence));
          }
          if (primitive.contains("targets")) {
            add(primitive.at("targets").size(), sizeof(MorphTarget) + sizeof(float));
            for (const auto& target : primitive.at("targets"))
              for (const char* semantic : {"POSITION", "NORMAL", "TANGENT"})
                if (target.contains(semantic)) add(count, sizeof(glm::vec3));
          }
        }
      }
  }
  if (hasDeformation) {
    if (json.contains("nodes")) {
      add(json.at("nodes").size(), sizeof(DeformationNode));
      for (const auto& node : json.at("nodes")) {
        if (node.contains("weights")) add(node.at("weights").size(), sizeof(float));
        else if (node.contains("mesh")) {
          const auto& mesh = json.at("meshes").at(node.at("mesh").get<size_t>());
          if (mesh.contains("weights")) add(mesh.at("weights").size(), sizeof(float));
          else if (!mesh.at("primitives").empty() && mesh.at("primitives").front().contains("targets"))
            add(mesh.at("primitives").front().at("targets").size(), sizeof(float));
        }
      }
    }
    if (json.contains("skins")) {
      add(json.at("skins").size(), sizeof(DeformationSkin));
      for (const auto& skin : json.at("skins"))
        add(skin.at("joints").size(), sizeof(uint32_t) + sizeof(glm::mat4));
    }
  }
  if (json.contains("animations")) {
    const auto& accessors = json.at("accessors");
    for (const auto& animation : json.at("animations"))
      for (const auto& channel : animation.at("channels")) {
        const auto& sampler = animation.at("samplers").at(channel.at("sampler").get<size_t>());
        for (const char* field : {"input","output"}) {
          const auto& accessor = accessors.at(sampler.at(field).get<size_t>());
          const auto type = accessor.at("type").get<std::string>();
          const uintmax_t components = type == "SCALAR" ? 1 : type == "VEC2" ? 2 :
              type == "VEC3" ? 3 : type == "VEC4" || type == "MAT2" ? 4 : type == "MAT3" ? 9 : type == "MAT4" ? 16 : 0;
          if (!components) throw std::invalid_argument("invalid animation accessor shape in input-memory preflight");
          // Tracks retain independent binary64 arrays per channel, even when
          // channels share one sampler or the source uses a sparse zero base.
          add(accessor.at("count").get<uintmax_t>(),components*sizeof(double));
        }
      }
  }
  return total;
}

void discardOpacityMicromapMetadata(tinygltf::Model& model);

void discardOpacityMicromapMetadata(nlohmann::json& document) {
  const auto discard = [](nlohmann::json& record) {
    if (!record.is_object()) return;
    const auto extensions = record.find("extensions");
    if (extensions != record.end() && extensions->is_object())
      extensions->erase(std::string(kGltfOpacityMicromapExtension));
  };
  discard(document);
  const auto meshes = document.find("meshes");
  if (meshes == document.end() || !meshes->is_array()) return;
  for (auto& mesh : *meshes) {
    if (!mesh.is_object()) continue;
    const auto primitives = mesh.find("primitives");
    if (primitives != mesh.end() && primitives->is_array())
      for (auto& primitive : *primitives) discard(primitive);
  }
}

tinygltf::Model loadGltfModel(const std::string& path,
                             const LoadOptions& options = {},
                             std::vector<CapabilityDiagnostic>* diagnostics = nullptr,
                             const GltfLinkedReadContext* linkedContext = nullptr,
                             GltfLinkedManifest* linkedManifest = nullptr,
                             uintmax_t* linkedBudget = nullptr) {
  const auto& stop = options.stop;
  checkModelImportCancellation(stop);
  tinygltf::TinyGLTF loader;
  tinygltf::Model model;
  std::string err;
  std::string warn;
  if (!options.maxInputBytes)
    throw std::invalid_argument("glTF asset '" + path + "': input budget must be positive");
  ImageSourcePlan imageSources;
  std::vector<ParserImageReference> parserImageReferences;
  std::vector<std::vector<unsigned char>> stagedImages;
  uintmax_t remainingBytes = options.maxInputBytes;
  uintmax_t imageAllocationBudget = options.maxInputBytes;
  CompressionSourcePlan compressionPlan;
  std::optional<GltfXmpImport> xmp;
  struct LinkedResource { std::filesystem::path path; std::string originalUri; };
  std::unordered_map<std::string, LinkedResource> linkedResources;
  const auto linkedResource = [&](const std::string& filename) -> const LinkedResource* {
    const auto basename = filename.substr(filename.find_last_of("/\\") + 1);
    const auto resource = linkedResources.find(basename);
    return resource == linkedResources.end() ? nullptr : &resource->second;
  };
  std::unordered_map<std::string, uint64_t> syntheticBuffers;
  auto syntheticSize = [&](const std::string& filename) -> std::optional<uint64_t> {
    const auto name = std::filesystem::path(
        std::u8string(filename.begin(), filename.end())).filename().string();
    const auto found = syntheticBuffers.find(name);
    if (found == syntheticBuffers.end()) return std::nullopt;
    return found->second;
  };
  loader.SetImageLoader(
      [&](tinygltf::Image* image, int imageIndex, std::string* error,
          std::string* warning, int requiredWidth, int requiredHeight,
          const unsigned char* bytes, int size, void*) {
        checkModelImportCancellation(stop);
        (void)image; (void)warning; (void)requiredWidth; (void)requiredHeight;
        if (!options.decodeImages) return true;
        if (imageIndex < 0 || size_t(imageIndex) >= stagedImages.size()) {
          if (error) *error = "image callback has an invalid source index";
          return false;
        }
        if (parserImageReferences[size_t(imageIndex)].bufferView >= 0) return true;
        // Decode only after detached compression normalization. In particular,
        // no callback may inspect logical bytes in a one-byte meshopt stand-in.
        if (size <= 0 || uint64_t(size) > container::material::TextureDecodeOptions{}.maxEncodedBytes) {
          if (error) *error = "image[" + std::to_string(imageIndex) + "] encoded bytes are empty or exceed the image limit";
          return false;
        }
        // URI decoding owns a temporary raw vector in addition to our retained
        // copy. Buffer-view bytes were already reserved by source preflight.
        const uintmax_t copies = image->bufferView >= 0 ? 1 : 2;
        if (uintmax_t(size) > imageAllocationBudget / copies) {
          if (error) *error = "image[" + std::to_string(imageIndex) + "] retained bytes exceed the aggregate input budget";
          return false;
        }
        imageAllocationBudget -= uintmax_t(size) * copies;
        stagedImages[size_t(imageIndex)].assign(bytes, bytes + size);
        return true;
      }, nullptr);
  bool sourceRead = false;
  {
    tinygltf::FsCallbacks callbacks{
        [&](const std::string& filename, void*) {
          checkModelImportCancellation(stop);
          if (options.requireSelfContained && sourceRead)
            return syntheticSize(filename).has_value();
          if (linkedContext && sourceRead)
            return syntheticSize(filename).has_value() || linkedResource(filename) != nullptr;
          return syntheticSize(filename).has_value() || tinygltf::FileExists(filename, nullptr);
        }, tinygltf::ExpandFilePath,
        [&](std::vector<unsigned char>* output, std::string* error,
            const std::string& filename, void*) {
          checkModelImportCancellation(stop);
          if (const auto placeholder = syntheticSize(filename)) {
            if (*placeholder > remainingBytes || *placeholder > std::numeric_limits<size_t>::max()) {
              if (error) *error = "meshopt placeholder exceeds the aggregate input budget";
              return false;
            }
            output->assign(static_cast<size_t>(*placeholder), 0);
            remainingBytes -= *placeholder;
            return true;
          }
          if (options.requireSelfContained && sourceRead) {
            if (error) *error = "self-contained glTF cannot read external resources";
            return false;
          }
          const auto* resource = linkedContext && sourceRead ? linkedResource(filename) : nullptr;
          if (linkedContext && sourceRead && !resource) {
            if (error) *error = "unmapped external URI in linked glTF";
            return false;
          }
          const auto filePath = resource ? resource->path : linkedContext ? linkedContext->source :
              std::filesystem::path(std::u8string(filename.begin(), filename.end()));
          std::error_code code;
          const uintmax_t bytes = std::filesystem::file_size(filePath, code);
          if (code || bytes > remainingBytes ||
              bytes > std::numeric_limits<size_t>::max() ||
              (!sourceRead && bytes > std::numeric_limits<unsigned int>::max()) ||
              bytes > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
            if (error) *error = "glTF resource exceeds the aggregate encoded input budget";
            return false;
          }
          std::ifstream file(filePath, std::ios::binary);
          output->clear();
          output->reserve(static_cast<size_t>(bytes));
          constexpr size_t readChunkBytes = 1024 * 1024;
          for (size_t offset = 0; offset < bytes && file;) {
            checkModelImportCancellation(stop);
            const auto chunkBytes = std::min(readChunkBytes, static_cast<size_t>(bytes) - offset);
            output->resize(offset + chunkBytes);
            file.read(reinterpret_cast<char*>(output->data() + offset),
                      static_cast<std::streamsize>(chunkBytes));
            offset += chunkBytes;
          }
          checkModelImportCancellation(stop);
          if (!file || file.peek() != std::char_traits<char>::eof()) {
            if (error) *error = "glTF resource changed size or could not be read";
            return false;
          }
          remainingBytes -= bytes;
          auto* linkedComposition = linkedContext ? linkedContext->composition : nullptr;
          if (sourceRead && linkedComposition && !linkedComposition->source.empty())
            linkedContext->observe(filePath, *output);
          if (!sourceRead) {
            try {
              auto json = parseSourceJson(*output, isBinaryGltf(path), stop);
              if (options.requireSelfContained) {
                for (const auto* table : {"buffers", "images"}) {
                  const auto records = json.find(table);
                  if (records == json.end() || !records->is_array()) continue;
                  for (const auto& record : *records) {
                    const auto uri = record.find("uri");
                    if (uri == record.end() || !uri->is_string()) continue;
                    const auto& value = uri->get_ref<const std::string&>();
                    if (!value.empty() && !value.starts_with("data:"))
                      throw std::invalid_argument(std::string("self-contained glTF cannot reference external ") + table);
                  }
                }
              }
              checkModelImportCancellation(stop);
              auto linked = PrepareGltfLinkedManifest(json, linkedContext != nullptr, stop);
              if (linkedComposition) {
                if (!linked.nodeFiles.empty() && linkedComposition->source.empty())
                  linkedComposition->source = filePath;
                if (!linkedComposition->source.empty()) linkedContext->observe(filePath, *output);
              }
              const bool normalizedLinkedManifest = linked.normalized;
              const bool linkedChild = linkedContext && linkedContext->child;
              const bool composing = options.mergeInput || linkedChild ||
                  std::ranges::any_of(linked.nodeFiles, [](int file) { return file >= 0; });
              if (normalizedLinkedManifest) {
                if (!linkedManifest || !options.decodeImages)
                  throw std::invalid_argument("linked glTF requires retained image and scene loading");
                *linkedManifest = std::move(linked);
              }
              std::vector<std::string_view> experimental;
              if (options.enableExperimentalMaterials)
                experimental.assign(kGltfExperimentalMaterialExtensions.begin(),
                                    kGltfExperimentalMaterialExtensions.end());
              if (options.enableOpacityMicromaps)
                experimental.push_back(kGltfOpacityMicromapExtension);
              auto unavailable = options.unavailableRuntimeExtensions;
              if (composing) {
                for (const auto& capability : extensionCapabilities()) {
                  if (!GltfMergeSupportsExtension(capability.name) &&
                      !IsGeometryCompressionExtension(capability.name)) {
                    unavailable.emplace_back(capability.name);
                  }
                }
              }
              if (linkedChild) unavailable.emplace_back(kGltfInteractivityExtension);
              auto requirements = sourceRequirements(json);
              auto validation = validateCapabilities(requirements, path, unavailable, experimental,
                                                     options.compression.dracoWorkerExecutable);
              if (normalizedLinkedManifest)
                validation.diagnostics.push_back({CapabilitySeverity::Warning, "experimental_linked_assets", path,
                    "Using the experimental URI-only glTF 2.1 linked asset profile"});
              std::string errors;
              for (const auto& diagnostic : validation.diagnostics) {
                if (diagnostic.severity == CapabilitySeverity::Error) {
                  if (!errors.empty()) errors += '\n';
                  errors += formatCapabilityDiagnostic(diagnostic);
                } else {
                  std::clog << formatCapabilityDiagnostic(diagnostic) << std::endl;
                }
              }
              if (diagnostics) *diagnostics = std::move(validation.diagnostics);
              if (!errors.empty()) throw std::runtime_error(errors);
              // Plan from the original image references so alternate-only
              // payloads stay distinguishable from ordinary unused images.
              const auto unavailableInConsumer = [&](std::string_view name) {
                return std::ranges::find(unavailable, name) != unavailable.end();
              };
              auto imageCapabilities = DefaultImageSourceCapabilities();
              imageCapabilities.basisu &= !unavailableInConsumer("KHR_texture_basisu");
              imageCapabilities.webp &= !unavailableInConsumer("EXT_texture_webp");
              imageCapabilities.dds &= !unavailableInConsumer("MSFT_texture_dds");
              imageSources = SelectTextureImageSources(json, imageCapabilities);
              const bool normalizedSamplerFields = normalizeTextureSamplerJson(json, stop);
              const bool omittedOptional = omitUnavailableOptionalExtensions(
                  json, requirements, unavailable, composing, stop);
              const bool normalizedVariantIndices = normalizeMaterialVariantsJson(json, stop);
              bool discardedMicromaps = false;
              if (options.enableOpacityMicromaps) {
                if (const auto invalid = ValidateGltfOpacityMicromapSourceJson(json)) {
                  CapabilityDiagnostic diagnostic{CapabilitySeverity::Warning, "opacity_micromap_fallback", path,
                      invalid->context + ": " + invalid->message + "; using sampled alpha"};
                  std::clog << formatCapabilityDiagnostic(diagnostic) << std::endl;
                  if (diagnostics) diagnostics->push_back(std::move(diagnostic));
                  discardOpacityMicromapMetadata(json);
                  discardedMicromaps = true;
                }
              }
              validateSceneHierarchyJson(json, stop);
              const auto nodeFlags = ValidateGltfNodeFlagsJson(json);
              const auto extendedMaterials = ValidateGltfExtendedMaterialsJson(json);
              const auto experimentalMaterialBytes = options.enableExperimentalMaterials
                  ? ValidateGltfExperimentalMaterialsJson(json) : uint64_t{};
              bool normalizedInstancingFields = false;
              const auto instancing = NormalizeMeshGpuInstancingJson(
                  json, normalizedInstancingFields, options.instancing, stop);
              xmp.emplace(json);
              for (const auto& warning : xmp->warnings()) {
                CapabilityDiagnostic diagnostic{CapabilitySeverity::Warning, "xmp_metadata", path, warning};
                std::clog << formatCapabilityDiagnostic(diagnostic) << std::endl;
                if (diagnostics) diagnostics->push_back(std::move(diagnostic));
              }
              {
                static std::atomic<uint64_t> sequence{};
                const std::string prefix = "__container_gltf_" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                    std::to_string(sequence.fetch_add(1)) + "_";
                bool rewritten = xmp->rewroteManifest() || normalizedLinkedManifest ||
                    normalizedSamplerFields || normalizedVariantIndices || normalizedInstancingFields ||
                    discardedMicromaps || omittedOptional;
                if (linkedContext) {
                  for (const auto* table : {"buffers", "images"}) {
                    auto records = json.find(table);
                    if (records == json.end() || !records->is_array()) continue;
                    for (auto& record : *records) {
                      auto uri = record.find("uri");
                      if (uri == record.end() || !uri->is_string()) continue;
                      const auto& text = uri->get_ref<const std::string&>();
                      if (text.starts_with("data:")) continue;
                      const auto name = prefix + "linked_" + std::to_string(linkedResources.size()) + ".bin";
                      linkedResources.emplace(name, LinkedResource{linkedContext->resource(text), text});
                      *uri = name;
                      rewritten = true;
                    }
                  }
                }
                parserImageReferences = prepareImageReferences(json, syntheticBuffers, prefix, rewritten);
                stagedImages.resize(parserImageReferences.size());
                compressionPlan = InspectCompressedGltfSource(json, isBinaryGltf(path), options.compression, stop);
                uint64_t retainedBytes = compressionPlan.decodedBytes;
                for (const uint64_t bytes : {instancing.decodedBytes, nodeFlags.retainedBytes,
                                            extendedMaterials.retainedBytes, experimentalMaterialBytes,
                                            uint64_t(xmp->retainedBytes())}) {
                  if (bytes > UINT64_MAX - retainedBytes)
                    throw std::invalid_argument("glTF decoded/instance/node/material output byte count overflows");
                  retainedBytes += bytes;
                }
                if (options.maxInputBytes != std::numeric_limits<uintmax_t>::max())
                  imageAllocationBudget = options.maxInputBytes -
                      preflightBoundedGltf(json, bytes, options.maxInputBytes,
                          retainedBytes);
                for (const auto& placeholder : compressionPlan.placeholders) {
                  const auto uri = prefix + std::to_string(placeholder.bufferIndex) + ".bin";
                  syntheticBuffers.emplace(uri, 1);
                  json.at("buffers").at(placeholder.bufferIndex)["uri"] = uri;
                  json.at("buffers").at(placeholder.bufferIndex)["byteLength"] = 1;
                  rewritten = true;
                }
                checkModelImportCancellation(stop);
                if (rewritten) replaceSourceJson(*output, json, isBinaryGltf(path));
              }
              checkModelImportCancellation(stop);
            } catch (const ModelImportCancelled&) {
              throw;
            } catch (const nlohmann::json::exception& exception) {
              throw gltfError("glTF asset '" + path + "'", exception.what());
            } catch (const std::invalid_argument& exception) {
              throw gltfError("glTF asset '" + path + "'", exception.what());
            } catch (const std::runtime_error& exception) {
              throw gltfError("glTF asset '" + path + "'", exception.what());
            }
            sourceRead = true;
          }
          return true;
        }, tinygltf::WriteWholeFile,
        [&](size_t* size, std::string* error, const std::string& filename, void*) {
          checkModelImportCancellation(stop);
          if (const auto placeholder = syntheticSize(filename)) {
            if (*placeholder > remainingBytes || *placeholder > std::numeric_limits<size_t>::max()) {
              if (error) *error = "meshopt placeholder exceeds the aggregate input budget";
              return false;
            }
            *size = static_cast<size_t>(*placeholder);
            return true;
          }
          if (options.requireSelfContained && sourceRead) {
            if (error) *error = "self-contained glTF cannot read external resources";
            return false;
          }
          if (linkedContext && sourceRead) {
            const auto* resource = linkedResource(filename);
            if (!resource) {
              if (error) *error = "unmapped external URI in linked glTF";
              return false;
            }
            std::error_code code;
            const auto bytes = std::filesystem::file_size(resource->path, code);
            if (code || bytes > remainingBytes || bytes > std::numeric_limits<size_t>::max()) {
              if (error) *error = "linked resource exceeds the aggregate encoded input budget";
              return false;
            }
            *size = size_t(bytes);
            return true;
          }
          return tinygltf::GetFileSizeInBytes(size, error, filename, nullptr);
        }, nullptr};
    if (!loader.SetFsCallbacks(std::move(callbacks), &err))
      throw gltfError("glTF asset '" + path + "'", "Cannot configure glTF input: " + err);
    loader.SetMaxExternalFileSize(static_cast<size_t>(std::min<uintmax_t>(
        options.maxInputBytes, std::numeric_limits<size_t>::max())));
  }

  bool loaded = false;
  if (isBinaryGltf(path)) {
    loaded = loader.LoadBinaryFromFile(&model, &err, &warn, path);
  } else {
    loaded = loader.LoadASCIIFromFile(&model, &err, &warn, path);
  }
  checkModelImportCancellation(stop);

  if (!warn.empty()) {
    CapabilityDiagnostic diagnostic{CapabilitySeverity::Warning, "parser_warning", path, warn};
    std::clog << formatCapabilityDiagnostic(diagnostic) << std::endl;
    if (diagnostics) diagnostics->push_back(std::move(diagnostic));
  }
  if (!loaded) {
    throw gltfError("glTF asset '" + path + "'", "Failed to load glTF file: " + err);
  }

  for (const auto& placeholder : compressionPlan.placeholders)
    model.buffers.at(placeholder.bufferIndex).uri.clear();
  try {
    if (model.images.size() != parserImageReferences.size())
      throw std::runtime_error("parser image table differs from source preflight");
    for (size_t image = 0; image < model.images.size(); ++image) {
      checkImportIteration(stop, image);
      const auto& reference = parserImageReferences[image];
      if (reference.bufferView < 0) continue;
      model.images[image].bufferView = reference.bufferView;
      model.images[image].mimeType = reference.mimeType;
      model.images[image].uri.clear();
    }
    CompressionStatistics compressionStatistics;
    model = DecodeCompressedGltf(std::move(model), options.compression, &compressionStatistics,
                                compressionPlan.placeholders, stop, compressionPlan.dracoAvailable);
    checkModelImportCancellation(stop);
    if (compressionStatistics.dracoPrimitives || compressionStatistics.meshoptViews ||
        compressionStatistics.fallbackPrimitives || compressionStatistics.fallbackViews)
      discardOpacityMicromapMetadata(model);
    if (linkedBudget) *linkedBudget = remainingBytes;
    for (auto& buffer : model.buffers)
      if (const auto* resource = linkedResource(buffer.uri)) buffer.uri = resource->originalUri;
    for (auto& image : model.images)
      if (const auto* resource = linkedResource(image.uri)) image.uri = resource->originalUri;
    (void)ParseMeshGpuInstancing(model, options.instancing, stop);
    (void)ParseGltfNodeFlags(model);
    const bool iesRequired = std::ranges::find(model.extensionsRequired, kGltfIesExtension) != model.extensionsRequired.end();
    for (const auto& message : PrepareGltfIesProfiles(model, std::filesystem::u8path(path).parent_path(),
             !options.requireSelfContained && !linkedContext, remainingBytes)) {
      if (iesRequired) throw std::invalid_argument(message);
      CapabilityDiagnostic diagnostic{CapabilitySeverity::Warning, "unsupported_ies_profile", path, message};
      std::clog << formatCapabilityDiagnostic(diagnostic) << std::endl;
      if (diagnostics) diagnostics->push_back(std::move(diagnostic));
    }
    try {
      ValidateGltfInteractivity(model);
    } catch (const std::invalid_argument& error) {
      if (std::ranges::find(model.extensionsRequired, kGltfInteractivityExtension) != model.extensionsRequired.end())
        throw;
      if (diagnostics)
        diagnostics->push_back({CapabilitySeverity::Warning, "unsupported_interactivity", path, error.what()});
    }
    (void)ParseGltfExtendedMaterials(model);
    if (xmp) std::move(*xmp).retain(model);
    checkModelImportCancellation(stop);
    if (!options.decodeImages) return model;
    // Sponza's ordinary source images total about272MiB of decoded RGBA.
    // Keep a finite asset-wide default while retaining per-image codec bounds.
    uintmax_t remainingDecodedImages = std::min<uintmax_t>(options.maxInputBytes, 512ull * 1024 * 1024);
    std::vector<bool> selectedAlternate(model.images.size());
    for (const auto& selected : imageSources.textures)
      if (selected.kind != TextureImageSourceKind::Core) selectedAlternate[size_t(selected.imageIndex)] = true;
    for (size_t imageIndex = 0; imageIndex < model.images.size(); ++imageIndex) {
      checkModelImportCancellation(stop);
      auto& image = model.images[imageIndex];
      auto& encoded = stagedImages[imageIndex];
      const auto label = "image[" + std::to_string(imageIndex) + "]";
      if (parserImageReferences[imageIndex].bufferView >= 0) {
        const auto& view = model.bufferViews.at(size_t(image.bufferView));
        if (view.buffer < 0 || size_t(view.buffer) >= model.buffers.size())
          throw std::runtime_error(label + " decoded view references a missing buffer");
        const auto& data = model.buffers[size_t(view.buffer)].data;
        if (view.byteOffset > data.size() || view.byteLength > data.size() - view.byteOffset ||
            !view.byteLength || view.byteLength > INT_MAX || view.byteLength > imageAllocationBudget ||
            view.byteLength > container::material::TextureDecodeOptions{}.maxEncodedBytes)
          throw std::runtime_error(label + " decoded image view range exceeds its buffer or input budget");
        imageAllocationBudget -= view.byteLength;
        encoded.assign(data.begin() + view.byteOffset, data.begin() + view.byteOffset + view.byteLength);
      }
      if (encoded.empty()) {
        // TinyGLTF deliberately treats missing external images as nonfatal.
        // Selected sources must instead fail before any scene publication.
        if (imageSources.selectedImages[imageIndex])
          throw std::runtime_error(label + " selected texture image could not be read or is empty");
        continue;
      }
      if (!options.retainEncodedImages && imageSources.unsupportedAlternateOnly[imageIndex]) continue;
      if (options.retainEncodedImages || selectedAlternate[imageIndex] || RequiresEncodedTextureRetention(encoded)) {
        if (const auto mime = EncodedTextureMimeType(encoded); !mime.empty()) image.mimeType = std::string(mime);
        image.image = std::move(encoded);
        image.as_is = true;
        image.width = image.height = image.component = image.bits = image.pixel_type = -1;
        continue;
      }
      int width{}, height{}, components{};
      if (stbi_info_from_memory(encoded.data(), int(encoded.size()), &width, &height, &components)) {
        const auto limits = container::material::TextureDecodeOptions{};
        if (width <= 0 || height <= 0 || uint32_t(width) > limits.maxDimension || uint32_t(height) > limits.maxDimension)
          throw std::runtime_error(label + " decoded image dimensions exceed the image limit");
        const uintmax_t decodedBytes = uintmax_t(width) * uintmax_t(height) * 4 *
            (stbi_is_16_bit_from_memory(encoded.data(), int(encoded.size())) ? 2 : 1);
        if (decodedBytes > limits.maxDecodedBytes || decodedBytes > remainingDecodedImages || decodedBytes > imageAllocationBudget)
          throw std::runtime_error(label + " decoded image bytes exceed the aggregate image/input budget");
        remainingDecodedImages -= decodedBytes;
        imageAllocationBudget -= decodedBytes;
      }
      std::string imageError, imageWarning;
      if (!tinygltf::LoadImageData(&image, int(imageIndex), &imageError, &imageWarning,
                                 std::max(image.width, 0), std::max(image.height, 0),
                                 encoded.data(), int(encoded.size()), nullptr))
        throw std::runtime_error(label + ": " + imageError);
    }
    checkModelImportCancellation(stop);
    return model;
  } catch (const ModelImportCancelled&) {
    throw;
  } catch (const std::exception& exception) {
    throw gltfError("glTF asset '" + path + "'", exception.what());
  }
}

void discardOpacityMicromapMetadata(tinygltf::Model& model) {
  const std::string extension(kGltfOpacityMicromapExtension);
  if (model.extensions.erase(extension) && !model.extensions.contains(kGltfXmpExtension))
    model.extensions_json_string.clear();
  std::erase(model.extensionsUsed, extension);
  std::erase(model.extensionsRequired, extension);
  for (auto& mesh : model.meshes)
    for (auto& primitive : mesh.primitives)
      if (primitive.extensions.erase(extension)) primitive.extensions_json_string.clear();
}

tinygltf::Model loadGltfComposition(const std::string& path, const LoadOptions& options,
                                  std::vector<CapabilityDiagnostic>* diagnostics = nullptr,
                                  GltfLinkedCompositionInfo* composition = nullptr) {
  checkModelImportCancellation(options.stop);
  if (options.requireSelfContained && options.enableLinkedAssets)
    throw gltfError("glTF asset '" + path + "'", "self-contained loading cannot enable linked assets");
  if (!options.enableLinkedAssets) return loadGltfModel(path, options, diagnostics);
  uintmax_t remaining = std::min<uintmax_t>(options.maxInputBytes, 512ull * 1024 * 1024);
  const auto reader = [&](const GltfLinkedReadContext& context) {
    if (!remaining) throw std::runtime_error("glTF linked assets exceed the aggregate encoded input budget");
    auto bounded = options;
    bounded.maxInputBytes = remaining;
    GltfLinkedDocument result;
    std::vector<CapabilityDiagnostic> documentDiagnostics;
    const auto source = context.source.generic_u8string();
    result.model = loadGltfModel(std::string(source.begin(), source.end()), bounded,
        diagnostics ? &documentDiagnostics : nullptr, &context, &result.manifest, &remaining);
    if (diagnostics)
      diagnostics->insert(diagnostics->end(), std::make_move_iterator(documentDiagnostics.begin()),
                          std::make_move_iterator(documentDiagnostics.end()));
    return result;
  };
  return LoadGltfLinkedComposition(std::filesystem::path(std::u8string(path.begin(), path.end())),
                                   reader, composition, options.stop);
}

}  // namespace

LoadOptions StaticGeometryLoadOptions() {
  constexpr std::array<std::string_view, 5> geometryExtensions{
      "KHR_draco_mesh_compression", "EXT_meshopt_compression",
      "KHR_meshopt_compression", "KHR_mesh_quantization", "KHR_accessor_float64",
  };
  LoadOptions options;
  options.decodeImages = false;
  for (const auto& capability : extensionCapabilities()) {
    if (capability.support != ExtensionSupport::Unsupported &&
        std::ranges::find(geometryExtensions, capability.name) == geometryExtensions.end()) {
      options.unavailableRuntimeExtensions.emplace_back(capability.name);
    }
  }
  return options;
}

tinygltf::Model LoadSourceModel(const std::string& path,
                               const LoadOptions& options) {
  return loadGltfComposition(path, options);
}

Model LoadModelFromFile(const std::string& path) {
  return LoadModelWithSource(path).model;
}

GltfLoadResult LoadModelWithSource(const std::string& path) {
  return LoadModelWithSource(path, {});
}

GltfLoadResult LoadModelWithSource(const std::string& path,
                                  const LoadOptions& options) {
  std::vector<CapabilityDiagnostic> diagnostics;
  auto linked = options.enableLinkedAssets ? std::make_shared<GltfLinkedCompositionInfo>() : nullptr;
  auto gltfModel = loadGltfComposition(path, options, &diagnostics, linked.get());
  try {
    checkModelImportCancellation(options.stop);
    GltfOpacityMicromapAssets micromaps;
    if (options.enableOpacityMicromaps && !options.mergeInput && (!linked || linked->placements.empty()))
      micromaps = ParseGltfOpacityMicromaps(gltfModel);
    auto meshes = parseMeshes(gltfModel, micromaps, options.stop);
    if (meshes.empty()) {
      throw std::runtime_error("No renderable primitives found in glTF file");
    }
    // Runtime export rebuilds topology and resources from the core material.
    // Keep the optional acceleration payload only in the runtime side table.
    discardOpacityMicromapMetadata(gltfModel);
    GltfLoadResult result{Model::FromMeshes(std::move(meshes), options.stop),
                          std::move(gltfModel), std::move(diagnostics)};
    result.opacityMicromaps = std::move(micromaps);
    if (linked && !linked->placements.empty()) result.linkedComposition = std::move(linked);
    const bool hasMorphs = std::ranges::any_of(result.gltfModel.meshes, [](const auto& mesh) {
      return std::ranges::any_of(mesh.primitives, [](const auto& primitive) {
        return !primitive.targets.empty();
      });
    });
    if (!result.gltfModel.skins.empty() || hasMorphs)
      result.deformation = LoadDeformationData(result.gltfModel, result.model.meshes(), options.stop);
    result.instancing = ParseMeshGpuInstancing(result.gltfModel, options.instancing, options.stop);
    checkModelImportCancellation(options.stop);
    return result;
  } catch (const ModelImportCancelled&) {
    throw;
  } catch (const std::runtime_error& exception) {
    throw gltfError("glTF asset '" + path + "'", exception.what());
  }
}

Model RegenerateGltfBindTangents(
    const tinygltf::Model& source, const Model& originalBindGeometry,
    std::span<const uint32_t> normalTextureTexCoords) {
  size_t primitiveCount = 0;
  for (const auto& mesh : source.meshes) {
    if (mesh.primitives.size() > SIZE_MAX - primitiveCount)
      throw gltfError("variant tangent generation", "source primitive count overflows");
    primitiveCount += mesh.primitives.size();
  }
  if (normalTextureTexCoords.size() != primitiveCount || originalBindGeometry.meshes().size() < primitiveCount)
    throw gltfError("variant tangent generation", "normal UV/source primitive/bind mesh table counts disagree");
  size_t primitiveIndex = 0;
  // Validate every primitive before allocating or changing any bind frame.
  for (const auto& mesh : source.meshes)
    for (const auto& primitive : mesh.primitives) {
      if (normalTextureTexCoords[primitiveIndex] > 1)
        throw gltfError("variant tangent generation", "normal textures support UV0 and UV1");
      const auto position = primitive.attributes.find("POSITION");
      if (position == primitive.attributes.end() || position->second < 0 ||
          size_t(position->second) >= source.accessors.size() ||
          source.accessors[size_t(position->second)].count != originalBindGeometry.meshes()[primitiveIndex].vertices().size())
        throw gltfError("variant tangent generation", "source POSITION count differs from bind vertices");
      const auto& bind = originalBindGeometry.meshes()[primitiveIndex];
      if (bind.indices().size() % 3 != 0 || std::ranges::any_of(bind.indices(), [&](uint32_t index) {
            return index >= bind.vertices().size();
          }))
        throw gltfError("variant tangent generation", "bind indices must be complete in-range triangles");
      ++primitiveIndex;
    }
  auto meshes = originalBindGeometry.meshes();
  primitiveIndex = 0;
  for (const auto& mesh : source.meshes)
    for (const auto& primitive : mesh.primitives) {
      if (!primitive.attributes.contains("NORMAL") || !primitive.attributes.contains("TANGENT")) {
        const auto& original = meshes[primitiveIndex];
        auto vertices = original.vertices();
        generateTangentsFromGeometry(vertices, original.indices(), normalTextureTexCoords[primitiveIndex]);
        meshes[primitiveIndex] = Mesh(std::move(vertices), original.indices(),
                                     original.materialIndex(), original.disableBackfaceCulling());
      }
      ++primitiveIndex;
    }
  return Model::FromMeshes(std::move(meshes));
}

}  // namespace gltf
}  // namespace container::geometry
