"""Validate normal UV changes on independent bindings of shared glTF geometry."""

from __future__ import annotations

import argparse
import base64
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


WIDTH, HEIGHT = 640, 360
PLACEMENTS = (-1.2, 1.2)
GRAPH_NODES = (1, 3)
TECHNIQUES = ("forward-raster", "deferred-raster")
DEFAULT_COHORTS = ("generated", "authored", "missing")
COHORTS = (*DEFAULT_COHORTS, "morphed")
MORPH_DELTAS = ((0, 0.125, 0), (0, 0.125, 0), (0, 0.25, 0))
GPU_OPT_IN = "CONTAINER_RUN_GPU_GLTF_LIVE_NORMAL_UV"
CONTROLS = {"initial": (0, 0), "edited": (1, 0), "cleared": (None, 0), "assigned": (1, 1)}
# The tuple after each visual control is the exact pair of source materials.
STEPS = (
    (13, "initial", (0, 0), (0, 0)),
    (19, "initial", (2, 0), (1, 0)),
    (25, "edited", (2, 0), (2, 0)),
    (31, "cleared", (2, 0), (3, 0)),
    (37, "edited", (2, 0), (2, 1)),
    (43, "initial", (2, 0), (1, 2)),
    (49, "edited", (2, 0), (2, 1)),
    (55, "assigned", (2, 1), (3, 0)),
    (61, "edited", (2, 0), (2, 1)),
)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def write_fixture(path, mode, *, reference=None, morphed_reference=False):
    from gltf_accessor_runtime_regression import Asset
    from gltf_tangent_uv_runtime_regression import (
        INDICES, POSITIONS, TANGENT, UV0, UV1, WRONG_UV0_TANGENT, normal_png,
    )

    asset = Asset()
    positions = POSITIONS
    if morphed_reference:
        positions = [tuple(p + d for p, d in zip(position, delta, strict=True))
                     for position, delta in zip(positions, MORPH_DELTAS, strict=True)]
    common = dict(POSITION=asset.accessor("VEC3", positions),
                  NORMAL=asset.accessor("VEC3", [(0, 0, 1)] * 3),
                  TEXCOORD_0=asset.accessor("VEC2", UV0))
    if mode != "missing":
        common["TEXCOORD_1"] = asset.accessor("VEC2", UV1)
    indices = asset.accessor("SCALAR", INDICES, 5123, indices=True)
    asset.model["textures"] = [dict(source=0, sampler=0)]
    asset.model["samplers"] = [dict(magFilter=9729, minFilter=9729, wrapS=33071, wrapT=33071)]
    asset.model["images"] = [dict(uri="data:image/png;base64," + base64.b64encode(normal_png()).decode())]

    def add_material(uv):
        material = dict(name=f"Normal UV {uv}", pbrMetallicRoughness=dict(
            baseColorFactor=[0.55, 0.65, 0.75, 1], metallicFactor=0, roughnessFactor=0.65))
        if uv is not None:
            material["normalTexture"] = dict(index=0, texCoord=uv)
        slot = len(asset.model["materials"])
        asset.model["materials"].append(material)
        return slot

    def add_mesh(material, tangent):
        attributes = dict(common)
        if tangent is not None:
            attributes["TANGENT"] = asset.accessor("VEC4", [tangent] * 3)
        slot = len(asset.model["meshes"])
        primitive = dict(attributes=attributes, indices=indices, material=material)
        mesh = dict(primitives=[primitive])
        if mode == "morphed":
            primitive["targets"] = [dict(POSITION=asset.accessor("VEC3", MORPH_DELTAS))]
            mesh["weights"] = [1]
        asset.model["meshes"].append(mesh)
        return slot

    if reference is None:
        material = add_material(0)
        if mode != "missing":
            add_material(1)
        shared = add_mesh(material, WRONG_UV0_TANGENT if mode == "authored" else None)
        meshes = (shared, shared)
        if mode != "missing":
            extension = "KHR_materials_variants"
            asset.model["extensionsUsed"] = [extension]
            asset.model["extensions"] = {extension: dict(variants=[dict(name="Alternate")])}
            asset.model["meshes"][shared]["primitives"][0]["extensions"] = {
                extension: dict(mappings=[dict(material=1, variants=[0])])}
    else:
        meshes = tuple(add_mesh(add_material(uv), TANGENT if uv == 1 else WRONG_UV0_TANGENT) for uv in reference)
    asset.model["nodes"] = [dict(name=name, mesh=mesh, translation=[x, 0, 0])
                            for name, mesh, x in zip(("Left", "Right"), meshes, PLACEMENTS, strict=True)]
    asset.model["scenes"][0]["nodes"] = [0, 1]
    asset.write(path)
    return asset.model


