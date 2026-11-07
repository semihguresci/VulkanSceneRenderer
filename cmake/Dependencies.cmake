# cmake/Dependencies.cmake
#
# Phase 1 refactoring: Targeted dependency groups (Dep_*) replace the former
# monolithic VulkanDependencies INTERFACE library.  Each internal library now
# links only the third-party packages it actually uses.

# ── External directory ───────────────────────────────────────────────────────
if(NOT DEFINED EXTERNAL_DIR)
    set(EXTERNAL_DIR "${CMAKE_SOURCE_DIR}/external" CACHE PATH "Directory for external dependencies")
endif()

include(cmake/DependenciesSettings.cmake)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(FetchContent)
include("${CMAKE_CURRENT_LIST_DIR}/OpenVdbDependencies.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/NanoVdbDependencies.cmake")

# ── Find Vulkan SDK ──────────────────────────────────────────────────────────
include("${CMAKE_CURRENT_LIST_DIR}/VulkanSdkCore.cmake")
find_package(Vulkan 1.4 REQUIRED)
include("${CMAKE_CURRENT_LIST_DIR}/VulkanLoaderRuntime.cmake")

if(Vulkan_FOUND)
    message(STATUS "✅ Vulkan found - Version: ${Vulkan_VERSION}")
else()
    message(FATAL_ERROR "❌ Vulkan not found - Please install Vulkan SDK and set VULKAN_SDK")
endif()

# ── Pinned GLM and Vulkan Memory Allocator ───────────────────────────────────
include("${CMAKE_CURRENT_LIST_DIR}/CoreLibraryDependencies.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/ActiveLibraryDependencies.cmake")

# ── Validation layers ────────────────────────────────────────────────────────
option(ENABLE_VULKAN_VALIDATION_LAYERS "Enable Vulkan validation layers" ON)

if(ENABLE_VULKAN_VALIDATION_LAYERS)
  set(_vulkan_sdk $ENV{VULKAN_SDK})

  set(_candidate_layer_dirs "")
  if(_vulkan_sdk)
    if(WIN32)
      list(APPEND _candidate_layer_dirs
        "${_vulkan_sdk}/Bin"
        "${_vulkan_sdk}/bin"
      )
    else()
      list(APPEND _candidate_layer_dirs
        "${_vulkan_sdk}/share/vulkan/explicit_layer.d"
        "${_vulkan_sdk}/etc/vulkan/explicit_layer.d"
        "${_vulkan_sdk}/lib/vulkan/layers"
      )
    endif()
  endif()

  if(DEFINED Vulkan_LAYER_DIR)
    list(APPEND _candidate_layer_dirs "${Vulkan_LAYER_DIR}")
  endif()

  find_file(VK_KHRONOS_VALIDATION_JSON
    NAMES VkLayer_khronos_validation.json
    HINTS ${_candidate_layer_dirs}
    NO_DEFAULT_PATH
  )

  if(VK_KHRONOS_VALIDATION_JSON)
    get_filename_component(VK_LAYER_PATH_DIR "${VK_KHRONOS_VALIDATION_JSON}" DIRECTORY)
    message(STATUS "✅ Found validation layer manifest: ${VK_KHRONOS_VALIDATION_JSON}")
    message(STATUS "   Suggested VK_LAYER_PATH: ${VK_LAYER_PATH_DIR}")
    set(ENV{VK_LAYER_PATH} "${VK_LAYER_PATH_DIR}:$ENV{VK_LAYER_PATH}")
  else()
    message(WARNING
      "⚠️ ENABLE_VULKAN_VALIDATION_LAYERS=ON but VkLayer_khronos_validation.json was not found.\n"
      "   Install Vulkan SDK (LunarG) or your distro's vulkan-validation-layers package.\n"
      "   If installed, set VK_LAYER_PATH to the directory containing VkLayer_khronos_validation.json."
    )
  endif()
endif()

