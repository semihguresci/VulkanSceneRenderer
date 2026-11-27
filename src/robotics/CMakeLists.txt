add_library(VulkanSceneRenderer_robotics_clock SimClock.cpp)

add_library(VulkanSceneRenderer_robotics_scene RoboticsScene.cpp RoboticsSceneManifest.cpp BimSensorLabels.cpp BimSceneSource.cpp)
target_link_libraries(VulkanSceneRenderer_robotics_scene PUBLIC Dep_Math
    PRIVATE Vulkan::Headers nlohmann_json::nlohmann_json)

add_library(VulkanSceneRenderer_robotics_collision_geometry BimCollisionGeometry.cpp BimWalkability.cpp)
target_link_libraries(VulkanSceneRenderer_robotics_collision_geometry PUBLIC VulkanSceneRenderer_robotics_scene
    PRIVATE Vulkan::Headers)

add_library(VulkanSceneRenderer_robotics_usd
    RoboticsOpenUsd.cpp RoboticsOpenUsdGeometry.cpp RoboticsOpenUsdWriter.cpp)
target_link_libraries(VulkanSceneRenderer_robotics_usd PUBLIC VulkanSceneRenderer_robotics_scene
    PRIVATE VulkanSceneRenderer_robotics_collision_geometry VulkanSceneRenderer_fingerprint Vulkan::Headers)

add_library(VulkanSceneRenderer_robotics_assets BimAssetCache.cpp BimCachedCollision.cpp BimCachedRender.cpp)
target_compile_definitions(VulkanSceneRenderer_robotics_assets PRIVATE
    CONTAINER_BIM_PREPROCESSOR_REVISION="${CONTAINER_GIT_REVISION}")
target_link_libraries(VulkanSceneRenderer_robotics_assets
    PUBLIC VulkanSceneRenderer_robotics_scene VulkanSceneRenderer_robotics_collision_geometry Vulkan::Headers
    PRIVATE VulkanSceneRenderer_geometry VulkanSceneRenderer_fingerprint tinygltf::tinygltf)
if(TARGET miniz::miniz)
    target_link_libraries(VulkanSceneRenderer_robotics_assets PRIVATE miniz::miniz)
else()
    find_path(CONTAINER_BIM_MINIZ_INCLUDE_DIR NAMES miniz/miniz.h REQUIRED)
    find_library(CONTAINER_TINYEXR_MINIZ_LIBRARY NAMES miniz REQUIRED)
    target_include_directories(VulkanSceneRenderer_robotics_assets PRIVATE "${CONTAINER_BIM_MINIZ_INCLUDE_DIR}")
    target_link_libraries(VulkanSceneRenderer_robotics_assets PRIVATE "${CONTAINER_TINYEXR_MINIZ_LIBRARY}")
endif()

add_library(VulkanSceneRenderer_robotics_drive DifferentialDrive.cpp DifferentialDriveActuation.cpp)
target_link_libraries(VulkanSceneRenderer_robotics_drive PUBLIC Dep_Math)

add_library(VulkanSceneRenderer_robotics_sensors
    DriveSensors.cpp RaycastLidar.cpp SensorSchedule.cpp SensorCamera.cpp DatasetSimulationReport.cpp)
target_link_libraries(VulkanSceneRenderer_robotics_sensors PUBLIC VulkanSceneRenderer_robotics_drive
    PRIVATE nlohmann_json::nlohmann_json VulkanSceneRenderer_fingerprint)

add_library(VulkanSceneRenderer_robotics_dataset RoboticsDatasetReader.cpp RoboticsDatasetWriter.cpp)
target_link_libraries(VulkanSceneRenderer_robotics_dataset PUBLIC nlohmann_json::nlohmann_json
    PRIVATE VulkanSceneRenderer_robotics_sensors VulkanSceneRenderer_fingerprint)

foreach(target IN ITEMS
        VulkanSceneRenderer_robotics_clock
        VulkanSceneRenderer_robotics_scene
        VulkanSceneRenderer_robotics_collision_geometry
        VulkanSceneRenderer_robotics_usd
        VulkanSceneRenderer_robotics_assets
        VulkanSceneRenderer_robotics_drive
        VulkanSceneRenderer_robotics_sensors
        VulkanSceneRenderer_robotics_dataset)
    target_compile_features(${target} PUBLIC cxx_std_23)
    target_include_directories(${target} PUBLIC ${CMAKE_SOURCE_DIR}/include)
endforeach()

add_executable(BimRoboticsScene ${CMAKE_SOURCE_DIR}/tools/bim_robotics_scene.cpp)
target_link_libraries(BimRoboticsScene PRIVATE
    VulkanSceneRenderer_robotics_scene VulkanSceneRenderer_geometry)
