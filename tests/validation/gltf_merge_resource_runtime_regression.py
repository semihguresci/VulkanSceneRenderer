"""Bounded #106 descriptor/resource publication witnesses (10 processes/48 PNGs).

Complements the independent static/dynamic merge cohorts. GUI loading is checked
through actual progress-frame telemetry, not changing UI pixels. Hard-ray/DDGI
uses lit opaque receivers and zero GI intensity: AS construction and published
probe work are execution witnesses, not a visual indirect-lighting oracle.
The first extended-material merge has a separately authored zero-factor variant
which must visibly differ, then return exactly to the active Default appearance.
The stale-destination case changes scene during source preparation and verifies
that the import is cancelled before installing resources.

--generate-only and --self-check never launch a renderer. Native execution needs
an explicit frozen manifest; all its entries and generated inputs are checked
before/after each process. Existing capture evidence is never overwritten.
"""
from __future__ import annotations

import argparse
import base64
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile

import numpy as np
from PIL import Image

from gltf_accessor_runtime_regression import Asset, decode_accessor, load_asset, require
from gltf_extended_materials_runtime_regression import extension_material, validation_log_error
from gltf_tangent_uv_runtime_regression import finite_json
from gltf_texture_formats_runtime_regression import verify_manifest


TECHNIQUES = ('deferred-raster', 'forward-raster')
CASES = ('gui-progress', 'ray-ddgi', 'frame-rebuild', 'first-extended', 'stale-destination')
ANISO, DIFFUSE, VARIANTS = ('KHR_materials_anisotropy',
    'KHR_materials_diffuse_transmission', 'KHR_materials_variants')
WIDTH, HEIGHT = 640, 360
ROI = (380, 145, 460, 215)
POSITIONS = [[-.75, -.75, 0.], [.75, -.75, 0.], [.75, .75, 0.], [-.75, .75, 0.]]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def immutable(path, payload):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        require(path.read_bytes() == payload, 'Refusing to replace existing evidence/input: '+str(path))
    else:
        path.write_bytes(payload)


def document(path, value):
    immutable(path, (json.dumps(value, indent=2, allow_nan=False)+'\n').encode())


def fixture(path, origin, *, extended=False):
    asset = Asset()
    positions = asset.accessor('VEC3', POSITIONS)
    primitive = dict(attributes=dict(POSITION=positions,
        NORMAL=asset.accessor('VEC3', [[0., 0., 1.]]*4),
        TANGENT=asset.accessor('VEC4', [[1., 0., 0., 1.]]*4),
        TEXCOORD_0=asset.accessor('VEC2', [[0., 0.], [1., 0.], [1., 1.], [0., 1.]])),
        indices=asset.accessor('SCALAR', [0, 1, 2, 0, 2, 3], 5123, indices=True), material=0, mode=4)
    texture = asset.image(np.tile([220, 180, 120, 255] if origin == 'B' else [180, 200, 220, 255], (2, 2, 1)))
    if extended:
        active = extension_material(anisotropy=.85, diffuse=.65, color=[.08, .25, .85], specular=1.)
        active['pbrMetallicRoughness']['roughnessFactor'] = .35
        active['extensions'][ANISO]['anisotropyRotation'] = .4
        active['pbrMetallicRoughness']['baseColorTexture'] = dict(index=texture)
        zero = copy.deepcopy(active)
        zero['name'] = 'Independent zero-factor appearance'
        zero['extensions'][ANISO]['anisotropyStrength'] = 0.
        zero['extensions'][DIFFUSE]['diffuseTransmissionFactor'] = 0.
        asset.model['materials'] = [active, zero]
        primitive['extensions'] = {VARIANTS: dict(mappings=[dict(material=1, variants=[0])])}
        asset.model['extensions'] = {VARIANTS: dict(variants=[dict(name='Zero factors')])}
        asset.model['extensionsUsed'] = [*active['extensions'], VARIANTS]
    else:
        asset.model['materials'] = [dict(name='Ordinary '+origin,
            pbrMetallicRoughness=dict(baseColorFactor=[.65, .4, .2, 1.] if origin == 'B' else [.3, .5, .7, 1.],
                baseColorTexture=dict(index=texture), metallicFactor=0., roughnessFactor=.8))]
    asset.model['meshes'] = [dict(name=origin+' shared source geometry', primitives=[primitive])]
    asset.model['nodes'] = [dict(name='Retained offscreen anchor', mesh=0, translation=[20., 0., 0.]),
        dict(name='Retained A receiver', mesh=0, translation=[-.9, 0., 0.])] if origin == 'A' else [
        dict(name='Incoming B receiver', mesh=0)]
    asset.model['scenes'] = [dict(name=origin+' authoring scene', nodes=list(range(len(asset.model['nodes']))))]
    asset.model['buffers'] = [dict(byteLength=len(asset.data),
        uri='data:application/octet-stream;base64,'+base64.b64encode(asset.data).decode())]
    document(path, asset.model)
    return asset.model


