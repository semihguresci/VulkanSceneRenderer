enable_testing()
find_package(GTest CONFIG REQUIRED)

set(TESTS_DIR "${CMAKE_SOURCE_DIR}/tests")
set(TEST_RESULTS_DIR "${CMAKE_BINARY_DIR}/test_results")
file(MAKE_DIRECTORY ${TEST_RESULTS_DIR})

set(DEFAULT_SHADER_DIR "${TESTS_DIR}/shaders")
set(TEST_CORE_DIR "${TESTS_DIR}/core")
set(TEST_GEOMETRY_DIR "${TESTS_DIR}/geometry")
set(TEST_PLATFORM_DIR "${TESTS_DIR}/platform")
set(TEST_RENDERER_DIR "${TESTS_DIR}/renderer")
set(TEST_RENDERER_BIM_DIR "${TEST_RENDERER_DIR}/bim")
set(TEST_RENDERER_CORE_DIR "${TEST_RENDERER_DIR}/core")
set(TEST_RENDERER_CULLING_DIR "${TEST_RENDERER_DIR}/culling")
set(TEST_RENDERER_DEFERRED_DIR "${TEST_RENDERER_DIR}/deferred")
set(TEST_RENDERER_FORWARD_DIR "${TEST_RENDERER_DIR}/forward")
set(TEST_RENDERER_LIGHTING_DIR "${TEST_RENDERER_DIR}/lighting")
set(TEST_RENDERER_PICKING_DIR "${TEST_RENDERER_DIR}/picking")
set(TEST_RENDERER_SCENE_DIR "${TEST_RENDERER_DIR}/scene")
set(TEST_RENDERER_SHADOW_DIR "${TEST_RENDERER_DIR}/shadow")
set(TEST_SCENE_DIR "${TESTS_DIR}/scene")
set(TEST_UI_DIR "${TESTS_DIR}/ui")
set(TEST_VALIDATION_DIR "${TESTS_DIR}/validation")
set(TEST_CMAKE_DIR "${TESTS_DIR}/cmake")

add_test(
    NAME fetch_gltf_sample_models_config_tests
    COMMAND ${CMAKE_COMMAND}
            "-DPROJECT_SOURCE_DIR_FOR_TEST=${CMAKE_SOURCE_DIR}"
            "-DTEST_WORK_DIR=${CMAKE_BINARY_DIR}/cmake_test_work/fetch_gltf_sample_models_config_tests"
            -P "${TEST_CMAKE_DIR}/fetch_gltf_sample_models_config_tests.cmake"
)

add_test(
    NAME fetch_buildingsmart_sample_test_files_tests
    COMMAND ${CMAKE_COMMAND}
            "-DPROJECT_SOURCE_DIR_FOR_TEST=${CMAKE_SOURCE_DIR}"
            "-DTEST_WORK_DIR=${CMAKE_BINARY_DIR}/cmake_test_work/fetch_buildingsmart_sample_test_files_tests"
            -P "${TEST_CMAKE_DIR}/fetch_buildingsmart_sample_test_files_tests.cmake"
)

add_test(
    NAME shaders_incremental_config_tests
    COMMAND ${CMAKE_COMMAND}
            "-DPROJECT_SOURCE_DIR_FOR_TEST=${CMAKE_SOURCE_DIR}"
            "-DTEST_WORK_DIR=${CMAKE_BINARY_DIR}/cmake_test_work/shaders_incremental_config_tests"
            "-DTEST_CMAKE_GENERATOR=${CMAKE_GENERATOR}"
            "-DTEST_CMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
            -P "${TEST_CMAKE_DIR}/shaders_incremental_config_tests.cmake"
)

