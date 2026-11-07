#include <Container/renderer/scene/GltfDeformationScene.h>
#include <Container/geometry/GltfDeformationFrames.h>
#include <Container/utility/SourcePrimitiveMapping.h>

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace container::renderer {
namespace {
namespace gltf = geometry::gltf;
constexpr uint32_t invalid = scene::SceneGraph::kInvalidNode;

[[noreturn]] void fail(const char* message) {
  throw std::runtime_error(std::string("glTF deformation scene: ") + message);
}

uint32_t narrow(size_t value) {
  if (value >= invalid) fail("geometry or node range exceeds 32-bit indexing");
  return static_cast<uint32_t>(value);
}

bool finite(glm::vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool sameVertex(const geometry::Vertex& a, const geometry::Vertex& b) {
  return a.position == b.position && a.normal == b.normal && a.tangent == b.tangent &&
      a.color == b.color && a.colorAlpha == b.colorAlpha &&
      a.texCoord == b.texCoord && a.texCoord1 == b.texCoord1;
}

bool sameVertices(std::span<const geometry::Vertex> a,
                    std::span<const geometry::Vertex> b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), sameVertex);
}

GltfDeformationBounds bounds(std::span<const geometry::Vertex> vertices,
                              std::span<const uint32_t> indices) {
  GltfDeformationBounds result;
  if (indices.empty()) return result;
  glm::vec3 minimum(std::numeric_limits<float>::max());
  glm::vec3 maximum(std::numeric_limits<float>::lowest());
  for (const uint32_t index : indices) {
    if (index >= vertices.size()) fail("index is outside the vertex stream");
    if (!finite(vertices[index].position)) fail("non-finite vertex position");
    minimum = glm::min(minimum, vertices[index].position);
    maximum = glm::max(maximum, vertices[index].position);
  }
  result.min = minimum;
  result.max = maximum;
  result.center = minimum * 0.5f + maximum * 0.5f;
  double radius = 0;
  for (const uint32_t index : indices) {
    radius = std::max(radius, glm::length(glm::dvec3(vertices[index].position) -
                                         glm::dvec3(result.center)));
  }
  if (!std::isfinite(radius) || radius > std::numeric_limits<float>::max()) {
    fail("bounds radius is outside finite float range");
  }
  result.radius = static_cast<float>(radius);
  result.valid = true;
  return result;
}

// Classification describes skin-blend determinant signs, which are additional
// to the later object model determinant. Morphing keeps authored index order.
// A collapsed triangle or mixed/degenerate skin blends require no-cull routing.
GltfDeformationOrientation orientation(const gltf::DeformationPrimitive& primitive,
    std::span<const geometry::Vertex> evaluated, std::span<const uint32_t> indices,
    std::span<const glm::mat4> palette) {
  int sign = 0;
  if (!palette.empty() && primitive.hasSkinning()) {
    for (const uint32_t vertex : indices) {
      glm::dmat3 blended(0.0);
      double weightSum = 0;
      for (const auto& influence : primitive.influences[vertex].values) {
        if (influence.joint >= palette.size()) fail("joint is outside the skin palette");
        blended += glm::dmat3(palette[influence.joint]) * static_cast<double>(influence.weight);
        weightSum += influence.weight;
      }
      if (!(weightSum > 0) || !std::isfinite(weightSum)) fail("invalid influence sum");
      blended /= weightSum;
      const double determinant = glm::determinant(blended);
      if (!std::isfinite(determinant) || determinant == 0) return GltfDeformationOrientation::MixedOrDegenerate;
      const int currentSign = determinant < 0 ? -1 : 1;
      if (sign != 0 && sign != currentSign) return GltfDeformationOrientation::MixedOrDegenerate;
      sign = currentSign;
    }
  }
  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    const glm::dvec3 edge0 = glm::dvec3(evaluated[indices[i + 1]].position) -
                              glm::dvec3(evaluated[indices[i]].position);
    const glm::dvec3 edge1 = glm::dvec3(evaluated[indices[i + 2]].position) -
                              glm::dvec3(evaluated[indices[i]].position);
    const glm::dvec3 cross = glm::cross(edge0, edge1);
    const double scale = std::max(glm::dot(edge0, edge0), glm::dot(edge1, edge1));
    // Relative to triangle scale, including tiny but well-shaped triangles.
    if (!(scale > 0) || glm::dot(cross, cross) <= scale * scale * 1e-20) {
      return GltfDeformationOrientation::MixedOrDegenerate;
    }
  }
  return sign < 0 ? GltfDeformationOrientation::Reversed : GltfDeformationOrientation::Preserved;
}

