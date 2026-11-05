# Geometry component
add_library(VulkanSceneRenderer_geometry
    DotBimLoader.cpp
    GltfAccessor.cpp
    GltfCapabilities.cpp
    GltfCompression.cpp
    GltfDracoWorker.cpp
    GltfDracoProtocol.cpp
    GltfImageSources.cpp
    GltfInstancing.cpp
    GltfNodeFlags.cpp
    GltfExtendedMaterials.cpp
    GltfMaterialTextureAuthoring.cpp
    GltfAnimation.cpp
    GltfAnimationRuntime.cpp
    GltfAnimatedMaterial.cpp
    GltfCameraEvaluation.cpp
    GltfDeformation.cpp
    GltfDeformationFrames.cpp
    GltfModelLoader.cpp
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
    vma_impl.cpp
    )

target_compile_features(VulkanSceneRenderer_geometry PUBLIC cxx_std_23)


target_include_directories(VulkanSceneRenderer_geometry PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../include/
)

target_link_libraries(VulkanSceneRenderer_geometry PUBLIC
    Dep_VulkanCore
    Dep_Math
    Dep_SceneIO
    nlohmann_json::nlohmann_json
    VulkanSceneRenderer_texture_codecs
    Dep_GltfCompression
    PRIVATE earcut_hpp::earcut_hpp
    manifold::manifold
)

target_link_libraries(VulkanSceneRenderer_geometry PUBLIC VulkanSceneRenderer_stb)

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
