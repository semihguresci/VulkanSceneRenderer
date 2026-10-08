if(NOT DEFINED PROJECT_SOURCE_DIR_FOR_TEST OR PROJECT_SOURCE_DIR_FOR_TEST STREQUAL "")
    message(FATAL_ERROR "PROJECT_SOURCE_DIR_FOR_TEST must point at the repository root.")
endif()

if(NOT DEFINED TEST_WORK_DIR OR TEST_WORK_DIR STREQUAL "")
    set(TEST_WORK_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders_incremental_config_tests")
endif()

set(shader_cmake "${PROJECT_SOURCE_DIR_FOR_TEST}/cmake/Shaders.cmake")
if(NOT EXISTS "${shader_cmake}")
    message(FATAL_ERROR "Expected shader script at ${shader_cmake}.")
endif()

file(REMOVE_RECURSE "${TEST_WORK_DIR}")
file(MAKE_DIRECTORY
    "${TEST_WORK_DIR}/fixture/shaders"
    "${TEST_WORK_DIR}/fake-vulkan/Bin")

file(TO_CMAKE_PATH "${shader_cmake}" shader_cmake_for_include)
file(WRITE "${TEST_WORK_DIR}/fixture/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.23)\n"
    "project(ShaderFixture LANGUAGES NONE)\n"
    "include(\"${shader_cmake_for_include}\")\n")

file(WRITE "${TEST_WORK_DIR}/fixture/shaders/gbuffer.slang"
    "void vertMain() {}\n"
    "void fragMain() {}\n")
file(WRITE "${TEST_WORK_DIR}/fixture/shaders/forward_opaque.slang"
    "void vertMain() {}\n"
    "void fragMain() {}\n")
file(WRITE "${TEST_WORK_DIR}/fixture/shaders/surface_normals.slang"
    "void vertMain() {}\n"
    "void geomMain() {}\n"
    "void fragMain() {}\n")
file(WRITE "${TEST_WORK_DIR}/fixture/shaders/tile_light_cull.slang"
    "void computeMain() {}\n")
file(WRITE "${TEST_WORK_DIR}/fixture/shaders/brdf_common.slang"
    "float fixtureCommon() { return 1.0; }\n")

set(invocation_log "${TEST_WORK_DIR}/slangc_invocations.txt")

if(WIN32)
    set(fake_slangc "${TEST_WORK_DIR}/fake-vulkan/Bin/slangc.cmd")
    file(WRITE "${fake_slangc}"
        "@echo off\r\n"
        "setlocal enabledelayedexpansion\r\n"
        "set \"out=\"\r\n"
        "if not \"%FAKE_SLANGC_LOG%\"==\"\" echo %*>>\"%FAKE_SLANGC_LOG%\"\r\n"
        ":parse_args\r\n"
        "if \"%~1\"==\"\" goto done_parse\r\n"
        "if not \"%~1\"==\"-o\" goto next_arg\r\n"
        "shift\r\n"
        "set \"out=%~1\"\r\n"
        ":next_arg\r\n"
        "shift\r\n"
        "goto parse_args\r\n"
        ":done_parse\r\n"
        "if \"!out!\"==\"\" exit /b 2\r\n"
        "for %%I in (\"!out!\") do if not exist \"%%~dpI\" mkdir \"%%~dpI\"\r\n"
        "set \"FAKE_SLANGC_OUTPUT=!out!\"\r\n"
        "powershell -NoProfile -ExecutionPolicy Bypass -Command \"[System.IO.File]::WriteAllBytes($env:FAKE_SLANGC_OUTPUT, [byte[]](0x03, 0x02, 0x23, 0x07))\"\r\n"
        "exit /b %ERRORLEVEL%\r\n")
else()
    set(fake_slangc "${TEST_WORK_DIR}/fake-vulkan/Bin/slangc")
    file(WRITE "${fake_slangc}"
        "#!/bin/sh\n"
        "if [ -n \"$FAKE_SLANGC_LOG\" ]; then printf '%s\\n' \"$*\" >> \"$FAKE_SLANGC_LOG\"; fi\n"
        "out=''\n"
        "while [ \"$#\" -gt 0 ]; do\n"
        "  if [ \"$1\" = '-o' ]; then shift; out=\"$1\"; fi\n"
        "  shift\n"
        "done\n"
        "if [ -z \"$out\" ]; then exit 2; fi\n"
        "mkdir -p \"$(dirname \"$out\")\"\n"
        "printf '\\003\\002#\\007' > \"$out\"\n")
    file(CHMOD "${fake_slangc}"
        PERMISSIONS
            OWNER_READ OWNER_WRITE OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE)
endif()

function(count_slang_invocations output_var)
    if(EXISTS "${invocation_log}")
        file(STRINGS "${invocation_log}" invocations)
        list(LENGTH invocations count)
    else()
        set(count 0)
    endif()
    set(${output_var} "${count}" PARENT_SCOPE)
endfunction()

function(expect_output_invocations expected_count)
    file(STRINGS "${invocation_log}" invocations)
    foreach(expected_output IN LISTS ARGN)
        set(count 0)
        foreach(invocation IN LISTS invocations)
            string(FIND "${invocation}" "${expected_output}" output_position)
            if(NOT output_position EQUAL -1)
                math(EXPR count "${count} + 1")
            endif()
        endforeach()
        if(NOT count EQUAL expected_count)
            message(FATAL_ERROR
                "Expected ${expected_output} to compile ${expected_count} times, "
                "but observed ${count} invocations.")
        endif()
    endforeach()
endfunction()

set(ENV{VULKAN_SDK} "${TEST_WORK_DIR}/fake-vulkan")
set(ENV{FAKE_SLANGC_LOG} "${invocation_log}")

set(configure_command
    "${CMAKE_COMMAND}"
    -S "${TEST_WORK_DIR}/fixture"
    -B "${TEST_WORK_DIR}/build"
    "-DSLANGC_EXECUTABLE:FILEPATH=${fake_slangc}")
if(DEFINED TEST_CMAKE_GENERATOR AND NOT TEST_CMAKE_GENERATOR STREQUAL "")
    list(APPEND configure_command -G "${TEST_CMAKE_GENERATOR}")
endif()
if(DEFINED TEST_CMAKE_MAKE_PROGRAM AND NOT TEST_CMAKE_MAKE_PROGRAM STREQUAL "")
    list(APPEND configure_command "-DCMAKE_MAKE_PROGRAM:FILEPATH=${TEST_CMAKE_MAKE_PROGRAM}")
endif()

execute_process(
    COMMAND ${configure_command}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "Expected shader fixture project to configure.\n"
        "stdout:\n${configure_stdout}\n"
        "stderr:\n${configure_stderr}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${TEST_WORK_DIR}/build" --target shaders
    RESULT_VARIABLE first_build_result
    OUTPUT_VARIABLE first_build_stdout
    ERROR_VARIABLE first_build_stderr
)
if(NOT first_build_result EQUAL 0)
    message(FATAL_ERROR
        "Expected first shader build to succeed.\n"
        "stdout:\n${first_build_stdout}\n"
        "stderr:\n${first_build_stderr}")
endif()

set(spv_dir "${TEST_WORK_DIR}/build/spv_shaders")
set(expected_outputs
        gbuffer.vert.spv
        gbuffer.frag.spv
        forward_opaque.vert.spv
        forward_opaque.frag.spv
        forward_opaque_ray.frag.spv
        surface_normals.vert.spv
        surface_normals.geom.spv
        surface_normals.frag.spv
        tile_light_cull.comp.spv)
foreach(expected_output IN LISTS expected_outputs)
    if(NOT EXISTS "${spv_dir}/${expected_output}")
        message(FATAL_ERROR "Expected shader output ${expected_output} to be generated.")
    endif()
endforeach()

if(EXISTS "${spv_dir}/brdf_common.vert.spv" OR EXISTS "${spv_dir}/brdf_common.frag.spv")
    message(FATAL_ERROR "Include-only shader brdf_common.slang was compiled as a render shader.")
endif()

count_slang_invocations(first_invocation_count)
list(LENGTH expected_outputs expected_invocation_count)
if(NOT first_invocation_count EQUAL expected_invocation_count)
    message(FATAL_ERROR
        "Expected first shader build to compile ${expected_invocation_count} "
        "outputs once, but observed ${first_invocation_count} invocations.")
endif()
expect_output_invocations(1 ${expected_outputs})

file(STRINGS "${invocation_log}" invocations)
foreach(invocation IN LISTS invocations)
    string(FIND "${invocation}" "forward_opaque_ray.frag.spv" ray_output)
    string(FIND "${invocation}" "-DFORWARD_RAY_QUERY=1" ray_define)
    string(FIND "${invocation}" "-capability spvRayQueryKHR" ray_capability)
    if(NOT ray_output EQUAL -1)
        if(ray_define EQUAL -1 OR ray_capability EQUAL -1)
            message(FATAL_ERROR
                "Forward ray variant must enable its define and ray-query capability: ${invocation}")
        endif()
    elseif(NOT ray_define EQUAL -1 OR NOT ray_capability EQUAL -1)
        message(FATAL_ERROR
            "Forward ray compiler flags leaked into a baseline shader: ${invocation}")
    endif()
endforeach()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${TEST_WORK_DIR}/build" --target shaders
    RESULT_VARIABLE second_build_result
    OUTPUT_VARIABLE second_build_stdout
    ERROR_VARIABLE second_build_stderr
)
if(NOT second_build_result EQUAL 0)
    message(FATAL_ERROR
        "Expected second shader build to succeed.\n"
        "stdout:\n${second_build_stdout}\n"
        "stderr:\n${second_build_stderr}")
