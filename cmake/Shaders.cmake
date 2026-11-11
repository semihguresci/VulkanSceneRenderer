# cmake/Shaders.cmake
# Slang → SPIR-V shader compilation.

set(SHADERS_DIR "${CMAKE_SOURCE_DIR}/shaders")
set(COMPILED_SHADERS_DIR "${CMAKE_BINARY_DIR}/spv_shaders")

if(NOT SLANGC_EXECUTABLE)
    # An empty cache entry is also an unset selection. Preserve every usable
    # explicit/cached path, even when VULKAN_SDK contains another compiler.
    unset(SLANGC_EXECUTABLE)
    unset(SLANGC_EXECUTABLE CACHE)
    find_program(SLANGC_EXECUTABLE
        slangc
        HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
        NO_DEFAULT_PATH
    )
    if(NOT SLANGC_EXECUTABLE)
        find_program(SLANGC_EXECUTABLE slangc REQUIRED)
    endif()
endif()

if(NOT SLANGC_EXECUTABLE)
    message(WARNING "slangc not found — shader compilation disabled")
    return()
endif()
message(STATUS "Slang compiler: ${SLANGC_EXECUTABLE}")

set(SLANG_MATRIX_LAYOUT_FLAG "-matrix-layout-column-major")
set(SLANG_SPIRV_FLAGS
    -target spirv
    -profile spirv_1_6
    -emit-spirv-directly
    -fvk-use-entrypoint-name
    -I "${SHADERS_DIR}"
    ${SLANG_MATRIX_LAYOUT_FLAG}
    -DCONTAINER_HAS_NANOVDB=${CONTAINER_HAS_NANOVDB}
)
if(CONTAINER_HAS_NANOVDB)
    list(APPEND SLANG_SPIRV_FLAGS -I "${NANOVDB_INCLUDE_DIR}")
endif()

file(GLOB SLANG_SOURCES CONFIGURE_DEPENDS "${SHADERS_DIR}/*.slang")
set(SLANG_INCLUDE_SHADER_NAMES
    depth_normal_common.slang
    ltc_path_profile.slang
    surface_normal_common.slang
    pbr_material_common.slang
    pbr_extended_common.slang
    pbr_extended_deferred_common.slang
    pbr_extended_sampling_common.slang
    ddgi_material_common.slang
    path_transport_common.slang
    path_dielectric_common.slang
    path_experimental_materials.slang
    path_clearcoat_common.slang
    object_data_common.slang
    material_data_common.slang
    alpha_mask_common.slang
    object_index_common.slang
    brdf_common.slang
    area_light_common.slang
    area_light_ltc.slang
    lighting_structs.slang
    shadow_common.slang
    local_shadow_common.slang
    screen_space_light_shadow_common.slang
    screen_reflections_ray_common.slang
    oit_common.slang
    push_constants_common.slang
    display_tone_curves.slang
    scene_clip_common.slang
    openvdb_geometry_shadow.slang
    volume_area_light_common.slang
    openvdb_area_light.slang
    openvdb_multiple_scattering.slang
    openvdb_transparency.slang
    openvdb_sparse.slang
    draw_indirect_common.slang
    oit_resolve_common.slang
    temporal_common.slang
    environment_sky_common.slang
    atmosphere_scattering.slang
    aerial_perspective.slang
    ray_shadow_data.slang
    ray_shadow_receiver_common.slang
    ray_shadow_read.slang
    ray_visibility_common.slang
    ray_scene_data.slang
    ray_surface_common.slang
    ray_footprint_common.slang
    ddgi_data.slang
    ddgi_lookup_common.slang
    ddgi_lighting_common.slang
    ddgi_transport_common.slang
)

set(SLANG_COMPUTE_SHADER_NAMES
    material_sampler_contract.slang
    gltf_deformation.slang
    hierarchy_transform.slang
    ray_instance_transform.slang
    tile_light_cull.slang
    brdf_lut.slang
    equirect_to_cubemap.slang
    irradiance_convolution.slang
    prefilter_specular.slang
    gtao.slang
    gtao_blur.slang
    screen_reflections.slang
    frustum_cull.slang
    hiz_generate.slang
    occlusion_cull.slang
    shadow_cull.slang
    bim_meshlet_residency.slang
    bim_visibility_filter.slang
    bim_draw_compact.slang
    bloom_downsample.slang
    bloom_upsample.slang
    exposure_histogram.slang
    temporal_compose.slang
    fsr_inputs.slang
    openvdb_volume.slang
    openvdb_transport_reference.slang
    temporal_resolve.slang
    exposure_adapt.slang
    ray_shadow_trace.slang
    ray_shadow_filter.slang
    ddgi_trace.slang
    reference_pathtrace.slang
    reference_background.slang
    ddgi_update.slang
)