def write_sequence(path, *, missing=False):
    def edit(frame, source, operation, **payload):
        return dict(frame=frame, materialTextureEdit=dict(sourceMaterial=source, slot="normal", operation=operation, **payload))

    if missing:
        events = [dict(edit(14, 0, "editMapping", mapping=dict(texCoord=1)), expectMaterialTextureFailure=True)]
        frames = [13, 19]
    else:
        events = [
            dict(frame=14, materialBinding=dict(operation="makeUnique", graphNode=GRAPH_NODES[0],
                                               expectedRuntimeMaterial=2, sourceMaterial=0, runtimeMaterial=2)),
            edit(20, 2, "editMapping", mapping=dict(texCoord=1)),
            edit(26, 2, "clear"),
            dict(frame=32, sceneHistory="undo"),
            dict(frame=38, sceneHistory="undo"),
            dict(frame=44, sceneHistory="redo"),
            dict(frame=50, materialBinding=dict(operation="assign", graphNode=GRAPH_NODES[1],
                                               expectedRuntimeMaterial=2, sourceMaterial=1, runtimeMaterial=3)),
            dict(frame=56, sceneHistory="undo"),
            dict(frame=62, materialVariant=0),
            dict(frame=68, sceneHistory="undo"),
        ]
        frames = [step[0] for step in STEPS] + [67, 73]
    path.write_text(json.dumps(dict(schemaVersion=1, frames=frames[-1], sampleFrames=frames, events=events), indent=2),
                    encoding="utf-8")
    return frames


def capture(renderer, output, model, technique, name, frames, record, *, sequence=None, save=None, timeout=120):
    from gltf_extended_materials_runtime_regression import validation_log_error

    target = output / f"{technique}-{name}"
    image, log = target.with_suffix(".png"), target.with_suffix(".log")
    command = [
        str(renderer), "--hidden", "--no-ui", "--validation", "--screenshot-viewport",
        "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1", "--no-taa", "--no-bloom",
        "--warmup-frames", "12", "--capture-frame", "13", "--fixed-dt", ".016666667",
        "--render-technique", technique, "--display-mode", "lit", "--tone-map", "clip", "--exposure", "1",
        "--no-ray-query", "--no-ddgi", "--ray-shadows", "raster", "--background-color", "0", "0", "0",
        "--environment-intensity", "0", "--bounce-intensity", "0", "--directional-intensity", "3",
        "--directional-direction", "0", "1", "-1", "--directional-color", "1", "1", "1",
        "--camera-position", "0", "0", "4", "--camera-target", "0", "0", "0", "--camera-fov", "45",
        "--import-scale", "1", "--model", str(model), "--screenshot", str(image),
    ]
    if technique == "path-tracing":
        command.remove("--no-ray-query")
        command += ["--path-spp", "4", "--path-samples", "4", "--path-bounces", "2", "--path-seed", "1"]
    if sequence:
        command += ["--capture-sequence", str(sequence)]
    if save:
        command += ["--save-scene", str(save)]
    record.update(command=command, log=str(log))
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
            env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output), VK_LOADER_DEBUG="layer"), timeout=timeout,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    text = log.read_text(encoding="utf-8", errors="replace")
    require(process.returncode == 0 and not validation_log_error(text), f"Native capture failed: {log}")
    require('Insert instance layer "VK_LAYER_KHRONOS_validation"' in text, f"Validation layer unavailable: {log}")
    record["validationClean"] = True
    captures = {}
    for frame in frames:
        png = image if frame == frames[-1] else Path(f"{target}.frame-{frame:04d}.png")
        telemetry = png.with_suffix(".telemetry.json")
        state = json.loads(telemetry.read_text(encoding="utf-8"))
        require(state["technique"] == technique and state["resolution"] == [WIDTH, HEIGHT] and
                state["msaaSamples"] == 1 and not state["taa"]["enabled"] and png.is_file(), f"Wrong native frame: {png}")
        if technique == "path-tracing":
            path = state["referencePath"]
            require(path["active"] and path["attemptedSamples"] == 4 and
                    path["unsupportedSamples"] == path["rejectedSamples"] == 0, f"Wrong path tracing state: {path}")
        captures[frame] = dict(image=png, state=state)
    record["samples"] = {frame: str(item["image"]) for frame, item in captures.items()}
    return captures


def effective_uv(material):
    info = material.get("normalTexture", {})
    if info.get("index", -1) < 0:
        return None
    return info.get("extensions", {}).get("KHR_texture_transform", {}).get("texCoord", info.get("texCoord", 0))


