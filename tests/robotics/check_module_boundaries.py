#!/usr/bin/env python3
"""Check the generated CMake dependency graph for the headless robotics modules."""

import argparse
from collections import deque
import json
from pathlib import Path
import re


PREFIX = "VulkanSceneRenderer_"

# Keep the computational modules isolated from importers and editor targets.
# The order here also makes the test output a readable dependency inventory.
CORE = {
    "robotics_clock": set(),
    "robotics_drive": set(),
    "robotics_sensors": {"robotics_drive", "fingerprint"},
    "robotics_dataset": {"robotics_sensors", "robotics_drive", "fingerprint"},
    "robotics_twin": {"robotics_scene"},
    "robotics_scene": set(),
    "robotics_collision_geometry": {"robotics_scene"},
    "robotics_usd": {"robotics_scene", "robotics_collision_geometry", "fingerprint"},
}
PHYSICS = {
    "physics_world": set(),
    "robotics_collision": {"physics_world", "robotics_collision_geometry", "robotics_scene"},
    "robotics_simulation": {
        "robotics_clock", "robotics_drive", "robotics_collision",
        "robotics_collision_geometry", "robotics_scene", "physics_world",
    },
}
EDITOR = {
    "VulkanSceneRenderer", PREFIX + "Core", PREFIX + "renderer",
    PREFIX + "scene", PREFIX + "ui", PREFIX + "window", PREFIX + "physics",
}
SDK_LIBRARY = re.compile(
    r'(?:^|[\s"/\\])(?:-l|lib)?'
    r'(?:rclcpp|rcl|rosidl[\w_]*|rmw[\w_]*|tf2_ros|isaac_?(?:sim|lab)|omni\.isaac[\w.]*)'
    r'(?:$|[\s".-])',
    re.IGNORECASE,
)


def read_json(path):
    return json.loads(path.read_bytes())


def load_targets(build, config):
    reply = build / ".cmake" / "api" / "v1" / "reply"
    indexes = sorted(reply.glob("index-*.json"))
    if not indexes:
        raise ValueError(f"CMake File API reply is missing in {reply}. Reconfigure the test build.")
    index = read_json(indexes[-1])
    models = [item for item in index["objects"]
              if item["kind"] == "codemodel" and item["version"]["major"] == 2]
    if not models:
        raise ValueError(f"{indexes[-1]} has no codemodel v2 reply.")
    model = read_json(reply / models[0]["jsonFile"])
    selected = next((entry for entry in model["configurations"] if entry["name"] == config), None)
    if selected is None:
        names = ", ".join(repr(entry["name"]) for entry in model["configurations"])
        raise ValueError(f"Configuration {config!r} is missing. Available configurations are {names}.")
    return {entry["name"]: read_json(reply / entry["jsonFile"]) for entry in selected["targets"]}


def paths_from(start, graph):
    paths = {start: (start,)}
    pending = deque([start])
    while pending:
        parent = pending.popleft()
        for child in graph[parent]:
            if child not in paths:
                paths[child] = (*paths[parent], child)
                pending.append(child)
    del paths[start]
    return paths


