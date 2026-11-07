#include <Container/utility/SceneManager.h>

#include <Container/geometry/GltfLinkedAssets.h>

#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace container::scene {
namespace {
using namespace container::geometry::gltf;

constexpr size_t kMaxContexts = 4096;
constexpr size_t kMaxPlacements = 131072;
constexpr size_t kMaxNodes = GltfMergeLimits{}.maxNodes;
constexpr uint64_t kMaxOwnershipBytes = 128ull * 1024 * 1024;

[[noreturn]] void invalidOwnership(const char* message) {
  throw std::runtime_error(std::string("Linked asset ownership: ") + message);
}

GltfLinkedIndexRange projectRange(GltfLinkedIndexRange range, std::span<const int32_t> map) {
  if (!range.count || map.empty()) return range;
  const auto first = size_t(range.first);
  if (first >= map.size() || range.count > map.size() - first ||
      map[first] < 0 || map[first + range.count - 1] != int64_t(map[first]) + int64_t(range.count) - 1)
    invalidOwnership("resource indices no longer map to a contiguous range");
  return {map[first], range.count};
}

uint64_t originBytes(const GltfLinkedCompositionInfo& composition) {
  uint64_t bytes = sizeof(composition) +
      composition.placements.capacity() * sizeof(int) +
      composition.sourceContexts.capacity() * sizeof(GltfLinkedSourceOwnership) +
      composition.placementRecords.capacity() * sizeof(GltfLinkedPlacementOwnership) +
      composition.nodePlacements.capacity() * sizeof(int32_t) + composition.dependencyWitnessBytes +
      composition.bindingResolutions.capacity() * sizeof(std::set<std::filesystem::path>);
  const auto pathBytes = [](const auto& path) {
    return uint64_t(path.native().capacity()) * sizeof(std::filesystem::path::value_type);
  };
  bytes += pathBytes(composition.source);
  for (const auto& [requested, resolved] : composition.resolutions)
    bytes += 128 + pathBytes(requested) + pathBytes(resolved);
  for (const auto& input : composition.inputs)
    bytes += 128 + pathBytes(input.first);
  for (const auto& context : composition.sourceContexts) {
    bytes += pathBytes(context.source);
    for (const auto& [uri, path] : context.aliases) bytes += 128 + pathBytes(uri) + pathBytes(path);
    for (const auto& light : context.extensionLights) bytes += 128 + light.first.capacity();
  }
  return bytes;
}

} // namespace

struct SceneManager::LinkedAssetState {
  using Origin = std::shared_ptr<const GltfLinkedCompositionInfo>;
  struct Context {
    size_t origin{}, source{}, group{};
    GltfLinkedIndexRange materials, cameras, images, textures, samplers;
    size_t implicitMaterialPrimitive{SIZE_MAX};
    bool linked{true};
  };
  struct Placement {
    size_t context{};
    int32_t parent{-1};
  };
  struct Node {
    int32_t payload{-1}, carrier{-1};
  };

  std::vector<Origin> origins;
  std::vector<Context> contexts;
  std::vector<Placement> placements;
  std::vector<Node> nodes;
  uint64_t retainedOriginBytes{};

  [[nodiscard]] uint64_t ownedBytes() const noexcept {
    return sizeof(*this) + retainedOriginBytes + origins.capacity() * sizeof(Origin) +
        contexts.capacity() * sizeof(Context) + placements.capacity() * sizeof(Placement) +
        nodes.capacity() * sizeof(Node);
  }

  [[nodiscard]] bool linked(int32_t placement) const noexcept {
    return placement >= 0 && contexts[placements[size_t(placement)].context].linked;
  }

  [[nodiscard]] const GltfLinkedSourceOwnership& identity(const Context& context) const {
    return origins[context.origin]->sourceContexts[context.source];
  }

