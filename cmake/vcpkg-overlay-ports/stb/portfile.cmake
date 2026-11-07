include("${CMAKE_CURRENT_LIST_DIR}/../active-libraries-common.cmake")
container_active_source(nothings/stb 4e9b368bfc071e97206ea5c6adba68dd0cbf8897 6af9b21d2c13ac06164627986562219317f72295e68e0ace1081e1208a95abc0b64a35f868920797c2446a9c30b485d6d341e75eba14054217169d7f5bbde87f)
file(INSTALL "${SOURCE_PATH}/stb_image.h" "${SOURCE_PATH}/stb_image_write.h" DESTINATION "${CURRENT_PACKAGES_DIR}/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
