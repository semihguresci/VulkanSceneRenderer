# AMD FSR SDK v2.4.0, commit 7539ae8b6568ad29ed78a753404351daf3b5cca7.
set(CONTAINER_FSR_SDK_ROOT "" CACHE PATH "Root of the pinned AMD FSR SDK v2.4.0")
set(CONTAINER_FSR_AVAILABLE OFF CACHE INTERNAL "Pinned Vulkan FSR backend built" FORCE)
set(CONTAINER_FSR_LOADER_SHA256 "38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9"
    CACHE INTERNAL "SHA-256 of the AMD-signed FSR SDK 2.4.0 Vulkan loader" FORCE)
set(CONTAINER_FSR_UPSCALER_SHA256 "4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7"
    CACHE INTERNAL "SHA-256 of the AMD-signed FSR SDK 2.4.0 Vulkan upscaler" FORCE)

function(container_check_fsr_file file hash)
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "The pinned AMD FSR SDK file is missing: ${file}")
    endif()
    if(file MATCHES "\\.dll$")
        file(SHA256 "${file}" actual)
    else()
        file(READ "${file}" contents)
        string(REPLACE "\r\n" "\n" contents "${contents}")
        string(SHA256 actual "${contents}")
    endif()
    if(NOT actual STREQUAL hash)
        message(FATAL_ERROR "AMD FSR SDK file is outside the v2.4.0 pin: ${file}")
    endif()
endfunction()

if(ENABLE_FSR_UPSCALING AND WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(NOT CONTAINER_FSR_SDK_ROOT)
        message(WARNING "AMD FSR SDK unavailable. Set CONTAINER_FSR_SDK_ROOT to enable the Vulkan FSR SDK 2.4.0 backend.")
    else()
        set(CONTAINER_FSR_INCLUDE_DIR "${CONTAINER_FSR_SDK_ROOT}/Kits/FidelityFX")
        set(CONTAINER_FSR_LOADER_DLL "${CONTAINER_FSR_INCLUDE_DIR}/signedbin/amd_fidelityfx_loader_vk.dll")
        set(CONTAINER_FSR_UPSCALER_DLL "${CONTAINER_FSR_INCLUDE_DIR}/signedbin/amd_fidelityfx_upscaler_vk.dll")
        set(CONTAINER_FSR_LICENSE "${CONTAINER_FSR_INCLUDE_DIR}/docs/license.md")
        set(CONTAINER_FSR_NOTICES "${CONTAINER_FSR_SDK_ROOT}/3rdpartynotice.md")
        container_check_fsr_file("${CONTAINER_FSR_LOADER_DLL}" "${CONTAINER_FSR_LOADER_SHA256}")
        container_check_fsr_file("${CONTAINER_FSR_UPSCALER_DLL}" "${CONTAINER_FSR_UPSCALER_SHA256}")
        container_check_fsr_file("${CONTAINER_FSR_INCLUDE_DIR}/api/include/ffx_api.h"
            "4d9dcf243ef5efbf03340a02f16b51a22053243a22e8a9a0b3ce91c26982b4bf")
        container_check_fsr_file("${CONTAINER_FSR_INCLUDE_DIR}/api/include/ffx_api_types.h"
            "53f07c113a9ba493f297656e4481b6539aed0afc9f3cc8da682aa477d270c46c")
        container_check_fsr_file("${CONTAINER_FSR_INCLUDE_DIR}/upscalers/include/ffx_upscale.h"
            "13091e7fe5fd9c82b1fb7c044b9f08f664d9653f599d2ea500738709236fca4d")
        container_check_fsr_file("${CONTAINER_FSR_INCLUDE_DIR}/api/include/vk/ffx_api_vk.h"
            "09ac1855bc4c2f711243d6982909385072ca3f8d30db44d1a82ee5bd637832ad")
        container_check_fsr_file("${CONTAINER_FSR_LICENSE}"
            "32357259b965d6a8001e9e508ba60eda6034027c5923a7d226ffbaff813c2bfb")
        container_check_fsr_file("${CONTAINER_FSR_NOTICES}"
            "3b69b1a1011622557a81b55a9a211dd797d01b8160924a5ec51b4c3432d33a4f")
        configure_file("${CONTAINER_FSR_LICENSE}"
            "${CMAKE_BINARY_DIR}/third_party_licenses/AMD-FSR-SDK-2.4.0-LICENSE.txt" COPYONLY)
        configure_file("${CONTAINER_FSR_NOTICES}"
            "${CMAKE_BINARY_DIR}/third_party_licenses/AMD-FSR-SDK-2.4.0-THIRD-PARTY-NOTICES.md" COPYONLY)
        set(CONTAINER_FSR_AVAILABLE ON CACHE INTERNAL "Pinned Vulkan FSR backend built" FORCE)
    endif()
elseif(ENABLE_FSR_UPSCALING)
    message(WARNING "The pinned signed AMD FSR SDK binary requires Windows x64. The native renderer remains available.")
endif()

function(container_stage_fsr_runtime target)
    if(CONTAINER_FSR_AVAILABLE)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CONTAINER_FSR_LOADER_DLL}"
                "$<TARGET_FILE_DIR:${target}>/amd_fidelityfx_loader_vk.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CONTAINER_FSR_UPSCALER_DLL}"
                "$<TARGET_FILE_DIR:${target}>/amd_fidelityfx_upscaler_vk.dll"
            VERBATIM)
    endif()
endfunction()