# ── Helper function ──────────────────────────────────────────────────────────
# add_custom_test(TARGET_NAME SOURCE_FILE SHADER_DIR TEST_RESULTS_DIR [deps...])
#
# Creates a GTest executable with consistent properties.  Dependencies are
# passed as additional arguments after TEST_RESULTS_DIR.
# If SHADER_DIR is empty (""), the post-build shader copy step is skipped.
function(add_custom_test TARGET_NAME SOURCE_FILE SHADER_DIR TEST_RESULTS_DIR)
    set(extra_deps ${ARGN})

    add_executable(${TARGET_NAME} ${SOURCE_FILE})

    target_link_libraries(${TARGET_NAME} PRIVATE
        ${extra_deps}
        GTest::gtest
        GTest::gtest_main
    )

    target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )

    set_target_properties(${TARGET_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/tests
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/tests
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/tests
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}/tests
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    add_test(
        NAME ${TARGET_NAME}
        COMMAND ${TARGET_NAME} --gtest_output=xml:${TEST_RESULTS_DIR}/${TARGET_NAME}.xml
    )

    # Copy shaders only when a shader directory is provided.
    if(NOT "${SHADER_DIR}" STREQUAL "")
        add_custom_command(
            TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${SHADER_DIR}
                $<TARGET_FILE_DIR:${TARGET_NAME}>/shaders
            COMMENT "Copying shaders to output directory for ${TARGET_NAME}"
        )
    endif()
endfunction()

# ── CPU-only tests (no shaders needed) ───────────────────────────────────────

add_custom_test(glm_tests
    ${TEST_CORE_DIR}/glm_tests.cpp  ""  ${TEST_RESULTS_DIR}
    Dep_Math
)

add_custom_test(ecs_tests
    ${TEST_CORE_DIR}/ecs_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_ecs  VulkanSceneRenderer_scene
)

add_custom_test(scene_graph_tests
    ${TEST_SCENE_DIR}/scene_graph_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)

add_custom_test(input_manager_tests
    ${TEST_PLATFORM_DIR}/input_manager_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_window
)

add_custom_test(gltf_loader_tests
    ${TEST_GEOMETRY_DIR}/gltf_loader_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)

add_custom_test(dotbim_loader_tests
    ${TEST_GEOMETRY_DIR}/dotbim_loader_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)

add_custom_test(ifc_tessellated_loader_tests
    ${TEST_GEOMETRY_DIR}/ifc_tessellated_loader_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)

add_custom_test(ifcx_loader_tests
    ${TEST_GEOMETRY_DIR}/ifcx_loader_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)
target_compile_definitions(ifcx_loader_tests PRIVATE
    CONTAINER_BINARY_DIR="${CMAKE_BINARY_DIR}"
)
add_dependencies(ifcx_loader_tests generate_models)

add_custom_test(usd_loader_tests
    ${TEST_GEOMETRY_DIR}/usd_loader_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)
target_compile_definitions(usd_loader_tests PRIVATE
    CONTAINER_BINARY_DIR="${CMAKE_BINARY_DIR}"
)
add_dependencies(usd_loader_tests generate_models)

add_custom_test(sample_model_regression_tests
    ${TEST_GEOMETRY_DIR}/sample_model_regression_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
    nlohmann_json::nlohmann_json
)
target_compile_definitions(sample_model_regression_tests PRIVATE
    CONTAINER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    CONTAINER_BINARY_DIR="${CMAKE_BINARY_DIR}"
)
add_dependencies(sample_model_regression_tests generate_models)

add_custom_test(materialx_integration_tests
    ${TEST_GEOMETRY_DIR}/materialx_integration_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)

add_custom_test(bcf_viewpoint_tests
    ${TEST_UI_DIR}/bcf_viewpoint_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(rendering_convention_tests
    ${TEST_VALIDATION_DIR}/rendering_convention_tests.cpp  ""  ${TEST_RESULTS_DIR}
    Dep_Math
)
target_sources(rendering_convention_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/bim/BimSectionCapBuilder.cpp
)

add_custom_test(render_technique_registry_tests
    ${TEST_RENDERER_CORE_DIR}/render_technique_registry_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
target_compile_definitions(render_technique_registry_tests PRIVATE
    CONTAINER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_custom_test(render_pass_scope_recorder_tests
    ${TEST_RENDERER_CORE_DIR}/render_pass_scope_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(command_buffer_scope_recorder_tests
    ${TEST_RENDERER_CORE_DIR}/command_buffer_scope_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_raster_post_process_tests
    ${TEST_RENDERER_DEFERRED_DIR}/deferred_raster_post_process_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(dynamic_rendering_tests
    ${TEST_RENDERER_CORE_DIR}/dynamic_rendering_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_raster_lighting_tests
    ${TEST_RENDERER_DEFERRED_DIR}/deferred_raster_lighting_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_lighting_pass_planner_tests
    ${TEST_RENDERER_DEFERRED_DIR}/deferred_lighting_pass_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_raster_frustum_cull_pass_planner_tests
    ${TEST_RENDERER_DEFERRED_DIR}/deferred_raster_frustum_cull_pass_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_raster_frustum_cull_pass_recorder_tests
    ${TEST_RENDERER_DEFERRED_DIR}/deferred_raster_frustum_cull_pass_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(gpu_cull_draw_upload_planner_tests
    ${TEST_RENDERER_CULLING_DIR}/gpu_cull_draw_upload_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
)

add_custom_test(deferred_raster_scene_gpu_cull_route_planner_tests
    ${TEST_RENDERER_DEFERRED_DIR}/deferred_raster_scene_gpu_cull_route_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_raster_gui_pass_recorder_tests
    ${TEST_RENDERER_DEFERRED_DIR}/deferred_raster_gui_pass_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_raster_depth_read_only_transition_recorder_tests
    ${TEST_RENDERER_DEFERRED_DIR}/deferred_raster_depth_read_only_transition_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_raster_hiz_depth_transition_recorder_tests
    ${TEST_RENDERER_DEFERRED_DIR}/deferred_raster_hiz_depth_transition_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_raster_hiz_pass_planner_tests
    ${TEST_RENDERER_DEFERRED_DIR}/deferred_raster_hiz_pass_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_raster_tile_cull_planner_tests
    ${TEST_RENDERER_DEFERRED_DIR}/deferred_raster_tile_cull_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_raster_tile_cull_recorder_tests
    ${TEST_RENDERER_DEFERRED_DIR}/deferred_raster_tile_cull_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_raster_scene_color_read_barrier_recorder_tests
    ${TEST_RENDERER_DEFERRED_DIR}/deferred_raster_scene_color_read_barrier_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_lighting_descriptor_planner_tests
    ${TEST_RENDERER_DEFERRED_DIR}/deferred_lighting_descriptor_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_directional_lighting_recorder_tests
    ${TEST_RENDERER_DEFERRED_DIR}/deferred_directional_lighting_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_light_gizmo_planner_tests
    ${TEST_RENDERER_DEFERRED_DIR}/deferred_light_gizmo_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(light_gizmo_icon_atlas_tests
    ${TEST_RENDERER_DEFERRED_DIR}/light_gizmo_icon_atlas_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_light_gizmo_recorder_tests
    ${TEST_RENDERER_DEFERRED_DIR}/deferred_light_gizmo_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(editable_light_tests
    ${TEST_RENDERER_DEFERRED_DIR}/editable_light_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_transparent_oit_recorder_tests
    ${TEST_RENDERER_DEFERRED_DIR}/deferred_transparent_oit_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_transparent_oit_frame_pass_recorder_tests
    ${TEST_RENDERER_DEFERRED_DIR}/deferred_transparent_oit_frame_pass_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_point_lighting_draw_planner_tests
    ${TEST_RENDERER_DEFERRED_DIR}/deferred_point_lighting_draw_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_transform_gizmo_tests
    ${TEST_RENDERER_DEFERRED_DIR}/deferred_transform_gizmo_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(screenshot_capture_recorder_tests
    ${TEST_RENDERER_CORE_DIR}/screenshot_capture_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(scene_viewport_tests
    ${TEST_RENDERER_CORE_DIR}/scene_viewport_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(scene_opaque_draw_planner_tests
    ${TEST_RENDERER_SCENE_DIR}/scene_opaque_draw_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(scene_opaque_draw_recorder_tests
    ${TEST_RENDERER_SCENE_DIR}/scene_opaque_draw_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(scene_raster_pass_planner_tests
    ${TEST_RENDERER_SCENE_DIR}/scene_raster_pass_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(scene_raster_pass_recorder_tests
    ${TEST_RENDERER_SCENE_DIR}/scene_raster_pass_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(scene_diagnostic_cube_recorder_tests
    ${TEST_RENDERER_SCENE_DIR}/scene_diagnostic_cube_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(scene_primitives_tests
    ${TEST_RENDERER_SCENE_DIR}/scene_primitives_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(scene_transparent_draw_planner_tests
    ${TEST_RENDERER_SCENE_DIR}/scene_transparent_draw_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(scene_transparent_draw_recorder_tests
    ${TEST_RENDERER_SCENE_DIR}/scene_transparent_draw_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(transparent_pick_pass_recorder_tests
    ${TEST_RENDERER_PICKING_DIR}/transparent_pick_pass_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(transparent_pick_depth_copy_recorder_tests
    ${TEST_RENDERER_PICKING_DIR}/transparent_pick_depth_copy_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(transparent_pick_raster_pass_recorder_tests
    ${TEST_RENDERER_PICKING_DIR}/transparent_pick_raster_pass_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_raster_debug_overlay_planner_tests
    ${TEST_RENDERER_DEFERRED_DIR}/deferred_raster_debug_overlay_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(deferred_raster_debug_overlay_recorder_tests
    ${TEST_RENDERER_DEFERRED_DIR}/deferred_raster_debug_overlay_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(forward_raster_technique_tests
    ${TEST_RENDERER_FORWARD_DIR}/forward_raster_technique_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
target_compile_definitions(forward_raster_technique_tests PRIVATE
    CONTAINER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_custom_test(forward_raster_lighting_pass_recorder_tests
    ${TEST_RENDERER_FORWARD_DIR}/forward_raster_lighting_pass_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
target_compile_definitions(forward_raster_lighting_pass_recorder_tests PRIVATE
    CONTAINER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_custom_test(bim_draw_compaction_planner_tests
    ${TEST_RENDERER_BIM_DIR}/bim_draw_compaction_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_draw_filter_state_tests
    ${TEST_RENDERER_BIM_DIR}/bim_draw_filter_state_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_drawing_export_tests
    ${TEST_RENDERER_BIM_DIR}/bim_drawing_export_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_relationship_graph_tests
    ${TEST_RENDERER_BIM_DIR}/bim_relationship_graph_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_measurement_snapping_tests
    ${TEST_RENDERER_BIM_DIR}/bim_measurement_snapping_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_schedule_extractor_tests
    ${TEST_RENDERER_BIM_DIR}/bim_schedule_extractor_tests.cpp  ""  ${TEST_RESULTS_DIR}
    Dep_Math
)
target_sources(bim_schedule_extractor_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/bim/BimScheduleExtractor.cpp
)

add_custom_test(bim_model_compare_tests
    ${TEST_RENDERER_BIM_DIR}/bim_model_compare_tests.cpp  ""  ${TEST_RESULTS_DIR}
    Dep_Math
)
target_sources(bim_model_compare_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/bim/BimModelCompare.cpp
)

add_custom_test(bim_georeference_transform_tests
    ${TEST_RENDERER_BIM_DIR}/bim_georeference_transform_tests.cpp  ""  ${TEST_RESULTS_DIR}
    Dep_Math
)
target_sources(bim_georeference_transform_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/bim/BimGeoreferenceTransform.cpp
)

add_custom_test(bim_coordination_overlay_tests
    ${TEST_RENDERER_BIM_DIR}/bim_coordination_overlay_tests.cpp  ""  ${TEST_RESULTS_DIR}
    Dep_Math
)
target_sources(bim_coordination_overlay_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/bim/BimCoordinationOverlay.cpp
)

add_custom_test(bim_frame_draw_routing_planner_tests
    ${TEST_RENDERER_BIM_DIR}/bim_frame_draw_routing_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_lighting_overlay_planner_tests
    ${TEST_RENDERER_BIM_DIR}/bim_lighting_overlay_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_lighting_overlay_recorder_tests
    ${TEST_RENDERER_BIM_DIR}/bim_lighting_overlay_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_frame_gpu_visibility_recorder_tests
    ${TEST_RENDERER_BIM_DIR}/bim_frame_gpu_visibility_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_metadata_catalog_tests
    ${TEST_RENDERER_BIM_DIR}/bim_metadata_catalog_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_metadata_index_tests
    ${TEST_RENDERER_BIM_DIR}/bim_metadata_index_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_primitive_pass_planner_tests
    ${TEST_RENDERER_BIM_DIR}/bim_primitive_pass_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_section_clip_cap_pass_planner_tests
    ${TEST_RENDERER_BIM_DIR}/bim_section_clip_cap_pass_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_surface_draw_routing_planner_tests
    ${TEST_RENDERER_BIM_DIR}/bim_surface_draw_routing_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_surface_pass_planner_tests
    ${TEST_RENDERER_BIM_DIR}/bim_surface_pass_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_surface_pass_recorder_tests
    ${TEST_RENDERER_BIM_DIR}/bim_surface_pass_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_surface_raster_pass_recorder_tests
    ${TEST_RENDERER_BIM_DIR}/bim_surface_raster_pass_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(lighting_shadow_sampling_tests
    ${TEST_RENDERER_LIGHTING_DIR}/lighting_shadow_sampling_tests.cpp "" ${TEST_RESULTS_DIR}
    Dep_Math
)
target_compile_definitions(lighting_shadow_sampling_tests PRIVATE
    CONTAINER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_custom_test(local_shadow_layer_allocator_tests
    ${TEST_RENDERER_LIGHTING_DIR}/local_shadow_layer_allocator_tests.cpp  ""  ${TEST_RESULTS_DIR}
)

add_custom_test(shadow_cascade_draw_planner_tests
    ${TEST_RENDERER_SHADOW_DIR}/shadow_cascade_draw_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(shadow_cascade_depth_planner_tests
    ${TEST_RENDERER_SHADOW_DIR}/shadow_cascade_depth_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(shadow_cascade_frame_pass_recorder_tests
    ${TEST_RENDERER_SHADOW_DIR}/shadow_cascade_frame_pass_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(shadow_cascade_gpu_cull_planner_tests
    ${TEST_RENDERER_SHADOW_DIR}/shadow_cascade_gpu_cull_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(shadow_cascade_preparation_planner_tests
    ${TEST_RENDERER_SHADOW_DIR}/shadow_cascade_preparation_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(shadow_cull_pass_planner_tests
    ${TEST_RENDERER_SHADOW_DIR}/shadow_cull_pass_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(shadow_cull_pass_recorder_tests
    ${TEST_RENDERER_SHADOW_DIR}/shadow_cull_pass_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(shadow_pass_draw_planner_tests
    ${TEST_RENDERER_SHADOW_DIR}/shadow_pass_draw_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(shadow_pass_scope_planner_tests
    ${TEST_RENDERER_SHADOW_DIR}/shadow_pass_scope_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(shadow_pass_raster_planner_tests
    ${TEST_RENDERER_SHADOW_DIR}/shadow_pass_raster_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(shadow_pass_raster_recorder_tests
    ${TEST_RENDERER_SHADOW_DIR}/shadow_pass_raster_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(shadow_cascade_secondary_command_buffer_recorder_tests
    ${TEST_RENDERER_SHADOW_DIR}/shadow_cascade_secondary_command_buffer_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(shadow_secondary_command_buffer_planner_tests
    ${TEST_RENDERER_SHADOW_DIR}/shadow_secondary_command_buffer_planner_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(shadow_pass_recorder_tests
    ${TEST_RENDERER_SHADOW_DIR}/shadow_pass_recorder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(resource_pipeline_registry_tests
    ${TEST_RENDERER_CORE_DIR}/resource_pipeline_registry_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
target_compile_definitions(resource_pipeline_registry_tests PRIVATE
    CONTAINER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_custom_test(scene_provider_extraction_tests
    ${TEST_RENDERER_CORE_DIR}/scene_provider_extraction_tests.cpp  ""  ${TEST_RESULTS_DIR}
    Dep_Math
)
target_compile_definitions(scene_provider_extraction_tests PRIVATE
    CONTAINER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_custom_test(technique_debug_model_tests
    ${TEST_RENDERER_CORE_DIR}/technique_debug_model_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
target_compile_definitions(technique_debug_model_tests PRIVATE
    CONTAINER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_custom_test(bim_section_cap_tests
    ${TEST_RENDERER_BIM_DIR}/bim_section_cap_tests.cpp  ""  ${TEST_RESULTS_DIR}
    Dep_Math
)
target_sources(bim_section_cap_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/bim/BimSectionCapBuilder.cpp
)

add_custom_test(bim_section_cap_builder_tests
    ${TEST_RENDERER_BIM_DIR}/bim_section_cap_builder_tests.cpp  ""  ${TEST_RESULTS_DIR}
    Dep_Math
)
target_sources(bim_section_cap_builder_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/bim/BimSectionCapBuilder.cpp
)

add_custom_test(realistic_rendering_validation_tests
    ${TEST_VALIDATION_DIR}/realistic_rendering_validation_tests.cpp  ""  ${TEST_RESULTS_DIR}
    nlohmann_json::nlohmann_json
)

add_custom_test(visual_regression_gpu_tests
    ${TEST_VALIDATION_DIR}/visual_regression_gpu_tests.cpp  ""  ${TEST_RESULTS_DIR}
    nlohmann_json::nlohmann_json
    VulkanSceneRenderer_geometry
)
target_compile_definitions(visual_regression_gpu_tests PRIVATE
    CONTAINER_APP_EXECUTABLE="$<TARGET_FILE:VulkanSceneRenderer>"
    CONTAINER_TEST_RESULTS_DIR="${TEST_RESULTS_DIR}"
)
add_dependencies(visual_regression_gpu_tests VulkanSceneRenderer)
set_tests_properties(visual_regression_gpu_tests
    PROPERTIES LABELS "requires-vulkan;requires-display;visual-regression")

add_custom_test(render_graph_tests
    ${TEST_RENDERER_CORE_DIR}/render_graph_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(renderer_telemetry_tests
    ${TEST_RENDERER_CORE_DIR}/renderer_telemetry_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

# ── Tests that need Vulkan / windowing runtime ───────────────────────────────

if(ENABLE_WINDOWED_TESTS)
    add_custom_test(triangle_test
        ${TEST_PLATFORM_DIR}/triangle_test.cpp  ${DEFAULT_SHADER_DIR}  ${TEST_RESULTS_DIR}
        Dep_Windowing
    )

    add_custom_test(vulkan_tests
        ${TEST_PLATFORM_DIR}/vulkan_tests.cpp  ${DEFAULT_SHADER_DIR}  ${TEST_RESULTS_DIR}
        Dep_VulkanCore
    )

    add_custom_test(window_creation_test
        ${TEST_PLATFORM_DIR}/window_creation_test.cpp  ${DEFAULT_SHADER_DIR}  ${TEST_RESULTS_DIR}
        Dep_Windowing
    )

    set_tests_properties(triangle_test vulkan_tests window_creation_test
        PROPERTIES LABELS "requires-vulkan;requires-display")
endif()