  void project(const GltfLinkedCompositionInfo& origin, size_t firstContext, std::span<const int32_t> map) {
    const auto nodeIndex = [&](size_t id) {
      if (id >= origin.nodePlacements.size()) invalidOwnership("source node index is outside its ownership table");
      return map.empty() ? int32_t(id) : map[id];
    };
    std::vector<int32_t> remapped(origin.placementRecords.size(), -1);
    for (size_t i = 0; i < origin.placementRecords.size(); ++i) {
      const auto& input = origin.placementRecords[i];
      const auto carrier = nodeIndex(size_t(input.carrierNode));
      if (carrier < 0) continue;
      if (size_t(carrier) >= nodes.size() || nodes[size_t(carrier)].carrier >= 0 ||
          input.sourceContext >= origin.sourceContexts.size() || input.parentPlacement >= int64_t(i))
        invalidOwnership("placement carrier or context is invalid");
      const auto parent = input.parentPlacement < 0 ? -1 : remapped[size_t(input.parentPlacement)];
      if (input.parentPlacement >= 0 && parent < 0) invalidOwnership("nested carrier lost its parent placement");
      remapped[i] = int32_t(placements.size());
      nodes[size_t(carrier)].carrier = remapped[i];
      placements.push_back({firstContext + input.sourceContext, parent});
    }
    for (size_t i = 0; i < origin.nodePlacements.size(); ++i) {
      const auto owner = origin.nodePlacements[i];
      if (owner < 0) continue;
      const auto node = nodeIndex(i);
      if (node < 0) continue;
      if (size_t(owner) >= remapped.size() || remapped[size_t(owner)] < 0 ||
          size_t(node) >= nodes.size() || nodes[size_t(node)].payload >= 0)
        invalidOwnership("payload node lost its concrete placement");
      nodes[size_t(node)].payload = remapped[size_t(owner)];
    }
  }

  void append(Origin origin, const tinygltf::Model& model, const GltfMergePlan* merge) {
    const auto projectionCount = merge ? 2 : 1;
    if (model.nodes.size() > kMaxNodes || origin->sourceContexts.size() > kMaxContexts - contexts.size() ||
        origin->placementRecords.size() > (kMaxPlacements - placements.size()) / projectionCount)
      invalidOwnership("merged linked context, placement, or node budget exceeded");
    if (merge && (merge->imported.nodes.size() != origin->nodePlacements.size() ||
                  merge->placement.nodes.size() != origin->nodePlacements.size()))
      invalidOwnership("incoming node ownership and merge projections differ");
    const auto retained = originBytes(*origin);
    if (retained > kMaxOwnershipBytes - retainedOriginBytes)
      invalidOwnership("retained linked source metadata budget exceeded");
    retainedOriginBytes += retained;
    const auto originIndex = origins.size();
    origins.push_back(std::move(origin));
    const auto& input = *origins.back();
    const auto firstContext = contexts.size();
    const GltfMergeIndexMaps empty;
    const auto& maps = merge ? merge->imported : empty;
    nodes.resize(model.nodes.size());
    size_t mesh = 0, primitive = 0;
    for (size_t i = 0; i < input.sourceContexts.size(); ++i) {
      const auto& source = input.sourceContexts[i];
      const auto meshes = projectRange(source.meshes, maps.meshes);
      size_t implicitMaterialPrimitive = SIZE_MAX;
      if (meshes.count) {
        const auto first = size_t(meshes.first);
        if (first < mesh || first >= model.meshes.size() || meshes.count > model.meshes.size() - first)
          invalidOwnership("source mesh ranges are unordered or outside the composed model");
        for (; mesh < first; ++mesh) primitive += model.meshes[mesh].primitives.size();
        for (; mesh < first + meshes.count; ++mesh) {
          for (const auto& surface : model.meshes[mesh].primitives) {
            if (surface.material < 0 && implicitMaterialPrimitive == SIZE_MAX)
              implicitMaterialPrimitive = primitive;
            ++primitive;
          }
        }
      }
      size_t group = contexts.size();
      for (const auto& previous : contexts) {
        const auto& previousIdentity = identity(previous);
        if (previousIdentity.source == source.source && previousIdentity.aliases == source.aliases) {
          group = previous.group;
          break;
        }
      }
      contexts.push_back({originIndex, i, group,
          projectRange(source.materials, maps.materials), projectRange(source.cameras, maps.cameras),
          projectRange(source.images, maps.images), projectRange(source.textures, maps.textures),
          projectRange(source.samplers, maps.samplers), implicitMaterialPrimitive});
    }
    project(input, firstContext, maps.nodes);
    if (merge) project(input, firstContext, merge->placement.nodes);
    if (ownedBytes() > kMaxOwnershipBytes)
      invalidOwnership("retained linked ownership storage budget exceeded");
  }

