"""Measure FSR disocclusion on moving foliage, glass, and emissive geometry."""

import base64
import io

import numpy as np
from PIL import Image, ImageDraw

from display_capture_test_support import Asset
from graphics_concurrency_benchmark import file_identity


EXTENT = (640, 384)
CAMERA_Z = 5.5
CARD_HALF = (0.675, 0.61)
SIDE = 1.55
PARTS = {"foliage": (0.77, 0.15, CARD_HALF), "glass": (-0.77, 0.28, CARD_HALF),
         "emissive": (0., 0.21, (0.675, 0.12))}
TECHNIQUES = ("deferred-raster", "forward-raster")
ARRIVALS = (28, 52)
OBSERVATIONS = (-2, 0, 1, 2, 4, 8, 16)
MASK_EROSION = 3


def foliage_texture():
    alpha = Image.new("L", (256, 256))
    draw = ImageDraw.Draw(alpha)
    draw.polygon([(126, 16), (134, 16), (142, 237), (121, 237)], fill=255)
    for index, y in enumerate((54, 91, 127, 163, 201)):
        reach = 26 + 4 * abs(index - 2)
        draw.polygon([(130, y), (76, y - 24), (reach, y - 21), (63, y + 4), (129, y + 14)], fill=255)
        draw.polygon([(130, y - 14), (184, y - 34), (256 - reach, y - 26), (192, y - 3),
                      (133, y + 3)], fill=255)
    rgba = Image.new("RGBA", alpha.size, (255, 255, 255, 255))
    rgba.putalpha(alpha)
    encoded = io.BytesIO()
    rgba.save(encoded, format="PNG")
    return "data:image/png;base64," + base64.b64encode(encoded.getvalue()).decode("ascii")


def write_fixture(path):
    asset = Asset()
    asset.model.update(extensionsUsed=["KHR_materials_unlit", "KHR_materials_emissive_strength"],
                       images=[dict(uri=foliage_texture())],
                       samplers=[dict(minFilter=9728, magFilter=9728, wrapS=33071, wrapT=33071)],
                       textures=[dict(source=0, sampler=0)])
    asset.model["scenes"][0]["nodes"] = [0, 1]
    asset.model["nodes"] = [dict(name="Opaque backdrop", mesh=0), dict(name="Moving foreground", children=[2, 3, 4]),
                            dict(name="Alpha cutout foliage", mesh=1), dict(name="Translucent pane", mesh=2),
                            dict(name="Emissive strip", mesh=3)]

    def quad(name, center_y, z, half, color, alpha, *, emissive=None):
        x, y = half
        corners = [(-x, center_y - y, z), (x, center_y - y, z), (x, center_y + y, z), (-x, center_y + y, z)]
        attributes = dict(POSITION=asset.accessor("VEC3", corners), NORMAL=asset.accessor("VEC3", [(0, 0, 1)] * 4),
                          TEXCOORD_0=asset.accessor("VEC2", [(0, 1), (1, 1), (1, 0), (0, 0)]))
        indices = asset.accessor("SCALAR", (0, 1, 2, 0, 2, 3), 5123, indices=True)
        parameters = dict(baseColorFactor=list(color), metallicFactor=0, roughnessFactor=1)
        material = dict(name=name, doubleSided=True, alphaMode=alpha,
                        extensions=dict(KHR_materials_unlit={}), pbrMetallicRoughness=parameters)
        if alpha == "MASK":
            material["alphaCutoff"] = .5
            parameters["baseColorTexture"] = dict(index=0)
        if emissive is not None:
            material["emissiveFactor"] = emissive
            material["extensions"] = {"KHR_materials_emissive_strength": {"emissiveStrength": 4}}
        asset.model["meshes"].append(dict(primitives=[dict(attributes=attributes, indices=indices,
                                                          material=len(asset.model["materials"]))]))
        asset.model["materials"].append(material)

    quad("Neutral background", 0, -.8, (6, 4), (.16, .16, .16, 1), "OPAQUE")
    quad("Foliage cutout", *PARTS["foliage"], (.02, .72, .045, 1), "MASK")
    quad("Glass overlay", *PARTS["glass"], (.72, .025, .52, .6), "BLEND")
    quad("Emissive strip", *PARTS["emissive"], (.005, .005, .005, 1), "OPAQUE", emissive=(1, .24, .02))
    asset.write(path)


