"""Opt-in GPU regression for scene lighting defaults and provider switches."""

import argparse
import json
import os
from pathlib import Path
import subprocess

try:
    import numpy as np
    from PIL import Image
except ImportError:
    print("SKIP: scene lighting regression requires Pillow and NumPy")
    raise SystemExit(77)


def linear(path):
    with Image.open(path) as image:
        rgb = np.asarray(image.convert("RGB"), dtype=np.float32) / 255.0
    return np.where(rgb <= .04045, rgb / 12.92, ((rgb + .055) / 1.055) ** 2.4)


def capture(exe, output, name, model, sequence, technique, extra=()):
    command = [str(exe), "--model", str(model), "--hidden", "--no-ui",
               "--validation", "--no-taa", "--msaa", "1", "--width", "640",
               "--height", "360", "--display-mode", "lit", "--no-bloom",
               "--render-technique", technique, "--fixed-dt", "0.016666667",
               "--exposure", ".25", "--capture-sequence", str(sequence),
               "--screenshot", str(output / (name + ".png")), *extra]
    logfile = output / (name + ".log")
    with logfile.open("w", encoding="utf-8") as log:
        result = subprocess.run(command, cwd=output,
                                env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                                stdout=log, stderr=subprocess.STDOUT, timeout=180)
    logtext = logfile.read_text(encoding="utf-8", errors="replace")
    if result.returncode or "VUID-" in logtext or "SYNC-HAZARD" in logtext:
        raise RuntimeError(f"Capture failed: {logfile}")
    files = sorted(output.glob(name + ".frame-*.png")) + [output / (name + ".png")]
    return [(path, json.loads(path.with_suffix(".telemetry.json").read_text()))
            for path in files]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if os.environ.get("CONTAINER_RUN_GPU_SCENE_LIGHTING") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_SCENE_LIGHTING=1 to run GPU captures")
        return 77
    exe, output = args.exe.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "vk_layer_settings.txt").write_text(
        "khronos_validation.validate_sync = true\n", encoding="utf-8")
    models = exe.parent / "models/validation"
    cornell, bim = models / "cornell_box_local_light.gltf", models / "taa_equivalent.bim"
    camera = [{"frame": 1, "position": [0, 1.8, 6.5], "target": [0, .8, 0]}]
    switch = {"schemaVersion": 1, "frames": 36, "sampleFrames": [8, 22, 36],
              "camera": camera, "events": [{"frame": 12, "reload": str(bim)},
                                            {"frame": 26, "reload": str(cornell)}]}
    direct = {"schemaVersion": 1, "frames": 22, "sampleFrames": [22], "camera": camera}
    switch_path, direct_path = output / "switch.json", output / "direct.json"
    switch_path.write_text(json.dumps(switch), encoding="utf-8")
    direct_path.write_text(json.dumps(direct), encoding="utf-8")
    results = {}
    for technique in ["deferred-raster", "forward-raster"]:
        prefix = technique.split("-")[0]
        roundtrip = capture(exe, output, prefix + "-switch", cornell, switch_path, technique)
        assert len(roundtrip) == 3, "Missing scene-switch captures"
        for index in [0, 2]:
            lighting = roundtrip[index][1]["lighting"]
            assert lighting["directionalIntensity"] == lighting["environmentIntensity"] == 0
            assert lighting["bounceIntensity"] == 0, "Artistic fill masks direct shadows"
            assert lighting["areaLightCount"] == 1
            assert lighting["localShadowLayerBudget"] == 24
            assert lighting["activeLocalShadowLayers"] == 24, "Area visibility samples missing"
        switched_path, switched_telemetry = roundtrip[1]
        lighting = switched_telemetry["lighting"]
        assert lighting["directionalIntensity"] == 2 and lighting["environmentIntensity"] == 1
        assert lighting["areaLightCount"] == lighting["pointLightCount"] == 0
        assert lighting["activeLocalShadowLayers"] == 0
        reference = capture(exe, output, prefix + "-direct", bim, direct_path, technique)[-1][0]
        switched, expected = linear(switched_path), linear(reference)
        mae = float(np.mean(np.abs(switched - expected)))
        mean = float(switched[90:250, 220:420].mean())
        assert mae < .002, f"Scene switch changed illumination: MAE {mae}"
        assert mean > .02, f"BIM surfaces are nearly black: {mean}"
        locked = capture(exe, output, prefix + "-locked", cornell, switch_path, technique,
                         ["--directional-intensity", "0", "--environment-intensity", "0"])
        assert all(item[1]["lighting"]["directionalIntensity"] == 0 and
                   item[1]["lighting"]["environmentIntensity"] == 0 for item in locked)
        mixed = capture(exe, output, prefix + "-mixed", cornell, direct_path, technique,
                        ["--bim-model", str(bim)])[-1][1]["lighting"]
        assert mixed["directionalIntensity"] == 2 and mixed["environmentIntensity"] == 1
        yellow = capture(exe, output, prefix + "-yellow", models / "cornell_box_yellow_area_light.gltf",
                         direct_path, technique)[-1][1]["lighting"]
        assert yellow["directionalIntensity"] == yellow["environmentIntensity"] == 0
        assert yellow["bounceIntensity"] == 0 and yellow["activeLocalShadowLayers"] == 24
        results[prefix] = {"switchVsDirectLinearMae": mae, "bimSurfaceLinearMean": mean,
                           "explicitZeroPreserved": True, "mixedProvidersIlluminated": True,
                           "yellowCornellProfileApplied": True,
                           "synchronizationValidationClean": True}
        print(f"{prefix}: scene defaults, round trip, explicit zero and mixed providers passed",
              flush=True)
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n",
                                         encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
