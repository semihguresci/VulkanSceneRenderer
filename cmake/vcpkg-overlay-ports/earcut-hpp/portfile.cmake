# Earcut 3.2.4 preserves the trimmed IFC face triangulations covered by the importer tests.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO mapbox/earcut.hpp
    REF 92acda9e2ed96ff1f910cda35de444727e9da7b0
    SHA512 a6d5441d6c8d7a543cd431652ee6298ed5a1e10ae513cb528eab05001b031c50fc6f532c7bc5f8155c09af7be82670d01ec1fd3a1ca09a2798264e5efdc3fcda
)

file(INSTALL "${SOURCE_PATH}/include/mapbox/earcut.hpp"
    DESTINATION "${CURRENT_PACKAGES_DIR}/include/mapbox")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
