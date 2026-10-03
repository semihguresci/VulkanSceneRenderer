#pragma once

#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/resources/FrameResourceRegistry.h"

#include <cstdint>
#include <string_view>

namespace container::renderer {

enum class DeferredRasterFramebufferId {
  DepthPrepass,
  BimDepthPrepass,
  GBuffer,
  BimGBuffer,
  TransparentPick,
  Lighting,
  TransformGizmo,
};

enum class DeferredRasterImageId {
  DepthStencil,
  DepthSamplingView,
  SceneColor,
  Normal,
  PickDepth,
  PickId,
  OitHeadPointers,
};

enum class DeferredRasterBufferId {
  Camera,
  SceneObject,
  OitNode,
  OitCounter,
  OitMetadata,
};

enum class DeferredRasterDescriptorSetId {
  Scene,
  BimScene,
  Light,
  TiledLighting,
  FrameLighting,
  PostProcess,
  Oit,
};

enum class DeferredRasterSamplerId {
  GBuffer,
};

[[nodiscard]] inline std::string_view deferredRasterFramebufferKey(
    DeferredRasterFramebufferId id) {
  switch (id) {
  case DeferredRasterFramebufferId::DepthPrepass:
    return "depth-prepass-framebuffer";
  case DeferredRasterFramebufferId::BimDepthPrepass:
    return "bim-depth-prepass-framebuffer";
  case DeferredRasterFramebufferId::GBuffer:
    return "gbuffer-framebuffer";
  case DeferredRasterFramebufferId::BimGBuffer:
    return "bim-gbuffer-framebuffer";
  case DeferredRasterFramebufferId::TransparentPick:
    return "transparent-pick-framebuffer";
  case DeferredRasterFramebufferId::Lighting:
    return "lighting-framebuffer";
  case DeferredRasterFramebufferId::TransformGizmo:
    return "transform-gizmo-framebuffer";
  }
  return {};
}

[[nodiscard]] inline std::string_view deferredRasterImageKey(
    DeferredRasterImageId id) {
  switch (id) {
  case DeferredRasterImageId::DepthStencil:
    return "depth-stencil";
  case DeferredRasterImageId::DepthSamplingView:
    return "depth-sampling-view";
  case DeferredRasterImageId::SceneColor:
    return "scene-color";
  case DeferredRasterImageId::Normal:
    return "normal";
  case DeferredRasterImageId::PickDepth:
    return "pick-depth";
  case DeferredRasterImageId::PickId:
    return "pick-id";
  case DeferredRasterImageId::OitHeadPointers:
    return "oit-head-pointers";
  }
  return {};
}

[[nodiscard]] inline std::string_view deferredRasterBufferKey(
    DeferredRasterBufferId id) {
  switch (id) {
  case DeferredRasterBufferId::Camera:
    return "camera-buffer";
  case DeferredRasterBufferId::SceneObject:
    return "scene-object-buffer";
  case DeferredRasterBufferId::OitNode:
    return "oit-node-buffer";
  case DeferredRasterBufferId::OitCounter:
    return "oit-counter-buffer";
  case DeferredRasterBufferId::OitMetadata:
    return "oit-metadata-buffer";
  }
  return {};
}

[[nodiscard]] inline std::string_view deferredRasterDescriptorSetKey(
    DeferredRasterDescriptorSetId id) {
  switch (id) {
  case DeferredRasterDescriptorSetId::Scene:
    return "scene-descriptor-set";
  case DeferredRasterDescriptorSetId::BimScene:
    return "bim-scene-descriptor-set";
  case DeferredRasterDescriptorSetId::Light:
    return "light-descriptor-set";
  case DeferredRasterDescriptorSetId::TiledLighting:
    return "tiled-lighting-descriptor-set";
  case DeferredRasterDescriptorSetId::FrameLighting:
    return "frame-lighting-descriptor-set";
  case DeferredRasterDescriptorSetId::PostProcess:
    return "post-process-descriptor-set";
  case DeferredRasterDescriptorSetId::Oit:
    return "oit-descriptor-set";
  }
  return {};
}

[[nodiscard]] inline std::string_view deferredRasterSamplerKey(
    DeferredRasterSamplerId id) {
  switch (id) {
  case DeferredRasterSamplerId::GBuffer:
    return "g-buffer-sampler";
  }
  return {};
}

[[nodiscard]] inline const FrameImageBinding* deferredRasterImageBinding(
    const FrameRecordParams& p, DeferredRasterImageId id) {
  return p.imageBinding(RenderTechniqueId::DeferredRaster,
                        deferredRasterImageKey(id));
}

[[nodiscard]] inline const FrameBufferBinding* deferredRasterBufferBinding(
    const FrameRecordParams& p, DeferredRasterBufferId id) {
  return p.bufferBinding(RenderTechniqueId::DeferredRaster,
                         deferredRasterBufferKey(id));
}

[[nodiscard]] inline const FrameDescriptorBinding*
deferredRasterDescriptorBinding(const FrameRecordParams& p,
                                DeferredRasterDescriptorSetId id) {
  return p.descriptorBinding(RenderTechniqueId::DeferredRaster,
                             deferredRasterDescriptorSetKey(id));
}

[[nodiscard]] inline const FrameSamplerBinding* deferredRasterSamplerBinding(
    const FrameRecordParams& p, DeferredRasterSamplerId id) {
  return p.samplerBinding(RenderTechniqueId::DeferredRaster,
                          deferredRasterSamplerKey(id));
}

[[nodiscard]] inline const FrameFramebufferBinding*
deferredRasterFramebufferBinding(const FrameRecordParams& p,
                                 DeferredRasterFramebufferId id) {
  return p.framebufferBinding(RenderTechniqueId::DeferredRaster,
                              deferredRasterFramebufferKey(id));
}

[[nodiscard]] inline VkImage deferredRasterImage(
    const FrameRecordParams& p, DeferredRasterImageId id) {
  const FrameImageBinding* binding = deferredRasterImageBinding(p, id);
  if (binding != nullptr && binding->image != VK_NULL_HANDLE) {
    return binding->image;
  }
  return VK_NULL_HANDLE;
}

[[nodiscard]] inline VkImageView deferredRasterImageView(
    const FrameRecordParams& p, DeferredRasterImageId id) {
  const FrameImageBinding* binding = deferredRasterImageBinding(p, id);
  if (binding != nullptr && binding->view != VK_NULL_HANDLE) {
    return binding->view;
  }
  return VK_NULL_HANDLE;
}

[[nodiscard]] inline bool deferredRasterImageReady(
    const FrameRecordParams& p, DeferredRasterImageId id) {
  return deferredRasterImage(p, id) != VK_NULL_HANDLE;
}

[[nodiscard]] inline bool deferredRasterImageViewReady(
    const FrameRecordParams& p, DeferredRasterImageId id) {
  return deferredRasterImageView(p, id) != VK_NULL_HANDLE;
}

[[nodiscard]] inline VkBuffer deferredRasterBuffer(
    const FrameRecordParams& p, DeferredRasterBufferId id) {
  const FrameBufferBinding* binding = deferredRasterBufferBinding(p, id);
  if (binding != nullptr && binding->buffer != VK_NULL_HANDLE) {
    return binding->buffer;
  }
  return VK_NULL_HANDLE;
}

[[nodiscard]] inline VkDeviceSize deferredRasterBufferSize(
    const FrameRecordParams& p, DeferredRasterBufferId id) {
  const FrameBufferBinding* binding = deferredRasterBufferBinding(p, id);
  if (binding != nullptr && binding->size > 0) {
    return binding->size;
  }
  return 0;
}

[[nodiscard]] inline VkDescriptorSet deferredRasterDescriptorSet(
    const FrameRecordParams& p, DeferredRasterDescriptorSetId id) {
  return p.descriptorSet(
      RenderTechniqueId::DeferredRaster, deferredRasterDescriptorSetKey(id));
}

[[nodiscard]] inline bool deferredRasterDescriptorSetReady(
    const FrameRecordParams& p, DeferredRasterDescriptorSetId id) {
  return deferredRasterDescriptorSet(p, id) != VK_NULL_HANDLE;
}

[[nodiscard]] inline VkSampler deferredRasterSampler(
    const FrameRecordParams& p, DeferredRasterSamplerId id) {
  return p.sampler(RenderTechniqueId::DeferredRaster,
                   deferredRasterSamplerKey(id));
}

[[nodiscard]] inline bool deferredRasterSamplerReady(
    const FrameRecordParams& p, DeferredRasterSamplerId id) {
  return deferredRasterSampler(p, id) != VK_NULL_HANDLE;
}

[[nodiscard]] inline RenderingTargetHandle deferredRasterFramebuffer(
    const FrameRecordParams& p, DeferredRasterFramebufferId id) {
  return p.framebuffer(
      RenderTechniqueId::DeferredRaster, deferredRasterFramebufferKey(id));
}

[[nodiscard]] inline RenderingPassHandle deferredRasterRenderPass(
    const FrameRecordParams& p, DeferredRasterFramebufferId id) {
  const FrameFramebufferBinding* binding =
      deferredRasterFramebufferBinding(p, id);
  if (binding != nullptr && binding->renderPass != VK_NULL_HANDLE) {
    return binding->renderPass;
  }
  return VK_NULL_HANDLE;
}

}  // namespace container::renderer
