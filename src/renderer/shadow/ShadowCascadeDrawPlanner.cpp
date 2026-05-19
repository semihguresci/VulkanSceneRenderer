#include "Container/renderer/shadow/ShadowCascadeDrawPlanner.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace container::renderer {

namespace {

using container::gpu::kShadowCascadeCount;

[[nodiscard]] uint32_t drawInstanceCount(const DrawCommand &command) {
  return std::max(command.instanceCount, 1u);
}

[[nodiscard]] bool isValidCascadeBoundingSphere(const glm::vec4 &sphere) {
  return std::isfinite(sphere.x) && std::isfinite(sphere.y) &&
         std::isfinite(sphere.z) && std::isfinite(sphere.w) &&
         sphere.w > 0.0f;
}

void mixHash(uint64_t &signature, uint64_t value) {
  signature ^= value;
  signature *= 1099511628211ull;
}

void hashBytes(uint64_t &signature, const void *data, size_t size) {
  const auto *bytes = static_cast<const unsigned char *>(data);
  for (size_t i = 0; i < size; ++i) {
    mixHash(signature, bytes[i]);
  }
}

[[nodiscard]] size_t
drawCommandCount(const std::vector<DrawCommand> *commands) {
  return commands != nullptr ? commands->size() : 0u;
}

void hashDrawCommands(uint64_t &signature,
                      const std::vector<DrawCommand> *commands) {
  mixHash(signature, reinterpret_cast<uintptr_t>(commands));
  mixHash(signature, drawCommandCount(commands));
  if (commands == nullptr) {
    return;
  }
  for (const DrawCommand &command : *commands) {
    mixHash(signature, command.objectIndex);
    mixHash(signature, command.firstIndex);
    mixHash(signature, command.indexCount);
    mixHash(signature, command.instanceCount);
  }
}

[[nodiscard]] bool
shouldWriteCascade(const ShadowCascadeDrawPlannerInputs &inputs,
                   bool skipGpuCulledSingleSided, uint32_t cascadeIndex) {
  return !skipGpuCulledSingleSided ||
         cascadeIndex >= inputs.sceneSingleSidedUsesGpuCull.size() ||
         !inputs.sceneSingleSidedUsesGpuCull[cascadeIndex];
}

[[nodiscard]] bool cascadeActive(const ShadowCascadeDrawPlannerInputs &inputs,
                                 uint32_t cascadeIndex) {
  return cascadeIndex < inputs.shadowPassActive.size() &&
         inputs.shadowPassActive[cascadeIndex];
}

template <typename Destination>
void distributeCommands(const ShadowCascadeDrawPlannerInputs &inputs,
                        const std::vector<DrawCommand> *source,
                        Destination &destination,
                        bool skipGpuCulledSingleSided) {
  if (source == nullptr) {
    return;
  }

  for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
       ++cascadeIndex) {
    if (!shouldWriteCascade(inputs, skipGpuCulledSingleSided, cascadeIndex)) {
      continue;
    }
    if (!cascadeActive(inputs, cascadeIndex)) {
      continue;
    }
    destination[cascadeIndex].reserve(source->size());
  }

  for (const DrawCommand &command : *source) {
    for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
         ++cascadeIndex) {
      if (!shouldWriteCascade(inputs, skipGpuCulledSingleSided, cascadeIndex)) {
        continue;
      }
      if (!cascadeActive(inputs, cascadeIndex)) {
        continue;
      }
      destination[cascadeIndex].push_back(command);
    }
  }
}

void appendVisibleRun(std::vector<DrawCommand> &commands,
                      const DrawCommand &sourceCommand, uint32_t runOffset,
                      uint32_t runCount) {
  if (runCount == 0u) {
    return;
  }
  DrawCommand visibleCommand = sourceCommand;
  visibleCommand.objectIndex = sourceCommand.objectIndex + runOffset;
  visibleCommand.instanceCount = runCount;
  commands.push_back(visibleCommand);
}

