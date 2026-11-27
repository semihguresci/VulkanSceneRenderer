#!/usr/bin/env python3
"""Drive the headless BIM robot through a doorway with Jazzy Nav2 and record the ROS graph."""

import argparse
from contextlib import ExitStack
from copy import deepcopy
import json
import math
import os
from pathlib import Path
import random
import signal
import subprocess
import time

from bim_nav2_map import CASES, load_map
from bim_nav2_planner import (
    FRAME, PLANNER, Waypoint, activate, child_process, cost_classes, guarded_cells,
    planner_parameters, read_fixture, read_json, require, spin_until, stamped_pose,
    verify_occupancy,
)
from bim_ros2_bag import BAG_TOPICS, verify_and_replay_bag


CONTROLLER = "DoorwayFollowPath"
GOAL_CHECKER = "waypoint_goal_checker"
PROGRESS_CHECKER = "waypoint_progress_checker"
GOAL_TOLERANCE = .14


def nav_parameters(fixture):
    params = planner_parameters(fixture)
    params["planner_server"]["ros__parameters"]["use_sim_time"] = True
    global_costmap = params["global_costmap"]["global_costmap"]["ros__parameters"]
    global_costmap["use_sim_time"] = True
    # The exported map already includes physical capsule clearance. This
    # footprint is the controller's center probe in that buffered map.
    local_costmap = deepcopy(global_costmap)
    local_costmap.update(robot_radius=.025, plugins=["static_layer"])
    local_costmap.pop("inflation_layer")
    params["local_costmap"] = {"local_costmap": {"ros__parameters": local_costmap}}
    params["controller_server"] = {"ros__parameters": {
        "use_sim_time": True, "enable_stamped_cmd_vel": False,
        "controller_frequency": 10.0, "costmap_update_timeout": 3.0, "odom_topic": "/odom",
        "controller_plugins": [CONTROLLER], "goal_checker_plugins": [GOAL_CHECKER],
        "progress_checker_plugins": [PROGRESS_CHECKER],
        GOAL_CHECKER: {"plugin": "nav2_controller::SimpleGoalChecker", "stateful": True,
                       "xy_goal_tolerance": .12, "yaw_goal_tolerance": .25},
        PROGRESS_CHECKER: {"plugin": "nav2_controller::SimpleProgressChecker",
                           "required_movement_radius": .12, "movement_time_allowance": 10.0},
        CONTROLLER: {
            "plugin": "nav2_regulated_pure_pursuit_controller::RegulatedPurePursuitController",
            "desired_linear_vel": .40, "lookahead_dist": .35,
            "use_velocity_scaled_lookahead_dist": False, "transform_tolerance": .4,
            "use_collision_detection": True, "use_cost_regulated_linear_velocity_scaling": False,
            "max_allowed_time_to_collision_up_to_carrot": .8, "min_approach_linear_velocity": .10,
            "use_rotate_to_heading": True, "rotate_to_heading_angular_vel": .65,
        },
    }}
    return params


def action(node, executor, deadline, processes, action_type, name, request):
    from action_msgs.msg import GoalStatus
    from rclpy.action import ActionClient

    client = ActionClient(node, action_type, name)
    try:
        spin_until(executor, deadline, processes, f"{name} server", client.server_is_ready)
        pending = client.send_goal_async(request)
        spin_until(executor, deadline, processes, f"{name} acceptance", pending.done)
        handle = pending.result()
        require(handle.accepted, f"{name} rejected the goal")
        pending = handle.get_result_async()
        spin_until(executor, deadline, processes, f"{name} completion", pending.done)
        result = pending.result()
        require(result.status == GoalStatus.STATUS_SUCCEEDED and result.result.error_code == result.result.NONE,
                f"{name} failed with {result.status}/{result.result.error_code}: {result.result.error_msg}")
        return result.result
    finally:
        client.destroy()


def position(buffer):
    from rclpy.time import Time

    tf = buffer.lookup_transform(FRAME, "base_link", Time()).transform
    t, q = tf.translation, tf.rotation
    yaw = math.atan2(2 * (q.w * q.z + q.x * q.y), 1 - 2 * (q.y * q.y + q.z * q.z))
    return [t.x, t.y, t.z], yaw


