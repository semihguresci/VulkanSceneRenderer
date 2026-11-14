"""Verify live raster wireframe and diagnostic overlays, including raw export."""

import argparse
from collections import defaultdict
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

from display_capture_test_support import emitter_fixture, validation_log_error
from physics_auxiliary_regression import cube


WIDTH, HEIGHT = 384, 288
DEFERRED, FORWARD = "deferred-raster", "forward-raster"
MODES = ("off", "overlay-depth", "overlay-xray", "overlay-dim", "overlay-wide", "full", "off-again")
WINDOWS = {"bim": (98, 98, 169, 190), "scene": (231, 97, 296, 190)}
FILL_PROBES = {"bim": (128, 130), "scene": (277, 158), "blend": (212, 182)}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def wireframe(mode):
    return {"enabled": mode not in ("off", "off-again"), "mode": "full" if mode == "full" else "overlay",
            "depthTest": mode != "overlay-xray", "color": [0., 1., 1.],
            "intensity": .35 if mode == "overlay-dim" else 1., "lineWidth": 4. if mode == "overlay-wide" else 1.}


def write_fixtures(output):
    gltf = output / "wireframe.gltf"
    material = {"emissiveFactor": [.5, .5, .5], "pbrMetallicRoughness": {"baseColorFactor": [0, 0, 0, 1]},
                "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 2}}}
    asset = emitter_fixture(gltf, material)
    asset["nodes"][0].update(translation=[1.65, 0, 0], scale=[.73, .94, 1])
    asset["materials"].append({"emissiveFactor": [.5, .25, .125], "alphaMode": "BLEND",
                               "pbrMetallicRoughness": {"baseColorFactor": [0, 0, 0, .5]},
                               "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 8}}})
    asset["meshes"].append({"primitives": [{**asset["meshes"][0]["primitives"][0], "material": 1}]})
    asset["nodes"].append({"mesh": 1, "translation": [.45, -1.05, .4], "scale": [.42, .43, 1]})
    asset["scenes"][0]["nodes"].append(1)
    gltf.write_text(json.dumps(asset), encoding="utf-8")

    bim = output / "wireframe.bim"
    points, _, indices = cube()
    meshes, elements = [], []
    boxes = (((-1.45, 0., .1), (1.6, 1.5, 1.1)), ((-1.45, 0., -1.5), (.8, .9, .55)))
    for index, (center, size) in enumerate(boxes):
        world = [[size[axis] * point[axis] + center[axis] for axis in range(3)] for point in points]
        coordinates = [component for x, y, z in world for component in (x, -z, y)]
        meshes.append({"mesh_id": index, "coordinates": coordinates, "indices": indices})
        elements.append({"mesh_id": index, "guid": f"test-wall-{index}", "type": "IfcWall",
                         "color": {"r": 165, "g": 165, "b": 165, "a": 255}})
    bim.write_text(json.dumps({"schema_version": "1.1.0", "meshes": meshes, "elements": elements}), encoding="utf-8")
    return gltf, bim


def phase_plan(kind):
    clear_selection = {"viewportPointer": {"action": "select", "pixel": [4, 4]}}
    if kind.startswith("diagnostics-"):
        return ((4, DEFERRED, "off", 1, clear_selection), (6, DEFERRED, "vertices", 5, {}),
                (8, DEFERRED, "normals", 7, {}),
                (12, FORWARD, "normals", 9, {"technique": FORWARD, "editorDiagnostics": None}),
                (14, FORWARD, "off", 13, {}), (16, FORWARD, "vertices", 15, {}),
                (18, FORWARD, "off-again", 17, {}),
                (22, DEFERRED, "off-again", 19, {"technique": DEFERRED}))
    if kind == "taa":
        return ((8, DEFERRED, "off", 1, clear_selection), (16, DEFERRED, "overlay-depth", 9, {}),
                (24, FORWARD, "overlay-depth", 17, {"technique": FORWARD, "wireframe": None}),
                (32, FORWARD, "full", 25, {}), (40, FORWARD, "off-again", 33, {}))
    phases = [(4, DEFERRED, "off", 1, clear_selection)]
    if kind == "exr":
        phases.extend(((5, DEFERRED, "overlay-depth", 5, {}), (6, DEFERRED, "full", 6, {}),
                       (9, FORWARD, "full", 7, {"technique": FORWARD, "wireframe": None}),
                       (10, FORWARD, "overlay-depth", 10, {}), (11, FORWARD, "off", 11, {}),
                       (14, DEFERRED, "off-again", 12, {"technique": DEFERRED})))
    else:
        phases.extend((frame, DEFERRED, mode, frame, {}) for frame, mode in enumerate(MODES[1:6], start=5))
        phases.extend(((12, FORWARD, "full", 10, {"technique": FORWARD, "wireframe": None}),
                       (13, FORWARD, "overlay-wide", 13, {}), (14, FORWARD, "overlay-dim", 14, {}),
                       (15, FORWARD, "overlay-xray", 15, {}), (16, FORWARD, "overlay-depth", 16, {}),
                       (17, FORWARD, "off", 17, {}), (18, FORWARD, "off-again", 18, {}),
                       (21, DEFERRED, "off-again", 19, {"technique": DEFERRED})))
    return tuple(phases)


def write_sequence(path, phases, kind):
    events = []
    diagnostics = kind.startswith("diagnostics-")
    control = "editorDiagnostics" if diagnostics else "wireframe"
    for _, _, mode, event_frame, options in phases:
        settings = {"vertices": mode == "vertices", "normalValidation": mode == "normals"} if diagnostics else wireframe(mode)
        event = {"frame": event_frame, control: settings, **options}
        if event[control] is None:
            del event[control]
        events.append(event)
    path.write_text(json.dumps({"schemaVersion": 1, "frames": phases[-1][0],
                                "sampleFrames": [phase[0] for phase in phases], "events": events}, indent=2) + "\n",
                    encoding="utf-8")


def capture(renderer, folder, gltf, bim, kind, report):
    phases = phase_plan(kind)
    extension = "exr" if kind.endswith("exr") else "png"
    msaa = 4 if kind == "diagnostics-png" else 1
    sequence, final, log = folder / f"{kind}.json", folder / f"{kind}.{extension}", folder / f"{kind}.log"
    write_sequence(sequence, phases, kind)
    command = [str(renderer), "--model", str(gltf), "--bim-model", str(bim), "--import-scale", "1", "--hidden",
               "--validation", "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", str(msaa), "--no-bloom", "--no-ray-query",
               "--no-ddgi", "--render-technique", DEFERRED, "--display-mode", "lit", "--camera-position", "0", "0", "8",
               "--camera-target", "0", "0", "0", "--camera-fov", "45", "--sky", "plain", "--sky-horizon", "0", "0", "0",
               "--exposure", "1", "--tone-map", "reinhard", "--directional-intensity", "2", "--directional-direction",
               "-.3", "-.2", "-1", "--environment-intensity", "0", "--bounce-intensity", ".2", "--fixed-dt", ".016666667",
               "--screenshot-viewport", "--capture-sequence", str(sequence), "--screenshot", str(final),
               "--taa" if kind == "taa" else "--no-taa"]
    row = {"lane": kind, "command": command, "log": str(log), "captures": []}
    report["runs"].append(row)
    if report["generateOnly"]:
        return {}, {}
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.run(command, cwd=renderer.parent, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(folder)),
                                 stdout=stream, stderr=subprocess.STDOUT, timeout=180,
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    row["exitCode"] = process.returncode
    text = log.read_text(encoding="utf-8", errors="replace")
    require(process.returncode == 0 and not validation_log_error(text), f"Renderer or Vulkan validation failed. See {log}")
    if os.environ.get("VK_LOADER_DEBUG") == "layer":
        for action in ("Insert instance", "Inserted device"):
            require(re.search(rf'{action} layer "VK_LAYER_KHRONOS_validation"', text), f"Missing Vulkan validation. See {log}")

    import cv2
    import numpy as np
    from PIL import Image

    images, records = defaultdict(dict), defaultdict(dict)
    for frame, technique, mode, *_ in phases:
        image = final if frame == phases[-1][0] else final.with_name(f"{final.stem}.frame-{frame:04d}.{extension}")
        state = json.loads(image.with_suffix(".telemetry.json").read_bytes())
        metadata = json.loads(Path(f"{image}.capture.json").read_bytes())
        require(state["validationEnabled"] and state["technique"] == technique and
                state["scene"]["auxiliaryObjectCount"] >= 2 and Path(state["scene"]["auxiliary"]).resolve() == bim and
                (metadata["width"], metadata["height"], metadata["includesUi"]) == (WIDTH, HEIGHT, False),
                f"Incorrect capture state for {image}")
        require(msaa not in state["supportedMsaaSamples"] or state["msaaSamples"] == msaa,
                f"Incorrect sample count for {image}")
        camera = state["camera"]["unjitteredViewProjColumns"]
        require(row.setdefault("camera", camera) == camera, f"Camera changed in {image}")
        passes = sorted(item["name"] for item in state["passes"] if item["active"])
        if extension == "exr":
            require(metadata["source"] == "raster-scene-composite" and metadata["colorSpace"] == "linear-sRGB" and
                    not metadata["exposureApplied"] and not metadata["toneMappingApplied"], f"Invalid EXR metadata in {image}")
            bgr = cv2.imdecode(np.frombuffer(image.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
            require(bgr is not None and bgr.shape == (HEIGHT, WIDTH, 3) and bgr.dtype == np.float32 and
                    np.isfinite(bgr).all(), f"Invalid radiance in {image}")
            pixels = bgr[:, :, ::-1]
        else:
            require(metadata["source"] == "viewport", f"Invalid viewport PNG metadata in {image}")
            with Image.open(image) as png:
                require(png.size == (WIDTH, HEIGHT) and png.format == "PNG", f"Invalid PNG {image}")
                pixels = np.asarray(png.convert("RGB"), dtype=np.int16)
        images[technique][mode] = pixels
        item = {"frame": frame, "technique": technique, "mode": mode, "image": str(image), "passes": passes,
                "revision": state["build"]["revision"],
                "msaaSamples": state["msaaSamples"],
                "wireframeWideLinesSupported": state.get("wireframeWideLinesSupported", False),
                "decodedSha256": hashlib.sha256(pixels.tobytes()).hexdigest()}
        row["captures"].append(item)
        records[technique][mode] = item
    return images, records


def luma(rgb):
    return rgb.mean(axis=-1)


def inspect_png(images, records, expect_missing_forward):
    import numpy as np

    for technique, captures in images.items():
        before = captures["off"]
        np.testing.assert_array_equal(captures["off-again"], before, err_msg=f"Stale lines after disabling {technique} wireframe")
        require("OitResolve" in records[technique]["off"]["passes"], f"No lit transparent geometry in {technique}")
        missing = expect_missing_forward and technique == FORWARD
        for mode in MODES[1:6]:
            after = captures[mode]
            difference = np.max(np.abs(before - after), axis=-1)
            cyan = (np.minimum(after[:, :, 1], after[:, :, 2]) - after[:, :, 0]) > 25
            mask = cyan & (difference > 8)
            row = records[technique][mode]
            row["wirePixels"] = {key: int(np.count_nonzero(mask[y0:y1, x0:x1]))
                                 for key, (x0, y0, x1, y1) in WINDOWS.items()}
            row["wireColorSignal"] = float(np.maximum(np.minimum(after[:, :, 1], after[:, :, 2]) - after[:, :, 0], 0)[mask].mean()) if mask.any() else 0.
            row["fillProbes"] = {key: after[y, x].tolist() for key, (x, y) in FILL_PROBES.items()}
            if missing:
                np.testing.assert_array_equal(after, before, err_msg=f"Expected baseline Forward to omit {mode}")
            else:
                require(min(row["wirePixels"].values()) > 25, f"Missing scene/BIM {mode} in {technique}: {row['wirePixels']}")
        if missing:
            continue
        depth, xray, dim, wide, full = [records[technique][mode] for mode in MODES[1:6]]
        require(xray["wirePixels"]["bim"] > depth["wirePixels"]["bim"] + 25, f"No hidden BIM edge witness in {technique}")
        require(dim["wireColorSignal"] < depth["wireColorSignal"] * .9, f"Wireframe intensity had no effect in {technique}")
        if wide["wireframeWideLinesSupported"]:
            require(sum(wide["wirePixels"].values()) > sum(depth["wirePixels"].values()) + 25,
                    f"Wireframe width had no effect in {technique}")
        for key, (x, y) in FILL_PROBES.items():
            np.testing.assert_array_equal(captures["overlay-depth"][y, x], before[y, x],
                                          err_msg=f"Overlay changed interior {key} shading in {technique}")
            require(float(luma(before[y, x])) > 35 and max(full["fillProbes"][key]) <= 4,
                    f"Full mode did not remove {key} fill in {technique}: {before[y, x]} to {full['fillProbes'][key]}")
        require("OitResolve" not in full["passes"], f"Full mode still composed transparency in {technique}")


def inspect_exr(images, records):
    import numpy as np

    for technique, captures in images.items():
        before = captures["off"]
        x, y = FILL_PROBES["blend"]
        np.testing.assert_allclose(before[y, x], [2, 1, .5], atol=.008, rtol=0)
        for mode, pixels in captures.items():
            require("OitResolve" in records[technique][mode]["passes"], f"EXR omitted composition in {technique} {mode}")
            np.testing.assert_array_equal(pixels, before, err_msg=f"Wireframe changed EXR radiance in {technique} {mode}")


def inspect_taa(images, records, expect_missing_forward):
    import numpy as np

    for technique, captures in images.items():
        for item in records[technique].values():
            require("TemporalResolve" in item["passes"], f"TAA was inactive in {item['image']}")
        for mode, pixels in captures.items():
            cyan = np.minimum(pixels[:, :, 1], pixels[:, :, 2]) - pixels[:, :, 0] > 25
            count = int(cyan.sum())
            records[technique][mode]["wirePixels"] = count
            expected = mode in ("overlay-depth", "full") and not (expect_missing_forward and technique == FORWARD)
            require(count > 100 if expected else count < 10, f"Wrong TAA wireframe witness {count} in {technique} {mode}")
        if "full" in captures and not expect_missing_forward:
            require("OitResolve" not in records[technique]["full"]["passes"], f"Full TAA mode composed transparency in {technique}")
            x, y = FILL_PROBES["blend"]
            require(captures["full"][y, x].max() <= 4 and captures["off-again"][y, x].max() > 100,
                    f"Unexpected TAA transparency fill after wireframe change in {technique}")


def inspect_diagnostics(images, records, *, scene_linear):
    import numpy as np

    for technique, captures in images.items():
        before = captures["off"]
        np.testing.assert_array_equal(captures["off-again"], before,
                                      err_msg=f"Stale diagnostic overlay in {technique}")
        for mode in ("vertices", "normals"):
            after = captures[mode]
            if scene_linear:
                np.testing.assert_array_equal(after, before,
                                              err_msg=f"{mode} changed scene-linear radiance in {technique}")
                continue
            changed = np.max(np.abs(after - before), axis=-1) > 20
            if mode == "normals":
                changed &= after[:, :, 2] - after[:, :, 0] > 30
            counts = {key: int(changed[y0:y1, x0:x1].sum())
                      for key, (x0, y0, x1, y1) in WINDOWS.items()}
            records[technique][mode]["diagnosticPixels"] = counts
            require(min(counts.values()) >= (100 if mode == "normals" else 12),
                    f"Missing scene/BIM {mode} diagnostics in {technique}: {counts}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--managed-output", action="store_true")
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--expect-missing-forward", action="store_true", help="check an instrumented pre-fix runtime")
    args = parser.parse_args()
    if not args.generate_only and os.environ.get("CONTAINER_RUN_GPU_BIM_FLOOR_PLAN") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_BIM_FLOOR_PLAN=1 to run GPU captures")
        return 77
    os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
    renderer, output = args.renderer.resolve(strict=not args.generate_only), args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
        print(f"Captures: {output}", flush=True)
    else:
        output.mkdir(parents=True)
    gltf, bim = write_fixtures(output)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    report = {"passed": False, "generateOnly": args.generate_only, "expectedMissingForward": args.expect_missing_forward,
              "executableSha256": None if args.generate_only else hashlib.sha256(renderer.read_bytes()).hexdigest(), "runs": []}
    try:
        lanes = ["png", "exr", "taa"]
        if not args.expect_missing_forward:
            lanes += ["diagnostics-png", "diagnostics-exr"]
        for kind in lanes:
            images, records = capture(renderer, output, gltf, bim, kind, report)
            if args.generate_only:
                continue
            if kind == "png":
                inspect_png(images, records, args.expect_missing_forward)
            elif kind == "exr":
                inspect_exr(images, records)
            elif kind == "taa":
                inspect_taa(images, records, args.expect_missing_forward)
            else:
                inspect_diagnostics(images, records, scene_linear=kind.endswith("exr"))
            print(f"{kind}: Forward and Deferred overlay behavior verified", flush=True)
        report["passed"] = True
    finally:
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
