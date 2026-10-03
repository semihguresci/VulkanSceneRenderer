#pragma once

#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/resources/FrameResourceRegistry.h"

#include <string_view>

namespace container::renderer {

enum class ForwardRasterFramebufferId {
  DepthPrepass,
  BimDepthPrepass,
  TransparentPick,
  Lighting,
  TransparentLighting,
  TransformGizmo,
};

enum class ForwardRasterImageId {
  DepthStencil,
  DepthSamplingView,
  SceneColor,
  PickDepth,
  PickId,
  OitHeadPointers,
};

enum class ForwardRasterBufferId {
  Camera,
  SceneObject,
  OitNode,
  OitCounter,
  OitMetadata,
};

enum class ForwardRasterDescriptorSetId {
  Scene,
  BimScene,
  Light,
  Shadow,
  LocalShadow,
  FrameLighting,
  PostProcess,
  Oit,
};

[[nodiscard]] inline std::string_view forwardRasterFramebufferKey(
    ForwardRasterFramebufferId id) {
  switch (id) {
  case ForwardRasterFramebufferId::DepthPrepass:
    return "depth-prepass-framebuffer";
  case ForwardRasterFramebufferId::BimDepthPrepass:
    return "bim-depth-prepass-framebuffer";
  case ForwardRasterFramebufferId::TransparentPick:
    return "transparent-pick-framebuffer";
  case ForwardRasterFramebufferId::Lighting:
    return "lighting-framebuffer";
  case ForwardRasterFramebufferId::TransparentLighting:
    return "transparent-lighting-framebuffer";
  case ForwardRasterFramebufferId::TransformGizmo:
    return "transform-gizmo-framebuffer";
  }
  return {};
}

[[nodiscard]] inline std::string_view forwardRasterImageKey(
    ForwardRasterImageId id) {
  switch (id) {
  case ForwardRasterImageId::DepthStencil:
    return "depth-stencil";
  case ForwardRasterImageId::DepthSamplingView:
    return "depth-sampling-view";
  case ForwardRasterImageId::SceneColor:
    return "scene-color";
  case ForwardRasterImageId::PickDepth:
    return "pick-depth";
  case ForwardRasterImageId::PickId:
    return "pick-id";
  case ForwardRasterImageId::OitHeadPointers:
    return "oit-head-pointers";
  }
  return {};
}

[[nodiscard]] inline std::string_view forwardRasterBufferKey(
    ForwardRasterBufferId id) {
  switch (id) {
  case ForwardRasterBufferId::Camera:
    return "camera-buffer";
  case ForwardRasterBufferId::SceneObject:
    return "scene-object-buffer";
  case ForwardRasterBufferId::OitNode:
    return "oit-node-buffer";
  case ForwardRasterBufferId::OitCounter:
    return "oit-counter-buffer";
  case ForwardRasterBufferId::OitMetadata:
    return "oit-metadata-buffer";
  }
  return {};
}

[[nodiscard]] inline std::string_view forwardRasterDescriptorSetKey(
    ForwardRasterDescriptorSetId id) {
  switch (id) {
  case ForwardRasterDescriptorSetId::Scene:
    return "scene-descriptor-set";
  case ForwardRasterDescriptorSetId::BimScene:
    return "bim-scene-descriptor-set";
  case ForwardRasterDescriptorSetId::Light:
    return "light-descriptor-set";
  case ForwardRasterDescriptorSetId::Shadow:
    return "shadow-descriptor-set";
  case ForwardRasterDescriptorSetId::LocalShadow:
    return "local-shadow-descriptor-set";
  case ForwardRasterDescriptorSetId::FrameLighting:
    return "frame-lighting-descriptor-set";
  case ForwardRasterDescriptorSetId::PostProcess:
    return "post-process-descriptor-set";
  case ForwardRasterDescriptorSetId::Oit:
    return "oit-descriptor-set";
  }
  return {};
}

[[nodiscard]] inline const FrameImageBinding* forwardRasterImageBinding(
    const FrameRecordParams& p, ForwardRasterImageId id) {
  return p.imageBinding(RenderTechniqueId::ForwardRaster,
                        forwardRasterImageKey(id));
}

[[nodiscard]] inline const FrameBufferBinding* forwardRasterBufferBinding(
    const FrameRecordParams& p, ForwardRasterBufferId id) {
  return p.bufferBinding(RenderTechniqueId::ForwardRaster,
                         forwardRasterBufferKey(id));
}

[[nodiscard]] inline const FrameDescriptorBinding*
forwardRasterDescriptorBinding(const FrameRecordParams& p,
                               ForwardRasterDescriptorSetId id) {
  return p.descriptorBinding(RenderTechniqueId::ForwardRaster,
                             forwardRasterDescriptorSetKey(id));
}

[[nodiscard]] inline const FrameFramebufferBinding*
forwardRasterFramebufferBinding(const FrameRecordParams& p,
                                ForwardRasterFramebufferId id) {
  return p.framebufferBinding(RenderTechniqueId::ForwardRaster,
                              forwardRasterFramebufferKey(id));
}

[[nodiscard]] inline VkImage forwardRasterImage(
    const FrameRecordParams& p, ForwardRasterImageId id) {
  const FrameImageBinding* binding = forwardRasterImageBinding(p, id);
  if (binding != nullptr && binding->image != VK_NULL_HANDLE) {
    return binding->image;
  }
  return VK_NULL_HANDLE;
}

[[nodiscard]] inline VkImageView forwardRasterImageView(
    const FrameRecordParams& p, ForwardRasterImageId id) {
  const FrameImageBinding* binding = forwardRasterImageBinding(p, id);
  if (binding != nullptr && binding->view != VK_NULL_HANDLE) {
    return binding->view;
  }
  return VK_NULL_HANDLE;
}

[[nodiscard]] inline bool forwardRasterImageReady(
    const FrameRecordParams& p, ForwardRasterImageId id) {
  return forwardRasterImage(p, id) != VK_NULL_HANDLE;
}

[[nodiscard]] inline bool forwardRasterImageViewReady(
    const FrameRecordParams& p, ForwardRasterImageId id) {
  return forwardRasterImageView(p, id) != VK_NULL_HANDLE;
}

[[nodiscard]] inline VkBuffer forwardRasterBuffer(
    const FrameRecordParams& p, ForwardRasterBufferId id) {
  const FrameBufferBinding* binding = forwardRasterBufferBinding(p, id);
  if (binding != nullptr && binding->buffer != VK_NULL_HANDLE) {
    return binding->buffer;
  }
  return VK_NULL_HANDLE;
}

[[nodiscard]] inline VkDeviceSize forwardRasterBufferSize(
    const FrameRecordParams& p, ForwardRasterBufferId id) {
  const FrameBufferBinding* binding = forwardRasterBufferBinding(p, id);
  if (binding != nullptr && binding->size > 0) {
    return binding->size;
  }
  return 0;
}

[[nodiscard]] inline bool forwardRasterBufferReady(
    const FrameRecordParams& p, ForwardRasterBufferId id) {
  return forwardRasterBuffer(p, id) != VK_NULL_HANDLE;
}

[[nodiscard]] inline VkDescriptorSet forwardRasterDescriptorSet(
    const FrameRecordParams& p, ForwardRasterDescriptorSetId id) {
  return p.descriptorSet(RenderTechniqueId::ForwardRaster,
                         forwardRasterDescriptorSetKey(id));
}

[[nodiscard]] inline bool forwardRasterDescriptorSetReady(
    const FrameRecordParams& p, ForwardRasterDescriptorSetId id) {
  return forwardRasterDescriptorSet(p, id) != VK_NULL_HANDLE;
}

[[nodiscard]] inline RenderingTargetHandle forwardRasterFramebuffer(
    const FrameRecordParams& p, ForwardRasterFramebufferId id) {
  return p.framebuffer(RenderTechniqueId::ForwardRaster,
                       forwardRasterFramebufferKey(id));
}

[[nodiscard]] inline bool forwardRasterFramebufferReady(
    const FrameRecordParams& p, ForwardRasterFramebufferId id) {
  return forwardRasterFramebuffer(p, id) != VK_NULL_HANDLE;
}

[[nodiscard]] inline RenderingPassHandle forwardRasterRenderPass(
    const FrameRecordParams& p, ForwardRasterFramebufferId id) {
  const FrameFramebufferBinding* binding =
      forwardRasterFramebufferBinding(p, id);
  if (binding != nullptr && binding->renderPass != VK_NULL_HANDLE) {
    return binding->renderPass;
  }
  return VK_NULL_HANDLE;
}

[[nodiscard]] inline bool forwardRasterRenderPassReady(
    const FrameRecordParams& p, ForwardRasterFramebufferId id) {
  return forwardRasterRenderPass(p, id) != VK_NULL_HANDLE;
}

}  // namespace container::renderer
