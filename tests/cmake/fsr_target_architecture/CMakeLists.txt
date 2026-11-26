cmake_minimum_required(VERSION 3.23)
project(FsrTargetArchitectureTest LANGUAGES CXX)

set(fsr_dependencies "${CMAKE_CURRENT_LIST_DIR}/../../../cmake/FsrUpscalerDependencies.cmake")

function(expect_fsr_target name expected macros)
    set(header "${CMAKE_CURRENT_BINARY_DIR}/${name}.h")
    # Exercise the production CMake gate independently of the CI compiler target.
    file(WRITE "${header}" [=[
#undef _WIN32
#undef _WIN64
#undef _M_X64
#undef _M_AMD64
#undef __x86_64__
#undef _M_ARM64EC
#undef __arm64ec__
#undef _M_ARM64
#undef __aarch64__
]=] "${macros}")
    if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set(CMAKE_REQUIRED_FLAGS "/FI\"${header}\"")
    else()
        set(CMAKE_REQUIRED_FLAGS "-include \"${header}\"")
    endif()

    # Every simulated target is 64-bit except Windows x86. Unsupported targets
    # must skip all SDK file validation, even with an explicit missing root.
    set(WIN32 ON)
    set(CMAKE_SIZEOF_VOID_P 8)
    if(name STREQUAL "windows-x86")
        set(CMAKE_SIZEOF_VOID_P 4)
    endif()
    set(ENABLE_FSR_UPSCALING ON)
    set(CONTAINER_FSR_SDK_ROOT "")
    if(NOT expected)
        set(CONTAINER_FSR_SDK_ROOT "${CMAKE_CURRENT_BINARY_DIR}/missing-sdk")
    endif()
    set(CONTAINER_FSR_AVAILABLE ON CACHE INTERNAL "" FORCE)
    include("${fsr_dependencies}")
    if(expected AND NOT _container_fsr_windows_x64)
        message(FATAL_ERROR "${name} was rejected by the FSR target architecture check")
    elseif(NOT expected AND _container_fsr_windows_x64)
        message(FATAL_ERROR "${name} was accepted by the FSR target architecture check")
    elseif(CONTAINER_FSR_AVAILABLE)
        message(FATAL_ERROR "${name} enabled FSR without the pinned SDK")
    endif()
endfunction()

set(windows_64 "#define _WIN32 1\n#define _WIN64 1\n")
expect_fsr_target(msvc-x64 ON "${windows_64}#define _M_X64 100\n#define _M_AMD64 100\n")
expect_fsr_target(mingw-x64 ON "${windows_64}#define __x86_64__ 1\n")
expect_fsr_target(arm64 OFF "${windows_64}#define _M_ARM64 1\n")
expect_fsr_target(arm64ec OFF "${windows_64}#define _M_ARM64EC 1\n#define _M_X64 100\n#define _M_AMD64 100\n")
expect_fsr_target(windows-x86 OFF "#define _WIN32 1\n#define _M_IX86 600\n")
expect_fsr_target(non-windows-x64 OFF "#define __x86_64__ 1\n")
