#include "Container/physics/PhysicsScene.h"

#include "Container/utility/SceneGraph.h"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace container::physics {
namespace {

using scene::SceneGraph;
using WorldMatrices = std::unordered_map<uint32_t, glm::mat4>;

bool finite(glm::vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite(glm::dvec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite(const glm::mat4 &matrix) {
  for (int column = 0; column < 4; ++column)
    for (int row = 0; row < 4; ++row)
      if (!std::isfinite(matrix[column][row]))
        return false;
  return true;
}

struct TransformParts {
  glm::dvec3 position{0.0};
  glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
  glm::vec3 signedScale{1.0f};
};

TransformParts decompose(const glm::mat4 &matrix) {
  if (!finite(matrix) || std::abs(matrix[0][3]) > 1e-5f ||
      std::abs(matrix[1][3]) > 1e-5f || std::abs(matrix[2][3]) > 1e-5f ||
      std::abs(matrix[3][3] - 1.0f) > 1e-5f)
    throw std::invalid_argument("Physics bindings require finite affine transforms");
  TransformParts result;
  result.position = glm::dvec3{matrix[3]};
  glm::mat3 basis{matrix};
  for (int column = 0; column < 3; ++column) {
    result.signedScale[column] = glm::length(basis[column]);
    if (!std::isfinite(result.signedScale[column]) || result.signedScale[column] < 1e-6f)
      throw std::invalid_argument("Physics bindings do not support singular transforms");
    basis[column] /= result.signedScale[column];
  }
  if (std::abs(glm::dot(basis[0], basis[1])) > 1e-4f ||
      std::abs(glm::dot(basis[0], basis[2])) > 1e-4f ||
      std::abs(glm::dot(basis[1], basis[2])) > 1e-4f)
    throw std::invalid_argument("Physics box bindings do not support sheared transforms");
  // Reflections have no unique TRS factorization. A canonical signed X axis
  // reproduces the complete authored matrix and its off-center collider.
  if (glm::determinant(basis) < 0.0f) {
    basis[0] = -basis[0];
    result.signedScale.x = -result.signedScale.x;
  }
  result.rotation = glm::normalize(glm::quat_cast(basis));
  return result;
}

glm::mat4 compose(const TransformParts &parts) {
  if (!finite(parts.position))
    throw std::invalid_argument("Physics produced a non-finite scene position");
  const glm::mat4 result =
      glm::translate(glm::mat4{1.0f}, glm::vec3{parts.position}) *
      glm::mat4_cast(parts.rotation) * glm::scale(glm::mat4{1.0f}, parts.signedScale);
  if (!finite(result))
    throw std::invalid_argument("Physics pose exceeds the scene transform range");
  return result;
}

const glm::mat4 &worldMatrix(const SceneGraph &graph, uint32_t node,
                             WorldMatrices &cache,
                             const WorldMatrices *overrides = nullptr) {
  if (const auto found = cache.find(node); found != cache.end())
    return found->second;
  std::vector<uint32_t> path;
  uint32_t current = node;
  glm::mat4 parent{1.0f};
  while (current != SceneGraph::kInvalidNode) {
    if (const auto found = cache.find(current); found != cache.end()) {
      parent = found->second;
      break;
    }
    if (overrides != nullptr) {
      if (const auto found = overrides->find(current); found != overrides->end()) {
        parent = found->second;
        cache.emplace(current, parent);
        break;
      }
    }
    const auto *entry = graph.getNode(current);
    if (entry == nullptr || path.size() >= graph.nodeCount())
      throw std::invalid_argument("Physics binding has an invalid scene hierarchy");
    path.push_back(current);
    current = entry->parent;
  }
  for (auto entry = path.rbegin(); entry != path.rend(); ++entry) {
    parent *= graph.getNode(*entry)->localTransform;
    cache.emplace(*entry, parent);
  }
  return cache.at(node);
}

bool sameScale(glm::vec3 first, glm::vec3 second) {
  for (int axis = 0; axis < 3; ++axis)
    if (std::abs(first[axis] - second[axis]) >
        1e-5f * std::max(std::abs(first[axis]), std::abs(second[axis])))
      return false;
  return true;
}

bool samePose(const PhysicsBodyPose &first, const PhysicsBodyPose &second) {
  if (first.position != second.position)
    return false;
  // Match the world's component-distance policy. A float dot product near
  // one loses small authored angles; q and -q still describe the same pose.
  const glm::dquat a{first.rotation}, b{second.rotation};
  const auto difference = a - b;
  const auto opposite = a + b;
  return std::min(glm::dot(difference, difference), glm::dot(opposite, opposite)) <= 1e-12;
}

PhysicsBodyPose colliderPose(const TransformParts &parts, glm::dvec3 center) {
  return {.position = parts.position + glm::dquat{parts.rotation} *
                        (glm::dvec3{parts.signedScale} * center),
          .rotation = parts.rotation};
}

glm::vec3 colliderHalfExtents(glm::vec3 scale, glm::dvec3 localHalf) {
  const auto half = glm::abs(glm::dvec3{scale}) * localHalf;
  const glm::vec3 result{std::max(half.x, 0.005), std::max(half.y, 0.005),
                          std::max(half.z, 0.005)};
  if (!finite(result))
    throw std::invalid_argument("Physics box dimensions exceed the supported range");
  return result;
}

} // namespace

struct PhysicsScene::Impl {
  struct Binding {
    PhysicsBodyId body{kInvalidPhysicsBodyId};
    PhysicsBodySettings initialSettings{};
    glm::mat4 authoredLocal{1.0f};
    glm::dvec3 localCenter{0.0};
    glm::dvec3 localHalf{0.0};
    glm::vec3 signedScale{1.0f};
    glm::vec3 halfExtents{0.5f};
    PhysicsBodyPose lastAuthoredPose{};
    glm::mat4 lastVisualWorld{1.0f};
  };

  explicit Impl(const PhysicsWorldSettings &settings)
      : worldSettings(settings), world(std::make_unique<PhysicsWorld>(settings)) {}
  PhysicsWorldSettings worldSettings;
  std::unique_ptr<PhysicsWorld> world;
  std::unordered_map<uint32_t, Binding> bindings;
  struct Joint {
    std::optional<uint32_t> nodeA;
    uint32_t nodeB;
    PhysicsJointId bodyJoint;
    PhysicsJointSettings settings;
    PhysicsJointFrame frameA, frameB;
    glm::dvec3 anchorAInNode, anchorBInNode;
  };
  std::unordered_map<PhysicsJointId, Joint> joints;
  uint64_t graphIdentity{0};
  bool playing{false};
  bool pendingDynamicEdits{false};
  double accumulator{0.0};
  double dropped{0.0};
  uint32_t substeps{0};
  uint64_t totalSteps{0};
  uint64_t transformPublications{0};
  uint64_t authoredPoseUpdates{0};
  std::optional<uint64_t> pausedSynchronizedRevision{};
  std::optional<uint64_t> publishedRevision{};

  void discard() noexcept {
    world->clear();
    joints.clear();
    bindings.clear();
    graphIdentity = 0u;
    playing = false;
    pendingDynamicEdits = false;
    accumulator = 0.0;
    dropped = 0.0;
    substeps = 0u;
    totalSteps = 0u;
    transformPublications = 0u;
    authoredPoseUpdates = 0u;
    pausedSynchronizedRevision.reset();
    publishedRevision.reset();
  }

  void bindGraph(const SceneGraph &graph) {
    if (graphIdentity != 0u && graphIdentity != graph.identity())
      discard();
    graphIdentity = graph.identity();
  }

  PhysicsBodyPose rebuiltKinematicPose(const Binding &binding, glm::vec3 newScale,
                                       glm::dvec3 newCenter) const {
    const auto current = world->pose(binding.body);
    if (!current)
      throw std::runtime_error("Physics scene body was retired unexpectedly");
    // A body pose is its collider center. Changing shape/scale must retain the
    // current physical node origin and rotation, then let MoveKinematic sweep
    // toward the pending authored target on the next advancing tick.
    const glm::dquat rotation{current->rotation};
    const auto origin = current->position - rotation *
        (glm::dvec3{binding.signedScale} * binding.localCenter);
    return PhysicsWorld::validatePose({origin + rotation *
        (glm::dvec3{newScale} * newCenter), current->rotation});
  }

  void removeJointsForNode(uint32_t node) noexcept {
    for (auto it = joints.begin(); it != joints.end();) {
      if (it->second.nodeA == node || it->second.nodeB == node) {
        (void)world->destroyJoint(it->second.bodyJoint);
        it = joints.erase(it);
      } else ++it;
    }
  }

  // Build every candidate first so a rejected motion-type change leaves both
  // the original body and all its joints intact. The world also owns endpoint
  // cleanup, while this layer preserves stable scene IDs across rebuilds.
  PhysicsJointFrame jointFrame(const Joint &joint, bool first,
                               const Binding *replacement = nullptr) const {
    auto frame = first ? joint.frameA : joint.frameB;
    const auto endpoint = first ? joint.nodeA : std::optional<uint32_t>{joint.nodeB};
    if (!endpoint) return frame;
    const auto &binding = replacement ? *replacement : bindings.at(*endpoint);
    const auto anchor = first ? joint.anchorAInNode : joint.anchorBInNode;
    frame.anchor = (anchor - binding.localCenter) * glm::dvec3{binding.signedScale};
    return frame;
  }

  void rebindJoints(uint32_t node, PhysicsBodyId replacement, const Binding &newBinding) {
    struct Pending {
      PhysicsJointId id, bodyJoint;
      PhysicsJointFrame frameA, frameB;
    };
    std::vector<Pending> pending;
    pending.reserve(joints.size());
    try {
      for (const auto &[id, joint] : joints) {
        if (joint.nodeA != node && joint.nodeB != node) continue;
        const auto first = !joint.nodeA ? kInvalidPhysicsBodyId
            : *joint.nodeA == node ? replacement : bindings.at(*joint.nodeA).body;
        const auto second = joint.nodeB == node ? replacement : bindings.at(joint.nodeB).body;
        const auto frameA = jointFrame(joint, true, joint.nodeA == node ? &newBinding : nullptr);
        const auto frameB = jointFrame(joint, false, joint.nodeB == node ? &newBinding : nullptr);
        const auto rebuilt = world->createJointLocal(first, second, joint.settings, frameA, frameB);
        pending.push_back({id, rebuilt, frameA, frameB});
      }
    } catch (...) {
      for (const auto &entry : pending) {
        (void)world->destroyJoint(entry.bodyJoint);
      }
      throw;
    }
    for (const auto &entry : pending) {
      auto &joint = joints.at(entry.id);
      (void)world->destroyJoint(joint.bodyJoint);
      joint.bodyJoint = entry.bodyJoint;
      joint.frameA = entry.frameA;
      joint.frameB = entry.frameB;
    }
  }

  void publishWorldTransforms(SceneGraph &graph) {
    if (publishedRevision && *publishedRevision == graph.revision())
      return;
    graph.updateWorldTransforms();
    publishedRevision = graph.revision();
    ++transformPublications;
  }

  glm::mat4 authoredWorld(const SceneGraph &graph, uint32_t node,
                           WorldMatrices *cache = nullptr) const {
    std::vector<uint32_t> path;
    glm::mat4 world{1.0f};
    for (uint32_t current = node; current != SceneGraph::kInvalidNode;) {
      if (cache != nullptr) {
        if (const auto found = cache->find(current); found != cache->end()) {
          world = found->second;
          break;
        }
      }
      const auto *entry = graph.getNode(current);
      if (entry == nullptr || path.size() >= graph.nodeCount())
        throw std::invalid_argument("Physics binding has an invalid scene hierarchy");
      path.push_back(current);
      current = entry->parent;
    }
    for (auto entry = path.rbegin(); entry != path.rend(); ++entry) {
      const auto bound = bindings.find(*entry);
      world *= bound == bindings.end() ? graph.getNode(*entry)->localTransform
                                      : bound->second.authoredLocal;
      if (cache != nullptr)
        cache->emplace(*entry, world);
    }
    return world;
  }

  std::vector<uint32_t> bindingsInSubtree(const SceneGraph &graph, uint32_t node) const {
    const auto *root = graph.getNode(node);
    if (root == nullptr)
      throw std::invalid_argument("Physics binding has an invalid scene hierarchy");
    if (root->children.empty())
      return bindings.contains(node) ? std::vector<uint32_t>{node} : std::vector<uint32_t>{};
    // Sparse physics scenes may contain millions of visual descendants. Only
    // walk attached nodes' ancestry, sharing each resolved path across bodies.
    std::unordered_map<uint32_t, bool> inside;
    inside.emplace(node, true);
    std::vector<uint32_t> affected, path;
    affected.reserve(bindings.size());
    for (const auto &[boundNode, binding] : bindings) {
      (void)binding;
      path.clear();
      auto current = boundNode;
      bool descendant = false;
      while (current != SceneGraph::kInvalidNode) {
        if (const auto found = inside.find(current); found != inside.end()) {
          descendant = found->second;
          break;
        }
        const auto *entry = graph.getNode(current);
        if (!entry || path.size() >= graph.nodeCount())
          throw std::invalid_argument("Physics binding has an invalid scene hierarchy");
        path.push_back(current);
        current = entry->parent;
      }
      for (const auto ancestor : path)
        inside.emplace(ancestor, descendant);
      if (descendant)
        affected.push_back(boundNode);
    }
    return affected;
  }

  void validateHierarchy(const SceneGraph &graph,
      uint32_t overrideNode = SceneGraph::kInvalidNode,
      uint32_t overrideParent = SceneGraph::kInvalidNode) const {
    // Cache ancestor motion masks so even deep nested dynamic bindings visit
    // each ancestor once, instead of traversing the full chain per body.
    std::unordered_map<uint32_t, uint8_t> masks;
    for (const auto &[node, binding] : bindings) {
      (void)binding;
      std::vector<uint32_t> path;
      uint32_t current = node;
      uint8_t inherited = 0u;
      while (current != SceneGraph::kInvalidNode) {
        if (const auto found = masks.find(current); found != masks.end()) {
          inherited = found->second;
          break;
        }
        const auto *entry = graph.getNode(current);
        if (entry == nullptr || path.size() >= graph.nodeCount())
          throw std::invalid_argument("Physics binding has an invalid scene hierarchy");
        path.push_back(current);
        current = current == overrideNode ? overrideParent : entry->parent;
      }
      for (auto entry = path.rbegin(); entry != path.rend(); ++entry) {
        if (const auto bound = bindings.find(*entry); bound != bindings.end()) {
          const uint8_t own = bound->second.initialSettings.motionType ==
                                      PhysicsMotionType::Dynamic ? 1u : 2u;
          if ((inherited & (own == 1u ? 2u : 1u)) != 0u)
            throw std::invalid_argument(
                "Dynamic and static/kinematic physics bindings cannot be ancestors of each other");
          inherited |= own;
        }
        masks.emplace(*entry, inherited);
      }
    }
  }

  void synchronizeAuthored(const SceneGraph &graph, bool advancing,
      float motionDuration = static_cast<float>(PhysicsScene::kFixedStepSeconds),
      bool acceptDynamicEdits = false) {
    validateHierarchy(graph);
    struct AuthoredUpdate {
      uint32_t node;
      glm::mat4 visualWorld;
      TransformParts parts;
      PhysicsBodyPose target;
      glm::vec3 halfExtents;
      PhysicsBodyPose rebuildPose;
    };
    std::vector<AuthoredUpdate> updates;
    updates.reserve(bindings.size());
    WorldMatrices cache;
    // Validate the complete authored batch before moving or rebuilding any
    // body. Backend allocation/shape creation can still fail independently.
    for (const auto &[node, binding] : bindings) {
      const auto visualWorld = worldMatrix(graph, node, cache);
      const auto parts = decompose(visualWorld);
      const auto *entry = graph.getNode(node);
      if (entry->parent != SceneGraph::kInvalidNode)
        (void)decompose(worldMatrix(graph, entry->parent, cache));
      const auto target = PhysicsWorld::validatePose(colliderPose(parts, binding.localCenter));
      const auto half = colliderHalfExtents(parts.signedScale, binding.localHalf);
      const auto rebuildPose = advancing &&
          binding.initialSettings.motionType == PhysicsMotionType::Kinematic &&
          !sameScale(parts.signedScale, binding.signedScale)
          ? rebuiltKinematicPose(binding, parts.signedScale, binding.localCenter) : target;
      updates.push_back({node, visualWorld, parts, target, half, rebuildPose});
    }
    for (const auto &[node, visualWorld, parts, target, half, rebuildPose] : updates) {
      auto &binding = bindings.at(node);
      if (!sameScale(parts.signedScale, binding.signedScale)) {
        PhysicsBodySettings replacement = binding.initialSettings;
        replacement.position = rebuildPose.position;
        replacement.rotation = rebuildPose.rotation;
        if (const auto velocity = world->linearVelocity(binding.body))
          replacement.linearVelocity = *velocity;
        if (const auto velocity = world->angularVelocity(binding.body))
          replacement.angularVelocity = *velocity;
        const auto newBody = world->createBox(half, replacement);
        try {
          auto resized = binding;
          resized.signedScale = parts.signedScale;
          rebindJoints(node, newBody, resized);
        } catch (...) {
          (void)world->destroyBody(newBody);
          throw;
        }
        (void)world->destroyBody(binding.body);
        binding.body = newBody;
        binding.signedScale = parts.signedScale;
        binding.halfExtents = half;
        binding.lastAuthoredPose = target;
      }
      if (binding.initialSettings.motionType == PhysicsMotionType::Static) {
        if (!samePose(target, binding.lastAuthoredPose)) {
          (void)world->setPose(binding.body, target, false);
          binding.lastAuthoredPose = target;
          ++authoredPoseUpdates;
        }
      } else if (binding.initialSettings.motionType == PhysicsMotionType::Kinematic) {
        if (advancing)
          (void)world->moveKinematic(binding.body, target, motionDuration);
        else
          (void)world->setPose(binding.body, target, false);
        ++authoredPoseUpdates;
      } else if ((!advancing || acceptDynamicEdits) && visualWorld != binding.lastVisualWorld) {
        // Name/visibility changes also advance the graph revision. Teleport
        // only when the actual authored world matrix differs from the last
        // published simulation matrix, retaining double-precision body poses
        // and velocities through unrelated UI edits.
        const auto previous = world->pose(binding.body);
        if (!previous || !samePose(*previous, target)) {
          // A previously resting body may have moved away from its support.
          // Retain its velocities, but let gravity resume when playback does.
          (void)world->setPose(binding.body, target, true);
          ++authoredPoseUpdates;
        }
        binding.lastVisualWorld = visualWorld;
      }
    }
  }

  struct DynamicPublication {
    struct Local {
      uint32_t node;
      glm::mat4 transform;
      bool authoredOverride;
    };
    std::vector<Local> locals;
  };

  DynamicPublication prepareDynamicPublication(const SceneGraph &graph,
      const WorldMatrices *pendingEdits = nullptr,
      uint32_t excludedNode = SceneGraph::kInvalidNode,
      const glm::mat4 *restoredLocal = nullptr) const {
    WorldMatrices desired;
    for (const auto &[node, binding] : bindings) {
      if (node == excludedNode ||
          binding.initialSettings.motionType != PhysicsMotionType::Dynamic)
        continue;
      if (pendingEdits) {
        if (const auto authored = pendingEdits->find(node); authored != pendingEdits->end()) {
          desired.emplace(node, authored->second);
          continue;
        }
      }
      const auto pose = world->pose(binding.body);
      if (!pose)
        throw std::runtime_error("Physics scene body was retired unexpectedly");
      TransformParts parts;
      parts.rotation = pose->rotation;
      parts.signedScale = binding.signedScale;
      parts.position = pose->position - glm::dquat{pose->rotation} *
          (glm::dvec3{binding.signedScale} * binding.localCenter);
      desired.emplace(node, compose(parts));
    }
    WorldMatrices parents;
    if (restoredLocal != nullptr) {
      // A detached ancestor's authored local changes the prospective parent
      // frame of remaining dynamics. Resolve it through their desired worlds
      // without publishing the removed body as another dynamic pose.
      const uint32_t parent = graph.getNode(excludedNode)->parent;
      glm::mat4 restoredWorld = *restoredLocal;
      if (parent != SceneGraph::kInvalidNode)
        restoredWorld = worldMatrix(graph, parent, parents, &desired) * restoredWorld;
      if (!finite(restoredWorld))
        throw std::invalid_argument("Restored scene transform exceeds the supported range");
      desired.emplace(excludedNode, restoredWorld);
    }
    DynamicPublication publication;
    publication.locals.reserve(desired.size());
    for (const auto &[node, desiredWorld] : desired) {
      if (node == excludedNode)
        continue;
      const uint32_t parent = graph.getNode(node)->parent;
      glm::mat4 local = desiredWorld;
      if (parent != SceneGraph::kInvalidNode) {
        const auto &parentWorld = worldMatrix(graph, parent, parents, &desired);
        (void)decompose(parentWorld);
        local = glm::inverse(parentWorld) * desiredWorld;
      }
      if (!finite(local))
        throw std::invalid_argument("Physics scene transform cannot be published safely");
      publication.locals.push_back({node, local, pendingEdits && pendingEdits->contains(node)});
    }
    return publication;
  }

  void commitDynamicPublication(SceneGraph &graph, const DynamicPublication &publication) {
    // Compute every ancestor's desired world transform before modifying any
    // local transform. Parent/child dynamics remain independent of map order.
    for (const auto &local : publication.locals)
      graph.setLocalTransform(local.node, local.transform);
    publishWorldTransforms(graph);
    for (const auto &local : publication.locals)
      if (!local.authoredOverride)
        bindings.at(local.node).lastVisualWorld = graph.getNode(local.node)->worldTransform;
  }

  void applyDynamicPoses(SceneGraph &graph,
                         const WorldMatrices *pendingEdits = nullptr) {
    const auto publication = prepareDynamicPublication(graph, pendingEdits);
    commitDynamicPublication(graph, publication);
  }

  void step(SceneGraph &graph, bool publish = true,
      float motionDuration = static_cast<float>(PhysicsScene::kFixedStepSeconds),
      bool acceptDynamicEdits = false) {
    synchronizeAuthored(graph, true, motionDuration, acceptDynamicEdits);
    world->step(static_cast<float>(PhysicsScene::kFixedStepSeconds));
    if (publish)
      applyDynamicPoses(graph);
    ++substeps;
    ++totalSteps;
  }
};

PhysicsScene::PhysicsScene(const PhysicsWorldSettings &settings)
    : impl_(std::make_unique<Impl>(settings)) {}
PhysicsScene::~PhysicsScene() = default;
PhysicsScene::PhysicsScene(PhysicsScene &&) noexcept = default;
PhysicsScene &PhysicsScene::operator=(PhysicsScene &&) noexcept = default;

PhysicsBodyId PhysicsScene::attachBox(SceneGraph &graph, uint32_t node,
                                      glm::vec3 localMin, glm::vec3 localMax,
                                      PhysicsBodySettings settings) {
  impl_->bindGraph(graph);
  const auto *entry = graph.getNode(node);
  if (entry && entry->retired)
    throw std::invalid_argument("Cannot attach physics to a retired scene node");
  if (entry == nullptr)
    throw std::invalid_argument("Cannot attach physics to a missing scene node");
  if (impl_->bindings.contains(node))
    throw std::invalid_argument("The scene node already has a physics binding");
  if (!finite(localMin) || !finite(localMax) || localMin.x > localMax.x ||
      localMin.y > localMax.y || localMin.z > localMax.z)
    throw std::invalid_argument("Physics box bounds must be finite and ordered");
  WorldMatrices cache;
  const auto parts = decompose(worldMatrix(graph, node, cache));
  if (entry->parent != SceneGraph::kInvalidNode)
    (void)decompose(worldMatrix(graph, entry->parent, cache));
  Impl::Binding binding;
  binding.authoredLocal = entry->localTransform;
  binding.localCenter = (glm::dvec3{localMin} + glm::dvec3{localMax}) * 0.5;
  binding.localHalf = (glm::dvec3{localMax} - glm::dvec3{localMin}) * 0.5;
  binding.signedScale = parts.signedScale;
  binding.halfExtents = colliderHalfExtents(parts.signedScale, binding.localHalf);
  const auto pose = colliderPose(parts, binding.localCenter);
  settings.position = pose.position;
  settings.rotation = pose.rotation;
  binding.initialSettings = settings;
  binding.lastAuthoredPose = pose;
  binding.lastVisualWorld = worldMatrix(graph, node, cache);
  auto [inserted, added] = impl_->bindings.emplace(node, std::move(binding));
  (void)added;
  try {
    impl_->validateHierarchy(graph);
    inserted->second.body = impl_->world->createBox(inserted->second.halfExtents, settings);
  } catch (...) {
    impl_->bindings.erase(inserted);
    throw;
  }
  impl_->pausedSynchronizedRevision.reset();
  return inserted->second.body;
}

PhysicsBodyId PhysicsScene::replaceBox(SceneGraph &graph, uint32_t node,
                                       glm::vec3 localMin, glm::vec3 localMax,
                                       PhysicsBodySettings settings) {
  impl_->bindGraph(graph);
  const auto found = impl_->bindings.find(node);
  if (found == impl_->bindings.end())
    throw std::invalid_argument("Cannot replace a missing physics binding");
  if (!finite(localMin) || !finite(localMax) || localMin.x > localMax.x ||
      localMin.y > localMax.y || localMin.z > localMax.z)
    throw std::invalid_argument("Physics box bounds must be finite and ordered");
  WorldMatrices cache;
  const auto parts = decompose(worldMatrix(graph, node, cache));
  const auto *entry = graph.getNode(node);
  if (entry->parent != SceneGraph::kInvalidNode)
    (void)decompose(worldMatrix(graph, entry->parent, cache));
  const auto previous = found->second;
  auto replacement = previous;
  replacement.localCenter = (glm::dvec3{localMin} + glm::dvec3{localMax}) * 0.5;
  replacement.localHalf = (glm::dvec3{localMax} - glm::dvec3{localMin}) * 0.5;
  replacement.signedScale = parts.signedScale;
  replacement.halfExtents = colliderHalfExtents(parts.signedScale, replacement.localHalf);
  const auto livePose = colliderPose(parts, replacement.localCenter);
  const auto creationPose = impl_->playing &&
      previous.initialSettings.motionType == PhysicsMotionType::Kinematic &&
      settings.motionType == PhysicsMotionType::Kinematic
      ? impl_->rebuiltKinematicPose(previous, parts.signedScale, replacement.localCenter) : livePose;
  settings.position = creationPose.position;
  settings.rotation = creationPose.rotation;
  replacement.initialSettings = settings;
  const auto resetParts = decompose(impl_->authoredWorld(graph, node));
  const auto resetPose = colliderPose(resetParts, replacement.localCenter);
  replacement.initialSettings.position = resetPose.position;
  replacement.initialSettings.rotation = resetPose.rotation;
  (void)colliderHalfExtents(resetParts.signedScale, replacement.localHalf);
  replacement.lastAuthoredPose = livePose;
  replacement.lastVisualWorld = worldMatrix(graph, node, cache);
  found->second = replacement;
  PhysicsBodyId newBody;
  try {
    // Evaluate the candidate's motion relationship before touching the world.
    impl_->validateHierarchy(graph);
    newBody = impl_->world->createBox(replacement.halfExtents, settings);
    try {
      impl_->rebindJoints(node, newBody, replacement);
    } catch (...) {
      (void)impl_->world->destroyBody(newBody);
      throw;
    }
  } catch (...) {
    found->second = previous;
    throw;
  }
  (void)impl_->world->destroyBody(previous.body);
  found->second.body = newBody;
  impl_->pausedSynchronizedRevision.reset();
  return newBody;
}

bool PhysicsScene::detach(SceneGraph &graph, uint32_t node, bool restore) {
  impl_->bindGraph(graph);
  const auto found = impl_->bindings.find(node);
  if (found == impl_->bindings.end())
    return false;
  WorldMatrices pendingEdits;
  if (!impl_->playing || impl_->pendingDynamicEdits) {
    // Retiring a different body must not publish simulated poses over pending
    // paused edits. Snapshot before restoring a detached ancestor, then leave
    // these matrices authored until the next paused update or Play tick.
    WorldMatrices cache;
    for (const auto &[otherNode, binding] : impl_->bindings) {
      if (otherNode == node || binding.initialSettings.motionType != PhysicsMotionType::Dynamic)
        continue;
      const auto visualWorld = worldMatrix(graph, otherNode, cache);
      if (visualWorld != binding.lastVisualWorld)
        pendingEdits.emplace(otherNode, visualWorld);
    }
  }
  const auto restoredLocal = restore ? std::optional{found->second.authoredLocal} : std::nullopt;
  // Resolve every remaining body's publication, including a restored ancestor,
  // before changing graph locals or retiring the endpoint and its joints.
  const auto publication = impl_->prepareDynamicPublication(
      graph, &pendingEdits, node, restoredLocal ? &*restoredLocal : nullptr);
  if (restoredLocal)
    graph.setLocalTransform(node, *restoredLocal);
  impl_->removeJointsForNode(node);
  (void)impl_->world->destroyBody(found->second.body);
  impl_->bindings.erase(found);
  impl_->pausedSynchronizedRevision.reset();
  impl_->commitDynamicPublication(graph, publication);
  return true;
}

void PhysicsScene::clear(SceneGraph *graph) {
  if (graph != nullptr && graph->identity() == impl_->graphIdentity) {
    for (const auto &[node, binding] : impl_->bindings)
      graph->setLocalTransform(node, binding.authoredLocal);
    impl_->publishWorldTransforms(*graph);
  }
  impl_->discard();
}

void PhysicsScene::update(SceneGraph &graph, double frameDeltaSeconds) {
  impl_->bindGraph(graph);
  impl_->substeps = 0u;
  if (impl_->bindings.empty()) {
    impl_->accumulator = 0.0;
    return;
  }
  if (!impl_->playing) {
    if (impl_->pausedSynchronizedRevision &&
        *impl_->pausedSynchronizedRevision == graph.revision())
      return;
    impl_->synchronizeAuthored(graph, false);
    impl_->publishWorldTransforms(graph);
    impl_->pausedSynchronizedRevision = graph.revision();
    return;
  }
  if (!std::isfinite(frameDeltaSeconds) || frameDeltaSeconds <= 0.0)
    return;
  const double accepted = std::min(frameDeltaSeconds, kMaxFrameSeconds);
  impl_->dropped += frameDeltaSeconds - accepted;
  impl_->accumulator += accepted;
  const auto plannedSubsteps = static_cast<uint32_t>(std::min(
      double(kMaxSubsteps), std::floor((impl_->accumulator + 1e-10) / kFixedStepSeconds)));
  while (impl_->substeps < plannedSubsteps &&
         impl_->accumulator + 1e-10 >= kFixedStepSeconds) {
    // An authored target describes this render frame's end pose. Traverse it
    // over the remaining simulated ticks so contacts do not receive the full
    // displacement as a one-tick impulse when rendering runs below 60 Hz.
    const float remainingDuration =
        static_cast<float>((plannedSubsteps - impl_->substeps) * kFixedStepSeconds);
    impl_->step(graph, false, remainingDuration, impl_->pendingDynamicEdits);
    // Retain the transition through no-step updates and failed ticks. Later
    // ticks in this batch must integrate rather than reauthor the same pose.
    impl_->pendingDynamicEdits = false;
    impl_->accumulator = std::max(0.0, impl_->accumulator - kFixedStepSeconds);
  }
  if (impl_->substeps != 0u)
    impl_->applyDynamicPoses(graph);
  if (impl_->accumulator + 1e-10 >= kFixedStepSeconds) {
    const double ticks = std::floor((impl_->accumulator + 1e-10) / kFixedStepSeconds);
    const double dropped = ticks * kFixedStepSeconds;
    impl_->dropped += dropped;
    impl_->accumulator = std::max(0.0, impl_->accumulator - dropped);
  }
}

void PhysicsScene::singleStep(SceneGraph &graph) {
  impl_->bindGraph(graph);
  impl_->playing = false;
  impl_->accumulator = 0.0;
  impl_->substeps = 0u;
  if (!impl_->bindings.empty())
    // Step can be requested immediately after a paused transform edit. Read
    // that edit before advancing, while preserving contact-driven kinematics.
    impl_->step(graph, true, static_cast<float>(kFixedStepSeconds), true);
  impl_->pendingDynamicEdits = false;
}

void PhysicsScene::validateTransform(const SceneGraph &graph, uint32_t node,
                                    const glm::mat4 &local,
                                    std::optional<uint32_t> parentOverride) const {
  if (graph.getNode(node) == nullptr || graph.isNodeRetired(node))
    throw std::invalid_argument("Cannot edit a missing scene node");
  const auto parent = parentOverride.value_or(graph.getNode(node)->parent);
  if (parentOverride && !graph.canSetParent(node,
          parent == SceneGraph::kInvalidNode ? std::nullopt : std::optional{parent}))
    throw std::invalid_argument("Invalid scene parent or hierarchy cycle");
  if (impl_->graphIdentity != graph.identity() || impl_->bindings.empty())
    return;
  if (parentOverride)
    impl_->validateHierarchy(graph, node, parent);
  const auto affected = impl_->bindingsInSubtree(graph, node);
  if (affected.empty())
    return;

  WorldMatrices liveCache, resetCache;
  const auto parentWorld = parent == SceneGraph::kInvalidNode ? glm::mat4{1}
      : worldMatrix(graph, parent, liveCache);
  const WorldMatrices liveOverride{{node, parentWorld * local}};
  // Bound locals return to their attached matrices on Reset; an edited
  // unbound ancestor retains the proposed local in that prospective world.
  const auto bound = impl_->bindings.find(node);
  const auto resetLocal = bound == impl_->bindings.end() ? local : bound->second.authoredLocal;
  const auto resetParent = parent == SceneGraph::kInvalidNode ? glm::mat4{1}
      : impl_->authoredWorld(graph, parent, &resetCache);
  resetCache.emplace(node, resetParent * resetLocal);
  struct Candidate {
    glm::vec3 scale;
    PhysicsBodyPose pose;
  };
  std::unordered_map<uint32_t, Candidate> live, resets;
  for (const auto boundNode : affected) {
    const auto &binding = impl_->bindings.at(boundNode);
    const auto parts = decompose(worldMatrix(graph, boundNode, liveCache, &liveOverride));
    const auto boundParent = boundNode == node ? parent : graph.getNode(boundNode)->parent;
    if (boundParent != SceneGraph::kInvalidNode)
      (void)decompose(worldMatrix(graph, boundParent, liveCache, &liveOverride));
    auto pose = PhysicsWorld::validatePose(colliderPose(parts, binding.localCenter));
    (void)colliderHalfExtents(parts.signedScale, binding.localHalf);
    if (impl_->playing && binding.initialSettings.motionType == PhysicsMotionType::Kinematic &&
        !sameScale(parts.signedScale, binding.signedScale))
      pose = impl_->rebuiltKinematicPose(binding, parts.signedScale, binding.localCenter);
    live.emplace(boundNode, Candidate{parts.signedScale, pose});

    const auto resetParts = decompose(impl_->authoredWorld(graph, boundNode, &resetCache));
    if (boundParent != SceneGraph::kInvalidNode)
      (void)decompose(impl_->authoredWorld(graph, boundParent, &resetCache));
    const auto resetPose = PhysicsWorld::validatePose(colliderPose(resetParts, binding.localCenter));
    (void)colliderHalfExtents(resetParts.signedScale, binding.localHalf);
    resets.emplace(boundNode, Candidate{resetParts.signedScale, resetPose});
  }
  const auto validateFrame = [&](const Impl::Joint &joint, bool first,
                                  const auto &candidates, bool resetting) {
    const auto endpoint = first ? joint.nodeA : std::optional<uint32_t>{joint.nodeB};
    if (!endpoint)
      return;
    const auto found = candidates.find(*endpoint);
    if (found == candidates.end())
      return;
    auto replacement = impl_->bindings.at(*endpoint);
    if (!resetting && sameScale(replacement.signedScale, found->second.scale))
      return; // A rigid edit retains the existing canonical frame.
    replacement.signedScale = found->second.scale;
    const auto frame = impl_->jointFrame(joint, first, &replacement);
    auto settings = joint.settings;
    settings.anchor = frame.anchor;
    settings.axis = frame.axis;
    settings.normal = frame.normal;
    (void)PhysicsWorld::validateJointSettings(settings);
    if (first) {
      // Rebuilding also derives the first endpoint's world frame. Check it
      // with the same pure validator used by the backend's public joint API.
      const glm::dquat rotation{found->second.pose.rotation};
      settings.anchor = found->second.pose.position + rotation * frame.anchor;
      settings.axis = glm::vec3{rotation * glm::dvec3{frame.axis}};
      settings.normal = glm::vec3{rotation * glm::dvec3{frame.normal}};
      (void)PhysicsWorld::validateJointSettings(settings);
    }
  };
  for (const auto &[id, joint] : impl_->joints) {
    (void)id;
    for (const bool first : {false, true}) {
      validateFrame(joint, first, live, false);
      validateFrame(joint, first, resets, true);
    }
  }
}

void PhysicsScene::validateReparent(const SceneGraph &graph, uint32_t node,
                                   std::optional<uint32_t> parent) const {
  if (!graph.canSetParent(node, parent))
    throw std::invalid_argument("Invalid scene parent or hierarchy cycle");
  if (impl_->graphIdentity != graph.identity() || impl_->bindings.empty())
    return;
  impl_->validateHierarchy(graph, node, parent.value_or(SceneGraph::kInvalidNode));
  // Only a subtree containing physics bindings needs a supported rigid parent.
  // Ordinary visual-only branches may still use general affine transforms.
  const auto affected = impl_->bindingsInSubtree(graph, node);
  if (affected.empty())
    return;
  WorldMatrices liveCache, authoredCache;
  const auto parentWorld = parent ? worldMatrix(graph, *parent, liveCache) : glm::mat4{1};
  (void)decompose(parentWorld);
  const auto preservedLocal = glm::inverse(parentWorld) * worldMatrix(graph, node, liveCache);
  const auto rootBinding = impl_->bindings.find(node);
  const auto resetLocal = rootBinding == impl_->bindings.end()
      ? preservedLocal : rootBinding->second.authoredLocal;
  const auto resetParent = parent ? impl_->authoredWorld(graph, *parent, &authoredCache) : glm::mat4{1};
  // Seed the changed branch in Reset's cache: unbound nodes retain the new
  // world-preserving local, while bound nodes restore their attached local.
  authoredCache.emplace(node, resetParent * resetLocal);
  for (const auto boundNode : affected) {
    const auto &binding = impl_->bindings.at(boundNode);
    const auto parts = decompose(impl_->authoredWorld(graph, boundNode, &authoredCache));
    const auto resetParentNode = boundNode == node
        ? parent.value_or(SceneGraph::kInvalidNode) : graph.getNode(boundNode)->parent;
    if (resetParentNode != SceneGraph::kInvalidNode)
      (void)decompose(impl_->authoredWorld(graph, resetParentNode, &authoredCache));
    (void)colliderPose(parts, binding.localCenter);
    (void)colliderHalfExtents(parts.signedScale, binding.localHalf);
  }
}

void PhysicsScene::reset(SceneGraph &graph) {
  impl_->bindGraph(graph);
  impl_->validateHierarchy(graph);
  struct ResetBody {
    PhysicsBodySettings settings;
    glm::vec3 scale;
    glm::vec3 halfExtents;
    PhysicsBodyId body{kInvalidPhysicsBodyId};
  };
  std::unordered_map<uint32_t, ResetBody> resets;
  resets.reserve(impl_->bindings.size());
  WorldMatrices authoredCache;
  for (const auto &[node, binding] : impl_->bindings) {
    const auto parts = decompose(impl_->authoredWorld(graph, node, &authoredCache));
    const auto parent = graph.getNode(node)->parent;
    if (parent != SceneGraph::kInvalidNode)
      (void)decompose(impl_->authoredWorld(graph, parent, &authoredCache));
    const auto pose = colliderPose(parts, binding.localCenter);
    auto settings = binding.initialSettings;
    settings.position = pose.position;
    settings.rotation = pose.rotation;
    resets.emplace(node, ResetBody{settings, parts.signedScale,
                      colliderHalfExtents(parts.signedScale, binding.localHalf)});
  }
  // Shape inertia and canonical joint frames can fail backend validation even
  // when every authored matrix is finite. Build the complete candidate before
  // touching graph locals or live bodies. A separate world also permits reset
  // when the configured body capacity is completely occupied.
  auto candidate = std::make_unique<PhysicsWorld>(impl_->worldSettings);
  for (auto &[node, reset] : resets) {
    (void)node;
    reset.body = candidate->createBox(reset.halfExtents, reset.settings);
  }
  struct ResetJoint {
    PhysicsJointId id, bodyJoint;
    PhysicsJointFrame frameA, frameB;
  };
  std::vector<ResetJoint> resetJoints;
  resetJoints.reserve(impl_->joints.size());
  const auto resetFrame = [&](const Impl::Joint &joint, bool first) {
    const auto endpoint = first ? joint.nodeA : std::optional<uint32_t>{joint.nodeB};
    if (!endpoint) return joint.frameA;
    auto replacement = impl_->bindings.at(*endpoint);
    replacement.signedScale = resets.at(*endpoint).scale;
    return impl_->jointFrame(joint, first, &replacement);
  };
  for (const auto &[id, joint] : impl_->joints) {
    const auto frameA = resetFrame(joint, true);
    const auto frameB = resetFrame(joint, false);
    const auto rebuilt = candidate->createJointLocal(
        joint.nodeA ? resets.at(*joint.nodeA).body : kInvalidPhysicsBodyId,
        resets.at(joint.nodeB).body, joint.settings, frameA, frameB);
    resetJoints.push_back({id, rebuilt, frameA, frameB});
  }
  for (const auto &[node, binding] : impl_->bindings)
    graph.setLocalTransform(node, binding.authoredLocal);
  impl_->world.swap(candidate);
  for (const auto &[node, reset] : resets) {
    auto &binding = impl_->bindings.at(node);
    binding.body = reset.body;
    binding.signedScale = reset.scale;
    binding.halfExtents = reset.halfExtents;
    binding.lastAuthoredPose = {reset.settings.position, reset.settings.rotation};
  }
  for (const auto &reset : resetJoints) {
    auto &joint = impl_->joints.at(reset.id);
    joint.bodyJoint = reset.bodyJoint;
    joint.frameA = reset.frameA;
    joint.frameB = reset.frameB;
  }
  impl_->publishWorldTransforms(graph);
  for (auto &[node, binding] : impl_->bindings)
    if (binding.initialSettings.motionType == PhysicsMotionType::Dynamic)
      binding.lastVisualWorld = graph.getNode(node)->worldTransform;
  impl_->pausedSynchronizedRevision.reset();
  impl_->playing = false;
  impl_->pendingDynamicEdits = false;
  impl_->accumulator = 0.0;
  impl_->dropped = 0.0;
  impl_->substeps = 0u;
  impl_->totalSteps = 0u;
}

void PhysicsScene::setPlaying(bool playing) noexcept {
  if (!playing)
    impl_->pendingDynamicEdits = false;
  else if (!impl_->playing)
    impl_->pendingDynamicEdits = true;
  if (impl_->playing != playing)
    impl_->accumulator = 0.0;
  if (impl_->playing != playing)
    impl_->pausedSynchronizedRevision.reset();
  impl_->playing = playing;
}

bool PhysicsScene::playing() const noexcept { return impl_->playing; }
size_t PhysicsScene::bodyCount() const noexcept { return impl_->bindings.size(); }
uint64_t PhysicsScene::graphIdentity() const noexcept { return impl_->graphIdentity; }
bool PhysicsScene::hasBody(uint32_t node) const noexcept { return impl_->bindings.contains(node); }

bool PhysicsScene::hasDynamicBodyInSubtree(const SceneGraph &graph, uint32_t node) const {
  if (impl_->graphIdentity != graph.identity() || impl_->bindings.empty() || !graph.getNode(node))
    return false;
  if (motionType(node) == PhysicsMotionType::Dynamic)
    return true;
  const auto affected = impl_->bindingsInSubtree(graph, node);
  return std::ranges::any_of(affected, [&](uint32_t boundNode) {
    return impl_->bindings.at(boundNode).initialSettings.motionType == PhysicsMotionType::Dynamic;
  });
}

std::optional<PhysicsMotionType> PhysicsScene::motionType(uint32_t node) const noexcept {
  if (const auto found = impl_->bindings.find(node); found != impl_->bindings.end())
    return found->second.initialSettings.motionType;
  return std::nullopt;
}

std::optional<PhysicsBodyId> PhysicsScene::bodyId(uint32_t node) const noexcept {
  if (const auto found = impl_->bindings.find(node); found != impl_->bindings.end())
    return found->second.body;
  return std::nullopt;
}

std::optional<PhysicsBodySettings> PhysicsScene::bodySettings(uint32_t node) const noexcept {
  if (const auto found = impl_->bindings.find(node); found != impl_->bindings.end())
    return found->second.initialSettings;
  return std::nullopt;
}

std::optional<PhysicsBodyPose> PhysicsScene::bodyPose(uint32_t node) const {
  if (const auto body = bodyId(node))
    return impl_->world->pose(*body);
  return std::nullopt;
}

PhysicsJointId PhysicsScene::createJoint(SceneGraph &graph,
    std::optional<uint32_t> nodeA, uint32_t nodeB, PhysicsJointSettings settings) {
  impl_->bindGraph(graph);
  if (!hasBody(nodeB) || (nodeA && (!hasBody(*nodeA) || *nodeA == nodeB)))
    throw std::invalid_argument("A joint requires two distinct attached bodies or a world anchor");
  settings = PhysicsWorld::validateJointSettings(settings);
  if (impl_->bindings.at(nodeB).initialSettings.motionType != PhysicsMotionType::Dynamic &&
      (!nodeA || impl_->bindings.at(*nodeA).initialSettings.motionType != PhysicsMotionType::Dynamic))
    throw std::invalid_argument("A joint requires at least one dynamic body");
  // During playback keep kinematic targets scheduled for the next tick. Joint
  // frames are defined from the current body poses, not those future targets.
  impl_->synchronizeAuthored(graph, impl_->playing, static_cast<float>(kFixedStepSeconds), true);
  const auto id = impl_->world->createJoint(
      nodeA ? impl_->bindings.at(*nodeA).body : kInvalidPhysicsBodyId,
      impl_->bindings.at(nodeB).body, settings);
  try {
    const auto canonical = *impl_->world->jointState(id);
    const auto nodeAnchor = [&](uint32_t node, const PhysicsJointFrame &frame) {
      const auto &binding = impl_->bindings.at(node);
      return binding.localCenter + frame.anchor / glm::dvec3{binding.signedScale};
    };
    impl_->joints.emplace(id, Impl::Joint{nodeA, nodeB, id, canonical.settings,
        canonical.frameA, canonical.frameB,
        nodeA ? nodeAnchor(*nodeA, canonical.frameA) : canonical.frameA.anchor,
        nodeAnchor(nodeB, canonical.frameB)});
  } catch (...) {
    (void)impl_->world->destroyJoint(id);
    throw;
  }
  return id;
}

bool PhysicsScene::destroyJoint(PhysicsJointId id) noexcept {
  const auto found = impl_->joints.find(id);
  if (found == impl_->joints.end()) return false;
  (void)impl_->world->destroyJoint(found->second.bodyJoint);
  impl_->joints.erase(found);
  return true;
}

bool PhysicsScene::setJointMotor(PhysicsJointId id, PhysicsJointMotorSettings motor) {
  const auto found = impl_->joints.find(id);
  if (found == impl_->joints.end()) return false;
  if (!impl_->world->setJointMotor(found->second.bodyJoint, motor)) return false;
  found->second.settings.motor = impl_->world->jointState(found->second.bodyJoint)->settings.motor;
  return true;
}

bool PhysicsScene::setJointLimits(PhysicsJointId id, bool enabled, float lower, float upper) {
  const auto found = impl_->joints.find(id);
  if (found == impl_->joints.end()) return false;
  if (!impl_->world->setJointLimits(found->second.bodyJoint, enabled, lower, upper)) return false;
  found->second.settings = impl_->world->jointState(found->second.bodyJoint)->settings;
  return true;
}

std::optional<PhysicsSceneJointState> PhysicsScene::jointState(PhysicsJointId id) const {
  const auto found = impl_->joints.find(id);
  if (found == impl_->joints.end()) return std::nullopt;
  const auto state = impl_->world->jointState(found->second.bodyJoint);
  if (!state) return std::nullopt;
  return PhysicsSceneJointState{id, found->second.nodeA, found->second.nodeB, *state};
}

std::vector<PhysicsSceneJointState> PhysicsScene::joints() const {
  std::vector<PhysicsSceneJointState> result;
  result.reserve(impl_->joints.size());
  for (const auto &[id, joint] : impl_->joints) {
    (void)joint;
    if (auto state = jointState(id)) result.push_back(*state);
  }
  std::ranges::sort(result, {}, &PhysicsSceneJointState::id);
  return result;
}

size_t PhysicsScene::jointCount() const noexcept { return impl_->joints.size(); }

PhysicsSceneStatistics PhysicsScene::statistics() const noexcept {
  return {.bodyCount = bodyCount(),
          .activeBodyCount = impl_->world->activeBodyCount(),
          .jointCount = jointCount(),
          .substeps = impl_->substeps,
          .totalSteps = impl_->totalSteps,
          .transformPublications = impl_->transformPublications,
          .authoredPoseUpdates = impl_->authoredPoseUpdates,
          .accumulatedSeconds = impl_->accumulator,
          .droppedSeconds = impl_->dropped};
}

} // namespace container::physics
