#!/usr/bin/env python3
"""Compare the robotics URDF adapter with Robot State Publisher and tf2."""

import argparse
from contextlib import contextmanager
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET


FIXTURES = Path(__file__).resolve().parents[1] / "fixtures" / "robotics"
DEFAULT_ROBOT = FIXTURES / "two-link-arm.urdf"
DEFAULT_SCENARIO = FIXTURES / "two-link-arm-pose.json"
TOLERANCE = 2e-7
IDENTITY = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0), (0.0, 0.0, 0.0))


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def dot(left, right):
    return sum(a * b for a, b in zip(left, right))


def transpose(columns):
    return tuple(zip(*columns))


def rotate(basis, vector):
    return tuple(dot(row, vector) for row in transpose(basis))


def add(left, right):
    return tuple(a + b for a, b in zip(left, right))


def compose(left, right):
    return tuple(rotate(left[:3], column) for column in right[:3]) + (
        add(rotate(left[:3], right[3]), left[3]),
    )


def relative_to_root(root, world):
    inverse_basis = transpose(root[:3])
    inverse = inverse_basis + (rotate(inverse_basis, tuple(-value for value in root[3])),)
    return compose(inverse, world)


def read_pose(matrix):
    require(len(matrix) == 4 and all(len(column) == 4 for column in matrix),
            "Expected a column-major 4×4 pose in the kinematic report")
    require(all(math.isfinite(value) for column in matrix for value in column),
            "Expected a finite pose in the kinematic report")
    require([column[3] for column in matrix] == [0.0, 0.0, 0.0, 1.0],
            "Expected an affine pose in the kinematic report")
    return tuple(tuple(column[:3]) for column in matrix)


def pose_error(actual, expected):
    return max(abs(a - b) for left, right in zip(actual, expected) for a, b in zip(left, right))


def quaternion_pose(transform):
    x, y, z, w = (transform.rotation.x, transform.rotation.y, transform.rotation.z, transform.rotation.w)
    require(abs(x*x + y*y + z*z + w*w - 1.0) < TOLERANCE,
            "Robot State Publisher returned a non-unit TF rotation")
    translation = (transform.translation.x, transform.translation.y, transform.translation.z)
    require(all(math.isfinite(value) for value in translation),
            "Robot State Publisher returned a nonfinite TF translation")
    return (
        (1 - 2*(y*y + z*z), 2*(x*y + w*z), 2*(x*z - w*y)),
        (2*(x*y - w*z), 1 - 2*(x*x + z*z), 2*(y*z + w*x)),
        (2*(x*z + w*y), 2*(y*z - w*x), 1 - 2*(x*x + y*y)),
        translation,
    )


def origin_pose(origin):
    xyz, rpy = ("0 0 0", "0 0 0") if origin is None else (
        origin.get("xyz", "0 0 0"), origin.get("rpy", "0 0 0")
    )
    roll, pitch, yaw = map(float, rpy.split())
    cr, sr = math.cos(roll), math.sin(roll)
    cp, sp = math.cos(pitch), math.sin(pitch)
    cy, sy = math.cos(yaw), math.sin(yaw)
    return (
        (cy*cp, sy*cp, -sp),
        (cy*sp*sr - sy*cr, sy*sp*sr + cy*cr, cp*sr),
        (cy*sp*cr + sy*sr, sy*sp*cr - cy*sr, cp*cr),
        tuple(map(float, xyz.split())),
    )


def visual_shape(visual):
    shape, = list(visual.find("geometry"))
    if shape.tag == "mesh":
        return {"type": "mesh", "uri": shape.attrib["filename"],
                "scale": list(map(float, shape.get("scale", "1 1 1").split()))}
    if shape.tag == "box":
        return {"type": "box", "size": list(map(float, shape.attrib["size"].split()))}
    if shape.tag == "sphere":
        return {"type": "sphere", "radius": float(shape.attrib["radius"])}
    require(shape.tag == "cylinder", f"Unsupported geometry for the ROS reference check: {shape.tag}")
    return {"type": "cylinder", "radius": float(shape.attrib["radius"]),
            "length": float(shape.attrib["length"])}


def check_visuals(xml_links, reported_links, root_pose, ros_poses):
    errors = []
    for name, reported in reported_links.items():
        source = xml_links[name].findall("visual")
        require(len(reported["visuals"]) == len(source), f"Wrong visual count on {name}")
        for index, (visual, xml_visual) in enumerate(zip(reported["visuals"], source)):
            frame_name = f"{name} visual {index}"
            require(visual["name"] == xml_visual.get("name", ""), f"Changed name of {frame_name}")
            require(visual["geometry"] == visual_shape(xml_visual), f"Changed geometry of {frame_name}")
            origin = origin_pose(xml_visual.find("origin"))
            origin_error = pose_error(read_pose(visual["linkFromVisual"]), origin)
            actual = relative_to_root(root_pose, read_pose(visual["worldFromVisual"]))
            error = pose_error(actual, compose(ros_poses[name], origin))
            require(max(origin_error, error) <= TOLERANCE,
                    f"{frame_name} disagrees with authored origin and ROS TF by {max(origin_error, error):.6g}")
            errors.append(error)
    return errors


