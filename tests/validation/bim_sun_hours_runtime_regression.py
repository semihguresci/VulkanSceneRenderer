"""Verify BIM direct sun hours, viewport heatmap, and exported study data."""

import argparse
import csv
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile

from bim_hard_clash_runtime_regression import DotBimFixture
from display_capture_test_support import validation_log_error
from physics_auxiliary_regression import cube


WIDTH, HEIGHT = 320, 240
TECHNIQUE = "deferred-raster"
SETTINGS = {
    "minX": -.65, "maxX": 3.25, "minZ": -.65, "maxZ": 3.25, "height": 0.,
    "columns": 3, "rows": 3, "latitude": 0., "longitude": 0.,
    "year": 2026, "month": 3, "day": 20, "utcOffsetMinutes": 60,
    "startMinute": 540, "endMinute": 690, "intervalMinutes": 60, "modelNorthDegrees": 0.,
}
ROTATED = SETTINGS | {"modelNorthDegrees": 90.}
NIGHT = SETTINGS | {"startMinute": 120, "endMinute": 260}
STUDIES = {"day": SETTINGS, "rotated": ROTATED, "night": NIGHT, "replacement": SETTINGS}
EXPECTED = {
    "day": [1.5, 2.5, 2.5, 2.5, 2.5, 2.5, 0., 2.5, 2.5],
    "rotated": [2.5, 2.5, 2.5, 2.5, 1.5, 2.5, 0., 2.5, 2.5],
    "night": [0.] * 9,
    "replacement": [2.5] * 9,
}
SELECTED = {"day": (0, 0), "rotated": (1, 1), "night": (0, 0), "replacement": (0, 2)}
FRAMES = {label: frame for frame, label in enumerate((
    "idle", "day", "day_pick", "day_csv", "overlay_off", "overlay_restored", "rotated", "rotated_pick", "rotated_csv",
    "night", "night_pick", "night_csv", "cancel_start", "cancel", "drain", "before_reload", "before_reload_pick",
    "reloaded", "replacement", "replacement_pick", "replacement_csv"), start=1)}
PICTURES = ("day_csv", "overlay_off", "overlay_restored", "rotated")
CSV_SETTINGS = {
    "min_x": "minX", "max_x": "maxX", "min_z": "minZ", "max_z": "maxZ",
    "columns": "columns", "rows": "rows", "latitude": "latitude", "longitude": "longitude",
    "year": "year", "month": "month", "day": "day", "utc_offset_minutes": "utcOffsetMinutes",
    "start_minute": "startMinute", "end_minute": "endMinute", "interval_minutes": "intervalMinutes",
    "model_north_degrees": "modelNorthDegrees",
}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def equal_number(actual, expected):
    return math.isclose(float(actual), expected, abs_tol=1e-5, rel_tol=0.)


def write_fixture(path, replacement):
    scene = DotBimFixture()
    slab_indices = [0, 1, 2, 0, 2, 3]

    def platform(label, number, category, center, side):
        corners = [(-side, 0., -side), (-side, 0., side), (side, 0., side), (side, 0., -side)]
        scene.mesh(label, number, category, corners, slab_indices, center)

    platform("Ground", 1, "IfcSite", (1.3, -.05, 1.3), 2.3)
    if not replacement:
        # The low cover stops all three rays in its cell. Rays from adjacent
        # sampled cells pass outside it for both north settings.
        platform("Permanent cover", 2, "IfcSlab", (0., .52, 2.6), .81)
        # With the chosen equinox settings the sun spans 35–62 degrees altitude
        # almost exactly due east. At 1.3 m, the middle ray crosses y≈1.59 m.
        # The first ray from 2.6 m away passes above the blocker, at y≈1.86 m.
        points, _, indices = cube()
        blocker = [(.24 * x, .34 * y, .24 * z) for x, y, z in points]
        scene.mesh("Partial blocker", 3, "IfcBuildingElementProxy", blocker, indices, (1.3, 1.55, 0.))
    scene.write(path)


def write_sequence(path, replacement, csv_files):
    def event(label, action, **fields):
        return dict(frame=FRAMES[label], bimSunHours=dict(action=action, **fields))

    def start(label, study="day", *, wait=True):
        return event(label, "start", settings=STUDIES[study], wait=wait)

    def select(label, study):
        column, row = SELECTED[study]
        return event(label, "select", column=column, row=row)

    def export(study):
        return event(f"{study}_csv", "export", path=str(csv_files[study]))

    events = [
        start("day"), select("day_pick", "day"), export("day"),
        event("overlay_off", "overlay", visible=False),
        event("overlay_restored", "overlay", visible=True),
        start("rotated", "rotated"), select("rotated_pick", "rotated"), export("rotated"),
        start("night", "night"), select("night_pick", "night"), export("night"),
        start("cancel_start", wait=False), event("cancel", "cancel"), event("drain", "wait"),
        start("before_reload"), select("before_reload_pick", "day"),
        event("reloaded", "wait") | dict(reload=str(replacement)),
        start("replacement", "replacement"), select("replacement_pick", "replacement"), export("replacement"),
    ]
    sequence = dict(schemaVersion=1, frames=max(FRAMES.values()), events=events,
                    sampleFrames=[FRAMES[label] for label in PICTURES], telemetryFrames=list(FRAMES.values()))
    path.write_text(json.dumps(sequence, indent=2) + "\n", encoding="utf-8")