endif()

count_slang_invocations(second_invocation_count)
if(NOT second_invocation_count EQUAL first_invocation_count)
    message(FATAL_ERROR
        "Expected unchanged shader target to be incremental, but fake slangc "
        "invocations changed from ${first_invocation_count} to "
        "${second_invocation_count}.")
endif()

file(APPEND "${TEST_WORK_DIR}/fixture/shaders/gbuffer.slang"
    "float changedFixtureValue() { return 2.0; }\n")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${TEST_WORK_DIR}/build" --target shaders
    RESULT_VARIABLE changed_build_result
    OUTPUT_VARIABLE changed_build_stdout
    ERROR_VARIABLE changed_build_stderr
)
if(NOT changed_build_result EQUAL 0)
    message(FATAL_ERROR
        "Expected changed shader build to succeed.\n"
        "stdout:\n${changed_build_stdout}\n"
        "stderr:\n${changed_build_stderr}")
endif()

count_slang_invocations(changed_invocation_count)
math(EXPR changed_invocation_delta
    "${changed_invocation_count} - ${second_invocation_count}")
if(NOT changed_invocation_delta EQUAL 2)
    message(FATAL_ERROR
        "Expected changing gbuffer.slang to rebuild only vert/frag outputs, "
        "but fake slangc invocation delta was ${changed_invocation_delta}.")
