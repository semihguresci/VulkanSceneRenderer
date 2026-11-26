"""Capture IFC spherical shells with shared edge loops and trimmed caps."""

import argparse
from itertools import pairwise
import math
import os
from pathlib import Path
import tempfile

from ifc_area_profiles_runtime_regression import (
    DEPTH, DISTANCE, FOV, HEIGHT, TECHNIQUES, WIDTH, Fixture, Profile,
    StepFixture, record, require, save_json, write_sequence,
)
from ifc_swept_surface_runtime_regression import world_ray


SPHERE_RADIUS = 1.68
CAP_RADIUS = 1.57
CAP_NORMAL = (.8, 0., .6)
CAP_OFFSET = .6 * CAP_RADIUS
HOLED_RADIUS = 2.
HOLE_HEIGHT = 1.6
HOLE_RADIUS = math.sqrt(HOLED_RADIUS ** 2 - HOLE_HEIGHT ** 2)
SINGLE_RADIUS = 1.45
SINGLE_HEIGHT = .86
SINGLE_RIM_RADIUS = math.sqrt(SINGLE_RADIUS ** 2 - SINGLE_HEIGHT ** 2)
RADII = (SPHERE_RADIUS, CAP_RADIUS, HOLED_RADIUS, SINGLE_RADIUS)
PRODUCTS = (
    Profile("Shared meridian sphere", (-2.55, -1.40), (.11, .64, .92)),
    Profile("Tilted cap", (1.90, -1.40), (.18, .78, .32)),
    Profile("Sphere with opposite-cut hole", (0., 1.85), (.16, .48, .93)),
    Profile("Sphere with one ordinary bound", (4.3, 1.55), (.84, .18, .72)),
)
EAST_COLOR = (.91, .31, .09)
CAP_BASE_COLOR = (.66, .20, .78)
HOLE_DISK_COLOR = (.95, .36, .10)
SINGLE_DISK_COLOR = (.12, .78, .34)
ELLIPSOID_RADIAL, ELLIPSOID_AXIAL = 1.75, 2.4
ELLIPSOID = Profile("Revolved ellipsoid", (0., 0.), (.12, .59, .92))
ELLIPSOID_EAST_COLOR = (.95, .34, .09)


def surface_probe(index, name, x, y, *, z=None):
    # Project an analytic surface point onto the depth plane used by write_sequence.
    cx, cy = PRODUCTS[index].center
    if z is None:
        z = math.sqrt(RADII[index] ** 2 - x * x - y * y)
    scale = (DISTANCE - DEPTH) / (DISTANCE - z)
    return (index, name, ((cx + x) * scale - cx, (cy + y) * scale - cy), True)


FIXTURE = Fixture(PRODUCTS, (
    surface_probe(0, "west hemisphere", -.75, .52),
    surface_probe(0, "east hemisphere", .80, -.52),
    surface_probe(0, "west of north pole", -.075, 0.),
    surface_probe(0, "east of north pole", .075, 0.),
    surface_probe(0, "shared north pole", 0., 0.),
    surface_probe(0, "shared meridian above pole", 0., .9),
    surface_probe(0, "shared meridian below pole", 0., -.9),
    (0, "clear outside sphere", (-1.97, .45), False),
    surface_probe(1, "near tilted pole", .12, .10),
    surface_probe(1, "curved cap crown", .76, -.31),
    surface_probe(1, "near tilted rim", .27, .88),
    (1, "clipped shoulder of full sphere", (-.72, .15), False),
    (1, "clear outside cap", (1.95, -.18), False),
    surface_probe(2, "disk on the opposite meridian", 0., 0., z=HOLE_HEIGHT),
    surface_probe(2, "disk west of the opposite meridian", -.72, .10, z=HOLE_HEIGHT),
    surface_probe(2, "disk east of the opposite meridian", .67, -.31, z=HOLE_HEIGHT),
    surface_probe(2, "spherical cut above the hole", 0., 1.37),
    surface_probe(2, "spherical cut below the hole", 0., -1.37),
    surface_probe(2, "spherical trim beside the hole", 1.43, .24),
    (2, "clear outside holed sphere", (2.23, .12), False),
    surface_probe(3, "center of the shared disk", 0., 0., z=SINGLE_HEIGHT),
    surface_probe(3, "disk inside the shared rim", .64, .60, z=SINGLE_HEIGHT),
    surface_probe(3, "spherical face left of the disk", -1.28, -.15),
    surface_probe(3, "spherical face below the disk", .78, -.99),
    surface_probe(3, "spherical face on the unlit side", -1.05, -.90),
    (3, "clear outside singly bounded sphere", (1.85, -.12), False),
), None, None)


