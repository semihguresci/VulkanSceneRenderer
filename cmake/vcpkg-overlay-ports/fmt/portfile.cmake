include("${CMAKE_CURRENT_LIST_DIR}/../active-libraries-common.cmake")
container_active_source(fmtlib/fmt 1be298e1bd68957e4cd352e1f676f00e07dcfb57 ed0fccb0eafa811aec228fe9e5983663291eba81bc996884a73e710c49c24cb6bd21751fc02249b7ca6add64a6b998db69a4f1e553121ca6fcc9e523efce2d4c)
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}" OPTIONS
    -DFMT_DOC=OFF
    -DFMT_TEST=OFF
    -DFMT_INSTALL=ON)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/fmt")
vcpkg_fixup_pkgconfig()
vcpkg_copy_pdbs()
container_active_prune_debug_data()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
