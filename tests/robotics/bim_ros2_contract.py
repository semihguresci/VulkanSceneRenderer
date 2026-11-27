#!/usr/bin/env python3
"""Launch the installed Jazzy bridge and check its live headless ROS contract."""

import argparse
from contextlib import ExitStack
from functools import partial
import json
import math
import os
from pathlib import Path
from random import SystemRandom
import struct
import sys
import time

from bim_nav2_planner import child_process, read_json, require, spin_until
from bim_ros2_bag import (_DEPTH, _DEPTH_INFO, _INFO, _OPTICAL, _RGB, _SECOND, _TYPES,
                          _RecordedObservations, _ns, _pose)


_CAMERA_TOPICS = (_RGB, _INFO, _DEPTH, _DEPTH_INFO)
_SCAN = "/scan"
_CLOUD = "/lidar/points"
_LIDAR_FRAME = "lidar_link"
_POINT_FIELD_FLOAT32 = 7
_MAP_ODOM = ("map", "odom")
_BASE_CAMERA = ("base_link", _OPTICAL)
_BASE_LIDAR = ("base_link", _LIDAR_FRAME)


def native_tick(stamp, rate):
    elapsed = stamp - _SECOND
    tick = (elapsed * rate + _SECOND - 1) // _SECOND
    require(tick >= 0 and elapsed == tick * _SECOND // rate,
            f"ROS stamp {stamp} is off the native {rate} Hz tick grid")
    return tick


