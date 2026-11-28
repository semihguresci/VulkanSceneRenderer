"""Compare the portable IFC corner replay and its native CPU witnesses."""

import hashlib
import json
import math
from pathlib import Path
import re

from robotics_usd_roundtrip import PRODUCTS, SOURCE_NAMESPACE, TOLERANCE, require, validate_stage


BUNDLE_SCHEMA = "container.bim-robotics-isaac-smoke"
PAYLOADS = {
    "lidar-corner.ifc", "scene.usda", "replay.usda", "simulation.json", "scenario.json", "simulator.log",
    "queries.json", "collision.json", "collision.log",
}
TICK_RATE = 60
WITNESS_TICKS = [0, 30, 60, 90]
ROBOT_ID = "isaac-corner-smoke"
SEED = 221
COMMANDS = ((0, .2, 0), (30, 0, .7), (75, 0, 0))
POSE_TOLERANCE = 1e-9
RAY_ORIGIN = (0, 0, 1)
RAY_LENGTH = 8.
# Independent metric witnesses from the IFC corner, in the order of the query file.
RAYS = (
    ("wall-x", (1, 0, 0), 27, 4.),
    ("wall-y", (0, 1, 0), 37, 3.),
    ("floor", (0, 0, -1), 17, 1.),
    ("clear", (-1, 0, 0), None, None),
)


def finite_number(value, label):
    require(type(value) in (int, float), f"{label} must be a finite number")
    try:
        number = float(value)
    except OverflowError:
        raise RuntimeError(f"{label} must be a finite number") from None
    require(math.isfinite(number), f"{label} must be a finite number")
    return number


def vector(value, label):
    require(isinstance(value, list) and len(value) == 3, f"{label} must be a three-component vector")
    return [finite_number(coordinate, label) for coordinate in value]


def check_vector(actual, expected, label, tolerance=TOLERANCE):
    require(actual is not None and len(actual) == len(expected) and all(
        math.isclose(a, b, abs_tol=tolerance, rel_tol=0) for a, b in zip(actual, expected)
    ), f"{label}: expected {expected}, got {actual}")


def file_in(directory, name):
    path = directory / name
    require(path.is_file(), f"Missing bundle file {name}")
    require(path.resolve(strict=True) == path, f"Bundle file {name} must be a local file without indirection")
    return path


def read_json(path):
    try:
        data = json.loads(path.read_bytes())
    except (UnicodeError, ValueError) as error:
        raise RuntimeError(f"{path.name}: invalid JSON: {error}") from error
    require(isinstance(data, dict), f"{path.name} must contain a JSON object")
    return data


def entity_id(source_id):
    return f"bim/{len(SOURCE_NAMESPACE)}:{SOURCE_NAMESPACE}/guid/22:{source_id:022d}"


def load_rays(queries, collision):
    require(collision.get("schema") == "container.bim-collision-report" and type(collision.get("version")) is int
            and collision["version"] == 1 and collision.get("sourceNamespace") == SOURCE_NAMESPACE
            and collision.get("coordinates") == "ROS REP-103, metres", "Invalid native collision report metadata")
    requests, results = queries.get("raycasts"), collision.get("raycasts")
    require(isinstance(requests, list) and isinstance(results, list) and len(requests) == len(results) == len(RAYS),
            "Expected four ordered native collision rays")
    rays = []
    for (name, direction, source_id, distance), request, result in zip(RAYS, requests, results):
        end = [origin + RAY_LENGTH * axis for origin, axis in zip(RAY_ORIGIN, direction)]
        for label, row in (("query", request), ("result", result)):
            require(isinstance(row, dict), f"{name} ray {label} must be a JSON object")
            check_vector(vector(row.get("from"), f"{name} ray {label} start"), RAY_ORIGIN, f"{name} ray {label} start")
            check_vector(vector(row.get("to"), f"{name} ray {label} end"), end, f"{name} ray {label} end")

        require("hit" in result, f"{name} native ray is missing its hit field")
        hit = result["hit"]
        measured = None
        if source_id is None:
            require(hit is None, f"{name} native ray should miss, got {hit}")
        else:
            require(isinstance(hit, dict) and hit.get("entityId") == entity_id(source_id),
                    f"{name} native ray did not hit the fixture entity #{source_id}")
            measured = finite_number(hit.get("fraction"), f"{name} hit fraction") * RAY_LENGTH
            check_vector((measured,), (distance,), f"{name} native hit distance in metres")
            point = [origin + distance * axis for origin, axis in zip(RAY_ORIGIN, direction)]
            check_vector(vector(hit.get("position"), f"{name} native hit position"), point, f"{name} native hit position")
            normal = [-axis for axis in direction]
            check_vector(vector(hit.get("normal"), f"{name} native hit normal"), normal, f"{name} native hit normal")
            require(hit.get("walkableSupport") is (source_id == 17), f"{name} native hit has the wrong support flag")
        rays.append({"name": name, "originMeters": list(RAY_ORIGIN), "direction": list(direction),
                     "maxDistanceMeters": RAY_LENGTH, "sourceId": source_id, "distanceMeters": measured})
    return rays


