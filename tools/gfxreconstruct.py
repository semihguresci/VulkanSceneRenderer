#!/usr/bin/env python3
"""Optional Vulkan capture/replay launcher. Only the child receives layer settings."""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time
import uuid

LAYER = "VK_LAYER_LUNARG_gfxreconstruct"
HOTKEYS = {f"F{i}" for i in range(1, 13)}  # TAB/CONTROL conflict with navigation/UI.
HOTKEYS.remove("F8")  # Renderer freeze-culling shortcut.


def frame_ranges(value: str) -> int:
    previous = 0
    for item in value.split(","):
        if not re.fullmatch(r"[0-9]+(?:-[0-9]+)?", item):
            raise ValueError("Frames must be ascending positive ranges, e.g. 9-12,20")
        pair = item.split("-")
        first, last = int(pair[0]), int(pair[-1])
        if first <= previous or last < first or last > 0xFFFFFFFF:
            raise ValueError("Frame ranges must be 1-based, ascending, nonoverlapping uint32 values")
        previous = last
    return previous


def architecture(path: Path) -> str:
    with path.open("rb") as stream:
        header = stream.read(64)
        if header[:2] == b"MZ" and len(header) == 64:
            stream.seek(struct.unpack_from("<I", header, 60)[0])
            pe = stream.read(6)
            if pe[:4] == b"PE\0\0":
                return {0x8664: "x64", 0xAA64: "arm64", 0x14C: "x86"}.get(struct.unpack_from("<H", pe, 4)[0], "unknown")
        if header[:4] == b"\x7fELF" and len(header) >= 20:
            order = "<" if header[5] == 1 else ">"
            return {62: "x64", 183: "arm64", 3: "x86"}.get(struct.unpack_from(order + "H", header, 18)[0], "unknown")
    raise ValueError(f"Not a supported PE/ELF executable or library: {path}")


def tool_environment(environment: dict[str, str] | None = None) -> dict[str, str]:
    env = dict(os.environ if environment is None else environment)
    for key in list(env):
        if key.startswith("GFXRECON_"):
            del env[key]
    # Avoid recording the inspection/replay tools themselves with inherited layers.
    env["GFXRECON_DISABLE"] = "1"
    return env


