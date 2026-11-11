"""Generate matched metre and millimetre BIM partitions for DDGI measurements."""

from __future__ import annotations

import hashlib
from pathlib import Path

from quality_fixtures import box, build_scene as build_architecture, gltf_scene, rectangle
from run_runtime import write_json


VARIANTS = ("open", "sheet", "sealed", "gap", "overlap", "mixed")
UNIT_SCALES = {"metre": 1.0, "millimetre": 0.001}
PARTITION_X = 0.12
WALL_THICKNESS = 0.03
GAP_WIDTH = 0.12
APERTURE_WIDTH = 0.8
OPENING_Z = -0.5


def wall(name, z_min, z_max, *, x=PARTITION_X, bottom=0.0, top=2.8):
    return box(name, [x - WALL_THICKNESS / 2, bottom, z_min],
               [x + WALL_THICKNESS / 2, top, z_max])


def bim_partitions(variant):
    if variant == "sheet":
        return {"partition-sheet": [rectangle("partition-sheet", [PARTITION_X, 1.4, -0.5],
                                               [0, 1.4, 0], [0, 0, 1.5], "partition")]}
    if variant == "sealed":
        return {"partition": wall("partition", -2.0, 1.0)}
    if variant == "overlap":
        # Separate products overlap in all three axes. Offset the second top
        # and bottom faces to avoid stacking coincident triangles at the seam.
        return {"partition-rear": wall("partition-rear", -2.0, -0.35),
                "partition-front": wall("partition-front", -0.65, 1.0,
                                        x=PARTITION_X + 0.01, bottom=-0.015, top=2.815)}
    if variant in ("gap", "mixed"):
        half_width = (GAP_WIDTH if variant == "gap" else APERTURE_WIDTH) / 2
        return {"partition-rear": wall("partition-rear", -2.0, OPENING_Z - half_width),
                "partition-front": wall("partition-front", OPENING_Z + half_width, 1.0)}
    return {}


def build_scene(variant, source_units, width=640, height=426):
    if variant not in VARIANTS or source_units not in UNIT_SCALES:
        raise ValueError(f"Unknown BIM leakage fixture {variant}/{source_units}")
    scene = build_architecture("open", width, height)
    # Keep the rear wall in IFCX even in the open control. Both providers are
    # always present, and the back and floor receivers exercise distinct paths.
    bim = {"rear-receiver-wall": [quad for quad in scene["quads"] if quad["name"] == "back"]}
    bim.update(bim_partitions(variant))
    for element, faces in bim.items():
        for quad in faces:
            quad.update(provider="bim", bimElement=element, doubleSided=element == "partition-sheet")
        if element != "rear-receiver-wall":
            scene["quads"].extend(faces)

    if variant == "mixed":
        pane = rectangle("glass-insert", [PARTITION_X, 1.4, OPENING_Z],
                         [0, 1.4, 0], [0, 0, APERTURE_WIDTH / 2], "partition")
        pane.update(rayGeometry="Glass insert", doubleSided=True)
        scene["quads"].append(pane)
        scene["partitionNode"] = 2
    for quad in scene["quads"]:
        quad.setdefault("provider", "gltf")
    for item in scene["receivers"]:
        item["provider"] = "bim" if item["name"].endswith("-back") else "gltf"

    corners = [point for faces in bim.values() for quad in faces for point in quad["corners"]]
    bounds = {"min": [min(point[axis] for point in corners) for axis in range(3)],
              "max": [max(point[axis] for point in corners) for axis in range(3)]}
    thickness = WALL_THICKNESS if variant in ("sealed", "gap", "overlap", "mixed") else 0.0
    feature = {"partitionCenterXMetres": None if variant == "open" else PARTITION_X,
               "wallThicknessMetres": thickness,
               "gapWidthMetres": GAP_WIDTH if variant == "gap" else 0.0,
               "apertureWidthMetres": APERTURE_WIDTH if variant == "mixed" else 0.0}
    if variant == "overlap":
        feature.update(overlapLengthMetres=0.30, overlapThicknessMetres=0.02)
    scene.update(name=f"bim-{variant}-{source_units}", variant=variant, sourceUnits=source_units,
                 description="Mixed IFCX and glTF cutaway rooms with a black environment",
                 bimImportScale=UNIT_SCALES[source_units], bimElements=list(bim), bimBounds=bounds,
                 partitionThickness=thickness, feature=feature,
                 materialPolicy={"bimPartitionDoubleSided": variant == "sheet",
                                 "bimAlphaMode": "OPAQUE", "gltfThinGlassInsert": variant == "mixed",
                                 "unoptedGlassTransport": "opaque terminal" if variant == "mixed" else None})
    policy = scene["referencePolicy"]
    policy["supported"] = variant not in ("sheet", "mixed")
    if variant not in ("sealed", "overlap"):
        policy.pop("sealedInvariant", None)
    if variant == "sheet":
        policy["unsupportedReason"] = "The reference excludes two-sided diffuse reflection"
    elif variant == "mixed":
        policy["unsupportedReason"] = "The reference excludes dielectric transmission"
    return scene


