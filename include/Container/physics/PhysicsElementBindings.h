#pragma once

#include "Container/physics/PhysicsScene.h"
#include "Container/utility/ObjectTransform.h"

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace container::physics {

// Owned by the adapter after attachment. Bounds and model are in the same
// world space; one product may contain independently transformed fragments.
// Models must be nonsingular affine transforms with finite float normals, as
// required when the renderer publishes their subsequent simulated poses.
struct PhysicsElementFragment {
  uint32_t objectIndex{UINT32_MAX};
  glm::mat4 model{1.0f};
  glm::vec3 worldMin{0.0f};
  glm::vec3 worldMax{0.0f};
};

// Binds auxiliary products to the same PhysicsScene as primary mesh nodes.
// Nonrenderable proxy nodes carry each product's aggregate box and preserve
// all fragment transforms relative to its attachment-time anchor.
class PhysicsElementBindings {
public:
  PhysicsElementBindings();
  ~PhysicsElementBindings();
  PhysicsElementBindings(const PhysicsElementBindings &) = delete;
  PhysicsElementBindings &operator=(const PhysicsElementBindings &) = delete;
  PhysicsElementBindings(PhysicsElementBindings &&) noexcept;
  PhysicsElementBindings &operator=(PhysicsElementBindings &&) noexcept;

  // A publication change retires only auxiliary bodies. Obsolete fragment
  // indices are never restored into a replacement model. Proxy nodes can be
  // reused while the primary SceneGraph publication remains unchanged.
  // Replacing that graph retires its entire obsolete physics world, unless
  // PhysicsScene has already switched to another graph's bodies.
  void synchronizePublication(scene::SceneGraph &graph, PhysicsScene &physics,
                              uint64_t modelGeneration);

  // Replacing settings retains the original fragment membership, box and
  // authored reset transforms. Membership is immutable within a publication.
  // Invalid inputs and failed replacements retain the existing live binding.
  [[nodiscard]] PhysicsBodyId attachOrReplace(
      scene::SceneGraph &graph, PhysicsScene &physics, uint64_t modelGeneration,
      uint32_t productIdentityId,
      std::span<const PhysicsElementFragment> fragments,
      PhysicsBodySettings settings = {}, std::string_view label = {});

  [[nodiscard]] std::optional<uint32_t>
  bodyNodeForObject(uint32_t objectIndex) const noexcept;
  [[nodiscard]] std::optional<uint32_t>
  bodyNodeForProduct(uint32_t productIdentityId) const noexcept;
  [[nodiscard]] std::optional<uint32_t>
  productForObject(uint32_t objectIndex) const noexcept;
  [[nodiscard]] std::span<const uint32_t>
  objectsForNode(uint32_t node) const noexcept;
  // Internal roots and pooled proxies retain ownership even when no body is
  // attached, and must not participate in user hierarchy reparent operations.
  [[nodiscard]] bool ownsNode(const scene::SceneGraph &graph,
                               uint32_t node) const noexcept;
  [[nodiscard]] size_t productCount() const noexcept;

  [[nodiscard]] std::vector<gpu::ObjectTransformUpdate>
  collectTransforms(const scene::SceneGraph &graph) const;
  // Preview an active proxy or its owned root without changing graph/bodies.
  // Unrelated nodes and obsolete graph publications produce no updates.
  [[nodiscard]] std::vector<gpu::ObjectTransformUpdate>
  previewTransforms(const scene::SceneGraph &graph, uint32_t node,
                    const glm::mat4 &local) const;
  [[nodiscard]] std::vector<gpu::ObjectTransformUpdate>
  detach(scene::SceneGraph &graph, PhysicsScene &physics, uint32_t objectIndex,
         bool restore = true);
  // A null graph only discards bookkeeping after its live bodies have already
  // been retired by PhysicsScene, including a switch to another graph. A
  // matching graph recycles proxy nodes.
  [[nodiscard]] std::vector<gpu::ObjectTransformUpdate>
  clear(scene::SceneGraph *graph, PhysicsScene &physics, bool restore = true);

  // Root edits also move attachment-time reset poses through an unbound proxy
  // parent. Dynamic edits pause playback; kinematic edits remain authored
  // motion. Existing velocities are retained by PhysicsScene synchronization.
  void translate(scene::SceneGraph &graph, PhysicsScene &physics,
                 glm::vec3 delta);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace container::physics
