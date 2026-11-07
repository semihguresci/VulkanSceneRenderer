include_guard(GLOBAL)

# Standalone defaults also permit small CPU-only integration/configuration tests.
option(ENABLE_GLTF_DRACO "Decode Draco glTF geometry in a bounded worker" OFF)
option(ENABLE_GLTF_MESHOPT "Decode KHR/EXT meshopt glTF buffer views" OFF)
option(CONTAINER_FETCH_GLTF_COMPRESSION "Fetch pinned missing glTF decoder dependencies" OFF)
set(CONTAINER_GLTF_DRACO_LICENSE_FILE "" CACHE FILEPATH "License/notice for a package-provided Draco dependency")
set(CONTAINER_GLTF_MESHOPT_LICENSE_FILE "" CACHE FILEPATH "License/notice for a package-provided meshoptimizer dependency")

add_library(Dep_GltfCompression INTERFACE)
if(NOT ENABLE_GLTF_DRACO AND NOT ENABLE_GLTF_MESHOPT)
    return()
endif()

include(FetchContent)

function(container_stage_compression_notice dependency package_dir override_file filename)
    if(override_file)
        if(NOT EXISTS "${override_file}" OR IS_DIRECTORY "${override_file}")
            message(FATAL_ERROR "${dependency} license override is not a readable file: ${override_file}")
        endif()
        set(notice "${override_file}")
    else()
        # Package managers commonly place the unmodified upstream license in
        # their config directory (vcpkg: share/<port>/copyright). Never guess a
        # notice from another installed package or claim one was staged.
        find_file(notice NAMES copyright LICENSE LICENSE.md LICENSE.txt
            PATHS "${package_dir}" "${package_dir}/../../../share/${dependency}"
            NO_DEFAULT_PATH NO_CACHE)
        if(NOT notice)
            message(FATAL_ERROR "${dependency} package does not expose its license. Set CONTAINER_GLTF_${dependency}_LICENSE_FILE to the package's upstream license/notice file.")
        endif()
    endif()
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/third_party_licenses")
    configure_file("${notice}" "${CMAKE_BINARY_DIR}/third_party_licenses/${filename}" COPYONLY)
endfunction()