set(SLANG_GEOMETRY_SHADER_NAMES
    surface_normals.slang
    wireframe_fallback.slang
    normal_validation.slang
)

set(SLANG_INCLUDE_SOURCES)
list(APPEND SLANG_INCLUDE_SOURCES
    "${CMAKE_SOURCE_DIR}/include/Container/renderer/debug/SelectionStrokeKernel.h")
foreach(SLANG_INCLUDE_SHADER_NAME IN LISTS SLANG_INCLUDE_SHADER_NAMES)
    set(SLANG_INCLUDE_SOURCE "${SHADERS_DIR}/${SLANG_INCLUDE_SHADER_NAME}")
    if(EXISTS "${SLANG_INCLUDE_SOURCE}")
        list(APPEND SLANG_INCLUDE_SOURCES "${SLANG_INCLUDE_SOURCE}")
    endif()
endforeach()

set(SLANG_RENDER_SHADER_EXCLUDE_NAMES
    ${SLANG_INCLUDE_SHADER_NAMES}
    ${SLANG_COMPUTE_SHADER_NAMES}
    reference_denoise_compose.slang
    dlss_transparency.slang
    volumetric_fog.slang
)
foreach(SLANG_RENDER_SHADER_EXCLUDE_NAME IN LISTS SLANG_RENDER_SHADER_EXCLUDE_NAMES)
    string(REPLACE "." "\\." SLANG_RENDER_SHADER_EXCLUDE_REGEX
        "${SLANG_RENDER_SHADER_EXCLUDE_NAME}")
    list(FILTER SLANG_SOURCES EXCLUDE
        REGEX "(^|.*/)${SLANG_RENDER_SHADER_EXCLUDE_REGEX}$")
endforeach()

list(LENGTH SLANG_SOURCES SLANG_COUNT)
message(STATUS "Found ${SLANG_COUNT} render Slang shaders in ${SHADERS_DIR}")

set(SLANG_OUTPUTS)
set(RELOAD_INTERFACE_DIR "${CMAKE_BINARY_DIR}/shader_reload_interfaces")
set(RELOAD_SCRIPT "${CMAKE_BINARY_DIR}/runtime_shader_reload.cmake")
configure_file("${CMAKE_CURRENT_LIST_DIR}/RuntimeShaderReload.cmake.in"
               "${RELOAD_SCRIPT}" @ONLY)
file(WRITE "${CMAKE_BINARY_DIR}/runtime_shader_reload_tool.txt" "${CMAKE_COMMAND}")

function(add_slang_output OUTPUTS_VAR SLANG_SOURCE ENTRY_POINT OUTPUT_PATH)
    get_filename_component(_output_name "${OUTPUT_PATH}" NAME)
    get_filename_component(_output_directory "${OUTPUT_PATH}" DIRECTORY)
    set(_reflection "${RELOAD_INTERFACE_DIR}/${_output_name}.json")
    set(_depfile "${RELOAD_INTERFACE_DIR}/${_output_name}.d")
    set(_interface_flags)
    set(_interface_products)
    set(_fingerprint_command)
    get_filename_component(_source_directory "${SLANG_SOURCE}" DIRECTORY)
    if(_source_directory STREQUAL SHADERS_DIR AND
       _output_directory STREQUAL COMPILED_SHADERS_DIR AND
       NOT _output_name STREQUAL "openvdb_transport_reference.comp.spv")
        set(_interface_flags -reflection-json "${_reflection}")
        set(_interface_products BYPRODUCTS "${_reflection}" "${_depfile}.sha256")
        set(_fingerprint_command COMMAND "${CMAKE_COMMAND}"
            "-DFINGERPRINT_OUTPUT=${_depfile}.sha256" "-DDEPFILE=${_depfile}"
            "-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ShaderDependencies.cmake")
        set(_reload_args "")
        foreach(_arg IN LISTS ARGN)
            string(APPEND _reload_args " [==[${_arg}]==]")
        endforeach()
        file(APPEND "${RELOAD_SCRIPT}"
            "compile_reload_shader([==[${SLANG_SOURCE}]==] [==[${ENTRY_POINT}]==] [==[${_output_name}]==]${_reload_args})\n")
    endif()
    add_custom_command(
        OUTPUT "${OUTPUT_PATH}"
        ${_interface_products}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_output_directory}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RELOAD_INTERFACE_DIR}"
        COMMAND "${SLANGC_EXECUTABLE}" "${SLANG_SOURCE}"
                ${SLANG_SPIRV_FLAGS}
                ${ARGN}
                -entry "${ENTRY_POINT}"
                -depfile "${_depfile}"
                ${_interface_flags}
                -o "${OUTPUT_PATH}"
        ${_fingerprint_command}
        DEPFILE "${_depfile}"
        DEPENDS
            "${SLANG_SOURCE}"
            ${SLANG_INCLUDE_SOURCES}
            "${CMAKE_CURRENT_LIST_FILE}"
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ShaderDependencies.cmake"
            "${SLANGC_EXECUTABLE}"
        COMMENT "Compiling ${SLANG_SOURCE} (${ENTRY_POINT})"
        VERBATIM
    )
    set(_outputs "${${OUTPUTS_VAR}}")
    list(APPEND _outputs "${OUTPUT_PATH}")
    set(${OUTPUTS_VAR} "${_outputs}" PARENT_SCOPE)
