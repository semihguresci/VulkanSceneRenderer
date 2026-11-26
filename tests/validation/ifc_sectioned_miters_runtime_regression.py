"""Capture sharp IFC sectioned joins in spatial and curved directrices."""

import argparse
import math
import os
from pathlib import Path
import tempfile

from ifc_area_profiles_runtime_regression import (
    DEPTH, DISTANCE, FOV, HEIGHT, TECHNIQUES, WIDTH, Fixture, Profile,
    StepFixture, record, require, save_json, write_sequence,
)
from ifc_sectioned_guides_runtime_regression import section_point
from ifc_swept_surface_runtime_regression import world_ray


SPATIAL_PATH = ((0., 0., 0.), (8., 0., 0.), (8., 6., 0.), (8., 12., 8.))
SPATIAL_SCALE = .60
SPATIAL_CENTER = (-4.55, -2.45)
ARC_RADIUS = 12.
ARC_WIDTH = .75
GUIDE_RADIUS = ARC_RADIUS + ARC_WIDTH + .55
ARC_SCALE = .26
ARC_CENTER = (1.28, -2.26)
EXIT_CROWN_HEIGHT = .8


def spatial_point(x, y, z):
    # The source path elevations project along the view direction.
    return (SPATIAL_CENTER[0] + SPATIAL_SCALE * x,
            SPATIAL_CENTER[1] + SPATIAL_SCALE * y, SPATIAL_SCALE * z)


def arc_point(x, y, z=0.):
    return (ARC_CENTER[0] + ARC_SCALE * x, ARC_CENTER[1] + ARC_SCALE * y, ARC_SCALE * z)


def arc_at(angle, radius=ARC_RADIUS):
    return arc_point(radius * math.cos(angle), ARC_RADIUS + radius * math.sin(angle))


def probe(point):
    scale = (DISTANCE - DEPTH) / (DISTANCE - point[2])
    return (scale * point[0], scale * point[1])


EXIT_CROWN_PROBE = probe(arc_point(ARC_RADIUS + 3.75, ARC_RADIUS + .25,
                                   EXIT_CROWN_HEIGHT * (1. - .25 / ARC_WIDTH)))


def spatial_row(index, r):
    # Intersection of the width-offset incoming and outgoing planes gives
    # x=8-r and y=r at the first joint. The rise turns about X, leaving that
    # same width offset on both sides of the second joint.
    return spatial_point(*((0, r, 0), (8 - r, r, 0), (8 - r, 6, 0), (8 - r, 12, 8))[index])


FIXTURE = Fixture((
    Profile("Spatial miter", (0., 0.), (.16, .67, .87)),
    Profile("Curved miter", (0., 0.), (.91, .45, .16)),
), (
    (0, "first span", probe(spatial_point(3.5, 0., 0.)), True),
    (0, "first miter outer extension", probe(spatial_point(8.61, -.53, 0.)), True),
    (0, "first miter inside corner", probe(spatial_point(7.24, .67, 0.)), True),
    (0, "first miter outside corner", probe(spatial_point(6.72, 1.29, 0.)), False),
    (0, "middle span", probe(spatial_point(7.92, 3.7, 0.)), True),
    (0, "before grade miter", probe(spatial_point(8.30, 5.78, 0.)), True),
    (0, "after grade miter", probe(spatial_point(7.70, 6.27, .36)), True),
    (0, "rising span", probe(spatial_point(7.81, 8.66, 3.5466666666667)), True),
    (0, "beyond rising side", probe(spatial_point(9.40, 8.66, 3.5466666666667)), False),
    (1, "arc near start", probe(arc_at(-math.radians(76))), True),
    (1, "arc interior", probe(arc_at(-math.radians(49), ARC_RADIUS + .12)), True),
    (1, "guided widening beyond the original arc",
     probe(arc_at(-math.radians(45), ARC_RADIUS + 1.04)), True),
    (1, "clear inside trimmed arc", probe(arc_at(-math.radians(52), ARC_RADIUS - 1.6)), False),
    (1, "clear outside swept arc", probe(arc_at(-math.radians(55), ARC_RADIUS + 1.6)), False),
    (1, "before circular miter", probe(arc_at(-math.radians(18))), True),
    (1, "after circular miter", probe(arc_point(ARC_RADIUS + 1.74, ARC_RADIUS + .37)), True),
    (1, "vertical crown guided on the directrix", EXIT_CROWN_PROBE, True),
    (1, "far straight exit", probe(arc_point(ARC_RADIUS + 5.4, ARC_RADIUS - .26)), True),
), None, None)


def polyline(step, points):
    return step.add("IFCPOLYLINE", "(" + ",".join(step.point(*p) for p in points) + ")")