def run_text(command: list[str]) -> str:
    result = subprocess.run(command, env=tool_environment(), stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace", timeout=30)
    if result.returncode:
        raise ValueError(f"Tool failed ({result.returncode}): {' '.join(command)}\n{result.stdout}")
    return result.stdout


def discover_tools(override: str | None) -> tuple[Path, dict, str]:
    suffix = ".exe" if os.name == "nt" else ""
    candidates = []
    if override:
        root = Path(override).expanduser().resolve()
        candidates = [root, root / "Bin", root / "bin"]
    else:
        sdk = os.environ.get("VULKAN_SDK")
        if sdk:
            candidates += [Path(sdk) / "Bin", Path(sdk) / "bin"]
        found = shutil.which("gfxrecon-info" + suffix)
        if found:
            candidates.append(Path(found).parent)
    for directory in candidates:
        if not all((directory / (name + suffix)).is_file() for name in ("gfxrecon-info", "gfxrecon-replay")):
            continue
        manifests = [directory / "VkLayer_gfxreconstruct.json",
                     directory.parent / "share/vulkan/explicit_layer.d/VkLayer_gfxreconstruct.json"]
        manifest_path = next((p for p in manifests if p.is_file()), None)
        if not manifest_path:
            continue
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        layer = manifest["layer"]
        if layer["name"] != LAYER:
            raise ValueError(f"Unexpected layer name in {manifest_path}")
        library = (manifest_path.parent / layer["library_path"]).resolve()
        if not library.is_file():
            raise ValueError(f"Capture layer library missing: {library}")
        machine = architecture(library)
        for name in ("gfxrecon-info", "gfxrecon-replay"):
            if architecture(directory / (name + suffix)) != machine:
                raise ValueError("GFXReconstruct tools and capture layer architectures do not match")
        versions = [run_text([str(directory / (name + suffix)), "--version"]) for name in ("gfxrecon-info", "gfxrecon-replay")]
        matches = [re.search(r"GFXReconstruct Version\s+(\d+\.\d+\.\d+)", v) for v in versions]
        if not all(matches) or matches[0].group(1) != matches[1].group(1):
            raise ValueError("Cannot establish matching GFXReconstruct info/replay versions")
        version = matches[0].group(1)
        if not re.search(r"\b" + re.escape(version) + r"\b", layer.get("description", "")):
            raise ValueError("GFXReconstruct layer and tools versions do not match")
        def setting_keys(value):
            if isinstance(value, dict):
                if "key" in value:
                    yield value["key"]
                for child in value.values():
                    yield from setting_keys(child)
            elif isinstance(value, list):
                for child in value:
                    yield from setting_keys(child)
        metadata = {key: layer[key] for key in ("name", "api_version", "implementation_version", "description")}
        metadata.update(manifestPath=str(manifest_path.resolve()), libraryPath=str(library),
                        machine=machine, supportedSettings=sorted(set(setting_keys(layer))))
        return directory.resolve(), metadata, version
    raise ValueError("GFXReconstruct tools/layer not found. Install the Vulkan SDK with GFXReconstruct, or pass --tools <SDK Bin or installed tool directory>.")


def managed_environment(args, directory: Path) -> dict[str, str]:
    return {
        "GFXRECON_CAPTURE_FILE": str(directory / (args.capture_name + ".gfxr")),
        "GFXRECON_CAPTURE_FRAMES": args.frames or "",
        "GFXRECON_CAPTURE_TRIGGER": args.trigger or "",
        "GFXRECON_CAPTURE_TRIGGER_FRAMES": str(args.trigger_frames or ""),
        "GFXRECON_CAPTURE_QUEUE_SUBMITS": "",
        "GFXRECON_CAPTURE_USE_ASSET_FILE": "false",
        "GFXRECON_CAPTURE_FILE_TIMESTAMP": "false",
        "GFXRECON_QUIT_AFTER_CAPTURE_FRAMES": "false",  # Let the renderer finish its boundary/journal.
        "GFXRECON_CAPTURE_COMPRESSION_TYPE": args.compression,
        "GFXRECON_MEMORY_TRACKING_MODE": args.memory_mode,
        "GFXRECON_CAPTURE_FILE_FLUSH": "true",
        "GFXRECON_LOG_LEVEL": args.log_level,
        "GFXRECON_LOG_FILE": str(directory / "layer.log"),
        "GFXRECON_LOG_FILE_FLUSH_AFTER_WRITE": "true",
        "GFXRECON_CAPTURE_PROCESS_NAME": Path(args.exe).name,
    }


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: Path, value) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def process_exit(code: int) -> int:
    # Preserve native codes in sidecars, but use a portable Python exit status.
    # Windows access violations can exceed sys.exit's supported integer range.
    return code if 0 <= code < 256 else 1


