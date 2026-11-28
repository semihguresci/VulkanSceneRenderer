"""Build a small BIM and robot replay bundle for an external Isaac smoke test."""

import math
from pathlib import Path
from shutil import copyfile
import sys
from tempfile import TemporaryDirectory

from robotics_usd_roundtrip import FIXTURE, SOURCE_NAMESPACE, export_corner, require

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from benchmark_bim_import import file_identity
from bim_robotics_replay_usd import write_replay_stage
from robotics_preview_dataset import read_object, run_stage, write_json


SCENARIO = FIXTURE.with_name("lidar-corner-isaac-smoke.json")
TICKS_PER_SECOND = 60
WITNESS_TICKS = (0, 30, 60, 90)
PROBE_ORIGIN = (0, 0, 1)
PROBE_RANGE = 8
# Source STEP ID, direction in REP-103 coordinates, and analytic range in metres.
PROBES = (
    (27, (1, 0, 0), 4),
    (37, (0, 1, 0), 3),
    (17, (0, 0, -1), 1),
    (None, (-1, 0, 0), None),
)


def identity(path):
    result = file_identity(path)
    return {"sha256": result["sha256"], "bytes": result["bytes"]}


def motion_witness(report, source):
    scene_source = report.get("sceneSource", {})
    history = report.get("history", [])
    require(report.get("schema") == "container.bim-robotics-simulation" and report.get("version") == 1
            and report.get("sourceNamespace") == SOURCE_NAMESPACE
            and report.get("coordinateConvention") == "ROS REP-103"
            and report.get("ticksPerSecond") == TICKS_PER_SECOND
            and [row.get("tick") for row in history] == list(range(WITNESS_TICKS[-1] + 1))
            and report.get("state") == history[-1], "Native simulation clock or committed history differs")
    require(scene_source.get("layers") == [{"sha256": source["sha256"], "byteSize": source["bytes"]}]
            and math.isclose(scene_source.get("metersPerUnscaledRendererUnit", 0), 1, rel_tol=0, abs_tol=2e-5),
            "Native simulation did not use the published IFC bytes with the expected metre scale")
    require(all(row.get("obstruction") == "none" for row in history),
            "Native corner trajectory was obstructed")
    poses = [history[tick]["base"] for tick in WITNESS_TICKS]
    # At 60 Hz the scripted 0.2 m/s for 30 ticks gives 0.1 m. The
    # following 0.7 rad/s turn covers 0.35 rad at tick 60 and 0.525 rad before stopping.
    expected = ((0, 0), (.1, 0), (.1, .35), (.1, .525))
    for tick, pose, (x, heading) in zip(WITNESS_TICKS, poses, expected):
        require(math.dist(pose["position"], (x, 0, 0)) <= 2e-5
                and abs(math.remainder(pose["yaw"] - heading, math.tau)) <= 2e-5,
                f"Native tick {tick} expected ({x}, 0, 0) m at {heading} rad, got {pose}")
    start, translated, turned, _ = poses
    forward = translated["position"][0] - start["position"][0]
    yaw = math.remainder(turned["yaw"] - translated["yaw"], math.tau)
    return {"translatedMetres": forward, "turnedRadians": yaw}


def collision_witness(report, queries):
    rows = report.get("raycasts", [])
    require(report.get("schema") == "container.bim-collision-report" and report.get("version") == 1
            and report.get("coordinates") == "ROS REP-103, metres"
            and report.get("sourceNamespace") == SOURCE_NAMESPACE
            and len(rows) == len(PROBES), "Native collision report differs from the metric fixture")
    for row, query, (source_id, direction, distance) in zip(rows, queries, PROBES):
        require(row.get("from") == query["from"] and row.get("to") == query["to"],
                f"Native segment for {direction} differs from the requested probe")
        hit = row.get("hit")
        if source_id is None:
            require(hit is None, f"Native clear ray {direction} hit {hit}")
            continue
        guid = f"{source_id:022d}"
        entity_id = f"bim/{len(SOURCE_NAMESPACE)}:{SOURCE_NAMESPACE}/guid/{len(guid)}:{guid}"
        point = [value + distance * component for value, component in zip(PROBE_ORIGIN, direction)]
        require(hit and hit.get("entityId") == entity_id
                and math.isclose(hit["fraction"] * PROBE_RANGE, distance, rel_tol=0, abs_tol=2e-5)
                and math.dist(hit["position"], point) <= 2e-5
                and math.dist(hit["normal"], [-component for component in direction]) <= 2e-5
                and hit.get("walkableSupport") is (source_id == 17),
                f"Native ray {direction} expected STEP #{source_id} at {distance} m, got {hit}")


def prepare_bundle(exporter, simulator, collision, output: Path) -> dict:
    """Prepare the reference bundle in an existing empty directory using native tools only."""
    output = Path(output).resolve(strict=True)
    require(output.is_dir() and not any(output.iterdir()), f"Expected an empty bundle directory: {output}")
    executables = {role: Path(tool).resolve(strict=True) for role, tool in
                   (("exporter", exporter), ("simulator", simulator), ("collision", collision))}
    native_tools = {role: {"name": tool.name, **identity(tool)} for role, tool in executables.items()}
    source = output / FIXTURE.name
    scene = output / "scene.usda"
    with TemporaryDirectory(prefix=".source-", dir=output) as staging:
        staging = Path(staging)
        _, scaled_scene, _ = export_corner(executables["exporter"], staging)
        copyfile(staging / FIXTURE.name, source)
        copyfile(scaled_scene, scene)
    copyfile(SCENARIO, output / "scenario.json")
    source_identity = identity(source)
    queries = [
        {"from": list(PROBE_ORIGIN),
         "to": [value + PROBE_RANGE * component for value, component in zip(PROBE_ORIGIN, direction)]}
        for _, direction, _ in PROBES
    ]
    write_json(output / "queries.json", {"version": 1, "raycasts": queries})
    arguments = {
        "exporter": [source.name, scene.name, SOURCE_NAMESPACE, "--import-scale", "2.5"],
        "simulator": [source.name, "simulation.json", SOURCE_NAMESPACE, "scenario.json"],
        "collision": [source.name, "collision.json", SOURCE_NAMESPACE, "queries.json"],
    }
    for role in ("simulator", "collision"):
        source_name, report_name, namespace, spec_name = arguments[role]
        run_stage(role, executables[role], [output / source_name, output / report_name, namespace, output / spec_name],
                  output, timeout=120)

    report = read_object(output / "simulation.json")
    motion = motion_witness(report, source_identity)
    collision_witness(read_object(output / "collision.json"), queries)
    write_replay_stage(output / "replay.usda", scene.name, report)
    manifest = {
        "schema": "container.bim-robotics-isaac-smoke",
        "version": 1,
        "sourceNamespace": SOURCE_NAMESPACE,
        "witnessTicks": list(WITNESS_TICKS),
        "ticksPerSecond": TICKS_PER_SECOND,
        "nativeTools": native_tools,
        "commands": {role: [executables[role].name, *args] for role, args in arguments.items()},
        "provenance": {
            "authoredFixture": {"name": FIXTURE.name, **identity(FIXTURE)},
            "authoredScenario": {"name": SCENARIO.name, **identity(SCENARIO)},
            "rendererImportScale": 2.5,
        },
        "motion": motion,
        "files": {path.name: identity(path) for path in sorted(output.iterdir())},
    }
    write_json(output / "bundle.json", manifest)
    return manifest