# ── Find all required packages ───────────────────────────────────────────────
set(REQUIRED_PACKAGES
    MaterialX
    earcut_hpp
    manifold
    fmt
    glfw3
    imgui
    EnTT
    nlohmann_json
    spdlog
)

foreach(pkg IN LISTS REQUIRED_PACKAGES)
    find_package(${pkg} CONFIG REQUIRED)
endforeach()

# Stage known shared-library dependencies even when packages come from separate
# prefixes (vcpkg's app-local hook only searches its active installation).
function(container_stage_runtime_dlls target)
    if(WIN32)
        # A manifest avoids shell metacharacters in Ninja's nested cmd.exe
        # post-build command while retaining per-configuration DLL discovery.
        set(runtime_manifest "${CMAKE_CURRENT_BINARY_DIR}/runtime-dlls/${target}-$<CONFIG>.txt")
        file(GENERATE OUTPUT "${runtime_manifest}"
            CONTENT "$<JOIN:$<TARGET_RUNTIME_DLLS:${target}>,\n>")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                "-DRUNTIME_DLLS_FILE=${runtime_manifest}"
                "-DDESTINATION=$<TARGET_FILE_DIR:${target}>"
                -P "${CMAKE_SOURCE_DIR}/cmake/StageRuntimeDlls.cmake"
            VERBATIM)
        if(CONTAINER_OPENVDB_RUNTIME_DLL)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND}
                    "-DOPENVDB_DLL=${CONTAINER_OPENVDB_RUNTIME_DLL}"
                    "-DEXECUTABLE=$<TARGET_FILE:${target}>"
                    "-DDUMPBIN=${CONTAINER_OPENVDB_DUMPBIN}"
                    "-DDESTINATION=$<TARGET_FILE_DIR:${target}>"
                    -P "${CMAKE_SOURCE_DIR}/cmake/StageOpenVdbRuntime.cmake"
                VERBATIM)
        elseif(CONTAINER_NANOVDB_RUNTIME_SEEDS)
            set(nano_manifest "${CMAKE_CURRENT_BINARY_DIR}/runtime-dlls/${target}-nano-$<CONFIG>.txt")
            file(GENERATE OUTPUT "${nano_manifest}"
                CONTENT "$<JOIN:${CONTAINER_NANOVDB_RUNTIME_SEEDS},\n>")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND}
                    "-DSEED_DLLS_FILE=${nano_manifest}"
                    "-DEXECUTABLE=$<TARGET_FILE:${target}>"
                    "-DDUMPBIN=${CONTAINER_NANOVDB_DUMPBIN}"
                    "-DDESTINATION=$<TARGET_FILE_DIR:${target}>"
                    -P "${CMAKE_SOURCE_DIR}/cmake/StageNanoVdbRuntime.cmake"
                VERBATIM)
        endif()
        container_stage_vulkan_loader_runtime(${target})
    endif()
endfunction()

include("${CMAKE_CURRENT_LIST_DIR}/SceneIoLibraryDependencies.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/MergeSelectedDependencyIndices.cmake")
set(_container_selected_runtime_versions
    fmt=12.2.0 spdlog=1.17.0 glfw3=3.5.1 imgui=1.93.0 materialx=1.39.5
    miniz=3.1.2 stb=snapshot-4e9b368bfc071e97206ea5c6adba68dd0cbf8897
    entt=4.0.0 nlohmann-json=3.12.0 tinygltf=3.1.0 tinyexr=3.4.0)
container_merge_selected_dependency_indices(
    "${CMAKE_BINARY_DIR}/third_party_licenses/selected-dependencies.json"
    "${_container_selected_runtime_versions}"
    "${CMAKE_BINARY_DIR}/third_party_licenses/selected-active-dependencies.json"
    "${CMAKE_BINARY_DIR}/selected-ecs-json-dependencies.json"
    "${CMAKE_BINARY_DIR}/selected-import-dependencies.json")