def sensor_tick_due(tick, rate, settings, frequency_key):
    elapsed = tick - settings.get("firstTick", 0)
    numerator, denominator = settings[frequency_key], 1000 * rate
    return elapsed >= 0 and (elapsed == 0 or elapsed * numerator // denominator
                             > (elapsed - 1) * numerator // denominator)


def expected_scan(settings):
    count = settings.get("azimuthSamples", 360)
    angle_min = math.remainder(settings.get("azimuthStart", -math.pi), math.tau)
    span = settings.get("azimuthSpan", math.tau)
    intervals = count if span == math.tau else count - 1
    step = span / intervals if count > 1 else 0.0
    return (count, angle_min, angle_min + (count - 1) * step, step, 0.0,
            1000.0 / settings["scansPerSecondMilliHz"],
            settings.get("minRangeMeters", 0.05), settings.get("maxRangeMeters", 25.0))


def laser_scan_period(tick, rate, settings):
    first_tick = settings.get("firstTick", 0)
    numerator, denominator = settings["scansPerSecondMilliHz"], 1000 * rate
    ordinal = (tick - first_tick) * numerator // denominator
    if ordinal == 0:
        return 1000.0 / numerator
    previous_tick = first_tick + ((ordinal - 1) * denominator + numerator - 1) // numerator
    return (tick * _SECOND // rate - previous_tick * _SECOND // rate) / _SECOND


def near(actual, expected, tolerance=2e-6):
    return len(actual) == len(expected) and all(
        math.isclose(a, b, rel_tol=tolerance, abs_tol=tolerance) for a, b in zip(actual, expected))


def cloud_points(message, settings):
    width, angle_min, _, step, _, _, minimum, maximum = expected_scan(settings)
    elevations = settings["elevationAngles"]
    xyz_fields = [(axis, offset, _POINT_FIELD_FLOAT32, 1) for axis, offset in zip("xyz", (0, 4, 8))]
    fields = [(field.name, field.offset, field.datatype, field.count) for field in message.fields]
    require((message.width, message.height) == (width, len(elevations)) and fields == xyz_fields,
            f"/lidar/points must preserve organized dimensions and FLOAT32 x/y/z fields: "
            f"{message.width} × {message.height}, {fields}")
    data = bytes(message.data)
    require(not message.is_bigendian and message.point_step == 12 and message.row_step == width * 12 and
            len(data) == message.row_step * message.height,
            "/lidar/points must use tightly packed little-endian XYZ data")

    points, ranges = [], []
    for index, point in enumerate(struct.iter_unpack("<fff", data)):
        if all(map(math.isnan, point)):
            # PointCloud2 encodes all invalid return reasons as XYZ NaNs.
            ranges.append(math.inf)
            points.append(None)
            continue
        require(all(map(math.isfinite, point)), f"/lidar/points has a partial NaN or infinite point at beam {index}")
        distance = math.hypot(*point)
        require(minimum - 2e-6 <= distance <= maximum + 2e-6,
                f"/lidar/points hit at beam {index} lies outside the calibrated range: {distance:g} metres")
        row, bearing = divmod(index, width)
        elevation, azimuth = elevations[row], angle_min + bearing * step
        horizontal = distance * math.cos(elevation)
        expected = (horizontal * math.cos(azimuth), horizontal * math.sin(azimuth), distance * math.sin(elevation))
        require(near(point, expected), f"/lidar/points beam ({row}, {bearing}) at {point} differs from {expected}")
        points.append(point)
        ranges.append(distance)
    require(bool(message.is_dense) == all(point is not None for point in points),
            "/lidar/points is_dense does not indicate whether points contain NaNs")
    return tuple(points), tuple(ranges)


class LiveObservations(_RecordedObservations):
    def __init__(self, rate, camera_timing, lidar_settings):
        super().__init__()
        self.rate = rate
        self.camera_timing = camera_timing
        self.lidar_settings = lidar_settings
        self.latest_stamps = {}
        self.static_poses = {}
        self.odom_twist = {}
        self.lidar_ranges = {}
        self.cloud_xyz = {}

    def check_stamp(self, topic, stamp):
        tick = native_tick(stamp, self.rate)
        require(stamp > self.latest_stamps.get(topic, 0), f"{topic} did not advance: {stamp}")
        self.latest_stamps[topic] = stamp
        return tick

    def update(self, topic, message):
        if topic == "/clock":
            self.check_stamp(topic, _ns(message.clock))
        elif topic == "/tf":
            require(len(message.transforms) == 1, "Each live /tf update must contain one odom → base_link transform")
            self.check_stamp(topic, _ns(message.transforms[0].header.stamp))
        elif topic == "/tf_static":
            for transform in message.transforms:
                edge = transform.header.frame_id, transform.child_frame_id
                require(edge not in self.static_poses and _ns(transform.header.stamp) == _SECOND,
                        f"Unexpected static TF stamp or repeated edge {edge}")
                self.static_poses[edge] = _pose(transform.transform.translation, transform.transform.rotation)
        elif topic == "/odom":
            stamp = _ns(message.header.stamp)
            self.check_stamp(topic, stamp)
            linear, angular = message.twist.twist.linear, message.twist.twist.angular
            require(all(map(math.isfinite, (linear.x, angular.z))) and not any((linear.y, linear.z, angular.x, angular.y)),
                    f"/odom twist at {stamp} is not finite and planar")
            require(all(map(math.isfinite, (*message.pose.covariance, *message.twist.covariance))),
                    f"/odom has a non-finite covariance at {stamp}")
            self.odom_twist[stamp] = linear.x, angular.z
        elif topic in (_SCAN, _CLOUD):
            self.update_lidar(topic, message)
            return
        elif topic in _CAMERA_TOPICS:
            tick = self.check_stamp(topic, _ns(message.header.stamp))
            require(sensor_tick_due(tick, self.rate, self.camera_timing, "framesPerSecondMilliHz"),
                    f"{topic} arrived for unscheduled native camera tick {tick}")
        super().update(topic, message)

    def update_lidar(self, topic, message):
        stamp = _ns(message.header.stamp)
        tick = self.check_stamp(topic, stamp)
        require(sensor_tick_due(tick, self.rate, self.lidar_settings, "scansPerSecondMilliHz"),
                f"{topic} arrived for unscheduled native LiDAR tick {tick}")
        require(message.header.frame_id == _LIDAR_FRAME, f"{topic} uses frame {message.header.frame_id!r}")
        if topic == _CLOUD:
            self.cloud_xyz[stamp], self.lidar_ranges[stamp] = cloud_points(message, self.lidar_settings)
        else:
            self.lidar_ranges[stamp] = self.laser_ranges(message, stamp, tick)

    def laser_ranges(self, scan, stamp, tick):
        actual = (len(scan.ranges), scan.angle_min, scan.angle_max, scan.angle_increment,
                  scan.time_increment, scan.scan_time, scan.range_min, scan.range_max)
        count, angle_min, angle_max, step, increment, _, minimum, maximum = expected_scan(self.lidar_settings)
        expected = (count, angle_min, angle_max, step, increment,
                    laser_scan_period(tick, self.rate, self.lidar_settings), minimum, maximum)
        require(actual[0] == expected[0] and near(actual[1:], expected[1:]),
                f"/scan calibration at {stamp}: expected {expected}, got {actual}")
        require(not scan.intensities, "Synthetic /scan contains unexpected intensity samples")
        require(all(not math.isfinite(distance) or
                    scan.range_min - 2e-6 <= distance <= scan.range_max + 2e-6 for distance in scan.ranges),
                f"/scan reports a finite hit outside the calibrated range at {stamp}")
        return tuple(scan.ranges)

    def aligned_images(self):
        return set.intersection(set(self.pose), set(self.tf), *(set(frames) for frames in self.frames.values()))

    def observed_transforms(self):
        return set(self.pose) & set(self.tf)

    def aligned_lidar(self):
        return self.lidar_ranges.keys() & self.observed_transforms()

    def summary(self, camera):
        common = self.aligned_images()
        require(len(common) >= 4, f"Only {len(common)} aligned RGB-D/CameraInfo/TF/odometry observations")
        require(self.rgb_nonzero and self.finite_depth > 0, "RGB-D data contains no BIM surface hits")
        intrinsics = camera["intrinsics"]
        expected = (*(intrinsics[key] for key in ("width", "height", "fx", "fy")),
                    intrinsics["cx"] - 0.5, intrinsics["cy"] - 0.5)
        require(near(self.intrinsics, expected), f"CameraInfo {self.intrinsics} differs from authored intrinsics {expected}")
        require(all(size == expected[:2] for frames in self.frames.values() for size in frames.values()),
                "Live RGB-D/CameraInfo frames do not share the calibrated dimensions")
        matches = self.observed_transforms()
        for stamp in matches:
            require(self.pose[stamp] == self.tf[stamp], f"/odom and /tf poses differ at {stamp}")
        return {"clockSamples": len(self.clock), "odometrySamples": len(self.pose), "odometryTfMatches": len(matches),
                "rgbdAlignedFrames": len(common), "cameraIntrinsics": dict(zip(("width", "height", "fx", "fy", "cx", "cy"),
                                                                              self.intrinsics, strict=True)),
                "finiteDepthPixels": self.finite_depth, "nanDepthPixels": self.nan_depth}


def endpoint_contract(node, expected):
    result = {}
    for topic, (kind, schema, reliability, durability, depth) in expected.items():
        getter = node.get_publishers_info_by_topic if kind == "publisher" else node.get_subscriptions_info_by_topic
        endpoints = [endpoint for endpoint in getter(topic) if endpoint.node_name == "bim_ros_bridge"]
        if not endpoints:
            continue
        require(len(endpoints) == 1 and endpoints[0].node_namespace == "/",
                f"Unexpected number or namespace of bridge {kind}s on {topic}")
        endpoint = endpoints[0]
        qos = endpoint.qos_profile
        actual = (endpoint.topic_type, qos.reliability.name, qos.durability.name)
        require(actual == (schema, reliability, durability),
                f"{topic} {kind} advertises {actual}; expected {(schema, reliability, durability)}")
        # Fast DDS does not expose a remote endpoint's history/depth over discovery.
        require(qos.history.name in ("UNKNOWN", "KEEP_LAST") and qos.depth in (0, depth),
                f"{topic} {kind} advertises unexpected history/depth {qos.history.name}/{qos.depth}")
        result[topic] = {"endpoint": kind, "type": schema, "reliability": reliability, "durability": durability,
                         "advertisedHistory": qos.history.name, "advertisedDepth": qos.depth or None}
    return result


def lidar_evidence(obs, expected_ranges, topic):
    aligned = obs.aligned_lidar()
    require(len(aligned) >= 4, f"Only {len(aligned)} LiDAR observations share a committed TF/odometry stamp")
    clock_matches = len(aligned.intersection(obs.clock))
    require(clock_matches >= 4, f"Only {clock_matches} aligned LiDAR observations share an actual /clock stamp")
    first_stamp = min(aligned)
    first = obs.lidar_ranges[first_stamp]
    count = len(first)
    require(all(0 <= index < count for index in expected_ranges), "Expected LiDAR beam index is out of range")
    for index, expected in expected_ranges.items():
        require(math.isclose(first[index], expected, abs_tol=2e-5),
                f"{topic} beam {index}: expected {expected:g} metres, got {first[index]:g}")
    finite = sum(math.isfinite(distance) for stamp in aligned for distance in obs.lidar_ranges[stamp])
    require(finite > 0, "LiDAR did not return any BIM surface hits")
    report = {"frameId": _LIDAR_FRAME, "alignedScans": len(aligned), "clockMatches": clock_matches,
              "beamsPerScan": count, "finiteHits": finite,
              "knownRangesMeters": {str(index): value if math.isfinite(value) else str(value)
                                    for index, value in expected_ranges.items()}}
    if topic == _CLOUD:
        width, *_ = expected_scan(obs.lidar_settings)
        report["organizedDimensions"] = [width, len(obs.lidar_settings["elevationAngles"])]
        report["firstXyzMeters"] = obs.cloud_xyz[first_stamp]
        report["missingPoints"] = len(aligned) * count - finite
    return report


def startup_and_drive(executor, node, publisher, process, deadline, obs, types, expected, camera):
    from rclpy.qos import DurabilityPolicy, QoSProfile, ReliabilityPolicy
    from rclpy.time import Time
    from tf2_ros import Buffer, TransformListener

    processes = {"ros2 launch bim_ros_bridge": process}
    spin = partial(spin_until, executor, deadline, processes)
    endpoints = {}

    def discovered():
        endpoints.update(endpoint_contract(node, expected))
        return len(endpoints) == len(expected) and publisher.get_subscription_count() >= 2

    spin("advertised bridge topic contracts and command subscriber", discovered)
    spin("advancing simulated /clock", lambda: len(obs.clock) >= 3 and node.get_clock().now().nanoseconds > _SECOND)

    # Join after the bridge has started ticking. This can only see the initial
    # static transforms when the bridge provides transient-local durability.
    static_qos = QoSProfile(depth=1, reliability=ReliabilityPolicy.RELIABLE,
                            durability=DurabilityPolicy.TRANSIENT_LOCAL)
    node.create_subscription(types["/tf_static"], "/tf_static", partial(obs.update, "/tf_static"), static_qos)
    tf_buffer = Buffer(node=node)
    tf_listener = TransformListener(tf_buffer, node)
    expected_edges = {_MAP_ODOM, _BASE_CAMERA}
    if obs.lidar_settings is not None:
        expected_edges.add(_BASE_LIDAR)
    spin("late transient-local static TF sample", lambda: len(obs.static_poses) >= len(expected_edges))
    require(set(obs.static_poses) == expected_edges, f"Wrong static TF edges: {sorted(obs.static_poses)}")
    mounts = {_BASE_CAMERA: camera.get("baseFromCamera", {})}
    if obs.lidar_settings is not None:
        mounts[_BASE_LIDAR] = obs.lidar_settings.get("mount", {})
    for edge, mount in mounts.items():
        actual, expected_xyz = obs.static_poses[edge][:3], mount.get("xyz", (0.0, 0.0, 0.0))
        require(near(actual, expected_xyz), f"Static TF {edge} mounts at {actual}, expected {expected_xyz} metres")

    def sensor_stamp():
        common = obs.aligned_images()
        if len(common) < 4:
            return None
        for candidate in sorted(common, reverse=True)[:4]:
            stamp = Time(nanoseconds=candidate)
            if tf_buffer.can_transform("map", _OPTICAL, stamp):
                return stamp
        return None

    spin("RGB-D/odometry TF at a common live tick", sensor_stamp)
    stamp = sensor_stamp()
    camera_pose = tf_buffer.lookup_transform("map", _OPTICAL, stamp)
    require(_ns(camera_pose.header.stamp) == stamp.nanoseconds and camera_pose.child_frame_id == _OPTICAL,
            "tf2 did not resolve map to camera at the exact observation time")
    lidar_pose = None
    if obs.lidar_settings is not None:
        spin("LiDAR, /clock, and odometry sharing four timestamps",
             lambda: len(obs.aligned_lidar().intersection(obs.clock)) >= 4)
        lidar_stamp = Time(nanoseconds=max(obs.aligned_lidar().intersection(obs.clock)))
        spin("LiDAR TF at its exact sample time", lambda: tf_buffer.can_transform("map", _LIDAR_FRAME, lidar_stamp))
        lidar_pose = tf_buffer.lookup_transform("map", _LIDAR_FRAME, lidar_stamp)
        require(_ns(lidar_pose.header.stamp) == lidar_stamp.nanoseconds and lidar_pose.child_frame_id == _LIDAR_FRAME,
                "tf2 did not resolve map to LiDAR at the exact observation time")

    sim_clock = node.get_clock().now().nanoseconds
    native_tick(sim_clock, obs.rate)
    require(abs(sim_clock - obs.clock[-1]) <= 3 * ((_SECOND + obs.rate - 1) // obs.rate),
            "ROS use_sim_time clock did not track the live /clock topic within three ticks")
    command = types["/cmd_vel"]()
    command.angular.z = 0.25
    stamp_before_command = max(obs.pose)
    latest_publish = 0.0

    def moving():
        nonlocal latest_publish
        now = time.monotonic()
        if now - latest_publish >= 0.05:
            publisher.publish(command)
            latest_publish = now
        return sum(stamp > stamp_before_command and yaw_rate > 0.05
                   for stamp, (_, yaw_rate) in obs.odom_twist.items()) >= 4

    spin("finite /cmd_vel causing four positive measured angular rates", moving)
    require(obs.nonzero_commands > 0, "The reliable /cmd_vel subscriber did not read back the real command")
    last_moving_stamp = max(obs.odom_twist)
    spin("watchdog stopping odometry without a zero command", lambda: any(
        stamp > last_moving_stamp and max(map(abs, twist)) < 1e-6 for stamp, twist in obs.odom_twist.items()))
    watchdog_seconds = time.monotonic() - latest_publish
    require(0.35 <= watchdog_seconds <= 1.5, f"Watchdog returned zero velocity after {watchdog_seconds:.3f} seconds")

    tf_listener.unregister()
    result = {"topicContracts": endpoints, "rosClockNanoseconds": sim_clock,
              "lateStaticTransforms": [list(edge) for edge in sorted(expected_edges)],
              "cameraTfLookupNanoseconds": _ns(camera_pose.header.stamp),
              "nonzeroCommandsReceived": obs.nonzero_commands, "watchdogStopWallSeconds": watchdog_seconds}
    if lidar_pose is not None:
        result["lidarTfLookupNanoseconds"] = _ns(lidar_pose.header.stamp)
    return result


def live_contract(args, scenario, camera):
    import rclpy
    from geometry_msgs.msg import Twist
    from nav_msgs.msg import Odometry
    from rclpy.context import Context
    from rclpy.executors import SingleThreadedExecutor
    from rclpy.parameter import Parameter
    from rclpy.qos import QoSProfile, ReliabilityPolicy, qos_profile_sensor_data
    from rosgraph_msgs.msg import Clock
    from sensor_msgs.msg import CameraInfo, Image, LaserScan, PointCloud2
    from tf2_msgs.msg import TFMessage

    rate = scenario.get("ticksPerSecond", 60)
    lidar_topic = _CLOUD if args.cloud else _SCAN if args.scan else None
    lidar = scenario.get("lidar") if lidar_topic else None
    require(not lidar_topic or lidar is not None, "--scan and --cloud need a LiDAR-enabled native stream scenario")
    if lidar is not None:
        planar = lidar.get("elevationAngles", [0.0]) == [0.0]
        require(planar == args.scan, "Use --scan for a single zero-elevation row and --cloud for other LiDAR angles")
    obs = LiveObservations(rate, camera["timing"], lidar)
    reliable, best_effort, volatile = "RELIABLE", "BEST_EFFORT", "VOLATILE"
    types = {"/clock": Clock, "/tf": TFMessage, "/tf_static": TFMessage, "/odom": Odometry, "/cmd_vel": Twist,
             _RGB: Image, _DEPTH: Image, _INFO: CameraInfo, _DEPTH_INFO: CameraInfo}
    expected = {"/clock": ("publisher", _TYPES["/clock"], best_effort, volatile, 1),
                "/odom": ("publisher", _TYPES["/odom"], reliable, volatile, 10),
                "/tf": ("publisher", _TYPES["/tf"], reliable, volatile, 100),
                "/tf_static": ("publisher", _TYPES["/tf_static"], reliable, "TRANSIENT_LOCAL", 1),
                "/cmd_vel": ("subscription", _TYPES["/cmd_vel"], reliable, volatile, 1)}
    for topic in _CAMERA_TOPICS:
        expected[topic] = ("publisher", _TYPES[topic], best_effort, volatile, 5)
    if lidar_topic:
        message_type = LaserScan if args.scan else PointCloud2
        types[lidar_topic] = message_type
        expected[lidar_topic] = ("publisher", f"sensor_msgs/msg/{message_type.__name__}", best_effort, volatile, 5)

    domain = args.domain if args.domain is not None else SystemRandom().randrange(50, 101)
    os.environ["ROS_AUTOMATIC_DISCOVERY_RANGE"] = "LOCALHOST"
    os.environ.pop("ROS_LOCALHOST_ONLY", None)
    os.environ.pop("ROS_STATIC_PEERS", None)
    env = dict(os.environ, ROS_DOMAIN_ID=str(domain), ROS_LOG_DIR=str(args.output / "ros-logs"))
    trace = args.output / "command-trace.jsonl"
    launch = ["ros2", "launch", "bim_ros_bridge", "bridge.launch.py", f"sim_executable:={args.sim_executable}",
              f"scene:={args.scene}", f"namespace:={args.namespace}", f"scenario:={args.scenario}",
              f"camera:={args.camera}", f"trace:={trace}"]
    if args.map_manifest:
        launch.append(f"map_manifest:={args.map_manifest}")

    context = Context()
    with ExitStack() as stack:
        rclpy.init(args=[], context=context, domain_id=domain)
        stack.callback(rclpy.try_shutdown, context=context)
        node = rclpy.create_node("bim_live_contract", context=context,
                                 parameter_overrides=[Parameter("use_sim_time", value=True)])
        stack.callback(node.destroy_node)
        executor = SingleThreadedExecutor(context=context)
        executor.add_node(node)
        stack.callback(executor.shutdown)
        clock_qos = QoSProfile(depth=20, reliability=ReliabilityPolicy.BEST_EFFORT)
        subscriptions = {"/clock": clock_qos, "/tf": 100, "/odom": 100, "/cmd_vel": 10,
                         **dict.fromkeys(_CAMERA_TOPICS, qos_profile_sensor_data)}
        if lidar_topic:
            subscriptions[lidar_topic] = qos_profile_sensor_data
        for topic, qos in subscriptions.items():
            node.create_subscription(types[topic], topic, partial(obs.update, topic), qos)
        publisher = node.create_publisher(Twist, "/cmd_vel", 10)
        process = stack.enter_context(child_process(launch, args.output / "launch.log", env))
        result = startup_and_drive(executor, node, publisher, process, time.monotonic() + args.timeout,
                                   obs, types, expected, camera)

    # The native trace proves that the observed motion and stop both came
    # from real committed simulator commands, not another ROS publisher.
    commands = [row["command"] for line in trace.read_text(encoding="utf-8").splitlines()
                if (row := json.loads(line))["command"] is not None]
    applied = {"linearMetersPerSecond": 0.0, "angularRadiansPerSecond": 0.25}
    require(applied in commands, "The native simulator did not apply the commanded angular rate")
    first_command = commands.index(applied)
    stopped = {"linearMetersPerSecond": 0.0, "angularRadiansPerSecond": 0.0}
    require(stopped in commands[first_command + 1:], "The native simulator did not commit a watchdog stop")
    result.update(obs.summary(camera), nativeCommandsApplied=commands.count(applied), domainId=domain)
    if lidar_topic:
        result["scan" if args.scan else "cloud"] = lidar_evidence(obs, dict(args.expect_range), lidar_topic)
    return result


def beam_range(value):
    try:
        beam, metres = value.split("=", 1)
        result = int(beam), float(metres)
        if result[0] >= 0 and not math.isnan(result[1]):
            return result
    except ValueError:
        pass
    raise argparse.ArgumentTypeError("expected BEAM=METRES or BEAM=inf")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("sim-executable", "scene", "scenario", "camera", "output"):
        parser.add_argument(f"--{option}", type=Path, required=True)
    parser.add_argument("--namespace", required=True)
    parser.add_argument("--map-manifest", type=Path)
    lidar_mode = parser.add_mutually_exclusive_group()
    lidar_mode.add_argument("--scan", action="store_true", help="Require planar LaserScan and its static frame")
    lidar_mode.add_argument("--cloud", action="store_true", help="Require organized PointCloud2 and its static frame")
    parser.add_argument("--expect-range", action="append", type=beam_range, default=[], metavar="BEAM=METRES")
    parser.add_argument("--timeout", type=float, default=25.0)
    parser.add_argument("--domain", type=int, help="Use this isolated ROS domain instead of a random local domain")
    args = parser.parse_args()
    if args.expect_range and not (args.scan or args.cloud):
        parser.error("--expect-range requires --scan or --cloud")
    if not 0 < args.timeout <= 120 or args.domain is not None and not 0 <= args.domain <= 101:
        parser.error("Provide a finite 0–120 second timeout and, if set, a ROS domain between 0 and 101")
    document = {"schema": "container.bim-ros2-live-contract", "version": 1}
    try:
        for name in ("sim_executable", "scene", "scenario", "camera", "map_manifest"):
            if path := getattr(args, name):
                setattr(args, name, path.resolve(strict=True))
        args.output = args.output.resolve()
        args.output.mkdir(parents=True)
        try:
            document.update(live_contract(args, read_json(args.scenario), read_json(args.camera)), passed=True)
        except (ImportError, RuntimeError, OSError, ValueError, KeyError, TypeError) as error:
            document.update(passed=False, error=str(error))
        (args.output / "contract.json").write_text(json.dumps(document, allow_nan=False, indent=2) + "\n", encoding="utf-8")
        if not document["passed"]:
            raise RuntimeError(f"{document['error']} (see {args.output / 'launch.log'})")
        print(f"Verified live ROS 2 contract: {args.output / 'contract.json'}")
    except (RuntimeError, OSError) as error:
        print(f"BIM ROS 2 contract: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