endfunction()

foreach(SLANG_SOURCE ${SLANG_SOURCES})
    get_filename_component(SHADER_BASE ${SLANG_SOURCE} NAME_WE)
    set(VERT_OUTPUT "${COMPILED_SHADERS_DIR}/${SHADER_BASE}.vert.spv")
    set(FRAG_OUTPUT "${COMPILED_SHADERS_DIR}/${SHADER_BASE}.frag.spv")

    add_slang_output(
        SLANG_OUTPUTS "${SLANG_SOURCE}" "vertMain" "${VERT_OUTPUT}")
    add_slang_output(
        SLANG_OUTPUTS "${SLANG_SOURCE}" "fragMain" "${FRAG_OUTPUT}")
endforeach()

# Optional forward fragment queries handle opaque MSAA receivers that differ
# from the single resolved-depth visibility buffer. Baseline lighting shaders
# remain usable on devices without ray-query support.
add_slang_output(
    SLANG_OUTPUTS "${SHADERS_DIR}/forward_opaque.slang" "fragMain"
    "${COMPILED_SHADERS_DIR}/forward_opaque_ray.frag.spv"
    -DFORWARD_RAY_QUERY=1 -capability spvRayQueryKHR)

add_slang_output(SLANG_OUTPUTS "${SHADERS_DIR}/screen_reflections.slang" "computeMain"
    "${COMPILED_SHADERS_DIR}/screen_reflections_ray.comp.spv"
    -DSCREEN_REFLECTION_RAY_QUERY=1 -capability spvRayQueryKHR)

foreach(FOG_STAGE inject integrate composite)
    string(TOUPPER "${FOG_STAGE}" FOG_DEFINE)
    add_slang_output(SLANG_OUTPUTS "${SHADERS_DIR}/volumetric_fog.slang" "computeMain"
        "${COMPILED_SHADERS_DIR}/volumetric_fog_${FOG_STAGE}.comp.spv" "-DFOG_${FOG_DEFINE}=1")
endforeach()

# Vulkan guarantees only four bound descriptor sets. Keep raster lighting
# usable on that limit without reserving a fifth set for optional DDGI.
foreach(DDGI_LIGHTING_SHADER forward_opaque deferred_directional point_light tiled_lighting)
    add_slang_output(SLANG_OUTPUTS "${SHADERS_DIR}/${DDGI_LIGHTING_SHADER}.slang" "fragMain"
        "${COMPILED_SHADERS_DIR}/${DDGI_LIGHTING_SHADER}_no_ddgi.frag.spv" -DDDGI_DISABLED=1)
endforeach()
add_slang_output(SLANG_OUTPUTS "${SHADERS_DIR}/forward_opaque.slang" "fragMain"
    "${COMPILED_SHADERS_DIR}/forward_opaque_ray_no_ddgi.frag.spv"
    -DDDGI_DISABLED=1 -DFORWARD_RAY_QUERY=1 -capability spvRayQueryKHR)

