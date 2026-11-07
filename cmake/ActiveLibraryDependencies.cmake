include_guard(GLOBAL)

# Exact active-library selection. Each selected package must carry its own
# authentic notice; an older vcpkg cache is never used as a notice substitute.
find_package(fmt 12.2.0 EXACT CONFIG REQUIRED)
find_package(spdlog 1.17.0 EXACT CONFIG REQUIRED)
find_package(glfw3 3.5.1 EXACT CONFIG REQUIRED)
find_package(imgui 1.93.0 EXACT CONFIG REQUIRED)
find_package(MaterialX 1.39.5 EXACT CONFIG REQUIRED)
find_package(miniz 3.1.2 EXACT CONFIG REQUIRED)
find_path(STB_INCLUDE_DIRS stb_image.h PATH_SUFFIXES include REQUIRED)

set(_active_index "{\"libraries\":[]}")
set(_active_notice_directory "${CMAKE_BINARY_DIR}/third_party_licenses")
file(MAKE_DIRECTORY "${_active_notice_directory}")

function(_container_stage_active_notice name version package_dir expected_hash)
    find_file(_notice NAMES copyright PATHS
        "${package_dir}"
        "${package_dir}/../share/${name}"
        "${package_dir}/../../../share/${name}"
        "${package_dir}/../../share/${name}"
        NO_DEFAULT_PATH NO_CACHE)
    if(NOT _notice)
        message(FATAL_ERROR "Selected ${name} ${version} package lacks its authentic notice")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_notice}")
    file(SHA256 "${_notice}" _hash)
    if(NOT _hash STREQUAL expected_hash)
        message(FATAL_ERROR "Selected ${name} ${version} notice is outside the pinned upstream sources")
    endif()
    set(_filename "${name}-${version}-NOTICES.txt")
    configure_file("${_notice}" "${_active_notice_directory}/${_filename}" COPYONLY)
    file(SHA256 "${_active_notice_directory}/${_filename}" _staged_hash)
    string(JSON _row SET "{}" name "\"${name}\"")
    string(JSON _row SET "${_row}" version "\"${version}\"")
    string(JSON _row SET "${_row}" notice "\"${_filename}\"")
    string(JSON _row SET "${_row}" sha256 "\"${_staged_hash}\"")
    string(JSON _length LENGTH "${_active_index}" libraries)
    string(JSON _active_index SET "${_active_index}" libraries ${_length} "${_row}")
    set(_active_index "${_active_index}" PARENT_SCOPE)
    message(STATUS "Selected ${name} ${version}: ${package_dir}")
endfunction()

_container_stage_active_notice(fmt 12.2.0 "${fmt_DIR}" a65fcc5a1095fc33cc37e4fff69366e49d7e92f613558a2b95ed1b70e33758b4)
_container_stage_active_notice(spdlog 1.17.0 "${spdlog_DIR}" d678c20826e1392578508e1c78649a1e3c31bda05ef4388d967dbd654461b140)
_container_stage_active_notice(glfw3 3.5.1 "${glfw3_DIR}" 149704059b5d0bf551637e50042dd4de9c2cae921021f6636298911e3a5f9462)
_container_stage_active_notice(imgui 1.93.0 "${imgui_DIR}" 173506a2d6f7fb67990d257fb2507f188690eca39060c39469ae7bef43aae2a3)
_container_stage_active_notice(materialx 1.39.5 "${MaterialX_DIR}" be8740c1e9b0eb759e36e101ab8cfcf311ed7b67f65063138342ec8ecd082072)
_container_stage_active_notice(miniz 3.1.2 "${miniz_DIR}" 0115478d567121238cf6cc1c0c361926cf07a49d9e4c9e66da97fac6a01646b3)

# stb has no semantic-versioned release; pin only the two actual implementation
# headers and notice to the latest official snapshot, not an invented version.
foreach(_entry IN ITEMS
    "stb_image.h|594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3"
    "stb_image_write.h|cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05")
    string(REPLACE "|" ";" _fields "${_entry}")
    list(GET _fields 0 _header)
    list(GET _fields 1 _expected)
    if(NOT EXISTS "${STB_INCLUDE_DIRS}/${_header}")
        message(FATAL_ERROR "Selected stb snapshot lacks ${_header}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${STB_INCLUDE_DIRS}/${_header}")
    file(SHA256 "${STB_INCLUDE_DIRS}/${_header}" _actual)
    if(NOT _actual STREQUAL _expected)
        message(FATAL_ERROR "Selected stb ${_header} differs from official snapshot 4e9b368bfc071e97206ea5c6adba68dd0cbf8897")
    endif()
endforeach()
_container_stage_active_notice(stb snapshot-4e9b368bfc071e97206ea5c6adba68dd0cbf8897 "${STB_INCLUDE_DIRS}"
    bebfe904b14301657e4e5d655c811d51fd31b97c455b9cc2d8600d6bac6cff63)
file(WRITE "${_active_notice_directory}/selected-active-dependencies.json" "${_active_index}\n")
