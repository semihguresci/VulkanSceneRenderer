"""Capture a widening IFC sectioned surface with tagged split and merge tips."""

import argparse
import math
import os
from pathlib import Path
import tempfile

from ifc_area_profiles_runtime_regression import (
    DISTANCE, HEIGHT, TECHNIQUES, WIDTH, Fixture, Profile, StepFixture,
    record, require, save_json, write_sequence,
)
from ifc_sectioned_guides_runtime_regression import section_point
from ifc_swept_surface_runtime_regression import world_ray


HALF_LENGTH = 4.
OUTER_EDGE = 1.44
LANE_HEIGHT = .45
LANE_RIDGE = .30
LANE_EDGE = 1.12
TIP = 2.65
FULL = 1.16
FIXTURE = Fixture((Profile("Tagged widening", (0., 0.), (.19, .70, .83)),), (
    (0, "flat pavement before split", (-3.53, LANE_RIDGE), True),
    (0, "rising lane during split", (-2.00, LANE_RIDGE + .12), True),
    (0, "lower outside pavement", (0., -.81), True),
    (0, "lower transition face", (-.46, -.19), True),
    (0, "active widening lane", (.00, .58), True),
    (0, "outer half of widening lane", (.52, .91), True),
    (0, "upper outside pavement", (.13, 1.31), True),
    (0, "falling lane during merge", (1.90, LANE_RIDGE + .12), True),
    (0, "flat pavement after merge", (3.52, LANE_RIDGE), True),
    (0, "clear below pavement", (-.29, -1.61), False),
    (0, "clear above pavement", (.34, 1.63), False),
    (0, "clear beyond sectioned end", (4.34, LANE_RIDGE), False),
), None, None)


def write_fixture(path):
    step = StepFixture()
    step.add("IFCSIUNIT", "*", ".LENGTHUNIT.", "$", ".METRE.")
    world = step.add("IFCAXIS2PLACEMENT3D", step.point(0., 0., 0.), "$", "$")
    context = step.add("IFCGEOMETRICREPRESENTATIONCONTEXT", "$", "'Model'", 3, 1e-6, world, "$")
    start, end = step.point(-HALF_LENGTH, 0., 0.), step.point(HALF_LENGTH, 0., 0.)
    directrix = step.add("IFCPOLYLINE", f"({start},{end})")
    locations = [step.add("IFCAXIS2PLACEMENTLINEAR", section_point(step, x + HALF_LENGTH, directrix), "$", "$")
                 for x in (-HALF_LENGTH, -TIP, -FULL, FULL, TIP, HALF_LENGTH)]
    origin = step.point(-OUTER_EDGE, 0.)

    def profile(lane, lane_tags):
        points = ((-OUTER_EDGE, 0.), *lane, (OUTER_EDGE, 0.))
        widths = [b[0] - a[0] for a, b in zip(points, points[1:])]
        slopes = [math.atan2(b[1] - a[1], b[0] - a[0]) for a, b in zip(points, points[1:])]
        tags = ("'A'", *lane_tags, "'C'")
        return step.add("IFCOPENCROSSPROFILEDEF", ".CURVE.", "$", ".T.",
                        "(" + ",".join(map(str, widths)) + ")",
                        "(" + ",".join(map(str, slopes)) + ")",
                        "(" + ",".join(tags) + ")", origin)

    # D is absent only outside the transition. B and D coincide at its
    # authored appearance and disappearance stations.
    closed = profile(((LANE_RIDGE, 0.),), ("'B'",))
    tip = profile(((LANE_RIDGE, 0.), (LANE_RIDGE, 0.)), ("'B'", "'D'"))
    opened = profile(((LANE_RIDGE, LANE_HEIGHT), (LANE_EDGE, 0.)), ("'B'", "'D'"))
    sections = (closed, tip, opened, opened, tip, closed)
    surface = step.add("IFCSECTIONEDSURFACE", directrix, f"({','.join(locations)})", f"({','.join(sections)})")
    body = step.add("IFCSHAPEREPRESENTATION", context, "'Body'", "'SectionedSurface'", f"({surface})")
    rgb = step.add("IFCCOLOURRGB", "$", *FIXTURE.products[0].color)
    appearance = step.add("IFCSURFACESTYLERENDERING", rgb, "0.", "$", "$", "$", "$", "$", "$", ".NOTDEFINED.")
    style = step.add("IFCSURFACESTYLE", "'Tagged widening'", ".BOTH.", f"({appearance})")
    step.add("IFCSTYLEDITEM", surface, f"({style})", "$")
    shape = step.add("IFCPRODUCTDEFINITIONSHAPE", "$", "$", f"({body})")
    position = step.add("IFCAXIS2PLACEMENT3D", step.point(0., 0., 0.),
                        step.direction(0., -1., 0.), step.direction(1., 0., 0.))
    placement = step.add("IFCLOCALPLACEMENT", "$", position)
    step.add("IFCBUILDINGELEMENTPROXY", "'0000000000000000000001'", "$", "'Tagged widening'",
             "$", "$", placement, shape, "$", "$")
    header = "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('ViewDefinition [DesignTransferView]'),'2;1');\n"
    header += (f"FILE_NAME('{path.name}','2026-11-14T00:00:00',(''),(''),'Container','','');\n"
               "FILE_SCHEMA(('IFC4X3_ADD2'));\nENDSEC;\nDATA;\n")
    path.write_text(header + "\n".join(step.elements) + "\nENDSEC;\nEND-ISO-10303-21;\n", encoding="ascii")