def drive_leg(node, executor, deadline, processes, buffer, waypoint, grid, floor):
    from nav2_msgs.action import ComputePathToPose, FollowPath

    start, yaw = position(buffer)
    stamp = node.get_clock().now().to_msg()
    plan = ComputePathToPose.Goal()
    plan.planner_id, plan.use_start = PLANNER, True
    plan.start = stamped_pose(Waypoint(start[0], start[1], yaw), stamp)
    plan.goal = stamped_pose(waypoint, stamp)
    path = action(node, executor, deadline, processes, ComputePathToPose, "/compute_path_to_pose", plan).path
    require(path.header.frame_id == FRAME and len(path.poses) >= 2, "Nav2 returned an invalid BIM path")
    xy = [(p.pose.position.x, p.pose.position.y) for p in path.poses]
    require(all(grid.covers_segment(a, b) for a, b in zip(xy, xy[1:])), "Planned route leaves the free map")
    follow = FollowPath.Goal()
    follow.path = path
    follow.controller_id, follow.goal_checker_id, follow.progress_checker_id = CONTROLLER, GOAL_CHECKER, PROGRESS_CHECKER
    action(node, executor, deadline, processes, FollowPath, "/follow_path", follow)
    arrived, _ = position(buffer)
    error = math.dist(arrived[:2], (waypoint.x, waypoint.y))
    require(error <= GOAL_TOLERANCE and abs(arrived[2] - floor) <= .02,
            f"Robot failed the map-frame waypoint at {waypoint}: {arrived}")
    return {"requestedMeters": [waypoint.x, waypoint.y], "actualMeters": arrived,
            "positionErrorMeters": error, "plannedPathMeters": xy}


def executables():
    from ament_index_python.packages import get_package_prefix

    pairs = {"map_server": ("nav2_map_server", "map_server"),
             "planner_server": ("nav2_planner", "planner_server"),
             "controller_server": ("nav2_controller", "controller_server"),
             "bridge": ("bim_ros_bridge", "bridge")}
    result = {name: Path(get_package_prefix(package)) / "lib" / package / exe
              for name, (package, exe) in pairs.items()}
    require(all(exe.is_file() for exe in result.values()), f"Missing Jazzy executables: {result}")
    return result