find_path(STB_INCLUDE_DIRS stb_image.h PATH_SUFFIXES stb)
if(STB_INCLUDE_DIRS)
    add_library(stb INTERFACE)
    target_include_directories(stb INTERFACE "${STB_INCLUDE_DIRS}")
    add_library(stb::stb ALIAS stb)
    message(STATUS "✅ STB found (manual): ${STB_INCLUDE_DIRS}")
else()
    message(WARNING "⚠️ STB not found - some features may be disabled")
endif()

find_package(mikktspace CONFIG REQUIRED)

add_library(Dep_TinyUSDZ INTERFACE)
if(ENABLE_TINYUSDZ_USD_LOADER)
    set(TINYUSDZ_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_WITH_C_API OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_WITH_TYDRA ON CACHE BOOL "" FORCE)
    set(TINYUSDZ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_WITH_BUILTIN_IMAGE_LOADER OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_WITH_USDMTLX OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_WITH_JSON OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_WITH_USD_TO_GLTF OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_WITH_USDOBJ OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_WITH_USDFBX OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_WITH_USDVOX OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_WITH_OPENSUBDIV OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_WITH_AUDIO OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_WITH_ALAC_AUDIO OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_WITH_PYTHON OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_WITH_PXR_COMPAT_API OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_WITH_QJS OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_WITH_MCP_SERVER OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_WITH_GEOGRAM OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_WITH_WAMR OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_WITH_COROUTINE OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_WITH_EXR OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_WITH_TIFF OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_WITH_COLORIO OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_WITH_MESHOPT OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_USE_CCACHE OFF CACHE BOOL "" FORCE)
    set(TINYUSDZ_PRODUCTION_BUILD ON CACHE BOOL "" FORCE)

    FetchContent_Declare(
        tinyusdz
        GIT_REPOSITORY https://github.com/lighttransport/tinyusdz.git
        GIT_TAG v0.9.3
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(tinyusdz)

    if(TARGET tinyusdz::tinyusdz_static)
        target_compile_definitions(tinyusdz_static PRIVATE
            nsel_CONFIG_SELECT_EXPECTED=1
            _CRT_SECURE_NO_WARNINGS
            _SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING)
        target_compile_options(tinyusdz_static PRIVATE
            $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/FI${CMAKE_SOURCE_DIR}/cmake/tinyusdz_msvc_compat.h>)
        target_link_libraries(Dep_TinyUSDZ INTERFACE tinyusdz::tinyusdz_static)
        target_include_directories(Dep_TinyUSDZ INTERFACE "${tinyusdz_SOURCE_DIR}/src")
        target_compile_definitions(Dep_TinyUSDZ INTERFACE
            CONTAINER_HAS_TINYUSDZ=1
            nsel_CONFIG_SELECT_EXPECTED=1
            _SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING)
        target_compile_options(Dep_TinyUSDZ INTERFACE
            $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/FI${CMAKE_SOURCE_DIR}/cmake/tinyusdz_msvc_compat.h>)
        message(STATUS "✅ TinyUSDZ found (FetchContent)")
    else()
        message(WARNING "⚠️ TinyUSDZ target was not created; USD falls back to the lightweight parser")
    endif()
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Targeted dependency groups (Dep_*)
#
# Each group bundles a coherent set of third-party libraries.  Internal
# targets link only the groups they actually need.
# ─────────────────────────────────────────────────────────────────────────────

# -- Dep_VulkanCore: Vulkan SDK + VMA ----------------------------------------
add_library(Dep_VulkanCore INTERFACE)
target_include_directories(Dep_VulkanCore BEFORE INTERFACE
    "${CONTAINER_GLM_INCLUDE_DIR}" "${CONTAINER_VMA_INCLUDE_DIR}" ${Vulkan_INCLUDE_DIRS})
target_link_libraries(Dep_VulkanCore INTERFACE
    Vulkan::Vulkan
    $<IF:$<TARGET_EXISTS:Vulkan::Headers>,Vulkan::Headers,>
    ${VMA_TARGET}
)
# NOMINMAX prevents Windows.h min/max macros from clashing with std::min/max.
# WIN32_LEAN_AND_MEAN reduces the Windows.h include footprint.
# Previously provided by Vulkan::CompilerConfiguration.
target_compile_definitions(Dep_VulkanCore INTERFACE
    $<$<PLATFORM_ID:Windows>:NOMINMAX>
    $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
)

# -- Dep_Math: GLM with project-wide defines --------------------------------
add_library(Dep_Math INTERFACE)
target_include_directories(Dep_Math BEFORE INTERFACE "${CONTAINER_GLM_INCLUDE_DIR}")
target_link_libraries(Dep_Math INTERFACE glm::glm-header-only)
target_compile_definitions(Dep_Math INTERFACE
    GLM_FORCE_COLUMN_MAJOR
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RADIANS
)

# -- Dep_Windowing: GLFW ----------------------------------------------------
add_library(Dep_Windowing INTERFACE)
target_link_libraries(Dep_Windowing INTERFACE glfw)

# -- Dep_UI: Dear ImGui ------------------------------------------------------
add_library(Dep_UI INTERFACE)
target_link_libraries(Dep_UI INTERFACE imgui::imgui)

# -- Dep_Logging: spdlog + fmt -----------------------------------------------
add_library(Dep_Logging INTERFACE)
target_link_libraries(Dep_Logging INTERFACE spdlog::spdlog fmt::fmt)

# -- Dep_SceneIO: Model/texture loading (tinygltf, stb) ---------------------
add_library(Dep_SceneIO INTERFACE)
target_include_directories(Dep_SceneIO INTERFACE
    $<IF:$<TARGET_EXISTS:tinygltf::tinygltf>,${TINYGLTF_INCLUDE_DIRS},"">
    $<IF:$<TARGET_EXISTS:stb::stb>,${STB_INCLUDE_DIRS},"">
    $<IF:$<TARGET_EXISTS:tinyexr::tinyexr>,${TINYEXR_INCLUDE_DIRS},"">
)
target_link_libraries(Dep_SceneIO INTERFACE
    $<IF:$<TARGET_EXISTS:tinygltf::tinygltf>,tinygltf::tinygltf,"">
    $<IF:$<TARGET_EXISTS:stb::stb>,stb::stb,"">
    $<IF:$<TARGET_EXISTS:tinyexr::tinyexr>,tinyexr::tinyexr,"">
    Dep_TinyUSDZ
    mikktspace::mikktspace
)

# -- Dep_Material: MaterialX --------------------------------------------------
add_library(Dep_Material INTERFACE)
target_link_libraries(Dep_Material INTERFACE
    MaterialXCore
    MaterialXFormat
    MaterialXGenShader
)

# -- Dep_ECS: EnTT ───────────────────────────────────────────────────────────
add_library(Dep_ECS INTERFACE)
target_link_libraries(Dep_ECS INTERFACE
    EnTT::EnTT
)

# -- Dep_Physics: optional, pinned Jolt CPU rigid-body backend -----------------
include("${CMAKE_CURRENT_LIST_DIR}/PhysicsDependencies.cmake")

# Optional CPU geometry decoders; Draco remains isolated in its worker.
include("${CMAKE_CURRENT_LIST_DIR}/GltfCompressionDependencies.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/TextureCodecDependencies.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/ReferenceOidnDependencies.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/ReferenceOptixDependencies.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/FsrUpscalerDependencies.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/DlssUpscalerDependencies.cmake")

# ── Summary
message(STATUS "--------------------------------------------------")
message(STATUS "Dependency Configuration Summary:")
message(STATUS "Vulkan: ${Vulkan_VERSION}")
message(STATUS "spdlog: ${spdlog_VERSION}")
message(STATUS "--------------------------------------------------")
