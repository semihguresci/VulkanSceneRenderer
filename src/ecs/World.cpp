#include "Container/ecs/World.h"
#include "Container/utility/SceneGraph.h"

#include <vector>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace container::ecs {
namespace {
using Point=container::gpu::PointLightData;
static_assert(std::is_trivially_copyable_v<Point> && sizeof(Point)==16*sizeof(float));
uint64_t nextLightOwner() noexcept {
  static std::atomic<uint64_t> next{1};
  auto value=next.load(std::memory_order_relaxed);
  while(value && value!=UINT64_MAX) {
    if(next.compare_exchange_weak(value,value+1,std::memory_order_relaxed)) return value;
  }
  return 0; // Exhaustion disables new prepared updates rather than reusing IDs.
}
bool pointEqual(const Point& a,const Point& b) noexcept {return std::memcmp(&a,&b,sizeof(Point))==0;}
bool pointFinite(const Point& p) noexcept {
  for(const auto* vector:{&p.positionRadius,&p.colorIntensity,&p.directionInnerCos,&p.coneOuterCosType})
    for(int i=0;i<4;++i) if(!std::isfinite((*vector)[i])) return false;
  return true;
}
bool engineType(const entt::type_info& type) noexcept {
  return type==entt::type_id<TransformComponent>() || type==entt::type_id<MeshComponent>() ||
      type==entt::type_id<MaterialComponent>() || type==entt::type_id<LightComponent>() ||
      type==entt::type_id<CameraComponent>() || type==entt::type_id<RenderableTag>() ||
      type==entt::type_id<LightTag>() || type==entt::type_id<CameraTag>() || type==entt::type_id<SceneNodeRef>();
}
bool matrixFinite(const glm::mat4& value) noexcept {
  for(int c=0;c<4;++c) for(int r=0;r<4;++r) if(!std::isfinite(value[c][r])) return false;return true;
}
[[noreturn]] void lightFailure(const char* message) {throw std::invalid_argument(message);}
}

