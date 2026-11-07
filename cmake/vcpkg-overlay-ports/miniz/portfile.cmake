include("${CMAKE_CURRENT_LIST_DIR}/../active-libraries-common.cmake")
container_active_source(richgel999/miniz 77d0dce8627735138c51770d1799a1ef48f2117d c6a2527cd9263acdc728efeef9d829ae19a5d3e02552d2f3698d68f9c14ee595fd2d62b14e86a5e2280956962e6980e1801003462dea10c8a39f20c35d0b6905)
set(_miniz_release_options "")
if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
    list(APPEND _miniz_release_options "-DCMAKE_C_FLAGS_RELEASE=/O2 /DNDEBUG /Z7")
endif()
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}" OPTIONS
    -DBUILD_TESTS=OFF
    -DBUILD_EXAMPLES=OFF
    -DBUILD_FUZZERS=OFF
    -DINSTALL_PROJECT=ON
    ${_miniz_release_options})
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/miniz")
vcpkg_fixup_pkgconfig()
vcpkg_copy_pdbs()
container_active_prune_debug_data()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
