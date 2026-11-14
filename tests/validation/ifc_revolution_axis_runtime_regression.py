"""Capture smooth IFC paraboloid axis endpoints and explicit seam topology."""

import argparse
import math
import os
from pathlib import Path
import tempfile

from ifc_area_profiles_runtime_regression import (
    DEPTH, DISTANCE, FOV, HEIGHT, TECHNIQUES, WIDTH, Fixture, Profile,
    StepFixture, record, require, save_json, write_sequence,
)
from ifc_spherical_edge_loops_runtime_regression import placement
from ifc_swept_surface_runtime_regression import world_ray


RADIUS, APEX, CURVATURE = 1.63, .96, .39
PROFILE_START, PROFILE_END = 2., 5.
INNER_R, OUTER_R, FIRST_ANGLE, LAST_ANGLE = .62, 1.29, .48, 1.40
PRODUCTS = (
    Profile("Shared axial meridians", (-2.10, -.22), (.11, .67, .93)),
    Profile("Full-turn axial seam", (2.10, -.22), (.15, .79, .31)),
)
SOUTH_COLOR = (.92, .34, .10)
INFILL_COLOR = (.76, .19, .86)
BASE_COLOR = (.72, .68, .19)


def height(r):
    return APEX - CURVATURE * r ** 2


def cap_point(r, angle):
    return (r * math.cos(angle), r * math.sin(angle), height(r))


def probe(index, name, r, angle):
    cx, cy = PRODUCTS[index].center
    x, y, z = cap_point(r, angle)
    scale = (DISTANCE - DEPTH) / (DISTANCE - z)
    return index, name, ((cx + x) * scale - cx, (cy + y) * scale - cy), True


FIXTURE = Fixture(PRODUCTS, (
    probe(0, "shared axis vertex", 0., 0.),
    probe(0, "north of shared axis", .11, .5 * math.pi),
    probe(0, "south of shared axis", .11, 1.5 * math.pi),
    probe(0, "positive radial seam", 1.14, 0.),
    probe(0, "negative radial seam", .94, math.pi),
    probe(0, "north outer cap", 1.46, 2.3),
    probe(0, "south outer cap", 1.35, 5.2),
    (0, "clear outside split cap", (-1.91, .55), False),
    probe(1, "full-turn axis vertex", 0., 0.),
    probe(1, "full-turn seam", 1.12, 0.),
    probe(1, "just above full-turn seam", 1.05, .10),
    probe(1, "just below full-turn seam", 1.05, -.10),
    probe(1, "far from full-turn seam", 1.39, 2.45),
    probe(1, "off-axis insert", .95, .93),
    probe(1, "between insert and rim", 1.45, .95),
    (1, "clear beyond closed rim", (1.91, .56), False),
), None, None)


def spline(step, controls):
    points = [step.point(*point) for point in controls]
    return step.add("IFCBSPLINECURVEWITHKNOTS", 2, f"({','.join(points)})",
                    ".UNSPECIFIED.", ".F.", ".F.", "(3,3)", f"({PROFILE_START},{PROFILE_END})", ".UNSPECIFIED.")


def edge(step, first, last, curve):
    return step.add("IFCEDGECURVE", first, last, curve, ".T.")


def advanced_face(step, surface, loops, name, color, same_sense=True):
    bounds = []
    for index, uses in enumerate(loops):
        oriented = [step.add("IFCORIENTEDEDGE", "*", "*", ref, ".T." if forward else ".F.")
                    for ref, forward in uses]
        loop = step.add("IFCEDGELOOP", f"({','.join(oriented)})")
        bounds.append(step.add("IFCFACEOUTERBOUND" if index == 0 else "IFCFACEBOUND", loop, ".T."))
    face = step.add("IFCADVANCEDFACE", f"({','.join(bounds)})", surface, ".T." if same_sense else ".F.")
    rgb = step.add("IFCCOLOURRGB", "$", *color)
    rendering = step.add("IFCSURFACESTYLERENDERING", rgb, "0.", "$", "$", "$", "$", "$", "$", ".NOTDEFINED.")
    style = step.add("IFCSURFACESTYLE", f"'{name}'", ".BOTH.", f"({rendering})")
    step.add("IFCSTYLEDITEM", face, f"({style})", "$")
    return face


def meridian(step, angle):
    c, s = math.cos(angle), math.sin(angle)
    return spline(step, ((0., 0., APEX), (.5 * RADIUS * c, .5 * RADIUS * s, APEX),
                         cap_point(RADIUS, angle)))


def circle(step, radius):
    frame = placement(step, (0., 0., height(radius)), (0., 0., 1.), (1., 0., 0.))
    return step.add("IFCCIRCLE", frame, radius)


def vertex(step, radius, angle):
    return step.add("IFCVERTEXPOINT", step.point(*cap_point(radius, angle)))