def ellipsoid_probe(name, x, y):
    z = ELLIPSOID_RADIAL * math.sqrt(1. - (x / ELLIPSOID_RADIAL) ** 2 - (y / ELLIPSOID_AXIAL) ** 2)
    scale = (DISTANCE - DEPTH) / (DISTANCE - z)
    return 0, name, (x * scale, y * scale), True


ELLIPSOID_FIXTURE = Fixture((ELLIPSOID,), (
    ellipsoid_probe("west face", -.8, .4),
    ellipsoid_probe("east face", .8, -.4),
    ellipsoid_probe("shared meridian", 0., 0.),
    ellipsoid_probe("shared meridian near the north pole", 0., 2.15),
    ellipsoid_probe("shared meridian near the south pole", 0., -2.15),
    ellipsoid_probe("west of the north meridian", -.10, 2.02),
    ellipsoid_probe("east of the south meridian", .10, -2.02),
    (0, "clear beyond the equatorial silhouette", (2.05, 0.), False),
    (0, "clear beyond the north silhouette", (0., 2.65), False),
), None, None)


def placement(step, origin, axis, reference):
    return step.add("IFCAXIS2PLACEMENT3D", step.point(*origin),
                    step.direction(*axis), step.direction(*reference))


def semicircle_edges(step, normal, offset, radius):
    # The hemisphere arcs cross opposite poles, while the cap rim crosses
    # the north pole. Offset the edge endpoints so the poles lie inside the
    # trims and off the regular dyadic sampling grid.
    origin = tuple(offset * component for component in normal)
    ring_radius = math.sqrt(radius * radius - offset * offset)
    frame = placement(step, origin, normal, (0., 1., 0.))
    circle = step.add("IFCCIRCLE", frame, ring_radius)
    t = .185
    radial = (-normal[2] * math.sin(t), math.cos(t), normal[0] * math.sin(t))
    vertices = [step.add("IFCVERTEXPOINT", step.point(*(
        a + sign * ring_radius * b for a, b in zip(origin, radial, strict=True)
    ))) for sign in (1., -1.)]
    first, last = vertices
    return (step.add("IFCEDGECURVE", first, last, circle, ".T."),
            step.add("IFCEDGECURVE", last, first, circle, ".T."))


def advanced_face(step, surface, loops, name, color, *, explicit_outer=True):
    bounds = []
    for index, uses in enumerate(loops):
        oriented = [step.add("IFCORIENTEDEDGE", "*", "*", edge, ".T." if forward else ".F.")
                    for edge, forward in uses]
        loop = step.add("IFCEDGELOOP", f"({','.join(oriented)})")
        bounds.append(step.add("IFCFACEOUTERBOUND" if explicit_outer and index == 0 else "IFCFACEBOUND", loop, ".T."))
    face = step.add("IFCADVANCEDFACE", f"({','.join(bounds)})", surface, ".T.")
    rgb = step.add("IFCCOLOURRGB", "$", *color)
    appearance = step.add("IFCSURFACESTYLERENDERING", rgb, "0.", "$", "$", "$", "$", "$", "$", ".NOTDEFINED.")
    style = step.add("IFCSURFACESTYLE", f"'{name}'", ".BOTH.", f"({appearance})")
    step.add("IFCSTYLEDITEM", face, f"({style})", "$")
    return face


