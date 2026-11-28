#!/usr/bin/env python3
"""Run the opt-in DDGI and ray-query validation matrix on one Vulkan adapter."""

import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time
from urllib.parse import unquote, urlsplit

from device_matrix_capabilities import match_capture_device, parse_vulkaninfo, select_device
from device_matrix_correctness import run_correctness
from device_matrix_ctest import CPU_TESTS, GPU_TESTS, SPIRV_TESTS, run_registered_tests
from device_matrix_timing import run_timing
from run_performance import Config, ERROR_PATTERN, validate_config


REPO = Path(__file__).resolve().parents[3]
VENDOR_IDS = {0x1002: "AMD", 0x8086: "Intel", 0x10DE: "NVIDIA"}
IDENTITY_FIELDS = ("vendorId", "deviceId", "gpu", "driverVersion", "vulkanApiVersion")
DEVICE_OVERRIDES = (
    "VK_DRIVER_FILES", "VK_ADD_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_LOADER_DRIVERS_SELECT",
    "VK_LOADER_DRIVERS_DISABLE", "VK_LOADER_DEVICE_SELECT", "VK_LOADER_DISABLE_SELECT",
    "MESA_VK_DEVICE_SELECT", "DRI_PRIME", "VK_LAYER_PATH", "VK_ADD_LAYER_PATH", "VK_LOADER_LAYERS_DISABLE",
)
DATA_SUFFIXES = {".ifc", ".ifcx", ".gltf", ".glb", ".bin", ".ktx2", ".dds", ".jpg", ".jpeg", ".webp"}


def now():
    return datetime.now(timezone.utc).isoformat()


def fingerprint(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return {"path": str(path), "sha256": digest.hexdigest(), "sizeBytes": path.stat().st_size}


def write_json(path, data):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(data, indent=2, allow_nan=False) + "\n", encoding="utf-8", newline="\n")
    temporary.replace(path)


def dependencies(model):
    files = [model]
    if model.suffix.lower() != ".gltf":
        return files
    document = json.loads(model.read_bytes())
    for resource in document.get("buffers", []) + document.get("images", []):
        uri = resource.get("uri")
        if not uri or uri.startswith("data:"):
            continue
        parsed = urlsplit(uri)
        if parsed.scheme or parsed.netloc or parsed.query or parsed.fragment:
            raise ValueError(f"Use local glTF dependencies for reproducible captures: {uri}")
        files.append((model.parent / unquote(parsed.path)).resolve(strict=True))
    return list(dict.fromkeys(files))


def environment_for_run():
    # Ignore inherited validation forcing and GoogleTest filters. Preserve the
    # caller's driver/ICD selection so inventory, captures and tests see it.
    removed = {"VK_INSTANCE_LAYERS", "VK_LOADER_LAYERS_ENABLE", "VK_LAYER_SETTINGS_PATH",
               "CONTAINER_DDGI_ASYNC_QUEUE", "CONTAINER_RUN_GPU_DDGI", "CONTAINER_RUN_GPU_RAY_QUERY"}
    return {k: v for k, v in os.environ.items() if k not in removed and not k.startswith("GTEST_")}


def invoke(command, log, *, cwd, environment, timeout, attached_console=False):
    result = {"command": command, "workingDirectory": str(cwd), "log": str(log), "status": "failed"}
    started = time.monotonic()
    with log.open("wb") as stream:
        try:
            proc = subprocess.run(command, cwd=cwd, env=environment, stdin=subprocess.DEVNULL, stdout=stream,
                                  stderr=subprocess.STDOUT, timeout=timeout,
                                  creationflags=0 if attached_console else getattr(subprocess, "CREATE_NO_WINDOW", 0))
            result.update(exitCode=proc.returncode, status="passed" if proc.returncode == 0 else "failed")
        except (OSError, subprocess.TimeoutExpired) as error:
            result["reason"] = str(error)
    result["elapsedSeconds"] = time.monotonic() - started
    return result


def read_validation_manifest(value):
    path = value.resolve(strict=True)
    if path.is_dir():
        path = path / "VkLayer_khronos_validation.json"
    manifest = json.loads(path.read_bytes())["layer"]
    if path.name != "VkLayer_khronos_validation.json" or manifest["name"] != "VK_LAYER_KHRONOS_validation":
        raise ValueError(f"Expected the Khronos validation layer manifest, got {path}")
    binary = path.parent / manifest["library_path"]
    record = {"manifest": fingerprint(path), "apiVersion": manifest.get("api_version"),
              "libraryPath": manifest["library_path"]}
    if binary.is_file():
        record["library"] = fingerprint(binary.resolve())
    return path, record


