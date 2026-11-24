"""Measure native fog history trails and foreground leaks in both raster engines."""

import argparse
import json
import math
import os
from pathlib import Path
from statistics import median
import subprocess
import sys
import tempfile

from volumetric_fog_runtime_regression import HEIGHT, OPT_IN, SUN_COLOR, TECHNIQUES, WIDTH, fog_pass, read_pixels


QUALITIES = ("low", "high")
FOV = 45
DENSITY = .27
ALBEDO = .9
SUN = 12
NEAR_DEPTH = 3
RESIZED = (256, 160)
PAN_COMPLETE = 6
CUT = 10
RESIZE = 12
MOTION_FRAMES = (1, 4, 5, 6, 7, 9, 10, 11, 12, 13)
CONTROL_MODES = ("off", "low", "low", "high", "high", "froxels", "froxels", "off", "off")
MODE_IDS = {"off": 0, "low": 1, "high": 2, "froxels": 5}


def make_scene(path, *, silhouette=False):
    from gltf_accessor_runtime_regression import Asset

    asset = Asset()
    model = asset.model
    model["nodes"], model["meshes"] = [], []
    model["extensionsUsed"] = ["KHR_materials_unlit", "KHR_node_hoverability", "KHR_node_selectability"]
    model["materials"] = [{
        "doubleSided": True,
        "extensions": {"KHR_materials_unlit": {}},
        "pbrMetallicRoughness": {"baseColorFactor": [0, 0, 0, 1]},
    }]

    def quad(name, corners, normal):
        # A delayed editor hover probe can otherwise draw a wire highlight in
        # viewport PNGs even when the fog pass is disabled.
        model["nodes"].append({
            "mesh": len(model["meshes"]),
            "name": name,
            "extensions": {
                "KHR_node_hoverability": {"hoverable": False},
                "KHR_node_selectability": {"selectable": False},
            },
        })
        model["meshes"].append({"primitives": [{
            "attributes": {"POSITION": asset.accessor("VEC3", corners), "NORMAL": asset.accessor("VEC3", [normal] * 4)},
            "indices": asset.accessor("SCALAR", [0, 1, 2, 0, 2, 3], 5123, indices=True),
            "material": 0,
        }]})

    edge = 0 if silhouette else -.1
    quad("Rear receiver", [[-12, -6, -2], [12, -6, -2], [12, 6, -2], [-12, 6, -2]], [0, 0, 1])
    quad("Foreground receiver", [[-12, -6, 3], [edge, -6, 3], [edge, 6, 3], [-12, 6, 3]], [0, 0, 1])
    if silhouette:
        # Incoming light travels along +Y. Cast a shadow aligned with the near
        # receiver's silhouette, including the section from camera to receiver.
        quad("Horizontal blocker", [[-12, -1, 2.5], [-12, -1, 6.2], [.006, -1, 6.2], [.006, -1, 2.5]], [0, 1, 0])
    else:
        # Incoming light travels along +X, and this blocker begins behind the
        # near receiver. Fog measured at the near depth has an analytic bound.
        quad("Side blocker", [[-1.4, -.7, -2.5], [-1.4, .7, -2.5], [-1.4, .7, 2.5], [-1.4, -.7, 2.5]], [1, 0, 0])
    model["scenes"] = [{"nodes": list(range(len(model["nodes"])))}]
    asset.write(path)
    return path


def motion_sequence(*, reset=False):
    poses = {1: (.50, 1.18), 4: (.50, 1.18), 5: (.25, .91), PAN_COMPLETE: (-.25, .42), 9: (-.25, .42),
             CUT: (2.0, 1.6), 13: (2.0, 1.6)}
    return {
        "schemaVersion": 1,
        "frames": MOTION_FRAMES[-1],
        "sampleFrames": list(MOTION_FRAMES),
        "camera": [{"frame": frame, "position": [x, y, 6], "target": [x, y, 0]} for frame, (x, y) in poses.items()],
        "events": [
            {"frame": frame, **({"reset": True} if reset else {}),
             **({"resize": list(RESIZED)} if frame == RESIZE else {})}
            for frame in (range(1, MOTION_FRAMES[-1] + 1) if reset else (RESIZE,))
        ],
    }


def silhouette_sequence():
    return {"schemaVersion": 1, "frames": 1, "sampleFrames": [1],
            "camera": [{"frame": 1, "position": [0, 0, 6], "target": [0, 0, 0]}]}


def controls_sequence():
    return {
        "schemaVersion": 1,
        "frames": len(CONTROL_MODES),
        "sampleFrames": list(range(1, len(CONTROL_MODES) + 1)),
        "camera": [{"frame": 1, "position": [0, 0, 6], "target": [0, 0, 0]}],
        "events": [{"frame": index + 1, "volumetricFogMode": mode} for index, mode in enumerate(CONTROL_MODES)
                   if index > 0 and CONTROL_MODES[index - 1] != mode],
    }


