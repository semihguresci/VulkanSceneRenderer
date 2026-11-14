"""Render IFC guide-driven sectioned geometry in both raster routes and TAA modes."""

import argparse
import os
from pathlib import Path
import tempfile

from ifc_area_profiles_runtime_regression import (
    DISTANCE, HEIGHT, TECHNIQUES, WIDTH, Fixture, Profile, StepFixture,
    record, require, save_json, write_sequence,
)
from ifc_swept_surface_runtime_regression import world_ray


HALF_LENGTH = 3.
HALF_WIDTH = .78
CROWN = 1.18
CENTER = 1.42
FIXTURE = Fixture((Profile("Guided pavement", (0., 0.), (.24, .68, .88)),), (
    (0, "center crown", (-.08, CENTER + .20), True),
    (0, "resolved shoulder", (.3, CENTER - .38), True),
    (0, "widened edge", (.12, CENTER + 1.46), True),
    (0, "outer opening", (.02, CENTER + 1.95), False),
    (0, "departure approach", (-2.48, CENTER + .19), True),
    (0, "preferred narrowed shoulder", (-.12, CENTER - .56), False),
    (0, "arrival approach", (2.24, CENTER - .27), True),
    (0, "beyond authored end", (3.24, CENTER + .12), False),
), None, None)


def section_point(step, station, basis, lateral="$", vertical="$"):
    return step.add("IFCPOINTBYDISTANCEEXPRESSION", f"IFCLENGTHMEASURE({station})",
                    lateral, vertical, "$", basis)


def write_fixture(path):
    step = StepFixture()
    step.add("IFCSIUNIT", "*", ".LENGTHUNIT.", "$", ".METRE.")
    world = step.add("IFCAXIS2PLACEMENT3D", step.point(0., 0., 0.), "$", "$")
    context = step.add("IFCGEOMETRICREPRESENTATIONCONTEXT", "$", "'Model'", 3, 1e-6, world, "$")
    directrix = step.add("IFCPOLYLINE", f"({step.point(-HALF_LENGTH, 0., 0.)},{step.point(HALF_LENGTH, 0., 0.)})")
    locations = [step.add("IFCAXIS2PLACEMENTLINEAR", section_point(step, s, directrix), "$", "$")
                 for s in (0., HALF_LENGTH * 2)]
    tags = "('narrow','crown','wide')"
    section = step.add("IFCOPENCROSSPROFILEDEF", ".CURVE.", "$", ".T.",
                       f"({HALF_WIDTH},{HALF_WIDTH})", "(0.,0.)", tags, step.point(-HALF_WIDTH, 0.))
    surface = step.add("IFCSECTIONEDSURFACE", directrix, f"({','.join(locations)})", f"({section},{section})")
    guide_ids = []
    for tag, lateral, crest, height in (("narrow", -HALF_WIDTH, -.45, 0.),
                                       ("crown", 0., 0., CROWN),
                                       ("wide", HALF_WIDTH, 1.8, 0.)):
        offsets = [section_point(step, station, directrix, sidestep, rise)
                   for station, sidestep, rise in ((0., lateral, 0.),
                      (HALF_LENGTH, crest, height), (2 * HALF_LENGTH, lateral, 0.))]
        guide_ids.append(step.add("IFCOFFSETCURVEBYDISTANCES", directrix,
                                  f"({','.join(offsets)})", f"'{tag}'"))
    guide_set = step.add("IFCGEOMETRICCURVESET", f"({','.join(guide_ids)})")
    body = step.add("IFCSHAPEREPRESENTATION", context, "'Body'", "'SectionedSurface'", f"({surface})")
    axis = step.add("IFCSHAPEREPRESENTATION", context, "'Axis'", "'GeometricCurveSet'", f"({guide_set})")
    rgb = step.add("IFCCOLOURRGB", "$", *FIXTURE.products[0].color)
    appearance = step.add("IFCSURFACESTYLERENDERING", rgb, "0.", "$", "$", "$", "$", "$", "$", ".NOTDEFINED.")
    style = step.add("IFCSURFACESTYLE", "'Guided pavement'", ".BOTH.", f"({appearance})")
    step.add("IFCSTYLEDITEM", surface, f"({style})", "$")
    product_shape = step.add("IFCPRODUCTDEFINITIONSHAPE", "$", "$", f"({body},{axis})")
    # Rotate the source XY pavement toward the camera and make its varying
    # source Z elevations visible as depth. The curve frame's lateral axis is -Y.
    position = step.add("IFCAXIS2PLACEMENT3D", step.point(0., 0., CENTER),
                        step.direction(0., -1., 0.), step.direction(1., 0., 0.))
    placement = step.add("IFCLOCALPLACEMENT", "$", position)
    step.add("IFCBUILDINGELEMENTPROXY", "'0000000000000000000001'", "$", "'Guided pavement'",
             "$", "$", placement, product_shape, "$", "$")
    header = "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('ViewDefinition [DesignTransferView]'),'2;1');\n"
    header += (f"FILE_NAME('{path.name}','2026-11-14T00:00:00',(''),(''),'Container','','');\n"
               "FILE_SCHEMA(('IFC4X3_ADD2'));\nENDSEC;\nDATA;\n")
    path.write_text(header + "\n".join(step.elements) + "\nENDSEC;\nEND-ISO-10303-21;\n", encoding="ascii")


