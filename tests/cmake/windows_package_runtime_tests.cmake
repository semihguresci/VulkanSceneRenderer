cmake_minimum_required(VERSION 3.23)
if(NOT WIN32 OR NOT PROJECT_SOURCE_DIR_FOR_TEST OR NOT TEST_RUNTIME_DIRECTORY OR NOT TEST_WORK_DIR)
    message(FATAL_ERROR "Windows, PROJECT_SOURCE_DIR_FOR_TEST, TEST_RUNTIME_DIRECTORY and TEST_WORK_DIR are required")
endif()
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef run_id)
set(TEST_WORK_DIR "${TEST_WORK_DIR}/run-${run_id}")
file(MAKE_DIRECTORY "${TEST_WORK_DIR}")
set(executables)
foreach(name IN ITEMS VulkanSceneRenderer OpenVdbTool OpenVdbTransportRender)
    set(source "${TEST_RUNTIME_DIRECTORY}/${name}.exe")
    if(NOT EXISTS "${source}")
        message(FATAL_ERROR "Missing built executable: ${source}")
    endif()
    file(COPY_FILE "${source}" "${TEST_WORK_DIR}/${name}.exe")
    list(APPEND executables "${source}")
endforeach()
set(image_shader "${TEST_RUNTIME_DIRECTORY}/spv_shaders/openvdb_transport_reference.comp.spv")
if(NOT EXISTS "${image_shader}")
    message(FATAL_ERROR "Missing production transport image shader: ${image_shader}")
endif()
file(MAKE_DIRECTORY "${TEST_WORK_DIR}/spv_shaders")
file(COPY_FILE "${image_shader}" "${TEST_WORK_DIR}/spv_shaders/openvdb_transport_reference.comp.spv")
string(JOIN "|" executables ${executables})
execute_process(COMMAND "${CMAKE_COMMAND}" "-DEXECUTABLES=${executables}"
    "-DDESTINATION=${TEST_WORK_DIR}" "-DDUMPBIN=${TEST_DUMPBIN}"
    -P "${PROJECT_SOURCE_DIR_FOR_TEST}/cmake/StageWindowsPackageRuntime.cmake"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Runtime staging failed:\n${output}\n${errors}")
endif()
if(NOT EXISTS "${TEST_WORK_DIR}/openvdb.dll")
    message(FATAL_ERROR "Enabled package is missing OpenVDB")
endif()
# Avoid resolving dependencies from the developer's vcpkg or compiler PATH.
set(ENV{PATH} "$ENV{SystemRoot}/System32;$ENV{SystemRoot}")
file(MAKE_DIRECTORY "${TEST_WORK_DIR}/missing-dependencies")
file(COPY_FILE "${TEST_RUNTIME_DIRECTORY}/OpenVdbTool.exe" "${TEST_WORK_DIR}/missing-dependencies/OpenVdbTool.exe")
execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DEXECUTABLES=${TEST_WORK_DIR}/missing-dependencies/OpenVdbTool.exe"
    "-DDESTINATION=${TEST_WORK_DIR}/rejected-stage" "-DDUMPBIN=${TEST_DUMPBIN}"
    -P "${PROJECT_SOURCE_DIR_FOR_TEST}/cmake/StageWindowsPackageRuntime.cmake"
    RESULT_VARIABLE missing_result OUTPUT_VARIABLE missing_output ERROR_VARIABLE missing_errors)
if(missing_result EQUAL 0 OR NOT "${missing_output}${missing_errors}" MATCHES "Unresolved package runtime dependencies")
    message(FATAL_ERROR "Incomplete runtime was not rejected:\n${missing_output}\n${missing_errors}")
