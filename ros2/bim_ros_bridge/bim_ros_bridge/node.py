"""Publish native fixed-step BIM observations and accept planar ROS commands."""

import argparse
from array import array
import base64
from contextlib import ExitStack
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import time

from builtin_interfaces.msg import Time
from geometry_msgs.msg import TransformStamped, Twist
from nav_msgs.msg import Odometry
import rclpy
from rclpy.clock import Clock, ClockType
from rclpy.executors import ExternalShutdownException, SingleThreadedExecutor
from rclpy.qos import QoSProfile, ReliabilityPolicy, qos_profile_sensor_data
from rclpy.utilities import remove_ros_args
from rosgraph_msgs.msg import Clock as ClockMessage
from sensor_msgs.msg import CameraInfo, Image, LaserScan, PointCloud2, PointField
from tf2_ros import StaticTransformBroadcaster, TransformBroadcaster

from .transport import DriveStream, native_input_path


_NS_PER_SECOND = 1_000_000_000
_ROS_EPOCH_NS = _NS_PER_SECOND
_COMMAND_TIMEOUT = 0.5
_MAP = "map"
_ODOM = "odom"
_BASE = "base_link"
_OPTICAL = "camera_optical_frame"
_LIDAR = "lidar_link"


def _number(value):
    if type(value) not in (int, float) or not math.isfinite(value):
        raise ValueError("Expected a finite native numeric value")
    return float(value)


def _vector(values, size):
    if not isinstance(values, list) or len(values) != size:
        raise ValueError(f"Expected a native vector of length {size}")
    return tuple(map(_number, values))


def _columns(value):
    if not isinstance(value, list) or len(value) != 4:
        raise ValueError("Expected four native transform columns")
    columns = tuple(_vector(column, 4) for column in value)
    if tuple(column[3] for column in columns) != (0, 0, 0, 1):
        raise ValueError("Expected an affine native transform")
    return columns


def _quaternion(columns):
    # Matrices are column-major on the wire. The largest diagonal branch
    # avoids division by a near-zero scalar component at rotations near pi.
    (xx, yx, zx, _), (xy, yy, zy, _), (xz, yz, zz, _), _ = columns
    trace = xx + yy + zz
    if trace > 0:
        scale = 2 * math.sqrt(1 + trace)
        return (zy - yz) / scale, (xz - zx) / scale, (yx - xy) / scale, scale / 4
    if xx >= max(yy, zz):
        scale = 2 * math.sqrt(1 + xx - yy - zz)
        return scale / 4, (xy + yx) / scale, (xz + zx) / scale, (zy - yz) / scale
    if yy >= zz:
        scale = 2 * math.sqrt(1 + yy - xx - zz)
        return (xy + yx) / scale, scale / 4, (yz + zy) / scale, (xz - zx) / scale
    scale = 2 * math.sqrt(1 + zz - xx - yy)
    return (xz + zx) / scale, (yz + zy) / scale, scale / 4, (yx - xy) / scale


def _transform(parent, child, columns, stamp):
    message = TransformStamped()
    message.header.frame_id = parent
    message.header.stamp = stamp
    message.child_frame_id = child
    translation = message.transform.translation
    translation.x, translation.y, translation.z = columns[3][:3]
    rotation = message.transform.rotation
    rotation.x, rotation.y, rotation.z, rotation.w = _quaternion(columns)
    return message


def _camera_info(calibration):
    message = CameraInfo()
    message.header.frame_id = _OPTICAL
    width, height = calibration["width"], calibration["height"]
    if any(type(value) is not int or not 0 < value <= 256 for value in (width, height)):
        raise ValueError("Native camera dimensions must be between 1 and 256 pixels")
    message.width, message.height = width, height
    fx, fy, cx, cy, near, far = (_number(calibration[key]) for key in ("fx", "fy", "cx", "cy", "nearMeters", "farMeters"))
    if min(fx, fy, near) <= 0 or far <= near:
        raise ValueError("Invalid native camera calibration")
    # The source measures pixels from the image edge. ROS projection uses
    # zero-indexed pixel centres, so the principal point moves by half a pixel.
    cx -= 0.5
    cy -= 0.5
    message.distortion_model = "plumb_bob"
    message.d = [0.0] * 5
    message.k = [fx, 0.0, cx, 0.0, fy, cy, 0.0, 0.0, 1.0]
    message.r = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
    message.p = [fx, 0.0, cx, 0.0, 0.0, fy, cy, 0.0, 0.0, 0.0, 1.0, 0.0]
    return message


