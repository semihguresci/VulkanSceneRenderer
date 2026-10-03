# ─────────────────────────────────────────────────────────────────────────────
# Phase 2: Decomposed utility sub-libraries
#
# The former monolithic VulkanSceneRenderer_utility (16 files) is now split into
# six focused libraries.  Header files stay in include/Container/utility/ to
# avoid mass-renaming #include paths across the project.
# ─────────────────────────────────────────────────────────────────────────────

set(_UTILITY_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../include/")

# ── 1. Logging ───────────────────────────────────────────────────────────────
add_library(VulkanSceneRenderer_log
    Logger.cpp
)
target_compile_features(VulkanSceneRenderer_log PUBLIC cxx_std_23)
target_include_directories(VulkanSceneRenderer_log PUBLIC
    ${_UTILITY_INCLUDE_DIR}
)
target_link_libraries(VulkanSceneRenderer_log PUBLIC
    Dep_Logging
    Dep_VulkanCore
)

# ── 2. Vulkan device lifecycle ───────────────────────────────────────────────
add_library(VulkanSceneRenderer_vulkan_device
    DynamicRendering.cpp
    VulkanObjects.cpp
    VulkanDevice.cpp
    VulkanInstance.cpp
    SwapChainManager.cpp
    FrameSyncManager.cpp
)
target_compile_features(VulkanSceneRenderer_vulkan_device PUBLIC cxx_std_23)
target_include_directories(VulkanSceneRenderer_vulkan_device PUBLIC
    ${_UTILITY_INCLUDE_DIR}
)
target_link_libraries(VulkanSceneRenderer_vulkan_device PUBLIC
    Dep_VulkanCore
    Dep_Math
    Dep_Windowing
)

# ── 3. GPU resources (memory, buffers, textures, pipelines) ──────────────────
add_library(VulkanSceneRenderer_gpu_resource
    AllocationManager.cpp
    VulkanMemoryManager.cpp
    TextureManager.cpp
    PipelineManager.cpp
)
target_compile_features(VulkanSceneRenderer_gpu_resource PUBLIC cxx_std_23)
target_include_directories(VulkanSceneRenderer_gpu_resource PUBLIC
    ${_UTILITY_INCLUDE_DIR}
)
target_link_libraries(VulkanSceneRenderer_gpu_resource PUBLIC
    Dep_VulkanCore
    Dep_Math
    Dep_SceneIO
    VulkanSceneRenderer_vulkan_device
)

# ── 4. Window + input ───────────────────────────────────────────────────────
add_library(VulkanSceneRenderer_window
    WindowManager.cpp
    InputManager.cpp
)
target_compile_features(VulkanSceneRenderer_window PUBLIC cxx_std_23)
target_include_directories(VulkanSceneRenderer_window PUBLIC
    ${_UTILITY_INCLUDE_DIR}
)
target_link_libraries(VulkanSceneRenderer_window PUBLIC
    Dep_VulkanCore
    Dep_Math
    Dep_Windowing
)

# ── 5. Scene (graph, materials, scene manager) ──────────────────────────────
add_library(VulkanSceneRenderer_scene
    SceneManager.cpp
    SceneGraph.cpp
    MaterialManager.cpp
    MaterialXIntegration.cpp
)
target_compile_features(VulkanSceneRenderer_scene PUBLIC cxx_std_23)
target_include_directories(VulkanSceneRenderer_scene PUBLIC
    ${_UTILITY_INCLUDE_DIR}
)
target_link_libraries(VulkanSceneRenderer_scene PUBLIC
    Dep_VulkanCore
    Dep_Math
    Dep_SceneIO
    Dep_Material
    VulkanSceneRenderer_gpu_resource
    VulkanSceneRenderer_geometry
)

# ── 6. UI (ImGui) ───────────────────────────────────────────────────────────
add_library(VulkanSceneRenderer_ui
    BcfViewpoint.cpp
    GuiManager.cpp
)
target_compile_features(VulkanSceneRenderer_ui PUBLIC cxx_std_23)
target_include_directories(VulkanSceneRenderer_ui PUBLIC
    ${_UTILITY_INCLUDE_DIR}
)
target_link_libraries(VulkanSceneRenderer_ui PUBLIC
    Dep_VulkanCore
    Dep_Math
    Dep_Windowing
    Dep_UI
    VulkanSceneRenderer_scene
    VulkanSceneRenderer_bim_ui_support
)
if(TARGET miniz::miniz)
    target_link_libraries(VulkanSceneRenderer_ui PUBLIC miniz::miniz)
endif()
