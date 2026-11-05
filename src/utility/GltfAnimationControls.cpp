#include <Container/utility/GltfAnimationControls.h>

#include <imgui.h>

namespace container::ui {
void DrawGltfAnimationControls(GltfAnimationControls& controls) {
  if (!ImGui::CollapsingHeader("glTF Animation")) return;
  if (!controls.error.empty()) ImGui::TextWrapped("%s",controls.error.c_str());
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
  if (!controls.diagnostics.empty() && ImGui::TreeNode("Animation diagnostics")) {
    for (const auto& message : controls.diagnostics) ImGui::TextWrapped("%s",message.c_str());
    ImGui::TreePop();
  }
}
} // namespace container::ui
