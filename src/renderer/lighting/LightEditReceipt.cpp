#include "Container/renderer/lighting/LightEditReceipt.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace container::renderer {
namespace {
[[noreturn]] void reject(const char* message) {throw std::invalid_argument(message);}
bool bit(float a,float b) noexcept {return std::bit_cast<uint32_t>(a)==std::bit_cast<uint32_t>(b);}
template<glm::length_t N> bool vec(const glm::vec<N,float,glm::defaultp>& a,const glm::vec<N,float,glm::defaultp>& b) noexcept {
  for(glm::length_t i=0;i<N;++i) if(!bit(a[i],b[i])) return false;return true;
}
bool has(uint32_t mask,LightEditField field) noexcept {return (mask&LightEditFieldBit(field))!=0;}
bool finite(const LightEditValues& v) noexcept {
  for(const auto* x:{&v.position,&v.direction,&v.tangent,&v.bitangent,&v.color})
    for(int i=0;i<3;++i) if(!std::isfinite((*x)[i])) return false;
  return std::isfinite(v.intensity)&&std::isfinite(v.range)&&std::isfinite(v.innerConeDegrees)&&std::isfinite(v.outerConeDegrees)&&
      std::isfinite(v.areaHalfSize.x)&&std::isfinite(v.areaHalfSize.y)&&std::isfinite(v.areaShape);
}
void assign(EditableLightEntity& e,const LightEditValues& v,uint32_t fields) {
  if(has(fields,LightEditField::Position)) e.position=v.position;
  if(has(fields,LightEditField::Direction)) e.direction=v.direction;
  if(has(fields,LightEditField::Color)) e.color=v.color;
  if(has(fields,LightEditField::Intensity)) e.intensity=v.intensity;
  if(has(fields,LightEditField::Range)) e.range=v.range;
  if(has(fields,LightEditField::SpotCone)) {e.innerConeDegrees=v.innerConeDegrees;e.outerConeDegrees=v.outerConeDegrees;}
  if(has(fields,LightEditField::AreaSize)) e.areaHalfSize=v.areaHalfSize;
  if(has(fields,LightEditField::AreaShape)) e.areaShape=v.areaShape;
  if(has(fields,LightEditField::AreaBasis)) {e.direction=v.direction;e.tangent=v.tangent;e.bitangent=v.bitangent;}
}
EditableLightEntity canonical(const EditableLightEntity& e,uint32_t fields) {
  EditableLightEntity result;
  if(e.type==EditableLightType::Area) result=editableAreaLight(e.source,e.id.index,areaLightDataFromEditable(e),e.selected);
  else if(e.type==EditableLightType::Point || e.type==EditableLightType::Spot)
    result=editablePointLight(e.source,e.id.index,pointLightDataFromEditable(e),e.selected);
  else {auto data=directionalLightDataFromEditable(e,{});result=editableDirectionalLight(e.source,e.position,data,e.selected);}
  auto selected=e;assign(selected,LightEditValuesFrom(result),fields);return selected;
}
LightEditCanonicalPayload raw(const EditableLightEntity& e) {
  LightEditCanonicalPayload r;
  if(e.type==EditableLightType::Area) {r.kind=LightEditCanonicalPayload::Kind::Area;r.area=areaLightDataFromEditable(e);}
  else if(e.type!=EditableLightType::Directional) {r.kind=LightEditCanonicalPayload::Kind::Point;r.point=pointLightDataFromEditable(e);}
  return r;
}
void rawFields(LightEditCanonicalPayload& live,const LightEditCanonicalPayload& next,uint32_t f) {
  if(live.kind!=next.kind) reject("Light canonical payload type changed");
  const auto xyz=[](glm::vec4& a,const glm::vec4& b) {a.x=b.x;a.y=b.y;a.z=b.z;};
  if(live.kind==LightEditCanonicalPayload::Kind::Point) {
    auto& a=live.point;const auto& b=next.point;
    if(has(f,LightEditField::Position)) xyz(a.positionRadius,b.positionRadius);
    if(has(f,LightEditField::Range)) a.positionRadius.w=b.positionRadius.w;
    if(has(f,LightEditField::Color)) xyz(a.colorIntensity,b.colorIntensity);
    if(has(f,LightEditField::Intensity)) a.colorIntensity.w=b.colorIntensity.w;
    if(has(f,LightEditField::Direction)) xyz(a.directionInnerCos,b.directionInnerCos);
    if(has(f,LightEditField::SpotCone)) {a.directionInnerCos.w=b.directionInnerCos.w;a.coneOuterCosType.x=b.coneOuterCosType.x;}
  } else if(live.kind==LightEditCanonicalPayload::Kind::Area) {
    auto& a=live.area;const auto& b=next.area;
    if(has(f,LightEditField::Position)) xyz(a.positionRange,b.positionRange);
    if(has(f,LightEditField::Range)) a.positionRange.w=b.positionRange.w;
    if(has(f,LightEditField::Color)) xyz(a.colorIntensity,b.colorIntensity);
    if(has(f,LightEditField::Intensity)) a.colorIntensity.w=b.colorIntensity.w;
    if(has(f,LightEditField::Direction)||has(f,LightEditField::AreaBasis)) {
      xyz(a.directionType,b.directionType);xyz(a.tangentHalfSize,b.tangentHalfSize);xyz(a.bitangentHalfSize,b.bitangentHalfSize);
    }
    if(has(f,LightEditField::AreaSize)) {a.tangentHalfSize.w=b.tangentHalfSize.w;a.bitangentHalfSize.w=b.bitangentHalfSize.w;}
    if(has(f,LightEditField::AreaShape)) a.directionType.w=b.directionType.w;
  }
}
EditableLightEntity rawView(const LightEditCanonicalPayload& r,const EditableLightEntity& current) {
  if(r.kind==LightEditCanonicalPayload::Kind::None) return current;
  auto e=r.kind==LightEditCanonicalPayload::Kind::Point?editablePointLight(current.source,current.id.index,r.point,current.selected):
      editableAreaLight(current.source,current.id.index,r.area,current.selected);
  e.label=current.label;e.id=current.id;return e;
}
bool value(const EditableLightEntity& a,const EditableLightEntity& b) noexcept {
  return a.id==b.id && a.type==b.type && a.source==b.source && LightEditValuesEqual(LightEditValuesFrom(a),LightEditValuesFrom(b));
}
bool optional(const std::optional<EditableLightEntity>& a,const std::optional<EditableLightEntity>& b) noexcept {
  return bool(a)==bool(b)&&(!a||value(*a,*b));
}
void preflight(const LightEditTarget& target,const EditableLightEntity& displayed,const EditableLightEntity& base,
    const std::optional<EditableLightEntity>& before,const LightEditLimits& limits) {
  if(!target.ownerIdentity||!target.assetIdentity||!target.graphIdentity||!target.incarnation||!target.revision||
      target.id!=displayed.id||target.id.type!=displayed.type||target.id.source!=displayed.source||
      (displayed.type!=EditableLightType::Point&&displayed.type!=EditableLightType::Spot&&displayed.type!=EditableLightType::Directional&&displayed.type!=EditableLightType::Area)||
      (displayed.source!=EditableLightSource::Generated&&displayed.source!=EditableLightSource::Imported&&displayed.source!=EditableLightSource::Manual)||
      !limits.maxPreparedCpuBytes||limits.maxPreparedCpuBytes==UINT64_MAX||!limits.maxCommandBytes||limits.maxCommandBytes==UINT64_MAX||!limits.maxLabelBytes||
      limits.maxLabelBytes>65536||!limits.maxWork||limits.maxWork==UINT64_MAX||
      displayed.label.size()>limits.maxLabelBytes||base.label.size()>limits.maxLabelBytes||
      (before&&before->label.size()>limits.maxLabelBytes)||!finite(LightEditValuesFrom(displayed))||!finite(LightEditValuesFrom(base))||
      (before&&!finite(LightEditValuesFrom(*before)))) reject("Light edit has an invalid target, current payload or finite limit");
}
uint32_t ownedFields(EditableLightType type,uint32_t fields) {
  uint32_t allowed=LightEditFieldBit(LightEditField::Color)|LightEditFieldBit(LightEditField::Intensity);
  if(type==EditableLightType::Directional) allowed|=LightEditFieldBit(LightEditField::Direction);
  else allowed|=LightEditFieldBit(LightEditField::Position)|LightEditFieldBit(LightEditField::Range);
  if(type==EditableLightType::Spot) allowed|=LightEditFieldBit(LightEditField::Direction)|LightEditFieldBit(LightEditField::SpotCone);
  if(type==EditableLightType::Area) {
    allowed|=LightEditFieldBit(LightEditField::Direction)|LightEditFieldBit(LightEditField::AreaBasis)|LightEditFieldBit(LightEditField::AreaSize)|LightEditFieldBit(LightEditField::AreaShape);
    if(fields&(LightEditFieldBit(LightEditField::Direction)|LightEditFieldBit(LightEditField::AreaBasis)))
      fields|=LightEditFieldBit(LightEditField::Direction)|LightEditFieldBit(LightEditField::AreaBasis);
  }
  if(fields&~allowed) reject("The selected light does not support this property or gizmo operation");return fields;
}
}
bool LightEditValuesEqual(const LightEditValues& a,const LightEditValues& b,uint32_t f) noexcept {
  return (!has(f,LightEditField::Position)||vec(a.position,b.position))&&(!has(f,LightEditField::Direction)||vec(a.direction,b.direction))&&
      (!has(f,LightEditField::Color)||vec(a.color,b.color))&&(!has(f,LightEditField::Intensity)||bit(a.intensity,b.intensity))&&
      (!has(f,LightEditField::Range)||bit(a.range,b.range))&&(!has(f,LightEditField::SpotCone)||(bit(a.innerConeDegrees,b.innerConeDegrees)&&bit(a.outerConeDegrees,b.outerConeDegrees)))&&
      (!has(f,LightEditField::AreaSize)||vec(a.areaHalfSize,b.areaHalfSize))&&(!has(f,LightEditField::AreaShape)||bit(a.areaShape,b.areaShape))&&
      (!has(f,LightEditField::AreaBasis)||(vec(a.direction,b.direction)&&vec(a.tangent,b.tangent)&&vec(a.bitangent,b.bitangent)));
}
struct LightEditCommand::State {
  LightEditTarget target;LightEditValues before,after,realized;
  LightEditCanonicalPayload rawBefore,rawAfter;
  uint32_t fields{};bool beforePresent{},afterPresent{},published{},applied{};uint64_t version{},bytes{},editorRevision{};
  std::shared_ptr<const LightEditAddState> addition;
};
struct PreparedLightEditPlan::State {
  LightEditTarget target;EditableLightEntity displayed,base,selected;
  std::optional<EditableLightEntity> before,after;
  LightEditCommand command;std::shared_ptr<LightEditCommand::State> expected;
  LightEditCanonicalPayload rawAfter;
  uint64_t version{},bytes{};bool changed{},consumed{},applied{true};
};
struct LightEditReceiptAccess {
  static auto& state(const LightEditCommand& c) {return c.state_;}
  static auto& state(const PreparedLightEditPlan& p) {return p.state_;}
  static LightEditCommand command() {LightEditCommand c;c.state_=std::make_shared<LightEditCommand::State>();return c;}
  static PreparedLightEditPlan plan() {PreparedLightEditPlan p;p.state_=std::make_shared<PreparedLightEditPlan::State>();return p;}
};
LightEditCommand::operator bool() const noexcept {return bool(state_);}
bool LightEditCommand::published() const noexcept {return state_&&state_->published;}
bool LightEditCommand::applied() const noexcept {return state_&&state_->applied;}
bool LightEditCommand::netChanged() const noexcept {
  return state_&&(state_->addition||state_->beforePresent!=state_->afterPresent||
      (state_->beforePresent&&!LightEditValuesEqual(state_->before,state_->after,state_->fields)));
}
uint32_t LightEditCommand::ownedFields() const noexcept {return state_?state_->fields:0;}
uint64_t LightEditCommand::ownedBytes() const noexcept {return state_?state_->bytes:0;}
LightEditTarget LightEditCommand::target() const noexcept {return state_?state_->target:LightEditTarget{};}
PreparedLightEditPlan::operator bool() const noexcept {return state_&&!state_->consumed;}
bool PreparedLightEditPlan::changed() const noexcept {return state_&&state_->changed;}
uint64_t PreparedLightEditPlan::ownedBytes() const noexcept {return state_?state_->bytes:0;}
LightEditCommand PreparedLightEditPlan::command() const noexcept {return state_?state_->command:LightEditCommand{};}
const std::optional<EditableLightEntity>& PreparedLightEditPlan::overrideAfter() const {if(!*this) reject("Light edit plan is empty");return state_->after;}
const EditableLightEntity& PreparedLightEditPlan::selectedAfter() const {if(!*this) reject("Light edit plan is empty");return state_->selected;}
const LightEditCanonicalPayload& PreparedLightEditPlan::canonicalAfter() const {if(!*this) reject("Light edit plan is empty");return state_->rawAfter;}
namespace {
PreparedLightEditPlan capture(const LightEditTarget& target,const EditableLightEntity& displayed,const EditableLightEntity& base,
    const std::optional<EditableLightEntity>& before,const LightEditLimits& limits) {
  preflight(target,displayed,base,before,limits);
  const uint64_t bytes=2048+6*sizeof(EditableLightEntity)+6*(limits.maxLabelBytes+1);
  if(bytes>limits.maxCommandBytes||bytes>limits.maxPreparedCpuBytes) reject("Selected light receipt exceeds its finite storage budget");
  auto p=LightEditReceiptAccess::plan();auto& s=*LightEditReceiptAccess::state(p);
  s.target=target;s.displayed=displayed;s.base=base;s.before=before;s.bytes=bytes;return p;
}
}
PreparedLightEditPlan PlanLightEdit(const LightEditTarget& target,const EditableLightEntity& displayed,const EditableLightEntity& base,
    const std::optional<EditableLightEntity>& before,const LightEditPatch& patch,const LightEditCommand* gesture,const LightEditLimits& limits,const LightEditCanonicalPayload* currentRaw) {
  const auto fields=ownedFields(displayed.type,patch.fields);
  if(!finite(patch.values)) reject("Light patch contains a nonfinite value");
  auto p=capture(target,displayed,base,before,limits);auto& s=*LightEditReceiptAccess::state(p);
  const auto beforeRaw=currentRaw?*currentRaw:raw(displayed);
  auto edited=displayed;assign(edited,patch.values,fields);s.selected=canonical(edited,fields);
  s.rawAfter=beforeRaw;rawFields(s.rawAfter,raw(edited),fields);s.selected=rawView(s.rawAfter,s.selected);
  s.changed=!LightEditValuesEqual(LightEditValuesFrom(displayed),LightEditValuesFrom(s.selected));
  s.after=s.changed?std::optional<EditableLightEntity>(s.selected):before;
  s.command=LightEditReceiptAccess::command();auto& c=*LightEditReceiptAccess::state(s.command);
  c.target=target;c.before=LightEditValuesFrom(before?*before:base);c.after=LightEditValuesFrom(s.selected);
  c.fields=fields;c.beforePresent=bool(before);c.bytes=512+sizeof(c);c.realized=c.after;
  c.rawBefore=beforeRaw;c.rawAfter=s.rawAfter;
  if(gesture) {
    const auto& old=LightEditReceiptAccess::state(*gesture);
    if(!old||!old->published||!old->applied||!SameLightEditIdentity(target,old->target)||
        !before||!LightEditValuesEqual(LightEditValuesFrom(*before),old->realized)) reject("Light gesture no longer owns its last preview");
    c.before=old->before;c.rawBefore=old->rawBefore;c.beforePresent=old->beforePresent;c.fields|=old->fields;
    s.expected=old;s.version=old->version;
    if(!c.beforePresent&&s.after&&LightEditValuesEqual(LightEditValuesFrom(*s.after),LightEditValuesFrom(base))) {
      s.after.reset();s.selected=base;s.changed=!optional(s.before,s.after);
    }
  }
  c.afterPresent=bool(s.after);
  return p;
}
PreparedLightEditPlan RebaseLightEdit(const LightEditTarget& target,const EditableLightEntity& displayed,const EditableLightEntity& base,
    const std::optional<EditableLightEntity>& before,const LightEditCommand& command,bool applied,const LightEditLimits& limits,const LightEditCanonicalPayload* currentRaw) {
  const auto& c=LightEditReceiptAccess::state(command);
  if(!c||!c->published||c->applied==applied||!SameLightEditIdentity(target,c->target)) reject("Light history is stale, foreign or on the wrong side");
  auto p=capture(target,displayed,base,before,limits);auto& s=*LightEditReceiptAccess::state(p);
  const auto& expected=applied?c->before:c->after;const auto& desired=applied?c->after:c->before;
  const bool expectedPresence=applied?c->beforePresent:c->afterPresent;
  if(bool(before)!=expectedPresence||(before&&!LightEditValuesEqual(LightEditValuesFrom(*before),expected,c->fields)))
    reject("Light history owned endpoint or override presence changed");
  s.command=command;s.expected=c;s.version=c->version;s.applied=applied;
  const bool desiredPresence=applied?c->afterPresent:c->beforePresent;
  if(!desiredPresence) {
    if(before&&!LightEditValuesEqual(LightEditValuesFrom(*before),c->realized)) reject("Cannot remove a light override changed by another owner");
    s.after.reset();s.selected=base;
  } else {
    auto e=before?*before:base;assign(e,desired,c->fields);s.selected=canonical(e,c->fields);
    s.rawAfter=currentRaw?*currentRaw:raw(before?*before:base);rawFields(s.rawAfter,applied?c->rawAfter:c->rawBefore,c->fields);
    s.selected=rawView(s.rawAfter,s.selected);s.after=s.selected;
  }
  s.changed=!optional(s.before,s.after);return p;
}
bool LightEditPlanMatches(const PreparedLightEditPlan& p,const LightEditTarget& target,const EditableLightEntity& displayed,
    const EditableLightEntity& base,const std::optional<EditableLightEntity>& before) noexcept {
  const auto& ptr=LightEditReceiptAccess::state(p);if(!ptr||ptr->consumed) return false;const auto& s=*ptr;
  return s.target==target&&value(s.displayed,displayed)&&value(s.base,base)&&optional(s.before,before)&&
      (!s.expected||(s.expected->published&&s.expected->version==s.version&&s.expected->version!=UINT64_MAX));
}
bool PublishLightEditPlan(PreparedLightEditPlan& p,uint64_t editorRevision) noexcept {
  auto& ptr=LightEditReceiptAccess::state(p);if(!ptr||ptr->consumed||!ptr->command) return false;auto& s=*ptr;
  if(s.expected&&(!s.expected->published||s.expected->version!=s.version||s.expected->version==UINT64_MAX)) return false;
  auto& c=*LightEditReceiptAccess::state(s.command);if(c.version==UINT64_MAX) return false;
  c.realized=LightEditValuesFrom(s.after?*s.after:s.base);c.published=true;c.applied=s.applied;c.editorRevision=editorRevision;++c.version;s.consumed=true;return true;
}
PreparedLightEditPlan PlanLightEditStructure(const LightEditTarget& target,const std::optional<EditableLightEntity>& before,
    const std::optional<EditableLightEntity>& after,const EditableLightEntity& selected,const LightEditCommand* original,bool applied,
    std::shared_ptr<const LightEditAddState> authority,const LightEditLimits& limits) {
  if(!authority) reject("Light creation requires its original owned capability");
  auto proxy=selected;proxy.id=target.id;proxy.type=target.id.type;proxy.source=target.id.source;
  auto p=capture(target,proxy,proxy,before,limits);auto& s=*LightEditReceiptAccess::state(p);
  s.after=after;s.selected=selected;s.changed=true;s.applied=applied;
  if(original) {
    const auto& c=LightEditReceiptAccess::state(*original);
    if(!c||!c->published||c->applied==applied||c->addition!=authority) reject("Light creation history is stale or on the wrong side");
    s.command=*original;s.expected=c;s.version=c->version;
  } else {
    s.command=LightEditReceiptAccess::command();auto& c=*LightEditReceiptAccess::state(s.command);
    c.target=target;c.beforePresent=bool(before);c.afterPresent=bool(after);c.before=LightEditValuesFrom(before?*before:selected);
    c.after=LightEditValuesFrom(after?*after:selected);c.fields=kAllLightEditFields;c.bytes=limits.maxLabelBytes*2+2048;
    c.addition=std::move(authority);
  }
  return p;
}
std::shared_ptr<const LightEditAddState> LightEditAddition(const LightEditCommand& c) noexcept {
  const auto& state=LightEditReceiptAccess::state(c);return state?state->addition:std::shared_ptr<const LightEditAddState>{};
}
bool LightEditCommandPreviewMatches(const LightEditCommand& command,const LightEditTarget& target,
    const std::optional<LightEditValues>& current) noexcept {
  const auto& c=LightEditReceiptAccess::state(command);
  if(!c||!c->published||!c->applied||!SameLightEditIdentity(c->target,target)||bool(current)!=c->afterPresent||
      (c->editorRevision&&c->editorRevision!=target.revision)) return false;
  if(!current) return true;
  uint32_t mask=LightEditFieldBit(LightEditField::Color)|LightEditFieldBit(LightEditField::Intensity);
  if(target.id.type==EditableLightType::Directional) mask|=LightEditFieldBit(LightEditField::Direction);
  else mask|=LightEditFieldBit(LightEditField::Position)|LightEditFieldBit(LightEditField::Range);
  if(target.id.type==EditableLightType::Spot) mask|=LightEditFieldBit(LightEditField::Direction)|LightEditFieldBit(LightEditField::SpotCone);
  if(target.id.type==EditableLightType::Area) mask|=LightEditFieldBit(LightEditField::Direction)|LightEditFieldBit(LightEditField::AreaBasis)|
      LightEditFieldBit(LightEditField::AreaSize)|LightEditFieldBit(LightEditField::AreaShape);
  return LightEditValuesEqual(*current,c->realized,mask);
}
} // namespace container::renderer
