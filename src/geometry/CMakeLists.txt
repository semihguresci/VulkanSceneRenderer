# Geometry component
# Keep OpenVDB's template-heavy implementation isolated from other importers.
find_package(Threads REQUIRED)
add_library(VulkanSceneRenderer_nanovdb_io OpenVdbNanoInterchange.cpp OpenVdbNanoStatistics.cpp OpenVdbNanoPrimitives.cpp)
target_compile_features(VulkanSceneRenderer_nanovdb_io PUBLIC cxx_std_23)
target_include_directories(VulkanSceneRenderer_nanovdb_io PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/)
target_link_libraries(VulkanSceneRenderer_nanovdb_io PRIVATE Dep_OpenVDB Dep_NanoVDB_IO)
add_library(VulkanSceneRenderer_openvdb
    OpenVdbLoader.cpp
    OpenVdbVolume.cpp
    OpenVdbVolumeSequence.cpp
    OpenVdbPoints.cpp
    OpenVdbPointPreview.cpp
    OpenVdbVectorPreview.cpp
    OpenVdbPointAttributes.cpp
    OpenVdbPointSelection.cpp
    OpenVdbNativePointPreview.cpp
    OpenVdbSlicePreview.cpp
    OpenVdbTransportReference.cpp
)
target_compile_features(VulkanSceneRenderer_openvdb PUBLIC cxx_std_23)
target_include_directories(VulkanSceneRenderer_openvdb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/)
target_link_libraries(VulkanSceneRenderer_openvdb PUBLIC
    Dep_VulkanCore Dep_Math Threads::Threads PRIVATE Dep_OpenVDB Dep_NanoVDB
    nlohmann_json::nlohmann_json)

add_library(VulkanSceneRenderer_geometry
    DotBimLoader.cpp
    GltfCapabilities.cpp
    GltfLinkedAssets.cpp
    GltfXmpMetadata.cpp
    GltfSky.cpp
    GltfAccessor.cpp
    GltfCompression.cpp
    GltfDracoWorker.cpp
    GltfDracoProtocol.cpp
    GltfImageSources.cpp
    GltfTextureResourceReferences.cpp
    GltfInstancing.cpp
    GltfNodeFlags.cpp
    GltfSceneCatalog.cpp
    GltfMerge.cpp
    GltfSessionMerge.cpp
    GltfMergeRuntimeData.cpp
    GltfMergeGeometry.cpp
    GltfNodeExportCompaction.cpp
    GltfMaterialExportCompaction.cpp
    GltfResourceExportCompaction.cpp
    GltfSubtreeClone.cpp
    GltfProjectedSubtreeClone.cpp
    ../utility/SceneNodeName.cpp
    GltfExtendedMaterials.cpp
    GltfIesProfiles.cpp
    GltfOpacityMicromaps.cpp
    GltfMaterialVariants.cpp
    GltfModelLoader.cpp
    GltfSceneExport.cpp
    GltfMaterialExport.cpp
    GltfLegacyMaterialSerialization.cpp
    GltfMaterialAuthoring.cpp
    GltfCameraAuthoring.cpp
    GltfCameraPoseAuthoring.cpp
    GltfMaterialTextureAuthoring.cpp
    GltfPrimitiveFrameVariants.cpp
    GltfAnimation.cpp
    GltfInteractivity.cpp
    GltfAnimationRuntime.cpp
    GltfCameraEvaluation.cpp
    GltfAnimatedMaterial.cpp
    GltfDeformation.cpp
    GltfDeformationFrames.cpp
    IfcxLoader.cpp
    IfcTessellatedLoader.cpp
    IfcCurveReader.cpp
    IfcBrepFaceGeometry.cpp
    Mesh.cpp
    Model.cpp
    PolylineGeometry.cpp
    ParametricCurve.cpp
    SweptDiskGeometry.cpp
    UsdLoader.cpp
    tinygltf_impl.cpp
    vma_impl.cpp)

target_compile_features(VulkanSceneRenderer_geometry PUBLIC cxx_std_23)


target_include_directories(VulkanSceneRenderer_geometry PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../include/
)

target_link_libraries(VulkanSceneRenderer_geometry PUBLIC
    Dep_VulkanCore
    Dep_Math
    Dep_SceneIO
    nlohmann_json::nlohmann_json
    VulkanSceneRenderer_openvdb
    VulkanSceneRenderer_texture_codecs
    VulkanSceneRenderer_stb
    PRIVATE earcut_hpp::earcut_hpp
    manifold::manifold
    Dep_GltfCompression
)