  [[nodiscard]] int32_t findPlacement(const SceneGraph& graph, uint32_t id) const noexcept {
    const auto forSource = [&](const SceneNode& node) {
      if (node.sourceNodeIndex < 0 || size_t(node.sourceNodeIndex) >= nodes.size()) return int32_t(-1);
      const auto& binding = nodes[size_t(node.sourceNodeIndex)];
      if (linked(binding.carrier)) return binding.carrier;
      return linked(binding.payload) ? binding.payload : -1;
    };
    const auto* node = graph.getNode(id);
    if (!node || node->retired) return -1;
    if (const auto placement = forSource(*node); placement >= 0) return placement;
    const auto* owner = graph.getNode(node->meshOwnerGraphNode);
    return owner && !owner->retired ? forSource(*owner) : -1;
  }
};

void SceneManager::initializeLinkedAssetState() {
  if (!linkedComposition_) return;
  auto state = std::make_shared<LinkedAssetState>();
  if (linkedComposition_->nodePlacements.size() != gltfModel_.nodes.size())
    invalidOwnership("primary node ownership does not cover the composed scene");
  state->append(linkedComposition_, gltfModel_, nullptr);
  linkedAssetState_ = std::move(state);
}

SceneManager::LinkedAssetStatePtr SceneManager::prepareLinkedMergeOwnership(
    const container::geometry::GltfLoadResult& incoming, const GltfMergePlan& plan) const {
  if (!incoming.linkedComposition) return linkedAssetState_;
  auto state = linkedAssetState_ ? std::make_shared<LinkedAssetState>(*linkedAssetState_)
                                 : std::make_shared<LinkedAssetState>();
  state->append(incoming.linkedComposition, plan.model, &plan);
  return state;
}

uint64_t SceneManager::linkedAssetStateBytes() const noexcept {
  return linkedAssetState_ ? linkedAssetState_->ownedBytes() : 0;
}

void SceneManager::validateSceneExportDestination(const std::filesystem::path& destination) const {
  if (linkedAssetState_)
    for (const auto& origin : linkedAssetState_->origins)
      ValidateGltfLinkedExportDestination(*origin, destination);
}

bool SceneManager::isLinkedSourceCarrier(size_t sourceNode) const noexcept {
  const auto* state = linkedAssetState_.get();
  return state && sourceNode < state->nodes.size() && state->linked(state->nodes[sourceNode].carrier);
}

bool SceneManager::isLinkedSourceNodeReadOnly(size_t sourceNode) const noexcept {
  const auto* state = linkedAssetState_.get();
  return state && sourceNode < state->nodes.size() && state->linked(state->nodes[sourceNode].payload);
}

bool SceneManager::isLinkedNodeReadOnly(const SceneGraph& graph, uint32_t graphNode) const noexcept {
  const auto* node = graph.getNode(graphNode);
  if (!node) return false;
  if (node->sourceNodeIndex >= 0 && isLinkedSourceNodeReadOnly(size_t(node->sourceNodeIndex))) return true;
  const auto* owner = graph.getNode(node->meshOwnerGraphNode);
  return owner && owner->sourceNodeIndex >= 0 && isLinkedSourceNodeReadOnly(size_t(owner->sourceNodeIndex));
}

bool SceneManager::isLinkedMaterialReadOnly(uint32_t runtime) const noexcept {
  if (!linkedAssetState_) return false;
  for (const auto& context : linkedAssetState_->contexts) {
    if (!context.linked) continue;
    const auto implicit = context.implicitMaterialPrimitive;
    if (implicit != SIZE_MAX &&
        (sourcePrimitiveImplicitMaterials_.empty() ? runtime == defaultMaterialIndex_
          : implicit < sourcePrimitiveImplicitMaterials_.size() &&
            runtime == sourcePrimitiveImplicitMaterials_[implicit]))
      return true;
    const auto first = size_t(context.materials.first);
    const auto end = std::min(first + context.materials.count, sourceMaterialToRuntime_.size());
    for (size_t i = first; i < end; ++i)
      if (sourceMaterialToRuntime_[i] == runtime) return true;
  }
  return false;
}

