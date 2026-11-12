"""Verify glTF capability fallback and rejection across the native import paths."""

import argparse
import copy
import json
import os
from pathlib import Path
import shutil
import subprocess

import numpy as np

from gltf_extended_materials_runtime_regression import validation_log_error
from gltf_linked_assets_runtime_regression import fixture, pixels, write_json
from gltf_material_variants_runtime_regression import run


EXTENSION = "VENDOR_optional_composition_fixture"
INSTANCING = "EXT_mesh_gpu_instancing"
DEFAULT_SCENE = "__default_test_scene__"
DEFAULT_ASSETS = tuple(Path("models/glTF-Sample-Models/2.0") / name / "glTF" / f"{name}.gltf"
                       for name in ("Triangle", "Cube"))
SCENE_PUBLICATION_KEYS = ("graphIdentity", "authoringNodes", "commitGeneration",
                          "descriptorGeneration", "sourceTables", "generationPublications")


def compatible(model, *, required=False):
    result = copy.deepcopy(model)
    result["asset"].update(version="2.1", minVersion="2.0")
    result.setdefault("extensionsUsed", []).append(EXTENSION)
    if required:
        result.setdefault("extensionsRequired", []).append(EXTENSION)
    result.setdefault("extensions", {})[EXTENSION] = {"note": "core fallback"}
    result["meshes"][0]["primitives"][0].setdefault("extensions", {})[EXTENSION] = {"opaqueIndex": 9001}
    return result


def rejected_source(model, path, asset_changes):
    result = copy.deepcopy(model)
    result["asset"].update(asset_changes)
    result["buffers"][0]["uri"] = f"must-not-read-{path.stem}.bin"
    write_json(path, result)


def unavailable_instancing(model):
    result = copy.deepcopy(model)
    result.setdefault("extensionsUsed", []).append(INSTANCING)
    # This data must be ignored by static consumers that use the core mesh.
    result["nodes"][0].setdefault("extensions", {})[INSTANCING] = {"attributes": {}}
    return result


def stage_diagnostic_renderer(renderer, output):
    # The sample assets resolve relative to the executable first. Isolate that
    # directory so a locally installed sample model cannot replace a fixture.
    runtime = output / "diagnostic-runtime"
    runtime.mkdir()
    for directory in ("spv_shaders", "hdr", "materials"):
        shutil.copytree(renderer.parent / directory, runtime / directory)
    for pattern in ("*.dll", "*.so", "*.so.*", "*.dylib"):
        for library in renderer.parent.glob(pattern):
            if library.is_file():
                shutil.copy2(library, runtime / library.name)
    executable = Path(shutil.copy2(renderer, runtime))
    for relative in DEFAULT_ASSETS:
        (runtime / relative).parent.mkdir(parents=True)
    return executable


def check_diagnostic(log, path, code, detail):
    assert any(path.name in line and code in line and detail in line for line in log.splitlines()), log


def check_unchanged(frames):
    initial, *rejected = frames
    assert rejected, frames
    expected = pixels(Path(initial["image"]))
    for frame in rejected:
        np.testing.assert_array_equal(pixels(Path(frame["image"])), expected)
        assert frame["state"]["scene"] == initial["state"]["scene"], frame
        for key in SCENE_PUBLICATION_KEYS:
            assert frame["state"]["gltfMerge"][key] == initial["state"]["gltfMerge"][key], (key, frame)