std::vector<float> nodeWeights(const gltf::DeformationData& data,
                                uint32_t sourceNode, size_t targetCount,
                                const tinygltf::Model* evaluated) {
  if (sourceNode >= data.nodes.size()) fail("source mesh node identity is out of range");
  if (!evaluated) return data.nodes[sourceNode].defaultMorphWeights;
  if (sourceNode >= evaluated->nodes.size()) fail("evaluated source mesh node is missing");
  const auto& node = evaluated->nodes[sourceNode];
  if (node.mesh != data.nodes[sourceNode].sourceMesh || node.skin != data.nodes[sourceNode].skin) {
    fail("evaluated source mesh/skin identity changed; configure the source again");
  }
  if (node.mesh < 0 || static_cast<size_t>(node.mesh) >= evaluated->meshes.size()) {
    fail("evaluated source mesh is missing");
  }
  const auto& authored = node.weights.empty() ? evaluated->meshes[static_cast<size_t>(node.mesh)].weights
                                              : node.weights;
  if (!authored.empty() && authored.size() != targetCount) fail("evaluated morph weight count mismatch");
  std::vector<float> result(targetCount, 0);
  for (size_t i = 0; i < authored.size(); ++i) {
    if (!std::isfinite(authored[i]) || std::abs(authored[i]) > std::numeric_limits<float>::max()) {
      fail("evaluated morph weight is outside finite float range");
    }
    result[i] = static_cast<float>(authored[i]);
  }
  return result;
}

bool sameRange(const GltfDeformationDrawRange& a, const GltfDeformationDrawRange& b) {
  return a.graphNode == b.graphNode && a.sourcePrimitive == b.sourcePrimitive &&
      a.meshOwnerGraphNode == b.meshOwnerGraphNode && a.firstIndex == b.firstIndex &&
      a.indexCount == b.indexCount && a.vertexOffset == b.vertexOffset &&
      a.vertexCount == b.vertexCount && a.instance == b.instance;
}

}  // namespace

struct GltfDeformationScene::Impl {
  struct Instance {
    uint32_t primitive{invalid}, definition{invalid}, owner{invalid}, sourceNode{invalid};
    int skin{-1};
    uint32_t outputOffset{0};
    uint32_t firstIndex{0};
    std::vector<float> weights;
    std::vector<glm::mat4> palette;
    std::vector<geometry::Vertex> current;
    std::vector<float> submittedWeights;
    std::vector<glm::mat4> submittedPalette;
    std::vector<geometry::Vertex> submittedVertices;
    GltfDeformationBounds bounds;
    GltfDeformationOrientation orientation{GltfDeformationOrientation::Preserved};
    bool hasCurrent{false}, submitted{false};
  };

  gltf::DeformationData data;
  std::vector<std::optional<gltf::PreparedDeformationFrames>> preparedFrames;
  std::vector<geometry::Vertex> sourceVertices;
  std::vector<uint32_t> sourceIndices;
  std::vector<geometry::PrimitiveRange> sourceRanges;
  std::vector<uint32_t> sourceVertexOffsets;
  std::vector<uint32_t> sourceVertexCounts;
  std::vector<GltfDeformationBounds> sourceBounds;
  std::vector<std::vector<uint32_t>> localIndices;
  std::vector<int32_t> runtimeToDefinition;
  std::vector<geometry::Vertex> expandedBind, current, previous;
  std::vector<uint32_t> expandedIndices;
  std::vector<Instance> instances;
  std::vector<GltfDeformationDrawRange> ranges;
  std::vector<uint32_t> nodeRanges;
  std::vector<GltfDeformationInstance> gpuInstances;
  std::vector<GltfDeformationPose> gpuPoses;
  uint64_t geometryRevision{0}, topologyRevision{0};
  bool configured{false};
  bool sourceGeometryChanged{false}, sourceTopologyChanged{false};
  bool resetPending{false};

