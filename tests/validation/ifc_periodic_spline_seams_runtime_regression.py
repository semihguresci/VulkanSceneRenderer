"""Capture exact IFC spline shells with authored pcurves and bare 3D trims."""

import argparse
import math
import os
from pathlib import Path
import tempfile

from ifc_area_profiles_runtime_regression import (
    DEPTH, DISTANCE, FOV, HEIGHT, TECHNIQUES, WIDTH, Fixture, Profile,
    StepFixture, record, require, save_json, write_sequence,
)
from ifc_revolution_axis_runtime_regression import advanced_face, edge
from ifc_spherical_edge_loops_runtime_regression import placement
from ifc_swept_surface_runtime_regression import world_ray


CIRCLE = ((1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1), (1, 0))
WEIGHTS = tuple(1. if i % 2 == 0 else math.sqrt(.5) for i in range(len(CIRCLE)))
MULTIPLICITIES = (3, 2, 2, 2, 3)
ANGULAR_KNOTS = (2.75, 4.25, 5.75, 7.25, 8.75)
TUBE_KNOTS = (-4.125, -3.375, -2.625, -1.875, -1.125)
AXIS_KNOTS = (-2.4, .8)
RADIUS, HALF_HEIGHT = 1.03, 1.47
TORUS_MAJOR, TORUS_TUBE = 1.20, .42
PRODUCTS = (
    Profile("U-closed unclamped rational cylinder", (-3.61, -.06), (.12, .63, .90)),
    Profile("V-closed unclamped rational cylinder", (3.61, -.06), (.92, .40, .13)),
    Profile("Doubly closed rational torus", (0., 1.77), (.20, .84, .36)),
    Profile("Mixed winding rational torus", (0., -1.77), (.90, .24, .67)),
)
CAP_COLOR = (.68, .36, .84)
BAND_COLOR = (.15, .77, .86)
X_AXIS, Y_AXIS, Z_AXIS = (1., 0., 0.), (0., 1., 0.), (0., 0., 1.)


def cylinder_point(x, y):
    return x, y, math.sqrt(RADIUS ** 2 - x ** 2)


def torus_point(r, angle):
    return (r * math.cos(angle), r * math.sin(angle),
            math.sqrt(TORUS_TUBE ** 2 - (r - TORUS_MAJOR) ** 2))


def probe(index, name, point):
    x, y, z = point
    cx, cy = PRODUCTS[index].center
    scale = (DISTANCE - DEPTH) / (DISTANCE - z)
    return index, name, ((cx + x) * scale - cx, (cy + y) * scale - cy), True


FIXTURE = Fixture(PRODUCTS, (
    probe(0, "near-side U seam", cylinder_point(0., .83)),
    probe(0, "left of U seam", cylinder_point(-.66, -.55)),
    probe(0, "right of U seam", cylinder_point(.58, .14)),
    probe(0, "near top U ring", cylinder_point(-.28, 1.29)),
    probe(0, "where the U seam approaches the bare lower ring", cylinder_point(.02, -1.38)),
    (0, "outside side silhouette", (-1.43, -.36), False),
    (0, "above upper end", (.0, 1.80), False),
    probe(1, "near-side V seam", cylinder_point(0., -.86)),
    probe(1, "left of V seam", cylinder_point(-.64, .56)),
    probe(1, "right of V seam", cylinder_point(.68, -.44)),
    probe(1, "near bottom V ring", cylinder_point(.31, -1.29)),
    probe(1, "where the V seam approaches the bare lower ring", cylinder_point(-.02, -1.38)),
    (1, "outside side silhouette", (1.40, .33), False),
    (1, "below lower end", (.0, -1.80), False),
    probe(2, "on major chart seam", torus_point(1.26, 0.)),
    probe(2, "on tube chart seam", torus_point(TORUS_MAJOR, 1.25)),
    probe(2, "near inner rim", torus_point(.88, 2.25)),
    probe(2, "far quadrant", torus_point(1.25, 4.2)),
    (2, "clear central hole", (.12, -.14), False),
    (2, "clear beyond outer ring", (1.88, 0.), False),
    probe(3, "across rising mixed winding", torus_point(1.25, .32)),
    probe(3, "far side of mixed winding", torus_point(1.13, 2.74)),
    probe(3, "crown between shared curves", torus_point(TORUS_MAJOR, 4.28)),
    probe(3, "toward inner radius", torus_point(.91, 5.29)),
    probe(3, "on bare mixed winding edge", torus_point(TORUS_MAJOR, 0.)),
    (3, "clear mixed winding bore", (.05, -.11), False),
    (3, "clear beyond mixed winding ring", (-1.88, 0.), False),
), None, None)


