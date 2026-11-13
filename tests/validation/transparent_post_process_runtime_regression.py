"""Check transparent HDR in bloom, exposure, and raster PNG/EXR capture."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile

from display_capture_test_support import Asset, validation_log_error
from frame_resource_initialization_runtime_regression import TECHNIQUES, require


WIDTH, HEIGHT = 320, 240
HALO = (65, 120)
CENTER = (160, 120)
METER = (272, 120)
RADIANCE = (16, 2, 0)


def write_fixture(path, *, meter=False):
    asset = Asset()
    model = asset.model
    indices = asset.accessor("SCALAR", [0, 1, 2, 0, 2, 3], 5123, indices=True)
    normals = asset.accessor("VEC3", [[0, 0, 1]] * 4)

    def quad(x0, y0, x1, y1, material):
        positions = [[x0, y0, 0], [x1, y0, 0], [x1, y1, 0], [x0, y1, 0]]
        return {"primitives": [dict(attributes=dict(POSITION=asset.accessor("VEC3", positions), NORMAL=normals),
                                    indices=indices, material=material)]}

    model["extensionsUsed"] = ["KHR_materials_emissive_strength"]
    model["materials"] = [{"doubleSided": True, "alphaMode": "BLEND", "emissiveFactor": [1, .125, 0],
                           "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 32}},
                           "pbrMetallicRoughness": {"baseColorFactor": [0, 0, 0, .5]}}]
    model["meshes"] = [quad(-1, -.8, 1, .8, 0)]
    if meter:
        # The neutral patch stays unchanged when the bright BLEND node toggles.
        model["materials"].append({"emissiveFactor": [.5, .5, .5],
                                   "pbrMetallicRoughness": {"baseColorFactor": [0, 0, 0, 1]}})
        model["meshes"].append(quad(1.3, -.4, 1.8, .4, 1))
        model["nodes"].append({"mesh": 1})
        model["scenes"][0]["nodes"].append(1)
    asset.write(path)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_capture(path, technique, *, taa, bloom, auto):
    import numpy as np
    from PIL import Image

    metadata = json.loads(Path(f"{path}.capture.json").read_bytes())
    state = json.loads(path.with_suffix(".telemetry.json").read_bytes())
    require((metadata["width"], metadata["height"], metadata["includesUi"]) == (WIDTH, HEIGHT, False), metadata)
    require(state["technique"] == technique and state["validationEnabled"] and state["msaaSamples"] == 1 and
            state["taa"]["enabled"] == taa, state["technique"])
    passes = {p["name"] for p in state["passes"] if p["active"]}
    require(("TemporalResolve" in passes) == taa, passes)
    require(("ExposureAdaptation" in passes) == auto, passes)
    if path.suffix == ".exr":
        import cv2

        require(metadata["source"] == "raster-scene-composite" and metadata["colorSpace"] == "linear-sRGB" and
                not metadata["exposureApplied"] and not metadata["toneMappingApplied"], metadata)
        bgr = cv2.imdecode(np.frombuffer(path.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
        require(bgr is not None and bgr.shape == (HEIGHT, WIDTH, 3) and bgr.dtype == np.float32 and
                np.isfinite(bgr).all(), f"Invalid scene-linear EXR: {path}")
        pixels = bgr[:, :, ::-1]
    else:
        require(("Bloom" in passes) == bloom, passes)
        require(metadata["source"] == "viewport" and metadata["toneMapper"] == 1 and
                metadata["exposureMode"] == ("auto" if auto else "manual") and
                metadata["manualExposure"] == (None if auto else 1), metadata)
        with Image.open(path) as image:
            require(image.format == "PNG" and image.size == (WIDTH, HEIGHT), path)
            pixels = np.asarray(image.convert("RGB"))
    return pixels, state, {"image": str(path), "revision": state["build"]["revision"],
                           "sha256": sha(path), "activePasses": sorted(passes)}


def capture(renderer, output, model, technique, taa, kind, report, *, sequence=None):
    auto = kind == "auto-exposure"
    bloom = kind in ("bloom", "linear")
    path = output / f"{technique}-{'taa' if taa else 'native'}-{kind}{'.exr' if kind == 'linear' else '.png'}"
    log = path.with_suffix(".log")
    command = [str(renderer), "--model", str(model), "--hidden", "--no-ui", "--validation", "--import-scale", "1",
               "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1", "--render-technique", technique,
               "--display-mode", "lit", "--camera-position", "0", "0", "4", "--camera-target", "0", "0", "0",
               "--camera-fov", "45", "--sky", "plain", "--sky-horizon", "0", "0", "0", "--environment-intensity", "0",
               "--directional-intensity", "0", "--bounce-intensity", "0", "--no-ray-query", "--no-ddgi",
               "--tone-map", "reinhard", "--fixed-dt", ".016666667", "--screenshot-viewport", "--screenshot", str(path),
               "--taa" if taa else "--no-taa", "--bloom" if bloom else "--no-bloom"]
    if auto:
        command.extend(["--exposure", "1", "--auto-exposure", "--capture-sequence", str(sequence)])
    else:
        command.extend(["--exposure", "1", "--warmup-frames", "8", "--capture-frame", "9"])
    row = {"technique": technique, "taa": taa, "kind": kind, "log": str(log), "command": command, "captures": []}
    report["runs"].append(row)
    with log.open("w", encoding="utf-8") as stream:
        result = subprocess.run(command, cwd=renderer.parent, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                                stdout=stream, stderr=subprocess.STDOUT, timeout=180,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    text = log.read_text(encoding="utf-8", errors="replace")
    require(result.returncode == 0 and not validation_log_error(text), f"Vulkan capture failed: {log}")
    if os.environ.get("VK_LOADER_DEBUG") == "layer":
        require('Insert instance layer "VK_LAYER_KHRONOS_validation"' in text and
                'Inserted device layer "VK_LAYER_KHRONOS_validation"' in text, f"Missing validation layer: {log}")
    samples = []
    for image in [path.with_name(f"{path.stem}.frame-0032.png"), path] if auto else [path]:
        pixels, state, record = read_capture(image, technique, taa=taa, bloom=bloom, auto=auto)
        row["captures"].append(record)
        samples.append((pixels, state))
    return samples


def reinhard_srgb(value):
    mapped = value / (1 + value)
    return round(255 * (12.92 * mapped if mapped <= .0031308 else 1.055 * mapped ** (1 / 2.4) - .055))


def patch(pixels, point, radius=3):
    x, y = point
    return pixels[y - radius:y + radius + 1, x - radius:x + radius + 1]


def check_manual(base, bloom, linear, *, missing_halo):
    import numpy as np

    off, on, hdr = base[0], bloom[0], linear[0]
    expected = [reinhard_srgb(v) for v in RADIANCE]
    require(np.max(np.abs(patch(off, CENTER).astype(np.int16) - expected)) <= 2,
            f"Lit output does not match a single OIT composite: {off[CENTER[1], CENTER[0]]} vs {expected}")
    np.testing.assert_allclose(patch(hdr, CENTER), np.broadcast_to(RADIANCE, (7, 7, 3)), rtol=0, atol=.008)
    require(not np.any(patch(hdr, HALO)), "Bloom or emissive radiance leaked into the raw EXR halo")
    require(not np.any(patch(off, HALO)), "Bloom-off halo is not black")
    halo = patch(on, HALO)
    require(not np.any(halo) if missing_halo else np.min(halo[:, :, 0]) >= 30,
            f"Unexpected bloom response outside the emitter: {halo[3, 3]}")
    for _, state in (base, bloom, linear):
        active = {p["name"] for p in state["passes"] if p["active"]}
        require("OitClear" in active and "TransparentPick" in active, active)
    return {"litCenter": off[CENTER[1], CENTER[0]].tolist(), "analyticCenter": expected,
            "rawCenter": hdr[CENTER[1], CENTER[0]].tolist(), "bloomHalo": halo[3, 3].tolist(),
            "bloomOffHalo": off[HALO[1], HALO[0]].tolist()}


def check_exposure(samples, *, missing_response):
    import numpy as np

    require(samples[0][1]["camera"]["unjitteredViewProjColumns"] ==
            samples[1][1]["camera"]["unjitteredViewProjColumns"], "Camera changed during exposure comparison")
    values = []
    for index, (pixels, state) in enumerate(samples):
        exposure = state["exposure"]
        require(exposure["mode"] == "auto" and all(math.isfinite(exposure[key]) and exposure[key] > 0
                for key in ("resolvedExposure", "averageLuminance")), exposure)
        meter = patch(pixels, METER)
        value = float(np.median(meter))
        require(value > 0, "The independent opaque exposure witness is dark")
        require(np.max(np.ptp(meter, axis=2)) <= 2, "The opaque exposure witness is not neutral")
        center = patch(pixels, CENTER)
        if index == 0:
            require(not np.any(center), "The emissive transparent node remained visible in the control")
        else:
            require(np.min(center[:, :, 0]) > 75 and np.min(center[:, :, 0].astype(np.int16) - center[:, :, 1]) > 20,
                    "The emissive transparent node did not become visible")
        values.append({"witness": value, "center": center[3, 3].tolist(), **exposure})
    before, after = values
    if missing_response:
        require(abs(after["witness"] - before["witness"]) <= 2 and
                abs(after["averageLuminance"] - before["averageLuminance"]) < .01, values)
    else:
        require(after["witness"] <= before["witness"] - 8 and
                after["averageLuminance"] > 2 * before["averageLuminance"] and
                after["resolvedExposure"] < .85 * before["resolvedExposure"], values)
    return {"transparentOff": before, "transparentOn": after}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--managed-output", action="store_true", help="write to a fresh run directory under --output")
    parser.add_argument("--expect-opaque-native-effects", action="store_true", help="verify the old native bloom/exposure inputs")
    parser.add_argument("--skip-auto-exposure", action="store_true", help="check a baseline build that predates the auto-exposure flag")
    parser.add_argument("--auto-exposure-only", action="store_true", help="verify the metering and display response only")
    args = parser.parse_args()
    if args.skip_auto_exposure and args.auto_exposure_only:
        parser.error("--skip-auto-exposure and --auto-exposure-only cannot be combined")
    if os.environ.get("CONTAINER_RUN_GPU_TRANSPARENT_POST_PROCESS") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_TRANSPARENT_POST_PROCESS=1 to run GPU captures")
        return 77
    os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
    renderer, output = args.renderer.resolve(strict=True), args.output.resolve()
    output.mkdir(parents=True, exist_ok=args.managed_output)
    if args.managed_output:
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    print(f"Captures: {output}", flush=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    model, meter, sequence = output / "emitter.gltf", output / "meter.gltf", output / "meter.sequence.json"
    write_fixture(model)
    write_fixture(meter, meter=True)
    events = [{"frame": 1, "nodeFlags": {"sourceNode": 0, "visible": False}},
              {"frame": 33, "nodeFlags": {"sourceNode": 0, "visible": True}}]
    sequence.write_text(json.dumps(dict(schemaVersion=1, frames=72, sampleFrames=[32, 72], events=events)), encoding="utf-8")
    report = {"rendererSha256": sha(renderer), "fixtureSha256": sha(model), "meterFixtureSha256": sha(meter),
              "meterSequenceSha256": sha(sequence), "expectOpaqueNativeEffects": args.expect_opaque_native_effects,
              "runs": [], "checks": {}, "passed": False}
    try:
        for technique in TECHNIQUES:
            for taa in (False, True):
                missing = args.expect_opaque_native_effects and not taa
                name = f"{technique}-{'taa' if taa else 'native'}"
                checks = report["checks"][name] = {}
                if not args.auto_exposure_only:
                    base, bloom, linear = (capture(renderer, output, model, technique, taa, kind, report)[0]
                                           for kind in ("no-bloom", "bloom", "linear"))
                    checks.update(check_manual(base, bloom, linear, missing_halo=missing))
                if not args.skip_auto_exposure:
                    samples = capture(renderer, output, meter, technique, taa, "auto-exposure", report, sequence=sequence)
                    checks["exposure"] = check_exposure(samples, missing_response=missing)
                print(name, checks, flush=True)
        require(sha(renderer) == report["rendererSha256"], "Renderer changed during captures")
        report["passed"] = True
    finally:
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