def probe_renderer(exe, scene, output, environment, validated, execute):
    sequence = output / "startup.sequence.json"
    write_json(sequence, {"schemaVersion": 1, "frames": 3, "sampleFrames": [3]})
    image = output / "startup.png"
    command = [str(exe), "--model", str(scene), "--hidden", "--no-ui",
               "--validation" if validated else "--no-validation", "--width", "160", "--height", "120",
               "--render-technique", "deferred-raster", "--display-mode", "lit", "--msaa", "1",
               "--no-bloom", "--no-taa", "--fixed-dt", str(1 / 60), "--ddgi", "--ray-shadows", "raster",
               "--screenshot-viewport", "--capture-sequence", str(sequence), "--screenshot", str(image)]
    result = execute(command, output / "startup.log", cwd=exe.parent, environment=environment)
    if result["status"] != "passed":
        raise RuntimeError(f"Renderer startup capture failed. See {result['log']}")
    log = Path(result["log"]).read_text(encoding="utf-8", errors="replace")
    if ERROR_PATTERN.search(log):
        raise RuntimeError(f"Renderer startup reported a validation or runtime error. See {result['log']}")
    if validated and 'Inserted device layer "VK_LAYER_KHRONOS_validation"' not in log:
        raise RuntimeError(f"The loader did not confirm Khronos device validation. See {result['log']}")
    path = image.with_suffix(".telemetry.json")
    telemetry = json.loads(path.read_bytes())
    if telemetry["validationEnabled"] is not validated or not image.is_file():
        raise ValueError("The startup capture did not confirm the requested validation state")
    result.update(telemetry=str(path), build=telemetry["build"], ddgi=telemetry["ddgi"])
    return result, telemetry


def match_all_captures(paths, inventory, device, validated, build):
    if not paths:
        raise ValueError("The lane produced no device-verifiable capture telemetry")
    for file in paths:
        path = Path(file)
        state = json.loads(path.read_bytes())
        try:
            match_capture_device(inventory, state, intended_device=device)
            if state["validationEnabled"] is not validated or state["build"] != build:
                raise ValueError("capture validation mode or renderer build changed")
        except (KeyError, ValueError) as error:
            raise ValueError(f"{path}: {error}") from error
    return len(paths)


def receipt(result, hardware, index, validate, build):
    result["status"] = "passed" if result["passed"] else "failed"
    try:
        result["matchedCaptures"] = match_all_captures(result["captures"], hardware, index, validate, build)
    except (KeyError, OSError, ValueError) as error:
        result.update(status="failed", captureVerificationFailure=str(error))
    return result


def matrix_status(mode, phases, failure):
    if failure or phases["failed"]:
        return "failed"
    if mode == "timing" and phases["unsupported"]:
        return "unsupported"
    if phases["partial"] or phases["unsupported"] or phases["untested"]:
        return "partial"
    return "passed"


def production_spirv(exe, output, validator, execute, environment):
    shaders = exe.parent / "spv_shaders"
    required = ("ddgi_update.comp.spv", "ddgi_trace.comp.spv", "ddgi_debug.vert.spv", "ddgi_debug.frag.spv")
    output.mkdir()
    cases = []
    for name in required:
        path = shaders / name
        if not path.is_file():
            cases.append({"name": name, "status": "failed", "reason": f"Missing production shader {path}"})
            continue
        command = [validator, "--target-env", "vulkan1.4", str(path)]
        result = execute(command, output / f"{name}.log", cwd=exe.parent, environment=environment)
        cases.append({**result, "name": name, "shader": fingerprint(path)})
    return {"status": "passed" if all(case["status"] == "passed" for case in cases) else "failed", "cases": cases}


def resolve_executable(build, configuration):
    name = "VulkanSceneRenderer.exe" if os.name == "nt" else "VulkanSceneRenderer"
    for folder in (build / configuration, build):
        if (folder / name).is_file():
            return folder / name
    raise FileNotFoundError(f"Build {name} under {build} or pass --exe")


