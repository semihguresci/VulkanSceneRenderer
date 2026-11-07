# Exact official source commit; SHA512 is for this commit tar.gz, not the release ZIP.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
    REF 0f1a757da579831283413295cc7f3b47eb521c77
    SHA512 b5bcab3335446366f13a534c33c1f5a28b736abe4342715e364e46035698fbf5e6f8611a32fa5f0a8ec55749d941d0d04958fe6744fefb92f8f20fc080e78120
)
set(VCPKG_BUILD_TYPE release) # Header-only; project flags remain on Dep_Math.
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}" OPTIONS
        -DVMA_ENABLE_INSTALL=ON
        -DVMA_BUILD_SAMPLES=OFF
        -DVMA_BUILD_DOCUMENTATION=OFF
        -DCMAKE_INSTALL_INCLUDEDIR=include/vma)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME VulkanMemoryAllocator CONFIG_PATH share/cmake/VulkanMemoryAllocator)
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")