def load_samples(simulation, scenario, source):
    require(type(scenario.get("version")) is int and scenario["version"] == 1
            and type(scenario.get("ticksPerSecond")) is int and scenario["ticksPerSecond"] == TICK_RATE
            and type(scenario.get("ticks")) is int and scenario["ticks"] == WITNESS_TICKS[-1]
            and type(scenario.get("seed")) is int and scenario["seed"] == SEED and scenario.get("robotId") == ROBOT_ID,
            "Invalid fixed-step corner scenario metadata")
    check_vector(vector(scenario.get("initialFeet"), "Scenario initial position"), (0., 0., 0.),
                 "Scenario initial position", 0.)
    require(finite_number(scenario.get("initialYaw"), "Scenario initial yaw") == 0., "Invalid scenario initial yaw")
    commands = scenario.get("commands")
    require(isinstance(commands, list) and len(commands) == len(COMMANDS), "Invalid corner scenario command schedule")
    for command, (tick, linear, angular) in zip(commands, COMMANDS):
        require(isinstance(command, dict) and type(command.get("tick")) is int and command["tick"] == tick,
                f"Invalid corner scenario command at tick {tick}")
        check_vector((finite_number(command.get("linear"), f"Linear command at tick {tick}"),
                      finite_number(command.get("angular"), f"Angular command at tick {tick}")),
                     (linear, angular), f"Corner scenario command at tick {tick}", 0.)

    require(simulation.get("schema") == "container.bim-robotics-simulation"
            and type(simulation.get("version")) is int and simulation["version"] == 1
            and simulation.get("sourceNamespace") == SOURCE_NAMESPACE
            and simulation.get("coordinateConvention") == "ROS REP-103"
            and simulation.get("robotId") == ROBOT_ID and type(simulation.get("seed")) is int
            and simulation["seed"] == SEED, "Invalid native simulation metadata")
    require(type(simulation.get("ticksPerSecond")) is int and simulation["ticksPerSecond"] == TICK_RATE,
            "Unexpected native simulation tick rate")
    scene_source = simulation.get("sceneSource")
    require(isinstance(scene_source, dict) and scene_source.get("layers") == [
        {"byteSize": source["bytes"], "sha256": source["sha256"]}
    ], "Native simulation source fingerprint differs from lidar-corner.ifc")
    source_scale = finite_number(scene_source.get("metersPerUnscaledRendererUnit"), "Native source metre scale")
    check_vector((source_scale,), (1.,), "Native source metre scale")

    history = simulation.get("history")
    require(isinstance(history, list) and len(history) == WITNESS_TICKS[-1] + 1,
            "Native simulation must contain all 91 committed pose ticks")
    samples = []
    for tick, row in enumerate(history):
        require(isinstance(row, dict) and type(row.get("tick")) is int and row["tick"] == tick,
                f"Native simulation is missing committed pose tick {tick}")
        seconds = finite_number(row.get("seconds"), f"Native seconds at tick {tick}")
        check_vector((seconds,), (tick / TICK_RATE,), f"Native clock at tick {tick}", POSE_TOLERANCE)
        require(row.get("obstruction") == "none" and row.get("blockingEntityId") == "" and "contact" not in row,
                f"Unexpected obstruction or contact at native pose tick {tick}")
        base = row.get("base")
        require(isinstance(base, dict), f"Native simulation has no base pose at tick {tick}")
        position = vector(base.get("position"), f"Native position at tick {tick}")
        yaw = finite_number(base.get("yaw"), f"Native yaw at tick {tick}")
        distance = min(tick, 30) * .2 / TICK_RATE
        angle = max(0, min(tick, 75) - 30) * .7 / TICK_RATE
        check_vector(position, (distance, 0., 0.), f"Native position at tick {tick}")
        check_vector((yaw,), (angle,), f"Native yaw at tick {tick}")
        samples.append({"tick": tick, "positionMeters": position, "yawRadians": yaw})
    require(simulation.get("state") == history[-1], "Native simulation final state differs from the final committed tick")
    return samples