def step_list(values):
    return "(" + ",".join(map(str, values)) + ")"


def rational_circle(step, points, knots):
    return step.add("IFCRATIONALBSPLINECURVEWITHKNOTS", 2, step_list(points), ".UNSPECIFIED.",
                    ".T.", ".F.", step_list(MULTIPLICITIES), step_list(knots), ".UNSPECIFIED.", step_list(WEIGHTS))


def rational_surface(step, points, weights, axes, closed):
    return step.add(
        "IFCRATIONALBSPLINESURFACEWITHKNOTS", *(axis[0] for axis in axes),
        step_list(step_list(row) for row in points), ".UNSPECIFIED.",
        *(".T." if is_closed else ".F." for is_closed in closed), ".F.",
        *(step_list(axis[1]) for axis in axes), *(step_list(axis[2]) for axis in axes),
        ".UNSPECIFIED.", step_list(step_list(row) for row in weights),
    )


def polyline(step, points):
    return step.add("IFCPOLYLINE", step_list(points))


def associated(step, context, kind, curve3d, segments, preferred=".CURVE3D."):
    pcurves = []
    for surface, a, b in segments:
        curve2d = polyline(step, (step.point(*a), step.point(*b)))
        definition = step.add("IFCDEFINITIONALREPRESENTATION", context, "$", "'Curve2D'", step_list((curve2d,)))
        pcurves.append(step.add("IFCPCURVE", surface, definition))
    return step.add(kind, curve3d, step_list(pcurves), preferred)


def cylinder(step, context, product, *, periodic_u, rim_pcurves):
    rings = [[step.point(RADIUS * s, y, RADIUS * c) for c, s in CIRCLE]
             for y in (-HALF_HEIGHT, HALF_HEIGHT)]
    bottom, top = [step.add("IFCVERTEXPOINT", ring[0]) for ring in rings]
    # The first and last exterior knots do not enter the active basis, so the
    # unclamped surface still agrees exactly with the clamped 3D rim curves.
    circular_axis = (2, (1, 2, 2, 2, 2, 2, 1), (1.25, *ANGULAR_KNOTS, 10.25))
    straight_axis = (1, (2, 2), AXIS_KNOTS)
    weights = [WEIGHTS, WEIGHTS]
    if periodic_u:
        points = list(zip(*rings, strict=True))
        weights = list(zip(*weights, strict=True))
        axes = (circular_axis, straight_axis)
    else:
        points, axes = rings, (straight_axis, circular_axis)
    surface = rational_surface(step, points, weights, axes, (periodic_u, not periodic_u))
    def uv(angular, linear):
        return (angular, linear) if periodic_u else (linear, angular)

    u0, u1 = ANGULAR_KNOTS[0], ANGULAR_KNOTS[-1]
    rims = []
    for v, ring, linear, has_pcurve in zip((bottom, top), rings, AXIS_KNOTS, rim_pcurves, strict=True):
        curve = rational_circle(step, ring, ANGULAR_KNOTS)
        if has_pcurve:
            curve = associated(step, context, "IFCSURFACECURVE", curve,
                               ((surface, uv(u0, linear), uv(u1, linear)),))
        rims.append(edge(step, v, v, curve))
    native = [(surface, uv(angular, AXIS_KNOTS[0]), uv(angular, AXIS_KNOTS[1])) for angular in (u0, u1)]
    seam_curve = associated(step, context, "IFCSEAMCURVE",
                            polyline(step, (rings[0][0], rings[1][0])), native)
    cut = edge(step, bottom, top, seam_curve)
    low, high = rims
    wall = advanced_face(step, surface, (((low, True), (cut, True), (high, False), (cut, False)),),
                         product.name, product.color, same_sense=periodic_u)
    face_low = step.add("IFCPLANE", placement(step, (0., -HALF_HEIGHT, 0.), (0., -1., 0.), Z_AXIS))
    face_high = step.add("IFCPLANE", placement(step, (0., HALF_HEIGHT, 0.), Y_AXIS, Z_AXIS))
    end_low = advanced_face(step, face_low, (((low, False),),), "Lower disk", CAP_COLOR)
    end_high = advanced_face(step, face_high, (((high, True),),), "Upper disk", CAP_COLOR)
    return wall, end_low, end_high


