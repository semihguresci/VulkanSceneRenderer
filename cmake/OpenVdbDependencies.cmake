include_guard(GLOBAL)

add_library(Dep_OpenVDB INTERFACE)
if(NOT ENABLE_OPENVDB)
    message(STATUS "OpenVDB disabled (ENABLE_OPENVDB=OFF)")
    return()
endif()

# vcpkg supplies a package config. Upstream installs FindOpenVDB.cmake
# instead, so also support an ordinary upstream installation prefix.
find_package(OpenVDB CONFIG QUIET)
if(NOT TARGET OpenVDB::openvdb)
    find_path(CONTAINER_OPENVDB_CMAKE_MODULE_DIR FindOpenVDB.cmake
        HINTS ${OpenVDB_ROOT} ENV OpenVDB_ROOT
        PATH_SUFFIXES lib/cmake/OpenVDB lib64/cmake/OpenVDB share/openvdb)
    if(CONTAINER_OPENVDB_CMAKE_MODULE_DIR)
        list(APPEND CMAKE_MODULE_PATH "${CONTAINER_OPENVDB_CMAKE_MODULE_DIR}")
    endif()
    find_package(OpenVDB MODULE QUIET)
endif()
if(NOT TARGET OpenVDB::openvdb)
    message(FATAL_ERROR
        "ENABLE_OPENVDB=ON requires OpenVDB::openvdb. Enable vcpkg manifest "
        "installation (the openvdb feature is selected automatically), or set "
        "OpenVDB_ROOT/CMAKE_PREFIX_PATH to an OpenVDB installation. "
        "Use -DENABLE_OPENVDB=OFF to build without volume support.")
endif()