def capture(args) -> int:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}", args.capture_name) or args.capture_name.endswith("."):
        raise ValueError("--capture-name must be a filename stem of 1-128 letters, digits, dots, underscores or hyphens")
    if re.fullmatch(r"(?i)(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?", args.capture_name):
        raise ValueError("--capture-name cannot use a reserved Windows device name")
    if args.trigger is not None and not args.trigger:
        raise ValueError("Capture hotkey cannot be empty")
    if args.frames is not None:
        stop = frame_ranges(args.frames)
    else:
        stop = 0
    if not args.frames and not args.all_frames and not args.trigger:
        args.trigger = "F12"
    if args.trigger and args.trigger not in HOTKEYS:
        raise ValueError("Use F1-F12 except F8; TAB/CONTROL/F8 conflict with renderer controls")
    if args.trigger_frames is not None and (not args.trigger or not 1 <= args.trigger_frames <= 0xFFFFFFFF):
        raise ValueError("--trigger-frames requires a hotkey and a positive frame count")
    executable = Path(args.exe).expanduser().resolve()
    if not executable.is_file():
        raise ValueError(f"Renderer executable not found: {executable}")
    directory, layer, version = discover_tools(args.tools)
    if architecture(executable) != layer["machine"]:
        raise ValueError("Renderer and GFXReconstruct layer architectures do not match")
    required = {"capture_frames", "capture_trigger", "capture_file", "memory_tracking_mode"}
    if args.trigger_frames:
        required.add("capture_trigger_frames")
    missing = required - set(layer["supportedSettings"])
    if missing:
        raise ValueError(f"Installed capture layer lacks requested settings: {sorted(missing)}")
    arguments = args.renderer_args
    if arguments[:1] == ["--"]:
        arguments = arguments[1:]
    samples = 1
    for index, argument in enumerate(arguments[:-1]):
        if argument == "--msaa":
            samples = int(arguments[index + 1])
    if version == "1.0.5" and samples > 1 and (args.trigger or (args.frames and not re.fullmatch(r"1(?:-[0-9]+)?", args.frames))):
        raise ValueError("GFXReconstruct 1.0.5 cannot reliably restore trimmed MSAA integer attachments. Use --frames 1-<last> or --all-frames with MSAA, or select --msaa 1 for runtime hotkeys/late trims.")
    if "--gfxrecon-session" in arguments:
        raise ValueError("The launcher owns --gfxrecon-session; do not pass a second session")
    output = Path(args.output_dir or ("captures/gfxrecon/" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8])).resolve()
    settings = managed_environment(args, output)
    env = dict(os.environ)
    env.pop("GFXRECON_DISABLE", None)
    env.update(settings)
    paths = env.get("VK_LAYER_PATH", "").split(os.pathsep)
    manifest_dir = str(Path(layer["manifestPath"]).parent)
    env["VK_LAYER_PATH"] = os.pathsep.join(dict.fromkeys([manifest_dir] + [p for p in paths if p]))
    session = {
        "schemaVersion": 1, "outputDirectory": str(output),
        "mode": "frames" if args.frames else "all" if args.all_frames else "hotkey",
        "frames": args.frames or "", "trigger": args.trigger or "", "stopAfterPresent": stop,
        "toolVersion": version, "toolDirectory": str(directory), "layer": layer,
        "environment": settings, "rendererArguments": arguments,
        "captureFile": settings["GFXRECON_CAPTURE_FILE"],
        "executable": str(executable), "executableSha256": sha256(executable),
        "createdUtc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "shaderFiles": [{"path": str(p.relative_to(executable.parent)), "sha256": sha256(p)}
                        for p in sorted((executable.parent / "spv_shaders").glob("*.spv"))],
    }
    command = [str(executable), "--gfxrecon-session", str(output / "session.json"), *arguments]
    if args.dry_run:
        print(json.dumps({"command": command, "session": session, "layerPath": env["VK_LAYER_PATH"]}, indent=2))
        return 0
    if output.exists() and any(output.iterdir()):
        raise ValueError(f"Output directory must be new or empty to preserve earlier captures: {output}")
    output.mkdir(parents=True, exist_ok=True)
    write_json(output / "session.json", session)
    print(f"GFXReconstruct {version}: {session['mode']}; output {output}", flush=True)
    if args.trigger:
        print(f"Capture is armed. Press {args.trigger} to start/stop; recording feedback is in layer.log.", flush=True)
    start = time.monotonic()
    with (output / "renderer.log").open("w", encoding="utf-8") as log:
        result = subprocess.run(command, cwd=executable.parent, env=env, stdout=log, stderr=subprocess.STDOUT)
    files = sorted(output.glob("*.gfxr"))
    write_json(output / "result.json", {"exitCode": result.returncode, "elapsedSeconds": time.monotonic() - start,
               "captures": [{"path": p.name, "bytes": p.stat().st_size} for p in files],
               "note": "File existence is not replay validation. Inspect/replay captures to establish completeness."})
    if result.returncode:
        print(f"Renderer exited with {result.returncode}. See {output / 'renderer.log'}", file=sys.stderr)
        return process_exit(result.returncode)
    if not files:
        print("No .gfxr file produced. Check the hotkey/frame selection and layer.log.", file=sys.stderr)
        return 2
    print(f"Produced {len(files)} capture(s), {sum(p.stat().st_size for p in files)} bytes", flush=True)
    return 0


def require_options(tool: Path, options: list[str]) -> None:
    help_text = run_text([str(tool), "--help"])
    for option in options:
        if option not in help_text:
            raise ValueError(f"Installed {tool.name} does not support {option}; select a compatible tool version")


