#!/usr/bin/env python3
"""Verify exported BIM fixtures with the ROS 2 Jazzy Nav2 map and planner servers."""

import argparse
from collections import Counter
from contextlib import ExitStack, contextmanager
from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import random
import re
import subprocess
import sys
import time


FRAME = "map"
PLANNER = "GridBased"
PGM_OCCUPANCY = {0: 100, 127: -1, 255: 0}
CASE_ID = re.compile(r"[a-z0-9][a-z0-9-]*")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


@dataclass(frozen=True)
class Waypoint:
    x: float
    y: float
    yaw: float


@dataclass(frozen=True)
class Fixture:
    id: str
    expected: str
    map_yaml: Path
    image_hash: str
    width: int
    height: int
    resolution: float
    origin: tuple[float, float, float]
    cells: tuple[int, ...]
    start: Waypoint
    goal: Waypoint


def read_binary_pgm(data, width, height):
    header = re.match(rb"P5[ \t\r\n]+(\d+)[ \t\r\n]+(\d+)[ \t\r\n]+255\r?\n", data)
    require(header and tuple(map(int, header.groups())) == (width, height), "Invalid P5 map header or dimensions")
    pixels = data[header.end():]
    require(len(pixels) == width * height and set(pixels) <= PGM_OCCUPANCY.keys(), "Invalid P5 map pixels")
    return tuple(PGM_OCCUPANCY[pixel]
                 for offset in range((height - 1) * width, -1, -width)
                 for pixel in pixels[offset:offset + width])


def read_fixture(root, row):
    import yaml

    name, location, expected = row["id"], row["directory"], row["expected"]
    require(CASE_ID.fullmatch(name) and CASE_ID.fullmatch(location), "Invalid case name or directory")
    require(expected in {"aperture", "detour", "unreachable", "direct"}, f"Unknown expected route {expected}")
    directory = root / location
    manifest = read_json(directory / "waypoints.json")
    require(manifest["schema"] == "container.bim-navigation-map" and manifest["version"] == 1,
            "Expected version 1 BIM navigation map manifest")
    require(manifest["frameId"] == FRAME, f"Waypoint frame must be {FRAME}")
    width, height = manifest["sizeCells"]
    resolution = manifest["resolutionMeters"]
    origin = tuple(manifest["originMeters"])
    require(type(width) is int and type(height) is int and width > 0 and height > 0 and
            math.isfinite(resolution) and resolution > 0 and len(origin) == 3 and origin[2] == 0 and
            all(map(math.isfinite, origin)), "Invalid manifest geometry")
    map_yaml = directory / "map.yaml"
    config = yaml.safe_load(map_yaml.read_text(encoding="utf-8"))
    require(config["mode"] == "trinary" and config["negate"] == 0 and config["free_thresh"] == .25 and
            config["occupied_thresh"] == .65 and tuple(config["origin"]) == origin and
            config["resolution"] == resolution, "Map YAML does not match the trinary manifest geometry")
    image_name = config["image"]
    require(Path(image_name).name == image_name and image_name.endswith(".pgm"), "Expected a local PGM image name")
    image = (directory / image_name).read_bytes()
    image_hash = hashlib.sha256(image).hexdigest()
    require(image_hash == manifest["imageSha256"], "PGM SHA-256 differs from the waypoint manifest")
    cells = read_binary_pgm(image, width, height)
    counts = Counter(cells)
    require(manifest["cells"] == {"free": counts[0], "occupied": counts[100], "unknown": counts[-1]},
            "PGM cell counts differ from the waypoint manifest")

    waypoints = {}
    for entry in manifest["waypoints"]:
        key = entry["id"]
        require(key not in waypoints, f"Duplicate waypoint {key}")
        x, y = entry["positionMeters"]
        yaw = entry["yawRadians"]
        require(all(map(math.isfinite, (x, y, yaw))), f"Non-finite waypoint {key}")
        col = math.floor((x - origin[0]) / resolution)
        row_index = math.floor((y - origin[1]) / resolution)
        require(0 <= col < width and 0 <= row_index < height and cells[row_index * width + col] == 0,
                f"Waypoint {key} is outside the free map")
        waypoints[key] = Waypoint(float(x), float(y), float(yaw))
    require(waypoints.keys() == {"start", "goal"}, "Expected exactly the start and goal waypoints")
    return Fixture(name, expected, map_yaml, image_hash, width, height, resolution, origin, cells,
                   waypoints["start"], waypoints["goal"])


