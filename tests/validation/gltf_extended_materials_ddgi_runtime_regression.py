"""Bounded actual-ray/DDGI material equivalence and visible indirect response.

Eight captures in one raster technique exercise three independent equivalent
representations and a nonzero-GI vs zero-intensity control. Every capture must
execute hard ray shadows, positive BLAS/TLAS work, 27 probes x 8 rays, GPU-timed
trace/update passes and committed probe history. An emissive side panel and a
hidden rear panel provide indirect incident radiance on both receiver sides.

The explicit 3x3x3 lattice is authored here; it is NOT an automatic scene fit.
The production fitter needs at least 64 probes for nonzero three-axis bounds.
Equivalent representations are mathematical controls, not physical ground
truth. Probe approximation and raster material storage remain approximate.
--generate-only and --self-check never execute a renderer or claim native work.
"""
from __future__ import annotations

import argparse
import base64
import copy
import json
import math
import os
from pathlib import Path
import re
import subprocess
import tempfile

import numpy as np

import gltf_extended_materials_runtime_regression as shared
from gltf_accessor_runtime_regression import Asset, load_asset, require
from gltf_tangent_uv_runtime_regression import finite_json
from gltf_texture_formats_runtime_regression import verify_manifest


SHARED_SHA = 'ba77669a4c34498ae38bababc205acff75f6b7f617b98a55c1bbb15551ab4e46'
COUNTS = [3, 3, 3]
ORIGIN = [-1., -.9, -.5]
SPACING = [1., .9, .75]
RAYS = 8
PROBES = 27
RAY_BUDGET = PROBES * RAYS
WARMUP = 63
CAPTURE_FRAME = 64
GI_INTENSITY = 4.
TECHNIQUES = ('deferred-raster', 'forward-raster')


def dependency_hashes():
    scripts = [Path(__file__), Path(shared.__file__),
        Path(__file__).with_name('gltf_accessor_runtime_regression.py'),
        Path(__file__).with_name('gltf_tangent_uv_runtime_regression.py'),
        Path(__file__).with_name('gltf_texture_formats_runtime_regression.py')]
    require(shared.sha(Path(shared.__file__)) == SHARED_SHA,
            'Shared frozen material harness changed; review dependencies before native execution')
    return {str(path.resolve()): shared.sha(path) for path in scripts}


def add_bounce_panels(path):
    model, buffers = load_asset(path)
    asset = Asset()
    asset.model, asset.data = model, bytearray(buffers[0])
    # Keep direct light weak so a real GI contribution cannot hide under a
    # clipped direct highlight. The lamp and its hard visibility still execute.
    model['extensions']['KHR_lights_punctual']['lights'][0]['intensity'] = 2.
    panels = [
        ('Side emissive bounce', [[-1.25, -1., 0.], [-1.25, 1., 0.],
             [-1.25, 1., 1.2], [-1.25, -1., 1.2]], [1., 0., 0.]),
        # This panel fits behind the opaque receiver silhouette from camera+Z.
        ('Hidden rear emissive bounce', [[-1.1, -.9, -.75], [1.1, -.9, -.75],
             [1.1, .9, -.75], [-1.1, .9, -.75]], [0., 0., 1.]),
    ]
    for name, positions, normal in panels:
        material_index = len(model['materials'])
        material = shared.extension_material(base=(0, 0, 0), specular=0.)
        material.update(name=name, emissiveFactor=[1., 1., 1.])
        model['materials'].append(material)
        primitive = dict(attributes=dict(
            POSITION=asset.accessor('VEC3', positions),
            NORMAL=asset.accessor('VEC3', [normal]*4)),
            indices=asset.accessor('SCALAR', [0, 1, 2, 0, 2, 3], 5123, indices=True),
            material=material_index, mode=4)
        mesh_index = len(model['meshes'])
        model['meshes'].append(dict(name=name, primitives=[primitive]))
        model['nodes'].append(dict(name=name, mesh=mesh_index))
    model['scenes'][0]['nodes'] = list(range(len(model['nodes'])))
    model['asset']['generator'] = 'Container bounded extended-material DDGI controls'
    asset.write(path)


