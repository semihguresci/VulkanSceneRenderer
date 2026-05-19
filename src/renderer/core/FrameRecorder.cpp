#include "Container/renderer/core/FrameRecorder.h"

#include "Container/renderer/core/CommandBufferScopeRecorder.h"
#include "Container/renderer/core/RenderPassGpuProfiler.h"
#include "Container/renderer/core/RendererTelemetry.h"
#include "Container/renderer/pipeline/PipelineRegistry.h"
#include "Container/renderer/resources/FrameResourceRegistry.h"

#include <stdexcept>
#include <string>

namespace container::renderer {

const FrameResourceBinding *FrameRecordParams::resourceBinding(
    RenderTechniqueId technique, std::string_view name) const {
  if (registries.resourceBindings == nullptr) {
    return nullptr;
  }
  return registries.resourceBindings->findBinding(
      TechniqueResourceKey{.technique = technique, .name = std::string(name)},
      runtime.imageIndex);
}

const FrameImageBinding *FrameRecordParams::imageBinding(
    RenderTechniqueId technique, std::string_view name) const {
  const FrameResourceBinding *binding = resourceBinding(technique, name);
  return binding != nullptr && binding->kind == FrameResourceKind::Image
             ? &binding->image
             : nullptr;
}

const FrameBufferBinding *FrameRecordParams::bufferBinding(
    RenderTechniqueId technique, std::string_view name) const {
  const FrameResourceBinding *binding = resourceBinding(technique, name);
  return binding != nullptr && binding->kind == FrameResourceKind::Buffer
             ? &binding->buffer
             : nullptr;
}

const FrameFramebufferBinding *FrameRecordParams::framebufferBinding(
    RenderTechniqueId technique, std::string_view name) const {
  const FrameResourceBinding *binding = resourceBinding(technique, name);
  return binding != nullptr && binding->kind == FrameResourceKind::Framebuffer
             ? &binding->framebuffer
             : nullptr;
}

const FrameDescriptorBinding *FrameRecordParams::descriptorBinding(
    RenderTechniqueId technique, std::string_view name) const {
  const FrameResourceBinding *binding = resourceBinding(technique, name);
  return binding != nullptr && binding->kind == FrameResourceKind::DescriptorSet
             ? &binding->descriptor
             : nullptr;
}

const FrameSamplerBinding *FrameRecordParams::samplerBinding(
    RenderTechniqueId technique, std::string_view name) const {
  const FrameResourceBinding *binding = resourceBinding(technique, name);
  return binding != nullptr && binding->kind == FrameResourceKind::Sampler
             ? &binding->sampler
             : nullptr;
}

VkFramebuffer FrameRecordParams::framebuffer(RenderTechniqueId technique,
                                             std::string_view name) const {
  const FrameFramebufferBinding *binding = framebufferBinding(technique, name);
  return binding != nullptr ? binding->framebuffer : VK_NULL_HANDLE;
}

VkDescriptorSet FrameRecordParams::descriptorSet(
    RenderTechniqueId technique, std::string_view name) const {
  const FrameDescriptorBinding *binding = descriptorBinding(technique, name);
  return binding != nullptr ? binding->descriptorSet : VK_NULL_HANDLE;
}

VkSampler FrameRecordParams::sampler(RenderTechniqueId technique,
                                     std::string_view name) const {
  const FrameSamplerBinding *binding = samplerBinding(technique, name);
  return binding != nullptr ? binding->sampler : VK_NULL_HANDLE;
}

VkPipeline FrameRecordParams::pipelineHandle(RenderTechniqueId technique,
                                             std::string_view name) const {
  if (registries.pipelineHandles == nullptr) {
    return VK_NULL_HANDLE;
  }
  return registries.pipelineHandles->pipelineHandle(
      TechniquePipelineKey{.technique = technique, .name = std::string(name)});
}

VkPipelineLayout FrameRecordParams::pipelineLayout(
    RenderTechniqueId technique, std::string_view name) const {
  if (registries.pipelineLayouts == nullptr) {
    return VK_NULL_HANDLE;
  }
  return registries.pipelineLayouts->pipelineLayout(
      TechniquePipelineKey{.technique = technique, .name = std::string(name)});
}

void FrameRecorder::record(VkCommandBuffer commandBuffer,
                            const FrameRecordParams &p) const {
  if (commandBuffer == VK_NULL_HANDLE) {
    throw std::runtime_error(
        "FrameRecorder::record received a null command buffer");
  }

  if (p.lifecycle.beforePrepareFrame) {
    p.lifecycle.beforePrepareFrame(p);
  }

  graph_.prepareFrame(p);

  if (p.lifecycle.afterPrepareFrame) {
    p.lifecycle.afterPrepareFrame(p, graph_);
  }

  if (!recordCommandBufferBeginCommands(commandBuffer, {})) {
    throw std::runtime_error("failed to begin recording command buffer!");
  }

  if (p.lifecycle.afterCommandBufferBegin) {
    p.lifecycle.afterCommandBufferBegin(commandBuffer, p);
  }

  if (p.services.telemetry || p.services.gpuProfiler) {
    if (p.services.gpuProfiler) {
      p.services.gpuProfiler->beginFrame(commandBuffer, p.runtime.frameSlot);
    }
    RenderPassExecutionHooks hooks{};
    hooks.beginPass = [gpuProfiler = p.services.gpuProfiler](
                          RenderPassId id, VkCommandBuffer cmd) {
      if (gpuProfiler) {
        gpuProfiler->beginPass(cmd, id);
      }
    };
    hooks.endPass = [telemetry = p.services.telemetry,
                     gpuProfiler = p.services.gpuProfiler](
                        RenderPassId id, VkCommandBuffer cmd, float cpuMs) {
      if (gpuProfiler) {
        gpuProfiler->endPass(cmd, id);
      }
      if (telemetry) {
        telemetry->recordPassCpuTime(id, cpuMs);
      }
    };
    graph_.executePreparedFrame(commandBuffer, p, hooks);
  } else {
    graph_.executePreparedFrame(commandBuffer, p);
  }

  if (p.lifecycle.afterGraphExecution) {
    p.lifecycle.afterGraphExecution(commandBuffer, p);
  }

  if (!recordCommandBufferEndCommands(commandBuffer)) {
    throw std::runtime_error("failed to record command buffer!");
  }
}

} // namespace container::renderer
