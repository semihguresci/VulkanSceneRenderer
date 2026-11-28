"""Check recorded BIM observations and replay their MCAP bag in isolated Jazzy DDS."""

from bisect import bisect_left
from collections import Counter
from contextlib import ExitStack
import hashlib
import json
import math
import os
from pathlib import Path
import random
import struct
import subprocess
import time
from unittest.mock import patch


_TYPES = {
    "/clock": "rosgraph_msgs/msg/Clock",
    "/tf": "tf2_msgs/msg/TFMessage",
    "/tf_static": "tf2_msgs/msg/TFMessage",
    "/odom": "nav_msgs/msg/Odometry",
    "/cmd_vel": "geometry_msgs/msg/Twist",
    "/map": "nav_msgs/msg/OccupancyGrid",
    "/camera/image_raw": "sensor_msgs/msg/Image",
    "/camera/camera_info": "sensor_msgs/msg/CameraInfo",
    "/camera/depth/image_raw": "sensor_msgs/msg/Image",
    "/camera/depth/camera_info": "sensor_msgs/msg/CameraInfo",
}
BAG_TOPICS = tuple(_TYPES)
_STATIC_TOPICS = {"/map", "/tf_static"}
_RGB, _INFO, _DEPTH = "/camera/image_raw", "/camera/camera_info", "/camera/depth/image_raw"
_DEPTH_INFO = "/camera/depth/camera_info"
_OPTICAL = "camera_optical_frame"
_SECOND = 1_000_000_000
_NATIVE_TICK_NS = (_SECOND + 59) // 60


def _check(condition, message):
    if not condition:
        raise ValueError(message)


def _ns(stamp):
    return stamp.sec * _SECOND + stamp.nanosec


def _pose(position, orientation):
    return (position.x, position.y, position.z, orientation.x, orientation.y, orientation.z, orientation.w)


def _nearest_skew(stamp, timeline):
    index = bisect_left(timeline, stamp)
    return min(abs(stamp - time_ns) for time_ns in timeline[max(index - 1, 0):index + 1])


def _digest(message):
    from rosidl_runtime_py.convert import message_to_ordereddict

    # CDR padding is transport-dependent. Hash the typed fields, including
    # every byte of the image data, independently of that padding.
    payload = json.dumps(message_to_ordereddict(message), allow_nan=False, separators=(",", ":"))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