template <typename Destination>
void filterCommands(const ShadowCascadeDrawPlannerInputs &inputs,
                    const std::vector<DrawCommand> *source,
                    const ShadowCascadeSceneDataView &scene,
                    Destination &destination, bool skipGpuCulledSingleSided) {
  if (source == nullptr) {
    return;
  }

  if (!inputs.cascadeIntersectsSphere || scene.objectData == nullptr) {
    distributeCommands(inputs, source, destination, skipGpuCulledSingleSided);
    return;
  }

  for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
       ++cascadeIndex) {
    if (!shouldWriteCascade(inputs, skipGpuCulledSingleSided, cascadeIndex)) {
      continue;
    }
    if (!cascadeActive(inputs, cascadeIndex)) {
      continue;
    }
    destination[cascadeIndex].reserve(source->size());
  }

  for (const DrawCommand &command : *source) {
    const uint32_t instanceCount = drawInstanceCount(command);
    if (command.objectIndex >= scene.objectData->size() ||
        instanceCount > scene.objectData->size() -
                            static_cast<size_t>(command.objectIndex)) {
      for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
           ++cascadeIndex) {
        if (!shouldWriteCascade(inputs, skipGpuCulledSingleSided,
                                cascadeIndex)) {
          continue;
        }
        if (!cascadeActive(inputs, cascadeIndex)) {
          continue;
        }
        destination[cascadeIndex].push_back(command);
      }
      continue;
    }

    for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
         ++cascadeIndex) {
      if (!shouldWriteCascade(inputs, skipGpuCulledSingleSided, cascadeIndex)) {
        continue;
      }
      if (!cascadeActive(inputs, cascadeIndex)) {
        continue;
      }

      uint32_t runOffset = 0u;
      uint32_t runCount = 0u;
      for (uint32_t instanceOffset = 0u; instanceOffset < instanceCount;
           ++instanceOffset) {
        const glm::vec4 boundingSphere =
            (*scene.objectData)[command.objectIndex + instanceOffset]
                .boundingSphere;
        const bool hasValidBounds =
            isValidCascadeBoundingSphere(boundingSphere);
        const bool visible =
            !hasValidBounds ||
            inputs.cascadeIntersectsSphere(cascadeIndex, boundingSphere);

        if (visible) {
          if (runCount == 0u) {
            runOffset = instanceOffset;
          }
          ++runCount;
          continue;
        }

        appendVisibleRun(destination[cascadeIndex], command, runOffset,
                         runCount);
        runCount = 0u;
      }
      appendVisibleRun(destination[cascadeIndex], command, runOffset, runCount);
    }
  }
}

} // namespace

size_t
ShadowCascadeDrawPlan::cpuCommandCount(uint32_t cascadeIndex,
                                       bool includeSceneSingleSided) const {
  if (cascadeIndex >= kShadowCascadeCount) {
    return 0u;
  }

  size_t count = sceneWindingFlipped[cascadeIndex].size() +
                 sceneDoubleSided[cascadeIndex].size() +
                 bimSingleSided[cascadeIndex].size() +
                 bimWindingFlipped[cascadeIndex].size() +
                 bimDoubleSided[cascadeIndex].size();
  if (includeSceneSingleSided) {
    count += sceneSingleSided[cascadeIndex].size();
  }
  return count;
}

ShadowCascadeDrawPlanner::ShadowCascadeDrawPlanner(
    ShadowCascadeDrawPlannerInputs inputs)
    : inputs_(std::move(inputs)) {}

uint64_t ShadowCascadeDrawPlanner::signature() const {
  return computeShadowCascadeDrawSignature(inputs_);
}

