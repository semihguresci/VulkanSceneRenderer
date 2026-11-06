"""Native glTF omitted-metallic default: three assets x two rasters = six runs.

Omitted metallicFactor and explicit1 must render exactly alike. Explicit0 is
an independent visible contrast control; all other source fields stay identical.
MSAA1, fixed point light18, camera z4/FOV45, no rays/DDGI/environment. Optional
--inspect-exports saves the omitted/explicit1 GLBs in their existing processes
and checks their actual bound materials resolve to metallicFactor1.

--self-check is in-memory only; --generate-only starts no renderer. Native
output must be fresh, preserving all earlier diagnostic/validation reports.
No actual GPU material-buffer readback is claimed.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image

from gltf_accessor_runtime_regression import NativeRun, TECHNIQUES, load_asset, require
from gltf_extended_materials_runtime_regression import extension_material, fixture, validation_log_error
from gltf_texture_formats_runtime_regression import verify_manifest


REPRESENTATIONS = ('omitted', 'explicit-one', 'explicit-zero')
WIDTH, HEIGHT = 640, 360


def source_materials():
    explicit = extension_material(base=(.6, .3, .12), metallic=1., specular=1.)
    explicit['name'] = 'Strict glTF metallic default receiver'
    explicit['pbrMetallicRoughness']['roughnessFactor'] = .45
    omitted, zero = copy.deepcopy(explicit), copy.deepcopy(explicit)
    omitted['pbrMetallicRoughness'].pop('metallicFactor')
    zero['pbrMetallicRoughness']['metallicFactor'] = 0.
    result = dict(zip(REPRESENTATIONS, (omitted, explicit, zero)))
    validate_sources(result)
    return result


def validate_sources(materials):
    require(set(materials) == set(REPRESENTATIONS), 'Missing metallic default representation')
    omitted, one, zero = (materials[name] for name in REPRESENTATIONS)
    require('metallicFactor' not in omitted['pbrMetallicRoughness'], 'Omitted source contains a metallic factor')
    require(one['pbrMetallicRoughness'].get('metallicFactor') == 1., 'Explicit-one source is not one')
    require(zero['pbrMetallicRoughness'].get('metallicFactor') == 0., 'Explicit-zero contrast is not zero')
    normalized = []
    for material in (omitted, one, zero):
        candidate = copy.deepcopy(material)
        candidate['pbrMetallicRoughness'].pop('metallicFactor', None)
        normalized.append(candidate)
    require(normalized[0] == normalized[1] == normalized[2], 'Metallic representations differ in unrelated source values')
    require(one['pbrMetallicRoughness']['roughnessFactor'] == .45 and
            one['extensions']['KHR_materials_specular']['specularFactor'] == 1.,
            'Metallic default test lost its lit specular witness')


def image_metrics(first, second, contrast=False):
    a = np.asarray(first, dtype=np.int16)
    b = np.asarray(second, dtype=np.int16)
    require(a.shape == b.shape == (HEIGHT, WIDTH, 3), 'Unexpected material image dimensions')
    crop = (slice(112, 248), slice(242, 398))
    delta = np.abs(a[crop]-b[crop])
    result = dict(region=[242, 112, 398, 248], meanAbsolutePixelError=float(delta.mean()),
        maximumPixelError=int(delta.max()), changedPixelFraction=float(np.any(delta >= 3, axis=2).mean()),
        fullImageMaximumError=int(np.abs(a-b).max()))
    require(float(np.mean(a[crop])) > 1., 'Reference image has no visible lit receiver')
    if contrast:
        require(result['meanAbsolutePixelError'] >= .35 and result['maximumPixelError'] >= 8 and
                result['changedPixelFraction'] >= .01, 'Explicit-zero metallic control has no meaningful contrast: '+str(result))
    else:
        require(result['fullImageMaximumError'] == 0, 'Omitted metallic differs from explicit schema default1: '+str(result))
    return result


def inspect_state(state):
    require(state['msaaSamples'] == 1, 'Metallic default cohort requires actual MSAA1')
    require(np.max(np.abs(np.asarray(state['camera']['position'])-[0, 0, 4])) < 1e-5, 'Fixed material camera changed')
    light = state['lighting']
    require(light['environmentIntensity'] == 0 and light['directionalIntensity'] == 0 and
            light['bounceIntensity'] == 0 and light['pointLightCount'] == 1,
            'Metallic material lighting is not isolated')
    require(not state['rayShadows']['active'] and not state['ddgi']['enabled'], 'Unexpected ray/DDGI path in isolated material test')
    rows = [row for row in state['gltfMaterialVariants']['assignments'] if row['mesh'] == 0 and row['primitive'] == 0]
    require(len(rows) == 1 and not rows[0]['transparent'] and not rows[0]['alphaMasked'] and
            not rows[0]['doubleSided'], 'Metallic default material changed opaque classification')
    return dict(node=rows[0]['node'], runtimeMaterial=rows[0]['material'], transparent=False,
                alphaMasked=False, doubleSided=False, rayActive=False, ddgiEnabled=False)


def inspect_export(path):
    model, _ = load_asset(path)
    bindings = {primitive['material'] for mesh in model['meshes'] for primitive in mesh['primitives']}
    require(len(bindings) == 1, 'Saved metallic receiver has ambiguous bound material')
    index = next(iter(bindings))
    material = model['materials'][index]
    factor = material.get('pbrMetallicRoughness', {}).get('metallicFactor', 1.)
    require(isinstance(factor, (float, int)) and factor == 1., 'Saved omitted/explicit-one material no longer resolves to metallic1')
    return dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                sourceMaterial=index, resolvedMetallicFactor=factor, schemaDefaultPreserved=True)


def artifacts():
    folder = Path(__file__).resolve().parent
    names = (Path(__file__).name, 'gltf_accessor_runtime_regression.py',
             'gltf_extended_materials_runtime_regression.py', 'gltf_texture_formats_runtime_regression.py')
    return {str(folder/name): hashlib.sha256((folder/name).read_bytes()).hexdigest() for name in names}


def self_check():
    materials = source_materials()
    rejected = []
    for name, mutate in (
        ('omitted-set-to-zero', lambda m: m['omitted']['pbrMetallicRoughness'].__setitem__('metallicFactor', 0.)),
        ('explicit-one-removed', lambda m: m['explicit-one']['pbrMetallicRoughness'].pop('metallicFactor')),
        ('vacuous-zero-control', lambda m: m['explicit-zero']['pbrMetallicRoughness'].__setitem__('metallicFactor', 1.)),
        ('unrelated-roughness-edit', lambda m: m['omitted']['pbrMetallicRoughness'].__setitem__('roughnessFactor', .7))):
        bad = copy.deepcopy(materials); mutate(bad)
        try:
            validate_sources(bad)
        except RuntimeError:
            rejected.append(name)
        else:
            raise RuntimeError('Source verifier accepted '+name)
    image = np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8)
    image[100:260, 208:432] = [60, 40, 20]
    image_metrics(image, image)
    changed = image.copy(); changed[112:248, 242:398] = [20, 50, 90]
    image_metrics(image, changed, contrast=True)
    for name, a, b, contrast in (
        ('image-default-drift', image, changed, False),
        ('image-positive-vacuous', image, image, True),
        ('image-reference-black', np.zeros_like(image), changed, True)):
        try:
            image_metrics(a, b, contrast)
        except RuntimeError:
            rejected.append(name)
        else:
            raise RuntimeError('Image verifier accepted '+name)
    return dict(passed=True, representations=list(REPRESENTATIONS), plannedCaptures=6,
                rejectedControls=rejected, nativeExecuted=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--technique', action='append', choices=TECHNIQUES)
    parser.add_argument('--timeout', type=float, default=240.)
    parser.add_argument('--inspect-exports', action='store_true')
    parser.add_argument('--self-check', action='store_true')
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    if args.self_check:
        print(json.dumps(self_check(), indent=2), flush=True)
        if args.output is None and args.renderer is None:
            return
    require(args.output is not None, '--output required')
    output = args.output.resolve()
    require(not output.exists() or not any(output.iterdir()), 'Output must be fresh; earlier evidence will not be overwritten')
    materials = source_materials()
    if args.generate_only:
        output.mkdir(parents=True, exist_ok=True)
        sources = []
        for name, material in materials.items():
            path = output/'fixtures'/name/'models/validation/cornell_box_local_light.gltf'
            fixture(path, dict(material=material, lighting='front'))
            sources.append(dict(name=name, path=str(path)))
        (output/'generation.json').write_text(json.dumps(dict(nativeExecuted=False, fixtures=sources,
            expectedCaptures=3*len(args.technique or TECHNIQUES)), indent=2), encoding='utf-8')
        print('GENERATED three metallic-default fixtures; no native execution', flush=True)
        return
    require(args.renderer is not None, '--renderer required for native execution')
    run = NativeRun(args.renderer, output, args.timeout)
    run.report.update(oracle='omitted-metallic-equals-explicit-schema-one', excludedCaptureCount=0,
        expectedCaptureCount=3*len(args.technique or TECHNIQUES), harnessArtifactsBefore=artifacts(), exports=[])
    if args.manifest:
        run.report['nativeManifestBefore'] = verify_manifest(args.manifest)
    try:
        sources = {}
        for name, material in materials.items():
            path = output/'fixtures'/name/'models/validation/cornell_box_local_light.gltf'
            model = fixture(path, dict(material=material, lighting='front')); sources[name] = path
            require(('metallicFactor' in model['materials'][0]['pbrMetallicRoughness']) == (name != 'omitted'),
                    'Fixture writer changed metallic leaf presence')
            run.report['fixtures'].append(dict(name=name, path=str(path),
                sourceSha256=hashlib.sha256(path.read_bytes()).hexdigest()))
        for technique in args.technique or TECHNIQUES:
            captures = {}
            for name in REPRESENTATIONS:
                destination = output/(technique+'-'+name+'.glb') if args.inspect_exports and name != 'explicit-zero' else None
                capture = run.capture(sources[name], technique+'-'+name, technique, destination=destination,
                    extra=['--camera-position', '0', '0', '4', '--camera-fov', '45', '--exposure', '.35',
                        '--warmup-frames', '12', '--capture-frame', '13', '--no-ray-query', '--ray-shadows', 'raster', '--no-ddgi'])
                require(not validation_log_error(Path(capture['log']).read_text(encoding='utf-8', errors='replace')),
                        'Strict Vulkan validation failed: '+capture['log'])
                run.report['checks'].append(dict(technique=technique, representation=name,
                    check='opaque-isolated-direct-light-material', passed=True, **inspect_state(capture['state'])))
                captures[name] = np.asarray(Image.open(capture['image']).convert('RGB'))
                if destination:
                    run.report['exports'].append(dict(technique=technique, representation=name, **inspect_export(destination)))
                run.save()
            run.report['checks'].append(dict(technique=technique, check='omitted-equals-explicit-one', passed=True,
                **image_metrics(captures['omitted'], captures['explicit-one'])))
            run.report['checks'].append(dict(technique=technique, check='explicit-zero-has-visible-contrast', passed=True,
                **image_metrics(captures['explicit-one'], captures['explicit-zero'], contrast=True)))
            run.save(); print('PASS', technique, 'omitted==one exactly; explicit-zero visibly differs', flush=True)
        require(len(run.report['captures']) == run.report['expectedCaptureCount'], 'Incomplete metallic-default cohort')
        run.report['harnessArtifactsAfter'] = artifacts()
        require(run.report['harnessArtifactsBefore'] == run.report['harnessArtifactsAfter'], 'Default harness/dependencies changed')
        if args.manifest:
            run.report['nativeManifestAfter'] = verify_manifest(args.manifest)
        run.finish()
    except Exception as error:
        run.report['failure'] = str(error)
        raise
    finally:
        run.report.update(captureCount=len(run.report['captures']), checkCount=len(run.report['checks']),
                          exportCheckCount=len(run.report['exports']))
        run.save()
    print('PASS', len(run.report['captures']), 'captures;', len(run.report['checks']), 'checks;', output)


if __name__ == '__main__':
    main()
