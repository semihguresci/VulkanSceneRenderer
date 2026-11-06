"""Generated multi-scene skin/morph switches against pruned single-scene assets.

The source mesh/accessor bytes are shared by independent owners. References
remove inactive node subtrees and remap joints/skins/animation targets, rather
than selecting a different scene through the implementation under test.
--generate-only never launches the renderer. --ray enables hard ray shadows
and DDGI in either raster technique. No GPU picking readback is claimed.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess

import numpy as np

import gltf_deformation_runtime_regression as deformation
from gltf_accessor_runtime_regression import TECHNIQUES, finite_json, require
from gltf_material_variants_runtime_regression import pixels
from gltf_texture_formats_runtime_regression import verify_manifest

CASES = ('skin', 'morph')
NAMES = ['Owner A', 'Owner B', 'Both owners', 'Empty']
SAMPLES = {1: 0, 5: 1, 9: 2, 13: 3, 17: 1, 21: 0, 25: 2, 29: 3, 33: 0}
POSE_TIME = .65


def fixture(path, case, ray):
    body = deformation.cube()
    # A stable, offscreen first draw keeps startup selection/outline identical
    # to each oracle; scene switches deliberately clear that selection.
    anchor = dict(positions=body['positions'][:3], normals=body['normals'][:3],
                  tangents=body['tangents'][:3], uv=body['uv'][:3],
                  indices=np.array([0, 1, 2]), targets=[], weights=[], influences=[], material=0)
    spec = dict(meshes=[body, anchor], nodes=[dict(name='Offscreen selection anchor', mesh=1,
                translation=[20, 0, 0])], skins=[], tracks=[])
    roots, owners = [], []
    if case == 'skin':
        body['targets'], body['weights'] = [], []
        body['influences'] = [(np.tile([0, 1, 0, 0], (24, 1)),
                               np.tile([.4, .6, 0, 0], (24, 1)), False)]
    for which in range(2):
        root = len(spec['nodes']); owner = root + 1
        roots.append(root); owners.append(owner)
        spec['nodes'].extend([
            dict(name=f'Independent root {which}', translation=[-.85 if which == 0 else .85, 0, 0], children=[owner]),
            dict(name=f'Shared mesh owner {which}', mesh=0)])
        if case == 'skin':
            joint, child = root + 2, root + 3
            spec['nodes'][root]['children'].append(joint)
            angle = .25 if which == 0 else -.2
            spec['nodes'].extend([
                dict(name=f'Own joint root {which}', children=[child],
                     translation=[.06 if which == 0 else -.08, .02, 0],
                     rotation=[0, 0, math.sin(angle/2), math.cos(angle/2)],
                     scale=[1.05, .9, 1] if which == 0 else [.9, 1.15, 1]),
                dict(name=f'Own animated joint {which}', translation=[.1 if which == 0 else -.1, 0, 0])])
            spec['skins'].append(dict(joints=[joint, child], inverseBind=[np.eye(4), np.eye(4)]))
            spec['nodes'][owner]['skin'] = which
            spec['tracks'].append((child, 'translation', [.1 if which == 0 else -.1, 0, 0],
                                   [.35, .25, 0] if which == 0 else [-.25, -.2, 0]))
        else:
            weights = [.1, .2] if which == 0 else [.8, .5]
            spec['nodes'][owner]['weights'] = weights
            spec['tracks'].append((owner, 'weights', weights, [.6, .35] if which == 0 else [.25, -.15]))
    common = [0]
    if ray:
        floor = dict(positions=deformation.f32([[-2.4,-1,1],[2.4,-1,1],[2.4,-1,-2],[-2.4,-1,-2]]),
                     normals=deformation.f32([[0,1,0]]*4), tangents=deformation.f32([[1,0,0,1]]*4),
                     uv=deformation.f32([[0,0],[1,0],[1,1],[0,1]]), indices=np.array([0,1,2,0,2,3]),
                     targets=[], weights=[], influences=[], material=0)
        spec['meshes'].append(floor)
        common.append(len(spec['nodes']))
        spec['nodes'].append(dict(name='Shared ordinary ray receiver', mesh=2))
    deformation.write_fixture(path, spec)
    model = json.loads(path.read_text(encoding='utf-8'))
    scene_roots = [common+[roots[0]], common+[roots[1]], common+roots, []]
    model['scenes'] = [dict(name=NAMES[i], nodes=nodes) for i, nodes in enumerate(scene_roots)]
    model['scene'] = 0
    path.write_text(json.dumps(model, indent=2), encoding='utf-8')
    # Check the fixture itself, independently of the native catalog/parser.
    parents = {child: parent for parent, node in enumerate(model['nodes']) for child in node.get('children', [])}
    require(all(root not in parents for scene in scene_roots for root in scene), 'Fixture scene roots must be parentless')
    require(model['nodes'][owners[0]]['mesh'] == model['nodes'][owners[1]]['mesh'] == 0,
            'Independent owners stopped sharing source geometry')
    return model, spec, owners


def active_nodes(model, selected):
    active, stack = set(), list(model['scenes'][selected]['nodes'])
    while stack:
        node = stack.pop()
        require(node not in active, 'Fixture violates global strict forest')
        active.add(node); stack.extend(model['nodes'][node].get('children', []))
    return sorted(active)


def single_scene_reference(model, selected, path):
    indices = active_nodes(model, selected)
    remap = {old: new for new, old in enumerate(indices)}
    reference = copy.deepcopy(model)
    reference['nodes'] = [copy.deepcopy(model['nodes'][old]) for old in indices]
    used_skins = sorted({node['skin'] for node in reference['nodes'] if 'skin' in node})
    skin_map = {old: new for new, old in enumerate(used_skins)}
    for node in reference['nodes']:
        if 'children' in node: node['children'] = [remap[child] for child in node['children']]
        if 'skin' in node: node['skin'] = skin_map[node['skin']]
    if used_skins:
        reference['skins'] = [copy.deepcopy(model['skins'][old]) for old in used_skins]
        for skin in reference['skins']:
            require(all(joint in remap for joint in skin['joints']), 'Skin joint escaped its independent scene root')
            skin['joints'] = [remap[joint] for joint in skin['joints']]
            if 'skeleton' in skin: skin['skeleton'] = remap[skin['skeleton']]
    else: reference.pop('skins', None)
    animations = []
    for original in model.get('animations', []):
        animation = copy.deepcopy(original)
        animation['channels'] = [channel for channel in animation['channels'] if channel['target']['node'] in remap]
        for channel in animation['channels']: channel['target']['node'] = remap[channel['target']['node']]
        if animation['channels']: animations.append(animation)
    if animations: reference['animations'] = animations
    else: reference.pop('animations', None)
    reference['scenes'] = [dict(nodes=[remap[root] for root in model['scenes'][selected]['nodes']])]
    reference['scene'] = 0
    require(reference['buffers'] == model['buffers'] and reference['meshes'] == model['meshes'],
            'Reference unexpectedly rewrote shared geometry/accessor bytes')
    path.write_text(json.dumps(reference, indent=2), encoding='utf-8')
    return reference, remap


def capture(renderer, output, fixture_path, prefix, technique, records, *, sequence=None, ray=False, animated=True):
    for pattern in (prefix+'*.png', prefix+'*.telemetry.json'):
        for old in output.glob(pattern): old.unlink()
    image, log = output/(prefix+'.png'), output/(prefix+'.log')
    command = [str(renderer), '--model', str(fixture_path), '--hidden', '--no-ui', '--validation',
               '--width', '640', '--height', '360', '--msaa', '1', '--no-taa', '--no-bloom',
               '--environment-intensity', '0', '--directional-intensity', '3', '--directional-direction', '0', '1', '-1',
               '--exposure', '1', '--camera-position', '0', '0', '4', '--camera-target', '0', '0', '0', '--camera-fov', '45',
               '--warmup-frames', '2', '--capture-frame', '3', '--fixed-dt', '.016666667',
               '--render-technique', technique, '--display-mode', 'lit', '--screenshot', str(image)]
    if animated: command += ['--animation-clip', '0', '--animation-time', str(POSE_TIME), '--animation-paused']
    if sequence: command += ['--capture-sequence', str(sequence)]
    if ray:
        command += ['--ray-shadows', 'hard', '--no-ray-shadow-denoise', '--ddgi', '--ddgi-counts', '3', '3', '3',
                    '--ddgi-rays', '4', '--ddgi-probes-per-frame', '27', '--ddgi-ray-budget', '108', '--ddgi-intensity', '0']
    with log.open('w', encoding='utf-8') as stream:
        process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT, timeout=240,
            env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)), creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    text = log.read_text(encoding='utf-8', errors='replace')
    require(process.returncode == 0, f'Native scene deformation capture failed ({process.returncode}): {log}')
    require(not re.search(r'VUID-|SYNC-HAZARD|Validation Error', text, re.I), f'Vulkan validation failed: {log}')
    records_out = []
    for telemetry in sorted(output.glob(prefix+'*.telemetry.json')):
        frame_image = telemetry.with_suffix('').with_suffix('.png')
        state = json.loads(telemetry.read_text(encoding='utf-8'))
        require(frame_image.is_file() and finite_json(state) and state['technique'] == technique, f'Invalid capture: {telemetry}')
        record = dict(image=str(frame_image), telemetry=str(telemetry), log=str(log), command=command)
        records.append(record); records_out.append(dict(**record, state=state))
    require(records_out, f'No fresh captures: {log}')
    return records_out


def assert_state(state, model, spec, owners, selected, ray, *, settled):
    active = active_nodes(model, selected)
    expected = [entry for entry in deformation.evaluate(spec, POSE_TIME)
                if entry['sourceNodeIndex'] in active and entry['deformed']]
    view, gpu = state['gltfScenes'], state['gltfDeformation']
    draws = sum('mesh' in model['nodes'][node] for node in active)
    require(view['selected'] == selected and view['names'] == NAMES, 'Wrong selected scene/catalog')
    require(sorted(view['activeSourceNodes']) == active, 'Stale source nodes after scene switch')
    require(view['activeRenderableCount'] == draws == state['scene']['primaryObjectCount'], 'Stale scene draw count')
    require(state['gltfInstancing']['drawCount'] == draws, 'Provider draw count retained an inactive owner')
    require(gpu['instanceCount'] == len(expected) == len(gpu['instances']), 'Stale deformation instance count')
    require({entry['sourceNodeIndex'] for entry in gpu['instances']} == {entry['sourceNodeIndex'] for entry in expected},
            'A shared source mesh retained another scene owner pose')
    source_vertices = sum(len(mesh['positions']) for mesh in spec['meshes'])
    source_indices = sum(len(mesh['indices']) for mesh in spec['meshes'])
    require(gpu['sourceVertexCount'] == source_vertices, 'Scene switch replaced shared source geometry')
    if not expected:
        require(not gpu['active'] and not gpu['gpuReady'] and not gpu['currentBoundToGpu'] and
                not gpu['previousBoundToGpu'] and gpu['gpuVertexCount'] == 0, 'Empty scene kept an active GPU deformation stream')
    else:
        animation = state['gltfAnimation']
        require(animation['selectedClip'] == 0 and abs(animation['time']-POSE_TIME) < 1e-6 and
                not animation['playing'] and animation['diagnostics'] == 0, 'Paused source pose changed during scene switch')
        require(gpu['active'] and gpu['gpuReady'] and gpu['currentBoundToGpu'] and gpu['previousBoundToGpu'],
                'Active scene did not bind current/previous GPU deformation streams')
        require(gpu['expandedVertexCount'] == source_vertices + sum(len(entry['positions']) for entry in expected),
                'GPU vertex layout retained inactive owners')
        require(gpu['expandedIndexCount'] == source_indices + sum(len(entry['mesh']['indices']) for entry in expected),
                'GPU index layout retained inactive owners')
        ranges = []
        for entry in expected:
            actual = next(item for item in gpu['instances'] if item['sourceNodeIndex'] == entry['sourceNodeIndex'])
            require(actual['sourceNodeIndex'] in owners and actual['boundsValid'], 'Wrong owner or invalid deformed bounds')
            for key, value in [('firstPosition', entry['positions'][0]), ('firstNormal', entry['normals'][0]),
                               ('firstTangent', entry['tangents'][0]), ('modelColumns', entry['model'].T),
                               ('boundsMin', entry['positions'].min(axis=0)), ('boundsMax', entry['positions'].max(axis=0)),
                               ('weights', entry['weights'])]:
                require(np.allclose(actual[key], value, atol=2e-5, rtol=2e-5), f'Stale/wrong {key} for owner {entry["sourceNodeIndex"]}')
            require(actual['vertexCount'] == 24 and actual['vertexOffset'] >= source_vertices and
                    actual['indexCount'] == 36 and actual['firstIndex'] >= source_indices, 'Wrong deformed draw ranges')
            ranges.append((actual['vertexOffset'], actual['vertexOffset'] + actual['vertexCount']))
        require(all(a[1] <= b[0] for a, b in zip(sorted(ranges), sorted(ranges)[1:])), 'Independent owners share writable pose ranges')
        if settled:
            # A scene switch can fit the view, and the next fixed capture-camera
            # application restores it. Samples wait three frames after switching
            # so both current/previous streams must have settled after that cut.
            require(gpu['maxPositionDelta'] < 1e-5 and not gpu['historyReset'],
                    'Paused current/previous streams did not settle after the scene/camera cut')
    if ray:
        shadows, gi = state['rayShadows'], state['ddgi']
        require(shadows['supported'] and gi['supported'], 'Requested production ray/DDGI path unavailable')
        if draws:
            require(shadows['active'] and shadows['build']['instances'] == draws and
                    shadows['build']['blasBuilt'] + shadows['build']['blasReused'] > 0, 'Active scene missing production TLAS/BLAS')
            require(gi['active'] and gi['updatedProbes'] > 0 and gi['tracedRays'] > 0, 'Active scene did not execute DDGI updates')
        else:
            require(not shadows['active'] and shadows.get('build', {}).get('instances', 0) == 0, 'Empty scene retained active/stale TLAS instances')
            require(not gi['active'] and gi['updatedProbes'] == 0 and gi['tracedRays'] == 0, 'Empty scene retained active/stale DDGI updates')
    return dict(scene=selected, activeSourceNodes=active, draws=draws, deformedOwners=len(expected),
                deformationHistoryReset=gpu['historyReset'], maxPositionDelta=gpu['maxPositionDelta'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--case', action='append', choices=CASES)
    parser.add_argument('--technique', action='append', choices=TECHNIQUES)
    parser.add_argument('--ray', action='store_true')
    parser.add_argument('--generate-only', action='store_true')
    parser.add_argument('--excluded-diagnostic', action='append', type=Path, default=[])
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    require(renderer.is_file(), f'Missing renderer: {renderer}')
    (output/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n', encoding='utf-8')
    report = dict(schemaVersion=1, passed=False, nativeValidationPerformed=False, generatedOnly=args.generate_only,
                  renderer=str(renderer), rendererSha256=hashlib.sha256(renderer.read_bytes()).hexdigest(),
                  ray=args.ray, captures=[], checks=[], fixtures=[],
                  expectedCaptureCount=len(args.case or CASES)*len(args.technique or TECHNIQUES)*(4+len(SAMPLES)),
                  expectedCheckCount=len(args.case or CASES)*len(args.technique or TECHNIQUES)*(1+len(SAMPLES)),
                  excludedDiagnostics=[str(path.resolve()) for path in args.excluded_diagnostic],
                  unverified=['GPU picking readback has no native capture action'])
    def save():
        report.update(captureCount=len(report['captures']), checkCount=len(report['checks']))
        (output/'results.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    if args.manifest: report['nativeManifestBefore'] = verify_manifest(args.manifest)
    try:
        for case in args.case or CASES:
            source = output/(case+'-multi-scene.gltf')
            model, spec, owners = fixture(source, case, args.ray)
            expected_poses = [entry for entry in deformation.evaluate(spec, POSE_TIME) if entry['deformed']]
            require(len(expected_poses) == 2 and all(np.isfinite(entry['positions']).all() for entry in expected_poses),
                    'Generated independent owner poses are invalid')
            references = {}
            for selected in range(4):
                path = output/f'{case}-oracle-{selected}.gltf'
                reference, remap = single_scene_reference(model, selected, path)
                references[selected] = (path, reference, remap)
            sequence = output/(case+'-switches.json')
            sequence.write_text(json.dumps(dict(schemaVersion=1, frames=max(SAMPLES), sampleFrames=list(SAMPLES),
                camera=[dict(frame=1, position=[0,0,4], target=[0,0,0])],
                events=[dict(frame=frame-3, gltfScene=selected) for frame, selected in SAMPLES.items() if frame > 1]), indent=2), encoding='utf-8')
            report['fixtures'].append(dict(case=case, source=str(source), references={k:str(v[0]) for k,v in references.items()}, sequence=str(sequence)))
            save()
            if args.generate_only: continue
            for technique in args.technique or TECHNIQUES:
                prefix = f'{case}-{technique}'
                oracles = {selected: capture(renderer, output, path, prefix+f'-oracle-{selected}', technique,
                    report['captures'], ray=args.ray, animated=bool(reference.get('animations')))[0]
                    for selected, (path, reference, _) in references.items()}
                negative = pixels(oracles[0]['image'], oracles[1]['image'])
                require(negative['meanAbsoluteError'] > .25 and negative['changedPixelFraction'] > .003,
                        'Wrong-scene negative reference is visually vacuous')
                report['checks'].append(dict(case=case, technique=technique, check='wrong-scene-negative', **negative))
                captured = capture(renderer, output, source, prefix+'-switches', technique, report['captures'], sequence=sequence, ray=args.ray)
                require(len(captured) == len(SAMPLES), 'Missing switch/revisit/empty captures')
                identity, authoring, epoch = None, None, None
                for record, (frame, selected) in zip(captured, sorted(SAMPLES.items())):
                    state = record['state']
                    semantics = assert_state(state, model, spec, owners, selected, args.ray, settled=frame > 1)
                    view = state['gltfScenes']
                    identity = view['graphIdentity'] if identity is None else identity
                    authoring = view['authoringNodeCount'] if authoring is None else authoring
                    require(identity == view['graphIdentity'] and authoring == view['authoringNodeCount'], 'Scene selection rebuilt the authoring graph')
                    require(epoch is None or state['taa']['epoch'] > epoch, 'Scene switch retained temporal history')
                    epoch = state['taa']['epoch']
                    error = pixels(record['image'], oracles[selected]['image'])
                    require(error['maximumError'] == 0, f'Independent single-scene reference mismatch frame {frame}: {error}')
                    report['checks'].append(dict(case=case, technique=technique, frame=frame, **semantics, **error))
                report['nativeValidationPerformed'] = True
                save(); print('PASS', prefix, 'ray' if args.ray else 'raster', flush=True)
        require(hashlib.sha256(renderer.read_bytes()).hexdigest() == report['rendererSha256'], 'Renderer changed during validation')
        if args.manifest: report['nativeManifestAfter'] = verify_manifest(args.manifest)
        report['passed'] = None if args.generate_only else True
    except Exception as error:
        report['failure'] = str(error)
        raise
    finally: save()
    print('GENERATED' if args.generate_only else 'PASS', report['captureCount'], 'captures', report['checkCount'], 'checks', flush=True)


if __name__ == '__main__':
    main()
