include("${CMAKE_CURRENT_LIST_DIR}/../active-libraries-common.cmake")
container_active_source(glfw/glfw d9d6f0f1f967807ffade6598ea9a631ebaf37a56 2ae16af082d03cfe490988b734b54d4f5f02adb564d7e74a9267bafa1a3710013add8892902ef5729c5c1501b1627c8038a3ac466ae8a8e0e00879a396b3b78f)
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}" OPTIONS
    -DGLFW_BUILD_DOCS=OFF
    -DGLFW_BUILD_TESTS=OFF
    -DGLFW_BUILD_EXAMPLES=OFF
    -DGLFW_INSTALL=ON)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/glfw3")
vcpkg_fixup_pkgconfig()
vcpkg_copy_pdbs()
container_active_prune_debug_data()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.md")