def lane_height(x, y):
    openness = min(1., max(0., (TIP - abs(x)) / (TIP - FULL)))
    if not openness:
        return 0.
    if y < LANE_RIDGE:
        return openness * LANE_HEIGHT * (y + OUTER_EDGE) / (LANE_RIDGE + OUTER_EDGE)
    # The D edge moves laterally away from B while B gains elevation.
    span = openness * (LANE_EDGE - LANE_RIDGE)
    return openness * LANE_HEIGHT * max(0., 1 - (y - LANE_RIDGE) / span)


def expected_depth(_index, point, pixel, matrix):
    import numpy as np

    ray = world_ray((pixel[0], HEIGHT - 1 - pixel[1]), np.linalg.inv(matrix))
    camera = np.array([0., 0., DISTANCE])
    low, high = 0., LANE_HEIGHT
    for _ in range(38):
        z = (low + high) / 2
        x, y, _ = camera + (z - DISTANCE) / ray[2] * ray
        if lane_height(x, y) > z:
            low = z
        else:
            high = z
    z = (low + high) / 2
    x, y, _ = camera + (z - DISTANCE) / ray[2] * ray
    require(abs(x) < HALF_LENGTH and abs(y) < OUTER_EDGE, f"Probe outside the authored pavement: {point}")
    clip = matrix @ [x, y, z, 1.]
    return clip[2] / clip[3]


def check_pixels(image, _fixture):
    import numpy as np
    from PIL import Image

    with Image.open(image) as source:
        pixels = np.asarray(source.convert("RGB"), dtype=np.int16)
    require(pixels.shape == (HEIGHT, WIDTH, 3), "Wrong tagged section image extent")
    sky = np.max(pixels[:12, :12])
    results = {}
    for label, top, bottom in (("lower transition", 194, 201), ("widened lane", 158, 171),
                               ("upper pavement", 132, 139)):
        samples = pixels[top:bottom, WIDTH // 2 - 30:WIDTH // 2 + 30]
        visible = np.max(samples, axis=2) > sky + 16
        coverage = float(np.mean(visible))
        color = samples.mean(axis=(0, 1))
        contrast = float(color.max() - color.min())
        require(coverage > .97 and contrast > 20, f"Missing or unstyled {label}: {coverage}, {color.tolist()}")
        results[label] = dict(visibleFraction=coverage, meanRgb8=color.tolist())
    return results


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
    model, sequence = output / "tagged-section.ifc", output / "depth-witnesses.json"
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
            print(f"PASS {technique}, TAA {taa}: tagged lane transitions and {len(frames)} depth samples", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