def capture_path(base, label):
    frame = FRAMES[label]
    return base if frame == max(FRAMES.values()) else base.with_name(f"{base.stem}.frame-{frame:04d}{base.suffix}")


def read_states(base, model, replacement):
    states = {}
    for label in FRAMES:
        telemetry = capture_path(base, label).with_suffix(".telemetry.json")
        state = json.loads(telemetry.read_bytes())
        require(state["validationEnabled"] and state["technique"] == TECHNIQUE and
                state["resolution"] == [WIDTH, HEIGHT], f"Wrong capture state at {label}")
        expected_model = replacement if FRAMES[label] >= FRAMES["reloaded"] else model
        require(Path(state["scene"]["auxiliary"]).resolve() == expected_model, f"Wrong BIM model at {label}")
        states[label] = state
    return states


def check_study(study, state):
    settings = STUDIES[study]
    require(state["status"] == "complete", f"{study}: {state}")
    for key, value in settings.items():
        require(equal_number(state["settings"][key], value), f"{study} lost {key}: {state['settings']}")
    require(state["sampleCount"] == 3 and state["daylightSampleCount"] == (0 if study == "night" else 3), state)
    require(equal_number(state["daylightHours"], 0. if study == "night" else 2.5), state)
    require(state["skippedTriangles"] == 0, state)
    require(len(state["hours"]) == 9 and all(equal_number(actual, value)
            for actual, value in zip(state["hours"], EXPECTED[study], strict=True)), f"{study} hours: {state['hours']}")


def check_readout_and_csv(study, state, path):
    selected = state["selected"]
    column, row_index = SELECTED[study]
    require((selected["column"], selected["row"]) == (column, row_index), selected)
    with path.open(encoding="utf-8", newline="") as stream:
        rows = list(csv.DictReader(stream))
    require(len(rows) == len(state["hours"]), f"Grid CSV has {len(rows)} rows: {path}")
    for index, row in enumerate(rows):
        expected_row, expected_column = divmod(index, 3)
        require((int(row["column"]), int(row["row"])) == (expected_column, expected_row), row)
        expected = {"x": expected_column * 1.3, "y": 0., "z": expected_row * 1.3,
                    "sun_hours": EXPECTED[study][index]}
        for field, key in CSV_SETTINGS.items():
            expected[field] = STUDIES[study][key]
        expected.update(sample_count=state["sampleCount"], daylight_sample_count=state["daylightSampleCount"],
                        daylight_hours=state["daylightHours"], skipped_triangles=state["skippedTriangles"])
        require(all(equal_number(row[field], value) for field, value in expected.items()), f"Incorrect CSV row: {row}")
    match = rows[row_index * 3 + column]
    require(all(equal_number(match[field], selected[key])
                for field, key in (("x", "x"), ("y", "y"), ("z", "z"), ("sun_hours", "hours"))),
            f"Selected viewport readout {selected} differs from CSV row {match}")


def check_lifecycle(states, csv_files):
    results = {label: state["bimSunHours"] for label, state in states.items()}
    for study, path in csv_files.items():
        check_study(study, results[study])
        check_readout_and_csv(study, results[f"{study}_csv"], path)
    require(results["before_reload_pick"]["selected"] is not None, results["before_reload_pick"])
    require(results["cancel_start"]["status"] in ("running", "complete"), results["cancel_start"])
    for label, status in (("idle", "idle"), ("cancel", "cancelled"), ("drain", "cancelled"), ("reloaded", "idle")):
        state = results[label]
        require(state["status"] == status and not state["hours"] and state["selected"] is None and
                not state["overlayVisible"], f"Stale study at {label}: {state}")
    for label in PICTURES:
        require(results[label]["overlayVisible"] == (label != "overlay_off"), results[label])


def read_picture(base, label):
    import numpy as np
    from PIL import Image

    path = capture_path(base, label)
    meta = json.loads(Path(f"{path}.capture.json").read_bytes())
    require(meta["source"] == "viewport" and not meta["includesUi"], f"Wrong viewport capture route: {path}")
    with Image.open(path) as image:
        require(image.format == "PNG" and image.size == (WIDTH, HEIGHT), f"Wrong capture dimensions: {path}")
        return np.asarray(image.convert("RGB"), dtype=np.int16)