def write_fixture(path):
    step = StepFixture()
    step.add("IFCSIUNIT", "*", ".LENGTHUNIT.", "$", ".METRE.")
    step.add("IFCSIUNIT", "*", ".PLANEANGLEUNIT.", "$", ".RADIAN.")
    origin = (0., 0., 0.)
    x_axis, z_axis = (1., 0., 0.), (0., 0., 1.)
    world = placement(step, origin, z_axis, x_axis)
    context = step.add("IFCGEOMETRICREPRESENTATIONCONTEXT", "$", "'Model'", 3, 1e-6, world, "$")
    generatrix = spline(step, ((0., APEX), (RADIUS / 2., APEX), (RADIUS, height(RADIUS))))
    profile = step.add("IFCARBITRARYOPENPROFILEDEF", ".CURVE.", "$", generatrix)
    axis = step.add("IFCAXIS1PLACEMENT", step.point(*origin), step.direction(0., 1., 0.))
    sweep_frame = placement(step, origin, (0., -1., 0.), x_axis)
    surface = step.add("IFCSURFACEOFREVOLUTION", profile, sweep_frame, axis)
    base_frame = placement(step, (0., 0., height(RADIUS)), (0., 0., -1.), x_axis)
    plane = step.add("IFCPLANE", base_frame)

    apex, east, west = vertex(step, 0., 0.), vertex(step, RADIUS, 0.), vertex(step, RADIUS, math.pi)
    rim = circle(step, RADIUS)
    east_meridian = meridian(step, 0.)
    east_edge = edge(step, apex, east, east_meridian)
    west_edge = edge(step, apex, west, meridian(step, math.pi))
    north_rim, south_rim = edge(step, east, west, rim), edge(step, west, east, rim)
    north = advanced_face(step, surface, (((east_edge, True), (north_rim, True), (west_edge, False)),),
                          "North paraboloid", PRODUCTS[0].color, same_sense=False)
    south = advanced_face(step, surface, (((west_edge, True), (south_rim, True), (east_edge, False)),),
                          "South paraboloid", SOUTH_COLOR, same_sense=False)
    split_base = advanced_face(step, plane, (((south_rim, False), (north_rim, False)),),
                               "Split base", BASE_COLOR)

    pcurves = []
    for angle in 0., math.tau:
        ends = (step.point(angle, PROFILE_START), step.point(angle, PROFILE_END))
        line = step.add("IFCPOLYLINE", f"({','.join(ends)})")
        pcurves.append(step.add("IFCPCURVE", surface, line))
    seam_curve = step.add("IFCSEAMCURVE", east_meridian, f"({','.join(pcurves)})", ".CURVE3D.")
    seam = edge(step, apex, east, seam_curve)
    full_rim = edge(step, east, east, rim)

    # A second styled face fills the off-axis hole. It shares the original
    # analytic paraboloid and all four edges, so it leaves no depth step.
    a, b, c, d = (vertex(step, r, u) for r, u in ((INNER_R, FIRST_ANGLE), (OUTER_R, FIRST_ANGLE),
                                                  (OUTER_R, LAST_ANGLE), (INNER_R, LAST_ANGLE)))
    start = edge(step, a, b, meridian(step, FIRST_ANGLE))
    outer = edge(step, b, c, circle(step, OUTER_R))
    end = edge(step, d, c, meridian(step, LAST_ANGLE))
    inner = edge(step, a, d, circle(step, INNER_R))
    insert = advanced_face(step, surface, (((start, True), (outer, True), (end, False), (inner, False)),),
                           "Off-axis insert", INFILL_COLOR, same_sense=False)
    full_cap = advanced_face(step, surface, (
        ((seam, True), (full_rim, True), (seam, False)),
        ((inner, True), (end, True), (outer, False), (start, False)),
    ), "Full-turn paraboloid", PRODUCTS[1].color, same_sense=False)
    full_base = advanced_face(step, plane, (((full_rim, False),),), "Full-turn base", BASE_COLOR)

    shells = ((north, south, split_base), (full_cap, insert, full_base))
    for index, (product, faces) in enumerate(zip(PRODUCTS, shells, strict=True)):
        shell = step.add("IFCCLOSEDSHELL", f"({','.join(faces)})")
        brep = step.add("IFCADVANCEDBREP", shell)
        rep = step.add("IFCSHAPEREPRESENTATION", context, "'Body'", "'AdvancedBrep'", f"({brep})")
        shape = step.add("IFCPRODUCTDEFINITIONSHAPE", "$", "$", f"({rep})")
        x, y = product.center
        position = placement(step, (x, 0., y), (0., -1., 0.), x_axis)
        local = step.add("IFCLOCALPLACEMENT", "$", position)
        step.add("IFCBUILDINGELEMENTPROXY", f"'{index + 1:022d}'", "$", f"'{product.name}'",
                 "$", "$", local, shape, "$", "$")
    header = "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('ViewDefinition [DesignTransferView]'),'2;1');\n"
    header += (f"FILE_NAME('{path.name}','2026-11-14T00:00:00',(''),(''),'Container','','');\n"
               "FILE_SCHEMA(('IFC4'));\nENDSEC;\nDATA;\n")
    path.write_text(header + "\n".join(step.elements) + "\nENDSEC;\nEND-ISO-10303-21;\n", encoding="ascii")