def _image(data, width, height, bytes_per_pixel, encoding, stamp):
    message = Image()
    message.header.frame_id = _OPTICAL
    message.header.stamp = stamp
    message.width, message.height = width, height
    message.encoding = encoding
    message.is_bigendian = 0
    message.step = width * bytes_per_pixel
    message.data = array("B", data)
    return message


class LidarPublisher:
    def __init__(self, node, calibration):
        self.bearings = calibration["azimuthSamples"]
        elevations = calibration["elevationAngles"]
        if (type(self.bearings) is not int or self.bearings <= 0 or not isinstance(elevations, list) or
                not elevations or self.bearings * len(elevations) > 4096):
            raise ValueError("Invalid native LiDAR organized scan size")
        self.elevations = tuple(map(_number, elevations))
        if any(abs(elevation) > math.pi / 2 for elevation in self.elevations):
            raise ValueError("Invalid native LiDAR elevation angles")
        self.start, self.increment = (_number(calibration[key]) for key in ("azimuthStart", "azimuthIncrement"))
        if abs(self.start) > math.pi or not 0 <= self.increment * (self.bearings - 1) <= math.tau:
            raise ValueError("Invalid native LiDAR azimuth calibration")
        self.min_range, self.max_range = (_number(calibration[key]) for key in ("minRangeMeters", "maxRangeMeters"))
        if not 0 <= self.min_range < self.max_range <= 3.4028234663852886e38:
            raise ValueError("Invalid native LiDAR range calibration")
        if struct.unpack("<f", struct.pack("<f", self.min_range)) == struct.unpack("<f", struct.pack("<f", self.max_range)):
            raise ValueError("Native LiDAR range interval collapses at ROS float32 precision")
        rate = calibration["scansPerSecondMilliHz"]
        if type(rate) is not int or rate <= 0:
            raise ValueError("Invalid native LiDAR scan rate")
        self.scan_period = 1000 / rate
        mount = calibration["baseFromSensor"]
        self.mount = TransformStamped()
        self.mount.header.frame_id = _BASE
        self.mount.header.stamp = Time(sec=1)
        self.mount.child_frame_id = _LIDAR
        offset = self.mount.transform.translation
        offset.x, offset.y, offset.z = _vector(mount["xyz"], 3)
        quaternion = _vector(mount["quaternionXyzw"], 4)
        if not math.isclose(math.hypot(*quaternion), 1, abs_tol=1e-9):
            raise ValueError("Invalid native LiDAR mounting rotation")
        rotation = self.mount.transform.rotation
        rotation.x, rotation.y, rotation.z, rotation.w = quaternion

        self.index = 0
        self.last_stamp_ns = None
        self.planar = self.elevations == (0.0,)
        self.directions = ()
        if self.planar:
            self.topic = node.create_publisher(LaserScan, "/scan", qos_profile_sensor_data)
        else:
            self.topic = node.create_publisher(PointCloud2, "/lidar/points", qos_profile_sensor_data)
            azimuths = [self.start + self.increment * beam for beam in range(self.bearings)]
            self.directions = tuple((math.cos(elevation) * math.cos(azimuth),
                                     math.cos(elevation) * math.sin(azimuth), math.sin(elevation))
                                    for elevation in self.elevations for azimuth in azimuths)

    def _range(self, value):
        if value == "no-hit":
            return math.inf
        if value == "below-minimum":
            return -math.inf
        if value in ("dropped", "noise-out-of-range"):
            return math.nan
        distance = _number(value)
        if not self.min_range <= distance <= self.max_range:
            raise ValueError("Native LiDAR return lies outside the calibrated interval")
        return distance

    def message(self, sample, stamp, stamp_ns):
        index, values = sample["sampleIndex"], sample["rangesMeters"]
        if (type(index) is not int or index != self.index or not isinstance(values, list) or
                len(values) != self.bearings * len(self.elevations)):
            raise ValueError("Invalid native LiDAR sample index or beam count")
        ranges = tuple(map(self._range, values))
        if self.planar:
            message = LaserScan()
            message.angle_min = self.start
            message.angle_increment = self.increment
            message.angle_max = self.start + self.increment * (self.bearings - 1)
            message.scan_time = self.scan_period if self.last_stamp_ns is None else (
                (stamp_ns - self.last_stamp_ns) / _NS_PER_SECOND)
            message.range_min, message.range_max = self.min_range, self.max_range
            message.ranges = array("f", ranges)
        else:
            message = PointCloud2()
            message.width, message.height = self.bearings, len(self.elevations)
            message.fields = [PointField(name=axis, offset=4 * index, datatype=PointField.FLOAT32, count=1)
                              for index, axis in enumerate(("x", "y", "z"))]
            message.point_step = 12
            message.row_step = self.bearings * message.point_step
            xyz = bytearray(len(ranges) * message.point_step)
            message.is_dense = True
            for beam, (distance, direction) in enumerate(zip(ranges, self.directions)):
                if math.isfinite(distance):
                    point = tuple(distance * component for component in direction)
                else:
                    point = (math.nan,) * 3
                    message.is_dense = False
                struct.pack_into("<fff", xyz, beam * message.point_step, *point)
            message.data = array("B", xyz)
        message.header.frame_id = _LIDAR
        message.header.stamp = stamp
        return message

    def publish(self, message, stamp_ns):
        self.topic.publish(message)
        self.index += 1
        self.last_stamp_ns = stamp_ns


