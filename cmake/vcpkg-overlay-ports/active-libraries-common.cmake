function(container_active_source repo commit archive_sha512)
    vcpkg_download_distfile(_archive
        URLS "https://codeload.github.com/${repo}/zip/${commit}"
        FILENAME "${PORT}-${commit}.zip" SHA512 "${archive_sha512}")
    vcpkg_extract_source_archive(_source ARCHIVE "${_archive}")
    set(SOURCE_PATH "${_source}" PARENT_SCOPE)
endfunction()

function(container_active_prune_debug_data)
    cmake_path(ABSOLUTE_PATH CURRENT_PACKAGES_DIR NORMALIZE OUTPUT_VARIABLE _root)
    foreach(_suffix IN ITEMS debug/include debug/share)
        cmake_path(APPEND _root "${_suffix}" OUTPUT_VARIABLE _target)
        cmake_path(IS_PREFIX _root "${_target}" NORMALIZE _inside)
        if(NOT _inside OR _target STREQUAL _root)
            message(FATAL_ERROR "Unsafe active-library package cleanup path")
        endif()
        file(REMOVE_RECURSE "${_target}")
    endforeach()
endfunction()