def verify_state(state, uvs, *, sources=None, history=(0, 0), built_from_shared_mesh=False):
    from scene_node_authoring_runtime_regression import history_cursor

    graph = state["gltfMerge"]
    catalog = state["gltfMaterialBindings"]
    require(not graph["authoringNodesTruncated"] and not catalog["drawsTruncated"] and
            not catalog["materialsTruncated"] and len(catalog["draws"]) == 2, "Incomplete binding telemetry")
    rows = sorted((node for node in graph["authoringNodes"] if node["renderable"] and node["active"] and not node["retired"]),
                  key=lambda node: node["worldColumns"][3][0])
    require(len(rows) == 2 and all(abs(row["worldColumns"][3][0] - x) < 1e-5
                for row, x in zip(rows, PLACEMENTS, strict=True)), "Incorrect live instance placement")
    if built_from_shared_mesh:
        require(tuple(row["graphNode"] for row in rows) == GRAPH_NODES and graph["sourceTables"]["meshes"] == 1,
                "Binding operations lost shared authored geometry or changed graph identities")
    records = {entry["runtimeMaterial"]: entry for entry in catalog["materials"]}
    draws = {entry["graphNode"]: entry for entry in catalog["draws"]}
    slots = {(entry["sourceMaterial"], entry["slot"]): entry for entry in state["gltfMaterialTextures"]["slots"]}
    source_bindings = []
    runtime_textures = set()
    for row, uv in zip(rows, uvs, strict=True):
        material = row["materialIndex"]
        draw = draws[row["graphNode"]]
        entry = records[material]
        source = entry["sourceMaterial"]
        source_bindings.append(source)
        slot = slots[(source, "normal")]
        require(entry["active"] and draw["runtimeMaterial"] == material and draw["indexCount"] == 3 and not draw["transparent"],
                "Actual draw stopped using its bound scene material")
        require(effective_uv(entry["authored"]) == effective_uv(entry["evaluated"]) == uv,
                "Authored and evaluated normal UV did not follow the edit")
        if uv is None:
            require(slot["authoredTexture"] == -1 and slot["runtimeTexture"] is None, "Clear left a bound runtime map")
        else:
            require(slot["texCoord"] == uv and not slot["srgb"] and slot["runtimeTexture"] is not None,
                    "Runtime normal slot did not publish the effective UV and linear texture")
            if built_from_shared_mesh:
                require(slot["authoredTexture"] == slot["evaluatedTexture"] == 0, "UV-only edit changed the source texture")
                runtime_textures.add(slot["runtimeTexture"])
    if built_from_shared_mesh:
        require(len(runtime_textures) == 1, "UV-only edit stopped sharing the existing runtime image")
    if sources is not None:
        require(tuple(source_bindings) == sources, "Edit affected the wrong source material binding")
    history_cursor(state, *history, allow_uncreated=history == (0, 0))
    return source_bindings


def pixels(record):
    import numpy as np
    from PIL import Image

    with Image.open(record["image"]) as image:
        rgb = np.asarray(image.convert("RGB"), dtype=np.int16)
    require(rgb.shape == (HEIGHT, WIDTH, 3), f"Wrong PNG dimensions: {record['image']}")
    return rgb


