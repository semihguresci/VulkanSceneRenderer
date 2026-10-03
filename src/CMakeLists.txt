add_library(VulkanSceneRenderer_Core
    ContainerCore.cpp
    app/Application.cpp
)

add_library(VulkanSceneRenderer_bim_ui_support
    renderer/bim/BimMeasurementSnapping.cpp
    renderer/bim/BimModelCompare.cpp
)
target_compile_features(VulkanSceneRenderer_bim_ui_support PUBLIC cxx_std_23)
target_include_directories(VulkanSceneRenderer_bim_ui_support PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_link_libraries(VulkanSceneRenderer_bim_ui_support PUBLIC
    Dep_Math
)

# Renderer subsystem
add_library(VulkanSceneRenderer_renderer
    renderer/core/RenderPassManager.cpp
    renderer/core/CommandBufferScopeRecorder.cpp
    renderer/core/FrameRecorder.cpp
    renderer/core/RendererMsaa.cpp
    renderer/core/RendererFrontend.cpp
    renderer/core/RendererTelemetry.cpp
    renderer/core/RenderGraph.cpp
    renderer/core/RenderPassGpuProfiler.cpp
    renderer/core/RenderPassScopeRecorder.cpp
    renderer/core/RenderTechnique.cpp
    renderer/core/ScreenshotCaptureRecorder.cpp

    renderer/resources/CommandBufferManager.cpp
    renderer/resources/FrameResourceManager.cpp
    renderer/resources/FrameResourceRegistry.cpp

    renderer/platform/VulkanContext.cpp
    renderer/platform/VulkanContextInitializer.cpp
    renderer/platform/WindowInputBridge.cpp

    renderer/pipeline/GraphicsPipelineBuilder.cpp
    renderer/pipeline/PipelineRegistry.cpp

    renderer/lighting/EnvironmentManager.cpp
    renderer/lighting/EditableLight.cpp
    renderer/lighting/LightGizmoIconAtlas.cpp
    renderer/lighting/LightingManager.cpp
    renderer/lighting/TinyExrImpl.cpp

    renderer/effects/BloomManager.cpp
    renderer/effects/ExposureManager.cpp
    renderer/effects/OitManager.cpp
    renderer/temporal/TemporalManager.cpp

    renderer/culling/GpuCullManager.cpp

    renderer/debug/DebugOverlayRenderer.cpp
    renderer/debug/DebugUiPresenter.cpp

    renderer/scene/CameraController.cpp
    renderer/scene/SceneController.cpp
    renderer/scene/SceneDiagnosticCubeRecorder.cpp
    renderer/scene/ScenePrimitives.cpp
    renderer/scene/SceneViewport.cpp
    renderer/scene/SceneOpaqueDrawPlanner.cpp
    renderer/scene/SceneOpaqueDrawRecorder.cpp
    renderer/scene/SceneRasterPassPlanner.cpp
    renderer/scene/SceneRasterPassRecorder.cpp
    renderer/scene/SceneTransparentDrawPlanner.cpp
    renderer/scene/SceneTransparentDrawRecorder.cpp

    renderer/picking/RenderSurfaceInteractionController.cpp
    renderer/picking/TransparentPickDepthCopyRecorder.cpp
    renderer/picking/TransparentPickPassRecorder.cpp
    renderer/picking/TransparentPickRasterPassRecorder.cpp

    renderer/deferred/DeferredDirectionalLightingRecorder.cpp
    renderer/deferred/DeferredLightGizmoPlanner.cpp
    renderer/deferred/DeferredLightGizmoRecorder.cpp
    renderer/deferred/DeferredLightingDescriptorPlanner.cpp
    renderer/deferred/DeferredLightingPassPlanner.cpp
    renderer/deferred/DeferredPointLightingDrawPlanner.cpp
    renderer/deferred/DeferredPointLightingRecorder.cpp
    renderer/deferred/DeferredRasterDebugOverlayPlanner.cpp
    renderer/deferred/DeferredRasterDebugOverlayRecorder.cpp
    renderer/deferred/DeferredRasterBimSurfacePassRecorder.cpp
    renderer/deferred/DeferredRasterDepthReadOnlyTransitionRecorder.cpp
    renderer/deferred/DeferredRasterFrameGraphContext.cpp
    renderer/deferred/DeferredRasterFrameState.cpp
    renderer/deferred/DeferredRasterFrustumCullPassPlanner.cpp
    renderer/deferred/DeferredRasterFrustumCullPassRecorder.cpp
    renderer/deferred/DeferredRasterGuiPassRecorder.cpp
    renderer/deferred/DeferredRasterHiZDepthTransitionRecorder.cpp
    renderer/deferred/DeferredRasterHiZPassPlanner.cpp
    renderer/deferred/DeferredRasterImageBarrier.cpp
    renderer/deferred/DeferredRasterLighting.cpp
    renderer/deferred/DeferredRasterLightingPassRecorder.cpp
    renderer/deferred/DeferredRasterPostProcess.cpp
    renderer/deferred/DeferredRasterSceneColorReadBarrierRecorder.cpp
    renderer/deferred/DeferredRasterScenePassRecorder.cpp
    renderer/deferred/DeferredRasterTechnique.cpp
    renderer/deferred/DeferredRasterTileCullPlanner.cpp
    renderer/deferred/DeferredRasterTileCullRecorder.cpp
    renderer/deferred/DeferredRasterTransformGizmo.cpp
    renderer/deferred/DeferredTransparentOitFramePassRecorder.cpp
    renderer/deferred/DeferredTransparentOitRecorder.cpp

    renderer/forward/ForwardRasterLightingPassRecorder.cpp
    renderer/forward/ForwardRasterTechnique.cpp

    renderer/bim/BimCoordinationOverlay.cpp
    renderer/bim/BimDrawFilterState.cpp
    renderer/bim/BimDrawingExport.cpp
    renderer/bim/BimDrawCompactionPlanner.cpp
    renderer/bim/BimFrameDrawRoutingPlanner.cpp
    renderer/bim/BimFrameGpuVisibilityRecorder.cpp
    renderer/bim/BimGeoreferenceTransform.cpp
    renderer/bim/BimLightingOverlayPlanner.cpp
    renderer/bim/BimLightingOverlayRecorder.cpp
    renderer/bim/BimManager.cpp
    renderer/bim/BimMetadataCatalog.cpp
    renderer/bim/BimMetadataIndex.cpp
    renderer/bim/BimPrimitivePassPlanner.cpp
    renderer/bim/BimPrimitivePassRecorder.cpp
    renderer/bim/BimRelationshipGraph.cpp
    renderer/bim/BimScheduleExtractor.cpp
    renderer/bim/BimSectionCapBuilder.cpp
    renderer/bim/BimSectionClipCapPassPlanner.cpp
    renderer/bim/BimSectionClipCapPassRecorder.cpp
    renderer/bim/BimSurfaceDrawRoutingPlanner.cpp
    renderer/bim/BimSurfacePassPlanner.cpp
    renderer/bim/BimSurfacePassRecorder.cpp
    renderer/bim/BimSurfaceRasterPassRecorder.cpp

    renderer/shadow/ShadowCascadeDrawPlanner.cpp
    renderer/shadow/ShadowCascadeDepthPlanner.cpp
    renderer/shadow/ShadowCascadeFramePassRecorder.cpp
    renderer/shadow/ShadowCascadeGpuCullPlanner.cpp
    renderer/shadow/ShadowCascadePreparationPlanner.cpp
    renderer/shadow/ShadowCascadeSecondaryCommandBufferRecorder.cpp
    renderer/shadow/ShadowCullPassPlanner.cpp
    renderer/shadow/ShadowCullPassRecorder.cpp
    renderer/shadow/ShadowPassDrawPlanner.cpp
    renderer/shadow/ShadowPassRasterPlanner.cpp
    renderer/shadow/ShadowPassRasterRecorder.cpp
    renderer/shadow/ShadowPassScopePlanner.cpp
    renderer/shadow/ShadowSecondaryCommandBufferPlanner.cpp
    renderer/shadow/ShadowPassRecorder.cpp
    renderer/shadow/ShadowCullManager.cpp
    renderer/shadow/ShadowManager.cpp
)

target_compile_features(VulkanSceneRenderer_renderer PUBLIC cxx_std_23)
target_include_directories(VulkanSceneRenderer_renderer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_link_libraries(VulkanSceneRenderer_renderer PUBLIC
    Dep_VulkanCore
    Dep_Math
    Dep_SceneIO
    VulkanSceneRenderer_log
    VulkanSceneRenderer_vulkan_device
    VulkanSceneRenderer_gpu_resource
    VulkanSceneRenderer_window
    VulkanSceneRenderer_scene
    VulkanSceneRenderer_ui
    VulkanSceneRenderer_bim_ui_support
    VulkanSceneRenderer_ecs
)

# Add subdirectories
add_subdirectory(geometry)
add_subdirectory(utility)
add_subdirectory(ecs)

target_link_libraries(VulkanSceneRenderer_Core PUBLIC
    VulkanSceneRenderer_renderer
    VulkanSceneRenderer_geometry
)

target_compile_features(VulkanSceneRenderer_Core PUBLIC cxx_std_23)
target_include_directories(VulkanSceneRenderer_Core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