struct World::PreparedLightValues::State {
  struct Row {entt::entity entity{entt::null};Point before{},after{};};
  const World* owner{};
  uint64_t ownerIdentity{},revision{},bytes{};
  std::vector<Row> rows;
  bool consumed{};
};
World::World():lightOwnerIdentity_(nextLightOwner()) {}
World::PreparedLightValues::PreparedLightValues() noexcept=default;
World::PreparedLightValues::~PreparedLightValues()=default;
World::PreparedLightValues::PreparedLightValues(PreparedLightValues&&) noexcept=default;
World::PreparedLightValues& World::PreparedLightValues::operator=(PreparedLightValues&&) noexcept=default;
World::PreparedLightValues::operator bool() const noexcept {return state_ && !state_->consumed;}
uint64_t World::PreparedLightValues::ownedBytes() const noexcept {return state_?state_->bytes:0;}
void World::lightStateChanged() noexcept {
  if(lightRevision_!=UINT64_MAX) ++lightRevision_;
}
World::PreparedLightValues World::prepareLightValues(std::span<const Point> before,std::span<const Point> after) const {
  return prepareLightValues(before,after,LightValueLimits{});
}
World::PreparedLightValues World::prepareLightValues(std::span<const Point> before,std::span<const Point> after,
    const LightValueLimits& limits) const {
  using State=PreparedLightValues::State;
  if(!lightOwnerIdentity_ || lightRevision_==UINT64_MAX || !limits.maxLights || limits.maxLights==UINT64_MAX ||
      !limits.maxOwnedBytes || limits.maxOwnedBytes==UINT64_MAX || !limits.maxWork || limits.maxWork==UINT64_MAX ||
      before.size()!=after.size() || before.size()>limits.maxLights || before.size()>UINT32_MAX)
    lightFailure("Invalid fixed-count World light value limits or shape");
  const auto* tags=registry_.storage<LightTag>();const auto* components=registry_.storage<LightComponent>();
  if((tags?tags->size():0)!=before.size() || (components?components->size():0)!=before.size())
    lightFailure("World light membership differs from the canonical light table");
  const uint64_t count=before.size(),overhead=sizeof(State)+256,stride=sizeof(State::Row)+sizeof(size_t);
  if(overhead>limits.maxOwnedBytes || count>(limits.maxOwnedBytes-overhead)/stride)
    lightFailure("World light value preparation exceeds its byte budget");
  uint64_t work{};
  const auto charge=[&](uint64_t amount) {
    if(amount>limits.maxWork-work) lightFailure("World light value preparation exceeds its work budget");
    work+=amount;
  };
  for(size_t i=0;i<before.size();++i) {
    charge(32);if(!pointFinite(before[i]) || !pointFinite(after[i])) lightFailure("World light value input is nonfinite");
  }
  PreparedLightValues result;result.state_=std::make_unique<State>();auto& s=*result.state_;
  s.owner=this;s.ownerIdentity=lightOwnerIdentity_;s.revision=lightRevision_;s.bytes=overhead+count*stride;
  s.rows.reserve(before.size());std::vector<size_t> canonical(before.size());std::iota(canonical.begin(),canonical.end(),size_t{0});
  for(const auto entity:registry_.view<const LightTag>()) {
    charge(16);const auto* value=registry_.try_get<LightComponent>(entity);
    if(!value || !pointFinite(value->data)) lightFailure("World contains an incomplete or nonfinite light entity");
    s.rows.push_back({entity,value->data,{}});
  }
  std::sort(canonical.begin(),canonical.end(),[&](size_t a,size_t b) {
    charge(16);const int order=std::memcmp(&before[a],&before[b],sizeof(Point));return order?order<0:a<b;
  });
  std::sort(s.rows.begin(),s.rows.end(),[&](const State::Row& a,const State::Row& b) {
    charge(16);const int order=std::memcmp(&a.before,&b.before,sizeof(Point));
    return order?order<0:entt::to_integral(a.entity)<entt::to_integral(b.entity);
  });
  for(size_t i=0;i<s.rows.size();++i) {
    charge(16);if(!pointEqual(s.rows[i].before,before[canonical[i]]))
      lightFailure("World light values differ from the canonical table");
    s.rows[i].after=after[canonical[i]];
  }
  if(!canExchangeLightValues(result)) lightFailure("World lights changed during value preparation");
  return result;
}
bool World::canExchangeLightValues(const PreparedLightValues& prepared) const noexcept {
  if(!prepared) return false;const auto& s=*prepared.state_;
  if(s.owner!=this || !lightOwnerIdentity_ || s.ownerIdentity!=lightOwnerIdentity_ || s.revision!=lightRevision_ ||
      lightRevision_==UINT64_MAX) return false;
  const auto* tags=registry_.storage<LightTag>();const auto* components=registry_.storage<LightComponent>();
  if((tags?tags->size():0)!=s.rows.size() || (components?components->size():0)!=s.rows.size()) return false;
  for(const auto& row:s.rows) {
    if(!registry_.valid(row.entity) || !registry_.all_of<LightTag>(row.entity)) return false;
    const auto* value=registry_.try_get<LightComponent>(row.entity);
    if(!value || !pointEqual(value->data,row.before)) return false;
  }
  return true;
}
bool World::exchangeLightValues(PreparedLightValues& prepared) noexcept {
  if(!canExchangeLightValues(prepared)) return false;
  for(const auto& row:prepared.state_->rows) registry_.get<LightComponent>(row.entity).data=row.after;
  lightStateChanged();
  prepared.state_->consumed=true;
  return true;
}

