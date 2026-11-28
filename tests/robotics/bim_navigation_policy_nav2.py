#!/usr/bin/env python3
"""Run one headless BIM navigation trial using Jazzy NavFn and regulated pure pursuit."""

import argparse
from contextlib import ExitStack
from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
from random import SystemRandom
import shutil
import sys
import tempfile
import time
import traceback

from bim_nav2_planner import (
    FRAME, PLANNER, activate, child_process, cost_classes, guarded_cells, json_header, json_pose,
    read_fixture, read_json, require, spin_until, stamped_pose, verify_occupancy,
)
from bim_ros2_nav2_demo import CONTROLLER, GOAL_CHECKER, PROGRESS_CHECKER, executables, nav_parameters, position


TICKS_PER_SECOND = 60
CONTROL_HZ = 10.0
DRIVE_TICKS = 1800
GOAL_TOLERANCE = 0.25
LINEAR_LIMIT = 0.40
ANGULAR_LIMIT = 0.65
BLOCKED_TICKS = 18
ROS_EPOCH_NS = 1_000_000_000
CMD_INPUT = "/benchmark/nav2_cmd_vel"
START_TOLERANCE = 0.02


def write_json(path, report):
    path.write_text(json.dumps(report, allow_nan=False, indent=2) + "\n", encoding="utf-8")


def finite_vector(values, length):
    return isinstance(values, list) and len(values) == length and all(
        type(value) in (int, float) and math.isfinite(value) for value in values)


def angular_error(first, second):
    return abs(math.remainder(first - second, math.tau))


def load_trial(args):
    map_dir = args.map_directory
    fixture = read_fixture(map_dir.parent, {"id": map_dir.name, "directory": map_dir.name, "expected": "direct"})
    manifest = read_json(map_dir / "waypoints.json")
    scenario = read_json(args.scenario)
    spawn = scenario["initialFeet"]
    yaw = scenario.get("initialYaw", 0.0)
    require(scenario["version"] == 1 and scenario.get("ticksPerSecond", TICKS_PER_SECOND) == TICKS_PER_SECOND,
            f"Expected version-1 native scenario at {TICKS_PER_SECOND} Hz")
    require(finite_vector(spawn, 3) and type(yaw) in (int, float) and math.isfinite(yaw) and
            math.dist(spawn[:2], (fixture.start.x, fixture.start.y)) <= 1e-8 and
            abs(spawn[2] - manifest["floorMeters"]) <= manifest["floorToleranceMeters"] and
            angular_error(yaw, fixture.start.yaw) <= 1e-8, "Scenario spawn differs from the exported start pose")
    require(sorted(scenario.get("openDoorIds", [])) == sorted(manifest["openDoorIds"]),
            "Scenario doors differ from the exported map")
    limit = scenario["ticks"]
    require(type(limit) is int and limit > args.drive_ticks,
            "Native tick limit must accommodate the whole scored drive and separate ROS startup")
    return fixture, manifest, scenario


