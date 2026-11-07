cmake_minimum_required(VERSION 3.23)
if(NOT WIN32)
    message(FATAL_ERROR "Windows runtime staging requires Windows")
endif()
if(NOT EXECUTABLES OR NOT DESTINATION)
    message(FATAL_ERROR "EXECUTABLES and DESTINATION are required")
endif()
string(REPLACE "|" ";" executables "${EXECUTABLES}")
set(runtime_directories)
foreach(executable IN LISTS executables)
    if(NOT EXISTS "${executable}")
        message(FATAL_ERROR "Missing runtime executable: ${executable}")
    endif()
    get_filename_component(runtime_directory "${executable}" DIRECTORY)
    list(APPEND runtime_directories "${runtime_directory}")
endforeach()
list(REMOVE_DUPLICATES runtime_directories)
if(DUMPBIN)
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${DUMPBIN}")
endif()
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "dumpbin")
# The separately installed VC runtime is the same prerequisite as the viewer.
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES ${executables}
    DIRECTORIES ${runtime_directories}
    RESOLVED_DEPENDENCIES_VAR dependencies
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    CONFLICTING_DEPENDENCIES_PREFIX conflicting
    PRE_EXCLUDE_REGEXES
        "[Aa][Pp][Ii]-[Mm][Ss]-.*" "[Ee][Xx][Tt]-[Mm][Ss]-.*"
        "[Mm][Ss][Vv][Cc][Pp].*" "[Vv][Cc][Rr][Uu][Nn][Tt][Ii][Mm][Ee].*"
        "[Cc][Oo][Nn][Cc][Rr][Tt].*" "[Uu][Cc][Rr][Tt][Bb][Aa][Ss][Ee].*"
    POST_EXCLUDE_REGEXES ".*[/\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\].*")
if(unresolved)
    message(FATAL_ERROR "Unresolved package runtime dependencies: ${unresolved}")
endif()
if(conflicting_FILENAMES)
    message(FATAL_ERROR "Conflicting package runtime dependencies: ${conflicting_FILENAMES}")
endif()
file(MAKE_DIRECTORY "${DESTINATION}")
foreach(dependency IN LISTS dependencies)
    get_filename_component(name "${dependency}" NAME)
    get_filename_component(destination_file "${DESTINATION}/${name}" ABSOLUTE)
    get_filename_component(source_file "${dependency}" ABSOLUTE)
    string(TOLOWER "${destination_file}" destination_key)
    string(TOLOWER "${source_file}" source_key)
    if(NOT destination_key STREQUAL source_key)
        file(COPY_FILE "${dependency}" "${destination_file}" ONLY_IF_DIFFERENT)
    endif()
endforeach()