  const gltf::DeformationPrimitive& definitionPrimitive(uint32_t definition) const {
    return preparedFrames[definition] ? preparedFrames[definition]->primitive : data.primitives[definition];
  }
  std::span<const uint32_t> definitionIndices(uint32_t definition) const {
    return preparedFrames[definition] ? std::span<const uint32_t>(preparedFrames[definition]->indices)
                                      : std::span<const uint32_t>(localIndices[definition]);
  }

  static bool sameIdentity(const Instance& a, const Instance& b) {
    return a.primitive == b.primitive && a.definition == b.definition && a.owner == b.owner &&
        a.sourceNode == b.sourceNode && a.skin == b.skin;
  }

  void rebuildGpuViews() {
    gpuInstances.clear();
    gpuPoses.clear();
    gpuInstances.reserve(instances.size());
    gpuPoses.reserve(instances.size());
    for (const auto& instance : instances) {
      const bool reset = resetPending || !instance.submitted;
      gpuInstances.push_back({&definitionPrimitive(instance.definition), instance.outputOffset});
      gpuPoses.push_back({instance.weights, instance.palette,
          reset ? std::span<const float>(instance.weights) : std::span<const float>(instance.submittedWeights),
          reset ? std::span<const glm::mat4>(instance.palette) : std::span<const glm::mat4>(instance.submittedPalette), reset,
          preparedFrames[instance.definition] ? std::span<const geometry::Vertex>(instance.current) : std::span<const geometry::Vertex>{},
          preparedFrames[instance.definition]
              ? std::span<const geometry::Vertex>(reset ? instance.current : instance.submittedVertices)
              : std::span<const geometry::Vertex>{}});
    }
  }

  void assemblePoses() {
    if (instances.empty()) {
      current.clear();
      previous.clear();
      return;
    }
    current = expandedBind;
    previous = expandedBind;
    for (const auto& instance : instances) {
      std::copy(instance.current.begin(), instance.current.end(), current.begin() + instance.outputOffset);
      const auto& old = resetPending || !instance.submitted ? instance.current : instance.submittedVertices;
      std::copy(old.begin(), old.end(), previous.begin() + instance.outputOffset);
    }
  }
};

GltfDeformationScene::GltfDeformationScene() : impl_(std::make_unique<Impl>()) {}
GltfDeformationScene::~GltfDeformationScene() = default;