def check_heatmap(base, states):
    import numpy as np

    matrix = np.asarray(states["day"]["camera"]["unjitteredViewProjColumns"]).T
    for label in PICTURES:
        np.testing.assert_allclose(states[label]["camera"]["unjitteredViewProjColumns"], matrix.T,
                                   atol=1e-6, rtol=0, err_msg=f"Camera changed at {label}")
    selected, off, restored, rotated = (read_picture(base, label) for label in PICTURES)
    changed = int(np.count_nonzero(np.max(np.abs(restored - off), axis=2) > 5))
    require(changed > 500, f"The study heatmap contributed only {changed} viewport pixels")
    np.testing.assert_array_equal(selected, restored, err_msg="Toggle did not restore the selected heatmap")

    # Compare interior pixels so a selected-cell border cannot affect the
    # comparison at the center of either shadow patch.
    def patch(image, x, z):
        clip = matrix @ np.array((x, 0., z, 1.))
        require(clip[3] > 0., "Heatmap grid is behind the camera")
        cx = int((1. + clip[0] / clip[3]) * WIDTH * .5)
        cy = int((1. - clip[1] / clip[3]) * HEIGHT * .5)
        require(5 < cx < WIDTH - 5 and 5 < cy < HEIGHT - 5, f"Cell center outside viewport: {cx}, {cy}")
        return image[cy - 4:cy + 5, cx - 4:cx + 5]

    dark_to_open = np.median(patch(rotated, 0., 0.) - patch(selected, 0., 0.), axis=(0, 1))
    open_to_dark = np.median(patch(rotated, 1.3, 1.3) - patch(selected, 1.3, 1.3), axis=(0, 1))
    require(np.max(np.abs(dark_to_open)) >= 8 and np.max(np.abs(open_to_dark)) >= 8 and
            float(dark_to_open @ open_to_dark) < 0., f"Expected opposite heatmap changes: {dark_to_open}, {open_to_dark}")
    require(np.max(np.abs(patch(rotated, 2.6, 0.) - patch(selected, 2.6, 0.))) <= 4,
            "North rotation changed the color of an open cell")
    covered = np.median(patch(selected, 0., 2.6), axis=(0, 1))
    sunlit = np.median(patch(selected, 2.6, 0.), axis=(0, 1))
    require(np.max(np.abs(covered - sunlit)) >= 20 and
            np.max(np.abs(patch(selected, 0., 2.6) - patch(off, 0., 2.6))) >= 10,
            "The covered cell did not render a distinct heatmap value")
    return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--managed-output", action="store_true", help="Make a fresh run directory beneath the output.")
    args = parser.parse_args()
    if not args.generate_only and os.environ.get("CONTAINER_RUN_GPU_BIM_SUN_HOURS") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_BIM_SUN_HOURS=1 to run GPU captures")
        return 77
    renderer = args.renderer.resolve()
    output = args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    else:
        output.mkdir(parents=True)
    model = output / "sun-hours.bim"
    replacement = output / "replacement.bim"
    sequence = output / "sun-hours.sequence.json"
    csv_files = {study: output / f"{study}.csv" for study in STUDIES}
    base = output / "sun-hours.png"
    write_fixture(model, replacement=False)
    write_fixture(replacement, replacement=True)
    write_sequence(sequence, replacement, csv_files)
    if args.generate_only:
        return 0

    command = [str(renderer), "--model", str(model), "--import-scale", "1",
               "--hidden", "--no-ui", "--validation", "--msaa", "1", "--no-taa", "--no-bloom",
               "--no-ray-query", "--no-ddgi", "--render-technique", TECHNIQUE,
               "--display-mode", "lit", "--width", str(WIDTH), "--height", str(HEIGHT),
               "--camera-position", "-4", "7.5", "7.5", "--camera-target", "1.3", "0", "1.3", "--camera-fov", "43",
               "--sky", "plain", "--sky-horizon", "0", "0", "0", "--environment-intensity", "0",
               "--directional-intensity", "2", "--directional-direction", "-.3", "-.2", "-1", "--bounce-intensity", "0",
               "--fixed-dt", ".016666667", "--screenshot-viewport", "--capture-sequence", str(sequence),
               "--screenshot", str(base)]
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    log = base.with_suffix(".log")
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                                 env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)), timeout=180,
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    text = log.read_text(encoding="utf-8", errors="replace")
    require(process.returncode == 0 and not validation_log_error(text), f"Vulkan capture failed: {log}")
    if os.environ.get("VK_LOADER_DEBUG") == "layer":
        require('Insert instance layer "VK_LAYER_KHRONOS_validation"' in text, f"Missing validation layer: {log}")
    states = read_states(base, model, replacement)
    check_lifecycle(states, csv_files)
    heatmap_pixels = check_heatmap(base, states)
    print(f"PASS direct sunlight for open, covered, partially shaded, rotated, and night grids ({output})", flush=True)
    print(f"PASS CSV/readout, cancellation, reload, and {heatmap_pixels} heatmap pixels", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
