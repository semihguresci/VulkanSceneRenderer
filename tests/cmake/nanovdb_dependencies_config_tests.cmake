cmake_minimum_required(VERSION 3.23)
if(NOT PROJECT_SOURCE_DIR_FOR_TEST OR NOT TEST_WORK_DIR)
    message(FATAL_ERROR "PROJECT_SOURCE_DIR_FOR_TEST and TEST_WORK_DIR are required")
endif()
if(NOT TEST_CMAKE_GENERATOR)
    set(TEST_CMAKE_GENERATOR Ninja)
endif()

file(MAKE_DIRECTORY "${TEST_WORK_DIR}/source")
file(WRITE "${TEST_WORK_DIR}/source/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.23)
project(NanoVdbDependencyFixture LANGUAGES NONE)
include(FetchContent)
# Never resolve machine-installed headers or fetch an archive in this fixture.
set(FETCHCONTENT_FULLY_DISCONNECTED ON)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
set(CMAKE_FIND_USE_CMAKE_SYSTEM_PATH FALSE)
set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH FALSE)
set(CMAKE_FIND_USE_CMAKE_ENVIRONMENT_PATH FALSE)
set(CMAKE_FIND_USE_CMAKE_PATH FALSE)
set(CMAKE_FIND_USE_PACKAGE_REGISTRY FALSE)
set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY FALSE)
include("${PROJECT_SOURCE_DIR_FOR_TEST}/cmake/NanoVdbDependencies.cmake")
include("${PROJECT_SOURCE_DIR_FOR_TEST}/cmake/NanoVdbDependencies.cmake")
if(NOT TARGET Dep_NanoVDB OR NOT TARGET Dep_NanoVDB_IO)
    message(FATAL_ERROR "Missing optional NanoVDB dependency interface")