endif()
expect_output_invocations(2 gbuffer.vert.spv gbuffer.frag.spv)
expect_output_invocations(1
    forward_opaque.vert.spv forward_opaque.frag.spv forward_opaque_ray.frag.spv)

file(APPEND "${TEST_WORK_DIR}/fixture/shaders/forward_opaque.slang"
    "float changedForwardFixtureValue() { return 3.0; }\n")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${TEST_WORK_DIR}/build" --target shaders
    RESULT_VARIABLE forward_changed_build_result
    OUTPUT_VARIABLE forward_changed_build_stdout
    ERROR_VARIABLE forward_changed_build_stderr
)
if(NOT forward_changed_build_result EQUAL 0)
    message(FATAL_ERROR
        "Expected changed forward shader build to succeed.\n"
        "stdout:\n${forward_changed_build_stdout}\n"
        "stderr:\n${forward_changed_build_stderr}")
endif()

count_slang_invocations(forward_changed_invocation_count)
math(EXPR forward_changed_invocation_delta
    "${forward_changed_invocation_count} - ${changed_invocation_count}")
if(NOT forward_changed_invocation_delta EQUAL 3)
    message(FATAL_ERROR
        "Expected changing forward_opaque.slang to rebuild only its vertex, "
        "baseline fragment and ray fragment outputs, but fake slangc invocation "
        "delta was ${forward_changed_invocation_delta}.")
endif()
expect_output_invocations(2
    gbuffer.vert.spv gbuffer.frag.spv
    forward_opaque.vert.spv forward_opaque.frag.spv forward_opaque_ray.frag.spv)
expect_output_invocations(1
    surface_normals.vert.spv surface_normals.geom.spv surface_normals.frag.spv
    tile_light_cull.comp.spv)
