# Header-only core dependencies. Versions and source archives are also pinned by
# scoped overlay ports; unrelated vcpkg registry versions stay unchanged.
include_guard(GLOBAL)
find_package(VulkanMemoryAllocator 3.5.0 EXACT CONFIG REQUIRED)
find_package(glm 1.0.3 EXACT CONFIG REQUIRED)

if(TARGET GPUOpen::VulkanMemoryAllocator)
    set(VMA_TARGET GPUOpen::VulkanMemoryAllocator)
elseif(TARGET VulkanMemoryAllocator::VulkanMemoryAllocator)
    set(VMA_TARGET VulkanMemoryAllocator::VulkanMemoryAllocator)
elseif(TARGET VulkanMemoryAllocator::Headers)
    set(VMA_TARGET VulkanMemoryAllocator::Headers)
else()
    message(FATAL_ERROR "The pinned VMA package has no supported imported target")
endif()
if(NOT TARGET glm::glm-header-only OR NOT TARGET glm::glm)
    message(FATAL_ERROR "The pinned GLM package must export its header-only target")
endif()

function(container_core_include target header output)
    get_target_property(directories ${target} INTERFACE_INCLUDE_DIRECTORIES)
    set(selected "")
    foreach(directory IN LISTS directories)
        if(EXISTS "${directory}/${header}")
            get_filename_component(directory "${directory}" REALPATH)
            if(selected AND NOT selected STREQUAL directory)
                message(FATAL_ERROR "Ambiguous selected core header ${header}")
            endif()
            set(selected "${directory}")
        endif()
    endforeach()
    if(NOT selected)
        message(FATAL_ERROR "The installed ${target} target does not expose ${header}")
    endif()
    set(${output} "${selected}" PARENT_SCOPE)
    # Ordinary include precedence is deliberate: SDKs and aggregate package
    # prefixes can contain older GLM/VMA headers alongside unrelated libraries.
    get_target_property(imported ${target} IMPORTED)
    if(imported)
        set_property(TARGET ${target} PROPERTY IMPORTED_NO_SYSTEM TRUE)
    endif()
endfunction()