endif()
get_target_property(definitions Dep_NanoVDB INTERFACE_COMPILE_DEFINITIONS)
get_target_property(includes Dep_NanoVDB INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(system_includes Dep_NanoVDB INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
get_target_property(options Dep_NanoVDB INTERFACE_COMPILE_OPTIONS)
get_target_property(libraries Dep_NanoVDB INTERFACE_LINK_LIBRARIES)
FetchContent_GetProperties(container_nanovdb)
if((ENABLE_OPENVDB AND ENABLE_NANOVDB) OR ENABLE_NANOVDB_IO)
    if(ENABLE_OPENVDB AND ENABLE_NANOVDB)
        if(NOT CONTAINER_HAS_NANOVDB STREQUAL "1" OR
           NOT "CONTAINER_HAS_NANOVDB=1" IN_LIST definitions)
            message(FATAL_ERROR "Enabled NanoVDB rendering does not propagate availability")
        endif()
    elseif(NOT CONTAINER_HAS_NANOVDB STREQUAL "0" OR "CONTAINER_HAS_NANOVDB=1" IN_LIST definitions)
        message(FATAL_ERROR "Native I/O incorrectly enabled OpenVDB rendering")
    endif()
    if(ENABLE_OPENVDB)
        if(NOT "NANOVDB_USE_OPENVDB" IN_LIST definitions OR NOT "NANOVDB_USE_TBB" IN_LIST definitions)
            message(FATAL_ERROR "OpenVDB conversion must consistently use the arena-controlled TBB backend")
        endif()
    elseif("NANOVDB_USE_OPENVDB" IN_LIST definitions OR "NANOVDB_USE_TBB" IN_LIST definitions)
        message(FATAL_ERROR "Native-only I/O must not depend on OpenVDB or TBB")
    endif()
    get_target_property(io_definitions Dep_NanoVDB_IO INTERFACE_COMPILE_DEFINITIONS)
    get_target_property(io_libraries Dep_NanoVDB_IO INTERFACE_LINK_LIBRARIES)
    if(ENABLE_NANOVDB_IO)
        if(NOT CONTAINER_HAS_NANOVDB_IO STREQUAL "1" OR
           NOT "CONTAINER_HAS_NANOVDB_IO=1" IN_LIST io_definitions OR
           NOT io_libraries STREQUAL "Dep_NanoVDB")
            message(FATAL_ERROR "Independent native I/O does not propagate its header-only availability")
        endif()
    elseif(NOT CONTAINER_HAS_NANOVDB_IO STREQUAL "0" OR io_definitions OR io_libraries)
        message(FATAL_ERROR "Disabled I/O retained a dependency")
    endif()
    if(NOT includes STREQUAL EXPECTED_INCLUDE_DIR OR
       NOT system_includes STREQUAL EXPECTED_INCLUDE_DIR)
        message(FATAL_ERROR "Incorrect system include interface: ${includes}; ${system_includes}")
    endif()
    if(NOT "$<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus>" IN_LIST options)
        message(FATAL_ERROR "NanoVDB must enable correct __cplusplus reporting for MSVC")
    endif()
    if(libraries)
        message(FATAL_ERROR "Portable NanoVDB unexpectedly links libraries: ${libraries}")
    endif()
    if(NOT NANOVDB_LICENSE_FILE STREQUAL EXPECTED_LICENSE_FILE)
        message(FATAL_ERROR "Expected installed/source license ${EXPECTED_LICENSE_FILE}, got ${NANOVDB_LICENSE_FILE}")
    endif()
    if(EXPECT_FETCH)
        if(NOT container_nanovdb_POPULATED OR
           NOT container_nanovdb_SOURCE_DIR STREQUAL EXPECTED_UPSTREAM_SOURCE)
            message(FATAL_ERROR "Header-only FetchContent did not use its local source override")
        endif()
    elseif(container_nanovdb_POPULATED OR EXISTS "${CMAKE_BINARY_DIR}/_deps")
        message(FATAL_ERROR "Explicit NanoVDB headers unexpectedly populate upstream sources")
    endif()
else()
    if(NOT CONTAINER_HAS_NANOVDB STREQUAL "0" OR definitions OR includes OR
       system_includes OR options OR libraries OR container_nanovdb_POPULATED OR
       EXISTS "${CMAKE_BINARY_DIR}/_deps")
        message(FATAL_ERROR "Disabled NanoVDB still requires headers, definitions, flags or source population")
    endif()
endif()
get_property(languages GLOBAL PROPERTY ENABLED_LANGUAGES)
if(languages AND NOT languages STREQUAL "NONE")
    message(FATAL_ERROR "Header-only NanoVDB unexpectedly enables compiler languages: ${languages}")
endif()
if(CMAKE_C_COMPILER OR CMAKE_CXX_COMPILER OR CMAKE_CUDA_COMPILER OR TARGET nanovdb)
    message(FATAL_ERROR "Portable NanoVDB configured an upstream native/CUDA project")
endif()
]=])

set(explicit_prefix "${TEST_WORK_DIR}/explicit")
set(explicit_include "${explicit_prefix}/include")
file(MAKE_DIRECTORY "${explicit_include}/nanovdb/tools")
file(WRITE "${explicit_include}/nanovdb/PNanoVDB.h" "// portable-header fixture\n")
file(WRITE "${explicit_include}/nanovdb/tools/CreateNanoGrid.h" "// CPU-converter fixture\n")
file(WRITE "${explicit_prefix}/LICENSE" "NanoVDB fixture notice\n")
foreach(header IN ITEMS HostBuffer.h io/IO.h tools/GridValidator.h tools/GridChecksum.h)
    file(WRITE "${explicit_include}/nanovdb/${header}" "// native-I/O fixture\n")
endforeach()

# Mirror the upstream repository layout without a buildable math subproject.
# Poison the parent build files to catch accidental CUDA/tools configuration.
set(upstream "${TEST_WORK_DIR}/upstream")
set(upstream_include "${upstream}/nanovdb")
file(MAKE_DIRECTORY "${upstream_include}/nanovdb/tools" "${upstream_include}/nanovdb/math")
file(WRITE "${upstream_include}/nanovdb/PNanoVDB.h" "// upstream portable-header fixture\n")
file(WRITE "${upstream_include}/nanovdb/tools/CreateNanoGrid.h" "// upstream CPU-converter fixture\n")
file(WRITE "${upstream}/LICENSE" "NanoVDB upstream fixture notice\n")
foreach(parent IN ITEMS "${upstream}" "${upstream_include}")
    file(WRITE "${parent}/CMakeLists.txt"
        "message(FATAL_ERROR \"Do not configure upstream NanoVDB CUDA/tools projects\")\n")