def verify_occupancy(grid, fixture, label):
    info = grid.info
    require(grid.header.frame_id == FRAME, f"{label} published frame {grid.header.frame_id!r}")
    require((info.width, info.height) == (fixture.width, fixture.height), f"{label} dimensions differ from PGM")
    require(math.isclose(info.resolution, fixture.resolution, rel_tol=1e-6), f"{label} resolution differs from PGM")
    origin = info.origin.position
    require(all(math.isclose(actual, expected, abs_tol=1e-6)
                for actual, expected in zip((origin.x, origin.y, origin.z), fixture.origin, strict=True)),
            f"{label} origin differs from map YAML")
    orientation = info.origin.orientation
    require((orientation.x, orientation.y, orientation.z, orientation.w) == (0, 0, 0, 1),
            f"{label} has a rotated origin")
    require(len(grid.data) == len(fixture.cells), f"{label} forwarded the wrong number of map cells")
    for index, (actual, expected) in enumerate(zip(grid.data, fixture.cells, strict=True)):
        if actual != expected:
            raise ValueError(f"{label} cell ({index % info.width}, {index // info.width}) is {actual}, expected {expected}")


def planner_parameters(fixture):
    return {
        "planner_server": {"ros__parameters": {
            "planner_plugins": [PLANNER], "costmap_update_timeout": 3.0,
            PLANNER: {"plugin": "nav2_navfn_planner::NavfnPlanner", "allow_unknown": False,
                      "tolerance": 0.0, "use_astar": False},
        }},
        "global_costmap": {"global_costmap": {"ros__parameters": {
            "global_frame": FRAME, "robot_base_frame": "base_link", "rolling_window": False,
            "resolution": fixture.resolution, "track_unknown_space": True,
            "robot_radius": 0.01, "footprint_padding": 0.0,
            "update_frequency": 10.0, "publish_frequency": 10.0, "always_send_full_costmap": True,
            "plugins": ["static_layer"],
            "static_layer": {"plugin": "nav2_costmap_2d::StaticLayer", "map_topic": "/map",
                             "map_subscribe_transient_local": True},
        }}},
    }


@contextmanager
def child_process(command, log, env):
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT,
                                   env=env)
        try:
            yield process
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)


def spin_until(executor, deadline, processes, what, ready):
    while not ready():
        for name, process in processes.items():
            code = process.poll()
            if code is not None:
                raise RuntimeError(f"{name} exited with code {code} while waiting for {what}")
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RuntimeError(f"Timed out waiting for {what}")
        executor.spin_once(timeout_sec=min(0.1, remaining))


def activate(node, executor, deadline, processes, name):
    from lifecycle_msgs.msg import Transition
    from lifecycle_msgs.srv import ChangeState

    client = node.create_client(ChangeState, f"/{name}/change_state")
    spin_until(executor, deadline, processes, f"{name} lifecycle service", client.service_is_ready)
    for transition_id, phase in ((Transition.TRANSITION_CONFIGURE, "configure"),
                                 (Transition.TRANSITION_ACTIVATE, "activate")):
        request = ChangeState.Request()
        request.transition.id = transition_id
        future = client.call_async(request)
        spin_until(executor, deadline, processes, f"{name} to {phase}", future.done)
        require(future.result().success, f"{name} failed to {phase}")


def stamped_pose(waypoint, stamp):
    from geometry_msgs.msg import PoseStamped

    pose = PoseStamped()
    pose.header.frame_id = FRAME
    pose.header.stamp = stamp
    pose.pose.position.x = waypoint.x
    pose.pose.position.y = waypoint.y
    pose.pose.orientation.z = math.sin(waypoint.yaw / 2)
    pose.pose.orientation.w = math.cos(waypoint.yaw / 2)
    return pose


def json_header(header):
    return {"frameId": header.frame_id, "stamp": [header.stamp.sec, header.stamp.nanosec]}


def json_pose(message):
    pos, rot = message.pose.position, message.pose.orientation
    return {"header": json_header(message.header), "positionMeters": [pos.x, pos.y, pos.z],
            "orientationXyzw": [rot.x, rot.y, rot.z, rot.w]}