def opposite_hole(step, context):
    center = (0., 0., 0.)
    # The placed +X seam is behind the sphere. Its opposite meridian lies
    # on the camera-facing half and crosses the center of the circular hole.
    pole, reference = (0., 1., 0.), (0., 0., -1.)
    frame = placement(step, center, pole, reference)
    surface = step.add("IFCSPHERICALSURFACE", frame, HOLED_RADIUS)
    meridian = step.add("IFCCIRCLE", placement(step, center, (1., 0., 0.), reference), HOLED_RADIUS)
    latitudes = (-math.pi / 2., -.41, .23, math.pi / 2.)
    points = [(0., -HOLED_RADIUS, 0.)]
    points.extend((0., HOLED_RADIUS * math.sin(lat), -HOLED_RADIUS * math.cos(lat)) for lat in latitudes[1:-1])
    points.append((0., HOLED_RADIUS, 0.))
    vertices = [step.add("IFCVERTEXPOINT", step.point(*point)) for point in points]
    seam = []
    for (low, high), (first, last) in zip(pairwise(latitudes), pairwise(vertices), strict=True):
        trim = step.add("IFCTRIMMEDCURVE", meridian, f"(IFCPARAMETERVALUE({low}))",
                        f"(IFCPARAMETERVALUE({high}))", ".T.", ".PARAMETER.")
        pcurves = []
        for angle in (0., math.tau):
            a, b = step.point(angle, low), step.point(angle, high)
            line = step.add("IFCPOLYLINE", f"({a},{b})")
            definition = step.add("IFCDEFINITIONALREPRESENTATION", context, "$", "'Curve2D'", f"({line})")
            pcurves.append(step.add("IFCPCURVE", surface, definition))
        curve = step.add("IFCSEAMCURVE", trim, f"({','.join(pcurves)})", ".CURVE3D.")
        seam.append(step.add("IFCEDGECURVE", first, last, curve, ".T."))
    forward = tuple((edge, True) for edge in seam)
    backward = tuple((edge, False) for edge in reversed(seam))
    # Start at the first interior join so the ascending run wraps the edge loop.
    seam_loop = forward[1:] + backward + forward[:1]

    disk_frame = placement(step, (0., 0., HOLE_HEIGHT), (0., 0., 1.), (1., 0., 0.))
    circle = step.add("IFCCIRCLE", disk_frame, HOLE_RADIUS)
    start = step.add("IFCVERTEXPOINT", step.point(HOLE_RADIUS, 0., HOLE_HEIGHT))
    rim = step.add("IFCEDGECURVE", start, start, circle, ".T.")
    wall = advanced_face(step, surface, (seam_loop, ((rim, False),)),
                         "Spherical wall around the hole", PRODUCTS[2].color)
    disk = advanced_face(step, step.add("IFCPLANE", disk_frame), (((rim, True),),),
                         "Disk in the spherical hole", HOLE_DISK_COLOR)
    return wall, disk


def single_bound_sphere(step):
    # Both poles are inside the spherical face and neither lies on the rim.
    frame = placement(step, (0., 0., 0.), (0., 1., 0.), (1., 0., 0.))
    sphere = step.add("IFCSPHERICALSURFACE", frame, SINGLE_RADIUS)
    disk_frame = placement(step, (0., 0., SINGLE_HEIGHT), (0., 0., 1.), (1., 0., 0.))
    circle = step.add("IFCCIRCLE", disk_frame, SINGLE_RIM_RADIUS)
    start = step.add("IFCVERTEXPOINT", step.point(SINGLE_RIM_RADIUS, 0., SINGLE_HEIGHT))
    rim = step.add("IFCEDGECURVE", start, start, circle, ".T.")
    wall = advanced_face(step, sphere, (((rim, False),),), "Spherical face with ordinary bound",
                         PRODUCTS[3].color, explicit_outer=False)
    disk = advanced_face(step, step.add("IFCPLANE", disk_frame), (((rim, True),),),
                         "Disk at the ordinary bound", SINGLE_DISK_COLOR)
    return wall, disk