# Keep diagnostic SPIR-V out of both default staging and runtime reload.
set(LTC_PROFILE_OUTPUTS)
set(LTC_PROFILE_SHADER_DIR "${CMAKE_BINARY_DIR}/ltc_profile_shaders")
set(LTC_PROFILE_SHADER_NAMES deferred_directional deferred_directional_no_ddgi post_process)
foreach(LTC_PROFILE_SHADER IN LISTS LTC_PROFILE_SHADER_NAMES)
    string(REGEX REPLACE "_no_ddgi$" "" LTC_PROFILE_SOURCE "${LTC_PROFILE_SHADER}")
    set(LTC_PROFILE_DEFINES -DCONTAINER_PROFILE_LTC_PATHS=1)
    if(LTC_PROFILE_SHADER MATCHES "_no_ddgi$")
        list(APPEND LTC_PROFILE_DEFINES -DDDGI_DISABLED=1)
    endif()
    add_slang_output(LTC_PROFILE_OUTPUTS "${SHADERS_DIR}/${LTC_PROFILE_SOURCE}.slang" "fragMain"
        "${LTC_PROFILE_SHADER_DIR}/ltc_profile_${LTC_PROFILE_SHADER}.frag.spv" ${LTC_PROFILE_DEFINES})
    list(APPEND LTC_PROFILE_OUTPUTS "${COMPILED_SHADERS_DIR}/${LTC_PROFILE_SHADER}.frag.spv")
endforeach()
set(LTC_PROFILE_HASHES "${LTC_PROFILE_SHADER_DIR}/shader-pairs.sha256")
add_custom_command(OUTPUT "${LTC_PROFILE_HASHES}"
    COMMAND ${CMAKE_COMMAND}
        "-DORDINARY_SHADER_DIR=${COMPILED_SHADERS_DIR}"
        "-DPROFILE_SHADER_DIR=${LTC_PROFILE_SHADER_DIR}"
        "-DPROFILE_SHADER_NAMES=${LTC_PROFILE_SHADER_NAMES}"
        -P "${CMAKE_CURRENT_LIST_DIR}/LtcProfileShaderHashes.cmake"
    DEPENDS ${LTC_PROFILE_OUTPUTS} "${CMAKE_CURRENT_LIST_DIR}/LtcProfileShaderHashes.cmake"
    COMMENT "Recording ordinary and diagnostic LTC shader hashes"
    VERBATIM)
add_custom_target(ltc_profile_shaders DEPENDS "${LTC_PROFILE_HASHES}")

# Geometry shaders for specific files.
foreach(SLANG_GEOMETRY_SHADER_NAME IN LISTS SLANG_GEOMETRY_SHADER_NAMES)
    set(SLANG_SOURCE "${SHADERS_DIR}/${SLANG_GEOMETRY_SHADER_NAME}")
    if(EXISTS "${SLANG_SOURCE}")
        get_filename_component(SHADER_BASE "${SLANG_SOURCE}" NAME_WE)
        set(GEOM_OUTPUT "${COMPILED_SHADERS_DIR}/${SHADER_BASE}.geom.spv")
        add_slang_output(
            SLANG_OUTPUTS "${SLANG_SOURCE}" "geomMain" "${GEOM_OUTPUT}")
    endif()
endforeach()

# Compute shaders.
foreach(SLANG_COMPUTE_SHADER_NAME IN LISTS SLANG_COMPUTE_SHADER_NAMES)
    set(SLANG_SOURCE "${SHADERS_DIR}/${SLANG_COMPUTE_SHADER_NAME}")
    if(EXISTS "${SLANG_SOURCE}")
        get_filename_component(SHADER_BASE "${SLANG_SOURCE}" NAME_WE)
        set(COMP_OUTPUT "${COMPILED_SHADERS_DIR}/${SHADER_BASE}.comp.spv")
        set(SLANG_OPTIONAL_CAPABILITIES)
        if(SHADER_BASE STREQUAL "ray_shadow_trace" OR SHADER_BASE STREQUAL "ddgi_trace")
            set(SLANG_OPTIONAL_CAPABILITIES -capability spvRayQueryKHR)
        endif()
        add_slang_output(
            SLANG_OUTPUTS "${SLANG_SOURCE}" "computeMain" "${COMP_OUTPUT}" ${SLANG_OPTIONAL_CAPABILITIES})
    endif()
endforeach()

if(CONTAINER_DLSS_AVAILABLE)
    add_slang_output(SLANG_OUTPUTS "${SHADERS_DIR}/temporal_compose.slang" "computeMain"
        "${COMPILED_SHADERS_DIR}/dlss_compose.comp.spv" -DDLSS_TRANSPARENCY=1)
    add_slang_output(SLANG_OUTPUTS "${SHADERS_DIR}/dlss_transparency.slang" "computeMain"
        "${COMPILED_SHADERS_DIR}/dlss_transparency.comp.spv")
endif()

