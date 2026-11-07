"""Raw scene convergence and matched raster/reference calibration for #47.

--self-check and --generate-only do not launch a renderer or use a GPU. Native
execution requires an explicit frozen runtime/helper/layer manifest. Defaults:
five independent generated scenes, four low seeds at 32/128/512 spp, two
different 4096-spp reference seeds, raw forward/deferred views and three matched
display views: 40 processes, 95 captures at 256x192. No production test API.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import tempfile
import time
import traceback

os.environ.setdefault('OPENCV_IO_ENABLE_OPENEXR', '1')
import cv2
import numpy as np
from PIL import Image

import reference_scene_convergence_fixtures as fixtures

WIDTH, HEIGHT = 256, 192
CHECKPOINTS = (32, 128, 512)
LOW_SEEDS = (23, 1543, 7919, 32413)
REFERENCE_SEEDS = (104729, 130363)
REFERENCE_SAMPLES, BATCH, BOUNCES = 4096, 64, 8
NAMES = ('cornell', 'area', 'blocker', 'roughness', 'interior')
EXPOSURE, TONE_MAPPER = .5, 1  # Reinhard, explicit manual exposure.
# Declared before native work. Empirical paired-seed noise envelopes, not a
# pixelwise monotonicity rule or a rigorous confidence guarantee for heavy tails.
THRESHOLDS = dict(pairedSeedStandardErrors=4.541, finalToInitialMse=.55,
    finalToInitialReplicateVariance=.4, referenceFloorAllowance=4.,
    maximumReferenceNoiseToFinalReplicateVariance=.5,
    maximumFinalNormalizedRmse=.18, maximumReferenceNormalizedNoise=.04)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2, allow_nan=False)+'\n', encoding='utf-8')


def verify_manifest(path, required=(), *, identity=None):
    require(identity is None or sha(path) == identity, 'Freeze manifest itself changed')
    document = json.loads(Path(path).read_text(encoding='utf-8-sig'))
    files = document.get('files', {})
    require(isinstance(files, dict) and files, 'Freeze manifest needs a nonempty files map')
    frozen = {Path(name).resolve(): expected.lower() for name, expected in files.items()}
    require(set(map(Path.resolve, required)) <= frozen.keys(), 'Freeze manifest omits a consumed native/helper/layer input')
    for file, expected in frozen.items():
        require(file.is_file() and sha(file) == expected, f'Frozen input changed: {file}')
    return dict(path=str(Path(path).resolve()), sha256=sha(path), entryCount=len(frozen), verified=True)


def dependencies():
    # Fixture serialization is independent of renderer code. Record its helper
    # along with both new modules; installed numerical/image tools are versioned.
    return [Path(__file__).resolve(), Path(fixtures.__file__).resolve(),
            Path(sys.modules[fixtures.Asset.__module__].__file__).resolve()]


def finite_image(image, shape=(HEIGHT, WIDTH, 3)):
    image = np.asarray(image)
    require(image.shape == shape and np.isfinite(image).all(), 'Image has wrong dimensions or nonfinite pixels')
    require(np.min(image) >= -1e-6, 'Raw radiance has a negative component')
    return image.astype(np.float64)


def convergence(low, references):
    """Independent-seed image errors with an explicitly measured noisy target.

    Reference sample variance / reference replicate count estimates variance of
    the reference mean. Signed excess MSE is preserved even when it is negative;
    no negative noise-corrected value is silently clamped to zero. Seed-paired
    MSE differences give a measured uncertainty envelope for rising checkpoints.
    """
    require(len(low) == len(CHECKPOINTS) and len(references) == 2, 'Expected three checkpoints and two high references')
    reference_stack = np.asarray([finite_image(a) for a in references])
    reference = reference_stack.mean(axis=0)
    scale = float(np.sqrt(np.mean(reference**2)))
    require(scale > 1e-5, 'Reference scene is black or has insufficient radiance')
    reference_variance = float(np.var(reference_stack, axis=0, ddof=1).mean()/len(references))
    require(reference_variance > 0, 'Independent high references are identical; no stochastic witness')
    result = []
    previous_mses = None
    for count, images in zip(CHECKPOINTS, low, strict=True):
        require(len(images) == len(LOW_SEEDS), 'Missing independent low-seed replicate')
        stack = np.asarray([finite_image(a) for a in images])
        mses = np.mean((stack-reference)**2, axis=(1, 2, 3))
        mse = float(mses.mean())
        sem = float(mses.std(ddof=1)/math.sqrt(len(mses)))
        variance = float(np.var(stack, axis=0, ddof=1).mean())
        require(variance > scale*scale*1e-12, 'Low seeds have no independent stochastic variation')
        row = dict(samples=count, perSeedMse=mses.tolist(), aggregateMse=mse,
            aggregateRmse=math.sqrt(mse), normalizedRmse=math.sqrt(mse)/scale,
            mseStandardErrorAcrossSeeds=sem, replicateVariance=variance,
            estimatedReferenceMeanVariance=reference_variance,
            signedExcessMseOverReferenceFloor=mse-reference_variance,
            referenceFloorDominated=bool(mse <= reference_variance+THRESHOLDS['pairedSeedStandardErrors']*sem))
        if previous_mses is not None:
            paired = mses-previous_mses
            change, change_sem = float(paired.mean()), float(paired.std(ddof=1)/math.sqrt(len(paired)))
            upper = THRESHOLDS['pairedSeedStandardErrors']*change_sem
            require(change <= upper, f'Scene error rises beyond declared paired-seed envelope at {count} spp')
            row['pairedMseChange'] = change
            row['pairedMseChangeStandardError'] = change_sem
            row['allowedPositiveMseChange'] = upper
        result.append(row)
        previous_mses = mses
    first, final = result[0], result[-1]
    require(first['aggregateMse'] > 4*reference_variance, 'Reference floor dominates initial error; inconclusive convergence fixture')
    require(final['aggregateMse'] <= THRESHOLDS['finalToInitialMse']*first['aggregateMse']+
            THRESHOLDS['referenceFloorAllowance']*reference_variance, 'No aggregate image error reduction')
    require(final['replicateVariance'] < THRESHOLDS['finalToInitialReplicateVariance']*first['replicateVariance'],
            'Independent-seed variance fails to decrease')
    require(reference_variance <= THRESHOLDS['maximumReferenceNoiseToFinalReplicateVariance']*final['replicateVariance'],
            'High reference uncertainty is too large for final low-spp estimate')
    require(final['normalizedRmse'] <= THRESHOLDS['maximumFinalNormalizedRmse'], 'Final raw image noise exceeds declared tolerance')
    require(math.sqrt(reference_variance)/scale <= THRESHOLDS['maximumReferenceNormalizedNoise'],
            'High-spp reference noise exceeds declared tolerance')
    return dict(passed=True, referenceSeeds=list(REFERENCE_SEEDS), referenceSamplesPerSeed=REFERENCE_SAMPLES,
        referenceMeanEffectiveSamples=REFERENCE_SAMPLES*len(references),
        referenceMeanRadianceRms=scale, estimatedReferenceMeanVariance=reference_variance,
        estimatedReferenceMeanNoiseRmse=math.sqrt(reference_variance), checkpoints=result,
        interpretation='Empirical image/seed statistics; finite noisy reference, no pixelwise monotonicity or formal confidence claim.')


def check_reference(state, metadata, count, seed, *, exact):
    path = state.get('referencePath', {})
    require(state['technique'] == 'path-tracing' and path.get('active'), 'Reference backend is inactive')
    require(path['attemptedSamples'] == count and path['seed'] == seed, 'Wrong reference sample count/seed')
    require(path['maxBounces'] == BOUNCES and path['samplesPerFrame'] == BATCH and path['historyResets'] == 1,
            'Reference bounce/batch/reset controls differ from plan')
    require(path['unsupportedSamples'] == path['rejectedSamples'] == 0, 'Reference path diagnostics are nonzero')
    require(path['allocatedBytes'] >= WIDTH*HEIGHT*16*3, 'Missing owned reference image allocations')
    require(path['fireflyClamp'] == 0 and path['display'] == 'beauty', 'Raw comparison has a clamp or diagnostic display')
    require(path['oidn']['quality'] == 'off' and not path['optix']['requested'] and not path['dlss']['active'],
            'Reference display filtering/reconstruction entered calibration')
    require(metadata['referencePath']['attemptedSamplesPerPixel'] == count, 'Capture count and telemetry differ')
    capture = metadata['referencePath']
    require(capture['seed'] == seed and capture['maxBounces'] == BOUNCES and capture['samplesPerFrame'] == BATCH,
            'Capture sampling metadata differs from actual plan')
    if exact:
        capture = metadata['referencePath']
        require(capture['acceptedSamplesPerPixel'] == capture['requestedSamplesPerPixel'] == count,
                'Exact sample export has incomplete accepted counts')
        require(not capture['hasDiagnostics'], 'Exact sample capture has diagnostics')
    lighting = next((p['gpuMs'] for p in state['passes'] if p['name'] == 'Lighting'), None)
    require(lighting is None or (math.isfinite(lighting) and lighting >= 0), 'Invalid GPU timestamp')
    return dict(attemptedSamples=count, acceptedSamplesPerPixel=count if exact else None,
        acceptedCountProof='Exact export checks every pixel' if exact else 'Telemetry has zero path faults; exact raw checkpoints checked separately',
        ownedReferenceBytes=path['allocatedBytes'], lightingGpuMs=lighting if lighting else None,
        totalInstrumentedGpuMs=state['gpuKnownMs'],
        borrowedAsBytes=state.get('rayShadows', {}).get('build', {}).get('asAllocatedBytes'))


def camera_and_display(samples, scene):
    expected = np.asarray(scene['cameraPosition'], dtype=float)
    first = samples[0]['state']['camera']
    matrix = np.asarray(first['unjitteredViewProjColumns'])
    require(matrix.shape == (4, 4) and np.isfinite(matrix).all(), 'Invalid comparison camera matrix')
    display_color_space = None
    for sample in samples:
        state, metadata = sample['state'], sample['metadata']
        require(state['resolution'] == [WIDTH, HEIGHT] and state['msaaSamples'] == 1, 'Comparison resolution/MSAA changed')
        require(not state['taa']['enabled'], 'TAA entered comparison')
        for backend in ('fsr', 'dlss'):
            require(not state[backend]['active'] and state[backend]['mode'] == state[backend]['requested'] == 'native',
                    'Upscaling entered comparison')
        passes = {p['name']:p['active'] for p in state['passes']}
        require(all(name in passes and not passes[name] for name in ('Bloom', 'TemporalResolve', 'TemporalVelocity')),
                'Display filtering entered comparison')
        np.testing.assert_allclose(state['camera']['position'], expected, rtol=0, atol=1e-5)
        np.testing.assert_allclose(state['camera']['unjitteredViewProjColumns'], matrix, rtol=0, atol=1e-6)
        require(all(abs(x) <= 1e-8 for x in state['camera']['jitterUv']), 'Comparison camera is jittered')
        require(not metadata['includesUi'], 'Comparison includes UI')
        if sample['image'].suffix == '.png':
            require(metadata['source'] == 'viewport' and metadata['colorSpace'] in ('sRGB', 'linear-sRGB'),
                    'Display comparison is not a declared viewport color space')
            if display_color_space is None: display_color_space=metadata['colorSpace']
            require(metadata['colorSpace'] == display_color_space, 'Display color spaces differ')
            require(metadata['exposureMode'] == 'manual' and metadata['manualExposure'] == EXPOSURE,
                    'Display exposure does not match')
            require(metadata['toneMapper'] == TONE_MAPPER and metadata['displayMode'] == 0,
                    'Display transform does not match Reinhard/lit')
    return dict(matched=True, cameraPosition=expected.tolist(), unjitteredViewProjColumns=matrix.tolist(),
        manualExposure=EXPOSURE, toneMapper='reinhard', bloom=False, taa=False, msaa=1,
        width=WIDTH, height=HEIGHT, displayColorSpace=display_color_space, rasterPhysicalPixelEquivalenceRequired=False)


def layer_proof(log, library):
    paths = re.findall(r'Insert instance layer "VK_LAYER_KHRONOS_validation" \(([^\r\n]+)\)', log)
    require(any(Path(p).resolve() == library.resolve() for p in paths), 'Loader log does not prove expected validation layer was inserted')
    require(not re.search(r'VUID-|SYNC-HAZARD|Validation Error|\bhazard detected\b|'
                         r'\b(?:READ|WRITE)_AFTER_(?:READ|WRITE)\b[^\r\n]*\bhazard\b', log, re.I),
            'Strict Vulkan validation/synchronization diagnostics')
    return dict(insertedLibrary=str(library.resolve()), loaderLines=[line for line in log.splitlines()
        if 'Insert instance layer "VK_LAYER_KHRONOS_validation"' in line])


def command(renderer, model, scene):
    return [str(renderer), '--hidden', '--validation', '--no-ui', '--screenshot-viewport',
        '--width', str(WIDTH), '--height', str(HEIGHT), '--msaa', '1', '--fixed-dt', '.016666667',
        '--model', str(model), '--import-scale', '1', '--render-technique', 'path-tracing',
        '--camera-position', *map(str, scene['cameraPosition']), '--camera-target', *map(str, scene['cameraTarget']),
        '--camera-fov', str(scene['cameraFov']), '--sky', 'plain', '--sky-horizon', '0', '0', '0',
        '--environment-intensity', '0', '--directional-intensity', '0', '--no-ddgi', '--no-bloom', '--no-taa',
        '--screen-reflections', 'off', '--ray-shadows', 'hard', '--area-lighting', 'ltc',
        '--fsr', 'native', '--dlss', 'native', '--display-mode', 'lit', '--exposure', str(EXPOSURE),
        '--tone-map', 'reinhard', '--path-oidn', 'off', '--path-firefly-clamp', '0',
        '--path-spp', str(BATCH), '--path-bounces', str(BOUNCES)]


def plan(output, scenes, renderer):
    runs = []
    for name, scene in scenes.items():
        model = Path(scene['path'])
        for role, seeds, counts in (('low', LOW_SEEDS, CHECKPOINTS),
                                   ('high', REFERENCE_SEEDS, (REFERENCE_SAMPLES,))):
            for seed in seeds:
                label = f'{name}-{role}-{seed}'
                base = output/(label+'.exr')
                paths = [base.with_name(base.stem+f'.samples-{n:08}.exr') for n in counts]
                runs.append(dict(name=label, scene=name, role=role, seed=seed, counts=list(counts),
                    images=list(map(str, paths)), command=command(renderer, model, scene)+[
                        '--path-seed', str(seed), '--path-sample-series', ','.join(map(str, counts)), '--screenshot', str(base)]))
        for role, extension, techniques in (('raster', '.exr', ('forward-raster', 'deferred-raster')),
                                             ('display', '.png', ('path-tracing', 'forward-raster', 'deferred-raster'))):
            label = f'{name}-{role}'
            first = CHECKPOINTS[-1]//BATCH if role == 'display' else 2
            frames = [first+2*i for i in range(len(techniques))]
            events = [dict(frame=f-1, technique=t) for f, t in zip(frames, techniques) if t != 'path-tracing']
            sequence = output/(label+'.sequence.json')
            write(sequence, dict(schemaVersion=1, frames=frames[-1], sampleFrames=frames, events=events))
            base = output/(label+extension)
            images = [output/(label+(f'.frame-{frame:04d}' if frame != frames[-1] else '')+extension) for frame in frames]
            runs.append(dict(name=label, scene=name, role=role, seed=LOW_SEEDS[0], frames=frames,
                techniques=list(techniques), images=list(map(str, images)),
                command=command(renderer, model, scene)+['--path-seed', str(LOW_SEEDS[0]),
                    '--path-samples', str(CHECKPOINTS[-1]), '--capture-sequence', str(sequence), '--screenshot', str(base)]))
    return dict(processCount=len(runs), captureCount=sum(len(r['images']) for r in runs), runs=runs)


def read_capture(path):
    path = Path(path)
    state = json.loads(path.with_suffix('.telemetry.json').read_text(encoding='utf-8'))
    metadata = json.loads(Path(str(path)+'.capture.json').read_text(encoding='utf-8'))
    if path.suffix == '.exr':
        pixels = cv2.imdecode(np.frombuffer(path.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
        require(pixels is not None and pixels.dtype == np.float32, 'EXR must decode as native float32')
        pixels = finite_image(pixels[:, :, ::-1])
        require(metadata['colorSpace'] == 'linear-sRGB' and not metadata['exposureApplied'] and
                not metadata['toneMappingApplied'], 'EXR includes display transforms')
    else:
        with Image.open(path) as image:
            pixels = np.asarray(image.convert('RGB'))
        require(pixels.shape == (HEIGHT, WIDTH, 3), 'PNG dimensions changed')
    return dict(image=path, state=state, metadata=metadata, pixels=pixels)


def analyze(output, scenes, inventory):
    report = {}
    for name, scene in scenes.items():
        low = [[] for _ in CHECKPOINTS]
        references, comparison, display, costs, all_captures = [], [], [], [], []
        for run in (r for r in inventory['runs'] if r['scene'] == name):
            samples = [read_capture(p) for p in run['images']]
            all_captures.extend(samples)
            require(all(s['state']['lighting']['areaLightCount'] == scene['expectedAreaLights'] for s in samples),
                    'Generated area lights did not reach the actual renderer')
            if run['role'] in ('low', 'high'):
                for index, (count, sample) in enumerate(zip(run['counts'], samples, strict=True)):
                    require(sample['metadata']['source'] == 'path-trace-mean', 'Reference is not raw mean')
                    costs.append(dict(image=str(sample['image']), **check_reference(
                        sample['state'], sample['metadata'], count, run['seed'], exact=True)))
                    if run['role'] == 'low': low[index].append(sample['pixels'])
                    else: references.append(sample['pixels'])
            else:
                require([s['state']['technique'] for s in samples] == run['techniques'], 'Comparison techniques differ from plan')
                if run['role'] == 'display':
                    check_reference(samples[0]['state'], samples[0]['metadata'], CHECKPOINTS[-1], LOW_SEEDS[0], exact=False)
                    display = samples
                else:
                    require(all(s['metadata']['source'] == 'raster-scene-composite' for s in samples), 'Raster raw source mismatch')
                    comparison = samples
        stats = convergence(low, references)
        state_match = camera_and_display(all_captures, scene)
        reference = np.mean(references, axis=0)
        raw_differences = [dict(technique=s['state']['technique'], image=str(s['image']),
            rmse=float(np.sqrt(np.mean((s['pixels']-reference)**2))),
            normalizedRmse=float(np.sqrt(np.mean((s['pixels']-reference)**2)))/stats['referenceMeanRadianceRms']) for s in comparison]
        # Save actual display views side by side. Pixel equality is deliberately
        # not an oracle for raster approximations versus multibounce transport.
        contact = Image.new('RGB', (WIDTH*3, HEIGHT+20))
        from PIL import ImageDraw
        draw = ImageDraw.Draw(contact)
        for i, sample in enumerate(display):
            contact.paste(Image.fromarray(sample['pixels']), (i*WIDTH, 20))
            draw.text((i*WIDTH+2, 2), sample['state']['technique'], fill='white')
        contact.save(output/(name+'-comparison.png'))
        require(any(c['lightingGpuMs'] is not None for c in costs), 'Missing measured reference GPU timing')
        report[name] = dict(convergence=stats, matchedComparisonState=state_match,
            rasterAgainstRawReference=raw_differences, costs=costs,
            timingScope='Latest retired reference Lighting GPU dispatch and instrumented pass sum; an initial unavailable timestamp is null, not zero-cost. Excludes CPU extraction/AS build/end-to-end latency.',
            memoryScope='Owned reference allocations; borrowed scene/AS and driver allocations excluded unless separately identified.',
            approximationPolicy='Raster uses LTC, hard ray visibility, DDGI/reflections off; indirect lighting/area visibility/BRDF differences are measured, not required to match.')
    return report


def calibration_guide(output, scene_results):
    lines = ['# Raw reference convergence and raster calibration', '',
        'Use each generated scene with the camera/lighting/units in fixtures.json. ',
        'The receipt records raw 32/128/512-spp images for four seeds and the mean of two independent 4096-spp references.',
        'Reported reference noise is measured from the two high-seed images; signed noise-corrected MSE is retained, including negative or floor-dominated values.', '',
        'The comparison PNGs come from actual engine views with manual exposure 0.5, Reinhard, no bloom/TAA/MSAA, and identical unjittered cameras. ',
        'Raw EXRs bypass display transforms. Raster pixels are not required to equal multibounce reference pixels.', '',
        'For LTC, inspect the isolated area/roughness scenes; distinguish fit error from raw-reference seed noise. ',
        'For shadows, inspect blocker penumbrae: raster hard visibility and finite-area path visibility deliberately differ. ',
        'For GI, use the Cornell/interior raw target, enable DDGI in a separately frozen matching raster configuration, ',
        'and measure indirect-region error after probe convergence; this baseline keeps DDGI off and does not claim DDGI quality acceptance. ',
        'For reflections, repeat the roughness/occlusion camera with the desired SSR/ray fallback mode; mask unsupported/out-of-screen cases ',
        'and compare against a noise-converged raw reference, not against one noisy low-spp frame. ',
        'Do not tune production approximation thresholds to a single seed. Standard-error view estimates noise, not model bias.', '',
        '| Scene | RMSE32 | RMSE128 | RMSE512 | Measured target noise |', '|---|---:|---:|---:|---:|']
    for name, item in scene_results.items():
        stats = item['convergence']
        values = [r['aggregateRmse'] for r in stats['checkpoints']]
        lines += [f'| {name} | '+ ' | '.join(f'{v:.7g}' for v in values+[stats['estimatedReferenceMeanNoiseRmse']])+' |',
                  f'\n![{name} matched views]({name}-comparison.png)\n']
    (output/'CALIBRATION.md').write_text('\n'.join(lines)+'\n', encoding='utf-8')


def self_check():
    scratch = Path(__file__).resolve().parents[2]/'build'/'codex-gltf-validation'
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='reference-convergence-', dir=scratch) as temporary:
        require(Path(temporary).resolve().is_relative_to(scratch.resolve()), 'Self-check scratch escaped workspace')
        output = Path(temporary)
        fixture_receipt = fixtures.self_check(output/'fixtures')
        rng = np.random.default_rng(470121)
        truth = np.full((HEIGHT, WIDTH, 3), 2.)
        # Independent noise fields with nested prefixes at increasing spp.
        fields = rng.normal(size=(len(LOW_SEEDS), CHECKPOINTS[-1], 8, 8, 3))
        # Expand small independent tiles to the native dimensions to keep this
        # fixture inexpensive; the statistics checker still sees native shapes.
        low = [[np.tile(truth[:8, :8]+fields[k, :n].mean(axis=0), (HEIGHT//8, WIDTH//8, 1))
                for k in range(len(LOW_SEEDS))] for n in CHECKPOINTS]
        high = [truth+rng.normal(size=truth.shape)/math.sqrt(REFERENCE_SAMPLES) for _ in REFERENCE_SEEDS]
        good = convergence(low, high)
        rejected = []
        def negative(name, callback):
            try: callback()
            except (RuntimeError, AssertionError, ValueError, KeyError): rejected.append(name)
            else: raise RuntimeError('Oracle accepted corruption: '+name)
        bad = copy.deepcopy(low); bad[2] = [a+1 for a in bad[2]]
        negative('nonconverging-bias', lambda: convergence(bad, high))
        bad = copy.deepcopy(low); bad[1][0][0, 0, 0] = np.nan
        negative('nonfinite-HDR', lambda: convergence(bad, high))
        negative('duplicated-low-seeds', lambda: convergence([[a[0]]*4 for a in low], high))
        negative('identical-high-reference-seeds', lambda: convergence(low, [high[0], high[0]]))
        negative('no-spp-variance-improvement', lambda: convergence([low[0]]*3, high))
        noisy = [truth+rng.normal(size=truth.shape) for _ in REFERENCE_SEEDS]
        negative('inadequate-high-reference', lambda: convergence(low, noisy))
        disabled_passes = [dict(name=name, active=False) for name in ('Bloom','TemporalResolve','TemporalVelocity')]
        native_upscale = dict(active=False, mode='native', requested='native')
        state = dict(technique='path-tracing', resolution=[WIDTH, HEIGHT], msaaSamples=1,
            taa=dict(enabled=False), fsr=copy.deepcopy(native_upscale), dlss=copy.deepcopy(native_upscale), passes=disabled_passes,
            camera=dict(position=[0,0,4], unjitteredViewProjColumns=np.eye(4).tolist(), jitterUv=[0]*4))
        metadata = dict(includesUi=False, source='viewport', colorSpace='sRGB', exposureMode='manual', manualExposure=EXPOSURE,
                        toneMapper=TONE_MAPPER, displayMode=0)
        samples = [dict(image=Path('view.png'), state=copy.deepcopy(state), metadata=copy.deepcopy(metadata)) for _ in range(3)]
        scene = dict(cameraPosition=[0,0,4])
        camera_and_display(samples, scene)
        for name, mutate in (
            ('camera-mismatch', lambda x:x[1]['state']['camera']['unjitteredViewProjColumns'][0].__setitem__(0, 2)),
            ('exposure-mismatch', lambda x:x[1]['metadata'].__setitem__('manualExposure', 2)),
            ('tone-map-mismatch', lambda x:x[1]['metadata'].__setitem__('toneMapper', 0)),
            ('unrequested-bloom', lambda x:x[1]['state']['passes'][0].__setitem__('active', True)),
            ('unrequested-upscaling', lambda x:x[1]['state']['fsr'].__setitem__('active', True)),
            ('temporal-jitter', lambda x:x[1]['state']['camera'].__setitem__('jitterUv', [.1,0,0,0]))):
            bad = copy.deepcopy(samples); mutate(bad)
            negative(name, lambda:camera_and_display(bad, scene))
        library = output/'VkLayer_khronos_validation.dll'
        log = 'Insert instance layer "VK_LAYER_KHRONOS_validation" ('+str(library)+')'
        layer_proof(log, library)
        negative('wrong-loaded-layer', lambda:layer_proof(log, output/'other.dll'))
        negative('strict-validation-diagnostic', lambda:layer_proof(log+'\nSYNC-HAZARD-WRITE-AFTER-WRITE', library))
        path_state = dict(technique='path-tracing', gpuKnownMs=1., passes=[dict(name='Lighting', gpuMs=.5)],
            referencePath=dict(active=True, attemptedSamples=32, seed=23, unsupportedSamples=0, rejectedSamples=0,
                maxBounces=BOUNCES, samplesPerFrame=BATCH, historyResets=1,
                allocatedBytes=WIDTH*HEIGHT*48, fireflyClamp=0, display='beauty',
                oidn=dict(quality='off'), optix=dict(requested=False), dlss=dict(active=False)))
        path_metadata = dict(referencePath=dict(attemptedSamplesPerPixel=32, requestedSamplesPerPixel=32,
            seed=23, maxBounces=BOUNCES, samplesPerFrame=BATCH,
            acceptedSamplesPerPixel=32, hasDiagnostics=False))
        check_reference(path_state, path_metadata, 32, 23, exact=True)
        for name, field, value in (('wrong-attempted-count', 'attemptedSamples', 31),
                                  ('wrong-seed', 'seed', 24), ('unsupported-path', 'unsupportedSamples', 1),
                                  ('rejected-path', 'rejectedSamples', 1), ('biased-firefly-clamp', 'fireflyClamp', 4)):
            bad = copy.deepcopy(path_state); bad['referencePath'][field] = value
            negative(name, lambda:check_reference(bad, path_metadata, 32, 23, exact=True))
        bad_metadata = copy.deepcopy(path_metadata); bad_metadata['referencePath']['acceptedSamplesPerPixel'] = 31
        negative('incomplete-accepted-count', lambda:check_reference(path_state, bad_metadata, 32, 23, exact=True))
        frozen_file = output/'frozen.txt'; frozen_file.write_text('original')
        manifest = output/'freeze.json'; write(manifest, dict(files={str(frozen_file):sha(frozen_file)}))
        verify_manifest(manifest, [frozen_file])
        negative('manifest-omitted-input', lambda:verify_manifest(manifest, [library]))
        frozen_file.write_text('changed')
        negative('manifest-changed-input', lambda:verify_manifest(manifest))
        manifest_identity=sha(manifest)
        write(manifest, dict(files={str(frozen_file):sha(frozen_file)}))
        negative('replaced-freeze-manifest', lambda:verify_manifest(manifest, identity=manifest_identity))
        generated = fixtures.generate(output/'plan')
        inventory = plan(output, generated, Path('renderer.exe'))
        require(inventory['processCount'] == 40 and inventory['captureCount'] == 95, 'Default capture inventory changed')
        # Exercise real EXR/PNG decoding, checkpoint/sequence filename mapping,
        # full five-scene analysis and artifact generation without a renderer.
        for run in inventory['runs']:
            scene_data = generated[run['scene']]
            for index, image_path in enumerate(map(Path, run['images'])):
                exact = run['role'] in ('low', 'high')
                count = run['counts'][index] if exact else CHECKPOINTS[-1]
                technique = 'path-tracing' if exact else run['techniques'][index]
                state = dict(technique=technique, resolution=[WIDTH, HEIGHT], msaaSamples=1,
                    taa=dict(enabled=False), fsr=copy.deepcopy(native_upscale), dlss=copy.deepcopy(native_upscale),
                    camera=dict(position=scene_data['cameraPosition'], unjitteredViewProjColumns=np.eye(4).tolist(), jitterUv=[0]*4),
                    lighting=dict(areaLightCount=scene_data['expectedAreaLights']),
                    gpuKnownMs=1., passes=[dict(name='Lighting', gpuMs=.5, active=True), *copy.deepcopy(disabled_passes)])
                metadata = dict(includesUi=False, colorSpace='linear-sRGB', exposureApplied=False,
                    toneMappingApplied=False, source='raster-scene-composite')
                pixels = truth
                if technique == 'path-tracing':
                    if run['role'] == 'low': pixels=low[CHECKPOINTS.index(count)][LOW_SEEDS.index(run['seed'])]
                    elif run['role'] == 'high': pixels=high[REFERENCE_SEEDS.index(run['seed'])]
                    else: pixels=low[-1][0]
                    state['referencePath'] = copy.deepcopy(path_state['referencePath'])
                    state['referencePath'].update(attemptedSamples=count, seed=run['seed'])
                    metadata['referencePath'] = dict(attemptedSamplesPerPixel=count,
                        seed=run['seed'], maxBounces=BOUNCES, samplesPerFrame=BATCH,
                        acceptedSamplesPerPixel=count, requestedSamplesPerPixel=count, hasDiagnostics=False)
                    metadata['source']='path-trace-mean'
                if image_path.suffix == '.exr':
                    ok, encoded=cv2.imencode('.exr', pixels[:, :, ::-1].astype(np.float32))
                    require(ok, 'Self-check OpenEXR encoder unavailable')
                    image_path.write_bytes(encoded.tobytes())
                else:
                    Image.fromarray(np.full((HEIGHT,WIDTH,3),127,dtype=np.uint8)).save(image_path)
                    metadata.update(source='viewport', colorSpace='sRGB', exposureMode='manual', manualExposure=EXPOSURE,
                        toneMapper=TONE_MAPPER, displayMode=0)
                write(image_path.with_suffix('.telemetry.json'), state)
                write(Path(str(image_path)+'.capture.json'), metadata)
        simulated = analyze(output, generated, inventory)
        calibration_guide(output, simulated)
        require(len(simulated) == 5 and (output/'CALIBRATION.md').is_file(), 'Full simulated capture analysis failed')
        return dict(passed=True, gpuUsed=False, fixtureChecks=fixture_receipt,
            statisticalAndComparisonNegativeChecks=rejected, processCount=40, captureCount=95,
            simulatedCapturedArtifactAnalysis=True, syntheticConvergence=good, thresholds=THRESHOLDS)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--layer-manifest', type=Path)
    parser.add_argument('--expect-layer-version', default='1.4.363')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--self-check', action='store_true')
    parser.add_argument('--generate-only', action='store_true')
    parser.add_argument('--analyze-only', action='store_true')
    parser.add_argument('--timeout', type=int, default=600)
    args = parser.parse_args()
    if args.self_check:
        receipt = self_check()
        if args.output:
            args.output.mkdir(parents=True, exist_ok=False)
            write(args.output/'self-check.json', receipt)
        print(json.dumps(receipt, indent=2)); return
    require(args.output is not None, '--output is required')
    output = args.output.resolve()
    if args.analyze_only:
        report = json.loads((output/'results.json').read_text())
        verify_manifest(Path(report['manifestBefore']['path']), identity=report['manifestBefore']['sha256'])
        require(all(sha(Path(p)) == h for p,h in report['generatedInputsSha256'].items()), 'Generated inputs changed')
        for run in report['runs']:
            require(all(sha(Path(p)) == h for p,h in run['captureArtifacts'].items()), 'Captured artifacts changed')
            layer_proof(Path(run['log']).read_text(encoding='utf-8', errors='replace'), Path(run['layer']['insertedLibrary']))
        report['scenes'] = analyze(output, json.loads((output/'fixtures.json').read_text()),
                                    json.loads((output/'launch-plan.json').read_text()))
        calibration_guide(output, report['scenes'])
        report['analysisPassed'] = True
        write(output/'results.json', report)
        print('PASS read-only capture analysis'); return
    output.mkdir(parents=True, exist_ok=False)
    scenes = fixtures.generate(output/'fixtures')
    for name, scene in scenes.items(): fixtures.validate(Path(scene['path']), name)
    inventory = plan(output, scenes, (args.renderer or Path('VulkanSceneRenderer.exe')).resolve())
    write(output/'fixtures.json', scenes)
    write(output/'launch-plan.json', inventory)
    write(output/'thresholds.json', THRESHOLDS)
    if args.generate_only:
        print(json.dumps(dict(gpuUsed=False, processCount=inventory['processCount'],
                              captureCount=inventory['captureCount'], output=str(output)), indent=2)); return
    require(args.renderer and args.renderer.is_file() and args.manifest and args.layer_manifest,
            'Native execution requires --renderer, --manifest and --layer-manifest')
    renderer, manifest, layer_manifest = args.renderer.resolve(), args.manifest.resolve(), args.layer_manifest.resolve()
    layer = json.loads(layer_manifest.read_text(encoding='utf-8'))['layer']
    require(layer['name'] == 'VK_LAYER_KHRONOS_validation' and layer['api_version'] == args.expect_layer_version,
            'Wrong validation-layer name/version')
    library = (layer_manifest.parent/layer['library_path']).resolve()
    consumed = [renderer, *renderer.parent.glob('*.dll'), *(renderer.parent/'spv_shaders').glob('*.spv'),
                *dependencies(), layer_manifest, library]
    before = verify_manifest(manifest, consumed)
    local_inputs = [output/'fixtures.json', output/'launch-plan.json', output/'thresholds.json',
                    *[Path(s['path']) for s in scenes.values()], *output.glob('*.sequence.json')]
    local_hashes = {str(p): sha(p) for p in local_inputs}
    settings = output/'vk_layer_settings.txt'
    settings.write_text('khronos_validation.validate_sync = true\nkhronos_validation.duplicate_message_limit = 1000\n', encoding='utf-8')
    local_hashes[str(settings)] = sha(settings)
    environment = dict(os.environ, VK_LAYER_PATH=str(layer_manifest.parent),
        VK_LAYER_SETTINGS_PATH=str(output), VK_LOADER_DEBUG='error,warn,info,layer,driver,debug')
    report = dict(passed=False, rendererSha256=sha(renderer), manifestBefore=before,
        generatedInputsSha256=local_hashes, thresholds=THRESHOLDS, completedProcesses=0,
        expectedProcesses=inventory['processCount'], expectedCaptures=inventory['captureCount'], runs=[],
        tools=dict(python=sys.version, numpy=np.__version__, opencv=cv2.__version__, platform=platform.platform()),
        sourceDependencies={str(p):sha(p) for p in dependencies()})
    started = time.perf_counter()
    def verify():
        verify_manifest(manifest, consumed, identity=before['sha256'])
        require(all(sha(Path(p)) == h for p,h in local_hashes.items()), 'Generated fixture/plan/settings changed during native work')
    try:
        for run in inventory['runs']:
            verify()
            log_path = output/(run['name']+'.log')
            run_started = time.perf_counter()
            with log_path.open('w', encoding='utf-8') as stream:
                process = subprocess.run(run['command'], cwd=renderer.parent, stdout=stream,
                    stderr=subprocess.STDOUT, env=environment, timeout=args.timeout,
                    creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            log = log_path.read_text(encoding='utf-8', errors='replace')
            require(process.returncode == 0, f'Native process failed ({process.returncode}): {log_path}')
            proof = layer_proof(log, library)
            require(all(Path(p).is_file() for p in run['images']), 'A native capture is missing')
            sample = read_capture(run['images'][0])
            hardware = {key:sample['state'][key] for key in ('gpu', 'driverVersion', 'vulkanApiVersion')}
            if 'hardware' in report: require(report['hardware'] == hardware, 'GPU/driver changed during acceptance')
            report['hardware'] = hardware
            artifacts = [Path(p) for p in run['images']]
            artifacts += [Path(p).with_suffix('.telemetry.json') for p in run['images']]
            artifacts += [Path(p+'.capture.json') for p in run['images']]
            artifacts.append(log_path)
            report['runs'].append(dict(name=run['name'], log=str(log_path), returnCode=process.returncode,
                elapsedSeconds=time.perf_counter()-run_started, captureCount=len(run['images']),
                layer=proof, captureArtifacts={str(p):sha(p) for p in artifacts}))
            report['completedProcesses'] += 1
            write(output/'results.json', report)
            print(f"{run['name']}: {len(run['images'])} strict-clean captures", flush=True)
        verify()
        for run in report['runs']:
            require(all(sha(Path(p)) == h for p,h in run['captureArtifacts'].items()), 'Captured artifacts changed')
        report['scenes'] = analyze(output, scenes, inventory)
        calibration_guide(output, report['scenes'])
        report['manifestAfter'] = verify_manifest(manifest, consumed)
        require(report['manifestAfter']['sha256'] == before['sha256'], 'Final freeze manifest identity changed')
        report['frozenInputsMatched'] = True
        report['captures'] = sum(r['captureCount'] for r in report['runs'])
        report['passed'] = report['captures'] == inventory['captureCount']
    except Exception:
        report['failure'] = traceback.format_exc()
        raise
    finally:
        report['elapsedSeconds'] = time.perf_counter()-started
        write(output/'results.json', report)
    print(f"PASS {report['captures']} captures / {report['completedProcesses']} processes", flush=True)


if __name__ == '__main__':
    main()