class Trace:
    """Hold exact native commits while the live bridge appends to its trace."""

    def __init__(self, path, limit):
        self.path = path
        self.limit = limit
        self.offset = 0
        self.partial = b""
        self.rows = []

    def refresh(self):
        if not self.path.is_file():
            return
        with self.path.open("rb") as stream:
            stream.seek(self.offset)
            block = stream.read()
            self.offset = stream.tell()
        if not block:
            return
        *lines, self.partial = (self.partial + block).split(b"\n")
        for line in lines:
            row = json.loads(line)
            tick = len(self.rows)
            stamp = tick * ROS_EPOCH_NS // TICKS_PER_SECOND
            require(row["schema"] == "container.bim-ros-bridge-trace" and row["version"] == 1 and
                    type(row["tick"]) is int and row["tick"] == tick <= self.limit and
                    row["timestampNanoseconds"] == stamp and row["rosTimestampNanoseconds"] == stamp + ROS_EPOCH_NS,
                    f"Missing or invalid native bridge trace row at tick {tick}")
            pose = row["base"]
            require(finite_vector(pose["position"], 3) and type(pose["yaw"]) in (int, float) and
                    math.isfinite(pose["yaw"]) and isinstance(row["obstruction"], str) and
                    isinstance(row["blockingEntityId"], str), f"Invalid native pose or obstruction at tick {tick}")
            command = row["command"]
            if tick == 0:
                require(command is None, "Initial native row contains an applied command")
            else:
                speeds = [command["linearMetersPerSecond"], command["angularRadiansPerSecond"]]
                require(finite_vector(speeds, 2) and abs(speeds[0]) <= LINEAR_LIMIT + 1e-9 and
                        abs(speeds[1]) <= ANGULAR_LIMIT + 1e-9,
                        f"Native command exceeded the common policy limits at tick {tick}: {speeds}")
            self.rows.append(row)

    @property
    def last_tick(self):
        return len(self.rows) - 1

    def check_stationary(self, end):
        base = self.rows[0]["base"]
        for row in self.rows[:end + 1]:
            command = row["command"]
            require(math.dist(row["base"]["position"], base["position"]) <= 1e-6 and
                    angular_error(row["base"]["yaw"], base["yaw"]) <= 1e-6 and
                    (command is None or not any(command.values())),
                    f"Native robot moved or took a command during setup at tick {row['tick']}")


@dataclass(frozen=True)
class Outcome:
    status: str
    tick: int
    reason: str


def goal_distance(row, goal):
    return math.dist(row["base"]["position"][:2], (goal.x, goal.y))


class DriveWindow:
    def __init__(self, begin, budget, goal, tolerance):
        self.next_tick = begin
        self.end = begin + budget
        self.goal = goal
        self.tolerance = tolerance
        self.obstruction_streak = 0

    def advance(self, rows):
        for row in rows[self.next_tick:self.end + 1]:
            tick, obstruction = row["tick"], row["obstruction"]
            self.next_tick = tick + 1
            if obstruction == "blocked":
                return Outcome("collision", tick, row["blockingEntityId"] or "blocked")
            if goal_distance(row, self.goal) <= self.tolerance:
                return Outcome("success", tick, "native pose reached the position target")
            self.obstruction_streak = 0 if obstruction == "none" else self.obstruction_streak + 1
            if self.obstruction_streak >= BLOCKED_TICKS:
                return Outcome("blocked", tick, f"sustained native obstruction: {obstruction}")
            if tick == self.end:
                return Outcome("time-budget", tick, "committed native drive budget exhausted")
        return None


class CommandGate:
    def __init__(self, publisher, twist_type):
        self.publisher = publisher
        self.twist_type = twist_type
        self.enabled = False
        self.forwarded = 0
        self.clamped = 0

    def relay(self, message):
        if not self.enabled:
            return
        raw = [message.linear.x, message.angular.z]
        require(finite_vector(raw, 2) and not any((message.linear.y, message.linear.z, message.angular.x,
                                                   message.angular.y)), "Nav2 emitted a non-planar/non-finite command")
        capped = [max(-limit, min(limit, value)) for value, limit in zip(raw, (LINEAR_LIMIT, ANGULAR_LIMIT), strict=True)]
        velocity = self.twist_type()
        velocity.linear.x, velocity.angular.z = capped
        self.publisher.publish(velocity)
        self.forwarded += 1
        self.clamped += raw != capped

    def stop(self):
        self.enabled = False
        self.publisher.publish(self.twist_type())


def action_goal(client, request, executor, processes, deadline, label):
    spin_until(executor, deadline, processes, f"{label} server", client.server_is_ready)
    pending = client.send_goal_async(request)
    spin_until(executor, deadline, processes, f"{label} acceptance", pending.done)
    handle = pending.result()
    require(handle is not None and handle.accepted, f"{label} refused a valid trial request")
    return handle, handle.get_result_async()


def supervise(executor, processes, deadline, check):
    while True:
        if (result := check()) is not None:
            return result
        for name, process in processes.items():
            code = process.poll()
            if code is not None:
                raise RuntimeError(f"{name} exited with code {code} during the policy trial")
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return None
        executor.spin_once(timeout_sec=min(0.05, remaining))