def build_plan(output, techniques, cases):
    paths = {name: output/'fixtures'/name/'scene.gltf' for name in ('A', 'B', 'extended')}
    captures, launches = 0, []
    for case in cases:
        frames = [13, 26, 39, 52] + ([65, 78] if case == 'first-extended' else [])
        incoming = paths['extended' if case == 'first-extended' else 'B']
        parent = 'Resource witness '+case
        events = [dict(frame=14, guiWaitForLoad=True, merge=dict(path=str(incoming), parentName=parent,
            placementMatrix=[1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0., .9, 0., 0., 1.])),
            dict(frame=32 if case == 'frame-rebuild' else 27, sceneHistory='undo'),
            dict(frame=45 if case == 'frame-rebuild' else 40, sceneHistory='redo')]
        if case == 'frame-rebuild':
            events += [dict(frame=30, presentSuboptimal=True), dict(frame=43, acquireOutOfDate=True)]
        if case == 'first-extended':
            events += [dict(frame=53, materialVariant=0), dict(frame=66, materialVariant=-1)]
        if case == 'stale-destination':
            # Queue on a skipped frame so drawFrame starts the second import
            # just before it applies the switch to the first import's scene.
            frames += [54, 65]
            events += [dict(frame=53, skip=True, guiWaitForLoad=False,
                merge=dict(path=str(incoming), parentName='Stale destination')),
                dict(frame=54, gltfScene=1)]
        sequence = dict(schemaVersion=1, frames=frames[-1], sampleFrames=frames,
            camera=[dict(frame=1, position=[0., 0., 4.], target=[0., 0., 0.])],
            events=sorted(events, key=lambda event: event['frame']))
        launches.append(dict(case=case, source=str(paths['A']), incoming=str(incoming), parentName=parent,
            sequence=sequence, guiEnabled=case in ('gui-progress', 'stale-destination'), ray=case == 'ray-ddgi'))
        captures += len(frames)
    return dict(schemaVersion=1, techniques=list(techniques), launches=launches,
        paths={key: str(value) for key, value in paths.items()},
        processes=len(launches)*len(techniques), captures=captures*len(techniques),
        limits=['No descriptor contents/readback or allocator leak-freedom claim.',
            'DDGI is execution-only (intensity zero); direct/raster images are not an indirect-lighting oracle.',
            'GUI screenshots intentionally have no exact-pixel assertion because UI state changes.',
            'Surface-generation availability is witnessed by active DDGI trace/update and actual ray generation; no separate surface ID telemetry exists.',
            'This cohort does not replace static independent baked-reference or dynamic deformation/export coverage.'])


