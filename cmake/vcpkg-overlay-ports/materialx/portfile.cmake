include("${CMAKE_CURRENT_LIST_DIR}/../active-libraries-common.cmake")
container_active_source(AcademySoftwareFoundation/MaterialX 7b64921ef1d42f2d57871e9d2c43dc11f041f26b 9d895f7fd969efc3c6eef8999879cfd0867f83dadcd87245a9da816de419d6419f6b1865800edf1c2abcbae1420bdec3d93bf4f0224207cdcddf000f05d1cf82)
if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    set(_materialx_shared ON)
else()
    set(_materialx_shared OFF)
endif()
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}" OPTIONS
    -DMATERIALX_BUILD_RENDER=OFF
    -DMATERIALX_BUILD_RENDER_PLATFORMS=OFF
    -DMATERIALX_BUILD_GEN_GLSL=OFF
    -DMATERIALX_BUILD_GEN_OSL=OFF
    -DMATERIALX_BUILD_GEN_MDL=OFF
    -DMATERIALX_BUILD_GEN_MSL=OFF
    -DMATERIALX_BUILD_GEN_SLANG=OFF
    -DMATERIALX_BUILD_TESTS=OFF
    -DMATERIALX_BUILD_PYTHON=OFF
    -DMATERIALX_BUILD_DOCS=OFF
    -DMATERIALX_INSTALL_RESOURCES=OFF
    -DMATERIALX_INSTALL_PYTHON=OFF
    -DMATERIALX_BUILD_SHARED_LIBS=${_materialx_shared})
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/MaterialX")
vcpkg_copy_pdbs()
container_active_prune_debug_data()
file(COPY "${SOURCE_PATH}/libraries" DESTINATION "${CURRENT_PACKAGES_DIR}")
file(SHA256 "${SOURCE_PATH}/source/MaterialXFormat/External/PugiXML/pugixml.hpp" _pugi_hash)
if(NOT _pugi_hash STREQUAL "dbfa4b37af2b0c90e4bbbab1e989badcf1b17daec370a103fecb4ea692a9eb76")
    message(FATAL_ERROR "Pinned MaterialX PugiXML notice source changed")
endif()
file(READ "${SOURCE_PATH}/LICENSE" _license)
file(READ "${SOURCE_PATH}/source/MaterialXFormat/External/PugiXML/pugixml.hpp" _pugi_notice OFFSET 51078)
file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/share/materialx")
file(WRITE "${CURRENT_PACKAGES_DIR}/share/materialx/copyright" "${_license}\n\n${_pugi_notice}")