def action_report(reply):
    if reply is None:
        return None
    return {"actionStatus": reply.status, "errorCode": reply.result.error_code, "errorMessage": reply.result.error_msg}


def planner_outcome(reply, tick, succeeded, aborted):
    if reply is None:
        return Outcome("time-budget", tick, "NavFn planning wall-clock deadline reached")
    result = reply.result
    if reply.status == succeeded and result.error_code == result.NONE:
        return None
    if reply.status == aborted:
        if result.error_code in (result.NO_VALID_PATH, result.START_OCCUPIED, result.GOAL_OCCUPIED):
            return Outcome("unreachable", tick, result.error_msg or "NavFn found no valid path")
        if result.error_code == result.TIMEOUT:
            return Outcome("time-budget", tick, result.error_msg or "NavFn planning timed out")
    raise RuntimeError(f"Unexpected NavFn result: {action_report(reply)}")


def follower_outcome(reply, tick, succeeded, aborted):
    if reply.status == succeeded and reply.result.error_code == reply.result.NONE:
        return Outcome("controller-goal-missed", tick, "RPP reported completion outside the native goal tolerance")
    result = reply.result
    if reply.status == aborted:
        if result.error_code in (result.FAILED_TO_MAKE_PROGRESS, result.NO_VALID_CONTROL, result.PATIENCE_EXCEEDED):
            return Outcome("controller-failed", tick, result.error_msg or "RPP could not make progress")
        if result.error_code == result.CONTROLLER_TIMED_OUT:
            return Outcome("time-budget", tick, result.error_msg or "RPP controller timed out")
    raise RuntimeError(f"Unexpected RPP result: {action_report(reply)}")


def export_route(path, fixture, reply):
    result = reply.result
    poses = [json_pose(pose) for pose in result.path.poses]
    require(result.path.header.frame_id == FRAME and len(poses) >= 2 and all(
        pose["header"]["frameId"] == FRAME and finite_vector(pose["positionMeters"], 3) and
        finite_vector(pose["orientationXyzw"], 4) for pose in poses), "NavFn returned malformed map-frame poses")
    for pose, waypoint in ((poses[0], fixture.start), (poses[-1], fixture.goal)):
        require(math.dist(pose["positionMeters"][:2], (waypoint.x, waypoint.y)) <= 2 * fixture.resolution + 1e-6,
                "NavFn path endpoint is more than two cells from its requested waypoint")
    route = {"schema": "container.bim-navigation-nav2-route", "version": 1, "imageSha256": fixture.image_hash,
             "pathHeader": json_header(result.path.header), "poses": poses,
             "pathMeters": [pose["positionMeters"][:2] for pose in poses],
             "planningTimeNanoseconds": result.planning_time.sec * ROS_EPOCH_NS + result.planning_time.nanosec,
             **action_report(reply)}
    write_json(path, route)
    return route