def generate(plan):
    paths = {name: Path(path) for name, path in plan['paths'].items()}
    fixture(paths['A'], 'A'); fixture(paths['B'], 'B'); fixture(paths['extended'], 'B', extended=True)
    for launch in plan['launches']:
        document(paths['A'].parents[2]/'sequences'/(launch['case']+'.json'), launch['sequence'])
    # The contrast crop is inside B by independent fixed-camera projection.
    focal = HEIGHT/(2*math.tan(math.pi/8))
    left, top, right, bottom = ROI
    require(WIDTH/2+focal*(.9-.75)/4 < left < right < WIDTH/2+focal*(.9+.75)/4 and
            HEIGHT/2-focal*.75/4 < top < bottom < HEIGHT/2+focal*.75/4,
            'Extended zero-control crop lies outside authored B geometry')
    for name, path in paths.items():
        model, _ = load_asset(path)
        require(np.array_equal(decode_accessor(path, model['meshes'][0]['primitives'][0]['attributes']['POSITION']),
            np.asarray(POSITIONS, dtype=np.float32)), 'Fixture source geometry was changed')
    model, _ = load_asset(paths['extended'])
    active, zero = copy.deepcopy(model['materials'][0]), copy.deepcopy(model['materials'][1])
    require(active['extensions'][ANISO]['anisotropyStrength'] > 0 and
            active['extensions'][DIFFUSE]['diffuseTransmissionFactor'] > 0, 'Extended positive control is vacuous')
    active.pop('name', None); zero.pop('name', None)
    active['extensions'][ANISO]['anisotropyStrength'] = 0.
    active['extensions'][DIFFUSE]['diffuseTransmissionFactor'] = 0.
    require(active == zero, 'Zero control differs in fields other than the two tested factors')
    return dict(geometryUnchanged=True, independentZeroMaterialOnlyChangesFactors=True, fixedCropInsideB=True)


def unsigned(value, label):
    require(type(value) is int and value >= 0, 'Invalid unsigned telemetry: '+label)
    return value


def passes(state):
    return {row['name']: row for row in state['passes']}


def history_cursor(history, *, initial_sample):
    # Headless sessions create SceneEditSession lazily at the first edit. Only
    # the initial sample can legitimately omit these two telemetry fields.
    require(initial_sample or ('undoCount' in history and 'redoCount' in history),
            'Post-merge history telemetry omitted its mandatory cursor')
    return tuple(unsigned(history.get(name, 0), 'history.'+name) for name in ('undoCount', 'redoCount'))


def history_schema_self_check():
    # Real uninitialized shape: variant identity/assignments exist but no
    # SceneEditSession has produced undoCount/redoCount yet.
    uninitialized = dict(selected=-1, count=0, assignments=[])
    require(history_cursor(uninitialized, initial_sample=True) == (0, 0),
            'Uninitialized headless baseline did not resolve to an empty cursor')
    committed = dict(uninitialized, undoCount=1, redoCount=0)
    require(history_cursor(committed, initial_sample=False) == (1, 0), 'Committed cursor changed')
    for missing in ('undoCount', 'redoCount'):
        malformed = copy.deepcopy(committed)
        del malformed[missing]
        try:
            history_cursor(malformed, initial_sample=False)
        except RuntimeError:
            pass
        else:
            raise RuntimeError('Post-merge omission was accepted: '+missing)
    return dict(uninitializedBaselineAccepted=True, postCommitMissingUndoRejected=True,
                postCommitMissingRedoRejected=True)


