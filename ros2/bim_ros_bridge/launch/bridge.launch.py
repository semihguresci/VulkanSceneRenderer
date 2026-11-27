from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def start_bridge(context):
    arguments = []
    for name in ("sim_executable", "scene", "namespace", "scenario", "camera", "map_manifest", "trace"):
        value = LaunchConfiguration(name).perform(context)
        if value:
            arguments.extend(("--" + name.replace("_", "-"), value))
    return [Node(package="bim_ros_bridge", executable="bridge", arguments=arguments, output="screen")]


def generate_launch_description():
    return LaunchDescription([
        DeclareLaunchArgument("sim_executable", description="Path to the built BimRoboticsSim executable"),
        DeclareLaunchArgument("scene", description="Path to the BIM source"),
        DeclareLaunchArgument("namespace", description="Semantic BIM source namespace"),
        DeclareLaunchArgument("scenario", description="Path to the fixed-step stream scenario"),
        DeclareLaunchArgument("camera", default_value="", description="Optional calibrated camera JSON"),
        DeclareLaunchArgument("map_manifest", default_value="", description="Optional exported waypoints.json"),
        DeclareLaunchArgument("trace", default_value="", description="Optional new native trace output path"),
        OpaqueFunction(function=start_bridge),
    ])