def image_match(actual, expected):
    import numpy as np

    delta = np.abs(pixels(actual) - pixels(expected))
    halves = (delta[:, :WIDTH // 2], delta[:, WIDTH // 2:])
    evidence = [dict(mae=float(half.mean()), p999=float(np.percentile(half, 99.9))) for half in halves]
    require(all(item["mae"] <= 0.15 and item["p999"] <= 2 for item in evidence),
            f"Tangent-frame oracles differ from {actual['image']}: {evidence}")
    return evidence


def verify_oracles(controls):
    import numpy as np

    results = []
    for before, after, side in (("initial", "edited", 0), ("edited", "cleared", 0), ("edited", "assigned", 1)):
        delta = np.abs(pixels(controls[before]) - pixels(controls[after]))
        halves = (delta[:, :WIDTH // 2], delta[:, WIDTH // 2:])
        active, other = halves[side], halves[1 - side]
        changed = float(np.mean(np.max(active, axis=2) >= 16))
        require(changed > 0.015 and active.mean() > 0.5 and other.max() <= 2,
                f"Literal tangent/clear controls are vacuous or leak to the other instance: {before}, {after}, {changed}")
        results.append(dict(before=before, after=after, changedSide=side, changedFraction=changed))
    return results


def verify_morph_pose(state, uvs):
    import numpy as np
    from gltf_tangent_uv_runtime_regression import POSITIONS, TANGENT, WRONG_UV0_TANGENT

    deformation = state["gltfDeformation"]
    require(deformation["active"] and deformation["gpuReady"] and deformation["currentBoundToGpu"],
            "Morph cohort did not exercise live deformation")
    instances = sorted(deformation["instances"], key=lambda row: row["modelColumns"][3][0])
    require(len(instances) == 2, "Morph cohort did not publish both instances")
    for sample, uv in zip(instances, uvs, strict=True):
        expected_tangent = TANGENT if uv == 1 else WRONG_UV0_TANGENT
        np.testing.assert_allclose(sample["firstTangent"], expected_tangent, atol=2e-4, rtol=0)
        np.testing.assert_allclose(sample["firstPosition"], np.add(POSITIONS[0], MORPH_DELTAS[0]), atol=2e-4, rtol=0)
        require(sample["weights"] == [1], "Edit changed the active morph weights")


def inspect_export(path, *, authored=False, morphed=False):
    import numpy as np
    from gltf_accessor_runtime_regression import decode_accessor, load_asset
    from gltf_merge_runtime_regression import image_pixels
    from gltf_tangent_uv_runtime_regression import INDICES, POSITIONS, UV0, UV1, WRONG_UV0_TANGENT

    model, buffers = load_asset(path)
    nodes = model["nodes"]
    todo = [(index, 0.0) for index in model["scenes"][model.get("scene", 0)]["nodes"]]
    instances, seen = [], set()
    while todo:
        index, parent_x = todo.pop()
        require(index not in seen and 0 <= index < len(nodes), "Invalid exported node tree")
        seen.add(index)
        node = nodes[index]
        x = parent_x + (node["matrix"][12] if "matrix" in node else node.get("translation", [0, 0, 0])[0])
        todo.extend((child, x) for child in node.get("children", ()))
        if "mesh" in node:
            primitive, = model["meshes"][node["mesh"]]["primitives"]
            instances.append((x, primitive))
    instances.sort(key=lambda row: row[0])
    require(len(instances) == 2 and instances[0][1]["material"] != instances[1][1]["material"],
            "Export collapsed distinct bindings of the shared mesh")
    np.testing.assert_allclose([item[0] for item in instances], PLACEMENTS, atol=1e-5, rtol=0)
    for (_, primitive), uv in zip(instances, CONTROLS["edited"], strict=True):
        attrs = primitive["attributes"]
        for semantic, values in (("POSITION", POSITIONS), ("NORMAL", [(0, 0, 1)] * 3),
                                 ("TEXCOORD_0", UV0), ("TEXCOORD_1", UV1)):
            np.testing.assert_array_equal(decode_accessor(path, attrs[semantic]), np.asarray(values, dtype=np.float32))
        np.testing.assert_array_equal(decode_accessor(path, primitive["indices"]).ravel(), INDICES)
        if morphed:
            target, = primitive["targets"]
            np.testing.assert_array_equal(decode_accessor(path, target["POSITION"]), MORPH_DELTAS)
        if uv == 1:
            require("TANGENT" not in attrs, "Export retained an authored tangent for a changed UV basis")
        if authored and uv == 0:
            require("TANGENT" in attrs, "Export lost the unchanged authored UV0 tangent")
        if "TANGENT" in attrs:
            np.testing.assert_array_equal(decode_accessor(path, attrs["TANGENT"]), [WRONG_UV0_TANGENT] * 3)
        material = model["materials"][primitive["material"]]
        require(effective_uv(material) == uv, "Export lost the per-surface normal UV")
        tex = model["textures"][material["normalTexture"]["index"]]
        np.testing.assert_array_equal(image_pixels(path, model, buffers, tex["source"]), np.tile([217, 153, 217, 255], (2, 2, 1)))
    return [primitive["material"] for _, primitive in instances]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--managed-output", action="store_true")
    parser.add_argument("--require-opt-in", action="store_true")
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--cohort", action="append", choices=COHORTS)
    parser.add_argument("--technique", action="append", choices=(*TECHNIQUES, "path-tracing"))
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()
    if args.require_opt_in and not args.generate_only and os.environ.get(GPU_OPT_IN) != "1":
        print(f"Set {GPU_OPT_IN}=1 to run the live normal UV GPU regression.")
        return 77
    if not args.generate_only and (args.renderer is None or not args.renderer.is_file()):
        parser.error("--renderer must name an existing executable")

    from gltf_merge_runtime_regression import portable_copy
    from gltf_tangent_uv_runtime_regression import verify_source_order

    output = args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    print(f"Live normal UV fixtures and captures: {output}", flush=True)
    sources = output / "sources"
    sources.mkdir(parents=True, exist_ok=False)
    source_models = {cohort: write_fixture(sources / f"{cohort}.gltf", cohort) for cohort in COHORTS}
    control_models = [write_fixture(sources / f"literal-{case}.gltf", "literal", reference=uvs)
                      for case, uvs in CONTROLS.items()]
    verify_source_order([source_models["generated"], *control_models])
    if "morphed" in (args.cohort or DEFAULT_COHORTS):
        for case, uvs in CONTROLS.items():
            write_fixture(sources / f"morph-literal-{case}.gltf", "literal", reference=uvs, morphed_reference=True)
    edit_frames = write_sequence(sources / "edits.json")
    missing_frames = write_sequence(sources / "missing.json", missing=True)
    if args.generate_only:
        return 0

    renderer = args.renderer.resolve()
    report = dict(passed=False, captures=[], checks=[])
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")

    def run(technique, model, name, frames=(13,), **options):
        record = dict(technique=technique, name=name)
        report["captures"].append(record)
        return capture(renderer, output, model, technique, name, frames, record, timeout=args.timeout, **options)

    try:
        for technique in args.technique or TECHNIQUES:
            controls = {case: run(technique, sources / f"literal-{case}.gltf", f"literal-{case}")[13] for case in CONTROLS}
            report["checks"].append(dict(technique=technique, literalDifferentiation=verify_oracles(controls)))
            for cohort in args.cohort or DEFAULT_COHORTS:
                if cohort == "missing":
                    result = run(technique, sources / "missing.gltf", cohort, missing_frames, sequence=sources / "missing.json")
                    before, after = (result[frame] for frame in missing_frames)
                    for frame in (before, after):
                        verify_state(frame["state"], (0, 0), sources=(0, 0), built_from_shared_mesh=True)
                    for section in ("gltfMaterialBindings", "gltfMaterialAuthoring"):
                        require(before["state"][section] == after["state"][section], "Rejected UV edit published material state")
                    error = after["state"]["gltfMaterialTextures"]["lastError"]
                    require(re.search(r"TEXCOORD|UV|texture.coordinate", error, re.I), f"Wrong missing UV rejection: {error}")
                    report["checks"].append(dict(technique=technique, cohort=cohort, rejection=error,
                                                 before=image_match(before, controls["initial"]), after=image_match(after, before)))
                    continue
                morphed = cohort == "morphed"
                expected = controls
                if morphed:
                    expected = {case: run(technique, sources / f"morph-literal-{case}.gltf", f"morph-literal-{case}")[13]
                                for case in CONTROLS}
                    report["checks"].append(dict(technique=technique, cohort=cohort, literalDifferentiation=verify_oracles(expected)))
                save = output / f"{technique}-{cohort}.glb"
                frames = run(technique, sources / f"{cohort}.gltf", cohort, edit_frames, sequence=sources / "edits.json", save=save)
                for frame, visual, bindings, history in STEPS:
                    current = frames[frame]
                    verify_state(current["state"], CONTROLS[visual], sources=bindings, history=history, built_from_shared_mesh=True)
                    if morphed:
                        verify_morph_pose(current["state"], CONTROLS[visual])
                    report["checks"].append(dict(technique=technique, cohort=cohort, frame=frame, reference=visual,
                                                 pixels=image_match(current, expected[visual])))
                verify_state(frames[67]["state"], (1, 1), sources=(1, 1), history=(3, 0), built_from_shared_mesh=True)
                verify_state(frames[73]["state"], (1, 0), sources=(2, 0), history=(2, 1), built_from_shared_mesh=True)
                variant_visual = "initial" if cohort == "authored" else "assigned"
                report["checks"].append(dict(technique=technique, cohort=cohort,
                                             namedVariant=image_match(frames[67], expected[variant_visual]),
                                             namedVariantUndo=image_match(frames[73], expected["edited"])))
                materials = inspect_export(save, authored=cohort == "authored", morphed=morphed)
                portable, files = portable_copy(save, output / f"{technique}-{cohort}-portable")
                reload = run(technique, portable, f"{cohort}-reload")[13]
                verify_state(reload["state"], CONTROLS["edited"])
                if morphed:
                    verify_morph_pose(reload["state"], CONTROLS["edited"])
                report["checks"].append(dict(technique=technique, cohort=cohort, exportMaterials=materials, portableFiles=files,
                                             reloadPixels=image_match(reload, expected["edited"])))
                print(f"{technique} {cohort}: shared binding edit, history, and reload matched", flush=True)
        report["passed"] = True
    finally:
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
