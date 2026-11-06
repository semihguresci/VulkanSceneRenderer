"""Independent disk-five selection stroke oracle; generated assets only.

Default native matrix: six fixtures x both raster techniques = twelve captures.
Single-sample strokes must equal the complete integer radius-five dilation of
the captured blue silhouette, minus that silhouette. Aligned source fill must
also match literal pixel rectangles. The sloped fill permits a one-pixel
analytical raster-boundary discrepancy; its stroke comparison stays exact.
MSAA4 alone permits mismatches in a one-pixel blue/stroke boundary band, never
holes in the stroke interior or disconnected extra yellow islands. No claims
about subpixel features that never cover a sample, alpha cutouts or depth-only
visible outlines are made. Instanced nonzero object-base validation is separate.

--self-check runs only in-memory oracle/generator checks. --generate-only writes
fixtures without starting the renderer. Native output must be a fresh directory;
failed old-kernel baseline reports are preserved, never overwritten by a rerun.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image

from gltf_accessor_runtime_regression import Asset, NativeRun, TECHNIQUES, mesh, require
from gltf_texture_formats_runtime_regression import verify_manifest
from gltf_extended_materials_runtime_regression import validation_log_error


RADIUS = 5
DISK = tuple((x, y) for y in range(-RADIUS, RADIUS + 1)
             for x in range(-RADIUS, RADIUS + 1) if x*x + y*y <= RADIUS*RADIUS)
NEIGHBORS = tuple((x, y) for y in (-1, 0, 1) for x in (-1, 0, 1))
CASES = (
    ('horizontal', 640, 360, 1),
    ('vertical-wide', 1280, 360, 1),
    ('plus-portrait', 360, 640, 1),
    ('ring', 640, 360, 1),
    ('concave-msaa4', 640, 360, 4),
    ('sloped', 640, 360, 1),
)


def shifted(mask, dx, dy):
    """Translate Boolean pixels with zero border; never wrap at image edges."""
    h, w = mask.shape
    result = np.zeros_like(mask)
    if abs(dx) >= w or abs(dy) >= h:
        return result
    left, right = max(0, dx), min(w, w + dx)
    top, bottom = max(0, dy), min(h, h + dy)
    result[top:bottom, left:right] = mask[top-dy:bottom-dy, left-dx:right-dx]
    return result


def dilate(mask, offsets=DISK):
    result = np.zeros_like(mask)
    for dx, dy in offsets:
        result |= shifted(mask, dx, dy)
    return result


def boundary(mask):
    return dilate(mask, NEIGHBORS) & dilate(~mask, NEIGHBORS)


def stroke_metrics(fill, actual, samples):
    require(fill.dtype == bool and actual.dtype == bool and fill.shape == actual.shape,
            'Oracle requires equal Boolean silhouettes')
    require(np.count_nonzero(fill) >= 50, 'Missing/nonvacuous blue silhouette witness')
    expected = dilate(fill) & ~fill
    require(np.count_nonzero(expected) >= 200, 'Vacuous expected stroke')
    missing, extra = expected & ~actual, actual & ~expected
    differences = missing | extra
    result = dict(radiusPixels=RADIUS, integerKernelPositions=len(DISK),
        bluePixels=int(fill.sum()), expectedStrokePixels=int(expected.sum()),
        actualStrokePixels=int(actual.sum()), missingPixels=int(missing.sum()),
        extraPixels=int(extra.sum()), mismatchPixels=int(differences.sum()),
        msaaSamples=samples, tolerance='exact' if samples == 1 else 'one-pixel-boundary-only')
    if samples == 1:
        require(not np.any(differences), 'Selection stroke is not a solid disk-five dilation: '+str(result))
    else:
        require(samples == 4, 'Only explicitly bounded MSAA4 is covered')
        allowed = boundary(expected) | boundary(fill)
        result['mismatchesOutsideBoundary'] = int((differences & ~allowed).sum())
        result['mismatchFraction'] = float(differences.sum()/expected.sum())
        require(not np.any(differences & ~allowed), 'MSAA stroke has an interior gap/extra island: '+str(result))
        require(result['mismatchFraction'] <= .25, 'MSAA tolerance covers too much of the stroke: '+str(result))
    return result


def rectangles(name, w, h):
    x, y = w//2, h//2
    if name == 'horizontal': return [(x-100, y, x+100, y+1)]
    if name == 'vertical-wide': return [(x, y-100, x+1, y+100)]
    if name == 'plus-portrait':
        return [(x-65, y, x+65, y+1), (x, y-65, x+1, y+65)]
    if name == 'ring':
        return [(x-50, y-50, x+50, y-49), (x-50, y+49, x+50, y+50),
                (x-50, y-49, x-49, y+49), (x+49, y-49, x+50, y+49)]
    if name == 'concave-msaa4':
        # Connected three-sided corner, two close disconnected parts whose
        # strokes must merge, and a detached one-pixel component with round cap.
        return [(x-90, y-40, x, y-39), (x-90, y-40, x-89, y+40),
                (x-90, y+39, x-50, y+40), (x+25, y-30, x+26, y+30),
                (x+29, y-30, x+30, y+30), (x+65, y, x+66, y+1)]
    require(name == 'sloped', 'Unknown fixture')
    return []


def polygons(name, w, h):
    result = [[(left, top), (left, bottom), (right, bottom), (right, top)]
              for left, top, right, bottom in rectangles(name, w, h)]
    if name == 'sloped':
        a, b = np.array([w/2-80.25, h/2+40.25]), np.array([w/2+75.25, h/2-34.75])
        direction = b-a
        normal = np.array([-direction[1], direction[0]])/np.linalg.norm(direction)*.5
        result.append([tuple(a-normal), tuple(a+normal), tuple(b+normal), tuple(b-normal)])
    return result


def literal_fill(name, w, h):
    result = np.zeros((h, w), dtype=bool)
    if name != 'sloped':
        for left, top, right, bottom in rectangles(name, w, h):
            result[top:bottom, left:right] = True
        return result
    # Independent center-sample triangle coverage, using literal screen-space
    # vertices rather than source accessors or the renderer's projection code.
    for polygon in polygons(name, w, h):
        for indices in ((0, 1, 2), (0, 2, 3)):
            triangle = np.array([polygon[i] for i in indices])
            left, top = np.floor(triangle.min(axis=0)).astype(int)
            right, bottom = np.ceil(triangle.max(axis=0)).astype(int)
            yy, xx = np.mgrid[top:bottom, left:right]
            xx, yy = xx+.5, yy+.5
            edges = []
            for i in range(3):
                a, b = triangle[i], triangle[(i+1) % 3]
                edges.append((b[0]-a[0])*(yy-a[1])-(b[1]-a[1])*(xx-a[0]))
            result[top:bottom, left:right] |= (np.logical_and.reduce([e >= 0 for e in edges]) |
                                              np.logical_and.reduce([e <= 0 for e in edges]))
    return result


def build_asset(name, w, h):
    a = Asset()
    positions, indices = [], []
    world_per_pixel = 6.*math.tan(math.pi/6.)/h  # camera z3, vertical FOV60
    screen_vertices = []
    for polygon in polygons(name, w, h):
        start = len(positions)
        for x, y in polygon:
            positions.append([(x-w/2.)*world_per_pixel, (h/2.-y)*world_per_pixel, 0.])
            screen_vertices.append([x, y])
        indices.extend(start+i for i in (0, 1, 2, 0, 2, 3))
    count = len(positions)
    mesh(a, a.accessor('VEC3', positions), a.accessor('VEC3', [[0., 0., 1.]]*count),
         a.accessor('VEC2', [[0., 0.]]*count),
         indices=a.accessor('SCALAR', indices, 5123, indices=True))
    a.model['nodes'][0]['name'] = 'Selected single-primitive '+name
    a.model['materials'] = [dict(pbrMetallicRoughness=dict(baseColorFactor=[.015, .04, 1., 1.]),
                                 extensions=dict(KHR_materials_unlit={}))]
    a.model['extensionsUsed'] = ['KHR_materials_unlit']
    # Check binary32 conversion still projects within 1/1000 pixel of the
    # authored screen positions; no renderer matrix helper is used.
    packed = np.asarray(positions, dtype=np.float32).astype(float)
    projected = np.column_stack((packed[:, 0]/world_per_pixel+w/2., h/2.-packed[:, 1]/world_per_pixel))
    error = float(np.max(np.abs(projected-np.asarray(screen_vertices))))
    require(error < .001, 'Generated float positions are not pixel aligned')
    return a, dict(cameraPosition=[0., 0., 3.], cameraTarget=[0., 0., 0.],
                   cameraFovDegrees=60., worldUnitsPerPixel=world_per_pixel,
                   binary32ProjectionMaxPixelError=error, geometryVertices=count,
                   geometryTriangles=len(indices)//3)


def image_masks(values):
    require(values.ndim == 3 and values.shape[2] == 3, 'Expected RGB capture')
    r, g, b = (values[:, :, i].astype(float) for i in range(3))
    blue = (b > 120) & (b > 2*r) & (b > 2*g)
    yellow = (r > 120) & (g > 60) & (b < 100)
    return blue, yellow


def inspect_capture(record, spec):
    name, w, h, samples = spec
    state = record['state']
    require(state['msaaSamples'] == samples, 'Requested MSAA mode did not execute')
    require(np.max(np.abs(np.asarray(state['camera']['position'])-[0, 0, 3])) < 1e-5,
            'Capture did not use literal camera position')
    rgb = np.asarray(Image.open(record['image']).convert('RGB'))
    require(rgb.shape == (h, w, 3), 'Capture extent differs from authored projection')
    blue, yellow = image_masks(rgb)
    analytic = literal_fill(name, w, h)
    differences = blue ^ analytic
    if samples == 1 and name != 'sloped':
        require(not np.any(differences), f'Pixel-aligned one-pixel source fill changed: {name}, {int(differences.sum())} pixels')
    else:
        require(not np.any(differences & ~boundary(analytic)), 'Source fill differs beyond its analytical raster boundary')
        require(np.count_nonzero(differences) <= max(4, .15*analytic.sum()), 'Too much analytical source fill changed')
    result = stroke_metrics(blue, yellow, samples)
    result.update(case=name, extent=[w, h], analyticalFillMismatchPixels=int(differences.sum()),
                  sourceFillTolerance='exact' if samples == 1 and name != 'sloped' else 'one-pixel-raster-boundary')
    return result


def self_check():
    require(len(DISK) == 81 and (0, 0) in DISK, 'Incorrect independent disk oracle')
    passed, rejected = [], []
    for name, w, h, samples in CASES:
        asset, projection = build_asset(name, w, h)
        require(len(asset.model['meshes']) == 1 and len(asset.model['meshes'][0]['primitives']) == 1,
                'Fixture must select all disconnected parts through one primitive')
        fill = literal_fill(name, w, h)
        expected = dilate(fill) & ~fill
        before = fill.copy()
        stroke_metrics(fill, expected, samples)
        require(np.array_equal(fill, before), 'Oracle mutated source silhouette')
        rgb = np.zeros((h, w, 3), dtype=np.uint8)
        rgb[fill], rgb[expected] = [20, 45, 230], [255, 190, 0]
        actual_fill, actual_stroke = image_masks(rgb)
        require(np.array_equal(actual_fill, fill) and np.array_equal(actual_stroke, expected), 'Color classifier invalid')
        passed.append(dict(case=name, bluePixels=int(fill.sum()), strokePixels=int(expected.sum()), **projection))
    fill = literal_fill('horizontal', 640, 360)
    expected = dilate(fill) & ~fill
    old_offsets = ((5, 0), (-5, 0), (0, 5), (0, -5), (4, 4), (-4, 4), (4, -4), (-4, -4))
    old = dilate(fill, old_offsets) & ~fill
    hole = expected.copy(); hole[182, 300] = False
    island = expected.copy(); island[20, 20] = True
    narrow = dilate(fill, tuple((x, y) for x, y in DISK if x*x+y*y <= 16)) & ~fill
    for name, value, samples in [('old-eight-boundary', old, 1), ('interior-hole', hole, 1),
                                  ('extra-island', island, 1), ('radius-four', narrow, 1),
                                  ('msaa-interior-hole', hole, 4), ('msaa-extra-island', island, 4)]:
        try:
            stroke_metrics(fill, value, samples)
        except RuntimeError:
            rejected.append(name)
        else:
            raise RuntimeError('Oracle accepted negative control '+name)
    require(len(rejected) == 6, 'Missing oracle negative controls')
    return dict(passed=True, generatedFixtures=passed, rejectedControls=rejected,
                oldEightBoundaryMissingPixels=int((expected & ~old).sum()),
                nativeExecuted=False)


def artifact_hashes():
    folder = Path(__file__).resolve().parent
    names = (Path(__file__).name, 'gltf_accessor_runtime_regression.py',
             'gltf_texture_formats_runtime_regression.py', 'gltf_extended_materials_runtime_regression.py')
    return {str(folder/name): hashlib.sha256((folder/name).read_bytes()).hexdigest() for name in names}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--case', action='append', choices=[case[0] for case in CASES])
    parser.add_argument('--technique', action='append', choices=TECHNIQUES)
    parser.add_argument('--timeout', type=float, default=180.)
    parser.add_argument('--self-check', action='store_true')
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    if args.self_check:
        print(json.dumps(self_check(), indent=2), flush=True)
        if args.output is None and args.renderer is None:
            return
    require(args.output is not None, '--output is required for generation/native execution')
    output = args.output.resolve()
    require(not output.exists() or not any(output.iterdir()), 'Output directory must be fresh; previous evidence will not be overwritten')
    cases = [case for case in CASES if not args.case or case[0] in args.case]
    if args.generate_only:
        output.mkdir(parents=True, exist_ok=True)
        fixtures = []
        for name, w, h, samples in cases:
            asset, info = build_asset(name, w, h)
            source = output/(name+'.gltf'); asset.write(source)
            fixtures.append(dict(case=name, path=str(source), extent=[w, h], msaa=samples, **info))
        (output/'generation.json').write_text(json.dumps(dict(nativeExecuted=False, fixtures=fixtures,
            plannedCaptures=len(cases)*len(args.technique or TECHNIQUES)), indent=2), encoding='utf-8')
        print('Generated', len(fixtures), 'fixtures; no native execution', flush=True)
        return
    require(args.renderer is not None, '--renderer is required for native execution')
    run = NativeRun(args.renderer, output, args.timeout)
    run.report.update(oracle='independent-integer-disk-five', excludedCaptureCount=0,
        limitations=['No GPU picking claim; no instanced/nonzero-base binding fixture in this bounded cohort.',
                     'MSAA4 boundary tolerance is reported separately from exact single-sample stroke checks.'],
        harnessArtifactsBefore=artifact_hashes())
    if args.manifest:
        run.report['nativeManifestBefore'] = verify_manifest(args.manifest)
    try:
        for case in cases:
            name, w, h, samples = case
            asset, info = build_asset(name, w, h)
            source = output/(name+'.gltf'); asset.write(source)
            run.report['fixtures'].append(dict(case=name, path=str(source), extent=[w, h], msaa=samples,
                sourceSha256=hashlib.sha256(source.read_bytes()).hexdigest(), **info))
            for technique in args.technique or TECHNIQUES:
                record = run.capture(source, technique+'-'+name, technique,
                    extra=['--width', str(w), '--height', str(h), '--msaa', str(samples)])
                require(not validation_log_error(Path(record['log']).read_text(encoding='utf-8', errors='replace')),
                        'Strict Vulkan validation failed: '+record['log'])
                result = inspect_capture(record, case)
                run.report['checks'].append(dict(check='continuous-solid-disk-five-stroke', technique=technique,
                    passed=True, **result))
                run.save()
                print('PASS', technique, name, result['mismatchPixels'], 'stroke mismatch pixels', flush=True)
        run.report['harnessArtifactsAfter'] = artifact_hashes()
        require(run.report['harnessArtifactsAfter'] == run.report['harnessArtifactsBefore'], 'Harness dependencies changed during validation')
        if args.manifest:
            run.report['nativeManifestAfter'] = verify_manifest(args.manifest)
        run.finish()
    except Exception as error:
        run.report['failure'] = str(error)
        raise
    finally:
        run.report.update(captureCount=len(run.report['captures']), checkCount=len(run.report['checks']))
        run.save()
    print('PASS', len(run.report['captures']), 'captures;', len(run.report['checks']), 'checks;', output)


if __name__ == '__main__':
    main()