function(container_check_core_file path expected)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "The selected core library file is missing: ${path}")
    endif()
    file(READ "${path}" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    string(SHA256 actual "${contents}")
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "The selected core library file is outside its release pin: ${path}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
endfunction()

container_core_include(${VMA_TARGET} vk_mem_alloc.h CONTAINER_VMA_INCLUDE_DIR)
container_core_include(glm::glm-header-only glm/detail/setup.hpp CONTAINER_GLM_INCLUDE_DIR)
container_check_core_file("${CONTAINER_VMA_INCLUDE_DIR}/vk_mem_alloc.h"
    "41a5897caee36a46d636873a1f6a2518371d4d419cd299d40457e63bde64ecea")
container_check_core_file("${CONTAINER_GLM_INCLUDE_DIR}/glm/detail/setup.hpp"
    "78c7278cc214b6ce75c5648f5afc65dd36488bf65abfd6aac8171b0439e98a10")

function(container_stage_core_notice include_dir package filename expected)
    # Support upstream include/ and the vcpkg Windows include/vma layout.
    find_file(notice copyright NO_CACHE NO_DEFAULT_PATH PATHS
        "${include_dir}/../share/${package}" "${include_dir}/../../share/${package}")
    if(NOT notice)
        message(FATAL_ERROR "The selected ${package} prefix lacks share/${package}/copyright")
    endif()
    container_check_core_file("${notice}" "${expected}")
    file(READ "${notice}" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/third_party_licenses/${filename}"
        CONTENT "${contents}" @ONLY NEWLINE_STYLE LF)
endfunction()
container_stage_core_notice("${CONTAINER_GLM_INCLUDE_DIR}" glm GLM-1.0.3-LICENSE.txt
    "62d2d642c7d054d4fb4c9b42faad617d6c88fcd91e317f8035aa9f277cc159c3")
container_stage_core_notice("${CONTAINER_VMA_INCLUDE_DIR}" vulkan-memory-allocator VMA-3.5.0-LICENSE.txt
    "52df2c03d6cfc9ffec13c9d3626c530fc9ce0cbe41d5ea3d10cd46edeb1aeb38")
set(CONTAINER_GLM_VERSION "1.0.3" CACHE INTERNAL "Verified selected GLM release" FORCE)
set(CONTAINER_VMA_VERSION "3.5.0" CACHE INTERNAL "Verified selected VMA release" FORCE)
message(STATUS "Verified GLM ${CONTAINER_GLM_VERSION}: ${CONTAINER_GLM_INCLUDE_DIR}")
message(STATUS "Verified VMA ${CONTAINER_VMA_VERSION}: ${CONTAINER_VMA_INCLUDE_DIR}")

# Exact C++ core releases. Scoped overlay ports pin complete official archives;
# selected-prefix header/legal pins prevent aggregate-prefix shadowing.
find_package(EnTT 4.0.0 EXACT CONFIG REQUIRED)
find_package(nlohmann_json 3.12.0 EXACT CONFIG REQUIRED)
container_core_include(EnTT::EnTT entt/config/version.h CONTAINER_ENTT_INCLUDE_DIR)
container_core_include(nlohmann_json::nlohmann_json nlohmann/json.hpp CONTAINER_JSON_INCLUDE_DIR)
function(container_stage_selected_cpp_notice include_dir package filename expected)
    find_file(notice copyright NO_CACHE NO_DEFAULT_PATH PATHS "${include_dir}/../share/${package}")
    if(NOT notice)
        message(FATAL_ERROR "The selected ${package} prefix lacks its authentic copyright")
    endif()
    container_check_core_file("${notice}" "${expected}")
    configure_file("${notice}" "${CMAKE_BINARY_DIR}/third_party_licenses/${filename}" COPYONLY)
endfunction()
set(_container_entt_pins
    "entt/config/config.h|45ea2e8561a80baa8484bb012195f2f2916843c844d0080057d36dfeb8c31a1e"
    "entt/config/macro.h|20e3c4368034f1be2afc95de574551089aba48e1380049671d572feb42fc5d28"
    "entt/config/version.h|d00ad578d1d2f1aa4739932dc122cf91f8c4e99b3a90f53359d5bc66d05dcc36"
    "entt/container/dense_map.hpp|7f147867a698dc59800533d0eb1ce1c0830faeeae54b45d59dee00529f592ef4"
    "entt/container/dense_set.hpp|db5aa02002eae3622e07f98ae9f96d89ade4c6d11439a3b800bd389bb5ea20fe"
    "entt/container/fwd.hpp|48e9783d642874e2173c9730a712350692ef1acceb6f1624233ec7e4aa3efeea"
    "entt/container/table.hpp|c60e611873ae6cf44524426bfd3ac03e9efe55ff27bcf541cca9aa0f2c00b56e"
    "entt/core/algorithm.hpp|db6e6971fb14157bda5a1ba7012ba8205cc87788d14a395793d6d50c0b488723"
    "entt/core/any.hpp|9efdbe7d52ed4898dd1f80a17c9a5c23ba3ef822b39ff24809299f9d2183b210"
    "entt/core/bit.hpp|e6a14ed889a65296fdd7c23d44386a9643adbe35def84c64f4e7efdba7650605"
    "entt/core/compressed_pair.hpp|a9baaa367c247214571c1aa6e6bf0a2e149baa286a913641920dccf46befb4e1"
    "entt/core/concepts.hpp|fd8d6891c0d8db4bc881d0f2233585127c19050ae08e0f48698a7874cecc2d29"
    "entt/core/enum.hpp|50218efe09cfb40f935e6703d26965621b40871413d38793081a7469412e50aa"
    "entt/core/family.hpp|d429318019b9d63ef3eff8a44fb6135917905d8e071b3f65d39fcab1a84e2e64"
    "entt/core/fwd.hpp|6b3422464a7ddfd3668184f7e0de7acea93eb1a730b0829f1b0693a5124bb7e1"
    "entt/core/hashed_string.hpp|9cb214f93b5c14c7f7ca090d99ffa416f59ae33d212e2804de43f849c6e645a1"
    "entt/core/ident.hpp|dd2e3c8616c7ffa7cb505f0f0c27682f90f3445715f49716ccba6ea780a3d293"
    "entt/core/iterator.hpp|d91b9eec7b06d191e56cd2b6c395ab715f858f9f47b0e4610dec869067a75522"
    "entt/core/memory.hpp|abbf1cac8d09363b5adba650354d4d0c9def2dcaaf63d35c258fc9bffbb50581"
    "entt/core/monostate.hpp|d5998a64b7192b026c77782216e613a4e0eb79d8e1a36c425ae1f2e08d0e99fd"
    "entt/core/ranges.hpp|5172e4cffa616dca5e33d10f4ca7ea3abe9303ea745522ebcb5a19796b6eab1e"
    "entt/core/tuple.hpp|9592623ec265e2f495230258f12034344d2405c254c3fef9a929c6624797574b"
    "entt/core/type_info.hpp|97be00b5ee0d39e7b1ddb06e962728d212fb423d024dc69dd40b713cd0217bcc"
    "entt/core/type_traits.hpp|3e8ddb202f882d46c8f9afc33b5711ed4a4072ee0b553b9a14dfcb77ce1a89b8"
    "entt/core/utility.hpp|8ec258214d34ed9a4813bda35019f0e01cf57df1f52042ca2b238abb769c53fc"
    "entt/entity/component.hpp|90228881e4fe05debd2b32eba49d9c231b36779408127199056fc8003c14d51a"
    "entt/entity/entity.hpp|5c5886b182617f360d576de40244a6b3c5d40a917e98b407a3feb81acdf6d9ad"
    "entt/entity/fwd.hpp|a37c1f0d0d85b4e7ce2a74c1809e387f9b34dbea5bdccaf4054e13cee4f5ff05"
    "entt/entity/group.hpp|6bcd68b59381796b02ae2b63f4c799f127b18fc08c410de78800d1ed0c9e64d2"
    "entt/entity/handle.hpp|8f0c654f2186006ccac74d83da4fd22c31b87d788295fe0d3f5aa2569c5e3aeb"
    "entt/entity/helper.hpp|175f20c29e868321978c0c55eb1d22cc6be153943cfe11f01f983eff5397830a"
    "entt/entity/mixin.hpp|aa5e4389fd89b3517a290c05f51bcdcc761ecb67113d79f26bc5e3c7294dbe04"
    "entt/entity/organizer.hpp|5e3eaba95a36a7909d492541d2428156e5fce62697e19a63356946b3b951ca0f"
    "entt/entity/ranges.hpp|3ed83456e763323bbca467c6d3588b1803a41b81be9906c82eeb833b3ffcbbca"
    "entt/entity/registry.hpp|ac1cce472f6802a19b540074f6594d551a734e1dcab2abc68c798ef3b82fb896"
    "entt/entity/runtime_view.hpp|1b8c00613027d938ea83c0f78c8a27ad1bc0159b984d1abd7b792af056165006"
    "entt/entity/snapshot.hpp|b52d0831c40da7fc31e7e73222cac45df45348f2dbf0a2cb2bad1844d9eec440"
    "entt/entity/sparse_set.hpp|8f3267f8c3be0d2472c30e77a39d062cf28c7b5cc0ecd86fcd32d095aabb0470"
    "entt/entity/storage.hpp|7cb3eef22b292a7f558d3bc4b9b3a0ccd065a4fc83064b96ced8e1548a55c080"
    "entt/entity/view.hpp|e7bf67d3fcc679aaff54262b78f27c632acb6f7897c570a38e803782e7700a1e"
    "entt/entt.hpp|49b24ac3042a44b07715092e2de50c0d23ea8f76a78fee6404f17247632e1d61"
    "entt/fwd.hpp|509ef8dd053ead41d8c9555bee3a3603dff0dacd723922680e7be54fadbdeaa3"
    "entt/graph/adjacency_matrix.hpp|f40cd7615cda4d38dc3ff77e50d59b5cc120b3cbf8bd85956f9a0f02ce8dc572"
    "entt/graph/dot.hpp|24c0491f7f8db5aaa98456a4ac5c35418d95677afd0ec61ee790315e978d6085"
    "entt/graph/flow.hpp|68440b755cdaeb12cb868c739e036b5fb425a548e579d6755a2e2412f4890b99"
    "entt/graph/fwd.hpp|2ddc1e9d2cdc7424cbf443d40b9db6bd299ab1f3eb2490ab9838c5bf796c7ee0"
    "entt/locator/locator.hpp|9870502f6a57c8e2b095b9aa85eeca2a086060dbe0a8b6b112ca0a725087d12f"
    "entt/meta/adl_pointer.hpp|cea9e4d2079ab6fe0e77bd64d73b75c7c965ec097d91b5ff658744aa5b0ca2ce"
    "entt/meta/container.hpp|5c808901e1a87f9c563e2343b89f1c122fde99596aeabab088802fe9f96aedaa"
    "entt/meta/context.hpp|713fe1a8400885f20185fdc2e6097ec2a8702c0752f998f95a1240b47e678df8"
    "entt/meta/factory.hpp|603e6c38246356306bc5c4e196e05b5a351c3a1091cc81c572ab343b3480cddf"
    "entt/meta/fwd.hpp|da1ea289e6bcd224ca99f59cc024988823d5f09ad535dba58ac93b30e10e4d05"
    "entt/meta/meta.hpp|d3ed3e5dd3eb7f7131d9376ee4a9051475f6cc238dcbf4a891c14b9f253049e2"
    "entt/meta/node.hpp|4a51c6a85c7bcd4c537cbcb105fdcf9826397d8c35b59417fbe774b56469c988"
    "entt/meta/pointer.hpp|b492086faecee75a950d886c9ab16901cbff20495710347097b74d44780ba6c4"
    "entt/meta/policy.hpp|2060237b597282c2257201a143d5e94af7ba7e55eaba884a55f722a2bf6afbe4"
    "entt/meta/range.hpp|0093ccac1ed67d7ff3f512ba7e703ea46fc0c481a40865da5d36e08883f15070"
    "entt/meta/resolve.hpp|70ae685355818337d947ee01e3d0ae6c1ac2865277a18b398702d8e8c4208d4b"
    "entt/meta/template.hpp|f8bc38d444d227e9f6b60937e75ba3c60190bb121fea74bf54574b87d4cf1f93"
    "entt/meta/type_traits.hpp|8c450389ec4cd56ff0b68bda47c44cddcdfe71c0c3f29e64fea7502034edda6f"
    "entt/meta/utility.hpp|e9c75ab469f07318db29df2a780b82ea92b8eae2a704fd9877109707f23e2b13"
    "entt/natvis/config.natvis|c5178b98ac3e371984843d6ad7ae5b344ec9eb52ea53fcd2072c7ee1c48fe1bd"
    "entt/natvis/container.natvis|4e6b654a3e834af16212d39545f91dee5895b8717587d31b81a2b5cee3e22b8c"
    "entt/natvis/core.natvis|931aa0b8dc96d2703368e976116147a75553329fcf39f36fb2ef1ab040828b72"
    "entt/natvis/entity.natvis|7eedd161ab3bbfdf23ed645a3d6697a582a14262a10953b829ee405c20b49956"
    "entt/natvis/graph.natvis|fcffab5a559cd2ac51cccb881e05399a3eec7978f343adeac1e2070b0ae70147"
    "entt/natvis/locator.natvis|c5178b98ac3e371984843d6ad7ae5b344ec9eb52ea53fcd2072c7ee1c48fe1bd"
    "entt/natvis/meta.natvis|39f171087febe7e9caeffce4f7fad45a8f986c62b75f0c21cdfcb1239f7fc234"
    "entt/natvis/poly.natvis|17b1efc2443c7c7a616a551c185bb69347f44af72dc5af7fdeb9f822f3dcc87b"
    "entt/natvis/process.natvis|8da5b400c598c1ecd14999e5f93f213e0c23c71e8b3b7b2c107181e67c70b155"
    "entt/natvis/resource.natvis|7b4e8a7d3625172418a7276dedbd89686b306cafc1c22e14b1e8557cfbbf6e1e"
    "entt/natvis/signal.natvis|38aa917559b6d5cf2dd3961df114ed2bc382b2c0c214614186d23a156d51771f"
    "entt/poly/fwd.hpp|7fab3cd3dad12b513fc072563cb7335ffc831db8800df7e0543a02d562f8fb94"
    "entt/poly/poly.hpp|243ee565292af5c67ecae0a80d1ae351ed48f32c76994c32ef0e34616bd19e75"
    "entt/process/fwd.hpp|4843218023842d0f150c89b9d0b87f3d7a12c2310b64f683ce266496e40bff2d"
    "entt/process/process.hpp|84257fafec4707e329f7c798cdba7a2cf9b1631c023284b54c9a6bb6d782ab88"
    "entt/process/scheduler.hpp|72d55056535aaeee7f33e1cb0c2166eb25a7ba485ec5c80f325f9fc915ea650b"
    "entt/resource/cache.hpp|25ef750a32a587905e8db6a0019bbd425989aa92c2f203fe934e512fe58498fe"
    "entt/resource/fwd.hpp|3d6a6b7ea77ea768b465201cbc738d0720e861a80671abcb4c7f07192f826558"
    "entt/resource/loader.hpp|0c4ab98561ec98018244e1e0b9ed8451090eaf522a377db191064ebb04264f42"
    "entt/resource/resource.hpp|3a1bc1703c4b7f9e6500881c9ac46d4e39577e5556896b7bbcd2b7e2bef0c0d6"
    "entt/signal/delegate.hpp|df83cada6c241550a050db6e57e30f65cc402ca7f36affe51908fb3e2269d0d1"
    "entt/signal/dispatcher.hpp|f92307fce27408fd68d8bf4e463dbd282211c939fc47d005995f163909c0bf1c"
    "entt/signal/emitter.hpp|1c788cdb7abba6f97a03ea36c818699005def4b99ba35b24260bb9f2a2f05cac"
    "entt/signal/fwd.hpp|1262a54d70fb35e076fb8db027aede9b13fccbfaaabde374881c615d35278603"
    "entt/signal/sigh.hpp|cee207cb80a53337e218910e97462d73ccc83f9595f9a84ad95f92ce7b2e1ffe"
    "entt/stl/algorithm.hpp|6e4189e134d292bb59338cb7713ebfe0b47bb2a385f4d8f0fc15c3f46208a393"
    "entt/stl/array.hpp|49d4b436cf27828be323fba0b769b762c78c1a77ed807ee62e66083180b3c35d"
    "entt/stl/atomic.hpp|e556c0a595c7e387673099acdddef99daad7e78c543f24578e578d38ffc16c38"
    "entt/stl/bit.hpp|f0929ad7c99eda38ed35379c4ffe003c82fce75bd414fde040c7b423be5f2fab"
    "entt/stl/cmath.hpp|3ea9ee34ab9fd8bc978255c05e78f07b9f05a5a6a84994a6e4b1a45a08ba8936"
    "entt/stl/concepts.hpp|e2575a305f12a0fff5ea0907beb4d14be704a3331d3828a743ad08828216f8a4"
    "entt/stl/cstddef.hpp|7b3d116c8c3b43ed51153c6bb63b21e471f033b18fa256efb44a10a43f438982"
    "entt/stl/cstdint.hpp|039f89db987e349860631c31c2fe6cc72cd05a000ed82b9fa09ef9a7a7b8d9ca"
    "entt/stl/functional.hpp|67bbd799aab99ad69e3534fd9f1f0e90142bea43c63647dc857283eec0a27038"
    "entt/stl/ios.hpp|ce80f686785d6cb0db56a36867ec3e292e10e4f95460f71b65d004aab12f7f50"
    "entt/stl/iterator.hpp|1eb08b72f9becefd5192616b5d0738d0f1a4e423d9c425c05d4aa51dc069454e"
    "entt/stl/limits.hpp|65c5f93106d00b3ba6cbc5d4c124ae7ca306aa56de3ecdd9970f85942e831c52"
    "entt/stl/memory.hpp|58fda7411a5f9f0bf4aa7a044ccd9c2f95f8e22d330527d068e92935c65759ab"
    "entt/stl/ostream.hpp|6ee8998a9594c8410094c6ada6f79323be7ed9fc73043dad594c269c8c756dff"
    "entt/stl/sstream.hpp|3b9a03d0fb6aab97e18097b0d18527013fc8121aa02fe9f8673c77ffdbfc49f7"
    "entt/stl/string.hpp|2aa5d7b6d6cf2e93d89bc4c2a4822603802122ebba41857f21d080da9d370eef"
    "entt/stl/string_view.hpp|cf3a42deb7d5b48c01907d0d066c28154125ad32edf5c393d63ee0bed57c3868"
    "entt/stl/tuple.hpp|1ac21fbc1779d2c9bc285c1187e124bb724fe7d40a8618642e42e22e4aaf92ca"
    "entt/stl/type_traits.hpp|11b89dce1a6f6af559b7b02d9094d0e90e50859586b419539f698454b179b3d3"
    "entt/stl/utility.hpp|9f87f297d7cbe987681ad7238382a2d8b78d291a6be57f6e62f806e8a02e1ff6"
    "entt/stl/vector.hpp|d909be4278e85621433cc89657f503dbf661a5756c6f7be2d2a09adc679db0ba"
    "entt/tools/davey.hpp|0f2c5fd73adc7a5659c2218921925628b412315986f5dbd2f6844a78c2677343"
    "entt/tools.hpp|7afb9378da7b064d0128e150ecc35b5c6b8abacbe6b9ba3cc56fcbb531221c84"
)
foreach(_pin IN LISTS _container_entt_pins)
    string(REPLACE "|" ";" _pair "${_pin}")
    list(GET _pair 0 _relative)
    list(GET _pair 1 _expected)
    container_check_core_file("${CONTAINER_ENTT_INCLUDE_DIR}/${_relative}" "${_expected}")
endforeach()
set(_container_json_pins
    "nlohmann/adl_serializer.hpp|80742b0081e0ac5094ad4d7a3549e969b004bb7ed86807911a8177c843701262"
    "nlohmann/byte_container_with_subtype.hpp|2b75a051d2f82fd95f613c70472b85a281d085f1973791c1118244099bf38942"
    "nlohmann/detail/abi_macros.hpp|cebb5441f1ce851444f47a7d8ea0446339ee499df83215ebd873c8347a191bd7"
    "nlohmann/detail/conversions/from_json.hpp|5f227d243c0d5c0d67c4c72b2b57fb8a7d50053ab2d667fb6a5880b1481fc9f1"
    "nlohmann/detail/conversions/to_chars.hpp|20b9bc4469ee490bc626d8dbcdee060a494292473df4bdbcecfe2f2145ed562d"
    "nlohmann/detail/conversions/to_json.hpp|46429c1b87ea71962ffb68dcb0a7e7811a3ebf5f102bcf318688e48ff1f6b73e"
    "nlohmann/detail/exceptions.hpp|5c9b21e0a13ac0bab8dea676b9804d739dfd2adc24f036f97ab243ef70e2aae7"
    "nlohmann/detail/hash.hpp|a025807281c5d92fdfddcc993b2b2a9658146435cc10b003723b130698445c80"
    "nlohmann/detail/input/binary_reader.hpp|9e617825317910352f2a2025bb8749898287fa3003e81ce22a57b52c535d0eab"
    "nlohmann/detail/input/input_adapters.hpp|01fa7af8fdd1ba7b1c12089bf92783f70d09a93db7564e4df70544fc85304de6"
    "nlohmann/detail/input/json_sax.hpp|0ed6fdb0ef91f163cca312b7ffd6e0842f12e1b8340204ab2d0c05277e0d1f18"
    "nlohmann/detail/input/lexer.hpp|a8b73e55242731375c088133f3a355af7b1bb82d6b9531678cb20560d3733c1d"
    "nlohmann/detail/input/parser.hpp|b4a762967b174de0c43896b465e4a7c8c92c3a8636f669debdec836050f7912a"
    "nlohmann/detail/input/position_t.hpp|0239037872b622a5f3f42fa1b453d201f977d21829b2dc4a19abe2594515eada"
    "nlohmann/detail/iterators/internal_iterator.hpp|bf015623518c5aab4762d9a4d21af0e85a26cdcb3fc6e69e464c8575a3b87aba"
    "nlohmann/detail/iterators/iter_impl.hpp|c6f0664c4965287154505837dde4ae13e7625978745e8876951fc90da61b5068"
    "nlohmann/detail/iterators/iteration_proxy.hpp|3c562f1783e1fc9476ed53d8054bc50309caa425d993996a9ddf9194fb2129cc"
    "nlohmann/detail/iterators/iterator_traits.hpp|960d1fefbe7cb220292b07e4ec047bfa9a3ccbd753adaf62e5d583f746e71e47"
    "nlohmann/detail/iterators/json_reverse_iterator.hpp|6f072fb2b9e151ad9645cb03a9aa23231480bf447e53b2e0ecb41f45fb7503e1"
    "nlohmann/detail/iterators/primitive_iterator.hpp|40a03177d519ba378e59bed0a53a9be8022fd3d0ee701409781c0e856d5bf510"
    "nlohmann/detail/json_custom_base_class.hpp|e622910868b019ba09460c96a6092c1018ed501f34cce4ed633641400c2d2500"
    "nlohmann/detail/json_pointer.hpp|9c5b54a455e96c63788c157d914b70cde6a3f36ef1e5df037f4fc17bb92f235d"
    "nlohmann/detail/json_ref.hpp|5d133945d0859acaa82c50c1a044b1e1f806c0a38107eae24bf07ff5944fc1e3"
    "nlohmann/detail/macro_scope.hpp|76012b860dc76e7c28efead61311d588596417f5b476fc20f762b1ad81858ae0"
    "nlohmann/detail/macro_unscope.hpp|626b7e10290a2bb558634b8bbfbc5118b5ce696151b5bc87418142c578bd501e"
    "nlohmann/detail/meta/call_std/begin.hpp|4629b547aa6b6629c266b1b474f576cbde5ce898c96502c828a72d99212b9f34"
    "nlohmann/detail/meta/call_std/end.hpp|e09dd80f2263ff192d60bad19bdbdc5402866d0b0c8caeb9c2759b26ba864a39"
    "nlohmann/detail/meta/cpp_future.hpp|71e606c586174ff875f5996a00bc6e84a49e0919eafd6fb38c7cb928dfdf5795"
    "nlohmann/detail/meta/detected.hpp|d144ac2ab6bb92be35bfda7eec560b9d030760e4330b7f951a2aca63469488a1"
    "nlohmann/detail/meta/identity_tag.hpp|37ff788879d4b14ffa2e79bdc75b470d2c2ca5ebadafef649312f653455d45fa"
    "nlohmann/detail/meta/is_sax.hpp|e60398b282767374747db20ab06101804c7345eea0761a313a298bf87b913f91"
    "nlohmann/detail/meta/std_fs.hpp|257716f0958e9fd6c1dc3f0335716d779550353c58bc929e8dda1c79bd336701"
    "nlohmann/detail/meta/type_traits.hpp|d5da966796c089fa89cba5474309fd2124b083d99adf17991084a6a532fdf55d"
    "nlohmann/detail/meta/void_t.hpp|c340e7b148543b0abfd719ddbc3759e23c2feffc63656136a15fd80b98ca8bf8"
    "nlohmann/detail/output/binary_writer.hpp|cdf56265d14b80505796dfb94d79896a226e8888e4f730ff6e92c31a2b891cd1"
    "nlohmann/detail/output/output_adapters.hpp|3322dce8f63e69930806bd8691c2cd9ece8a76b54b34f617641329c44559bbb5"
    "nlohmann/detail/output/serializer.hpp|4c079acb8f6936c4f787fd5706a52c77a1751dd34787a821705845698ec8540e"
    "nlohmann/detail/string_concat.hpp|fb30fb50c325a1b5610a62a9061a1966b4e850dd964635c1ad1c4c9da48f7daa"
    "nlohmann/detail/string_escape.hpp|cb12b0e5903b1ec90aaa96a4a71e0cf7106440ca9cedcff6495677d01f244733"
    "nlohmann/detail/string_utils.hpp|85e2fbdfe07f1a44b90d5422ee1b951d1053f32e429dc4aee36274fb03fd9354"
    "nlohmann/detail/value_t.hpp|67ed7b164db48556bb4ae409a645401faba459081570f9b9f9ff977fd32c6114"
    "nlohmann/json.hpp|88d7eb42b19599cab48856576dd4ea52538df72ad7c57200c50de98bda246458"
    "nlohmann/json_fwd.hpp|8777a878c20a412dd04c8011c87c3d72d74e48a4dda5e52f7948e26dd2168931"
    "nlohmann/ordered_map.hpp|2d4e2d9f6e6a0c2d2adadcb620da1f49585fa4e54e54cfa16b6d20e01d857d61"
    "nlohmann/thirdparty/hedley/hedley.hpp|1547b367895f8be9f8283ae21f583182db572ad990236220002b632a15238712"
    "nlohmann/thirdparty/hedley/hedley_undef.hpp|ddf06784ca9448b4899db96a409ec1cbc65a0f682db9c966c011f00b77ae75c4"
)
foreach(_pin IN LISTS _container_json_pins)
    string(REPLACE "|" ";" _pair "${_pin}")
    list(GET _pair 0 _relative)
    list(GET _pair 1 _expected)
    container_check_core_file("${CONTAINER_JSON_INCLUDE_DIR}/${_relative}" "${_expected}")
endforeach()
container_stage_selected_cpp_notice("${CONTAINER_ENTT_INCLUDE_DIR}" entt EnTT-4.0.0-LICENSE.txt
    "73547f6370ee16ea560665329bb62d4e2d8250f41eaa204b47a6c694ca3286ea")
container_stage_selected_cpp_notice("${CONTAINER_JSON_INCLUDE_DIR}" nlohmann-json nlohmann-json-3.12.0-LICENSE.txt
    "46a65cffd1ea955132d95a8dd921640714a8d6b537d2e4e482d31145ae95b603")
set(CONTAINER_ENTT_VERSION "4.0.0" CACHE INTERNAL "Verified selected entt release" FORCE)
set(CONTAINER_JSON_VERSION "3.12.0" CACHE INTERNAL "Verified selected nlohmann-json release" FORCE)
set(_cpp_library_rows "")
file(SHA256 "${CMAKE_BINARY_DIR}/third_party_licenses/EnTT-4.0.0-LICENSE.txt" _raw)
list(APPEND _cpp_library_rows "{\"name\":\"entt\",\"version\":\"4.0.0\",\"notice\":\"EnTT-4.0.0-LICENSE.txt\",\"sha256\":\"${_raw}\"}")
file(SHA256 "${CMAKE_BINARY_DIR}/third_party_licenses/nlohmann-json-3.12.0-LICENSE.txt" _raw)
list(APPEND _cpp_library_rows "{\"name\":\"nlohmann-json\",\"version\":\"3.12.0\",\"notice\":\"nlohmann-json-3.12.0-LICENSE.txt\",\"sha256\":\"${_raw}\"}")
string(JOIN "," _cpp_library_json ${_cpp_library_rows})
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/selected-ecs-json-dependencies.json" CONTENT "{\"libraries\":[${_cpp_library_json}]}\n" @ONLY)
