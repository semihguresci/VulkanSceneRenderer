"""Render IFC swept surface charts in both native raster routes and TAA modes."""

import argparse
import math
import os
from pathlib import Path
import tempfile

from ifc_area_profiles_runtime_regression import (
    DEPTH, DISTANCE, FOV, HEIGHT, TECHNIQUES, WIDTH, Fixture, Profile,
    StepFixture, record, require, save_json, write_sequence,
)


# Profile X becomes viewport X, profile Y is negative depth, and the sweep
# vector becomes viewport +Y. A whole strip has no end cap.
STRIPS = (
    (((-.95, .24), (0., -.3), (.95, .3)),),
    (((-1.1, -.36), (-.35, .16)), ((.39, -.20), (1.1, .34))),
)
CENTERLINE_THICKNESS = .22
RADIUS = .98
ANGLE = math.radians(64)
WITNESSES = (
    (0, "left panel", (-.58, .45), True),
    (0, "right panel", (.53, .50), True),
    (0, "below start", (.01, -.23), False),
    (0, "above end", (.05, 1.03), False),
    (1, "left child", (-.74, .42), True),
    (1, "right child", (.72, .40), True),
    (1, "unbridged chart gap", (0., .39), False),
    (2, "axis of symmetry", (0., .43), True),
    (2, "left arc flank", (-.61, .40), True),
    (2, "right arc flank", (.60, .51), True),
    (2, "outside cylinder trim", (1.10, .42), False),
    (2, "above cylinder", (.04, 1.02), False),
)
FIXTURE = Fixture((
    Profile("Kinked centerline", (-2.78, -.45), (.16, .78, .68)),
    Profile("Two composite charts", (0., -.45), (.83, .22, .12)),
    Profile("Trimmed circular sweep", (2.76, -.45), (.68, .35, .89)),
), WITNESSES, None, None)


def polyline(step, points):
    return step.add("IFCPOLYLINE", "(" + ",".join(step.point(*p) for p in points) + ")")


def write_fixture(path):
    step = StepFixture()
    step.add("IFCSIUNIT", "*", ".LENGTHUNIT.", "$", ".METRE.")
    world = step.add("IFCAXIS2PLACEMENT3D", step.point(0., 0., 0.), "$", "$")
    context = step.add("IFCGEOMETRICREPRESENTATIONCONTEXT", "$", "'Model'", 3, 1e-6, world, "$")
    x_axis, z_axis = step.direction(1., 0., 0.), step.direction(0., 0., 1.)

    centerline = step.add("IFCCENTERLINEPROFILEDEF", ".CURVE.", "$",
                          polyline(step, STRIPS[0][0]), CENTERLINE_THICKNESS)
    children = [step.add("IFCARBITRARYOPENPROFILEDEF", ".CURVE.", "$", polyline(step, points))
                for points in STRIPS[1]]
    composite = step.add("IFCCOMPOSITEPROFILEDEF", ".CURVE.", "$",
                         "(" + ",".join(children) + ")", "$")
    circle = step.add("IFCCIRCLEPROFILEDEF", ".CURVE.", "$", "$", RADIUS)
    for index, (product, profile) in enumerate(zip(FIXTURE.products, (centerline, composite, circle), strict=True)):
        sweep = step.add("IFCSURFACEOFLINEAREXTRUSION", profile, world, z_axis, DEPTH)
        if index == 2:
            # This arc faces the camera after mapping source -Y to renderer +Z.
            sweep = step.add("IFCRECTANGULARTRIMMEDSURFACE", sweep, 1.5 * math.pi - ANGLE, 0.,
                             1.5 * math.pi + ANGLE, 1., ".T.", ".T.")
        rgb = step.add("IFCCOLOURRGB", "$", *product.color)
        rendering = step.add("IFCSURFACESTYLERENDERING", rgb, "0.", "$", "$", "$", "$", "$", "$", ".NOTDEFINED.")
        style = step.add("IFCSURFACESTYLE", f"'{product.name}'", ".BOTH.", f"({rendering})")
        step.add("IFCSTYLEDITEM", sweep, f"({style})", "$")
        kind, item = "Surface3D", sweep
        if index == 1:
            kind, item = "GeometricSet", step.add("IFCGEOMETRICSET", f"({sweep})")
        representation = step.add("IFCSHAPEREPRESENTATION", context, "'Surface'", f"'{kind}'", f"({item})")
        shape = step.add("IFCPRODUCTDEFINITIONSHAPE", "$", "$", f"({representation})")
        placement = step.add("IFCAXIS2PLACEMENT3D", step.point(product.center[0], 0., product.center[1]), z_axis, x_axis)
        local = step.add("IFCLOCALPLACEMENT", "$", placement)
        guid = f"{index + 1:022d}"
        step.add("IFCBUILDINGELEMENTPROXY", f"'{guid}'", "$", f"'{product.name}'", "$", "$", local, shape, "$", "$")

    header = "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('ViewDefinition [DesignTransferView]'),'2;1');\n"
    header += (f"FILE_NAME('{path.name}','2026-11-14T00:00:00',(''),(''),'Container','','');\n"
               "FILE_SCHEMA(('IFC4'));\nENDSEC;\nDATA;\n")
    path.write_text(header + "\n".join(step.elements) + "\nENDSEC;\nEND-ISO-10303-21;\n", encoding="ascii")


