"""Verify extracted Windows FSR packages with the normal OS runtime environment."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import zipfile

import numpy as np
from PIL import Image

from fsr_upscaling_runtime_regression import FALLBACK_REASONS, RUNTIME_HASHES, SDK, X64_PE_MACHINE, pe_machine
from ltc_package_smoke import inspect_package, isolated_environment, require, runtime_hashes, sha256


DISPLAY = [640, 360]
QUALITY_RENDER = [426, 240]
TECHNIQUES = ("deferred-raster", "forward-raster")
UPSCALER = "amd_fidelityfx_upscaler_vk.dll"
NOTICE_HASHES = {
    "AMD-FSR-SDK-2.4.0-LICENSE.txt": "32357259b965d6a8001e9e508ba60eda6034027c5923a7d226ffbaff813c2bfb",
    "AMD-FSR-SDK-2.4.0-THIRD-PARTY-NOTICES.md": "3b69b1a1011622557a81b55a9a211dd797d01b8160924a5ec51b4c3432d33a4f",
}
MODEL = "models/validation/taa_scene.gltf"


def extract(archive, directory):
    prefix = archive.stem + "/"
    with zipfile.ZipFile(archive) as package:
        members = package.namelist()
        require(bool(members), f"Package archive is empty: {archive}")
        require(all(name.startswith(prefix) and "\\" not in name
                    and ".." not in PurePosixPath(name).parts for name in members),
                f"Package members must use one top-level directory named {archive.stem}")
        require(len(set(name.casefold() for name in members)) == len(members),
                f"Duplicate package archive paths: {archive}")
        package.extractall(directory)
    return directory / archive.stem


def load_package(archive, directory, enabled):
    root = extract(archive, directory)
    info, hashes, archive_hash = inspect_package(root, archive)
    version = SDK if enabled else "disabled"
    require(info["fsr_sdk"] == version, f"Unexpected FSR SDK field in {archive}: {info['fsr_sdk']}")
    require(info["fsr_bundled_portable"] == ("3.1.6" if enabled else "disabled"),
            f"Unexpected bundled FSR provider in {archive}")
    require(pe_machine(root / "VulkanSceneRenderer.exe") == X64_PE_MACHINE, f"Renderer is not x64 in {archive}")
    for name, expected_hash in RUNTIME_HASHES.items():
        path = root / name
        if enabled:
            require(hashes.get(name) == expected_hash and pe_machine(path) == X64_PE_MACHINE,
                    f"Wrong hash or architecture for {path}")
        else:
            require(name not in hashes and not path.exists(), f"Unexpected FSR DLL in the SDK-off archive: {name}")
    for name, expected_hash in NOTICE_HASHES.items():
        path = root / "THIRD_PARTY_LICENSES" / name
        if enabled:
            notice_hash = hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
            require(notice_hash == expected_hash, f"Wrong pinned FSR notice: {path}")
        else:
            require(not path.exists(), f"Unexpected FSR notice in the SDK-off archive: {name}")
    require((root / MODEL).is_file(), f"Missing packaged FSR smoke scene: {root / MODEL}")
    if enabled:
        require("spv_shaders/fsr_inputs.comp.spv" in hashes, f"Missing packaged FSR input shader: {root}")
    receipt = {"archive": str(archive), "archiveSha256": archive_hash,
               "sourceCommit": info["source_commit"], "runtime": str(root),
               "rendererSha256": hashes["VulkanSceneRenderer.exe"], "fsrSdk": version,
               "runtimeHashesVerified": len(hashes), "compiledShaders": info["compiled_shader_count"],
               "runtimeDllCount": info["runtime_dll_count"]}
    return root, hashes, receipt


def clean_environment():
    env, removed = isolated_environment()
    build_variables = {"CMAKE_PREFIX_PATH", "CMAKE_TOOLCHAIN_FILE", "INCLUDE", "LIB", "LIBPATH",
                       "VCINSTALLDIR", "VSINSTALLDIR", "WINDOWSSDKDIR"}
    for key in list(env):
        name = key.upper()
        if name in build_variables or name.startswith(("VCPKG_", "CONTAINER_", "VSCMD_")):
            del env[key]
            removed.append(key)
    # Prevent registry-discovered developer layers from injecting into the run.
    env["VK_LOADER_LAYERS_DISABLE"] = "~implicit~"
    return env, sorted(removed)


def capture(root, output, cwd, env, label, technique, mode, commit, timeout):
    png = output / (label + ".png")
    log = output / (label + ".log")
    executable = root / "VulkanSceneRenderer.exe"
    command = [str(executable), "--hidden", "--no-ui", "--no-validation", "--no-taa",
               "--msaa", "1", "--width", str(DISPLAY[0]), "--height", str(DISPLAY[1]),
               "--model", MODEL, "--import-scale", "1", "--camera-position", "0", "1.8", "6.5",
               "--camera-target", "0", ".8", "0", "--render-technique", technique, "--display-mode", "lit",
               "--fsr", mode, "--no-bloom", "--no-ddgi", "--no-ray-query", "--ray-shadows", "raster",
               "--sky", "plain", "--exposure", ".5", "--fixed-dt", ".016666667",
               "--warmup-frames", "7", "--capture-frame", "8", "--screenshot-viewport", "--screenshot", str(png)]
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.run(command, cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=timeout)
    require(process.returncode == 0, f"Extracted package failed to start or render: {log}")
    state_path = png.with_suffix(".telemetry.json")
    state = json.loads(state_path.read_text(encoding="utf-8"))
    capture_path = png.with_suffix(".png.capture.json")
    capture_info = json.loads(capture_path.read_text(encoding="utf-8"))
    require(capture_info["source"] == "viewport" and not capture_info["includesUi"]
            and capture_info["displayMode"] == 0, f"Expected a Lit viewport capture: {capture_path}")
    require(state["technique"] == technique and state["msaaSamples"] == 1
            and not state["validationEnabled"] and not state["taa"]["enabled"],
            f"Unintended render settings: {state_path}")
    require(state["build"]["revision"] == commit, f"Renderer revision differs from the package manifest: {state_path}")
    scene = state["scene"]
    require(Path(scene["primary"]) in (Path(MODEL), root / MODEL) and scene["primaryObjectCount"] > 1,
            f"App did not load the packaged smoke scene: {scene}")
    fsr = state["fsr"]
    require(fsr["requested"] == mode and fsr["displayExtent"] == DISPLAY, f"Unexpected FSR request or extent: {fsr}")
    with Image.open(png) as image:
        require(image.format == "PNG" and list(image.size) == DISPLAY, f"Wrong capture format or image size: {png}")
        pixels = np.array(image.convert("RGB"))
    require(int(pixels.max()) - int(pixels.min()) > 10, f"Capture is empty or flat: {png}")
    return pixels, {"file": str(png), "telemetry": str(state_path), "log": str(log), "command": command,
                    "imageSha256": sha256(png), "gpu": state["gpu"], "fsr": fsr,
                    "temporalResolve": any(p["active"] and p["name"] == "TemporalResolve" for p in state["passes"])}


def require_native(capture_receipt, reason=None):
    fsr = capture_receipt["fsr"]
    require(not fsr["active"] and fsr["mode"] == "native" and fsr["renderExtent"] == DISPLAY
            and not capture_receipt["temporalResolve"], f"Renderer did not use native resolution: {fsr}")
    if reason:
        require(fsr["status"] in FALLBACK_REASONS[reason], f"Incorrect {reason} status: {fsr}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path, help="FSR-enabled ZIP beside SHA256SUMS.txt")
    parser.add_argument("--sdk-off-archive", type=Path, help="Separately built SDK-off ZIP and its checksum file")
    parser.add_argument("--output", required=True, type=Path, help="Fresh output directory")
    parser.add_argument("--expect-provider", default="3.1.6")
    parser.add_argument("--timeout", default=120, type=int)
    parser.add_argument("--preflight-only", action="store_true", help="Check archive and payload without launching Vulkan")
    args = parser.parse_args()
    require(args.timeout > 0, "Timeout must be positive")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    archive = args.archive.resolve(strict=True)
    off_archive = args.sdk_off_archive.resolve(strict=True) if args.sdk_off_archive else None
    report = {"passed": False, "preflightOnly": args.preflight_only, "validationEnabled": False, "captures": {}}
    try:
        enabled, enabled_hashes, report["enabledPackage"] = load_package(archive, output / "on", True)
        if off_archive:
            disabled, disabled_hashes, report["sdkOffPackage"] = load_package(off_archive, output / "off", False)
        if not args.preflight_only:
            require(os.name == "nt", "Native package launches require Windows")
            cwd = output / "unrelated-cwd"
            cwd.mkdir()
            env, removed = clean_environment()
            report.update(workingDirectory=str(cwd), path=env["PATH"], removedEnvironmentVariables=removed,
                          loaderLayerFilter=env["VK_LOADER_LAYERS_DISABLE"])
            # Fresh independent files from the verified archive. Keep the enabled
            # extraction and the original ZIP intact for both native controls.
            missing = extract(archive, output / "missing")
            require(runtime_hashes(missing) == enabled_hashes, "Missing-DLL fixture differs from the verified archive")
            (missing / UPSCALER).unlink()
            report["missingModule"] = UPSCALER

            def run(root, technique, mode, variant, package):
                label = f"{variant}-{technique}-{mode}"
                pixels, receipt = capture(root, output, cwd, env, label, technique, mode, package["sourceCommit"], args.timeout)
                report["captures"][label] = receipt
                return pixels, receipt

            for technique in TECHNIQUES:
                native, receipt = run(enabled, technique, "native", "enabled", report["enabledPackage"])
                require_native(receipt)
                quality, receipt = run(enabled, technique, "quality", "enabled", report["enabledPackage"])
                fsr = receipt["fsr"]
                expected_status = f"FSR {args.expect_provider} is ready (SDK {SDK})"
                require(fsr["active"] and fsr["mode"] == "quality" and fsr["status"] == expected_status
                        and fsr["renderExtent"] == QUALITY_RENDER and receipt["temporalResolve"],
                        f"Packaged FSR provider did not reconstruct at Quality: {fsr}")
                require(not np.array_equal(native, quality), f"Quality did not change the image: {technique}")
                fallback, receipt = run(missing, technique, "quality", "missing", report["enabledPackage"])
                require_native(receipt, "missing-runtime")
                receipt["pixelExactNative"] = bool(np.array_equal(native, fallback))
                require(receipt["pixelExactNative"], f"Missing-module fallback changed native pixels: {technique}")
                if off_archive:
                    off_native, receipt = run(disabled, technique, "native", "sdk-off", report["sdkOffPackage"])
                    require_native(receipt)
                    off_quality, receipt = run(disabled, technique, "quality", "sdk-off", report["sdkOffPackage"])
                    require_native(receipt, "sdk-disabled")
                    receipt["pixelExactNative"] = bool(np.array_equal(off_native, off_quality))
                    require(receipt["pixelExactNative"], f"SDK-off fallback changed native pixels: {technique}")
            require(runtime_hashes(enabled) == enabled_hashes
                    and sha256(archive) == report["enabledPackage"]["archiveSha256"],
                    "Enabled package changed during smoke verification")
            if off_archive:
                require(runtime_hashes(disabled) == disabled_hashes
                        and sha256(off_archive) == report["sdkOffPackage"]["archiveSha256"],
                        "SDK-off package changed during smoke verification")
        report["passed"] = True
    except Exception as error:
        report["failure"] = str(error)
        raise
    finally:
        path = output / "results.json"
        path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(path, flush=True)


if __name__ == "__main__":
    main()