def has_file_fingerprint(entry):
    return (isinstance(entry, dict) and type(entry.get("bytes")) is int and entry["bytes"] >= 0
            and isinstance(entry.get("sha256"), str) and re.fullmatch(r"[0-9a-f]{64}", entry["sha256"]) is not None)


def load_bundle(directory):
    """Read a versioned fixture bundle and validate native reports without importing an SDK."""
    directory = Path(directory).resolve(strict=True)
    require(directory.is_dir(), f"Expected a comparison bundle directory: {directory}")
    manifest = read_json(file_in(directory, "bundle.json"))
    require(manifest.get("schema") == BUNDLE_SCHEMA and type(manifest.get("version")) is int
            and manifest["version"] == 1 and manifest.get("sourceNamespace") == SOURCE_NAMESPACE,
            "Invalid version 1 Isaac comparison bundle metadata")
    witnesses = manifest.get("witnessTicks")
    require(isinstance(witnesses, list) and all(type(tick) is int for tick in witnesses)
            and witnesses == WITNESS_TICKS and type(manifest.get("ticksPerSecond")) is int
            and manifest["ticksPerSecond"] == TICK_RATE, "Bundle witness ticks or tick rate are incorrect")

    files = manifest.get("files")
    require(isinstance(files, dict) and files.keys() == PAYLOADS, "Bundle manifest has missing or unexpected payload paths")
    paths = {}
    for name, entry in sorted(files.items()):
        require(has_file_fingerprint(entry), f"Invalid manifest byte count or SHA-256 for {name}")
        path = file_in(directory, name)
        require(path.stat().st_size == entry["bytes"], f"Bundle file {name} has a different byte count")
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        require(digest.hexdigest() == entry["sha256"], f"Bundle file {name} SHA-256 differs from the manifest")
        paths[name] = path

    provenance = manifest.get("provenance")
    require(isinstance(provenance, dict) and provenance.get("rendererImportScale") == 2.5,
            "Expected native source provenance and renderer import scale 2.5")
    # The archived IFC also carries the fixture generator's material and placement edits.
    authored_fixture = provenance.get("authoredFixture")
    require(has_file_fingerprint(authored_fixture) and authored_fixture.get("name") == "lidar-corner.ifc",
            "Bundle is missing the original IFC fingerprint")
    authored_scenario = {"name": "lidar-corner-isaac-smoke.json", **files["scenario.json"]}
    require(provenance.get("authoredScenario") == authored_scenario, "Archived corner scenario fingerprint disagrees with provenance")
    samples = load_samples(read_json(paths["simulation.json"]), read_json(paths["scenario.json"]), files["lidar-corner.ifc"])
    motion = manifest.get("motion")
    require(isinstance(motion, dict), "Bundle manifest is missing its motion witnesses")
    travelled = math.dist(samples[0]["positionMeters"], samples[30]["positionMeters"])
    turned = samples[60]["yawRadians"] - samples[30]["yawRadians"]
    check_vector((finite_number(motion.get("translatedMetres"), "Manifest travel"),
                  finite_number(motion.get("turnedRadians"), "Manifest turn")),
                 (travelled, turned), "Bundle motion witnesses differ from native simulation", POSE_TOLERANCE)
    rays = load_rays(read_json(paths["queries.json"]), read_json(paths["collision.json"]))
    return {"stage": paths["replay.usda"], "scene": paths["scene.usda"], "ticksPerSecond": TICK_RATE,
            "witnessTicks": witnesses, "robotId": ROBOT_ID, "samples": samples, "rays": rays}


def verify_dependencies(bundle):
    """Check authored layers and asset paths using OpenUSD before opening the composition."""
    from pxr import Sdf, UsdUtils

    for key, expected_layers in (("stage", [bundle["scene"].name]), ("scene", [])):
        path = bundle[key]
        try:
            layer = Sdf.Layer.FindOrOpen(str(path))
            require(layer, f"Could not read {path.name}")
            require(list(layer.subLayerPaths) == expected_layers,
                    f"{path.name} sublayers must be exactly {expected_layers}, got {list(layer.subLayerPaths)}")
            sublayers, references, payloads = UsdUtils.ExtractExternalReferences(str(path))
        except Exception as error:
            raise RuntimeError(f"{path.name} layer validation failed: {error}") from error
        require(sublayers == expected_layers and not references and not payloads,
                f"{path.name} has unexpected external dependencies: {sublayers}, {references}, {payloads}")
        require(layer.defaultPrim == "World" and layer.pseudoRoot.GetInfo("metersPerUnit") == 1.
                and layer.pseudoRoot.GetInfo("upAxis") == "Z", f"{path.name} must define a metre-based Z-up World")


