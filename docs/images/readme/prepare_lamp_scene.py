"""Add a shadow receiver and key light to the DGG lamp gallery scene.

Input: the complete Khronos LightsPunctualLamp/glTF folder (CC-BY-4.0).
The floor geometry is from this project's MIT-licensed temporal fixture.
"""

import copy
import json
from pathlib import Path
import sys


def prepare_lamp(source: Path) -> Path:
    root = Path(__file__).resolve().parents[3]
    scene = json.loads(source.read_text(encoding="utf-8"))
    fixture = json.loads(
        (root / "models/validation/taa_equivalent.gltf").read_text(encoding="utf-8")
    )
    primitive = copy.deepcopy(fixture["meshes"][0]["primitives"][0])
    buffer_offset = len(scene["buffers"])
    view_offset = len(scene["bufferViews"])
    accessor_offset = len(scene["accessors"])
    material_offset = len(scene["materials"])
    scene["buffers"].extend(fixture["buffers"])
    for view in fixture["bufferViews"]:
        view["buffer"] += buffer_offset
        scene["bufferViews"].append(view)
    for accessor in fixture["accessors"]:
        if "bufferView" in accessor:
            accessor["bufferView"] += view_offset
        scene["accessors"].append(accessor)
    scene["materials"].append(fixture["materials"][primitive["material"]])
    primitive["material"] = material_offset
    primitive["indices"] += accessor_offset
    primitive["attributes"] = {
        key: value + accessor_offset for key, value in primitive["attributes"].items()
    }
    scene["meshes"].append({"name": "Gallery floor", "primitives": [primitive]})
    roots = scene["scenes"][scene.get("scene", 0)]["nodes"]
    scene["nodes"].append({"name": "Gallery floor", "mesh": len(scene["meshes"]) - 1})
    roots.append(len(scene["nodes"]) - 1)
    lights = scene["extensions"]["KHR_lights_punctual"]["lights"]
    for light in lights:
        light["intensity"] *= 0.02
    lights.append(
        {"name": "Gallery key", "type": "point", "color": [1, 0.95, 0.85],
         "intensity": 120, "range": 12}
    )
    scene["nodes"].append(
        {"name": "Gallery key", "translation": [1.8, 3.8, 2],
         "extensions": {"KHR_lights_punctual": {"light": len(lights) - 1}}}
    )
    roots.append(len(scene["nodes"]) - 1)
    scene["asset"]["generator"] += (
        "; README gallery: added floor and key light, reduced fill light intensities"
    )
    destination = source.with_name("LightsPunctualLamp-gallery.gltf")
    destination.write_text(json.dumps(scene, indent=2) + "\n", encoding="utf-8")
    return destination


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: prepare_lamp_scene.py PATH/TO/LightsPunctualLamp.gltf")
    print(prepare_lamp(Path(sys.argv[1]).resolve()))