void GltfDeformationScene::configure(gltf::DeformationData data,
                                      const geometry::Model& sourceGeometry,
                                      std::span<const uint32_t> sourceDeformationToRuntime) {
  auto& state = *impl_;
  const auto& vertices = sourceGeometry.vertices();
  const auto& indices = sourceGeometry.indices();
  const auto& ranges = sourceGeometry.primitiveRanges();
  const auto& meshes = sourceGeometry.meshes();
  if (ranges.size() != meshes.size() || data.primitives.size() > meshes.size()) {
    fail("source primitive mapping differs from runtime geometry");
  }
  auto mapping = scene::MakeSourcePrimitiveMapping(data.primitives.size(), meshes.size(),
                                                  sourceDeformationToRuntime);
  narrow(vertices.size());
  narrow(indices.size());
  std::vector<uint32_t> offsets, counts;
  std::vector<GltfDeformationBounds> sourceBounds;
  std::vector<std::vector<uint32_t>> localIndices(data.primitives.size());
  size_t vertexOffset = 0;
  for (size_t i = 0; i < meshes.size(); ++i) {
    offsets.push_back(narrow(vertexOffset));
    counts.push_back(narrow(meshes[i].vertices().size()));
    vertexOffset += meshes[i].vertices().size();
    if (vertexOffset > vertices.size()) fail("source mesh vertices are outside the flattened stream");
    const auto& range = ranges[i];
    if (range.firstIndex > indices.size() || range.indexCount > indices.size() - range.firstIndex) {
      fail("source primitive index range is outside the flattened stream");
    }
    const auto view = std::span<const uint32_t>(indices).subspan(range.firstIndex, range.indexCount);
    sourceBounds.push_back(bounds(vertices, view));
    const int32_t definition = mapping.runtimeToSource[i];
    if (definition < 0) continue;
    if (data.primitives[size_t(definition)].bindVertices.size() != meshes[i].vertices().size()) {
      fail("deformation bind vertex count differs from runtime mesh");
    }
    for (const uint32_t index : view) {
      if (index < offsets.back() || index - offsets.back() >= counts.back()) {
        fail("source primitive index addresses another primitive's vertices");
      }
      localIndices[size_t(definition)].push_back(index - offsets.back());
    }
  }
  if (vertexOffset != vertices.size()) fail("flattened source vertex count mismatch");
  std::vector<std::optional<gltf::PreparedDeformationFrames>> preparedFrames(data.primitives.size());
  for (size_t i = 0; i < data.primitives.size(); ++i) {
    const auto& primitive = data.primitives[i];
    if (primitive.hasMorphing() && (!primitive.authoredBaseNormal || !primitive.authoredBaseTangent)) {
      preparedFrames[i] = gltf::PrepareDeformationFrames(primitive, localIndices[i],
          {primitive.authoredBaseNormal, primitive.authoredBaseTangent, primitive.normalTextureTexCoord});
    }
  }
  const bool sourceVerticesChanged = !sameVertices(state.sourceVertices, vertices);
  bool rangesChanged = state.sourceRanges.size() != ranges.size();
  if (!rangesChanged) {
    for (size_t i = 0; i < ranges.size(); ++i) {
      rangesChanged |= state.sourceRanges[i].firstIndex != ranges[i].firstIndex ||
          state.sourceRanges[i].indexCount != ranges[i].indexCount;
    }
  }
  const bool indicesChanged = state.sourceIndices != indices;
  const bool mappingChanged = state.runtimeToDefinition != mapping.runtimeToSource;
  state.sourceGeometryChanged |= sourceVerticesChanged || indicesChanged || !state.instances.empty();
  state.sourceTopologyChanged |= rangesChanged || indicesChanged || sourceVerticesChanged || mappingChanged || !state.instances.empty();
  state.data = std::move(data);
  state.preparedFrames = std::move(preparedFrames);
  state.sourceVertices = vertices;
  state.sourceIndices = indices;
  state.sourceRanges = ranges;
  state.sourceVertexOffsets = std::move(offsets);
  state.sourceVertexCounts = std::move(counts);
  state.sourceBounds = std::move(sourceBounds);
  state.localIndices = std::move(localIndices);
  state.runtimeToDefinition = std::move(mapping.runtimeToSource);
  state.instances.clear();
  state.expandedBind.clear();
  state.expandedIndices.clear();
  state.current.clear();
  state.previous.clear();
  state.gpuInstances.clear();
  state.gpuPoses.clear();
  state.configured = true;
  state.resetPending = false;
}

