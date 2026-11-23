"""Exercise texture and resource history with live ray-shadow and DDGI readers.

An off-axis MASK panel shadows an independent receiver. Its constant texture
alpha changes from one to zero, so the receiver's expected visibility follows
directly from .8 * .5 * textureAlpha >= .3. DDGI executes with zero contribution
to isolate the deterministic direct shadow. Static controls use the same authored
geometry, with independent literal image payloads and no material edits.
"""
from __future__ import annotations

import argparse
import base64
import json
import os
from pathlib import Path
import re
import sys
import tempfile

import numpy as np

from gltf_accessor_runtime_regression import NativeRun, finite_json, load_asset, require
from gltf_extended_materials_runtime_regression import sha, validation_log_error
from gltf_material_authoring_runtime_regression import ray_witness
from gltf_material_textures_runtime_regression import edit, texture_resources
from gltf_merge_runtime_regression import image_pixels, portable_copy
from gltf_resource_retirement_runtime_regression import png, resource_catalog, retire
from gltf_vertex_alpha_runtime_regression import CUTOFF_ARGS, cutoff_roi, write_cutoff_fixture
from scene_node_authoring_runtime_regression import history_cursor


GPU_OPT_IN = "CONTAINER_RUN_GPU_GLTF_MATERIAL_AUTHORING"
TECHNIQUES = ("forward-raster", "deferred-raster")
# Label, bound source texture (-1 means Clear), imported texture phase, history.
STEPS = (
    ("before", 0, "missing", (0, 0)),
    ("imported", 1, "active", (1, 0)),
    ("failed-import", 1, "active", (1, 0)),
    ("undo-import", 0, "missing", (0, 1)),
    ("redo-import", 1, "active", (1, 0)),
    ("cleared", -1, "active", (2, 0)),
    ("deleted", -1, "retired", (3, 0)),
    ("undo-delete", -1, "active", (2, 1)),
    ("undo-clear", 1, "active", (1, 2)),
    ("undo-original-import", 0, "missing", (0, 3)),
    ("redo-original-import", 1, "active", (1, 2)),
    ("redo-clear", -1, "active", (2, 1)),
    ("redo-delete", -1, "retired", (3, 0)),
)
SAMPLER = dict(minFilter=9728, magFilter=9728, wrapS=10497, wrapT=10497)
ORIGINAL_SAMPLER = dict(minFilter=9729, magFilter=9729, wrapS=33071, wrapT=33071)
# Clear changes the binding without allocating or retiring a source resource.
# Delete changes resource phases while preserving every material definition.
BINDING_ONLY_STEPS = {"cleared", "undo-clear", "redo-clear"}
RESOURCE_ONLY_STEPS = {"deleted", "undo-delete", "redo-delete"}
RAY_ARGS = ("--directional-direction", "-1.35", "0", "-1", "--bounce-intensity", "0",
            "--ray-shadows", "hard", "--no-ray-shadow-denoise", "--ddgi",
            "--ddgi-counts", "3", "3", "3", "--ddgi-rays", "4",
            "--ddgi-probes-per-frame", "27", "--ddgi-ray-budget", "108", "--ddgi-intensity", "0")