def plan_path(node, executor, deadline, processes, fixture):
    from action_msgs.msg import GoalStatus
    from nav2_msgs.action import ComputePathToPose

    from rclpy.action import ActionClient

    client = ActionClient(node, ComputePathToPose, "/compute_path_to_pose")
    spin_until(executor, deadline, processes, "compute_path_to_pose action", client.server_is_ready)
    goal = ComputePathToPose.Goal()
    goal.planner_id = PLANNER
    goal.use_start = True
    stamp = node.get_clock().now().to_msg()
    goal.start = stamped_pose(fixture.start, stamp)
    goal.goal = stamped_pose(fixture.goal, stamp)
    accepted = client.send_goal_async(goal)
    spin_until(executor, deadline, processes, "Nav2 goal acceptance", accepted.done)
    handle = accepted.result()
    require(handle.accepted, "Nav2 rejected the planning action")
    future = handle.get_result_async()
    spin_until(executor, deadline, processes, "Nav2 planning result", future.done)
    reply = future.result()
    result = reply.result
    poses = [json_pose(pose) for pose in result.path.poses]
    diagnostic = f"action status {reply.status}, Nav2 error {result.error_code}: {result.error_msg}"
    if fixture.expected == "unreachable":
        require(reply.status == GoalStatus.STATUS_ABORTED and result.error_code == result.NO_VALID_PATH and not poses,
                f"Disconnected map should return an empty NO_VALID_PATH result, received {diagnostic}")
        status = "unreachable"
    else:
        require(reply.status == GoalStatus.STATUS_SUCCEEDED and result.error_code == result.NONE and len(poses) >= 2,
                f"Connected map should return a path, received {diagnostic}")
        require(result.path.header.frame_id == FRAME and all(pose["header"]["frameId"] == FRAME for pose in poses),
                "Nav2 path poses are outside the map frame")
        for pose in poses:
            require(all(map(math.isfinite, (*pose["positionMeters"], *pose["orientationXyzw"]))),
                    "Nav2 returned a non-finite pose")
        # NavFn samples the start on its grid. The native fixture verifier replays the exact returned coordinates.
        for pose, waypoint in ((poses[0], fixture.start), (poses[-1], fixture.goal)):
            require(math.dist(pose["positionMeters"][:2], (waypoint.x, waypoint.y)) <= 2 * fixture.resolution,
                    "Nav2 returned an endpoint outside two cells of the requested waypoint")
        status = "succeeded"
    return {"id": fixture.id, "imageSha256": fixture.image_hash, "frameId": FRAME, "status": status,
            "pathMeters": [pose["positionMeters"][:2] for pose in poses], "poses": poses,
            "pathHeader": json_header(result.path.header), "actionStatus": reply.status,
            "errorCode": result.error_code, "errorMessage": result.error_msg,
            "planningTimeNanoseconds": result.planning_time.sec * 1_000_000_000 + result.planning_time.nanosec}