def validate_stale_destination(records, checks):
    before, pending, failed = (row['state'] for row in records)
    original = before['gltfMerge']
    loading = before['gui']['modelLoading']
    for state, waiting in ((pending, True), (failed, False)):
        merge, scene = state['gltfMerge'], state['gltfScenes']
        current_load = state['gui']['modelLoading']
        require(merge['requestId'] == original['requestId']+1 and
                merge['sourceReadCount'] == original['sourceReadCount']+1 and
                merge['parentName'] == 'Stale destination', 'Unexpected import request after scene switch')
        require(all(merge[key] == original[key] for key in ('generationPublications', 'commitGeneration',
                'descriptorGeneration', 'graphNodeCount', 'sourceTables')), 'Stale import published scene content')
        require(scene['graphIdentity'] == before['gltfScenes']['graphIdentity'] and scene['selected'] == 1 and
                scene['names'] == before['gltfScenes']['names'] and scene['activeRenderableCount'] == 1,
                'The scene switch did not expose only the canonical incoming scene')
        require(history_cursor(state['gltfMaterialVariants'], initial_sample=False) == (0, 0),
                'Stale import added edit history after the scene switch')
        require(current_load['pending'] is waiting and current_load['requests'] == loading['requests']+1 and
                current_load['completed'] == loading['completed']+int(not waiting) and
                current_load['cancelled'] == loading['cancelled']+int(not waiting) and
                current_load['progressFrames'] == loading['progressFrames'], 'Stale import reached installation or did not settle')
        require(merge['outcome'] == ('pending' if waiting else 'failed'), 'Wrong stale import outcome')
    error = failed['gltfMerge']['error']
    require('Select the destination scene and import again.' in error and error in failed['gui']['status'],
            'The cancelled import did not explain how to retry')
    checks.append(dict(name='stale-destination-cancelled', passed=True, selectedScene=1,
        generationPublications=original['generationPublications'], diagnostic=error))


