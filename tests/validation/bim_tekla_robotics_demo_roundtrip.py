#!/usr/bin/env python3
"""Verify a completed Tekla robotics demo package with OpenUSD."""

import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import sys


NAMESPACE = "tekla-robotics-demo"
SOURCE_SHA256 = "1e0e087b7b5a9a8aaaccb2896ad9317c5a1cf1a86da30236b04a3ead8d9eac71"
COLUMN_SOURCE_ID = "#14449"
COLUMN_GUID = "1GPKJW000_dp4pD3WuCZ8u"
COLUMN_ENTITY_ID = f"bim/19:{NAMESPACE}/guid/22:{COLUMN_GUID}"
LAST_TICK = 570
TICK_RATE = 60
POSE_TOLERANCE = 1e-9


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def read_json(path):
    return json.loads(path.read_bytes())


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def open_demo_stage(directory, summary):
    from pxr import Usd, UsdGeom, UsdUtils

    require(summary["entryStage"] == "replay.usda" and summary["scene"] == "scene.usda",
            "Expected replay.usda with a sibling scene.usda")
    replay = directory / "replay.usda"
    scene = directory / "scene.usda"
    for path in (replay, scene):
        require(sha256(path) == summary["stageSha256"][path.name], f"{path.name} differs from the demo manifest")

    stage = Usd.Stage.Open(str(replay))
    require(stage and stage.GetRootLayer().subLayerPaths == [scene.name],
            "The replay stage must compose scene.usda as its relative sublayer")
    layers, assets, unresolved = UsdUtils.ComputeAllDependencies(str(replay))
    layer_by_path = {Path(layer.realPath).resolve(): layer for layer in layers}
    require(layer_by_path.keys() == {replay.resolve(), scene.resolve()} and not assets and not unresolved,
            f"Unexpected OpenUSD dependencies: layers={list(layer_by_path)}, assets={assets}, unresolved={unresolved}")
    scene_layer = layer_by_path[scene.resolve()]
    require(scene_layer.defaultPrim == "World" and scene_layer.pseudoRoot.GetInfo("metersPerUnit") == 1.0 and
            scene_layer.pseudoRoot.GetInfo("upAxis") == "Z", "The source BIM layer must use a metre-based Z-up World")
    world = stage.GetDefaultPrim()
    require(world and world.IsA(UsdGeom.Xform) and str(world.GetPath()) == "/World" and
            world.GetAttribute("robotics:sourceNamespace").Get() == NAMESPACE,
            "The composed default prim lost the Tekla scene identity")
    require(UsdGeom.GetStageMetersPerUnit(stage) == 1.0 and UsdGeom.GetStageUpAxis(stage) == "Z",
            "The replay stage must use metres and Z-up")
    require(stage.GetStartTimeCode() == 0 and stage.GetEndTimeCode() == LAST_TICK and
            stage.GetTimeCodesPerSecond() == stage.GetFramesPerSecond() == TICK_RATE,
            "OpenUSD timing differs from the native fixed-step clock")
    return stage


def check_vector(actual, expected, label):
    require(len(actual) == len(expected) and all(
        math.isclose(a, b, rel_tol=0, abs_tol=POSE_TOLERANCE) for a, b in zip(actual, expected)
    ), f"{label}: expected {expected}, got {actual}")


def check_robot(stage, report):
    from pxr import Gf, Usd, UsdGeom, UsdPhysics

    expected_ticks = list(range(LAST_TICK + 1))
    require(report["schema"] == "container.bim-robotics-simulation" and report["version"] == 1 and
            report["sourceNamespace"] == NAMESPACE and report["ticksPerSecond"] == TICK_RATE and
            report["sceneSource"]["layers"][0]["sha256"] == SOURCE_SHA256 and
            [row["tick"] for row in report["history"]] == expected_ticks,
            "Expected the pinned native simulation with all 571 committed ticks")

    robot = stage.GetPrimAtPath("/World/ReplayRobot")
    require(robot and robot.IsA(UsdGeom.Xform) and
            robot.GetAttribute("robotics:role").Get() == "recorded-trajectory-visual-proxy" and
            robot.GetAttribute("robotics:robotId").Get() == report["robotId"] and
            not robot.HasAPI(UsdPhysics.ArticulationRootAPI) and not robot.HasAPI(UsdPhysics.RigidBodyAPI),
            "The replay stage lost the visual robot proxy or gave it a dynamics schema")
    for name in ("xformOp:translate", "xformOp:rotateZ"):
        require(robot.GetAttribute(name).GetTimeSamples() == expected_ticks,
                f"Expected one authored {name} sample per committed tick")

    cache = UsdGeom.XformCache()
    initial = report["history"][0]["base"]["position"]
    check_vector(cache.GetLocalToWorldTransform(robot).ExtractTranslation(), initial, "Default robot pose")
    for row in report["history"]:
        tick, base = row["tick"], row["base"]
        cache.SetTime(Usd.TimeCode(tick))
        transform = cache.GetLocalToWorldTransform(robot)
        check_vector(transform.ExtractTranslation(), base["position"], f"Translation at tick {tick}")
        yaw = base["yaw"]
        check_vector(transform.TransformDir(Gf.Vec3d(1, 0, 0)), (math.cos(yaw), math.sin(yaw), 0),
                     f"Heading at tick {tick}")
        check_vector(transform.TransformDir(Gf.Vec3d(0, 0, 1)), (0, 0, 1), f"Up axis at tick {tick}")


