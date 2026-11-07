#include <Container/utility/GltfAnimationControls.h>

#include <imgui.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

namespace container::ui {
namespace {
using Projection=container::geometry::gltf::GltfCameraProjection;
using Property=container::geometry::gltf::GltfCameraProperty;
using Patch=container::geometry::gltf::GltfCameraProjectionPatch;
using Target=container::scene::CameraProjectionTarget;
using PoseTarget=container::scene::CameraViewPoseTarget;
constexpr size_t propertyCount=size_t(Property::Count);
constexpr size_t maxReasonBytes=512;
bool exact(double a,double b) noexcept {return std::bit_cast<uint64_t>(a)==std::bit_cast<uint64_t>(b);}
bool validTarget(const Target& t) noexcept {
  return t.asset && t.sourceNamespace && t.graphIdentity && t.incarnation && t.graphNode!=UINT32_MAX &&
      t.sourceNode>=0 && t.authoredCamera>=0 && t.evaluatedCamera>=0;
}
bool sameDraftIdentity(const Target& a,const Target& b) noexcept {
  auto latest=a;latest.graphRevision=b.graphRevision;return latest==b;
}
bool validType(Projection type) noexcept {return type==Projection::Perspective || type==Projection::Orthographic;}
bool activeProperty(Projection type,size_t property) noexcept {
  return property<propertyCount && ((property<4)==(type==Projection::Perspective));
}
bool validLens(Projection type,const std::array<double,propertyCount>& v) noexcept {
  if(!validType(type)) return false;
  for(const auto value:v) if(!std::isfinite(value)) return false;
  const auto positive=[](double n) {return n>0 && std::isfinite(float(n)) && float(n)>0;};
  if(type==Projection::Perspective) {
    return positive(v[0]) && v[0]<std::numbers::pi && float(v[0])<std::numbers::pi_v<float> &&
        (v[1]==0 || positive(v[1])) && positive(v[2]) &&
        (v[3]==0 || (positive(v[3]) && v[3]>v[2] && float(v[3])>float(v[2])));
  }
  return positive(v[4]) && positive(v[5]) && v[6]>=0 && std::isfinite(float(v[6])) &&
      positive(v[7]) && v[7]>v[6] && float(v[7])>float(v[6]);
}
bool validSnapshot(const CameraProjectionSnapshot& s) noexcept {
  if(!validTarget(s.target) || !validLens(s.type,s.values) || !s.sharedDefinitionUses || s.sharedDefinitionUses>1'000'000 ||
      s.typeSwitchBlockReason.size()>maxReasonBytes) return false;
  for(const auto& reason:s.propertyBlockReasons) if(reason.size()>maxReasonBytes) return false;
  return true;
}
bool validSnapshot(const CameraViewPoseSnapshot& s) noexcept {
  return validTarget(s.target.camera) && s.target.nodeIncarnation && s.viewRevision &&
      s.destinationName.size()<=4096 && s.blockReason.size()<=maxReasonBytes;
}
bool conflictingRequest(const GltfAnimationControls& c) noexcept {
  return c.requestedCameraNode || c.requestedClip || c.requestedTime || c.requestedSpeed || c.requestedPlaying || c.requestedLoop ||
      c.requestedCameraViewPose;
}
bool poseCanApply(const GltfAnimationControls& c) noexcept {
  return c.cameraViewPose && validSnapshot(*c.cameraViewPose) && c.cameraViewPose->viewportActive && c.cameraNode<0 &&
      c.cameraViewPose->blockReason.empty() && !c.requestedCameraProjection && !conflictingRequest(c);
}
bool patchMatches(const CameraProjectionSnapshot& s,const Patch& patch) noexcept {
  if(patch.values.size()>propertyCount || (!patch.type && patch.values.empty())) return false;
  const auto type=patch.type.value_or(s.type);if(!validType(type)) return false;
  const bool switched=type!=s.type;
  if(switched && !s.typeSwitchBlockReason.empty()) return false;
  std::array<bool,propertyCount> seen{};auto values=s.values;bool changed=switched;
  for(const auto& edit:patch.values) {
    const size_t index=size_t(edit.property);
    if(!activeProperty(type,index) || seen[index]) return false;seen[index]=true;
    if(const auto* number=std::get_if<double>(&edit.value)) {
      if(!std::isfinite(*number) || ((index==1 || index==3) && *number<=0)) return false;
      values[index]=*number;
    } else {
      if((index!=1 && index!=3) || s.animatedProperties[index]) return false;values[index]=0;
    }
    if(!exact(values[index],s.values[index])) {
      if(!s.propertyBlockReasons[index].empty()) return false;changed=true;
    }
  }
  if(switched && (type==Projection::Perspective?(!seen[0] || !seen[2]):(!seen[4] || !seen[5] || !seen[6] || !seen[7]))) return false;
  return changed && validLens(type,values);
}
Patch draftPatch(const CameraProjectionSnapshot& s,const CameraProjectionDraft& draft) {
  Patch patch;if(draft.typeEdited && draft.type!=s.type) patch.type=draft.type;
  patch.values.reserve(4);
  for(size_t i=0;i<propertyCount;++i) if(activeProperty(draft.type,i) && draft.edited[i] &&
      (patch.type || !exact(draft.values[i],s.values[i]))) {
    if((i==1 || i==3) && draft.values[i]==0) patch.values.push_back({Property(i),std::monostate{}});
    else patch.values.push_back({Property(i),draft.values[i]});
  }
  return patch;
}
bool draftCanApply(const GltfAnimationControls& c) noexcept {
  if(!c.cameraProjection || c.requestedCameraProjection || conflictingRequest(c)) return false;
  const auto& d=c.cameraProjectionDraft;const auto& s=*c.cameraProjection;
  if(d.target!=s.target || !validLens(d.type,d.values)) return false;
  for(size_t i : {size_t{1},size_t{3}})
    if(d.edited[i] && d.values[i]==0 && s.animatedProperties[i]) return false;
  if(d.type!=s.type) return d.typeEdited && s.typeSwitchBlockReason.empty();
  for(size_t i=0;i<propertyCount;++i) if(activeProperty(d.type,i) && d.edited[i] &&
      !exact(d.values[i],s.values[i]) && s.propertyBlockReasons[i].empty()) return true;
  return false;
}
void selectDraftType(CameraProjectionDraft& draft,Projection type) noexcept {
  draft.type=type;draft.typeEdited=true;draft.edited.fill(false);
  const auto positive=[](double value) {return value>0 && std::isfinite(float(value)) && float(value)>0;};
  if(type==Projection::Perspective) {
    if(!positive(draft.values[0]) || draft.values[0]>=std::numbers::pi || float(draft.values[0])>=std::numbers::pi_v<float>)
      draft.values[0]=std::numbers::pi/3;
    if(draft.values[1]!=0 && !positive(draft.values[1])) draft.values[1]=0;
    if(!positive(draft.values[2])) draft.values[2]=.1;
    if(draft.values[3]!=0 && (!positive(draft.values[3]) || float(draft.values[3])<=float(draft.values[2]))) draft.values[3]=0;
    for(size_t i=0;i<4;++i) draft.edited[i]=true;
  } else {
    if(!positive(draft.values[4])) draft.values[4]=5;
    if(!positive(draft.values[5])) draft.values[5]=5;
    if(draft.values[6]<0 || !std::isfinite(float(draft.values[6]))) draft.values[6]=0;
    if(!positive(draft.values[7]) || float(draft.values[7])<=float(draft.values[6])) {
      draft.values[7]=std::max(1000.,draft.values[6]*2);
      if(!positive(draft.values[7]) || float(draft.values[7])<=float(draft.values[6])) {draft.values[6]=0;draft.values[7]=1000.;}
    }
    for(size_t i=4;i<propertyCount;++i) draft.edited[i]=true;
  }
}
} // namespace

bool SetCameraProjectionSnapshot(GltfAnimationControls& c,const std::optional<CameraProjectionSnapshot>& snapshot) {
  if(snapshot && !validSnapshot(*snapshot)) return false;
  const bool same=snapshot && c.cameraProjection && sameDraftIdentity(snapshot->target,c.cameraProjection->target) &&
      sameDraftIdentity(snapshot->target,c.cameraProjectionDraft.target);
  // All owned string copies finish before the old snapshot/draft/request change.
  auto next=snapshot;auto draft=c.cameraProjectionDraft;
  if(snapshot) {
    if(!same) {draft={};draft.type=snapshot->type;draft.values=snapshot->values;}
    else {
      if(!draft.typeEdited) draft.type=snapshot->type;
      for(size_t i=0;i<propertyCount;++i) if(!draft.edited[i]) draft.values[i]=snapshot->values[i];
    }
    draft.target=snapshot->target;
  } else draft={};
  c.cameraProjection.swap(next);c.cameraProjectionDraft=draft;
  if(!same) {c.requestedCameraProjection.reset();c.cameraProjectionError.clear();}
  return true;
}
bool SetCameraProjectionDraft(GltfAnimationControls& c,const CameraProjectionDraft& draft) noexcept {
  if(!c.cameraProjection || c.requestedCameraProjection || c.requestedCameraViewPose || draft.target!=c.cameraProjection->target ||
      !validLens(draft.type,draft.values) || (draft.type!=c.cameraProjection->type && !draft.typeEdited)) return false;
  if(draft.type!=c.cameraProjection->type && !c.cameraProjection->typeSwitchBlockReason.empty()) return false;
  if(draft.type!=c.cameraProjection->type && (draft.type==Projection::Perspective?
      (!draft.edited[0] || !draft.edited[2]):(!draft.edited[4] || !draft.edited[5] || !draft.edited[6] || !draft.edited[7]))) return false;
  for(size_t i=0;i<propertyCount;++i) if(draft.edited[i] &&
      (!activeProperty(draft.type,i) || (!exact(draft.values[i],c.cameraProjection->values[i]) &&
          !c.cameraProjection->propertyBlockReasons[i].empty()) ||
       ((i==1 || i==3) && draft.values[i]==0 && c.cameraProjection->animatedProperties[i]))) return false;
  c.cameraProjectionDraft=draft;return true;
}
bool QueueCameraProjectionEdit(GltfAnimationControls& c,const Target& target,const Patch& patch) {
  if(!c.cameraProjection || c.requestedCameraProjection || conflictingRequest(c) ||
      target!=c.cameraProjection->target || !validSnapshot(*c.cameraProjection) || !patchMatches(*c.cameraProjection,patch)) return false;
  CameraProjectionEditRequest request{target,patch};
  c.requestedCameraProjection.emplace(std::move(request));return true;
}
bool QueueCameraProjectionDraft(GltfAnimationControls& c) {
  if(!c.cameraProjection || !draftCanApply(c)) return false;
  const auto patch=draftPatch(*c.cameraProjection,c.cameraProjectionDraft);
  return QueueCameraProjectionEdit(c,c.cameraProjectionDraft.target,patch);
}

bool SetCameraViewPoseSnapshot(GltfAnimationControls& c,const std::optional<CameraViewPoseSnapshot>& snapshot) {
  if(snapshot && !validSnapshot(*snapshot)) return false;
  // Finish both bounded string copies before changing any displayed or queued
  // state. Numeric requests retain their original target/view intent on refresh.
  auto next=snapshot;
  c.cameraViewPose.swap(next);
  if(!snapshot) {c.requestedCameraViewPose.reset();c.cameraViewPoseError.clear();}
  return true;
}
bool QueueCameraViewPose(GltfAnimationControls& c,const PoseTarget& target,uint64_t viewRevision) noexcept {
  if(!poseCanApply(c) || target!=c.cameraViewPose->target || viewRevision!=c.cameraViewPose->viewRevision) return false;
  c.requestedCameraViewPose.emplace(CameraViewPoseRequest{target,viewRevision});return true;
}

void DrawCameraViewPoseControls(GltfAnimationControls& c) {
  ImGui::Separator();ImGui::TextUnformatted("Camera pose");
  const auto* snapshot=c.cameraViewPose?&*c.cameraViewPose:nullptr;
  if(snapshot) {
    if(snapshot->destinationName.empty()) ImGui::Text("Destination: camera node %d",snapshot->target.camera.sourceNode);
    else ImGui::TextWrapped("Destination: %s",snapshot->destinationName.c_str());
    ImGui::TextWrapped("Writes the node's default pose and keeps its lens. Active animation still controls targeted pose components; clear or switch the clip to see their edited defaults.");
  }
  const char* reason=nullptr;
  if(!snapshot) reason="Choose an authored camera, then switch to Viewport camera to write its pose.";
  else if(!snapshot->blockReason.empty()) reason=snapshot->blockReason.c_str();
  else if(!snapshot->viewportActive || c.cameraNode>=0) reason="Switch to Viewport camera before writing its pose.";
  else if(c.requestedCameraProjection || conflictingRequest(c)) reason="Apply the pending camera or playback action first.";
  if(reason) ImGui::TextWrapped("%s",reason);
  if(!c.cameraViewPoseError.empty()) ImGui::TextWrapped("%s",c.cameraViewPoseError.c_str());
  ImGui::BeginDisabled(!poseCanApply(c));
  if(ImGui::Button("Write viewport pose as default") && snapshot) {
    if(QueueCameraViewPose(c,snapshot->target,snapshot->viewRevision)) c.cameraViewPoseError.clear();
    else c.cameraViewPoseError="The destination or viewport changed. Refresh the camera selection.";
  }
  ImGui::EndDisabled();
}

void DrawCameraProjectionControls(GltfAnimationControls& c) {
  if(!c.cameraProjection) {
    if(!c.cameraProjectionError.empty()) ImGui::TextWrapped("%s",c.cameraProjectionError.c_str());
    return;
  }
  const auto& snapshot=*c.cameraProjection;
  ImGui::Separator();ImGui::TextUnformatted("Camera projection");
  ImGui::Text("Definition used by %u source node%s",snapshot.sharedDefinitionUses,snapshot.sharedDefinitionUses==1?"":"s");
  if(std::ranges::any_of(snapshot.animatedProperties,[](bool value) {return value;}))
    ImGui::TextWrapped("Lens edits change authored defaults. Active animation keeps controlling its lens fields.");
  if(!c.cameraProjectionError.empty()) ImGui::TextWrapped("%s",c.cameraProjectionError.c_str());
  const bool pending=bool(c.requestedCameraProjection);auto draft=c.cameraProjectionDraft;
  ImGui::BeginDisabled(pending || conflictingRequest(c));
  ImGui::BeginDisabled(!snapshot.typeSwitchBlockReason.empty());
  ImGui::SetNextItemWidth(160);
  if(ImGui::BeginCombo("Projection",draft.type==Projection::Perspective?"Perspective":"Orthographic")) {
    if(ImGui::Selectable("Perspective",draft.type==Projection::Perspective)) {
      selectDraftType(draft,Projection::Perspective);(void)SetCameraProjectionDraft(c,draft);
    }
    if(ImGui::Selectable("Orthographic",draft.type==Projection::Orthographic)) {
      selectDraftType(draft,Projection::Orthographic);(void)SetCameraProjectionDraft(c,draft);
    }
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  if(!snapshot.typeSwitchBlockReason.empty()) ImGui::TextDisabled("%s",snapshot.typeSwitchBlockReason.c_str());
  draft=c.cameraProjectionDraft;
  const auto input=[&](const char* label,size_t index,bool disabled=false,bool degrees=false) {
    ImGui::BeginDisabled(disabled || !snapshot.propertyBlockReasons[index].empty());
    double value=degrees?draft.values[index]*180./std::numbers::pi:draft.values[index];
    ImGui::SetNextItemWidth(160);
    if(ImGui::InputDouble(label,&value,0,0,"%.6g")) {
      auto next=draft;next.values[index]=degrees?value*std::numbers::pi/180.:value;next.edited[index]=true;
      if(SetCameraProjectionDraft(c,next)) {draft=next;c.cameraProjectionError.clear();}
      else c.cameraProjectionError="Enter finite lens values with a valid near/far range.";
    }
    ImGui::EndDisabled();
    if(!snapshot.propertyBlockReasons[index].empty()) ImGui::TextDisabled("%s",snapshot.propertyBlockReasons[index].c_str());
  };
  if(draft.type==Projection::Perspective) {
    input("Vertical FOV (degrees)",0,false,true);
    const auto optional=[&](const char* label,size_t index,double enabledValue) {
      ImGui::BeginDisabled(!snapshot.propertyBlockReasons[index].empty() || snapshot.animatedProperties[index]);
      bool omitted=draft.values[index]==0;
      if(ImGui::Checkbox(label,&omitted)) {
        auto next=draft;next.values[index]=omitted?0:enabledValue;next.edited[index]=true;
        if(SetCameraProjectionDraft(c,next)) {draft=next;c.cameraProjectionError.clear();}
      }
      ImGui::EndDisabled();
    };
    optional("Auto aspect",1,1.);input("Aspect ratio",1,draft.values[1]==0);
    input("Near plane",2);
    optional("Infinite far",3,std::max(1000.,draft.values[2]*2));input("Far plane",3,draft.values[3]==0);
  } else {
    input("Horizontal magnitude",4);input("Vertical magnitude",5);input("Near plane",6);input("Far plane",7);
  }
  ImGui::BeginDisabled(!draftCanApply(c));
  if(ImGui::Button(pending?"Apply camera edit (queued)":"Apply camera edit")) {
    if(!QueueCameraProjectionDraft(c)) c.cameraProjectionError="Camera target changed or the lens edit is unavailable. Refresh the selection.";
  }
  ImGui::EndDisabled();ImGui::EndDisabled();
}

void DrawGltfAnimationControls(GltfAnimationControls& controls) {
  if (!ImGui::CollapsingHeader("glTF Animation")) return;
  if (!controls.error.empty()) ImGui::TextWrapped("%s",controls.error.c_str());
  ImGui::BeginDisabled(controls.requestedCameraProjection.has_value() || controls.requestedCameraViewPose.has_value());
  if (controls.clips.empty()) ImGui::TextDisabled("No animation clips in this scene.");
  else {
    const char* selected = controls.selectedClip >= 0 && static_cast<size_t>(controls.selectedClip) < controls.clips.size()
      ? controls.clips[static_cast<size_t>(controls.selectedClip)].name.c_str() : "Rest pose";
    if (ImGui::BeginCombo("Clip",selected)) {
      if (ImGui::Selectable("Rest pose",controls.selectedClip < 0)) controls.requestedClip = -1;
      for (size_t i = 0; i < controls.clips.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(controls.clips[i].name.c_str(),controls.selectedClip == static_cast<int>(i))) controls.requestedClip = static_cast<int>(i);
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    ImGui::BeginDisabled(controls.selectedClip < 0);
    if (ImGui::Button(controls.playing ? "Pause" : "Play")) controls.requestedPlaying = !controls.playing;
    ImGui::SameLine();
    bool loop = controls.loop;
    if (ImGui::Checkbox("Loop",&loop)) controls.requestedLoop = loop;
    float speed = static_cast<float>(controls.speed);
    if (ImGui::DragFloat("Speed",&speed,0.05f,-16,16,"%.2fx",ImGuiSliderFlags_AlwaysClamp)) controls.requestedSpeed = speed;
    double time = controls.time;
    const double zero = 0, duration = controls.selectedClip >= 0 && static_cast<size_t>(controls.selectedClip) < controls.clips.size()
      ? controls.clips[static_cast<size_t>(controls.selectedClip)].duration : 0;
    if (ImGui::SliderScalar("Time",ImGuiDataType_Double,&time,&zero,&duration,"%.3f s",ImGuiSliderFlags_AlwaysClamp)) controls.requestedTime = time;
    ImGui::EndDisabled();
  }
  if (!controls.cameras.empty()) {
    const char* camera = "Viewport camera";
    for (const auto& choice : controls.cameras)
      if (static_cast<int>(choice.sourceNode) == controls.cameraNode) camera = choice.name.c_str();
    if (ImGui::BeginCombo("Camera",camera)) {
      if (ImGui::Selectable("Viewport camera",controls.cameraNode < 0)) controls.requestedCameraNode = -1;
      for (const auto& choice : controls.cameras) {
        ImGui::PushID(static_cast<int>(choice.sourceNode));
        if (ImGui::Selectable(choice.name.c_str(),controls.cameraNode == static_cast<int>(choice.sourceNode))) controls.requestedCameraNode = static_cast<int>(choice.sourceNode);
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
  }
  ImGui::EndDisabled();
  if ((controls.interactivityAvailable || !controls.interactivityDiagnostic.empty()) &&
      ImGui::TreeNode("Behavior graph")) {
    if (!controls.interactivityDiagnostic.empty()) ImGui::TextWrapped("%s", controls.interactivityDiagnostic.c_str());
    if (controls.interactivityAvailable) {
      ImGui::PushID("Interactivity");
      ImGui::Text("Elapsed %.3f s", controls.interactivityTime);
      if (ImGui::Button(controls.interactivityPlaying ? "Pause" : "Play"))
        controls.requestedInteractivityPlaying = !controls.interactivityPlaying;
      ImGui::SameLine();
      if (ImGui::Button("Reset")) controls.requestedInteractivityReset = true;
      if (!controls.interactivityEvents.empty()) {
        auto& selected = controls.selectedInteractivityEvent;
        selected = std::min(selected, controls.interactivityEvents.size() - 1);
        if (ImGui::BeginCombo("Custom event", controls.interactivityEvents[selected].c_str())) {
          for (size_t i = 0; i < controls.interactivityEvents.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(controls.interactivityEvents[i].c_str(), selected == i)) selected = i;
            ImGui::PopID();
          }
          ImGui::EndCombo();
        }
        ImGui::BeginDisabled(!controls.interactivityPlaying);
        if (ImGui::Button("Send event")) controls.requestedInteractivityEvent = controls.interactivityEvents[selected];
        ImGui::EndDisabled();
      }
      ImGui::PopID();
    }
    ImGui::TreePop();
  }
  DrawCameraProjectionControls(controls);
  DrawCameraViewPoseControls(controls);
  if (!controls.diagnostics.empty() && ImGui::TreeNode("Animation diagnostics")) {
    for (const auto& message : controls.diagnostics) ImGui::TextWrapped("%s",message.c_str());
    ImGui::TreePop();
  }
}
} // namespace container::ui