def _map_manifest(path, description):
    if path is None:
        return None
    map_data = json.loads(path.read_text(encoding="utf-8"))
    if (not isinstance(map_data, dict) or map_data.get("schema") != "container.bim-navigation-map" or
            type(map_data.get("version")) is not int or map_data["version"] != 1 or map_data.get("frameId") != _MAP):
        raise ValueError("Expected a version-1 BIM navigation map manifest in map frame")
    map_source, sim_source = map_data["sceneSource"], description["sceneSource"]
    if (map_data["sourceNamespace"] != description["sourceNamespace"] or
            map_source["layers"] != sim_source["layers"] or not math.isclose(
                _number(map_source["metersPerUnscaledRendererUnit"]),
                _number(sim_source["metersPerUnscaledRendererUnit"]), rel_tol=1e-6)):
        raise ValueError("BIM map and simulator source or metre scale differ")
    if sorted(map_data["openDoorIds"]) != sorted(description["openDoorIds"]):
        raise ValueError("BIM map and simulator door states differ")
    capsule = description["capsule"]
    if any(not math.isclose(_number(map_data[key]), _number(capsule[key]), abs_tol=1e-9)
           for key in ("radiusMeters", "heightMeters")):
        raise ValueError("BIM map and simulator capsule dimensions differ")
    floor = _number(map_data["floorMeters"])
    tolerance = _number(map_data["floorToleranceMeters"])
    if tolerance < 0:
        raise ValueError("BIM map floor tolerance must be nonnegative")
    return floor, tolerance


