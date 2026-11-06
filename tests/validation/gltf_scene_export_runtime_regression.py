"""Native primary-scene export/reload validation with matched viewer settings."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess

import numpy as np
from PIL import Image


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    renderer = args.renderer.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    repository = Path(__file__).resolve().parents[2]
    # Use the same neutral filename for the source and exported asset. The
    # bundled Cornell filename activates a diagnostic viewer lighting preset;
    # viewer settings are intentionally outside glTF scene serialization.
    source = output / "source.gltf"
    source.write_bytes((repository / "models/validation/cornell_box_local_light.gltf").read_bytes())
    common = ["--hidden", "--no-ui", "--validation", "--width", "960", "--height", "540",
              "--msaa", "1", "--warmup-frames", "8", "--capture-frame", "9",
              "--display-mode", "lit", "--no-bloom", "--environment-intensity", "0",
              "--directional-intensity", "0", "--exposure", "1"]
    runs = [("source", source, "deferred-raster", output / "scene.glb"),
            ("glb", output / "scene.glb", "deferred-raster", output / "scene.gltf"),
            ("gltf", output / "scene.gltf", "deferred-raster", None),
            ("forward", output / "scene.gltf", "forward-raster", None)]
    results = []
    for name, model, technique, destination in runs:
        command = [str(renderer), *common, "--model", str(model), "--render-technique", technique,
                   "--screenshot", str(output / f"{name}.png")]
        if destination:
            command.extend(["--save-scene", str(destination)])
        process = subprocess.run(command, cwd=renderer.parent, capture_output=True, text=True,
                                 creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
                                 timeout=120)
        (output / f"{name}.stdout.txt").write_text(process.stdout, encoding="utf-8")
        (output / f"{name}.stderr.txt").write_text(process.stderr, encoding="utf-8")
        result = {"name": name, "command": command, "exitCode": process.returncode}
        results.append(result)
        if process.returncode or "VUID-" in process.stderr or "SYNC-HAZARD" in process.stderr:
            raise RuntimeError(f"Native export run failed: {name}; see {output}")
        if not (output / f"{name}.png").is_file():
            raise RuntimeError(f"Missing native capture: {name}")
    baseline = np.asarray(Image.open(output / "source.png").convert("RGB"), dtype=np.int16)
    for result in results[1:3]:
        pixels = np.asarray(Image.open(output / f"{result['name']}.png").convert("RGB"), dtype=np.int16)
        difference = np.abs(pixels - baseline)
        result["meanAbsolutePixelError"] = float(difference.mean())
        result["percentile99_9PixelError"] = float(np.percentile(difference, 99.9))
        result["passed"] = result["meanAbsolutePixelError"] <= .1 and result["percentile99_9PixelError"] <= 2
    (output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    if not all(result.get("passed", True) for result in results):
        raise RuntimeError(f"Exported appearance differs; see {output / 'results.json'}")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