def read_viewport_png(path, expected_extent):
    import cv2
    import numpy as np

    sidecar = Path(f"{path}.capture.json")
    metadata = json.loads(sidecar.read_bytes())
    assert metadata["source"] == "viewport" and metadata["colorSpace"] == "sRGB", sidecar
    assert not metadata["includesUi"] and (metadata["width"], metadata["height"]) == expected_extent, sidecar
    pixels = cv2.imdecode(np.frombuffer(path.read_bytes(), np.uint8), cv2.IMREAD_COLOR)
    assert pixels is not None and pixels.dtype == np.uint8, path
    return pixels[..., ::-1] / 255.0


def capture(renderer, output, label, scene, technique, quality, sequence, light_direction, *, controls=False):
    from gltf_extended_materials_runtime_regression import validation_log_error

    extension = "png" if controls else "exr"
    image = output / f"{label}.{extension}"
    log = output / f"{label}.log"
    schedule = output / f"{label}.sequence.json"
    schedule.write_text(json.dumps(sequence, indent=2) + "\n", encoding="utf-8")
    command = [
        str(renderer), "--model", str(scene), "--import-scale", "1", "--hidden", "--no-ui", "--validation",
        "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1", "--no-taa", "--no-bloom", "--no-ddgi",
        "--render-technique", technique, "--display-mode", "lit", "--ray-shadows", "raster",
        "--environment-intensity", "0", "--directional-intensity", str(SUN),
        "--directional-direction", *map(str, light_direction), "--directional-color", *map(str, SUN_COLOR),
        "--volumetric-fog", quality, "--fog-density", str(DENSITY), "--fog-albedo", str(ALBEDO),
        "--fog-anisotropy", "0", "--fog-distance", "15", "--camera-fov", str(FOV),
        "--capture-sequence", str(schedule), "--screenshot", str(image),
    ]
    if controls:
        command += ["--screenshot-viewport", "--tone-map", "neutral", "--exposure", "1"]
    with log.open("w", encoding="utf-8") as stream:
        subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT, check=True,
                       timeout=240, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output), VK_LOADER_DEBUG="layer"),
                       creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    log_text = log.read_text(encoding="utf-8", errors="replace")
    assert 'Insert instance layer "VK_LAYER_KHRONOS_validation"' in log_text, f"Khronos validation was not loaded: {log}"
    assert not validation_log_error(log_text), f"Vulkan validation failed: {log}"
    assert "Volumetric fog unavailable" not in log_text, log

    frames = {}
    transitions = {1, *(event["frame"] for event in sequence.get("events", [])
                       if "resize" in event or "volumetricFogMode" in event)}
    resize_events = {event["frame"]: tuple(event["resize"]) for event in sequence.get("events", []) if "resize" in event}
    for frame in sequence["sampleFrames"]:
        prefix = label if frame == sequence["sampleFrames"][-1] else f"{label}.frame-{frame:04d}"
        capture_image = output / f"{prefix}.{extension}"
        telemetry = output / f"{prefix}.telemetry.json"
        state = json.loads(telemetry.read_text(encoding="utf-8"))
        fog = fog_pass(state)
        settings = state.get("volumetricFog", {})
        expected_extent = next((extent for resize_frame, extent in reversed(resize_events.items()) if resize_frame <= frame),
                               (WIDTH, HEIGHT))
        mode = CONTROL_MODES[frame - 1] if controls else quality
        active = mode != "off"
        assert state["technique"] == technique and state["gpu"] and state["validationEnabled"], telemetry
        assert not state["rayShadows"]["active"], telemetry
        assert bool(fog and fog["active"]) == active and settings.get("active", False) == active, telemetry
        assert settings.get("mode", 0) == MODE_IDS[mode], (telemetry, settings)
        allocation = settings.get("allocationBytes", 0)
        if active:
            assert any(record["active"] and record["name"] == "ShadowCascade0" for record in state["passes"]), telemetry
            assert allocation > 0 and math.isclose(settings["density"], DENSITY, abs_tol=1e-7), (telemetry, settings)
        else:
            assert allocation == 0, (telemetry, settings)
        assert tuple(state["fsr"]["renderExtent"]) == expected_extent, (telemetry, state["fsr"])
        if active and fog["gpuTimed"]:
            assert math.isfinite(fog["gpuMs"]) and fog["gpuMs"] > 0, (label, frame, fog)
        elif active and frame not in transitions:
            raise AssertionError(f"Fog GPU timestamp missing in {telemetry}")
        pixels = read_viewport_png(capture_image, expected_extent) if controls else read_pixels(capture_image)[..., ::-1]
        assert pixels.shape == (*expected_extent[::-1], 3), (telemetry, pixels.shape)
        frames[frame] = (pixels, state, {"image": str(capture_image), "telemetry": str(telemetry),
                                        "gpuPass": fog, "fog": settings})
    evidence = {"sequence": str(schedule), "log": str(log), "command": command,
                "frames": {frame: data[2] for frame, data in frames.items()}}
    return frames, evidence


