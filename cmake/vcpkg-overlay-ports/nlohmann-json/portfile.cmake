# Official stable nlohmann/json v3.12.0; archive content verified against Git.
vcpkg_from_github(OUT_SOURCE_PATH SOURCE_PATH REPO nlohmann/json
    REF 55f93686c01528224f448c19128836e7df245f72
    SHA512 9d4a665eb8c2e1e15b9964b9d70b166ae701f633897e0e9f06ab2e5762d64c7366cac4b32e63a6febb64717985d048c8dce118413405dec0fb5845041b9174e5)
set(VCPKG_BUILD_TYPE release)
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}" OPTIONS -DJSON_BuildTests=OFF -DJSON_Install=ON -DJSON_MultipleHeaders=ON)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME nlohmann_json CONFIG_PATH share/cmake/nlohmann_json)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/lib")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.MIT")
