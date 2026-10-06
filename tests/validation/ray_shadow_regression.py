"""Opt-in runtime ray visibility against independent segment/AABB integration."""
from __future__ import annotations
import argparse
import base64
import copy
import io
import json
import os
from pathlib import Path
import subprocess
import struct

import numpy as np
from PIL import Image
from area_shadow_regression import floor_points, hdr, reference


def blocked(points, emitter, translation=0):
    lower = np.array([-.24 + translation, 0, -.15])
    upper = np.array([.24 + translation, .72, .36])
    direction = np.asarray(emitter) - points
    with np.errstate(divide="ignore", invalid="ignore"):
        a, b = (lower - points) / direction, (upper - points) / direction
    enter = np.maximum(np.minimum(a, b).max(axis=1), 1e-6)
    leave = np.minimum(np.maximum(a, b).min(axis=1), 1 - 1e-6)
    return enter <= leave


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--case", choices=("all", "quality", "coverage", "motion", "lifecycle", "budget"), default="all")
    args = parser.parse_args()
    if os.environ.get("CONTAINER_RUN_GPU_RAY_SHADOW") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_RAY_SHADOW=1")
        return 77
    exe, output = args.exe.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    source = json.loads((exe.parent / "models/validation/cornell_box_local_light.gltf").read_text())
    records = []
    oracle_cache = {}

    def oracle(points, size, height, disk=False, resolution=256, source_x=0):
        key = (points.shape, points.tobytes(), size, height, disk, resolution, source_x)
        if key not in oracle_cache:
            oracle_cache[key] = reference(points, size, height, disk, resolution, source_x)
        return oracle_cache[key]

    def fixture(name, kind="rect", blocker=True, size=(.8, .8)):
        doc = copy.deepcopy(source)
        primitives = doc["meshes"][0]["primitives"]
        doc["meshes"][0]["primitives"] = [primitives[0]]
        doc["meshes"].append({"primitives": primitives[5:]})
        if blocker:
            doc["nodes"].append({"name": "Shadow blocker", "mesh": 1})
            doc["scenes"][0]["nodes"].append(len(doc["nodes"]) - 1)
        light = doc["extensions"]["KHR_lights_punctual"]["lights"][0]
        if kind == "point":
            light.pop("extras", None)
        else:
            light["extras"]["areaLight"].update(shape="disk" if kind == "disk" else "rect",
                width=size[0], height=size[1], radius=size[0] * .5)
        path = output / name / "models/validation/cornell_box_local_light.gltf"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(doc))
        return path

    def capture(name, model, technique, mode, sequence=None, extra=()):
        screenshot = output / (name + ".png")
        for stale in [screenshot, *output.glob(name + ".frame-*.png")]:
            stale.unlink(missing_ok=True)
        command = [str(exe), "--model", str(model), "--hidden", "--no-ui", "--validation",
            "--no-bloom", "--msaa", "1", "--width", "960", "--height", "540",
            "--display-mode", "lit", "--camera-position", "0", "1.05", "4.1",
            "--camera-target", "0", ".95", "0", "--camera-fov", "36",
            "--environment-intensity", "0", "--directional-intensity", "0",
            "--exposure", ".03", "--render-technique", technique, "--no-taa",
            "--ray-shadows", mode, "--ray-shadow-samples", "32",
            "--warmup-frames", "48", "--capture-frame", "49", "--screenshot", str(screenshot), *extra]
        if sequence:
            path = output / (name + "-sequence.json")
            path.write_text(json.dumps(sequence))
            command += ["--capture-sequence", str(path)]
        log = output / (name + ".log")
        with log.open("w") as stream:
            run = subprocess.run(command, cwd=exe.parent,
                env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                stdout=stream, stderr=subprocess.STDOUT, timeout=180)
        text = log.read_text(errors="replace")
        assert run.returncode == 0 and "VUID-" not in text and "SYNC-HAZARD" not in text, log
        telemetry = json.loads(screenshot.with_suffix(".telemetry.json").read_text())
        if mode != "raster" and "--no-ray-query" not in extra:
            if not telemetry["rayShadows"]["supported"]:
                print("SKIP: selected device lacks ray queries")
                raise SystemExit(77)
            assert telemetry["rayShadows"]["active"], telemetry["rayShadows"]
            assert telemetry["rayShadows"]["build"]["instances"] > 0
        return screenshot, telemetry

    for technique in (() if args.case not in ("all", "quality") else ("deferred-raster", "forward-raster")):
        prefix = technique
        model = fixture("rect")
        clear = fixture("clear", blocker=False)
        raster, _ = capture(prefix + "-raster", model, technique, "raster")
        fallback, info = capture(prefix + "-fallback", model, technique, "soft", extra=("--no-ray-query",))
        assert not info["rayShadows"]["active"]
        assert np.array_equal(np.asarray(Image.open(raster)), np.asarray(Image.open(fallback)))
        records.append(dict(technique=technique, fallbackExact=True))
        for name, kind, size in (("rect", "rect", (.8, .8)), ("disk", "disk", (.8, .8)),
                                 ("small-rect", "rect", (.3, .3)),
                                 ("wide-rect", "rect", (1.4, .6)), ("wide-disk", "disk", (1.4, 1.4))):
            model = fixture(name, kind, size=size)
            clear = fixture(name + "-clear", kind, False, size=size)
            white, _ = capture(prefix + "-" + name + "-clear", clear, technique, "soft")
            candidate, telemetry = capture(prefix + "-" + name, model, technique, "soft")
            actual_hdr, clear_hdr = hdr(candidate), hdr(white)
            errors = []
            for row in (485, 490, 495):
                points, mask = floor_points(telemetry, row)
                expected = oracle(points, size, 1.86, kind == "disk")
                actual = np.mean(actual_hdr[row, mask] / np.maximum(clear_hdr[row, mask], 1e-5), axis=1)
                penumbra = (expected > .05) & (expected < .95)
                errors.extend(abs(actual[penumbra] - expected[penumbra]))
            mae, p95 = float(np.mean(errors)), float(np.percentile(errors, 95))
            assert mae < .035 and p95 < .10, (technique, name, mae, p95)
            records.append(dict(technique=technique, emitter=kind, size=size, penumbraMae=mae, p95=p95,
                memory=telemetry["rayShadows"], passes=telemetry.get("passes")))
            print(technique, name, "visibility MAE", mae, "P95", p95, flush=True)
        point = fixture("point", "point")
        debug, telemetry = capture(prefix + "-point", point, technique, "hard",
            extra=("--ray-shadow-debug-layer", "2", "--exposure", "1"))
        image = np.asarray(Image.open(debug).convert("RGB"), dtype=float) / 255
        errors = []
        for row in (485, 490, 495):
            points, mask = floor_points(telemetry, row)
            expected = ~blocked(points, [0, 1.86, .18])
            actual = image[row, mask].mean(axis=1) > .25
            errors.extend(actual != expected)
        assert np.mean(errors) < .005, (technique, "hard point", np.mean(errors))
        records.append(dict(technique=technique, hardPointMismatch=float(np.mean(errors))))
        point_clear = fixture("point-clear", "point", False)
        lit_clear, _ = capture(prefix + "-point-lit-clear", point_clear, technique, "hard")
        lit_shadow, info = capture(prefix + "-point-lit", point, technique, "hard")
        errors = []
        for row in (485, 490, 495):
            points, mask = floor_points(info, row)
            expected = ~blocked(points, [0, 1.86, .18])
            actual = (hdr(lit_shadow)[row, mask] / np.maximum(hdr(lit_clear)[row, mask], 1e-5)).mean(axis=1) > .5
            errors.extend(actual != expected)
        assert np.mean(errors) < .005, (technique, "point lighting integration", np.mean(errors))
        records.append(dict(technique=technique, hardPointShadingMismatch=float(np.mean(errors))))

    def plane_fixture(name, policy="opaque", mirrored=False, double_sided=True):
        path = fixture(name, "point", False)
        doc = json.loads(path.read_text())
        blob = bytearray()

        def accessor(values, components, index=False):
            offset = len(blob)
            flat = values if index else [component for value in values for component in value]
            blob.extend(struct.pack("<" + ("I" if index else "f") * len(flat), *flat))
            view = len(doc["bufferViews"])
            doc["bufferViews"].append(dict(buffer=len(doc["buffers"]), byteOffset=offset, byteLength=len(blob) - offset))
            result = len(doc["accessors"])
            data = dict(bufferView=view, componentType=5125 if index else 5126, count=len(values),
                        type="SCALAR" if index else "VEC" + str(components))
            if components == 3 and not index:
                data.update(min=np.min(values, axis=0).tolist(), max=np.max(values, axis=0).tolist())
            doc["accessors"].append(data)
            return result

        positions = [[-.45, .65, -.25], [-.45, .65, .65], [.45, .65, .65], [.45, .65, -.25]]
        uv = [[0, 0], [0, 1], [1, 1], [1, 0]]
        attrs = dict(POSITION=accessor(positions, 3), NORMAL=accessor([[0, -1 if policy == "back" else 1, 0]] * 4, 3),
                     TEXCOORD_0=accessor(uv, 2), TEXCOORD_1=accessor([[1 - u, v] for u, v in uv], 2))
        indices = accessor([0, 2, 1, 0, 3, 2] if policy == "back" else [0, 1, 2, 0, 2, 3], 1, True)
        material = dict(doubleSided=double_sided, pbrMetallicRoughness=dict(
            baseColorFactor=[1, 1, 1, 1], metallicFactor=0, roughnessFactor=1))
        if policy == "alpha":
            texture = Image.new("RGBA", (2, 2))
            texture.putdata([(255, 255, 255, 255), (255, 255, 255, 0),
                             (255, 255, 255, 0), (255, 255, 255, 255)])
            png = io.BytesIO(); texture.save(png, format="PNG")
            doc["images"] = [dict(uri="data:image/png;base64," + base64.b64encode(png.getvalue()).decode())]
            doc["samplers"] = [dict(magFilter=9728, minFilter=9728, wrapS=10497, wrapT=10497)]
            doc["textures"] = [dict(source=0, sampler=0)]
            material.update(alphaMode="MASK", alphaCutoff=.5)
            material["pbrMetallicRoughness"]["baseColorTexture"] = dict(index=0, extensions={
                "KHR_texture_transform": dict(texCoord=1, offset=[.13, .21], scale=[1.7, .8])})
            doc["extensionsUsed"].append("KHR_texture_transform")
        elif policy == "blend":
            material.update(alphaMode="BLEND")
            material["pbrMetallicRoughness"]["baseColorFactor"][3] = .5
        elif policy == "transmission":
            material["extensions"] = {"KHR_materials_transmission": dict(transmissionFactor=1)}
            doc["extensionsUsed"].append("KHR_materials_transmission")
        doc["materials"].append(material)
        doc["meshes"].append(dict(primitives=[dict(attributes=attrs, indices=indices, material=len(doc["materials"]) - 1)]))
        doc["nodes"].append(dict(name="Thin blocker", mesh=len(doc["meshes"]) - 1,
                                  scale=[-1 if mirrored else 1, 1, 1]))
        doc["scenes"][0]["nodes"].append(len(doc["nodes"]) - 1)
        doc["buffers"].append(dict(byteLength=len(blob), uri="data:application/octet-stream;base64," + base64.b64encode(blob).decode()))
        path.write_text(json.dumps(doc))
        return path

    for technique in (() if args.case not in ("all", "coverage") else ("deferred-raster", "forward-raster")):
        for kind in ("directional", "spot"):
            name = technique + "-" + kind
            model = fixture(name, "point")
            doc = json.loads(model.read_text())
            light = doc["extensions"]["KHR_lights_punctual"]["lights"][0]
            light["type"] = kind
            if kind == "spot": light["spot"] = dict(innerConeAngle=.6, outerConeAngle=1.0)
            else:
                light.pop("range", None)
                doc["nodes"][1]["rotation"] = [-.9238795325, 0, 0, .3826834324]
            model.write_text(json.dumps(doc))
            layer = "1" if kind == "directional" else "2"
            image, telemetry = capture(name, model, technique, "hard",
                extra=("--ray-shadow-debug-layer", layer, "--exposure", "1", "--directional-intensity", "2" if kind == "directional" else "0"))
            errors = []
            for row in (485, 490, 495):
                points, mask = floor_points(telemetry, row)
                emitter = [0, 1.86, .18] if kind == "spot" else points - np.array(telemetry["lighting"]["directionalDirection"]) * 10
                expected = ~blocked(points, emitter)
                errors.extend((hdr(image)[row, mask].mean(axis=1) > .5) != expected)
            mismatch = float(np.mean(errors))
            assert mismatch < .005, (name, mismatch)
            records.append(dict(case="coverage", name=name, mismatch=mismatch))
        for policy, mirrored, sided in (("opaque", False, True), ("opaque", True, True),
                                        ("opaque", False, False), ("back", False, False), ("alpha", False, True),
                                        ("alpha", True, True), ("blend", False, True),
                                        ("transmission", False, True)):
            name = f"{technique}-{policy}-mirror{int(mirrored)}-double{int(sided)}"
            model = plane_fixture(name, policy, mirrored, sided)
            image, telemetry = capture(name, model, technique, "hard",
                extra=("--ray-shadow-debug-layer", "2", "--exposure", "1"))
            pixels = hdr(image).mean(axis=2)
            errors = []
            for row in (485, 490, 495):
                points, mask = floor_points(telemetry, row)
                hit = points + (np.array([0, 1.86, .18]) - points) * (.65 / 1.86)
                inside = (abs(hit[:, 0]) < .45) & (hit[:, 2] > -.25) & (hit[:, 2] < .65)
                if policy in ("blend", "transmission", "back"):
                    inside[:] = False
                if policy == "alpha":
                    u = (hit[:, 0] * (-1 if mirrored else 1) + .45) / .9
                    v = (hit[:, 2] + .25) / .9
                    texel_x = np.floor(np.mod((1 - u) * 1.7 + .13, 1) * 2).astype(int)
                    texel_y = np.floor(np.mod(v * .8 + .21, 1) * 2).astype(int)
                    inside &= texel_x == texel_y
                errors.extend((pixels[row, mask] > .5) != ~inside)
            mismatch = float(np.mean(errors))
            assert mismatch < .015, (name, mismatch)
            records.append(dict(case="coverage", name=name, mismatch=mismatch))
            print(name, "mismatch", mismatch, flush=True)

        # A thin blocker completely outside the camera frustum still casts onto
        # the visible floor. The independent plane intersection ignores culling.
        name = technique + "-offscreen"
        model = plane_fixture(name)
        doc = json.loads(model.read_text())
        doc["nodes"][-1]["translation"] = [6, 0, 0]
        doc["nodes"][1]["translation"] = [17, 1.86, .18]
        doc["extensions"]["KHR_lights_punctual"]["lights"][0]["range"] = 40
        model.write_text(json.dumps(doc))
        image, telemetry = capture(name, model, technique, "hard",
            extra=("--ray-shadow-debug-layer", "2", "--exposure", "1"))
        errors = []
        for row in (485, 490, 495):
            points, mask = floor_points(telemetry, row)
            hit = points + (np.array([17, 1.86, .18]) - points) * (.65 / 1.86)
            inside = (abs(hit[:, 0] - 6) < .45) & (hit[:, 2] > -.25) & (hit[:, 2] < .65)
            assert inside.sum() > 10, "offscreen blocker missed reference rays"
            errors.extend((hdr(image)[row, mask].mean(axis=1) > .5) != ~inside)
        mismatch = float(np.mean(errors))
        assert mismatch < .005, (name, mismatch)
        records.append(dict(case="coverage", name=name, mismatch=mismatch))

    for technique in (() if args.case not in ("all", "motion") else ("deferred-raster", "forward-raster")):
        model = fixture("motion")
        for motion in ("blocker", "light", "camera"):
            samples = [1, 16, 17, 32, 48, 49, 64, 80]
            sequence = dict(schemaVersion=1, frames=80, sampleFrames=samples)
            if motion == "blocker":
                sequence.update(objectName="Shadow blocker", objectTranslation=[dict(frame=f, translation=[x, 0, 0])
                    for f, x in ((1, 0), (16, 0), (17, -.25), (48, -.25), (49, 0), (80, 0))])
            elif motion == "light":
                sequence["events"] = [dict(frame=f, areaLightPosition=[x, 1.86, .18]) for f, x in ((17, .25), (49, 0))]
            else:
                sequence["camera"] = [dict(frame=f, position=[x, 1.05, 4.1], target=[x, .95, 0])
                    for f, x in ((1, 0), (40, .15), (80, 0))]
            name = f"{technique}-{motion}-motion"
            image, _ = capture(name, model, technique, "soft", sequence,
                extra=("--ray-shadow-debug-layer", "6", "--exposure", "1", "--ray-shadow-samples", "8"))
            previous = None
            for frame in samples:
                path = image if frame == 80 else image.with_name(image.stem + f".frame-{frame:04d}.png")
                telemetry = json.loads(path.with_suffix(".telemetry.json").read_text())
                ray = telemetry["rayShadows"]
                if motion != "camera" and frame in (17, 49):
                    assert ray["historyResets"] > previous["historyResets"], (name, frame, ray)
                    if motion == "blocker":
                        assert ray["generation"] > previous["generation"] and ray["build"]["blasBuilt"] == 0
                        assert ray["build"]["blasReused"] == 7
                    else:
                        assert ray["generation"] == previous["generation"]
                errors = []
                visibility = hdr(path).mean(axis=2)
                for row in (485, 490, 495):
                    points, mask = floor_points(telemetry, row)
                    translation = -.25 if motion == "blocker" and 17 <= frame <= 48 else 0
                    source_x = .25 if motion == "light" and 17 <= frame <= 48 else 0
                    expected = oracle(points - np.array([translation, 0, 0]), (.8, .8), 1.86, False, 128, source_x - translation)
                    errors.extend(abs(visibility[row, mask] - expected))
                mae = float(np.mean(errors))
                assert mae < (.085 if frame in (1, 17, 49) else .035), (name, frame, mae)
                records.append(dict(case="motion", name=name, frame=frame, mae=mae, ray=ray, passes=telemetry["passes"]))
                previous = ray
                print(name, frame, "MAE", mae, flush=True)

    if args.case in ("all", "lifecycle"):
        model = fixture("lifecycle")
        empty = fixture("empty", "point", False)
        empty_doc = json.loads(empty.read_text())
        empty_doc["scenes"][0]["nodes"] = [1] # Keep valid geometry assets, with only the light node instantiated.
        empty.write_text(json.dumps(empty_doc))
        models = exe.parent / "models"
        for technique in ("deferred-raster", "forward-raster"):
            alternative = "forward-raster" if technique == "deferred-raster" else "deferred-raster"
            frames = [1, 12, 24, 36, 48, 60, 72, 84, 96, 108, 120, 132, 144, 156, 168, 180, 192, 204]
            sequence = dict(schemaVersion=1, frames=204, sampleFrames=frames, events=[
                dict(frame=8, rayShadows="hard"), dict(frame=20, rayShadows="soft", rayShadowSamples=8),
                dict(frame=32, rayShadows="raster"), dict(frame=44, rayShadows="soft"),
                dict(frame=56, reload=str(empty)), dict(frame=68, reload=str(models / "validation/taa_equivalent.bim")),
                dict(frame=80, reload=str(models / "validation/taa_equivalent.usda")),
                dict(frame=92, reload=str(models / "buildingSMART-IFC5-development/examples/Hello Wall/hello-wall.ifc")),
                dict(frame=104, reload=str(models / "buildingSMART-IFC5-development/examples/Hello Wall/hello-wall.ifcx")),
                dict(frame=116, reload=str(model)), dict(frame=128, samples=4),
                dict(frame=140, samples=1, taa=True), dict(frame=152, resize=[800, 450]),
                dict(frame=164, technique=alternative), dict(frame=176, rayShadows="hard"),
                dict(frame=188, rayShadows="soft", rayShadowSamples=32, rayShadowDenoise=False),
                dict(frame=200, rayShadowDenoise=True)])
            image, _ = capture(technique + "-lifecycle", model, technique, "soft", sequence)
            previous_epoch = None
            for frame in frames:
                path = image if frame == 204 else image.with_name(image.stem + f".frame-{frame:04d}.png")
                telemetry = json.loads(path.with_suffix(".telemetry.json").read_text())
                ray = telemetry["rayShadows"]
                assert ray["active"] == (frame != 36), (technique, frame, ray)
                if frame == 60: assert ray["build"]["instances"] == 0
                elif frame != 36: assert ray["build"]["instances"] > 0
                if frame in (72, 84, 96, 108): assert telemetry["scene"]["auxiliaryObjectCount"] > 0
                if frame in (180, 192, 204): assert telemetry["taa"]["epoch"] > previous_epoch
                previous_epoch = telemetry["taa"]["epoch"]
                records.append(dict(case="lifecycle", technique=technique, frame=frame, ray=ray,
                                    passes=telemetry["passes"]))

    if args.case in ("all", "budget"):
        model = fixture("budget")
        for technique in ("deferred-raster", "forward-raster"):
            clear = fixture("budget-clear", blocker=False)
            white, _ = capture(technique + "-budget-clear", clear, technique, "soft")
            clear_hdr = hdr(white)
            for samples, denoise in ((1, True), (8, True), (32, True), (8, False)):
                extra = ("--ray-shadow-samples", str(samples)) + (() if denoise else ("--no-ray-shadow-denoise",))
                image, info = capture(f"{technique}-budget-{samples}-filter{int(denoise)}", model, technique, "soft", extra=extra)
                errors = []
                for row in (485, 490, 495):
                    points, mask = floor_points(info, row)
                    expected = oracle(points, (.8, .8), 1.86)
                    actual = (hdr(image)[row, mask] / np.maximum(clear_hdr[row, mask], 1e-5)).mean(axis=1)
                    penumbra = (expected > .05) & (expected < .95)
                    errors.extend(abs(actual[penumbra] - expected[penumbra]))
                records.append(dict(case="budget", technique=technique, samples=samples, denoise=denoise,
                                    mae=float(np.mean(errors)), ray=info["rayShadows"], passes=info["passes"]))
            filtered = next(r["mae"] for r in records if r.get("case") == "budget" and r["technique"] == technique and r["samples"] == 8 and r["denoise"])
            raw = next(r["mae"] for r in records if r.get("case") == "budget" and r["technique"] == technique and r["samples"] == 8 and not r["denoise"])
            assert filtered < raw * .8 and filtered < .035, (technique, filtered, raw)
            sequence = dict(schemaVersion=1, frames=24, sampleFrames=[4, 8, 16, 24], objectName="Shadow blocker",
                objectTranslation=[dict(frame=1, translation=[0, 0, 0]), dict(frame=24, translation=[.2, 0, 0])])
            image, _ = capture(technique + "-build-cost", model, technique, "soft", sequence)
            for frame in sequence["sampleFrames"]:
                path = image if frame == 24 else image.with_name(image.stem + f".frame-{frame:04d}.png")
                telemetry = json.loads(path.with_suffix(".telemetry.json").read_text())
                assert telemetry["rayShadows"]["build"]["blasBuilt"] == 0
                assert telemetry["rayShadows"]["build"]["blasReused"] == 7
                records.append(dict(case="build-cost", technique=technique, frame=frame,
                                    ray=telemetry["rayShadows"], passes=telemetry["passes"]))

    for record in records:
        record["validationClean"] = True
    (output / "results.json").write_text(json.dumps(records, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