bool SceneManager::isLinkedCameraReadOnly(int32_t authored, int32_t evaluated) const noexcept {
  return linkedAssetState_ && std::ranges::any_of(linkedAssetState_->contexts, [&](const auto& context) {
    return context.linked && (context.cameras.contains(authored) || context.cameras.contains(evaluated));
  });
}

bool SceneManager::isLinkedResourceReadOnly(const TextureResourceSelection& resource) const noexcept {
  using container::util::ResourceDomainKind;
  using container::util::ResourceKind;
  if (!linkedAssetState_ || resource.row > uint32_t(INT32_MAX)) return false;
  const auto row = int32_t(resource.row);
  const auto usesRuntime = [&](const auto& roles, GltfLinkedIndexRange range) {
    const auto uses = [&](const auto& ids) {
      const auto first = std::min(size_t(range.first), ids.size());
      const auto end = std::min(first + range.count, ids.size());
      return std::find(ids.begin() + first, ids.begin() + end, resource.row) != ids.begin() + end;
    };
    return uses(roles.color) || uses(roles.data);
  };
  for (const auto& context : linkedAssetState_->contexts) {
    if (!context.linked) continue;
    if (resource.domain == ResourceDomainKind::Runtime && resource.kind == ResourceKind::RuntimeTexture &&
        (usesRuntime(gltfTextureResources_, context.textures) || usesRuntime(archiveTextureResources_, context.textures)))
      return true;
    if (resource.domain != ResourceDomainKind::Archive && resource.domain != ResourceDomainKind::Evaluated)
      continue;
    if ((resource.kind == ResourceKind::Texture && context.textures.contains(row)) ||
        (resource.kind == ResourceKind::Image && context.images.contains(row)) ||
        (resource.kind == ResourceKind::Sampler && context.samplers.contains(row)) ||
        (resource.kind == ResourceKind::Material && context.materials.contains(row)))
      return true;
  }
  return false;
}

bool SceneManager::canMakeLinkedAssetEditable(const SceneGraph& graph, uint32_t graphNode) const noexcept {
  return linkedAssetState_ && linkedAssetState_->findPlacement(graph, graphNode) >= 0;
}

size_t SceneManager::makeLinkedAssetEditable(const SceneGraph& graph, uint32_t graphNode) {
  if (!linkedAssetState_) return 0;
  const auto selected = linkedAssetState_->findPlacement(graph, graphNode);
  if (selected < 0) return 0;
  auto state = std::make_shared<LinkedAssetState>(*linkedAssetState_);
  // A nested payload belongs to an opaque link. Detach the connected
  // placement families together so no linked occurrence observes edits.
  std::vector<size_t> groups(state->contexts.size());
  std::iota(groups.begin(), groups.end(), size_t(0));
  const auto root = [&](size_t group) {
    while (groups[group] != group) {
      groups[group] = groups[groups[group]];
      group = groups[group];
    }
    return group;
  };
  for (const auto& placement : state->placements) {
    const auto& context = state->contexts[placement.context];
    if (!context.linked || !state->linked(placement.parent)) continue;
    const auto& parent = state->contexts[state->placements[size_t(placement.parent)].context];
    groups[root(context.group)] = root(parent.group);
  }
  const auto family = root(state->contexts[state->placements[size_t(selected)].context].group);
  const auto detaches = [&](const auto& context) { return context.linked && root(context.group) == family; };
  const auto count = std::ranges::count_if(state->placements, [&](const auto& placement) {
    return detaches(state->contexts[placement.context]);
  });
  for (auto& context : state->contexts)
    if (detaches(context)) context.linked = false;
  linkedAssetState_ = std::move(state);
  return size_t(count);
}

} // namespace container::scene