def check_joints(xml_joints, reported_joints):
    positions = {}
    require(len(xml_joints) == len(reported_joints) and
            xml_joints.keys() == {joint["name"] for joint in reported_joints},
            "The kinematic report joints disagree with the input URDF")
    for reported in reported_joints:
        name = reported["name"]
        joint = xml_joints[name]
        require(reported["type"] == joint.attrib["type"] and
                all(reported[end] == joint.find(end).attrib["link"] for end in ("parent", "child")),
                f"Changed type or link endpoints on joint {name}")
        origin = origin_pose(joint.find("origin"))
        require(pose_error(read_pose(reported["parentFromJoint"]), origin) <= TOLERANCE,
                f"Changed authored frame on joint {name}")
        if reported["type"] == "fixed":
            continue
        axis_node = joint.find("axis")
        axis = (1.0, 0.0, 0.0) if axis_node is None else tuple(map(float, axis_node.attrib["xyz"].split()))
        length = math.hypot(*axis)
        require(length > 0.0 and len(reported["axis"]) == 3 and
                all(abs(actual - value / length) <= TOLERANCE for actual, value in zip(reported["axis"], axis)),
                f"Changed authored axis on joint {name}")
        positions[name] = reported["position"]
    return positions


def robot_state_publisher_executable():
    from ament_index_python.packages import get_package_prefix

    prefix = Path(get_package_prefix("robot_state_publisher"))
    name = "robot_state_publisher.exe" if os.name == "nt" else "robot_state_publisher"
    executable = prefix / "lib" / "robot_state_publisher" / name
    require(executable.is_file(), f"Could not find Robot State Publisher at {executable}")
    return executable


@contextmanager
def robot_state_publisher(xml, directory):
    params = directory / "robot-state-publisher.yaml"
    params.write_text("/**:\n  ros__parameters:\n    robot_description: " +
                      json.dumps(xml, ensure_ascii=False) + "\n", encoding="utf-8")
    log_path = directory / "robot-state-publisher.log"
    with log_path.open("wb") as log:
        process = subprocess.Popen(
            [str(robot_state_publisher_executable()), "--ros-args", "--params-file", str(params)],
            stdout=log, stderr=subprocess.STDOUT,
        )
        try:
            yield process, log_path
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


def compare_state(state, xml_links, xml_joints, node, tf_buffer, publisher, process, log_path):
    import rclpy
    from rclpy.time import Time
    from sensor_msgs.msg import JointState
    from tf2_ros import TransformException

    require(state.get("schema") == "container.urdf-kinematic-state" and state.get("version") == 1
            and state.get("coordinateConvention") == "ROS REP-103"
            and state.get("matrixLayout") == "column-major", "Unrecognized URDF kinematic report")
    links = {link["name"]: link for link in state["links"]}
    require(len(links) == len(state["links"]) and links.keys() == xml_links.keys(),
            "The kinematic report links disagree with the input URDF")
    root = state["root"]
    root_pose = read_pose(links[root]["worldFromLink"])
    poses = {name: relative_to_root(root_pose, read_pose(link["worldFromLink"]))
             for name, link in links.items()}
    joints = check_joints(xml_joints, state["joints"])
    deadline = time.monotonic() + 25.0
    error_details = "waiting for ROS TF frames"
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("Robot State Publisher exited before producing TF:\n" +
                               log_path.read_text(encoding="utf-8", errors="replace")[-2400:])
        message = JointState()
        message.header.stamp = node.get_clock().now().to_msg()
        message.name = list(joints)
        message.position = list(joints.values())
        publisher.publish(message)
        rclpy.spin_once(node, timeout_sec=0.1)
        ros_poses = {root: IDENTITY}
        try:
            for name in links:
                if name != root:
                    tf = tf_buffer.lookup_transform(root, name, Time()).transform
                    ros_poses[name] = quaternion_pose(tf)
        except TransformException as error:
            error_details = str(error)
            continue
        errors = {name: pose_error(ros_poses[name], expected) for name, expected in poses.items()}
        largest = max(errors, key=errors.get)
        if errors[largest] > TOLERANCE:
            error_details = f"{largest} differs by {errors[largest]:.6g}"
            continue
        visual_errors = check_visuals(xml_links, links, root_pose, ros_poses)
        return {"tick": state["ticks"], "linkCount": len(links), "visualCount": len(visual_errors),
                "maxLinkError": errors[largest], "maxVisualError": max(visual_errors, default=0.0),
                "joints": joints}, ros_poses
    raise RuntimeError(f"Robot State Publisher TF never matched the state: {error_details}")


