add_library(VulkanSceneRenderer_robotics
    RoboticsScene.cpp RoboticsSceneManifest.cpp BimCollisionGeometry.cpp BimWalkability.cpp)
target_compile_features(VulkanSceneRenderer_robotics PUBLIC cxx_std_23)
target_include_directories(VulkanSceneRenderer_robotics PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(VulkanSceneRenderer_robotics PUBLIC Dep_Math
    PRIVATE VulkanSceneRenderer_geometry nlohmann_json::nlohmann_json)

add_executable(BimRoboticsScene ${CMAKE_SOURCE_DIR}/tools/bim_robotics_scene.cpp)
target_link_libraries(BimRoboticsScene PRIVATE VulkanSceneRenderer_robotics VulkanSceneRenderer_geometry)
set_target_properties(BimRoboticsScene PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
container_stage_runtime_dlls(BimRoboticsScene)

if(ENABLE_PHYSICS)
    add_library(VulkanSceneRenderer_robotics_physics
        BimCollisionWorld.cpp BimCollisionOverlay.cpp BimRoomGraph.cpp BimRoomGeometry.cpp)
    target_link_libraries(VulkanSceneRenderer_robotics_physics PUBLIC
        VulkanSceneRenderer_robotics VulkanSceneRenderer_physics PRIVATE VulkanSceneRenderer_geometry)

    add_executable(BimRoboticsCollision ${CMAKE_SOURCE_DIR}/tools/bim_robotics_collision.cpp)
    target_link_libraries(BimRoboticsCollision PRIVATE
        VulkanSceneRenderer_robotics_physics VulkanSceneRenderer_geometry)
    set_target_properties(BimRoboticsCollision PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    container_stage_runtime_dlls(BimRoboticsCollision)
endif()