GltfDeformationSceneUpdate GltfDeformationScene::update(const scene::SceneGraph& graph,
    std::span<const uint32_t> meshOwnerGraphNodes, const tinygltf::Model* evaluatedSource,
    bool resetHistory) {
  auto& state = *impl_;
  if (!state.configured) fail("configure must precede update");
  if (!meshOwnerGraphNodes.empty() && meshOwnerGraphNodes.size() != graph.nodeCount()) {
    fail("mesh owner mapping must be indexed by runtime graph node");
  }
  state.resetPending |= resetHistory;
  std::vector<std::vector<uint32_t>> sourceGraphNodes(state.data.nodes.size());
  for (size_t i = 0; i < graph.nodeCount(); ++i) {
    const auto* node = graph.getNode(narrow(i));
    if (node->retired || node->sourceNodeIndex < 0) continue;
    if (static_cast<size_t>(node->sourceNodeIndex) >= sourceGraphNodes.size()) {
      fail("runtime source node identity is outside the configured source");
    }
    sourceGraphNodes[static_cast<size_t>(node->sourceNodeIndex)].push_back(narrow(i));
  }
  std::vector<GltfDeformationDrawRange> candidateRanges;
  std::vector<Impl::Instance> candidates;
  std::map<std::pair<uint32_t,uint32_t>,size_t> ownerPrimitives;
  size_t vertexOffset = state.sourceVertices.size();
  size_t firstIndex = state.sourceIndices.size();
  for (size_t i = 0; i < graph.nodeCount(); ++i) {
    const uint32_t nodeIndex = narrow(i);
    const auto& node = *graph.getNode(nodeIndex);
    if (!node.renderable || node.primitiveIndex == invalid || !graph.isNodeActive(nodeIndex)) continue;
    if (node.primitiveIndex >= state.sourceRanges.size()) fail("runtime primitive index is outside the source");
    const uint32_t primitiveIndex = node.primitiveIndex;
    const auto& sourceRange = state.sourceRanges[primitiveIndex];
    const uint32_t owner = meshOwnerGraphNodes.empty() ? invalid : meshOwnerGraphNodes[i];
    GltfDeformationDrawRange range{nodeIndex, primitiveIndex, owner,
        sourceRange.firstIndex, sourceRange.indexCount,
        state.sourceVertexOffsets[primitiveIndex], state.sourceVertexCounts[primitiveIndex]};
    range.bounds = state.sourceBounds[primitiveIndex];
    const int32_t definition = state.runtimeToDefinition[primitiveIndex];
    if (definition >= 0) {
      const auto& primitive = state.data.primitives[size_t(definition)];
      if (primitive.hasMorphing() || primitive.hasSkinning()) {
        if (owner == invalid || owner >= graph.nodeCount()) fail("deforming primitive mesh owner is missing/deleted");
        const auto& ownerNode = *graph.getNode(owner);
        if (ownerNode.sourceNodeIndex < 0 || static_cast<size_t>(ownerNode.sourceNodeIndex) >= state.data.nodes.size()) {
          fail("mesh owner has no configured source node identity");
        }
        const uint32_t sourceNode = static_cast<uint32_t>(ownerNode.sourceNodeIndex);
        const auto& source = state.data.nodes[sourceNode];
        if (source.sourceMesh < 0 || static_cast<size_t>(source.sourceMesh) != primitive.sourceMesh) {
          fail("mesh owner source mesh differs from the primitive's source mesh");
        }
        if (source.skin >= 0 && !primitive.hasSkinning()) fail("skin references a primitive without influences");
        const bool skinned = source.skin >= 0 && primitive.hasSkinning();
        if (primitive.hasMorphing() || skinned) {
          if (primitive.bindVertices.empty() || sourceRange.indexCount % 3 != 0) fail("deforming primitive must contain triangle vertices/indices");
          const auto key=std::pair{owner,primitiveIndex};
          if (const auto existing=ownerPrimitives.find(key);existing!=ownerPrimitives.end()) {
            // Instance transforms apply after the shared owner-local pose.
            // Reuse one deformation stream/palette per owner and primitive.
            range.instance=narrow(existing->second);
            range.vertexOffset=candidates[existing->second].outputOffset;
            range.vertexCount=narrow(state.definitionPrimitive(uint32_t(definition)).bindVertices.size());
            range.firstIndex=candidates[existing->second].firstIndex;
            candidateRanges.push_back(range);
            continue;
          }
          Impl::Instance instance;
          instance.primitive = primitiveIndex;
          instance.definition = uint32_t(definition);
          instance.owner = owner;
          instance.sourceNode = sourceNode;
          instance.skin = skinned ? source.skin : -1;
          instance.outputOffset = narrow(vertexOffset);
          instance.firstIndex = narrow(firstIndex);
          range.instance = narrow(candidates.size());
          range.firstIndex = narrow(firstIndex);
          range.vertexOffset = instance.outputOffset;
          range.vertexCount = narrow(state.definitionPrimitive(uint32_t(definition)).bindVertices.size());
          vertexOffset += range.vertexCount;
          firstIndex += sourceRange.indexCount;
          narrow(vertexOffset);
          narrow(firstIndex);
          ownerPrimitives.emplace(key,candidates.size());
          candidates.push_back(std::move(instance));
        }
      }
    }
    candidateRanges.push_back(range);
  }
  bool topologyChanged = state.sourceTopologyChanged || candidateRanges.size() != state.ranges.size();
  if (!topologyChanged) {
    topologyChanged = !std::equal(candidateRanges.begin(), candidateRanges.end(), state.ranges.begin(), sameRange);
  }
  bool dynamicLayoutChanged = state.sourceTopologyChanged || candidates.size() != state.instances.size();
  if (!dynamicLayoutChanged) {
    for (size_t i = 0; i < candidates.size(); ++i) {
      dynamicLayoutChanged |= !Impl::sameIdentity(candidates[i], state.instances[i]) ||
          candidates[i].outputOffset != state.instances[i].outputOffset;
    }
  }
  topologyChanged |= dynamicLayoutChanged;
  // Stage replacement storage and poses before publishing any new state, so a
  // missing owner/joint or invalid animation value leaves the prior frame valid.
  std::vector<geometry::Vertex> replacementBind;
  std::vector<uint32_t> replacementIndices;
  if (dynamicLayoutChanged && !candidates.empty()) {
    replacementBind = state.sourceVertices;
    replacementIndices = state.sourceIndices;
    replacementBind.reserve(vertexOffset);
    replacementIndices.reserve(firstIndex);
    for (const auto& instance : candidates) {
      const auto& primitive = state.definitionPrimitive(instance.definition);
      replacementBind.insert(replacementBind.end(), primitive.bindVertices.begin(), primitive.bindVertices.end());
      for (const auto index : state.definitionIndices(instance.definition)) {
        replacementIndices.push_back(instance.outputOffset + index);
      }
    }
  }
  std::map<std::pair<uint32_t,uint32_t>, size_t> previousInstances;
  for (size_t i = 0; i < state.instances.size(); ++i)
    previousInstances.emplace(std::pair{state.instances[i].owner,state.instances[i].primitive}, i);
  bool poseChanged = false;
  bool anyReset = false;
  std::vector<glm::mat4> sourceWorlds(state.data.nodes.size(), glm::mat4(1));
  for (size_t i = 0; i < sourceGraphNodes.size(); ++i) {
    if (sourceGraphNodes[i].size() == 1) sourceWorlds[i] = graph.getNode(sourceGraphNodes[i][0])->worldTransform;
  }
  for (auto& instance : candidates) {
    const uint32_t outputOffset = instance.outputOffset;
    const uint32_t indexOffset = instance.firstIndex;
    if (const auto found = previousInstances.find(std::pair{instance.owner,instance.primitive}); found != previousInstances.end()) {
      auto& old = state.instances[found->second];
      if (Impl::sameIdentity(instance, old)) {
        instance = old;
        instance.outputOffset = outputOffset;
        instance.firstIndex = indexOffset;
      }
    }
    const auto& primitive = state.definitionPrimitive(instance.definition);
    std::vector<float> weights = nodeWeights(state.data, instance.sourceNode,
                                               primitive.morphTargets.size(), evaluatedSource);
    std::vector<glm::mat4> palette;
    if (instance.skin >= 0) {
      if (static_cast<size_t>(instance.skin) >= state.data.skins.size()) fail("source skin identity is out of range");
      const auto& skin = state.data.skins[static_cast<size_t>(instance.skin)];
      for (const uint32_t joint : skin.joints) {
        if (joint >= sourceGraphNodes.size() || sourceGraphNodes[joint].empty()) fail("required joint runtime node is missing/deleted");
        if (sourceGraphNodes[joint].size() != 1) fail("required joint runtime source identity is ambiguous");
      }
      palette = gltf::ComputeSkinPalette(skin, graph.getNode(instance.owner)->worldTransform, sourceWorlds);
    }
    const bool changed = !instance.hasCurrent || instance.weights != weights || instance.palette != palette;
    if (changed) {
      instance.current = state.preparedFrames[instance.definition]
          ? gltf::EvaluateDeformationFrames(*state.preparedFrames[instance.definition], weights, palette)
          : gltf::EvaluatePrimitive(primitive, weights, palette);
      instance.bounds = bounds(instance.current, state.definitionIndices(instance.definition));
      instance.orientation = orientation(primitive, instance.current,
          state.definitionIndices(instance.definition), palette);
      instance.weights = std::move(weights);
      instance.palette = std::move(palette);
      instance.hasCurrent = true;
      poseChanged = true;
    }
    anyReset |= state.resetPending || !instance.submitted;
  }
  for (auto& range : candidateRanges) {
    if (range.instance == invalid) continue;
    range.bounds = candidates[range.instance].bounds;
    range.orientation = candidates[range.instance].orientation;
  }
  const bool geometryChanged = state.sourceGeometryChanged || poseChanged ||
      (dynamicLayoutChanged && (!state.instances.empty() || !candidates.empty()));
  if (dynamicLayoutChanged) {
    state.expandedBind = std::move(replacementBind);
    state.expandedIndices = std::move(replacementIndices);
  }
  state.instances = std::move(candidates);
  state.ranges = std::move(candidateRanges);
  state.nodeRanges.assign(graph.nodeCount(), invalid);
  for (size_t i = 0; i < state.ranges.size(); ++i) state.nodeRanges[state.ranges[i].graphNode] = narrow(i);
  state.assemblePoses();
  state.rebuildGpuViews();
  if (geometryChanged) ++state.geometryRevision;
  if (topologyChanged) ++state.topologyRevision;
  state.sourceGeometryChanged = false;
  state.sourceTopologyChanged = false;
  return {topologyChanged, geometryChanged, anyReset};
}

