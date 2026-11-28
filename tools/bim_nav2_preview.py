#!/usr/bin/env python3
"""Export BIM maps, drive the doorway demo with Nav2 Jazzy and verify its recording."""

import argparse
import json
import math
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
HELPERS = ROOT / "tests" / "robotics"
FIXTURES = ROOT / "tests" / "fixtures" / "robotics"
BRIDGE = ROOT / "ros2" / "bim_ros_bridge"
NATIVE_TOOLS = ("BimRoboticsNavMap", "BimRoboticsCollision", "BimRoboticsSim")
ROS_PACKAGES = (
    "nav2_map_server", "nav2_planner", "nav2_navfn_planner", "nav2_controller",
    "nav2_regulated_pure_pursuit_controller", "rclpy", "tf2_ros_py", "ros2bag",
    "rosbag2_py", "rosbag2_storage_mcap",
)


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def read_json(path):
    return json.loads(path.read_bytes())


def prerequisites(directory):
    if sys.platform != "linux":
        raise ValueError("Run the Nav2 preview with Linux-native binaries on Ubuntu 24.04 or Ubuntu under WSL 2.")
    if os.environ.get("ROS_DISTRO") != "jazzy":
        raise ValueError("Source /opt/ros/jazzy/setup.bash before running the Nav2 preview.")
    missing_tools = [name for name in ("bash", "colcon", "ros2") if not shutil.which(name)]
    if missing_tools:
        raise ValueError("Missing commands: " + ", ".join(missing_tools))
    try:
        from ament_index_python.packages import get_packages_with_prefixes
        import yaml
    except ImportError as error:
        raise ValueError(f"The current Python needs Jazzy and python3-yaml: {error}") from error
    if not callable(yaml.safe_load):
        raise ValueError("The current Python needs PyYAML.")
    prefixes = get_packages_with_prefixes()
    missing_packages = [name for name in ROS_PACKAGES if name not in prefixes]
    if missing_packages:
        raise ValueError("Missing ROS 2 Jazzy packages: " + ", ".join(missing_packages))
    binaries = tuple(directory / name for name in NATIVE_TOOLS)
    for binary in binaries:
        if not binary.is_file() or not os.access(binary, os.X_OK):
            raise ValueError(f"Missing executable {binary}. Point --native-bin at a physics-enabled Linux build.")
        with binary.open("rb") as executable:
            if executable.read(4) != b"\x7fELF":
                raise ValueError(f"Expected a Linux ELF executable: {binary}")
    for path in (
        BRIDGE / "package.xml", BRIDGE / "setup.py", BRIDGE / "bim_ros_bridge" / "node.py",
        *(HELPERS / name for name in ("bim_nav2_map.py", "bim_nav2_planner.py", "bim_ros2_bag.py",
                                      "bim_ros2_nav2_demo.py", "bim_ros2_drive_verify.py")),
        *(FIXTURES / name for name in ("doorway.ifc", "room-connectivity.ifc", "room-connectivity-metres.ifc",
                                       "doorway-ros2-drive.json", "doorway-ros2-camera.json")),
    ):
        if not path.is_file():
            raise ValueError(f"Missing preview input {path}. Extract the full preview archive.")
    return binaries