def expected_foreground():
    import numpy as np

    y, x = np.mgrid[0:HEIGHT, 0:WIDTH] + .5
    slope = math.tan(math.radians(FOV) / 2)
    horizontal = (2 * x / WIDTH - 1) * slope * WIDTH / HEIGHT
    vertical = (1 - 2 * y / HEIGHT) * slope
    length = NEAR_DEPTH * np.sqrt(1 + horizontal**2 + vertical**2)
    opacity = -np.expm1(-DENSITY * length)[..., None]
    return opacity * (SUN * ALBEDO / (4 * math.pi)) * np.array(SUN_COLOR)


def evaluate_motion(active, reset, require):
    import numpy as np

    shadow = np.s_[92:108, 205:226]
    foreground = np.s_[92:108, 148:158]
    expected = expected_foreground()[foreground]
    expected_history = {1: False, 4: True, 5: True, 6: True, 7: True, 9: True, 10: False,
                        11: True, 12: False, 13: True}
    reference_start = reset[4][0]
    reference_end = reset[PAN_COMPLETE][0]
    shadow_contrast = float((reference_start[shadow] - reference_end[shadow]).mean())
    foreground_contrast = float((reference_start[foreground] - reference_end[foreground]).mean())
    require(shadow_contrast > .15, f"camera must pan into a shadow with strong contrast, observed {shadow_contrast}")
    require(foreground_contrast > .12, f"camera must pan onto the closer receiver, observed {foreground_contrast}")
    require(active[RESIZE][1]["volumetricFog"]["allocationBytes"] < active[CUT][1]["volumetricFog"]["allocationBytes"],
            "fog storage did not shrink with the viewport")
    comparisons = {}
    for frame, should_reproject in expected_history.items():
        pixels, state, _ = active[frame]
        fresh_pixels, fresh_state, _ = reset[frame]
        assert state["camera"] == fresh_state["camera"], (frame, state["camera"], fresh_state["camera"])
        require(state["volumetricFog"]["historyReprojected"] == should_reproject, f"unexpected history state at frame {frame}")
        require(not fresh_state["volumetricFog"]["historyReprojected"], f"forced reset reused history at frame {frame}")
        delta = pixels - fresh_pixels
        comparisons[frame] = {"maximumAbsoluteDelta": float(np.abs(delta).max())}
        if not should_reproject:
            require(comparisons[frame]["maximumAbsoluteDelta"] <= .001,
                    f"history reset frame {frame} differed from its cold reference")
        if PAN_COMPLETE <= frame < CUT:
            shadow_excess = np.maximum(delta[shadow], 0)
            shadow_delta = float(shadow_excess.mean())
            shadow_p95 = float(np.percentile(shadow_excess, 95))
            foreground_error = float(np.abs(pixels[foreground] - expected).max())
            comparisons[frame].update(shadowMeanPositiveDelta=shadow_delta, shadowP95PositiveDelta=shadow_p95,
                                      foregroundMaximumAbsoluteError=foreground_error)
            require(shadow_delta <= shadow_contrast * (.12 if frame == PAN_COMPLETE else .06),
                    f"lit fog trails the moving shadow at frame {frame} with excess {shadow_delta}")
            require(shadow_p95 <= shadow_contrast * .15,
                    f"lit fog leaves a localized trail at frame {frame} with excess {shadow_p95}")
            require(foreground_error <= .003, f"fog leaks behind the foreground at frame {frame} with error {foreground_error}")
    return {"shadowContrast": shadow_contrast, "foregroundContrast": foreground_contrast, "frames": comparisons}


def evaluate_silhouette(frame, require):
    pixels = frame[0]
    dark = float(pixels[96:104, 134:142].mean())
    covered_edge = float(pixels[96:104, 157:160].mean())
    lit = float(pixels[96:104, 177:190].mean())
    require(lit - dark > .2, f"silhouette shadow must have strong contrast, observed {lit-dark}")
    require(dark < .03, f"left receiver interior should be in full shadow, observed {dark}")
    require(covered_edge - dark <= .035, f"foreground silhouette receives excess fog {covered_edge-dark}")
    return {"sameDepthDarkMean": dark, "coveredEdgeMean": covered_edge, "brightBackgroundMean": lit}


