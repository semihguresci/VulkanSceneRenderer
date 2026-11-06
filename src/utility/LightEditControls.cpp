#include "Container/utility/LightEditControls.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace container::ui {
namespace {
using namespace container::renderer;

bool finite(const LightEditValues& v) noexcept {
  const auto vector=[](const auto& values) {
    for(int i=0;i<values.length();++i) if(!std::isfinite(values[i])) return false;
    return true;
  };
  return vector(v.position)&&vector(v.direction)&&vector(v.tangent)&&vector(v.bitangent)&&
      vector(v.color)&&vector(v.areaHalfSize)&&std::isfinite(v.intensity)&&std::isfinite(v.range)&&
      std::isfinite(v.innerConeDegrees)&&std::isfinite(v.outerConeDegrees)&&std::isfinite(v.areaShape);
}
bool validType(EditableLightType type) noexcept {
  return type==EditableLightType::Directional || type==EditableLightType::Point ||
      type==EditableLightType::Spot || type==EditableLightType::Area;
}
void retainField(LightEditValues& next,const LightEditValues& old,uint32_t field) noexcept {
  if(field&LightEditFieldBit(LightEditField::Position)) next.position=old.position;
  if(field&LightEditFieldBit(LightEditField::Direction)) next.direction=old.direction;
  if(field&LightEditFieldBit(LightEditField::Color)) next.color=old.color;
  if(field&LightEditFieldBit(LightEditField::Intensity)) next.intensity=old.intensity;
  if(field&LightEditFieldBit(LightEditField::Range)) next.range=old.range;
  if(field&LightEditFieldBit(LightEditField::SpotCone)) {
    next.innerConeDegrees=old.innerConeDegrees;next.outerConeDegrees=old.outerConeDegrees;
  }
  if(field&LightEditFieldBit(LightEditField::AreaSize)) next.areaHalfSize=old.areaHalfSize;
  if(field&LightEditFieldBit(LightEditField::AreaShape)) next.areaShape=old.areaShape;
  if(field&LightEditFieldBit(LightEditField::AreaBasis)) {next.tangent=old.tangent;next.bitangent=old.bitangent;}
}
} // namespace

void LightEditControls::setSnapshot(LightEditSnapshot snapshot) {
  if(snapshot.light.label.size()>maxDisplayBytes || snapshot.reason.size()>maxDisplayBytes || snapshot.error.size()>maxDisplayBytes)
    throw std::invalid_argument("Light inspector text exceeds its display budget");
  if(snapshot.available && (!snapshot.target.ownerIdentity || !snapshot.target.assetIdentity ||
      !snapshot.target.graphIdentity || !snapshot.target.incarnation || !snapshot.target.revision ||
      !isValidEditableLightId(snapshot.target.id) || snapshot.target.id!=snapshot.light.id ||
      snapshot.light.type!=snapshot.target.id.type || snapshot.light.source!=snapshot.target.id.source ||
      !validType(snapshot.light.type) ||
      (snapshot.light.source!=EditableLightSource::Generated && snapshot.light.source!=EditableLightSource::Imported &&
       snapshot.light.source!=EditableLightSource::Manual) || !finite(LightEditValuesFrom(snapshot.light))))
    throw std::invalid_argument("Light inspector snapshot is invalid");
  const bool same=snapshot.available && snapshot_.available && SameLightEditIdentity(snapshot.target,snapshot_.target);
  auto next=LightEditValuesFrom(snapshot.light);
  if(same && active_) retainField(next,draft_,active_->field);
  if(!same) {active_.reset();requests_.clear();add_.reset();error_.clear();}
  snapshot_=std::move(snapshot);
  draft_=next;
  if(active_) active_->target=snapshot_.target;
}

void LightEditControls::queue(LightEditRequest request) {
  const bool standalone=request.phase==LightEditPhase::Commit;
  const bool patchRequired=request.phase==LightEditPhase::Begin || request.phase==LightEditPhase::Update || standalone;
  const bool known=standalone || request.phase==LightEditPhase::Begin || request.phase==LightEditPhase::Update ||
      request.phase==LightEditPhase::End || request.phase==LightEditPhase::Cancel;
  if(!snapshot_.available || (!snapshot_.light.editable && patchRequired) || request.target!=snapshot_.target || !known ||
      (standalone ? request.gesture!=0 : request.gesture==0) ||
      (patchRequired && !request.patch.fields) || (request.patch.fields&~kAllLightEditFields) ||
      (request.phase==LightEditPhase::Cancel && request.patch.fields) || !finite(request.patch.values))
    throw std::invalid_argument("Light edit no longer matches the current inspector");
  if(requests_.size()>=maxPendingRequests) throw std::length_error("Light edit request batch is full");
  if(requests_.capacity()<maxPendingRequests) requests_.reserve(maxPendingRequests);
  requests_.push_back(request);
}