def generate(output):
    sources = output / "sources"
    sources.mkdir(parents=True, exist_ok=False)
    models = {name: sources / (name + ".gltf") for name in ("covered", "uncovered")}
    for name, path in models.items():
        write_cutoff_fixture(path, .8, .3, shadow="directional", textured_vertex_alpha=.5)
        model = json.loads(path.read_text(encoding="utf-8"))
        if name == "uncovered":
            model["images"][0]["uri"] = "data:image/png;base64," + base64.b64encode(png((255, 255, 255, 0))).decode()
        path.write_text(json.dumps(model, indent=2) + "\n", encoding="utf-8")
    replacement, bad = sources / "zero-alpha.png", sources / "invalid.png"
    replacement.write_bytes(png((255, 255, 255, 0)))
    bad.write_bytes(b"invalid image bytes for the expected decode failure\n")
    failure = edit(27, "baseColor", "importFile", path=str(bad), sampler=SAMPLER)
    failure["expectMaterialTextureFailure"] = True
    events = [
        edit(14, "baseColor", "importFile", path=str(replacement), sampler=SAMPLER), failure,
        dict(frame=40, sceneHistory="undo"), dict(frame=53, sceneHistory="redo"),
        edit(66, "baseColor", "clear"), retire(79, 1),
        dict(frame=92, sceneHistory="undo"), dict(frame=105, sceneHistory="undo"),
        dict(frame=118, sceneHistory="undo"), dict(frame=131, sceneHistory="redo"),
        dict(frame=144, sceneHistory="redo"), dict(frame=157, sceneHistory="redo"),
    ]
    script = output / "texture-ray-history.json"
    script.write_text(json.dumps(dict(schemaVersion=1, frames=13 * len(STEPS),
        sampleFrames=[13 * (index + 1) for index in range(len(STEPS))], events=events), indent=2) + "\n", encoding="utf-8")
    for name, path in models.items():
        model, buffers = load_asset(path)
        alpha = image_pixels(path, model, buffers, 0)[:, :, 3]
        require(np.all(alpha == (255 if name == "covered" else 0)), "Wrong literal alpha control")
        require(model["nodes"][0]["translation"] == [1.35, 0, 1], "Caster placement changed")
        require(len(model["materials"]) == len(model["meshes"]) == 2, "Independent receiver is absent")
    return models, script


def input_hashes(renderer, output):
    directory = Path(__file__).resolve().parent
    helpers = {Path(module.__file__).resolve() for module in tuple(sys.modules.values())
               if getattr(module, "__file__", None) and Path(module.__file__).resolve().parent == directory}
    files = {renderer, *renderer.parent.glob("*.dll"), *(renderer.parent / "spv_shaders").glob("*.spv"),
             *helpers, *(output / "sources").glob("*"), output / "texture-ray-history.json"}
    return {str(path): sha(path) for path in sorted(files)}


def sampled_records(run, last):
    image = Path(last["image"])
    records = {}
    for index, (label, _, _, _) in enumerate(STEPS):
        frame = 13 * (index + 1)
        path = image if index == len(STEPS) - 1 else image.with_name(image.stem + f".frame-{frame:04d}.png")
        telemetry = path.with_suffix(".telemetry.json")
        require(path.is_file() and telemetry.is_file(), "Missing ray-resource sample " + str(path))
        state = json.loads(telemetry.read_text(encoding="utf-8-sig"))
        require(finite_json(state) and state["technique"] == last["state"]["technique"] and
                state["resolution"] == [256, 256] and state["msaaSamples"] == 1 and not state["taa"]["enabled"],
                "Wrong ray-resource capture configuration")
        record = dict(image=str(path), telemetry=str(telemetry), log=last["log"], label=label, frame=frame, state=state)
        records[label] = record
        run.report["samples"].append({key: value for key, value in record.items() if key != "state"})
    return records


def geometry(state):
    merge = state["gltfMerge"]
    require(not merge["authoringNodesTruncated"], "Truncated graph telemetry")
    return dict(graph=merge["graphIdentity"], nodes=merge["authoringNodes"], roots=merge["activeRoots"],
                vertices=state["scene"]["primaryVertexCount"], indices=state["scene"]["primaryIndexCount"])


def check_sampler_policies(state, count, *, imported_retired=False):
    textures = state["gltfMaterialTextures"]
    require(not textures["resourcesTruncated"], "Sampler policy telemetry was truncated")
    rows = {row["sourceTexture"]: row for row in textures["textures"]}
    require(len(rows) == len(textures["textures"]) == count and set(rows) == set(range(count)),
            "Incomplete source texture sampler inventory")
    for index, row in rows.items():
        expected = ORIGINAL_SAMPLER if index == 0 else SAMPLER
        evaluated = -1 if index == 1 and imported_retired else index
        require(row["image"] == row["sampler"] == index and row["evaluatedTexture"] == evaluated and
                all(row[field] == value for field, value in expected.items()),
                "Source texture lost its original sampler policy during history or reload")
    return {str(index): {field: row[field] for field in ORIGINAL_SAMPLER} for index, row in rows.items()}