def verify_robot(stage, bundle):
    from pxr import Gf, Usd, UsdGeom, UsdPhysics

    samples = bundle["samples"]
    ticks = [sample["tick"] for sample in samples]
    require(stage.HasAuthoredTimeCodeRange() and stage.GetStartTimeCode() == ticks[0]
            and stage.GetEndTimeCode() == ticks[-1]
            and stage.GetTimeCodesPerSecond() == stage.GetFramesPerSecond() == bundle["ticksPerSecond"],
            "Composed replay timeline differs from the native fixed-step clock")
    robot = stage.GetPrimAtPath("/World/ReplayRobot")
    require(robot and robot.IsA(UsdGeom.Xform) and robot.GetAttribute("robotics:robotId").Get() == bundle["robotId"]
            and robot.GetAttribute("robotics:role").Get() == "recorded-trajectory-visual-proxy"
            and not robot.HasAPI(UsdPhysics.ArticulationRootAPI) and not robot.HasAPI(UsdPhysics.RigidBodyAPI),
            "Missing or incorrect replay robot proxy at /World/ReplayRobot")
    for name in ("Chassis", "FrontMarker"):
        require(robot.GetChild(name).IsA(UsdGeom.Cube), f"Missing replay robot visual {name}")
    for attribute_name in ("xformOp:translate", "xformOp:rotateZ"):
        require(robot.GetAttribute(attribute_name).GetTimeSamples() == ticks,
                f"Expected exactly one {attribute_name} sample at every committed tick")

    cache = UsdGeom.XformCache()
    rotations = robot.GetAttribute("xformOp:rotateZ")
    previous_rotation = None
    for time, sample in [(Usd.TimeCode.Default(), samples[0]), *[(Usd.TimeCode(i), samples[i]) for i in ticks]]:
        tick = sample["tick"]
        label = "at default time" if time.IsDefault() else f"at tick {tick}"
        cache.SetTime(time)
        transform = cache.GetLocalToWorldTransform(robot)
        yaw = sample["yawRadians"]
        check_vector(transform.ExtractTranslation(), sample["positionMeters"], f"Replay robot translation {label}", POSE_TOLERANCE)
        check_vector(transform.TransformDir(Gf.Vec3d(1, 0, 0)), (math.cos(yaw), math.sin(yaw), 0),
                     f"Replay robot heading {label}", POSE_TOLERANCE)
        check_vector(transform.TransformDir(Gf.Vec3d(0, 1, 0)), (-math.sin(yaw), math.cos(yaw), 0),
                     f"Replay robot left axis {label}", POSE_TOLERANCE)
        check_vector(transform.TransformDir(Gf.Vec3d(0, 0, 1)), (0, 0, 1), f"Replay robot up axis {label}", POSE_TOLERANCE)
        rotation = finite_number(rotations.Get(time), f"Replay robot rotateZ {label}")
        if previous_rotation is not None:
            require(abs(rotation - previous_rotation) <= 180., f"Replay robot heading makes an extra turn {label}")
        previous_rotation = rotation


def verify_stage(stage, bundle):
    """Check the actual composed stage and return pose, semantics and collision evidence."""
    verify_dependencies(bundle)
    require(stage and Path(stage.GetRootLayer().realPath).resolve() == bundle["stage"],
            "OpenUSD did not open the bundle's exact replay stage")
    layers = {Path(layer.realPath).resolve() for layer in stage.GetUsedLayers() if layer.realPath}
    require(layers == {bundle["stage"], bundle["scene"]}, f"Replay stage has unexpected composed layer paths: {layers}")
    identity, collider_paths = validate_stage(stage)
    products = {}
    for source_id, (stable_id, path) in identity.items():
        guid = f"{source_id:022d}"
        require(stable_id == entity_id(source_id), f"#{source_id} lost its native IFC entity ID")
        prim = stage.GetPrimAtPath(path)
        products[source_id] = {"path": path, "guid": guid, "entityId": stable_id, "ifcClass": PRODUCTS[source_id][0],
                               "labels": list(prim.GetAttribute("semantics:labels:class").Get())}
    verify_robot(stage, bundle)
    return {"products": products, "colliders": {source_id: str(path) for source_id, path in collider_paths.items()},
            "samples": bundle["samples"], "ticksPerSecond": bundle["ticksPerSecond"],
            "witnessTicks": bundle["witnessTicks"], "rays": bundle["rays"]}
