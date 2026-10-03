# cmake/Shaders.cmake
# Slang → SPIR-V shader compilation.

set(SHADERS_DIR "${CMAKE_SOURCE_DIR}/shaders")
set(COMPILED_SHADERS_DIR "${CMAKE_BINARY_DIR}/spv_shaders")

find_program(SLANGC_VULKAN_SDK_EXECUTABLE
    slangc
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
    NO_DEFAULT_PATH
)

if(SLANGC_VULKAN_SDK_EXECUTABLE)
    set(SLANGC_EXECUTABLE "${SLANGC_VULKAN_SDK_EXECUTABLE}" CACHE FILEPATH
        "Slang compiler" FORCE)
else()
    find_program(SLANGC_EXECUTABLE slangc REQUIRED)
endif()

if(NOT SLANGC_EXECUTABLE)
    message(WARNING "slangc not found — shader compilation disabled")
    return()
endif()

set(SLANG_MATRIX_LAYOUT_FLAG "-matrix-layout-column-major")
set(SLANG_SPIRV_FLAGS
    -target spirv
    -profile spirv_1_6
    -emit-spirv-directly
    -fvk-use-entrypoint-name
    -I "${SHADERS_DIR}"
    ${SLANG_MATRIX_LAYOUT_FLAG}
)

file(GLOB SLANG_SOURCES CONFIGURE_DEPENDS "${SHADERS_DIR}/*.slang")
set(SLANG_INCLUDE_SHADER_NAMES
    depth_normal_common.slang
    surface_normal_common.slang
    pbr_material_common.slang
    object_data_common.slang
    material_data_common.slang
    alpha_mask_common.slang
    object_index_common.slang
    brdf_common.slang
    area_light_common.slang
    lighting_structs.slang
    shadow_common.slang
    local_shadow_common.slang
    screen_space_light_shadow_common.slang
    oit_common.slang
    push_constants_common.slang
    scene_clip_common.slang
    draw_indirect_common.slang
    oit_resolve_common.slang
    temporal_common.slang
)

set(SLANG_COMPUTE_SHADER_NAMES
    tile_light_cull.slang
    brdf_lut.slang
    equirect_to_cubemap.slang
    irradiance_convolution.slang
    prefilter_specular.slang
    gtao.slang
    gtao_blur.slang
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
    temporal_resolve.slang
    exposure_adapt.slang
)

set(SLANG_GEOMETRY_SHADER_NAMES
    surface_normals.slang
    wireframe_fallback.slang
    normal_validation.slang
)

set(SLANG_INCLUDE_SOURCES)
foreach(SLANG_INCLUDE_SHADER_NAME IN LISTS SLANG_INCLUDE_SHADER_NAMES)
    set(SLANG_INCLUDE_SOURCE "${SHADERS_DIR}/${SLANG_INCLUDE_SHADER_NAME}")
    if(EXISTS "${SLANG_INCLUDE_SOURCE}")
        list(APPEND SLANG_INCLUDE_SOURCES "${SLANG_INCLUDE_SOURCE}")
    endif()
endforeach()

set(SLANG_RENDER_SHADER_EXCLUDE_NAMES
    ${SLANG_INCLUDE_SHADER_NAMES}
    ${SLANG_COMPUTE_SHADER_NAMES}
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

function(add_slang_output OUTPUTS_VAR SLANG_SOURCE ENTRY_POINT OUTPUT_PATH)
    add_custom_command(
        OUTPUT "${OUTPUT_PATH}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${COMPILED_SHADERS_DIR}"
        COMMAND "${SLANGC_EXECUTABLE}" "${SLANG_SOURCE}"
                ${SLANG_SPIRV_FLAGS}
                -entry "${ENTRY_POINT}"
                -o "${OUTPUT_PATH}"
        DEPENDS
            "${SLANG_SOURCE}"
            ${SLANG_INCLUDE_SOURCES}
            "${CMAKE_CURRENT_LIST_FILE}"
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
        add_slang_output(
            SLANG_OUTPUTS "${SLANG_SOURCE}" "computeMain" "${COMP_OUTPUT}")
    endif()
endforeach()

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