def torus_surface(step, tube, points=None):
    # Tensor-product exact revolution of the minor circle around the major axis.
    if points is None:
        points = [[step.point((TORUS_MAJOR + TORUS_TUBE * c_v) * c_u,
                              (TORUS_MAJOR + TORUS_TUBE * c_v) * s_u, TORUS_TUBE * s_v)
                   for c_v, s_v in tube] for c_u, s_u in CIRCLE]
    weights = [[u * v for v in WEIGHTS] for u in WEIGHTS]
    surface = rational_surface(step, points, weights,
                               ((2, MULTIPLICITIES, ANGULAR_KNOTS), (2, MULTIPLICITIES, TUBE_KNOTS)),
                               (True, True))
    return surface, points


def torus(step, context, product):
    # A quarter-turn puts both authored seams on camera-facing portions.
    tube = tuple((-s, c) for c, s in CIRCLE)
    surface, points = torus_surface(step, tube)
    vertex = step.add("IFCVERTEXPOINT", points[0][0])
    u0, u1 = ANGULAR_KNOTS[0], ANGULAR_KNOTS[-1]
    v0, v1 = TUBE_KNOTS[0], TUBE_KNOTS[-1]
    major = rational_circle(step, [row[0] for row in points], ANGULAR_KNOTS)
    minor = rational_circle(step, points[0], TUBE_KNOTS)
    seam_u = associated(step, context, "IFCSEAMCURVE", major,
                        ((surface, (u0, v0), (u1, v0)), (surface, (u0, v1), (u1, v1))))
    seam_v = associated(step, context, "IFCSEAMCURVE", minor,
                        ((surface, (u0, v0), (u0, v1)), (surface, (u1, v0), (u1, v1))))
    edge_u, edge_v = edge(step, vertex, vertex, seam_u), edge(step, vertex, vertex, seam_v)
    face = advanced_face(step, surface, (((edge_u, True), (edge_v, True), (edge_u, False), (edge_v, False)),),
                         product.name, product.color)
    return (face,)


