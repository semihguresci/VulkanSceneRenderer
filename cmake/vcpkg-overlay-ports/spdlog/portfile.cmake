include("${CMAKE_CURRENT_LIST_DIR}/../active-libraries-common.cmake")
container_active_source(gabime/spdlog 79524ddd08a4ec981b7fea76afd08ee05f83755d 2815405f00669cee0bd756d8db98fbe634fa50e2a629702ae2fd784fba344c8a43ac8de7b5caa0c02f121ce59d0f5ff61435981dd260909c9710ac36f2b51f12)
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}" OPTIONS
    -DSPDLOG_FMT_EXTERNAL=ON
    -DSPDLOG_BUILD_EXAMPLE=OFF
    -DSPDLOG_BUILD_TESTS=OFF
    -DSPDLOG_BUILD_BENCH=OFF
    -DSPDLOG_INSTALL=ON)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/spdlog")
vcpkg_fixup_pkgconfig()
vcpkg_copy_pdbs()
container_active_prune_debug_data()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
