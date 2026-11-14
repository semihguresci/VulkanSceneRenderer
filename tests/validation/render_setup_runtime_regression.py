"""Exercise named glTF render setup capture, replay, restoration, and rejection."""

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from display_capture_test_support import Asset, validation_log_error
from hdr_environment_runtime_regression import ENV_HEIGHT, ENV_WIDTH, write_hdr


WIDTH, HEIGHT = 320, 240
TECHNIQUE = "deferred-raster"
NAMES = ("Amber draft", "Blue review")
FAULT_NAMES = ("Missing HDR", "Stale node")
VARIANTS = "KHR_materials_variants"
VISIBILITY = "KHR_node_visibility"
GRADE_FIELDS = dict(toneMap="clip", saturation=1., contrast=1., temperatureKelvin=6506.11144,
                    tintDuv=.00325895312, vibrance=0., shadowTone=[1., 1., 1.], highlightTone=[1., 1., 1.])
GRADES = {
    "base": GRADE_FIELDS | dict(saturation=.875),
    "a": GRADE_FIELDS | dict(toneMap="reinhard", saturation=1.375, contrast=.875, vibrance=.25),
    "b": GRADE_FIELDS | dict(toneMap="filmic", saturation=.625, contrast=1.125, temperatureKelvin=5400.,
                             tintDuv=-.0078125, shadowTone=[.875, .9375, 1.]),
}
MAPPINGS = {"base": (-1, -1, (True, False)), "a": (3, 0, (False, True)), "b": (4, 1, (True, True))}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def write_json(path, document):
    path.write_text(json.dumps(document, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def write_gltf(path):
    asset = Asset()
    model = asset.model
    model["extensionsUsed"] = [VARIANTS, VISIBILITY, "KHR_materials_unlit"]
    model["extensions"] = {VARIANTS: {"variants": [{"name": name} for name in NAMES]}}
    colors = ([.44, .26, .48], [.8, .12, .03], [.02, .25, .85], [.28, .8, .22], [.94, .7, .045])
    model["materials"] = [dict(pbrMetallicRoughness=dict(baseColorFactor=[*color, 1.]),
                               extensions={"KHR_materials_unlit": {}}) for color in colors]
    indices = asset.accessor("SCALAR", [0, 1, 2, 0, 2, 3], 5123, indices=True)

    def panel(half_x, half_y, material):
        points = [[-half_x, -half_y, 0.], [half_x, -half_y, 0.],
                  [half_x, half_y, 0.], [-half_x, half_y, 0.]]
        return dict(attributes=dict(POSITION=asset.accessor("VEC3", points)), indices=indices, material=material)

    chart = panel(.51, .49, 0)
    chart["extensions"] = {VARIANTS: {"mappings": [dict(material=1, variants=[0]), dict(material=2, variants=[1])]}}
    model["meshes"] = [dict(primitives=[primitive]) for primitive in (chart, panel(.27, .32, 3), panel(.27, .32, 4))]
    model["cameras"] = [
        dict(name="Draft lens", type="perspective", perspective=dict(yfov=.7, znear=.1, zfar=50.)),
        dict(name="Review lens", type="orthographic", orthographic=dict(xmag=2., ymag=1.5, znear=.1, zfar=50.)),
    ]
    model["nodes"] = [
        dict(name="Variant panel", mesh=0),
        dict(name="Left panel", mesh=1, translation=[-.95, 0., 0.]),
        dict(name="Right panel", mesh=2, translation=[.95, 0., 0.], extensions={VISIBILITY: dict(visible=False)}),
        dict(name="Draft camera", camera=0, translation=[-.2, .14, 3.6]),
        dict(name="Review camera", camera=1, translation=[.25, -.15, 3.]),
    ]
    model["scenes"][0]["nodes"] = list(range(len(model["nodes"])))
    asset.write(path)


def write_looks(output):
    hdr = {look: output / f"look-{look}.hdr" for look in ("a", "b")}
    for look, colors in (("a", ((154, 73, 24, 129), (97, 35, 17, 129))),
                         ("b", ((29, 69, 169, 129), (16, 39, 137, 129)))):
        west, east = colors
        write_hdr(hdr[look], [[west] * (ENV_WIDTH // 2) + [east] * (ENV_WIDTH // 2) for _ in range(ENV_HEIGHT)])
    grade = {look: output / f"grade-{look}.json" for look in GRADES}
    for look, path in grade.items():
        write_json(path, dict(version=1, **GRADES[look]))
    return hdr, grade


class Capture:
    def __init__(self):
        self.frame = 2
        self.events = []
        self.samples = {}
        self.object_name = ""

    def step(self, **event):
        self.frame += 1
        self.events.append(dict(frame=self.frame, **event))

    def setup(self, action, **fields):
        self.step(renderSetup=dict(action=action, **fields))

    def sample(self, label):
        self.frame += 2
        self.samples[label] = self.frame

    def write(self, path):
        data = dict(schemaVersion=1, frames=self.frame, events=self.events, sampleFrames=list(self.samples.values()))
        if self.object_name:
            data["objectName"] = self.object_name
        write_json(path, data)


def authoring_sequence(sidecar, hdr, grade):
    sequence = Capture()
    sequence.sample("base")
    for look, name in zip(("a", "b"), NAMES, strict=True):
        camera, variant, visible = MAPPINGS[look]
        sequence.step(cameraSelection=dict(sourceNode=camera))
        sequence.step(materialVariant=variant)
        sequence.step(nodeFlags=dict(sourceNode=1, visible=visible[0]))
        sequence.step(nodeFlags=dict(sourceNode=2, visible=visible[1]))
        sequence.step(environment=str(hdr[look]))
        sequence.step(displayGradePreset=str(grade[look]))
        sequence.setup("capture", name=name)
        sequence.sample(look)
    sequence.setup("save", path=str(sidecar))
    sequence.sample("after-save")
    return sequence


def replay_sequence(sidecar, faulty):
    sequence = Capture()
    sequence.object_name = "Right panel"
    sequence.sample("base")
    sequence.setup("load", path=str(sidecar))
    sequence.sample("loaded")
    sequence.setup("activate", name=NAMES[0])
    sequence.sample("a")
    sequence.step(nodeFlags=dict(sourceNode=1, visible=False))
    sequence.sample("node-visibility-blocked")
    sequence.step(objectVisible=True)
    sequence.sample("object-visibility-blocked")
    sequence.setup("activate", name=NAMES[1])
    sequence.sample("b")
    sequence.setup("deactivate")
    sequence.sample("restored")
    sequence.setup("load", path=str(faulty))
    sequence.setup("activate", name=NAMES[0])
    sequence.sample("a-again")
    for name, label in zip(FAULT_NAMES, ("missing-hdr", "stale-node"), strict=True):
        sequence.setup("activate", name=name, expectFailure=True)
        sequence.sample(label)
    sequence.setup("activate", name=NAMES[1])
    sequence.sample("b-again")
    sequence.setup("deactivate")
    sequence.sample("restored-again")
    sequence.setup("activate", name=NAMES[1])
    sequence.sample("exported-preview")
    return sequence


def require_grade(actual, expected):
    import numpy as np

    require(actual["toneMap"] == expected["toneMap"], f"Wrong tone mapper: {actual}")
    for field in expected.keys() - {"toneMap"}:
        expected_float = np.asarray(expected[field], dtype=np.float32).astype(np.float64)
        np.testing.assert_allclose(actual[field], expected_float, atol=1e-6, rtol=0,
                                   err_msg=f"Wrong display grade {field}")


def fault_sidecar(sidecar, faulty, hdr, source):
    data = json.loads(sidecar.read_bytes())
    identity = dict(sceneIndex=0, byteSize=len(source), digest=hashlib.sha256(source).hexdigest())
    require(data["version"] == 1 and data["source"] == identity, f"Invalid render setup source identity: {data.get('source')}")
    rows = data["setups"]
    require([row["name"] for row in rows] == list(NAMES), f"Missing setup names: {rows}")
    for look, row in zip(("a", "b"), rows, strict=True):
        camera, variant, flags = MAPPINGS[look]
        require(row["camera"] == camera and row["variant"] == variant, row)
        visibility = {entry["node"]: entry["visible"] for entry in row["visibility"]}
        require(visibility[1] == flags[0] and visibility[2] == flags[1], row)
        relative = Path(row["hdr"])
        require(not relative.is_absolute() and (sidecar.parent / relative).resolve() == hdr[look], row)
        require_grade(row["grade"], GRADES[look])

    # Use a different valid camera, variant, grade and visibility from the
    # preview under test so failed preflight would expose partial application.
    bad_hdr = copy.deepcopy(rows[1])
    bad_hdr.update(name=FAULT_NAMES[0], hdr="missing-panorama.hdr")
    stale_node = copy.deepcopy(rows[1])
    stale_node.update(name=FAULT_NAMES[1])
    stale_node["visibility"].append(dict(node=999, visible=False))
    rows.extend((bad_hdr, stale_node))
    write_json(faulty, data)


def run(renderer, output, model, name, sequence, base_grade, *, export=None):
    base = output / f"{name}.png"
    log = base.with_suffix(".log")
    sequence_path = output / f"{name}.sequence.json"
    sequence.write(sequence_path)
    command = [str(renderer), "--model", str(model), "--import-scale", "1", "--hidden", "--no-ui",
               "--validation", "--msaa", "1", "--no-taa", "--no-bloom", "--no-ray-query", "--no-ddgi",
               "--render-technique", TECHNIQUE, "--display-mode", "lit", "--width", str(WIDTH), "--height", str(HEIGHT),
               "--camera-position", "0", "0", "4.4", "--camera-target", "0", "0", "0", "--camera-fov", "46",
               "--sky", "plain", "--sky-horizon", ".025", ".055", ".095", "--exposure", "1",
               "--environment-intensity", "1", "--directional-intensity", "0", "--bounce-intensity", "0",
               "--display-grade", str(base_grade), "--fixed-dt", ".016666667", "--screenshot-viewport",
               "--capture-sequence", str(sequence_path), "--screenshot", str(base)]
    if export:
        command += ["--save-scene", str(export)]
    with log.open("w", encoding="utf-8") as stream:
        result = subprocess.run(command, cwd=renderer.parent, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                                stdout=stream, stderr=subprocess.STDOUT, timeout=210,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    text = log.read_text(encoding="utf-8", errors="replace")
    require(result.returncode == 0 and not validation_log_error(text), f"Vulkan render setup capture failed: {log}")
    if os.environ.get("VK_LOADER_DEBUG") == "layer":
        require('Insert instance layer "VK_LAYER_KHRONOS_validation"' in text, f"Missing validation layer: {log}")
    return read_samples(base, sequence, model)


def read_samples(base, sequence, model):
    import numpy as np
    from PIL import Image

    states, pictures = {}, {}
    for label, frame in sequence.samples.items():
        path = base if frame == sequence.frame else base.with_name(f"{base.stem}.frame-{frame:04d}.png")
        state = json.loads(path.with_suffix(".telemetry.json").read_bytes())
        require(state["technique"] == TECHNIQUE and state["validationEnabled"] and state["resolution"] == [WIDTH, HEIGHT],
                f"Wrong capture state at {label}")
        require(Path(state["scene"]["primary"]).resolve() == model, f"Wrong glTF fixture at {label}")
        meta = json.loads(Path(f"{path}.capture.json").read_bytes())
        require(meta["source"] == "viewport" and not meta["includesUi"], f"Wrong viewport capture at {label}")
        with Image.open(path) as image:
            require(image.format == "PNG" and image.size == (WIDTH, HEIGHT), f"Wrong capture dimensions for {path}")
            pictures[label] = np.asarray(image.convert("RGB"))
        states[label] = state
    return states, pictures


def check_look(state, look, hdr, *, preview=None):
    cam, variant, visible = MAPPINGS[look]
    info = state["renderSetup"]
    require(info["active"] == preview, info)
    camera = state["gltfCameraAuthoring"]["selectedSourceNode"]
    require(camera == cam and state["gltfMaterialVariants"]["selected"] == variant,
            f"Wrong camera/variant for {look}: {camera}, {state['gltfMaterialVariants']}")
    flags = {entry["sourceIndex"]: entry["effectiveVisible"] for entry in state["gltfNodeFlags"]["nodes"]}
    require((flags[1], flags[2]) == visible, f"Wrong effective visibility for {look}: {flags}")
    environment = state["environment"]
    require(not environment["loading"] and not environment["generating"], environment)
    if look == "base":
        require(environment["skyMode"] == 1 and not environment["hdrPath"], environment)
    else:
        require(environment["skyMode"] == 0 and Path(environment["hdrPath"]).resolve() == hdr[look], environment)
    require_grade(info["displayGrade"], GRADES[look])


def check_replay(authored, replayed, hdr):
    import numpy as np

    authored_states, authored_images = authored
    states, pictures = replayed
    for label, look in (("base", "base"), ("a", "a"), ("b", "b"), ("after-save", "b")):
        check_look(authored_states[label], look, hdr)
    expected = {
        "base": "base", "loaded": "base", "a": "a", "b": "b", "restored": "base",
        "node-visibility-blocked": "a", "object-visibility-blocked": "a",
        "a-again": "a", "missing-hdr": "a", "stale-node": "a", "b-again": "b", "restored-again": "base",
        "exported-preview": "b",
    }
    for label, look in expected.items():
        name = {"base": None, "a": NAMES[0], "b": NAMES[1]}[look]
        check_look(states[label], look, hdr, preview=name)
        require(states[label]["gltfMaterialVariants"]["assignments"] ==
                authored_states[look]["gltfMaterialVariants"]["assignments"], f"Wrong material binding at {label}")
        np.testing.assert_allclose(states[label]["camera"]["unjitteredViewProjColumns"],
                                   authored_states[look]["camera"]["unjitteredViewProjColumns"], atol=1e-6, rtol=0,
                                   err_msg=f"Wrong view or projection at {label}")
        np.testing.assert_array_equal(pictures[label], authored_images[look], err_msg=f"Viewport replay changed at {label}")
    require(states["loaded"]["renderSetup"]["names"] == list(NAMES), states["loaded"]["renderSetup"])
    require(states["a-again"]["renderSetup"]["names"] == list(NAMES + FAULT_NAMES), states["a-again"]["renderSetup"])
    for label in ("node-visibility-blocked", "object-visibility-blocked"):
        diagnostic = states[label]["renderSetup"]["diagnostic"].lower()
        require("deactivate" in diagnostic and "visibility" in diagnostic, diagnostic)
    np.testing.assert_array_equal(authored_images["after-save"], authored_images["b"],
                                  err_msg="Saving the sidecar changed the live scene")
    diagnostics = [states[label]["renderSetup"]["diagnostic"].lower() for label in ("missing-hdr", "stale-node")]
    require(any(word in diagnostics[0] for word in ("hdr", "panorama")) and
            any(word in diagnostics[0] for word in ("missing", "open", "exist", "file")), diagnostics)
    require("node" in diagnostics[1] and any(word in diagnostics[1] for word in ("stale", "999", "missing", "active")),
            diagnostics)
    for label in ("base", "a", "b"):
        require(int(np.max(authored_images[label])) > 50, f"The {label} capture is blank")
    difference = np.max(np.abs(authored_images["a"].astype(np.int16) - authored_images["b"]), axis=2)
    require(np.count_nonzero(difference > 8) > WIDTH * HEIGHT // 4, "The named looks are visually indistinguishable")


def check_export(path, source):
    import numpy as np

    exported = json.loads(path.read_bytes())
    original = json.loads(source)
    nodes = {node.get("name"): node for node in exported["nodes"]}
    require(nodes["Right panel"]["extensions"][VISIBILITY]["visible"] is False,
            "The exported scene incorporated the preview's visibility")
    variants = exported["extensions"][VARIANTS]["variants"]
    require([variant["name"] for variant in variants] == list(NAMES), "The exported scene lost variant alternatives")
    mapped = [primitive for mesh in exported["meshes"] for primitive in mesh["primitives"]
              if VARIANTS in primitive.get("extensions", {})]
    require(mapped, "The exported scene lost material variant bindings")
    expected = original["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"]
    for primitive in mapped:
        base = exported["materials"][primitive["material"]]["pbrMetallicRoughness"]["baseColorFactor"]
        np.testing.assert_allclose(base, expected, atol=1e-6, rtol=0,
                                   err_msg="The exported scene incorporated the preview's material")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--managed-output", action="store_true", help="Make a fresh run directory beneath the output.")
    args = parser.parse_args()
    if not args.generate_only and os.environ.get("CONTAINER_RUN_GPU_RENDER_SETUPS") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_RENDER_SETUPS=1 to run GPU captures")
        return 77
    renderer, output = args.renderer.resolve(), args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    else:
        output.mkdir(parents=True)
    model = output / "render-setup.gltf"
    sidecar, faulty = output / "named-setups.json", output / "setups-with-rejections.json"
    write_gltf(model)
    source = model.read_bytes()
    hdr, grade = write_looks(output)
    authoring, replay = authoring_sequence(sidecar, hdr, grade), replay_sequence(sidecar, faulty)
    if args.generate_only:
        authoring.write(output / "author.sequence.json")
        replay.write(output / "reopen.sequence.json")
        return 0
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    authored = run(renderer, output, model, "author", authoring, grade["base"])
    fault_sidecar(sidecar, faulty, hdr, source)
    exported = output / "saved-while-previewing.gltf"
    replayed = run(renderer, output, model, "reopen", replay, grade["base"], export=exported)
    check_replay(authored, replayed, hdr)
    check_export(exported, source)
    require(model.read_bytes() == source, "The source glTF was changed during setup editing")
    print(f"PASS named render setup capture, sidecar reopen, switch, and base restoration ({output})", flush=True)
    print("PASS invalid previews rejected and primary export preserved base visibility and material", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