def paraboloid_hit(ray, index):
    import numpy as np

    cx, cy = PRODUCTS[index].center
    camera = np.array([-cx, -cy, DISTANCE])
    a = CURVATURE * np.sum(ray[..., :2] ** 2, axis=-1)
    b = ray[..., 2] + 2. * CURVATURE * (ray[..., :2] @ camera[:2])
    c = DISTANCE - APEX + CURVATURE * (cx ** 2 + cy ** 2)
    discriminant = b * b - 4. * a * c
    denominator = -b + np.sqrt(np.maximum(0., discriminant))
    t = 2. * c / np.where(denominator > 0., denominator, 1.)
    hit = camera + t[..., None] * ray
    valid = (discriminant >= 0.) & (denominator > 0.) & (np.linalg.norm(hit[..., :2], axis=-1) <= RADIUS)
    return hit, valid


def expected_depth(index, point, pixel, matrix):
    import numpy as np

    ray = world_ray((pixel[0], HEIGHT - 1 - pixel[1]), np.linalg.inv(matrix))
    hit, valid = paraboloid_hit(ray, index)
    require(valid, f"Positive depth probe outside the analytic cap: {point}")
    x, y = PRODUCTS[index].center
    clip = matrix @ [*(hit + [x, y, 0.]), 1.]
    return clip[2] / clip[3]


def check_pixels(image, _fixture):
    import numpy as np
    from PIL import Image

    with Image.open(image) as source:
        pixels = np.asarray(source.convert("RGB"), dtype=np.int16)
    require(pixels.shape == (HEIGHT, WIDTH, 3), "Wrong revolution image extent")
    visible = pixels.max(axis=2) > int(pixels[:12, :12].max()) + 16
    scale = 2. * math.tan(math.radians(FOV / 2.)) / HEIGHT
    yy, xx = np.mgrid[:HEIGHT, :WIDTH]
    rays = np.stack(((xx + .5 - WIDTH / 2.) * scale,
                     (HEIGHT / 2. - yy - .5) * scale, np.full_like(xx, -1.)), axis=-1)
    split, split_valid = paraboloid_hit(rays, 0)
    full, full_valid = paraboloid_hit(rays, 1)
    radius = np.linalg.norm(full[..., :2], axis=-1)
    angle = np.arctan2(full[..., 1], full[..., 0])
    insert = ((radius > INNER_R + .09) & (radius < OUTER_R - .09) &
              (angle > FIRST_ANGLE + .11) & (angle < LAST_ANGLE - .11))
    insert_guard = ((radius > INNER_R - .09) & (radius < OUTER_R + .09) &
                    (angle > FIRST_ANGLE - .11) & (angle < LAST_ANGLE + .11))
    split_safe = split_valid & (split[..., 2] > height(RADIUS - .15))
    full_safe = full_valid & (radius < RADIUS - .15)
    styles = (
        ("North paraboloid", split_safe & (split[..., 1] > .12), 2, 0),
        ("South paraboloid", split_safe & (split[..., 1] < -.12), 0, 2),
        ("Full-turn paraboloid", full_safe & ~insert_guard, 1, 0),
        ("Off-axis insert", full_safe & insert, 2, 1),
    )
    results = {}
    for label, mask, dominant, opposite in styles:
        colors = pixels[mask]
        require(len(colors) >= 175, f"Insufficient analytic interior for {label}")
        styled = float(np.mean(colors[:, dominant] > colors[:, opposite] + 18))
        coverage = float(np.mean(visible[mask]))
        require(coverage >= .98 and styled >= .98, f"Missing or incorrectly styled {label}: {coverage}, {styled}")
        results[label] = dict(pixels=len(colors), visibleFraction=coverage, styledFraction=styled)
    seams = (
        ("Shared axial meridians", split_safe & (np.abs(split[..., 1]) < .08)),
        ("Full-turn axis and seam", full_safe & (full[..., 0] > -.10) & (np.abs(full[..., 1]) < .08)),
    )
    for label, mask in seams:
        coverage = float(np.mean(visible[mask]))
        require(np.count_nonzero(mask) >= 150 and coverage == 1., f"Open {label}: {coverage}")
        results[label] = dict(pixels=int(np.count_nonzero(mask)), visibleFraction=coverage)
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
    model, sequence = output / "smooth-revolution-axis.ifc", output / "depth-witnesses.json"
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
            print(f"PASS {technique}, TAA {taa}: smooth cap, axial seams, and {len(frames)} depth samples", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
