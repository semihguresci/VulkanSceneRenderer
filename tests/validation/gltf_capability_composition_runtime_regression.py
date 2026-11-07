"""Verify compatible glTF fallback through primary, merge, and linked import."""

import argparse
import copy
import json
from pathlib import Path

import numpy as np

from gltf_extended_materials_runtime_regression import validation_log_error
from gltf_linked_assets_runtime_regression import fixture, pixels, write_json
from gltf_material_variants_runtime_regression import run


EXTENSION = "VENDOR_optional_composition_fixture"


def compatible(model, *, required=False):
    result = copy.deepcopy(model)
    result["asset"].update(version="2.1", minVersion="2.0")
    result.setdefault("extensionsUsed", []).append(EXTENSION)
    if required:
        result.setdefault("extensionsRequired", []).append(EXTENSION)
    result.setdefault("extensions", {})[EXTENSION] = {"note": "core fallback"}
    result["meshes"][0]["primitives"][0].setdefault("extensions", {})[EXTENSION] = {"opaqueIndex": 9001}
    return result


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
    write_json(required_leaf, compatible(leaf_model, required=True))
    required_link = linked.with_name("required-link.gltf")
    linked_model = json.loads(linked.read_text(encoding="utf-8"))
    linked_model["files"][0]["uri"] = "parts/Required%20panel.gltf"
    write_json(required_link, linked_model)

    sequence = output / "capability-merges.json"
    write_json(sequence, {"schemaVersion": 1, "frames": 49, "sampleFrames": [1, 13, 25, 37, 49],
                          "events": [{"frame": frame, "merge": {"path": str(path), "parentName": "Imported panel"}}
                                     for frame, path in zip((2, 14, 26, 38),
                                         (optional, required, invalid_min, required_link))]})
    records = []
    common = ("--import-scale", "1", "--sky", "plain", "--no-ddgi", "--ray-shadows", "raster")
    linked_flag = "--experimental-gltf-linked-assets"

    for technique in ("deferred-raster", "forward-raster"):
        def capture(label, model, *, scripted=None, links=False):
            name = f"{label}-{technique}"
            result = run(renderer, output, model, name, technique, "lit", records, sequence=scripted,
                         extra=(*common, linked_flag) if links else common)
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
        baseline, committed, *rejections = [frame["state"]["gltfMerge"] for frame in merge]
        assert committed["outcome"] == "committed" and committed["commitGeneration"] > baseline["commitGeneration"]
        for result, reason in zip(rejections, (EXTENSION, "asset.minVersion", EXTENSION)):
            assert result["outcome"] == "failed" and reason in result["error"], result
            for key in ("commitGeneration", "descriptorGeneration", "sourceTables", "generationPublications"):
                assert result[key] == committed[key], (key, committed, result)
        optional_warnings = [line for line in merge_log.splitlines() if optional.name in line]
        assert any("newer_minor_version" in line for line in optional_warnings), merge_log
        assert any("unsupported_optional_extension" in line and EXTENSION in line for line in optional_warnings), merge_log
        assert required_leaf.name in merge_log and "unsupported_required_extension" in merge_log, merge_log
        assert "must-not-read-required.bin" not in merge_log and "must-not-read-version.bin" not in merge_log, merge_log

    write_json(output / "results.json", {"passed": True, "captures": records})
    print(f"PASS shared glTF capability fallback and rejected merge publication ({len(records)} captures)")


if __name__ == "__main__":
    main()
