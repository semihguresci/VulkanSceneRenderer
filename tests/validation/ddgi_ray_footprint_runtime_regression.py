"""Compare DDGI ray cones to a shader-only mip-zero control in both raster paths."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
import cv2
import numpy as np

from gltf_accessor_runtime_regression import Asset
from gltf_extended_materials_runtime_regression import validation_log_error

FRAMES = list(range(14, 30, 2))
TRACE_SHADER = "spv_shaders/ddgi_trace.comp.spv"
TECHNIQUES = {"deferred": "deferred-raster", "forward": "forward-raster"}


def runtime_manifest(renderer):
    root = renderer.parent
    files = [renderer, *root.glob("*.dll"), *(root / "spv_shaders").glob("*.spv")]
    for directory in ("materials", "hdr"):
        assets = sorted(path for path in (root / directory).rglob("*") if path.is_file())
        if not assets:
            raise ValueError(f"Missing runtime assets in {root / directory}")
        files.extend(assets)
    return {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(files)}


def create_scene(path, uniform):
    asset = Asset()
    receiver = dict(pbrMetallicRoughness=dict(baseColorFactor=[1, 1, 1, 1],
                                             metallicFactor=0, roughnessFactor=1))
    emitter = dict(pbrMetallicRoughness=dict(baseColorFactor=[0, 0, 0, 1],
                                            metallicFactor=0, roughnessFactor=1),
                   emissiveFactor=[.5 if uniform else 1] * 3,
                   extensions={"KHR_materials_emissive_strength": dict(emissiveStrength=8)})
    if not uniform:
        x = np.arange(512) // 2
        data = np.full((512, 512, 4), 255, dtype=np.uint8)
        data[:, :, :3] = (((x[:, None] + x[None, :]) % 2) * 255)[:, :, None]
        emitter["emissiveTexture"] = dict(index=asset.image(data))
        asset.model["samplers"][0].update(minFilter=9987, magFilter=9728)

    def quad(x0, x1, y0, y1, z, faces_up, material):
        return dict(attributes=dict(
            POSITION=asset.accessor("VEC3", [[x0,y0,z], [x1,y0,z], [x1,y1,z], [x0,y1,z]]),
            NORMAL=asset.accessor("VEC3", [[0,0,1 if faces_up else -1]] * 4),
            TEXCOORD_0=asset.accessor("VEC2", [[0,0],[1,0],[1,1],[0,1]])),
            indices=asset.accessor("SCALAR", [0,1,2,0,2,3] if faces_up else [0,2,1,0,3,2],
                                   5123, indices=True), material=material)

    asset.model["materials"] = [receiver, emitter]
    asset.model["meshes"] = [dict(primitives=[quad(-1.5, 1.5, -1.5, 1.5, 0, True, 0),
                                             quad(.48, 2.2, -1.2, 1.2, .75, False, 1)])]
    asset.model["nodes"] = [dict(mesh=0)]
    asset.model["scenes"] = [dict(nodes=[0])]
    asset.model["extensionsUsed"] = ["KHR_materials_emissive_strength"]
    asset.write(path)


def capture(renderer, output, model, label, route, technique):
    stem = output / (label + "-" + route)
    sequence = stem.with_suffix(".sequence.json")
    sequence.write_text(json.dumps(dict(schemaVersion=1, frames=FRAMES[-1], sampleFrames=FRAMES,
                                       events=[])), encoding="utf-8")
    command = [str(renderer), "--hidden", "--no-ui", "--validation", "--no-bloom", "--no-taa",
               "--width", "256", "--height", "192", "--msaa", "1", "--fixed-dt", ".016666667",
               "--model", str(model), "--import-scale", "1", "--render-technique", technique,
               "--display-mode", "lit", "--directional-intensity", "0", "--environment-intensity", "0",
               "--camera-position", "0", "0", "4", "--camera-target", "0", "0", "0", "--camera-fov", "18",
               "--ddgi", "--no-ddgi-async", "--ddgi-origin", "-1.5", "-1.5", "-.2",
               "--ddgi-counts", "6", "6", "4", "--ddgi-spacing", ".6", ".6", ".4",
               "--ddgi-rays", "64", "--ddgi-probes-per-frame", "144", "--ddgi-hysteresis", "0",
               "--capture-sequence", str(sequence), "--screenshot", str(stem.with_suffix(".exr")),
               "--screenshot-viewport"]
    log = stem.with_suffix(".log")
    with log.open("w", encoding="utf-8") as stream:
        run = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                             env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)), timeout=180)
    assert run.returncode == 0 and not validation_log_error(log.read_text(encoding="utf-8", errors="replace")), log
    captures, timings = [], []
    for frame in FRAMES:
        sample = stem if frame == FRAMES[-1] else Path(f"{stem}.frame-{frame:04d}")
        image = cv2.imdecode(np.frombuffer(Path(f"{sample}.exr").read_bytes(), np.uint8),
                             cv2.IMREAD_UNCHANGED)
        assert image.shape == (192,256,3) and np.isfinite(image).all()
        captures.append(image[65:128, 80:135].mean(axis=-1))
        state = json.loads(Path(f"{sample}.telemetry.json").read_text(encoding="utf-8"))
        assert state["technique"] == technique and state["validationEnabled"], sample
        assert state["ddgi"]["active"] and state["ddgi"]["tracedRays"] == 144 * 64
        trace = next(row for row in state["passes"] if row["name"] == "DdgiTrace")
        assert trace["active"] and trace["gpuTimed"] and np.isfinite(trace["gpuMs"]) and trace["gpuMs"] > 0, sample
        timings.append(trace["gpuMs"])
    images = np.asarray(captures, dtype=np.float64)
    return dict(mean=images.mean(), temporalRms=float(np.sqrt(np.mean(np.var(images, axis=0)))),
                medianTraceGpuMs=statistics.median(timings), captures=len(captures))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    renderer, baseline, output = args.renderer.resolve(), args.baseline.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    runtimes = {"filtered": renderer, "mipZero": baseline}
    results = {}
    proof = dict(passed=False, rendererPaths={name: str(path) for name, path in runtimes.items()})
    try:
        before = {name: runtime_manifest(path) for name, path in runtimes.items()}
        proof["before"] = before
        fixed, control = before["filtered"], before["mipZero"]
        differing = sorted(name for name in fixed.keys() | control.keys() if fixed.get(name) != control.get(name))
        proof["differingFiles"] = differing
        if differing != [TRACE_SHADER] or not all(TRACE_SHADER in manifest for manifest in before.values()):
            raise ValueError(f"Only the DDGI trace shader should differ, got {differing}")
        try:
            (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
            textured, uniform = output / "checker.gltf", output / "uniform.gltf"
            create_scene(textured, False)
            create_scene(uniform, True)
            for route, technique in TECHNIQUES.items():
                result = results[route] = {}
                result["filtered"] = capture(renderer, output, textured, "filtered", route, technique)
                result["mipZero"] = capture(baseline, output, textured, "mip-zero", route, technique)
                result["uniform"] = capture(renderer, output, uniform, "uniform", route, technique)
                print(route, json.dumps(result), flush=True)
                assert result["filtered"]["temporalRms"] < .65 * result["mipZero"]["temporalRms"], result
                assert result["uniform"]["mean"] > .1, result
                assert abs(result["filtered"]["mean"] / result["uniform"]["mean"] - 1) < .03, result
        finally:
            proof["after"] = {name: runtime_manifest(path) for name, path in runtimes.items()}
            if proof["after"] != before:
                raise ValueError("Frozen runtime inputs changed during capture")
        proof["passed"] = True
    except Exception as error:
        proof["failure"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        (output / "results.json").write_text(json.dumps(results, indent=2))
        (output / "runtime-proof.json").write_text(json.dumps(proof, indent=2))


if __name__ == "__main__":
    main()
