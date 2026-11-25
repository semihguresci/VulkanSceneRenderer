"""Check that matched native and FSR captures draw UI at presentation resolution."""

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


# The default Scene Controls title includes opaque white text. The rest of
# the panel is translucent and can legitimately change with the scene below.
TITLE_CROP = (18, 9, 150, 25)


def compare_presentation_ui(native_window, fsr_window, native_viewport, fsr_viewport):
    captures = (native_window, fsr_window, native_viewport, fsr_viewport)
    height, width, channels = native_window.shape
    left, top, right, bottom = TITLE_CROP
    assert channels == 3 and width >= right and height >= bottom, native_window.shape
    assert all(image.shape == native_window.shape and image.dtype == np.uint8
               for image in captures), "Expected matching display-size RGB PNGs"

    def white_title_pixels(image):
        return np.all(image[top:bottom, left:right] >= 250, axis=2)

    native_glyphs = white_title_pixels(native_window)
    fsr_glyphs = white_title_pixels(fsr_window)
    glyph_count = int(np.count_nonzero(native_glyphs))
    assert glyph_count >= 64, "Scene Controls title was absent from its default position"
    changed = int(np.count_nonzero(native_glyphs ^ fsr_glyphs))
    assert changed == 0, f"{changed} opaque title pixels shifted or changed under FSR"
    assert not any(np.any(native_glyphs & white_title_pixels(viewport))
                   for viewport in (native_viewport, fsr_viewport)), "UI glyphs leaked into the viewport capture"
    return {"displayExtent": [width, height], "titleCrop": list(TITLE_CROP),
            "opaqueTitlePixels": glyph_count, "changedTitlePixels": changed}


def load_capture(path, source):
    receipt = json.loads(Path(f"{path}.capture.json").read_text(encoding="utf-8"))
    telemetry = json.loads(path.with_suffix(".telemetry.json").read_text(encoding="utf-8"))
    with Image.open(path) as image:
        assert image.format == "PNG", path
        extent = [image.width, image.height]
        pixels = np.array(image.convert("RGB"))
    assert receipt["source"] == source and receipt["includesUi"] == (source == "window"), receipt
    assert extent == [receipt["width"], receipt["height"]] == telemetry["fsr"]["displayExtent"], (path, extent)
    return pixels, telemetry


def verify_captures(native_window, fsr_window, native_viewport, fsr_viewport):
    paths = (native_window, fsr_window, native_viewport, fsr_viewport)
    pairs = [load_capture(path, source) for path, source in
             zip(paths, ("window", "window", "viewport", "viewport"))]
    native, upscaled, native_only, upscaled_only = (telemetry for _, telemetry in pairs)
    native_fsr, active_fsr = native["fsr"], upscaled["fsr"]
    assert native_fsr == native_only["fsr"] and active_fsr == upscaled_only["fsr"], "Window and viewport settings differ"
    assert native_fsr["mode"] == native_fsr["requested"] == "native" and not native_fsr["active"], native_fsr
    assert native_fsr["renderExtent"] == native_fsr["displayExtent"], native_fsr
    assert active_fsr["active"] and active_fsr["mode"] == active_fsr["requested"] != "native", active_fsr
    assert all(0 < render < display for render, display in
               zip(active_fsr["renderExtent"], active_fsr["displayExtent"], strict=True)), active_fsr

    camera = native["camera"]["unjitteredViewProjColumns"]
    for state in (upscaled, native_only, upscaled_only):
        assert state["technique"] == native["technique"], "Captures use different render techniques"
        np.testing.assert_allclose(state["camera"]["unjitteredViewProjColumns"], camera, rtol=0, atol=1e-5)
    result = compare_presentation_ui(*(pixels for pixels, _ in pairs))
    return {**result, "fsrRenderExtent": active_fsr["renderExtent"], "fsrMode": active_fsr["mode"],
            "captures": {name: str(path.resolve()) for name, path in
                         zip(("nativeWindow", "fsrWindow", "nativeViewport", "fsrViewport"), paths)}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-window", required=True, type=Path)
    parser.add_argument("--fsr-window", required=True, type=Path)
    parser.add_argument("--native-viewport", required=True, type=Path)
    parser.add_argument("--fsr-viewport", required=True, type=Path)
    parser.add_argument("--output", type=Path, help="Write the comparison report to this path")
    args = parser.parse_args()
    result = verify_captures(args.native_window, args.fsr_window, args.native_viewport, args.fsr_viewport)
    report = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(report, encoding="utf-8")
    else:
        print(report, end="")


if __name__ == "__main__":
    main()