def plan(output):
    material = lambda **kwargs: shared.extension_material(
        base=(.5, .3, .12), metallic=.15, specular=1., **kwargs)
    specs = {}
    factor_textured = material(anisotropy=.65, diffuse=1.)
    specs['factor-textured'] = dict(material=factor_textured,
        textures={f'{shared.DIFFUSE}/diffuseTransmissionTexture': [0, 0, 0, 128]})
    specs['factor-scalar'] = dict(material=material(anisotropy=.65, diffuse=128/255))
    srgb = np.array([64, 128, 192], dtype=float)/255
    linear = np.where(srgb <= .04045, srgb/12.92, ((srgb+.055)/1.055)**2.4).tolist()
    specs['color-textured'] = dict(material=material(anisotropy=.65, diffuse=.6, color=(1, 1, 1)),
        textures={f'{shared.DIFFUSE}/diffuseTransmissionColorTexture': [64, 128, 192, 255]})
    specs['color-linear'] = dict(material=material(anisotropy=.65, diffuse=.6, color=linear))
    rotated = material(anisotropy=.8, diffuse=.5)
    rotated['extensions'][shared.ANISO]['anisotropyRotation'] = math.pi/4
    specs['anisotropy-rotation'] = dict(material=rotated)
    specs['anisotropy-tangent'] = dict(material=material(anisotropy=.8, diffuse=.5),
        tangent=[math.sqrt(.5), math.sqrt(.5), 0., 1.])
    specs['gi-witness'] = dict(material=material(anisotropy=.65, diffuse=.7))
    fixtures = {name: dict(path=str(output/'fixtures'/name/'models/validation/cornell_box_local_light.gltf'),
                          spec=spec) for name, spec in specs.items()}
    captures = [dict(name=name, fixture=name, intensity=GI_INTENSITY) for name in specs if name != 'gi-witness']
    captures += [dict(name='gi-on', fixture='gi-witness', intensity=GI_INTENSITY),
                 dict(name='gi-off-intensity-control', fixture='gi-witness', intensity=0.)]
    checks = [
        dict(kind='near', name='strict-factor-times-A128-equals-scalar', images=['factor-textured', 'factor-scalar']),
        dict(kind='near', name='independent-srgb-decode-equals-linear-color', images=['color-textured', 'color-linear']),
        dict(kind='near', name='independent-tangent-equals-positive-rotation', images=['anisotropy-tangent', 'anisotropy-rotation']),
        dict(kind='contrast', name='nonzero-published-GI-changes-lit-receiver', images=['gi-on', 'gi-off-intensity-control']),
    ]
    thresholds = shared.build_plan(output/'unused-plan-only', ('essential',))['thresholds']
    return dict(schemaVersion=1, fixtures=fixtures, captures=captures, checks=checks,
        thresholds=thresholds, expectedCaptureCount=8, expectedImageCheckCount=4,
        expectedExecutionCheckCount=8, expectedPositiveGiCaptures=7,
        probeVolume=dict(placement='explicit independently authored lattice; no automatic fit',
            counts=COUNTS, origin=ORIGIN, spacing=SPACING, probes=PROBES,
            raysPerProbe=RAYS, maximumRaysPerUpdate=RAY_BUDGET),
        warmupFrames=WARMUP, captureFrame=CAPTURE_FRAME,
        oracleNotes=[
            'Factor1 multiplied by unorm8 alpha128 equals independently authored float32 factor128/255.',
            'Color64/128/192 is decoded with the IEC sRGB transfer equation into an independent linear factor.',
            'Authored tangent(cos(pi/4),sin(pi/4),0,1) is independent of the shader rotation implementation.',
            'All comparisons use fixed existing byte precision thresholds, never an output-generated golden baseline.',
            'GIoff is intensity0 with trace/update execution retained; the GIon image must visibly differ at intensity4.',
            'Positive GPU timestamps, committed history, complete probe scheduling and captured output witness actual submitted work.',
            'These checks prove material equivalence and a visible GI response, not path-traced physical correctness or atlas readback.'])