uint64_t World::stateStorageBytes(uint64_t maximum,uint64_t maximumWork) const {
  if(!maximum || maximum==UINT64_MAX || !maximumWork || maximumWork==UINT64_MAX)
    lightFailure("World structural staging requires finite positive budgets");
  uint64_t bytes{},work{};
  const auto charge=[&](uint64_t count,uint64_t stride) {
    if(count>(maximum-bytes)/stride) lightFailure("World structural staging exceeds its byte budget");bytes+=count*stride;
  };
  const auto visit=[&](uint64_t count) {
    if(count>maximumWork-work) lightFailure("World structural staging exceeds its work budget");work+=count;
  };
  charge(1,sizeof(World)+1024);
  for(auto&& [id,pool]:registry_.storage()) {
    (void)id;visit(1);if(!pool.size()) continue;
    if(!engineType(pool.type())) lightFailure("Cannot stage an unknown ECS component pool");
    charge(pool.capacity(),1024);charge(pool.extent(),2*sizeof(entt::entity));visit(pool.size());
  }
  if(const auto* entities=registry_.storage<entt::entity>()) {
    charge(entities->capacity(),1024);charge(entities->extent(),2*sizeof(entt::entity));visit(entities->size());
    for(auto&& [entity]:entities->each()) {
      visit(1);if(uint64_t(entt::to_entity(entity))>maximum/sizeof(entt::entity))
        lightFailure("World structural staging exceeds its sparse entity address budget");
    }
  }
  for(const auto e:registry_.view<const TransformComponent>()) {
    visit(32);const auto& t=registry_.get<TransformComponent>(e);
    if(!matrixFinite(t.localTransform) || !matrixFinite(t.worldTransform)) lightFailure("World transform is nonfinite");
  }
  for(const auto e:registry_.view<const LightComponent>()) {
    visit(16);if(!pointFinite(registry_.get<LightComponent>(e).data)) lightFailure("World light is nonfinite");
  }
  for(const auto e:registry_.view<const CameraComponent>()) {
    visit(128);const auto& c=registry_.get<CameraComponent>(e);
    static_assert(sizeof(container::gpu::CameraData)==6*sizeof(glm::mat4)+4*sizeof(glm::vec4)+sizeof(glm::uvec4));
    if(!std::isfinite(c.nearPlane) || !std::isfinite(c.farPlane)) lightFailure("World camera planes are nonfinite");
    for(const auto* m:{&c.data.viewProj,&c.data.inverseViewProj,&c.data.unjitteredViewProj,
        &c.data.previousViewProj,&c.data.previousInverseViewProj,&c.data.previousJitteredViewProj})
      if(!matrixFinite(*m)) lightFailure("World camera matrix is nonfinite");
    for(const auto* v:{&c.data.cameraWorldPosition,&c.data.cameraForward,&c.data.jitterUv,&c.data.renderExtent})
      for(int i=0;i<4;++i) if(!std::isfinite((*v)[i])) lightFailure("World camera value is nonfinite");
  }
  return bytes;
}
bool World::stateValuesMatch(const World& other) const noexcept {
  if(activeCameraEntity_!=other.activeCameraEntity_ || entityCount()!=other.entityCount()) return false;
  const auto& right=other.registry_;
  for(const auto* registry:{&registry_,&right}) for(auto&& [id,pool]:registry->storage()) {
    (void)id;if(pool.size() && !engineType(pool.type())) return false;
  }
  if(const auto* entities=registry_.storage<entt::entity>())
    for(auto&& [entity]:entities->each()) if(!right.valid(entity)) return false;
  const auto same=[&]<class T>() {
    const auto* left=registry_.storage<T>();const auto* target=right.storage<T>();
    if((left?left->size():0)!=(target?target->size():0)) return false;
    if(left) for(const auto e:registry_.view<const T>()) {
      if constexpr(std::is_empty_v<T>) {if(!right.all_of<T>(e)) return false;}
      else {
        const auto* b=right.try_get<T>(e);if(!b) return false;const auto& a=registry_.get<T>(e);
        if constexpr(std::is_same_v<T,TransformComponent>) {
          if(std::memcmp(&a.localTransform,&b->localTransform,sizeof(glm::mat4)) ||
              std::memcmp(&a.worldTransform,&b->worldTransform,sizeof(glm::mat4))) return false;
        } else if constexpr(std::is_same_v<T,LightComponent>) {if(!pointEqual(a.data,b->data)) return false;}
        else if constexpr(std::is_same_v<T,CameraComponent>) {
          if(std::memcmp(&a.data,&b->data,sizeof(a.data)) ||
              std::bit_cast<uint32_t>(a.nearPlane)!=std::bit_cast<uint32_t>(b->nearPlane) ||
              std::bit_cast<uint32_t>(a.farPlane)!=std::bit_cast<uint32_t>(b->farPlane)) return false;
        } else if constexpr(std::is_same_v<T,MeshComponent>) {if(a.primitiveIndex!=b->primitiveIndex) return false;}
        else if constexpr(std::is_same_v<T,MaterialComponent>) {if(a.materialIndex!=b->materialIndex) return false;}
        else if constexpr(std::is_same_v<T,SceneNodeRef>) {if(a.nodeIndex!=b->nodeIndex) return false;}
      }
    }
    return true;
  };
  return same.template operator()<TransformComponent>() && same.template operator()<MeshComponent>() &&
      same.template operator()<MaterialComponent>() && same.template operator()<LightComponent>() &&
      same.template operator()<CameraComponent>() && same.template operator()<RenderableTag>() &&
      same.template operator()<LightTag>() && same.template operator()<CameraTag>() && same.template operator()<SceneNodeRef>();
}

