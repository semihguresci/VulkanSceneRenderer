"""Strict legacy-material textured-zero/black raster and export regression.

Default: six cases x three representations x two techniques, plus a tiny
positive transmission classification witness per technique = 38 captures.
The zero reference is untextured; the tested zero keeps a real texture; the
positive control changes the factor/color and must visibly affect the image.
Roughness-zero cases retain an active coat/sheen lobe and compare exactly with
the untextured numerical-floor reference. No old renderer image is an oracle.

MSAA1 only; no DDGI/path-transport or GPU material-buffer readback claim.
--self-check is in-memory only. --generate-only never launches the renderer.
Native output must be fresh, preserving expected-failure old-SPIR-V baselines.
Optional --inspect-exports saves textured-zero GLBs in the same capture process
and checks the actual bound exported factor and texture identity presence.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image

from gltf_accessor_runtime_regression import NativeRun, TECHNIQUES, load_asset, require
from gltf_extended_materials_runtime_regression import (extension_material, fixture,
    validation_log_error)
from gltf_texture_formats_runtime_regression import verify_manifest


TRANSMISSION = 'KHR_materials_transmission'
CLEARCOAT = 'KHR_materials_clearcoat'
SHEEN = 'KHR_materials_sheen'
IRIDESCENCE = 'KHR_materials_iridescence'
CASES = ('transmission', 'clearcoat', 'clearcoat-roughness', 'sheen-black', 'sheen-roughness', 'iridescence')
REPRESENTATIONS = ('untextured-zero', 'textured-zero', 'textured-positive')
WIDTH, HEIGHT = 640, 360


def settings(case):
    material = extension_material(base=(.22, .32, .48), specular=0.)
    extension, leaf, texture, positive, tilt = '', '', '', None, False
    if case == 'transmission':
        extension, leaf, texture, positive = TRANSMISSION, 'transmissionFactor', 'transmissionTexture', 1.
        material['extensions'][extension] = dict(transmissionFactor=0.)
    elif case == 'clearcoat':
        extension, leaf, texture, positive = CLEARCOAT, 'clearcoatFactor', 'clearcoatTexture', 1.
        material['extensions'][extension] = dict(clearcoatFactor=0., clearcoatRoughnessFactor=.15)
    elif case == 'clearcoat-roughness':
        extension, leaf, texture, positive = CLEARCOAT, 'clearcoatRoughnessFactor', 'clearcoatRoughnessTexture', .65
        material['pbrMetallicRoughness']['baseColorFactor'] = [.04, .04, .04, 1.]
        material['extensions'][extension] = dict(clearcoatFactor=1., clearcoatRoughnessFactor=0.)
    elif case == 'sheen-black':
        extension, leaf, texture, positive = SHEEN, 'sheenColorFactor', 'sheenColorTexture', [.85, .85, .85]
        material['extensions'][extension] = dict(sheenColorFactor=[0., 0., 0.], sheenRoughnessFactor=.55)
        tilt = True
    elif case == 'sheen-roughness':
        extension, leaf, texture, positive = SHEEN, 'sheenRoughnessFactor', 'sheenRoughnessTexture', .7
        material['extensions'][extension] = dict(sheenColorFactor=[.85, .25, .08], sheenRoughnessFactor=0.)
        tilt = True
    elif case == 'iridescence':
        extension, leaf, texture, positive = IRIDESCENCE, 'iridescenceFactor', 'iridescenceTexture', 1.
        material = extension_material(base=(.6, .6, .6), metallic=1., specular=1.)
        material['pbrMetallicRoughness']['roughnessFactor'] = .3
        material['extensions'][extension] = dict(iridescenceFactor=0., iridescenceIor=1.3,
            iridescenceThicknessMinimum=400., iridescenceThicknessMaximum=400.)
    else:
        raise RuntimeError('Unknown zero-factor case')
    rgba = [255, 64, 16, 255] if case == 'sheen-black' else [255, 255, 255, 255]
    return dict(material=material, extension=extension, leaf=leaf, texture=texture,
                positive=positive, rgba=rgba, tilt=tilt)


def plan_case(case):
    description = settings(case)
    result = []
    for representation in REPRESENTATIONS:
        material = copy.deepcopy(description['material'])
        if representation == 'textured-positive':
            material['extensions'][description['extension']][description['leaf']] = copy.deepcopy(description['positive'])
        spec = dict(material=material, lighting='front')
        if representation != 'untextured-zero':
            spec['textures'] = {description['extension']+'/'+description['texture']: description['rgba']}
        result.append(dict(case=case, representation=representation, spec=spec,
            extension=description['extension'], leaf=description['leaf'], texture=description['texture'],
            tilt=description['tilt'], transparent=case == 'transmission' and representation == 'textured-positive'))
    if case == 'transmission':
        material = copy.deepcopy(description['material'])
        material['extensions'][TRANSMISSION]['transmissionFactor'] = .0005
        result.append(dict(case=case, representation='tiny-untextured-positive', spec=dict(material=material, lighting='front'),
            extension=TRANSMISSION, leaf='transmissionFactor', texture='transmissionTexture', tilt=False, transparent=True))
    return result


def validate_plan(entries):
    for case in CASES:
        rows = {entry['representation']: entry for entry in entries if entry['case'] == case}
        require(set(REPRESENTATIONS) <= set(rows), 'Missing native representation')
        reference, tested, positive = (rows[name] for name in REPRESENTATIONS)
        require(reference['spec']['material'] == tested['spec']['material'], 'Zero pair factors differ before texture sampling')
        require('textures' not in reference['spec'] and len(tested['spec'].get('textures', {})) == 1,
                'Zero pair does not isolate one real texture')
        require(tested['spec']['textures'] == positive['spec']['textures'], 'Positive control changes source texture identity')
        expected = copy.deepcopy(tested['spec']['material'])
        e, leaf = tested['extension'], tested['leaf']
        value = expected['extensions'][e][leaf]
        require(np.all(np.asarray(value) == 0), 'Tested zero factor/color is not zero')
        expected['extensions'][e][leaf] = copy.deepcopy(settings(case)['positive'])
        require(positive['spec']['material'] == expected, 'Positive control changes unrelated material values')
        require(not np.all(np.asarray(positive['spec']['material']['extensions'][e][leaf]) == 0), 'Positive material is vacuous')
        if case == 'clearcoat-roughness':
            require(tested['spec']['material']['extensions'][CLEARCOAT]['clearcoatFactor'] == 1., 'Roughness test has no active coat')
        if case == 'sheen-roughness':
            require(any(tested['spec']['material']['extensions'][SHEEN]['sheenColorFactor']), 'Roughness test has no active sheen')
    transmission = next(row for row in entries if row['representation'] == 'tiny-untextured-positive')
    require(transmission['transparent'] and 'textures' not in transmission['spec'] and
            transmission['spec']['material']['extensions'][TRANSMISSION]['transmissionFactor'] == .0005,
            'Tiny transmission witness does not test a positive factor below .001')


def generate_fixture(path, entry):
    model = fixture(path, entry['spec'])
    if entry['tilt']:
        # An actual grazing receiver, with transformed geometry AND normals.
        # This gives the sheen roughness/color controls meaningful direct light.
        angle = math.pi/3.
        model['nodes'][0]['rotation'] = [0., math.sin(angle/2.), 0., math.cos(angle/2.)]
        path.write_text(json.dumps(model, indent=2), encoding='utf-8')
    return model


def classification(state, entry):
    rows = [row for row in state['gltfMaterialVariants']['assignments'] if row['mesh'] == 0 and row['primitive'] == 0]
    require(len(rows) == 1, 'Receiver has ambiguous/missing runtime material classification')
    row = rows[0]
    require(row['transparent'] is entry['transparent'],
            'Strict transmission classification changed: '+entry['representation']+' '+str(row))
    require(not row['alphaMasked'] and not row['doubleSided'], 'Fixture changed alpha/sidedness classification')
    return dict(node=row['node'], runtimeMaterial=row['material'], transparent=row['transparent'],
                alphaMasked=row['alphaMasked'], doubleSided=row['doubleSided'], rayCompatible=row['rayCompatible'])


def inspect_state(state, entry):
    require(state['msaaSamples'] == 1, 'Zero-factor cohort requires actual MSAA1')
    require(np.max(np.abs(np.asarray(state['camera']['position'])-[0, 0, 4])) < 1e-5, 'Fixed direct-light camera changed')
    light = state['lighting']
    require(light['environmentIntensity'] == 0 and light['directionalIntensity'] == 0 and
            light['bounceIntensity'] == 0 and light['pointLightCount'] == 1,
            'Direct-light material fixture is not isolated')
    return classification(state, entry)


def image_check(first, second, *, positive, tilted):
    a = np.asarray(Image.open(first['image']).convert('RGB'), dtype=np.int16)
    b = np.asarray(Image.open(second['image']).convert('RGB'), dtype=np.int16)
    require(a.shape == (HEIGHT, WIDTH, 3) and b.shape == a.shape, 'Wrong material capture extent')
    crop = (slice(130, 230), slice(285, 350)) if tilted else (slice(112, 248), slice(242, 398))
    delta = np.abs(a[crop]-b[crop])
    result = dict(region=[crop[1].start, crop[0].start, crop[1].stop, crop[0].stop],
        meanAbsolutePixelError=float(delta.mean()), maximumPixelError=int(delta.max()),
        changedPixelFraction=float(np.any(delta >= 3, axis=2).mean()),
        fullImageMaximumError=int(np.abs(a-b).max()))
    require(float(np.mean(a[crop])) > 1., 'Untextured reference has no visible lit receiver')
    if positive:
        require(result['meanAbsolutePixelError'] >= .35 and result['changedPixelFraction'] >= .01 and result['maximumPixelError'] >= 8,
                'Positive factor/texture control has no meaningful visible effect: '+str(result))
    else:
        require(result['fullImageMaximumError'] == 0, 'Textured zero/black changed the no-effect reference: '+str(result))
    return result


def inspect_export(path, entry):
    model, _ = load_asset(path)
    indices = {primitive['material'] for mesh in model['meshes'] for primitive in mesh['primitives']}
    require(len(indices) == 1, 'Saved receiver has ambiguous material binding')
    material = model['materials'][next(iter(indices))]
    extension = material['extensions'][entry['extension']]
    default = [0., 0., 0.] if entry['leaf'] == 'sheenColorFactor' else 0.
    require(np.all(np.asarray(extension.get(entry['leaf'], default)) == 0), 'Save promoted authored zero/black factor')
    index = extension[entry['texture']]['index']
    require(isinstance(index, int) and 0 <= index < len(model['textures']), 'Save removed/broke the zero-factor texture binding')
    require(model['textures'][index].get('source', -1) >= 0, 'Saved zero-factor texture has no portable source image')
    return dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
        sourceMaterial=next(iter(indices)), factor=extension.get(entry['leaf'], default), texture=index,
        zeroFactorAndTexturePreserved=True)


def artifacts():
    folder = Path(__file__).resolve().parent
    names = (Path(__file__).name, 'gltf_accessor_runtime_regression.py',
             'gltf_extended_materials_runtime_regression.py', 'gltf_texture_formats_runtime_regression.py')
    return {str(folder/name): hashlib.sha256((folder/name).read_bytes()).hexdigest() for name in names}


def self_check():
    entries = [row for case in CASES for row in plan_case(case)]
    validate_plan(entries)
    require(len(entries) == 19, 'Default cohort must remain38 captures across both techniques')
    rejected = []
    for name, mutate in (
        ('zero-promoted-to-one', lambda rows: rows[1]['spec']['material']['extensions'][TRANSMISSION].__setitem__('transmissionFactor', 1.)),
        ('missing-zero-texture', lambda rows: rows[1]['spec'].pop('textures')),
        ('vacuous-positive-control', lambda rows: rows[2]['spec']['material']['extensions'][TRANSMISSION].__setitem__('transmissionFactor', 0.)),
        ('tiny-positive-threshold', lambda rows: rows[3]['spec']['material']['extensions'][TRANSMISSION].__setitem__('transmissionFactor', .001))):
        changed = copy.deepcopy(entries); mutate(changed)
        try:
            validate_plan(changed)
        except RuntimeError:
            rejected.append(name)
        else:
            raise RuntimeError('Fixture audit accepted '+name)
    for case in CASES:
        rows = plan_case(case)
        settings_for_case = settings(case)
        rgba = settings_for_case['rgba']
        # An independent scalar/vector multiplication witness. The former
        # factor<=0?1 convention disagrees for every nonblack sampled texture.
        sample = np.asarray(rgba[:3] if case == 'sheen-black' else
            [rgba[1] if case == 'clearcoat-roughness' else rgba[3] if case == 'sheen-roughness' else rgba[0]], dtype=float)/255.
        zero = np.asarray(rows[1]['spec']['material']['extensions'][rows[1]['extension']][rows[1]['leaf']])
        require(np.all(zero*sample == 0) and np.any(sample > 0), 'Strict multiplication fixture cannot reject old promotion')
    return dict(passed=True, cases=list(CASES), capturesPerTechnique=len(entries), plannedCaptures=38,
        rejectedControls=rejected, nativeExecuted=False,
        positiveImageContrast='Must be measured during native execution; offline checks do not claim it.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--case', action='append', choices=CASES)
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
    require(not output.exists() or not any(output.iterdir()), 'Output must be fresh; previous baseline/evidence will not be overwritten')
    entries = [row for case in args.case or CASES for row in plan_case(case)]
    if args.generate_only:
        output.mkdir(parents=True, exist_ok=True)
        rows = []
        for entry in entries:
            name = entry['case']+'-'+entry['representation']
            path = output/'fixtures'/name/'models/validation/cornell_box_local_light.gltf'
            generate_fixture(path, entry)
            rows.append(dict(name=name, path=str(path), transparent=entry['transparent']))
        (output/'generation.json').write_text(json.dumps(dict(nativeExecuted=False, fixtures=rows,
            expectedCaptures=len(entries)*len(args.technique or TECHNIQUES)), indent=2), encoding='utf-8')
        print('GENERATED', len(rows), 'fixtures; no renderer execution', flush=True)
        return
    require(args.renderer is not None, '--renderer required for native execution')
    run = NativeRun(args.renderer, output, args.timeout)
    run.report.update(oracle='strict-factor-multiplication-and-independent-no-texture-reference',
        excludedCaptureCount=0, expectedCaptureCount=len(entries)*len(args.technique or TECHNIQUES),
        exports=[], harnessArtifactsBefore=artifacts(),
        limits=['MSAA1 only; no DDGI/path-transport validation.', 'No GPU material-buffer readback claim.'])
    if args.manifest:
        run.report['nativeManifestBefore'] = verify_manifest(args.manifest)
    try:
        sources = {}
        for entry in entries:
            name = entry['case']+'-'+entry['representation']
            path = output/'fixtures'/name/'models/validation/cornell_box_local_light.gltf'
            generate_fixture(path, entry); sources[name] = path
            run.report['fixtures'].append(dict(name=name, path=str(path),
                sourceSha256=hashlib.sha256(path.read_bytes()).hexdigest(), expectedTransparent=entry['transparent']))
        for technique in args.technique or TECHNIQUES:
            records = {}
            for entry in entries:
                name = entry['case']+'-'+entry['representation']
                destination = output/(technique+'-'+name+'.glb') if args.inspect_exports and entry['representation'] == 'textured-zero' else None
                record = run.capture(sources[name], technique+'-'+name, technique, destination=destination,
                    extra=['--camera-position', '0', '0', '4', '--camera-fov', '45', '--exposure', '.35',
                        '--warmup-frames', '12', '--capture-frame', '13', '--no-ray-query', '--ray-shadows', 'raster'])
                require(not validation_log_error(Path(record['log']).read_text(encoding='utf-8', errors='replace')),
                        'Strict Vulkan validation failed: '+record['log'])
                evidence = inspect_state(record['state'], entry)
                run.report['checks'].append(dict(case=entry['case'], representation=entry['representation'],
                    technique=technique, check='strict-material-classification-and-lighting', passed=True, **evidence))
                records[name] = record
                if destination:
                    run.report['exports'].append(dict(case=entry['case'], technique=technique, **inspect_export(destination, entry)))
                if entry['representation'] == 'textured-positive':
                    reference = records[entry['case']+'-untextured-zero']
                    tested = records[entry['case']+'-textured-zero']
                    run.report['checks'].append(dict(case=entry['case'], technique=technique,
                        check='textured-zero-equals-independent-no-texture', passed=True,
                        **image_check(reference, tested, positive=False, tilted=entry['tilt'])))
                    run.report['checks'].append(dict(case=entry['case'], technique=technique,
                        check='positive-factor-and-texture-is-visible', passed=True,
                        **image_check(reference, record, positive=True, tilted=entry['tilt'])))
                run.save()
                print('PASS', technique, name, 'classification; pair controls checked afterpositive', flush=True)
        require(len(run.report['captures']) == run.report['expectedCaptureCount'], 'Incomplete zero-factor capture cohort')
        run.report['harnessArtifactsAfter'] = artifacts()
        require(run.report['harnessArtifactsAfter'] == run.report['harnessArtifactsBefore'], 'Harness/dependencies changed')
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
