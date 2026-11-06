# Optional CPU rigid-body physics. Keep dependency flags scoped to Jolt so its
# generic option names cannot change the renderer's compiler/runtime settings.
include_guard(GLOBAL)

add_library(Dep_Physics INTERFACE)
if(NOT ENABLE_PHYSICS)
    return()
endif()

include(FetchContent)

function(container_fetch_jolt)
    # Jolt's option() calls must respect these normal, function-local values
    # instead of writing generic options into the parent project's cache.
    cmake_policy(SET CMP0077 NEW)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()

    set(DOUBLE_PRECISION ON)
    set(JPH_BUILD_SHARED_LIBS OFF)
    set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF)
    set(OVERRIDE_CXX_FLAGS OFF)
    set(INTERPROCEDURAL_OPTIMIZATION OFF)
    set(ENABLE_ALL_WARNINGS OFF)
    set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF)
    set(CPP_EXCEPTIONS_ENABLED OFF)
    set(CPP_RTTI_ENABLED OFF)

    # SSE2 is the x86 baseline; Jolt handles other architectures separately.
    # These options also control PUBLIC flags used by all Jolt consumers.
    foreach(feature SSE4_1 SSE4_2 AVX AVX2 AVX512 LZCNT TZCNT F16C FMADD)
        set(USE_${feature} OFF)
    endforeach()

    # Initial integration uses CPU rigid bodies, without Jolt's separate GPU
    # compute/shader stack, sample applications, serialization or debug tools.
    foreach(backend DX12 VK MTL CPU_COMPUTE)
        set(JPH_USE_${backend} OFF)
    endforeach()
    set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF)
    set(DEBUG_RENDERER_IN_DISTRIBUTION OFF)
    set(PROFILER_IN_DEBUG_AND_RELEASE OFF)
    set(PROFILER_IN_DISTRIBUTION OFF)
    set(JPH_USE_EXTERNAL_PROFILE OFF)
    set(ENABLE_OBJECT_STREAM OFF)
    set(ENABLE_INSTALL OFF)
    set(TARGET_UNIT_TESTS OFF)
    set(TARGET_HELLO_WORLD OFF)
    set(TARGET_PERFORMANCE_TEST OFF)
    set(TARGET_SAMPLES OFF)
    set(TARGET_VIEWER OFF)

    # v5.6.0 (2026-07-11), pinned to its exact commit and archive digest. The
    # archive avoids downloading repository history. FetchContent still honors
    # FETCHCONTENT_SOURCE_DIR_JOLT for an explicitly supplied local checkout.
    FetchContent_Declare(
        jolt
        URL https://codeload.github.com/jrouwe/JoltPhysics/zip/e77f175595e64cb44218cc9d9d56fc365ad0e36a
        URL_HASH SHA256=ff13202d21a7a09efea72534a0a041cd4971cb9ac66b53262275881387cc6871
        SOURCE_SUBDIR Build
    )
    FetchContent_MakeAvailable(jolt)
    if(NOT TARGET Jolt::Jolt)
        message(FATAL_ERROR "Jolt Physics did not provide the Jolt::Jolt target")
    endif()

    # Preserve the upstream MIT notice in a predictable build-tree location;
    # release packaging can copy this beside the other third-party notices.
    set(license_file "${jolt_SOURCE_DIR}/LICENSE")
    if(NOT EXISTS "${license_file}")
        message(FATAL_ERROR "The Jolt Physics source is missing its MIT license")
    endif()
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/third_party_licenses")
    configure_file("${license_file}"
        "${CMAKE_BINARY_DIR}/third_party_licenses/Jolt-MIT.txt" COPYONLY)
    set_property(TARGET Dep_Physics PROPERTY CONTAINER_THIRD_PARTY_LICENSE
        "${CMAKE_BINARY_DIR}/third_party_licenses/Jolt-MIT.txt")
endfunction()

container_fetch_jolt()
target_link_libraries(Dep_Physics INTERFACE Jolt::Jolt)
target_compile_definitions(Dep_Physics INTERFACE CONTAINER_HAS_JOLT=1)
