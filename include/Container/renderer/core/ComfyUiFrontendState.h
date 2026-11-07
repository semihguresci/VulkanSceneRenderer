#pragma once

#include "Container/renderer/core/ComfyUiBridge.h"
#include "Container/utility/AllocationManager.h"

#include <array>
#include <future>

namespace container::renderer {

struct ComfyUiFrontendState {
  explicit ComfyUiFrontendState(std::filesystem::path root) : bridge(std::move(root)) {}

  ComfyUiBridge bridge;
  std::array<char, 2049> prompt{};
  std::array<char, 1024> exportPath{};
  uint64_t seed{1};
  int task{};
  bool open{true};
  std::string captureId;
  std::string resultStatus;
  std::filesystem::path applyingEnvironment;
  uint64_t applyingEnvironmentRevision{};
  std::optional<ComfyUiResult> result;
  std::future<std::string> exportJob;
  std::string uploadedId;
  VkDescriptorSet previewDescriptor{VK_NULL_HANDLE};
  container::material::TextureResource preview;
  std::shared_ptr<const container::gpu::AllocationManager::TextureAllocationLease> previewLease;
};

}  // namespace container::renderer