def publication_clocks(state):
    catalog = state["gltfResourceCatalog"]
    return {**{key: catalog[key] for key in ("resourceRevision", "materialRevision", "authoredMaterialRevision", "generationEpoch")},
            "publications": state["gltfMaterialAuthoring"]["publications"]}


def check_publication_clocks(state, previous, label):
    current = publication_clocks(state)
    if previous is not None:
        succeeded = label != "failed-import"
        resource_delta = int(succeeded and label not in BINDING_ONLY_STEPS)
        material_delta = int(succeeded and label not in RESOURCE_ONLY_STEPS)
        expected = dict(resourceRevision=resource_delta, materialRevision=material_delta,
                        authoredMaterialRevision=material_delta, generationEpoch=int(succeeded), publications=int(succeeded))
        require(all(current[key] - previous[key] == delta for key, delta in expected.items()),
                f"Wrong texture/resource publication clock changes for {label}: {previous} -> {current}")
    return current


def failed_import_state(state):
    return dict(resources=texture_resources(state), catalog=state["gltfResourceCatalog"],
                materials=state["gltfMaterialBindings"], authoring=state["gltfMaterialAuthoring"],
                history=state["gltfMaterialVariants"], historyDepth=state["gltfScenes"]["editHistoryDepth"],
                geometry=geometry(state),
                vma={key: state["gltfMerge"][key] for key in ("vmaAllocationCount", "vmaAllocationBytes")})


def check_state(record, baseline, texture, phase, history, identities):
    state = record["state"]
    require(geometry(state) == geometry(baseline) and state["scene"]["primaryObjectCount"] == 2,
            "Texture or resource edit changed the receiver geometry")
    history_cursor(state, *history, allow_uncreated=True)
    require(state["gltfScenes"]["editHistoryDepth"] == sum(history), "Wrong ray-resource history depth")
    materials = state["gltfMaterialBindings"]
    require(not materials["materialsTruncated"] and len(materials["materials"]) == 2, "Lost a fixture material")
    caster = next(row for row in materials["materials"] if row["sourceMaterial"] == 0)
    for side in ("authored", "evaluated"):
        material = caster[side]
        require(material["alphaMode"] == "MASK" and abs(material["alphaCutoff"] - .3) <= 1e-6 and material["doubleSided"],
                "Texture edit changed MASK or sidedness")
        binding = material["pbrMetallicRoughness"].get("baseColorTexture")
        require(binding is None if texture == -1 else binding is not None and binding["index"] == texture,
                "Material texture differs from the expected source binding")
    properties = caster["materialProperties"]
    require(properties["alphaMasked"] and not properties["transparent"] and properties["doubleSided"],
            "MASK ray classification changed")
    slots = [row for row in state["gltfMaterialTextures"]["slots"]
             if row["sourceMaterial"] == 0 and row["slot"] == "baseColor"]
    require(len(slots) == 1 and slots[0]["authoredTexture"] == texture and slots[0]["evaluatedTexture"] == texture,
            "Published sampled texture role differs from the material")
    catalog, rows = resource_catalog(state)
    textures = state["gltfMaterialTextures"]
    count = 1 if phase == "missing" else 2
    require(not textures["resourcesTruncated"] and textures["resourceRevision"] == catalog["resourceRevision"],
            "Sampled texture and resource catalog revisions differ")
    require(all(textures[field] == count for field in ("archiveImageCount", "archiveTextureCount", "archiveSamplerCount",
                "evaluatedImageCount", "evaluatedTextureCount", "evaluatedSamplerCount", "runtimeTextureCount")),
            "Texture import, retirement or replay changed the wrong resource table")
    for key, row in rows.items():
        identity = row["domainIdentity"], row["incarnation"]
        require(key not in identities or identities[key] == identity, "History changed an original resource incarnation")
        identities[key] = identity
    for domain in ("archive", "evaluated"):
        for kind in ("texture", "image", "sampler"):
            original, imported = (domain, kind, 0), (domain, kind, 1)
            require(original in rows and rows[original]["active"], "Retirement removed an original resource")
            require(imported not in rows if phase == "missing" else
                    imported in rows and rows[imported]["active"] == (phase == "active"),
                    "Wrong imported resource phase after history replay")
    runtime_rows = {row["row"]: row for row in catalog["runtimeRows"]}
    require(len(runtime_rows) == count and runtime_rows[0]["active"], "Original runtime image row was lost")
    if phase != "missing":
        require(runtime_rows[1]["active"] == (phase == "active"), "Imported sampled runtime phase is wrong")
    return dict(history=list(history), sourceTexture=texture, importedPhase=phase,
                resourceRevision=catalog["resourceRevision"], activeRuntimeCount=catalog["activeRuntimeCount"],
                samplerPolicies=check_sampler_policies(state, count, imported_retired=phase == "retired"),
                ray=ray_witness(state, None))