def write_fixture(path):
    step = StepFixture()
    step.add("IFCSIUNIT", "*", ".LENGTHUNIT.", "$", ".METRE.")
    step.add("IFCSIUNIT", "*", ".PLANEANGLEUNIT.", "$", ".RADIAN.")
    origin = placement(step, (0., 0., 0.), (0., 0., 1.), (1., 0., 0.))
    context = step.add("IFCGEOMETRICREPRESENTATIONCONTEXT", "$", "'Model'", 3, 1e-6, origin, "$")
    sphere = step.add("IFCSPHERICALSURFACE", origin, SPHERE_RADIUS)
    meridian = semicircle_edges(step, (1., 0., 0.), 0., SPHERE_RADIUS)
    west = advanced_face(step, sphere, (tuple((edge, False) for edge in reversed(meridian)),),
                         "West hemisphere", PRODUCTS[0].color)
    east = advanced_face(step, sphere, (tuple((edge, True) for edge in meridian),), "East hemisphere", EAST_COLOR)

    cap_surface = step.add("IFCSPHERICALSURFACE", origin, CAP_RADIUS)
    cap_rim = semicircle_edges(step, CAP_NORMAL, CAP_OFFSET, CAP_RADIUS)
    cap = advanced_face(step, cap_surface, (tuple((edge, True) for edge in cap_rim),),
                        "Tilted spherical face", PRODUCTS[1].color)
    base_frame = placement(step, tuple(CAP_OFFSET * value for value in CAP_NORMAL),
                           tuple(-value for value in CAP_NORMAL), (0., 1., 0.))
    base_surface = step.add("IFCPLANE", base_frame)
    disk = advanced_face(step, base_surface, (tuple((edge, False) for edge in reversed(cap_rim)),),
                         "Tilted disk", CAP_BASE_COLOR)
    shells = ((west, east), (cap, disk), opposite_hole(step, context), single_bound_sphere(step))
    write_shells(path, step, context, PRODUCTS, shells)


def write_ellipsoid_fixture(path):
    step = StepFixture()
    step.add("IFCSIUNIT", "*", ".LENGTHUNIT.", "$", ".METRE.")
    step.add("IFCSIUNIT", "*", ".PLANEANGLEUNIT.", "$", ".RADIAN.")
    center, axis, radial = (0., 0., 0.), (0., 1., 0.), (0., 0., 1.)
    origin = placement(step, center, radial, (1., 0., 0.))
    context = step.add("IFCGEOMETRICREPRESENTATIONCONTEXT", "$", "'Model'", 3, 1e-6, origin, "$")
    center2d = step.add("IFCCARTESIANPOINT", "(0.,0.)")
    axial2d = step.add("IFCDIRECTION", "(0.,1.)")
    profile_frame = step.add("IFCAXIS2PLACEMENT2D", center2d, axial2d)
    ellipse = step.add("IFCELLIPSE", profile_frame, ELLIPSOID_AXIAL, ELLIPSOID_RADIAL)
    trim = step.add("IFCTRIMMEDCURVE", ellipse, f"(IFCPARAMETERVALUE({math.pi}))",
                    f"(IFCPARAMETERVALUE({math.tau}))", ".T.", ".PARAMETER.")
    profile = step.add("IFCARBITRARYOPENPROFILEDEF", ".CURVE.", "$", trim)
    axis_position = step.add("IFCAXIS1PLACEMENT", step.point(*center), step.direction(*axis))
    meridian_normal = (-1., 0., 0.)
    swept_frame = placement(step, center, meridian_normal, radial)
    surface = step.add("IFCSURFACEOFREVOLUTION", profile, swept_frame, axis_position)
    edge_frame = placement(step, center, meridian_normal, axis)
    edge_curve = step.add("IFCELLIPSE", edge_frame, ELLIPSOID_AXIAL, ELLIPSOID_RADIAL)
    angle = .137
    vertex = step.add("IFCVERTEXPOINT", step.point(0., ELLIPSOID_AXIAL * math.sin(angle),
                                                  ELLIPSOID_RADIAL * math.cos(angle)))
    edge = step.add("IFCEDGECURVE", vertex, vertex, edge_curve, ".T.")
    west = advanced_face(step, surface, (((edge, True),),), "West ellipsoid face", ELLIPSOID.color)
    east = advanced_face(step, surface, (((edge, False),),), "East ellipsoid face", ELLIPSOID_EAST_COLOR)
    write_shells(path, step, context, (ELLIPSOID,), ((west, east),))