std::unique_ptr<World> World::cloneState() const {
  return cloneStateImpl(true);
}
std::unique_ptr<World> World::cloneStateWithoutContext() const {
  return cloneStateImpl(false);
}
std::unique_ptr<World> World::cloneStateImpl(bool includeContext) const {
  for (auto&& [id, pool] : registry_.storage()) {
    (void)id;
    if (!pool.size()) continue;
    const auto& type = pool.type();
    if (type != entt::type_id<TransformComponent>() && type != entt::type_id<MeshComponent>() &&
        type != entt::type_id<MaterialComponent>() && type != entt::type_id<LightComponent>() &&
        type != entt::type_id<CameraComponent>() && type != entt::type_id<RenderableTag>() &&
        type != entt::type_id<LightTag>() && type != entt::type_id<CameraTag>() &&
        type != entt::type_id<SceneNodeRef>())
      throw std::runtime_error("Cannot stage an unknown ECS component pool");
  }
  auto result = std::make_unique<World>();
  if(includeContext) result->registry_.ctx() = registry_.ctx();
  if (const auto* entities = registry_.storage<entt::entity>())
    for (auto&& [entity] : entities->each()) (void)result->registry_.create(entity);
  const auto copy = [&]<class T>() {
    for (const auto entity : registry_.view<const T>()) {
      if constexpr (std::is_empty_v<T>) result->registry_.emplace<T>(entity);
      else result->registry_.emplace<T>(entity, registry_.get<T>(entity));
    }
  };
  copy.template operator()<TransformComponent>(); copy.template operator()<MeshComponent>();
  copy.template operator()<MaterialComponent>(); copy.template operator()<LightComponent>();
  copy.template operator()<CameraComponent>(); copy.template operator()<RenderableTag>();
  copy.template operator()<LightTag>(); copy.template operator()<CameraTag>();
  copy.template operator()<SceneNodeRef>();
  result->activeCameraEntity_ = activeCameraEntity_;
  return result;
}

void World::swapState(World& other) noexcept {
  if (this==&other) return;
  registry_.swap(other.registry_);
  std::swap(activeCameraEntity_, other.activeCameraEntity_);
  lightStateChanged();other.lightStateChanged();
}
void World::swapStatePreservingContext(World& other) noexcept {
  if(this==&other) return;
  using Context=std::remove_reference_t<decltype(registry_.ctx())>;
  static_assert(std::is_nothrow_swappable_v<Context>);
  // Context uses EnTT's noexcept vector-backed dense_map moves. Swap it first
  // so the subsequent registry exchange leaves each World's context in place.
  using std::swap;swap(registry_.ctx(),other.registry_.ctx());
  swapState(other);
}

void World::syncFromSceneGraph(const container::scene::SceneGraph &graph) {
  clearRenderables();

  for (const uint32_t nodeIndex : graph.renderableNodes()) {
    const auto *node = graph.getNode(nodeIndex);
    if (!node || !graph.isNodeEffectivelyVisible(nodeIndex))
      continue;

    const auto entity = registry_.create();
    registry_.emplace<TransformComponent>(entity, node->localTransform,
                                          node->worldTransform);
    registry_.emplace<MeshComponent>(entity, node->primitiveIndex);
    registry_.emplace<MaterialComponent>(entity, node->materialIndex);
    registry_.emplace<RenderableTag>(entity);
    registry_.emplace<SceneNodeRef>(entity, nodeIndex);
  }
}