void GltfDeformationScene::commitSubmitted() {
  auto& state = *impl_;
  for (auto& instance : state.instances) {
    instance.submittedWeights = instance.weights;
    instance.submittedPalette = instance.palette;
    instance.submittedVertices = instance.current;
    instance.submitted = true;
  }
  state.resetPending = false;
  state.assemblePoses();
  state.rebuildGpuViews();
}

std::span<const geometry::Vertex> GltfDeformationScene::bindVertices() const {
  return impl_->instances.empty() ? std::span<const geometry::Vertex>(impl_->sourceVertices)
                                  : std::span<const geometry::Vertex>(impl_->expandedBind);
}
std::span<const geometry::Vertex> GltfDeformationScene::currentVertices() const {
  return impl_->instances.empty() ? std::span<const geometry::Vertex>(impl_->sourceVertices)
                                  : std::span<const geometry::Vertex>(impl_->current);
}
std::span<const geometry::Vertex> GltfDeformationScene::previousVertices() const {
  return impl_->instances.empty() ? std::span<const geometry::Vertex>(impl_->sourceVertices)
                                  : std::span<const geometry::Vertex>(impl_->previous);
}
std::span<const uint32_t> GltfDeformationScene::indices() const {
  return impl_->instances.empty() ? std::span<const uint32_t>(impl_->sourceIndices)
                                  : std::span<const uint32_t>(impl_->expandedIndices);
}
const GltfDeformationDrawRange* GltfDeformationScene::drawRange(uint32_t graphNode) const {
  if (graphNode >= impl_->nodeRanges.size() || impl_->nodeRanges[graphNode] == invalid) return nullptr;
  return &impl_->ranges[impl_->nodeRanges[graphNode]];
}
std::span<const GltfDeformationDrawRange> GltfDeformationScene::drawRanges() const { return impl_->ranges; }
std::span<const GltfDeformationInstance> GltfDeformationScene::gpuInstances() const { return impl_->gpuInstances; }
std::span<const GltfDeformationPose> GltfDeformationScene::gpuPoses() const { return impl_->gpuPoses; }
uint64_t GltfDeformationScene::geometryRevision() const { return impl_->geometryRevision; }
uint64_t GltfDeformationScene::topologyRevision() const { return impl_->topologyRevision; }

}  // namespace container::renderer