def reject_auxiliary(command, path, output, technique, rejections):
    command = command.copy()
    image = output / f"bim-required-{technique}.png"
    log = image.with_suffix(".log")
    command[command.index("--bim-model") + 1] = str(path)
    command[command.index("--screenshot") + 1] = str(image)
    with log.open("x", encoding="utf-8") as stream:
        process = subprocess.run(command, cwd=Path(command[0]).parent, stdout=stream,
                                 stderr=subprocess.STDOUT, timeout=180,
                                 env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    text = log.read_text(encoding="utf-8", errors="replace")
    assert process.returncode != 0 and not image.exists(), text
    assert not validation_log_error(text) and "must-not-read-bim-required.bin" not in text, text
    check_diagnostic(text, path, "unsupported_required_extension", INSTANCING)
    assert "unavailable in the active scene consumer" in text, text
    rejections.append(dict(command=command, log=str(log), returnCode=process.returncode))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(strict=True), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    linked, ordinary = fixture(output)
    original = json.loads(ordinary.read_text(encoding="utf-8"))
    primary_optional = output / "primary-optional.gltf"
    write_json(primary_optional, compatible(original))

    base = copy.deepcopy(original)
    base["nodes"] = original["nodes"][:1]
    base["scenes"][0]["nodes"] = [0]
    base_path = output / "merge-base.gltf"
    write_json(base_path, compatible(base))
    right = copy.deepcopy(base)
    right["nodes"] = original["nodes"][1:]
    optional = output / "merge-optional.gltf"
    write_json(optional, compatible(right))
    required = output / "merge-required.gltf"
    mandatory = compatible(right, required=True)
    mandatory["buffers"][0]["uri"] = "must-not-read-required.bin"
    write_json(required, mandatory)
    invalid_min = output / "merge-invalid-min.gltf"
    incompatible = compatible(right)
    incompatible["asset"]["minVersion"] = "2.bad"
    incompatible["buffers"][0]["uri"] = "must-not-read-version.bin"
    write_json(invalid_min, incompatible)

    leaf = linked.parent / "parts/Sample panel.gltf"
    leaf_model = json.loads(leaf.read_text(encoding="utf-8"))
    write_json(leaf, compatible(leaf_model))
    required_leaf = leaf.with_name("Required panel.gltf")
    rejected_source(compatible(leaf_model, required=True), required_leaf, {})
    required_link = linked.with_name("required-link.gltf")
    linked_model = json.loads(linked.read_text(encoding="utf-8"))
    linked_model["files"][0]["uri"] = "parts/Required%20panel.gltf"
    write_json(required_link, linked_model)

    sequence = output / "capability-merges.json"
    write_json(sequence, {"schemaVersion": 1, "frames": 49, "sampleFrames": [1, 13, 25, 37, 49],
                          "events": [{"frame": frame, "merge": {"path": str(path), "parentName": "Imported panel"}}
                                     for frame, path in zip((2, 14, 26, 38),
                                         (optional, required, invalid_min, required_link))]})
    reload_failures = [
        (output / "reload-invalid-version.gltf", "invalid_version", "asset.version", {"version": "2.bad"}),
        (output / "reload-invalid-min.gltf", "invalid_min_version", "asset.minVersion", {"minVersion": "2.bad"}),
        (output / "reload-inconsistent-min.gltf", "inconsistent_min_version", "asset.minVersion", {"minVersion": "2.1"}),
    ]
    for path, _, _, changes in reload_failures:
        rejected_source(original, path, changes)
    reload_sequence = output / "capability-reloads.json"
    reload_sources = [path for path, *_ in reload_failures] + [required_link]
    write_json(reload_sequence, {"schemaVersion": 1, "frames": 49, "sampleFrames": [1, 13, 25, 37, 49],
                                 "events": [{"frame": frame, "reload": str(path)}
                                            for frame, path in zip((2, 14, 26, 38), reload_sources)]})

    diagnostic_renderer = stage_diagnostic_renderer(renderer, output)
    triangle, cube = (diagnostic_renderer.parent / path for path in DEFAULT_ASSETS)
    write_json(cube, base)
    consumer_optional = unavailable_instancing(base)
    consumer_required = copy.deepcopy(consumer_optional)
    consumer_required["extensionsRequired"] = [INSTANCING]
    diagnostic_sources = tuple(output / f"diagnostic-{mode}.gltf"
                               for mode in ("control", "optional", "required"))
    write_json(diagnostic_sources[0], base)
    write_json(diagnostic_sources[1], consumer_optional)
    rejected_source(consumer_required, diagnostic_sources[2], {})
    diagnostic_sequence = output / "diagnostic-reload.json"
    write_json(diagnostic_sequence, {"schemaVersion": 1, "frames": 13, "sampleFrames": [1, 13],
                                     "events": [{"frame": 2, "reload": DEFAULT_SCENE}]})
    bim_control, bim_optional, bim_required = (output / f"bim-{suffix}.gltf"
                                               for suffix in ("control", "optional", "required"))
    write_json(bim_control, base)
    write_json(bim_optional, consumer_optional)
    rejected_source(consumer_required, bim_required, {})

    records = []
    rejections = []
    common = ("--import-scale", "1", "--sky", "plain", "--no-ddgi", "--ray-shadows", "raster")
    linked_flag = "--experimental-gltf-linked-assets"

    for technique in ("deferred-raster", "forward-raster"):
        def capture(label, model, *, scripted=None, links=False, executable=renderer, extra=()):
            name = f"{label}-{technique}"
            flags = (*common, linked_flag, *extra) if links else (*common, *extra)
            result = run(executable, output, model, name, technique, "lit", records, sequence=scripted,
                         extra=flags)
            log = (output / f"{name}.log").read_text(encoding="utf-8", errors="replace")
            assert not validation_log_error(log), log
            return result, log

        control, _ = capture("plain", ordinary)
        expected = pixels(Path(control[0]["image"]))
        main_scene, primary_log = capture("primary-fallback", primary_optional)
        link_scene, link_log = capture("linked-fallback", linked, links=True)
        for frames, log, source_name in ((main_scene, primary_log, primary_optional.name),
                                         (link_scene, link_log, leaf.name)):
            np.testing.assert_array_equal(pixels(Path(frames[0]["image"])), expected)
            assert "newer_minor_version" in log and "unsupported_optional_extension" in log, log
            assert EXTENSION in log and source_name in log, log

        merge, merge_log = capture("merge-fallback", base_path, scripted=sequence, links=True)
        assert len(merge) == 5, merge
        assert not np.array_equal(pixels(Path(merge[0]["image"])), expected)
        for capture_frame in merge[1:]:
            np.testing.assert_array_equal(pixels(Path(capture_frame["image"])), expected)
        baseline, committed, *merge_rejections = [frame["state"]["gltfMerge"] for frame in merge]
        assert committed["outcome"] == "committed" and committed["commitGeneration"] > baseline["commitGeneration"]
        for result, reason in zip(merge_rejections, (EXTENSION, "asset.minVersion", EXTENSION), strict=True):
            assert result["outcome"] == "failed" and reason in result["error"], result
            for key in ("commitGeneration", "descriptorGeneration", "sourceTables", "generationPublications"):
                assert result[key] == committed[key], (key, committed, result)
        optional_warnings = [line for line in merge_log.splitlines() if optional.name in line]
        assert any("newer_minor_version" in line for line in optional_warnings), merge_log
        assert any("unsupported_optional_extension" in line and EXTENSION in line for line in optional_warnings), merge_log
        assert required_leaf.name in merge_log and "unsupported_required_extension" in merge_log, merge_log
        assert "must-not-read-required.bin" not in merge_log and "must-not-read-version.bin" not in merge_log, merge_log

        reload, reload_log = capture("primary-reload", primary_optional, scripted=reload_sequence, links=True)
        assert len(reload) == 5, reload
        np.testing.assert_array_equal(pixels(Path(reload[0]["image"])), expected)
        check_unchanged(reload)
        for path, code, detail, _ in reload_failures:
            check_diagnostic(reload_log, path, code, detail)
        check_diagnostic(reload_log, required_leaf, "unsupported_required_extension", EXTENSION)
        assert "must-not-read-" not in reload_log, reload_log

        shutil.copyfile(diagnostic_sources[0], triangle)
        diagnostic_control, _ = capture("diagnostic-control", DEFAULT_SCENE, executable=diagnostic_renderer)
        shutil.copyfile(diagnostic_sources[1], triangle)
        diagnostic_optional, diagnostic_log = capture("diagnostic-optional", DEFAULT_SCENE,
                                                      executable=diagnostic_renderer)
        check_diagnostic(diagnostic_log, triangle, "unsupported_optional_extension", INSTANCING)
        diagnostic_pixels = pixels(Path(diagnostic_control[0]["image"]))
        assert not np.array_equal(diagnostic_pixels, expected)
        np.testing.assert_array_equal(pixels(Path(diagnostic_optional[0]["image"])), diagnostic_pixels)
        shutil.copyfile(diagnostic_sources[2], triangle)
        diagnostic_reload, diagnostic_log = capture("diagnostic-reload", ordinary, scripted=diagnostic_sequence,
                                                     executable=diagnostic_renderer)
        assert len(diagnostic_reload) == 2, diagnostic_reload
        check_unchanged(diagnostic_reload)
        check_diagnostic(diagnostic_log, triangle, "unsupported_required_extension", INSTANCING)
        assert "must-not-read-" not in diagnostic_log, diagnostic_log

        bim_reference, _ = capture("bim-control", ordinary, extra=("--bim-model", str(bim_control)))
        bim_fallback, bim_log = capture("bim-optional", ordinary, extra=("--bim-model", str(bim_optional)))
        check_diagnostic(bim_log, bim_optional, "unsupported_optional_extension", INSTANCING)
        assert bim_fallback[0]["state"]["scene"]["auxiliaryObjectCount"] == 1, bim_fallback
        bim_pixels = pixels(Path(bim_reference[0]["image"]))
        assert not np.array_equal(bim_pixels, expected)
        np.testing.assert_array_equal(pixels(Path(bim_fallback[0]["image"])), bim_pixels)
        reject_auxiliary(bim_fallback[0]["command"], bim_required, output, technique, rejections)

    assert len(rejections) == 2, rejections
    write_json(output / "results.json", {"passed": True, "captures": records, "rejectedStartup": rejections})
    print(f"PASS shared glTF capability fallback and transactional rejection "
          f"({len(records)} captures, {len(rejections)} rejected starts)")


if __name__ == "__main__":
    main()
