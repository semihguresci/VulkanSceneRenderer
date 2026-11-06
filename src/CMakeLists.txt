add_library(VulkanSceneRenderer_Core
    ContainerCore.cpp
    app/Application.cpp
)

# ASL exchange validation stays independently testable without Vulkan startup.
add_library(VulkanSceneRenderer_asl_pose asl/PoseReplay.cpp asl/HandKinematics.cpp)
target_compile_features(VulkanSceneRenderer_asl_pose PUBLIC cxx_std_23)
target_include_directories(VulkanSceneRenderer_asl_pose PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(VulkanSceneRenderer_asl_pose PUBLIC Dep_Math nlohmann_json::nlohmann_json)

# CPU-only numerical processor shared by the workbench's provenance wrapper.
add_executable(ASLHandKinematicsProcessor asl/HandKinematicsProcessor.cpp)
set_target_properties(ASLHandKinematicsProcessor PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_link_libraries(ASLHandKinematicsProcessor PRIVATE VulkanSceneRenderer_asl_pose)
container_stage_runtime_dlls(ASLHandKinematicsProcessor)

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

# CPU light intents and selected receipts are shared by UI and renderer.
add_library(VulkanSceneRenderer_light_edit_support
    renderer/lighting/EditableLight.cpp
    renderer/lighting/LightEditReceipt.cpp
)
target_compile_features(VulkanSceneRenderer_light_edit_support PUBLIC cxx_std_23)
target_include_directories(VulkanSceneRenderer_light_edit_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(VulkanSceneRenderer_light_edit_support PUBLIC Dep_Math Dep_VulkanCore)

# Renderer subsystem
add_library(VulkanSceneRenderer_renderer
    asl/PoseReplayScene.cpp
    renderer/core/RenderPassManager.cpp
    renderer/core/CommandBufferScopeRecorder.cpp
    renderer/core/FrameRecorder.cpp
    renderer/core/RendererMsaa.cpp
    renderer/core/RendererFrontend.cpp
    renderer/core/RendererFrontendComfyUi.cpp
    renderer/core/ComfyUiBridge.cpp
    renderer/core/RendererFrontendReference.cpp
    renderer/core/RendererFrontendPhysics.cpp
    renderer/core/RendererFrontendEdits.cpp
    renderer/core/RendererFrontendRetirement.cpp
    renderer/core/RendererFrontendMerge.cpp
    renderer/core/RendererFrontendLights.cpp
    renderer/core/RendererFrontendMaterials.cpp
    renderer/core/RendererFrontendAnimation.cpp
    renderer/core/RendererFrontendInteractivity.cpp
    renderer/core/RendererFrontendCameras.cpp
    renderer/core/RendererFrontendCameraPoses.cpp
    renderer/core/RendererFrontendDeformation.cpp
    renderer/core/RendererFrontendDeformationTelemetry.cpp
    renderer/scene/SceneLightExport.cpp
    renderer/scene/GltfDeformationPass.cpp
    renderer/scene/GpuHierarchyPass.cpp
    renderer/raytracing/RayInstanceTransformPass.cpp
    renderer/scene/PbrAuxiliaryPass.cpp
    renderer/scene/GltfDeformationScene.cpp
    renderer/core/RendererTelemetry.cpp
    renderer/core/RenderGraph.cpp
    renderer/core/RenderPassGpuProfiler.cpp
    renderer/core/RendererMcpServer.cpp
    renderer/core/RendererMcpMeasurement.cpp
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
    renderer/pipeline/RuntimeShaderCompiler.cpp
    renderer/pipeline/PipelineRegistry.cpp

    renderer/lighting/EnvironmentManager.cpp
    renderer/lighting/AtmosphereLut.cpp
    renderer/lighting/LightGizmoIconAtlas.cpp
    renderer/lighting/DdgiProbeVolume.cpp
    renderer/lighting/DdgiSettings.cpp
    renderer/lighting/DdgiVolumeFit.cpp
    renderer/lighting/DdgiSceneBounds.cpp
    renderer/lighting/DdgiSceneChanges.cpp
    renderer/lighting/DdgiVolumePlacement.cpp
    renderer/lighting/DdgiVolumeScroll.cpp
    renderer/lighting/DdgiVolumeSet.cpp
    renderer/lighting/DdgiPriorityScheduler.cpp
    renderer/lighting/DdgiPreset.cpp
    renderer/lighting/DdgiManager.cpp
    renderer/debug/DdgiDebugRenderer.cpp
    renderer/lighting/LtcLutData.cpp
    renderer/lighting/LtcLutResources.cpp
    renderer/lighting/LightingManager.cpp

    renderer/effects/BloomManager.cpp
    renderer/effects/ExposureManager.cpp
    renderer/effects/OitManager.cpp
    renderer/effects/OpenVdbVolumeRenderer.cpp
    renderer/temporal/TemporalManager.cpp

    renderer/raytracing/RayScene.cpp
    renderer/raytracing/RaySceneExtraction.cpp
    renderer/raytracing/RaySceneAcceleration.cpp
    renderer/raytracing/RayShadowManager.cpp
    renderer/pathtracing/ReferencePathTracePass.cpp
    renderer/pathtracing/ReferencePathTracingTechnique.cpp

    renderer/culling/GpuCullManager.cpp

    renderer/debug/DebugOverlayRenderer.cpp
    renderer/debug/DebugUiPresenter.cpp
    app/GfxCapture.cpp

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
    renderer/bim/BimInspectionCache.cpp
    renderer/bim/BimStatisticsCache.cpp
    renderer/bim/BimObjectTransforms.cpp
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
find_package(Git QUIET)
set(CONTAINER_GIT_REVISION "unknown")
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE CONTAINER_GIT_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND "${GIT_EXECUTABLE}" diff --quiet HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" RESULT_VARIABLE CONTAINER_GIT_DIRTY)
    if(CONTAINER_GIT_DIRTY)
        string(APPEND CONTAINER_GIT_REVISION "-dirty")
    endif()
endif()
target_compile_definitions(VulkanSceneRenderer_renderer PRIVATE
    CONTAINER_GIT_REVISION="${CONTAINER_GIT_REVISION}"
    CONTAINER_BUILD_CONFIG="$<CONFIG>"
    CONTAINER_BUILD_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}"
)
target_include_directories(VulkanSceneRenderer_renderer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_link_libraries(VulkanSceneRenderer_renderer PUBLIC
    VulkanSceneRenderer_light_edit_support
    VulkanSceneRenderer_asl_pose
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
# CPU image decoding has no scene/UI dependency. Geometry image-source
# selection and GPU uploads share it without a geometry/utility cycle.
add_library(VulkanSceneRenderer_stb geometry/stb_impl.cpp)
target_compile_features(VulkanSceneRenderer_stb PUBLIC cxx_std_23)
target_link_libraries(VulkanSceneRenderer_stb PUBLIC Dep_SceneIO)
add_library(VulkanSceneRenderer_texture_codecs
    utility/TextureImageData.cpp utility/TextureUploadPlan.cpp)
target_compile_features(VulkanSceneRenderer_texture_codecs PUBLIC cxx_std_23)
target_include_directories(VulkanSceneRenderer_texture_codecs PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(VulkanSceneRenderer_texture_codecs PUBLIC Dep_VulkanCore
    PRIVATE Dep_TextureCodecs VulkanSceneRenderer_stb)

add_subdirectory(geometry)
add_subdirectory(utility)
add_subdirectory(ecs)

# One TinyEXR implementation shared by environment loading and volume export.
add_library(VulkanSceneRenderer_exr renderer/lighting/TinyExrImpl.cpp)
target_compile_features(VulkanSceneRenderer_exr PUBLIC cxx_std_23)
target_link_libraries(VulkanSceneRenderer_exr PUBLIC Dep_SceneIO)
if(TARGET miniz::miniz)
    target_link_libraries(VulkanSceneRenderer_exr PRIVATE miniz::miniz)
else()
    find_library(CONTAINER_TINYEXR_MINIZ_LIBRARY NAMES miniz REQUIRED)
    target_link_libraries(VulkanSceneRenderer_exr PRIVATE "${CONTAINER_TINYEXR_MINIZ_LIBRARY}")
endif()
target_link_libraries(VulkanSceneRenderer_renderer PRIVATE VulkanSceneRenderer_exr)
if(CONTAINER_VK_OIDN_AVAILABLE)
    target_link_libraries(VulkanSceneRenderer_renderer PRIVATE vk_oidn::vk_oidn)
    target_compile_definitions(VulkanSceneRenderer_renderer PRIVATE CONTAINER_HAS_VK_OIDN=1)
endif()
if(WIN32)
    target_link_libraries(VulkanSceneRenderer_renderer PRIVATE ws2_32)
endif()

# Standalone compute reference; the frame renderer retains its interactive path.
add_library(VulkanSceneRenderer_openvdb_transport_reference
    renderer/effects/OpenVdbTransportGpuReference.cpp)
target_compile_features(VulkanSceneRenderer_openvdb_transport_reference PUBLIC cxx_std_23)
target_include_directories(VulkanSceneRenderer_openvdb_transport_reference PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(VulkanSceneRenderer_openvdb_transport_reference PUBLIC
    VulkanSceneRenderer_openvdb VulkanSceneRenderer_gpu_resource
    VulkanSceneRenderer_geometry PRIVATE Dep_NanoVDB)

add_library(VulkanSceneRenderer_openvdb_transport_image
    geometry/OpenVdbTransportImage.cpp
    geometry/OpenVdbTransportImageScene.cpp
    renderer/effects/OpenVdbTransportImageGpu.cpp
    renderer/effects/OpenVdbTransportRenderJob.cpp)
target_compile_features(VulkanSceneRenderer_openvdb_transport_image PUBLIC cxx_std_23)
target_include_directories(VulkanSceneRenderer_openvdb_transport_image PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(VulkanSceneRenderer_openvdb_transport_image PUBLIC
    VulkanSceneRenderer_openvdb_transport_reference VulkanSceneRenderer_exr)
target_link_libraries(VulkanSceneRenderer_ui PRIVATE VulkanSceneRenderer_openvdb_transport_image)

add_executable(OpenVdbTransportRender ${CMAKE_SOURCE_DIR}/tools/openvdb_transport_render.cpp)
target_link_libraries(OpenVdbTransportRender PRIVATE VulkanSceneRenderer_openvdb_transport_image)
set_target_properties(OpenVdbTransportRender PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
container_stage_runtime_dlls(OpenVdbTransportRender)

if(ENABLE_PHYSICS)
    add_library(VulkanSceneRenderer_physics
        physics/PhysicsWorld.cpp
        physics/PhysicsScene.cpp
        physics/PhysicsElementBindings.cpp
    )
    target_compile_features(VulkanSceneRenderer_physics PUBLIC cxx_std_23)
    target_include_directories(VulkanSceneRenderer_physics PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    target_link_libraries(VulkanSceneRenderer_physics PUBLIC
        Dep_Math
    )
    target_link_libraries(VulkanSceneRenderer_physics PRIVATE
        Dep_Physics
        VulkanSceneRenderer_scene
    )
    target_link_libraries(VulkanSceneRenderer_renderer PRIVATE
        VulkanSceneRenderer_physics
    )
    target_compile_definitions(VulkanSceneRenderer_renderer PUBLIC
        CONTAINER_HAS_JOLT=1
    )
else()
    target_compile_definitions(VulkanSceneRenderer_renderer PUBLIC
        CONTAINER_HAS_JOLT=0
    )
endif()

target_link_libraries(VulkanSceneRenderer_Core PUBLIC
    VulkanSceneRenderer_renderer
    VulkanSceneRenderer_geometry
)

target_compile_features(VulkanSceneRenderer_Core PUBLIC cxx_std_23)
target_include_directories(VulkanSceneRenderer_Core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
