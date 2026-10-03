if(NOT DEFINED PROJECT_SOURCE_DIR_FOR_TEST OR NOT DEFINED TEST_WORK_DIR)
    message(FATAL_ERROR "The project source and test work directories are required.")
endif()

get_filename_component(TEST_WORK_DIR "${TEST_WORK_DIR}" ABSOLUTE)
file(MAKE_DIRECTORY "${TEST_WORK_DIR}")
set(revision "0123456789012345678901234567890123456789")
set(renamed_root "Renamed-Repository-${revision}")
set(archive_dir "${TEST_WORK_DIR}/archive")
file(MAKE_DIRECTORY "${archive_dir}/${renamed_root}")
file(WRITE "${archive_dir}/${renamed_root}/sample.ifc" "sample fixture\n")
set(archive_path "${TEST_WORK_DIR}/samples.zip")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${archive_path}" --format=zip "${renamed_root}"
    WORKING_DIRECTORY "${archive_dir}"
    RESULT_VARIABLE archive_result
)
if(NOT archive_result EQUAL 0)
    message(FATAL_ERROR "Failed to create the sample archive fixture.")
endif()

set(ref_path "${TEST_WORK_DIR}/ref.json")
file(WRITE "${ref_path}" "{\"object\":{\"sha\":\"${revision}\"}}\n")
if(WIN32)
    set(file_prefix "file:///")
else()
    set(file_prefix "file://")
endif()
set(destination "${TEST_WORK_DIR}/samples")
set(stamp "${destination}/.fetched")
# Force the download path even when this test has run before.
file(REMOVE "${stamp}")
set(fetch_command
    "${CMAKE_COMMAND}"
    "-DDESTINATION:PATH=${destination}"
    "-DSTAMP_FILE:FILEPATH=${stamp}"
    "-DREPO_REF_URL:STRING=${file_prefix}${ref_path}"
    "-DREPO_ARCHIVE_URL:STRING=${file_prefix}${archive_path}"
    -P "${PROJECT_SOURCE_DIR_FOR_TEST}/cmake/fetch_buildingsmart_sample_test_files.cmake"
)
execute_process(
    COMMAND ${fetch_command}
    RESULT_VARIABLE fetch_result
    OUTPUT_VARIABLE fetch_output
    ERROR_VARIABLE fetch_error
)
if(NOT fetch_result EQUAL 0)
    message(FATAL_ERROR "Fetching a renamed archive failed:\n${fetch_output}\n${fetch_error}")
endif()
if(NOT EXISTS "${destination}/sample.ifc" OR NOT EXISTS "${stamp}")
    message(FATAL_ERROR "The sample and revision stamp were not installed.")
endif()
file(READ "${stamp}" stamp_text)
if(NOT stamp_text MATCHES "${revision}")
    message(FATAL_ERROR "The installed revision was not recorded.")
endif()

# Cached files must remain usable without downloading the archive again.
file(REMOVE "${archive_path}")
execute_process(
    COMMAND ${fetch_command}
    RESULT_VARIABLE cached_result
    OUTPUT_VARIABLE cached_output
    ERROR_VARIABLE cached_error
)
if(NOT cached_result EQUAL 0 OR NOT cached_output MATCHES "skipping download")
    message(FATAL_ERROR "Reusing cached samples failed:\n${cached_output}\n${cached_error}")
endif()
if(NOT EXISTS "${destination}/sample.ifc")
    message(FATAL_ERROR "The cached sample was lost.")
endif()