def write_shells(path, step, context, products, shells):
    for index, (product, faces) in enumerate(zip(products, shells, strict=True)):
        shell = step.add("IFCCLOSEDSHELL", f"({','.join(faces)})")
        brep = step.add("IFCADVANCEDBREP", shell)
        rep = step.add("IFCSHAPEREPRESENTATION", context, "'Body'", "'AdvancedBrep'", f"({brep})")
        shape = step.add("IFCPRODUCTDEFINITIONSHAPE", "$", "$", f"({rep})")
        x, y = product.center
        position = placement(step, (x, 0., y), (0., -1., 0.), (1., 0., 0.))
        local = step.add("IFCLOCALPLACEMENT", "$", position)
        step.add("IFCBUILDINGELEMENTPROXY", f"'{index + 1:022d}'", "$", f"'{product.name}'",
                 "$", "$", local, shape, "$", "$")
    header = "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('ViewDefinition [DesignTransferView]'),'2;1');\n"
    header += (f"FILE_NAME('{path.name}','2026-11-14T00:00:00',(''),(''),'Container','','');\n"
               "FILE_SCHEMA(('IFC4'));\nENDSEC;\nDATA;\n")
    path.write_text(header + "\n".join(step.elements) + "\nENDSEC;\nEND-ISO-10303-21;\n", encoding="ascii")


def sphere_hits(ray, index):
    import numpy as np

    x, y = PRODUCTS[index].center
    relative_camera = np.array([-x, -y, DISTANCE])
    a = np.sum(ray * ray, axis=-1)
    b = ray @ relative_camera
    c = relative_camera @ relative_camera - RADII[index] ** 2
    discriminant = b * b - a * c
    t = (-b - np.sqrt(np.maximum(0., discriminant))) / a
    return discriminant, relative_camera + t[..., None] * ray


def disk_hits(ray, index, height):
    import numpy as np

    x, y = PRODUCTS[index].center
    camera = np.array([-x, -y, DISTANCE])
    t = (height - DISTANCE) / ray[..., 2]
    return camera + t[..., None] * ray


def expected_depth(index, point, pixel, matrix):
    import numpy as np

    ray = world_ray((pixel[0], HEIGHT - 1 - pixel[1]), np.linalg.inv(matrix))
    discriminant, hit = sphere_hits(ray, index)
    require(discriminant > 0 and (index != 1 or hit @ CAP_NORMAL > CAP_OFFSET),
            f"Positive depth probe outside the analytic spherical trim: {point}")
    if index in (2, 3):
        height, radius = (HOLE_HEIGHT, HOLE_RADIUS) if index == 2 else (SINGLE_HEIGHT, SINGLE_RIM_RADIUS)
        if hit[2] > height:
            hit = disk_hits(ray, index, height)
            require(np.linalg.norm(hit[:2]) < radius, f"Depth probe outside the circular disk: {point}")
    x, y = PRODUCTS[index].center
    clip = matrix @ [*(hit + [x, y, 0.]), 1.]
    return clip[2] / clip[3]


