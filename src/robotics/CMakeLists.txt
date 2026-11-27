add_library(VulkanSceneRenderer_robotics
    RoboticsScene.cpp RoboticsSceneManifest.cpp BimCollisionGeometry.cpp BimWalkability.cpp
    BimAssetCache.cpp BimCachedCollision.cpp BimCachedRender.cpp SimClock.cpp DifferentialDrive.cpp)
target_compile_features(VulkanSceneRenderer_robotics PUBLIC cxx_std_23)
target_include_directories(VulkanSceneRenderer_robotics PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(VulkanSceneRenderer_robotics PRIVATE
    CONTAINER_BIM_PREPROCESSOR_REVISION="${CONTAINER_GIT_REVISION}")
target_link_libraries(VulkanSceneRenderer_robotics PUBLIC Dep_Math
    PRIVATE VulkanSceneRenderer_geometry VulkanSceneRenderer_fingerprint nlohmann_json::nlohmann_json)
if(TARGET miniz::miniz)
    target_link_libraries(VulkanSceneRenderer_robotics PRIVATE miniz::miniz)
else()
    find_path(CONTAINER_BIM_MINIZ_INCLUDE_DIR NAMES miniz/miniz.h REQUIRED)
    find_library(CONTAINER_TINYEXR_MINIZ_LIBRARY NAMES miniz REQUIRED)
    target_include_directories(VulkanSceneRenderer_robotics PRIVATE "${CONTAINER_BIM_MINIZ_INCLUDE_DIR}")
    target_link_libraries(VulkanSceneRenderer_robotics PRIVATE "${CONTAINER_TINYEXR_MINIZ_LIBRARY}")
endif()

add_executable(BimRoboticsScene ${CMAKE_SOURCE_DIR}/tools/bim_robotics_scene.cpp)
target_link_libraries(BimRoboticsScene PRIVATE VulkanSceneRenderer_robotics VulkanSceneRenderer_geometry)
set_target_properties(BimRoboticsScene PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
container_stage_runtime_dlls(BimRoboticsScene)

add_executable(BimRoboticsAssets ${CMAKE_SOURCE_DIR}/tools/bim_robotics_assets.cpp)
target_link_libraries(BimRoboticsAssets PRIVATE
    VulkanSceneRenderer_robotics VulkanSceneRenderer_geometry nlohmann_json::nlohmann_json)
set_target_properties(BimRoboticsAssets PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
container_stage_runtime_dlls(BimRoboticsAssets)

if(ENABLE_PHYSICS)
    add_library(VulkanSceneRenderer_robotics_physics
        BimCollisionWorld.cpp BimCollisionOverlay.cpp BimRoomGraph.cpp BimRoomGeometry.cpp
        DifferentialDriveSimulation.cpp)
    target_link_libraries(VulkanSceneRenderer_robotics_physics PUBLIC
        VulkanSceneRenderer_robotics VulkanSceneRenderer_physics PRIVATE VulkanSceneRenderer_geometry)

    add_executable(BimRoboticsCollision ${CMAKE_SOURCE_DIR}/tools/bim_robotics_collision.cpp)
    target_link_libraries(BimRoboticsCollision PRIVATE
        VulkanSceneRenderer_robotics_physics VulkanSceneRenderer_geometry)
    set_target_properties(BimRoboticsCollision PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    container_stage_runtime_dlls(BimRoboticsCollision)

    add_executable(BimRoboticsSim ${CMAKE_SOURCE_DIR}/tools/bim_robotics_sim.cpp)
    target_link_libraries(BimRoboticsSim PRIVATE
        VulkanSceneRenderer_robotics_physics VulkanSceneRenderer_geometry)
    set_target_properties(BimRoboticsSim PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    container_stage_runtime_dlls(BimRoboticsSim)
endif()