def write_fixture(path):
    step = StepFixture()
    step.add("IFCSIUNIT", "*", ".LENGTHUNIT.", "$", ".METRE.")
    world = step.add("IFCAXIS2PLACEMENT3D", step.point(0., 0., 0.), "$", "$")
    context = step.add("IFCGEOMETRICREPRESENTATIONCONTEXT", "$", "'Model'", 3, 1e-6, world, "$")
    spatial = polyline(step, SPATIAL_PATH)
    center = step.add("IFCAXIS2PLACEMENT3D", step.point(0., ARC_RADIUS, 0.), "$", step.direction(0., -1., 0.))
    circle = step.add("IFCCIRCLE", center, ARC_RADIUS)
    arc = step.add("IFCTRIMMEDCURVE", circle, "(IFCPARAMETERVALUE(0.))",
                   f"(IFCPARAMETERVALUE({math.pi / 2}))", ".T.", ".PARAMETER.")
    exit_line = polyline(step, ((ARC_RADIUS, ARC_RADIUS, 0.), (ARC_RADIUS + 6., ARC_RADIUS, 0.)))
    first_segment = step.add("IFCCOMPOSITECURVESEGMENT", ".CONTINUOUS.", ".T.", arc)
    second_segment = step.add("IFCCOMPOSITECURVESEGMENT", ".DISCONTINUOUS.", ".T.", exit_line)
    curved = step.add("IFCCOMPOSITECURVE", f"({first_segment},{second_segment})", ".F.")
    for index, (directrix, extent, width, scale, center_xy) in enumerate((
        (spatial, 24., 1., SPATIAL_SCALE, SPATIAL_CENTER),
        (curved, ARC_RADIUS * math.pi / 2 + 6., ARC_WIDTH, ARC_SCALE, ARC_CENTER),
    )):
        product = FIXTURE.products[index]
        positions = [step.add("IFCAXIS2PLACEMENTLINEAR", section_point(step, station, directrix), "$", "$")
                     for station in (0., extent)]
        profile = step.add("IFCOPENCROSSPROFILEDEF", ".CURVE.", "$", ".T.", f"({width},{width})", "(0.,0.)",
                           "('left','center','right')", step.point(-width, 0.))
        surface = step.add("IFCSECTIONEDSURFACE", directrix, f"({','.join(positions)})", f"({profile},{profile})")
        rgb = step.add("IFCCOLOURRGB", "$", *product.color)
        appearance = step.add("IFCSURFACESTYLERENDERING", rgb, "0.", "$", "$", "$", "$", "$", "$", ".NOTDEFINED.")
        style = step.add("IFCSURFACESTYLE", f"'{product.name}'", ".BOTH.", f"({appearance})")
        step.add("IFCSTYLEDITEM", surface, f"({style})", "$")
        body = step.add("IFCSHAPEREPRESENTATION", context, "'Body'", "'SectionedSurface'", f"({surface})")
        mapping = step.add("IFCREPRESENTATIONMAP", world, body)
        target = step.add("IFCCARTESIANTRANSFORMATIONOPERATOR3D", "$", "$", step.point(0., 0., 0.), scale, "$")
        item = step.add("IFCMAPPEDITEM", mapping, target)
        rep = step.add("IFCSHAPEREPRESENTATION", context, "'Body'", "'MappedRepresentation'", f"({item})")
        representations = [rep]
        if directrix == curved:
            # The separately authored outer rail widens the arc, intersects
            # the half-angle row at the unchanged corner, then follows the exit.
            knots = [(0., -ARC_WIDTH, 0.)]
            for angle in (-67.5, -45., -22.5):
                theta = math.radians(angle)
                knots.append((GUIDE_RADIUS * math.cos(theta),
                              ARC_RADIUS + GUIDE_RADIUS * math.sin(theta), 0.))
            knots.extend(((ARC_RADIUS + ARC_WIDTH, ARC_RADIUS - ARC_WIDTH, 0.),
                          (ARC_RADIUS + 6., ARC_RADIUS - ARC_WIDTH, 0.)))
            rail = polyline(step, knots)
            position = section_point(step, 0., rail, 0., 0.)
            guide = step.add("IFCOFFSETCURVEBYDISTANCES", rail, f"({position})", "'left'")
            arc_length = ARC_RADIUS * math.pi / 2.
            crown_knots = [section_point(step, arc_length + s, directrix, 0., height)
                           for s, height in ((2.4, 0.), (3.1, EXIT_CROWN_HEIGHT),
                                             (4.4, EXIT_CROWN_HEIGHT), (4.75, 0.))]
            crown_knots.insert(0, section_point(step, 0., directrix, 0., 0.))
            crown = step.add("IFCOFFSETCURVEBYDISTANCES", directrix,
                             f"({','.join(crown_knots)})", "'center'")
            guides = step.add("IFCGEOMETRICCURVESET", f"({guide},{crown})")
            axis = step.add("IFCSHAPEREPRESENTATION", context, "'Axis'", "'GeometricCurveSet'", f"({guides})")
            axis_map = step.add("IFCREPRESENTATIONMAP", world, axis)
            axis_item = step.add("IFCMAPPEDITEM", axis_map, target)
            representations.append(step.add("IFCSHAPEREPRESENTATION", context, "'Axis'", "'MappedRepresentation'",
                                            f"({axis_item})"))
        shape = step.add("IFCPRODUCTDEFINITIONSHAPE", "$", "$", f"({','.join(representations)})")
        position = step.add("IFCAXIS2PLACEMENT3D", step.point(center_xy[0], 0., center_xy[1]),
                            step.direction(0., -1., 0.), step.direction(1., 0., 0.))
        placement = step.add("IFCLOCALPLACEMENT", "$", position)
        step.add("IFCBUILDINGELEMENTPROXY", f"'{index + 1:022d}'", "$", f"'{product.name}'",
                 "$", "$", placement, shape, "$", "$")
    header = "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('ViewDefinition [DesignTransferView]'),'2;1');\n"
    header += (f"FILE_NAME('{path.name}','2026-11-14T00:00:00',(''),(''),'Container','','');\n"
               "FILE_SCHEMA(('IFC4X3_ADD2'));\nENDSEC;\nDATA;\n")
    path.write_text(header + "\n".join(step.elements) + "\nENDSEC;\nEND-ISO-10303-21;\n", encoding="ascii")


