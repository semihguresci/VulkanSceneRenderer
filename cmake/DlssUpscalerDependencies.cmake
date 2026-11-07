# NVIDIA DLSS SDK v310.9.16 (release commit 9981c6350dc58c0ce21f024fd65e31e851c31689).
set(CONTAINER_DLSS_SDK_ROOT "" CACHE PATH "Root of the pinned NVIDIA DLSS SDK v310.9.16")
set(CONTAINER_DLSS_AVAILABLE OFF CACHE INTERNAL "Pinned NVIDIA DLSS Super Resolution backend built" FORCE)
set(CONTAINER_DLSS_RR_AVAILABLE OFF CACHE INTERNAL "Pinned NVIDIA DLSS Ray Reconstruction backend built" FORCE)
set(CONTAINER_DLSS_RUNTIME_SHA256 "d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e"
    CACHE INTERNAL "SHA-256 of the NVIDIA-signed DLSS 310.9.16 redistributable" FORCE)
set(CONTAINER_DLSS_RR_RUNTIME_SHA256 "b49435ae346f227feafe7bbe937d88277ea0ee7323975ec06488f377fdb46cee"
    CACHE INTERNAL "SHA-256 of the NVIDIA-signed DLSS Ray Reconstruction 310.9.16 redistributable" FORCE)

function(container_check_dlss_file file hash)
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "The pinned NVIDIA DLSS SDK file is missing: ${file}")
    endif()
    if(file MATCHES "\\.(dll|lib)$")
        file(SHA256 "${file}" actual)
    else()
        file(READ "${file}" contents)
        string(REPLACE "\r\n" "\n" contents "${contents}")
        string(SHA256 actual "${contents}")
    endif()
    if(NOT actual STREQUAL hash)
        message(FATAL_ERROR "NVIDIA DLSS SDK file is outside the v310.9.16 pin: ${file}")
    endif()
endfunction()