if(CONTAINER_DLSS_RR_AVAILABLE)
    add_slang_output(SLANG_OUTPUTS "${SHADERS_DIR}/reference_pathtrace.slang" "computeMain"
        "${COMPILED_SHADERS_DIR}/reference_pathtrace_rr.comp.spv" -DREFERENCE_DLSS_RR=1)
    add_slang_output(SLANG_OUTPUTS "${SHADERS_DIR}/reference_background.slang" "computeMain"
        "${COMPILED_SHADERS_DIR}/reference_pathtrace_empty_rr.comp.spv" -DREFERENCE_DLSS_RR=1)
endif()

if(CONTAINER_VK_OIDN_AVAILABLE OR CONTAINER_OPTIX_AVAILABLE)
    add_slang_output(SLANG_OUTPUTS "${SHADERS_DIR}/reference_pathtrace.slang" "computeMain"
        "${COMPILED_SHADERS_DIR}/reference_guided_pathtrace.comp.spv" -DREFERENCE_DENOISE_GUIDES=1)
    add_slang_output(SLANG_OUTPUTS "${SHADERS_DIR}/reference_background.slang" "computeMain"
        "${COMPILED_SHADERS_DIR}/reference_guided_background.comp.spv" -DREFERENCE_DENOISE_GUIDES=1)
endif()
if(CONTAINER_VK_OIDN_AVAILABLE OR CONTAINER_OPTIX_AVAILABLE OR CONTAINER_DLSS_RR_AVAILABLE)
    add_slang_output(SLANG_OUTPUTS "${SHADERS_DIR}/reference_denoise_compose.slang" "computeMain"
        "${COMPILED_SHADERS_DIR}/reference_denoise_compose.comp.spv")
endif()

set(CONTAINER_SCENE_TEXTURE_SAMPLING_SPIRV "${COMPILED_SHADERS_DIR}/scene_texture_sampling_contract.comp.spv")
add_slang_output(SLANG_OUTPUTS
    "${CMAKE_SOURCE_DIR}/tests/shaders/scene_texture_sampling_contract.slang"
    "computeMain" "${CONTAINER_SCENE_TEXTURE_SAMPLING_SPIRV}")

set(CONTAINER_CAMERA_PROJECTION_SPIRV "${COMPILED_SHADERS_DIR}/camera_projection_contract.comp.spv")
add_slang_output(SLANG_OUTPUTS
    "${CMAKE_SOURCE_DIR}/tests/shaders/camera_projection_contract.slang"
    "computeMain" "${CONTAINER_CAMERA_PROJECTION_SPIRV}")

set(CONTAINER_LIGHT_EDIT_SPIRV "${COMPILED_SHADERS_DIR}/light_edit_contract.comp.spv")
add_slang_output(SLANG_OUTPUTS
    "${CMAKE_SOURCE_DIR}/tests/shaders/light_edit_contract.slang"
    "computeMain" "${CONTAINER_LIGHT_EDIT_SPIRV}")
set(CONTAINER_LIGHTING_SCRATCH_SPIRV "${COMPILED_SHADERS_DIR}/lighting_scratch_contract.comp.spv")
add_slang_output(SLANG_OUTPUTS
    "${CMAKE_SOURCE_DIR}/tests/shaders/lighting_scratch_contract.slang"
    "computeMain" "${CONTAINER_LIGHTING_SCRATCH_SPIRV}")

if(NOT SLANG_OUTPUTS)
    message(WARNING "No Slang shader outputs found in ${SHADERS_DIR}")
    return()
endif()

add_custom_target(shaders_all
    DEPENDS ${SLANG_OUTPUTS}
    COMMENT "Building Slang shaders"
    VERBATIM
)

add_custom_target(shaders DEPENDS shaders_all)

# The offline image tool and GPU reference share the production compute kernel.
set(CONTAINER_OPENVDB_TRANSPORT_SPIRV "${COMPILED_SHADERS_DIR}/openvdb_transport_reference.comp.spv")
add_custom_target(openvdb_transport_reference_shader DEPENDS "${CONTAINER_OPENVDB_TRANSPORT_SPIRV}")
if(TARGET OpenVdbTransportRender)
    add_custom_target(stage_openvdb_transport_shader
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:OpenVdbTransportRender>/spv_shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CONTAINER_OPENVDB_TRANSPORT_SPIRV}"
            "$<TARGET_FILE_DIR:OpenVdbTransportRender>/spv_shaders/openvdb_transport_reference.comp.spv"
        VERBATIM)
    add_dependencies(stage_openvdb_transport_shader openvdb_transport_reference_shader)
    add_dependencies(OpenVdbTransportRender stage_openvdb_transport_shader)
endif()