def world_ray(pixel, inverse):
    import numpy as np

    x, y = pixel
    far = inverse @ np.array([2 * (x + .5) / WIDTH - 1, 2 * (y + .5) / HEIGHT - 1, 0., 1.])
    return far[:3] / far[3] - [0., 0., DISTANCE]


def expected_depth(index, point, pixel, matrix):
    import numpy as np

    ray = world_ray(pixel, np.linalg.inv(matrix))
    cx, cy = FIXTURE.products[index].center
    if index == 2:
        # Front intersection with x² + z² = R² about the product's Y axis.
        a = ray[0] ** 2 + ray[2] ** 2
        b = 2 * (-cx * ray[0] + DISTANCE * ray[2])
        c = cx ** 2 + DISTANCE ** 2 - RADIUS ** 2
        t = (-b - math.sqrt(b * b - 4 * a * c)) / (2 * a)
    else:
        edges = [(a, b) for path in STRIPS[index] for a, b in zip(path, path[1:])]
        a, b = next((a, b) for a, b in edges if a[0] < point[0] < b[0])
        slope = -(b[1] - a[1]) / (b[0] - a[0])
        intercept = -a[1] - slope * (cx + a[0])
        t = (intercept - DISTANCE) / (ray[2] - slope * ray[0])
    hit = t * ray + [0., 0., DISTANCE]
    require(cy < hit[1] < cy + DEPTH, f"Depth witness leaves expected sweep: {point}")
    clip = matrix @ [*hit, 1.]
    return clip[2] / clip[3]


def check_pixels(image, fixture):
    import numpy as np
    from PIL import Image

    with Image.open(image) as source:
        pixels = np.asarray(source.convert("RGB"), dtype=np.int16)
    require(pixels.shape == (HEIGHT, WIDTH, 3), "Wrong swept fixture image extent")
    visible = np.max(pixels, axis=2) > np.max(pixels[:12, :12]) + 16
    dimension = 2 * DISTANCE * math.tan(math.radians(FOV / 2)) / HEIGHT
    yy, xx = np.mgrid[:HEIGHT, :WIDTH]
    x, y = (xx + .5 - WIDTH / 2) * dimension, (HEIGHT / 2 - yy - .5) * dimension
    results = {}
    for index, product in enumerate(fixture.products):
        cx, cy = product.center
        # A conservative interior region excludes the perspective-scaled
        # endpoints, join, chart gap, and curved trim silhouette.
        spans = ((-.68, -.26), (.27, .67)) if index == 0 else (
            ((-.81, -.52), (.56, .86)) if index == 1 else ((-.58, .58),))
        spans_mask = np.logical_or.reduce([(x > cx + left) & (x < cx + right) for left, right in spans])
        inner = spans_mask & (y > cy + .13) & (y < cy + DEPTH - .13)
        lit = pixels[inner].mean(axis=0)
        coverage = float(np.mean(visible[inner]))
        contrast = float(lit.max() - lit.min())
        require(coverage > .97 and contrast > 20., f"Missing or unstyled {product.name}: {coverage}, {lit.tolist()}")
        results[product.name] = dict(interiorPixels=int(np.sum(inner)), visibleFraction=coverage,
                                     meanRgb8=lit.tolist(), meanChannelContrast=contrast)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--managed-output", action="store_true")
    parser.add_argument("--generate-only", action="store_true")
    args = parser.parse_args()
    if not args.generate_only and os.environ.get("CONTAINER_RUN_GPU_IFC_AREA_PROFILES") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_IFC_AREA_PROFILES=1 to run IFC profile captures")
        return 77
    renderer, output = args.renderer.resolve(), args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    else:
        output.mkdir(parents=True)
    model, sequence = output / "swept-profile-charts.ifc", output / "depth-witnesses.json"
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
            print(f"PASS {technique}, TAA {taa}: swept surfaces and {len(frames)} depth samples", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
