# Official stable google/googletest v1.18.0; archive content verified against Git.
vcpkg_from_github(OUT_SOURCE_PATH SOURCE_PATH REPO google/googletest
    REF 063de7e9578f82b369302001269680b4b1553359
    SHA512 7ae565edcd00d541cd65cfedf7768b464b9158b994f68db07ecd45cd1442bb3da8dd892bd0487ced2188a18148476c2ce3205e8279800a4f96f126d9e2cdacdf)
string(COMPARE EQUAL "${VCPKG_CRT_LINKAGE}" "dynamic" GTEST_FORCE_SHARED_CRT)
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}" OPTIONS -DBUILD_GMOCK=ON -DINSTALL_GTEST=ON -Dgtest_force_shared_crt=${GTEST_FORCE_SHARED_CRT})
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/GTest)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_copy_pdbs()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