def capture_plan(reset):
    keys = ((1, -SIDE), (24, -SIDE), (28, SIDE), (48, SIDE), (52, -SIDE), (68, -SIDE))
    samples = sorted({24, 48, 68, *(a + offset for a in ARRIVALS for offset in OBSERVATIONS)})
    return dict(schemaVersion=1, frames=68, sampleFrames=samples, objectName="Moving foreground",
                objectTranslation=[dict(frame=frame, translation=[x, 0, 0]) for frame, x in keys],
                camera=[dict(frame=1, position=[0, 0, CAMERA_Z], target=[0, 0, 0])],
                events=[dict(frame=frame, reset=True) for frame in samples] if reset else [])


def projected_card(part, x, projection):
    y, z, half = PARTS[part]
    world = np.array([(x + side_x * half[0], y + side_y * half[1], z, 1)
                      for side_x in (-1, 1) for side_y in (-1, 1)])
    clip = world @ projection
    ndc = clip[:, :2] / clip[:, 3, None]
    # Scene raster uses a negative-height viewport with a top-left origin.
    pixels = np.column_stack((.5 + .5 * ndc[:, 0], .5 - .5 * ndc[:, 1])) * EXTENT
    lo = [int(round(value)) for value in pixels.min(axis=0)]
    hi = [int(round(value)) for value in pixels.max(axis=0)]
    assert 0 <= lo[0] < hi[0] <= EXTENT[0] and 0 <= lo[1] < hi[1] <= EXTENT[1], (part, x, lo, hi)
    return lo[0], lo[1], hi[0], hi[1]


def card_region(bounds):
    left, top, right, bottom = bounds
    region = np.zeros((EXTENT[1], EXTENT[0]), dtype=bool)
    region[top:bottom, left:right] = True
    return region


def erode(mask, radius):
    for _ in range(radius):
        padded = np.pad(mask, 1)
        mask &= padded[1:-1, :-2] & padded[1:-1, 2:] & padded[:-2, 1:-1] & padded[2:, 1:-1]
    return mask


def contrast_mask(current, reference, region):
    delta = current - reference
    return delta, erode((np.max(np.abs(delta), axis=2) >= 20) & region, MASK_EROSION)


def stale_signal(residual, contrast):
    # Positive projection measures retained prior color. Clamp per pixel so an
    # overshoot across the current color cannot cancel a trail elsewhere.
    energy = np.square(contrast).sum(axis=1)
    projection = np.maximum(np.sum(residual * contrast, axis=1), 0) / energy
    return dict(pixels=int(energy.size), nativeContrastMae8Bit=float(np.mean(np.abs(contrast))),
                liveVsResetMae8Bit=float(np.mean(np.abs(residual))),
                previousColorFraction=float(np.average(projection, weights=energy)),
                previousColorFractionP95=float(np.quantile(projection, .95)))