class _RecordedObservations:
    def __init__(self):
        self.clock = []
        self.pose = {}
        self.tf = {}
        self.statics = set()
        self.frames = {_RGB: {}, _INFO: {}, _DEPTH: {}, _DEPTH_INFO: {}}
        self.intrinsics = None
        self.finite_depth = 0
        self.nan_depth = 0
        self.depth_min = math.inf
        self.depth_max = 0.0
        self.nonzero_commands = 0
        self.rgb_nonzero = False

    def _camera(self, topic, message):
        stamp = _ns(message.header.stamp)
        size = (message.width, message.height)
        _check(message.header.frame_id == _OPTICAL and min(size) > 0, f"Incorrect optical frame or size on {topic}")
        _check(stamp > 0 and stamp not in self.frames[topic], f"Duplicate or invalid image stamp on {topic}: {stamp}")
        self.frames[topic][stamp] = size
        if topic in (_INFO, _DEPTH_INFO):
            fx, fy, cx, cy = message.k[0], message.k[4], message.k[2], message.k[5]
            _check(all(map(math.isfinite, (fx, fy, cx, cy))) and fx > 0 and fy > 0 and
                   tuple(message.k) == (fx, 0, cx, 0, fy, cy, 0, 0, 1) and
                   tuple(message.p) == (fx, 0, cx, 0, 0, fy, cy, 0, 0, 0, 1, 0) and
                   tuple(message.r) == (1, 0, 0, 0, 1, 0, 0, 0, 1) and
                   message.distortion_model == "plumb_bob" and tuple(message.d) == (0,) * 5,
                   "CameraInfo pinhole matrices or distortion differ from the native calibrated camera")
            calibration = (*size, fx, fy, cx, cy)
            if self.intrinsics is None:
                self.intrinsics = calibration
            _check(calibration == self.intrinsics, "CameraInfo intrinsics changed during the run")
            return

        encoding, pixel_bytes = ("rgb8", 3) if topic == _RGB else ("32FC1", 4)
        _check(message.encoding == encoding and message.is_bigendian == 0 and
               message.step == message.width * pixel_bytes and len(message.data) == message.step * message.height,
               f"Wrong byte layout for {topic} at {stamp}")
        if topic == _RGB:
            self.rgb_nonzero = self.rgb_nonzero or any(message.data)
            return
        for (depth,) in struct.iter_unpack("<f", bytes(message.data)):
            if math.isnan(depth):
                self.nan_depth += 1
            elif math.isfinite(depth) and depth > 0:
                self.finite_depth += 1
                self.depth_min = min(self.depth_min, depth)
                self.depth_max = max(self.depth_max, depth)
            else:
                raise ValueError(f"Invalid axial depth {depth} at {stamp}; missing pixels must use NaN")

    def update(self, topic, message):
        if topic in self.frames:
            self._camera(topic, message)
        elif topic == "/clock":
            stamp = _ns(message.clock)
            _check(stamp > 0 and (not self.clock or stamp > self.clock[-1]), "Simulation clock failed to advance")
            self.clock.append(stamp)
        elif topic == "/odom":
            stamp = _ns(message.header.stamp)
            pose = _pose(message.pose.pose.position, message.pose.pose.orientation)
            _check((message.header.frame_id, message.child_frame_id) == ("odom", "base_link") and
                   stamp > 0 and stamp not in self.pose and all(map(math.isfinite, pose)),
                   f"Invalid odometry frame or state at {stamp}")
            self.pose[stamp] = pose
        elif topic in ("/tf", "/tf_static"):
            for transform in message.transforms:
                edge = (transform.header.frame_id, transform.child_frame_id)
                stamp = _ns(transform.header.stamp)
                pose = _pose(transform.transform.translation, transform.transform.rotation)
                _check(stamp > 0 and all(map(math.isfinite, pose)) and math.isclose(math.hypot(*pose[3:]), 1.0, abs_tol=1e-6),
                       f"Invalid TF pose for {edge} at {stamp}")
                if topic == "/tf_static":
                    self.statics.add(edge)
                else:
                    _check(edge == ("odom", "base_link") and stamp not in self.tf, f"Invalid dynamic TF edge {edge}")
                    self.tf[stamp] = pose
        elif topic == "/cmd_vel":
            linear, angular = message.linear, message.angular
            _check(all(map(math.isfinite, (linear.x, angular.z))) and
                   not any((linear.y, linear.z, angular.x, angular.y)), "Recorded command is not finite planar Twist")
            self.nonzero_commands += bool(linear.x or angular.z)
        elif topic == "/map":
            _check(message.header.frame_id == "map" and message.info.resolution > 0 and
                   len(message.data) == message.info.width * message.info.height and
                   {0, 100}.issubset(message.data) and set(message.data) <= {0, -1, 100},
                   "Recorded /map is not the expected trinary BIM occupancy grid")

    def summary(self):
        _check(self.statics == {("map", "odom"), ("base_link", _OPTICAL)},
               f"Incorrect static TF frames: {sorted(self.statics)}")
        _check(len(self.clock) > 1 and len(self.pose) > 1 and self.nonzero_commands > 0,
               "Bag lacks advancing simulated time, odometry, or real controller commands")
        rgb_frames = self.frames[_RGB]
        _check(len(rgb_frames) > 1 and all(frames == rgb_frames for frames in self.frames.values()),
               "RGB-D images and both CameraInfo streams do not share the same stamps and dimensions")
        _check(self.rgb_nonzero and self.finite_depth > 0 and self.nan_depth > 0,
               "RGB-D observations lack colour, positive depth hits, or NaN misses")
        stamps = self.pose.keys() & self.tf.keys()
        _check(self.frames[_RGB].keys() <= stamps, "An RGB-D frame is missing its committed odometry or dynamic TF")
        for stamp in stamps:
            _check(self.pose[stamp] == self.tf[stamp], f"Odometry and TF disagree at {stamp}")
        skew = max(_nearest_skew(stamp, self.clock) for stamp in self.pose)
        _check(skew <= _NATIVE_TICK_NS, f"An odometry stamp differs from /clock by {skew} ns (over one native tick)")
        width, height, fx, fy, cx, cy = self.intrinsics
        return {"camera": {"frames": len(self.frames[_RGB]), "width": width, "height": height,
                           "fx": fx, "fy": fy, "cx": cx, "cy": cy, "frameId": _OPTICAL,
                           "finiteDepthPixels": self.finite_depth, "nanDepthPixels": self.nan_depth,
                           "minDepthMeters": self.depth_min, "maxDepthMeters": self.depth_max},
                "clockSamples": len(self.clock), "odometrySamples": len(self.pose),
                "odometryTfMatches": len(stamps), "maxClockSkewNanoseconds": skew,
                "nonzeroCommands": self.nonzero_commands}


