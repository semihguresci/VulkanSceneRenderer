# Official commit tar.gz; every Git blob was checked against the primary tree.
vcpkg_from_github(OUT_SOURCE_PATH SOURCE_PATH REPO syoyo/tinyexr
  REF 9b565f64599fe25accf6ba41245c07587c4fa7f4
  SHA512 aa0d627b803d7492c1397314114703bd9d12b629557ff5a9323e66ace69c4934946892a1a7a2784d4d0f168a03c4ff7b492396e9e6b26bdb7ac8fa03b5148acf)
if(NOT "legacy-cpp" IN_LIST FEATURES)
  message(FATAL_ERROR "This scoped overlay requires explicit legacy-cpp selection; it does not silently select a different API")
endif()
file(INSTALL "${SOURCE_PATH}/tinyexr.h" DESTINATION "${CURRENT_PACKAGES_DIR}/include")
file(INSTALL "${SOURCE_PATH}/exr_reader.hh" DESTINATION "${CURRENT_PACKAGES_DIR}/include")
file(INSTALL "${SOURCE_PATH}/streamreader.hh" DESTINATION "${CURRENT_PACKAGES_DIR}/include")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/tinyexrConfig.cmake" "${CMAKE_CURRENT_LIST_DIR}/tinyexrConfigVersion.cmake"
  DESTINATION "${CURRENT_PACKAGES_DIR}/share/tinyexr")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/copyright" DESTINATION "${CURRENT_PACKAGES_DIR}/share/tinyexr")