def run_demo(args, fixture, manifest, grid, scenario, camera):
    import rclpy
    from nav_msgs.msg import OccupancyGrid
    from rclpy.context import Context
    from rclpy.executors import SingleThreadedExecutor
    from rclpy.parameter import Parameter
    from rclpy.qos import DurabilityPolicy, QoSProfile, ReliabilityPolicy
    from rclpy.time import Time
    from tf2_ros import Buffer, TransformListener
    import yaml

    out, bins = args.output, executables()
    params = out / "nav2.yaml"
    params.write_text(yaml.safe_dump(nav_parameters(fixture)), encoding="utf-8")
    model = args.fixtures / "doorway.ifc"
    domain = random.SystemRandom().randrange(10, 100)
    env = dict(os.environ, ROS_DOMAIN_ID=str(domain), ROS_AUTOMATIC_DISCOVERY_RANGE="LOCALHOST",
               ROS_LOG_DIR=str(out / "ros-logs"))
    env.pop("ROS_STATIC_PEERS", None)
    os.environ.update(ROS_AUTOMATIC_DISCOVERY_RANGE="LOCALHOST")
    os.environ.pop("ROS_STATIC_PEERS", None)
    deadline = time.monotonic() + args.timeout
    context = Context()
    rclpy.init(args=[], context=context, domain_id=domain)
    try:
        with ExitStack() as stack:
            node = rclpy.create_node("bim_nav2_drive_acceptance", context=context,
                                     parameter_overrides=[Parameter("use_sim_time", value=True)])
            stack.callback(node.destroy_node)
            executor = SingleThreadedExecutor(context=context)
            executor.add_node(node)
            stack.callback(executor.shutdown)
            buffer = Buffer(node=node)
            listener = TransformListener(buffer, node, spin_thread=False)
            stack.callback(listener.unregister)
            topics = {}
            qos = QoSProfile(depth=1, reliability=ReliabilityPolicy.RELIABLE,
                             durability=DurabilityPolicy.TRANSIENT_LOCAL)
            node.create_subscription(OccupancyGrid, "/map", lambda msg: topics.update(map=msg), qos)
            node.create_subscription(OccupancyGrid, "/global_costmap/costmap", lambda msg: topics.update(costmap=msg), qos)

            def launch(name, command):
                return stack.enter_context(child_process(command, out / f"{name}.log", env))

            processes = {}
            recorder = launch("record", ["ros2", "bag", "record", "--use-sim-time", "--storage", "mcap",
                                         "--disable-keyboard-controls", "--output", out / "observations",
                                         "--topics", *BAG_TOPICS])
            processes["recorder"] = recorder
            processes["bridge"] = launch("bridge", [
                bins["bridge"], "--sim-executable", args.sim_executable, "--scene", model,
                "--namespace", manifest["sourceNamespace"], "--scenario", scenario, "--camera", camera,
                "--map-manifest", fixture.map_yaml.parent / "waypoints.json", "--trace", out / "drive.jsonl",
            ])
            spin_until(executor, deadline, processes, "clock and robot TF",
                       lambda: node.get_clock().now().nanoseconds > 1_000_000_000 and
                       buffer.can_transform(FRAME, "base_link", Time()))
            start, _ = position(buffer)
            require(math.dist(start, [fixture.start.x, fixture.start.y, manifest["floorMeters"]]) < .02,
                    f"Spawn does not match the BIM map: {start}")
            processes["map_server"] = launch("map_server", [
                bins["map_server"], "--ros-args", "-p", "use_sim_time:=true", "-p", f"yaml_filename:={fixture.map_yaml}",
            ])
            activate(node, executor, deadline, processes, "map_server")
            spin_until(executor, deadline, processes, "/map", lambda: "map" in topics)
            verify_occupancy(topics["map"], fixture, "/map")
            for name in ("planner_server", "controller_server"):
                processes[name] = launch(name, [bins[name], "--ros-args", "--params-file", params])
                activate(node, executor, deadline, processes, name)
            guarded = guarded_cells(fixture)
            spin_until(executor, deadline, processes, "NavFn planning guard",
                       lambda: "costmap" in topics and cost_classes(topics["costmap"].data) == guarded)

            itinerary = (Waypoint(.65, fixture.goal.y, 0.0), fixture.goal)
            legs = []
            for waypoint in itinerary:
                legs.append(drive_leg(node, executor, deadline, processes, buffer, waypoint, grid, manifest["floorMeters"]))
                print(f"Reached BIM waypoint ({waypoint.x:.2f}, {waypoint.y:.2f}) m, "
                      f"position error {legs[-1]['positionErrorMeters']:.3f} m", flush=True)

            recorder.send_signal(signal.SIGINT)
            require(recorder.wait(timeout=15) in (0, -signal.SIGINT), "rosbag2 did not finalize the recording")
            require((out / "observations" / "metadata.yaml").is_file(), "MCAP recording metadata is missing")
        return {"case": fixture.id, "sceneSource": manifest["sceneSource"], "imageSha256": fixture.image_hash,
                "floorMeters": manifest["floorMeters"], "startMeters": start, "goals": legs}
    finally:
        rclpy.try_shutdown(context=context)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sim-executable", type=Path, required=True)
    parser.add_argument("--maps", type=Path, required=True, help="Fixture bundle containing cases.json")
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="New directory for the drive trace and bag")
    parser.add_argument("--timeout", type=float, default=75.0, help="Wall-clock limit for the live ROS demo")
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")
    try:
        args.maps, args.fixtures, args.sim_executable = (path.resolve(strict=True)
                                                         for path in (args.maps, args.fixtures, args.sim_executable))
        args.output = args.output.resolve()
        args.output.mkdir(parents=True)
        case = next(case for case in CASES if case.id == "doorway-open")
        row = next(row for row in read_json(args.maps / "cases.json")["cases"] if row["id"] == case.id)
        fixture = read_fixture(args.maps, row)
        manifest, grid = load_map(case, args.fixtures, fixture.map_yaml.parent)
        scenario, camera = (args.fixtures / name for name in ("doorway-ros2-drive.json", "doorway-ros2-camera.json"))
        live = run_demo(args, fixture, manifest, grid, scenario, camera)
        (args.output / "navigation.json").write_text(json.dumps(live, indent=2) + "\n", encoding="utf-8")
        verify_and_replay_bag(args.output / "observations", args.output / "bag-verification.json")
        return 0
    except (ImportError, KeyError, OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"BIM Nav2 demo failed: {error}. Evidence: {args.output}\n")


if __name__ == "__main__":
    raise SystemExit(main())
