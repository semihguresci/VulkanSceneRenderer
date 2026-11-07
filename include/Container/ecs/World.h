#pragma once

#include "Container/ecs/Components.h"

#include <entt/entt.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace container::scene {
class SceneGraph;
}

namespace container::ecs {

// Thin wrapper around entt::registry providing typed helpers for the
// VulkanSceneRenderer rendering pipeline.
//
// The World does NOT own or replace the SceneGraph.  Instead it provides
// a mirror: syncFromSceneGraph() reads the graph and creates/updates
// entities so that ECS views can be used for draw-call extraction.
class World {
public:
  World();

  struct LightValueLimits {
    uint64_t maxLights{1'000'000};
    uint64_t maxOwnedBytes{64ull * 1024 * 1024};
    uint64_t maxWork{64ull * 1024 * 1024};
  };
  // Fixed-count point/spot mirror update. No registry replacement, context
  // copy, entity creation/destruction or user signals occur at publication.
  class PreparedLightValues {
  public:
    PreparedLightValues() noexcept;
    ~PreparedLightValues();
    PreparedLightValues(PreparedLightValues&&) noexcept;
    PreparedLightValues& operator=(PreparedLightValues&&) noexcept;
    PreparedLightValues(const PreparedLightValues&)=delete;
    PreparedLightValues& operator=(const PreparedLightValues&)=delete;
    explicit operator bool() const noexcept;
    [[nodiscard]] uint64_t ownedBytes() const noexcept;
  private:
    struct State;
    std::unique_ptr<State> state_;
    friend class World;
  };
  // ECS storage order differs from the renderer's canonical light order.
  // Match the complete old payload multiset to distinct current entities,
  // including duplicate records, before assigning desired canonical rows.
  [[nodiscard]] PreparedLightValues prepareLightValues(
      std::span<const container::gpu::PointLightData> beforeCanonical,
      std::span<const container::gpu::PointLightData> desiredCanonical) const;
  [[nodiscard]] PreparedLightValues prepareLightValues(
      std::span<const container::gpu::PointLightData> beforeCanonical,
      std::span<const container::gpu::PointLightData> desiredCanonical,
      const LightValueLimits&) const;
  [[nodiscard]] bool canExchangeLightValues(const PreparedLightValues&) const noexcept;
  [[nodiscard]] bool exchangeLightValues(PreparedLightValues&) noexcept;

  // Rebuild renderable entities from the current SceneGraph state.
  // Re-creates one entity per renderable node with TransformComponent,
  // MeshComponent, MaterialComponent, RenderableTag, and SceneNodeRef.
  // Light and camera entities are left intact.
  void syncFromSceneGraph(const container::scene::SceneGraph &graph);

  // Iterate all renderable entities and invoke the callback with each
  // entity's components.  The callback receives transform, mesh, and
  // material components in a cache-friendly order via an EnTT view.
  using RenderableVisitor =
      std::function<void(const TransformComponent &, const MeshComponent &,
                         const MaterialComponent &)>;
  void forEachRenderable(const RenderableVisitor &visitor) const;

  using RenderableWithNodeVisitor =
      std::function<void(const TransformComponent &, const MeshComponent &,
                         const MaterialComponent &, const SceneNodeRef &)>;
  void forEachRenderableWithNode(const RenderableWithNodeVisitor &visitor) const;

  // Point-light entity helpers used by lighting systems.
  using LightVisitor = std::function<void(const LightComponent &)>;
  [[nodiscard]] entt::entity
  createPointLight(const container::gpu::PointLightData &data);
  void
  replacePointLights(std::span<const container::gpu::PointLightData> lights);
  void forEachPointLight(const LightVisitor &visitor) const;
  void clearPointLights();

  // Active camera entity helpers.
  [[nodiscard]] entt::entity
  setActiveCamera(const container::gpu::CameraData &data,
                  float nearPlane = 0.1f, float farPlane = 100.0f);
  [[nodiscard]] const CameraComponent *activeCamera() const;
  [[nodiscard]] bool hasActiveCamera() const;
  void clearActiveCamera();

  // Number of entities that carry the RenderableTag.
  [[nodiscard]] uint32_t renderableCount() const;

  // Number of entities that carry the LightTag.
  [[nodiscard]] uint32_t pointLightCount() const;

  // Number of total entities in the registry.
  [[nodiscard]] uint32_t entityCount() const;

  // Direct access for advanced queries.
  [[nodiscard]] entt::registry &registry() { return registry_; }
  [[nodiscard]] const entt::registry &registry() const { return registry_; }

  // Remove all entities and components.
  void clear();

  // Detached copy of the engine's component set, entity identities, active
  // camera and copyable registry context. Unknown nonempty component pools
  // reject, rather than silently disappearing from a staged scene.
  [[nodiscard]] std::unique_ptr<World> cloneState() const;
  // Same checked engine component/entity copy, without copying opaque context
  // values. Used by pose generations that retain each World's own context.
  [[nodiscard]] std::unique_ptr<World> cloneStateWithoutContext() const;
  // Bounded admission and exact engine-state witness for rare structural
  // staging. Opaque context is neither counted/copied nor compared.
  [[nodiscard]] uint64_t stateStorageBytes(uint64_t maxOwnedBytes,uint64_t maxWork) const;
  [[nodiscard]] bool stateValuesMatch(const World&) const noexcept;

  // Exchanges the complete ECS state, including context and active camera,
  // without replacing either World or registry object. Existing views and
  // component pointers must be reacquired after the exchange.
  void swapState(World& other) noexcept;
  void swapStatePreservingContext(World& other) noexcept;

private:
  [[nodiscard]] std::unique_ptr<World> cloneStateImpl(bool includeContext) const;
  void clearRenderables();
  void lightStateChanged() noexcept;

  entt::registry registry_;
  entt::entity activeCameraEntity_{entt::null};
  uint64_t lightOwnerIdentity_{},lightRevision_{1};
};

} // namespace container::ecs