def generate(document):
    records = []
    for name, entry in document['fixtures'].items():
        path = Path(entry['path'])
        shared.fixture(path, entry['spec'])
        add_bounce_panels(path)
        model, _ = load_asset(path)
        require(len(model['meshes']) == 3 and len(model['materials']) == 3,
                'DDGI fixture lost its receiver or either independent bounce panel')
        require(model['materials'][0].get('alphaMode', 'OPAQUE') == 'OPAQUE' and
                not model['materials'][0].get('doubleSided', False) and
                'KHR_materials_transmission' not in model['materials'][0].get('extensions', {}),
                'DDGI receiver must remain ordinary opaque geometry')
        records.append(dict(name=name, path=str(path), sha256=shared.sha(path)))
    # Verify representation arithmetic from the authored inputs, before any
    # renderer launch. These values never come from telemetry or screenshots.
    scalar = document['fixtures']['factor-scalar']['spec']['material']['extensions'][shared.DIFFUSE]['diffuseTransmissionFactor']
    require(np.float32(128/255) == np.float32(scalar), 'Factor equivalence changed')
    tangent = np.array(document['fixtures']['anisotropy-tangent']['spec']['tangent'][:3])
    angle = document['fixtures']['anisotropy-rotation']['spec']['material']['extensions'][shared.ANISO]['anisotropyRotation']
    require(np.max(abs(tangent-np.array([math.cos(angle), math.sin(angle), 0.]))) < 1e-12,
            'Independent authored anisotropy tangent changed')
    require(document['captures'][-1]['fixture'] == document['captures'][-2]['fixture'],
            'GI intensity control must use the exact same source fixture')
    return records


def execution_evidence(state, intensity):
    require(finite_json(state), 'DDGI capture telemetry contains nonfinite values')
    require(not state['taa']['enabled'] and state['msaaSamples'] == 1, 'DDGI capture changed its fixed temporal/MSAA path')
    require(np.allclose(state['camera']['position'], [0, 0, 4], atol=1e-6, rtol=0), 'DDGI camera moved')
    lighting = state['lighting']
    require(lighting['environmentIntensity'] == 0 and lighting['directionalIntensity'] == 0 and
            lighting['bounceIntensity'] == 0 and lighting['pointLightCount'] == 1 and lighting['areaLightCount'] == 0,
            'DDGI witness lost isolated authored-light defaults')
    ray, gi = state['rayShadows'], state['ddgi']
    require(ray['supported'] and ray['active'] and ray['mode'] == 1 and not ray['denoise'], 'Hard ray visibility did not execute')
    build = ray['build']
    require(build['instances'] == 3 and build['blasBuilt']+build['blasReused'] >= 3 and
            build['asAllocatedBytes'] > 0 and build['inputAllocatedBytes'] > 0, 'DDGI scene has no complete positive TLAS/BLAS build')
    require(gi['supported'] and gi['enabled'] and gi['active'] and not gi['paused'], 'DDGI is requested but inactive')
    settings = gi['settings']
    require(settings['counts'] == COUNTS and np.allclose(settings['origin'], ORIGIN, atol=1e-6, rtol=0) and
            np.allclose(settings['spacing'], SPACING, atol=1e-6, rtol=0), 'Explicit DDGI lattice changed')
    require(settings['raysPerProbe'] == RAYS and settings['maxProbesPerFrame'] == PROBES and
            settings['maxRaysPerFrame'] == RAY_BUDGET and settings['intensity'] == intensity,
            'DDGI workload or GI intensity differs from its frozen plan')
    require(not settings['asyncCompute'] and not settings['cameraFollow'] and not gi['asyncActive'],
            'DDGI witness must use the serialized fixed-volume publication path')
    require(not gi['sceneFit']['applied'], 'Manual lattice was misreported as an automatic scene fit')
    require(gi['updatedProbes'] == PROBES and gi['tracedRays'] == RAY_BUDGET and gi['updateFrame'] >= 32 and
            gi['refreshRemainingProbes'] == 0 and sorted(gi['selectedProbes']) == list(range(PROBES)),
            'DDGI did not trace, update and commit every planned probe')
    passes = {entry['name']: entry for entry in state['passes']}
    times = {}
    for name in ('RayShadowTrace', 'DdgiTrace', 'DdgiUpdate'):
        require(name in passes and passes[name]['active'] and passes[name]['gpuTimed'] and passes[name]['gpuMs'] > 0,
                'Requested DDGI/ray work has no active GPU timestamp: '+name)
        times[name] = passes[name]['gpuMs']
    return dict(instances=build['instances'], blasBuiltOrReused=build['blasBuilt']+build['blasReused'],
        updatedProbes=gi['updatedProbes'], tracedRays=gi['tracedRays'], committedUpdateFrame=gi['updateFrame'],
        refreshRemainingProbes=gi['refreshRemainingProbes'], selectedProbes=gi['selectedProbes'],
        intensity=intensity, activeGpuPassMilliseconds=times, automaticFitClaimed=False,
        publicationWitness='serialized DdgiUpdate and committed history observed by completed capture',
        gpuAtlasReadbackPerformed=False)


