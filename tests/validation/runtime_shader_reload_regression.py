"""Exercise graphics shader reload without changing workspace shader files."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

import numpy as np
from PIL import Image

from gltf_extended_materials_runtime_regression import fixture, extension_material, validation_log_error
from hdr_environment_runtime_regression import write_float_exr
from display_grade_runtime_regression import expected_rgb


def cmake_literal(value):
    if isinstance(value, Path):
        value = value.as_posix()
    delimiter = "=="
    while f"]{delimiter}]" in value:
        delimiter += "="
    return f"[{delimiter}[{value}]{delimiter}]"


def manifest_setting(manifest, name):
    match = re.search(rf"^set\({name} \[==\[(.*)\]==\]\)$", manifest, re.MULTILINE)
    if not match:
        raise RuntimeError(f"Shader reload manifest does not define {name}")
    return match[1]


def seed_shadow_baseline(root, runtime, source, output, manifest, shader, shadow):
    baseline_interfaces = Path(manifest_setting(manifest, "interfaces"))
    interfaces = output / "shadow baseline interfaces"
    shutil.copytree(baseline_interfaces, interfaces)
    spirv = runtime / "spv_shaders"
    script = output / "seed-shadow-baseline.cmake"
    body = [
        "cmake_minimum_required(VERSION 3.23)",
        f"set(compiler [==[{manifest_setting(manifest, 'compiler')}]==])",
        f"set(flags [==[{manifest_setting(manifest, 'flags')}]==])",
    ]
    fingerprints = cmake_literal(root / "cmake" / "ShaderDependencies.cmake")
    for entry, suffix in (("vertMain", "vert"), ("fragMain", "frag")):
        name = f"post_process.{suffix}.spv"
        binary = cmake_literal(spirv / name)
        depfile = interfaces / f"{name}.d"
        repro = interfaces / f"{name}.slang-repro"
        body += [
            f'execute_process(COMMAND "${{compiler}}" {cmake_literal(shader)} ${{flags}} -entry {entry}',
            f"    -o {binary} -depfile {cmake_literal(depfile)}",
            f"    -reflection-json {cmake_literal(interfaces / f'{name}.json')} -dump-repro {cmake_literal(repro)}",
            "    COMMAND_ERROR_IS_FATAL ANY)",
            'execute_process(COMMAND "${CMAKE_COMMAND}"',
            f"    {cmake_literal(f'-DFINGERPRINT_OUTPUT={depfile.as_posix()}.sha256')}",
            f"    {cmake_literal(f'-DDEPFILE={depfile.as_posix()}')}",
            f"    {cmake_literal(f'-DSOURCE_ROOT={source.as_posix()}')}",
            f"    {cmake_literal(f'-DLOOKUP_OUTPUT={depfile.as_posix()}.lookups')}",
            f"    {cmake_literal(f'-DLOOKUP_REPRO={repro.as_posix()}')} \"-DSLANGC=${{compiler}}\"",
            f"    -P {fingerprints} COMMAND_ERROR_IS_FATAL ANY)",
        ]
    script.write_text("\n".join(body) + "\n", encoding="utf-8")
    cmake = (runtime / "runtime_shader_reload_tool.txt").read_text(encoding="utf-8").strip()
    with (output / "seed-shadow-baseline.log").open("w", encoding="utf-8") as log:
        subprocess.run([cmake, "-P", str(script)], cwd=runtime, check=True, timeout=120,
                       stdout=log, stderr=subprocess.STDOUT)
    for suffix in ("vert", "frag"):
        depfile = interfaces / f"post_process.{suffix}.spv.d"
        dependencies = depfile.read_text(encoding="utf-8").replace("\\\\", "/")
        lookups = depfile.with_name(f"{depfile.name}.lookups").read_text(encoding="utf-8").splitlines()
        misses = {Path(row[2:]) for row in lookups[1:] if row.startswith("- ")}
        hits = {Path(row[65:]) for row in lookups[1:] if re.match(r"^[a-f0-9]{64} ", row)}
        fallback = shader.parent / shadow.name
        if (f"/shaders/{shadow.name}" not in dependencies or not lookups or
                lookups[0] != "slang-lookup-v2" or shadow not in misses or fallback not in hits):
            raise RuntimeError(f"Slang did not witness the fallback hit and nested lookup miss for {depfile.name}")
    for setting, replacement in (("baseline_spirv", spirv), ("interfaces", interfaces),
                                 ("baseline_source", source)):
        old = f"set({setting} [==[{manifest_setting(manifest, setting)}]==])"
        manifest = manifest.replace(old, f"set({setting} {cmake_literal(replacement)})", 1)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renderer", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--section", action="append",
                        choices=("existing", "bakes", "switch", "hierarchy", "successive-bakes", "include-shadowing"),
                        help="Run selected groups, using the original matrix by default")
    args = parser.parse_args()
    sections = set(args.section or ("existing", "bakes", "switch", "hierarchy"))
    renderer, output = args.renderer.resolve(), args.output.resolve()
    root = Path(__file__).resolve().parents[2]
    runtime = output / "staged runtime"
    source = output / "isolated source"
    runtime.mkdir(parents=True, exist_ok=True)
    for directory in ("spv_shaders", "hdr", "materials"):
        shutil.copytree(renderer.parent / directory, runtime / directory,
                        dirs_exist_ok=True, copy_function=shutil.copy2)
    for dll in renderer.parent.glob("*.dll"):
        shutil.copy2(dll, runtime)
    executable = Path(shutil.copy2(renderer, runtime))
    for directory in ("shaders", "include"):
        shutil.copytree(root / directory, source / directory, dirs_exist_ok=True)

    manifest = (renderer.parent / "runtime_shader_reload.cmake").read_text(encoding="utf-8")
    manifest = manifest.replace((root / "shaders").as_posix(), (source / "shaders").as_posix())
    manifest = manifest.replace(f"set(source_root [==[{root.as_posix()}]==])",
                                f"set(source_root [==[{source.as_posix()}]==])")
    runtime_manifest = runtime / "runtime_shader_reload.cmake"
    runtime_manifest.write_text(manifest, encoding="utf-8")
    shutil.copy2(renderer.parent / "runtime_shader_reload_tool.txt", runtime)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")

    model = output / "reload-receiver.gltf"
    fixture(model, {"material": {
        "pbrMetallicRoughness": {"baseColorFactor": [0, 0, 0, 1]},
        "emissiveFactor": [.5, .25, .125],
    }})
    receiver = output / "reload-lit-receiver.gltf"
    fixture(receiver, {"material": {"pbrMetallicRoughness": {
        "baseColorFactor": [.7, .7, .7, 1], "metallicFactor": 0,
    }}})
    auxiliary_model = output / "reload-auxiliary-receiver.gltf"
    fixture(auxiliary_model, {"material": extension_material(base=(.7, .7, .7), diffuse=.8)})
    include = source / "shaders" / "display_tone_curves.slang"
    original = include.read_text(encoding="utf-8")
    change = "        return color;"
    assert change in original
    start = original.index("float3 ApplyWhiteBalance(")
    edited = original[:start] + original[start:].replace(
        change, "        return color * float3(0.25, 1.0, 1.0);", 1)

    def capture(technique, label, events, frames, *, extra=(), scene=None, point=None, after_first=None, sequence_data=None):
        base = output / f"{technique}-{label}"
        sequence = base.with_suffix(".json")
        sequence.write_text(json.dumps({"schemaVersion": 1, "frames": frames[-1],
            "sampleFrames": frames, "events": events, **(sequence_data or {})}))
        cmd = [str(executable), "--hidden", "--no-ui", "--validation", "--no-taa",
               "--no-bloom", "--render-technique", technique, "--width", "320", "--height", "240",
               "--model", str(scene or (receiver if label.startswith("compute") else model)),
               "--import-scale", "1", "--camera-position", "0", "0", "4",
               "--camera-target", "0", "0", "0", "--environment-intensity", "0",
               "--directional-intensity", "0", "--exposure", "1", "--display-mode", "lit",
               "--capture-sequence", str(sequence), "--screenshot", str(base.with_suffix(".png"))]
        if label in ("valid", "switch") or label.startswith("compute"):
            cmd += ["--directional-intensity", "1", "--ray-shadows", "hard",
                    "--ddgi", "--ddgi-async", "--ddgi-intensity", "0"]
        if label.startswith("compute"):
            cmd += ["--directional-direction", "0", "0", "-1",
                    "--no-ray-shadow-denoise", "--ray-shadow-debug-layer", "1"]
        if label == "managed":
            cmd += ["--taa"]
        cmd += list(extra)
        for frame in frames:
            stem = base if frame == frames[-1] else Path(f"{base}.frame-{frame:04d}")
            Path(f"{stem}.png").unlink(missing_ok=True)
        log = base.with_suffix(".log")
        with log.open("w", encoding="utf-8") as stream:
            process = subprocess.Popen(cmd, cwd=runtime, stdout=stream, stderr=subprocess.STDOUT,
                                       env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)))
            try:
                if after_first:
                    first_image = Path(f"{base}.frame-{frames[0]:04d}.png")
                    deadline = time.monotonic() + 120
                    while not first_image.exists() and process.poll() is None and time.monotonic() < deadline:
                        time.sleep(.01)
                    if not first_image.exists() or process.poll() is not None:
                        raise RuntimeError(f"Source mutation was not exercised, see {log}")
                    after_first()
                status = process.wait(timeout=240)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
        if status or validation_log_error(log.read_text(encoding="utf-8", errors="replace")):
            raise RuntimeError(f"Vulkan reload capture failed, see {log}")
        outputs = []
        for frame in frames:
            stem = base if frame == frames[-1] else Path(f"{base}.frame-{frame:04d}")
            with Image.open(Path(f"{stem}.png")) as image:
                rgb = np.array(image.convert("RGB").getpixel(point or (image.width // 2, image.height // 2)))
            telemetry = json.loads(Path(f"{stem}.telemetry.json").read_text(encoding="utf-8"))
            outputs.append((rgb, telemetry))
        return outputs

    def verify_retirement(first, repeated):
        for key in ("ownedObjects", "vmaAllocations", "vmaBytes"):
            if first["shaderReload"][key] != repeated["shaderReload"][key]:
                raise RuntimeError(f"Repeated reload retained additional resources in {key}")

    if "existing" in sections:
        for technique in ("deferred-raster", "forward-raster"):
            include.write_text(edited, encoding="utf-8")
            before, after, resized, repeated = capture(technique, "valid", [
                {"frame": 3, "reloadShaders": True},
                {"frame": 5, "resize": [384, 200], "samples": 4},
                {"frame": 7, "reloadShaders": True},
            ], [2, 4, 6, 8])
            if [state["shaderReload"]["generation"] for _, state in (before, after, resized, repeated)] != [0, 1, 1, 2]:
                raise RuntimeError(f"{technique}: candidate generation did not publish")
            if "2 stages compiled" not in after[1]["shaderReload"]["status"]:
                raise RuntimeError(f"{technique}: reload did not isolate the include's dependent stages")
            if int(before[0][0]) - int(after[0][0]) < 12:
                raise RuntimeError(f"{technique}: include edit did not change red radiance")
            np.testing.assert_array_equal(after[0], resized[0])
            np.testing.assert_array_equal(after[0], repeated[0])
            if not resized[1]["swapchainRecreates"] or resized[1]["msaaSamples"] != 4:
                raise RuntimeError(f"{technique}: resize and graphics pipeline rebuild were not exercised")
            if after[1]["ddgi"]["supported"]:
                if (not after[1]["ddgi"]["asyncActive"] or
                        not after[1]["ddgi"]["asyncSubmittedUpdates"] or
                        after[1]["ddgi"]["historyResets"] <= before[1]["ddgi"]["historyResets"]):
                    raise RuntimeError(f"{technique}: reload did not reset asynchronous DDGI")
            if after[1]["rayShadows"]["supported"] and not after[1]["rayShadows"]["active"]:
                raise RuntimeError(f"{technique}: ray shadows were not active during reload")

            include.write_text(original + "\nshader_reload_intentionally_invalid !\n", encoding="utf-8")
            baseline, rejected = capture(technique, "invalid", [{"frame": 3, "reloadShaders": True}], [2, 4])
            np.testing.assert_array_equal(baseline[0], rejected[0])
            status = rejected[1]["shaderReload"]
            if status["generation"] or "display_tone_curves.slang" not in status["status"]:
                raise RuntimeError(f"{technique}: compile failure lacks actionable diagnostics")
            print(f"{technique}: include reload, resize, MSAA rebuild, and rollback passed", flush=True)

        include.write_text(original, encoding="utf-8")
        post = source / "shaders" / "post_process.slang"
        post.write_text(post.read_text(encoding="utf-8").replace("vk::binding(16, 0)", "vk::binding(17, 0)"),
                        encoding="utf-8")
        baseline, rejected = capture("deferred-raster", "interface", [{"frame": 3, "reloadShaders": True}], [2, 4])
        np.testing.assert_array_equal(baseline[0], rejected[0])
        status = rejected[1]["shaderReload"]
        if status["generation"] or "changed its reflected interface" not in status["status"]:
            raise RuntimeError("Incompatible shader interface was not rejected")

        shutil.copy2(root / "shaders" / post.name, post)
        shutil.copy2(root / "shaders" / include.name, include)
        trace = source / "shaders" / "ray_shadow_trace.slang"
        trace.write_text(trace.read_text(encoding="utf-8").replace(
            "gRaw[tid] = visibility;", "gRaw[tid] = tid.z == 0u ? min(visibility, 0.125) : visibility;"),
            encoding="utf-8")
        update = source / "shaders" / "ddgi_update.slang"
        update.write_text(update.read_text(encoding="utf-8").replace(
            "float4(blended, active ? 1.0 : 0.0)", "float4(blended * 0.5, active ? 1.0 : 0.0)"),
            encoding="utf-8")
        for technique in ("deferred-raster", "forward-raster"):
            before, after = capture(technique, "compute", [{"frame": 3, "reloadShaders": True}], [2, 5])
            status = after[1]["shaderReload"]
            if status["generation"] != 1 or "2 stages compiled" not in status["status"]:
                raise RuntimeError(f"{technique}: ray and DDGI compute candidates did not publish: {status}")
            if after[1]["rayShadows"]["supported"]:
                if int(before[0][0]) - int(after[0][0]) < 12:
                    raise RuntimeError(f"{technique}: the new ray visibility did not reach the debug view")
                if after[1]["rayShadows"]["historyResets"] <= before[1]["rayShadows"]["historyResets"]:
                    raise RuntimeError(f"{technique}: ray shadow history did not reset")
            if after[1]["ddgi"]["supported"] and not after[1]["ddgi"]["asyncPublishedUpdates"]:
                raise RuntimeError(f"{technique}: DDGI did not publish after compute reload")
            print(f"{technique}: ray shadow and DDGI compute reload passed", flush=True)

        with update.open("a", encoding="utf-8") as stream:
            stream.write("\nshader_reload_intentionally_invalid !\n")
        baseline, rejected = capture("deferred-raster", "compute-invalid",
                                     [{"frame": 3, "reloadShaders": True}], [2, 5])
        np.testing.assert_array_equal(baseline[0], rejected[0])
        status = rejected[1]["shaderReload"]
        if status["generation"] or "ddgi_update.slang" not in status["status"]:
            raise RuntimeError("Invalid compute candidate did not preserve the active generation")

        for shader in (trace, update):
            shutil.copy2(root / "shaders" / shader.name, shader)
        compose = source / "shaders" / "temporal_compose.slang"
        original = compose.read_text(encoding="utf-8")
        marker = "float4(clamp(color, 0.0.xxx, 65000.0.xxx), reactive)"
        assert marker in original
        compose.write_text(original.replace(marker,
            "float4(clamp(color * float3(0.25, 1.0, 1.0), 0.0.xxx, 65000.0.xxx), reactive)"),
            encoding="utf-8")
        for technique in ("deferred-raster", "forward-raster"):
            before, after, resized, repeated = capture(technique, "managed", [
                {"frame": 3, "reloadShaders": True},
                {"frame": 5, "resize": [384, 200]},
                {"frame": 7, "reloadShaders": True},
            ], [2, 4, 6, 8])
            if int(before[0][0]) - int(after[0][0]) < 12:
                raise RuntimeError(f"{technique}: managed compute shader did not change the image")
            np.testing.assert_array_equal(after[0], resized[0])
            np.testing.assert_array_equal(after[0], repeated[0])
            if "1 managed compute pipeline replaced" not in after[1]["shaderReload"]["status"]:
                raise RuntimeError(f"{technique}: managed compute shader replacement was not isolated")
            if ("0 managed compute pipelines replaced" not in repeated[1]["shaderReload"]["status"]
                    or repeated[1]["shaderReload"]["generation"] != 2):
                raise RuntimeError(f"{technique}: unchanged managed compute pipeline was rebuilt")
            if not resized[1]["swapchainRecreates"]:
                raise RuntimeError(f"{technique}: managed compute resize was not exercised")
            print(f"{technique}: managed temporal compute reload and unchanged reuse passed", flush=True)

        shutil.copy2(root / "shaders" / compose.name, compose)
        velocity = source / "shaders" / "temporal_velocity.slang"
        original_velocity = velocity.read_text(encoding="utf-8")
        velocity.write_text(original_velocity.replace("float4(motion.uv, motion.previousReverseZDepth",
            "float4(motion.uv + float2(0.01, 0.0), motion.previousReverseZDepth"), encoding="utf-8")
        for technique in ("deferred-raster", "forward-raster"):
            before, after, resized = capture(technique, "velocity", [
                {"frame": 3, "reloadShaders": True}, {"frame": 6, "resize": [384, 200]},
            ], [2, 5, 8], extra=("--taa", "--display-mode", "taa-velocity"))
            if int(after[0][0]) - int(before[0][0]) < 12:
                raise RuntimeError(f"{technique}: velocity graphics edit did not reach the motion view")
            if resized[1]["shaderReload"]["generation"] != 1 or not resized[1]["swapchainRecreates"]:
                raise RuntimeError(f"{technique}: velocity graphics generation did not survive resize")
            print(f"{technique}: temporal velocity graphics reload passed", flush=True)
        shutil.copy2(root / "shaders" / velocity.name, velocity)

        auxiliary = source / "shaders" / "extended_material_aux.slang"
        auxiliary.write_text(auxiliary.read_text(encoding="utf-8").replace(
            "output.diffuseTransmission = EncodeExtendedPbrDiffuseStorage(terms);",
            "output.diffuseTransmission = EncodeExtendedPbrDiffuseStorage(terms);\n"
            "        output.diffuseTransmission.rgb *= 0.125;"), encoding="utf-8")
        before, after, rebuilt = capture("deferred-raster", "auxiliary", [
            {"frame": 3, "reloadShaders": True}, {"frame": 6, "samples": 4},
        ], [2, 5, 8], extra=("--directional-intensity", "2", "--directional-direction", "0", "0", "1"),
            scene=auxiliary_model)
        if int(before[0][2]) - int(after[0][2]) < 20:
            raise RuntimeError("Auxiliary material graphics edit did not reach deferred lighting")
        np.testing.assert_array_equal(after[0], rebuilt[0])
        if rebuilt[1]["msaaSamples"] != 4:
            raise RuntimeError("Auxiliary graphics generation was not recreated for MSAA")
        shutil.copy2(root / "shaders" / auxiliary.name, auxiliary)

        debug = source / "shaders" / "ddgi_debug.slang"
        debug.write_text(debug.read_text(encoding="utf-8").replace(
            "return float4(color, 1);", "return float4(lerp(color, float3(1, 0, 1), 0.95), 1);"),
            encoding="utf-8")
        for technique in ("deferred-raster", "forward-raster"):
            captures = capture(technique, "probe-debug", [{"frame": 4, "reloadShaders": True}], [3, 8],
                extra=("--ddgi", "--ddgi-async", "--ddgi-debug", "classification",
                       "--ddgi-origin", "-.75", "-.75", ".75", "--ddgi-spacing", ".75", ".75", ".75",
                       "--ddgi-counts", "3", "3", "3", "--ddgi-probe-radius", ".12"))
            before_path = output / f"{technique}-probe-debug.frame-0003.png"
            after_path = output / f"{technique}-probe-debug.png"
            with Image.open(before_path) as first, Image.open(after_path) as second:
                initial = np.asarray(first.convert("RGB"), dtype=np.int16)
                changed = np.asarray(second.convert("RGB"), dtype=np.int16)
            magenta = (changed[:, :, 0] > 200) & (changed[:, :, 2] > 200) & (changed[:, :, 1] < 80)
            delta = np.max(np.abs(changed - initial), axis=-1) > 20
            if (magenta & delta).sum() < 20 or captures[-1][1]["shaderReload"]["generation"] != 1:
                raise RuntimeError(f"{technique}: DDGI debug graphics edit did not reach probe markers")
            print(f"{technique}: DDGI debug graphics reload passed", flush=True)

    if "bakes" in sections:
        shutil.copy2(root / "shaders" / "ddgi_debug.slang", source / "shaders" / "ddgi_debug.slang")
        panorama = output / "bake-source.exr"
        gradient_preset = output / "bake-gradient.json"
        subprocess.run([str(executable), "--sky", "gradient",
            "--sky-horizon", ".5", ".25", ".125", "--sky-zenith", ".8", ".2", ".1",
            "--sky-ground", ".2", ".1", ".6", "--save-sky-preset", str(gradient_preset)],
            cwd=runtime, check=True, capture_output=True, timeout=20)
        gradient_settings = json.loads(gradient_preset.read_bytes())
        gradient_settings["zenithCurve"] = .4
        gradient_preset.write_text(json.dumps(gradient_settings), encoding="utf-8")
        equirect = source / "shaders" / "equirect_to_cubemap.slang"
        equirect.write_text(equirect.read_text(encoding="utf-8").replace(
            "= float4(radiance, 1.0);", "= float4(radiance * float3(0.25, 1, 1), 1.0);").replace(
            "outCubemap[uint3(dtid.xy, pc.faceIndex)] = color;",
            "outCubemap[uint3(dtid.xy, pc.faceIndex)] = color * float4(0.25, 1, 1, 1);"),
            encoding="utf-8")
        for technique in ("deferred-raster", "forward-raster"):
            for mode in ("hdr", "gradient"):
                write_float_exr(panorama, [[(.5, .25, .125)] * 16 for _ in range(8)])
                flags = ("--environment-intensity", "1", "--environment", str(panorama))
                if mode == "gradient":
                    flags = ("--environment-intensity", "1", "--sky-preset", str(gradient_preset))
                before, after, repeated = capture(technique, f"bake-{mode}", [
                    {"frame": 3, "reloadShaders": True}, {"frame": 7, "reloadShaders": True},
                ], [2, 6, 10], extra=flags, point=(20, 20),
                    after_first=panorama.unlink if mode == "hdr" else None)
                if int(before[0][0]) - int(after[0][0]) < 20:
                    raise RuntimeError(f"{technique}: {mode} environment was not rebaked")
                np.testing.assert_array_equal(after[0], repeated[0])
                verify_retirement(after[1], repeated[1])
                np.testing.assert_array_equal(before[0][1:], after[0][1:])
                if mode == "hdr":
                    np.testing.assert_allclose(before[0], expected_rgb((.5, .25, .125), "aces", 1, 1), atol=2)
                    np.testing.assert_allclose(after[0], expected_rgb((.125, .25, .125), "aces", 1, 1), atol=2)
                    if panorama.exists():
                        raise RuntimeError("HDR source removal did not run")
                print(f"{technique}: {mode} environment rebake passed", flush=True)
        shutil.copy2(root / "shaders" / equirect.name, equirect)

        diffuse_model = output / "bake-diffuse.gltf"
        fixture(diffuse_model, {"material": extension_material(base=(.7, .7, .7), specular=0),
                               "lighting": "environment"})
        specular_model = output / "bake-specular.gltf"
        fixture(specular_model, {"material": extension_material(base=(.7, .7, .7), metallic=1, specular=1),
                                "lighting": "environment"})
        for name, filename, marker, replacement, material in (
            ("irradiance", "irradiance_convolution.slang", "float4(irradiance, 1.0)",
             "float4(irradiance * 0.125, 1.0)", diffuse_model),
            ("prefilter", "prefilter_specular.slang", "float4(prefilteredColor, 1.0)",
             "float4(prefilteredColor * 0.125, 1.0)", specular_model),
            ("brdf", "brdf_lut.slang", "= IntegrateBRDF(NdotV, roughness);",
             "= IntegrateBRDF(NdotV, roughness) * 0.125;", specular_model),
        ):
            shader = source / "shaders" / filename
            original_shader = shader.read_text(encoding="utf-8")
            assert marker in original_shader
            shader.write_text(original_shader.replace(marker, replacement), encoding="utf-8")
            for technique in ("deferred-raster", "forward-raster"):
                write_float_exr(panorama, [[(.5, .25, .125)] * 16 for _ in range(8)])
                before, after, repeated = capture(technique, f"bake-{name}", [
                    {"frame": 3, "reloadShaders": True}, {"frame": 7, "reloadShaders": True},
                ], [2, 6, 10], extra=("--environment-intensity", "1", "--environment", str(panorama)),
                    scene=material)
                if int(before[0][0]) - int(after[0][0]) < 20:
                    raise RuntimeError(f"{technique}: {name} texture was not rebaked")
                np.testing.assert_array_equal(after[0], repeated[0])
                verify_retirement(after[1], repeated[1])
                print(f"{technique}: {name} lighting texture rebake passed", flush=True)
            shutil.copy2(root / "shaders" / filename, shader)

    if "successive-bakes" in sections:
        panorama = output / "successive-bake-source.exr"
        write_float_exr(panorama, [[(.5, .25, .125)] * 16 for _ in range(8)])
        shader = source / "shaders" / "equirect_to_cubemap.slang"
        original_shader = shader.read_text(encoding="utf-8")
        marker = "float4(radiance, 1.0)"
        assert original_shader.count(marker) == 1
        first_source, second_source = (output / f"successive-bake-{stage}.slang" for stage in ("first", "second"))
        for version, scale in ((first_source, ".5"), (second_source, ".125")):
            version.write_text(original_shader.replace(marker, f"float4(radiance * float3({scale}, 1, 1), 1.0)"),
                               encoding="utf-8")
        started = output / "successive-bake-started.txt"
        # Install each edit before its candidate compiler starts. Capture frames
        # can advance at any speed without racing the source updates.
        preamble = f"""cmake_minimum_required(VERSION 3.23)
