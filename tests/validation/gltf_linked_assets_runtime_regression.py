"""Validate native linked glTF placement, alias reads, picks and portable save."""

import argparse
import base64
import copy
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image

from gltf_accessor_runtime_regression import Asset, load_asset, mesh, textured_material
from gltf_material_variants_runtime_regression import run


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2), encoding="utf-8")


def pixels(path):
    with Image.open(path) as image:
        return np.array(image.convert("RGB"))


def fixture(output):
    folder = output / "linked-inputs"
    leaf_dir = folder / "parts"
    leaf_dir.mkdir(parents=True)
    leaf_path = leaf_dir / "Sample panel.gltf"
    asset = Asset()
    mesh(asset, asset.accessor("VEC3", [[-.3, -.28, 0], [.3, -.28, 0], [.3, .28, 0], [-.3, .28, 0]]),
         asset.accessor("VEC3", [[0, 0, 1]] * 4), asset.accessor("VEC2", [[0, 0], [1, 0], [1, 1], [0, 1]]))
    textured_material(asset)
    asset.model["nodes"][0]["name"] = "Aliased textured panel"
    reference = output / "ordinary.gltf"
    positions = [[-.75, 0, 0], [.75, 0, 0]]
    asset.model["nodes"] = [{"name": "Reference panel", "mesh": 0, "translation": position} for position in positions]
    asset.model["scenes"][0]["nodes"] = [0, 1]
    asset.write(reference)

    leaf = copy.deepcopy(asset.model)
    leaf["nodes"] = [{"name": "Aliased textured panel", "mesh": 0}]
    leaf["scenes"][0]["nodes"] = [0]
    for table, actual, alias in (("buffers", "Shared mesh.bin", "virtual%20mesh.bin"),
                                 ("images", "Shared image.png", "virtual%20image.png")):
        record = leaf[table][0]
        (folder / actual).write_bytes(base64.b64decode(record["uri"].split(",", 1)[1]))
        record["uri"] = alias
    write_json(leaf_path, leaf)
    primary = folder / "linked.gltf"
    write_json(primary, {
        "asset": {"version": "2.1", "minVersion": "2.1"},
        "files": [
            {"uri": "parts/Sample%20panel.gltf", "mimeType": "model/gltf+json",
             "aliases": [{"alias": "virtual%20mesh.bin", "file": 1}, {"alias": "virtual%20image.png", "file": 2}]},
            {"uri": "Shared%20mesh.bin", "mimeType": "application/octet-stream"},
            {"uri": "Shared%20image.png", "mimeType": "image/png"},
        ],
        "externalAssets": [{"file": 0}],
        "nodes": [{"name": f"Placement {i}", "externalAsset": 0, "translation": position}
                  for i, position in enumerate(positions)],
        "scenes": [{"nodes": [0, 1]}], "scene": 0,
    })
    return primary, reference


def verify_picks(state):
    instancing = state["gltfInstancing"]
    assert instancing["sourceVertexCount"] == 4 and instancing["sourceIndexCount"] == 6, instancing
    assert instancing["drawCount"] == 2, instancing
    picks = state["gltfNodeFlags"]["probes"]
    assert len(picks) == 2 and len({pick["sourceNodeIndex"] for pick in picks}) == 2, picks
    for pick in picks:
        assert pick["projected"] and pick["hit"] and pick["nearestHitGraphNode"] == pick["graphNode"], pick
        assert pick["resolvedSelection"] == pick["graphNode"], pick


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    renderer_digest = hashlib.sha256(renderer.read_bytes()).hexdigest()
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    primary, ordinary = fixture(output)
    saved = output / "portable.glb"
    records = []
    common = ("--import-scale", "1", "--sky", "plain")
    references = {}
    for technique in ("deferred-raster", "forward-raster"):
        linked = run(renderer, output, primary, f"linked-{technique}", technique, "lit", records,
                     save=saved if technique == "deferred-raster" else None,
                     extra=(*common, "--experimental-gltf-linked-assets"))[0]
        independent = run(renderer, output, ordinary, f"reference-{technique}", technique, "lit", records, extra=common)[0]
        verify_picks(linked["state"])
        references[technique] = pixels(Path(independent["image"]))
        np.testing.assert_array_equal(pixels(Path(linked["image"])), references[technique])

    model, _ = load_asset(saved)
    assert model["asset"]["version"] == "2.0" and "externalAssets" not in model and "files" not in model
    assert sum("containerLinkedSource" in node.get("extras", {}) for node in model["nodes"]) == 2
    primary.parent.rename(output / "original-inputs-removed")
    for technique in references:
        restored = run(renderer, output, saved, f"reloaded-{technique}", technique, "lit", records, extra=common)[0]
        verify_picks(restored["state"])
        np.testing.assert_array_equal(pixels(Path(restored["image"])), references[technique])
    assert hashlib.sha256(renderer.read_bytes()).hexdigest() == renderer_digest
    write_json(output / "results.json", {"passed": True, "rendererSha256": renderer_digest, "captures": records})
    print("PASS linked shared geometry, two production CPU picks and pixel-exact portable reload", flush=True)


if __name__ == "__main__":
    main()