std::vector<LightEditRequest> LightEditControls::consume() {
  auto result=requests_; // Preserve pending intent if allocation fails.
  requests_.clear();
  return result;
}
void LightEditControls::queueAdd(EditableLightType type) {
  if(!validType(type)) throw std::invalid_argument("Unknown manual light type");
  if(add_) throw std::invalid_argument("A manual light Add is already pending");
  finishGesture();
  add_=type;
}
std::optional<EditableLightType> LightEditControls::consumeAdd() noexcept {return std::exchange(add_,{});}

void LightEditControls::finishGesture(bool cancel) {
  if(!active_) return;
  if(active_->started) queue({active_->target,cancel?LightEditPhase::Cancel:LightEditPhase::End,active_->id,{}});
  active_.reset();
  draft_=LightEditValuesFrom(snapshot_.light);
  if(cancel) error_.clear();
}
void LightEditControls::processInput() {
  if(!ImGui::GetCurrentContext() || !active_) return;
  if(ImGui::GetIO().AppFocusLost || ImGui::IsKeyPressed(ImGuiKey_Escape,false)) {
    finishGesture(true);
    cancelledFrame_=ImGui::GetFrameCount();
  }
}
void LightEditControls::track(uint32_t item,LightEditField field,bool changed,bool activated,bool active) {
  if(cancelledFrame_==ImGui::GetFrameCount()) return;
  if((activated || changed) && (!active_ || active_->item!=item)) {
    const auto edited=draft_;
    finishGesture();
    draft_=edited;
    if(nextGesture_==std::numeric_limits<uint64_t>::max()) throw std::overflow_error("Light gesture identity exhausted");
    active_=Gesture{snapshot_.target,++nextGesture_,item,LightEditFieldBit(field),false};
  }
  if(active_ && active_->item==item) {
    if(changed) {
      queue({active_->target,active_->started?LightEditPhase::Update:LightEditPhase::Begin,
          active_->id,{LightEditFieldBit(field),draft_}});
      active_->started=true;
      error_.clear();
    }
    if(!active) finishGesture();
  }
}

void LightEditControls::drawAddButtons() {
  constexpr EditableLightType types[]{EditableLightType::Directional,EditableLightType::Point,EditableLightType::Spot,EditableLightType::Area};
  constexpr const char* labels[]{"Add directional","Add point","Add spot","Add area"};
  for(int i=0;i<4;++i) {
    if(i) ImGui::SameLine();
    if(ImGui::SmallButton(labels[i])) {
      try {queueAdd(types[i]);} catch(const std::exception& e) {error_=e.what();}
    }
  }
}

