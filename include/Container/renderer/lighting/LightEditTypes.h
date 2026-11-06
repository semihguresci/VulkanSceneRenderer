#pragma once

#include "Container/renderer/lighting/EditableLight.h"

#include <bit>
#include <cstdint>
#include <string>

namespace container::renderer {

// Weak editor intent. A backend-issued incarnation, rather than a display
// ordinal, identifies the original selected light. Scene zero is valid.
struct LightEditTarget {
  uint64_t ownerIdentity{},assetIdentity{},graphIdentity{},scene{},incarnation{},revision{};
  EditableLightId id{};
  bool operator==(const LightEditTarget& b) const noexcept {
    return ownerIdentity==b.ownerIdentity&&assetIdentity==b.assetIdentity&&graphIdentity==b.graphIdentity&&scene==b.scene&&
        incarnation==b.incarnation&&revision==b.revision&&id.type==b.id.type&&id.source==b.id.source&&id.index==b.id.index;
  }
};
enum class LightEditField : uint32_t {
  Position=1u<<0, Direction=1u<<1, Color=1u<<2, Intensity=1u<<3,
  Range=1u<<4, SpotCone=1u<<5, AreaSize=1u<<6, AreaShape=1u<<7,
  AreaBasis=1u<<8
};
[[nodiscard]] constexpr uint32_t LightEditFieldBit(LightEditField field) noexcept {
  return static_cast<uint32_t>(field);
}
inline constexpr uint32_t kAllLightEditFields=(1u<<9)-1;
struct LightEditValues {
  glm::vec3 position{0},direction{0,-1,0},tangent{1,0,0},bitangent{0,0,1},color{1};
  float intensity{1},range{1},innerConeDegrees{},outerConeDegrees{};
  glm::vec2 areaHalfSize{.5f};
  float areaShape{container::gpu::kAreaLightTypeRectangle};
};
struct LightEditPatch { uint32_t fields{}; LightEditValues values; };
enum class LightEditPhase : uint8_t { Commit,Begin,Update,End,Cancel };
struct LightEditRequest {
  LightEditTarget target;
  LightEditPhase phase{LightEditPhase::Commit};
  uint64_t gesture{};
  LightEditPatch patch;
};
struct LightEditSnapshot {
  LightEditTarget target;
  EditableLightEntity light;
  bool available{};
  std::string reason,error;
};
[[nodiscard]] inline LightEditValues LightEditValuesFrom(const EditableLightEntity& light) noexcept {
  return {light.position,light.direction,light.tangent,light.bitangent,light.color,
      light.intensity,light.range,light.innerConeDegrees,light.outerConeDegrees,light.areaHalfSize,light.areaShape};
}
[[nodiscard]] inline bool SameLightEditIdentity(const LightEditTarget& a,const LightEditTarget& b) noexcept {
  auto left=a,right=b;left.revision=right.revision=0;left.id.index=right.id.index=0;return left==right;
}
[[nodiscard]] inline LightEditPatch LightEditPatchFrom(const EditableLightEntity& before,const EditableLightEntity& after) noexcept {
  LightEditPatch patch;patch.values=LightEditValuesFrom(after);
  const auto scalar=[](float a,float b) {return std::bit_cast<uint32_t>(a)==std::bit_cast<uint32_t>(b);};
  const auto vector=[&]<glm::length_t N>(const glm::vec<N,float,glm::defaultp>& a,const glm::vec<N,float,glm::defaultp>& b) {
    for(glm::length_t i=0;i<N;++i) if(!scalar(a[i],b[i])) return false;return true;
  };
  const auto changed=[&](LightEditField field,bool equal) {if(!equal) patch.fields|=LightEditFieldBit(field);};
  changed(LightEditField::Position,vector(before.position,after.position));
  changed(LightEditField::Direction,vector(before.direction,after.direction));
  changed(LightEditField::Color,vector(before.color,after.color));
  changed(LightEditField::Intensity,scalar(before.intensity,after.intensity));
  changed(LightEditField::Range,scalar(before.range,after.range));
  changed(LightEditField::SpotCone,scalar(before.innerConeDegrees,after.innerConeDegrees)&&scalar(before.outerConeDegrees,after.outerConeDegrees));
  changed(LightEditField::AreaSize,vector(before.areaHalfSize,after.areaHalfSize));
  changed(LightEditField::AreaShape,scalar(before.areaShape,after.areaShape));
  changed(LightEditField::AreaBasis,vector(before.tangent,after.tangent)&&vector(before.bitangent,after.bitangent));
  return patch;
}

} // namespace container::renderer
