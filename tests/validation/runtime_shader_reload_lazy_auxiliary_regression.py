"""Check the first deferred PBR auxiliary pipelines created after a shader reload."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil

from PIL import Image

from gltf_extended_materials_runtime_regression import (
    COLOR, DIFFUSE, extension_material, fixture, require, validation_log_error,
)
from gltf_material_variants_runtime_regression import run


MARKER = "output.diffuseTransmission = EncodeExtendedPbrDiffuseStorage(terms);"
REPLACEMENT = MARKER + "\n        output.diffuseTransmission.rgb *= 0.125;"
CASES = {"reference": [2, 4], "material": [2, 4, 7], "merge": [2, 4, 7]}
PARENT = "First extended-material import"


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def fixtures(output):
    plain = output / "plain-receiver.gltf"
    extended = output / "extended-receiver.gltf"
    offscreen = output / "offscreen-receiver.gltf"
    fixture(plain, dict(material=extension_material(base=(.7, .7, .7)), lighting="directional"))
    fixture(extended, dict(material=extension_material(base=(.7, .7, .7), diffuse=.8), lighting="directional"))
    initial = json.loads(plain.read_text(encoding="utf-8"))
    next(node for node in initial["nodes"] if node.get("mesh") == 0)["translation"] = [6, 0, 0]
    save_json(offscreen, initial)
    edit = dict(sourceMaterial=0, extensions=[dict(extension=DIFFUSE, enabled=True)], values=[
        dict(property=f"extensions/{DIFFUSE}/diffuseTransmissionFactor", value=.8),
        dict(property=f"extensions/{DIFFUSE}/diffuseTransmissionColorFactor", value=COLOR),
    ])
    actions = {"reference": [], "material": [dict(frame=5, materialEdit=edit)],
               "merge": [dict(frame=5, merge=dict(path=str(extended), parentName=PARENT), guiWaitForLoad=True)]}
    inputs = {"reference": extended, "material": plain, "merge": offscreen}
    sequences = {}
    for name, frames in CASES.items():
        sequences[name] = output / f"{name}.sequence.json"
        save_json(sequences[name], dict(schemaVersion=1, frames=frames[-1], sampleFrames=frames,
                                       events=[dict(frame=3, reloadShaders=True), *actions[name]]))
    return inputs, sequences


def stage_runtime(renderer, output):
    root = Path(__file__).resolve().parents[2]
    runtime, source = output / "staged runtime", output / "isolated source"
    runtime.mkdir()
    for directory in ("spv_shaders", "hdr", "materials"):
        shutil.copytree(renderer.parent / directory, runtime / directory)
    for dll in renderer.parent.glob("*.dll"):
        shutil.copy2(dll, runtime)
    executable = Path(shutil.copy2(renderer, runtime))
    for directory in ("shaders", "include"):
        shutil.copytree(root / directory, source / directory)
    manifest = (renderer.parent / "runtime_shader_reload.cmake").read_text(encoding="utf-8")
    source_setting = f"set(source_root [==[{root.as_posix()}]==])"
    require(source_setting in manifest and (root / "shaders").as_posix() in manifest,
            "Build the renderer and runtime shader manifest from this checkout")
    manifest = manifest.replace((root / "shaders").as_posix(), (source / "shaders").as_posix())
    manifest = manifest.replace(source_setting, f"set(source_root [==[{source.as_posix()}]==])")
    (runtime / "runtime_shader_reload.cmake").write_text(manifest, encoding="utf-8")
    shutil.copy2(renderer.parent / "runtime_shader_reload_tool.txt", runtime)
    shader = source / "shaders" / "extended_material_aux.slang"
    original = shader.read_text(encoding="utf-8")
    require(original.count(MARKER) == 1, "Auxiliary shader fixture no longer isolates one assignment")
    shader.write_text(original.replace(MARKER, REPLACEMENT), encoding="utf-8")
    return executable


def center(image_path):
    with Image.open(image_path) as image:
        return list(image.convert("RGB").getpixel((image.width // 2, image.height // 2)))


def diffuse_sources(state):
    return [material["authored"]["extensions"][DIFFUSE]["diffuseTransmissionFactor"]
            for material in state["gltfMaterialBindings"]["materials"]
            if material["active"] and DIFFUSE in material["authored"]["extensions"]]


def prove(report, captured):
    for name, records in captured.items():
        rows = report["cases"][name]
        require([row["generation"] for row in rows] == [0] + [1] * (len(rows) - 1)
                and rows[1]["status"].startswith("Shaders reloaded"), f"{name}: shader reload did not publish")
        first, second, last = records[0]["state"], records[1]["state"], records[-1]["state"]
        if name != "reference":
            require(not diffuse_sources(first) and not diffuse_sources(second),
                    f"{name}: the auxiliary material existed before the staged operation")
        diffuse = diffuse_sources(last)
        require(len(diffuse) == 1 and math.isclose(diffuse[0], .8), f"{name}: expected one active extended material")
        if name == "material":
            authoring = last["gltfMaterialAuthoring"]
            require(authoring["sourceMaterial"] == 0 and authoring["publications"] == first["gltfMaterialAuthoring"]["publications"] + 1,
                    "Material editor did not publish the staged material")
            rows[-1]["materialPublications"] = authoring["publications"]
        if name == "merge":
            merge = last["gltfMerge"]
            require(merge["outcome"] == "committed" and merge["parentName"] == PARENT
                    and merge["generationPublications"] == first["gltfMerge"]["generationPublications"] + 1,
                    "Model merge did not publish the staged material")
            rows[-1]["mergePublications"] = merge["generationPublications"]
    before, edited = [row["rgb"] for row in report["cases"]["reference"]]
    require(before[2] - edited[2] >= 20, "Auxiliary shader edit did not change the reference image")
    for name in ("material", "merge"):
        row = report["cases"][name][-1]
        row["maximumErrorFromReloadedReference"] = max(abs(a-b) for a, b in zip(row["rgb"], edited, strict=True))
    failures = [name for name in ("material", "merge")
                if report["cases"][name][-1]["maximumErrorFromReloadedReference"] > 2]
    require(not failures, "Staged auxiliary pipelines did not use the active shader generation: " + ", ".join(failures))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renderer", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--generate-only", action="store_true")
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True)
    inputs, sequences = fixtures(output)
    report = dict(schemaVersion=1, passed=False, nativeValidationPerformed=False,
                  cases={}, captures=[], captureCount=0, processCount=0)
    try:
        if args.generate_only:
            return
        (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
        report["rendererSha256"] = hashlib.sha256(renderer.read_bytes()).hexdigest()
        executable = stage_runtime(renderer, output)
        captured = {}
        for name, frames in CASES.items():
            records = run(executable, output, inputs[name], name, "deferred-raster", "lit", report["captures"],
                          sequence=sequences[name], timeout=240, require_validation=True, extra=(
                              "--width", "320", "--height", "240", "--import-scale", "1",
                              "--directional-intensity", "2", "--directional-direction", "0", "0", "1",
                              "--no-ray-query", "--ray-shadows", "raster",
                          ))
            require(len(records) == len(frames), f"{name}: wrong native capture count")
            log = Path(records[0]["log"])
            require(not validation_log_error(log.read_text(encoding="utf-8", errors="replace")),
                    f"{name}: Vulkan validation diagnostics in {log}")
            captured[name] = records
            report["processCount"] += 1
            report["cases"][name] = [dict(frame=frame, rgb=center(record["image"]), **record["state"]["shaderReload"])
                                      for frame, record in zip(frames, records, strict=True)]
        report["nativeValidationPerformed"] = True
        prove(report, captured)
        report["passed"] = True
    finally:
        report["captureCount"] = len(report["captures"])
        save_json(output / "results.json", report)


if __name__ == "__main__":
    main()