def capture(renderer, output, requested, document, technique, timeout, report):
    name = technique+'-'+requested['name']
    image = output/'captures'/(name+'.png')
    image.parent.mkdir(parents=True, exist_ok=True)
    telemetry, log = image.with_suffix('.telemetry.json'), image.with_suffix('.log')
    image.unlink(missing_ok=True); telemetry.unlink(missing_ok=True)
    source = Path(document['fixtures'][requested['fixture']]['path'])
    command = [str(renderer), '--hidden', '--no-ui', '--validation', '--no-bloom', '--no-taa',
        '--msaa', '1', '--width', '640', '--height', '360', '--fixed-dt', '.016666667',
        '--warmup-frames', str(WARMUP), '--capture-frame', str(CAPTURE_FRAME),
        '--render-technique', technique, '--display-mode', 'lit', '--exposure', '.35',
        '--camera-position', '0', '0', '4', '--camera-target', '0', '0', '0', '--camera-fov', '45',
        '--environment-intensity', '0', '--directional-intensity', '0',
        '--ray-shadows', 'hard', '--no-ray-shadow-denoise', '--ddgi',
        '--ddgi-counts', *map(str, COUNTS), '--ddgi-origin', *map(str, ORIGIN),
        '--ddgi-spacing', *map(str, SPACING), '--ddgi-rays', str(RAYS),
        '--ddgi-probes-per-frame', str(PROBES), '--ddgi-ray-budget', str(RAY_BUDGET),
        '--ddgi-hysteresis', '.9', '--ddgi-intensity', str(requested['intensity']),
        '--ddgi-normal-bias', '.02', '--ddgi-view-bias', '.01', '--ddgi-max-distance', '8',
        '--no-ddgi-async', '--no-ddgi-camera-follow', '--no-ddgi-thin-glass', '--no-ddgi-dielectric',
        '--model', str(source), '--screenshot', str(image)]
    attempt = dict(name=requested['name'], command=command, log=str(log), status='launched')
    report['attempts'].append(attempt)
    try:
        with log.open('w', encoding='utf-8') as stream:
            process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                timeout=timeout, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        attempt.update(returnCode=process.returncode, status='process-completed')
    except Exception as error:
        attempt.update(status='process-failed', failure=str(error))
        raise
    text = log.read_text(encoding='utf-8', errors='replace')
    require(process.returncode == 0, 'DDGI native process failed: '+str(log))
    require(not shared.validation_log_error(text), 'Vulkan validation failed: '+str(log))
    require(image.is_file() and telemetry.is_file(), 'Missing fresh DDGI image/telemetry: '+str(log))
    state = json.loads(telemetry.read_text(encoding='utf-8'))
    require(state['technique'] == technique, 'Wrong DDGI raster technique')
    evidence = execution_evidence(state, requested['intensity'])
    record = dict(name=requested['name'], technique=technique, image=str(image), telemetry=str(telemetry),
        log=str(log), command=command, imageSha256=shared.sha(image), telemetrySha256=shared.sha(telemetry),
        logSha256=shared.sha(log), validationClean=True)
    report['captures'].append(record)
    report['executionChecks'].append(dict(name=requested['name'], **evidence))
    attempt['status'] = 'validated'
    return record, state