def _read(bag):
    from rclpy.serialization import deserialize_message
    import rosbag2_py
    from rosidl_runtime_py.utilities import get_message

    reader = rosbag2_py.SequentialReader()
    reader.open(rosbag2_py.StorageOptions(uri=str(bag), storage_id="mcap"), rosbag2_py.ConverterOptions("cdr", "cdr"))
    try:
        metadata = {topic.name: topic for topic in reader.get_all_topics_and_types()}
        for name, kind in _TYPES.items():
            _check(name in metadata and metadata[name].type == kind and metadata[name].serialization_format == "cdr",
                   f"Bag lacks expected CDR topic {name} ({kind})")
        types = {name: get_message(kind) for name, kind in _TYPES.items()}
        digests = {name: Counter() for name in _TYPES}
        observations = _RecordedObservations()
        first, last = None, 0
        while reader.has_next():
            name, cdr, received_ns = reader.read_next()
            if name not in types:
                continue
            first = received_ns if first is None else min(first, received_ns)
            last = max(last, received_ns)
            message = deserialize_message(cdr, types[name])
            observations.update(name, message)
            digests[name][_digest(message)] += 1
        counts = {topic: sum(counter.values()) for topic, counter in digests.items()}
        _check(all(counts.values()), f"Bag has empty required topics: {counts}")
        _check(digests[_INFO] == digests[_DEPTH_INFO], "Recorded RGB and depth CameraInfo payloads differ")
        _check(first is not None and first > 0 and last > first, "Bag has no positive simulation recording range")
        return types, digests, counts, observations.summary(), (last - first) / _SECOND
    finally:
        reader.close()


def _stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2.0)