def generate_states(exporter, urdf, scenario, directory):
    files = []
    for name, motion in (("initial", None), ("posed", scenario)):
        path = directory / f"{name}.json"
        args = [str(exporter), str(urdf), str(path)]
        if motion:
            args.append(str(motion))
        run = subprocess.run(args, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
        require(run.returncode == 0, f"URDF state export {name} failed:\n{run.stdout}{run.stderr}")
        files.append(path)
    return files


def run_reference(args, directory):
    import rclpy
    from sensor_msgs.msg import JointState
    from tf2_ros import Buffer, TransformListener

    # Share a private, local-only ROS domain with the reference child process.
    os.environ["ROS_DOMAIN_ID"] = str(20 + os.getpid() % 80)
    os.environ["ROS_AUTOMATIC_DISCOVERY_RANGE"] = "LOCALHOST"
    os.environ.pop("ROS_STATIC_PEERS", None)
    os.environ["ROS_LOG_DIR"] = str(directory / "ros-logs")
    xml = args.urdf.read_text(encoding="utf-8")
    source = ET.fromstring(xml)
    xml_links = {link.attrib["name"]: link for link in source.findall("link")}
    xml_joints = {joint.attrib["name"]: joint for joint in source.findall("joint")}
    require(xml_links, "No links in the reference URDF")
    files = generate_states(args.exporter, args.urdf, args.scenario, directory) if args.exporter else args.state
    state_reports, poses = [], []
    with robot_state_publisher(xml, directory) as (process, log_path):
        # The subprocess owns its ROS logs. Avoid loading a second native file
        # logger in the Python checker, which can fail on Windows.
        rclpy.get_default_context().init(args=[], initialize_logging=False)
        node, listener = None, None
        try:
            node = rclpy.create_node("vulkan_urdf_reference_check")
            tf_buffer = Buffer()
            listener = TransformListener(tf_buffer, node, spin_thread=False)
            publisher = node.create_publisher(JointState, "joint_states", 10)
            for path in files:
                state = json.loads(path.read_text(encoding="utf-8"))
                report, frame_poses = compare_state(
                    state, xml_links, xml_joints, node, tf_buffer, publisher, process, log_path
                )
                state_reports.append(report)
                poses.append(frame_poses)
        finally:
            if listener is not None:
                listener.unregister()
            if node is not None:
                node.destroy_node()
            rclpy.shutdown()

    moved = any(pose_error(first[name], second[name]) > TOLERANCE
                for first, second in zip(poses, poses[1:]) for name in first)
    require(not args.exporter or moved, "The demo's initial and posed states did not move in ROS TF")
    summary = {"passed": True, "reference": "ROS 2 Robot State Publisher / tf2",
               "robot": source.get("name"), "motionVerified": moved, "states": state_reports}
    result = json.dumps(summary, indent=2)
    (directory / "reference-results.json").write_text(result + "\n", encoding="utf-8")
    print(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--urdf", type=lambda p: Path(p).resolve(), default=DEFAULT_ROBOT)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--exporter", type=lambda p: Path(p).resolve(), help="BimRoboticsUrdf executable")
    inputs.add_argument("--state", action="append", type=lambda p: Path(p).resolve(),
                        help="Compare a kinematic JSON report. Repeat to check multiple poses.")
    parser.add_argument("--scenario", type=lambda p: Path(p).resolve(), default=DEFAULT_SCENARIO,
                        help="Pose scenario used to generate the second state with --exporter")
    parser.add_argument("--output-dir", type=lambda p: Path(p).resolve(),
                        help="New directory for retaining results, ROS logs and generated states")
    args = parser.parse_args()
    try:
        import rclpy  # noqa: F401
        import sensor_msgs.msg  # noqa: F401
        import tf2_ros  # noqa: F401
        from ament_index_python.packages import PackageNotFoundError
    except ModuleNotFoundError as error:
        print(f"SKIP: ROS 2 Python modules unavailable: {error}", file=sys.stderr)
        return 77
    try:
        robot_state_publisher_executable()
        if args.output_dir:
            args.output_dir.mkdir(parents=True, exist_ok=False)
            run_reference(args, args.output_dir)
        else:
            with tempfile.TemporaryDirectory(prefix="urdf-ros-reference-") as directory:
                run_reference(args, Path(directory))
        return 0
    except PackageNotFoundError:
        print("SKIP: the ROS 2 robot_state_publisher package is unavailable", file=sys.stderr)
        return 77
    except (ET.ParseError, OSError, RuntimeError, TypeError, ValueError, KeyError) as error:
        print(f"ROS URDF reference check: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