def run_matrix(args):
    output = args.output.resolve()
    output.mkdir(parents=True)
    report_file = output / "matrix.json"
    report = {"schema": "container.ddgi-device-matrix", "version": 1, "startedUtc": now(),
              "host": platform.platform(), "mode": args.mode, "status": "running", "phases": {}}
    write_json(report_file, report)
    environment = environment_for_run()

    def execute(command, log, *, cwd, environment):
        return invoke(command, log, cwd=cwd, environment=environment, timeout=args.timeout)

    def phase(name, work):
        print(f"Running {name}", flush=True)
        started = time.monotonic()
        try:
            result = work()
        except (AssertionError, KeyError, OSError, RuntimeError, ValueError) as error:
            result = {"status": "failed", "reason": str(error)}
        result["elapsedSeconds"] = time.monotonic() - started
        report["phases"][name] = result
        write_json(report_file, report)
        return result

    def run():
        build = args.build_dir.resolve(strict=True)
        exe = args.exe.resolve(strict=True) if args.exe else resolve_executable(build, args.configuration)
        scene = args.scene.resolve(strict=True)
        inputs = [exe, *dependencies(scene), *sorted((exe.parent / "spv_shaders").glob("*.spv"))]
        scripts = Path(__file__).resolve().parent
        inputs.extend(sorted(scripts.glob("device_matrix_*.py")))
        inputs.extend(scripts / name for name in ("run_runtime.py", "run_glass.py", "run_performance.py"))
        inputs.extend((Path(__file__).resolve(), REPO / "tests/validation/ddgi_queue_runtime_regression.py"))
        report.update(configuration=args.configuration, buildDirectory=str(build),
                      inputFiles=[fingerprint(path) for path in inputs],
                      deviceEnvironment={key: environment[key] for key in DEVICE_OVERRIDES if key in environment})
        if args.mode != "timing":
            log = output / "ctest-inventory.json"
            inventory_run = execute([args.ctest, "--test-dir", str(build), "-C", args.configuration,
                                     "--show-only=json-v1"], log, cwd=build, environment=environment)
            report["ctestInventory"] = inventory_run
            if inventory_run["status"] != "passed":
                raise RuntimeError(f"Failed to read registered CTest commands. See {log}")
            inventory = {row["name"]: row for row in json.loads(log.read_bytes())["tests"]}
            phase("cpu-contracts", lambda: run_registered_tests(
                CPU_TESTS, inventory, build, output / "cpu", execute, fingerprint, environment))
            phase("contract-spirv", lambda: run_registered_tests(
                SPIRV_TESTS, inventory, build, output / "contract-spirv", execute, fingerprint, environment))
            phase("production-spirv", lambda: production_spirv(
                exe, output / "production-spirv", args.spirv_val, execute, environment))
        if args.mode == "cpu":
            return

        raw = output / "vulkaninfo.txt"
        # On Windows, vulkaninfo can stall at shutdown when launched without a
        # console, even when all of its output has been redirected to a file.
        report["vulkaninfo"] = invoke([args.vulkaninfo, "--text", "--show-formats"], raw, cwd=output,
                                      environment=environment, timeout=args.timeout, attached_console=True)
        if report["vulkaninfo"]["status"] != "passed":
            raise RuntimeError(f"vulkaninfo failed. See {raw}")
        report["vulkaninfo"]["rawOutput"] = fingerprint(raw)
        hardware = parse_vulkaninfo(raw.read_bytes().decode("utf-8", errors="replace"))
        write_json(output / "vulkan-inventory.json", hardware)
        report["hardwareInventory"] = str(output / "vulkan-inventory.json")
        if args.device is not None:
            select_device(hardware, args.device)

        validated = args.mode != "timing"
        validation_env = environment.copy()
        if validated:
            manifest, report["validationLayer"] = read_validation_manifest(args.validation_layer)
            settings = output / "vk_layer_settings.txt"
            settings.write_text("khronos_validation.validate_sync = true\n", encoding="ascii", newline="\n")
            validation_env.update(VK_LAYER_PATH=str(manifest.parent), VK_LAYER_SETTINGS_PATH=str(output),
                                  VK_LOADER_DEBUG="layer", CONTAINER_RUN_GPU_DDGI="1", CONTAINER_RUN_GPU_RAY_QUERY="1")
        startup, telemetry = probe_renderer(exe, scene, output, validation_env, validated, execute)
        device = match_capture_device(hardware, telemetry, intended_device=args.device)
        index = device["deviceIndex"]
        report["device"] = device
        report["startup"] = startup
        report["otherEnumeratedAdapters"] = [
            {"index": row["index"], "identity": row["identity"], "status": "untested"}
            for row in hardware["devices"] if row["index"] != index
        ]
        seen = {row["identity"]["vendorId"] for row in hardware["devices"]}
        report["vendorsNotEnumerated"] = [name for id_, name in VENDOR_IDS.items() if id_ not in seen]
        app = {key: telemetry[key] for key in IDENTITY_FIELDS}
        build_id = telemetry["build"]
        write_json(report_file, report)

        if validated:
            if len(hardware["devices"]) == 1:
                phase("gpu-contracts", lambda: run_registered_tests(
                    GPU_TESTS, inventory, build, output / "gpu", execute, fingerprint, validation_env,
                    allow_skips=True, verify_validation=True))
            else:
                phase("gpu-contracts", lambda: {"status": "untested", "reason":
                      "These independent executables have no per-case adapter receipt. Expose one Vulkan device to attribute their results."})

            def correctness():
                dest = output / "correctness"
                result = run_correctness(exe, dest, build_id["revision"], manifest,
                                         timeout=args.timeout, environment=validation_env)
                result["fixtures"] = [fingerprint(path) for path in sorted(dest.rglob("*"))
                                      if path.is_file() and path.suffix.lower() in DATA_SUFFIXES]
                return receipt(result, hardware, index, True, build_id)

            phase("native-correctness", correctness)

        if args.mode in {"all", "timing"}:
            gi = telemetry["ddgi"]
            if gi["supported"] is False and gi["active"] is False:
                phase("release-timing", lambda: {"status": "unsupported", "startupDdgi": gi,
                                                "reason": "The actual renderer reported unavailable DDGI"})
            else:
                def timing():
                    config = Config(warmup=args.warmup, samples=args.samples, repeats=args.repeats,
                                    width=args.width, height=args.height)
                    result = run_timing(exe, scene, output / "timing", environment, app,
                                        timeout=args.timeout, config=config)
                    return receipt(result, hardware, index, False, build_id)

                phase("release-timing", timing)

        if [fingerprint(path) for path in inputs] != report["inputFiles"]:
            raise ValueError("The renderer, shaders, runner or timing fixture changed while collecting evidence")

    try:
        run()
    except (AssertionError, KeyError, OSError, RuntimeError, ValueError) as error:
        report["failure"] = str(error)
    except KeyboardInterrupt:
        report["failure"] = "Interrupted before completing the requested phases"
        raise
    finally:
        statuses = Counter(phase["status"] for phase in report["phases"].values())
        report.update(finishedUtc=now(), phaseCounts=dict(statuses),
                      status=matrix_status(args.mode, statuses, report.get("failure")))
        write_json(report_file, report)
    print(f"{report['status'].upper()}: {report_file}", flush=True)
    if report.get("failure"):
        print(report["failure"], file=sys.stderr)
    return {"failed": 1, "unsupported": 2}.get(report["status"], 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--exe", type=Path, help="renderer path, if staged outside the build directory")
    parser.add_argument("--output", type=Path, required=True, help="new directory for all evidence")
    parser.add_argument("--configuration", default="Release")
    parser.add_argument("--mode", choices=("all", "cpu", "correctness", "timing"), default="all")
    parser.add_argument("--device", help="expected vulkaninfo index, full device UUID or exact device name")
    parser.add_argument("--validation-layer", type=Path, help="path to VkLayer_khronos_validation.json or its directory")
    parser.add_argument("--scene", type=Path, default=REPO / "models/validation/cornell_box_local_light.gltf")
    parser.add_argument("--ctest", default=shutil.which("ctest") or "ctest")
    parser.add_argument("--spirv-val", default=shutil.which("spirv-val") or "spirv-val")
    parser.add_argument("--vulkaninfo", default=shutil.which("vulkaninfo") or "vulkaninfo")
    parser.add_argument("--timeout", type=float, default=300, help="maximum seconds for one process")
    parser.add_argument("--warmup", type=int, default=120)
    parser.add_argument("--samples", type=int, default=90)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--width", type=int, default=1024)
    parser.add_argument("--height", type=int, default=768)
    args = parser.parse_args()
    if args.mode in {"all", "correctness"} and args.validation_layer is None:
        parser.error("--validation-layer is required for synchronization-validated correctness")
    if args.device is not None and args.device.isdecimal():
        args.device = int(args.device)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive and finite")
    if args.mode in {"all", "timing"}:
        try:
            validate_config(Config(warmup=args.warmup, samples=args.samples, repeats=args.repeats,
                                   width=args.width, height=args.height))
        except AssertionError as error:
            parser.error(str(error))
    try:
        return run_matrix(args)
    except OSError as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    sys.exit(main())
