"""Small, independently authored glTF fixtures for display/capture checks."""

import base64
import json
import re
import struct


def validation_log_error(text):
    return bool(re.search(
        r"VUID-|SYNC-HAZARD|Validation Error|\bhazard detected\b|"
        r"\b(?:READ|WRITE)_AFTER_(?:READ|WRITE)\b[^\r\n]*\bhazard\b",
        text, re.I))


class Asset:
    def __init__(self):
        self.data = bytearray()
        self.model = dict(asset=dict(version="2.0", generator="Container display capture regression"),
                          scene=0, scenes=[dict(nodes=[0])], nodes=[dict(mesh=0)],
                          meshes=[], materials=[], bufferViews=[], accessors=[])

    def accessor(self, kind, values, component=5126, *, indices=False):
        width = {"SCALAR": 1, "VEC3": 3}[kind]
        rows = [[value] for value in values] if width == 1 else values
        code = {5123: "H", 5126: "f"}[component]
        payload = b"".join(struct.pack("<" + code * width, *row) for row in rows)
        while len(self.data) % 4:
            self.data.append(0)
        view = dict(buffer=0, byteOffset=len(self.data), byteLength=len(payload))
        if indices:
            view["target"] = 34963
        self.model["bufferViews"].append(view)
        self.data.extend(payload)
        accessor = dict(bufferView=len(self.model["bufferViews"]) - 1,
                        componentType=component, count=len(rows), type=kind)
        if kind == "VEC3":
            accessor.update(min=list(map(min, zip(*rows))), max=list(map(max, zip(*rows))))
        self.model["accessors"].append(accessor)
        return len(self.model["accessors"]) - 1

    def write(self, path):
        self.model["buffers"] = [dict(byteLength=len(self.data), uri=
            "data:application/octet-stream;base64," + base64.b64encode(self.data).decode())]
        path.write_text(json.dumps(self.model), encoding="utf-8")


def emitter_fixture(path, material):
    asset = Asset()
    asset.model["extensionsUsed"] = ["KHR_materials_emissive_strength"]
    positions = [[-1., -.8, 0.], [1., -.8, 0.], [1., .8, 0.], [-1., .8, 0.]]
    primitive = dict(attributes=dict(POSITION=asset.accessor("VEC3", positions),
                                    NORMAL=asset.accessor("VEC3", [[0., 0., 1.]] * 4)),
                     indices=asset.accessor("SCALAR", [0, 1, 2, 0, 2, 3], 5123, indices=True),
                     material=0)
    asset.model["meshes"] = [dict(primitives=[primitive])]
    asset.model["materials"] = [material]
    asset.write(path)
    return asset.model