void World::clearRenderables() {
  auto view = registry_.view<const RenderableTag>();

  std::vector<entt::entity> entities;
  entities.reserve(view.size());
  for (const auto entity : view) {
    entities.push_back(entity);
  }

  for (const auto entity : entities) {
    registry_.destroy(entity);
  }
}

void World::forEachRenderable(const RenderableVisitor &visitor) const {
  auto view = registry_.view<const TransformComponent, const MeshComponent,
                             const MaterialComponent, const RenderableTag>();
  for (auto [entity, transform, mesh, material] : view.each()) {
    visitor(transform, mesh, material);
  }
}

void World::forEachRenderableWithNode(
    const RenderableWithNodeVisitor &visitor) const {
  auto view =
      registry_.view<const TransformComponent, const MeshComponent,
                     const MaterialComponent, const SceneNodeRef,
                     const RenderableTag>();
  for (auto [entity, transform, mesh, material, node] : view.each()) {
    visitor(transform, mesh, material, node);
  }
}

entt::entity
World::createPointLight(const container::gpu::PointLightData &data) {
  const auto entity = registry_.create();
  registry_.emplace<LightComponent>(entity, LightComponent{data});
  registry_.emplace<LightTag>(entity);
  lightStateChanged();
  return entity;
}

void World::replacePointLights(
    std::span<const container::gpu::PointLightData> lights) {
  clearPointLights();
  for (const auto &light : lights) {
    (void)createPointLight(light);
  }
}

void World::forEachPointLight(const LightVisitor &visitor) const {
  auto view = registry_.view<const LightComponent, const LightTag>();
  for (auto [entity, light] : view.each()) {
    visitor(light);
  }
}

void World::clearPointLights() {
  auto view = registry_.view<const LightTag>();

  std::vector<entt::entity> entities;
  entities.reserve(view.size());
  for (const auto entity : view) {
    entities.push_back(entity);
  }

  for (const auto entity : entities) {
    registry_.destroy(entity);
  }
  lightStateChanged();
}

entt::entity World::setActiveCamera(const container::gpu::CameraData &data,
                                    float nearPlane, float farPlane) {
  if (activeCameraEntity_ == entt::null ||
      !registry_.valid(activeCameraEntity_)) {
    activeCameraEntity_ = registry_.create();
    registry_.emplace<CameraTag>(activeCameraEntity_);
  }

  registry_.emplace_or_replace<CameraComponent>(
      activeCameraEntity_, CameraComponent{data, nearPlane, farPlane});
  return activeCameraEntity_;
}

const CameraComponent *World::activeCamera() const {
  if (activeCameraEntity_ == entt::null ||
      !registry_.valid(activeCameraEntity_)) {
    return nullptr;
  }
  return registry_.try_get<CameraComponent>(activeCameraEntity_);
}

bool World::hasActiveCamera() const { return activeCamera() != nullptr; }

void World::clearActiveCamera() {
  if (activeCameraEntity_ != entt::null &&
      registry_.valid(activeCameraEntity_)) {
    registry_.destroy(activeCameraEntity_);
  }
  activeCameraEntity_ = entt::null;
}

uint32_t World::renderableCount() const {
  return static_cast<uint32_t>(registry_.view<const RenderableTag>().size());
}

uint32_t World::pointLightCount() const {
  return static_cast<uint32_t>(registry_.view<const LightTag>().size());
}

uint32_t World::entityCount() const {
  // In EnTT 3.14, the entity storage's each() iterates alive entities.
  const auto *storage = registry_.storage<entt::entity>();
  if (!storage)
    return 0;
  uint32_t count = 0;
  for ([[maybe_unused]] auto &&entry : storage->each()) {
    ++count;
  }
  return count;
}

void World::clear() {
  registry_.clear();
  activeCameraEntity_ = entt::null;
  lightStateChanged();
}

} // namespace container::ecs