endforeach()

set(missing_converter "${TEST_WORK_DIR}/missing-converter")
file(MAKE_DIRECTORY "${missing_converter}/nanovdb")
file(WRITE "${missing_converter}/nanovdb/PNanoVDB.h" "// no converter\n")
set(missing_portable "${TEST_WORK_DIR}/missing-portable")
file(MAKE_DIRECTORY "${missing_portable}/nanovdb/tools")
file(WRITE "${missing_portable}/nanovdb/tools/CreateNanoGrid.h" "// no portable GPU header\n")

function(check_config name openvdb_enabled nanovdb_enabled include_path succeeds)
    set(command "${CMAKE_COMMAND}"
        -S "${TEST_WORK_DIR}/source" -B "${TEST_WORK_DIR}/${name}"
        -G "${TEST_CMAKE_GENERATOR}"
        "-DPROJECT_SOURCE_DIR_FOR_TEST=${PROJECT_SOURCE_DIR_FOR_TEST}"
        "-DENABLE_OPENVDB=${openvdb_enabled}" "-DENABLE_NANOVDB=${nanovdb_enabled}"
        "-DENABLE_NANOVDB_IO=OFF" "-DENABLE_NANOVDB_ZIP=OFF" "-DENABLE_NANOVDB_BLOSC=OFF"
        "-DNANOVDB_INCLUDE_DIR=${include_path}"
        "-DFETCHCONTENT_SOURCE_DIR_CONTAINER_NANOVDB=${upstream}"
        ${ARGN})
    if(TEST_CMAKE_MAKE_PROGRAM)
        list(APPEND command "-DCMAKE_MAKE_PROGRAM=${TEST_CMAKE_MAKE_PROGRAM}")
    endif()
    execute_process(COMMAND ${command}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(succeeds AND NOT result EQUAL 0)
        message(FATAL_ERROR "${name} failed:\n${output}\n${errors}")
    elseif(NOT succeeds)
        if(result EQUAL 0 OR NOT "${output}${errors}" MATCHES "ENABLE_NANOVDB requires NanoVDB headers")
            message(FATAL_ERROR "${name} did not report invalid required headers:\n${output}\n${errors}")
        endif()
    endif()
endfunction()

check_config(both_disabled OFF OFF "${missing_converter}" TRUE)
check_config(openvdb_disabled OFF ON "${missing_converter}" TRUE)
check_config(nanovdb_disabled ON OFF "${missing_converter}" TRUE)
check_config(explicit_headers ON ON "${explicit_include}" TRUE
    "-DEXPECTED_INCLUDE_DIR=${explicit_include}"
    "-DEXPECTED_LICENSE_FILE=${explicit_prefix}/LICENSE")
check_config(native_io_only OFF OFF "${explicit_include}" TRUE
    -DENABLE_NANOVDB_IO=ON
    "-DEXPECTED_INCLUDE_DIR=${explicit_include}"
    "-DEXPECTED_LICENSE_FILE=${explicit_prefix}/LICENSE")
check_config(native_io_with_openvdb ON OFF "${explicit_include}" TRUE
    -DENABLE_NANOVDB_IO=ON
    "-DEXPECTED_INCLUDE_DIR=${explicit_include}"
    "-DEXPECTED_LICENSE_FILE=${explicit_prefix}/LICENSE")
check_config(local_fetchcontent ON ON "NANOVDB_INCLUDE_DIR-NOTFOUND" TRUE
    -DEXPECT_FETCH=ON "-DEXPECTED_UPSTREAM_SOURCE=${upstream}"
    "-DEXPECTED_INCLUDE_DIR=${upstream_include}"
    "-DEXPECTED_LICENSE_FILE=${upstream}/LICENSE")
check_config(missing_converter_header ON ON "${missing_converter}" FALSE)
check_config(missing_portable_header ON ON "${missing_portable}" FALSE)
message(STATUS "NanoVDB dependency configuration checks passed")
