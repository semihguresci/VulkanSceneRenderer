#include "Container/renderer/core/RendererFrontend.h"

#include "Container/common/VulkanObjects.h"
#include "Container/renderer/core/ComfyUiFrontendState.h"
#include "Container/renderer/lighting/EnvironmentManager.h"
#include "Container/renderer/temporal/TemporalCapture.h"
#include "Container/utility/AtomicFile.h"
#include "Container/utility/Platform.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string_view>

namespace container::renderer {
namespace {

struct PromptPreset {
  const char* title;
  std::string_view prompt;
};

constexpr PromptPreset hdriPresets[]{
    {"Overcast studio", "Seamless 360 degree equirectangular HDRI panorama, soft overcast daylight, empty neutral studio"},
    {"Golden hour", "Seamless 360 degree equirectangular HDRI panorama of an open field at golden hour"},
};
constexpr PromptPreset restylePresets[]{
    {"Architectural photograph", "Photorealistic architectural photograph preserving the input camera, geometry and composition"},
    {"Watercolor study", "Watercolor architectural study with soft washes, preserving the input composition and geometry"},
};

void removePreview(ComfyUiFrontendState& state) {
  if (state.previewDescriptor != VK_NULL_HANDLE)
    ImGui_ImplVulkan_RemoveTexture(state.previewDescriptor);
  state.previewDescriptor = VK_NULL_HANDLE;
  state.previewLease.reset();
  state.preview = {};
}

}  // namespace

void RendererFrontend::initializeComfyUi(bool scripted) {
  if (comfyUi_ || !svc_.config.pathSampleSeries.empty() ||
      (!scripted && (!subs_.guiManager || !svc_.config.screenshotCapturePath.empty() ||
                     !svc_.config.temporalCaptureSequencePath.empty())))
    return;
  const auto* root = std::getenv("CONTAINER_COMFYUI_ROOT");
  if (!root || !*root) return;
  try {
    comfyUi_ = std::make_unique<ComfyUiFrontendState>(container::util::pathFromUtf8(root));
    comfyUi_->open = bool(subs_.guiManager);
  } catch (const std::exception& error) {
    spdlog::warn("ComfyUI bridge disabled: {}", error.what());
  }
}

bool RendererFrontend::applyComfyUiEnvironment() {
  auto& state = *comfyUi_;
  if (!state.result || state.result->kind != ComfyUiJobKind::Hdri) {
    state.resultStatus = "No generated environment is ready";
    return false;
  }
  if (subs_.environmentManager->environmentLoading()) {
    state.resultStatus = "An environment import is already in progress";
    return false;
  }
  if (!subs_.environmentManager->requestHdrEnvironment(state.result->path, state.result->encoded)) {
    state.resultStatus = subs_.environmentManager->environmentStatus();
    return false;
  }
  state.applyingEnvironment = state.result->path;
  state.applyingEnvironmentRevision = subs_.environmentManager->environmentRevision();
  state.resultStatus = "Preparing generated environment";
  return true;
}

void RendererFrontend::applyComfyUiCaptureAction(const container::temporal::CaptureComfyUi& action) {
  using Action = container::temporal::CaptureComfyUi::Action;
  initializeComfyUi(true);
  if (!comfyUi_) throw std::runtime_error("ComfyUI capture requires CONTAINER_COMFYUI_ROOT");
  switch (action.action) {
    case Action::GenerateEnvironment:
    case Action::RestyleViewport: {
      const auto kind = action.action == Action::GenerateEnvironment ? ComfyUiJobKind::Hdri : ComfyUiJobKind::Restyle;
      if (!comfyUi_->bridge.submit(kind, action.prompt, action.seed))
        throw std::runtime_error("ComfyUI capture could not start a generation job");
      comfyUi_->resultStatus.clear();
      break;
    }
    case Action::ApplyEnvironment:
      if (!applyComfyUiEnvironment()) throw std::runtime_error(comfyUi_->resultStatus);
      break;
    case Action::Cancel:
      comfyUi_->bridge.cancel();
      break;
  }
}

bool RendererFrontend::comfyUiCapturePending() const {
  if (!comfyUi_) return false;
  const auto status = comfyUi_->bridge.status();
  return status.busy() || !comfyUi_->captureId.empty() || !comfyUi_->applyingEnvironment.empty() ||
      (status.phase == ComfyUiJobPhase::Ready && (!comfyUi_->result || comfyUi_->result->id != status.id));
}

nlohmann::json RendererFrontend::comfyUiCaptureTelemetry() const {
  const auto& state = *comfyUi_;
  const auto status = state.bridge.status();
  constexpr std::array phases{"idle", "preparing", "capture", "queued", "running", "ready", "failed", "cancelled"};
  return {{"id", status.id}, {"phase", phases.at(static_cast<size_t>(status.phase))},
      {"message", status.message}, {"progress", status.progress}, {"resultStatus", state.resultStatus},
      {"result", state.result ? container::util::pathToUtf8(state.result->path) : std::string{}},
      {"previewReady", state.previewDescriptor != VK_NULL_HANDLE},
      {"activeEnvironment", container::util::pathToUtf8(subs_.environmentManager->activeHdrPath())}};
}

bool RendererFrontend::finishComfyUiCapture(std::string error) {
  if (!comfyUi_ || comfyUi_->captureId.empty()) return false;
  comfyUi_->bridge.completeCapture(comfyUi_->captureId, std::move(error));
  comfyUi_->captureId.clear();
  return true;
}

void RendererFrontend::processComfyUi() {
  if (!comfyUi_) return;
  using namespace std::chrono_literals;
  auto& state = *comfyUi_;
  if (!screenshot_.pending) {
    if (auto capture = state.bridge.takeCaptureRequest()) {
      try {
        requestScreenshot(std::move(capture->path), true);
        state.captureId = std::move(capture->id);
      } catch (const std::exception& error) {
        state.bridge.completeCapture(capture->id, error.what());
      }
    }
  }

  if (state.exportJob.valid() && state.exportJob.wait_for(0s) == std::future_status::ready)
    state.resultStatus = state.exportJob.get();
  if (!state.applyingEnvironment.empty() && !subs_.environmentManager->environmentLoading()) {
    state.resultStatus = subs_.environmentManager->activeHdrPath() == state.applyingEnvironment &&
        subs_.environmentManager->environmentRevision() != state.applyingEnvironmentRevision
        ? "Generated environment applied"
        : subs_.environmentManager->environmentStatus();
    state.applyingEnvironment.clear();
  }

  auto result = state.bridge.takeResult();
  const bool previewInvalid = state.previewDescriptor != VK_NULL_HANDLE &&
      !svc_.allocationManager.textureImageMatches(state.preview);
  if (result || previewInvalid) {
    // drawFrame has retired all graphics frames that could sample the ImGui preview.
    removePreview(state);
    state.uploadedId.clear();
    if (result) {
      state.result = std::move(result);
      state.resultStatus.clear();
    }
  }

  if (!state.open || !state.result || state.result->kind != ComfyUiJobKind::Restyle ||
      state.uploadedId == state.result->id)
    return;
  state.uploadedId = state.result->id;
  try {
    // ImGui 1.93 owns a linear, clamp-to-edge sampler matching this preview.
    auto batch = svc_.allocationManager.beginTextureAllocationBatch();
    auto texture = svc_.allocationManager.createTextureFromImageData("Generated ComfyUI preview", state.result->preview);
    auto lease = batch.commit();
    auto descriptor = ImGui_ImplVulkan_AddTexture(texture.imageView,
                                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (descriptor == VK_NULL_HANDLE) throw std::runtime_error("Could not allocate generated preview descriptor");
    state.preview = std::move(texture);
    state.previewLease = std::move(lease);
    state.previewDescriptor = descriptor;
  } catch (const std::exception& error) {
    state.resultStatus = std::string("Preview upload failed: ") + error.what();
  }
}

void RendererFrontend::drawComfyUi() {
  if (!comfyUi_) return;
  auto& state = *comfyUi_;
  if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) state.open = !state.open;
  if (!state.open) return;

  ImGui::SetNextWindowSize(ImVec2(460.f, 420.f), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("ComfyUI Generation", &state.open)) {
    const auto status = state.bridge.status();
    ImGui::BeginDisabled(status.busy());
    ImGui::Combo("Workflow", &state.task, "Environment panorama\0Restyle viewport\0");
    const bool hdri = state.task == 0;
    if (ImGui::BeginCombo("Prompt preset", "Choose a preset")) {
      for (const auto& preset : hdri ? hdriPresets : restylePresets) {
        if (ImGui::Selectable(preset.title)) {
          std::copy(preset.prompt.begin(), preset.prompt.end(), state.prompt.begin());
          state.prompt[preset.prompt.size()] = '\0';
        }
      }
      ImGui::EndCombo();
    }
    ImGui::InputTextMultiline("Prompt", state.prompt.data(), state.prompt.size(), ImVec2(-70.f, 62.f));
    ImGui::InputScalar("Seed", ImGuiDataType_U64, &state.seed);
    if (ImGui::Button(hdri ? "Generate environment" : "Capture and restyle")) {
      try {
        const auto kind = hdri ? ComfyUiJobKind::Hdri : ComfyUiJobKind::Restyle;
        if (!state.bridge.submit(kind, state.prompt.data(), state.seed))
          state.resultStatus = "Enter a prompt before submitting a job";
        else
          state.resultStatus.clear();
      } catch (const std::exception& error) {
        state.resultStatus = error.what();
      }
    }
    ImGui::EndDisabled();
    if (status.busy()) {
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) state.bridge.cancel();
      ImGui::ProgressBar(status.progress, ImVec2(-1.f, 0.f));
    }
    if (!status.message.empty()) ImGui::TextWrapped("%s", status.message.c_str());

    if (state.result) {
      ImGui::SeparatorText("Generated result");
      if (state.result->kind == ComfyUiJobKind::Hdri) {
        ImGui::TextUnformatted("Generated panorama");
        ImGui::BeginDisabled(subs_.environmentManager->environmentLoading());
        if (ImGui::Button("Apply generated environment")) applyComfyUiEnvironment();
        ImGui::EndDisabled();
      } else {
        if (state.previewDescriptor != VK_NULL_HANDLE && svc_.allocationManager.textureImageMatches(state.preview)) {
          const float width = std::min(ImGui::GetContentRegionAvail().x, 460.f);
          if (width > 0.f)
            ImGui::Image((ImTextureID)state.previewDescriptor,
                         ImVec2(width, width * state.preview.height / state.preview.width));
        }
        ImGui::InputText("Export path", state.exportPath.data(), state.exportPath.size());
        ImGui::BeginDisabled(state.exportJob.valid() || state.exportPath[0] == '\0');
        if (ImGui::Button("Save generated PNG")) {
          try {
            auto path = container::util::pathFromUtf8(state.exportPath.data());
            if (path.extension() != ".png") throw std::invalid_argument("Export path must end in .png");
            state.exportJob = std::async(std::launch::async, [path = std::move(path), bytes = state.result->encoded] {
              try {
                container::util::writeFileAtomically(path,
                    {reinterpret_cast<const char*>(bytes->data()), bytes->size()},
                    container::util::AtomicFilePublication::CreateNew);
                return std::string("Saved generated PNG");
              } catch (const std::exception& error) {
                return std::string("Generated PNG export failed: ") + error.what();
              }
            });
          } catch (const std::exception& error) {
            state.resultStatus = error.what();
          }
        }
        ImGui::EndDisabled();
      }
    }
    if (!state.resultStatus.empty()) ImGui::TextWrapped("%s", state.resultStatus.c_str());
  }
  ImGui::End();
}

void RendererFrontend::retireComfyUiPreview() {
  if (!comfyUi_) return;
  removePreview(*comfyUi_);
  comfyUi_->uploadedId.clear();
}

void RendererFrontend::shutdownComfyUi() {
  if (!comfyUi_) return;
  retireComfyUiPreview();
  comfyUi_.reset();
}

}  // namespace container::renderer