def ifcx_scene(scene):
    elements = []
    scale = scene["bimImportScale"]
    for element in scene["bimElements"]:
        quads = [quad for quad in scene["quads"] if quad.get("bimElement") == element]
        # IFCX source axes are X right, Y forward, Z up. The inverse of the
        # loader's right-handed conversion is (world x, -world z, world y).
        points = [[x / scale, -z / scale, y / scale] for quad in quads for x, y, z in quad["corners"]]
        elements.append({"path": element, "attributes": {
            "bsi::ifc::class": "IfcWall", "bsi::ifc::name": element,
            "bsi::ifc::globalId": f"bim-leakage-{element}",
            "gltf::material": {"doubleSided": quads[0]["doubleSided"], "alphaMode": "OPAQUE",
                               "pbrMetallicRoughness": {"baseColorFactor": quads[0]["albedo"] + [1],
                                                        "metallicFactor": 0, "roughnessFactor": 1}},
            "usd::usdgeom::mesh": {"points": points, "faceVertexCounts": [4] * len(quads),
                                   "faceVertexIndices": list(range(len(points)))}}})
    return {"data": elements}


def primary_gltf(scene):
    gltf = gltf_scene({**scene, "name": f"BIM leakage {scene['variant']}",
                       "quads": [quad for quad in scene["quads"] if quad["provider"] == "gltf"]})
    if scene["materialPolicy"]["gltfThinGlassInsert"]:
        material = next(item for item in gltf["materials"] if item["name"] == "partition")
        material.update(doubleSided=True, alphaMode="OPAQUE",
                        pbrMetallicRoughness={"baseColorFactor": [1, 1, 1, 1],
                                              "metallicFactor": 0, "roughnessFactor": 0.05},
                        extensions={"KHR_materials_transmission": {"transmissionFactor": 1},
                                    "KHR_materials_volume": {"thicknessFactor": 0}})
        gltf["extensionsUsed"] += ["KHR_materials_transmission", "KHR_materials_volume"]
    return gltf


def write_fixtures(output, width=640, height=426):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    fixtures = []
    for variant in VARIANTS:
        for source_units in UNIT_SCALES:
            scene = build_scene(variant, source_units, width, height)
            for key, extension, document in (("gltf", "gltf", primary_gltf(scene)),
                                              ("bim", "ifcx", ifcx_scene(scene))):
                path = output / f"{scene['name']}.{extension}"
                write_json(path, document)
                scene[key] = path.name
                scene[f"{key}Sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
            fixtures.append(scene)
    manifest = {"schemaVersion": 1, "fixtures": fixtures}
    write_json(output / "bim-leakage-fixture-manifest.json", manifest)
    return manifest