set_target_properties(BimRoboticsScene PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
container_stage_runtime_dlls(BimRoboticsScene)

add_executable(BimRoboticsUsd ${CMAKE_SOURCE_DIR}/tools/bim_robotics_usd.cpp)
target_link_libraries(BimRoboticsUsd PRIVATE
    VulkanSceneRenderer_robotics_usd VulkanSceneRenderer_geometry)
set_target_properties(BimRoboticsUsd PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
container_stage_runtime_dlls(BimRoboticsUsd)

add_executable(BimRoboticsAssets ${CMAKE_SOURCE_DIR}/tools/bim_robotics_assets.cpp)
target_link_libraries(BimRoboticsAssets PRIVATE
    VulkanSceneRenderer_robotics_assets nlohmann_json::nlohmann_json)
set_target_properties(BimRoboticsAssets PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
container_stage_runtime_dlls(BimRoboticsAssets)

add_executable(BimRoboticsDataset ${CMAKE_SOURCE_DIR}/tools/bim_robotics_dataset.cpp)
target_link_libraries(BimRoboticsDataset PRIVATE VulkanSceneRenderer_robotics_dataset)
set_target_properties(BimRoboticsDataset PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
container_stage_runtime_dlls(BimRoboticsDataset)

if(ENABLE_ROBOTICS_URDF)
    find_package(tinyxml2 CONFIG REQUIRED)
    add_library(VulkanSceneRenderer_urdf UrdfModel.cpp UrdfRuntime.cpp)
    target_compile_features(VulkanSceneRenderer_urdf PUBLIC cxx_std_23)
    target_include_directories(VulkanSceneRenderer_urdf PUBLIC ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(VulkanSceneRenderer_urdf PUBLIC Dep_Math VulkanSceneRenderer_robotics_clock
        PRIVATE tinyxml2::tinyxml2)

    add_library(VulkanSceneRenderer_urdf_assets UrdfAssetExport.cpp UrdfAssetGeometry.cpp)
    target_compile_features(VulkanSceneRenderer_urdf_assets PUBLIC cxx_std_23)
    target_include_directories(VulkanSceneRenderer_urdf_assets PUBLIC ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(VulkanSceneRenderer_urdf_assets PUBLIC VulkanSceneRenderer_urdf
        PRIVATE VulkanSceneRenderer_geometry tinygltf::tinygltf)

    add_executable(BimRoboticsUrdf ${CMAKE_SOURCE_DIR}/tools/bim_robotics_urdf.cpp)
    target_link_libraries(BimRoboticsUrdf PRIVATE VulkanSceneRenderer_urdf_assets nlohmann_json::nlohmann_json)
    set_target_properties(BimRoboticsUrdf PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    container_stage_runtime_dlls(BimRoboticsUrdf)

    if(ENABLE_PHYSICS)
        add_library(VulkanSceneRenderer_urdf_physics UrdfCollisionBindings.cpp)
        target_link_libraries(VulkanSceneRenderer_urdf_physics PUBLIC
            VulkanSceneRenderer_urdf VulkanSceneRenderer_physics_world)
    endif()
endif()

if(ENABLE_PHYSICS)
    add_library(VulkanSceneRenderer_robotics_collision
        BimCollisionWorld.cpp BimRoomGraph.cpp BimRoomGeometry.cpp)
    target_link_libraries(VulkanSceneRenderer_robotics_collision
        PUBLIC VulkanSceneRenderer_robotics_collision_geometry VulkanSceneRenderer_robotics_scene
               VulkanSceneRenderer_physics_world
        PRIVATE Vulkan::Headers)

    add_library(VulkanSceneRenderer_robotics_simulation DifferentialDriveSimulation.cpp Articulation.cpp)
    target_link_libraries(VulkanSceneRenderer_robotics_simulation PUBLIC
        VulkanSceneRenderer_robotics_collision VulkanSceneRenderer_robotics_drive
        VulkanSceneRenderer_robotics_clock)

    add_library(VulkanSceneRenderer_robotics_diagnostics BimCollisionOverlay.cpp)
    target_link_libraries(VulkanSceneRenderer_robotics_diagnostics PRIVATE
        VulkanSceneRenderer_robotics_collision VulkanSceneRenderer_geometry)

    foreach(target IN ITEMS
            VulkanSceneRenderer_robotics_collision
            VulkanSceneRenderer_robotics_simulation
            VulkanSceneRenderer_robotics_diagnostics)
        target_compile_features(${target} PUBLIC cxx_std_23)
        target_include_directories(${target} PUBLIC ${CMAKE_SOURCE_DIR}/include)
    endforeach()

    add_executable(BimRoboticsCollision ${CMAKE_SOURCE_DIR}/tools/bim_robotics_collision.cpp)
    target_link_libraries(BimRoboticsCollision PRIVATE
        VulkanSceneRenderer_robotics_assets VulkanSceneRenderer_robotics_collision
        VulkanSceneRenderer_robotics_diagnostics VulkanSceneRenderer_geometry nlohmann_json::nlohmann_json)
    set_target_properties(BimRoboticsCollision PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    container_stage_runtime_dlls(BimRoboticsCollision)

    add_executable(BimRoboticsSim ${CMAKE_SOURCE_DIR}/tools/bim_robotics_sim.cpp)
    target_link_libraries(BimRoboticsSim PRIVATE
        VulkanSceneRenderer_robotics_assets VulkanSceneRenderer_robotics_simulation
        VulkanSceneRenderer_robotics_sensors nlohmann_json::nlohmann_json)
    set_target_properties(BimRoboticsSim PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    container_stage_runtime_dlls(BimRoboticsSim)
endif()