class Bridge:
    def __init__(self, node, stream, description, initial, floor, trace, max_steps):
        self.node = node
        self.stream = stream
        self.trace = trace
        tick_limit = description.get("tickLimit")
        if type(tick_limit) is not int or tick_limit < 0:
            raise ValueError("Invalid native tickLimit")
        self.max_steps = tick_limit if max_steps is None else min(max_steps, tick_limit)
        self.floor = floor
        self.tick = 0
        self.camera_index = 0
        self.command = (0.0, 0.0)
        self.command_time = None
        self.ticks_per_second = description["ticksPerSecond"]
        if type(self.ticks_per_second) is not int or not 0 < self.ticks_per_second <= 10_000:
            raise ValueError("Invalid native ticksPerSecond")
        self.map_from_odom = _columns(initial["transforms"]["mapFromOdometry"])
        clock_qos = QoSProfile(depth=1, reliability=ReliabilityPolicy.BEST_EFFORT)
        self.clock = node.create_publisher(ClockMessage, "/clock", clock_qos)
        self.odom = node.create_publisher(Odometry, "/odom", 10)
        self.tf = TransformBroadcaster(node)
        self.tf_static = StaticTransformBroadcaster(node)
        node.create_subscription(Twist, "/cmd_vel", self._command, 1)

        calibration = description["camera"]
        self.camera_info = None
        static = [_transform(_MAP, _ODOM, self.map_from_odom, Time(sec=1))]
        self.lidar = None
        if description.get("lidar") is not None:
            self.lidar = LidarPublisher(node, description["lidar"])
            static.append(self.lidar.mount)
        if calibration is not None:
            self.camera_info = _camera_info(calibration)
            self.camera_info_topic = node.create_publisher(CameraInfo, "/camera/camera_info", qos_profile_sensor_data)
            self.rgb = node.create_publisher(Image, "/camera/image_raw", qos_profile_sensor_data)
            self.depth = node.create_publisher(Image, "/camera/depth/image_raw", qos_profile_sensor_data)
            self.depth_info_topic = node.create_publisher(CameraInfo, "/camera/depth/camera_info",
                                                         qos_profile_sensor_data)
            camera = _columns(calibration["baseFromCamera"])
            optical = (camera[0], tuple(-value for value in camera[1]), tuple(-value for value in camera[2]), camera[3])
            static.append(_transform(_BASE, _OPTICAL, optical, Time(sec=1)))
        self._publish(initial, None, expected_tick=0)
        self.tf_static.sendTransform(static)
        self.timer = None
        if not self.finished:
            self.timer = node.create_timer(1 / self.ticks_per_second, self._step, clock=Clock(clock_type=ClockType.STEADY_TIME))

    @property
    def finished(self):
        return self.tick >= self.max_steps

    def _command(self, message):
        linear, angular = message.linear, message.angular
        values = (linear.x, linear.y, linear.z, angular.x, angular.y, angular.z)
        if (not all(map(math.isfinite, values)) or any((linear.y, linear.z, angular.x, angular.y))
                or abs(linear.x) > 10.0 or abs(angular.z) > 50.0):
            self.command_time = None
            self.node.get_logger().warning("Ignoring invalid planar /cmd_vel", throttle_duration_sec=2.0)
            return
        self.command = (linear.x, angular.z)
        self.command_time = time.monotonic()

    def _step(self):
        live = self.command_time is not None and time.monotonic() - self.command_time <= _COMMAND_TIMEOUT
        linear, angular = self.command if live else (0.0, 0.0)
        command = {"linearMetersPerSecond": linear, "angularRadiansPerSecond": angular}
        next_tick = self.tick + 1
        state = self.stream.request("step", tick=next_tick, **command)
        self._publish(state, command, next_tick)
        if self.finished:
            self.timer.cancel()

    def _camera_messages(self, sample, stamp):
        if sample is None:
            return None
        if self.camera_info is None:
            raise ValueError("Native RGB-D sample lacks calibration")
        index = sample["sampleIndex"]
        if type(index) is not int or index != self.camera_index:
            raise ValueError("Native RGB-D sample index is out of order")
        rgb = base64.b64decode(sample["rgb8Base64"], validate=True)
        depth = base64.b64decode(sample["depth32fc1Base64"], validate=True)
        w, h = self.camera_info.width, self.camera_info.height
        if len(rgb) != w * h * 3 or len(depth) != w * h * 4:
            raise ValueError("Native RGB-D sample byte count differs from camera calibration")
        evidence = {"sampleIndex": index, "rgb8Sha256": hashlib.sha256(rgb).hexdigest(),
                    "depth32fc1Sha256": hashlib.sha256(depth).hexdigest()} if self.trace is not None else None
        return _image(rgb, w, h, 3, "rgb8", stamp), _image(depth, w, h, 4, "32FC1", stamp), evidence

    def _publish(self, state, command, expected_tick):
        tick, native_ns = state["tick"], state["timestampNanoseconds"]
        if (type(tick) is not int or tick != expected_tick or type(native_ns) is not int or
                native_ns != tick * _NS_PER_SECOND // self.ticks_per_second):
            raise ValueError("Native BIM simulator did not commit the requested tick and timestamp")
        stamp_ns = native_ns + _ROS_EPOCH_NS
        sec, nanosec = divmod(stamp_ns, _NS_PER_SECOND)
        stamp = Time(sec=sec, nanosec=nanosec)
        state_transforms = state["transforms"]
        if _columns(state_transforms["mapFromOdometry"]) != self.map_from_odom:
            raise ValueError("Native BIM simulator changed the fixed map/odometry alignment")
        _, _, z = _vector(state["base"]["position"], 3)
        if self.floor is not None and abs(z - self.floor[0]) > self.floor[1] + 1e-6:
            raise ValueError(f"Committed robot elevation {z:g} differs from BIM map floor {self.floor[0]:g}")
        transform = _transform(_ODOM, _BASE, _columns(state_transforms["odometryFromBase"]), stamp)
        odom = Odometry()
        odom.header.frame_id = _ODOM
        odom.header.stamp = stamp
        odom.child_frame_id = _BASE
        pose = odom.pose.pose
        pose.position.x, pose.position.y, pose.position.z = (
            transform.transform.translation.x, transform.transform.translation.y, transform.transform.translation.z)
        pose.orientation = transform.transform.rotation
        covariance = _vector(state["odometryCovariance"], 9)
        for row, target_row in enumerate((0, 1, 5)):
            for col, target_col in enumerate((0, 1, 5)):
                odom.pose.covariance[target_row * 6 + target_col] = covariance[row * 3 + col]
        for axis in (2, 3, 4):
            odom.pose.covariance[axis * 6 + axis] = 1e6
        twist = state["twist"]
        odom.twist.twist.linear.x = _number(twist["linearMetersPerSecond"])
        odom.twist.twist.angular.z = _number(twist["angularRadiansPerSecond"])
        messages = self._camera_messages(state["camera"], stamp)
        lidar_message = None
        if state.get("lidar") is not None:
            if self.lidar is None:
                raise ValueError("Native LiDAR sample lacks calibration")
            lidar_message = self.lidar.message(state["lidar"], stamp, stamp_ns)

        clock = ClockMessage()
        clock.clock = stamp
        self.clock.publish(clock)
        self.tf.sendTransform(transform)
        self.odom.publish(odom)
        if lidar_message is not None:
            self.lidar.publish(lidar_message, stamp_ns)
        camera_evidence = None
        if messages is not None:
            rgb, depth, camera_evidence = messages
            self.camera_info.header.stamp = stamp
            self.camera_info_topic.publish(self.camera_info)
            self.depth_info_topic.publish(self.camera_info)
            self.rgb.publish(rgb)
            self.depth.publish(depth)
            self.camera_index += 1
        if self.trace is not None:
            row = {"schema": "container.bim-ros-bridge-trace", "version": 1, "tick": tick,
                   "timestampNanoseconds": native_ns, "rosTimestampNanoseconds": stamp_ns, "command": command,
                   "base": state["base"], "odometry": state["odometry"], "twist": twist,
                   "obstruction": state["obstruction"], "blockingEntityId": state["blockingEntityId"],
                   "contact": state.get("contact"), "camera": camera_evidence}
            self.trace.write(json.dumps(row, allow_nan=False, separators=(",", ":")) + "\n")
        self.tick = tick


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sim-executable", required=True, type=Path)
    parser.add_argument("--scene", required=True, type=Path)
    parser.add_argument("--namespace", required=True, help="BIM source namespace")
    parser.add_argument("--scenario", required=True, type=Path)
    parser.add_argument("--camera", type=Path, help="Optional native RGB-D sensor specification")
    parser.add_argument("--map-manifest", type=Path, help="Validate against the exported waypoints.json")
    parser.add_argument("--trace", type=Path, help="Fresh output path for committed tick evidence")
    parser.add_argument("--max-steps", type=int, help="Stop at this limit or the native tick limit, whichever comes first")
    ros_args = [sys.argv[0], *(sys.argv[1:] if argv is None else argv)]
    args = parser.parse_args(remove_ros_args(ros_args)[1:])
    if args.max_steps is not None and args.max_steps < 0:
        parser.error("--max-steps must be nonnegative")
    try:
        executable = args.sim_executable.resolve(strict=True)
        command = [str(executable), "--stream", native_input_path(executable, args.scene),
                   args.namespace, native_input_path(executable, args.scenario)]
        if args.camera is not None:
            command.extend(("--camera", native_input_path(executable, args.camera)))
        with ExitStack() as stack:
            stream = stack.enter_context(DriveStream(command))
            description = stream.request("describe", timeout=20.0)
            floor = _map_manifest(args.map_manifest, description)
            initial = stream.request("observe")
            trace = stack.enter_context(args.trace.open("x", encoding="utf-8", buffering=1)) if args.trace else None
            rclpy.init(args=ros_args)
            stack.callback(rclpy.try_shutdown)
            node = rclpy.create_node("bim_ros_bridge")
            stack.callback(node.destroy_node)
            executor = SingleThreadedExecutor()
            executor.add_node(node)
            stack.callback(executor.shutdown)
            bridge = Bridge(node, stream, description, initial, floor, trace, args.max_steps)
            while rclpy.ok() and not bridge.finished:
                executor.spin_once(timeout_sec=0.2)
    except (KeyboardInterrupt, ExternalShutdownException):
        return 0
    except (KeyError, TypeError, ValueError, RuntimeError, OSError) as error:
        print(f"BIM ROS bridge: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
