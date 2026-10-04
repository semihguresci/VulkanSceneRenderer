#include "Container/renderer/pipeline/PipelineRegistry.h"
#include "Container/renderer/pipeline/PipelineTypes.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <utility>

namespace container::renderer {

namespace {

void validateKey(const TechniquePipelineKey &key) {
  if (key.name.empty()) {
    throw std::invalid_argument("pipeline key name must not be empty");
  }
}

void validateHandle(const RegisteredPipelineHandle &handle) {
  validateKey(handle.key);
  if (handle.pipeline == VK_NULL_HANDLE) {
    throw std::invalid_argument("registered pipeline handle must not be null");
  }
}

void validateLayout(const RegisteredPipelineLayout &layout) {
  validateKey(layout.key);
  if (layout.layout == VK_NULL_HANDLE) {
    throw std::invalid_argument("registered pipeline layout must not be null");
  }
}

void registerIfPresent(PipelineRegistry &registry, RenderTechniqueId technique,
                       const char *name, VkPipeline pipeline) {
  if (pipeline == VK_NULL_HANDLE) {
    return;
  }

  registry.registerHandle(
      RegisteredPipelineHandle{.key = {technique, name}, .pipeline = pipeline});
}

void registerIfPresent(PipelineRegistry &registry, const char *name,
                       VkPipeline pipeline) {
  registerIfPresent(registry, RenderTechniqueId::DeferredRaster, name,
                    pipeline);
}

void registerLayoutIfPresent(PipelineRegistry &registry,
                             RenderTechniqueId technique, const char *name,
                             VkPipelineLayout layout) {
  if (layout == VK_NULL_HANDLE) {
    return;
  }

  registry.registerLayout(
      RegisteredPipelineLayout{.key = {technique, name}, .layout = layout});
}

void registerLayoutIfPresent(PipelineRegistry &registry, const char *name,
                             VkPipelineLayout layout) {
  registerLayoutIfPresent(registry, RenderTechniqueId::DeferredRaster, name,
                          layout);
}

} // namespace

void PipelineRegistry::registerRecipe(PipelineRecipe recipe) {
  validateKey(recipe.key);
  if (contains(recipe.key)) {
    throw std::invalid_argument("pipeline recipe is already registered");
  }
  recipes_.push_back(std::move(recipe));
}

void PipelineRegistry::registerHandle(RegisteredPipelineHandle handle) {
  validateHandle(handle);
  if (findHandle(handle.key) != nullptr) {
    throw std::invalid_argument("pipeline handle is already registered");
  }
  handles_.push_back(std::move(handle));
}

void PipelineRegistry::registerLayout(RegisteredPipelineLayout layout) {
  validateLayout(layout);
  if (findLayout(layout.key) != nullptr) {
    throw std::invalid_argument("pipeline layout is already registered");
  }
  layouts_.push_back(std::move(layout));
}

const PipelineRecipe *
PipelineRegistry::find(const TechniquePipelineKey &key) const {
  const std::size_t index = findIndex(key);
  return index == recipes_.size() ? nullptr : &recipes_[index];
}

const RegisteredPipelineHandle *
PipelineRegistry::findHandle(const TechniquePipelineKey &key) const {
  const std::size_t index = findHandleIndex(key);
  return index == handles_.size() ? nullptr : &handles_[index];
}

const RegisteredPipelineLayout *
PipelineRegistry::findLayout(const TechniquePipelineKey &key) const {
  const std::size_t index = findLayoutIndex(key);
  return index == layouts_.size() ? nullptr : &layouts_[index];
}

VkPipeline
PipelineRegistry::pipelineHandle(const TechniquePipelineKey &key) const {
  const RegisteredPipelineHandle *handle = findHandle(key);
  return handle ? handle->pipeline : VK_NULL_HANDLE;
}

VkPipelineLayout
PipelineRegistry::pipelineLayout(const TechniquePipelineKey &key) const {
  const RegisteredPipelineLayout *layout = findLayout(key);
  return layout ? layout->layout : VK_NULL_HANDLE;
}

std::vector<const PipelineRecipe *>
PipelineRegistry::recipesForTechnique(RenderTechniqueId technique) const {
  std::vector<const PipelineRecipe *> matches;
  for (const PipelineRecipe &recipe : recipes_) {
    if (recipe.key.technique == technique) {
      matches.push_back(&recipe);
    }
  }
  return matches;
}

std::vector<const RegisteredPipelineHandle *>
PipelineRegistry::pipelineHandlesForTechnique(
    RenderTechniqueId technique) const {
  std::vector<const RegisteredPipelineHandle *> matches;
  for (const RegisteredPipelineHandle &handle : handles_) {
    if (handle.key.technique == technique) {
      matches.push_back(&handle);
    }
  }
  return matches;
}

std::vector<const RegisteredPipelineLayout *>
PipelineRegistry::pipelineLayoutsForTechnique(
    RenderTechniqueId technique) const {
  std::vector<const RegisteredPipelineLayout *> matches;
  for (const RegisteredPipelineLayout &layout : layouts_) {
    if (layout.key.technique == technique) {
      matches.push_back(&layout);
    }
  }
  return matches;
}

void PipelineRegistry::clearHandles() { handles_.clear(); }

void PipelineRegistry::clearLayouts() { layouts_.clear(); }

void PipelineRegistry::clearTechnique(RenderTechniqueId technique) {
  std::erase_if(recipes_, [technique](const PipelineRecipe &recipe) {
    return recipe.key.technique == technique;
  });
  std::erase_if(handles_, [technique](const RegisteredPipelineHandle &handle) {
    return handle.key.technique == technique;
  });
  std::erase_if(layouts_, [technique](const RegisteredPipelineLayout &layout) {
    return layout.key.technique == technique;
  });
}

void PipelineRegistry::clear() {
  recipes_.clear();
  handles_.clear();
  layouts_.clear();
}

std::size_t PipelineRegistry::findIndex(const TechniquePipelineKey &key) const {
  const auto it = std::find_if(
      recipes_.begin(), recipes_.end(),
      [&key](const PipelineRecipe &recipe) { return recipe.key == key; });
  if (it == recipes_.end()) {
    return recipes_.size();
  }
  return static_cast<std::size_t>(std::distance(recipes_.begin(), it));
}

std::size_t
PipelineRegistry::findHandleIndex(const TechniquePipelineKey &key) const {
  const auto it = std::find_if(handles_.begin(), handles_.end(),
                               [&key](const RegisteredPipelineHandle &handle) {
                                 return handle.key == key;
                               });
  if (it == handles_.end()) {
    return handles_.size();
  }
  return static_cast<std::size_t>(std::distance(handles_.begin(), it));
}

std::size_t
PipelineRegistry::findLayoutIndex(const TechniquePipelineKey &key) const {
  const auto it = std::find_if(layouts_.begin(), layouts_.end(),
                               [&key](const RegisteredPipelineLayout &layout) {
                                 return layout.key == key;
                               });
  if (it == layouts_.end()) {
    return layouts_.size();
  }
  return static_cast<std::size_t>(std::distance(layouts_.begin(), it));
}

std::shared_ptr<const PipelineRegistry>
buildGraphicsPipelineHandleRegistry(const GraphicsPipelines &pipelines) {
  auto registry = std::make_shared<PipelineRegistry>();

  registerIfPresent(*registry, "depth-prepass", pipelines.depthPrepass);
  registerIfPresent(*registry, "depth-prepass-front-cull",
                    pipelines.depthPrepassFrontCull);
  registerIfPresent(*registry, "depth-prepass-no-cull",
                    pipelines.depthPrepassNoCull);
  registerIfPresent(*registry, "bim-depth-prepass", pipelines.bimDepthPrepass);
  registerIfPresent(*registry, "bim-depth-prepass-front-cull",
                    pipelines.bimDepthPrepassFrontCull);
  registerIfPresent(*registry, "bim-depth-prepass-no-cull",
                    pipelines.bimDepthPrepassNoCull);
  registerIfPresent(*registry, "gbuffer", pipelines.gBuffer);
  registerIfPresent(*registry, "gbuffer-front-cull",
                    pipelines.gBufferFrontCull);
  registerIfPresent(*registry, "gbuffer-no-cull", pipelines.gBufferNoCull);
  registerIfPresent(*registry, "bim-gbuffer", pipelines.bimGBuffer);
  registerIfPresent(*registry, "bim-gbuffer-front-cull",
                    pipelines.bimGBufferFrontCull);
  registerIfPresent(*registry, "bim-gbuffer-no-cull",
                    pipelines.bimGBufferNoCull);
  registerIfPresent(*registry, "shadow-depth", pipelines.shadowDepth);
  registerIfPresent(*registry, "shadow-depth-front-cull",
                    pipelines.shadowDepthFrontCull);
  registerIfPresent(*registry, "shadow-depth-no-cull",
                    pipelines.shadowDepthNoCull);
  registerIfPresent(*registry, "local-shadow-depth",
                    pipelines.localShadowDepth);
  registerIfPresent(*registry, "local-shadow-depth-front-cull",
                    pipelines.localShadowDepthFrontCull);
  registerIfPresent(*registry, "local-shadow-depth-no-cull",
                    pipelines.localShadowDepthNoCull);
  registerIfPresent(*registry, "lighting", pipelines.directionalLight);
  registerIfPresent(*registry, "stencil-volume", pipelines.stencilVolume);
  registerIfPresent(*registry, "point-light", pipelines.pointLight);
  registerIfPresent(*registry, "point-light-stencil-debug",
                    pipelines.pointLightStencilDebug);
  registerIfPresent(*registry, "tiled-point-light", pipelines.tiledPointLight);
  registerIfPresent(*registry, "transparent", pipelines.transparent);
  registerIfPresent(*registry, "transparent-front-cull",
                    pipelines.transparentFrontCull);
  registerIfPresent(*registry, "transparent-no-cull",
                    pipelines.transparentNoCull);
  registerIfPresent(*registry, "transparent-pick", pipelines.transparentPick);
  registerIfPresent(*registry, "transparent-pick-front-cull",
                    pipelines.transparentPickFrontCull);
  registerIfPresent(*registry, "transparent-pick-no-cull",
                    pipelines.transparentPickNoCull);
  registerIfPresent(*registry, "post-process", pipelines.postProcess);
  registerIfPresent(*registry, "geometry-debug", pipelines.geometryDebug);
  registerIfPresent(*registry, "normal-validation", pipelines.normalValidation);
  registerIfPresent(*registry, "normal-validation-front-cull",
                    pipelines.normalValidationFrontCull);
  registerIfPresent(*registry, "normal-validation-no-cull",
                    pipelines.normalValidationNoCull);
  registerIfPresent(*registry, "wireframe-depth", pipelines.wireframeDepth);
  registerIfPresent(*registry, "wireframe-depth-front-cull",
                    pipelines.wireframeDepthFrontCull);
  registerIfPresent(*registry, "wireframe-no-depth",
                    pipelines.wireframeNoDepth);
  registerIfPresent(*registry, "wireframe-no-depth-front-cull",
                    pipelines.wireframeNoDepthFrontCull);
  registerIfPresent(*registry, "selection-mask", pipelines.selectionMask);
  registerIfPresent(*registry, "selection-outline", pipelines.selectionOutline);
  registerIfPresent(*registry, "bim-floor-plan-depth",
                    pipelines.bimFloorPlanDepth);
  registerIfPresent(*registry, "bim-floor-plan-no-depth",
                    pipelines.bimFloorPlanNoDepth);
  registerIfPresent(*registry, "bim-point-cloud-depth",
                    pipelines.bimPointCloudDepth);
  registerIfPresent(*registry, "bim-point-cloud-no-depth",
                    pipelines.bimPointCloudNoDepth);
  registerIfPresent(*registry, "bim-curve-depth", pipelines.bimCurveDepth);
  registerIfPresent(*registry, "bim-curve-no-depth", pipelines.bimCurveNoDepth);
  registerIfPresent(*registry, "bim-section-clip-cap-fill",
                    pipelines.bimSectionClipCapFill);
  registerIfPresent(*registry, "bim-section-clip-cap-hatch",
                    pipelines.bimSectionClipCapHatch);
  registerIfPresent(*registry, "surface-normal-line",
                    pipelines.surfaceNormalLine);
  registerIfPresent(*registry, "object-normal-debug",
                    pipelines.objectNormalDebug);
  registerIfPresent(*registry, "object-normal-debug-front-cull",
                    pipelines.objectNormalDebugFrontCull);
  registerIfPresent(*registry, "object-normal-debug-no-cull",
                    pipelines.objectNormalDebugNoCull);
  registerIfPresent(*registry, "light-gizmo", pipelines.lightGizmo);
  registerIfPresent(*registry, "light-gizmo-coverage",
                    pipelines.lightGizmoCoverage);
  registerIfPresent(*registry, "light-gizmo-pick", pipelines.lightGizmoPick);
  registerIfPresent(*registry, "transform-gizmo", pipelines.transformGizmo);
  registerIfPresent(*registry, "transform-gizmo-solid",
                    pipelines.transformGizmoSolid);
  registerIfPresent(*registry, "transform-gizmo-overlay",
                    pipelines.transformGizmoOverlay);
  registerIfPresent(*registry, "transform-gizmo-solid-overlay",
                    pipelines.transformGizmoSolidOverlay);
  registerIfPresent(*registry, RenderTechniqueId::ForwardRaster,
                    "depth-prepass", pipelines.depthPrepass);
  registerIfPresent(*registry, RenderTechniqueId::ForwardRaster,
                    "depth-prepass-front-cull",
                    pipelines.depthPrepassFrontCull);
  registerIfPresent(*registry, RenderTechniqueId::ForwardRaster,
                    "depth-prepass-no-cull", pipelines.depthPrepassNoCull);
  registerIfPresent(*registry, RenderTechniqueId::ForwardRaster,
                    "bim-depth-prepass", pipelines.bimDepthPrepass);
  registerIfPresent(*registry, RenderTechniqueId::ForwardRaster,
                    "bim-depth-prepass-front-cull",
                    pipelines.bimDepthPrepassFrontCull);
  registerIfPresent(*registry, RenderTechniqueId::ForwardRaster,
                    "bim-depth-prepass-no-cull",
                    pipelines.bimDepthPrepassNoCull);
  registerIfPresent(*registry, RenderTechniqueId::ForwardRaster,
                    "forward-transparent", pipelines.transparent);
  registerIfPresent(*registry, RenderTechniqueId::ForwardRaster,
                    "forward-transparent-front-cull",
                    pipelines.transparentFrontCull);
  registerIfPresent(*registry, RenderTechniqueId::ForwardRaster,
                    "forward-transparent-no-cull", pipelines.transparentNoCull);
  registerIfPresent(*registry, RenderTechniqueId::ForwardRaster,
                    "bim-point-cloud-depth", pipelines.bimPointCloudDepth);
  registerIfPresent(*registry, RenderTechniqueId::ForwardRaster,
                    "bim-point-cloud-no-depth", pipelines.bimPointCloudNoDepth);
  registerIfPresent(*registry, RenderTechniqueId::ForwardRaster,
                    "bim-curve-depth", pipelines.bimCurveDepth);
  registerIfPresent(*registry, RenderTechniqueId::ForwardRaster,
                    "bim-curve-no-depth", pipelines.bimCurveNoDepth);
  registerIfPresent(*registry, RenderTechniqueId::ForwardRaster, "post-process",
                    pipelines.postProcess);
  registerIfPresent(*registry, RenderTechniqueId::ForwardRaster, "light-gizmo",
                    pipelines.lightGizmo);
  registerIfPresent(*registry, RenderTechniqueId::ForwardRaster,
                    "light-gizmo-coverage", pipelines.lightGizmoCoverage);
  registerIfPresent(*registry, RenderTechniqueId::ForwardRaster,
                    "transform-gizmo", pipelines.transformGizmo);
  registerIfPresent(*registry, RenderTechniqueId::ForwardRaster,
                    "transform-gizmo-solid", pipelines.transformGizmoSolid);
  registerIfPresent(*registry, RenderTechniqueId::ForwardRaster,
                    "transform-gizmo-overlay", pipelines.transformGizmoOverlay);
  registerIfPresent(*registry, RenderTechniqueId::ForwardRaster,
                    "transform-gizmo-solid-overlay",
                    pipelines.transformGizmoSolidOverlay);
  for (const RegisteredPipelineHandle &handle : pipelines.extraHandles) {
    registry->registerHandle(handle);
  }

  return registry;
}

std::shared_ptr<const PipelineRegistry>
buildGraphicsPipelineLayoutRegistry(const PipelineLayouts &layouts) {
  auto registry = std::make_shared<PipelineRegistry>();

  registerLayoutIfPresent(*registry, "scene", layouts.scene);
  registerLayoutIfPresent(*registry, RenderTechniqueId::ForwardRaster, "scene",
                          layouts.scene);
  registerLayoutIfPresent(*registry, "transparent", layouts.transparent);
  registerLayoutIfPresent(*registry, RenderTechniqueId::ForwardRaster,
                          "transparent", layouts.transparent);
  registerLayoutIfPresent(*registry, "lighting", layouts.lighting);
  registerLayoutIfPresent(*registry, "light-gizmo", layouts.lightGizmo);
  registerLayoutIfPresent(*registry, RenderTechniqueId::ForwardRaster,
                          "light-gizmo", layouts.lightGizmo);
  registerLayoutIfPresent(*registry, "tiled-lighting", layouts.tiledLighting);
  registerLayoutIfPresent(*registry, "shadow", layouts.shadow);
  registerLayoutIfPresent(*registry, "post-process", layouts.postProcess);
  registerLayoutIfPresent(*registry, RenderTechniqueId::ForwardRaster,
                          "post-process", layouts.postProcess);
  registerLayoutIfPresent(*registry, "wireframe", layouts.wireframe);
  registerLayoutIfPresent(*registry, RenderTechniqueId::ForwardRaster,
                          "wireframe", layouts.wireframe);
  registerLayoutIfPresent(*registry, "normal-validation",
                          layouts.normalValidation);
  registerLayoutIfPresent(*registry, "surface-normal", layouts.surfaceNormal);
  registerLayoutIfPresent(*registry, "transform-gizmo", layouts.transformGizmo);
  registerLayoutIfPresent(*registry, RenderTechniqueId::ForwardRaster,
                          "transform-gizmo", layouts.transformGizmo);
  for (const RegisteredPipelineLayout &layout : layouts.extraLayouts) {
    registry->registerLayout(layout);
  }

  return registry;
}

} // namespace container::renderer
