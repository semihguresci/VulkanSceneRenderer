# AMD FidelityFX SDK v1.1.4, commit c6efa6bf7f2027b3ec94f28578bb5965eabb9e55.
set(CONTAINER_FSR_SDK_ROOT "" CACHE PATH "Root of the pinned AMD FidelityFX SDK v1.1.4")
set(CONTAINER_FSR_AVAILABLE OFF CACHE INTERNAL "Pinned Vulkan FSR backend built" FORCE)

if(ENABLE_FSR_UPSCALING AND WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(NOT CONTAINER_FSR_SDK_ROOT)
        message(WARNING "FidelityFX SDK unavailable. Set CONTAINER_FSR_SDK_ROOT to enable the Vulkan FSR 3.1.4 backend.")
    else()
        set(CONTAINER_FSR_DLL "${CONTAINER_FSR_SDK_ROOT}/PrebuiltSignedDLL/amd_fidelityfx_vk.dll")
        set(CONTAINER_FSR_INCLUDE_DIR "${CONTAINER_FSR_SDK_ROOT}/ffx-api/include")
        set(CONTAINER_FSR_LICENSE "${CONTAINER_FSR_SDK_ROOT}/sdk/LICENSE.txt")

        file(SHA256 "${CONTAINER_FSR_DLL}" _fsr_dll_sha256)
        if(NOT _fsr_dll_sha256 STREQUAL "a1624cc4238fef046f30c4d80ce3f47be63fc5f5373f49e3ee9edb9960f54c78")
            message(FATAL_ERROR "FidelityFX Vulkan DLL must be from the pinned v1.1.4 SDK")
        endif()

        function(container_check_fsr_source file hash)
            file(READ "${file}" contents)
            string(REPLACE "\r\n" "\n" contents "${contents}")
            string(SHA256 actual "${contents}")
            if(NOT actual STREQUAL hash)
                message(FATAL_ERROR "FidelityFX SDK file is outside the v1.1.4 pin: ${file}")
            endif()
        endfunction()
        container_check_fsr_source("${CONTAINER_FSR_INCLUDE_DIR}/ffx_api/ffx_api.h"
            "408b4203545e8299ed188fbb1d9ddfe0932b73ea6e8b14fe10edcf1f45771561")
        container_check_fsr_source("${CONTAINER_FSR_INCLUDE_DIR}/ffx_api/ffx_api_types.h"
            "dee2782813d22047c068e0fb27d1347324f8ce79d280a4672848b3b294cb12fe")
        container_check_fsr_source("${CONTAINER_FSR_INCLUDE_DIR}/ffx_api/ffx_upscale.h"
            "1155bcdcea9c10e6a95507bc9422ca062a853ac84250f0b09a637a66836b3ac1")
        container_check_fsr_source("${CONTAINER_FSR_INCLUDE_DIR}/ffx_api/vk/ffx_api_vk.h"
            "a50a8d5ff5689a10a6ce378f2df5569fd880c89ec46e8356aaf35eb40512edd9")
        container_check_fsr_source("${CONTAINER_FSR_LICENSE}"
            "82cf74fc23885107c7f69249ac3f1dac55898ff68320eb2c753984cae6b2d78a")
        configure_file("${CONTAINER_FSR_LICENSE}"
            "${CMAKE_BINARY_DIR}/third_party_licenses/FidelityFX-SDK-MIT.txt" COPYONLY)
        set(CONTAINER_FSR_AVAILABLE ON CACHE INTERNAL "Pinned Vulkan FSR backend built" FORCE)
    endif()
elseif(ENABLE_FSR_UPSCALING)
    message(WARNING "The pinned signed FidelityFX SDK binary requires Windows x64. The native renderer remains available.")
endif()

function(container_stage_fsr_runtime target)
    if(CONTAINER_FSR_AVAILABLE)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CONTAINER_FSR_DLL}"
                "$<TARGET_FILE_DIR:${target}>/amd_fidelityfx_vk.dll"
            VERBATIM)
    endif()
endfunction()
