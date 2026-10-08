"""Verify an extracted Windows runtime from an unrelated cwd without SDK paths."""
import argparse
import hashlib
import json
import os
import pathlib
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    root, output = args.runtime.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    exe = root / "VulkanSceneRenderer.exe"
    info = json.loads((root / "build-info.json").read_text(encoding="utf-8-sig"))
    shader_count = len(list((root / "spv_shaders").glob("*.spv")))
    assert shader_count == info["compiled_shader_count"] and shader_count >= 73
    for name in ("ray_shadow_trace.comp.spv", "ray_shadow_filter.comp.spv"):
        assert (root / "spv_shaders" / name).is_file(), name
    for name in ["taa_coverage.gltf", "taa_scene.gltf", "taa_equivalent.gltf", "taa_equivalent.bim",
                 "taa_equivalent.usda", "taa_lifecycle.json", "taa_reset.json"]:
        assert (root / "models/validation" / name).is_file(), name
    env = dict(os.environ)
    windows = os.environ.get("SYSTEMROOT", "C:/Windows")
    env["PATH"] = os.pathsep.join([windows, windows + "/System32", windows + "/SysWOW64"])
    for key in list(env):
        if key.upper() in ["VULKAN_SDK", "VK_LAYER_PATH", "VK_ADD_LAYER_PATH", "VK_LAYER_SETTINGS_PATH", "VK_INSTANCE_LAYERS"]:
            del env[key]
    results = {"sourceCommit": info["source_commit"], "compiledShaderCount": shader_count,
               "exeSha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
               "workingDirectory": str(output), "path": "Windows, System32 and SysWOW64 only",
               "sdkVariablesRemoved": True, "validationEnabled": False, "captures": []}
    cases = [("deferred-taa", "deferred-raster", "taa_scene.gltf", "taa_object.json", True, 1),
             ("forward-usd-taa", "forward-raster", "taa_equivalent.usda", "taa_pan.json", True, 1),
             ("forward-bim-taa", "forward-raster", "taa_equivalent.bim", "taa_provider_motion.json", True, 1),
             ("forward-msaa4", "forward-raster", "taa_scene.gltf", None, False, 4)]
    for name, technique, model, sequence, taa, msaa in cases:
        command = [str(exe), "--hidden", "--no-ui", "--no-validation", "--taa" if taa else "--no-taa",
                   "--msaa", str(msaa), "--model", "models/validation/" + model, "--width", "640", "--height", "360",
                   "--render-technique", technique, "--display-mode", "lit", "--screenshot", str(output / (name + ".png")),
                   "--fixed-dt", "0.016666667", "--exposure", "0.25", "--no-bloom"]
        command += ["--capture-sequence", str(root / "models/validation" / sequence)] if sequence else ["--capture-frame", "3"]
        with (output / (name + ".log")).open("w", encoding="utf-8") as log:
            completed = subprocess.run(command, cwd=output, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=120)
        if completed.returncode or not (output / (name + ".png")).is_file():
            raise RuntimeError("Package capture failed: " + name)
        telemetry = json.loads((output / (name + ".telemetry.json")).read_text())
        assert telemetry["taa"]["enabled"] == taa and telemetry["msaaSamples"] == msaa
        results["captures"].append({"name": name, "technique": technique, "model": model, "taa": taa, "msaa": msaa,
                                   "submittedFrame": telemetry["taa"]["submittedFrame"], "exitCode": completed.returncode,
                                   "imageProduced": True, "gpu": telemetry["gpu"], "imagePayloadBytes": telemetry["taa"]["imagePayloadBytes"]})
        print(name + ": extracted package capture passed without SDK/build-tool paths", flush=True)
    for technique, mode, disabled in (("deferred-raster", "soft", False),
                                      ("forward-raster", "hard", False),
                                      ("forward-raster", "soft", True)):
        name = technique + "-ray-" + ("disabled" if disabled else mode)
        command = [str(exe), "--hidden", "--no-ui", "--no-validation", "--no-taa", "--msaa", "1",
                   "--model", "models/validation/cornell_box_local_light.gltf", "--width", "640", "--height", "360",
                   "--render-technique", technique, "--display-mode", "lit", "--ray-shadows", mode,
                   "--capture-frame", "8", "--screenshot", str(output / (name + ".png"))]
        if disabled: command.append("--no-ray-query")
        with (output / (name + ".log")).open("w") as log:
            completed = subprocess.run(command, cwd=output, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=120)
        assert completed.returncode == 0, name
        ray = json.loads((output / (name + ".telemetry.json")).read_text())["rayShadows"]
        assert ray["active"] == (ray["supported"] and not disabled), (name, ray)
        results["captures"].append(dict(name=name, rayShadows=ray, exitCode=completed.returncode))
        print(name + ": extracted ray shadow capture passed without SDK/build-tool paths", flush=True)
    (output / "package-smoke-results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
