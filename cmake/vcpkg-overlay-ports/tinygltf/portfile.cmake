# Official commit tar.gz; every Git blob was checked against the primary tree.
vcpkg_from_github(OUT_SOURCE_PATH SOURCE_PATH REPO syoyo/tinygltf
  REF 97789b346ed792f413b9520c80bb479f3b63368f
  SHA512 3aac51f4c68e03ad11dec2ffc550c85535a0641a52815337c09d731c76800a89a4ee755308a4f550e478415db3d10b2159428912d1293d80c08da43f4625aa12)
if(NOT "legacy-cpp" IN_LIST FEATURES)
  message(FATAL_ERROR "This scoped overlay requires explicit legacy-cpp selection; it does not silently select a different API")
endif()
file(INSTALL "${SOURCE_PATH}/attic/tiny_gltf.h" DESTINATION "${CURRENT_PACKAGES_DIR}/include")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/json.hpp" DESTINATION "${CURRENT_PACKAGES_DIR}/include")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/tinygltfConfig.cmake" "${CMAKE_CURRENT_LIST_DIR}/tinygltfConfigVersion.cmake"
  DESTINATION "${CURRENT_PACKAGES_DIR}/share/tinygltf")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/copyright" DESTINATION "${CURRENT_PACKAGES_DIR}/share/tinygltf")