def surface_height(x, y):
    station = 1 - abs(x) / HALF_LENGTH
    lateral = y - CENTER
    width = HALF_WIDTH + (1.02 if lateral >= 0 else -.33) * station
    return CROWN * station * (1 - abs(lateral) / width), station, width


def expected_depth(index, point, pixel, matrix):
    import numpy as np

    ray = world_ray((pixel[0], HEIGHT - 1 - pixel[1]), np.linalg.inv(matrix))
    # Intersect the ray with the analytically ruled ramp. Each shoulder's
    # width changes linearly with station, independently from the crown height.
    low, high = 0., 1.3
    for _ in range(42):
        z = (low + high) / 2
        x, y, _ = (z - DISTANCE) / ray[2] * ray + [0., 0., DISTANCE]
        height, _, _ = surface_height(x, y)
        if height > z:
            low = z
        else:
            high = z
    z = (low + high) / 2
    x, y, _ = (z - DISTANCE) / ray[2] * ray + [0., 0., DISTANCE]
    _, station, width = surface_height(x, y)
    require(station > 0 and abs(y - CENTER) < width, f"Probe outside guided pavement: {point}")
    clip = matrix @ [x, y, z, 1.]
    return clip[2] / clip[3]


def check_pixels(image, fixture):
    import numpy as np
    from PIL import Image

    with Image.open(image) as source:
        pixels = np.asarray(source.convert("RGB"), dtype=np.int16)
    require(pixels.shape == (HEIGHT, WIDTH, 3), "Wrong guided pavement image extent")
    # Keep the color sample in the broad shoulder interior, clear of the
    # projected guide lines and the dark crease at the crown.
    left, right = WIDTH // 2 - 66, WIDTH // 2 + 66
    samples = pixels[105:119, left:right]
    sky = np.max(pixels[:12, :12])
    visible = np.max(samples, axis=2) > sky + 16
    coverage = float(np.mean(visible))
    color = samples[visible].mean(axis=0)
    contrast = float(color.max() - color.min())
    require(coverage > .97 and contrast > 20, f"Missing or unstyled pavement interior: {coverage}, {color.tolist()}")
    return {fixture.products[0].name: dict(visibleFraction=coverage, meanRgb8=color.tolist())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--managed-output", action="store_true")
    parser.add_argument("--generate-only", action="store_true")
    args = parser.parse_args()
    if not args.generate_only and os.environ.get("CONTAINER_RUN_GPU_IFC_AREA_PROFILES") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_IFC_AREA_PROFILES=1 to run IFC geometry captures")
        return 77
    renderer, output = args.renderer.resolve(), args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    else:
        output.mkdir(parents=True)
    model, sequence = output / "guided-section.ifc", output / "depth-witnesses.json"
    write_fixture(model)
    frames = write_sequence(sequence, FIXTURE)
    if args.generate_only:
        return 0
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    results = []
    for technique in TECHNIQUES:
        for taa in (False, True):
            results.append(record(renderer, output, model, sequence, frames, technique, taa, FIXTURE,
                                  expected_depth=expected_depth, check_image=check_pixels))
            save_json(output / "results.json", results)
            print(f"PASS {technique}, TAA {taa}: guided section and {len(frames)} depth samples", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