def validate_series(launch, records, checks):
    baseline, case = records[0]['state'], launch['case']
    if case == 'stale-destination':
        validate_stale_destination(records[-3:], checks)
        records = records[:4]
    base_merge = baseline['gltfMerge']
    for index, record in enumerate(records):
        state, merged = record['state'], index not in (0, 2)
        merge, history, scene = state['gltfMerge'], state['gltfMaterialVariants'], state['gltfScenes']
        publication = min(index, 3)
        for name in ('requestId', 'sourceReadCount', 'generationPublications', 'commitGeneration', 'descriptorGeneration'):
            unsigned(merge[name], 'gltfMerge.'+name)
        require(merge['requestId']-base_merge['requestId'] == int(index > 0) and
                merge['sourceReadCount']-base_merge['sourceReadCount'] == int(index > 0),
                'Undo/redo/resize/variant unexpectedly reread an incoming file')
        require(merge['generationPublications']-base_merge['generationPublications'] == publication and
                merge['commitGeneration']-base_merge['commitGeneration'] == publication and
                merge['descriptorGeneration'] == merge['commitGeneration'], 'Wrong resource generation publication')
        require(merge['outcome'] == ('idle' if index == 0 else 'committed') and not merge['error'],
                'Merge remained pending or failed')
        require(index == 0 or merge['parentName'] == launch['parentName'], 'Wrong completed request identity')
        require(scene['graphIdentity'] == baseline['gltfScenes']['graphIdentity'] and scene['selected'] == 0 and
                len(scene['names']) == 1+int(merged), 'Graph identity/scene catalog changed incorrectly')
        expected = dict(nodes=5 if merged else 2, meshes=1+int(merged), materials=1+int(merged)*(2 if case == 'first-extended' else 1),
            images=1+int(merged), textures=1+int(merged), samplers=1+int(merged), buffers=1+int(merged), scenes=1+int(merged))
        require(merge['sourceTables'] == expected, 'Canonical/placement/resource source counts differ')
        require(scene['activeRenderableCount'] == state['scene']['primaryObjectCount'] == 2+int(merged) and
                state['gltfInstancing']['drawCount'] == 2+int(merged), 'Wrong prepared/active draw count')
        require(sorted(scene['activeSourceNodes']) == ([0, 1, 3, 4] if merged else [0, 1]),
                'Inactive canonical B or absent placement became active')
        cursor = ((0, 0), (1, 0), (0, 1), (1, 0), (2, 0), (3, 0))[index]
        require(history_cursor(history, initial_sample=index == 0) == cursor,
                'History cursor lost after resource staging/rebuild')
        require(merge['importScale'] == state['scene']['primaryImportScale'] == 1. and
                state['scene']['primary'] == baseline['scene']['primary'], 'Merge replaced the retained primary source/scale')
        loading = state['gui']['modelLoading']
        require(not loading['pending'] and loading['requests']-baseline['gui']['modelLoading']['requests'] == int(index > 0) and
                loading['completed']-baseline['gui']['modelLoading']['completed'] == int(index > 0) and
                loading['cancelled'] == baseline['gui']['modelLoading']['cancelled'], 'GUI transaction did not settle exactly once')
        require(state['gui']['selectedMeshNodeId'] == baseline['gui']['selectedMeshNodeId'], 'Retained selection changed')
        rows = {row['sourceNode']: row for row in merge['retainedNodes'] if row['sourceNode'] in (0, 1)}
        original = {row['sourceNode']: row for row in base_merge['retainedNodes'] if row['sourceNode'] in (0, 1)}
        require(not merge['retainedNodesTruncated'] and rows == original and set(rows) == {0, 1},
                'Retained source identities/world transforms changed')
        buffers = merge['objectBuffers']
        require(buffers and all(unsigned(row['handle'], 'object buffer') > 0 and type(row['controllerOwned']) is bool and
                (index == 0 or row['controllerOwned']) for row in buffers),
                'Published generation has absent/unowned object buffers')
        require(unsigned(merge['retainedHistoryLogicalCpuBytes'], 'history bytes') <=
                unsigned(merge['historyLogicalCpuBudgetBytes'], 'history budget'), 'History payload exceeds finite budget')
        table = passes(state)
        require(table.get('PostProcess', {}).get('active') and
                table.get('GBuffer' if state['technique'] == TECHNIQUES[0] else 'Lighting', {}).get('active'),
                'Requested raster consumer did not execute')
        if case == 'first-extended':
            require(history['selected'] == (0 if index == 4 else -1), 'Zero/default variant binding is stale')
            # Context-owned attachments remain ready through undo; their clear
            # still executes when A has no extended owners. Baseline is lazy.
            require(bool(table.get('PbrAuxiliary', {}).get('active')) ==
                    (state['technique'] == TECHNIQUES[0] and index > 0), 'Deferred auxiliary readiness/forward routing differs')
        checks.append(dict(name=case+'-state-'+str(index), passed=True,
            commitGeneration=merge['commitGeneration'], descriptorGeneration=merge['descriptorGeneration'],
            readAttempts=merge['sourceReadCount'], objectBuffers=buffers, sourceTables=expected,
            undoCount=cursor[0], redoCount=cursor[1], retainedIdentityAndWorldUnchanged=True))
    if case == 'gui-progress':
        loading, before = records[1]['state']['gui']['modelLoading'], baseline['gui']['modelLoading']
        require(loading['progressFrames'] > before['progressFrames'] and loading['eventPumps'] > before['eventPumps'],
                'GUI-enabled request did not submit an actual progress frame and pump events')
        require('status' in records[1]['state']['gui'] and all('--no-ui' not in row['command'] for row in records),
                'GUI-enabled witness accidentally used headless/no-UI mode')
        checks.append(dict(name=case+'-submitted-progress', passed=True, progressFrames=loading['progressFrames'],
            eventPumps=loading['eventPumps'], imageAssertion='none: changing GUI pixels'))
    if case == 'ray-ddgi':
        generations = []
        for index, record in enumerate(records):
            state = record['state']; ray, gi = state['rayShadows'], state['ddgi']; build = ray.get('build', {})
            require(ray['supported'] and ray['active'] and ray['mode'] == 1 and not ray['denoise'], 'Hard ray shadows are unavailable')
            require(build.get('instances') == (3 if index in (1, 3) else 2) and
                    build.get('blasBuilt', 0)+build.get('blasReused', 0) > 0 and build.get('asAllocatedBytes', 0) > 0,
                    'Actual production TLAS/BLAS did not match this generation')
            require(gi['supported'] and gi['enabled'] and gi['active'] and gi['contentEpoch'] > 0 and
                    gi['updatedProbes'] > 0 and gi['tracedRays'] > 0 and gi['settings']['intensity'] == 0 and
                    gi['settings']['counts'] == [3, 3, 3] and gi['settings']['raysPerProbe'] == 4,
                    'No active surface-backed DDGI trace/update was published')
            generations.append(unsigned(ray['generation'], 'actual ray scene generation'))
            require(index == 0 or generations[-1] > generations[-2], 'Actual ray generation was reused across a different scene')
            checks.append(dict(name=case+'-AS-surface-'+str(index), passed=True, rayGeneration=generations[-1],
                TLASInstances=build['instances'], asAllocatedBytes=build['asAllocatedBytes'],
                ddgiUpdatedProbes=gi['updatedProbes'], ddgiTracedRays=gi['tracedRays'],
                witness='execution only; GI intensity zero; no separate surface generation ID available'))
    if case == 'frame-rebuild':
        counters = [unsigned(row['state']['swapchainRecreates'], 'swapchain recreates') for row in records]
        require(counters[2] > counters[1] and counters[3] > counters[2], 'Injected present/acquire recovery did not rebuild frame resources')
        checks.append(dict(name=case+'-both-rebuilds', passed=True, swapchainRecreates=counters,
            undoRedoStillWorks=True, retainedGeneration='restaged against current frame resources'))