# Upstream's find module creates a single-location imported target. vcpkg puts
# Debug first in a single-config Debug build, so the discovered location can be
# in either lib/ or debug/lib/. Preserve explicit package configurations and
# fill in the missing ones from the corresponding installation layout.
if(WIN32)
    get_target_property(_openvdb_type OpenVDB::openvdb TYPE)
    get_target_property(_openvdb_library OpenVDB::openvdb IMPORTED_LOCATION)
    foreach(_openvdb_config IN ITEMS RELEASE DEBUG)
        get_target_property(_openvdb_${_openvdb_config}_library OpenVDB::openvdb
            IMPORTED_LOCATION_${_openvdb_config})
        if(NOT _openvdb_library AND _openvdb_${_openvdb_config}_library)
            set(_openvdb_library "${_openvdb_${_openvdb_config}_library}")
        endif()
    endforeach()
    if((_openvdb_type STREQUAL "UNKNOWN_LIBRARY" OR _openvdb_type STREQUAL "STATIC_LIBRARY")
       AND _openvdb_library MATCHES "\\.lib$")
        get_filename_component(_openvdb_libdir "${_openvdb_library}" DIRECTORY)
        get_filename_component(_openvdb_prefix "${_openvdb_libdir}/.." ABSOLUTE)
        get_filename_component(_openvdb_parent_name "${_openvdb_prefix}" NAME)
        string(TOLOWER "${_openvdb_parent_name}" _openvdb_parent_name)
        if(_openvdb_parent_name STREQUAL "debug")
            get_filename_component(_openvdb_prefix "${_openvdb_prefix}/.." ABSOLUTE)
        endif()
        get_filename_component(_openvdb_stem "${_openvdb_library}" NAME_WE)
        # Ordinary upstream installations can use a Debug postfix in lib/.
        string(REGEX REPLACE "_d$" "" _openvdb_base_stem "${_openvdb_stem}")
        if(NOT _openvdb_RELEASE_library AND
           EXISTS "${_openvdb_prefix}/lib/${_openvdb_base_stem}.lib")
            set(_openvdb_RELEASE_library "${_openvdb_prefix}/lib/${_openvdb_base_stem}.lib")
        endif()
        if(NOT _openvdb_DEBUG_library)
            foreach(_openvdb_candidate IN ITEMS
                "${_openvdb_prefix}/debug/lib/${_openvdb_stem}.lib"
                "${_openvdb_prefix}/lib/${_openvdb_base_stem}_d.lib")
                if(EXISTS "${_openvdb_candidate}")
                    set(_openvdb_DEBUG_library "${_openvdb_candidate}")
                    break()
                endif()
            endforeach()
        endif()
        foreach(_openvdb_config IN ITEMS RELEASE DEBUG)
            if(_openvdb_${_openvdb_config}_library)
                set_property(TARGET OpenVDB::openvdb APPEND PROPERTY IMPORTED_CONFIGURATIONS "${_openvdb_config}")
                set_property(TARGET OpenVDB::openvdb PROPERTY
                    IMPORTED_LOCATION_${_openvdb_config} "${_openvdb_${_openvdb_config}_library}")
                get_filename_component(_openvdb_config_libdir "${_openvdb_${_openvdb_config}_library}" DIRECTORY)
                get_filename_component(_openvdb_config_stem "${_openvdb_${_openvdb_config}_library}" NAME_WE)
                set(_openvdb_${_openvdb_config}_dll
                    "${_openvdb_config_libdir}/../bin/${_openvdb_config_stem}.dll")
            endif()
        endforeach()
        if(_openvdb_RELEASE_library)
            foreach(_openvdb_config IN ITEMS RELWITHDEBINFO MINSIZEREL)
                get_property(_openvdb_mapping_set TARGET OpenVDB::openvdb
                    PROPERTY MAP_IMPORTED_CONFIG_${_openvdb_config} SET)
                if(NOT _openvdb_mapping_set)
                    set_property(TARGET OpenVDB::openvdb PROPERTY MAP_IMPORTED_CONFIG_${_openvdb_config} RELEASE)
                endif()
            endforeach()
        endif()
        if(_openvdb_type STREQUAL "UNKNOWN_LIBRARY")
            if(EXISTS "${_openvdb_RELEASE_dll}" OR EXISTS "${_openvdb_DEBUG_dll}")
                # A DLL-only imported target lets CMake apply the same config
                # maps as the linker target, including explicit Debug->Release.
                add_library(Container_OpenVDB_Runtime SHARED IMPORTED)
                foreach(_openvdb_config IN ITEMS RELEASE DEBUG)
                    if(EXISTS "${_openvdb_${_openvdb_config}_dll}")
                        set_property(TARGET Container_OpenVDB_Runtime APPEND PROPERTY
                            IMPORTED_CONFIGURATIONS "${_openvdb_config}")
                        set_property(TARGET Container_OpenVDB_Runtime PROPERTY
                            IMPORTED_LOCATION_${_openvdb_config} "${_openvdb_${_openvdb_config}_dll}")
                    endif()
                endforeach()
                set(_openvdb_runtime_configs RELEASE DEBUG RELWITHDEBINFO MINSIZEREL
                    ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
                foreach(_openvdb_config IN LISTS _openvdb_runtime_configs)
                    string(TOUPPER "${_openvdb_config}" _openvdb_config)
                    get_property(_openvdb_mapping_set TARGET OpenVDB::openvdb
                        PROPERTY MAP_IMPORTED_CONFIG_${_openvdb_config} SET)
                    if(_openvdb_mapping_set)
                        get_target_property(_openvdb_mapping OpenVDB::openvdb MAP_IMPORTED_CONFIG_${_openvdb_config})
                        set_property(TARGET Container_OpenVDB_Runtime PROPERTY
                            MAP_IMPORTED_CONFIG_${_openvdb_config} "${_openvdb_mapping}")
                    endif()
                endforeach()
                set(CONTAINER_OPENVDB_RUNTIME_DLL "$<TARGET_FILE:Container_OpenVDB_Runtime>")
            endif()
        endif()
    elseif(_openvdb_type STREQUAL "SHARED_LIBRARY")
        # Config packages can hide compression dependencies from their imported
        # link interface. Inspect the selected DLL's actual dependency closure.
        set(CONTAINER_OPENVDB_RUNTIME_DLL "$<TARGET_FILE:OpenVDB::openvdb>")
    endif()
    get_filename_component(_openvdb_linker_dir "${CMAKE_LINKER}" DIRECTORY)
    if(EXISTS "${_openvdb_linker_dir}/dumpbin.exe")
        set(CONTAINER_OPENVDB_DUMPBIN "${_openvdb_linker_dir}/dumpbin.exe")
    endif()
endif()

target_link_libraries(Dep_OpenVDB INTERFACE OpenVDB::openvdb)
target_compile_definitions(Dep_OpenVDB INTERFACE CONTAINER_HAS_OPENVDB=1)
message(STATUS "OpenVDB enabled: ${OpenVDB_VERSION}")