def image_match(actual, expected):
    first, second = cutoff_roi(actual), cutoff_roi(expected)
    error = np.abs(first - second)
    require(np.array_equal(first, second), f"Wrong independent ray-shadow receiver pixels {actual['image']} ({error.max()})")
    return dict(receiverPixelCount=first.shape[0] * first.shape[1], maximumByteDifference=int(error.max()))


def inspect_export(path):
    model, buffers = load_asset(path)
    require(len(model.get("images", [])) == len(model.get("textures", [])) == len(model.get("samplers", [])) == 1,
            "GLB did not remove the imported resource holes")
    caster = model["materials"][model["meshes"][0]["primitives"][0]["material"]]
    require(caster.get("alphaMode") == "MASK" and abs(caster.get("alphaCutoff", .5) - .3) <= 1e-6 and
            "baseColorTexture" not in caster["pbrMetallicRoughness"], "Saved material lost its cleared MASK state")
    require(np.all(image_pixels(path, model, buffers, 0) == 255), "Saved surviving image lost its original white RGBA payload")
    require(model["textures"][0]["source"] == model["textures"][0]["sampler"] == 0, "GLB resource indices are not dense")
    sampler = model["samplers"][0]
    require(all(sampler.get(field, 10497 if field.startswith("wrap") else -1) == value
                for field, value in ORIGINAL_SAMPLER.items()), "Saved original sampler policy changed")
    require(all(not row.get("uri") or row["uri"].startswith("data:")
                for table in ("images", "buffers") for row in model.get(table, [])), "Portable GLB retained an external source URI")
    return dict(path=str(path), sha256=sha(path), sourceIndependent=True, images=1, textures=1, samplers=1,
                samplerPolicy=ORIGINAL_SAMPLER)