def check_column(stage):
    from pxr import UsdGeom, UsdPhysics

    products = [prim for prim in stage.Traverse() if prim.GetAttribute("ifc:sourceId").Get() == COLUMN_SOURCE_ID]
    require(len(products) == 1, f"Expected exactly one IFC product for {COLUMN_SOURCE_ID}")
    column = products[0]
    kind = column.GetAttribute("robotics:kind").Get()
    labels = column.GetAttribute("semantics:labels:class").Get()
    schemas = column.GetMetadata("apiSchemas")
    require(column.IsA(UsdGeom.Xform) and column.GetAttribute("ifc:guid").Get() == COLUMN_GUID and
            column.GetAttribute("ifc:class").Get() == "IFCCOLUMN" and
            column.GetAttribute("robotics:entityId").Get() == COLUMN_ENTITY_ID and
            kind and labels is not None and list(labels) == [kind] and
            schemas and "SemanticsLabelsAPI:class" in schemas.GetAppliedItems(),
            f"{COLUMN_SOURCE_ID} lost its authored GUID, IFC class, or robotics semantic label")

    physics_scene = stage.GetPrimAtPath("/World/PhysicsScene")
    require(physics_scene.IsA(UsdPhysics.Scene), "The BIM stage lost its physics scene")
    colliders = stage.GetPrimAtPath("/World/Colliders")
    owned = [prim for prim in colliders.GetChildren()
             if prim.GetRelationship("robotics:entity").GetTargets() == [column.GetPath()]]
    require(len(owned) == 1, f"Expected one collision proxy owned by {COLUMN_SOURCE_ID}")
    proxy = owned[0]
    mesh = UsdGeom.Mesh(proxy)
    require(mesh and proxy.HasAPI(UsdPhysics.CollisionAPI) and
            UsdPhysics.CollisionAPI(proxy).GetCollisionEnabledAttr().Get() is True and
            UsdPhysics.CollisionAPI(proxy).GetSimulationOwnerRel().GetTargets() == [physics_scene.GetPath()] and
            proxy.HasAPI(UsdPhysics.MeshCollisionAPI) and
            UsdPhysics.MeshCollisionAPI(proxy).GetApproximationAttr().Get() == "none" and
            proxy.GetAttribute("robotics:entityId").Get() == COLUMN_ENTITY_ID and
            proxy.GetAttribute("robotics:representationId").Get() == COLUMN_SOURCE_ID,
            f"The collision proxy for {COLUMN_SOURCE_ID} lost its native identity or physics binding")
    triangles = mesh.GetFaceVertexCountsAttr().Get()
    indices = mesh.GetFaceVertexIndicesAttr().Get()
    points = mesh.GetPointsAttr().Get()
    require(triangles and indices and points and len(indices) == 3 * len(triangles) and
            all(count == 3 for count in triangles) and all(0 <= index < len(points) for index in indices),
            f"The collision proxy for {COLUMN_SOURCE_ID} lost its triangle geometry")


def check_dataset_label(directory, summary):
    path = directory / "dataset" / "labels.json"
    if summary.get("cameras") is None and not path.exists():
        return
    labels = read_json(path)
    column = [row for row in labels["instances"] if row["occurrenceId"] == COLUMN_SOURCE_ID]
    classes = {row["semanticId"]: row["ifcClass"] for row in labels["classes"]}
    require(labels["sourceNamespace"] == NAMESPACE and len(column) == 1 and
            column[0]["entityId"] == COLUMN_ENTITY_ID and column[0]["guid"] == COLUMN_GUID and
            classes.get(column[0]["semanticId"]) == "IFCCOLUMN",
            f"The sensor label table lost the IFC column identity for {COLUMN_SOURCE_ID}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--demo", type=Path, required=True, help="Completed Tekla robotics demo directory")
    args = parser.parse_args()
    if importlib.util.find_spec("pxr") is None:
        print("SKIP: OpenUSD (pxr) is unavailable in this Python environment", file=sys.stderr)
        return 77
    try:
        directory = args.demo.resolve(strict=True)
        summary = read_json(directory / "summary.json")
        require(summary["schema"] == "container.tekla-robotics-demo" and summary["version"] == 1 and
                summary["sourceNamespace"] == NAMESPACE and summary["source"]["sha256"] == SOURCE_SHA256 and
                summary["tickCount"] == LAST_TICK + 1 and summary["simulation"] == "simulation.json",
                "Expected a completed version 1 demo for the pinned Tekla House IFC")
        stage = open_demo_stage(directory, summary)
        check_robot(stage, read_json(directory / summary["simulation"]))
        check_column(stage)
        check_dataset_label(directory, summary)
        print(f"Verified the portable Tekla scene, {LAST_TICK + 1} robot poses, and column {COLUMN_SOURCE_ID}.")
        return 0
    except Exception as error:
        print(f"Tekla robotics OpenUSD round trip failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
