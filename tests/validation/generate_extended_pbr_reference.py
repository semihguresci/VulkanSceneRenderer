"""Generate reproducible GPU fixtures from the independent binary64 PBR oracle.

No renderer, Vulkan loader, images, external assets, or third-party Python
packages are used. Inputs are rounded to binary32 before oracle evaluation so
the checked JSON describes the exact values uploaded by the C++ contract.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import struct

import extended_pbr_reference as pbr


DEFAULT_OUTPUT = Path(__file__).resolve().parents[1] / "fixtures/rendering/extended_pbr_reference.json"


def binary32(value):
    if isinstance(value, float):
        return struct.unpack("<f", struct.pack("<f", value))[0]
    if isinstance(value, list):
        return [binary32(item) for item in value]
    if isinstance(value, dict):
        return {key: binary32(item) for key, item in value.items()}
    return value


def make_case(name, **changes):
    case = {
        "name": name,
        "material": {
            "baseColor": [0.4, 0.2, 0.6], "metallic": 0.0, "roughness": 0.3,
            "strength": 0.0, "rotation": 0.0, "diffuseFactor": 0.0,
            "transmissionColor": [0.1, 0.8, 0.3], "specularTransmission": 0.0,
            "clearcoat": 0.0, "clearcoatRoughness": 0.25,
            "sheenColor": [0.0, 0.0, 0.0], "sheenRoughness": 0.4,
        },
        "query": {
            "view": [0.2, -0.3, 1.0], "light": [0.65, 0.15, 1.0],
            "f0": [0.03, 0.1, 0.4], "radiance": [2.0, 0.7, 1.3],
            "modelX": 1.0, "tangentSign": 1.0, "faceSign": 1.0,
            "textureRG": [1.0, 0.5], "iblNormalScale": 1.0,
        },
    }
    for key, value in changes.items():
        owner = case["material"] if key in case["material"] else case["query"]
        if key not in owner:
            raise ValueError(f"Unknown fixture field: {key}")
        owner[key] = value
    return case


def evaluate(case, index):
    # Every array element has a different tail, including reserved channels and
    # all six transform rows. These are raw ABI sentinels, never sampled images.
    case["abi"] = {
        "factorsW": (index + 1) / 128.0,
        "colorW": -(index + 1) / 64.0,
        "indices": [11 + index * 3, 23 + index * 5, 47 + index * 7],
        "padding": 0xA5000000 + index,
        "transforms": [
            [(index + 1) / 8.0, (row + 1) / 16.0, (index + row + 1) / 32.0,
             float((index + row) % (2 if row % 2 == 0 else 5))]
            for row in range(6)
        ],
    }
    case = binary32(case)
    material, query = case["material"], case["query"]
    normal = (0.0, 0.0, query["faceSign"])
    world_tangent = (query["modelX"], 0.0, 0.0)
    frame_options = dict(tangent_sign=query["tangentSign"],
                         model_determinant=query["modelX"])
    source = pbr.anisotropy_frame(normal, world_tangent, **frame_options)
    frame = pbr.anisotropy_frame(normal, world_tangent, material["rotation"],
                                 query["textureRG"], **frame_options)
    view, light = pbr.to_local(query["view"], frame), pbr.to_local(query["light"], frame)
    direct = pbr.extended_compatible_direct(
        view, light, material["roughness"], material["strength"], material["baseColor"],
        material["transmissionColor"], material["metallic"], material["specularTransmission"],
        material["diffuseFactor"], query["f0"], query["radiance"])
    cosine = max(0.0, light[2])
    coat_specular = pbr.legacy_compatible_specular(
        view, light, material["clearcoatRoughness"], 0.0, (0.04, 0.04, 0.04))
    coat = [s * r * cosine * material["clearcoat"]
            for s, r in zip(coat_specular, query["radiance"])]
    sheen_roughness = max(0.045, min(1.0, material["sheenRoughness"]))
    sheen_scale = (1.0 - max(0.0, view[2])) ** 2 * (1.0 - 0.35 * sheen_roughness)
    sheen = [c * r * cosine * sheen_scale * 0.35
             for c, r in zip(material["sheenColor"], query["radiance"])]
    coat_base = 1.0 - material["clearcoat"] * 0.04
    sheen_base = 1.0 - min(1.0, max(material["sheenColor"]) * 0.25)
    layered = [(d * coat_base + c) * sheen_base + s for d, c, s in zip(direct, coat, sheen)]
    case["expected"] = {
        "direct": direct, "layered": layered, "coat": coat,
        "tangent": frame[0], "sourceTangent": source[0], "sourceBitangent": source[1],
        "sourceTangentSign": math.copysign(1.0, query["tangentSign"] * query["modelX"]),
        "active": material["strength"] > 0.0 or material["diffuseFactor"] > 0.0,
        "iblNormal": pbr.anisotropic_ibl_normal(
            tuple(n * query["iblNormalScale"] for n in normal), query["view"],
            material["roughness"], material["strength"], frame[0]),
        "diffuseWeights": [*pbr.extended_diffuse_weights(material["strength"],
            material["diffuseFactor"], material["specularTransmission"]), 0.0],
    }
    return case


def document():
    cases = []
    for roughness in (0.045, 0.3, 1.0):
        for strength in (0.0, 1e-6, 0.5, 1.0):
            for label, rotation in (("zero", 0.0), ("plus45", math.pi / 4),
                                    ("minus45", -math.pi / 4)):
                cases.append(make_case(f"aniso-r{roughness:g}-s{strength:g}-{label}",
                                       roughness=roughness, strength=strength, rotation=rotation))
    for roughness in (0.045, 0.3, 1.0):
        for strength in (0.0, 0.5, 1.0):
            cases.append(make_case(f"grazing-r{roughness:g}-s{strength:g}",
                                   roughness=roughness, strength=strength,
                                   view=[1.0, 0.2, 1e-4], light=[-0.7, 0.6, 1e-4]))
    for diffuse in (0.0, 0.25, 1.0):
        for side, light in (("front", [0.4, 0.2, 1.0]), ("back", [0.4, 0.2, -1.0])):
            cases.append(make_case(f"diffuse-{diffuse:g}-{side}", diffuseFactor=diffuse,
                                   metallic=0.2, specularTransmission=0.3, light=light))
    for base in ([1.0, 0.0, 0.0], [0.0, 0.0, 0.0]):
        cases.append(make_case(f"independent-green-back-base{int(base[0])}",
                               baseColor=base, transmissionColor=[0.0, 1.0, 0.0],
                               diffuseFactor=0.7, view=[0.0, 0.0, 1.0], light=[0.0, 0.0, -1.0]))
    cases.extend((
        make_case("black-color-still-reduces-front", diffuseFactor=0.7,
                  transmissionColor=[0.0, 0.0, 0.0]),
        make_case("black-color-transmits-nothing", diffuseFactor=1.0,
                  transmissionColor=[0.0, 0.0, 0.0], light=[0.0, 0.0, -1.0]),
        make_case("metallic-excludes-back-diffuse", diffuseFactor=0.8, metallic=1.0,
                  light=[0.0, 0.0, -1.0]),
        make_case("specular-transmission-excludes-back-diffuse", diffuseFactor=0.8,
                  specularTransmission=1.0, light=[0.0, 0.0, -1.0]),
        make_case("specular-transmission-keeps-front-specular", diffuseFactor=0.8,
                  specularTransmission=1.0),
        make_case("disabled-keeps-legacy-specular-transmission", specularTransmission=0.8),
        make_case("sampled-zero-strength-stays-disabled", strength=pbr.sample_factor(0.8, 0.0),
                  diffuseFactor=pbr.sample_factor(0.0, 0.9)),
    ))
    for sign in (-1.0, 1.0):
        for model in (-1.0, 1.0):
            for face in (-1.0, 1.0):
                cases.append(make_case(f"frame-tangent{sign:g}-model{model:g}-face{face:g}",
                                       strength=0.8, rotation=math.pi / 4,
                                       tangentSign=sign, modelX=model, faceSign=face,
                                       textureRG=[0.5, 1.0], view=[0.2, 0.1, face],
                                       light=[0.4, -0.2, face]))
    for split in (0.0, 0.5, 1.0):
        for side in (-1.0, 1.0):
            cases.append(make_case(f"zero-fresnel-energy-d{split:g}-side{side:g}",
                                   diffuseFactor=split, baseColor=[1.0, 1.0, 1.0],
                                   transmissionColor=[1.0, 1.0, 1.0], f0=[0.0, 0.0, 0.0],
                                   view=[0.0, 0.0, 1.0], light=[0.0, 0.0, side],
                                   radiance=[1.0, 1.0, 1.0]))
    for strength in (0.0, 1.0):
        cases.append(make_case(f"layered-coat-remains-isotropic-s{strength:g}", strength=strength,
                               clearcoat=0.7, clearcoatRoughness=0.35,
                               sheenColor=[0.2, 0.05, 0.1], sheenRoughness=0.25))
    cases.append(make_case("layered-back-diffuse", diffuseFactor=0.7, clearcoat=0.4,
                           sheenColor=[0.1, 0.2, 0.3], light=[0.4, 0.2, -1.0]))
    cases.extend((
        make_case("ibl-raw-projection-magnitude", strength=1.0, roughness=0.5,
                  view=[0.6, 0.8, 0.0]),
        make_case("ibl-parallel-bitangent-fallback", strength=1.0, roughness=0.0,
                  view=[0.0, 1.0, 0.0]),
        make_case("ibl-near-parallel-bitangent", strength=1.0, roughness=0.045,
                  view=[1e-6, 1.0, 1e-6]),
        make_case("ibl-fully-rough-normal", strength=1.0, roughness=1.0,
                  view=[0.2, 0.8, 0.6]),
        make_case("ibl-disabled-preserves-unnormalized-input", strength=0.0,
                  iblNormalScale=2.0),
        make_case("ibl-back-view-oriented-to-normal", strength=1.0, roughness=0.3,
                  view=[0.3, 0.4, -0.5]),
    ))
    return {
        "schemaVersion": 1, "generator": "tests/validation/generate_extended_pbr_reference.py",
        "oracle": "independent binary64 slope-density GGX; separate legacy-compatible cap/Schlick profile",
        "materialStrideBytes": 1040, "queryStrideBytes": 112, "resultStrideBytes": 320,
        "textureSampling": "excluded; supplied sampled factors/colors and texture RG frame directions only",
        "absoluteTolerance": 3e-6, "relativeTolerance": 4e-4,
        "cases": [evaluate(case, index) for index, case in enumerate(cases)],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--check", action="store_true", help="fail if the saved fixture is stale")
    arguments = parser.parse_args()
    fixture = document()
    text = json.dumps(fixture, indent=2, allow_nan=False) + "\n"
    if arguments.check:
        if not arguments.output.is_file() or arguments.output.read_text(encoding="utf-8") != text:
            raise SystemExit(f"Stale extended-PBR reference: {arguments.output}")
    else:
        arguments.output.write_text(text, encoding="utf-8", newline="\n")
    print(f"{'Checked' if arguments.check else 'Generated'} {len(fixture['cases'])} independent cases: {arguments.output}")


if __name__ == "__main__":
    main()
