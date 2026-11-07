include_guard(GLOBAL)

# Latest runtime component of SDK 1.4.363.1; that point release updates VVL
# while retaining the signed Vulkan-Loader 1.4.363.0 component.
set(CONTAINER_VULKAN_LOADER_ROOT "" CACHE PATH "Optional pinned Windows x64 Vulkan Runtime Components directory")
set(CONTAINER_VULKAN_LOADER_AVAILABLE OFF CACHE INTERNAL "Pinned portable Vulkan loader selected" FORCE)
set(CONTAINER_VULKAN_LOADER_VERSION "1.4.363.0" CACHE INTERNAL "Pinned portable Vulkan loader version" FORCE)
set(CONTAINER_VULKAN_LOADER_SHA256 "e1fcfc9489beefa6a6d9d11d6c7517f6a2b7f16e0d3fb8f4103bee0210f314a3"
    CACHE INTERNAL "SHA-256 of LunarG-signed x64 Vulkan loader 1.4.363.0" FORCE)
set(CONTAINER_VULKAN_LOADER_LICENSE_SHA256 "404deb98dabf3e54bf9070d59676b31d69a3da2f9f6e56bdf09ad315c04c718c"
    CACHE INTERNAL "SHA-256 of official Vulkan Runtime Components license" FORCE)

function(container_verify_vulkan_loader root)
    foreach(name IN ITEMS vulkan-1.dll VulkanRT-License.txt)
        if(NOT EXISTS "${root}/${name}" OR IS_DIRECTORY "${root}/${name}")
            message(FATAL_ERROR "Pinned Vulkan loader file is missing: ${root}/${name}")
        endif()
    endforeach()
    file(SHA256 "${root}/vulkan-1.dll" dll_hash)
    file(SHA256 "${root}/VulkanRT-License.txt" license_hash)
    # The exact signed binary pin also binds its architecture and FileVersion.
    if(NOT dll_hash STREQUAL CONTAINER_VULKAN_LOADER_SHA256)
        message(FATAL_ERROR "Vulkan loader is outside the signed Windows x64 1.4.363.0 pin: ${root}/vulkan-1.dll")
    endif()
    if(NOT license_hash STREQUAL CONTAINER_VULKAN_LOADER_LICENSE_SHA256)
        message(FATAL_ERROR "Vulkan loader license is outside the 1.4.363.0 pin: ${root}/VulkanRT-License.txt")
    endif()
endfunction()

# Recheck both inputs at build time before copying, so changing a supplied
# directory after configuration cannot silently change a portable runtime.
if(DEFINED CONTAINER_VULKAN_LOADER_STAGE_DESTINATION)
    if(NOT WIN32 OR NOT CONTAINER_VULKAN_LOADER_ROOT OR NOT CONTAINER_VULKAN_LOADER_STAGE_DESTINATION)
        message(FATAL_ERROR "Portable Vulkan loader staging requires Windows and CONTAINER_VULKAN_LOADER_ROOT")
    endif()
    container_verify_vulkan_loader("${CONTAINER_VULKAN_LOADER_ROOT}")
    file(MAKE_DIRECTORY "${CONTAINER_VULKAN_LOADER_STAGE_DESTINATION}/third_party_licenses")
    file(COPY_FILE "${CONTAINER_VULKAN_LOADER_ROOT}/vulkan-1.dll"
        "${CONTAINER_VULKAN_LOADER_STAGE_DESTINATION}/vulkan-1.dll" ONLY_IF_DIFFERENT)
    file(COPY_FILE "${CONTAINER_VULKAN_LOADER_ROOT}/VulkanRT-License.txt"
        "${CONTAINER_VULKAN_LOADER_STAGE_DESTINATION}/third_party_licenses/VulkanRT-License.txt" ONLY_IF_DIFFERENT)
    return()
endif()

if(CONTAINER_VULKAN_LOADER_ROOT)
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "CONTAINER_VULKAN_LOADER_ROOT requires a Windows x64 build")
    endif()
    container_verify_vulkan_loader("${CONTAINER_VULKAN_LOADER_ROOT}")
    configure_file("${CONTAINER_VULKAN_LOADER_ROOT}/VulkanRT-License.txt"
        "${CMAKE_BINARY_DIR}/third_party_licenses/VulkanRT-License.txt" COPYONLY)
    set(CONTAINER_VULKAN_LOADER_AVAILABLE ON CACHE INTERNAL "Pinned portable Vulkan loader selected" FORCE)
    message(STATUS "Portable Vulkan loader: ${CONTAINER_VULKAN_LOADER_VERSION} (${CONTAINER_VULKAN_LOADER_ROOT}/vulkan-1.dll)")
endif()

function(container_stage_vulkan_loader_runtime target)
    if(CONTAINER_VULKAN_LOADER_AVAILABLE)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                "-DCONTAINER_VULKAN_LOADER_ROOT=${CONTAINER_VULKAN_LOADER_ROOT}"
                "-DCONTAINER_VULKAN_LOADER_STAGE_DESTINATION=$<TARGET_FILE_DIR:${target}>"
                -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
            VERBATIM)
    endif()
endfunction()
