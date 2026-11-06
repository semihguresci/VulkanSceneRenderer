#include <Container/renderer/core/RendererFrontend.h>

#include <Container/renderer/scene/GltfDeformationPass.h>
#include <Container/renderer/scene/GltfDeformationScene.h>
#include <Container/renderer/scene/SceneController.h>
#include <Container/utility/SceneManager.h>
#include <Container/utility/SwapChainManager.h>

#include <nlohmann/json.hpp>

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace container::renderer {
namespace {

nlohmann::json vector(glm::vec3 value) {
  return nlohmann::json::array({value.x, value.y, value.z});
}
nlohmann::json vector(glm::vec4 value) {
  return nlohmann::json::array({value.x, value.y, value.z, value.w});
}
nlohmann::json columns(const glm::mat4& value) {
  return nlohmann::json::array({vector(value[0]), vector(value[1]),
                                 vector(value[2]), vector(value[3])});
}

const char* orientationName(GltfDeformationOrientation orientation) {
  switch (orientation) {
    case GltfDeformationOrientation::Preserved: return "preserved";
    case GltfDeformationOrientation::Reversed: return "reversed";
    case GltfDeformationOrientation::MixedOrDegenerate: return "mixed-or-degenerate";
  }
  return "unknown";
}

}  // namespace

void RendererFrontend::appendGltfDeformationTelemetry(nlohmann::json& json) const {
  auto& deformation = json["gltfDeformation"];
  const auto* controller = subs_.sceneController.get();
  const auto* scene = controller ? controller->deformationScene() : nullptr;
  const auto* pass = subs_.gltfDeformationPass.get();
  deformation = {
      {"active", scene && !scene->gpuInstances().empty()},
      {"gpuReady", pass && pass->isReady()},
      {"instanceCount", scene ? scene->gpuInstances().size() : 0},
      {"sourceVertexCount", subs_.sceneManager ? subs_.sceneManager->model().vertices().size() : 0},
      {"expandedVertexCount", scene ? scene->currentVertices().size() : 0},
      {"expandedIndexCount", scene ? scene->indices().size() : 0},
      {"geometryRevision", scene ? scene->geometryRevision() : 0},
      {"topologyRevision", scene ? scene->topologyRevision() : 0},
      {"cpuGeometryRevision", controller ? controller->geometryRevision() : 0},
      {"gpuVertexCount", pass ? pass->vertexCount() : 0},
      {"currentBoundToGpu", gltfDeformationCurrentBoundToGpu_},
      {"previousBoundToGpu", gltfDeformationPreviousBoundToGpu_},
      {"maxPositionDelta", gltfDeformationMaxPositionDelta_},
      {"historyReset", gltfDeformationHistoryReset_},
      {"instances", nlohmann::json::array()}};
  if (!scene || !controller) return;
  const auto vertices = scene->currentVertices();
  const auto indices = scene->indices();
  const auto poses = scene->gpuPoses();
  const VkExtent2D extent = svc_.swapChainManager.extent();
  auto pickCamera = buffers_.cameraData;
  // The diagnostic deliberately uses one unjittered projection for both the
  // world-to-pixel probe and the actual production CPU pick-ray construction.
  pickCamera.viewProj = pickCamera.unjitteredViewProj;
  pickCamera.inverseViewProj = glm::inverse(pickCamera.viewProj);
  for (const auto& range : scene->drawRanges()) {
    if (!range.deformed() || range.vertexOffset >= vertices.size()) continue;
    const auto* node = sceneGraph_.getNode(range.graphNode);
    const auto* owner = sceneGraph_.getNode(range.meshOwnerGraphNode);
    if (!node) continue;
    const auto& vertex = vertices[range.vertexOffset];
    nlohmann::json sample = {
        {"graphNode", range.graphNode},
        {"name", node->name},
        {"sourceNodeIndex", owner ? owner->sourceNodeIndex : -1},
        {"sourcePrimitive", range.sourcePrimitive},
        {"meshOwnerGraphNode", range.meshOwnerGraphNode},
        {"firstIndex", range.firstIndex},
        {"indexCount", range.indexCount},
        {"vertexOffset", range.vertexOffset},
        {"vertexCount", range.vertexCount},
        {"weights", range.instance < poses.size()
            ? nlohmann::json(std::vector<float>(poses[range.instance].morphWeights.begin(),
                                                  poses[range.instance].morphWeights.end()))
            : nlohmann::json::array()},
        {"boundsMin", vector(range.bounds.min)},
        {"boundsMax", vector(range.bounds.max)},
        {"boundsValid", range.bounds.valid},
        {"orientation", orientationName(range.orientation)},
        {"firstPosition", vector(vertex.position)},
        {"firstNormal", vector(vertex.normal)},
        {"firstTangent", vector(vertex.tangent)},
        {"modelColumns", columns(node->worldTransform)}};
    auto& probe = sample["pick"];
    probe = {{"projected", false}, {"expectedNode", range.graphNode},
               {"hit", false}, {"hitNode", container::scene::SceneGraph::kInvalidNode}};
    const float determinant = glm::determinant(glm::mat3(node->worldTransform));
    if (extent.width && extent.height && std::isfinite(determinant) && determinant != 0) {
      const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(node->worldTransform)));
      const size_t end = std::min(indices.size(), static_cast<size_t>(range.firstIndex) + range.indexCount);
      for (size_t index = range.firstIndex; index + 2 < end; index += 3) {
        const uint32_t a = indices[index], b = indices[index + 1], c = indices[index + 2];
        if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size()) continue;
        const glm::vec3 centroid = glm::vec3(node->worldTransform * glm::vec4(
            (vertices[a].position + vertices[b].position + vertices[c].position) / 3.0f, 1));
        const glm::vec3 normal = normalMatrix *
            (vertices[a].normal + vertices[b].normal + vertices[c].normal);
        if (glm::dot(normal, glm::vec3(pickCamera.cameraWorldPosition) - centroid) <= 0) continue;
        const glm::vec4 clip = pickCamera.viewProj * glm::vec4(centroid, 1);
        if (!(clip.w > 0)) continue;
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        const glm::vec2 uv(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
        if (uv.x < 0.01f || uv.x >= 0.99f || uv.y < 0.01f || uv.y >= 0.99f ||
            ndc.z < 0 || ndc.z > 1) continue;
        const double x = static_cast<double>(uv.x) * extent.width;
        const double y = static_cast<double>(uv.y) * extent.height;
        const auto hit = controller->pickRenderableNodeHit(pickCamera, extent, x, y);
        probe = {{"projected", true}, {"expectedNode", range.graphNode},
                   {"pixel", {x, y}}, {"worldProbe", vector(centroid)},
                   {"hit", hit.hit}, {"hitNode", hit.nodeIndex},
                   {"worldPosition", vector(hit.worldPosition)}, {"distance", hit.distance}};
        break;
      }
    }
    deformation["instances"].push_back(std::move(sample));
  }
}

}  // namespace container::renderer