ShadowCascadeDrawPlan ShadowCascadeDrawPlanner::build() const {
  ShadowCascadeDrawPlan plan{};
  plan.signature = signature();

  filterCommands(inputs_, inputs_.sceneDraws.singleSided, inputs_.scene,
                 plan.sceneSingleSided, true);
  filterCommands(inputs_, inputs_.sceneDraws.windingFlipped, inputs_.scene,
                 plan.sceneWindingFlipped, false);
  filterCommands(inputs_, inputs_.sceneDraws.doubleSided, inputs_.scene,
                 plan.sceneDoubleSided, false);

  if (inputs_.hasBimShadowGeometry) {
    const uint32_t bimDrawListCount =
        std::min<uint32_t>(inputs_.bimDrawListCount,
                           static_cast<uint32_t>(inputs_.bimDraws.size()));
    for (uint32_t listIndex = 0; listIndex < bimDrawListCount; ++listIndex) {
      const ShadowCascadeSurfaceDrawLists &draws = inputs_.bimDraws[listIndex];
      if (!draws.cpuFallbackAllowed) {
        continue;
      }
      filterCommands(inputs_, draws.singleSided, inputs_.bimScene,
                     plan.bimSingleSided, false);
      filterCommands(inputs_, draws.windingFlipped, inputs_.bimScene,
                     plan.bimWindingFlipped, false);
      filterCommands(inputs_, draws.doubleSided, inputs_.bimScene,
                     plan.bimDoubleSided, false);
    }
  }

  return plan;
}

uint64_t computeShadowCascadeDrawSignature(
    const ShadowCascadeDrawPlannerInputs &inputs) {
  uint64_t signature = 1469598103934665603ull;
  mixHash(signature, inputs.scene.objectDataRevision);
  mixHash(signature, reinterpret_cast<uintptr_t>(inputs.scene.objectData));
  mixHash(signature, inputs.scene.objectData != nullptr
                         ? inputs.scene.objectData->size()
                         : 0u);
  mixHash(signature, inputs.bimScene.objectDataRevision);
  mixHash(signature, reinterpret_cast<uintptr_t>(inputs.bimScene.objectData));
  mixHash(signature, inputs.bimScene.objectData != nullptr
                         ? inputs.bimScene.objectData->size()
                         : 0u);
  mixHash(signature, inputs.hasBimShadowGeometry ? 1u : 0u);
  mixHash(signature, reinterpret_cast<uintptr_t>(inputs.shadowManagerIdentity));
  mixHash(signature, inputs.useGpuShadowCull ? 1u : 0u);

  for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
       ++cascadeIndex) {
    mixHash(signature, inputs.shadowPassActive[cascadeIndex] ? 1u : 0u);
    mixHash(signature, inputs.shadowCullPassActive[cascadeIndex] ? 1u : 0u);
    mixHash(signature,
            inputs.sceneSingleSidedUsesGpuCull[cascadeIndex] ? 1u : 0u);
  }

  hashDrawCommands(signature, inputs.sceneDraws.singleSided);
  hashDrawCommands(signature, inputs.sceneDraws.windingFlipped);
  hashDrawCommands(signature, inputs.sceneDraws.doubleSided);

  const uint32_t bimDrawListCount = std::min<uint32_t>(
      inputs.bimDrawListCount, static_cast<uint32_t>(inputs.bimDraws.size()));
  for (uint32_t listIndex = 0; listIndex < bimDrawListCount; ++listIndex) {
    const ShadowCascadeSurfaceDrawLists &draws = inputs.bimDraws[listIndex];
    hashDrawCommands(signature, draws.singleSided);
    hashDrawCommands(signature, draws.windingFlipped);
    hashDrawCommands(signature, draws.doubleSided);
    mixHash(signature, draws.cpuFallbackAllowed ? 1u : 0u);
  }

  if (inputs.shadowData != nullptr) {
    hashBytes(signature, inputs.shadowData, sizeof(*inputs.shadowData));
  }
  return signature;
}

ShadowCascadeDrawPlan
buildShadowCascadeDrawPlan(const ShadowCascadeDrawPlannerInputs &inputs) {
  return ShadowCascadeDrawPlanner(inputs).build();
}

} // namespace container::renderer