if(EXISTS {cmake_literal(started)})
    file(COPY_FILE {cmake_literal(second_source)} {cmake_literal(shader)} ONLY_IF_DIFFERENT)
else()
    file(COPY_FILE {cmake_literal(first_source)} {cmake_literal(shader)} ONLY_IF_DIFFERENT)
    file(WRITE {cmake_literal(started)} "1")
endif()
"""
        runtime_manifest.write_text(preamble + manifest, encoding="utf-8")
        try:
            for technique in ("deferred-raster", "forward-raster"):
                started.unlink(missing_ok=True)
                shutil.copy2(root / "shaders" / shader.name, shader)
                captures = capture(technique, "bake-successive", [
                    {"frame": 3, "reloadShaders": True},
                    {"frame": 7, "reloadShaders": True},
                    {"frame": 11, "reloadShaders": True},
                ], [2, 6, 10, 14], extra=("--environment-intensity", "1", "--environment", str(panorama)),
                    point=(20, 20))
                generations = [state["shaderReload"]["generation"] for _, state in captures]
                if generations != [0, 1, 2, 3]:
                    raise RuntimeError(f"{technique}: successive bake generations were {generations}")
                for (pixel, _), red in zip(captures, (.5, .25, .0625, .0625), strict=True):
                    np.testing.assert_allclose(pixel, expected_rgb((red, .25, .125), "aces", 1, 1), atol=2)
                np.testing.assert_array_equal(captures[2][0], captures[3][0])
                verify_retirement(captures[1][1], captures[2][1])
                verify_retirement(captures[2][1], captures[3][1])
                print(f"{technique}: successive distinct environment bakes and unchanged reload passed", flush=True)
        finally:
            shutil.copy2(root / "shaders" / shader.name, shader)
            runtime_manifest.write_text(manifest, encoding="utf-8")

    if "include-shadowing" in sections:
        shaders = source / "shaders"
        shader = shaders / "post_process.slang"
        post = shader.read_text(encoding="utf-8")
        helper_dir = shaders / "reload-shadow" / "nested"
        helper_dir.mkdir(parents=True)
        witness = "reload_shadow_witness.slang"
        shadow = helper_dir / witness
        (helper_dir / "helper.slang").write_text(f'#include "{witness}"\n', encoding="utf-8")
        (shaders / witness).write_text(
            "float3 ReloadShadowTint() { return float3(1.0, 1.0, 1.0); }\n", encoding="utf-8")
        marker = "    float3 positive = clamp(FiniteColorOr(exposedColor,"
        assert post.count(marker) == 1
        shader.write_text('#include "reload-shadow/nested/helper.slang"\n' + post.replace(
            marker, "    exposedColor *= ReloadShadowTint();\n" + marker), encoding="utf-8")
        failures = []
        try:
            shadow_manifest = seed_shadow_baseline(root, runtime, source, output, manifest, shader, shadow)
            runtime_manifest.write_text(shadow_manifest, encoding="utf-8")
            # This is the only source-tree change after the baseline compile.
            # The nested include takes precedence over the original -I hit.
            shadow.write_text(
                "float3 ReloadShadowTint() { return float3(0.25, 1.0, 1.0); }\n", encoding="utf-8")
            for technique in ("deferred-raster", "forward-raster"):
                before, after, repeated = capture(technique, "include-shadowing", [
                    {"frame": 3, "reloadShaders": True}, {"frame": 7, "reloadShaders": True},
                ], [2, 5, 9])
                status = after[1]["shaderReload"]
                if status["generation"] != 1 or int(before[0][0]) - int(after[0][0]) < 12:
                    failures.append(f"{technique}: include shadow did not change red from "
                                    f"{before[0][0]} to {after[0][0]} with {status['status']}")
                    continue
                if repeated[1]["shaderReload"]["generation"] != 2:
                    raise RuntimeError(f"{technique}: repeated shadowed include did not publish")
                np.testing.assert_array_equal(before[0][1:], after[0][1:])
                np.testing.assert_array_equal(after[0], repeated[0])
                verify_retirement(after[1], repeated[1])
                print(f"{technique}: a new transitive include shadowed the recorded baseline", flush=True)
        finally:
            shutil.copy2(root / "shaders" / shader.name, shader)
            for suffix in ("vert", "frag"):
                binary = f"post_process.{suffix}.spv"
                shutil.copy2(renderer.parent / "spv_shaders" / binary, runtime / "spv_shaders" / binary)
            runtime_manifest.write_text(manifest, encoding="utf-8")
        if failures:
            raise RuntimeError("\n".join(failures))

    if "hierarchy" in sections:
        shader = source / "shaders" / "hierarchy_transform.slang"
        original_shader = shader.read_text(encoding="utf-8")
        marker = "objects[index].model = model;"
        assert marker in original_shader
        shader.write_text(original_shader.replace(marker,
            "model[0][3] += 3.0; objects[index].model = model;"), encoding="utf-8")
        for technique in ("deferred-raster", "forward-raster"):
            before, reloaded, cpu, gpu = capture(technique, "hierarchy", [
                {"frame": 3, "reloadShaders": True}, {"frame": 7, "gpuTransforms": False},
                {"frame": 11, "gpuTransforms": True},
            ], [2, 6, 10, 14], extra=("--gpu-transforms",))
            if [state["shaderReload"]["generation"] for _, state in (before,reloaded,cpu,gpu)] != [0,1,1,1]:
                raise RuntimeError(f"{technique}: hierarchy pipeline reload did not publish")
            if [state["hierarchyTransforms"]["active"] for _, state in (before,reloaded,cpu,gpu)] != [True,True,False,True]:
                raise RuntimeError(f"{technique}: hierarchy path switch did not take effect")
            if int(before[0][0])-int(reloaded[0][0]) < 12:
                raise RuntimeError(f"{technique}: hierarchy shader did not move the visible receiver")
            np.testing.assert_array_equal(before[0],cpu[0])
            np.testing.assert_array_equal(reloaded[0],gpu[0])
            verify_retirement(reloaded[1],gpu[1])
            print(f"{technique}: hierarchy shader reload and CPU/GPU restoration passed", flush=True)
        shader.write_text(original_shader, encoding="utf-8")

        blocker_model = output / "gpu-tlas-shadow.gltf"
        fixture(blocker_model, {"material": extension_material(base=(.7,.7,.7)),
                                "blocker": True, "lighting": "directional"})
        asset = json.loads(blocker_model.read_text(encoding="utf-8"))
        next(node for node in asset["nodes"] if node.get("name")=="Analytical back blocker")["translation"]=[.35,.225,2.]
        # This thin sheet blocks either ray-facing direction. Its projected
        # bounds stay beside the center receiver sample in the camera view.
        asset["materials"][-1]["doubleSided"]=True
        blocker_model.write_text(json.dumps(asset), encoding="utf-8")
        ray_shader = source / "shaders" / "ray_instance_transform.slang"
        ray_original = ray_shader.read_text(encoding="utf-8")
        marker = "uint output=index*64;"
        assert marker in ray_original
        ray_shader.write_text(ray_original.replace(marker,"row0.w += 3.0; uint output=index*64;"), encoding="utf-8")
        for technique in ("deferred-raster", "forward-raster"):
            before, reloaded, cpu, gpu = capture(technique, "gpu-tlas", [
                {"frame": 3, "reloadShaders": True}, {"frame": 7, "gpuTransforms": False},
                {"frame": 11, "gpuTransforms": True},
            ], [2,6,10,14], scene=blocker_model,
                extra=("--gpu-transforms","--ray-shadows","hard","--no-ray-shadow-denoise",
                       "--directional-intensity","3","--directional-direction","-.35","-.225","-1",
                       "--ray-shadow-debug-layer","1"))
            if int(reloaded[0][0])-int(before[0][0])<20:
                raise RuntimeError(f"{technique}: GPU TLAS shader edit did not remove the receiver's blocker")
            np.testing.assert_array_equal(before[0],cpu[0])
            np.testing.assert_array_equal(reloaded[0],gpu[0])
            for _,state in (before,reloaded,gpu):
                if state["rayShadows"]["build"]["gpuInstances"]!=2:
                    raise RuntimeError(f"{technique}: GPU TLAS publication was not exercised")
            if cpu[1]["rayShadows"]["build"]["gpuInstances"]:
                raise RuntimeError(f"{technique}: CPU fallback retained GPU instance inputs")
            verify_retirement(reloaded[1],gpu[1])
            print(f"{technique}: GPU TLAS shader reload and CPU ray restoration passed", flush=True)
        ray_shader.write_text(ray_original, encoding="utf-8")

        # Exercise CPU restoration after deferring full object uploads, followed
        # by real compute recovery. Only the isolated staged runtime is edited.
        compiled = runtime / "spv_shaders" / "hierarchy_transform.comp.spv"
        original_spirv = compiled.read_bytes()
        motion = {"objectName": "Accessor probe", "objectTranslation": [
            {"frame":1,"translation":[0,0,0]}, {"frame":2,"translation":[0,0,0]},
            {"frame":3,"translation":[.75,0,0]}, {"frame":8,"translation":[.75,0,0]}]}
        for technique in ("deferred-raster","forward-raster"):
            reference=capture(technique,"gpu-metadata-cpu-reference",[],[2,8],point=(120,120),sequence_data=motion)
            compiled.unlink()
            def restore_shader():
                temporary=compiled.with_suffix(".restore")
                temporary.write_bytes(original_spirv)
                temporary.replace(compiled)
            try:
                fallback,recovered=capture(technique,"gpu-metadata-recovery",[],[2,8],
                    extra=("--gpu-transforms",),point=(120,120),sequence_data=motion,after_first=restore_shader)
            finally:
                compiled.write_bytes(original_spirv)
            if fallback[1]["hierarchyTransforms"]["active"] or "CPU fallback" not in fallback[1]["hierarchyTransforms"]["status"]:
                raise RuntimeError(f"{technique}: missing compute shader did not select CPU restoration")
            if not recovered[1]["hierarchyTransforms"]["active"] or recovered[1]["hierarchyTransforms"]["cpuObjectUploadBytes"]:
                raise RuntimeError(f"{technique}: GPU metadata publication did not recover")
            np.testing.assert_array_equal(reference[0][0],fallback[0])
            np.testing.assert_array_equal(reference[1][0],recovered[0])
            if int(reference[0][0][0])-int(reference[1][0][0])<12:
                raise RuntimeError(f"{technique}: fallback/recovery pose fixture did not move")
            print(f"{technique}: deferred object upload fallback/recovery passed",flush=True)

    if "switch" in sections:
        switches = capture("deferred-raster", "switch", [
            {"frame": 3, "reloadShaders": True},
            {"frame": 5, "technique": "forward-raster"},
            {"frame": 7, "reloadShaders": True},
            {"frame": 9, "technique": "deferred-raster"},
            {"frame": 11, "resize": [384, 200]},
        ], [2, 4, 6, 8, 10, 12])
        if [state["shaderReload"]["generation"] for _, state in switches] != [0, 1, 1, 2, 2, 2]:
            raise RuntimeError("Shader reload generation did not survive technique changes")
        for pixel, _ in switches[1:]:
            np.testing.assert_array_equal(pixel, switches[0][0])
        if not switches[-1][1]["swapchainRecreates"]:
            raise RuntimeError("Technique switching did not exercise resize")
        verify_retirement(switches[1][1], switches[3][1])

    runtime_manifest.unlink()
    baseline, unavailable = capture("forward-raster", "packaged", [{"frame": 3, "reloadShaders": True}], [2, 4])
    np.testing.assert_array_equal(baseline[0], unavailable[0])
    status = unavailable[1]["shaderReload"]
    if status["generation"] or "source build" not in status["status"]:
        raise RuntimeError("Packaged runtime did not explain unavailable reload")


if __name__ == "__main__":
    main()
