#include <Container/geometry/Model.h>

#include <Container/geometry/GltfModelLoader.h>
#include <Container/utility/ModelImportCancellation.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace container::geometry {
namespace {

using container::util::checkModelImportCancellation;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = kPi * 2.0f;

uint32_t countVertices(const std::vector<Mesh>& meshes, const std::stop_token& stop) {
  uint32_t total = 0;
  for (const auto& mesh : meshes) {
    checkModelImportCancellation(stop);
    total += static_cast<uint32_t>(mesh.vertices().size());
  }
  return total;
}

uint32_t countIndices(const std::vector<Mesh>& meshes, const std::stop_token& stop) {
  uint32_t total = 0;
  for (const auto& mesh : meshes) {
    checkModelImportCancellation(stop);
    total += static_cast<uint32_t>(mesh.indices().size());
  }
  return total;
}

Vertex makeFaceVertex(const glm::vec3& position, const glm::vec2& texCoord,
                      const glm::vec3& normal, const glm::vec3& tangent) {
  Vertex vertex{};
  const glm::vec3 safeNormal = glm::normalize(normal);
  glm::vec3 safeTangent = tangent - safeNormal * glm::dot(safeNormal, tangent);
  if (glm::dot(safeTangent, safeTangent) < 1e-8f) {
    const glm::vec3 axis = std::abs(safeNormal.y) < 0.999f
                               ? glm::vec3(0.0f, 1.0f, 0.0f)
                               : glm::vec3(1.0f, 0.0f, 0.0f);
    safeTangent = glm::cross(axis, safeNormal);
  }
  safeTangent = glm::normalize(safeTangent);
  vertex.position = position;
  vertex.color = glm::vec3(1.0f);
  vertex.texCoord = texCoord;
  vertex.normal = safeNormal;
  vertex.tangent = glm::vec4(safeTangent, 1.0f);
  return vertex;
}

Vertex makeSphereVertex(float theta, float phi, float radius) {
  const float sinTheta = std::sin(theta);
  const float cosTheta = std::cos(theta);
  const float sinPhi = std::sin(phi);
  const float cosPhi = std::cos(phi);

  const glm::vec3 normal(
      sinTheta * cosPhi,
      cosTheta,
      sinTheta * sinPhi);

  glm::vec3 tangent(-sinPhi, 0.0f, cosPhi);
  if (glm::dot(tangent, tangent) < 1e-8f) {
    tangent = glm::vec3(1.0f, 0.0f, 0.0f);
  }

  Vertex vertex{};
  vertex.position = normal * radius;
  vertex.color = glm::vec3(1.0f);
  vertex.texCoord = glm::vec2(phi / kTwoPi, theta / kPi);
  vertex.normal = glm::normalize(normal);
  vertex.tangent = glm::vec4(glm::normalize(tangent), 1.0f);
  return vertex;
}

void appendQuadFace(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
                    const glm::vec3& p0, const glm::vec3& p1,
                    const glm::vec3& p2, const glm::vec3& p3,
                    const glm::vec3& normal, const glm::vec3& tangent) {
  const uint32_t baseIndex = static_cast<uint32_t>(vertices.size());
  vertices.push_back(makeFaceVertex(p0, glm::vec2(0.0f, 0.0f), normal, tangent));
  vertices.push_back(makeFaceVertex(p1, glm::vec2(1.0f, 0.0f), normal, tangent));
  vertices.push_back(makeFaceVertex(p2, glm::vec2(1.0f, 1.0f), normal, tangent));
  vertices.push_back(makeFaceVertex(p3, glm::vec2(0.0f, 1.0f), normal, tangent));

  indices.insert(indices.end(),
                 {baseIndex, baseIndex + 1, baseIndex + 2,
                  baseIndex, baseIndex + 2, baseIndex + 3});
}

void appendFlattenedMesh(const Mesh& mesh, std::vector<Vertex>& vertices,
                         std::vector<uint32_t>& indices, const std::stop_token& stop) {
  const uint32_t vertexOffset = static_cast<uint32_t>(vertices.size());
  const auto& sourceVertices = mesh.vertices();
  constexpr size_t chunkSize = 4096;
  for (size_t offset = 0; offset < sourceVertices.size();) {
    checkModelImportCancellation(stop);
    const auto count = std::min(chunkSize, sourceVertices.size() - offset);
    const auto first = sourceVertices.begin() + offset;
    vertices.insert(vertices.end(), first, first + count);
    offset += count;
  }

  indices.reserve(indices.size() + mesh.indices().size());
  for (size_t i = 0; i < mesh.indices().size(); ++i) {
    if (i % chunkSize == 0) checkModelImportCancellation(stop);
    indices.push_back(mesh.indices()[i] + vertexOffset);
  }
}

}  // namespace