def run_policy(args, fixture, manifest, scenario, trace_path):
    import rclpy
    from action_msgs.msg import GoalStatus
    from geometry_msgs.msg import Twist
    from nav2_msgs.action import ComputePathToPose, FollowPath
    from nav_msgs.msg import OccupancyGrid
    from rclpy.action import ActionClient
    from rclpy.context import Context
    from rclpy.executors import SingleThreadedExecutor
    from rclpy.parameter import Parameter
    from rclpy.qos import DurabilityPolicy, QoSProfile, ReliabilityPolicy
    from rclpy.time import Time
    from tf2_ros import Buffer, TransformListener
    import yaml

    started = time.monotonic()
    out, bins = args.output, executables()
    params = nav_parameters(fixture)
    controller = params["controller_server"]["ros__parameters"]
    controller["controller_frequency"] = CONTROL_HZ
    controller[GOAL_CHECKER].update(xy_goal_tolerance=args.goal_tolerance, yaw_goal_tolerance=math.pi)
    controller[CONTROLLER].update(desired_linear_vel=LINEAR_LIMIT, rotate_to_heading_angular_vel=ANGULAR_LIMIT)
    params_file = out / "nav2.yaml"
    params_file.write_text(yaml.safe_dump(params), encoding="utf-8")

    domain = args.domain if args.domain is not None else SystemRandom().randrange(10, 100)
    os.environ["ROS_AUTOMATIC_DISCOVERY_RANGE"] = "LOCALHOST"
    os.environ.pop("ROS_STATIC_PEERS", None)
    os.environ.pop("ROS_LOCALHOST_ONLY", None)
    env = dict(os.environ, ROS_DOMAIN_ID=str(domain), ROS_LOG_DIR=str(out / "ros-logs"))
    trace = Trace(trace_path, scenario["ticks"])
    paths = {"trace": "drive.jsonl", "route": None, "nav2Parameters": "nav2.yaml"}
    context = Context()
    rclpy.init(args=[], context=context, domain_id=domain)
    try:
        with ExitStack() as stack:
            node = rclpy.create_node("bim_nav2_policy", context=context,
                                     parameter_overrides=[Parameter("use_sim_time", value=True)])
            stack.callback(node.destroy_node)
            executor = SingleThreadedExecutor(context=context)
            executor.add_node(node)
            stack.callback(executor.shutdown)
            buffer = Buffer(node=node)
            listener = TransformListener(buffer, node, spin_thread=False)
            stack.callback(listener.unregister)
            topics = {}
            map_qos = QoSProfile(depth=1, reliability=ReliabilityPolicy.RELIABLE,
                                 durability=DurabilityPolicy.TRANSIENT_LOCAL)
            node.create_subscription(OccupancyGrid, "/map", lambda msg: topics.update(map=msg), map_qos)
            node.create_subscription(OccupancyGrid, "/global_costmap/costmap", lambda msg: topics.update(costmap=msg), map_qos)
            gate = CommandGate(node.create_publisher(Twist, "/cmd_vel", 1), Twist)
            node.create_subscription(Twist, CMD_INPUT, gate.relay, 1)

            processes = {}

            def launch(name, command):
                processes[name] = stack.enter_context(child_process(command, out / f"{name}.log", env))

            startup_deadline = started + args.startup_timeout
            launch("bridge", [bins["bridge"], "--sim-executable", args.sim_executable, "--scene", args.scene,
                              "--namespace", manifest["sourceNamespace"], "--scenario", args.scenario,
                              "--map-manifest", args.map_directory / "waypoints.json", "--trace", trace.path])
            spin_until(executor, startup_deadline, processes, "native clock and robot TF",
                       lambda: node.get_clock().now().nanoseconds > ROS_EPOCH_NS and
                       buffer.can_transform(FRAME, "base_link", Time()))
            xyz, yaw = position(buffer)
            require(math.dist(xyz[:2], (fixture.start.x, fixture.start.y)) <= START_TOLERANCE and
                    abs(xyz[2] - manifest["floorMeters"]) <= manifest["floorToleranceMeters"] + 1e-6 and
                    angular_error(yaw, fixture.start.yaw) <= START_TOLERANCE,
                    f"Live BIM spawn differs from the exported start: {xyz}, yaw {yaw}")

            launch("map_server", [bins["map_server"], "--ros-args", "-p", "use_sim_time:=true", "-p",
                                  f"yaml_filename:={fixture.map_yaml}"])
            activate(node, executor, startup_deadline, processes, "map_server")
            spin_until(executor, startup_deadline, processes, "/map occupancy", lambda: "map" in topics)
            verify_occupancy(topics["map"], fixture, "/map")
            launch("planner_server", [bins["planner_server"], "--ros-args", "--params-file", params_file])
            activate(node, executor, startup_deadline, processes, "planner_server")
            launch("controller_server", [bins["controller_server"], "--ros-args", "--params-file", params_file,
                                        "-r", f"/cmd_vel:={CMD_INPUT}"])
            activate(node, executor, startup_deadline, processes, "controller_server")
            guard = guarded_cells(fixture)
            spin_until(executor, startup_deadline, processes, "NavFn guarded map",
                       lambda: "costmap" in topics and cost_classes(topics["costmap"].data) == guard)
            verify_occupancy(topics["costmap"], fixture, "NavFn guarded map", guarded=guard)
            spin_until(executor, startup_deadline, processes, "bridge velocity subscriber and Nav2 output",
                       lambda: gate.publisher.get_subscription_count() > 0 and node.count_publishers(CMD_INPUT) > 0)
            planner = ActionClient(node, ComputePathToPose, "/compute_path_to_pose")
            follower = ActionClient(node, FollowPath, "/follow_path")
            for label, client in (("NavFn", planner), ("RPP", follower)):
                stack.callback(client.destroy)
                spin_until(executor, startup_deadline, processes, f"{label} action", client.server_is_ready)

            plan_started = time.monotonic()
            wall = {"startupSeconds": plan_started - started, "planSeconds": 0.0, "driveSeconds": 0.0}
            request = ComputePathToPose.Goal()
            request.planner_id, request.use_start = PLANNER, True
            stamp = node.get_clock().now().to_msg()
            request.start, request.goal = (stamped_pose(point, stamp) for point in (fixture.start, fixture.goal))
            plan_handle, plan_future = action_goal(planner, request, executor, processes,
                                                   plan_started + args.plan_timeout, "NavFn")
            reply = supervise(executor, processes, plan_started + args.plan_timeout,
                              lambda: plan_future.result() if plan_future.done() else None)
            wall["planSeconds"] = time.monotonic() - plan_started
            if reply is None:
                plan_handle.cancel_goal_async()

            trace.refresh()
            require(trace.last_tick >= 0, "No native trace was committed before planning")
            trace.check_stationary(trace.last_tick)
            drive_start = trace.last_tick
            plan_status = action_report(reply)
            follower_status = None
            outcome = planner_outcome(reply, drive_start, GoalStatus.STATUS_SUCCEEDED, GoalStatus.STATUS_ABORTED)
            if outcome is None:
                export_route(out / "route.json", fixture, reply)
                paths["route"] = "route.json"
                request = FollowPath.Goal()
                request.path = reply.result.path
                request.controller_id, request.goal_checker_id = CONTROLLER, GOAL_CHECKER
                request.progress_checker_id = PROGRESS_CHECKER
                handle, future = action_goal(follower, request, executor, processes,
                                             time.monotonic() + args.startup_timeout, "RPP")
                trace.refresh()
                drive_start = trace.last_tick
                trace.check_stationary(drive_start)
                require(scenario["ticks"] - drive_start >= args.drive_ticks,
                        "Native scenario tick limit leaves too few drive ticks after ROS setup and planning")
                window = DriveWindow(drive_start, args.drive_ticks, fixture.goal, args.goal_tolerance)

                def drive_terminal():
                    trace.refresh()
                    native = window.advance(trace.rows)
                    if native is not None:
                        return native
                    if future.done() and trace.rows[-1]["rosTimestampNanoseconds"] >= node.get_clock().now().nanoseconds:
                        return follower_outcome(future.result(), trace.last_tick,
                                                GoalStatus.STATUS_SUCCEEDED, GoalStatus.STATUS_ABORTED)
                    return None

                drive_started = time.monotonic()
                gate.enabled = True
                try:
                    outcome = supervise(executor, processes, drive_started + args.drive_wall_timeout, drive_terminal)
                    if outcome is None:
                        outcome = Outcome("time-budget", trace.last_tick, "RPP drive wall-clock deadline reached")
                    follower_status = action_report(future.result()) if future.done() else None
                finally:
                    wall["driveSeconds"] = time.monotonic() - drive_started
                    gate.stop()
                    if not future.done():
                        handle.cancel_goal_async()
                        executor.spin_once(timeout_sec=0.05)

            scored = trace.rows[drive_start:outcome.tick + 1]
            start_pos, final_pos = (scored[index]["base"]["position"] for index in (0, -1))
            path_length = sum(math.dist(a["base"]["position"][:2], b["base"]["position"][:2])
                              for a, b in zip(scored, scored[1:]))
            return {
                "schema": "container.bim-navigation-policy-episode", "version": 1, "policy": "navfn-rpp",
                "case": fixture.id, "status": outcome.status, "reason": outcome.reason,
                "success": outcome.status == "success", "collision": outcome.status == "collision",
                "imageSha256": fixture.image_hash, "sceneSource": manifest["sceneSource"],
                "sourceNamespace": manifest["sourceNamespace"], "openDoorIds": manifest["openDoorIds"],
                "scenarioSha256": hashlib.sha256(args.scenario.read_bytes()).hexdigest(),
                "initialPositionMeters": start_pos, "initialYawRadians": scored[0]["base"]["yaw"],
                "goalPositionMeters": [fixture.goal.x, fixture.goal.y, manifest["floorMeters"]],
                "finalPositionMeters": final_pos, "distanceToGoalMeters": goal_distance(scored[-1], fixture.goal),
                "travelledMeters": path_length, "driveStartTick": drive_start, "driveEndTick": outcome.tick,
                "committedDriveTicks": outcome.tick - drive_start,
                "simulatedDriveSeconds": (outcome.tick - drive_start) / TICKS_PER_SECOND,
                "blockingEntityId": scored[-1]["blockingEntityId"], "nativeObstruction": scored[-1]["obstruction"],
                "limits": {"linearMetersPerSecond": LINEAR_LIMIT, "angularRadiansPerSecond": ANGULAR_LIMIT,
                           "ticksPerSecond": TICKS_PER_SECOND, "controllerHz": CONTROL_HZ,
                           "driveTicks": args.drive_ticks, "goalToleranceMeters": args.goal_tolerance},
                "wallTime": wall, "commandCounts": {"forwarded": gate.forwarded, "clamped": gate.clamped},
                "plannerAction": plan_status, "controllerAction": follower_status, "domainId": domain,
                "evidence": paths,
            }
    finally:
        rclpy.try_shutdown(context=context)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("sim-executable", "map-directory", "scene", "scenario", "output"):
        parser.add_argument(f"--{option}", type=Path, required=True)
    parser.add_argument("--goal-tolerance", type=float, default=GOAL_TOLERANCE)
    parser.add_argument("--drive-ticks", type=int, default=DRIVE_TICKS)
    parser.add_argument("--startup-timeout", type=float, default=45.0)
    parser.add_argument("--plan-timeout", type=float, default=10.0)
    parser.add_argument("--drive-wall-timeout", type=float, default=45.0)
    parser.add_argument("--domain", type=int, help="Dedicated localhost DDS domain")
    args = parser.parse_args(argv)
    bounds = (args.goal_tolerance, args.startup_timeout, args.plan_timeout, args.drive_wall_timeout)
    if not all(math.isfinite(value) and value > 0 for value in bounds) or not 1 <= args.drive_ticks <= 8191:
        parser.error("Set positive finite timeouts/tolerance and a 1–8191 tick scoring window")
    if args.domain is not None and not 0 <= args.domain <= 101:
        parser.error("Use a localhost ROS domain from 0 to 101")
    try:
        for option in ("sim_executable", "map_directory", "scene", "scenario"):
            setattr(args, option, getattr(args, option).resolve(strict=True))
        args.output = args.output.resolve()
        args.output.mkdir(parents=True)
        try:
            # The bridge appends while the scorer tails this file. Keep that
            # live exchange on the native filesystem and export after shutdown.
            with tempfile.TemporaryDirectory(prefix="bim-nav2-trace-") as directory:
                trace = Path(directory) / "drive.jsonl"
                try:
                    result = run_policy(args, *load_trial(args), trace)
                finally:
                    if trace.is_file():
                        shutil.copyfile(trace, args.output / "drive.jsonl")
        except (ImportError, KeyError, OSError, RuntimeError, TypeError, ValueError) as error:
            write_json(args.output / "failure.json", {"status": "harness-error", "error": str(error),
                                                      "diagnostics": traceback.format_exc()})
            raise
        write_json(args.output / "episode.json", result)
        print(f"{result['case']} NavFn/RPP: {result['status']} ({args.output / 'episode.json'})", flush=True)
        return 0
    except (ImportError, KeyError, OSError, RuntimeError, TypeError, ValueError) as error:
        print(f"BIM Nav2 policy failed: {error}. Evidence: {args.output}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