def replay(args) -> int:
    source = Path(args.capture).resolve()
    if not source.is_file():
        raise ValueError(f"Capture not found: {source}")
    directory, _, version = discover_tools(args.tools)
    suffix = ".exe" if os.name == "nt" else ""
    tool = directory / ("gfxrecon-replay" + suffix)
    output = Path(args.output_dir or source.parent / (source.stem + "-replay")).resolve()
    if output.exists() and any(output.iterdir()):
        raise ValueError(f"Replay output must be new or empty: {output}")
    options = []
    options += ["--memory-translation", args.memory_translation]
    if args.screenshots:
        frame_ranges(args.screenshots)
        options += ["--screenshots", args.screenshots, "--screenshot-format", "png", "--screenshot-dir", str(output)]
    if args.offscreen:
        if not args.screenshots:
            raise ValueError("--offscreen requires --screenshots")
        options += ["--swapchain", "offscreen"]
    if args.validation:
        options += ["--validate"]
    require_options(tool, [x for x in options if x.startswith("--")])
    for name, enabled in (("gfxrecon-extract", args.extract_shaders), ("gfxrecon-convert", args.json_lines)):
        if enabled and not (directory / (name + suffix)).is_file():
            raise ValueError(f"Optional tool missing: {name}")
    output.mkdir(parents=True, exist_ok=True)
    env = tool_environment()
    report = {"toolVersion": version, "capture": str(source), "options": options}

    def finish(code: int, step: str) -> int:
        report.update(exitCode=process_exit(code), failedStep=step if code else None)
        write_json(output / "result.json", report)
        if code:
            print(f"{step} failed; see logs in {output}", file=sys.stderr)
        return process_exit(code)

    with (output / "info.log").open("w", encoding="utf-8") as log:
        info = subprocess.run([str(directory / ("gfxrecon-info" + suffix)), str(source)], env=env, stdout=log, stderr=subprocess.STDOUT)
    report["infoExitCode"] = info.returncode
    if info.returncode:
        return finish(info.returncode, "Inspection")
    with (output / "replay.log").open("w", encoding="utf-8") as log:
        result = subprocess.run([str(tool), *options, str(source)], cwd=output, env=env, stdout=log, stderr=subprocess.STDOUT)
    replay_text = (output / "replay.log").read_text(encoding="utf-8", errors="replace")
    errors = re.findall(r"\[gfxrecon\]\s+(?:ERROR|FATAL)\b|Validation Error", replay_text, re.IGNORECASE)
    exit_code = result.returncode or (2 if errors else 0)
    report.update(toolExitCode=result.returncode, diagnosticErrors=len(errors))
    if exit_code == 0:
        for name, enabled in (("gfxrecon-extract", args.extract_shaders), ("gfxrecon-convert", args.json_lines)):
            if enabled:
                optional_tool = directory / (name + suffix)
                extra = ["--dir", str(output / "shaders")] if name == "gfxrecon-extract" else ["--format", "jsonl", "--output", str(output / "api.jsonl")]
                require_options(optional_tool, [x for x in extra if x.startswith("--")])
                with (output / (name + ".log")).open("w", encoding="utf-8") as log:
                    process = subprocess.run([str(optional_tool), *extra, str(source)], cwd=output,
                                             env=env, stdout=log, stderr=subprocess.STDOUT)
                report[name + "ExitCode"] = process.returncode
                if process.returncode:
                    return finish(process.returncode, name)
    return finish(exit_code, "Replay")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    cap = commands.add_parser("capture", help="Launch renderer with Vulkan capture armed")
    cap.add_argument("--exe", required=True)
    cap.add_argument("--tools")
    cap.add_argument("--output-dir")
    cap.add_argument("--capture-name", default="capture", help="Capture filename stem (default: capture)")
    mode = cap.add_mutually_exclusive_group()
    mode.add_argument("--frames")
    mode.add_argument("--trigger", type=str.upper)
    mode.add_argument("--all-frames", action="store_true")
    cap.add_argument("--trigger-frames", type=int)
    cap.add_argument("--compression", choices=("LZ4", "ZLIB", "ZSTD", "NONE"), default="LZ4")
    cap.add_argument("--memory-mode", choices=("page_guard", "unassisted"), default="page_guard")
    cap.add_argument("--log-level", choices=("debug", "info", "warning", "error", "fatal"), default="info")
    cap.add_argument("--dry-run", action="store_true")
    cap.add_argument("renderer_args", nargs=argparse.REMAINDER)
    rep = commands.add_parser("replay", help="Inspect, replay, and optionally export images/shaders/API JSON")
    rep.add_argument("capture")
    rep.add_argument("--tools")
    rep.add_argument("--output-dir")
    rep.add_argument("--screenshots")
    rep.add_argument("--offscreen", action="store_true")
    rep.add_argument("--validation", action="store_true")
    rep.add_argument("--memory-translation", choices=("none", "remap", "realign", "rebind"), default="rebind")
    rep.add_argument("--extract-shaders", action="store_true")
    rep.add_argument("--json-lines", action="store_true")
    args = parser.parse_args(argv)
    try:
        return capture(args) if args.command == "capture" else replay(args)
    except (ValueError, OSError, KeyError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        print(f"GFXReconstruct: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