if(ENABLE_GLTF_DRACO)
    if(NOT WIN32)
        message(FATAL_ERROR "ENABLE_GLTF_DRACO currently requires the Windows bounded worker launcher")
    endif()
    add_executable(ContainerDracoWorker
        GltfDracoWorkerMain.cpp GltfDracoProtocol.cpp)
    target_compile_features(ContainerDracoWorker PRIVATE cxx_std_23)
    target_include_directories(ContainerDracoWorker PRIVATE
        ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(ContainerDracoWorker PRIVATE draco::draco)
    target_compile_definitions(ContainerDracoWorker PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_target_properties(ContainerDracoWorker PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    container_stage_runtime_dlls(ContainerDracoWorker)
endif()

add_library(VulkanSceneRenderer_openvdb_processing
    OpenVdbProcessing.cpp
    OpenVdbParticles.cpp
    OpenVdbAdvection.cpp
    OpenVdbSegmentation.cpp
    OpenVdbMeshExport.cpp
    OpenVdbInspect.cpp
    OpenVdbPointProcessing.cpp
    OpenVdbPointRasterization.cpp
    OpenVdbPointAttributesJson.cpp
    OpenVdbDenseInterchange.cpp
    OpenVdbMultiResolution.cpp
    OpenVdbFileAuthoring.cpp
    OpenVdbFileAuthoringSpec.cpp
    OpenVdbFieldFilter.cpp
    OpenVdbFieldComposite.cpp
    OpenVdbPointMask.cpp
    OpenVdbTopology.cpp
    OpenVdbPointFieldSampling.cpp
    OpenVdbVolumeSpheres.cpp
    OpenVdbFastSweeping.cpp
    OpenVdbFastSweepingFloat.cpp
    OpenVdbFastSweepingDouble.cpp
    OpenVdbRegions.cpp
    OpenVdbRegionQueries.cpp
    OpenVdbRayQueries.cpp
    OpenVdbFogRayIntegrals.cpp
    OpenVdbValueComponents.cpp
    OpenVdbSurfaceReference.cpp
    OpenVdbParticleGeometry.cpp
    OpenVdbBoundaryFluidStep.cpp
    OpenVdbSurfaceQueries.cpp
    OpenVdbFracture.cpp
    OpenVdbFieldAdvection.cpp
    OpenVdbPotentialFlow.cpp
    OpenVdbPointNeighbors.cpp
    OpenVdbLevelSetRebuild.cpp
    OpenVdbSurfaceFields.cpp
    OpenVdbPointMotion.cpp
    OpenVdbNativePointAdvection.cpp
    OpenVdbPointTransfer.cpp
    OpenVdbFieldResampling.cpp
    OpenVdbDiagnostics.cpp
    OpenVdbBoundaryProjection.cpp
    OpenVdbPrimitives.cpp
    OpenVdbVectorProcessing.cpp
    OpenVdbFluidProjection.cpp
    OpenVdbFluidSimulation.cpp
    OpenVdbFluidSequence.cpp
    OpenVdbFluidViscosity.cpp
    OpenVdbFluidSources.cpp
    OpenVdbLevelSetTransport.cpp
    OpenVdbMeasure.cpp
    OpenVdbMeshConversion.cpp
    OpenVdbPoisson.cpp)
target_compile_features(VulkanSceneRenderer_openvdb_processing PUBLIC cxx_std_23)
target_link_libraries(VulkanSceneRenderer_openvdb_processing
    PUBLIC VulkanSceneRenderer_geometry VulkanSceneRenderer_nanovdb_io PRIVATE Dep_OpenVDB VulkanSceneRenderer_exr)

add_executable(OpenVdbTool ${CMAKE_SOURCE_DIR}/tools/openvdb_process.cpp)
if(ENABLE_OPENVDB)
    target_compile_definitions(OpenVdbTool PRIVATE CONTAINER_OPENVDB_BUILD_VERSION="${OpenVDB_VERSION}")
endif()
target_link_libraries(OpenVdbTool PRIVATE VulkanSceneRenderer_openvdb_processing)
set_target_properties(OpenVdbTool PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
container_stage_runtime_dlls(OpenVdbTool)