Model::Model(std::vector<Mesh> meshes, const std::stop_token& stop)
    : meshes_(std::move(meshes)) {
  flattenMeshes(stop);
}

void Model::flattenMeshes(const std::stop_token& stop) {
  checkModelImportCancellation(stop);
  vertices_.clear();
  indices_.clear();
  primitiveRanges_.clear();

  vertices_.reserve(countVertices(meshes_, stop));
  indices_.reserve(countIndices(meshes_, stop));
  primitiveRanges_.reserve(meshes_.size());

  for (const auto& mesh : meshes_) {
    const uint32_t firstIndex = static_cast<uint32_t>(indices_.size());
    appendFlattenedMesh(mesh, vertices_, indices_, stop);
    primitiveRanges_.push_back(
        {firstIndex, static_cast<uint32_t>(mesh.indices().size()),
         mesh.materialIndex(), mesh.disableBackfaceCulling()});
  }
}

Model Model::LoadFromGltf(const std::string& path) {
  return gltf::LoadModelFromFile(path);
}

Model Model::FromMeshes(std::vector<Mesh> meshes, const std::stop_token& stop) {
  return Model(std::move(meshes), stop);
}

Model Model::MakeCube() {
  std::vector<Vertex> verts;
  verts.reserve(24);
  std::vector<uint32_t> idx;
  idx.reserve(36);

  appendQuadFace(verts, idx,
                 {.5f, -.5f, .5f}, {.5f, -.5f, -.5f},
                 {.5f, .5f, -.5f}, {.5f, .5f, .5f},
                 {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
  appendQuadFace(verts, idx,
                 {-.5f, -.5f, -.5f}, {-.5f, -.5f, .5f},
                 {-.5f, .5f, .5f}, {-.5f, .5f, -.5f},
                 {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
  appendQuadFace(verts, idx,
                 {-.5f, .5f, .5f}, {.5f, .5f, .5f},
                 {.5f, .5f, -.5f}, {-.5f, .5f, -.5f},
                 {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
  appendQuadFace(verts, idx,
                 {-.5f, -.5f, -.5f}, {.5f, -.5f, -.5f},
                 {.5f, -.5f, .5f}, {-.5f, -.5f, .5f},
                 {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
  appendQuadFace(verts, idx,
                 {-.5f, -.5f, .5f}, {.5f, -.5f, .5f},
                 {.5f, .5f, .5f}, {-.5f, .5f, .5f},
                 {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f});
  appendQuadFace(verts, idx,
                 {.5f, -.5f, -.5f}, {-.5f, -.5f, -.5f},
                 {-.5f, .5f, -.5f}, {.5f, .5f, -.5f},
                 {0.0f, 0.0f, -1.0f}, {-1.0f, 0.0f, 0.0f});

  std::vector<Mesh> meshes;
  meshes.emplace_back(verts, idx);
  return Model{std::move(meshes)};
}

Model Model::MakeSphere(uint32_t latitudeSegments,
                        uint32_t longitudeSegments,
                        float radius) {
  latitudeSegments = std::max(latitudeSegments, 3u);
  longitudeSegments = std::max(longitudeSegments, 3u);
  radius = std::max(radius, 0.001f);

  std::vector<Vertex> verts;
  std::vector<uint32_t> idx;
  verts.reserve((latitudeSegments + 1) * (longitudeSegments + 1));
  idx.reserve(latitudeSegments * longitudeSegments * 6);

  for (uint32_t lat = 0; lat <= latitudeSegments; ++lat) {
    const float theta = kPi *
                        (static_cast<float>(lat) / static_cast<float>(latitudeSegments));
    for (uint32_t lon = 0; lon <= longitudeSegments; ++lon) {
      const float phi = kTwoPi *
                        (static_cast<float>(lon) / static_cast<float>(longitudeSegments));
      verts.push_back(makeSphereVertex(theta, phi, radius));
    }
  }

  const uint32_t stride = longitudeSegments + 1;
  for (uint32_t lat = 0; lat < latitudeSegments; ++lat) {
    for (uint32_t lon = 0; lon < longitudeSegments; ++lon) {
      const uint32_t first = lat * stride + lon;
      const uint32_t second = first + stride;

      idx.push_back(first);
      idx.push_back(first + 1);
      idx.push_back(second);

      idx.push_back(second);
      idx.push_back(first + 1);
      idx.push_back(second + 1);
    }
  }

  std::vector<Mesh> meshes;
  meshes.emplace_back(verts, idx);
  return Model{std::move(meshes)};
}

}  // namespace container::geometry
