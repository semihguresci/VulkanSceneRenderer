# Geometry component
add_library(VulkanSceneRenderer_geometry
    DotBimLoader.cpp
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
    stb_impl.cpp)

target_compile_features(VulkanSceneRenderer_geometry PUBLIC cxx_std_23)


target_include_directories(VulkanSceneRenderer_geometry PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../include/
)

target_link_libraries(VulkanSceneRenderer_geometry PUBLIC
    Dep_VulkanCore
    Dep_Math
    Dep_SceneIO
    nlohmann_json::nlohmann_json
    PRIVATE earcut_hpp::earcut_hpp
    manifold::manifold
)
