# Pin the ordinary and diagnostic binaries from the same build invocation.
set(profile_hashes "")
foreach(shader IN LISTS PROFILE_SHADER_NAMES)
    set(name "${shader}.frag.spv")
    file(SHA256 "${ORDINARY_SHADER_DIR}/${name}" ordinary_sha)
    file(SHA256 "${PROFILE_SHADER_DIR}/ltc_profile_${name}" profile_sha)
    string(APPEND profile_hashes "${name} ${ordinary_sha} ${profile_sha}\n")
endforeach()
file(WRITE "${PROFILE_SHADER_DIR}/shader-pairs.sha256" "${profile_hashes}")