function(container_fetch_meshoptimizer)
    cmake_policy(SET CMP0077 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    set(MESHOPT_BUILD_DEMO OFF)
    set(MESHOPT_BUILD_GLTFPACK OFF)
    set(MESHOPT_BUILD_SHARED_LIBS OFF)
    set(MESHOPT_WERROR OFF)
    set(MESHOPT_INSTALL OFF)
    FetchContent_Declare(container_meshoptimizer
        URL https://codeload.github.com/zeux/meshoptimizer/zip/9e1f07b159d3cb777f1c67ed31fc11fd117986f4
        URL_HASH SHA256=0a26e578edbe4c392fdc7c41033d50cde2abbf8f598888b1abef6a6333418f49)
    FetchContent_MakeAvailable(container_meshoptimizer)
    set(container_meshoptimizer_FETCHED TRUE PARENT_SCOPE)
    if(TARGET meshoptimizer AND NOT TARGET meshoptimizer::meshoptimizer)
        add_library(meshoptimizer::meshoptimizer ALIAS meshoptimizer)
    endif()
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/third_party_licenses")
    configure_file("${container_meshoptimizer_SOURCE_DIR}/LICENSE.md"
        "${CMAKE_BINARY_DIR}/third_party_licenses/meshoptimizer-MIT.txt" COPYONLY)
endfunction()

function(container_fetch_draco)
    cmake_policy(SET CMP0077 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    set(BUILD_SHARED_LIBS OFF)
    set(DRACO_BUILD_EXECUTABLES OFF)
    set(DRACO_TESTS OFF)
    # Draco still maps a parent ENABLE_TESTS variable to its deprecated test
    # option. Shadow it only in this dependency scope; retain project tests.
    set(ENABLE_TESTS OFF)
    set(DRACO_JS_GLUE OFF)
    set(DRACO_UNITY_PLUGIN OFF)
    set(DRACO_MAYA_PLUGIN OFF)
    set(DRACO_TRANSCODER_SUPPORTED OFF)
    set(DRACO_PMC_SUPPORTED OFF)
    set(DRACO_GLTF_BITSTREAM ON)
    FetchContent_Declare(container_draco
        URL https://codeload.github.com/google/draco/zip/554628821c3448319d7d4ae48db170b8c63e1aaf
        URL_HASH SHA256=7c83c9c8e96e264e97d91209dc5e9dd3fc999adf88704abe68065752196e3d36)
    FetchContent_MakeAvailable(container_draco)
    set(container_draco_FETCHED TRUE PARENT_SCOPE)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/third_party_licenses")
    configure_file("${container_draco_SOURCE_DIR}/LICENSE"
        "${CMAKE_BINARY_DIR}/third_party_licenses/Draco-Apache-2.0.txt" COPYONLY)
endfunction()

if(ENABLE_GLTF_MESHOPT)
    # Upstream uses ExactVersion config compatibility: asking for1.1 rejects
    # an otherwise compatible installed1.3 package. Find its config first,
    # then check the required stable header/API instead of that version policy.
    find_package(meshoptimizer CONFIG QUIET)
    if(TARGET meshoptimizer::meshoptimizer)
        include(CheckCXXSourceCompiles)
        function(container_check_meshoptimizer_api)
            set(CMAKE_REQUIRED_LIBRARIES meshoptimizer::meshoptimizer)
            unset(CONTAINER_MESHOPT_STABLE_API CACHE)
            check_cxx_source_compiles("
                #include <meshoptimizer.h>
                static_assert(MESHOPTIMIZER_VERSION >= 1010);
                int main() {
                    auto* volatile color = &meshopt_decodeFilterColor;
                    auto* volatile version = &meshopt_decodeVertexVersion;
                    return color && version ? 0 : 1;
                }" CONTAINER_MESHOPT_STABLE_API)
            if(NOT CONTAINER_MESHOPT_STABLE_API)
                message(FATAL_ERROR "Installed meshoptimizer must provide stable version>=1.1 COLOR and bitstream-version APIs. Select a newer meshoptimizer_DIR/CMAKE_PREFIX_PATH.")
            endif()
        endfunction()
        container_check_meshoptimizer_api()
    endif()
    if(NOT TARGET meshoptimizer::meshoptimizer AND CONTAINER_FETCH_GLTF_COMPRESSION)
        container_fetch_meshoptimizer()
    endif()
    if(NOT TARGET meshoptimizer::meshoptimizer)
        message(FATAL_ERROR "ENABLE_GLTF_MESHOPT requires meshoptimizer>=1.1. Set meshoptimizer_DIR/CMAKE_PREFIX_PATH or explicitly enable CONTAINER_FETCH_GLTF_COMPRESSION.")
    endif()
    if(NOT container_meshoptimizer_FETCHED)
        container_stage_compression_notice(MESHOPT "${meshoptimizer_DIR}"
            "${CONTAINER_GLTF_MESHOPT_LICENSE_FILE}" meshoptimizer-MIT.txt)
    endif()
    target_link_libraries(Dep_GltfCompression INTERFACE meshoptimizer::meshoptimizer)
    target_compile_definitions(Dep_GltfCompression INTERFACE CONTAINER_HAS_MESHOPT=1)
endif()

if(ENABLE_GLTF_DRACO)
    find_package(draco 1.5.9 CONFIG QUIET)
    if(NOT TARGET draco::draco AND CONTAINER_FETCH_GLTF_COMPRESSION)
        container_fetch_draco()
    endif()
    if(NOT TARGET draco::draco)
        message(FATAL_ERROR "ENABLE_GLTF_DRACO requires Draco>=1.5.9. Set draco_DIR/CMAKE_PREFIX_PATH or explicitly enable CONTAINER_FETCH_GLTF_COMPRESSION.")
    endif()
    if(NOT container_draco_FETCHED)
        container_stage_compression_notice(DRACO "${draco_DIR}"
            "${CONTAINER_GLTF_DRACO_LICENSE_FILE}" Draco-Apache-2.0.txt)
    endif()
    # Link Draco only to ContainerDracoWorker. The geometry/client process must
    # not gain an in-process decoder or TINYGLTF_ENABLE_DRACO definition.
    target_compile_definitions(Dep_GltfCompression INTERFACE CONTAINER_HAS_DRACO=1)
endif()
