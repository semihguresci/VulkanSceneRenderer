"""Capture IFC spherical shells whose explicit edge curves pass through poles."""

import argparse
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
RADII = (SPHERE_RADIUS, CAP_RADIUS)
PRODUCTS = (
    Profile("Shared meridian sphere", (-2.55, -.15), (.11, .64, .92)),
    Profile("Tilted cap", (1.90, -.15), (.18, .78, .32)),
)
EAST_COLOR = (.91, .31, .09)
CAP_BASE_COLOR = (.66, .20, .78)


def surface_probe(index, name, x, y):
    # Project an analytic surface point onto the depth plane used by write_sequence.
    cx, cy = PRODUCTS[index].center
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


def advanced_face(step, surface, edges, name, color, reverse=False):
    sense = ".F." if reverse else ".T."
    ordered = reversed(edges) if reverse else edges
    oriented = [step.add("IFCORIENTEDEDGE", "*", "*", edge, sense) for edge in ordered]
    loop = step.add("IFCEDGELOOP", f"({','.join(oriented)})")
    bound = step.add("IFCFACEOUTERBOUND", loop, ".T.")
    face = step.add("IFCADVANCEDFACE", f"({bound})", surface, ".T.")
    rgb = step.add("IFCCOLOURRGB", "$", *color)
    appearance = step.add("IFCSURFACESTYLERENDERING", rgb, "0.", "$", "$", "$", "$", "$", "$", ".NOTDEFINED.")
    style = step.add("IFCSURFACESTYLE", f"'{name}'", ".BOTH.", f"({appearance})")
    step.add("IFCSTYLEDITEM", face, f"({style})", "$")
    return face


def write_fixture(path):
    step = StepFixture()
    step.add("IFCSIUNIT", "*", ".LENGTHUNIT.", "$", ".METRE.")
    origin = placement(step, (0., 0., 0.), (0., 0., 1.), (1., 0., 0.))
    context = step.add("IFCGEOMETRICREPRESENTATIONCONTEXT", "$", "'Model'", 3, 1e-6, origin, "$")
    sphere = step.add("IFCSPHERICALSURFACE", origin, SPHERE_RADIUS)
    meridian = semicircle_edges(step, (1., 0., 0.), 0., SPHERE_RADIUS)
    west = advanced_face(step, sphere, meridian, "West hemisphere", PRODUCTS[0].color, reverse=True)
    east = advanced_face(step, sphere, meridian, "East hemisphere", EAST_COLOR)

    cap_surface = step.add("IFCSPHERICALSURFACE", origin, CAP_RADIUS)
    cap_rim = semicircle_edges(step, CAP_NORMAL, CAP_OFFSET, CAP_RADIUS)
    cap = advanced_face(step, cap_surface, cap_rim, "Tilted spherical face", PRODUCTS[1].color)
    base_frame = placement(step, tuple(CAP_OFFSET * value for value in CAP_NORMAL),
                           tuple(-value for value in CAP_NORMAL), (0., 1., 0.))
    base_surface = step.add("IFCPLANE", base_frame)
    disk = advanced_face(step, base_surface, cap_rim, "Tilted disk", CAP_BASE_COLOR, reverse=True)
    for index, (product, faces) in enumerate(zip(PRODUCTS, ((west, east), (cap, disk)), strict=True)):
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


def expected_depth(index, point, pixel, matrix):
    import numpy as np

    ray = world_ray((pixel[0], HEIGHT - 1 - pixel[1]), np.linalg.inv(matrix))
    discriminant, hit = sphere_hits(ray, index)
    require(discriminant > 0 and (index == 0 or hit @ CAP_NORMAL > CAP_OFFSET),
            f"Positive depth probe outside the analytic spherical trim: {point}")
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
    sphere_interior = (sphere_disc > 0.) & (sphere[..., 2] > .60 * SPHERE_RADIUS)
    cap_interior = (cap_disc > 0.) & (cap[..., 2] > .55 * CAP_RADIUS)
    cap_margin = cap @ CAP_NORMAL - CAP_OFFSET
    masks = (
        ("West hemisphere", sphere_interior & (sphere[..., 0] < -.17), 2, 0),
        ("East hemisphere", sphere_interior & (sphere[..., 0] > .17), 0, 2),
        ("Tilted spherical face", cap_interior & (cap_margin > .15), 1, 0),
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
    clipped = cap_interior & (cap_margin < -.16)
    require(np.count_nonzero(meridian) >= 100 and np.count_nonzero(clipped) >= 450, "Insufficient trim witness pixels")
    seam_coverage = float(np.mean(visible[meridian]))
    clipped_coverage = float(np.mean(visible[clipped]))
    require(seam_coverage == 1., f"Open shared pole seam: {seam_coverage}")
    require(clipped_coverage < .01, f"Visible geometry past the tilted cap edge: {clipped_coverage}")
    results["Shared meridian"] = dict(pixels=int(np.count_nonzero(meridian)), visibleFraction=seam_coverage)
    results["Clipped spherical shoulder"] = dict(pixels=int(np.count_nonzero(clipped)), visibleFraction=clipped_coverage)
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
    model, sequence = output / "spherical-edge-loops.ifc", output / "depth-witnesses.json"
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
            print(f"PASS {technique}, TAA {taa}: sphere seam, clipped cap, and {len(frames)} depth samples", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
