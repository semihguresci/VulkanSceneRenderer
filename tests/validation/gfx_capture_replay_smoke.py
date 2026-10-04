"""Opt-in capture/replay of warmed-up TAA history, compared to the original PNG."""
from __future__ import annotations
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
LAUNCHER = ROOT / "tools/gfxreconstruct.py"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--tools")
    parser.add_argument("--matrix", action="store_true", help="Include both techniques/AA, BIM, mixed scenes and lifecycle events")
    parser.add_argument("--memory-mode", choices=("page_guard", "unassisted"), default="page_guard")
    parser.add_argument("--case", action="append", help="Run only named matrix cases (requires --matrix)")
    args = parser.parse_args()
    if args.case and not args.matrix:
        parser.error("--case requires --matrix")
    if os.environ.get("CONTAINER_RUN_GPU_GFXRECON") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_GFXRECON=1 to enable Vulkan capture/replay")
        return 77
    try:
        from PIL import Image, ImageChops, ImageStat
        spec = importlib.util.spec_from_file_location("gfx_launcher", LAUNCHER)
        gfx = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gfx)
        _, _, version = gfx.discover_tools(args.tools)
    except (ImportError, ValueError, OSError) as error:
        print(f"SKIP: capture tools/Pillow unavailable: {error}")
        return 77
    exe = Path(args.exe).resolve()
    if not exe.is_file():
        print("Renderer executable missing", file=sys.stderr)
        return 1
    output = Path(args.output).resolve() / time.strftime("%Y%m%d-%H%M%S")
    output.mkdir(parents=True, exist_ok=False)
    tools = ["--tools", args.tools] if args.tools else []
    cases = [("deferred-taa", ["--taa"], False)]
    if args.matrix:
        cases += [("deferred-native", ["--no-taa"], False),
                  ("forward-taa", ["--render-technique", "forward-raster", "--taa"], False),
                  ("forward-native", ["--render-technique", "forward-raster", "--no-taa"], False),
                  ("deferred-msaa", ["--no-taa", "--msaa", "4"], False),
                  ("forward-msaa", ["--render-technique", "forward-raster", "--no-taa", "--msaa", "4"], False),
                  ("bim-taa", ["--taa", "--model", "models/validation/taa_equivalent.bim"], False),
                  ("mixed-taa", ["--taa", "--model", "models/validation/taa_scene.gltf",
                                 "--bim-model", "models/validation/taa_equivalent.bim"], False),
                  ("deferred-textures", ["--taa", "--model", "models/basic_cube.gltf"], False),
                  ("forward-textures", ["--render-technique", "forward-raster", "--taa",
                                        "--model", "models/basic_cube.gltf"], False),
                  ("lifecycle", ["--taa"], True)]
    if args.case:
        unknown = set(args.case) - {name for name, _, _ in cases}
        if unknown:
            raise ValueError(f"Unknown selected case(s): {sorted(unknown)}")
        cases = [case for case in cases if case[0] in args.case]
    results = []
    for name, extra, lifecycle in cases:
        folder = output / name
        folder.mkdir()
        original = folder / "original.png"
        selected, first, expected_presents, samples = "9", 9, 9, [9]
        if "--msaa" in extra:
            # 1.0.5's state-snapshot copy shader treats multisampled UINT IDs as
            # FLOAT and its snapshot barriers are invalid. Startup capture avoids
            # these injected operations while retaining the renderer's MSAA path.
            selected, first = "1-9", 1
        sequence_args = []
        if lifecycle:
            selected, first, expected_presents, samples = "6-17", 6, 17, [9, 12, 14, 18]
            sequence = {"schemaVersion": 1, "frames": 18, "sampleFrames": samples,
                        "camera": [{"frame": 1, "position": [0, 1.2, 4.2], "target": [0, 1, 0]},
                                   {"frame": 18, "position": [0.3, 1.2, 4.2], "target": [0, 1, 0]}],
                        "events": [{"frame": 6, "skip": True}, {"frame": 7, "acquireOutOfDate": True},
                                   {"frame": 8, "presentSuboptimal": True}, {"frame": 10, "reset": True},
                                   {"frame": 13, "resize": [360, 256]},
                                   {"frame": 16, "reload": "models/validation/taa_scene.gltf"}]}
            sequence_path = folder / "sequence.json"
            sequence_path.write_text(json.dumps(sequence))
            sequence_args = ["--capture-sequence", str(sequence_path)]
        capture_dir = folder / "capture"
        capture_process = subprocess.run([sys.executable, str(LAUNCHER), "capture", "--exe", str(exe), *tools,
                        "--output-dir", str(capture_dir), "--frames", selected, "--memory-mode", args.memory_mode, "--",
                        "--hidden", "--no-ui", "--msaa", "1", "--validation", "--width", "320", "--height", "240",
                        "--fixed-dt", "0.016666667", "--screenshot", str(original), "--capture-frame", "9",
                        *sequence_args, *extra], timeout=180)
        if capture_process.returncode:
            renderer_log = capture_dir / "renderer.log"
            unavailable = ("failed to find GPUs with Vulkan support!", "Vulkan 1.4 loader required;",
                           "Vulkan 1.4 GPU with dynamic rendering, synchronization2, descriptor indexing, and indirect counts required")
            if (not (capture_dir / "runtime.json").exists() and renderer_log.is_file() and
                    any(message in renderer_log.read_text(errors="replace") for message in unavailable)):
                print(f"SKIP: required Vulkan runtime/GPU unavailable; see {renderer_log}")
                return 77
            capture_process.check_returncode()
        captures = list(capture_dir.glob("*.gfxr"))
        if len(captures) != 1:
            raise RuntimeError(f"Expected exactly one trim; found {len(captures)}")
        journal = [json.loads(line) for line in (capture_dir / "frames.jsonl").read_text().splitlines()]
        presents = [entry for entry in journal if entry["type"] == "present"]
        if len(presents) != expected_presents:
            raise RuntimeError(f"Unexpected number of presents: {len(presents)} != {expected_presents}")
        relative = []
        for tick in samples:
            boundary = next(entry for entry in presents if entry["tick"] == tick)
            if boundary["telemetry"]["taa"]["submittedFrame"] != boundary["successfulSubmissions"]:
                raise RuntimeError("TAA/journal submission identities diverged")
            relative.append(boundary["presentCalls"] - first + 1)
        if lifecycle:
            if not any(e["type"] == "tick" and e["skipped"] for e in journal) or not any(e["type"] == "acquireFailed" for e in journal):
                raise RuntimeError("Lifecycle skip/acquisition retry was not recorded")
            if len({e["telemetry"]["taa"]["epoch"] for e in presents}) < 2:
                raise RuntimeError("TAA lifecycle did not invalidate history")
        replay_dir = folder / "replay"
        subprocess.run([sys.executable, str(LAUNCHER), "replay", str(captures[0]), *tools,
                        "--output-dir", str(replay_dir), "--screenshots", ",".join(map(str, relative)),
                        "--offscreen", "--validation"], check=True, timeout=180)
        images = sorted(replay_dir.glob("*.png"), key=lambda p: int(re.search(r"frame_?(\d+)", p.name).group(1)))
        if len(images) != len(samples):
            raise RuntimeError("Replay did not produce the requested screenshots")
        comparisons = []
        for tick, image in zip(samples, images):
            source_path = original if tick == (18 if lifecycle else 9) else original.with_name(f"original.frame-{tick:04d}.png")
            source = Image.open(source_path).convert("RGB")
            target = Image.open(image).convert("RGB")
            if source.size != target.size:
                raise RuntimeError("Replay image dimensions differ from the original")
            delta = ImageChops.difference(source, target)
            mae = sum(ImageStat.Stat(delta).mean) / 3 / 255
            maximum = max(channel[1] for channel in delta.getextrema()) / 255
            comparisons.append({"tick": tick, "normalizedMeanAbsoluteError": mae, "normalizedMaxError": maximum,
                                "passed": mae <= 1 / 255 and maximum <= 4 / 255})
        for log in (capture_dir / "renderer.log", capture_dir / "layer.log", replay_dir / "replay.log"):
            text = log.read_text(errors="replace")
            if re.search(r"Validation Error|VUID-|\[gfxrecon\].*(?:ERROR|FATAL)", text, re.IGNORECASE):
                raise RuntimeError(f"Validation/capture errors in {log}")
        result = {"case": name, "toolVersion": version, "memoryMode": args.memory_mode, "comparisons": comparisons,
                  "captureBytes": captures[0].stat().st_size, "presentCalls": len(presents),
                  "passed": all(c["passed"] for c in comparisons)}
        (folder / "comparison.json").write_text(json.dumps(result, indent=2))
        print(json.dumps(result), flush=True)
        results.append(result)
    (output / "comparison.json").write_text(json.dumps(results, indent=2))
    return 0 if all(r["passed"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
