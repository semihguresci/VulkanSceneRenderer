"""Layered-material image responses, raster parity, and MSAA/resize coverage.

The fixtures isolate environment and direct lighting independently. Comparisons
use visible output and authored texture changes rather than shader equations.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
from pathlib import Path
import time

from ltc_area_light_regression import Runtime, case, receiver_fixture
import numpy as np
from PIL import Image


TECHNIQUES = ("deferred-raster", "forward-raster")
PARITY_MEAN_BYTES = 3.0
PARITY_P95_BYTES = 7.0


def build_hashes(executable):
    root = Path(__file__).resolve().parents[2]
    source_files = [
        root / "src/renderer/resources/FrameResourceManager.cpp",
        root / "src/renderer/lighting/LtcLutResources.cpp",
        root / "include/Container/renderer/lighting/SubmittedUploadWait.h",
        Path(__file__).resolve(),
        root / "tests/validation/ltc_area_light_regression.py",
    ]
    source_files += sorted((root / "shaders").rglob("*.slang"))
    runtime_files = sorted((executable.parent / "spv_shaders").rglob("*.spv"))
    assert runtime_files, "Runtime SPIR-V shader files were not found"
    return dict(executable=dict(path=str(executable), sha256=hashlib.sha256(executable.read_bytes()).hexdigest()),
        sourceFiles={str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                     for path in source_files},
        runtimeShaders={str(path.relative_to(executable.parent)): hashlib.sha256(path.read_bytes()).hexdigest()
                        for path in runtime_files})


def texture_fixture(path, settings, extension, field, pixels, *, transform=None,
                    texcoord=0):
    """A nearest-filtered texture with both UV sets supplied explicitly."""
    receiver_fixture(path, settings)
    document = json.loads(path.read_text())
    blob = base64.b64decode(document["buffers"][0]["uri"].split(",", 1)[1])
    primitive = document["meshes"][0]["primitives"][0]
    for index, coordinate in enumerate(([.25, .5], [.75, .5])):
        uv = np.tile(np.asarray(coordinate, dtype="<f4"), (6, 1))
        view_index = len(document["bufferViews"])
        document["bufferViews"].append(dict(buffer=0, byteOffset=len(blob),
                                           byteLength=uv.nbytes))
        blob += uv.tobytes()
        accessor_index = len(document["accessors"])
        document["accessors"].append(dict(bufferView=view_index, componentType=5126,
                                           count=6, type="VEC2"))
        primitive["attributes"][f"TEXCOORD_{index}"] = accessor_index
    document["buffers"][0] = dict(byteLength=len(blob),
        uri="data:application/octet-stream;base64," + base64.b64encode(blob).decode())
    texture_path = path.parent / "layer.png"
    Image.fromarray(np.asarray([pixels], dtype=np.uint8), "RGBA").save(texture_path)
    document["images"] = [dict(uri=texture_path.name)]
    document["samplers"] = [dict(magFilter=9728, minFilter=9728,
                                 wrapS=33071, wrapT=33071)]
    document["textures"] = [dict(source=0, sampler=0)]
    texture_info = dict(index=0, texCoord=texcoord)
    if transform is not None:
        texture_info["extensions"] = dict(KHR_texture_transform=transform)
        document["extensionsUsed"].append("KHR_texture_transform")
    document["materials"][0]["extensions"][extension][field] = texture_info
    path.write_text(json.dumps(document), encoding="utf-8")
    return path


def image_region(path):
    with Image.open(path) as image:
        pixels = np.asarray(image.convert("RGB"), dtype=np.float64)
    height, width = pixels.shape[:2]
    # Stay inside the receiver, away from geometry coverage or the sky.
    return pixels[round(height * .4):round(height * .6),
                  round(width * .425):round(width * .575)]


def difference(first, second):
    delta = np.asarray(second) - np.asarray(first)
    return dict(meanAbsByteDelta=float(np.mean(abs(delta))),
                p95AbsByteDelta=float(np.percentile(abs(delta), 95)),
                maxAbsByteDelta=float(np.max(abs(delta))),
                meanChannelByteDelta=np.mean(delta, axis=(0, 1)).tolist())


def illumination(kind):
    if kind == "ibl":
        return ["--environment-intensity", "1"]
    if kind == "point":
        return []
    assert kind == "direct", kind
    return ["--directional-intensity", "1", "--directional-direction", "0", "0", "-1"]


def validate_isolation(telemetry, kind):
    lighting = telemetry["lighting"]
    assert lighting["pointLightCount"] == int(kind == "point") and lighting["areaLightCount"] == 1, lighting
    assert lighting["bounceIntensity"] == 0, lighting
    assert lighting["environmentIntensity"] == int(kind == "ibl"), lighting
    assert lighting["directionalIntensity"] == int(kind == "direct"), lighting


class MaterialRegression:
    def __init__(self, exe, output, assets):
        self.runtime = Runtime(exe, output, assets)
        self.output = self.runtime.output
        self.captures = {}
        self.capture_count = 0

    def fixture(self, name, settings, texture=None):
        path = self.output / "fixtures" / name / "models/validation/cornell_box_local_light.gltf"
        if texture is None:
            return receiver_fixture(path, settings)
        return texture_fixture(path, settings, **texture)

    def capture(self, name, settings, kind, texture=None):
        model = self.fixture(name, settings, texture)
        if kind == "point":
            document = json.loads(model.read_text())
            document["extensions"]["KHR_lights_punctual"]["lights"].append(
                dict(name="Isolated punctual point light", type="point", color=[1., 1., 1.], intensity=4.))
            document["nodes"].append(dict(name="Punctual point emitter", translation=[0., 0., 1.5],
                extensions=dict(KHR_lights_punctual=dict(light=1))))
            document["scenes"][0]["nodes"].append(len(document["nodes"]) - 1)
            model.write_text(json.dumps(document), encoding="utf-8")
        captures = {}
        for technique in TECHNIQUES:
            image, telemetry = self.runtime.capture(name + "-" + technique, model,
                settings, technique, "sampled", exposure=.2,
                extra=["--no-ray-query", *illumination(kind)])
            validate_isolation(telemetry, kind)
            captures[technique] = image_region(image)
            self.capture_count += 1
        self.parity(name, captures)
        self.captures[name] = captures
        return captures

    def parity(self, name, captures):
        metrics = difference(captures["forward-raster"], captures["deferred-raster"])
        assert metrics["meanAbsByteDelta"] <= PARITY_MEAN_BYTES, (name, "raster parity", metrics)
        assert metrics["p95AbsByteDelta"] <= PARITY_P95_BYTES, (name, "raster parity", metrics)
        self.runtime.record(dict(case="material-parity", name=name,
                                 validationClean=True, **metrics))

    def response(self, name, before, after, minimum=.75):
        for technique in TECHNIQUES:
            metrics = difference(before[technique], after[technique])
            assert metrics["meanAbsByteDelta"] >= minimum, (name, technique,
                "authored layer change has no visible response", metrics)
            self.runtime.record(dict(case="material-response", name=name,
                technique=technique, minimumMeanAbsByteDelta=minimum,
                validationClean=True, **metrics))

    def core(self):
        base = dict(intensity=0., albedo=[.05, .05, .05], roughness=.8)
        for layer, changes, camera in (
                ("sheen", dict(sheen=[1., 0., 0.]), [4., 0., .8]),
                ("clearcoat", dict(clearcoat=1., clearcoatRoughness=.3), [0., 0., 5.])):
            off = case(layer + "-ibl-off", camera=camera, **base)
            on = case(layer + "-ibl-on", camera=camera, **base, **changes)
            before = self.capture(off["name"], off, "ibl")
            after = self.capture(on["name"], on, "ibl")
            self.response(layer + "-ibl", before, after)

        metallic = dict(intensity=0., metallic=1., albedo=[.4, .4, .4], roughness=.1,
            iridescenceIor=1.33, iridescenceThicknessMinimum=100.,
            iridescenceThicknessMaximum=400., doubleSided=False)
        for kind in ("direct", "ibl"):
            off = case("metal-iridescence-" + kind + "-off", **metallic)
            on = case("metal-iridescence-" + kind + "-on", iridescence=1., **metallic)
            before = self.capture(off["name"], off, kind)
            after = self.capture(on["name"], on, kind)
            self.response("metal-iridescence-" + kind, before, after)

    def textures(self):
        descriptions = (
            ("clearcoat", "KHR_materials_clearcoat", "clearcoatTexture", "direct",
             dict(clearcoat=1., clearcoatRoughness=.3), (0, 0, 0, 255), (255, 255, 255, 255)),
            ("clearcoat-roughness", "KHR_materials_clearcoat", "clearcoatRoughnessTexture", "direct",
             dict(clearcoat=1., clearcoatRoughness=.8), (255, 0, 255, 255), (255, 255, 255, 255)),
            ("sheen-color", "KHR_materials_sheen", "sheenColorTexture", "ibl",
             dict(sheen=[1., 0., 0.], camera=[4., 0., .8]), (0, 0, 0, 255), (255, 255, 255, 255)),
            ("sheen-roughness", "KHR_materials_sheen", "sheenRoughnessTexture", "ibl",
             dict(sheen=[1., 0., 0.], sheenRoughness=1., camera=[4., 0., .8]),
             (255, 255, 255, 0), (255, 255, 255, 255)),
            ("iridescence-factor", "KHR_materials_iridescence", "iridescenceTexture", "direct",
             dict(metallic=1., albedo=[.4, .4, .4], roughness=.1, iridescence=1., iridescenceIor=1.33),
             (0, 255, 255, 255), (255, 255, 255, 255)),
            ("iridescence-thickness", "KHR_materials_iridescence", "iridescenceThicknessTexture", "ibl",
             dict(metallic=1., albedo=[.4, .4, .4], roughness=.1, iridescence=1., iridescenceIor=1.33),
             (255, 0, 255, 255), (255, 255, 255, 255)),
        )
        for name, extension, field, kind, changes, low, high in descriptions:
            settings = case(name, intensity=0., **dict(dict(albedo=[.05] * 3, roughness=.8), **changes))
            captures = []
            for label, pixel in (("low", low), ("high", high)):
                texture = dict(extension=extension, field=field, pixels=[pixel])
                captures.append(self.capture(name + "-texture-" + label, settings, kind, texture))
            self.response(name + "-texture", *captures)

        # The same two-texel image must select the authored UV set and transform.
        settings = case("clearcoat-uv", intensity=0., albedo=[.05] * 3,
                        roughness=.8, clearcoat=1., clearcoatRoughness=.3)
        texture = dict(extension="KHR_materials_clearcoat", field="clearcoatTexture",
                       pixels=[(0, 0, 0, 255), (255, 255, 255, 255)])
        untransformed = self.capture("clearcoat-uv0", settings, "direct", texture)
        transformed = self.capture("clearcoat-transform", settings, "direct",
            dict(texture, transform=dict(offset=[.5, 0.])))
        uv1 = self.capture("clearcoat-uv1", settings, "direct", dict(texture, texcoord=1))
        self.response("clearcoat-uv-transform", untransformed, transformed)
        for technique in TECHNIQUES:
            for name, actual, expected in (
                ("uv0-low", untransformed, self.captures["clearcoat-texture-low"]),
                ("transform-high", transformed, self.captures["clearcoat-texture-high"]),
                ("uv1-high", uv1, self.captures["clearcoat-texture-high"])):
                metrics = difference(expected[technique], actual[technique])
                assert metrics["meanAbsByteDelta"] <= .5, (name, technique, metrics)
                self.runtime.record(dict(case="material-texture-coordinates", name=name,
                    technique=technique, validationClean=True, **metrics))

    def point(self):
        settings = case("clearcoat-point", intensity=0., albedo=[.05] * 3,
                        roughness=.8, clearcoat=1., clearcoatRoughness=.3)
        captures = []
        for label, pixel in (("low", (0, 0, 0, 255)), ("high", (255, 255, 255, 255))):
            texture = dict(extension="KHR_materials_clearcoat", field="clearcoatTexture", pixels=[pixel])
            captures.append(self.capture("clearcoat-point-" + label, settings, "point", texture))
        self.response("clearcoat-punctual-point-texture", *captures)

    def lifecycle(self):
        settings = case("layered-lifecycle", intensity=0., albedo=[.15, .2, .25],
            metallic=.5, roughness=.5, clearcoat=.8, clearcoatRoughness=.2,
            sheen=[.2, .1, .05], iridescence=1.)
        model = self.fixture("layered-lifecycle", settings,
            dict(extension="KHR_materials_clearcoat", field="clearcoatTexture",
                 pixels=[(255, 255, 255, 255)]))
        sequence = dict(schemaVersion=1, frames=41, sampleFrames=[17, 25, 33, 41], events=[
            dict(frame=21, samples=4), dict(frame=29, resize=[400, 300]),
            dict(frame=37, samples=1, resize=[320, 240])])
        captures = {frame: {} for frame in sequence["sampleFrames"]}
        for technique in TECHNIQUES:
            image, _ = self.runtime.capture("layered-lifecycle-" + technique, model,
                settings, technique, "sampled", exposure=.2, sequence=sequence,
                extra=["--no-ray-query", "--environment-intensity", "1"])
            self.capture_count += 1
            for frame in sequence["sampleFrames"]:
                path = image if frame == 41 else image.with_name(image.stem + f".frame-{frame:04d}.png")
                telemetry = json.loads(path.with_suffix(".telemetry.json").read_text())
                validate_isolation(telemetry, "ibl")
                assert telemetry["msaaSamples"] == (4 if frame in (25, 33) else 1), telemetry
                assert telemetry["resolution"] == ([400, 300] if frame == 33 else [320, 240]), telemetry
                captures[frame][technique] = image_region(path)
                current_mean = captures[frame][technique].mean((0, 1))
                baseline_mean = captures[17][technique].mean((0, 1))
                error = float(np.mean(abs(current_mean - baseline_mean)))
                assert error <= 3., ("layer values lost after MSAA or resize", frame, technique, error)
                self.runtime.record(dict(case="material-lifecycle", technique=technique,
                    frame=frame, msaa=telemetry["msaaSamples"], resolution=telemetry["resolution"],
                    meanChannelDriftBytes=error, validationClean=True))
        for frame, images in captures.items():
            self.parity("layered-lifecycle-" + str(frame), images)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--assets", type=Path, default=Path(__file__).resolve().parents[2] / "materials/ltc")
    parser.add_argument("--case", choices=("all", "core", "textures", "point", "lifecycle"), default="all")
    args = parser.parse_args()
    started = time.monotonic()
    regression = MaterialRegression(args.exe, args.output, args.assets)
    initial_hashes = build_hashes(args.exe.resolve())
    summary = dict(executable=str(args.exe.resolve()),
        executableSha256=initial_hashes["executable"]["sha256"], requestedCase=args.case,
        parityMeanByteLimit=PARITY_MEAN_BYTES, parityP95ByteLimit=PARITY_P95_BYTES)
    try:
        for name in ("core", "textures", "point", "lifecycle"):
            if args.case in ("all", name):
                getattr(regression, name)()
        assert build_hashes(args.exe.resolve()) == initial_hashes, "Build or source files changed during capture"
        (regression.output / "build-hashes.json").write_text(json.dumps(initial_hashes, indent=2) + "\n")
        summary["buildHashes"] = "build-hashes.json"
        summary["passed"] = True
    except Exception as error:
        summary.update(passed=False, failure=str(error))
        raise
    finally:
        summary.update(captureCount=regression.capture_count,
                       recordCount=len(regression.runtime.records),
                       elapsedSeconds=time.monotonic() - started)
        (regression.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary), flush=True)


if __name__ == "__main__":
    main()