def check_pixels(image, _fixture):
    import numpy as np
    from PIL import Image

    with Image.open(image) as source:
        pixels = np.asarray(source.convert("RGB"), dtype=np.int16)
    require(pixels.shape == (HEIGHT, WIDTH, 3), "Wrong spherical trim image extent")
    sky = int(np.max(pixels[:12, :12]))
    visible = pixels.max(axis=2) > sky + 16
    scale = 2. * math.tan(math.radians(FOV / 2.)) / HEIGHT
    yy, xx = np.mgrid[:HEIGHT, :WIDTH]
    ray = np.stack(((xx + .5 - WIDTH / 2.) * scale,
                    (HEIGHT / 2. - yy - .5) * scale, np.full_like(xx, -1.)), axis=-1)
    sphere_disc, sphere = sphere_hits(ray, 0)
    cap_disc, cap = sphere_hits(ray, 1)
    holed_disc, holed = sphere_hits(ray, 2)
    single_disc, single = sphere_hits(ray, 3)
    disk = disk_hits(ray, 2, HOLE_HEIGHT)
    single_disk_distance = np.linalg.norm(disk_hits(ray, 3, SINGLE_HEIGHT)[..., :2], axis=-1)
    sphere_interior = (sphere_disc > 0.) & (sphere[..., 2] > .60 * SPHERE_RADIUS)
    cap_interior = (cap_disc > 0.) & (cap[..., 2] > .55 * CAP_RADIUS)
    cap_margin = cap @ CAP_NORMAL - CAP_OFFSET
    holed_wall = (holed_disc > 0.) & (holed[..., 2] > .53 * HOLED_RADIUS) & (holed[..., 2] < HOLE_HEIGHT - .08)
    hole_disk = np.linalg.norm(disk[..., :2], axis=-1) < HOLE_RADIUS - .16
    # The capture uses only a directional light. Depth probes also cover the
    # unlit wall, where an opaque surface can have the same color as the sky.
    light = np.array((.3, .35, 1.))
    light /= np.linalg.norm(light)
    single_wall = ((single_disc > 0.) & (single[..., 2] > .35) &
                   (single[..., 2] < SINGLE_HEIGHT - .08) & (single @ light > .20 * SINGLE_RADIUS))
    single_disk = (single_disc > 0.) & (single[..., 2] > SINGLE_HEIGHT) & (single_disk_distance < SINGLE_RIM_RADIUS - .12)
    masks = (
        ("West hemisphere", sphere_interior & (sphere[..., 0] < -.17), 2, 0),
        ("East hemisphere", sphere_interior & (sphere[..., 0] > .17), 0, 2),
        ("Tilted spherical face", cap_interior & (cap_margin > .15), 1, 0),
        ("Spherical wall around the hole", holed_wall, 2, 0),
        ("Disk in the spherical hole", hole_disk, 0, 2),
        ("Spherical face with ordinary bound", single_wall, 0, 1),
        ("Disk at the ordinary bound", single_disk, 1, 0),
    )
    results = {}
    for label, mask, dominant, opposite in masks:
        colors = pixels[mask]
        require(len(colors) >= 450, f"Insufficient analytic interior for {label}")
        colored = float(np.mean(colors[:, dominant] > colors[:, opposite] + 18))
        coverage = float(np.mean(visible[mask]))
        require(coverage >= .98 and colored >= .98, f"Missing or incorrectly styled {label}: {coverage}, {colored}")
        results[label] = dict(pixels=len(colors), visibleFraction=coverage, styledFraction=colored)
    meridian = sphere_interior & (np.abs(sphere[..., 0]) < .10)
    opposite = holed_wall & (np.abs(holed[..., 0]) < .12)
    clipped = cap_interior & (cap_margin < -.16)
    single_rim = ((single_disc > 0.) & (np.abs(single_disk_distance - SINGLE_RIM_RADIUS) < .045) &
                  ((single[..., 2] <= SINGLE_HEIGHT) | (single_disk_distance < SINGLE_RIM_RADIUS)))
    require(np.count_nonzero(meridian) >= 100 and np.count_nonzero(opposite) >= 100 and np.count_nonzero(clipped) >= 450,
            "Insufficient trim witness pixels")
    require(np.count_nonzero(single_rim) >= 100, "Insufficient pixels across the ordinary bound")
    seam_coverage = float(np.mean(visible[meridian]))
    opposite_coverage = float(np.mean(visible[opposite]))
    clipped_coverage = float(np.mean(visible[clipped]))
    single_rim_coverage = float(np.mean(visible[single_rim]))
    require(seam_coverage == 1., f"Open shared pole seam: {seam_coverage}")
    require(opposite_coverage == 1., f"Open opposite meridian beside the spherical hole: {opposite_coverage}")
    require(clipped_coverage < .01, f"Visible geometry past the tilted cap edge: {clipped_coverage}")
    require(single_rim_coverage == 1., f"Open spherical rim at the ordinary bound: {single_rim_coverage}")
    results["Shared meridian"] = dict(pixels=int(np.count_nonzero(meridian)), visibleFraction=seam_coverage)
    results["Opposite meridian around hole"] = dict(pixels=int(np.count_nonzero(opposite)), visibleFraction=opposite_coverage)
    results["Clipped spherical shoulder"] = dict(pixels=int(np.count_nonzero(clipped)), visibleFraction=clipped_coverage)
    results["Shared rim at the ordinary bound"] = dict(pixels=int(np.count_nonzero(single_rim)),
                                                      visibleFraction=single_rim_coverage)
    return results


def ellipsoid_hits(ray):
    import numpy as np

    inverse_radii = np.array((1. / ELLIPSOID_RADIAL, 1. / ELLIPSOID_AXIAL, 1. / ELLIPSOID_RADIAL))
    camera = np.array((0., 0., DISTANCE))
    scaled_ray, scaled_camera = ray * inverse_radii, camera * inverse_radii
    a = np.sum(scaled_ray * scaled_ray, axis=-1)
    b = scaled_ray @ scaled_camera
    c = scaled_camera @ scaled_camera - 1.
    discriminant = b * b - a * c
    distance = (-b - np.sqrt(np.maximum(0., discriminant))) / a
    return discriminant, camera + distance[..., None] * ray