endif()
execute_process(COMMAND "${TEST_WORK_DIR}/OpenVdbTool.exe" --sphere "${TEST_WORK_DIR}/sphere.vdb"
    --voxel-size 0.2 WORKING_DIRECTORY "${TEST_WORK_DIR}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
if(NOT result EQUAL 0 OR NOT EXISTS "${TEST_WORK_DIR}/sphere.vdb")
    message(FATAL_ERROR "Staged OpenVDB tool failed on clean PATH (${result}):\n${output}\n${errors}")
endif()
execute_process(COMMAND "${TEST_WORK_DIR}/OpenVdbTool.exe" --nano-capabilities
    WORKING_DIRECTORY "${TEST_WORK_DIR}" RESULT_VARIABLE result
    OUTPUT_VARIABLE nano_output ERROR_VARIABLE errors TIMEOUT 30)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Staged native capability discovery failed: ${errors}")
endif()
string(JSON native_io GET "${nano_output}" nativeIo)
if(native_io)
    set(native_codecs none)
    foreach(codec IN ITEMS zip blosc)
        string(JSON available GET "${nano_output}" "${codec}")
        if(available)
            list(APPEND native_codecs "${codec}")
        endif()
    endforeach()
    foreach(codec IN LISTS native_codecs)
        execute_process(COMMAND "${TEST_WORK_DIR}/OpenVdbTool.exe" --to-nano
            "${TEST_WORK_DIR}/sphere.vdb" "${TEST_WORK_DIR}/${codec}.nvdb"
            --codec "${codec}" --allow-float-map-rounding
            WORKING_DIRECTORY "${TEST_WORK_DIR}" RESULT_VARIABLE result
            OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Staged ${codec} native writer failed on clean PATH: ${output}\n${errors}")
        endif()
        execute_process(COMMAND "${TEST_WORK_DIR}/OpenVdbTool.exe" --validate-nano
            "${TEST_WORK_DIR}/${codec}.nvdb" --require-full-checksum
            WORKING_DIRECTORY "${TEST_WORK_DIR}" RESULT_VARIABLE result
            OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Staged ${codec} native reader failed on clean PATH: ${output}\n${errors}")
        endif()
    endforeach()
    execute_process(COMMAND "${TEST_WORK_DIR}/OpenVdbTool.exe" --from-nano
        "${TEST_WORK_DIR}/none.nvdb" "${TEST_WORK_DIR}/restored.vdb"
        WORKING_DIRECTORY "${TEST_WORK_DIR}" RESULT_VARIABLE result
        OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Staged native-to-VDB converter failed on clean PATH: ${output}\n${errors}")
    endif()
    execute_process(COMMAND "${TEST_WORK_DIR}/OpenVdbTool.exe" --stats-nano
        "${TEST_WORK_DIR}/none.nvdb" "${TEST_WORK_DIR}/statistics.nvdb" --native-statistics all
        WORKING_DIRECTORY "${TEST_WORK_DIR}" RESULT_VARIABLE result
        OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Staged native statistics failed on clean PATH: ${output}\n${errors}")
    endif()
    string(JSON native_average GET "${output}" summaries 0 average)
    if(native_average STREQUAL "")
        message(FATAL_ERROR "Staged native statistics omitted its scalar average")
    endif()
endif()
file(WRITE "${TEST_WORK_DIR}/vacuum.json"
    "{\"schemaVersion\":1,\"camera\":{\"width\":2,\"height\":2},\"environment\":[1,0.5,0.25]}")
execute_process(COMMAND "${TEST_WORK_DIR}/OpenVdbTransportRender.exe"
    --scene "${TEST_WORK_DIR}/vacuum.json" --output "${TEST_WORK_DIR}/vacuum.exr"
    --samples 2 --order 0 WORKING_DIRECTORY "${TEST_WORK_DIR}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
if(NOT result EQUAL 0 OR NOT EXISTS "${TEST_WORK_DIR}/vacuum.exr")
    message(FATAL_ERROR "Staged transport image tool failed on clean PATH (${result}):\n${output}\n${errors}")
endif()
file(READ "${TEST_WORK_DIR}/vacuum.exr" exr_magic OFFSET 0 LIMIT 4 HEX)
if(NOT exr_magic STREQUAL "762f3101")
    message(FATAL_ERROR "Staged image tool did not write OpenEXR")
endif()
execute_process(COMMAND "${TEST_WORK_DIR}/VulkanSceneRenderer.exe" --help
    WORKING_DIRECTORY "${TEST_WORK_DIR}" RESULT_VARIABLE result
    OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Staged renderer failed on clean PATH (${result}):\n${output}\n${errors}")
endif()
message(STATUS "Windows package runtime staging and clean-PATH executable checks passed")
