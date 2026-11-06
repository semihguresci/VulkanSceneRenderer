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
  auto& sceneView=json["gltfScenes"];
  sceneView={{"graphIdentity",sceneGraph_.identity()},
      {"authoringNodeCount",sceneGraph_.nodeCount()},
      {"activeRenderableCount",sceneGraph_.renderableNodes().size()},
      {"selected",subs_.sceneManager?int64_t(subs_.sceneManager->selectedScene()):-1},
      {"editHistoryDepth",sceneEdits_?sceneEdits_->size():0},
      {"names",nlohmann::json::array()}, {"activeSourceNodes",nlohmann::json::array()}};
  if(subs_.sceneManager) {
    sceneView["pointLightCount"]=subs_.sceneManager->authoredPointLights().size();
    sceneView["directionalLightCount"]=subs_.sceneManager->authoredDirectionalLights().size();
    sceneView["areaLightCount"]=subs_.sceneManager->authoredAreaLights().size();
  }
  if(subs_.sceneManager) for(const auto& entry:subs_.sceneManager->sceneCatalog().entries)
    sceneView["names"].push_back(entry.name);
  for(uint32_t i=0;i<sceneGraph_.nodeCount();++i) {
    const auto& node=*sceneGraph_.getNode(i);
    if(sceneGraph_.isNodeActive(i) && node.sourceNodeIndex>=0)
      sceneView["activeSourceNodes"].push_back(node.sourceNodeIndex);
  }
  const auto graphId = [](uint32_t id) -> int64_t {
    return id == container::scene::SceneGraph::kInvalidNode ? -1 : int64_t(id);
  };
  auto& flags = json["gltfNodeFlags"];
  flags = {{"nodes", nlohmann::json::array()}, {"probes", nlohmann::json::array()},
           {"cameraSourceNodes", nlohmann::json::array()}, {"nodeCount", sceneGraph_.nodeCount()}};
  constexpr uint32_t maxFlagTelemetryNodes = 4096;
  flags["truncated"] = sceneGraph_.nodeCount() > maxFlagTelemetryNodes;
  for (uint32_t id = 0; id < std::min<size_t>(sceneGraph_.nodeCount(), maxFlagTelemetryNodes); ++id) {
    const auto& node = *sceneGraph_.getNode(id);
    flags["nodes"].push_back({{"sourceIndex", node.sourceNodeIndex}, {"graphNode", id},
        {"name", node.name}, {"parentGraphNode", graphId(node.parent)},
        {"active", sceneGraph_.isNodeActive(id)},
        {"localVisible", node.visible}, {"localSelectable", node.selectable}, {"localHoverable", node.hoverable},
        {"retired", node.retired},
        {"effectiveVisible", sceneGraph_.isNodeEffectivelyVisible(id)},
        {"effectiveSelectable", sceneGraph_.isNodeEffectivelySelectable(id)},
        {"effectiveHoverable", sceneGraph_.isNodeEffectivelyHoverable(id)}});
    if (subs_.sceneManager && node.sourceNodeIndex >= 0 && sceneGraph_.isNodeActive(id)) {
      const auto& source = subs_.sceneManager->animationSource();
      if (size_t(node.sourceNodeIndex) < source.nodes.size()) {
        const int camera = source.nodes[size_t(node.sourceNodeIndex)].camera;
        if (camera >= 0 && size_t(camera) < source.cameras.size())
          flags["cameraSourceNodes"].push_back(node.sourceNodeIndex);
      }
    }
  }
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
  auto& instancing=json["gltfInstancing"];
  instancing={{"drawCount",controller?controller->objectData().size():0},
      {"sourceVertexCount",subs_.sceneManager?subs_.sceneManager->model().vertices().size():0},
      {"sourceIndexCount",subs_.sceneManager?subs_.sceneManager->model().indices().size():0},
      {"gpuBindVertexCount",controller?controller->gpuBindVertices().size():0},
      {"instances",nlohmann::json::array()}};
  if(controller && subs_.sceneManager) {
    const auto& bounds=subs_.sceneManager->modelBounds();
    instancing["modelBounds"]={{"valid",bounds.valid},{"min",vector(bounds.min)},
        {"max",vector(bounds.max)},{"center",vector(bounds.center)},{"radius",bounds.radius}};
    const auto points=controller->geometryVertices();
    const auto triangles=controller->geometryIndices();
    auto camera=buffers_.cameraData;
    camera.viewProj=camera.unjitteredViewProj; camera.inverseViewProj=glm::inverse(camera.viewProj);
    const auto extent=svc_.swapChainManager.extent();
    const auto& ranges=subs_.sceneManager->model().primitiveRanges();
    for(const auto id:sceneGraph_.renderableNodes()) {
      const auto* node=sceneGraph_.getNode(id);
      if(node->primitiveIndex>=ranges.size()) continue;
      const auto* owner=sceneGraph_.getNode(node->meshOwnerGraphNode);
      nlohmann::json item={{"graphNode",id},{"name",node->name},
          {"sourceNode",owner?owner->sourceNodeIndex:-1},{"sourceMesh",node->sourceMeshIndex},
          {"sourcePrimitive",node->sourcePrimitiveIndex},{"sourceInstance",node->sourceInstanceIndex},
          {"primitiveIndex",node->primitiveIndex},{"visible",sceneGraph_.isNodeEffectivelyVisible(id)},
          {"modelColumns",columns(node->worldTransform)}};
      auto& probe=item["pick"]; probe={{"projected",false},{"expectedNode",id},{"hit",false}};
      const auto* posed=scene?scene->drawRange(id):nullptr;
      const auto first=posed?posed->firstIndex:ranges[node->primitiveIndex].firstIndex;
      const auto count=posed?posed->indexCount:ranges[node->primitiveIndex].indexCount;
      const auto normalMatrix=glm::transpose(glm::inverse(glm::mat3(node->worldTransform)));
      for(size_t i=first;i+2<triangles.size() && i+2<size_t(first)+count;i+=3) {
        const auto a=triangles[i],b=triangles[i+1],c=triangles[i+2];
        if(a>=points.size()||b>=points.size()||c>=points.size()) continue;
        const auto centroid=glm::vec3(node->worldTransform*glm::vec4((points[a].position+points[b].position+points[c].position)/3.f,1));
        const auto normal=normalMatrix*(points[a].normal+points[b].normal+points[c].normal);
        if(glm::dot(normal,glm::vec3(camera.cameraWorldPosition)-centroid)<=0) continue;
        const auto clip=camera.viewProj*glm::vec4(centroid,1);
        if(!(clip.w>0)) continue;
        const auto ndc=glm::vec3(clip)/clip.w;
        const glm::vec2 uv(ndc.x*.5f+.5f,.5f-ndc.y*.5f);
        if(uv.x<.01f||uv.x>=.99f||uv.y<.01f||uv.y>=.99f||ndc.z<0||ndc.z>1) continue;
        const double x=double(uv.x)*extent.width,y=double(uv.y)*extent.height;
        const auto hit=controller->pickRenderableNodeHit(camera,extent,x,y);
        probe={{"projected",true},{"expectedNode",id},{"pixel",{x,y}},{"hit",hit.hit},{"hitNode",hit.nodeIndex}};
        break;
      }
      if (flags["probes"].size() < maxFlagTelemetryNodes) {
        const uint32_t hit = probe.value("hit", false)
            ? probe.value("hitNode", container::scene::SceneGraph::kInvalidNode)
            : container::scene::SceneGraph::kInvalidNode;
        const auto* hitOwner = sceneGraph_.getNode(node->meshOwnerGraphNode);
        flags["probes"].push_back({{"graphNode", id},
            {"sourceNodeIndex", hitOwner ? hitOwner->sourceNodeIndex : node->sourceNodeIndex},
            {"sourceName", hitOwner ? hitOwner->name : node->name},
            {"projected", probe.value("projected", false)}, {"hit", hit != container::scene::SceneGraph::kInvalidNode},
            {"nearestHitGraphNode", graphId(hit)},
            {"resolvedSelection", graphId(sceneGraph_.nearestSelectableAncestor(hit))},
            {"resolvedHover", graphId(sceneGraph_.nearestHoverableAncestor(hit))}});
      }
      if (node->sourceInstanceIndex >= 0) instancing["instances"].push_back(std::move(item));
    }
  }
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