if((ENABLE_DLSS_UPSCALING OR ENABLE_DLSS_RAY_RECONSTRUCTION) AND WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(NOT CONTAINER_DLSS_SDK_ROOT)
        message(WARNING "NVIDIA DLSS SDK unavailable. Set CONTAINER_DLSS_SDK_ROOT to enable the requested Vulkan DLSS backend.")
    else()
        set(CONTAINER_DLSS_INCLUDE_DIR "${CONTAINER_DLSS_SDK_ROOT}/include")
        set(_dlss_libraries "${CONTAINER_DLSS_SDK_ROOT}/lib/Windows_x86_64")
        set(CONTAINER_DLSS_DLL "${_dlss_libraries}/rel/nvngx_dlss.dll")
        set(CONTAINER_DLSS_RR_DLL "${_dlss_libraries}/rel/nvngx_dlssd.dll")
        set(CONTAINER_DLSS_LIBRARY "${_dlss_libraries}/x64/nvsdk_ngx_d.lib")
        set(CONTAINER_DLSS_DEBUG_LIBRARY "${_dlss_libraries}/x64/nvsdk_ngx_d_dbg.lib")
        set(_dlss_license "${CONTAINER_DLSS_SDK_ROOT}/LICENSE.txt")

        container_check_dlss_file("${CONTAINER_DLSS_LIBRARY}"
            "bc43df9ec032004fde681d3e3afd01cde239e03a0125b822e88674c84287f99c")
        container_check_dlss_file("${CONTAINER_DLSS_DEBUG_LIBRARY}"
            "3f804d50b0323baa2c4976c4c94e2454c4a3f9c6c04e7d1f2302392d5add0009")
        container_check_dlss_file("${CONTAINER_DLSS_INCLUDE_DIR}/nvsdk_ngx_vk.h"
            "2d364ce7132881eb669e9498fd570d74cba563b1fbebc82c235f8e1ad2dd8b6d")
        container_check_dlss_file("${CONTAINER_DLSS_INCLUDE_DIR}/nvsdk_ngx_helpers_vk.h"
            "c192bff72138f12f770db48e12c1d8f712dfc81ea0e8e055ebb27d8dd1b31623")
        container_check_dlss_file("${CONTAINER_DLSS_INCLUDE_DIR}/nvsdk_ngx_helpers.h"
            "5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471")
        container_check_dlss_file("${CONTAINER_DLSS_INCLUDE_DIR}/nvsdk_ngx_helpers_d3d.h"
            "ec75224f36ed6580baaf250fa83620405ad81a1d98c2c436654a0c9cf6a6b8ba")
        container_check_dlss_file("${CONTAINER_DLSS_INCLUDE_DIR}/nvsdk_ngx_helpers_cuda.h"
            "f2851a76107bdf4fbcb7b261f3c35573c312d8ecb4bc18fb02d9e2cec7705042")
        container_check_dlss_file("${CONTAINER_DLSS_INCLUDE_DIR}/nvsdk_ngx.h"
            "dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848")
        container_check_dlss_file("${CONTAINER_DLSS_INCLUDE_DIR}/nvsdk_ngx_defs.h"
            "ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e")
        container_check_dlss_file("${CONTAINER_DLSS_INCLUDE_DIR}/nvsdk_ngx_defs_vk.h"
            "0a24d0861ace7d6b9362a67b7f08bea1b33ea123c5ce730b19133de8a891d031")
        container_check_dlss_file("${CONTAINER_DLSS_INCLUDE_DIR}/nvsdk_ngx_params.h"
            "943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc")
        container_check_dlss_file("${_dlss_license}"
            "d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0")
        configure_file("${_dlss_license}"
            "${CMAKE_BINARY_DIR}/third_party_licenses/DLSS-310.9.16-LICENSE.txt" COPYONLY)
        if(ENABLE_DLSS_UPSCALING)
            container_check_dlss_file("${CONTAINER_DLSS_DLL}" "${CONTAINER_DLSS_RUNTIME_SHA256}")
            set(CONTAINER_DLSS_AVAILABLE ON CACHE INTERNAL "Pinned NVIDIA DLSS Super Resolution backend built" FORCE)
        endif()
        if(ENABLE_DLSS_RAY_RECONSTRUCTION)
            container_check_dlss_file("${CONTAINER_DLSS_RR_DLL}" "${CONTAINER_DLSS_RR_RUNTIME_SHA256}")
            container_check_dlss_file("${CONTAINER_DLSS_INCLUDE_DIR}/nvsdk_ngx_helpers_dlssd.h"
                "a25fdda925b479dfe869593aeba311ef08793da54e0391f2fe2fe756cbef5532")
            container_check_dlss_file("${CONTAINER_DLSS_INCLUDE_DIR}/nvsdk_ngx_helpers_dlssd_d3d.h"
                "6df9d02c6f47feee3afc8d12340a9f554a7ddce4ddacb1b6a13a42b26c2c6293")
            container_check_dlss_file("${CONTAINER_DLSS_INCLUDE_DIR}/nvsdk_ngx_helpers_dlssd_vk.h"
                "11df8fbda532a97fe867c8ef2b40058b49b7898183710f3468e55b1eedb8f40b")
            container_check_dlss_file("${CONTAINER_DLSS_INCLUDE_DIR}/nvsdk_ngx_defs_dlssd.h"
                "2e97fe1595b1334f71b51724f2cb67496e8c296a7a2eec5606da53055882e716")
            container_check_dlss_file("${CONTAINER_DLSS_INCLUDE_DIR}/nvsdk_ngx_params_dlssd.h"
                "d41e3ba8f1d72583b55273092c99bf426dc4a0d46de827229e25d5b1325a370e")
            set(CONTAINER_DLSS_RR_AVAILABLE ON CACHE INTERNAL "Pinned NVIDIA DLSS Ray Reconstruction backend built" FORCE)
        endif()
    endif()
elseif(ENABLE_DLSS_UPSCALING OR ENABLE_DLSS_RAY_RECONSTRUCTION)
    message(WARNING "The pinned NVIDIA DLSS SDK requires Windows x64 and MSVC. The native renderer remains available.")
endif()

function(container_stage_dlss_runtime target)
    if(CONTAINER_DLSS_AVAILABLE)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CONTAINER_DLSS_DLL}"
                "$<TARGET_FILE_DIR:${target}>/nvngx_dlss.dll"
            VERBATIM)
    endif()
    if(CONTAINER_DLSS_RR_AVAILABLE)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CONTAINER_DLSS_RR_DLL}"
                "$<TARGET_FILE_DIR:${target}>/nvngx_dlssd.dll"
            VERBATIM)
    endif()
endfunction()
