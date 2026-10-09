"""Verify an extracted LTC Windows package without SDK or build-tool paths."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import subprocess
import time
import zipfile


TECHNIQUES = ("deferred-raster", "forward-raster")
DLLS = {"fmt.dll", "glfw3.dll", "MaterialXCore.dll", "MaterialXFormat.dll",
        "miniz.dll", "spdlog.dll", "vulkan-1.dll", "manifold.dll"}
LTC_FILES = {"matrix.bin", "amplitude.bin", "LICENSE.txt", "provenance.json"}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def runtime_hashes(root):
    files = [root / "VulkanSceneRenderer.exe", *root.glob("*.dll"),
             *(root / "spv_shaders").glob("*.spv"),
             *(root / "materials/ltc").glob("*")]
    return {path.relative_to(root).as_posix(): sha256(path)
            for path in sorted(files) if path.is_file()}


def inspect_package(root, archive):
    require(root.is_dir() and archive.is_file(), "Runtime directory and archive must exist")
    info = json.loads((root / "build-info.json").read_text(encoding="utf-8-sig"))
    require(re.fullmatch(r"[0-9a-f]{40}", info["source_commit"]) is not None,
            "Package source_commit must be a complete Git commit")
    require(info["configuration"] == "Release" and info["platform"] == "windows-x64",
            "This smoke check requires a Windows x64 Release package")
    expected = info["runtime_sha256"]
    require(all(PurePosixPath(name).as_posix() == name and not PurePosixPath(name).is_absolute()
                and ".." not in PurePosixPath(name).parts and "\\" not in name
                for name in expected), "Manifest paths must be normalized relative paths")
    actual = runtime_hashes(root)
    require(actual == expected, "Extracted EXE/DLL/SPIR-V/LTC files differ from the package manifest")
    dlls = {path.name for path in root.glob("*.dll")}
    shaders = list((root / "spv_shaders").glob("*.spv"))
    ltc_files = {path.name for path in (root / "materials/ltc").iterdir() if path.is_file()}
    require(DLLS <= dlls and len(dlls) == info["runtime_dll_count"], "Runtime DLL payload is incomplete")
    require(len(shaders) == info["compiled_shader_count"] and len(shaders) >= 78,
            "Compiled shader count differs from the manifest")
    require(LTC_FILES <= ltc_files and len(ltc_files) == info["ltc_asset_file_count"],
            "LTC asset payload is incomplete")
    for shader in shaders:
        require(struct.unpack("<II", shader.read_bytes()[:8]) == (0x07230203, 0x00010600),
                "Shader is not SPIR-V 1.6: " + shader.name)
    provenance = json.loads((root / "materials/ltc/provenance.json").read_text())
    for name in ("matrix.bin", "amplitude.bin"):
        data = (root / "materials/ltc" / name).read_bytes()
        require(struct.unpack("<4sIII", data[:16]) == (b"CLTC", 1, 64, 64),
                "Invalid packaged LTC table header: " + name)
        require(len(data) == provenance["files"][name]["bytes"] == 65552
                and hashlib.sha256(data).hexdigest() == provenance["files"][name]["sha256"],
                "LTC table differs from its fitter provenance: " + name)
    for name in ("README.md", "THIRD_PARTY_NOTICES.md", "ltc-area-lighting.md",
                 "models/validation/cornell_box_local_light.gltf"):
        require((root / name).is_file(), "Missing packaged guide or sample: " + name)
    checksum_file = archive.parent / "SHA256SUMS.txt"
    checksums = {}
    for line in checksum_file.read_text(encoding="utf-8-sig").splitlines():
        match = re.fullmatch(r"([0-9a-fA-F]{64})\s+\*?(.+)", line)
        if match:
            checksums[match[2]] = match[1].lower()
    archive_hash = sha256(archive)
    require(checksums.get(archive.name) == archive_hash, "ZIP differs from SHA256SUMS.txt")
    with zipfile.ZipFile(archive) as package:
        manifest_names = [name for name in package.namelist() if name.endswith("/build-info.json")]
        require(len(manifest_names) == 1, "Archive must contain exactly one build-info.json")
        manifest_name = manifest_names[0]
        archive_info = json.loads(package.read(manifest_name).decode("utf-8-sig"))
        require(info == archive_info, "Extracted and archived build manifests differ")
        prefix = manifest_name.removesuffix("build-info.json")
        archive_payload = set()
        for entry in package.namelist():
            if not entry.startswith(prefix) or entry.endswith("/"):
                continue
            relative = entry.removeprefix(prefix)
            path = PurePosixPath(relative)
            if ((len(path.parts) == 1 and path.suffix.lower() in (".exe", ".dll"))
                    or (relative.startswith("spv_shaders/") and path.suffix.lower() == ".spv")
                    or relative.startswith("materials/ltc/")):
                archive_payload.add(relative)
        require(archive_payload == set(expected), "Archive runtime file set differs from its payload manifest")
        for name, digest in expected.items():
            require(hashlib.sha256(package.read(prefix + name)).hexdigest() == digest,
                    "Archived payload differs from build-info.json: " + name)
    return info, actual, archive_hash


def isolated_environment():
    env = dict(os.environ)
    windows = Path(env.get("SYSTEMROOT", "C:/Windows"))
    env["PATH"] = os.pathsep.join(map(str, [windows, windows / "System32", windows / "SysWOW64"]))
    removed = []
    for key in list(env):
        upper = key.upper()
        if upper == "VULKAN_SDK" or upper.startswith("VK_"):
            removed.append(key)
            del env[key]
    return env, sorted(removed)


def capture(root, output, cwd, env, name, technique, mode, ready, source_commit, timeout):
    screenshot = output / (name + ".png")
    log = output / (name + ".log")
    command = [str(root / "VulkanSceneRenderer.exe"), "--hidden", "--no-ui", "--no-validation",
               "--no-taa", "--no-bloom", "--msaa", "1", "--width", "320", "--height", "240",
               "--model", "models/validation/cornell_box_local_light.gltf", "--display-mode", "lit",
               "--render-technique", technique, "--area-lighting", mode, "--area-light-samples", "25",
               "--fixed-dt", "0.016666667", "--warmup-frames", "8", "--capture-frame", "9",
               "--screenshot", str(screenshot)]
    with log.open("w", encoding="utf-8") as stream:
        completed = subprocess.run(command, cwd=cwd, env=env, stdout=stream,
                                   stderr=subprocess.STDOUT, timeout=timeout)
    text = log.read_text(encoding="utf-8", errors="replace")
    require(completed.returncode == 0, "Package capture failed: " + name + "; see " + str(log))
    require("VUID-" not in text and "SYNC-HAZARD" not in text, "Vulkan error in " + str(log))
    data = screenshot.read_bytes()
    require(data[:8] == b"\x89PNG\r\n\x1a\n" and struct.unpack(">II", data[16:24]) == (320, 240),
            "Package capture did not produce the requested PNG: " + name)
    telemetry = json.loads(screenshot.with_suffix(".telemetry.json").read_text())
    lighting = telemetry["lighting"]
    require(telemetry["technique"] == technique and telemetry["msaaSamples"] == 1,
            "Unexpected raster pipeline or sample count: " + name)
    require(lighting["ltcReady"] == ready and lighting["areaLightingRequestedMode"] == int(mode == "ltc")
            and lighting["areaLightingMode"] == int(mode == "ltc" and ready),
            "Unexpected LTC readiness or effective mode: " + name + ": " + str(lighting))
    require(lighting["areaLightSampleCount"] == 25 and lighting["areaLightCount"] > 0
            and lighting["ltcAllocatedImageBytes"] > 0, "Area-light or LUT binding missing: " + name)
    if not ready:
        require("Sampled area lighting" in lighting["ltcStatus"], "Fallback reason missing: " + name)
    build = telemetry.get("runtime", {}).get("build", telemetry.get("build"))
    if build is not None:
        require(build["revision"] == source_commit, "Captured build revision differs from the package source commit")
    print(name + ": package capture passed without SDK/build paths", flush=True)
    return dict(name=name, technique=technique, requestedMode=mode,
                effectiveMode="ltc" if lighting["areaLightingMode"] else "sampled", ltcReady=ready,
                ltcStatus=lighting["ltcStatus"], allocatedImageBytes=lighting["ltcAllocatedImageBytes"],
                exitCode=completed.returncode, imageSha256=sha256(screenshot), gpu=telemetry["gpu"],
                capturedBuildMetadata=build, capturedBuildMetadataAvailable=build is not None,
                validationEnabled=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True, help="Extracted package directory")
    parser.add_argument("--archive", type=Path, required=True, help="ZIP beside its SHA256SUMS.txt")
    parser.add_argument("--output", type=Path, required=True, help="A fresh output directory outside the runtime")
    parser.add_argument("--timeout", type=int, default=120, help="Timeout in seconds per launch")
    parser.add_argument("--preflight-only", action="store_true", help="Verify package payload without launching Vulkan")
    args = parser.parse_args()
    root, output, archive = args.runtime.resolve(), args.output.resolve(), args.archive.resolve()
    require(args.timeout > 0, "Timeout must be positive")
    require(output != root and root not in output.parents and output not in root.parents,
            "Output and runtime directories must be separate")
    require(not output.exists() or not any(output.iterdir()), "Use a fresh empty output directory")
    output.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    results = dict(runtime=str(root), archive=str(archive), validationEnabled=False, captures=[], passed=False)
    try:
        info, original_hashes, archive_hash = inspect_package(root, archive)
        results.update(sourceCommit=info["source_commit"], packageVersion=info["version"],
                       archiveSha256=archive_hash, payloadHashCount=len(original_hashes),
                       compiledShaderCount=info["compiled_shader_count"], runtimeDllCount=info["runtime_dll_count"],
                       ltcAssetFileCount=info["ltc_asset_file_count"], payloadHashesVerified=True)
        if not args.preflight_only:
            require(os.name == "nt", "Runtime launches require Windows")
            cwd = output / "unrelated-cwd"
            cwd.mkdir()
            env, removed = isolated_environment()
            results.update(workingDirectory=str(cwd), path=env["PATH"], removedEnvironmentVariables=removed,
                           sdkAndBuildPathsRemoved=True)
            for technique in TECHNIQUES:
                for mode in ("ltc", "sampled"):
                    name = technique + "-" + mode
                    results["captures"].append(capture(root, output, cwd, env, name, technique, mode,
                                                      True, info["source_commit"], args.timeout))
            for state in ("missing", "corrupt"):
                isolated = output / "isolated-runtime" / state
                # copytree creates independent files; malformed fixtures never change
                # the original extracted runtime through links or shared mappings.
                shutil.copytree(root, isolated)
                if state == "missing":
                    (isolated / "materials/ltc/amplitude.bin").unlink()
                else:
                    table = isolated / "materials/ltc/matrix.bin"
                    data = table.read_bytes()
                    table.write_bytes(b"FAIL" + data[4:])
                for technique in TECHNIQUES:
                    name = technique + "-fallback-" + state
                    results["captures"].append(capture(isolated, output, cwd, env, name, technique,
                                                      "ltc", False, info["source_commit"], args.timeout))
            require(runtime_hashes(root) == original_hashes, "The original extracted payload changed during smoke tests")
            results["originalRuntimePreserved"] = True
        results["passed"] = True
    except Exception as error:
        results["failure"] = str(error)
        raise
    finally:
        results.update(captureCount=len(results["captures"]), elapsedSeconds=time.monotonic() - started,
                       preflightOnly=args.preflight_only)
        (output / "ltc-package-smoke-results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({key: value for key, value in results.items() if key != "captures"}), flush=True)


if __name__ == "__main__":
    main()
