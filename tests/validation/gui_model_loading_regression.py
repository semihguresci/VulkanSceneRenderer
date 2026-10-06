"""Exercise Scene Controls model requests with an active ImGui frame lifecycle.

Uses the same sample-selection and load-request methods as the widgets; it does
not simulate OS mouse clicks. Real buildingSMART assets are staged by CMake.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess

try:
    import numpy as np
    from PIL import Image
except ImportError:
    print("SKIP: GUI model regression requires Pillow and NumPy")
    raise SystemExit(77)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if os.environ.get("CONTAINER_RUN_GPU_GUI_MODEL_LOADING") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_GUI_MODEL_LOADING=1")
        return 77
    exe, output = args.exe.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    models = exe.parent / "models"
    examples = models / "buildingSMART-IFC5-development/examples"
    wall = examples / "Hello Wall/hello-wall.ifc"
    tekla = examples / "Tekla House/TeklaHouse.ifcx"
    if not wall.is_file() or not tekla.is_file():
        print("SKIP: buildingSMART Hello Wall and Tekla House assets unavailable")
        return 77
    cornell = models / "validation/cornell_box_local_light.gltf"
    missing = output / "missing.ifcx"
    malformed = output / "malformed.ifcx"
    malformed.write_text("{broken JSON", encoding="utf-8")
    (output / "vk_layer_settings.txt").write_text(
        "khronos_validation.validate_sync = true\n", encoding="utf-8")
    # Collapse ordinary panels to keep the model visible. Rendering/UI remain on.
    (output / "imgui.ini").write_text(
        "[Window][Scene Controls]\nPos=0,0\nSize=350,700\nCollapsed=1\n\n"
        "[Window][Renderer Telemetry]\nPos=0,25\nSize=350,700\nCollapsed=1\n",
        encoding="utf-8")

    def capture(name, technique, source, sequence, gui=True):
        script = output / (name + ".json")
        script.write_text(json.dumps(sequence), encoding="utf-8")
        screenshot = output / (name + ".png")
        # Do not allow stale samples to satisfy the assertions after a crash.
        for stale in [screenshot, *output.glob(name + ".frame-*.png")]:
            stale.unlink(missing_ok=True)
        command = [str(exe), "--model", str(source), "--hidden", "--validation",
                   "--no-ray-query", "--no-taa", "--msaa", "1", "--width", "960",
                   "--height", "540", "--display-mode", "lit", "--exposure", ".25",
                   "--fixed-dt", "0.016666667", "--render-technique", technique,
                   "--capture-sequence", str(script), "--screenshot", str(screenshot)]
        if not gui:
            command.append("--no-ui")
        logfile = output / (name + ".log")
        with logfile.open("w", encoding="utf-8") as log:
            result = subprocess.run(command, cwd=output, stdout=log,
                                    stderr=subprocess.STDOUT, timeout=240,
                                    env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)))
        logtext = logfile.read_text(encoding="utf-8", errors="replace")
        assert result.returncode == 0, f"App failed: {logfile} ({result.returncode})"
        assert "VUID-" not in logtext and "SYNC-HAZARD" not in logtext, logfile
        files = sorted(output.glob(name + ".frame-*.png")) + [screenshot]
        return [(path, json.loads(path.with_suffix(".telemetry.json").read_text()))
                for path in files]

    sample_frames = [8, 20, 32, 44, 56, 68, 80, 92, 104]
    sequence = {"schemaVersion": 1, "frames": 104, "sampleFrames": sample_frames,
                "events": [
                    {"frame": 12, "guiSampleModel": "IFC5 / Hello Wall / hello-wall (STEP)"},
                    {"frame": 24, "guiSampleModel": "IFC5 / Hello Wall / hello-wall"},
                    {"frame": 36, "guiSampleModel": "IFC5 / Hello Wall / hello-wall-add-fire-rating-60"},
                    {"frame": 48, "guiReload": str(missing)},
                    {"frame": 60, "guiReload": str(malformed)},
                    {"frame": 72, "guiSampleModel": "IFC5 / Tekla House / TeklaHouse"},
                    {"frame": 84, "guiSampleModel": "IFC5 / Tekla House / TeklaHouse (STEP)"},
                    {"frame": 96, "guiReload": str(cornell)}]}
    results = {}
    for technique in ["deferred-raster", "forward-raster"]:
        prefix = technique.split("-")[0]
        samples = capture(prefix + "-ui", technique, cornell, sequence)
        assert len(samples) == len(sample_frames), "Missing GUI load captures"
        for index in [0, 8]:
            lighting = samples[index][1]["lighting"]
            assert lighting["directionalIntensity"] == lighting["environmentIntensity"] == 0
            assert lighting["areaLightCount"] == 1
        for index in range(1, 8):
            telemetry = samples[index][1]
            assert telemetry["gui"]["displayMode"] == 0, "UI ignored --display-mode lit"
            assert telemetry["scene"]["auxiliaryObjectCount"] > 0, "BIM scene absent"
            assert telemetry["scene"]["primary"] == "", "Old glTF scene retained"
            lighting = telemetry["lighting"]
            assert lighting["directionalIntensity"] == 2, (prefix, sample_frames[index], lighting)
            assert lighting["environmentIntensity"] == 1, (prefix, sample_frames[index], lighting)
            assert lighting["areaLightCount"] == lighting["pointLightCount"] == 0
        for index, expected in [(1, wall), (2, wall.with_suffix(".ifcx")),
                                (3, examples / "Hello Wall/hello-wall-add-fire-rating-60.ifcx"),
                                (6, tekla), (7, tekla.with_suffix(".ifc"))]:
            assert Path(samples[index][1]["scene"]["auxiliary"]) == expected
            assert samples[index][1]["gui"]["status"].startswith("Loaded model:")
        for index in [4, 5]:
            status = samples[index][1]["gui"]["status"]
            assert status.startswith("Failed to load model:") and "\n" in status, status
            assert samples[index][1]["scene"]["auxiliary"] == samples[3][1]["scene"]["auxiliary"]
            assert samples[index][1]["scene"]["auxiliaryObjectCount"] == samples[3][1]["scene"]["auxiliaryObjectCount"]
        assert "failed to open IFCX file" in samples[4][1]["gui"]["status"]
        assert "parse error" in samples[5][1]["gui"]["status"]
        assert samples[1][1]["scene"]["import"]["importedProducts"] == 4
        assert samples[7][1]["scene"]["import"]["importedProducts"] == 10042
        # Compare illumination and framing against direct startup without UI.
        reference = capture(prefix + "-direct", technique, wall,
                            {"schemaVersion": 1, "frames": 20}, gui=False)[0]
        assert samples[1][1]["camera"]["position"] == reference[1]["camera"]["position"]
        camera = samples[1][1]["camera"]["position"]
        assert camera[0] > 10 or camera[1] > 3.1 or camera[2] < -5.1 or camera[2] > .1, (
            "Hello Wall starts inside its bounds", camera)
        def pixels(path):
            with Image.open(path) as image:
                # Exclude the left tool palette, top/right navigation HUD and
                # collapsed panels while retaining the center of the model.
                return np.asarray(image.convert("RGB"), dtype=np.float32)[70:440, 370:755] / 255
        mae = float(np.abs(pixels(samples[1][0]) - pixels(reference[0])).mean())
        assert mae < .003, f"UI reload differs from direct IFC startup: {mae}"
        recovery_mae = max(float(np.abs(pixels(samples[index][0]) -
                                        pixels(samples[3][0])).mean()) for index in [4, 5])
        assert recovery_mae < .001, f"Failed load changed the restored scene: {recovery_mae}"
        classic = models / "buildingSMART-Sample-Test-Files/IFC 4.0.2.1 (IFC 4)/ISO Spec archive/basin-tessellation.ifc"
        assert classic.is_file() and not classic.read_bytes().startswith(b"version https://git-lfs"), (
            "Classic IFC asset is missing or still an LFS pointer", classic)
        classic_sample = capture(prefix + "-classic", technique, cornell,
                                 {"schemaVersion": 1, "frames": 20, "events": [{"frame": 12,
                                  "guiSampleModel": "IFC / IFC 4.0.2.1 (IFC 4) / ISO Spec archive / basin-tessellation (STEP)"}]})[0][1]
        assert Path(classic_sample["scene"]["auxiliary"]) == classic
        assert classic_sample["scene"]["auxiliaryObjectCount"] > 0
        assert classic_sample["gui"]["status"].startswith("Loaded model:")
        assert classic_sample["lighting"]["directionalIntensity"] == 2
        assert classic_sample["lighting"]["environmentIntensity"] == 1
        results[prefix] = {"captures": len(samples), "ifcStartupVsUiRgbMae": mae,
                           "failedLoadRecoveryRgbMae": recovery_mae,
                           "nativeIfcProducts": [4, 10042], "failedLoadsRestoreScene": True,
                           "detailedErrorsPreserved": True, "validationClean": True}
        results[prefix]["classicIfcSampleLoaded"] = True
        print(f"{prefix}: IFC/IFCX sample selection, layers, reload recovery and lighting passed",
              flush=True)
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n",
                                         encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