def check(build, config, physics, urdf):
    targets = load_targets(build, config)
    names = {target["id"]: name for name, target in targets.items()}
    graph = {name: [names[edge["id"]] for edge in target.get("dependencies", [])]
             for name, target in targets.items()}
    expected = dict(CORE)
    if physics:
        expected.update(PHYSICS)
    if urdf:
        expected["urdf"] = {"robotics_clock"}
        if physics:
            expected["urdf_physics"] = {"urdf", "robotics_clock", "physics_world"}

    for suffix, role_deps in expected.items():
        name = PREFIX + suffix
        if name not in graph:
            raise ValueError(f"The configured target graph is missing {name}.")
        cxx = [group for group in targets[name].get("compileGroups", []) if group["language"] == "CXX"]
        if not cxx or any(int(group.get("languageStandard", {}).get("standard", 0)) < 23 for group in cxx):
            raise ValueError(f"{name} must compile as C++23 or newer.")
        paths = paths_from(name, graph)
        project_deps = {dep.removeprefix(PREFIX) for dep in paths if dep.startswith(PREFIX)}
        if project_deps != role_deps:
            missing = sorted(role_deps - project_deps)
            unexpected = [" -> ".join(paths[PREFIX + dep]) for dep in sorted(project_deps - role_deps)]
            raise ValueError(f"{name}: missing module dependencies {missing}, unexpected paths {unexpected}.")
        if (suffix in CORE or suffix == "urdf") and "Jolt" in paths:
            raise ValueError(f"Unexpected physics dependency: {' -> '.join(paths['Jolt'])}.")
        if suffix == "physics_world" and "Jolt" not in paths:
            raise ValueError(f"{name} must link the Jolt backend.")
        print(f"{name}: {', '.join(sorted(project_deps)) or '(no project-library dependencies)'}")

    optional = {
        "BimRoboticsCollision": physics,
        "BimRoboticsSim": physics,
        PREFIX + "robotics_diagnostics": physics,
        **{PREFIX + suffix: physics for suffix in PHYSICS},
        "BimRoboticsUrdf": urdf,
        PREFIX + "urdf": urdf,
        PREFIX + "urdf_assets": urdf,
        PREFIX + "urdf_physics": physics and urdf,
    }
    for name, enabled in optional.items():
        if (name in targets) != enabled:
            expected_state = "present" if enabled else "absent"
            raise ValueError(f"{name} must be {expected_state} with physics={physics}, URDF={urdf}.")

    standalone_tests = {"drive_actuation_tests": "robotics_drive",
                        "drive_sensor_tests": "robotics_sensors",
                        "raycast_lidar_tests": "robotics_sensors",
                        "dataset_simulation_report_tests": "robotics_dataset",
                        "robotics_dataset_reader_tests": "robotics_dataset",
                        "twin_replay_tests": "robotics_twin"}
    if physics:
        standalone_tests["physics_world_tests"] = "physics_world"
    executables = {"BimRoboticsAssets": {"robotics_assets"},
                   "BimRoboticsScene": {"robotics_scene"},
                   "BimRoboticsUsd": {"robotics_usd"},
                   "BimRoboticsDataset": {"robotics_dataset"},
                   "BimRoboticsTwin": {"robotics_twin", "robotics_sensors"},
                   **{name: {module} for name, module in standalone_tests.items()}}
    if physics:
        executables["BimRoboticsCollision"] = {"robotics_assets", "robotics_collision", "robotics_diagnostics"}
        executables["BimRoboticsSim"] = {"robotics_assets", "robotics_sensors", "robotics_simulation"}
    if urdf:
        executables["BimRoboticsUrdf"] = {"urdf", "urdf_assets"}
    for name, modules in executables.items():
        if name not in targets:
            raise ValueError(f"The configured target graph is missing executable {name}.")
        paths = paths_from(name, graph)
        missing = sorted(PREFIX + module for module in modules if PREFIX + module not in paths)
        if missing:
            raise ValueError(f"{name} is missing its module dependencies {missing}.")
        for editor in sorted(EDITOR.intersection(paths)):
            raise ValueError(f"Headless executable has an editor dependency: {' -> '.join(paths[editor])}.")
        if name in standalone_tests:
            module = standalone_tests[name]
            allowed = {PREFIX + role for role in expected[module] | {module}}
            for dep, path in paths.items():
                if dep.startswith(PREFIX) and dep not in allowed:
                    raise ValueError(f"Standalone test has a broad dependency: {' -> '.join(path)}.")
        for linked in (name, *paths):
            for fragment in targets[linked].get("link", {}).get("commandFragments", []):
                if fragment["role"] == "libraries" and SDK_LIBRARY.search(fragment["fragment"]):
                    raise ValueError(f"{name} reaches a ROS or Isaac runtime via {linked}: {fragment['fragment']}.")
        print(f"{name}: links {', '.join(sorted(modules))} outside the editor dependency graph")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--physics", choices=("0", "1"), required=True)
    parser.add_argument("--urdf", choices=("0", "1"), required=True)
    args = parser.parse_args()
    try:
        check(args.build, args.config, args.physics == "1", args.urdf == "1")
    except (KeyError, OSError, ValueError) as error:
        parser.exit(1, f"Robotics module boundary check failed: {error}\n")


if __name__ == "__main__":
    main()
