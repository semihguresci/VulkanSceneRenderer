enable_testing()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME openvdb_python_bridge_tests COMMAND ${CMAKE_COMMAND} -E env
        "CONTAINER_OPENVDB_TOOL=$<TARGET_FILE:OpenVdbTool>" "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/python/test_openvdb_bridge.py")
    set_tests_properties(openvdb_python_bridge_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    set(_vdb_image_cli_availability)
    if(NOT ENABLE_OPENVDB)
        set(_vdb_image_cli_availability --expect-disabled)
    endif()
    add_test(NAME openvdb_transport_image_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_transport_image_cli_tests.py"
        --exe "$<TARGET_FILE:OpenVdbTransportRender>" --output-root "${CMAKE_BINARY_DIR}/openvdb-image-cli"
        ${_vdb_image_cli_availability})
    set_tests_properties(openvdb_transport_image_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_tool_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_tool_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_tool_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_transport_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_transport_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_transport_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_interchange_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_interchange_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_interchange_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_fields_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_fields_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_fields_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_point_flow_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_point_flow_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_point_flow_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_composite_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_composite_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_composite_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_topology_sampling_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_topology_sampling_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_topology_sampling_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_sweeping_spheres_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_sweeping_spheres_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_sweeping_spheres_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_regions_surface_fracture_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_regions_surface_fracture_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_regions_surface_fracture_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_field_advection_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_field_advection_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_field_advection_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_potential_flow_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_potential_flow_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_potential_flow_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_point_neighbors_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_point_neighbors_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_point_neighbors_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_level_set_rebuild_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_level_set_rebuild_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_level_set_rebuild_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_surface_fields_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_surface_fields_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_surface_fields_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_point_motion_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_point_motion_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_point_motion_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_point_transfer_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_point_transfer_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_point_transfer_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_field_resampling_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_field_resampling_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_field_resampling_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_diagnostics_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_diagnostics_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_diagnostics_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_boundary_fluid_step_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_boundary_fluid_step_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_boundary_fluid_step_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 240)
    add_test(NAME openvdb_fog_ray_integrals_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_fog_ray_integrals_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_fog_ray_integrals_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_boundary_projection_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_boundary_projection_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_boundary_projection_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_particle_geometry_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_particle_geometry_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_particle_geometry_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_surface_reference_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_surface_reference_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_surface_reference_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_value_components_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_value_components_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --generator "$<TARGET_FILE:OpenVdbQueryFixtureGenerator>"
        --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_value_components_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_region_queries_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_region_queries_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --generator "$<TARGET_FILE:OpenVdbQueryFixtureGenerator>"
        --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_region_queries_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_ray_queries_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_ray_queries_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --generator "$<TARGET_FILE:OpenVdbQueryFixtureGenerator>"
        --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_ray_queries_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_native_point_advection_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_native_point_advection_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>")
    set_tests_properties(openvdb_native_point_advection_cli_tests PROPERTIES LABELS "openvdb;cpu;python" TIMEOUT 180)
    add_test(NAME openvdb_nano_primitives_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_nano_primitives_cli_tests.py"
        --tool "$<TARGET_FILE:OpenVdbTool>" --enabled "$<BOOL:${ENABLE_OPENVDB}>"
        --native-enabled "$<BOOL:${CONTAINER_HAS_NANOVDB_IO}>")
    set_tests_properties(openvdb_nano_primitives_cli_tests PROPERTIES LABELS "openvdb;cpu;python;nanovdb" TIMEOUT 180)
    add_test(NAME openvdb_nano_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_nano_cli_tests.py"
        --exe "$<TARGET_FILE:OpenVdbTool>" --generator "$<TARGET_FILE:nanovdb_fixture_generator>"
        --enabled "$<BOOL:${ENABLE_OPENVDB}>" --native-enabled "$<BOOL:${CONTAINER_HAS_NANOVDB_IO}>")
    set_tests_properties(openvdb_nano_cli_tests PROPERTIES LABELS "openvdb;cpu;python;nanovdb" TIMEOUT 180)
    add_test(NAME openvdb_preview_cli_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_preview_cli_tests.py"
        --exe "$<TARGET_FILE:VulkanSceneRenderer>")
    set_tests_properties(openvdb_preview_cli_tests PROPERTIES
        LABELS "openvdb;cpu;python;requires-renderer" TIMEOUT 120)
    if(ENABLE_OPENVDB AND ENABLE_WINDOWED_TESTS)
        set(_vdb_native_regression_args)
        if(CONTAINER_HAS_NANOVDB)
            list(APPEND _vdb_native_regression_args --native-sparse)
        endif()
        add_test(NAME openvdb_runtime_regression COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_runtime_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>"
            --tool "$<TARGET_FILE:OpenVdbTool>"
            --output-dir "${CMAKE_BINARY_DIR}/openvdb-runtime" --surface-smoke ${_vdb_native_regression_args})
        set_tests_properties(openvdb_runtime_regression PROPERTIES
            LABELS "requires-vulkan;requires-display;openvdb" RUN_SERIAL TRUE
            TIMEOUT 1020 SKIP_RETURN_CODE 77)
        add_test(NAME openvdb_transport_runtime_regression COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_transport_runtime_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>"
            --tool "$<TARGET_FILE:OpenVdbTool>"
            --output-dir "${CMAKE_BINARY_DIR}/openvdb-transport-runtime" ${_vdb_native_regression_args})
        set_tests_properties(openvdb_transport_runtime_regression PROPERTIES
            LABELS "requires-vulkan;requires-display;openvdb" RUN_SERIAL TRUE
            TIMEOUT 420 SKIP_RETURN_CODE 77)
        add_test(NAME openvdb_interchange_runtime_regression COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_interchange_runtime_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>"
            --tool "$<TARGET_FILE:OpenVdbTool>"
            --output-dir "${CMAKE_BINARY_DIR}/openvdb-interchange-runtime")
        set_tests_properties(openvdb_interchange_runtime_regression PROPERTIES
            LABELS "requires-vulkan;requires-display;openvdb" RUN_SERIAL TRUE
            TIMEOUT 240 SKIP_RETURN_CODE 77)
        add_test(NAME openvdb_points_runtime_regression COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_points_runtime_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>"
            --tool "$<TARGET_FILE:OpenVdbTool>"
            --output-dir "${CMAKE_BINARY_DIR}/openvdb-points-runtime" --simulation-parity)
        set_tests_properties(openvdb_points_runtime_regression PROPERTIES
            LABELS "requires-vulkan;requires-display;openvdb" RUN_SERIAL TRUE
            TIMEOUT 540 SKIP_RETURN_CODE 77)
        add_test(NAME openvdb_vector_sequence_runtime_regression COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_vector_sequence_runtime_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>" --tool "$<TARGET_FILE:OpenVdbTool>"
            --output-dir "${CMAKE_BINARY_DIR}/openvdb-vector-sequence-runtime")
        set_tests_properties(openvdb_vector_sequence_runtime_regression PROPERTIES
            LABELS "requires-vulkan;requires-display;openvdb" RUN_SERIAL TRUE
            TIMEOUT 480 SKIP_RETURN_CODE 77)
        add_test(NAME openvdb_extended_preview_runtime_regression COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_extended_preview_runtime_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>" --tool "$<TARGET_FILE:OpenVdbTool>"
            --output-dir "${CMAKE_BINARY_DIR}/openvdb-extended-preview-runtime")
        set_tests_properties(openvdb_extended_preview_runtime_regression PROPERTIES
            LABELS "requires-vulkan;requires-display;openvdb" RUN_SERIAL TRUE
            TIMEOUT 360 SKIP_RETURN_CODE 77)
        add_test(NAME openvdb_discrete_preview_runtime_regression COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/validation/openvdb_discrete_preview_runtime_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>" --generator "$<TARGET_FILE:OpenVdbDiscretePreviewFixture>"
            --output-dir "${CMAKE_BINARY_DIR}/openvdb-discrete-preview-runtime")
        set_tests_properties(openvdb_discrete_preview_runtime_regression PROPERTIES
            LABELS "requires-vulkan;requires-display;openvdb" RUN_SERIAL TRUE
            TIMEOUT 150 SKIP_RETURN_CODE 77)
    endif()
    add_test(NAME physics_runtime_validation_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/test_physics_runtime_validation.py")
    set_tests_properties(physics_runtime_validation_tests PROPERTIES
        LABELS "physics;cpu;python" TIMEOUT 60 SKIP_RETURN_CODE 77)
    add_test(NAME physics_auxiliary_validation_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/test_physics_auxiliary_validation.py")
    set_tests_properties(physics_auxiliary_validation_tests PROPERTIES
        LABELS "physics;cpu;python" TIMEOUT 60 SKIP_RETURN_CODE 77)
    add_test(NAME physics_joint_validation_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/test_physics_joint_validation.py")
    set_tests_properties(physics_joint_validation_tests PROPERTIES
        LABELS "physics;joints;cpu;python" TIMEOUT 60 SKIP_RETURN_CODE 77)
    add_test(NAME physics_scene_diversity_validation_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/test_physics_scene_diversity_validation.py")
    set_tests_properties(physics_scene_diversity_validation_tests PROPERTIES
        LABELS "physics;cpu;python" TIMEOUT 60 SKIP_RETURN_CODE 77)
    add_test(NAME physics_moving_support_validation_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/test_physics_moving_support_validation.py")
    set_tests_properties(physics_moving_support_validation_tests PROPERTIES
        LABELS "physics;cpu;python" TIMEOUT 60 SKIP_RETURN_CODE 77)
    add_test(NAME physics_scale_validation_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/test_physics_scale_validation.py")
    set_tests_properties(physics_scale_validation_tests PROPERTIES
        LABELS "physics;cpu;python" TIMEOUT 60 SKIP_RETURN_CODE 77)
    add_test(NAME physics_pending_edit_validation_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/test_physics_pending_edit_validation.py")
    set_tests_properties(physics_pending_edit_validation_tests PROPERTIES
        LABELS "physics;cpu;python" TIMEOUT 60 SKIP_RETURN_CODE 77)
    add_test(NAME scene_browser_scale_runner_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/test_scene_browser_scale.py")
    add_test(NAME gfx_capture_launcher_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/gfx_capture_launcher_tests.py")
    add_test(NAME gfx_capture_replay_smoke COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/validation/gfx_capture_replay_smoke.py"
        --exe "$<TARGET_FILE:VulkanSceneRenderer>"
        --output "${CMAKE_BINARY_DIR}/test_results/gfxreconstruct")
    set_tests_properties(gfx_capture_replay_smoke PROPERTIES
        LABELS "requires-vulkan;requires-display;gfxreconstruct" SKIP_RETURN_CODE 77)
endif()
find_package(GTest 1.18.0 EXACT CONFIG REQUIRED)
container_core_include(GTest::gtest gtest/gtest.h CONTAINER_GTEST_INCLUDE_DIR)
set(_container_googletest_pins
    "gtest/gtest-assertion-result.h|c6264d766e880c30452da03c1cf783230f7152233612f13cebff6177f6527cec"
    "gtest/gtest-death-test.h|d70139e593088f79c9f25ba528ce212fab3cdf9c350fb3cceaa91e1e94d6af00"
    "gtest/gtest-matchers.h|7116aca6c9c224d8015bf74866e21994231f972a1af3bfceb6dc88e13e48690f"
    "gtest/gtest-message.h|26f2e9ea9dc21b63eb78d4d3eed423ed00f28bdc6fccc735cff94da0e9c8c55f"
    "gtest/gtest-param-test.h|7a246e017cb84010ae4bad5b6f789fb16941b4e46a130ffb8de9f328fa19ee78"
    "gtest/gtest-printers.h|69428a5fa909fd7f8f3bfe1d4f1a8be9a260375bceab738b8813fa4c2f5c9ff4"
    "gtest/gtest-spi.h|490f165098688c18d053a4ba57b2812bd97535b90af514f5a916116a230374ef"
    "gtest/gtest-test-part.h|2ba7bcb638f82889a619d52a5e28d4f9dc0aefd718a590beabd35cce8c27550a"
    "gtest/gtest-typed-test.h|d5c7a2853a4f250cd172f7d4218298f6aa9aa2df219210231e4877cd91bc7790"
    "gtest/gtest.h|c6f886e33c2e426d6f634112b12b0041aa0bad85b9f897be853b083c14aea169"
    "gtest/gtest_pred_impl.h|85e850e735a42ebb5f9a01a018708c1c0b9b7d83c94b14b7d5d29512c00b6ffd"
    "gtest/gtest_prod.h|3c6301ef533db8686e474dd5a0de4d98a31cbb630e2273f712472d1be25aa261"
    "gtest/internal/custom/gtest-port.h|5e06747c9e8b86e9d841fabd61c2f045b2bb1133716f127be4a963ced1c44e01"
    "gtest/internal/custom/gtest-printers.h|b95afda2426e8c4f09c11d3296d522fb336b248c0da7e078388db4bb072c2166"
    "gtest/internal/custom/gtest.h|21d20a1c64ea38c20c4b12337c261a1a18989d2a30710a83dfb5cd9a3bb3c55e"
    "gtest/internal/custom/README.md|608ec5ac574a6e64e4fc2f4bce2cfc7ff753a04d6822d6ff96e43fff51c6ccc1"
    "gtest/internal/gtest-death-test-internal.h|5a81ad35daf1049940c677b38c908945040cc25f07366b3ddf577ce72d8c44f1"
    "gtest/internal/gtest-filepath.h|584be91365316bf97fa7e3fd9f21f595919c48691f0c87071d2b7f739aaabcc0"
    "gtest/internal/gtest-internal.h|57b904e1b1763a9ac5f381db3717a92e285e707adf093d0f17c5cfc9603a6f34"
    "gtest/internal/gtest-param-util.h|869ec29efd36f14b75446b06de5bd58491da2b77575d092e67cab3583bb10e7a"
    "gtest/internal/gtest-port-arch.h|58ca63b3540d7752466cd34b6c87f9fb5605f5aa620fdabe0179e82f0c12e556"
    "gtest/internal/gtest-port.h|19d5e5bfcba9fb952e46352218d626f6e1778ed5f6186cc6770bcfc7d8a672d0"
    "gtest/internal/gtest-string.h|794b59f7afd2fbfb9fdfd133244903b4904e39d16d45253a85f1b89c196285ac"
    "gtest/internal/gtest-type-util.h|94336b1703807db6d9d2b729b5a30cf53016fd236b14774af118467d27f94ad8"
)
foreach(_pin IN LISTS _container_googletest_pins)
    string(REPLACE "|" ";" _pair "${_pin}")
    list(GET _pair 0 _relative)
    list(GET _pair 1 _expected)
    container_check_core_file("${CONTAINER_GTEST_INCLUDE_DIR}/${_relative}" "${_expected}")
endforeach()
container_stage_selected_cpp_notice("${CONTAINER_GTEST_INCLUDE_DIR}" gtest GTest-1.18.0-LICENSE.txt
    "9702de7e4117a8e2b20dafab11ffda58c198aede066406496bef670d40a22138")
set(CONTAINER_GTEST_VERSION "1.18.0" CACHE INTERNAL "Verified test-only GoogleTest release" FORCE)
file(SHA256 "${CMAKE_BINARY_DIR}/third_party_licenses/GTest-1.18.0-LICENSE.txt" _gtest_raw)
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/selected-test-dependencies.json"
    CONTENT "{\"libraries\":[{\"name\":\"gtest\",\"version\":\"1.18.0\",\"notice\":\"GTest-1.18.0-LICENSE.txt\",\"sha256\":\"${_gtest_raw}\"}]}\n" @ONLY)

find_package(Threads REQUIRED)

add_executable(asl_pose_replay_tests ${CMAKE_SOURCE_DIR}/tests/asl/pose_replay_tests.cpp)
target_link_libraries(asl_pose_replay_tests PRIVATE VulkanSceneRenderer_asl_pose GTest::gtest)
container_stage_runtime_dlls(asl_pose_replay_tests)
add_test(NAME asl_pose_replay_tests COMMAND asl_pose_replay_tests)

add_executable(hand_kinematics_tests ${CMAKE_SOURCE_DIR}/tests/asl/hand_kinematics_tests.cpp)
target_link_libraries(hand_kinematics_tests PRIVATE VulkanSceneRenderer_asl_pose GTest::gtest_main)
container_stage_runtime_dlls(hand_kinematics_tests)
add_test(NAME hand_kinematics_tests COMMAND hand_kinematics_tests)

set(TESTS_DIR "${CMAKE_SOURCE_DIR}/tests")
set(TEST_RESULTS_DIR "${CMAKE_BINARY_DIR}/test_results")
file(MAKE_DIRECTORY ${TEST_RESULTS_DIR})

if(Python3_Interpreter_FOUND AND ENABLE_PHYSICS)
    add_test(NAME physics_runtime_regression
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/validation/physics_runtime_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>"
            --output "${TEST_RESULTS_DIR}/physics-runtime-$<CONFIG>"
            --require-opt-in)
    set_tests_properties(physics_runtime_regression PROPERTIES
        LABELS "requires-vulkan;requires-display;requires-ray-query;physics"
        RUN_SERIAL TRUE TIMEOUT 1200 SKIP_RETURN_CODE 77)
    add_test(NAME physics_auxiliary_regression
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/validation/physics_auxiliary_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>"
            --output "${TEST_RESULTS_DIR}/physics-auxiliary-$<CONFIG>"
            --require-opt-in)
    set_tests_properties(physics_auxiliary_regression PROPERTIES
        LABELS "requires-vulkan;requires-display;requires-ray-query;physics"
        RUN_SERIAL TRUE TIMEOUT 1200 SKIP_RETURN_CODE 77)
    add_test(NAME physics_joint_regression
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/validation/physics_joint_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>"
            --output "${TEST_RESULTS_DIR}/physics-joints-$<CONFIG>"
            --include-ray-taa --require-opt-in)
    set_tests_properties(physics_joint_regression PROPERTIES
        LABELS "requires-vulkan;requires-display;requires-ray-query;physics;joints"
        RUN_SERIAL TRUE TIMEOUT 1200 SKIP_RETURN_CODE 77)
    add_test(NAME physics_scene_diversity_regression
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/validation/physics_scene_diversity_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>"
            --output "${TEST_RESULTS_DIR}/physics-scene-diversity-$<CONFIG>"
            --include-ray-taa --require-opt-in)
    set_tests_properties(physics_scene_diversity_regression PROPERTIES
        LABELS "requires-vulkan;requires-display;requires-ray-query;physics"
        RUN_SERIAL TRUE TIMEOUT 1200 SKIP_RETURN_CODE 77)
    add_test(NAME physics_moving_support_regression
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/validation/physics_moving_support_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>"
            --output "${TEST_RESULTS_DIR}/physics-moving-support-$<CONFIG>"
            --require-opt-in)
    set_tests_properties(physics_moving_support_regression PROPERTIES
        LABELS "requires-vulkan;requires-display;physics"
        RUN_SERIAL TRUE TIMEOUT 1200 SKIP_RETURN_CODE 77)
    add_test(NAME physics_scale_regression
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/validation/physics_scale_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>"
            --output "${TEST_RESULTS_DIR}/physics-scale-$<CONFIG>"
            --require-opt-in)
    set_tests_properties(physics_scale_regression PROPERTIES
        LABELS "requires-vulkan;requires-display;physics"
        RUN_SERIAL TRUE TIMEOUT 1200 SKIP_RETURN_CODE 77)
    add_test(NAME physics_pending_edit_regression
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/validation/physics_pending_edit_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>"
            --output "${TEST_RESULTS_DIR}/physics-pending-edit-$<CONFIG>"
            --require-opt-in)
    set_tests_properties(physics_pending_edit_regression PROPERTIES
        LABELS "requires-vulkan;requires-display;physics"
        RUN_SERIAL TRUE TIMEOUT 1200 SKIP_RETURN_CODE 77)
endif()

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

add_test(NAME openvdb_dependencies_config_tests
    COMMAND ${CMAKE_COMMAND}
        "-DPROJECT_SOURCE_DIR_FOR_TEST=${CMAKE_SOURCE_DIR}"
        "-DTEST_WORK_DIR=${CMAKE_BINARY_DIR}/cmake_test_work/openvdb_dependencies"
        "-DTEST_CMAKE_GENERATOR=${CMAKE_GENERATOR}"
        "-DTEST_CMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        -P "${TEST_CMAKE_DIR}/openvdb_dependencies_config_tests.cmake")
set_tests_properties(openvdb_dependencies_config_tests PROPERTIES LABELS "openvdb;cpu")
add_test(NAME openvdb_nanovdb_dependencies_config_tests
    COMMAND ${CMAKE_COMMAND}
        "-DPROJECT_SOURCE_DIR_FOR_TEST=${CMAKE_SOURCE_DIR}"
        "-DTEST_WORK_DIR=${CMAKE_BINARY_DIR}/cmake_test_work/nanovdb_dependencies"
        "-DTEST_CMAKE_GENERATOR=${CMAKE_GENERATOR}"
        "-DTEST_CMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        -P "${TEST_CMAKE_DIR}/nanovdb_dependencies_config_tests.cmake")
set_tests_properties(openvdb_nanovdb_dependencies_config_tests PROPERTIES LABELS "openvdb;cpu")
if(WIN32 AND ENABLE_OPENVDB)
    add_test(NAME openvdb_windows_package_runtime_tests
        COMMAND ${CMAKE_COMMAND}
            "-DPROJECT_SOURCE_DIR_FOR_TEST=${CMAKE_SOURCE_DIR}"
            "-DTEST_RUNTIME_DIRECTORY=$<TARGET_FILE_DIR:VulkanSceneRenderer>"
            "-DTEST_WORK_DIR=${CMAKE_BINARY_DIR}/cmake_test_work/openvdb_package_runtime"
            "-DTEST_DUMPBIN=${CONTAINER_OPENVDB_DUMPBIN}"
            -P "${TEST_CMAKE_DIR}/windows_package_runtime_tests.cmake")
    set_tests_properties(openvdb_windows_package_runtime_tests PROPERTIES
        LABELS "openvdb;cpu;packaging" TIMEOUT 120 RUN_SERIAL TRUE)
endif()

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

add_test(NAME vulkan_loader_runtime_config_tests
    COMMAND ${CMAKE_COMMAND}
        "-DPROJECT_SOURCE_DIR_FOR_TEST=${CMAKE_SOURCE_DIR}"
        "-DTEST_WORK_DIR=${CMAKE_BINARY_DIR}/cmake_test_work/vulkan_loader_runtime"
        "-DTEST_VULKAN_LOADER_ROOT=${CONTAINER_VULKAN_LOADER_ROOT}"
        "-DTEST_CMAKE_GENERATOR=${CMAKE_GENERATOR}"
        "-DTEST_CMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        -P "${TEST_CMAKE_DIR}/vulkan_loader_runtime_config_tests.cmake")
set_tests_properties(vulkan_loader_runtime_config_tests PROPERTIES LABELS "cpu;cmake;packaging" TIMEOUT 60)

add_test(
    NAME physics_dependencies_config_tests
    COMMAND ${CMAKE_COMMAND}
            "-DPROJECT_SOURCE_DIR_FOR_TEST=${CMAKE_SOURCE_DIR}"
            "-DTEST_WORK_DIR=${CMAKE_BINARY_DIR}/cmake_test_work/physics_dependencies_config_tests"
            "-DTEST_CMAKE_GENERATOR=${CMAKE_GENERATOR}"
            "-DTEST_CMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
            -P "${TEST_CMAKE_DIR}/physics_dependencies_config_tests.cmake"
)
set_tests_properties(physics_dependencies_config_tests PROPERTIES
    LABELS "physics;cpu;cmake" TIMEOUT 60)

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
    container_stage_runtime_dlls(${TARGET_NAME})

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

add_custom_test(async_model_preparation_tests
    ${CMAKE_SOURCE_DIR}/tests/utility/async_model_preparation_tests.cpp "" ${TEST_RESULTS_DIR}
    Threads::Threads)

# ── CPU-only tests (no shaders needed) ───────────────────────────────────────

add_custom_test(queue_family_selection_tests
    ${CMAKE_SOURCE_DIR}/tests/utility/queue_family_selection_tests.cpp "" ${TEST_RESULTS_DIR}
    Dep_VulkanCore)
set_tests_properties(queue_family_selection_tests PROPERTIES LABELS "cpu;vulkan")

add_test(NAME ddgi_python_regressions
    COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
            -s "${CMAKE_SOURCE_DIR}/tests/visual-regression/ddgi" -p "test_*.py"
)
set_tests_properties(ddgi_python_regressions PROPERTIES LABELS "ddgi;cpu")

add_custom_test(ddgi_probe_volume_tests
    ${TEST_RENDERER_LIGHTING_DIR}/ddgi_probe_volume_tests.cpp "" ${TEST_RESULTS_DIR}
    Dep_Math
)
target_sources(ddgi_probe_volume_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiProbeVolume.cpp
)

add_custom_test(ddgi_settings_tests
    ${TEST_RENDERER_LIGHTING_DIR}/ddgi_settings_tests.cpp "" ${TEST_RESULTS_DIR}
    Dep_Math
)
target_sources(ddgi_settings_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiProbeVolume.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiSettings.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiVolumeSet.cpp
)

add_custom_test(ddgi_volume_fit_tests
    ${TEST_RENDERER_LIGHTING_DIR}/ddgi_volume_fit_tests.cpp "" ${TEST_RESULTS_DIR}
    Dep_Math
)
target_sources(ddgi_volume_fit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiProbeVolume.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiSettings.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiVolumeSet.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiVolumeFit.cpp
)

add_custom_test(ddgi_scene_bounds_tests
    ${TEST_RENDERER_LIGHTING_DIR}/ddgi_scene_bounds_tests.cpp "" ${TEST_RESULTS_DIR}
    Dep_VulkanCore Dep_Math
)

add_custom_test(ddgi_volume_set_tests
    ${TEST_RENDERER_LIGHTING_DIR}/ddgi_volume_set_tests.cpp "" ${TEST_RESULTS_DIR}
    Dep_Math
)
target_sources(ddgi_volume_set_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiVolumeSet.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiProbeVolume.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiSettings.cpp
)
add_custom_test(ddgi_priority_scheduler_tests
    ${TEST_RENDERER_LIGHTING_DIR}/ddgi_priority_scheduler_tests.cpp "" ${TEST_RESULTS_DIR}
    Dep_Math
)
target_sources(ddgi_priority_scheduler_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiPriorityScheduler.cpp
)
add_custom_test(ddgi_preset_tests
    ${TEST_RENDERER_LIGHTING_DIR}/ddgi_preset_tests.cpp "" ${TEST_RESULTS_DIR}
    Dep_Math nlohmann_json::nlohmann_json
)

add_custom_test(ddgi_async_history_tests
    ${TEST_RENDERER_LIGHTING_DIR}/ddgi_async_history_tests.cpp "" ${TEST_RESULTS_DIR}
)
add_custom_test(ddgi_volume_placement_tests
    ${TEST_RENDERER_LIGHTING_DIR}/ddgi_volume_placement_tests.cpp "" ${TEST_RESULTS_DIR}
    Dep_Math
)
target_sources(ddgi_volume_placement_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiVolumePlacement.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiVolumeFit.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiVolumeSet.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiSettings.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiProbeVolume.cpp
)
add_custom_test(ddgi_volume_scroll_tests
    ${TEST_RENDERER_LIGHTING_DIR}/ddgi_volume_scroll_tests.cpp "" ${TEST_RESULTS_DIR}
    Dep_Math
)
target_sources(ddgi_volume_scroll_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiVolumeScroll.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiProbeVolume.cpp
)
target_sources(ddgi_preset_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiPreset.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiSettings.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiVolumeSet.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiProbeVolume.cpp
)
add_custom_test(ddgi_scene_changes_tests
    ${TEST_RENDERER_LIGHTING_DIR}/ddgi_scene_changes_tests.cpp "" ${TEST_RESULTS_DIR}
    Dep_VulkanCore Dep_Math
)
target_sources(ddgi_scene_changes_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiSceneChanges.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiSceneBounds.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/raytracing/RayScene.cpp
)
target_sources(ddgi_scene_bounds_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiSceneBounds.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/raytracing/RayScene.cpp
)

add_custom_test(ddgi_gpu_tests
    ${TEST_RENDERER_LIGHTING_DIR}/ddgi_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_gpu_resource VulkanSceneRenderer_geometry Dep_Windowing Dep_Math
)
target_compile_definitions(ddgi_gpu_tests PRIVATE
    CONTAINER_DDGI_UPDATE_SPIRV="${CMAKE_BINARY_DIR}/spv_shaders/ddgi_update.comp.spv"
)
add_dependencies(ddgi_gpu_tests shaders_all)
target_sources(ddgi_gpu_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiVolumeSet.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiSettings.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiProbeVolume.cpp
)
set_tests_properties(ddgi_gpu_tests PROPERTIES LABELS "requires-vulkan;requires-display;ddgi")

set(DDGI_LOOKUP_CONTRACT_SPIRV "${CMAKE_BINARY_DIR}/test_shaders/ddgi_lookup_contract.comp.spv")
add_custom_command(
    OUTPUT "${DDGI_LOOKUP_CONTRACT_SPIRV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/test_shaders"
    COMMAND "${SLANGC_EXECUTABLE}" "${DEFAULT_SHADER_DIR}/ddgi_lookup_contract.slang"
            ${SLANG_SPIRV_FLAGS} -entry computeMain -o "${DDGI_LOOKUP_CONTRACT_SPIRV}"
    DEPENDS "${DEFAULT_SHADER_DIR}/ddgi_lookup_contract.slang"
            ${SLANG_INCLUDE_SOURCES}
            "${CMAKE_CURRENT_LIST_FILE}" "${CMAKE_SOURCE_DIR}/cmake/Shaders.cmake"
            "${SLANGC_EXECUTABLE}"
    VERBATIM
)
add_custom_target(ddgi_lookup_contract_shader DEPENDS "${DDGI_LOOKUP_CONTRACT_SPIRV}")
add_custom_test(ddgi_lookup_gpu_tests
    ${TEST_RENDERER_LIGHTING_DIR}/ddgi_lookup_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_gpu_resource VulkanSceneRenderer_geometry Dep_Windowing Dep_Math
)
target_compile_definitions(ddgi_lookup_gpu_tests PRIVATE
    CONTAINER_DDGI_LOOKUP_SPIRV="${DDGI_LOOKUP_CONTRACT_SPIRV}"
)
add_dependencies(ddgi_lookup_gpu_tests ddgi_lookup_contract_shader)
target_sources(ddgi_lookup_gpu_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiVolumeSet.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiSettings.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/DdgiProbeVolume.cpp
)
set(DDGI_TRANSPORT_CONTRACT_SPIRV "${CMAKE_BINARY_DIR}/test_shaders/ddgi_transport_contract.comp.spv")
add_custom_command(
    OUTPUT "${DDGI_TRANSPORT_CONTRACT_SPIRV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/test_shaders"
    COMMAND "${SLANGC_EXECUTABLE}" "${DEFAULT_SHADER_DIR}/ddgi_transport_contract.slang"
            ${SLANG_SPIRV_FLAGS} -entry computeMain -o "${DDGI_TRANSPORT_CONTRACT_SPIRV}"
    DEPENDS "${DEFAULT_SHADER_DIR}/ddgi_transport_contract.slang"
            ${SLANG_INCLUDE_SOURCES} "${CMAKE_CURRENT_LIST_FILE}" "${SLANGC_EXECUTABLE}"
    VERBATIM
)
add_custom_target(ddgi_transport_contract_shader DEPENDS "${DDGI_TRANSPORT_CONTRACT_SPIRV}")
add_custom_test(ddgi_transport_gpu_tests
    ${TEST_RENDERER_LIGHTING_DIR}/ddgi_transport_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_gpu_resource VulkanSceneRenderer_geometry Dep_Windowing Dep_Math
)
target_compile_definitions(ddgi_transport_gpu_tests PRIVATE
    CONTAINER_DDGI_TRANSPORT_SPIRV="${DDGI_TRANSPORT_CONTRACT_SPIRV}"
)
add_dependencies(ddgi_transport_gpu_tests ddgi_transport_contract_shader)
set_tests_properties(ddgi_transport_gpu_tests PROPERTIES LABELS "requires-vulkan;requires-display;ddgi")

set(PATH_TRANSPORT_CONTRACT_SPIRV "${CMAKE_BINARY_DIR}/test_shaders/path_transport_contract.comp.spv")
add_custom_command(
    OUTPUT "${PATH_TRANSPORT_CONTRACT_SPIRV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/test_shaders"
    COMMAND "${SLANGC_EXECUTABLE}" "${DEFAULT_SHADER_DIR}/path_transport_contract.slang"
            ${SLANG_SPIRV_FLAGS} -entry computeMain -o "${PATH_TRANSPORT_CONTRACT_SPIRV}"
    DEPENDS "${DEFAULT_SHADER_DIR}/path_transport_contract.slang"
            ${SLANG_INCLUDE_SOURCES} "${CMAKE_CURRENT_LIST_FILE}" "${SLANGC_EXECUTABLE}"
    VERBATIM
)
add_custom_target(path_transport_contract_shader DEPENDS "${PATH_TRANSPORT_CONTRACT_SPIRV}")
add_custom_test(path_transport_gpu_tests
    ${TEST_RENDERER_LIGHTING_DIR}/path_transport_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_gpu_resource VulkanSceneRenderer_geometry Dep_Windowing Dep_Math
)
target_compile_definitions(path_transport_gpu_tests PRIVATE
    CONTAINER_PATH_TRANSPORT_SPIRV="${PATH_TRANSPORT_CONTRACT_SPIRV}"
)
add_dependencies(path_transport_gpu_tests path_transport_contract_shader)
set_tests_properties(path_transport_gpu_tests PROPERTIES
    LABELS "requires-vulkan;requires-display;path-tracing" RUN_SERIAL TRUE TIMEOUT 180)

set(MANAGED_COMPUTE_A_SPIRV "${CMAKE_BINARY_DIR}/test_shaders/managed_compute_a.comp.spv")
set(MANAGED_COMPUTE_B_SPIRV "${CMAKE_BINARY_DIR}/test_shaders/managed_compute_b.comp.spv")
add_custom_command(
    OUTPUT "${MANAGED_COMPUTE_A_SPIRV}" "${MANAGED_COMPUTE_B_SPIRV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/test_shaders"
    COMMAND "${SLANGC_EXECUTABLE}" "${DEFAULT_SHADER_DIR}/managed_compute_contract.slang"
            ${SLANG_SPIRV_FLAGS} -entry computeMain -DMANAGED_COMPUTE_VALUE=17 -o "${MANAGED_COMPUTE_A_SPIRV}"
    COMMAND "${SLANGC_EXECUTABLE}" "${DEFAULT_SHADER_DIR}/managed_compute_contract.slang"
            ${SLANG_SPIRV_FLAGS} -entry computeMain -DMANAGED_COMPUTE_VALUE=29 -o "${MANAGED_COMPUTE_B_SPIRV}"
    DEPENDS "${DEFAULT_SHADER_DIR}/managed_compute_contract.slang" "${CMAKE_CURRENT_LIST_FILE}" "${SLANGC_EXECUTABLE}"
    VERBATIM)
add_custom_target(managed_compute_contract_shader DEPENDS "${MANAGED_COMPUTE_A_SPIRV}" "${MANAGED_COMPUTE_B_SPIRV}")
add_custom_test(managed_compute_pipeline_gpu_tests
    ${TEST_RENDERER_LIGHTING_DIR}/managed_compute_pipeline_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_gpu_resource VulkanSceneRenderer_geometry Dep_Windowing Dep_Math)
target_compile_definitions(managed_compute_pipeline_gpu_tests PRIVATE
    CONTAINER_MANAGED_COMPUTE_A_SPIRV="${MANAGED_COMPUTE_A_SPIRV}"
    CONTAINER_MANAGED_COMPUTE_B_SPIRV="${MANAGED_COMPUTE_B_SPIRV}")
add_dependencies(managed_compute_pipeline_gpu_tests managed_compute_contract_shader)
set_tests_properties(managed_compute_pipeline_gpu_tests PROPERTIES
    LABELS "requires-vulkan;requires-display;pipeline-reload" RUN_SERIAL TRUE TIMEOUT 120)

set_tests_properties(ddgi_lookup_gpu_tests PROPERTIES LABELS "requires-vulkan;requires-display;ddgi")

set(DDGI_EXTENDED_MATERIAL_SPIRV "${CMAKE_BINARY_DIR}/test_shaders/ddgi_extended_material_contract.comp.spv")
add_custom_command(
    OUTPUT "${DDGI_EXTENDED_MATERIAL_SPIRV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/test_shaders"
    COMMAND "${SLANGC_EXECUTABLE}" "${DEFAULT_SHADER_DIR}/ddgi_extended_material_contract.slang"
            ${SLANG_SPIRV_FLAGS} -entry computeMain -o "${DDGI_EXTENDED_MATERIAL_SPIRV}"
    DEPENDS "${DEFAULT_SHADER_DIR}/ddgi_extended_material_contract.slang"
            ${SLANG_INCLUDE_SOURCES} "${CMAKE_CURRENT_LIST_FILE}"
            "${CMAKE_SOURCE_DIR}/cmake/Shaders.cmake" "${SLANGC_EXECUTABLE}"
    VERBATIM
)
add_custom_target(ddgi_extended_material_contract_shader DEPENDS "${DDGI_EXTENDED_MATERIAL_SPIRV}")
add_custom_test(ddgi_extended_material_gpu_tests
    ${TEST_RENDERER_LIGHTING_DIR}/ddgi_extended_material_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_gpu_resource VulkanSceneRenderer_geometry Dep_Windowing Dep_Math
)
target_compile_definitions(ddgi_extended_material_gpu_tests PRIVATE
    CONTAINER_DDGI_EXTENDED_MATERIAL_SPIRV="${DDGI_EXTENDED_MATERIAL_SPIRV}")
add_dependencies(ddgi_extended_material_gpu_tests ddgi_extended_material_contract_shader)
set_tests_properties(ddgi_extended_material_gpu_tests PROPERTIES
    LABELS "requires-vulkan;requires-display;ddgi;materials" RUN_SERIAL TRUE TIMEOUT 120)

add_custom_test(ltc_lut_data_tests
    ${TEST_RENDERER_LIGHTING_DIR}/ltc_lut_data_tests.cpp "" ${TEST_RESULTS_DIR}
    Dep_Math
)
target_sources(ltc_lut_data_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/lighting/LtcLutData.cpp
)

add_custom_test(submitted_upload_wait_tests
    ${TEST_RENDERER_LIGHTING_DIR}/submitted_upload_wait_tests.cpp "" ${TEST_RESULTS_DIR}
    Dep_VulkanCore
)

add_custom_test(ray_scene_tests
    ${TEST_RENDERER_CORE_DIR}/ray_scene_tests.cpp "" ${TEST_RESULTS_DIR}
    Dep_VulkanCore Dep_Math Dep_SceneIO
)
target_sources(ray_scene_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/renderer/raytracing/RayScene.cpp)
target_sources(ray_scene_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/renderer/raytracing/RaySceneExtraction.cpp)

# Ray-query SPIR-V is a separate optional consumer. Raster shaders must never
# acquire RayQueryKHR capabilities on unsupported devices.
set(RAY_QUERY_CONTRACT_SPIRV "${CMAKE_BINARY_DIR}/test_shaders/ray_query_contract.comp.spv")
add_custom_command(
    OUTPUT "${RAY_QUERY_CONTRACT_SPIRV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/test_shaders"
    COMMAND "${SLANGC_EXECUTABLE}" "${DEFAULT_SHADER_DIR}/ray_query_contract.slang"
            ${SLANG_SPIRV_FLAGS} -capability spvRayQueryKHR -entry computeMain
            -o "${RAY_QUERY_CONTRACT_SPIRV}"
    DEPENDS "${DEFAULT_SHADER_DIR}/ray_query_contract.slang"
            "${CMAKE_SOURCE_DIR}/shaders/ray_scene_data.slang"
            "${CMAKE_SOURCE_DIR}/shaders/material_data_common.slang"
            "${CMAKE_CURRENT_LIST_FILE}" "${CMAKE_SOURCE_DIR}/cmake/Shaders.cmake"
            "${SLANGC_EXECUTABLE}"
    VERBATIM
)
add_custom_target(ray_query_contract_shader DEPENDS "${RAY_QUERY_CONTRACT_SPIRV}")

# Closest-hit surface attributes are a separate future GI/reflection consumer.
# Compile its host-layout/reconstruction/traversal contract without adding any
# ray-query capability or surface buffer allocation to raster/shadow modules.
set(RAY_SURFACE_CONTRACT_SPIRV "${CMAKE_BINARY_DIR}/test_shaders/ray_surface_contract.comp.spv")
add_custom_command(
    OUTPUT "${RAY_SURFACE_CONTRACT_SPIRV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/test_shaders"
    COMMAND "${SLANGC_EXECUTABLE}" "${DEFAULT_SHADER_DIR}/ray_surface_contract.slang"
            ${SLANG_SPIRV_FLAGS} -capability spvRayQueryKHR -entry computeMain
            -o "${RAY_SURFACE_CONTRACT_SPIRV}"
    DEPENDS "${DEFAULT_SHADER_DIR}/ray_surface_contract.slang"
            "${CMAKE_SOURCE_DIR}/shaders/ray_scene_data.slang"
            "${CMAKE_SOURCE_DIR}/shaders/ray_surface_common.slang"
            "${CMAKE_CURRENT_LIST_FILE}" "${CMAKE_SOURCE_DIR}/cmake/Shaders.cmake"
            "${SLANGC_EXECUTABLE}"
    VERBATIM
)
add_custom_target(ray_surface_contract_shader DEPENDS "${RAY_SURFACE_CONTRACT_SPIRV}")
add_dependencies(ray_scene_tests ray_surface_contract_shader)
find_program(SPIRV_VAL_EXECUTABLE spirv-val
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
if(SPIRV_VAL_EXECUTABLE)
    add_test(NAME ddgi_lookup_spirv_validation
        COMMAND "${SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.4 "${DDGI_LOOKUP_CONTRACT_SPIRV}")
    add_test(NAME ddgi_transport_spirv_validation
        COMMAND "${SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.4 "${DDGI_TRANSPORT_CONTRACT_SPIRV}")
    add_test(NAME ray_surface_spirv_validation
        COMMAND "${SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.4 "${RAY_SURFACE_CONTRACT_SPIRV}")
endif()
add_custom_test(ray_scene_gpu_tests
    ${TEST_RENDERER_CORE_DIR}/ray_scene_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_gpu_resource VulkanSceneRenderer_geometry Dep_Windowing
)
target_sources(ray_scene_gpu_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/raytracing/RayScene.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/raytracing/RaySceneAcceleration.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/raytracing/RayInstanceTransformPass.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/raytracing/RaySceneExtraction.cpp
)
target_compile_definitions(ray_scene_gpu_tests PRIVATE
    CONTAINER_RAY_QUERY_CONTRACT_SPIRV="${RAY_QUERY_CONTRACT_SPIRV}"
    CONTAINER_RAY_INSTANCE_SHADER_DIR="${CMAKE_BINARY_DIR}")
add_dependencies(ray_scene_gpu_tests ray_query_contract_shader shaders_all)
set_tests_properties(ray_scene_gpu_tests PROPERTIES LABELS "requires-vulkan;requires-display;ray-query")

add_custom_test(ray_shadow_surface_gpu_tests
    ${TEST_RENDERER_CORE_DIR}/ray_shadow_surface_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
target_compile_definitions(ray_shadow_surface_gpu_tests PRIVATE
    CONTAINER_RAY_SHADOW_SHADER_ROOT="${CMAKE_BINARY_DIR}"
)
add_dependencies(ray_shadow_surface_gpu_tests shaders_all)
add_custom_test(reference_path_trace_gpu_tests
    ${TEST_RENDERER_LIGHTING_DIR}/reference_path_trace_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
target_compile_definitions(reference_path_trace_gpu_tests PRIVATE
    CONTAINER_REFERENCE_PATH_SHADER_ROOT="${CMAKE_BINARY_DIR}")
add_dependencies(reference_path_trace_gpu_tests shaders_all)
set_tests_properties(reference_path_trace_gpu_tests PROPERTIES
    LABELS "requires-vulkan;requires-display;ray-query;path-tracing" RUN_SERIAL TRUE TIMEOUT 600)
set_tests_properties(ray_shadow_surface_gpu_tests PROPERTIES
    LABELS "requires-vulkan;requires-display;ray-query;ddgi")

add_custom_test(ddgi_manager_gpu_tests
    ${TEST_RENDERER_LIGHTING_DIR}/ddgi_manager_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
target_compile_definitions(ddgi_manager_gpu_tests PRIVATE
    CONTAINER_DDGI_SHADER_ROOT="${CMAKE_BINARY_DIR}"
)
add_dependencies(ddgi_manager_gpu_tests shaders_all)
set_tests_properties(ddgi_manager_gpu_tests PROPERTIES
    LABELS "requires-vulkan;requires-display;ray-query;ddgi")

add_custom_test(scene_merge_generation_gpu_tests
    ${CMAKE_SOURCE_DIR}/tests/renderer/scene/scene_merge_generation_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)
set_tests_properties(scene_merge_generation_gpu_tests PROPERTIES
    LABELS "requires-vulkan;requires-display;gltf;scene;merge"
    TIMEOUT 120 RUN_SERIAL TRUE)

add_custom_test(material_texture_generation_gpu_tests
    ${CMAKE_SOURCE_DIR}/tests/renderer/scene/material_texture_generation_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)
set_tests_properties(material_texture_generation_gpu_tests PROPERTIES
    LABELS "requires-vulkan;requires-display;gltf;material;texture"
    TIMEOUT 120 RUN_SERIAL TRUE)

add_custom_test(material_retirement_generation_gpu_tests
    ${CMAKE_SOURCE_DIR}/tests/renderer/scene/material_retirement_generation_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)
target_compile_definitions(material_retirement_generation_gpu_tests PRIVATE
    CONTAINER_SCENE_TEXTURE_SAMPLING_SHADER_PATH="${CONTAINER_SCENE_TEXTURE_SAMPLING_SPIRV}"
)
add_dependencies(material_retirement_generation_gpu_tests shaders_all)
set_tests_properties(material_retirement_generation_gpu_tests PROPERTIES
    LABELS "requires-vulkan;requires-display;gltf;material;lifecycle"
    TIMEOUT 120 RUN_SERIAL TRUE)

add_custom_test(source_resource_retirement_gpu_tests
    ${CMAKE_SOURCE_DIR}/tests/renderer/scene/source_resource_retirement_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)

add_custom_test(camera_projection_generation_gpu_tests
    ${CMAKE_SOURCE_DIR}/tests/renderer/scene/camera_projection_generation_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)
target_compile_definitions(camera_projection_generation_gpu_tests PRIVATE
    CONTAINER_CAMERA_PROJECTION_SHADER_PATH="${CONTAINER_CAMERA_PROJECTION_SPIRV}"
)
add_dependencies(camera_projection_generation_gpu_tests shaders_all)
set_tests_properties(camera_projection_generation_gpu_tests PROPERTIES
    LABELS "requires-vulkan;requires-display;gltf;camera;authoring"
    TIMEOUT 120 RUN_SERIAL TRUE)
target_compile_definitions(source_resource_retirement_gpu_tests PRIVATE
    CONTAINER_SCENE_TEXTURE_SAMPLING_SHADER_PATH="${CONTAINER_SCENE_TEXTURE_SAMPLING_SPIRV}"
)
add_dependencies(source_resource_retirement_gpu_tests shaders_all)
set_tests_properties(source_resource_retirement_gpu_tests PROPERTIES
    LABELS "requires-vulkan;requires-display;gltf;resource;lifecycle"
    TIMEOUT 120 RUN_SERIAL TRUE)

add_custom_test(scene_texture_sampling_gpu_tests
    ${CMAKE_SOURCE_DIR}/tests/renderer/scene/scene_texture_sampling_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)
target_compile_definitions(scene_texture_sampling_gpu_tests PRIVATE
    CONTAINER_SCENE_TEXTURE_SAMPLING_SHADER_PATH="${CONTAINER_SCENE_TEXTURE_SAMPLING_SPIRV}"
)
add_dependencies(scene_texture_sampling_gpu_tests shaders_all)
set_tests_properties(scene_texture_sampling_gpu_tests PROPERTIES
    LABELS "requires-vulkan;requires-display;gltf;material;texture;lifecycle"
    TIMEOUT 120 RUN_SERIAL TRUE)

add_custom_test(lighting_generation_gpu_tests
    ${TEST_RENDERER_LIGHTING_DIR}/lighting_generation_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
target_compile_definitions(lighting_generation_gpu_tests PRIVATE
    CONTAINER_LIGHTING_SHADER_ROOT="${CMAKE_BINARY_DIR}"
)
add_dependencies(lighting_generation_gpu_tests shaders_all)
set_tests_properties(lighting_generation_gpu_tests PROPERTIES
    LABELS "requires-vulkan;requires-display;scene;lighting;merge"
    TIMEOUT 120 RUN_SERIAL TRUE)

add_custom_test(scene_controller_generation_gpu_tests
    ${TEST_RENDERER_SCENE_DIR}/scene_controller_generation_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
set_tests_properties(scene_controller_generation_gpu_tests PROPERTIES
    LABELS "requires-vulkan;requires-display;gltf;scene;merge"
    TIMEOUT 120 RUN_SERIAL TRUE)

add_custom_test(gui_model_merge_tests
    ${CMAKE_SOURCE_DIR}/tests/utility/gui_model_merge_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
set_tests_properties(gui_model_merge_tests PROPERTIES LABELS "cpu;gltf;scene;merge")

add_custom_test(cull_generation_gpu_tests
    ${CMAKE_SOURCE_DIR}/tests/renderer/culling/cull_generation_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
target_compile_definitions(cull_generation_gpu_tests PRIVATE
    CONTAINER_CULL_SHADER_ROOT="${CMAKE_BINARY_DIR}")
add_dependencies(cull_generation_gpu_tests shaders_all)
set_tests_properties(cull_generation_gpu_tests PROPERTIES
    LABELS "requires-vulkan;requires-display;scene;culling;merge"
    TIMEOUT 120 RUN_SERIAL TRUE)

set(OCCLUSION_SPHERE_CONTRACT_SPIRV "${CMAKE_BINARY_DIR}/test_shaders/occlusion_sphere_contract.comp.spv")
add_custom_command(
    OUTPUT "${OCCLUSION_SPHERE_CONTRACT_SPIRV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/test_shaders"
    COMMAND "${SLANGC_EXECUTABLE}" "${DEFAULT_SHADER_DIR}/occlusion_sphere_contract.slang"
            ${SLANG_SPIRV_FLAGS} -entry computeMain -o "${OCCLUSION_SPHERE_CONTRACT_SPIRV}"
    DEPENDS "${DEFAULT_SHADER_DIR}/occlusion_sphere_contract.slang"
            "${CMAKE_SOURCE_DIR}/shaders/occlusion_sphere_common.slang"
            "${CMAKE_CURRENT_LIST_FILE}" "${SLANGC_EXECUTABLE}"
    VERBATIM
)
add_custom_target(occlusion_sphere_contract_shader DEPENDS "${OCCLUSION_SPHERE_CONTRACT_SPIRV}")
add_custom_test(occlusion_sphere_projection_gpu_tests
    ${CMAKE_SOURCE_DIR}/tests/renderer/culling/occlusion_sphere_projection_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_gpu_resource VulkanSceneRenderer_geometry Dep_Windowing Dep_Math
)
target_compile_definitions(occlusion_sphere_projection_gpu_tests PRIVATE
    CONTAINER_OCCLUSION_SPHERE_SPIRV="${OCCLUSION_SPHERE_CONTRACT_SPIRV}")
add_dependencies(occlusion_sphere_projection_gpu_tests occlusion_sphere_contract_shader)
set_tests_properties(occlusion_sphere_projection_gpu_tests PROPERTIES
    LABELS "requires-vulkan;requires-display;scene;culling"
    TIMEOUT 120 RUN_SERIAL TRUE)

add_custom_test(glm_tests
    ${TEST_CORE_DIR}/glm_tests.cpp  ""  ${TEST_RESULTS_DIR}
    Dep_Math
)

add_custom_test(temporal_convention_tests
    ${TEST_RENDERER_CORE_DIR}/temporal_convention_tests.cpp "" ${TEST_RESULTS_DIR}
    Dep_Math
)

target_sources(temporal_convention_tests PRIVATE ${TEST_RENDERER_CORE_DIR}/temporal_state_tests.cpp)

add_custom_test(dlss_rr_clear_sync_tests
    ${TEST_RENDERER_CORE_DIR}/dlss_rr_clear_sync_tests.cpp "" ${TEST_RESULTS_DIR}
    Threads::Threads)
target_include_directories(dlss_rr_clear_sync_tests PRIVATE
    ${Vulkan_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/src/renderer/temporal)
set_tests_properties(dlss_rr_clear_sync_tests PROPERTIES LABELS "cpu;temporal;dlss;path-tracing")

add_custom_test(fsr_upscaler_gpu_tests
    ${TEST_RENDERER_CORE_DIR}/fsr_upscaler_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_fsr VulkanSceneRenderer_gpu_resource VulkanSceneRenderer_geometry)
container_stage_fsr_runtime(fsr_upscaler_gpu_tests)
set_tests_properties(fsr_upscaler_gpu_tests PROPERTIES
    LABELS "temporal;requires-vulkan" RUN_SERIAL TRUE TIMEOUT 120)

add_custom_test(fsr_vulkan_image_adapter_tests
    ${TEST_RENDERER_CORE_DIR}/fsr_vulkan_image_adapter_tests.cpp "" ${TEST_RESULTS_DIR}
    Threads::Threads)
target_include_directories(fsr_vulkan_image_adapter_tests PRIVATE
    ${Vulkan_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/src)
set_tests_properties(fsr_vulkan_image_adapter_tests PROPERTIES LABELS "cpu;temporal;fsr")

add_custom_test(temporal_capture_tests
    ${TEST_RENDERER_CORE_DIR}/temporal_capture_tests.cpp "" ${TEST_RESULTS_DIR}
    Dep_Math nlohmann_json::nlohmann_json VulkanSceneRenderer_scene
)

add_custom_test(scene_lighting_defaults_tests
    ${TEST_RENDERER_LIGHTING_DIR}/scene_lighting_defaults_tests.cpp "" ${TEST_RESULTS_DIR}
)

if(Python3_Interpreter_FOUND)
    add_test(NAME renderer_lighting_cli_tests
        COMMAND "${Python3_EXECUTABLE}" "${TEST_VALIDATION_DIR}/renderer_lighting_cli_tests.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>")
    set_tests_properties(renderer_lighting_cli_tests PROPERTIES LABELS "cpu;cli;lighting" TIMEOUT 120)
    add_test(NAME texture_memory_cli_tests
        COMMAND "${Python3_EXECUTABLE}" "${TEST_VALIDATION_DIR}/texture_memory_cli_tests.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>")
    set_tests_properties(texture_memory_cli_tests PROPERTIES LABELS "cpu;cli;texture" TIMEOUT 120)
endif()

add_test(NAME temporal_quality_metrics
    COMMAND "${Python3_EXECUTABLE}" "${TEST_VALIDATION_DIR}/temporal_regression.py" --self-test)
set_tests_properties(temporal_quality_metrics PROPERTIES SKIP_RETURN_CODE 77)

add_test(NAME model_asset_lfs_tests
    COMMAND "${Python3_EXECUTABLE}" -B "${TEST_VALIDATION_DIR}/model_asset_lfs_tests.py")

add_test(NAME scene_lighting_gpu_regression
    COMMAND "${Python3_EXECUTABLE}" -B "${TEST_VALIDATION_DIR}/scene_lighting_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>"
            --output "${TEST_RESULTS_DIR}/scene-lighting")
set_tests_properties(scene_lighting_gpu_regression PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 360 LABELS "requires-vulkan;requires-display;visual-regression")

add_test(NAME gui_model_loading_gpu_regression
    COMMAND "${Python3_EXECUTABLE}" -B "${TEST_VALIDATION_DIR}/gui_model_loading_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>"
            --suite legacy
            --output "${TEST_RESULTS_DIR}/gui-model-loading")
set_tests_properties(gui_model_loading_gpu_regression PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 600 LABELS "requires-vulkan;requires-display;visual-regression")

add_test(NAME gui_model_lifecycle_gpu_regression
    COMMAND "${Python3_EXECUTABLE}" -B "${TEST_VALIDATION_DIR}/gui_model_loading_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>"
            --suite lifecycle
            --output "${TEST_RESULTS_DIR}/gui-model-lifecycle")
set_tests_properties(gui_model_lifecycle_gpu_regression PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 600 LABELS "requires-vulkan;requires-display;visual-regression")

add_test(NAME gui_model_cancellation_gpu_regression
    COMMAND "${Python3_EXECUTABLE}" -B "${TEST_VALIDATION_DIR}/gui_model_loading_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>"
            --suite cancellation
            --output "${TEST_RESULTS_DIR}/gui-model-cancellation")
set_tests_properties(gui_model_cancellation_gpu_regression PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 600 LABELS "requires-vulkan;requires-display;visual-regression")

add_test(NAME gui_model_loading_harness_tests
    COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
            -s "${TEST_VALIDATION_DIR}" -p "test_gui_model_loading_regression.py")
set_tests_properties(gui_model_loading_harness_tests PROPERTIES LABELS "cpu")

add_test(NAME forward_culling_gpu_regression
    COMMAND "${Python3_EXECUTABLE}" -B "${TEST_VALIDATION_DIR}/forward_culling_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>"
            --output "${TEST_RESULTS_DIR}/forward-culling")
set_tests_properties(forward_culling_gpu_regression PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 360 LABELS "requires-vulkan;requires-display;visual-regression")

add_test(NAME hiz_odd_extent_gpu_regression
    COMMAND "${Python3_EXECUTABLE}" -B "${TEST_VALIDATION_DIR}/forward_culling_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>"
            --output "${TEST_RESULTS_DIR}/hiz-odd-extent"
            --hiz-odd-only)
set_tests_properties(hiz_odd_extent_gpu_regression PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 540 LABELS "requires-vulkan;requires-display;visual-regression")

add_test(NAME bim_floor_plan_gpu_regression
    COMMAND "${Python3_EXECUTABLE}" -B "${TEST_VALIDATION_DIR}/bim_floor_plan_runtime_regression.py"
            --renderer "$<TARGET_FILE:VulkanSceneRenderer>"
            --output "${TEST_RESULTS_DIR}/bim-floor-plan" --managed-output)
set_tests_properties(bim_floor_plan_gpu_regression PROPERTIES
    SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 900
    LABELS "requires-vulkan;requires-display;visual-regression")

add_test(NAME bim_floor_plan_forward_gpu_regression
    COMMAND "${Python3_EXECUTABLE}" -B "${TEST_VALIDATION_DIR}/bim_floor_plan_runtime_regression.py"
            --renderer "$<TARGET_FILE:VulkanSceneRenderer>"
            --output "${TEST_RESULTS_DIR}/bim-floor-plan-forward" --managed-output
            --comparison forward-raster)
set_tests_properties(bim_floor_plan_forward_gpu_regression PROPERTIES
    SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 900
    LABELS "requires-vulkan;requires-display;visual-regression")

add_test(NAME bim_annotation_gpu_regression
    COMMAND "${Python3_EXECUTABLE}" -B "${TEST_VALIDATION_DIR}/bim_annotation_runtime_regression.py"
            --renderer "$<TARGET_FILE:VulkanSceneRenderer>"
            --output "${TEST_RESULTS_DIR}/bim-annotations" --managed-output)
set_tests_properties(bim_annotation_gpu_regression PROPERTIES
    SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 900
    LABELS "requires-vulkan;requires-display;visual-regression")

add_test(NAME frame_resource_initialization_gpu_regression
    COMMAND "${Python3_EXECUTABLE}" -B "${TEST_VALIDATION_DIR}/frame_resource_initialization_runtime_regression.py"
            --renderer "$<TARGET_FILE:VulkanSceneRenderer>"
            --output "${TEST_RESULTS_DIR}/frame-resource-initialization" --managed-output)
set_tests_properties(frame_resource_initialization_gpu_regression PROPERTIES
    SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 420
    LABELS "requires-vulkan;requires-display;visual-regression")

add_test(NAME forward_sky_gpu_regression
    COMMAND "${Python3_EXECUTABLE}" -B "${TEST_VALIDATION_DIR}/forward_sky_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>"
            --output "${TEST_RESULTS_DIR}/forward-sky")
set_tests_properties(forward_sky_gpu_regression PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 360 LABELS "requires-vulkan;requires-display;visual-regression")

add_test(NAME area_shadow_gpu_regression
    COMMAND "${Python3_EXECUTABLE}" -B "${TEST_VALIDATION_DIR}/area_shadow_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>"
            --output "${TEST_RESULTS_DIR}/area-shadows")
add_test(NAME ltc_area_light_numeric_oracle
    COMMAND "${Python3_EXECUTABLE}" -B "${TEST_VALIDATION_DIR}/ltc_area_light_regression.py"
            --case cpu --assets "${CMAKE_SOURCE_DIR}/materials/ltc"
            --output "${TEST_RESULTS_DIR}/ltc-area-numeric")
set_tests_properties(ltc_area_light_numeric_oracle PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 60 LABELS "lighting;numeric-reference")
add_test(NAME ltc_area_light_benchmark_statistics
    COMMAND "${Python3_EXECUTABLE}" -B "${TEST_VALIDATION_DIR}/test_ltc_benchmark_stats.py")
set_tests_properties(ltc_area_light_benchmark_statistics PROPERTIES
    TIMEOUT 30 LABELS "lighting;benchmark")
add_test(NAME ltc_area_light_gpu_regression
    COMMAND "${Python3_EXECUTABLE}" -B "${TEST_VALIDATION_DIR}/ltc_area_light_regression.py"
            --case correctness --exe "$<TARGET_FILE:VulkanSceneRenderer>" --assets "${CMAKE_SOURCE_DIR}/materials/ltc"
            --output "${TEST_RESULTS_DIR}/ltc-area-gpu")
set_tests_properties(ltc_area_light_gpu_regression PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 3600 LABELS "requires-vulkan;requires-display;lighting;visual-regression")
add_test(NAME material_layer_gpu_regression
    COMMAND "${Python3_EXECUTABLE}" -B "${TEST_VALIDATION_DIR}/material_layer_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>" --assets "${CMAKE_SOURCE_DIR}/materials/ltc"
            --output "${TEST_RESULTS_DIR}/material-layers")
set_tests_properties(material_layer_gpu_regression PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 1800 LABELS "requires-vulkan;requires-display;lighting;visual-regression")
add_test(NAME ray_shadow_gpu_regression
    COMMAND "${Python3_EXECUTABLE}" -B "${TEST_VALIDATION_DIR}/ray_shadow_regression.py"
            --exe "$<TARGET_FILE:VulkanSceneRenderer>"
            --output "${TEST_RESULTS_DIR}/ray-shadows")
set_tests_properties(ray_shadow_gpu_regression PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 900 LABELS "requires-vulkan;requires-display;ray-query")
set_tests_properties(area_shadow_gpu_regression PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 600 LABELS "requires-vulkan;requires-display;visual-regression")

# Exercise all Slang temporal helpers with dynamic inputs, outside runtime
# assets. Building the math test also compiles this probe with engine flags.
set(TEMPORAL_CONTRACT_SPIRV "${CMAKE_BINARY_DIR}/test_shaders/temporal_contract.comp.spv")
add_custom_command(
    OUTPUT "${TEMPORAL_CONTRACT_SPIRV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/test_shaders"
    COMMAND "${SLANGC_EXECUTABLE}" "${DEFAULT_SHADER_DIR}/temporal_contract.slang"
            ${SLANG_SPIRV_FLAGS} -entry computeMain -o "${TEMPORAL_CONTRACT_SPIRV}"
    DEPENDS "${DEFAULT_SHADER_DIR}/temporal_contract.slang"
            "${SHADERS_DIR}/temporal_common.slang"
            "${CMAKE_CURRENT_LIST_FILE}"
            "${CMAKE_SOURCE_DIR}/cmake/Shaders.cmake"
            "${SLANGC_EXECUTABLE}"
    COMMENT "Compiling the temporal convention Slang probe"
    VERBATIM
)
add_custom_target(temporal_contract_shader DEPENDS "${TEMPORAL_CONTRACT_SPIRV}")
add_dependencies(temporal_convention_tests temporal_contract_shader)

find_program(TEMPORAL_SPIRV_VAL_EXECUTABLE NAMES spirv-val
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
if(TEMPORAL_SPIRV_VAL_EXECUTABLE)
    add_test(NAME ray_query_contract_spirv_validation
        COMMAND "${TEMPORAL_SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.4
                "${RAY_QUERY_CONTRACT_SPIRV}")
    add_test(NAME temporal_contract_spirv_validation
        COMMAND "${TEMPORAL_SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.4
                "${TEMPORAL_CONTRACT_SPIRV}")
endif()

add_custom_test(ecs_tests
    ${TEST_CORE_DIR}/ecs_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_ecs  VulkanSceneRenderer_scene
)

add_custom_test(scene_graph_tests
    ${TEST_SCENE_DIR}/scene_graph_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)
add_custom_test(material_binding_authoring_tests
    ${TEST_SCENE_DIR}/material_binding_authoring_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)
set_tests_properties(material_binding_authoring_tests PROPERTIES LABELS "editor;gltf;material;cpu" TIMEOUT 60)
add_custom_test(material_tail_references_tests
    ${TEST_SCENE_DIR}/material_tail_references_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)
set_tests_properties(material_tail_references_tests PROPERTIES LABELS "editor;gltf;material;cpu" TIMEOUT 60)
add_custom_test(resource_lifecycle_tests
    ${CMAKE_SOURCE_DIR}/tests/utility/resource_lifecycle_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)
set_tests_properties(resource_lifecycle_tests PROPERTIES LABELS "editor;resource;lifecycle;cpu" TIMEOUT 60)
add_custom_test(texture_resource_lifecycle_tests
    ${CMAKE_SOURCE_DIR}/tests/utility/texture_resource_lifecycle_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)
set_tests_properties(texture_resource_lifecycle_tests PROPERTIES LABELS "editor;resource;lifecycle;cpu" TIMEOUT 60)
add_custom_test(gltf_texture_resource_references_tests
    ${CMAKE_SOURCE_DIR}/tests/geometry/gltf_texture_resource_references_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)
set_tests_properties(gltf_texture_resource_references_tests PROPERTIES LABELS "editor;gltf;resource;cpu" TIMEOUT 60)
add_custom_test(texture_manager_tests
    ${CMAKE_SOURCE_DIR}/tests/utility/texture_manager_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_gpu_resource
)
set_tests_properties(texture_manager_tests PROPERTIES LABELS "editor;resource;texture;lifecycle;cpu" TIMEOUT 60)
add_custom_test(material_references_tests
    ${TEST_SCENE_DIR}/material_references_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)
set_tests_properties(material_references_tests PROPERTIES LABELS "editor;gltf;material;cpu" TIMEOUT 60)

add_custom_test(edit_history_tests
    ${CMAKE_SOURCE_DIR}/tests/utility/edit_history_tests.cpp "" ${TEST_RESULTS_DIR}
    Dep_Math
)
add_custom_test(scene_edit_history_tests
    ${CMAKE_SOURCE_DIR}/tests/utility/scene_edit_history_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)
add_custom_test(scene_graph_edit_tests
    ${CMAKE_SOURCE_DIR}/tests/utility/scene_graph_edit_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)
add_custom_test(scene_node_authoring_policy_tests
    ${CMAKE_SOURCE_DIR}/tests/utility/scene_node_authoring_policy_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)
add_custom_test(gui_scene_node_authoring_tests
    ${CMAKE_SOURCE_DIR}/tests/utility/gui_scene_node_authoring_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
add_custom_test(gui_material_authoring_tests
    ${CMAKE_SOURCE_DIR}/tests/ui/gui_material_authoring_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
set_tests_properties(gui_material_authoring_tests PROPERTIES LABELS "editor;cpu;gltf;material" TIMEOUT 120)

add_custom_test(gltf_xmp_inspector_widget_tests
    ${CMAKE_SOURCE_DIR}/tests/ui/gltf_xmp_inspector_widget_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
target_include_directories(gltf_xmp_inspector_widget_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/utility)
set_tests_properties(gltf_xmp_inspector_widget_tests PROPERTIES LABELS "editor;cpu;gltf;xmp" TIMEOUT 60)

add_custom_test(camera_projection_controls_tests
    ${CMAKE_SOURCE_DIR}/tests/utility/camera_projection_controls_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_ui
)
set_tests_properties(camera_projection_controls_tests PROPERTIES LABELS "editor;cpu;gltf;camera" TIMEOUT 120)
set_tests_properties(edit_history_tests scene_edit_history_tests scene_graph_edit_tests
    PROPERTIES LABELS "editor;cpu" TIMEOUT 60)

if(ENABLE_PHYSICS)
    add_custom_test(physics_world_tests
        ${CMAKE_SOURCE_DIR}/tests/physics/physics_world_tests.cpp "" ${TEST_RESULTS_DIR}
        VulkanSceneRenderer_physics Threads::Threads Jolt::Jolt
    )
    add_custom_test(physics_scene_tests
        ${CMAKE_SOURCE_DIR}/tests/physics/physics_scene_tests.cpp "" ${TEST_RESULTS_DIR}
        VulkanSceneRenderer_physics
    )
    add_custom_test(physics_element_bindings_tests
        ${CMAKE_SOURCE_DIR}/tests/physics/physics_element_bindings_tests.cpp "" ${TEST_RESULTS_DIR}
        VulkanSceneRenderer_physics
    )
    set_tests_properties(physics_world_tests physics_scene_tests physics_element_bindings_tests PROPERTIES
        LABELS "physics;cpu" TIMEOUT 60)
endif()

# Controls remain available in builds without the backend, where the panel
# explains its unavailable state and suppresses every simulation action.
add_custom_test(physics_controls_widget_tests
    ${CMAKE_SOURCE_DIR}/tests/utility/physics_controls_widget_tests.cpp "" ${TEST_RESULTS_DIR}
    Dep_Math Dep_UI
)
target_sources(physics_controls_widget_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/utility/PhysicsControls.cpp
)
set_tests_properties(physics_controls_widget_tests PROPERTIES
    LABELS "physics;cpu;ui" TIMEOUT 60)

add_custom_test(input_manager_tests
    ${TEST_PLATFORM_DIR}/input_manager_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_window
)


add_custom_test(gltf_vertex_alpha_tests
    ${TEST_GEOMETRY_DIR}/gltf_vertex_alpha_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)

add_custom_test(gltf_loader_tests
    ${TEST_GEOMETRY_DIR}/gltf_loader_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)

add_custom_test(gltf_node_flags_tests
    ${TEST_GEOMETRY_DIR}/gltf_node_flags_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)
set_tests_properties(gltf_node_flags_tests PROPERTIES LABELS "gltf;scene;cpu" TIMEOUT 60)

add_custom_test(gltf_merge_tests
    ${TEST_GEOMETRY_DIR}/gltf_merge_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)
set_tests_properties(gltf_merge_tests PROPERTIES LABELS "gltf;scene;cpu" TIMEOUT 60)

add_custom_test(gltf_subtree_clone_tests
    ${TEST_GEOMETRY_DIR}/gltf_subtree_clone_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)
set_tests_properties(gltf_subtree_clone_tests PROPERTIES LABELS "gltf;scene;cpu;authoring" TIMEOUT 60)

add_custom_test(gltf_projected_subtree_clone_tests
    ${TEST_GEOMETRY_DIR}/gltf_projected_subtree_clone_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)
add_custom_test(graph_clone_projection_tests
    ${CMAKE_SOURCE_DIR}/tests/utility/graph_clone_projection_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)
add_custom_test(gltf_projected_animation_tests
    ${TEST_GEOMETRY_DIR}/gltf_projected_animation_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry VulkanSceneRenderer_scene
)
set_tests_properties(gltf_projected_subtree_clone_tests graph_clone_projection_tests gltf_projected_animation_tests
    PROPERTIES LABELS "gltf;scene;cpu;authoring" TIMEOUT 60)

add_custom_test(gltf_node_export_compaction_tests
    ${TEST_GEOMETRY_DIR}/gltf_node_export_compaction_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)
set_tests_properties(gltf_node_export_compaction_tests PROPERTIES LABELS "gltf;scene;cpu;authoring" TIMEOUT 60)
add_custom_test(gltf_material_export_compaction_tests
    ${TEST_GEOMETRY_DIR}/gltf_material_export_compaction_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry VulkanSceneRenderer_scene
)
set_tests_properties(gltf_material_export_compaction_tests PROPERTIES LABELS "gltf;scene;material;cpu;authoring" TIMEOUT 60)
add_custom_test(gltf_resource_export_compaction_tests
    ${TEST_GEOMETRY_DIR}/gltf_resource_export_compaction_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry VulkanSceneRenderer_scene
)
set_tests_properties(gltf_resource_export_compaction_tests PROPERTIES LABELS "gltf;scene;resource;cpu;authoring" TIMEOUT 60)

add_custom_test(gltf_session_merge_tests
    ${TEST_GEOMETRY_DIR}/gltf_session_merge_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)
add_custom_test(gltf_merge_runtime_data_tests
    ${TEST_GEOMETRY_DIR}/gltf_merge_runtime_data_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)
add_custom_test(gltf_merge_graph_tests
    ${CMAKE_SOURCE_DIR}/tests/utility/gltf_merge_graph_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)
add_custom_test(gltf_merge_geometry_tests
    ${TEST_GEOMETRY_DIR}/gltf_merge_geometry_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)
set_tests_properties(gltf_session_merge_tests gltf_merge_runtime_data_tests gltf_merge_graph_tests gltf_merge_geometry_tests
    PROPERTIES LABELS "gltf;scene;cpu" TIMEOUT 60)

add_custom_test(gltf_material_mapping_tests
    ${TEST_SCENE_DIR}/gltf_material_mapping_tests.cpp "" ${TEST_RESULTS_DIR}
)
set_tests_properties(gltf_material_mapping_tests PROPERTIES LABELS "gltf;scene;cpu" TIMEOUT 60)

add_custom_test(gltf_extended_materials_tests
    ${TEST_GEOMETRY_DIR}/gltf_extended_materials_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)
set_tests_properties(gltf_extended_materials_tests PROPERTIES LABELS "gltf;material;cpu" TIMEOUT 60)

add_custom_test(gltf_opacity_micromap_tests
    ${TEST_GEOMETRY_DIR}/gltf_opacity_micromap_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)
set_tests_properties(gltf_opacity_micromap_tests PROPERTIES LABELS "gltf;ray-scene;cpu" TIMEOUT 60)

add_custom_test(gltf_material_authoring_tests
    ${TEST_GEOMETRY_DIR}/gltf_material_authoring_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry VulkanSceneRenderer_scene
)
set_tests_properties(gltf_material_authoring_tests PROPERTIES LABELS "gltf;material;authoring;cpu" TIMEOUT 60)

add_custom_test(gltf_camera_authoring_tests
    ${CMAKE_SOURCE_DIR}/tests/scene/gltf_camera_authoring_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry VulkanSceneRenderer_scene
)
set_tests_properties(gltf_camera_authoring_tests PROPERTIES LABELS "gltf;camera;authoring;cpu" TIMEOUT 60)

add_custom_test(gltf_camera_pose_authoring_tests
    ${CMAKE_SOURCE_DIR}/tests/scene/gltf_camera_pose_authoring_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry VulkanSceneRenderer_scene)
add_custom_test(gltf_camera_node_rest_tests
    ${CMAKE_SOURCE_DIR}/tests/scene/gltf_camera_node_rest_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry VulkanSceneRenderer_scene)
add_custom_test(scene_node_lifetime_tests
    ${CMAKE_SOURCE_DIR}/tests/scene/scene_node_lifetime_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene)
add_custom_test(camera_view_pose_controls_tests
    ${CMAKE_SOURCE_DIR}/tests/utility/camera_view_pose_controls_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_ui)
set_tests_properties(gltf_camera_pose_authoring_tests gltf_camera_node_rest_tests scene_node_lifetime_tests
    camera_view_pose_controls_tests PROPERTIES LABELS "gltf;camera;authoring;cpu" TIMEOUT 120)
add_custom_test(world_pose_generation_tests
    ${CMAKE_SOURCE_DIR}/tests/ecs/world_pose_generation_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_ecs VulkanSceneRenderer_scene)
set_tests_properties(world_pose_generation_tests PROPERTIES LABELS "ecs;camera;cpu" TIMEOUT 120)
add_custom_test(camera_pose_scene_generation_gpu_tests
    ${CMAKE_SOURCE_DIR}/tests/renderer/scene/camera_pose_scene_generation_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer)
set_tests_properties(camera_pose_scene_generation_gpu_tests PROPERTIES
    LABELS "requires-vulkan;requires-display;gltf;camera;authoring" TIMEOUT 180 RUN_SERIAL TRUE)
add_custom_test(camera_view_pose_generation_gpu_tests
    ${CMAKE_SOURCE_DIR}/tests/renderer/scene/camera_view_pose_generation_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene)
target_compile_definitions(camera_view_pose_generation_gpu_tests PRIVATE
    CONTAINER_CAMERA_PROJECTION_SHADER_PATH="${CONTAINER_CAMERA_PROJECTION_SPIRV}")
add_dependencies(camera_view_pose_generation_gpu_tests shaders_all)
add_custom_test(camera_pose_lighting_gpu_tests
    ${CMAKE_SOURCE_DIR}/tests/renderer/lighting/camera_pose_lighting_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer)
target_compile_definitions(camera_pose_lighting_gpu_tests PRIVATE
    CONTAINER_LIGHTING_SHADER_ROOT="${CMAKE_BINARY_DIR}")
add_dependencies(camera_pose_lighting_gpu_tests shaders_all)
set_tests_properties(camera_view_pose_generation_gpu_tests camera_pose_lighting_gpu_tests PROPERTIES
    LABELS "requires-vulkan;requires-display;gltf;camera;authoring" TIMEOUT 180 RUN_SERIAL TRUE)

add_custom_test(gltf_material_texture_authoring_tests
    ${CMAKE_SOURCE_DIR}/tests/scene/gltf_material_texture_authoring_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry VulkanSceneRenderer_scene
)
set_tests_properties(gltf_material_texture_authoring_tests PROPERTIES LABELS "gltf;material;texture;authoring;cpu" TIMEOUT 60)

add_custom_test(gltf_material_animation_tests
    ${TEST_GEOMETRY_DIR}/gltf_material_animation_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry VulkanSceneRenderer_scene
)
set_tests_properties(gltf_material_animation_tests PROPERTIES LABELS "gltf;material;animation;cpu" TIMEOUT 60)

add_custom_test(gltf_material_conversion_tests
    ${TEST_GEOMETRY_DIR}/gltf_material_conversion_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)
set_tests_properties(gltf_material_conversion_tests PROPERTIES LABELS "gltf;material;authoring;cpu" TIMEOUT 60)

add_custom_test(gltf_runtime_material_authoring_tests
    ${CMAKE_SOURCE_DIR}/tests/scene/gltf_runtime_material_authoring_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)
set_tests_properties(gltf_runtime_material_authoring_tests PROPERTIES LABELS "gltf;material;authoring;cpu" TIMEOUT 60)

add_custom_test(gltf_source_material_export_tests
    ${CMAKE_SOURCE_DIR}/tests/scene/gltf_source_material_export_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)
set_tests_properties(gltf_source_material_export_tests PROPERTIES LABELS "gltf;material;export;cpu" TIMEOUT 60)

add_custom_test(gltf_primitive_frame_variants_tests
    ${TEST_GEOMETRY_DIR}/gltf_primitive_frame_variants_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)
set_tests_properties(gltf_primitive_frame_variants_tests PROPERTIES LABELS "gltf;material;deformation;cpu" TIMEOUT 60)

add_custom_test(gltf_compression_loader_tests
    ${TEST_GEOMETRY_DIR}/gltf_compression_loader_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry Dep_GltfCompression
)
set_tests_properties(gltf_compression_loader_tests PROPERTIES LABELS "gltf;compression;cpu" TIMEOUT 60)

add_custom_test(gltf_compression_tests
    ${TEST_GEOMETRY_DIR}/gltf_compression_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry Dep_GltfCompression
)
set_tests_properties(gltf_compression_tests PROPERTIES LABELS "gltf;compression;cpu" TIMEOUT 90)
if(TARGET ContainerDracoWorker)
    target_link_libraries(gltf_compression_tests PRIVATE draco::draco)
    target_compile_definitions(gltf_compression_tests PRIVATE
        CONTAINER_DRACO_WORKER_EXECUTABLE="$<TARGET_FILE:ContainerDracoWorker>")
    add_dependencies(gltf_compression_tests ContainerDracoWorker)
endif()

add_custom_test(gltf_draco_worker_tests
    ${TEST_GEOMETRY_DIR}/gltf_draco_worker_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)
set_tests_properties(gltf_draco_worker_tests PROPERTIES LABELS "gltf;compression;cpu" TIMEOUT 90)
if(WIN32)
    add_executable(ContainerDracoWorkerProbe
        ${TEST_GEOMETRY_DIR}/gltf_draco_worker_probe.cpp
        ${CMAKE_SOURCE_DIR}/src/geometry/GltfDracoProtocol.cpp)
    target_include_directories(ContainerDracoWorkerProbe PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_compile_definitions(ContainerDracoWorkerProbe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_features(ContainerDracoWorkerProbe PRIVATE cxx_std_23)
    set_target_properties(ContainerDracoWorkerProbe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    target_compile_definitions(gltf_draco_worker_tests PRIVATE
        CONTAINER_DRACO_PROBE_EXECUTABLE="$<TARGET_FILE:ContainerDracoWorkerProbe>")
    add_dependencies(gltf_draco_worker_tests ContainerDracoWorkerProbe)
endif()
if(TARGET ContainerDracoWorker)
    target_link_libraries(gltf_draco_worker_tests PRIVATE draco::draco)
    target_compile_definitions(gltf_draco_worker_tests PRIVATE CONTAINER_HAS_DRACO=1
        CONTAINER_DRACO_WORKER_EXECUTABLE="$<TARGET_FILE:ContainerDracoWorker>")
    add_dependencies(gltf_draco_worker_tests ContainerDracoWorker)
endif()

if(ENABLE_GLTF_DRACO AND ENABLE_GLTF_MESHOPT)
    add_executable(GltfCompressionFixtureGenerator
        ${TESTS_DIR}/fixtures/gltf_compression_fixture_generator.cpp)
    target_compile_features(GltfCompressionFixtureGenerator PRIVATE cxx_std_23)
    target_link_libraries(GltfCompressionFixtureGenerator PRIVATE
        draco::draco meshoptimizer::meshoptimizer nlohmann_json::nlohmann_json)
    set_target_properties(GltfCompressionFixtureGenerator PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()

add_custom_test(gltf_accessor_tests
    ${TEST_GEOMETRY_DIR}/gltf_accessor_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)

add_custom_test(texture_image_data_tests
    ${TESTS_DIR}/utility/texture_image_data_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_texture_codecs Dep_TextureCodecs Dep_SceneIO
)
add_custom_test(texture_upload_plan_tests
    ${TEST_GEOMETRY_DIR}/texture_upload_plan_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_texture_codecs
)
add_custom_test(gltf_image_sources_tests
    ${TEST_GEOMETRY_DIR}/gltf_image_sources_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry Dep_GltfCompression
)
add_custom_test(gltf_texture_resources_tests
    ${TEST_GEOMETRY_DIR}/gltf_texture_resources_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)
add_custom_test(gltf_instancing_tests
    ${TEST_GEOMETRY_DIR}/gltf_instancing_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)
add_custom_test(gltf_scene_instancing_tests
    ${TEST_GEOMETRY_DIR}/gltf_scene_instancing_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)
add_custom_test(gltf_scene_selection_tests
    ${TEST_GEOMETRY_DIR}/gltf_scene_selection_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene
)
add_custom_test(gltf_material_variants_tests
    ${TEST_GEOMETRY_DIR}/gltf_material_variants_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry VulkanSceneRenderer_scene
)

add_custom_test(gltf_capabilities_tests
    ${TEST_GEOMETRY_DIR}/gltf_capabilities_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)

add_custom_test(gltf_scene_export_tests
    ${TEST_GEOMETRY_DIR}/gltf_scene_export_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry VulkanSceneRenderer_scene
)
target_sources(gltf_scene_export_tests PRIVATE
    ${TEST_GEOMETRY_DIR}/gltf_camera_serialization_tests.cpp)
add_custom_test(gltf_material_export_tests
    ${TEST_GEOMETRY_DIR}/gltf_material_export_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)
add_custom_test(gltf_animation_tests
    ${TEST_GEOMETRY_DIR}/gltf_animation_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry VulkanSceneRenderer_scene
)
add_custom_test(gltf_interactivity_tests
    ${TEST_GEOMETRY_DIR}/gltf_interactivity_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)
add_custom_test(gltf_deformation_tests
    ${TEST_GEOMETRY_DIR}/gltf_deformation_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)
add_custom_test(gltf_deformation_frame_tests
    ${TEST_GEOMETRY_DIR}/gltf_deformation_frame_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)
add_custom_test(gltf_deformation_loader_tests
    ${TEST_GEOMETRY_DIR}/gltf_deformation_loader_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)
add_custom_test(scene_light_export_tests
    ${TEST_RENDERER_DIR}/scene/scene_light_export_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
add_custom_test(gltf_deformation_gpu_tests
    ${TEST_RENDERER_SCENE_DIR}/gltf_deformation_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
add_custom_test(gpu_hierarchy_tests
    ${TEST_RENDERER_SCENE_DIR}/gpu_hierarchy_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
target_compile_definitions(gpu_hierarchy_tests PRIVATE CONTAINER_HIERARCHY_SHADER_DIR="${CMAKE_BINARY_DIR}")
add_dependencies(gpu_hierarchy_tests shaders_all)
set_tests_properties(gpu_hierarchy_tests PROPERTIES LABELS "requires-vulkan;hierarchy" RUN_SERIAL TRUE TIMEOUT 180)
target_compile_definitions(gltf_deformation_gpu_tests PRIVATE
    CONTAINER_GLTF_DEFORMATION_SHADER_DIR="${CMAKE_BINARY_DIR}")
add_dependencies(gltf_deformation_gpu_tests shaders_all)
set_tests_properties(gltf_deformation_gpu_tests PROPERTIES
    LABELS "requires-vulkan;gltf;deformation" RUN_SERIAL TRUE TIMEOUT 120)
add_custom_test(gltf_deformation_scene_tests
    ${TEST_RENDERER_SCENE_DIR}/gltf_deformation_scene_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
add_custom_test(temporal_deformed_vertex_tests
    ${TEST_RENDERER_CORE_DIR}/temporal_deformed_vertex_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(dotbim_loader_tests
    ${TEST_GEOMETRY_DIR}/dotbim_loader_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)

add_custom_test(ifc_tessellated_loader_tests
    ${TEST_GEOMETRY_DIR}/ifc_tessellated_loader_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_geometry
)
target_sources(ifc_tessellated_loader_tests PRIVATE
    ${TEST_GEOMETRY_DIR}/ifc_curve_family_tests.cpp
    ${TEST_GEOMETRY_DIR}/ifc_brep_profile_tests.cpp
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

add_custom_test(openvdb_volume_settings_tests
    ${TEST_RENDERER_CORE_DIR}/openvdb_volume_settings_tests.cpp "" ${TEST_RESULTS_DIR}
)
set_tests_properties(openvdb_volume_settings_tests PROPERTIES LABELS "openvdb;cpu" TIMEOUT 30)

set(_vdb_scattering_shader_dir "${CMAKE_BINARY_DIR}/tests/openvdb/shaders")
set(_vdb_scattering_spirv "${_vdb_scattering_shader_dir}/openvdb_multiple_scattering_test.comp.spv")
add_custom_command(
    OUTPUT "${_vdb_scattering_spirv}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_vdb_scattering_shader_dir}"
    COMMAND "${SLANGC_EXECUTABLE}" "${DEFAULT_SHADER_DIR}/openvdb_multiple_scattering_test.slang"
            ${SLANG_SPIRV_FLAGS} -entry computeMain -o "${_vdb_scattering_spirv}"
    DEPENDS "${DEFAULT_SHADER_DIR}/openvdb_multiple_scattering_test.slang"
            "${CMAKE_SOURCE_DIR}/shaders/openvdb_multiple_scattering.slang"
            "${CMAKE_CURRENT_LIST_FILE}" "${CMAKE_SOURCE_DIR}/cmake/Shaders.cmake"
            "${SLANGC_EXECUTABLE}"
    VERBATIM
)
add_custom_target(openvdb_multiple_scattering_test_shader DEPENDS "${_vdb_scattering_spirv}")
add_executable(openvdb_multiple_scattering_gpu_tests
    "${TEST_RENDERER_CORE_DIR}/openvdb_multiple_scattering_gpu_tests.cpp")
target_link_libraries(openvdb_multiple_scattering_gpu_tests PRIVATE
    VulkanSceneRenderer_gpu_resource VulkanSceneRenderer_geometry GTest::gtest)
target_compile_definitions(openvdb_multiple_scattering_gpu_tests PRIVATE
    CONTAINER_OPENVDB_SCATTERING_SPIRV="${_vdb_scattering_spirv}")
add_dependencies(openvdb_multiple_scattering_gpu_tests openvdb_multiple_scattering_test_shader)
container_stage_runtime_dlls(openvdb_multiple_scattering_gpu_tests)
add_test(NAME openvdb_multiple_scattering_gpu_tests
    COMMAND openvdb_multiple_scattering_gpu_tests
        "--gtest_output=xml:${TEST_RESULTS_DIR}/openvdb_multiple_scattering_gpu_tests-$<CONFIG>.xml")
set_tests_properties(openvdb_multiple_scattering_gpu_tests PROPERTIES
    LABELS "requires-vulkan;openvdb" RUN_SERIAL TRUE TIMEOUT 120 SKIP_RETURN_CODE 77)

set(_vdb_reference_spirv "${CONTAINER_OPENVDB_TRANSPORT_SPIRV}")
if(SPIRV_VAL_EXECUTABLE)
    add_test(NAME openvdb_transport_reference_spirv_validation
        COMMAND "${SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.4 "${_vdb_reference_spirv}")
    set_tests_properties(openvdb_transport_reference_spirv_validation PROPERTIES
        LABELS "openvdb;shader" TIMEOUT 30)
endif()
add_executable(openvdb_transport_gpu_reference_tests
    "${TEST_RENDERER_CORE_DIR}/openvdb_transport_gpu_reference_tests.cpp")
target_link_libraries(openvdb_transport_gpu_reference_tests PRIVATE
    VulkanSceneRenderer_openvdb_transport_reference Dep_OpenVDB Dep_NanoVDB GTest::gtest)
target_compile_definitions(openvdb_transport_gpu_reference_tests PRIVATE
    CONTAINER_OPENVDB_TRANSPORT_REFERENCE_SPIRV="${_vdb_reference_spirv}")
add_dependencies(openvdb_transport_gpu_reference_tests openvdb_transport_reference_shader)
set_target_properties(openvdb_transport_gpu_reference_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/openvdb/$<CONFIG>")
container_stage_runtime_dlls(openvdb_transport_gpu_reference_tests)
add_test(NAME openvdb_transport_gpu_reference_tests COMMAND openvdb_transport_gpu_reference_tests
    "--gtest_output=xml:${TEST_RESULTS_DIR}/openvdb_transport_gpu_reference_tests-$<CONFIG>.xml")
set_tests_properties(openvdb_transport_gpu_reference_tests PROPERTIES
    LABELS "requires-vulkan;openvdb" RUN_SERIAL TRUE TIMEOUT 180 SKIP_RETURN_CODE 77)

add_custom_test(openvdb_transport_image_tests
    ${TEST_GEOMETRY_DIR}/openvdb_transport_image_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb_transport_image Dep_OpenVDB)
set_tests_properties(openvdb_transport_image_tests PROPERTIES LABELS "openvdb;cpu" TIMEOUT 120)
add_custom_test(openvdb_transport_render_job_tests
    ${TEST_GEOMETRY_DIR}/openvdb_transport_render_job_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb_transport_image Dep_OpenVDB)
set_tests_properties(openvdb_transport_render_job_tests PROPERTIES LABELS "openvdb;cpu" TIMEOUT 120)
foreach(_image_config IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${_image_config}" _image_config_upper)
    set_target_properties(openvdb_transport_image_tests openvdb_transport_render_job_tests PROPERTIES
        "RUNTIME_OUTPUT_DIRECTORY_${_image_config_upper}" "${CMAKE_BINARY_DIR}/tests/openvdb/${_image_config}")
endforeach()
add_executable(openvdb_transport_image_gpu_tests
    "${TEST_RENDERER_CORE_DIR}/openvdb_transport_image_gpu_tests.cpp")
target_link_libraries(openvdb_transport_image_gpu_tests PRIVATE
    VulkanSceneRenderer_openvdb_transport_image Dep_OpenVDB Dep_NanoVDB GTest::gtest)
target_compile_definitions(openvdb_transport_image_gpu_tests PRIVATE
    CONTAINER_OPENVDB_TRANSPORT_REFERENCE_SPIRV="${_vdb_reference_spirv}")
add_dependencies(openvdb_transport_image_gpu_tests openvdb_transport_reference_shader)
set_target_properties(openvdb_transport_image_gpu_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/openvdb/$<CONFIG>")
container_stage_runtime_dlls(openvdb_transport_image_gpu_tests)
add_test(NAME openvdb_transport_image_gpu_tests COMMAND openvdb_transport_image_gpu_tests
    "--gtest_output=xml:${TEST_RESULTS_DIR}/openvdb_transport_image_gpu_tests-$<CONFIG>.xml")
set_tests_properties(openvdb_transport_image_gpu_tests PROPERTIES
    LABELS "requires-vulkan;openvdb" RUN_SERIAL TRUE TIMEOUT 180 SKIP_RETURN_CODE 77)

add_custom_test(openvdb_loader_tests
    ${TEST_GEOMETRY_DIR}/openvdb_loader_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb Dep_OpenVDB
)
add_custom_test(openvdb_processing_tests
    ${TEST_GEOMETRY_DIR}/openvdb_processing_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb_processing Dep_OpenVDB
)
add_custom_test(openvdb_particles_tests
    ${TEST_GEOMETRY_DIR}/openvdb_particles_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb_processing Dep_OpenVDB
)
add_custom_test(openvdb_advection_tests
    ${TEST_GEOMETRY_DIR}/openvdb_advection_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb_processing Dep_OpenVDB
)
add_custom_test(openvdb_segmentation_tests
    ${TEST_GEOMETRY_DIR}/openvdb_segmentation_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb_processing Dep_OpenVDB
)
add_custom_test(openvdb_mesh_export_tests
    ${TEST_GEOMETRY_DIR}/openvdb_mesh_export_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb_processing Dep_OpenVDB
)
add_custom_test(openvdb_inspect_tests
    ${TEST_GEOMETRY_DIR}/openvdb_inspect_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb_processing Dep_OpenVDB
)
add_custom_test(openvdb_points_tests
    ${TEST_GEOMETRY_DIR}/openvdb_points_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb_processing Dep_OpenVDB
)
add_custom_test(openvdb_vector_processing_tests
    ${TEST_GEOMETRY_DIR}/openvdb_vector_processing_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb_processing Dep_OpenVDB
)
add_custom_test(openvdb_poisson_tests
    ${TEST_GEOMETRY_DIR}/openvdb_poisson_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb_processing Dep_OpenVDB
)

add_custom_test(openvdb_point_processing_tests
    ${TEST_GEOMETRY_DIR}/openvdb_point_processing_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb_processing Dep_OpenVDB
)
add_custom_test(openvdb_fluid_projection_tests
    ${TEST_GEOMETRY_DIR}/openvdb_fluid_projection_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb_processing Dep_OpenVDB
)
add_custom_test(openvdb_point_preview_tests
    ${TEST_GEOMETRY_DIR}/openvdb_point_preview_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb Dep_OpenVDB
)
add_custom_test(openvdb_primitives_tests
    ${TEST_GEOMETRY_DIR}/openvdb_primitives_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb_processing Dep_OpenVDB
)
add_custom_test(openvdb_fluid_simulation_tests
    ${TEST_GEOMETRY_DIR}/openvdb_fluid_simulation_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb_processing Dep_OpenVDB
)
add_custom_test(openvdb_transport_reference_tests
    ${TEST_GEOMETRY_DIR}/openvdb_transport_reference_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb
)
add_custom_test(openvdb_fluid_sequence_tests
    ${TEST_GEOMETRY_DIR}/openvdb_fluid_sequence_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb_processing Dep_OpenVDB
)
add_custom_test(openvdb_measure_tests
    ${TEST_GEOMETRY_DIR}/openvdb_measure_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb_processing Dep_OpenVDB
)
set_tests_properties(openvdb_fluid_sequence_tests openvdb_measure_tests PROPERTIES
    LABELS "openvdb;cpu" TIMEOUT 120)
add_custom_test(openvdb_mesh_conversion_tests
    ${TEST_GEOMETRY_DIR}/openvdb_mesh_conversion_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb_processing Dep_OpenVDB
)
add_custom_test(openvdb_vector_preview_tests
    ${TEST_GEOMETRY_DIR}/openvdb_vector_preview_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb Dep_OpenVDB
)
foreach(_vdb_component IN ITEMS point_rasterization fluid_viscosity fluid_sources level_set_transport point_attributes point_attributes_json point_selection dense_interchange multi_resolution file_authoring file_authoring_spec field_filter field_composite point_mask native_point_preview slice_preview topology sdf_filter point_field_sampling volume_spheres fast_sweeping regions region_queries ray_queries fog_ray_integrals value_components surface_reference particle_geometry boundary_fluid_step surface_queries fracture field_advection potential_flow point_neighbors level_set_rebuild surface_fields point_motion native_point_advection point_transfer field_resampling diagnostics boundary_projection provenance nano_interchange nano_statistics nano_primitives)
    add_custom_test(openvdb_${_vdb_component}_tests
        ${TEST_GEOMETRY_DIR}/openvdb_${_vdb_component}_tests.cpp "" ${TEST_RESULTS_DIR}
        VulkanSceneRenderer_openvdb_processing Dep_OpenVDB)
    set_tests_properties(openvdb_${_vdb_component}_tests PROPERTIES
        LABELS "openvdb;cpu" TIMEOUT 120)
    set_target_properties(openvdb_${_vdb_component}_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/openvdb/$<CONFIG>")
    foreach(_vdb_component_config IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${_vdb_component_config}" _vdb_component_config_upper)
        set_target_properties(openvdb_${_vdb_component}_tests PROPERTIES
            "RUNTIME_OUTPUT_DIRECTORY_${_vdb_component_config_upper}"
            "${CMAKE_BINARY_DIR}/tests/openvdb/${_vdb_component_config}")
    endforeach()
endforeach()
target_link_libraries(openvdb_nano_interchange_tests PRIVATE Dep_NanoVDB_IO)
target_link_libraries(openvdb_nano_statistics_tests PRIVATE Dep_NanoVDB_IO)
target_link_libraries(openvdb_nano_primitives_tests PRIVATE Dep_NanoVDB_IO)
add_executable(nanovdb_fixture_generator "${CMAKE_SOURCE_DIR}/tests/fixtures/nanovdb_fixture_generator.cpp")
add_executable(OpenVdbQueryFixtureGenerator "${CMAKE_SOURCE_DIR}/tests/fixtures/openvdb_query_fixture_generator.cpp")
target_compile_features(OpenVdbQueryFixtureGenerator PRIVATE cxx_std_23)
target_compile_definitions(OpenVdbQueryFixtureGenerator PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(OpenVdbQueryFixtureGenerator PRIVATE Dep_OpenVDB)
container_stage_runtime_dlls(OpenVdbQueryFixtureGenerator)
add_executable(OpenVdbDiscretePreviewFixture "${CMAKE_SOURCE_DIR}/tests/fixtures/openvdb_discrete_preview_fixture.cpp")
target_compile_features(OpenVdbDiscretePreviewFixture PRIVATE cxx_std_23)
target_compile_definitions(OpenVdbDiscretePreviewFixture PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(OpenVdbDiscretePreviewFixture PRIVATE Dep_OpenVDB)
container_stage_runtime_dlls(OpenVdbDiscretePreviewFixture)
target_compile_features(nanovdb_fixture_generator PRIVATE cxx_std_23)
target_include_directories(nanovdb_fixture_generator PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(nanovdb_fixture_generator PRIVATE Dep_NanoVDB_IO Dep_OpenVDB)
container_stage_runtime_dlls(nanovdb_fixture_generator)
set_tests_properties(openvdb_mesh_conversion_tests openvdb_vector_preview_tests PROPERTIES
    LABELS "openvdb;cpu" TIMEOUT 120)
set_tests_properties(openvdb_fluid_simulation_tests openvdb_transport_reference_tests PROPERTIES
    LABELS "openvdb;cpu" TIMEOUT 120)
set_tests_properties(openvdb_point_preview_tests openvdb_primitives_tests PROPERTIES
    LABELS "openvdb;cpu" TIMEOUT 120)
set_tests_properties(openvdb_point_processing_tests openvdb_fluid_projection_tests PROPERTIES
    LABELS "openvdb;cpu" TIMEOUT 120)
add_custom_test(openvdb_authoring_tests
    ${CMAKE_SOURCE_DIR}/tests/utility/openvdb_authoring_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_ui
)
set_tests_properties(openvdb_authoring_tests PROPERTIES LABELS "openvdb;cpu" TIMEOUT 60)
add_custom_test(openvdb_volume_tests
    ${TEST_GEOMETRY_DIR}/openvdb_volume_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb Dep_OpenVDB Dep_NanoVDB
)
add_custom_test(openvdb_volume_sequence_tests
    ${TEST_GEOMETRY_DIR}/openvdb_volume_sequence_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_openvdb Dep_OpenVDB Dep_NanoVDB
)
set_tests_properties(openvdb_loader_tests openvdb_processing_tests openvdb_particles_tests openvdb_advection_tests openvdb_segmentation_tests openvdb_mesh_export_tests openvdb_inspect_tests openvdb_points_tests openvdb_vector_processing_tests openvdb_poisson_tests openvdb_volume_tests openvdb_volume_sequence_tests PROPERTIES
    LABELS "openvdb;cpu" TIMEOUT 120)
if(ENABLE_OPENVDB)
    add_executable(openvdb_volume_upload_gpu_tests
        "${TEST_RENDERER_CORE_DIR}/openvdb_volume_upload_gpu_tests.cpp")
    target_link_libraries(openvdb_volume_upload_gpu_tests PRIVATE
        VulkanSceneRenderer_renderer Dep_Windowing GTest::gtest Threads::Threads)
    target_compile_definitions(openvdb_volume_upload_gpu_tests PRIVATE
        CONTAINER_OPENVDB_SHADER_ROOT="${CMAKE_BINARY_DIR}")
    add_dependencies(openvdb_volume_upload_gpu_tests shaders_all)
    set_target_properties(openvdb_volume_upload_gpu_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/openvdb/$<CONFIG>")
    container_stage_runtime_dlls(openvdb_volume_upload_gpu_tests)
    add_test(NAME openvdb_volume_upload_gpu_tests
        COMMAND openvdb_volume_upload_gpu_tests
            "--gtest_output=xml:${TEST_RESULTS_DIR}/openvdb_volume_upload_gpu_tests-$<CONFIG>.xml")
    set_tests_properties(openvdb_volume_upload_gpu_tests PROPERTIES
        LABELS "requires-vulkan;requires-display;openvdb" RUN_SERIAL TRUE
        TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()
# OpenVDB and its compression DLLs can share basenames across CRT variants.
# Keep these new tests' configuration outputs separate.
foreach(_vdb_config IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${_vdb_config}" _vdb_config_upper)
    set_target_properties(openvdb_loader_tests openvdb_processing_tests openvdb_particles_tests openvdb_advection_tests openvdb_segmentation_tests openvdb_mesh_export_tests openvdb_inspect_tests openvdb_points_tests openvdb_vector_processing_tests openvdb_poisson_tests openvdb_point_processing_tests openvdb_fluid_projection_tests openvdb_point_preview_tests openvdb_primitives_tests openvdb_fluid_simulation_tests openvdb_transport_reference_tests openvdb_fluid_sequence_tests openvdb_measure_tests openvdb_mesh_conversion_tests openvdb_vector_preview_tests openvdb_volume_tests openvdb_volume_sequence_tests openvdb_authoring_tests
        PROPERTIES "RUNTIME_OUTPUT_DIRECTORY_${_vdb_config_upper}"
        "${CMAKE_BINARY_DIR}/tests/openvdb/${_vdb_config}")
endforeach()

add_custom_target(openvdb_cpu_validation DEPENDS
    OpenVdbTool openvdb_loader_tests openvdb_processing_tests
    openvdb_particles_tests openvdb_advection_tests openvdb_segmentation_tests
    openvdb_mesh_export_tests openvdb_inspect_tests openvdb_points_tests
    openvdb_point_processing_tests openvdb_vector_processing_tests
    openvdb_poisson_tests openvdb_fluid_projection_tests openvdb_authoring_tests
    openvdb_point_preview_tests openvdb_primitives_tests
    openvdb_fluid_simulation_tests openvdb_transport_reference_tests openvdb_fluid_sequence_tests openvdb_measure_tests
    openvdb_mesh_conversion_tests openvdb_vector_preview_tests
    openvdb_point_rasterization_tests openvdb_fluid_viscosity_tests
    openvdb_fluid_sources_tests openvdb_level_set_transport_tests openvdb_point_attributes_tests openvdb_point_attributes_json_tests
    openvdb_point_selection_tests openvdb_dense_interchange_tests openvdb_multi_resolution_tests
    openvdb_file_authoring_tests openvdb_file_authoring_spec_tests
    openvdb_field_filter_tests
    openvdb_field_composite_tests openvdb_point_mask_tests openvdb_native_point_preview_tests openvdb_slice_preview_tests
    openvdb_topology_tests openvdb_sdf_filter_tests openvdb_point_field_sampling_tests openvdb_volume_spheres_tests openvdb_fast_sweeping_tests openvdb_regions_tests openvdb_region_queries_tests openvdb_ray_queries_tests openvdb_fog_ray_integrals_tests openvdb_value_components_tests openvdb_surface_reference_tests openvdb_particle_geometry_tests openvdb_boundary_fluid_step_tests openvdb_surface_queries_tests openvdb_fracture_tests openvdb_field_advection_tests openvdb_potential_flow_tests openvdb_point_neighbors_tests openvdb_level_set_rebuild_tests openvdb_surface_fields_tests openvdb_point_motion_tests openvdb_native_point_advection_tests openvdb_point_transfer_tests openvdb_field_resampling_tests openvdb_diagnostics_tests openvdb_boundary_projection_tests openvdb_provenance_tests openvdb_nano_interchange_tests openvdb_nano_statistics_tests openvdb_nano_primitives_tests nanovdb_fixture_generator OpenVdbQueryFixtureGenerator
    openvdb_transport_image_tests openvdb_transport_render_job_tests OpenVdbTransportRender
    openvdb_volume_tests openvdb_volume_sequence_tests openvdb_volume_settings_tests)

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

add_custom_test(material_sampler_key_tests
    ${TEST_GEOMETRY_DIR}/material_sampler_key_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_gpu_resource
)

add_custom_test(material_sampler_gpu_tests
    ${TEST_RENDERER_CORE_DIR}/material_sampler_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_gpu_resource VulkanSceneRenderer_geometry
)
target_compile_definitions(material_sampler_gpu_tests PRIVATE
    CONTAINER_MATERIAL_SAMPLER_SHADER_DIR="${CMAKE_BINARY_DIR}")
add_dependencies(material_sampler_gpu_tests shaders_all)
set_tests_properties(material_sampler_gpu_tests PROPERTIES
    LABELS "requires-vulkan;materials;samplers" RUN_SERIAL TRUE TIMEOUT 120)

set(EXTENDED_PBR_CONTRACT_SPIRV "${CMAKE_BINARY_DIR}/spv_shaders/extended_pbr_contract.comp.spv")
add_custom_command(
    OUTPUT "${EXTENDED_PBR_CONTRACT_SPIRV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/spv_shaders"
    COMMAND "${SLANGC_EXECUTABLE}" "${DEFAULT_SHADER_DIR}/extended_pbr_contract.slang"
            ${SLANG_SPIRV_FLAGS} -entry computeMain -o "${EXTENDED_PBR_CONTRACT_SPIRV}"
    DEPENDS "${DEFAULT_SHADER_DIR}/extended_pbr_contract.slang"
            ${SLANG_INCLUDE_SOURCES} "${CMAKE_CURRENT_LIST_FILE}"
            "${CMAKE_SOURCE_DIR}/cmake/Shaders.cmake" "${SLANGC_EXECUTABLE}"
    VERBATIM
)
add_custom_target(extended_pbr_contract_shader DEPENDS "${EXTENDED_PBR_CONTRACT_SPIRV}")
add_custom_test(extended_pbr_gpu_tests
    ${TEST_RENDERER_CORE_DIR}/extended_pbr_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_gpu_resource VulkanSceneRenderer_geometry nlohmann_json::nlohmann_json
)
target_compile_definitions(extended_pbr_gpu_tests PRIVATE
    CONTAINER_EXTENDED_PBR_SHADER_DIR="${CMAKE_BINARY_DIR}"
    CONTAINER_EXTENDED_PBR_REFERENCE_FILE="${CMAKE_SOURCE_DIR}/tests/fixtures/rendering/extended_pbr_reference.json")
add_dependencies(extended_pbr_gpu_tests extended_pbr_contract_shader)
set_tests_properties(extended_pbr_gpu_tests PROPERTIES
    LABELS "requires-vulkan;materials;pbr" RUN_SERIAL TRUE TIMEOUT 120)

set(EXTENDED_PBR_RESOLVE_OUTPUTS)
foreach(EXTENDED_PBR_RESOLVE_STAGE vert frag comp)
    if(EXTENDED_PBR_RESOLVE_STAGE STREQUAL "vert")
        set(EXTENDED_PBR_RESOLVE_ENTRY vertMain)
    elseif(EXTENDED_PBR_RESOLVE_STAGE STREQUAL "frag")
        set(EXTENDED_PBR_RESOLVE_ENTRY fragMain)
    else()
        set(EXTENDED_PBR_RESOLVE_ENTRY computeMain)
    endif()
    set(EXTENDED_PBR_RESOLVE_OUTPUT "${CMAKE_BINARY_DIR}/spv_shaders/extended_pbr_resolve_contract.${EXTENDED_PBR_RESOLVE_STAGE}.spv")
    add_custom_command(
        OUTPUT "${EXTENDED_PBR_RESOLVE_OUTPUT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/spv_shaders"
        COMMAND "${SLANGC_EXECUTABLE}" "${DEFAULT_SHADER_DIR}/extended_pbr_resolve_contract.slang"
                ${SLANG_SPIRV_FLAGS} -entry "${EXTENDED_PBR_RESOLVE_ENTRY}" -o "${EXTENDED_PBR_RESOLVE_OUTPUT}"
        DEPENDS "${DEFAULT_SHADER_DIR}/extended_pbr_resolve_contract.slang"
                ${SLANG_INCLUDE_SOURCES} "${CMAKE_CURRENT_LIST_FILE}"
                "${CMAKE_SOURCE_DIR}/cmake/Shaders.cmake" "${SLANGC_EXECUTABLE}"
        VERBATIM
    )
    list(APPEND EXTENDED_PBR_RESOLVE_OUTPUTS "${EXTENDED_PBR_RESOLVE_OUTPUT}")
endforeach()
add_custom_target(extended_pbr_resolve_contract_shader DEPENDS ${EXTENDED_PBR_RESOLVE_OUTPUTS})
add_custom_test(extended_pbr_resolve_gpu_tests
    ${TEST_RENDERER_CORE_DIR}/extended_pbr_resolve_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_gpu_resource VulkanSceneRenderer_geometry)
target_compile_definitions(extended_pbr_resolve_gpu_tests PRIVATE
    CONTAINER_EXTENDED_PBR_SHADER_DIR="${CMAKE_BINARY_DIR}")
add_dependencies(extended_pbr_resolve_gpu_tests extended_pbr_resolve_contract_shader)
set_tests_properties(extended_pbr_resolve_gpu_tests PROPERTIES
    LABELS "requires-vulkan;materials;pbr;msaa" RUN_SERIAL TRUE TIMEOUT 120)

add_custom_test(texture_upload_gpu_tests
    ${TEST_RENDERER_CORE_DIR}/texture_upload_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_gpu_resource VulkanSceneRenderer_geometry
)
set_tests_properties(texture_upload_gpu_tests PROPERTIES
    LABELS "requires-vulkan;materials;textures" RUN_SERIAL TRUE TIMEOUT 120)
set(TEXTURE_UPLOAD_SAMPLE_SPV "${CMAKE_BINARY_DIR}/test_shaders/texture_upload_sample_contract.comp.spv")
add_custom_command(
    OUTPUT "${TEXTURE_UPLOAD_SAMPLE_SPV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/test_shaders"
    COMMAND "${SLANGC_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/shaders/texture_upload_sample_contract.slang"
            ${SLANG_SPIRV_FLAGS} -entry computeMain -o "${TEXTURE_UPLOAD_SAMPLE_SPV}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tests/shaders/texture_upload_sample_contract.slang"
            "${CMAKE_SOURCE_DIR}/cmake/Shaders.cmake" "${SLANGC_EXECUTABLE}"
    VERBATIM
)
add_custom_target(texture_upload_sample_shader DEPENDS "${TEXTURE_UPLOAD_SAMPLE_SPV}")
add_dependencies(texture_upload_gpu_tests texture_upload_sample_shader)
target_compile_definitions(texture_upload_gpu_tests PRIVATE
    CONTAINER_TEXTURE_UPLOAD_SAMPLE_SPV="${TEXTURE_UPLOAD_SAMPLE_SPV}")

add_custom_test(bcf_viewpoint_tests
    ${TEST_UI_DIR}/bcf_viewpoint_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(rendering_convention_tests
    ${TEST_VALIDATION_DIR}/rendering_convention_tests.cpp  ""  ${TEST_RESULTS_DIR}
    Dep_Math Dep_VulkanCore Dep_SceneIO
)
target_sources(rendering_convention_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/bim/BimSectionCapBuilder.cpp
    ${CMAKE_SOURCE_DIR}/src/renderer/bim/BimMetadata.cpp
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

add_custom_test(camera_controller_transform_tests
    ${TEST_RENDERER_SCENE_DIR}/camera_controller_transform_tests.cpp "" ${TEST_RESULTS_DIR}
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

add_custom_test(bim_inspection_cache_tests
    ${TEST_RENDERER_BIM_DIR}/bim_inspection_cache_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_statistics_cache_tests
    ${TEST_RENDERER_BIM_DIR}/bim_statistics_cache_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
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

add_custom_test(bim_metadata_storage_tests
    ${TEST_RENDERER_BIM_DIR}/bim_metadata_storage_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_object_transform_tests
    ${TEST_RENDERER_BIM_DIR}/bim_object_transform_tests.cpp "" ${TEST_RESULTS_DIR}
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
    Dep_Math Dep_SceneIO
)
target_compile_definitions(scene_provider_extraction_tests PRIVATE
    CONTAINER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

# CPU measurements are opt-in and have no timing thresholds in CTest.
if(ENABLE_BIM_FRAME_CACHE_BENCHMARK)
    add_executable(bim_frame_cache_benchmark
        ${TESTS_DIR}/benchmarks/bim_frame_cache_benchmark.cpp)
    target_link_libraries(bim_frame_cache_benchmark PRIVATE VulkanSceneRenderer_renderer)
    target_compile_definitions(bim_frame_cache_benchmark PRIVATE
        CONTAINER_BIM_BENCHMARK_CONFIG="$<CONFIG>")
    set_target_properties(bim_frame_cache_benchmark PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/benchmarks)
    container_stage_runtime_dlls(bim_frame_cache_benchmark)

    if(CONTAINER_BIM_CACHE_BASELINE_INCLUDE_DIR)
        if(NOT EXISTS "${CONTAINER_BIM_CACHE_BASELINE_INCLUDE_DIR}/Container/scene/SceneProvider.h")
            message(FATAL_ERROR "BIM benchmark baseline must contain captured Container headers")
        endif()
        add_executable(bim_frame_cache_baseline
            ${TESTS_DIR}/benchmarks/bim_frame_cache_benchmark.cpp)
        target_include_directories(bim_frame_cache_baseline BEFORE PRIVATE
            ${CONTAINER_BIM_CACHE_BASELINE_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/include)
        target_link_libraries(bim_frame_cache_baseline PRIVATE Dep_Math Dep_VulkanCore)
        target_compile_features(bim_frame_cache_baseline PRIVATE cxx_std_23)
        target_compile_definitions(bim_frame_cache_baseline PRIVATE
            CONTAINER_BIM_CACHE_BASELINE CONTAINER_BIM_BENCHMARK_CONFIG="$<CONFIG>")
        set_target_properties(bim_frame_cache_baseline PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/benchmarks)
        container_stage_runtime_dlls(bim_frame_cache_baseline)
    endif()
endif()

add_custom_test(technique_debug_model_tests
    ${TEST_RENDERER_CORE_DIR}/technique_debug_model_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_element_search_cache_tests
    ${TESTS_DIR}/utility/bim_element_search_cache_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_ui
)

add_custom_test(scene_hierarchy_ui_tests
    ${TEST_RENDERER_CORE_DIR}/scene_hierarchy_ui_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

add_custom_test(bim_relationship_browser_cache_tests
    ${TEST_RENDERER_BIM_DIR}/bim_relationship_browser_cache_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
target_include_directories(bim_relationship_browser_cache_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/utility")

add_custom_test(bim_element_browser_widget_tests
    ${TESTS_DIR}/utility/bim_element_browser_widget_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

if(ENABLE_SCENE_BROWSER_BENCHMARK)
    add_executable(scene_browser_benchmark ${TESTS_DIR}/benchmarks/scene_browser_benchmark.cpp)
    target_link_libraries(scene_browser_benchmark PRIVATE VulkanSceneRenderer_renderer)
    set_target_properties(scene_browser_benchmark PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/benchmarks)
    container_stage_runtime_dlls(scene_browser_benchmark)
    if(Python3_Interpreter_FOUND)
        add_test(NAME scene_browser_scale_stress COMMAND "${Python3_EXECUTABLE}"
            "${TESTS_DIR}/benchmarks/run_scene_browser_scale.py"
            --exe "$<TARGET_FILE:scene_browser_benchmark>"
            --counts 100000 1000000 10000000 --fixture compact --require-opt-in
            --output "${TEST_RESULTS_DIR}/scene-browser-scale-$<CONFIG>/results.json")
        set_tests_properties(scene_browser_scale_stress PROPERTIES
            LABELS "cpu;stress;large-memory" SKIP_RETURN_CODE 77 TIMEOUT 12000
            RUN_SERIAL TRUE)
    endif()
endif()
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

add_custom_test(renderer_mcp_measurement_tests
    ${TEST_RENDERER_CORE_DIR}/renderer_mcp_measurement_tests.cpp "" ${TEST_RESULTS_DIR}
    nlohmann_json::nlohmann_json
)
target_sources(renderer_mcp_measurement_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/core/RendererMcpMeasurement.cpp
)

if(ENABLE_RENDERER_MCP)
    if(WIN32 AND Python3_Interpreter_FOUND)
        add_test(NAME scene_linear_capture_visibility_gpu_regression
            COMMAND "${Python3_EXECUTABLE}" -B
                "${TEST_VALIDATION_DIR}/scene_linear_capture_visibility_runtime_regression.py"
                "$<TARGET_FILE:VulkanSceneRenderer>"
                "${TEST_RESULTS_DIR}/scene-linear-capture-visibility")
        set_tests_properties(scene_linear_capture_visibility_gpu_regression PROPERTIES
            LABELS "requires-vulkan;requires-display;visual-regression"
            RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 600)
    endif()
    add_custom_test(renderer_mcp_transport_tests
        ${TEST_RENDERER_CORE_DIR}/renderer_mcp_transport_tests.cpp "" ${TEST_RESULTS_DIR}
        VulkanSceneRenderer_renderer
    )
    if(WIN32)
        target_link_libraries(renderer_mcp_transport_tests PRIVATE ws2_32)
    endif()
    set_tests_properties(renderer_mcp_transport_tests PROPERTIES LABELS "renderer;cpu" TIMEOUT 20)
endif()

add_custom_test(comfyui_bridge_tests
    ${TEST_RENDERER_CORE_DIR}/comfyui_bridge_tests.cpp "" ${TEST_RESULTS_DIR}
    nlohmann_json::nlohmann_json VulkanSceneRenderer_texture_codecs
)
target_sources(comfyui_bridge_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/renderer/core/ComfyUiBridge.cpp
)

add_custom_test(gpu_hardware_monitor_tests
    ${TEST_RENDERER_CORE_DIR}/gpu_hardware_monitor_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_gpu_hardware_monitor
)
set_tests_properties(gpu_hardware_monitor_tests PROPERTIES LABELS "renderer;cpu" TIMEOUT 15)

# ── Tests that need Vulkan / windowing runtime ───────────────────────────────
add_custom_test(gfx_capture_tests
    ${TEST_RENDERER_CORE_DIR}/gfx_capture_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)

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

# Selected light history: pure receipts, actual widgets, lifetime guards and GPU consumers.
add_custom_test(light_edit_receipt_tests
    ${CMAKE_SOURCE_DIR}/tests/renderer/lighting/light_edit_receipt_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_light_edit_support VulkanSceneRenderer_scene)
add_custom_test(world_light_values_tests
    ${CMAKE_SOURCE_DIR}/tests/ecs/world_light_values_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_ecs VulkanSceneRenderer_scene)
add_custom_test(light_edit_controls_tests
    ${CMAKE_SOURCE_DIR}/tests/utility/light_edit_controls_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_ui)
add_custom_test(transform_gesture_lifecycle_tests
    ${CMAKE_SOURCE_DIR}/tests/platform/transform_gesture_lifecycle_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer)
add_custom_test(light_edit_cache_tests
    ${CMAKE_SOURCE_DIR}/tests/renderer/core/light_edit_cache_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_light_edit_support VulkanSceneRenderer_scene)
set_tests_properties(light_edit_receipt_tests world_light_values_tests light_edit_controls_tests
    transform_gesture_lifecycle_tests light_edit_cache_tests PROPERTIES LABELS "editor;lighting;history;cpu" TIMEOUT 120)
add_custom_test(light_edit_generation_gpu_tests
    ${CMAKE_SOURCE_DIR}/tests/renderer/lighting/light_edit_generation_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer)
target_compile_definitions(light_edit_generation_gpu_tests PRIVATE
    CONTAINER_LIGHT_EDIT_SHADER_PATH="${CONTAINER_LIGHT_EDIT_SPIRV}"
    CONTAINER_LIGHTING_SHADER_ROOT="${CMAKE_BINARY_DIR}")
add_custom_test(lighting_scratch_recovery_gpu_tests
    ${CMAKE_SOURCE_DIR}/tests/renderer/lighting/lighting_scratch_recovery_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer)
target_compile_definitions(lighting_scratch_recovery_gpu_tests PRIVATE
    CONTAINER_LIGHTING_SCRATCH_SHADER_PATH="${CONTAINER_LIGHTING_SCRATCH_SPIRV}"
    CONTAINER_LIGHTING_SHADER_ROOT="${CMAKE_BINARY_DIR}")
add_dependencies(light_edit_generation_gpu_tests shaders_all)
add_dependencies(lighting_scratch_recovery_gpu_tests shaders_all)
set_tests_properties(light_edit_generation_gpu_tests lighting_scratch_recovery_gpu_tests PROPERTIES
    LABELS "requires-vulkan;requires-display;lighting;history" TIMEOUT 180 RUN_SERIAL TRUE)

add_custom_test(managed_compute_owner_gpu_tests
    ${TEST_RENDERER_LIGHTING_DIR}/managed_compute_owner_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer)
target_compile_definitions(managed_compute_owner_gpu_tests PRIVATE
    CONTAINER_MANAGED_OWNER_SHADER_ROOT="${CMAKE_BINARY_DIR}")
add_dependencies(managed_compute_owner_gpu_tests shaders_all)
set_tests_properties(managed_compute_owner_gpu_tests PROPERTIES
    LABELS "requires-vulkan;requires-display;pipeline-reload" RUN_SERIAL TRUE TIMEOUT 120)

set(CONTAINER_ENVIRONMENT_SAMPLE_SPIRV "${CMAKE_BINARY_DIR}/test_shaders/environment_sample_contract.comp.spv")
add_custom_command(OUTPUT "${CONTAINER_ENVIRONMENT_SAMPLE_SPIRV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/test_shaders"
    COMMAND "${SLANGC_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/shaders/environment_sample_contract.slang"
            ${SLANG_SPIRV_FLAGS} -entry computeMain -o "${CONTAINER_ENVIRONMENT_SAMPLE_SPIRV}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tests/shaders/environment_sample_contract.slang"
            "${SLANGC_EXECUTABLE}" "${CMAKE_CURRENT_LIST_FILE}"
    VERBATIM)
add_custom_target(environment_sample_contract_shader DEPENDS "${CONTAINER_ENVIRONMENT_SAMPLE_SPIRV}")
target_compile_definitions(managed_compute_owner_gpu_tests PRIVATE
    CONTAINER_ENVIRONMENT_SAMPLE_SPIRV="${CONTAINER_ENVIRONMENT_SAMPLE_SPIRV}")
add_dependencies(managed_compute_owner_gpu_tests environment_sample_contract_shader)

set(SAMPLER_PUBLICATION_SPIRV "${CMAKE_BINARY_DIR}/spv_shaders/scene_sampler_publication.comp.spv")
add_custom_command(OUTPUT "${SAMPLER_PUBLICATION_SPIRV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/spv_shaders"
    COMMAND ${SLANGC_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tests/shaders/scene_texture_sampling_contract.slang"
        -entry computeMain -stage compute -target spirv -profile spirv_1_6
        -emit-spirv-directly -fvk-use-entrypoint-name -matrix-layout-column-major
        -I "${CMAKE_SOURCE_DIR}/shaders" -o "${SAMPLER_PUBLICATION_SPIRV}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tests/shaders/scene_texture_sampling_contract.slang"
        "${CMAKE_SOURCE_DIR}/shaders/material_data_common.slang"
    VERBATIM)
add_custom_target(scene_sampler_publication_shader DEPENDS "${SAMPLER_PUBLICATION_SPIRV}")
add_custom_test(scene_sampler_publication_gpu_tests
    ${CMAKE_SOURCE_DIR}/tests/renderer/scene/scene_sampler_publication_gpu_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_scene)
target_compile_definitions(scene_sampler_publication_gpu_tests PRIVATE
    CONTAINER_SCENE_TEXTURE_SAMPLING_SHADER_PATH="${SAMPLER_PUBLICATION_SPIRV}")
add_dependencies(scene_sampler_publication_gpu_tests scene_sampler_publication_shader shaders_all)
set_tests_properties(scene_sampler_publication_gpu_tests PROPERTIES
    LABELS "requires-vulkan;materials;samplers" RUN_SERIAL TRUE TIMEOUT 120)

add_custom_test(runtime_shader_compiler_tests
    ${TEST_RENDERER_CORE_DIR}/runtime_shader_compiler_tests.cpp "" ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
target_compile_definitions(runtime_shader_compiler_tests PRIVATE
    CONTAINER_TEST_CMAKE="${CMAKE_COMMAND}")
set_tests_properties(runtime_shader_compiler_tests PROPERTIES LABELS "renderer;shader-reload" TIMEOUT 30)

if(Python3_Interpreter_FOUND AND SLANGC_EXECUTABLE)
    add_test(NAME runtime_shader_include_lookup_tests
        COMMAND "${Python3_EXECUTABLE}" "${TEST_VALIDATION_DIR}/runtime_shader_include_lookup_tests.py"
            --slangc "${SLANGC_EXECUTABLE}" --cmake "${CMAKE_COMMAND}")
    set_tests_properties(runtime_shader_include_lookup_tests PROPERTIES LABELS "renderer;shader-reload" TIMEOUT 30)
endif()

if(WIN32)
    find_program(CONTAINER_PACKAGE_PWSH NAMES pwsh)
    if(CONTAINER_PACKAGE_PWSH)
        add_test(NAME windows_package_selected_dependencies_tests
            COMMAND "${CONTAINER_PACKAGE_PWSH}" -NoProfile -File
                "${TEST_CMAKE_DIR}/windows_package_selected_dependencies_tests.ps1"
                -TestWorkDirectory "${CMAKE_BINARY_DIR}/selected-dependency-package-tests")
        set_tests_properties(windows_package_selected_dependencies_tests PROPERTIES
            LABELS "packaging;dependencies" TIMEOUT 30)
    endif()
endif()
