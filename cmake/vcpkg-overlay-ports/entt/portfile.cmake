# Official stable skypjack/entt v4.0.0; archive content verified against Git.
vcpkg_from_github(OUT_SOURCE_PATH SOURCE_PATH REPO skypjack/entt
    REF 85c6bba014049b5de8fad49d25424df2f1f6a8c1
    SHA512 4badd8be0f1a5eda7e157d6d24f335e98a0ad3584f257a0eae13ef685a7e3b45d3d2e6ba0208639893d585abb7fb4e26cc39f76b5ff43f6c16fd9558af180fb7)
set(VCPKG_BUILD_TYPE release)
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}" OPTIONS -DENTT_INSTALL=ON -DENTT_BUILD_TESTING=OFF -DENTT_BUILD_TESTBED=OFF -DENTT_BUILD_DOCS=OFF)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/EnTT/cmake)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/lib")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
