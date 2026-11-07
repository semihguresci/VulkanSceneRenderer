# Optional portable Windows x64 core subset of LunarG SDK 1.4.363.1.
# Headers/Loader source tags are vulkan-sdk-1.4.363.0; the .1 SDK point
# release updates validation. No SDK installer or global environment changes.
include_guard(GLOBAL)
set(CONTAINER_VULKAN_SDK_CORE_ROOT "" CACHE PATH "Pinned Vulkan SDK 1.4.363.1 headers/import-library subset")
set(CONTAINER_VULKAN_SDK_CORE_AVAILABLE OFF CACHE INTERNAL "Pinned portable Vulkan SDK core was selected" FORCE)
set(CONTAINER_VULKAN_SDK_CORE_VERSION "" CACHE INTERNAL "Verified portable Vulkan SDK release" FORCE)
if(NOT CONTAINER_VULKAN_SDK_CORE_ROOT)
    return()
endif()
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The portable Vulkan SDK core subset requires Windows x64")
endif()
set(_container_sdk_pins
    "Include/vk_video/vulkan_video_codec_av1std.h|110d61f53214322f4d0aec8f77a7cec40c1517829b125c0d01d6fc9fdf0fdd83"
    "Include/vk_video/vulkan_video_codec_av1std_decode.h|fa2e85e9669f1666badfab9211de9dbe279bc2adc3ed095635313712ced9219d"
    "Include/vk_video/vulkan_video_codec_av1std_encode.h|5bb49198ac7a63c810115c62c9ab44b2e60f20c60869a68a813730f30777db5f"
    "Include/vk_video/vulkan_video_codec_h264std.h|84d669c6010443e268f940e57a26e3ff0c4050c9c434b79efaaa1bfa3d3d203c"
    "Include/vk_video/vulkan_video_codec_h264std_decode.h|fbee59666126c323b2bacc0b4b2c6fd1d1a28ced72e4c6791026a730f05bc129"
    "Include/vk_video/vulkan_video_codec_h264std_encode.h|cd7e050bc0062d791a5e4e3cc97efdd4b9b3838214168785743fb48ebd5a041a"
    "Include/vk_video/vulkan_video_codec_h265std.h|d3ae5c05dca0765d950e53aa1aafc9526be8be6be72b373803c70c94086665a1"
    "Include/vk_video/vulkan_video_codec_h265std_decode.h|4ce92dc8ff11f8f9652b01713992f1235b6c88694a658b8fe8777ac7c79fb5df"
    "Include/vk_video/vulkan_video_codec_h265std_encode.h|182114ca162aae8b3c1ed3c9693852126b2dda009712e387a1ab6f475ab8058e"
    "Include/vk_video/vulkan_video_codec_vp9std.h|b4a35526ddf93652c6244e26a71f6d2d9536c0bd5aebde35ed777d1cd8a64978"
    "Include/vk_video/vulkan_video_codec_vp9std_decode.h|ac1ac9e1494a26855e327001fa6ffa26020eb5bf852dab1ea3e89939ced3d5ee"
    "Include/vk_video/vulkan_video_codecs_common.h|cf775e4d74384e3d4ed5657fb7d87faa4a7eaf6fee21f4f4113640565494a924"
    "Include/vulkan/utility/vk_concurrent_unordered_map.hpp|1f7e600ebe38f833fb99c5441cd40d54ad146c84ebc9b3c926f131c750a0e8fd"
    "Include/vulkan/utility/vk_dispatch_table.h|cdcced3eecfcc1749661af58f486720acbd51bfd2a4419a38af20df2790482af"
    "Include/vulkan/utility/vk_format_utils.h|e25ea5171c39c45e95476c3b3d4de4e968a3be8efc1c5afff5ed57ce31f53fda"
    "Include/vulkan/utility/vk_safe_struct.hpp|7e062eb49888e0a6093d4ac48e1e1218fd5ebc7fd6c000a55056fc566d2ac64c"
    "Include/vulkan/utility/vk_safe_struct_utils.hpp|f0a9841c556ccdd5773aa6ca935c24ce0ad2cca9cad323b56c27e9a41f8e6160"
    "Include/vulkan/utility/vk_small_containers.hpp|8a7752668228b254fafb3a5b13d139bf0155e95a2c8da40a5727cc797b5ea134"
    "Include/vulkan/utility/vk_sparse_range_map.hpp|e391c16df50cd12780a428b5ecd05b7690d86c1c41df6ac0ebfc0595c4742ed8"
    "Include/vulkan/utility/vk_struct_helper.hpp|4e853736690fd228c98183be0a741b99f4971b5cec0f190450467fb06de97eab"
    "Include/vulkan/vk_enum_string_helper.h|9031ebe1ad065f2b199d5633bde16e3aa11c94bc118bb967ca6510dc05fbdca5"
    "Include/vulkan/vk_icd.h|46a46bf9bb5176becff061262f1093c1caa9ef33a2ec6f3432e8b1da1ce2c436"
    "Include/vulkan/vk_layer.h|5ad88b2b4108855d6c53d81da654a5663953e2102fb1f6e3ba47b2d7f8c95ade"
    "Include/vulkan/vk_platform.h|1fc08c530b46e587295aafb19493eb9d434e963f7e48d3a323adbee06f59976c"
    "Include/vulkan/vulkan.cppm|6843c24b75ecc0b14c8fd8f83339974ac7c9f7abdd90a8efbc4c7d60f413324b"
    "Include/vulkan/vulkan.h|2d1a6f00bfdcddbe1c68bf5f574a0fc4cf95c4be9eae683123b6db8af9d1a1de"
    "Include/vulkan/vulkan.hpp|e438776a3d842c19fee78b67fe89e9639a2a8522675f75ca831e513aeef5424e"
    "Include/vulkan/vulkan_android.h|4f5afd4b0b31b655b7e6a27d9954ab232f0f3cd8bfa6a340335ba939d20648c7"
    "Include/vulkan/vulkan_beta.h|cbee78bfb43fb7faac49e1da0d8b7c46132036ea6c021469650f2e57888da505"
    "Include/vulkan/vulkan_core.h|944cd8f81d98e94999c7353dd632f73be471f2da95afecbc7d9ffe0ad5295deb"
    "Include/vulkan/vulkan_directfb.h|e68438c77132d629ce6c05eae7f21ff8818a6b743b2adcdeaee2c1669f7bc4e9"
    "Include/vulkan/vulkan_enums.hpp|dc89c08f7d7d0f31208a119695bebd113611341455b5da6b6667152e426c1671"
    "Include/vulkan/vulkan_extension_inspection.hpp|14c9631d67532b8e395d7b35bf421df05dc875b6799ca52834c8308cbcfa003f"
    "Include/vulkan/vulkan_format_traits.hpp|df996975ba4c12548403ad99d44e4ff0cdcd88c771bcd6176774e37416f89853"
    "Include/vulkan/vulkan_fuchsia.h|46915b5d3390cf9d6c46baa13fa6792b7f8c172187d00662d7843cd0bba2b8f0"
    "Include/vulkan/vulkan_funcs.hpp|0514500fdebaad4eab0c4d4ba893a0563c588587c7ccab2a82787e154915c91b"
    "Include/vulkan/vulkan_ggp.h|be82c59a2459d70a6ceaab715d7c3470a61cc791131babb7e70f66906bffb23a"
    "Include/vulkan/vulkan_handles.hpp|832d8574813c7261ca6cb8c4c5114d5850480103b1d0a1ac4afa248676ba1aa0"
    "Include/vulkan/vulkan_hash.hpp|ed202c7a73f27398ab6763a66111632a4a975dff20b82471bbb90c59c75a9b94"
    "Include/vulkan/vulkan_hpp_macros.hpp|05d1547fad3804901c235542dd0a27772c6c2bf1e06a8768e21ddeecfb4e47e2"
    "Include/vulkan/vulkan_ios.h|cd5a5f1b33ca5904d08df8a90ac41c8be5ac604d30e023b8036f19430f56c151"
    "Include/vulkan/vulkan_layer_settings.hpp|fe89e0fa168732b060ba4168f08537fe7a49152c9e70b2ef9542156bab9a9528"
    "Include/vulkan/vulkan_macos.h|d7ff1957e199b1c2e128c148d5e66637e5e49dae35ec5a97db87fca5e02d4c04"
    "Include/vulkan/vulkan_metal.h|ae36aacf5139410c9608e702e56d42770a5ba264f0e9dfc9ef52605bda4be47f"
    "Include/vulkan/vulkan_ohos.h|005aaa229cf9961e3edc09a0f47c9c3593cbc57ca15a9cacbb521bef27455b7e"
    "Include/vulkan/vulkan_profiles.hpp|1376aae97bc402aace25e02e545d752ccb70438f2c01282546d96d6e63cdd682"
    "Include/vulkan/vulkan_raii.hpp|d33dede3ba45812da1afcb1307170b68e2740034a6b3dd070192e3ae089d48dc"
    "Include/vulkan/vulkan_screen.h|06563ce3cdbda16f74976fabf42132d514625f53dec7974fa9330972990edfb7"
    "Include/vulkan/vulkan_shared.hpp|12f924b2afa1cc1744bcf83f663d788bc2df32c89768a5156c4c23a00cd96618"
    "Include/vulkan/vulkan_static_assertions.hpp|27b470f175b3c5474e6b01f73771e246a949bda0720bcf7aa2a87101c26cfef0"
    "Include/vulkan/vulkan_structs.hpp|353c4bc77fbe813653947e853bd59b0ec01a16ac5c01df1960d477e57178b5b5"
    "Include/vulkan/vulkan_to_string.hpp|c8ca33d5ab65c9c930d255d3799791d50cd9c9ab1670860090ff195ff86ac2b2"
    "Include/vulkan/vulkan_ubm.h|aae80e021e1122a157923177676dc2a9447bd9bd970e3d80cec043f981d26605"
    "Include/vulkan/vulkan_vi.h|2adb9ae5b758783548e79398f3ecad65af1e536bbeb701b3628ede54467999ec"
    "Include/vulkan/vulkan_video.cppm|ea0d8227b25773b7e76a8c42a235e96c531d36b42f2a77602562f0abcf3850ec"
    "Include/vulkan/vulkan_video.hpp|77ee6fc130aff3f0b4fd840bd0eaa2399892bd0e789e59f86f59cda6c4b9b989"
    "Include/vulkan/vulkan_wayland.h|f9b46a0805eeea61b42c2a52d783cf65690ad330c1869ac422c4a2b81adcebaf"
    "Include/vulkan/vulkan_win32.h|9dc4d69efa98e5d2b94458c60861b963ab5fe18b7af6be20ec7e6c020086c1d5"
    "Include/vulkan/vulkan_xcb.h|ba4bfcef79ec4479aa6309d206cb075fbcfec1c9b625ffae39531a49904a08ef"
    "Include/vulkan/vulkan_xlib.h|cdd0677b5a5130994630c8fa21f6cdc2655cf5310bc416f332c5b8d386eed06f"
    "Include/vulkan/vulkan_xlib_xrandr.h|d2131832eecbd4e53cac6cf67ecae05b655fbe0fb8543440d7b285b3495d2ae7"
    "Lib/vulkan-1.lib|02caa31b8665a79f327cbfe97286fd7d0017c0b1f9b39eb78687b7a509de52b3"
    "licenses/Vulkan-Headers-LICENSE.md|95ad366d23fadf701d355bc45fb8b82ae2d700239471d35d41286ac3b08ff903"
    "licenses/Vulkan-Loader-LICENSE.txt|43c0a37e6a0fa7ff3c843b3ec5a4fac84b712558ddac103fbd4c1649662a9ece"
)
foreach(_container_sdk_pin IN LISTS _container_sdk_pins)
    string(REPLACE "|" ";" _container_sdk_pair "${_container_sdk_pin}")
    list(GET _container_sdk_pair 0 _container_sdk_relative)
    list(GET _container_sdk_pair 1 _container_sdk_expected)
    set(_container_sdk_file "${CONTAINER_VULKAN_SDK_CORE_ROOT}/${_container_sdk_relative}")
    if(NOT EXISTS "${_container_sdk_file}")
        message(FATAL_ERROR "The pinned Vulkan SDK core file is missing: ${_container_sdk_file}")
    endif()
    file(SHA256 "${_container_sdk_file}" _container_sdk_actual)
    if(NOT _container_sdk_actual STREQUAL _container_sdk_expected)
        message(FATAL_ERROR "The selected Vulkan SDK core file is outside the 1.4.363.1 pin: ${_container_sdk_file}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_container_sdk_file}")
