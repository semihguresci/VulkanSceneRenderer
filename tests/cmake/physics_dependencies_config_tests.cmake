if(NOT DEFINED PROJECT_SOURCE_DIR_FOR_TEST OR PROJECT_SOURCE_DIR_FOR_TEST STREQUAL "")
    message(FATAL_ERROR "PROJECT_SOURCE_DIR_FOR_TEST must point at the repository root.")
endif()
if(NOT DEFINED TEST_WORK_DIR OR TEST_WORK_DIR STREQUAL "")
    set(TEST_WORK_DIR "${CMAKE_CURRENT_BINARY_DIR}/physics_dependencies_config_tests")
endif()

set(physics_module "${PROJECT_SOURCE_DIR_FOR_TEST}/cmake/PhysicsDependencies.cmake")
if(NOT EXISTS "${physics_module}")
    message(FATAL_ERROR "Expected physics dependency module at ${physics_module}.")
endif()
file(TO_CMAKE_PATH "${physics_module}" PHYSICS_MODULE_FOR_FIXTURE)

# All enabled cases use an explicitly supplied miniature local source tree.
# FULLY_DISCONNECTED also makes accidental archive fetching fail immediately.
# No compiler, Jolt download or physics build is required for these contracts.
set(mock_jolt_template [=[
cmake_minimum_required(VERSION 3.23)
project(MockJolt LANGUAGES NONE)
option(DOUBLE_PRECISION "Mock double precision" OFF)
if(NOT DOUBLE_PRECISION)
    message(FATAL_ERROR "The physics dependency did not enable double precision")
endif()
foreach(option_name
        JPH_BUILD_SHARED_LIBS USE_STATIC_MSVC_RUNTIME_LIBRARY
        OVERRIDE_CXX_FLAGS INTERPROCEDURAL_OPTIMIZATION ENABLE_ALL_WARNINGS
        FLOATING_POINT_EXCEPTIONS_ENABLED CPP_EXCEPTIONS_ENABLED CPP_RTTI_ENABLED
        USE_SSE4_1 USE_SSE4_2 USE_AVX USE_AVX2 USE_AVX512
        USE_LZCNT USE_TZCNT USE_F16C USE_FMADD
        JPH_USE_DX12 JPH_USE_VK JPH_USE_MTL JPH_USE_CPU_COMPUTE
        DEBUG_RENDERER_IN_DEBUG_AND_RELEASE DEBUG_RENDERER_IN_DISTRIBUTION
        PROFILER_IN_DEBUG_AND_RELEASE PROFILER_IN_DISTRIBUTION JPH_USE_EXTERNAL_PROFILE
        ENABLE_OBJECT_STREAM ENABLE_INSTALL TARGET_UNIT_TESTS TARGET_HELLO_WORLD
        TARGET_PERFORMANCE_TEST TARGET_SAMPLES TARGET_VIEWER)
    option(${option_name} "Mock dependency option" ON)
    if(${option_name})
        message(FATAL_ERROR "Physics dependency left ${option_name} enabled")
    endif()
endforeach()
# Deliberately alter directory-local flags to verify they cannot escape into
# the parent project's compiler policy through the dependency function.
set(CMAKE_CXX_FLAGS "mock-child-cxx-flags")
set(CMAKE_CXX_FLAGS_RELEASE "mock-child-release-flags")
set(CMAKE_MSVC_RUNTIME_LIBRARY "mock-child-runtime")
if(NOT CASE_NAME STREQUAL "missing-target")
    add_library(Jolt INTERFACE)
    add_library(Jolt::Jolt ALIAS Jolt)
    target_compile_definitions(Jolt INTERFACE JPH_DOUBLE_PRECISION)
endif()
]=])