def run_fixture(fixture, executables, domain, log_dir, timeout):
    from geometry_msgs.msg import TransformStamped
    from nav_msgs.msg import OccupancyGrid
    import rclpy
    from rclpy.context import Context
    from rclpy.executors import SingleThreadedExecutor
    from rclpy.qos import DurabilityPolicy, QoSProfile, ReliabilityPolicy
    from tf2_ros.static_transform_broadcaster import StaticTransformBroadcaster
    import yaml

    log_dir.mkdir(parents=True, exist_ok=True)
    params = log_dir / "planner-params.yaml"
    params.write_text(yaml.safe_dump(planner_parameters(fixture)), encoding="utf-8")
    deadline = time.monotonic() + timeout
    context = Context()
    rclpy.init(args=[], context=context, domain_id=domain)
    try:
        with ExitStack() as stack:
            node = rclpy.create_node("bim_nav2_acceptance", context=context)
            stack.callback(node.destroy_node)
            executor = SingleThreadedExecutor(context=context)
            executor.add_node(node)
            stack.callback(executor.shutdown)
            topics = {}
            qos = QoSProfile(depth=1, reliability=ReliabilityPolicy.RELIABLE,
                             durability=DurabilityPolicy.TRANSIENT_LOCAL)
            node.create_subscription(OccupancyGrid, "/map", lambda msg: topics.update(map=msg), qos)
            node.create_subscription(OccupancyGrid, "/global_costmap/costmap", lambda msg: topics.update(costmap=msg), qos)
            broadcaster = StaticTransformBroadcaster(node)
            transform = TransformStamped()
            transform.header.frame_id = FRAME
            transform.header.stamp = node.get_clock().now().to_msg()
            transform.child_frame_id = "base_link"
            transform.transform.translation.x = fixture.start.x
            transform.transform.translation.y = fixture.start.y
            transform.transform.rotation.z = math.sin(fixture.start.yaw / 2)
            transform.transform.rotation.w = math.cos(fixture.start.yaw / 2)
            broadcaster.sendTransform(transform)

            env = dict(os.environ, ROS_DOMAIN_ID=str(domain), ROS_LOG_DIR=str(log_dir))
            map_server = stack.enter_context(child_process(
                [executables["map_server"], "--ros-args", "-p", f"yaml_filename:={fixture.map_yaml}"],
                log_dir / "map_server.stdout.log", env))
            processes = {"map_server": map_server}
            activate(node, executor, deadline, processes, "map_server")
            spin_until(executor, deadline, processes, "/map occupancy grid", lambda: "map" in topics)
            verify_occupancy(topics["map"], fixture, "/map")

            processes["planner_server"] = stack.enter_context(child_process(
                [executables["planner_server"], "--ros-args", "--params-file", params],
                log_dir / "planner_server.stdout.log", env))
            activate(node, executor, deadline, processes, "planner_server")
            spin_until(executor, deadline, processes, "global costmap to receive the static map",
                       lambda: "costmap" in topics and tuple(topics["costmap"].data) == fixture.cells)
            verify_occupancy(topics["costmap"], fixture, "global costmap")
            return plan_path(node, executor, deadline, processes, fixture)
    finally:
        rclpy.try_shutdown(context=context)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--maps", type=Path, required=True, help="Export directory containing cases.json")
    parser.add_argument("--output", type=Path, required=True, help="Destination for planner results")
    parser.add_argument("--log-dir", type=Path, help="Per-case ROS logs (default: next to the result)")
    parser.add_argument("--timeout", type=float, default=35.0, help="Startup and planning time per case, in seconds")
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")
    try:
        from ament_index_python.packages import get_package_prefix

        executables = {name: Path(get_package_prefix(package)) / "lib" / package / name
                       for name, package in (("map_server", "nav2_map_server"), ("planner_server", "nav2_planner"))}
        for path in executables.values():
            require(path.is_file(), f"Missing Nav2 executable {path}")
        logs = (args.log_dir or args.output.with_suffix(".logs")).resolve()
        maps = args.maps.resolve(strict=True)
        index = read_json(maps / "cases.json")
        require(index["schema"] == "container.bim-nav2-map-cases" and index["version"] == 1,
                "Expected version 1 BIM Nav2 case index")
        rows = index["cases"]
        require(isinstance(rows, list) and 0 < len(rows) <= 92 and len({row["id"] for row in rows}) == len(rows),
                "Case index needs unique IDs and 1–92 cases")
        os.environ["ROS_AUTOMATIC_DISCOVERY_RANGE"] = "LOCALHOST"
        os.environ.pop("ROS_STATIC_PEERS", None)
        # A new localhost ROS domain per case keeps late DDS samples out of the next fixture.
        domains = random.SystemRandom().sample(range(10, 102), len(rows))
        results = []
        for row, domain in zip(rows, domains, strict=True):
            try:
                fixture = read_fixture(maps, row)
                result = run_fixture(fixture, executables, domain, logs / fixture.id, args.timeout)
            except (OSError, KeyError, TypeError, ValueError, RuntimeError) as error:
                raise RuntimeError(f"Case {row['id']}: {error}. Logs: {logs}") from error
            print(f"{fixture.id}: {result['status']} ({len(result['poses'])} poses)", flush=True)
            results.append(result)
        report = {"schema": "container.bim-nav2-planner-results", "version": 1, "cases": results}
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, allow_nan=False, indent=2) + "\n", encoding="utf-8")
    except ImportError as error:
        parser.exit(1, f"Missing ROS 2 Jazzy Python dependencies. Source /opt/ros/jazzy/setup.bash: {error}\n")
    except (OSError, KeyError, TypeError, ValueError, RuntimeError) as error:
        parser.exit(1, f"Nav2 acceptance failed: {error}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