def analyze_ghosting(native, live, reset, projection):
    measurements = {}
    for start, arrival, stop, origin in ((24, 28, 48, -SIDE), (48, 52, 68, SIDE)):
        swing = {}
        for part in PARTS:
            old_bounds = projected_card(part, origin, projection)
            new_bounds = projected_card(part, -origin, projection)
            old_area, new_area = card_region(old_bounds), card_region(new_bounds)
            before, after = native[start], native[stop]
            contrast, vacated = contrast_mask(before, after, old_area)
            _, appeared = contrast_mask(after, before, new_area)
            coverage = float(np.mean((np.max(np.abs(contrast), axis=2) >= 20)[old_area]))
            limits = (.12, .70) if part == "foliage" else (.92, 1.0)
            assert limits[0] <= coverage <= limits[1], (part, arrival, "Wrong native alpha coverage", coverage)
            assert np.count_nonzero(vacated) >= 400 and np.count_nonzero(appeared) >= 400, (part, arrival)
            # A full stale image must fail this oracle on the real measured mask.
            control = stale_signal(contrast[vacated], contrast[vacated])
            assert control["previousColorFraction"] == 1, (part, arrival, control)

            observations = {}
            for offset in OBSERVATIONS:
                frame = arrival + offset
                values = stale_signal((live[frame] - reset[frame])[vacated], contrast[vacated])
                entrant = appeared
                entrant_contrast = after - before
                if offset < 0:
                    middle_area = card_region(projected_card(part, 0, projection))
                    entrant_contrast, entrant = contrast_mask(native[frame], before, middle_area)
                    assert np.count_nonzero(entrant) >= 400, (part, frame, "Missing moving foreground")
                values["enteredLiveVsNativeMae8Bit"] = float(np.mean(np.abs(live[frame][entrant] - native[frame][entrant])))
                values["enteredResetVsNativeMae8Bit"] = float(np.mean(np.abs(reset[frame][entrant] - native[frame][entrant])))
                values["enteredNativeContrastMae8Bit"] = float(np.mean(np.abs(entrant_contrast[entrant])))
                # Allow one-frame reconstruction differences. Persistent energy
                # above 15% of the vacated sample's actual source contrast fails.
                max_old = .45 if offset <= 0 else .30 if offset < 4 else .15
                assert values["previousColorFraction"] < max_old, (part, frame, "Retained old foreground color", values)
                assert values["enteredLiveVsNativeMae8Bit"] < .45 * values["enteredNativeContrastMae8Bit"], \
                    (part, frame, "Newly visible foreground was lost", values)
                observations[frame] = values
            swing[part] = dict(oldPixelRegion=old_bounds, newPixelRegion=new_bounds,
                               nativeCoverage=coverage, staleControl=control, observations=observations)
        measurements[arrival] = swing
    return measurements


def verify_fsr_ghosting(output, capture, check_mode):
    model = output / "moving-foliage-glass-emissive.gltf"
    write_fixture(model)
    report = dict(model=file_identity(model), techniques={})
    for technique in TECHNIQUES:
        record = report["techniques"][technique] = dict(captures={})
        states, images = {}, {}
        for profile in ("native", "live", "reset"):
            label = f"ghosting-{technique}-{profile}"
            mode = "native" if profile == "native" else "quality"
            plan = capture_plan(reset=profile == "reset")
            states[profile], raw = capture(label, mode, plan, extent=EXTENT, technique=technique, model_path=model)
            images[profile] = {frame: value.astype(np.float32) for frame, value in raw.items()}
            paths = {}
            for frame, state in states[profile].items():
                check_mode(state, mode, mode, EXTENT)
                passes = {item["name"]: item["active"] for item in state["passes"]}
                fsr_active = profile != "native"
                assert not state["taa"]["enabled"] and passes["TemporalResolve"] == fsr_active, (label, frame)
                assert passes["OitResolve"] and passes["TemporalVelocity"] == fsr_active, (label, frame)
                assert state["exposure"]["mode"] == "manual", (label, frame, state["exposure"])
                if profile == "reset":
                    assert state["taa"]["resetReason"] == "capture sequence reset", (label, frame, state["taa"])
                name = label if frame == plan["frames"] else f"{label}.frame-{frame:04d}"
                paths[frame] = dict(image=str(output / f"{name}.png"), telemetry=str(output / f"{name}.telemetry.json"),
                                    historyEpoch=state["taa"]["epoch"])
            record["captures"][profile] = paths

        live_epochs = {state["taa"]["epoch"] for state in states["live"].values()}
        reset_epochs = {state["taa"]["epoch"] for state in states["reset"].values()}
        assert len(live_epochs) == 1, (technique, "Live FSR reset", live_epochs)
        assert len(reset_epochs) == len(states["reset"]), (technique, reset_epochs)
        matrix = states["native"][24]["camera"]["unjitteredViewProjColumns"]
        exposure = states["native"][24]["exposure"]["resolvedExposure"]
        for frame, live in states["live"].items():
            native, reset = states["native"][frame], states["reset"][frame]
            assert all(state["camera"]["unjitteredViewProjColumns"] == matrix and
                       state["exposure"]["resolvedExposure"] == exposure for state in (native, live, reset)), (technique, frame)
            assert live["camera"]["jitterUv"][:2] == reset["camera"]["jitterUv"][:2], (technique, frame)
        record["ghosting"] = analyze_ghosting(images["native"], images["live"], images["reset"], np.asarray(matrix))
    return report