def self_check():
    with tempfile.TemporaryDirectory(prefix='container-pbr92-ddgi-') as directory:
        document = plan(Path(directory))
        generated = generate(document)
        require(len(generated) == 7 and len(document['captures']) == 8 and len(document['checks']) == 4,
                'DDGI bounded cohort changed')
    state = dict(taa=dict(enabled=False), msaaSamples=1, camera=dict(position=[0, 0, 4]),
        lighting=dict(environmentIntensity=0, directionalIntensity=0, bounceIntensity=0, pointLightCount=1, areaLightCount=0),
        rayShadows=dict(supported=True, active=True, mode=1, denoise=False,
            build=dict(instances=3, blasBuilt=3, blasReused=0, asAllocatedBytes=1, inputAllocatedBytes=1)),
        ddgi=dict(supported=True, enabled=True, active=True, paused=False, asyncActive=False,
            sceneFit=dict(applied=False), updatedProbes=27, tracedRays=216, updateFrame=64,
            refreshRemainingProbes=0, selectedProbes=list(range(27)), settings=dict(
                counts=COUNTS, origin=ORIGIN, spacing=SPACING, raysPerProbe=RAYS,
                maxProbesPerFrame=27, maxRaysPerFrame=216, intensity=4., asyncCompute=False, cameraFollow=False)),
        passes=[dict(name=name, active=True, gpuTimed=True, gpuMs=.01)
                for name in ('RayShadowTrace', 'DdgiTrace', 'DdgiUpdate')])
    execution_evidence(state, 4.)
    mutations = [('tracedRays', 0), ('updatedProbes', 0), ('updateFrame', 0), ('refreshRemainingProbes', 27)]
    for field, value in mutations:
        bad = copy.deepcopy(state); bad['ddgi'][field] = value
        rejected = False
        try:
            execution_evidence(bad, 4.)
        except RuntimeError:
            rejected = True
        require(rejected, 'Execution witness accepted missing work: '+field)
    bad = copy.deepcopy(state); bad['passes'][-1]['gpuTimed'] = False
    rejected = False
    try:
        execution_evidence(bad, 4.)
    except RuntimeError:
        rejected = True
    require(rejected, 'Execution witness accepted an unmeasured GPU update pass')
    return dict(pureSelfCheck=True, nativeValidationPerformed=False, generatedFixtureCount=7,
        plannedCaptures=8, plannedImageChecks=4, plannedExecutionChecks=8,
        missingExecutionMutationsRejected=5, probeVolumePlacement='explicit3x3x3; automatic fit not claimed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--technique', choices=TECHNIQUES, default='deferred-raster')
    parser.add_argument('--timeout', type=float, default=300)
    parser.add_argument('--generate-only', action='store_true')
    parser.add_argument('--self-check', action='store_true')
    args = parser.parse_args()
    dependencies = dependency_hashes()
    if args.self_check:
        print(json.dumps(self_check(), indent=2)); return
    if args.renderer is None or args.output is None:
        parser.error('--renderer and --output are required unless --self-check is selected')
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    document = plan(output)
    report = dict(schemaVersion=1, passed=None if args.generate_only else False,
        generatedOnly=args.generate_only, nativeValidationPerformed=False, renderer=str(renderer),
        rendererSha256=None, technique=args.technique, sourceDependencies=dependencies,
        captures=[], attempts=[], imageChecks=[], executionChecks=[], fixtures=[],
        expectedCaptureCount=8, expectedImageCheckCount=4, expectedExecutionCheckCount=8,
        unverified=['No probe-atlas GPU readback or path-traced physical ground truth is claimed.',
            'Eight rays/probe and a coarse explicit27-probe lattice are a bounded execution/equivalence test, not a converged GI reference.',
            'GIoff retains actual probe updates at intensity0; the full witness still requires a nonvacuous intensity4 response.',
            'Anisotropic direct/IBL and raster half-float material terms retain their declared approximations.'])

    def persist():
        report.update(captureCount=len(report['captures']), imageCheckCount=len(report['imageChecks']),
                      executionCheckCount=len(report['executionChecks']))
        (output/'results.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')

    try:
        report['fixtures'] = generate(document)
        (output/'launch-plan.json').write_text(json.dumps(document, indent=2)+'\n', encoding='utf-8')
        report['launchPlanSha256'] = shared.sha(output/'launch-plan.json')
        persist()
        if args.generate_only:
            return
        require(renderer.is_file() and args.manifest is not None, 'Native DDGI work requires renderer and frozen manifest')
        report['nativeManifestBefore'] = verify_manifest(args.manifest)
        frozen = json.loads(args.manifest.read_text(encoding='utf-8-sig'))['files']
        require(any(Path(name).resolve() == renderer for name in frozen), 'Freeze manifest does not bind this renderer')
        require({'ddgi_trace.comp.spv', 'ddgi_update.comp.spv', 'ray_shadow_trace.comp.spv'} <=
                {Path(name).name for name in frozen}, 'Freeze manifest omits required ray/DDGI consumers')
        report['rendererSha256'] = shared.sha(renderer)
        (output/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n', encoding='utf-8')
        captures, states = {}, {}
        for requested in document['captures']:
            captures[requested['name']], states[requested['name']] = capture(
                renderer, output, requested, document, args.technique, args.timeout, report)
            persist()
        for expected in document['checks']:
            if expected['kind'] == 'near':
                left, right = [states[name]['ddgi'] for name in expected['images']]
                require(left['updateFrame'] == right['updateFrame'] and left['selectedProbes'] == right['selectedProbes'],
                        'Equivalent controls used different stochastic update frames/probe schedules')
            result = shared.compare(expected, captures, document['thresholds'])
            report['imageChecks'].append(shared.check_record(args.technique, expected, result))
            persist()
        require(len(report['captures']) == 8 and len(report['executionChecks']) == 8 and len(report['imageChecks']) == 4,
                'Bounded DDGI cohort is incomplete')
        require(all(shared.sha(Path(path)) == digest for path, digest in dependencies.items()), 'Frozen harness dependency changed')
        require(all(shared.sha(Path(entry['path'])) == entry['sha256'] for entry in report['fixtures']), 'DDGI fixture changed')
        require(shared.sha(output/'launch-plan.json') == report['launchPlanSha256'] and
                shared.sha(renderer) == report['rendererSha256'], 'Frozen plan or renderer changed')
        report['nativeManifestAfter'] = verify_manifest(args.manifest)
        report['nativeValidationPerformed'] = True
        report['passed'] = True
    except Exception as error:
        report['failure'] = str(error)
        if report.get('nativeManifestBefore') and args.manifest:
            try:
                report['nativeManifestAfter'] = verify_manifest(args.manifest)
            except Exception as manifest_error:
                report['manifestAfterFailure'] = str(manifest_error)
        raise
    finally:
        persist()
        print('GENERATED' if args.generate_only else 'NATIVE', report['captureCount'], 'captures,',
              report['imageCheckCount'], 'image checks,', report['executionCheckCount'], 'execution checks', flush=True)


if __name__ == '__main__':
    main()