def ellipsoid_depth(_index, point, pixel, matrix):
    import numpy as np

    ray = world_ray((pixel[0], HEIGHT - 1 - pixel[1]), np.linalg.inv(matrix))
    discriminant, hit = ellipsoid_hits(ray)
    require(discriminant > 0., f"Positive depth probe outside the analytic ellipsoid: {point}")
    clip = matrix @ [*hit, 1.]
    return clip[2] / clip[3]


def check_ellipsoid_pixels(image, _fixture):
    import numpy as np
    from PIL import Image

    with Image.open(image) as source:
        pixels = np.asarray(source.convert("RGB"), dtype=np.int16)
    require(pixels.shape == (HEIGHT, WIDTH, 3), "Wrong revolved ellipsoid image extent")
    visible = pixels.max(axis=2) > int(np.max(pixels[:12, :12])) + 16
    scale = 2. * math.tan(math.radians(FOV / 2.)) / HEIGHT
    yy, xx = np.mgrid[:HEIGHT, :WIDTH]
    ray = np.stack(((xx + .5 - WIDTH / 2.) * scale,
                    (HEIGHT / 2. - yy - .5) * scale, np.full_like(xx, -1.)), axis=-1)
    discriminant, hit = ellipsoid_hits(ray)
    interior = (discriminant > 0.) & (hit[..., 2] > .92)
    normal = hit / np.square((ELLIPSOID_RADIAL, ELLIPSOID_AXIAL, ELLIPSOID_RADIAL))
    light = np.array((.3, .35, 1.))
    lit = (normal @ light) > .20 * np.linalg.norm(normal, axis=-1) * np.linalg.norm(light)
    results = {}
    masks = (
        ("West ellipsoid face", interior & lit & (hit[..., 0] < -.14), 2, 0),
        ("East ellipsoid face", interior & lit & (hit[..., 0] > .14), 0, 2),
    )
    for label, mask, dominant, opposite in masks:
        colors = pixels[mask]
        require(len(colors) >= 450, f"Insufficient analytic interior for {label}")
        styled = float(np.mean(colors[:, dominant] > colors[:, opposite] + 18))
        coverage = float(np.mean(visible[mask]))
        require(coverage >= .98 and styled >= .98, f"Missing or incorrectly styled {label}: {coverage}, {styled}")
        results[label] = dict(pixels=len(colors), visibleFraction=coverage, styledFraction=styled)
    for label, sign in (("North polar meridian", 1.), ("South polar meridian", -1.)):
        mask = interior & (sign * hit[..., 1] > 1.6) & (np.abs(hit[..., 0]) < .1)
        count = int(np.count_nonzero(mask))
        require(count >= 50, f"Insufficient pixels along the {label}")
        coverage = float(np.mean(visible[mask]))
        require(coverage == 1., f"Open shared {label}: {coverage}")
        results[label] = dict(pixels=count, visibleFraction=coverage)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--managed-output", action="store_true")
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--fixture", choices=("spherical-edge-loops", "revolved-ellipsoid"),
                        default="spherical-edge-loops")
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
    model, sequence = output / f"{args.fixture}.ifc", output / "depth-witnesses.json"
    if args.fixture == "revolved-ellipsoid":
        writer, fixture = write_ellipsoid_fixture, ELLIPSOID_FIXTURE
        depth, image = ellipsoid_depth, check_ellipsoid_pixels
    else:
        writer, fixture = write_fixture, FIXTURE
        depth, image = expected_depth, check_pixels
    writer(model)
    frames = write_sequence(sequence, fixture)
    if args.generate_only:
        return 0
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    results = []
    for technique in TECHNIQUES:
        for taa in (False, True):
            results.append(record(renderer, output, model, sequence, frames, technique, taa, fixture,
                                  expected_depth=depth, check_image=image))
            save_json(output / "results.json", results)
            print(f"PASS {technique}, TAA {taa}: {args.fixture} with {len(frames)} depth samples", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