def pixels(record):
    with Image.open(record['image']) as source:
        require(source.size == (WIDTH, HEIGHT), 'Unexpected screenshot dimensions')
        return np.asarray(source.convert('RGB'), dtype=np.int16)


def image_checks(launch, records, checks):
    if launch['guiEnabled']:
        return
    for name, first, second in [('undo', 0, 2), ('redo', 1, 3)]+(
            [('restored-default', 1, 5)] if launch['case'] == 'first-extended' else []):
        delta = np.abs(pixels(records[first])-pixels(records[second]))
        require(int(delta.max()) == 0, 'Resource/history image mismatch: '+launch['case']+'/'+name)
        checks.append(dict(name=launch['case']+'-'+name+'-exact', passed=True, maximumByteError=0))
    if launch['case'] == 'first-extended':
        left, top, right, bottom = ROI
        active, zero = pixels(records[1])[top:bottom, left:right], pixels(records[4])[top:bottom, left:right]
        require(float(active.mean()) > 8 and float(zero.mean()) > 8 and
                float(active.mean()) < 250 and float(zero.mean()) < 250,
                'Extended-material contrast uses black or clipped receiver interiors')
        delta = np.abs(active-zero)
        mean, changed = float(delta.mean()), float(np.any(delta >= 4, axis=2).mean())
        require(mean >= 1. and changed >= .1, 'First extended-material merge/zero control is visually vacuous')
        checks.append(dict(name='first-extended-positive-vs-zero', passed=True, roi=list(ROI),
            meanAbsoluteByteError=mean, changedFractionAtLeast4=changed, minimumMean=1., minimumChangedFraction=.1))


def manifest_check(manifest, renderer):
    proof = verify_manifest(manifest)
    expected = json.loads(manifest.read_text(encoding='utf-8-sig'))['files']
    require(any(Path(path).resolve() == renderer and digest.lower() == sha(renderer)
                for path, digest in expected.items()), 'Renderer is not covered by the frozen manifest')
    return proof