def helix_curve(step, quarter_offset):
    # The two turns around the main circle take eight quarter spans. On
    # each, the tube takes half a quarter. Multiply the homogeneous
    # quadratic Bezier data to obtain the exact quartic torus curve.
    circle = tuple((x * w, y * w, w) for (x, y), w in zip(CIRCLE, WEIGHTS, strict=True))
    controls, weights = [], []
    for span in range(8):
        u = [circle[(2 * span + i) % 8] for i in range(3)]
        v = [circle[(2 * (span // 2 + quarter_offset) + i) % 8] for i in range(3)]
        left = tuple((a + b) / 2. for a, b in zip(v[0], v[1], strict=True))
        right = tuple((a + b) / 2. for a, b in zip(v[1], v[2], strict=True))
        middle = tuple((a + b) / 2. for a, b in zip(left, right, strict=True))
        v = (v[0], left, middle) if span % 2 == 0 else (middle, right, v[2])
        product = [[0.] * 4 for _ in range(5)]
        for i, (x, y, u_weight) in enumerate(u):
            for j, (tube_x, tube_y, v_weight) in enumerate(v):
                radius = TORUS_MAJOR * v_weight + TORUS_TUBE * tube_x
                point = (radius * x, radius * y, TORUS_TUBE * u_weight * tube_y, u_weight * v_weight)
                fraction = math.comb(2, i) * math.comb(2, j) / math.comb(4, i + j)
                for component, value in enumerate(point):
                    product[i + j][component] += value * fraction
        for x, y, z, weight in product[0 if span == 0 else 1:]:
            controls.append(step.point(x / weight, y / weight, z / weight))
            weights.append(weight)
    curve = step.add("IFCRATIONALBSPLINECURVEWITHKNOTS", 4, step_list(controls), ".UNSPECIFIED.", ".T.", ".F.",
                     "(5,4,4,4,4,4,4,4,5)", "(1,2,3,4,5,6,7,8,9)", ".UNSPECIFIED.", step_list(weights))
    return step.add("IFCVERTEXPOINT", controls[0]), curve


def mixed_torus(step, context, product):
    lower, points = torus_surface(step, CIRCLE)
    upper, _ = torus_surface(step, CIRCLE, points)
    u0, u1 = ANGULAR_KNOTS[0], ANGULAR_KNOTS[-1]
    v0, v1 = TUBE_KNOTS[0], TUBE_KNOTS[-1]
    du, dv = u1 - u0, v1 - v0
    boundaries = []
    for quarter in (0, 1):
        vertex, curve = helix_curve(step, quarter)
        # One edge has authored pcurves offset by one full U period across
        # the two faces. The other edge is shared native 3D geometry only.
        if quarter == 0:
            lifts = ((lower, (u0, v0), (u0 + 2 * du, v0 + dv)),
                     (upper, (u0 - du, v0), (u0 + du, v0 + dv)))
            curve = associated(step, context, "IFCSURFACECURVE", curve, lifts, ".PCURVE_S1.")
        boundaries.append(edge(step, vertex, vertex, curve))

    # A periodic band has two noncontractible IFCFACEBOUND rings.
    def band(surface, first, second, name, color):
        bounds = []
        for ref, direction in ((first, ".T."), (second, ".F.")):
            use = step.add("IFCORIENTEDEDGE", "*", "*", ref, direction)
            loop = step.add("IFCEDGELOOP", step_list((use,)))
            bounds.append(step.add("IFCFACEBOUND", loop, ".T."))
        face = step.add("IFCADVANCEDFACE", step_list(bounds), surface, ".T.")
        rgb = step.add("IFCCOLOURRGB", "$", *color)
        rendering = step.add("IFCSURFACESTYLERENDERING", rgb, "0.", "$", "$", "$", "$", "$", "$", ".NOTDEFINED.")
        style = step.add("IFCSURFACESTYLE", f"'{name}'", ".BOTH.", step_list((rendering,)))
        step.add("IFCSTYLEDITEM", face, step_list((style,)), "$")
        return face

    a, b = boundaries
    return (band(lower, a, b, product.name, product.color), band(upper, b, a, "Complementary band", BAND_COLOR))


def write_fixture(path):
    step = StepFixture()
    step.add("IFCSIUNIT", "*", ".LENGTHUNIT.", "$", ".METRE.")
    step.add("IFCSIUNIT", "*", ".PLANEANGLEUNIT.", "$", ".RADIAN.")
    origin = placement(step, (0., 0., 0.), Z_AXIS, X_AXIS)
    context = step.add("IFCGEOMETRICREPRESENTATIONCONTEXT", "$", "'Model'", 3, 1e-6, origin, "$")
    # Both caps share the exact rim EdgeCurves with the spline wall. The U wall
    # gets both rim trims from 3D, and the V wall mixes a 3D rim and a pcurve rim.
    shells = (cylinder(step, context, PRODUCTS[0], periodic_u=True, rim_pcurves=(False, False)),
              cylinder(step, context, PRODUCTS[1], periodic_u=False, rim_pcurves=(False, True)),
              torus(step, context, PRODUCTS[2]), mixed_torus(step, context, PRODUCTS[3]))
    for index, (product, faces) in enumerate(zip(PRODUCTS, shells, strict=True)):
        shell = step.add("IFCCLOSEDSHELL", step_list(faces))
        brep = step.add("IFCADVANCEDBREP", shell)
        rep = step.add("IFCSHAPEREPRESENTATION", context, "'Body'", "'AdvancedBrep'", step_list((brep,)))
        shape = step.add("IFCPRODUCTDEFINITIONSHAPE", "$", "$", step_list((rep,)))
        x, y = product.center
        position = placement(step, (x, 0., y), (0., -1., 0.), X_AXIS)
        local = step.add("IFCLOCALPLACEMENT", "$", position)
        step.add("IFCBUILDINGELEMENTPROXY", f"'{index + 1:022d}'", "$", f"'{product.name}'",
                 "$", "$", local, shape, "$", "$")
    header = "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('ViewDefinition [DesignTransferView]'),'2;1');\n"
    header += (f"FILE_NAME('{path.name}','2026-11-14T00:00:00',(''),(''),'Container','','');\n"
               "FILE_SCHEMA(('IFC4'));\nENDSEC;\nDATA;\n")
    path.write_text(header + "\n".join(step.elements) + "\nENDSEC;\nEND-ISO-10303-21;\n", encoding="ascii")


def cylinder_hit(camera, ray):
    a = ray[0] ** 2 + ray[2] ** 2
    b = camera[0] * ray[0] + camera[2] * ray[2]
    c = camera[0] ** 2 + camera[2] ** 2 - RADIUS ** 2
    discriminant = b * b - a * c
    if discriminant < 0.:
        return None
    t = (-b - math.sqrt(discriminant)) / a
    return t if abs(camera[1] + t * ray[1]) < HALF_HEIGHT else None


def torus_hit(camera, ray):
    import numpy as np
    from numpy.polynomial.polynomial import polymul, polyroots

    # Along p(t), (p·p + R² - r²)² - 4R²(p.x² + p.y²) = 0.
    sphere = (camera @ camera + TORUS_MAJOR ** 2 - TORUS_TUBE ** 2,
              2. * camera @ ray, ray @ ray)
    radial = (camera[:2] @ camera[:2], 2. * camera[:2] @ ray[:2], ray[:2] @ ray[:2])
    polynomial = polymul(sphere, sphere)
    polynomial[:3] -= 4. * TORUS_MAJOR ** 2 * np.asarray(radial)
    real_roots = [t.real for t in polyroots(polynomial) if abs(t.imag) < 1e-8 and t.real > 0.]
    return min(real_roots, default=None)


def expected_depth(index, point, pixel, matrix):
    import numpy as np

    ray = world_ray((pixel[0], HEIGHT - 1 - pixel[1]), np.linalg.inv(matrix))
    cx, cy = PRODUCTS[index].center
    camera = np.array([-cx, -cy, DISTANCE])
    t = torus_hit(camera, ray) if index >= 2 else cylinder_hit(camera, ray)
    require(t is not None, f"Positive depth witness outside the analytic surface: {point}")
    clip = matrix @ [*(camera + t * ray + [cx, cy, 0.]), 1.]
    return clip[2] / clip[3]


def circle_parameter_fraction(angle):
    import numpy as np

    quadrants = np.mod(angle, math.tau) / (math.pi / 2.)
    quadrant = np.floor(quadrants)
    t = np.linspace(0., 1., 129)
    tangent = 2. * t * (1. - t) * math.sqrt(.5)
    native_angles = np.arctan2(t * t + tangent, (1. - t) ** 2 + tangent)
    local = np.interp((quadrants - quadrant) * math.pi / 2., native_angles, t)
    return (quadrant + local) / 4.


def check_pixels(image, _fixture):
    import numpy as np
    from PIL import Image

    with Image.open(image) as source:
        pixels = np.asarray(source.convert("RGB"), dtype=np.int16)
    require(pixels.shape == (HEIGHT, WIDTH, 3), "Wrong periodic spline capture extent")
    sky = int(pixels[:12, :12].max())
    visible = pixels.max(axis=2) > sky + 16
    yy, xx = np.mgrid[:HEIGHT, :WIDTH]
    projection = HEIGHT / (2. * math.tan(math.radians(FOV / 2.)))
    masks = []
    for product, dominant, opposite in zip(PRODUCTS[:2], (2, 0), (0, 2), strict=True):
        scale = projection / (DISTANCE - RADIUS)
        cx, cy = product.center
        # A wide strip around each near-side cut. The two tubes are small
        # compared with the camera distance, so this is safely inside the wall.
        x = (xx + .5 - WIDTH / 2.) / scale - cx
        y = (HEIGHT / 2. - yy - .5) / scale - cy
        masks.append((product.name, (np.abs(x) < .37) & (np.abs(y) < 1.17), dominant, opposite))
    scale = projection / (DISTANCE - TORUS_TUBE)
    holes = []
    for index, product in enumerate(PRODUCTS[2:], 2):
        x = (xx + .5 - WIDTH / 2.) / scale - product.center[0]
        y = (HEIGHT / 2. - yy - .5) / scale - product.center[1]
        radius = np.hypot(x, y)
        crown = np.abs(radius - TORUS_MAJOR) < .20
        holes.append((product.name, radius < TORUS_MAJOR - TORUS_TUBE - .15))
        if index == 2:
            masks.append((product.name, crown, 1, 0))
            continue
        z = np.sqrt(np.maximum(0., TORUS_TUBE ** 2 - (radius - TORUS_MAJOR) ** 2))
        major = circle_parameter_fraction(np.arctan2(y, x))
        tube = circle_parameter_fraction(np.arctan2(z, radius - TORUS_MAJOR))
        # Unwrapped pcurves have slope 1/2 in normalized native coordinates.
        # The phase is invariant under whole chart turns along either axis.
        phase = np.mod(2. * tube - major, 1.)
        masks.append(("Mixed winding band", crown & (phase > .13) & (phase < .37), 0, 1))
        masks.append(("Complementary band", crown & (phase > .63) & (phase < .87), 1, 0))
        require(np.mean(visible[crown]) == 1., "Mixed winding faces leave a visible gap")
    results = {}
    for label, mask, dominant, opposite in masks:
        colors = pixels[mask]
        coverage = float(np.mean(visible[mask]))
        styled = float(np.mean(colors[:, dominant] > colors[:, opposite] + 14))
        require(len(colors) >= 500 and coverage == 1. and styled > .98,
                f"Missing seam coverage or incorrect style on {label}: {coverage}, {styled}")
        results[label] = dict(pixels=len(colors), visibleFraction=coverage, styledFraction=styled)
    for label, hole in holes:
        coverage = float(np.mean(visible[hole]))
        require(coverage == 0., f"{label} bore filled: {coverage}")
        results[f"{label} clear bore"] = dict(pixels=int(np.count_nonzero(hole)), visibleFraction=coverage)
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
    model, sequence = output / "periodic-rational-splines.ifc", output / "depth-witnesses.json"
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
            print(f"PASS {technique}, TAA {taa}: periodic spline seams and {len(frames)} depth samples", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