def _replay(bag, output, types, expected, counts, duration):
    import rclpy
    from rclpy.context import Context
    from rclpy.executors import SingleThreadedExecutor
    from rclpy.qos import DurabilityPolicy, QoSProfile, ReliabilityPolicy
    from rosbag2_interfaces.srv import Resume

    caller = int(os.environ.get("ROS_DOMAIN_ID", "0"))
    # The live fixture uses domains 10–99. Keep replay in the separate Linux
    # DDS port range above the default ephemeral port allocation.
    domain = random.SystemRandom().choice([value for value in range(215, 232) if value != caller])
    log_path = output.with_suffix(".play.log")
    qos_path = output.with_suffix(".play-qos.yaml")
    qos = {topic: {"history": "keep_last", "depth": 64, "reliability": "reliable",
                   "durability": "transient_local" if topic in _STATIC_TOPICS else "volatile"} for topic in types}
    # JSON is a YAML subset and keeps this ROS-only test free of an extra parser dependency.
    qos_path.write_text(json.dumps(qos, indent=2) + "\n", encoding="utf-8")
    replayed = {name: Counter() for name in types}
    received = Counter()

    def record(message, topic):
        replayed[topic][_digest(message)] += 1
        received[topic] += 1

    with patch.dict(os.environ, {"ROS_AUTOMATIC_DISCOVERY_RANGE": "LOCALHOST"}), ExitStack() as stack:
        os.environ.pop("ROS_STATIC_PEERS", None)
        context = Context()
        rclpy.init(args=[], context=context, domain_id=domain)
        stack.callback(rclpy.try_shutdown, context=context)
        node = rclpy.create_node("bim_bag_replay_check", context=context)
        stack.callback(node.destroy_node)
        executor = SingleThreadedExecutor(context=context)
        executor.add_node(node)
        stack.callback(executor.shutdown)
        subscribers = {}
        for topic, kind in types.items():
            durability = DurabilityPolicy.TRANSIENT_LOCAL if topic in _STATIC_TOPICS else DurabilityPolicy.VOLATILE
            profile = QoSProfile(depth=64, reliability=ReliabilityPolicy.RELIABLE, durability=durability)
            subscribers[topic] = node.create_subscription(kind, topic, lambda msg, name=topic: record(msg, name), profile)
        resume = node.create_client(Resume, "/rosbag2_player/resume")
        stream = stack.enter_context(log_path.open("w", encoding="utf-8"))
        player = subprocess.Popen(
            ["ros2", "bag", "play", str(bag), "--start-paused", "--disable-keyboard-controls",
             "--wait-for-all-acked", "2000", "--qos-profile-overrides-path", str(qos_path)],
            stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT,
            env=dict(os.environ, ROS_DOMAIN_ID=str(domain)),
        )
        stack.callback(_stop, player)

        startup = time.monotonic() + 20
        while not (resume.service_is_ready() and all(sub.get_publisher_count() for sub in subscribers.values())):
            _check(player.poll() is None and time.monotonic() < startup, f"Paused bag player did not start. See {log_path}")
            executor.spin_once(timeout_sec=0.1)
        resumed = resume.call_async(Resume.Request())
        executor.spin_until_future_complete(resumed, timeout_sec=5.0)
        _check(resumed.done() and resumed.result() is not None, f"Could not resume paused bag playback. See {log_path}")

        deadline = time.monotonic() + duration + 15
        exited_at = None
        while time.monotonic() < deadline:
            code = player.poll()
            if code is not None:
                _check(code == 0, f"ros2 bag play exited {code}. See {log_path}")
                if exited_at is None:
                    exited_at = time.monotonic()
                if all(received[name] >= count for name, count in counts.items()) or time.monotonic() - exited_at >= 2:
                    break
            executor.spin_once(timeout_sec=0.05)
        _check(player.poll() == 0, f"Bag player did not finish before deadline. See {log_path}")
        for name, counter in expected.items():
            _check(replayed[name] == counter,
                   f"DDS replay differed on {name}: expected {counts[name]}, received {received[name]}, "
                   f"{sum((counter - replayed[name]).values())} missing payloads. See {log_path}")
    return domain


def verify_and_replay_bag(bag: Path, output: Path) -> dict:
    """Validate an MCAP bag, replay its ROS graph, and publish JSON evidence."""
    bag = bag.resolve(strict=True)
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    types, digests, counts, summary, duration = _read(bag)
    domain = _replay(bag, output, types, digests, counts, duration)
    result = {"schema": "container.bim-ros2-bag-evidence", "version": 1, "storage": "mcap",
              "topicCounts": counts, "topicTypes": _TYPES, "durationSeconds": duration, **summary,
              "replay": {"domainId": domain, "payloadsMatch": True, "topicCounts": counts}}
    output.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return result
