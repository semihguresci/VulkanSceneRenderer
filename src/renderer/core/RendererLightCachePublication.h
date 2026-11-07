#pragma once

#include <Container/renderer/lighting/ImportedSceneLightEdits.h>

#include <bit>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

namespace container::renderer::detail {

inline bool sameLightCacheVector(const glm::vec4& a,const glm::vec4& b) noexcept {
  for(int i=0;i<4;++i) if(std::bit_cast<uint32_t>(a[i])!=std::bit_cast<uint32_t>(b[i])) return false;
  return true;
}
inline bool sameLightCacheValue(const container::gpu::PointLightData& a,const container::gpu::PointLightData& b) noexcept {
  return sameLightCacheVector(a.positionRadius,b.positionRadius) && sameLightCacheVector(a.colorIntensity,b.colorIntensity) &&
      sameLightCacheVector(a.directionInnerCos,b.directionInnerCos) && sameLightCacheVector(a.coneOuterCosType,b.coneOuterCosType) &&
      sameLightCacheVector(a.iesTangent,b.iesTangent) && a.iesProfileAddress==b.iesProfileAddress;
}
inline bool sameLightCacheValue(const container::gpu::AreaLightData& a,const container::gpu::AreaLightData& b) noexcept {
  return sameLightCacheVector(a.positionRange,b.positionRange) && sameLightCacheVector(a.colorIntensity,b.colorIntensity) &&
      sameLightCacheVector(a.directionType,b.directionType) && sameLightCacheVector(a.tangentHalfSize,b.tangentHalfSize) &&
      sameLightCacheVector(a.bitangentHalfSize,b.bitangentHalfSize);
}
inline bool sameLightCacheValue(const EditableLightEntity& a,const EditableLightEntity& b) noexcept {
  return a.id==b.id && a.type==b.type && a.source==b.source && a.label==b.label &&
      a.selected==b.selected && a.editable==b.editable &&
      LightEditValuesEqual(LightEditValuesFrom(a),LightEditValuesFrom(b));
}
template<class T> bool sameLightCacheValue(const std::optional<T>& a,const std::optional<T>& b) noexcept {
  return a.has_value()==b.has_value() && (!a || sameLightCacheValue(*a,*b));
}

// Own one preallocated replacement map node. Unrelated cache cells are never
// copied, compared, overwritten or retained by a light history command.
template<class Map> class LightCacheCell {
public:
  using Key=typename Map::key_type;
  using Value=typename Map::mapped_type;
  LightCacheCell(const Map& live,const Key& key,const Value& after):key_(key) {
    if(const auto found=live.find(key);found!=live.end()) before_.emplace(found->second);
    Map scratch;scratch.emplace(key,after);next_=scratch.extract(scratch.begin());
  }
  LightCacheCell(LightCacheCell&&) noexcept=default;
  LightCacheCell& operator=(LightCacheCell&&) noexcept=default;
  LightCacheCell(const LightCacheCell&)=delete;
  [[nodiscard]] bool matches(const Map& live) const noexcept {
    if(consumed_ || !next_) return false;
    const auto found=live.find(key_);
    return before_ ? found!=live.end() && sameLightCacheValue(found->second,*before_) : found==live.end();
  }
  void exchange(Map& live) noexcept {
    if(const auto found=live.find(key_);found!=live.end()) retired_=live.extract(found);
    const auto inserted=live.insert(std::move(next_));
    if(!inserted.inserted) std::terminate();
    consumed_=true;
  }
private:
  Key key_;
  std::optional<Value> before_;
  typename Map::node_type next_,retired_;
  bool consumed_{};
};

class LightCachePublication {
public:
  using Mirrors=std::map<size_t,std::shared_ptr<SceneLightEdits>>;
  using PointMap=decltype(ImportedSceneLightEdits::points);
  using AreaMap=decltype(ImportedSceneLightEdits::areas);
  using DirectionMap=decltype(ImportedSceneLightEdits::directionals);
  struct SourceView {
    std::span<const container::scene::AuthoredLightBinding> pointBindings,directionalBindings,areaBindings;
  };
  LightCachePublication(std::shared_ptr<ImportedSceneLightEdits>& global,Mirrors& mirrors,
      const container::scene::AuthoredSceneLights& source,const SceneLightEdits& before,
      const SceneLightEdits& after,EditableLightId target,size_t scene,uint64_t maxOwnedBytes=64*1024)
      :LightCachePublication(global,mirrors,SourceView{source.pointBindings,source.directionalBindings,source.areaBindings},
          before,after,target,scene,maxOwnedBytes) {}
  LightCachePublication(std::shared_ptr<ImportedSceneLightEdits>& global,Mirrors& mirrors,
      SourceView source,const SceneLightEdits& before,const SceneLightEdits& after,
      EditableLightId target,size_t scene,uint64_t maxOwnedBytes=64*1024)
      :global_(global),mirrors_(mirrors),original_(global),scene_(scene) {
    const auto mirror=mirrors.find(scene);
    mirrorExisted_=mirror!=mirrors.end();if(mirrorExisted_) mirror_=mirror->second;
    const auto check=[&](const container::scene::AuthoredLightBinding& key,size_t label=0,size_t priorLabel=0) {
      if(key.extension.size()>256 || label>4096 || priorLabel>4096 ||
          sizeof(*this)+16384+3*key.extension.size()+3*label+priorLabel>maxOwnedBytes)
        throw std::length_error("Light cache publication exceeds its selected-cell budget");
    };
    const auto priorDirectionalLabel=[&](const container::scene::AuthoredLightBinding& key) noexcept -> size_t {
      if(!global) return 0;
      const auto found=global->directionals.find(key);
      return found!=global->directionals.end() && found->second ? found->second->label.size():0;
    };
    const auto destination=[&]() -> ImportedSceneLightEdits& {
      if(!destination_) destination_=global?global:std::make_shared<ImportedSceneLightEdits>();
      return *destination_;
    };
    if(target.source==EditableLightSource::Imported) {
      if(target.type==EditableLightType::Point || target.type==EditableLightType::Spot) {
        if(target.index>=source.pointBindings.size() || target.index>=after.importedPointOverrides.size())
          throw std::invalid_argument("The imported point light cache binding changed");
        const auto& key=source.pointBindings[target.index];check(key);
        point_.emplace(destination().points,key,after.importedPointOverrides[target.index]);
      } else if(target.type==EditableLightType::Area) {
        if(target.index>=source.areaBindings.size() || target.index>=after.importedAreaOverrides.size())
          throw std::invalid_argument("The imported area light cache binding changed");
        const auto& key=source.areaBindings[target.index];check(key);
        area_.emplace(destination().areas,key,after.importedAreaOverrides[target.index]);
      } else if(target.type==EditableLightType::Directional) {
        if(source.directionalBindings.empty()) throw std::invalid_argument("The imported sun cache binding changed");
        const auto& key=source.directionalBindings.front();
        check(key,after.directionalOverride?after.directionalOverride->label.size():0,priorDirectionalLabel(key));
        const auto value=after.directionalOverride && after.directionalOverride->source==EditableLightSource::Imported
            ?after.directionalOverride:std::optional<EditableLightEntity>{};
        directional_.emplace(destination().directionals,key,value);
      }
    } else if(target.type==EditableLightType::Directional && target.source==EditableLightSource::Manual &&
        !source.directionalBindings.empty() &&
        (!before.directionalOverride || before.directionalOverride->source!=EditableLightSource::Manual)) {
      // Adding a manual sun masks, rather than deletes, the current imported
      // override. Preserve it by binding before the visible adapter changes.
      const auto& key=source.directionalBindings.front();
      check(key,before.directionalOverride?before.directionalOverride->label.size():0,priorDirectionalLabel(key));
      const auto value=before.directionalOverride && before.directionalOverride->source==EditableLightSource::Imported
          ?before.directionalOverride:std::optional<EditableLightEntity>{};
      directional_.emplace(destination().directionals,key,value);
    }
    if(sizeof(*this)>maxOwnedBytes) throw std::length_error("Light cache publication budget is too small");
  }
  [[nodiscard]] bool matches() const noexcept {
    if(consumed_ || global_.get()!=original_.get()) return false;
    const auto mirror=mirrors_.find(scene_);
    if(mirrorExisted_ ? mirror==mirrors_.end() || mirror->second!=mirror_ : mirror!=mirrors_.end()) return false;
    return (!point_ || point_->matches(destination_->points)) && (!area_ || area_->matches(destination_->areas)) &&
        (!directional_ || directional_->matches(destination_->directionals));
  }
  bool exchange() noexcept {
    if(!matches()) return false;
    if(point_) point_->exchange(destination_->points);
    if(area_) area_->exchange(destination_->areas);
    if(directional_) directional_->exchange(destination_->directionals);
    if(destination_ && !original_) global_.swap(destination_);
    // The active scene is always rebuilt from live edits before scene departure
    // or save. Drop only its stale mirror; every inactive scene stays untouched.
    if(const auto found=mirrors_.find(scene_);found!=mirrors_.end()) retiredMirror_=mirrors_.extract(found);
    consumed_=true;return true;
  }
private:
  std::shared_ptr<ImportedSceneLightEdits>& global_;
  Mirrors& mirrors_;
  std::shared_ptr<ImportedSceneLightEdits> original_,destination_;
  size_t scene_{};
  bool mirrorExisted_{},consumed_{};
  std::shared_ptr<SceneLightEdits> mirror_;
  Mirrors::node_type retiredMirror_;
  std::optional<LightCacheCell<PointMap>> point_;
  std::optional<LightCacheCell<AreaMap>> area_;
  std::optional<LightCacheCell<DirectionMap>> directional_;
};
} // namespace container::renderer::detail
