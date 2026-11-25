"""Sample analytical OIT opacity in the live FSR reactive-mask view."""

import json
import math

import numpy as np

from forward_culling_regression import accessor, embed_buffer


EXTENT = (640, 360)
FRAME = 12
PROBES = {"opaque": (280, 240), "singleLayer": (140, 128),
          "overlappingLayers": (380, 128), "maskSurface": (440, 254),
          "maskHole": (530, 254), "emissive": (142, 254)}


def write_fixture(output):
    # Framebuffer rectangles unprojected for the default 60-degree projection
    # and a camera at (0, 0, 3).
    # The entire screen sees opaque depth behind the test surfaces.
    panels = (
        ("Opaque backing", (-32, -32, 672, 392), 4, "OPAQUE", (.2, .3, .5, 1)),
        ("Single alpha layer", (80, 72, 210, 184), 2, "BLEND", (.8, .2, .1, .35)),
        ("Overlap back", (288, 72, 466, 184), 2.5, "BLEND", (.2, .8, .1, .35)),
        ("Overlap front", (288, 72, 466, 184), 2, "BLEND", (.1, .2, .8, .35)),
        ("Opaque alpha test", (404, 210, 479, 298), 2, "MASK", (.8, .5, .1, .75)),
        ("Discarded alpha test", (490, 210, 565, 298), 2, "MASK", (.8, .5, .1, .25)),
        ("Emitter", (80, 210, 210, 298), 2, "OPAQUE", (.1, .1, .1, 1)),
    )
    data = bytearray()
    asset = {"asset": {"version": "2.0"}, "bufferViews": [], "accessors": [],
             "materials": [], "meshes": [], "nodes": [], "scene": 0,
             "scenes": [{"nodes": list(range(len(panels)))}],
             "extensionsUsed": ["KHR_materials_unlit"]}
    indices = accessor(asset, data, [0, 1, 2, 0, 2, 3], 5125, "SCALAR")
    normal = accessor(asset, data, [[0, 0, 1]] * 4, 5126, "VEC3")
    width, height = EXTENT
    for index, (name, (left, top, right, bottom), distance, alpha_mode, rgba) in enumerate(panels):
        pitch = 2 * distance * math.tan(math.radians(60 / 2)) / height
        positions = [[(x - width / 2) * pitch, (height / 2 - y) * pitch, 3 - distance]
                     for x, y in ((left, bottom), (right, bottom), (right, top), (left, top))]
        position = accessor(asset, data, positions, 5126, "VEC3")
        material = {"alphaMode": alpha_mode, "doubleSided": True,
                    "pbrMetallicRoughness": {"baseColorFactor": rgba, "metallicFactor": 0}}
        if name == "Emitter":
            material["emissiveFactor"] = [1, .5, .1]
        else:
            material["extensions"] = {"KHR_materials_unlit": {}}
        if alpha_mode == "MASK":
            material["alphaCutoff"] = .5
        asset["materials"].append(material)
        asset["meshes"].append({"primitives": [{"attributes": {"POSITION": position, "NORMAL": normal},
                                               "indices": indices, "material": index}]})
        asset["nodes"].append({"name": name, "mesh": index})
    embed_buffer(asset, data)
    path = output / "fsr-oit-mask-fixture.gltf"
    path.write_text(json.dumps(asset), encoding="utf-8")
    return path


def encoded_srgb(value):
    return round(255 * (12.92 * value if value <= .0031308 else 1.055 * value ** (1 / 2.4) - .055))


def check_opacity(image, expected, label):
    assert image.shape == (EXTENT[1], EXTENT[0], 3), image.shape
    samples = {}
    for role, (x, y) in PROBES.items():
        target = encoded_srgb(expected[role])
        patch = image[y - 8:y + 9, x - 8:x + 9].astype(np.int16)
        assert np.max(np.abs(patch - target)) <= 2, (label, role, target, np.unique(patch).tolist())
        samples[role] = {"pixelCenter": [x, y], "linearTarget": expected[role],
                         "expected8Bit": target, "measured8Bit": float(np.median(patch))}
    return samples


def verify_fsr_oit_masks(output, capture, check_mode, *, expect_binary=False):
    model = write_fixture(output)
    results = {}
    for technique in ("deferred-raster", "forward-raster"):
        cases = (("quality", .9 if expect_binary else .35,
                  .9 if expect_binary else 1 - (1 - .35) ** 2, .9),
                 ("native", 1., 1., 1.))
        for mode, single, overlap, emitter in cases:
            label = f"oit-opacity-{technique}-{mode}"
            config = {"schemaVersion": 1, "frames": FRAME, "sampleFrames": [FRAME],
                      "camera": [{"frame": 1, "position": [0, 0, 3], "target": [0, 0, 0]}]}
            if mode == "native":
                config["events"] = [{"frame": 1, "taa": True}]
            states, captures = capture(label, mode, config, extent=EXTENT,
                                       technique=technique, display="taa-reactive", model_path=model)
            state = states[FRAME]
            check_mode(state, mode, mode, EXTENT)
            assert state["taa"]["enabled"] == (mode == "native"), state["taa"]
            assert any(item["active"] and item["name"] == "TemporalResolve" for item in state["passes"])
            expected = {"opaque": 0., "singleLayer": single, "overlappingLayers": overlap,
                        "maskSurface": 0., "maskHole": 0., "emissive": emitter}
            results[label] = check_opacity(captures[FRAME], expected, label)
    return {"model": str(model), "expectedBinaryFsr": expect_binary, "captures": results}
