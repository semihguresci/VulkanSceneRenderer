# CMakeLists.txt in project-root
cmake_minimum_required(VERSION 3.23)

# Ensure MSVC targets link the correct C runtime (DLL variant).
# Required for VS 2026+ / MSVC 19.50+ where auto-linking may not
# inject the CRT libs unless the policy is explicitly set.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

project(VulkanSceneRenderer)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Include third-party dependencies
include(cmake/Options.cmake)
include(cmake/Dependencies.cmake)

add_subdirectory(src)

# Shader compilation and asset generation (modular includes)
include(cmake/Shaders.cmake)
include(cmake/Assets.cmake)

# Main executable
add_executable(${PROJECT_NAME} main.cpp)

target_link_libraries(${PROJECT_NAME} PRIVATE
    VulkanSceneRenderer_Core
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Wire asset dependencies to the executable
if(TARGET shaders)
    add_dependencies(${PROJECT_NAME} shaders)
endif()
add_dependencies(${PROJECT_NAME} copy_materials)
add_dependencies(${PROJECT_NAME} generate_models)
if(TARGET copy_hdr)
    add_dependencies(${PROJECT_NAME} copy_hdr)
endif()

# Multi-configuration generators place the executable in Debug/Release while
# asset producers write to the build root. Stage assets on every build so shader
# changes are copied even when the executable does not need to be relinked.
if(CMAKE_CONFIGURATION_TYPES)
    add_custom_target(stage_runtime_assets
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${COMPILED_SHADERS_DIR}"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/spv_shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_BINARY_DIR}/materials"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/materials"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${MODELS_OUTPUT_DIR}"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/models"
        COMMENT "Staging runtime assets beside the executable"
        VERBATIM
    )
    add_dependencies(stage_runtime_assets shaders copy_materials generate_models)
    if(TARGET copy_hdr)
        add_dependencies(stage_runtime_assets copy_hdr)
        add_custom_command(TARGET stage_runtime_assets POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_BINARY_DIR}/hdr"
                    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/hdr"
            VERBATIM
        )
    endif()
    add_dependencies(${PROJECT_NAME} stage_runtime_assets)
endif()

# Include the tests configuration
if(ENABLE_TESTS)
    include(${CMAKE_SOURCE_DIR}/tests/CMakeLists.tests.cmake)
endif()




