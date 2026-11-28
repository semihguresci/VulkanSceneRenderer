"""Compose a BIM export with a visual replay of a committed robot trajectory."""

import json
import math
from pathlib import Path
from tempfile import NamedTemporaryFile


def _finite_number(value):
    if type(value) not in (int, float):
        raise ValueError("Expected a finite number")
    try:
        number = float(value)
    except OverflowError as error:
        raise ValueError("Expected a finite number") from error
    if not math.isfinite(number):
        raise ValueError("Expected a finite number")
    return number


def _poses(history):
    if not isinstance(history, list) or not history:
        raise ValueError("Simulation history must include the committed pose at tick zero")

    previous_yaw = None
    turns = 0
    poses = []
    for tick, entry in enumerate(history):
        if not isinstance(entry, dict) or type(entry.get("tick")) is not int or entry["tick"] != tick:
            raise ValueError(f"Expected a committed pose at tick {tick}")
        base = entry.get("base")
        position = base.get("position") if isinstance(base, dict) else None
        if not isinstance(position, list) or len(position) != 3:
            raise ValueError(f"Expected a three-component base position at tick {tick}")
        try:
            x, y, z = map(_finite_number, position)
            yaw = math.remainder(_finite_number(base.get("yaw")), math.tau)
        except ValueError as error:
            raise ValueError(f"Expected finite base position and yaw at tick {tick}") from error

        # Preserve the pose and avoid a full turn between samples that wrap at pi.
        if previous_yaw is not None:
            delta = yaw - previous_yaw
            if delta > math.pi:
                turns -= 1
            elif delta < -math.pi:
                turns += 1
        previous_yaw = yaw
        poses.append((f"({x}, {y}, {z})", math.degrees(yaw + turns * math.tau)))
    return poses


def write_replay_stage(path, scene_name, report):
    """Write a USDA wrapper beside an exported BIM stage, with a visual proxy at each committed pose."""
    path = Path(path)
    if (not isinstance(scene_name, str) or not scene_name.strip() or scene_name in (".", "..") or
            any(char in "@/\\:" or ord(char) < 32 for char in scene_name) or
            Path(scene_name) == Path(path.name)):
        raise ValueError("The scene must be a separate file in the replay stage directory")
    if (not isinstance(report, dict) or report.get("schema") != "container.bim-robotics-simulation" or
            type(report.get("version")) is not int or report["version"] != 1):
        raise ValueError("Expected a version 1 BIM robotics simulation report")
    rate = report.get("ticksPerSecond")
    if type(rate) is not int or rate <= 0:
        raise ValueError("Simulation ticksPerSecond must be a positive integer")

    poses = _poses(report.get("history"))
    first_position, first_rotation = poses[0]
    translations = ",\n".join(f"            {tick}: {pose[0]}" for tick, pose in enumerate(poses))
    rotations = ",\n".join(f"            {tick}: {pose[1]}" for tick, pose in enumerate(poses))
    robot_id = report.get("robotId")
    robot_id_property = ""
    if robot_id is not None:
        if not isinstance(robot_id, str) or not robot_id:
            raise ValueError("Simulation robotId must be a nonempty string")
        robot_id_property = f"        custom string robotics:robotId = {json.dumps(robot_id, ensure_ascii=False)}\n"

    layer = f'''#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Z"
    metersPerUnit = 1
    startTimeCode = 0
    endTimeCode = {len(poses) - 1}
    timeCodesPerSecond = {rate}
    framesPerSecond = {rate}
    subLayers = [@{scene_name}@]
)

over "World"
{{
    def Xform "ReplayRobot"
    {{
        custom string robotics:role = "recorded-trajectory-visual-proxy"
{robot_id_property}        double3 xformOp:translate = {first_position}
        double3 xformOp:translate.timeSamples = {{
{translations}
        }}
        double xformOp:rotateZ = {first_rotation}
        double xformOp:rotateZ.timeSamples = {{
{rotations}
        }}
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateZ"]

        def Cube "Chassis"
        {{
            color3f[] primvars:displayColor = [(0.09, 0.52, 0.81)]
            double3 xformOp:translate = (0, 0, 0.15)
            double3 xformOp:scale = (0.22, 0.18, 0.15)
            uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]
        }}
        def Cube "FrontMarker"
        {{
            color3f[] primvars:displayColor = [(1, 0.58, 0.08)]
            double3 xformOp:translate = (0.19, 0, 0.31)
            double3 xformOp:scale = (0.045, 0.12, 0.02)
            uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]
        }}
    }}
}}
'''
    with NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n", dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        try:
            stream.write(layer)
            stream.close()
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)