def run_native(args, output, models, script):
    renderer = args.renderer.resolve()
    os.environ["VK_LOADER_DEBUG"] = "layer"
    run = NativeRun(renderer, output, args.timeout)
    run.report.update(samples=[], checks=[], exports=[], mode="native", nativeValidationPerformed=False,
        limitations=["DDGI dispatch and reader lifetime are exercised at zero GI intensity. Receiver pixels validate direct ray shadows."])
    frozen = input_hashes(renderer, output)
    run.report["inputSha256"] = frozen
    try:
        for technique in args.technique or TECHNIQUES:
            controls = {name: run.capture(source, technique + "-" + name, technique, lit=True,
                        extra=(*CUTOFF_ARGS, *RAY_ARGS, "--warmup-frames", "12", "--capture-frame", "13"))
                        for name, source in models.items()}
            means = {name: float(cutoff_roi(record).mean()) for name, record in controls.items()}
            require(means["uncovered"] - means["covered"] > 20, f"Vacuous ray-shadow receiver controls {means}")
            for control in controls.values():
                ray_witness(control["state"], None)
            exported = output / (technique + "-retired.glb")
            last = run.capture(models["covered"], technique + "-history", technique, lit=True, destination=exported,
                               extra=(*CUTOFF_ARGS, *RAY_ARGS, "--capture-sequence", str(script)))
            records = sampled_records(run, last)
            identities = {}
            previous = None
            clocks = None
            for label, texture, phase, history in STEPS:
                record = records[label]
                result = check_state(record, records["before"]["state"], texture, phase, history, identities)
                reference = controls["uncovered" if texture == 1 else "covered"]
                result.update(technique=technique, label=label, pixels=image_match(record, reference))
                ray = result["ray"]
                if previous is not None and label != "failed-import":
                    require(ray["rayGeneration"] > previous[0] and ray["ddgiContentEpoch"] > previous[1],
                            "Texture/resource replay kept stale ray or DDGI content")
                previous = ray["rayGeneration"], ray["ddgiContentEpoch"]
                clocks = check_publication_clocks(record["state"], clocks, label)
                result["publicationClocks"] = clocks
                run.report["checks"].append(result)
            failed, imported = records["failed-import"]["state"], records["imported"]["state"]
            error = failed["gltfMaterialTextures"]["lastError"]
            require(error and re.search(r"decode|invalid|fail|unsupported", error, re.I) and
                    failed["gltfMaterialTextures"]["errorSourceMaterial"] == 0, "Missing expected import failure reason")
            require(failed_import_state(failed) == failed_import_state(imported), "Failed image import changed the live scene or history")
            require(failed["rayShadows"]["generation"] == imported["rayShadows"]["generation"] and
                    failed["ddgi"]["contentEpoch"] == imported["ddgi"]["contentEpoch"], "Failed import replaced ray/DDGI reader generations")
            active = resource_catalog(records["cleared"]["state"])[0]["activeRuntimeCount"]
            retired = resource_catalog(records["deleted"]["state"])[0]["activeRuntimeCount"]
            require(retired < active, "Delete retained the unused sampled runtime row")
            run.report["exports"].append(inspect_export(exported))
            copied, files = portable_copy(exported, output / (technique + "-portable"))
            reloaded = run.capture(copied, technique + "-reload", technique, lit=True,
                                   extra=(*CUTOFF_ARGS, *RAY_ARGS, "--warmup-frames", "12", "--capture-frame", "13"))
            ray = ray_witness(reloaded["state"], None)
            policies = check_sampler_policies(reloaded["state"], 1)
            history_cursor(reloaded["state"], 0, 0, allow_uncreated=True)
            run.report["checks"].append(dict(technique=technique, label="portable-reload", ray=ray,
                pixels=image_match(reloaded, controls["covered"]), portableFiles=files, samplerPolicies=policies))
            run.save()
            print(technique + ": ray alpha replacement, failed decode, original history and portable reload passed", flush=True)
        for capture in run.report["captures"]:
            text = Path(capture["log"]).read_text(encoding="utf-8", errors="replace")
            require('Insert instance layer "VK_LAYER_KHRONOS_validation"' in text and not validation_log_error(text),
                    "Strict Vulkan validation or layer loading failed " + capture["log"])
        require(input_hashes(renderer, output) == frozen, "Renderer, helpers, runtime assets or authored fixtures changed during validation")
        run.report["nativeValidationPerformed"] = True
        run.finish()
    except Exception as error:
        run.report["failure"] = str(error)
        raise
    finally:
        run.save()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--managed-output", action="store_true")
    parser.add_argument("--require-opt-in", action="store_true")
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--technique", action="append", choices=TECHNIQUES)
    parser.add_argument("--timeout", type=float, default=240)
    args = parser.parse_args()
    if args.require_opt_in and not args.generate_only and os.environ.get(GPU_OPT_IN) != "1":
        print(f"Set {GPU_OPT_IN}=1 to run live ray material resource validation.", flush=True)
        return 77
    if not args.generate_only and (args.renderer is None or not args.renderer.is_file()):
        parser.error("--renderer must name an existing executable")
    output = args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    print("Ray material resource fixtures and captures " + str(output), flush=True)
    models, script = generate(output)
    if not args.generate_only:
        run_native(args, output, models, script)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