set(fixture_template [=[
cmake_minimum_required(VERSION 3.23)
project(PhysicsDependencyFixture LANGUAGES NONE)
set(FETCHCONTENT_FULLY_DISCONNECTED ON)
set(FETCHCONTENT_SOURCE_DIR_JOLT "@MOCK_SOURCE_FOR_FIXTURE@")
set(CASE_NAME "@CASE_FOR_FIXTURE@")
set(CMAKE_CXX_FLAGS "parent-cxx-flags")
set(CMAKE_CXX_FLAGS_RELEASE "parent-release-flags")
set(CMAKE_MSVC_RUNTIME_LIBRARY "parent-runtime")
# Existing generic cache choices must remain unchanged in the parent, while
# the dependency receives its own scoped settings under CMP0077.
set(USE_AVX ON CACHE BOOL "Parent fixture option" FORCE)
set(ENABLE_INSTALL ON CACHE BOOL "Parent fixture option" FORCE)
if(CASE_NAME STREQUAL "disabled")
    set(ENABLE_PHYSICS OFF)
else()
    set(ENABLE_PHYSICS ON)
endif()
include("@PHYSICS_MODULE_FOR_FIXTURE@")
include("@PHYSICS_MODULE_FOR_FIXTURE@")
if(NOT TARGET Dep_Physics)
    message(FATAL_ERROR "Physics dependency group is absent")
endif()
if(NOT CMAKE_CXX_FLAGS STREQUAL "parent-cxx-flags" OR
        NOT CMAKE_CXX_FLAGS_RELEASE STREQUAL "parent-release-flags" OR
        NOT CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "parent-runtime" OR
        NOT USE_AVX OR NOT ENABLE_INSTALL)
    message(FATAL_ERROR "Physics dependency changed the parent build policy")
endif()
get_target_property(physics_links Dep_Physics INTERFACE_LINK_LIBRARIES)
get_target_property(physics_definitions Dep_Physics INTERFACE_COMPILE_DEFINITIONS)
if(NOT ENABLE_PHYSICS)
    if(TARGET Jolt OR TARGET Jolt::Jolt OR physics_links OR physics_definitions OR
            EXISTS "${CMAKE_BINARY_DIR}/_deps" OR
            EXISTS "${CMAKE_BINARY_DIR}/third_party_licenses/Jolt-MIT.txt")
        message(FATAL_ERROR "Disabled physics configured or fetched a dependency")
    endif()
else()
    if(NOT physics_links STREQUAL "Jolt::Jolt" OR
            NOT physics_definitions STREQUAL "CONTAINER_HAS_JOLT=1")
        message(FATAL_ERROR "Enabled physics is missing its public dependency contract")
    endif()
    FetchContent_GetProperties(jolt SOURCE_DIR resolved_jolt_source)
    file(REAL_PATH "${resolved_jolt_source}" resolved_jolt_source)
    file(REAL_PATH "@MOCK_SOURCE_FOR_FIXTURE@" expected_jolt_source)
    if(NOT resolved_jolt_source STREQUAL expected_jolt_source)
        message(FATAL_ERROR "Local Jolt source override was not honored")
    endif()
    get_target_property(license Dep_Physics CONTAINER_THIRD_PARTY_LICENSE)
    if(NOT EXISTS "${license}")
        message(FATAL_ERROR "Physics dependency did not stage its license")
    endif()
    file(SHA256 "${license}" staged_license_hash)
    file(SHA256 "@MOCK_SOURCE_FOR_FIXTURE@/LICENSE" source_license_hash)
    if(NOT staged_license_hash STREQUAL source_license_hash)
        message(FATAL_ERROR "Staged physics license differs from its source")
    endif()
endif()
]=])

foreach(case_name disabled local-source missing-license missing-target)
    set(case_root "${TEST_WORK_DIR}/${case_name}")
    set(mock_source "${case_root}/mock-jolt")
    file(MAKE_DIRECTORY "${case_root}/fixture" "${mock_source}/Build")
    file(WRITE "${mock_source}/Build/CMakeLists.txt" "${mock_jolt_template}")
    if(NOT case_name STREQUAL "missing-license")
        file(WRITE "${mock_source}/LICENSE"
            "Mock MIT notice used only by the offline configuration fixture.\n")
    endif()
    set(CASE_FOR_FIXTURE "${case_name}")
    file(TO_CMAKE_PATH "${mock_source}" MOCK_SOURCE_FOR_FIXTURE)
    string(CONFIGURE "${fixture_template}" fixture_source @ONLY)
    file(WRITE "${case_root}/fixture/CMakeLists.txt" "${fixture_source}")
    set(configure_command
        "${CMAKE_COMMAND}" -S "${case_root}/fixture" -B "${case_root}/build")
    if(DEFINED TEST_CMAKE_GENERATOR AND NOT TEST_CMAKE_GENERATOR STREQUAL "")
        list(APPEND configure_command -G "${TEST_CMAKE_GENERATOR}")
    endif()
    if(DEFINED TEST_CMAKE_MAKE_PROGRAM AND NOT TEST_CMAKE_MAKE_PROGRAM STREQUAL "")
        list(APPEND configure_command "-DCMAKE_MAKE_PROGRAM:FILEPATH=${TEST_CMAKE_MAKE_PROGRAM}")
    endif()
    execute_process(COMMAND ${configure_command}
        RESULT_VARIABLE configure_result
        OUTPUT_VARIABLE configure_stdout ERROR_VARIABLE configure_stderr)
    file(WRITE "${case_root}/configure.log" "${configure_stdout}\n${configure_stderr}")
    if(case_name STREQUAL "missing-license")
        set(expected_error "missing its MIT license")
    elseif(case_name STREQUAL "missing-target")
        set(expected_error "did not provide the Jolt::Jolt target")
    else()
        set(expected_error "")
    endif()
    if(expected_error STREQUAL "")
        if(NOT configure_result EQUAL 0)
            message(FATAL_ERROR "Physics dependency ${case_name} failed.\n${configure_stdout}\n${configure_stderr}")
        endif()
    else()
        string(FIND "${configure_stdout}\n${configure_stderr}" "${expected_error}" error_position)
        if(configure_result EQUAL 0 OR error_position EQUAL -1)
            message(FATAL_ERROR "Physics dependency ${case_name} did not reject the expected contract.\n${configure_stdout}\n${configure_stderr}")
        endif()
    endif()
endforeach()
message(STATUS "Physics dependency offline configuration contracts passed (4 cases)")