def evaluate_controls(frames, require):
    import numpy as np

    history = [frames[frame][2]["fog"].get("historyReprojected", False) for frame in frames]
    require(history == [False, False, True, False, True, False, True, False, False],
            f"changing or disabling the preset must reject history, observed {history}")
    off_difference = float(np.abs(frames[1][0] - frames[9][0]).max())
    require(off_difference == 0, f"turning fog off changed scene color by {off_difference}")
    require(float(frames[3][0][92:108, 176:202].mean()) > .2, "the Low preset did not add visible scattering")
    require(float(frames[5][0][92:108, 176:202].mean()) > .2, "the High preset did not add visible scattering")

    diagnostic = frames[7][0]
    grid_edge = diagnostic[98:102, 143:145].mean(axis=(0, 1))
    near_band = diagnostic[98:102, 147:149].mean(axis=(0, 1))
    rear_band = diagnostic[98:102, 171:173].mean(axis=(0, 1))
    require(grid_edge[1] - near_band[1] > .25, "the froxel diagnostic does not show cyan column boundaries")
    require(rear_band[0] - near_band[0] > .04 and near_band[2] - rear_band[2] > .02,
            "the froxel diagnostic did not distinguish the near and rear depth slices")
    return {"historyReprojected": history, "offMaximumDifference": off_difference, "gridEdgeRgb": grid_edge.tolist(),
            "nearBandRgb": near_band.tolist(), "rearBandRgb": rear_band.tolist()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--technique", choices=TECHNIQUES)
    parser.add_argument("--quality", choices=QUALITIES)
    parser.add_argument("--with-controls", action="store_true", help="Exercise runtime preset switches and the froxel view")
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--managed-output", action="store_true", help="Keep each run in a fresh output subdirectory")
    parser.add_argument("--require-opt-in", action="store_true", help=f"Require {OPT_IN}=1 for native runs")
    args = parser.parse_args()
    if args.require_opt_in and not args.generate_only and os.environ.get(OPT_IN) != "1":
        print(f"Set {OPT_IN}=1 to run native volumetric fog validation.", flush=True)
        return 77
    renderer, output = args.renderer.resolve(), args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    else:
        output.mkdir(parents=True, exist_ok=False)
    print(f"Volumetric fog temporal fixtures and captures {output}", flush=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    motion = make_scene(output / "motion.gltf")
    silhouette = make_scene(output / "silhouette.gltf", silhouette=True)
    if args.generate_only:
        return 0

    results, failures, controls = {}, [], {}
    for technique in ((args.technique,) if args.technique else TECHNIQUES):
        for quality in ((args.quality,) if args.quality else QUALITIES):
            label = f"{technique}-{quality}"
            print(f"Capturing {label}", flush=True)

            def require(condition, message):
                if not condition:
                    failures.append(f"{label}: {message}")

            active, active_artifacts = capture(renderer, output, f"{label}-history", motion, technique, quality,
                                               motion_sequence(), (1, 0, 0))
            reset, reset_artifacts = capture(renderer, output, f"{label}-reset", motion, technique, quality,
                                             motion_sequence(reset=True), (1, 0, 0))
            edge, edge_artifacts = capture(renderer, output, f"{label}-silhouette", silhouette, technique, quality,
                                           silhouette_sequence(), (0, 1, 0))
            stable = [data[2] for frame, data in active.items() if frame < RESIZE and data[2]["gpuPass"]["gpuTimed"]]
            timings = [entry["gpuPass"]["gpuMs"] for entry in stable]
            results[label] = {
                "device": active[1][1]["gpu"],
                "motion": evaluate_motion(active, reset, require),
                "silhouette": evaluate_silhouette(edge[1], require),
                "gpuMetrics": {"framesTimed": len(timings), "minMs": min(timings), "medianMs": median(timings),
                               "maxMs": max(timings), "allocationBytes": active[1][1]["volumetricFog"]["allocationBytes"],
                               "resizedAllocationBytes": active[RESIZE][1]["volumetricFog"]["allocationBytes"]},
                "captures": {"history": active_artifacts, "reset": reset_artifacts, "silhouette": edge_artifacts},
            }
        if args.with_controls:
            label = f"{technique}-controls"
            print(f"Capturing {label}", flush=True)
            frames, artifacts = capture(renderer, output, label, motion, technique, "off",
                                        controls_sequence(), (1, 0, 0), controls=True)
            controls[technique] = {"metrics": evaluate_controls(frames, require), "captures": artifacts}
    report = {"passed": not failures, "failures": failures, "results": results, "controls": controls}
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for failure in failures:
        print(failure, file=sys.stderr)
    if failures:
        return 1
    print("PASS fog history, shadow motion, foreground occlusion, cut, resize, and GPU telemetry")
    return 0


if __name__ == "__main__":
    sys.exit(main())