endforeach()
set(Vulkan_INCLUDE_DIR "${CONTAINER_VULKAN_SDK_CORE_ROOT}/Include" CACHE PATH "Selected Vulkan SDK include directory" FORCE)
set(Vulkan_LIBRARY "${CONTAINER_VULKAN_SDK_CORE_ROOT}/Lib/vulkan-1.lib" CACHE FILEPATH "Selected Vulkan x64 import library" FORCE)
foreach(_container_sdk_component IN ITEMS Vulkan-Headers Vulkan-Loader)
    if(_container_sdk_component STREQUAL "Vulkan-Headers")
        set(_container_sdk_extension md)
    else()
        set(_container_sdk_extension txt)
    endif()
    configure_file("${CONTAINER_VULKAN_SDK_CORE_ROOT}/licenses/${_container_sdk_component}-LICENSE.${_container_sdk_extension}"
        "${CMAKE_BINARY_DIR}/third_party_licenses/${_container_sdk_component}-1.4.363-LICENSE.${_container_sdk_extension}" COPYONLY)
endforeach()
set(CONTAINER_VULKAN_SDK_CORE_AVAILABLE ON CACHE INTERNAL "Pinned portable Vulkan SDK core was selected" FORCE)
set(CONTAINER_VULKAN_SDK_CORE_VERSION "1.4.363.1" CACHE INTERNAL "Verified portable Vulkan SDK release" FORCE)
message(STATUS "Verified portable Vulkan SDK core ${CONTAINER_VULKAN_SDK_CORE_VERSION}: ${CONTAINER_VULKAN_SDK_CORE_ROOT}")
