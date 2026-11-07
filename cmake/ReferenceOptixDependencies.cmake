# NVIDIA OptiX v9.1.0, commit f1f6dd803f3159992d248178f6e09421c6eb8b6d.
set(CONTAINER_OPTIX_AVAILABLE OFF CACHE INTERNAL "OptiX reference denoiser was built" FORCE)
set(CONTAINER_OPTIX_SDK_ROOT "" CACHE PATH "NVIDIA OptiX 9.1.0 header archive or SDK root")

function(container_check_optix_file file hash)
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "The pinned NVIDIA OptiX SDK file is missing: ${file}")
    endif()
    file(READ "${file}" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    string(SHA256 actual "${contents}")
    if(NOT actual STREQUAL hash)
        message(FATAL_ERROR "NVIDIA OptiX SDK file is outside the v9.1.0 pin: ${file}")
    endif()
endfunction()

if(ENABLE_REFERENCE_OPTIX)
    set(CONTAINER_OPTIX_INCLUDE_DIR "${CONTAINER_OPTIX_SDK_ROOT}/include")
    find_path(CONTAINER_OPTIX_CUDA_INCLUDE_DIR cuda.h HINTS ENV CUDA_PATH PATH_SUFFIXES include)
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CONTAINER_OPTIX_CUDA_INCLUDE_DIR
            AND EXISTS "${CONTAINER_OPTIX_INCLUDE_DIR}/optix.h"
            AND EXISTS "${CONTAINER_OPTIX_SDK_ROOT}/LICENSE.txt")
        file(STRINGS "${CONTAINER_OPTIX_INCLUDE_DIR}/optix.h" _optix_version REGEX "^#define OPTIX_VERSION ")
        if(_optix_version MATCHES "^#define OPTIX_VERSION +90100( |$)")
            container_check_optix_file("${CONTAINER_OPTIX_SDK_ROOT}/include/optix.h"
                "65b70a4a33d3ee4d0ce8b2a90debdb2caa4eaa0a38129092ee5eacdf6a1984a7")
            container_check_optix_file("${CONTAINER_OPTIX_SDK_ROOT}/include/optix_host.h"
                "deb94fb5eebe8509b507ebd9633398127e4bbbcd32d41383401da22440f2cb38")
            container_check_optix_file("${CONTAINER_OPTIX_SDK_ROOT}/include/optix_types.h"
                "42928fa0fc471f09eff90c8ff96d541cab9062602f98ce50c2c2aa40a62440f6")
            container_check_optix_file("${CONTAINER_OPTIX_SDK_ROOT}/include/optix_function_table.h"
                "a885a24230f7d1f89a2e753bc66b0adae7f84ceecea50cab67dc370c147e27de")
            container_check_optix_file("${CONTAINER_OPTIX_SDK_ROOT}/include/optix_function_table_definition.h"
                "a9b118be4c7ca3ee13824baecdfb8d85f60aa179c8f957342d2d2fba624a5c66")
            container_check_optix_file("${CONTAINER_OPTIX_SDK_ROOT}/include/optix_stubs.h"
                "8353f4a14951956f771a6208669ba6d17b2d34877459628785e7339b97eda0ca")
            container_check_optix_file("${CONTAINER_OPTIX_SDK_ROOT}/LICENSE.txt"
                "be14b34e78b0d05d110e0ff6f0e750466da1d054968946a4bd3c1a34d7387673")
            container_check_optix_file("${CONTAINER_OPTIX_SDK_ROOT}/license_info.txt"
                "b6a984d04028bd71928d4103f4ac7a6e7083b805c2f7d2c18e4627baaa5c769d")
            configure_file("${CONTAINER_OPTIX_SDK_ROOT}/LICENSE.txt"
                "${CMAKE_BINARY_DIR}/third_party_licenses/optix-9.1.0-LICENSE.txt" COPYONLY)
            configure_file("${CONTAINER_OPTIX_SDK_ROOT}/license_info.txt"
                "${CMAKE_BINARY_DIR}/third_party_licenses/optix-9.1.0-LICENSE-INFO.txt" COPYONLY)
            set(_optix_notices "")
            foreach(_optix_header IN ITEMS optix_stubs.h optix_function_table_definition.h)
                file(READ "${CONTAINER_OPTIX_INCLUDE_DIR}/${_optix_header}" _optix_prefix LIMIT 4096)
                string(FIND "${_optix_prefix}" "*/" _optix_notice_end)
                if(_optix_notice_end LESS 0 OR NOT _optix_prefix MATCHES "SPDX-License-Identifier: BSD-3-Clause")
                    message(FATAL_ERROR "The pinned OptiX BSD notice is missing: ${_optix_header}")
                endif()
                math(EXPR _optix_notice_length "${_optix_notice_end} + 2")
                string(SUBSTRING "${_optix_prefix}" 0 ${_optix_notice_length} _optix_notice)
                string(APPEND _optix_notices "${_optix_header}\n${_optix_notice}\n\n")
            endforeach()
            file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/third_party_licenses/optix-9.1.0-BSD-3-Clause.txt"
                CONTENT "${_optix_notices}" NEWLINE_STYLE LF)
            set(CONTAINER_OPTIX_AVAILABLE ON CACHE INTERNAL "OptiX reference denoiser was built" FORCE)
        endif()
    endif()
    if(NOT CONTAINER_OPTIX_AVAILABLE)
        message(WARNING "Reference OptiX requires Windows x64, CUDA headers, and pinned OptiX 9.1.0 headers/license. Set CONTAINER_OPTIX_SDK_ROOT and CUDA_PATH. Raw reference rendering remains available.")
    endif()
endif()
