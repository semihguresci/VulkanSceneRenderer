if(NOT RUNTIME_LABEL)
    set(RUNTIME_LABEL "OpenVDB")
endif()
if(NOT EXISTS "${OPENVDB_DLL}" OR NOT EXISTS "${EXECUTABLE}" OR NOT DESTINATION)
    message(FATAL_ERROR "${RUNTIME_LABEL} runtime staging needs an existing DLL, executable and destination")
endif()
if(DUMPBIN)
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${DUMPBIN}")
endif()
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "dumpbin")
get_filename_component(runtime_dir "${OPENVDB_DLL}" DIRECTORY)
get_filename_component(openvdb_name "${OPENVDB_DLL}" NAME)
string(TOLOWER "${openvdb_name}" openvdb_key)
set(pre_excludes
    "[Aa][Pp][Ii]-[Mm][Ss]-.*" "[Ee][Xx][Tt]-[Mm][Ss]-.*"
    "[Mm][Ss][Vv][Cc][Pp].*" "[Vv][Cc][Rr][Uu][Nn][Tt][Ii][Mm][Ee].*"
    "[Cc][Oo][Nn][Cc][Rr][Tt].*" "[Uu][Cc][Rr][Tt][Bb][Aa][Ss][Ee].*")
set(post_excludes ".*[/\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\].*")
# Inspect the executable to avoid copying OpenVDB into unrelated test/tool
# folders. Existing target staging supplies its ordinary dependencies first.
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${EXECUTABLE}"
    DIRECTORIES "${DESTINATION}" "${runtime_dir}"
    RESOLVED_DEPENDENCIES_VAR dependencies
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    CONFLICTING_DEPENDENCIES_PREFIX conflicting
    PRE_EXCLUDE_REGEXES ${pre_excludes}
    POST_EXCLUDE_REGEXES ${post_excludes})
set(needs_openvdb FALSE)
foreach(dependency IN LISTS dependencies conflicting_FILENAMES unresolved)
    get_filename_component(name "${dependency}" NAME)
    string(TOLOWER "${name}" name_key)
    if(name_key STREQUAL openvdb_key)
        set(needs_openvdb TRUE)
    endif()
endforeach()
if(NOT needs_openvdb)
    return()
endif()
# Resolve from the configured library, rather than a previously staged DLL.
# Debug and Release may share a test output directory and DLL basename while
# using incompatible CRT/STL layouts and different transitive DLL names.
file(GET_RUNTIME_DEPENDENCIES
    LIBRARIES "${OPENVDB_DLL}"
    DIRECTORIES "${runtime_dir}"
    RESOLVED_DEPENDENCIES_VAR dependencies
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    PRE_EXCLUDE_REGEXES ${pre_excludes}
    POST_EXCLUDE_REGEXES ${post_excludes})
if(unresolved)
    message(FATAL_ERROR "Unresolved ${RUNTIME_LABEL} runtime dependencies: ${unresolved}")
endif()
list(APPEND dependencies "${OPENVDB_DLL}")
foreach(dependency IN LISTS dependencies)
    get_filename_component(name "${dependency}" NAME)
    string(TOLOWER "${name}" lower_name)
    # The VC runtime remains the separately installed prerequisite used by the
    # renderer's other dependencies.
    if(lower_name MATCHES "^(msvcp|vcruntime|concrt|ucrtbase|api-ms-|ext-ms-)")
        continue()
    endif()
    get_filename_component(destination_file "${DESTINATION}/${name}" ABSOLUTE)
    get_filename_component(source_file "${dependency}" ABSOLUTE)
    string(TOLOWER "${destination_file}" destination_key)
    string(TOLOWER "${source_file}" source_key)
    if(destination_key STREQUAL source_key)
        continue()
    endif()
    file(COPY_FILE "${dependency}" "${DESTINATION}/${name}" ONLY_IF_DIFFERENT)
endforeach()
