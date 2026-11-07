# Exact official source commit; SHA512 is for this commit tar.gz, not the release ZIP.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO g-truc/glm
    REF 8d1fd52e5ab5590e2c81768ace50c72bae28f2ed
    SHA512 e05779baab888e13e64f0f8bd955887398e8de2a7f05360a1e9e84a5e91cf21e9be0bccaaef94ca3511f418e91a509f78a92e3dd6ab9a3cb045fa3e84c57d7d8
)
set(VCPKG_BUILD_TYPE release) # Header-only; project flags remain on Dep_Math.
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}" OPTIONS
        -DGLM_BUILD_LIBRARY=OFF
        -DGLM_BUILD_TESTS=OFF
        -DGLM_BUILD_INSTALL=ON)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME glm CONFIG_PATH share/glm)
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/copying.txt")