def stop_group(process, interrupt):
    try:
        os.killpg(process.pid, signal.SIGINT if interrupt else signal.SIGTERM)
        process.wait(timeout=5)
    except ProcessLookupError:
        pass
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def run_stage(name, command, output, scratch, environment, timeout):
    log = output / "logs" / f"{name}.log"
    print(f"{name} · {log}", flush=True)
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen(command, cwd=scratch, env=environment, stdin=subprocess.DEVNULL,
                                   stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            stop_group(process, interrupt=False)
            raise RuntimeError(f"{name} exceeded {timeout:g} s. See {log}") from error
        except KeyboardInterrupt:
            stop_group(process, interrupt=True)
            raise
    if code != 0:
        raise RuntimeError(f"{name} exited with status {code}. See {log}")


def run_preview(args, binaries, environment):
    exporter, collision, simulator = binaries
    python = [sys.executable, "-B"]
    output = args.output
    maps_output = output / "maps"
    with tempfile.TemporaryDirectory(prefix="bim-nav2-preview-") as work:
        # Keep the active trace and bag in local scratch while their producers
        # hold them open. The requested evidence directory can be a shared drive.
        scratch = Path(work)
        bridge_source = shutil.copytree(BRIDGE, scratch / "bridge-source")
        bridge_install = scratch / "bridge-install"
        navigation = scratch / "navigation"
        try:
            run_stage("bridge-build", ["colcon", "--log-base", scratch / "colcon-logs", "build",
                      "--paths", bridge_source, "--build-base", scratch / "bridge-build",
                      "--install-base", bridge_install], output, scratch, environment, 180)
            run_stage("map-export", [*python, HELPERS / "bim_nav2_map.py", "--exporter", exporter,
                      "--collision", collision, "--fixtures", FIXTURES, "--output", maps_output],
                      output, scratch, environment, 240)
            bundles = tuple(maps_output.glob("run-*/cases.json"))
            if len(bundles) != 1:
                raise RuntimeError("The map exporter did not produce exactly one map bundle. See logs/map-export.log")
            maps = bundles[0].parent
            demo = [*python, HELPERS / "bim_ros2_nav2_demo.py", "--sim-executable", simulator,
                    "--maps", maps, "--fixtures", FIXTURES, "--output", navigation, "--timeout", str(args.timeout)]
            run_stage("nav2-drive-and-bag", ["bash", "-c", 'source "$1" && shift && exec "$@"', "bim-nav2-preview",
                      bridge_install / "local_setup.bash", *demo], output, scratch, environment, args.timeout + 180)
            run_stage("drive-verification", [*python, HELPERS / "bim_ros2_drive_verify.py", "--simulator", simulator,
                      "--collision", collision, "--evidence", navigation, "--maps", maps, "--fixtures", FIXTURES],
                      output, scratch, environment, 180)
        finally:
            if navigation.is_dir():
                shutil.copytree(navigation, output / "navigation", ignore_dangling_symlinks=True)

    drive = read_json(output / "navigation" / "drive-verification.json")
    bag = read_json(output / "navigation" / "bag-verification.json")
    verification = {
        "schema": "container.bim-nav2-preview",
        "version": 1,
        "rosDistro": "jazzy",
        "case": "doorway-open",
        "mapBundle": maps.relative_to(output).as_posix(),
        "navigationReport": "navigation/navigation.json",
        "recording": "navigation/observations",
        "driveVerification": "navigation/drive-verification.json",
        "bagVerification": "navigation/bag-verification.json",
        "goalErrorsMeters": drive["goalErrorsMeters"],
        "sweptDriveSegments": drive["sweptDriveSegments"],
        "reproducedCameraFrames": drive["reproducedCameraFrames"],
        "bagReplayPayloadsMatch": bag["replay"]["payloadsMatch"],
    }
    staged = output / ".preview-summary.json"
    write_json(staged, verification)
    staged.replace(output / "preview-summary.json")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-bin", type=Path, default=ROOT / "bin",
                        help="Linux build directory containing the three BIM robotics executables (default: archive bin/)")
    parser.add_argument("--output", type=Path, required=True, help="Fresh directory for maps, logs and verified navigation")
    parser.add_argument("--timeout", type=float, default=75.0, help="Live Nav2 deadline in seconds")
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")
    try:
        args.output = args.output.resolve()
        binaries = prerequisites(args.native_bin.resolve())
        args.output.mkdir(parents=True)
        (args.output / "logs").mkdir()
    except (OSError, ValueError) as error:
        parser.exit(2, f"BIM Nav2 preview setup: {error}\n")
    environment = dict(os.environ)
    environment.pop("PYTHONOPTIMIZE", None)
    try:
        run_preview(args, binaries, environment)
    except KeyboardInterrupt:
        print(f"BIM Nav2 preview interrupted. Partial evidence: {args.output}", file=sys.stderr)
        return 130
    except (KeyError, OSError, RuntimeError, ValueError) as error:
        print(f"BIM Nav2 preview failed: {error}. Evidence: {args.output}", file=sys.stderr)
        return 1
    print(f"BIM Nav2 preview verified. Summary: {args.output / 'preview-summary.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
