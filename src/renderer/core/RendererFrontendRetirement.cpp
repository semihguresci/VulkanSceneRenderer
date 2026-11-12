#include "Container/renderer/core/RendererFrontend.h"

#include "Container/geometry/GltfAnimation.h"
#include "Container/renderer/lighting/ImportedSceneLightEdits.h"
#include "Container/renderer/lighting/LightingManager.h"
#include "Container/renderer/temporal/TemporalManager.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/SceneNodeAuthoringPolicy.h"
#if CONTAINER_HAS_JOLT
#include "Container/physics/PhysicsElementBindings.h"
#include "Container/physics/PhysicsScene.h"
#endif

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace container::renderer {
namespace {
using container::scene::SceneGraph;
using container::scene::SceneNodeLocalPose;
using container::geometry::gltf::AnimationPlayer;
constexpr uint64_t snapshotLimit = 128ull * 1024 * 1024;
constexpr uint64_t stagingLimit = 512ull * 1024 * 1024;
constexpr size_t nodeLimit = 262144;

[[noreturn]] void retirementError(const char* message) {
  throw std::invalid_argument(std::string("Delete scene subtree: ") + message);
}

// Bound every recursively owned typed-table allocation before copying it.
// Resource buffers/images, accessor payloads and geometry are never copied.
struct SnapshotBudget {
  uint64_t bytes{}, values{};
  void charge(uint64_t count, uint64_t stride = 1) {
    if (count > (snapshotLimit - bytes) / stride)
      retirementError("deletion-time snapshot exceeds the 128 MiB bound");
    bytes += count * stride;
  }
  void text(const std::string& value) { charge(value.size()); charge(1); }
  template<class T> void flat(const std::vector<T>& values) { charge(values.size(), sizeof(T)); }
  void value(const tinygltf::Value& root) {
    struct Entry { const tinygltf::Value* value; uint32_t depth; };
    std::vector<Entry> pending{{&root, 0}};
    while (!pending.empty()) {
      const auto [pointer, depth] = pending.back(); pending.pop_back();
      if (depth > 64 || ++values > 1048576) retirementError("snapshot metadata nesting/object bound exceeded");
      const auto& item = *pointer;
      charge(1, sizeof(tinygltf::Value) + 64);
      if (item.IsString()) text(item.Get<std::string>());
      else if (item.IsBinary()) flat(item.Get<std::vector<unsigned char>>());
      else if (item.IsNumber() && !std::isfinite(item.GetNumberAsDouble())) retirementError("nonfinite snapshot metadata");
      else if (item.IsArray()) {
        const auto& array = item.Get<tinygltf::Value::Array>();
        if (array.size() > 1048576 - values || pending.size() > 1048576 - array.size())
          retirementError("snapshot metadata object bound exceeded");
        for (const auto& child : array) pending.push_back({&child, depth + 1});
      } else if (item.IsObject()) {
        const auto& object = item.Get<tinygltf::Value::Object>();
        if (object.size() > 1048576 - values || pending.size() > 1048576 - object.size())
          retirementError("snapshot metadata object bound exceeded");
        for (const auto& [name, child] : object) { text(name); pending.push_back({&child, depth + 1}); }
      }
    }
  }
  template<class T> void property(const T& item) {
    if constexpr (requires { item.name; }) text(item.name);
    text(item.extras_json_string); text(item.extensions_json_string); value(item.extras);
    for (const auto& [name, extension] : item.extensions) { charge(96); text(name); value(extension); }
  }
  void node(const tinygltf::Node& item) {
    property(item); flat(item.children); flat(item.lods); flat(item.matrix);
    flat(item.translation); flat(item.rotation); flat(item.scale); flat(item.weights);
  }
  void material(const tinygltf::Material& item) {
    property(item); text(item.alphaMode); flat(item.emissiveFactor); flat(item.lods);
    const auto& pbr = item.pbrMetallicRoughness;
    property(pbr); flat(pbr.baseColorFactor); property(pbr.baseColorTexture); property(pbr.metallicRoughnessTexture);
    property(item.normalTexture); property(item.occlusionTexture); property(item.emissiveTexture);
    for (const auto* map : {&item.values, &item.additionalValues})
      for (const auto& [name, parameter] : *map) {
        charge(1, sizeof(parameter) + 96); text(name); text(parameter.string_value); flat(parameter.number_array);
        for (const auto& [key, number] : parameter.json_double_value) { (void)number; charge(96); text(key); }
      }
  }
  void light(const tinygltf::Light& item) { property(item); text(item.type); flat(item.color); property(item.spot); }
  void camera(const tinygltf::Camera& item) { property(item); text(item.type); property(item.perspective); property(item.orthographic); }
  template<class N, class M, class L, class C> void tables(const N& nodes, const M& materials, const L& lights, const C& cameras) {
    flat(nodes); flat(materials); flat(lights); flat(cameras);
    for (const auto& item : nodes) node(item);
    for (const auto& item : materials) material(item);
    for (const auto& item : lights) light(item);
    for (const auto& item : cameras) camera(item);
  }
  void player(const AnimationPlayer* player) {
    if (!player) return;
    charge(sizeof(*player)); const auto& library = player->library();
    tables(library.restNodes, library.restMaterials, library.restLights, library.restCameras);
    flat(library.clips); flat(library.diagnostics);
    for (const auto& clip : library.clips) {
      text(clip.name); flat(clip.tracks);
      for (const auto& track : clip.tracks) { text(track.target.property); flat(track.times); flat(track.values); }
    }
    for (const auto& diagnostic : library.diagnostics) text(diagnostic.message);
  }
  void lightEdits(const SceneLightEdits& edits) {
    charge(sizeof(edits)); flat(edits.generatedPointOverrides); flat(edits.importedPointOverrides);
    flat(edits.generatedAreaOverrides); flat(edits.importedAreaOverrides);
    flat(edits.manualPointLights); flat(edits.manualAreaLights);
    if (edits.directionalOverride) text(edits.directionalOverride->label);
  }
  void imported(const ImportedSceneLightEdits& edits) {
    charge(sizeof(edits));
    for (const auto& [binding, value] : edits.points) { charge(sizeof(value) + 128); text(binding.extension); }
    for (const auto& [binding, value] : edits.areas) { charge(sizeof(value) + 128); text(binding.extension); }
    for (const auto& [binding, value] : edits.directionals) {
      charge(sizeof(value) + 128); text(binding.extension); if (value) text(value->label);
    }
  }
};

bool finite(const glm::mat4& matrix) {
  for (int column = 0; column < 4; ++column)
    for (int row = 0; row < 4; ++row) if (!std::isfinite(matrix[column][row])) return false;
  return true;
}

container::scene::SceneNodeAuthoringSubtree validateBindings(const SceneGraph& graph,
    const tinygltf::Model& source, uint32_t root
#if CONTAINER_HAS_JOLT
    , const container::physics::PhysicsScene* physics, const container::physics::PhysicsElementBindings* products
#endif
    ) {
  auto closure = container::scene::CollectSceneNodeAuthoringSubtree(graph, root);
  std::vector<uint8_t> removed(graph.nodeCount(), 0);
  for (const auto id : closure.nodes) removed[id] = 1;
  std::vector<uint32_t> bodies, endpoints, owned, skinDependencies;
#if CONTAINER_HAS_JOLT
  if (physics && physics->jointCount() > nodeLimit / 2)
    retirementError("joint endpoint metadata exceeds the traversal bound");
  for (uint32_t id = 0; id < graph.nodeCount(); ++id) {
    if (physics && physics->graphIdentity() == graph.identity() && physics->hasBody(id)) bodies.push_back(id);
    if (products && products->ownsNode(graph, id)) owned.push_back(id);
  }
  if (physics && physics->graphIdentity() == graph.identity())
    for (const auto& joint : physics->joints()) {
      if (joint.nodeA) endpoints.push_back(*joint.nodeA);
      endpoints.push_back(joint.nodeB);
    }
#endif
  if (source.nodes.size() > nodeLimit || source.skins.size() > nodeLimit)
    retirementError("source/skin identity metadata exceeds the graph bound");
  std::vector<uint32_t> mapping(source.nodes.size(), SceneGraph::kInvalidNode);
  for (uint32_t id = 0; id < graph.nodeCount(); ++id) {
    const auto index = graph.getNode(id)->sourceNodeIndex;
    if (index == -1) continue;
    if (index < 0 || size_t(index) >= mapping.size() || mapping[size_t(index)] != SceneGraph::kInvalidNode)
      retirementError("invalid or ambiguous source-node identity");
    mapping[size_t(index)] = id;
  }
  // All retained skins matter, including inactive scene content. Deleting a
  // complete private skin closure is allowed; deleting a joint needed outside
  // that closure must reject before the graph/source/history is changed.
  std::vector<uint8_t> retainedSkins(source.skins.size(), 0);
  for (uint32_t id = 0; id < graph.nodeCount(); ++id) {
    const auto& node = *graph.getNode(id);
    if (removed[id] || node.retired || node.sourceNodeIndex < 0) continue;
    const auto& authored = source.nodes[size_t(node.sourceNodeIndex)];
    if (authored.skin < 0) continue;
    if (size_t(authored.skin) >= source.skins.size()) retirementError("retained skin index is invalid");
    retainedSkins[size_t(authored.skin)] = 1;
  }
  const auto dependency = [&](int sourceNode) {
    if (sourceNode < 0 || size_t(sourceNode) >= mapping.size() || mapping[size_t(sourceNode)] == SceneGraph::kInvalidNode)
      retirementError("retained skin joint/skeleton has no runtime binding");
    skinDependencies.push_back(mapping[size_t(sourceNode)]);
  };
  uint64_t jointReferences = 0;
  for (size_t skin = 0; skin < retainedSkins.size(); ++skin) if (retainedSkins[skin]) {
    const uint64_t references = uint64_t(source.skins[skin].joints.size()) +
        uint64_t(source.skins[skin].skeleton >= 0);
    if (references > 1048576 - jointReferences)
      retirementError("retained skin dependency metadata exceeds the traversal bound");
    jointReferences += references;
    for (const auto joint : source.skins[skin].joints) dependency(joint);
    if (source.skins[skin].skeleton >= 0) dependency(source.skins[skin].skeleton);
  }
  // Restriction input cardinality is bounded even for many shared skins.
  std::ranges::sort(skinDependencies);
  skinDependencies.erase(std::unique(skinDependencies.begin(), skinDependencies.end()), skinDependencies.end());
  container::scene::SceneNodeAuthoringRestrictions restrictions;
  restrictions.physicsBodyNodes = bodies; restrictions.jointEndpointNodes = endpoints;
  restrictions.physicsProductNodes = owned; restrictions.skinDependencyNodes = skinDependencies;
  closure = container::scene::CollectSceneNodeAuthoringSubtree(graph, root, restrictions);
  if (closure.containsProtectedNodes || closure.containsPhysicsProductNodes || closure.containsPhysicsBodies ||
      closure.containsJointEndpoints || closure.containsSkinDependencies)
    retirementError("detach protected, physics, joint or retained skin dependencies before deleting this subtree");
  return closure;
}

} // namespace

struct RendererFrontend::SceneRetirementBudget {
  static constexpr uint64_t limit = 512ull * 1024 * 1024;
  uint64_t reserved{};
};

struct RendererFrontend::SceneRetirementHistory {
  struct Publication {
    std::vector<tinygltf::Node> nodes;
    std::vector<tinygltf::Material> materials;
    std::vector<tinygltf::Light> lights;
    std::vector<tinygltf::Camera> cameras;
    std::unique_ptr<AnimationPlayer> player;
    std::shared_ptr<ImportedSceneLightEdits> imported;
    std::map<size_t, std::shared_ptr<SceneLightEdits>> sceneEdits;
  };
  ~SceneRetirementHistory() { if (budget) budget->reserved -= reservation; }
  std::shared_ptr<SceneRetirementBudget> budget;
  std::weak_ptr<SceneRetirementHistory> self;
  uint64_t reservation{}, identity{};
  size_t slotCount{}, selectedScene{};
  uint32_t root{SceneGraph::kInvalidNode};
  bool retired{};
  std::optional<SceneGraph::SubtreeRetirement> receipt;
  std::vector<uint8_t> closure;
  std::vector<SceneNodeLocalPose> poses;
  Publication saved;
  SceneLightEdits lightEdits;
  container::geometry::gltf::AnimationDirtySet dirty;
  std::optional<size_t> camera;
  SceneEditSelection before{}, after{};

  Publication stage() const {
    Publication result;
    result.nodes = saved.nodes; result.materials = saved.materials;
    result.lights = saved.lights; result.cameras = saved.cameras;
    if (saved.player) result.player = std::make_unique<AnimationPlayer>(*saved.player);
    result.imported = std::make_shared<ImportedSceneLightEdits>(*saved.imported);
    for (const auto& [scene, edits] : saved.sceneEdits)
      result.sceneEdits.emplace(scene, std::make_shared<SceneLightEdits>(*edits));
    return result;
  }
  void publish(RendererFrontend& frontend, Publication& next, bool deleting) noexcept {
    auto& source = frontend.subs_.sceneManager->animationSource();
    source.nodes.swap(next.nodes); source.materials.swap(next.materials);
    source.lights.swap(next.lights); source.cameras.swap(next.cameras);
    frontend.gltfAnimation_.swap(next.player);
    frontend.gltfImportedLightEdits_.swap(next.imported);
    frontend.gltfSceneLightEdits_.swap(next.sceneEdits);
    frontend.gltfAnimationCameraNode_ = camera;
    if (deleting && camera)
      for (uint32_t id = 0; id < frontend.sceneGraph_.nodeCount(); ++id)
        if (closure[id] && frontend.sceneGraph_.getNode(id)->sourceNodeIndex == static_cast<int64_t>(*camera)) {
          frontend.gltfAnimationCameraNode_.reset(); break;
        }
    frontend.gltfAnimationCamera_.reset();
    frontend.gltfAnimationGraphIdentity_ = frontend.sceneGraph_.identity();
    frontend.pendingGltfAnimationDeltaSeconds_ = 0;
    frontend.restoreSceneEditSelection(deleting ? after : before);
    if (frontend.hoveredMeshNode_ < closure.size() && closure[frontend.hoveredMeshNode_]) frontend.clearHoveredMeshNode();
    frontend.selectionNavigationAnchor_ = {};
    frontend.captureObjectBase_.reset(); frontend.captureObjectNode_ = SceneGraph::kInvalidNode;
    frontend.transformDragSession_.active = false;
#if CONTAINER_HAS_JOLT
    if (frontend.physicsScene_) frontend.physicsScene_->setPlaying(false);
    frontend.pendingPhysicsDeltaSeconds_ = 0;
#endif
    retired = deleting;
    frontend.sceneEditPublicationPending_ = true;
  }
};

bool RendererFrontend::retireSceneNode(uint32_t selected) {
  finishEditGestures();
  if (!subs_.sceneManager) retirementError("no primary scene is loaded");
  if (pendingSceneRetirement_) retirementError("the previous deletion refresh must complete first");
  const auto root = container::scene::ResolveSceneNodeAuthoringTarget(sceneGraph_, selected,
      container::scene::SceneNodeAuthoringPurpose::DeleteSubtree);
  requireEditableSceneNode(root);
  const auto& source = subs_.sceneManager->animationSource();
  const auto subtree = validateBindings(sceneGraph_, source, root
#if CONTAINER_HAS_JOLT
      , physicsScene_.get(), physicsElements_.get()
#endif
      );
  // A linked carrier can be editable while its descendants still belong to a source asset.
  for (const auto id : subtree.nodes) requireEditableSceneNode(id);
  SnapshotBudget measured;
  measured.charge(4096);
  measured.tables(source.nodes, source.materials, source.lights, source.cameras);
  measured.player(gltfAnimation_.get());
  measured.charge(sceneGraph_.nodeCount(), sizeof(SceneNodeLocalPose) + 2 * sizeof(uint32_t));
  measured.charge(source.nodes.size(), 2 * sizeof(size_t));
  measured.charge(source.materials.size(), sizeof(size_t));
  measured.charge(source.lights.size(), sizeof(size_t)); measured.charge(source.cameras.size(), sizeof(size_t));
  for (uint32_t id = 0; id < sceneGraph_.nodeCount(); ++id)
    if (!finite(sceneGraph_.getNode(id)->localTransform)) retirementError("nonfinite captured graph local pose");
  if (subs_.lightingManager) {
    // One captured edit table and its possible stable imported override copy.
    measured.charge(subs_.lightingManager->sceneLightEditsStorageBytes(), 2);
    measured.charge(subs_.sceneManager->authoredPointLights().size(), 512);
    measured.charge(subs_.sceneManager->authoredAreaLights().size(), 512);
    measured.charge(subs_.sceneManager->authoredDirectionalLights().size(), 512);
  }
  if (gltfImportedLightEdits_) measured.imported(*gltfImportedLightEdits_);
  for (const auto& [scene, edits] : gltfSceneLightEdits_) {
    (void)scene; measured.charge(128); if (edits) measured.lightEdits(*edits);
  }
  if (measured.bytes > (stagingLimit - 64ull * 1024 * 1024) / 3)
    retirementError("snapshot and detached replay staging exceed the peak bound");
  if (!sceneRetirementBudget_) sceneRetirementBudget_ = std::make_shared<SceneRetirementBudget>();
  if (measured.bytes > SceneRetirementBudget::limit - sceneRetirementBudget_->reserved)
    retirementError("retained deletion history exceeds the 512 MiB bound");
  auto state = std::make_shared<SceneRetirementHistory>();
  state->self = state;
  state->budget = sceneRetirementBudget_; state->reservation = measured.bytes;
  state->budget->reserved += state->reservation;
  state->identity = sceneGraph_.identity(); state->slotCount = sceneGraph_.nodeCount();
  state->selectedScene = subs_.sceneManager->selectedScene(); state->root = root;
  state->closure.assign(state->slotCount, 0); for (const auto id : subtree.nodes) state->closure[id] = 1;
  state->poses.reserve(state->slotCount);
  for (uint32_t id = 0; id < state->slotCount; ++id) state->poses.push_back({id, sceneGraph_.getNode(id)->localTransform});
  state->saved.nodes = source.nodes; state->saved.materials = source.materials;
  state->saved.lights = source.lights; state->saved.cameras = source.cameras;
  if (gltfAnimation_) { state->saved.player = std::make_unique<AnimationPlayer>(*gltfAnimation_); state->saved.player->pause(); }
  state->saved.imported = std::make_shared<ImportedSceneLightEdits>(
      gltfImportedLightEdits_ ? *gltfImportedLightEdits_ : ImportedSceneLightEdits{});
  if (subs_.lightingManager) {
    state->lightEdits = subs_.lightingManager->snapshotSceneLightEdits();
    const auto lights = subs_.sceneManager->publishedAuthoredSceneLights();
    state->saved.imported->capture(lights, state->lightEdits);
  }
  for (const auto& [scene, edits] : gltfSceneLightEdits_)
    state->saved.sceneEdits.emplace(scene, std::make_shared<SceneLightEdits>(edits ? *edits : SceneLightEdits{}));
  const auto all = [](auto& indices, size_t size) { indices.resize(size); std::iota(indices.begin(), indices.end(), size_t(0)); };
  all(state->dirty.nodes, source.nodes.size()); all(state->dirty.weights, source.nodes.size());
  all(state->dirty.materials, source.materials.size()); all(state->dirty.lights, source.lights.size());
  all(state->dirty.cameras, source.cameras.size());
  state->camera = gltfAnimationCameraNode_; state->before = sceneEditSelection(); state->after = state->before;
  if (state->after.mesh < state->closure.size() && state->closure[state->after.mesh]) state->after.mesh = SceneGraph::kInvalidNode;
  if (state->before.lightBinding)
    for (uint32_t id = 0; id < state->slotCount; ++id)
      if (state->closure[id] && sceneGraph_.getNode(id)->sourceNodeIndex == state->before.lightBinding->sourceNode)
        state->after.light = {};
  auto prepared = sceneGraph_.prepareRetireSubtree(root);
  auto next = state->stage();
  // Complete all captures/labels before the first stable graph publication.
  container::util::EditHistory::Command command{"Delete subtree",
      [this, state] { return replaySceneRetirement(*state, false); },
      [this, state] { return replaySceneRetirement(*state, true); }};
  ensureSceneEditHistory(); sceneEdits_->finishGesture();
  if (!sceneGraph_.publishRetirement(prepared)) return false;
  state->receipt = prepared.takeRetirement();
  state->publish(*this, next, true);
  pendingSceneRetirement_ = state;
  // Ordinary history invokes changed() after recording the cursor. GPU/cache
  // refresh is deliberately separate; this is not a full generation exchange.
  sceneEdits_->recordApplied(std::move(command));
  return true;
}

bool RendererFrontend::replaySceneRetirement(SceneRetirementHistory& state, bool retired) {
  if (!subs_.sceneManager || !state.receipt || pendingSceneRetirement_ || state.retired == retired ||
      sceneGraph_.identity() != state.identity || sceneGraph_.nodeCount() != state.slotCount ||
      subs_.sceneManager->selectedScene() != state.selectedScene) return false;
  auto pending = state.self.lock();
  if (!pending) return false;
  const auto& source = subs_.sceneManager->animationSource();
  if (source.nodes.size() != state.saved.nodes.size() || source.materials.size() != state.saved.materials.size() ||
      source.lights.size() != state.saved.lights.size() || source.cameras.size() != state.saved.cameras.size()) return false;
  (void)validateBindings(sceneGraph_, source, state.root
#if CONTAINER_HAS_JOLT
      , physicsScene_.get(), physicsElements_.get()
#endif
      );
#if CONTAINER_HAS_JOLT
  // Physics state has its own authored/reset poses and velocities. First-scope
  // deletion history cannot silently rewind an unrelated moved body with only
  // a visual matrix snapshot; refuse that replay before changing anything.
  if (physicsScene_ && physicsScene_->graphIdentity() == sceneGraph_.identity())
    for (uint32_t id = 0; id < state.slotCount; ++id)
      if (physicsScene_->hasBody(id) && sceneGraph_.getNode(id)->localTransform != state.poses[id].localTransform)
        retirementError("an unrelated physics body moved after deletion; restore its pose before history playback");
#endif
  auto prepared = sceneGraph_.prepareRetirementChange(*state.receipt, retired, state.poses);
  if (!prepared) return false;
  auto next = state.stage();
  if (!sceneGraph_.publishRetirement(prepared)) return false;
  state.publish(*this, next, retired);
  pendingSceneRetirement_ = std::move(pending);
  return true;
}

void RendererFrontend::publishSceneRetirementRefresh() {
  if (!pendingSceneRetirement_) return;
  const auto state = pendingSceneRetirement_;
  if (!subs_.sceneManager || sceneGraph_.identity() != state->identity) { pendingSceneRetirement_.reset(); return; }
  subs_.sceneManager->publishAnimationChanges(state->dirty, sceneGraph_);
  // Source vector swaps retain the permanent manager/model addresses, but any
  // node-derived authored-light cache must be rebuilt before ordinal adapters.
  subs_.sceneManager->refreshActiveScene(sceneGraph_);
  if (subs_.lightingManager) {
    subs_.lightingManager->restoreSceneLightEdits(gltfImportedLightEdits_->merge(
        subs_.sceneManager->publishedAuthoredSceneLights(), state->lightEdits));
    const auto& selection = state->retired ? state->after : state->before;
    subs_.lightingManager->selectEditableLight(remapImportedLightSelection(selection.light, selection.lightBinding.get(),
        authoredLightBindings(*subs_.sceneManager, selection.light.type)));
  }
  if (subs_.guiManager) {
    auto& controls = subs_.guiManager->gltfAnimationControls();
    controls.requestedClip.reset(); controls.requestedTime.reset(); controls.requestedPlaying.reset();
    controls.requestedSpeed.reset(); controls.requestedLoop.reset(); controls.requestedCameraNode.reset();
    controls.playing = false; controls.cameraNode = gltfAnimationCameraNode_ ? int(*gltfAnimationCameraNode_) : -1;
    if (gltfAnimation_) {
      controls.selectedClip = gltfAnimation_->selectedClip() ? int(*gltfAnimation_->selectedClip()) : -1;
      controls.time = gltfAnimation_->time(); controls.speed = gltfAnimation_->speed(); controls.loop = gltfAnimation_->loop();
    } else { controls.selectedClip = -1; controls.time = 0; }
  }
  refreshGltfCameraChoices(); updateGltfDeformation();
  if (subs_.temporalManager) subs_.temporalManager->reset("scene subtree deletion history at captured animation pose");
  pendingSceneRetirement_.reset();
}
} // namespace container::renderer