def spatial_hit(camera, ray):
    import numpy as np

    intersections = []
    for i in range(3):
        a, b, c, d = map(np.array, (spatial_row(i, -1), spatial_row(i + 1, -1),
                                    spatial_row(i + 1, 1), spatial_row(i, 1)))
        normal = np.cross(b - a, c - a)
        denominator = normal @ ray
        if abs(denominator) < 1e-12:
            continue
        t = (normal @ (a - camera)) / denominator
        hit = camera + t * ray
        corners = (a, b, c, d)
        if t > 0 and all(normal @ np.cross(end - start, hit - start) >= -1e-9
                         for start, end in zip(corners, (*corners[1:], a), strict=True)):
            intersections.append(t)
    return min(intersections, default=None)


def expected_depth(index, point, pixel, matrix):
    import numpy as np

    camera = np.array([0., 0., DISTANCE])
    ray = world_ray((pixel[0], HEIGHT - 1 - pixel[1]), np.linalg.inv(matrix))
    if index == 0:
        t = spatial_hit(camera, ray)
    elif point == EXIT_CROWN_PROBE:
        # On the right half of the plateau the guided cross section runs
        # linearly from the raised center to the original rail at zero height.
        slope = EXIT_CROWN_HEIGHT / ARC_WIDTH
        center_y = arc_point(0., ARC_RADIUS)[1]
        t = (ARC_SCALE * EXIT_CROWN_HEIGHT + slope * center_y - DISTANCE) / (ray[2] + slope * ray[1])
    else:
        t = -DISTANCE / ray[2]
    require(t is not None, f"Probe outside analytic miter polygons: {point}")
    clip = matrix @ [*(camera + t * ray), 1.]
    return clip[2] / clip[3]


def check_pixels(image, _fixture):
    import numpy as np
    from PIL import Image

    with Image.open(image) as source:
        pixels = np.asarray(source.convert("RGB"), dtype=np.int16)
    require(pixels.shape == (HEIGHT, WIDTH, 3), "Wrong sharp section image extent")
    sky = np.max(pixels[:12, :12])
    scale = HEIGHT / (2 * math.tan(math.radians(FOV / 2)))
    regions = (
        ("Spatial first span", spatial_point(3.5, 0., 0.)),
        ("Spatial second span", spatial_point(8., 3., 0.)),
        ("Spatial rising span", spatial_point(8., 8.4, 3.2)),
        ("Far curved interior", arc_at(-math.radians(49))),
        ("Straight exit", arc_point(ARC_RADIUS + 3.6, ARC_RADIUS)),
    )
    results = {}
    for label, (x, y, z) in regions:
        px, py = int(WIDTH / 2 + x * scale / (DISTANCE - z)), int(HEIGHT / 2 - y * scale / (DISTANCE - z))
        samples = pixels[py - 4:py + 5, px - 4:px + 5]
        coverage = float(np.mean(np.max(samples, axis=2) > sky + 16))
        color = samples.mean(axis=(0, 1))
        contrast = float(color.max() - color.min())
        require(samples.shape == (9, 9, 3) and coverage > .97 and contrast > 20,
                f"Missing or unstyled {label}: {coverage}, {color.tolist()}")
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
    model, sequence = output / "sharp-sections.ifc", output / "depth-witnesses.json"
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
            print(f"PASS {technique}, TAA {taa}: sharp sectioned miters and {len(frames)} depth samples", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