def run_case(args, output, technique, launch, report, persist, input_hashes):
    folder = output/'captures'/technique
    folder.mkdir(parents=True, exist_ok=True)
    prefix = folder/launch['case']; image = prefix.with_suffix('.png'); log = prefix.with_suffix('.log')
    require(not list(folder.glob(launch['case']+'*.png')) and not log.exists(), 'Refusing to overwrite native evidence: '+str(prefix))
    command = [str(args.renderer), '--hidden', '--validation', '--no-bloom', '--no-taa',
        '--width', str(WIDTH), '--height', str(HEIGHT), '--msaa', '1', '--warmup-frames', '12', '--capture-frame', '13',
        '--fixed-dt', '.016666667', '--render-technique', technique, '--display-mode', 'lit', '--exposure', '.35',
        '--camera-position', '0', '0', '4', '--camera-target', '0', '0', '0', '--camera-fov', '45',
        '--environment-intensity', '0', '--directional-intensity', '3', '--directional-direction', '0', '0', '-1',
        '--model', launch['source'], '--capture-sequence', str(output/'sequences'/(launch['case']+'.json')),
        '--screenshot', str(image)]
    if not launch['guiEnabled']: command.append('--no-ui')
    command += ['--ray-shadows', 'hard', '--no-ray-shadow-denoise', '--ddgi', '--ddgi-counts', '3', '3', '3',
        '--ddgi-rays', '4', '--ddgi-probes-per-frame', '27', '--ddgi-ray-budget', '108', '--ddgi-intensity', '0'] if launch['ray'] else [
        '--no-ray-query', '--ray-shadows', 'raster']
    entry = dict(case=launch['case'], technique=technique, command=command, log=str(log), validated=False,
        manifestBefore=manifest_check(args.manifest, args.renderer))
    report['processes'].append(entry); persist()
    try:
        with log.open('x', encoding='utf-8') as stream:
            process = subprocess.run(command, cwd=args.renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                timeout=args.timeout, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        entry['returnCode'] = process.returncode; entry['logSha256'] = sha(log)
        require(process.returncode == 0, 'Renderer process failed: '+str(log))
        require(not validation_log_error(log.read_text(encoding='utf-8', errors='replace')), 'Vulkan validation/synchronization error: '+str(log))
        records = []
        frames = launch['sequence']['sampleFrames']
        for frame in frames:
            shot = image if frame == frames[-1] else folder/(prefix.name+f'.frame-{frame:04d}.png')
            telemetry = shot.with_suffix('.telemetry.json')
            require(shot.is_file() and telemetry.is_file(), 'Missing fresh capture/telemetry: '+str(shot))
            state = json.loads(telemetry.read_text(encoding='utf-8'))
            row = dict(case=launch['case'], technique=technique, frame=frame, image=str(shot), telemetry=str(telemetry),
                log=str(log), command=command, imageSha256=sha(shot), telemetrySha256=sha(telemetry), validated=False)
            report['captures'].append(row); records.append(dict(**row, state=state))
            require(finite_json(state) and state['technique'] == technique and state['resolution'] == [WIDTH, HEIGHT] and
                    state['msaaSamples'] == 1, 'Nonfinite or wrong rendering configuration: '+str(telemetry))
            pixels(records[-1])
        require(len(list(folder.glob(prefix.name+'*.telemetry.json'))) == len(frames), 'Unexpected extra captures')
        local_checks = []
        validate_series(launch, records, local_checks); image_checks(launch, records, local_checks)
        report['checks'] += [dict(**check, technique=technique) for check in local_checks]
        for row in report['captures']:
            if row['case'] == launch['case'] and row['technique'] == technique: row['validated'] = True
        entry['validated'] = True; entry['validationClean'] = True
    finally:
        # Even a process/log/schema failure retains every produced PNG/hash as
        # excluded diagnostics. They never inflate validated-capture totals.
        listed = {row['image'] for row in report['captures']}
        entry['evidenceFiles'] = {str(path): sha(path) for path in folder.glob(prefix.name+'*') if path.is_file()}
        for shot in sorted(folder.glob(prefix.name+'*.png')):
            if str(shot) not in listed:
                telemetry = shot.with_suffix('.telemetry.json')
                report['captures'].append(dict(case=launch['case'], technique=technique, image=str(shot),
                    telemetry=str(telemetry), log=str(log), command=command, imageSha256=sha(shot),
                    telemetrySha256=sha(telemetry) if telemetry.is_file() else None, validated=False,
                    diagnostic='Produced before process/log/schema validation failed'))
        try:
            entry['manifestAfter'] = manifest_check(args.manifest, args.renderer)
            require(all(Path(path).is_file() and sha(Path(path)) == digest for path, digest in input_hashes.items()),
                    'Generated fixture/sequence bytes changed during native validation')
        except Exception as error:
            entry['provenanceError'] = str(error)
            entry['validated'] = False
            raise
        finally:
            persist()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--technique', action='append', choices=TECHNIQUES)
    parser.add_argument('--case', action='append', choices=CASES)
    parser.add_argument('--generate-only', action='store_true')
    parser.add_argument('--self-check', action='store_true')
    parser.add_argument('--timeout', type=int, default=240)
    args = parser.parse_args()
    techniques, cases = tuple(dict.fromkeys(args.technique or TECHNIQUES)), tuple(dict.fromkeys(args.case or CASES))
    if args.self_check:
        with tempfile.TemporaryDirectory(prefix='gltf-merge-resource-') as temporary:
            plan = build_plan(Path(temporary), techniques, cases)
            proof = generate(plan)
            require(plan['processes'] == len(techniques)*len(cases), 'Process bound changed')
            print(json.dumps(dict(passed=True, nativeExecuted=False, processes=plan['processes'],
                captures=plan['captures'], fixtureProof=proof, historySchema=history_schema_self_check())))
        return
    require(args.output is not None, '--output is required')
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    plan = build_plan(output, techniques, cases); proof = generate(plan)
    document(output/'launch-plan.json', plan)
    inputs = {str(path.resolve()): sha(path) for folder in ('fixtures', 'sequences')
        for path in (output/folder).rglob('*') if path.is_file()}
    report = dict(schemaVersion=1, passed=False, generatedOnly=args.generate_only, nativeValidationPerformed=False,
        expectedProcesses=plan['processes'], expectedCaptures=plan['captures'], fixtureProof=proof,
        generatedInputs=inputs, processes=[], captures=[], checks=[], limitations=plan['limits'])
    report['harnessSources'] = {str(path.resolve()): sha(path) for path in (Path(__file__),
        Path(__file__).with_name('gltf_accessor_runtime_regression.py'),
        Path(__file__).with_name('gltf_extended_materials_runtime_regression.py'),
        Path(__file__).with_name('gltf_tangent_uv_runtime_regression.py'),
        Path(__file__).with_name('gltf_texture_formats_runtime_regression.py'))}
    def persist():
        (output/'results.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    require(not (output/'results.json').exists(), 'Refusing to overwrite an existing native/generation report')
    persist()
    if args.generate_only:
        print(f'Generated {plan["processes"]} process / {plan["captures"]} capture plan; renderer not executed')
        return
    require(args.renderer and args.manifest, 'Native execution requires --renderer and --manifest')
    args.renderer, args.manifest = args.renderer.resolve(), args.manifest.resolve()
    report['renderer'] = str(args.renderer); report['rendererSha256'] = sha(args.renderer)
    immutable(output/'vk_layer_settings.txt', b'khronos_validation.validate_sync = true\n')
    report['manifestBefore'] = manifest_check(args.manifest, args.renderer)
    report['nativeValidationPerformed'] = True; persist()
    try:
        for technique in techniques:
            for launch in plan['launches']:
                print(technique, launch['case'], 'starting', flush=True)
                run_case(args, output, technique, launch, report, persist, inputs)
                print(technique, launch['case'], 'PASS', flush=True)
        require(len(report['captures']) == plan['captures'] and all(row['validated'] for row in report['captures']),
                'Capture validation count differs from the bounded plan')
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        try:
            report['manifestAfter'] = manifest_check(args.manifest, args.renderer)
            require(all(sha(Path(path)) == digest for path, digest in report['harnessSources'].items()),
                    'Harness/dependency source changed during native validation')
        except Exception as error:
            report['provenanceError'] = str(error)
            report['passed'] = False
            raise
        finally:
            report['validatedCaptureCount'] = sum(row['validated'] for row in report['captures'])
            report['excludedDiagnosticCaptureCount'] = sum(not row['validated'] for row in report['captures'])
            persist()
    print(f'PASS {len(report["captures"])} captures, {len(report["checks"])} checks, {len(report["processes"])} strict-clean logs')


if __name__ == '__main__':
    main()