void LightEditControls::draw() {
  processInput();
  if(!snapshot_.available) {
    ImGui::TextDisabled("%s",snapshot_.reason.empty()?"Select an editable light":snapshot_.reason.c_str());
    if(!error_.empty()) ImGui::TextWrapped("%s",error_.c_str());
    if(!snapshot_.error.empty()) ImGui::TextWrapped("%s",snapshot_.error.c_str());
    return;
  }
  ImGui::TextUnformatted("Selected Light Properties");
  ImGui::Text("Source: %s",editableLightSourceLabel(snapshot_.light.source));
  ImGui::Text("Type: %s",editableLightTypeLabel(snapshot_.light.type));
  if(snapshot_.light.source==EditableLightSource::Imported)
    ImGui::TextDisabled("Renderer override; animation playback is unchanged");
  if(snapshot_.light.source==EditableLightSource::Generated)
    ImGui::TextDisabled("Generated light; generator changes may replace its edits");
  ImGui::TextDisabled("Ctrl+click a value to type; Esc cancels the current edit");
  const auto observe=[&](uint32_t id,LightEditField field,bool changed,bool extraActive=false) {
    try {track(id,field,changed,ImGui::IsItemActivated(),ImGui::IsItemActive()||extraActive);}
    catch(const std::exception& e) {error_=e.what();}
  };
  const auto scalar=[&](const char* label,float& value,LightEditField field,float speed,float minimum=0,float maximum=0) {
    const auto id=ImGui::GetID(label);
    const bool changed=ImGui::DragFloat(label,&value,speed,minimum,maximum,"%.9g");
    observe(id,field,changed);
  };
  const auto vector=[&](const char* label,glm::vec3& values,LightEditField field,float speed) {
    ImGui::TextUnformatted(label);ImGui::PushID(label);
    const float width=std::max(20.f,(ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ItemSpacing.x*2)/3);
    for(int i=0;i<3;++i) {
      if(i) ImGui::SameLine();ImGui::SetNextItemWidth(width);
      scalar(i==0?"##X":i==1?"##Y":"##Z",values[i],field,speed);
    }
    ImGui::PopID();
  };
  const auto section=[](const char* label) {ImGui::Spacing();ImGui::Separator();ImGui::TextUnformatted(label);};
  ImGui::BeginDisabled(!snapshot_.light.editable);
  section("Light Transform");
  if(snapshot_.light.type!=EditableLightType::Directional) {
    vector("Position",draft_.position,LightEditField::Position,.05f);
    scalar("Range",draft_.range,LightEditField::Range,.05f,0,100000);
  }
  if(snapshot_.light.type!=EditableLightType::Point)
    vector("Direction",draft_.direction,LightEditField::Direction,.01f);
  section("Light Radiance");
  const auto colorId=ImGui::GetID("Color picker");
  const bool opened=ImGui::ColorButton("Color preview",ImVec4(draft_.color.r,draft_.color.g,draft_.color.b,1),ImGuiColorEditFlags_NoAlpha);
  if(opened) ImGui::OpenPopup("Light color picker");
  ImGui::SameLine();ImGui::TextUnformatted("Color");
  bool colorChanged=false;
  if(ImGui::BeginPopup("Light color picker")) {
    ImGui::SetNextItemWidth(220.f);
    colorChanged=ImGui::ColorPicker3("##Picker",&draft_.color.x,
        ImGuiColorEditFlags_Float|ImGuiColorEditFlags_NoInputs|ImGuiColorEditFlags_NoAlpha|ImGuiColorEditFlags_NoOptions);
    ImGui::EndPopup();
  }
  try {track(colorId,LightEditField::Color,colorChanged,opened,ImGui::IsPopupOpen("Light color picker"));}
  catch(const std::exception& e) {error_=e.what();}
  // ColorEdit's internal float inputs use three decimals. Keep precision-safe
  // numeric RGB entry in the same scalar adapter as the other light fields.
  vector("Color RGB",draft_.color,LightEditField::Color,.005f);
  scalar("Intensity",draft_.intensity,LightEditField::Intensity,.05f,0,100000);
  if(snapshot_.light.type==EditableLightType::Spot) {
    section("Spot Beam");
    scalar("Inner cone (degrees)",draft_.innerConeDegrees,LightEditField::SpotCone,.1f,0,179);
    scalar("Outer cone (degrees)",draft_.outerConeDegrees,LightEditField::SpotCone,.1f,0,179);
  }
  if(snapshot_.light.type==EditableLightType::Area) {
    section("Area Emitter");
    int shape=draft_.areaShape>=container::gpu::kAreaLightTypeDisk-.5f?1:0;
    constexpr const char* shapes[]{"Rectangle","Disk"};
    if(ImGui::Combo("Area shape",&shape,shapes,2) && cancelledFrame_!=ImGui::GetFrameCount()) {
      try {
        finishGesture();draft_.areaShape=shape?container::gpu::kAreaLightTypeDisk:container::gpu::kAreaLightTypeRectangle;
        queue({snapshot_.target,LightEditPhase::Commit,0,{LightEditFieldBit(LightEditField::AreaShape),draft_}});
      } catch(const std::exception& e) {error_=e.what();}
    }
    float width=draft_.areaHalfSize.x*2,height=draft_.areaHalfSize.y*2;
    const auto areaDimension=[&](const char* label,float& value,int component) {
      const auto id=ImGui::GetID(label);
      const bool changed=ImGui::DragFloat(label,&value,.05f,.002f,200000,"%.9g");
      if(changed) draft_.areaHalfSize[component]=std::max(value*.5f,.001f);
      observe(id,LightEditField::AreaSize,changed);
    };
    areaDimension("Area Width",width,0);areaDimension("Area Height",height,1);
    vector("Area Tangent",draft_.tangent,LightEditField::AreaBasis,.01f);
    vector("Area Bitangent",draft_.bitangent,LightEditField::AreaBasis,.01f);
  }
  ImGui::EndDisabled();
  section("Shadow Eligibility");
  if(snapshot_.light.type==EditableLightType::Directional)
    ImGui::TextUnformatted("Uses directional cascaded shadows when enabled.");
  else if(snapshot_.light.type==EditableLightType::Area)
    ImGui::TextUnformatted("Uses the renderer local-shadow assignment.");
  else {
    ImGui::Text("Eligible for local shadows: %s",draft_.intensity>0&&draft_.range>0?"yes":"no");
    ImGui::TextWrapped("The local shadow budget assigns maps to the first eligible point and spot lights.");
  }
  if(!snapshot_.reason.empty()) ImGui::TextWrapped("%s",snapshot_.reason.c_str());
  if(!error_.empty()) ImGui::TextWrapped("%s",error_.c_str());
  if(!snapshot_.error.empty()) ImGui::TextWrapped("%s",snapshot_.error.c_str());
}

} // namespace container::ui
