"""Native authored camera lens/history acceptance in both raster renderers.

Pure self-check/generation never launch the renderer. Native evidence requires
an immutable executable/shader/DLL/Python manifest. Literal control assets and
analytical RH/reverse-Z matrices are independent of the production lens planner.
CPU telemetry is checked separately from rendered pixels and backend GPU readback.
"""
from __future__ import annotations

import argparse
import copy
import json
import math
import os
from pathlib import Path
import struct
import sys
import tempfile

import numpy as np
from PIL import Image

import gltf_material_binding_runtime_regression as binding
from gltf_accessor_runtime_regression import Asset, decode_accessor, load_asset, require
from gltf_extended_materials_runtime_regression import sha
from gltf_merge_runtime_regression import document, immutable, portable_copy
from gltf_texture_formats_runtime_regression import verify_manifest
from gltf_tangent_uv_runtime_regression import finite_json
from scene_node_authoring_runtime_regression import history_cursor

TECHNIQUES = binding.TECHNIQUES
WIDTH, HEIGHT = 640, 360
PATHS = ('perspective/yfov', 'perspective/aspectRatio', 'perspective/znear', 'perspective/zfar',
         'orthographic/xmag', 'orthographic/ymag', 'orthographic/znear', 'orthographic/zfar')
NAMES = {1: 'Main camera', 2: 'Shared camera', 4: 'Ortho camera'}
COLORS = [[.85, .05, .025, 1.], [.025, .75, .08, 1.], [.035, .12, .9, 1.], [.9, .65, .025, 1.], [.8, .02, .7, 1.]]
# RGB foreground, a distant yellow far-plane witness, and a near magenta
# witness. The near patch lies 0.15 units from the parented camera's z=6 plane.
QUADS = [(-1.3, .45, .65, .55, 0.), (.95, .4, .7, .4, 0.), (.05, -.85, .55, .35, 0.),
         (6., -2., 4., 3., -30.), (.478, .173, .008, .008, 5.85)]
INDICES = [0, 1, 2, 0, 2, 3]
UNLIT, POINTER = 'KHR_materials_unlit', 'KHR_animation_pointer'
UNVERIFIED = [
    'Pure checker self-checks and generated inputs are not native renderer acceptance.',
    'Camera/source/history telemetry and analytical matrix checks are CPU evidence, not actual camera UBO GPU readback; the separate backend GPU suite covers that boundary.',
    'Native image controls prove visible framing, shared definition selection and lens changes, not complete graphics parity or all temporal consumers.',
    'The headless capture protocol is not native ImGui interaction; separate headless ImGui widget tests exercise the real controls.',
    'Update from view and authored camera pose writeback remain outside this slice.',
    'Portable glTF/GLB here use the CLI graph-save path; coverage of both public backend save APIs belongs to the separate backend tests.']


def close(actual, expected, label, tolerance=3e-5):
    a, b = np.asarray(actual, dtype=float), np.asarray(expected, dtype=float)
    require(a.shape == b.shape and np.isfinite(a).all() and np.isfinite(b).all() and
            np.allclose(a, b, rtol=tolerance, atol=tolerance*.001), 'Incorrect camera '+label)


def camera_records():
    return [dict(name='Shared perspective lens', type='perspective',
                 values=[.8, 0., .1, 0., 0., 0., 0., 0.]),
            dict(name='Independent orthographic lens', type='orthographic',
                 values=[0., 0., 0., 0., 2.4, 1.4, 0., 25.])]


def sampled_fov(time):
    a, b = map(float, np.asarray([.6, 1.], dtype=np.float32))
    return a+(b-a)*(time/2.)


def records(*, edited=False, ortho=False, auto=False, infinite=False, near=False):
    result = camera_records()
    if edited: result[0]['values'][:4] = [.55, 0. if auto else 1.5, .2, 0. if infinite else 20.]
    if near: result[0]['values'][2] = .2
    if ortho: result[1]['values'][4:] = [3., 1.5, 0., 30.]
    return result


def step(label, *, authored=None, selected=1, history=(0, 0), pubs=0,
         time=None, clip=None, resolution=(WIDTH, HEIGHT), scene=0, failure_as=None, unchanged_as=None):
    authored = copy.deepcopy(authored if authored is not None else camera_records())
    evaluated = copy.deepcopy(authored)
    if time is not None and clip == 0: evaluated[0]['values'][0] = sampled_fov(time)
    return dict(label=label, authored=authored, evaluated=evaluated, rests=copy.deepcopy(authored), selected=selected,
                history=list(history), pubs=pubs, time=time, clip=clip, resolution=list(resolution), msaa=1,
                scene=scene, failureAs=failure_as, unchangedAs=unchanged_as)


def lens(source=1, *, expected=None, **values):
    result = dict(sourceNode=source, values=[dict(property=PATHS[int(index)], value=value) for index, value in values.items()])
    if expected is not None: result['expectedCameraRevision'] = expected
    return dict(cameraProjection=result)


def profiles():
    changed, both = records(edited=True), records(edited=True, ortho=True)
    descriptions = [
        (step('before'), None),
        (step('lens', authored=changed, history=(1, 0), pubs=1), lens(**{'0': .55, '1': 1.5, '2': .2, '3': 20.})),
        (step('undo', history=(0, 1), pubs=2), dict(sceneHistory='undo')),
        (step('stale-rejected', history=(0, 1), pubs=2, failure_as='undo'), dict(**lens(expected=9007199254740991, **{'2': .3}), expectCameraProjectionFailure=True)),
        (step('range-rejected', history=(0, 1), pubs=2, failure_as='undo'), dict(**lens(**{'3': .05}), expectCameraProjectionFailure=True)),
        (step('node-rejected', history=(0, 1), pubs=2, failure_as='undo'), dict(cameraSelection=dict(sourceNode=999), expectCameraSelectionFailure=True)),
        (step('name-rejected', history=(0, 1), pubs=2, failure_as='undo'), dict(cameraSelection=dict(nodeName='Ambiguous camera'), expectCameraSelectionFailure=True)),
        (step('redo', authored=changed, history=(1, 0), pubs=3), dict(sceneHistory='redo')),
        (step('shared', authored=changed, selected=2, history=(1, 0), pubs=3), dict(cameraSelection=dict(sourceNode=2))),
        (step('undo-shared', selected=2, history=(0, 1), pubs=4), dict(sceneHistory='undo')),
        (step('redo-shared', authored=changed, selected=2, history=(1, 0), pubs=5), dict(sceneHistory='redo')),
        (step('ortho', authored=changed, selected=4, history=(1, 0), pubs=5), dict(cameraSelection=dict(nodeName='Ortho camera'))),
        (step('ortho-lens', authored=both, selected=4, history=(2, 0), pubs=6), lens(4, **{'4': 3., '5': 1.5, '6': 0., '7': 30.})),
        (step('undo-ortho', authored=changed, selected=4, history=(1, 1), pubs=7), dict(sceneHistory='undo')),
        (step('redo-ortho', authored=both, selected=4, history=(2, 0), pubs=8), dict(sceneHistory='redo')),
        (step('viewport', authored=both, selected=-1, history=(2, 0), pubs=8), dict(cameraSelection=dict(viewport=True))),
        (step('main-again', authored=both, history=(2, 0), pubs=8), dict(cameraSelection=dict(nodeName='Main camera'))),
        (step('fixed-resize', authored=both, history=(2, 0), pubs=8, resolution=(800, 360)), dict(resize=[800, 360])),
        (step('auto-aspect', authored=records(edited=True, ortho=True, auto=True), history=(3, 0), pubs=9, resolution=(800, 360)), lens(**{'1': None})),
        (step('infinite-far', authored=records(edited=True, ortho=True, auto=True, infinite=True), history=(4, 0), pubs=10, resolution=(800, 360)), lens(**{'3': None})),
    ]
    ortho = records(ortho=True)
    ortho_descriptions = [
        (step('before', selected=4), None),
        (step('lens', authored=ortho, selected=4, history=(1, 0), pubs=1), lens(4, **{'4': 3., '5': 1.5, '6': 0., '7': 30.})),
        (step('undo', selected=4, history=(0, 1), pubs=2), dict(sceneHistory='undo')),
        (step('redo', authored=ortho, selected=4, history=(1, 0), pubs=3), dict(sceneHistory='redo')),
        (step('scene-switch', authored=ortho, selected=-1, pubs=3, scene=1), dict(gltfScene=1)),
        (step('scene-camera', authored=ortho, selected=4, pubs=3, scene=1), dict(cameraSelection=dict(nodeName='Ortho camera'))),
    ]
    near = records(near=True)
    animated_default = copy.deepcopy(near)
    animated_default[0]['values'][0] = .55
    animation = [
        (step('before', time=.5, clip=0), None),
        (step('near', authored=near, history=(1, 0), pubs=1, time=.5, clip=0), lens(**{'2': .2})),
        (step('seek', authored=near, history=(1, 0), pubs=1, time=1.5, clip=0), dict(animationTime=1.5)),
        (step('undo-current-time', history=(0, 1), pubs=2, time=1.5, clip=0), dict(sceneHistory='undo')),
        (step('redo-current-time', authored=near, history=(1, 0), pubs=3, time=1.5, clip=0), dict(sceneHistory='redo')),
        (step('animated-default', authored=animated_default, history=(2, 0), pubs=4, time=1.5, clip=0,
              unchanged_as='redo-current-time'), lens(**{'0': .55})),
        (step('seek-animated-default', authored=animated_default, history=(2, 0), pubs=4, time=.75, clip=0),
         dict(animationTime=.75)),
        (step('undo-animated-current-time', authored=near, history=(1, 1), pubs=5, time=.75, clip=0,
              unchanged_as='seek-animated-default'), dict(sceneHistory='undo')),
        (step('redo-animated-current-time', authored=animated_default, history=(2, 0), pubs=6, time=.75, clip=0,
              unchanged_as='seek-animated-default'), dict(sceneHistory='redo')),
        (step('clear-clip', authored=animated_default, history=(2, 0), pubs=6, time=0., clip=-1), dict(animationClip=-1)),
        (step('undo-animated-rest', authored=near, history=(1, 1), pubs=7, time=0., clip=-1), dict(sceneHistory='undo')),
        (step('redo-animated-rest', authored=animated_default, history=(2, 0), pubs=8, time=0., clip=-1), dict(sceneHistory='redo')),
    ]
    runs = []
    for name, values, selected, extra in [('shared-perspective', descriptions, 1, dict(save='gltf')),
            ('orthographic-scene', ortho_descriptions, 4, {}),
            ('animated-rest', animation, 1, dict(animated=True, save='glb')),
            ('animated-rest-gltf', animation, 1, dict(animated=True, save='gltf'))]:
        steps, events = [], [dict(frame=1, cameraSelection=dict(sourceNode=selected))]
        for index, (expected, event) in enumerate(copy.deepcopy(values)):
            expected['frame'] = 13*(index+1); steps.append(expected)
            if event is not None: events.append(dict(frame=13*index+1, **event))
        runs.append(dict(name=name, steps=steps, events=events, **extra))
    return runs


def quat_z(degrees):
    angle = math.radians(degrees)/2
    return [0., 0., math.sin(angle), math.cos(angle)]


def positions(index):
    x, y, w, h, z = QUADS[index]
    return [[x-w, y-h, z], [x+w, y-h, z], [x+w, y+h, z], [x-w, y+h, z]]


def camera_json(record):
    v = record['values']; result = dict(name=record['name'], type=record['type'],
        extras=dict(cameraEditor='opaque camera metadata', integer=16777217, indexLike=991))
    if record['type'] == 'perspective':
        lens = dict(yfov=v[0], znear=v[2], extras=dict(lens='preserve perspective'))
        if v[1] != 0: lens['aspectRatio'] = v[1]
        if v[3] != 0: lens['zfar'] = v[3]
        result['perspective'] = lens
    else: result['orthographic'] = dict(xmag=v[4], ymag=v[5], znear=v[6], zfar=v[7], extras=dict(lens='preserve orthographic'))
    return result


def write_fixture(path, *, cameras=None, animated=False):
    asset = Asset(); asset.model['materials'] = [binding.material('Surface '+str(i), c, sided=True) for i, c in enumerate(COLORS)]
    meshes = []
    for index in range(len(QUADS)):
        attrs = dict(POSITION=asset.accessor('VEC3', positions(index)), NORMAL=asset.accessor('VEC3', [[0., 0., 1.]]*4),
                     TANGENT=asset.accessor('VEC4', [[1., 0., 0., 1.]]*4))
        meshes.append(dict(name='Geometry '+str(index), primitives=[dict(attributes=attrs,
            indices=asset.accessor('SCALAR', INDICES, 5123, indices=True), material=index, mode=4)], extras=dict(geometryIdentity=index)))
    asset.model['meshes'] = meshes
    asset.model['cameras'] = [camera_json(c) for c in cameras or camera_records()]
    asset.model['nodes'] = [
        dict(name='Perspective rolled parent', translation=[.3, .2, 6.], rotation=quat_z(25), children=[1, 2], extras=dict(parentIdentity=101)),
        dict(name='Main camera', camera=0, translation=[.15, -.1, 0.], extras=dict(nodeIdentity=102)),
        dict(name='Shared camera', camera=0, translation=[-.8, .3, 0.], extras=dict(nodeIdentity=103)),
        dict(name='Orthographic rolled parent', translation=[-.2, -.15, 6.], rotation=quat_z(-18), children=[4], extras=dict(parentIdentity=104)),
        dict(name='Ortho camera', camera=1, translation=[.15, .1, 0.], extras=dict(nodeIdentity=105)),
        dict(name='Ambiguous camera', camera=1, translation=[0., 0., 6.]),
        dict(name='Ambiguous camera', camera=1, translation=[.2, 0., 6.]),
        *[dict(name='Surface '+str(i), mesh=i) for i in range(len(QUADS))]]
    surfaces = list(range(7, 7+len(QUADS)))
    asset.model['scenes'] = [dict(name='All camera definitions', nodes=[0, 3, 5, 6, *surfaces]),
                             dict(name='Orthographic scene', nodes=[3, 5, 6, *surfaces])]
    asset.model['scene'] = 0; asset.model['extensionsUsed'] = [UNLIT]
    if animated:
        times = asset.accessor('SCALAR', [0., 2.]); fov = asset.accessor('SCALAR', [.6, 1.]); roughness = asset.accessor('SCALAR', [.2, .8])
        asset.model['animations'] = [dict(name='Camera and unrelated material', extras=dict(curveMetadata='retain original keys'),
            samplers=[dict(input=times, output=fov, interpolation='LINEAR'), dict(input=times, output=roughness, interpolation='LINEAR')],
            channels=[dict(sampler=0, target=dict(path='pointer', extensions={POINTER: dict(pointer='/cameras/0/perspective/yfov')})),
                      dict(sampler=1, target=dict(path='pointer', extensions={POINTER: dict(pointer='/materials/0/pbrMetallicRoughness/roughnessFactor')}))])]
        asset.model['extensionsUsed'].append(POINTER)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='camera-projection-fixture-') as directory:
        temp = Path(directory)/'fixture.gltf'; asset.write(temp); immutable(path, temp.read_bytes())


def visual_key(expected):
    selected = expected['selected']; camera = None if selected < 0 else expected['evaluated'][0 if selected in (1, 2) else 1]
    # Scene switching clears the initial mesh selection/outline. Literal
    # controls must reproduce that context as well as the camera lens/pose.
    return json.dumps(dict(selected=selected, camera=camera, resolution=expected['resolution'], scene=expected['scene']), sort_keys=True)


def literal_events(expected):
    events = [dict(frame=1, gltfScene=expected['scene'])] if expected['scene'] else []
    if expected['selected'] >= 0:
        events.append(dict(frame=2 if expected['scene'] else 1,
                           cameraSelection=dict(nodeName=NAMES[expected['selected']])))
    return events


def check_literal_context(expected, control):
    require(visual_key(expected) == visual_key(control['step']), 'Literal camera scene/selection context changed')
    script = json.loads(Path(control['script']).read_text())
    require(script['events'] == literal_events(control['step']), 'Literal control lost its scene-switch/selection ordering')


def generate(output):
    output.mkdir(parents=True, exist_ok=True)
    sources = dict(source=output/'sources/scene.gltf', animated=output/'sources/animated.gltf')
    write_fixture(sources['source']); write_fixture(sources['animated'], animated=True)
    runs = profiles(); controls, by_key, inputs = [], {}, list(sources.values())
    for run in runs:
        for expected in run['steps']:
            key = visual_key(expected)
            if key not in by_key:
                name = 'literal-'+str(len(controls)); path = output/'sources'/(name+'.gltf')
                write_fixture(path, cameras=expected['evaluated'])
                control = copy.deepcopy(expected); control.update(label='control', frame=13, history=[0, 0], pubs=0, time=None, clip=None, failureAs=None)
                control['authored'] = copy.deepcopy(expected['evaluated']);control['rests'] = copy.deepcopy(expected['evaluated'])
                script = output/'scripts'/(name+'.json')
                events = literal_events(expected)
                document(script, dict(schemaVersion=1, frames=13, sampleFrames=[13], camera=[dict(frame=1, position=[0., 0., 6.], target=[0., 0., 0.])], events=events))
                controls.append(dict(name=name, source=str(path), script=str(script), step=control)); by_key[key] = name
                inputs += [path, script]
            expected['control'] = by_key[key]
            check_literal_context(expected, next(c for c in controls if c['name'] == expected['control']))
        script = output/'scripts'/(run['name']+'.json')
        document(script, dict(schemaVersion=1, frames=run['steps'][-1]['frame'], sampleFrames=[s['frame'] for s in run['steps']],
            camera=[dict(frame=1, position=[0., 0., 6.], target=[0., 0., 0.])], events=run['events']))
        run['script'] = str(script); inputs.append(script)
        if 'save' in run:
            reload_script = output/'scripts'/(run['name']+'-reload.json')
            expected = copy.deepcopy(run['steps'][-1])
            expected.update(label='reload', frame=13, pubs=0, history=[0, 0], unchangedAs=None)
            reload_steps = [expected]
            reload_events = [dict(frame=1, cameraSelection=dict(nodeName=NAMES[expected['selected']]))]
            if run.get('animated'):
                posed = copy.deepcopy(expected)
                posed.update(label='reload-curve', frame=26, clip=0, time=.75)
                posed['evaluated'][0]['values'][0] = sampled_fov(.75)
                restored = copy.deepcopy(expected)
                restored.update(label='reload-rest', frame=39)
                reload_steps += [posed, restored]
                reload_events += [dict(frame=14, animationClip=0, animationTime=.75, animationPlaying=False),
                                  dict(frame=27, animationClip=-1)]
            run['reloadSteps'] = reload_steps
            document(reload_script, dict(schemaVersion=1, frames=reload_steps[-1]['frame'],
                sampleFrames=[s['frame'] for s in reload_steps],
                camera=[dict(frame=1, position=[0., 0., 6.], target=[0., 0., 0.])],
                events=reload_events))
            run['reloadScript'] = str(reload_script); inputs.append(reload_script)
    count = len(controls)+sum(len(r['steps'])+len(r.get('reloadSteps', [])) for r in runs)
    image_checks = count+sum(bool(s['failureAs'])+bool(s['unchangedAs']) for r in runs for s in r['steps'])+8
    plan = dict(schemaVersion=1, sources={k: str(v) for k, v in sources.items()}, controls=controls, runs=runs,
        expectedCaptureCount=count, expectedProcessCount=len(controls)+len(runs)+sum('save' in r for r in runs),
        expectedImageCheckCount=image_checks, expectedExportCount=sum('save' in r for r in runs),
        immutableInputs=[dict(path=str(p), byteLength=p.stat().st_size, sha256=sha(p)) for p in inputs])
    document(output/'launch-plan.json', plan); return plan


def camera_world(selected):
    if selected in (1, 2): angle, parent, child = 25, [.3, .2, 6.], [.15, -.1, 0.] if selected == 1 else [-.8, .3, 0.]
    else: angle, parent, child = -18, [-.2, -.15, 6.], [.15, .1, 0.]
    a = math.radians(angle); rotation = np.array([[math.cos(a), -math.sin(a), 0.], [math.sin(a), math.cos(a), 0.], [0., 0., 1.]])
    world = np.eye(4); world[:3, :3] = rotation; world[:3, 3] = np.asarray(parent)+rotation@np.asarray(child)
    return world


def projection_matrix(camera, resolution, shadow=False):
    v = camera['values']; m = np.zeros((4, 4))
    if camera['type'] == 'perspective':
        aspect = v[1] if v[1] else resolution[0]/resolution[1]; near, far = v[2:4]
        scale = 1/math.tan(v[0]/2); m[0, 0] = scale/aspect; m[1, 1] = scale; m[3, 2] = -1
        if far == 0 and not shadow: m[2, 3] = near
        else:
            if far == 0: far = max(1e6, 2*near)
            m[2, 2] = near/(far-near); m[2, 3] = far*near/(far-near)
    else:
        x, y, near, far = v[4:]; m[0, 0] = 1/x; m[1, 1] = 1/y; m[2, 2] = 1/(far-near); m[2, 3] = far/(far-near); m[3, 3] = 1
    return m


def graph_rows(state):
    merge = state['gltfMerge']; require(not merge['authoringNodesTruncated'], 'Truncated authored camera graph telemetry')
    rows = merge['authoringNodes']; require(len(rows) == merge['graphNodeCount'] and len(rows) <= 4096, 'Incomplete authored graph')
    return rows


def retained_signature(state):
    merge = state['gltfMerge']
    return dict(graph=merge['graphIdentity'], nodes=graph_rows(state), tables=merge['sourceTables'],
                vertices=state['scene']['primaryVertexCount'], indices=state['scene']['primaryIndexCount'])


def failure_signature(state):
    keys = ('gltfCameraAuthoring', 'gltfAnimation', 'gltfScenes', 'gltfMaterialVariants', 'gltfMaterialBindings', 'gltfResourceCatalog', 'gltfMerge')
    result = {key: copy.deepcopy(state[key]) for key in keys if key in state}
    result['temporal'] = {key: state['taa'][key] for key in ('epoch', 'resetReason')}
    return result


def temporal_check(states, steps):
    previous = None
    for expected in steps:
        state = states[expected['label']]['state'];epoch = state['taa']['epoch']
        require(type(epoch) is int and epoch >= 0, 'Missing temporal discontinuity epoch')
        if expected['failureAs']:
            old = states[expected['failureAs']]['state']['taa']
            require(epoch == old['epoch'] and state['taa']['resetReason'] == old['resetReason'], 'Failed camera request reset temporal state')
        elif previous is not None: require(epoch > previous, 'Camera edit/history/selection/scene/timeline did not reset temporal compatibility')
        previous = epoch


def check_state(state, expected, baseline=None, *, reload=False):
    require(finite_json(state), 'Nonfinite camera telemetry')
    value = state['gltfCameraAuthoring']; require(not value['truncated'], 'Truncated camera source/identity telemetry')
    require(type(value['asset']) is int and value['asset'] > 0 and type(value['revision']) is int and value['revision'] > 0,
            'Missing camera asset/revision authority')
    require(value['publications'] == expected['pubs'], 'Wrong camera publication count')
    for domain in ('authored', 'evaluated', 'rests'):
        require(len(value[domain]) == len(expected[domain]) == 2, 'Lost camera definitions or rest rows')
        for actual, wanted in zip(value[domain], expected[domain]):
            require(actual['name'] == wanted['name'] and actual['type'] == wanted['type'], 'Camera record identity/type changed')
            close(actual['values'], wanted['values'], domain+' lens values')
    rows = graph_rows(state); selected = expected['selected']
    if reload and selected >= 0:
        matching = [r for r in rows if r['name'] == NAMES[selected] and r['active'] and not r['retired'] and r['sourceNode'] >= 0]
        require(len(matching) == 1, 'Portable selected camera name was not resolved uniquely')
        selected_source = matching[0]['sourceNode']
    else: selected_source = selected
    require(value['selectedSourceNode'] == selected_source and state['gltfAnimation']['selectedCameraNode'] == selected_source,
            'Camera edit/history changed current viewport selection')
    history_cursor(state, *expected['history'], allow_uncreated=expected['history'] == [0, 0])
    require(state['gltfScenes']['selected'] == expected['scene'], 'Camera/scene switch selected the wrong source scene')
    require(state['resolution'] == expected['resolution'] and state['msaaSamples'] == 1, 'Wrong camera capture dimensions')
    require(state['scene']['primaryObjectCount'] == len(QUADS), 'Camera edit lost scene primitives')
    if expected['time'] is not None:
        player = state['gltfAnimation']; require(player['selectedClip'] == expected['clip'] and not player['playing'] and player['loop'], 'Camera edit changed animation controls')
        close(player['time'], expected['time'], 'current clock');close(player['speed'], 1., 'playback speed')
        if expected['clip'] == 0 and not reload:
            materials = state['gltfMaterialBindings']['materials']; first = next(m for m in materials if m['sourceMaterial'] == 0)
            a, b = map(float, np.asarray([.2, .8], dtype=np.float32)); rough = a+(b-a)*expected['time']/2
            close(first['evaluated']['pbrMetallicRoughness']['roughnessFactor'], rough, 'unrelated animated material')
    mappings = value['cameras']; require(len({r['graphNode'] for r in mappings}) == len(mappings), 'Duplicate camera graph identities')
    for row in mappings:
        require(all(type(row[k]) is int and row[k] > 0 for k in ('sourceNamespace', 'incarnation', 'graphIdentity')),
                'Camera binding lacks namespace/incarnation proof')
        require(0 <= row['authoredCamera'] < 2 and 0 <= row['evaluatedCamera'] < 2, 'Invalid explicit A/E camera mapping')
    if not reload and expected['scene'] == 0:
        shared = [r for r in mappings if r['sourceNode'] in (1, 2)]
        require(len(shared) == 2 and len({(r['sourceNamespace'], r['incarnation'], r['authoredCamera'], r['evaluatedCamera']) for r in shared}) == 1,
                'Shared camera definitions were duplicated or confused with node identity')
    if selected >= 0:
        binding_rows = [r for r in mappings if r['sourceNode'] == selected_source]; require(len(binding_rows) == 1, 'Selected camera has no exact graph binding')
        current = next(r for r in rows if r['graphNode'] == binding_rows[0]['graphNode'])
        world = camera_world(selected);close(np.asarray(current['worldColumns']).T, world, 'parented rolled world pose')
        camera = expected['evaluated'][0 if selected in (1, 2) else 1]
        close(value['view'], np.linalg.inv(world).T.reshape(-1), 'selected view')
        close(value['projection'], projection_matrix(camera, expected['resolution']).T.reshape(-1), 'reverse-Z projection')
        close(value['shadowProjection'], projection_matrix(camera, expected['resolution'], True).T.reshape(-1), 'shadow projection')
        require(np.linalg.det(np.asarray(value['projection']).reshape(4, 4)) != 0, 'Camera projection is singular')
    else: require(not any(k in value for k in ('view', 'projection', 'shadowProjection')), 'Viewport selection retained an old authored camera view')
    if baseline is not None:
        old = baseline['gltfCameraAuthoring'];require(value['asset'] == old['asset'], 'Camera operation replaced its asset')
        require(value['revision'] == old['revision']+expected['pubs'], 'Authored revision advanced on seek/selection or skipped a lens publication')
        old_bindings = {r['sourceNode']: r for r in old['cameras']}
        identity_fields = ('graphNode', 'sourceNode', 'authoredCamera', 'evaluatedCamera', 'sourceNamespace', 'incarnation', 'graphIdentity')
        for row in mappings:
            previous = old_bindings.get(row['sourceNode']);require(previous is not None and
                all(row[k] == previous[k] for k in identity_fields), 'Lens edit/history/selection replaced a camera row namespace or identity')
        if expected['scene'] == 0: require(retained_signature(state) == retained_signature(baseline), 'Lens edit changed graph/geometry/resource source tables')
        else:
            for field in ('primaryVertexCount', 'primaryIndexCount'): require(state['scene'][field] == baseline['scene'][field], 'Scene switch changed retained geometry')
            require(state['gltfMerge']['sourceTables'] == baseline['gltfMerge']['sourceTables'], 'Scene switch changed retained source tables')
    return dict(passed=True, selectedSourceNode=selected_source, publications=value['publications'], authoredRevision=value['revision'],
                history=expected['history'], temporalEpoch=state['taa']['epoch'],
                evidence='CPU selected A/E/rest identities, graph pose, source lens and analytical camera matrices')


def pixels(record):
    array = np.asarray(Image.open(record['image']).convert('RGB'), dtype=np.int16)
    w, h = record['resolution'];require(array.shape == (h, w, 3), 'Wrong camera image extent');return array


def compare_images(actual, reference):
    a, b = pixels(actual), pixels(reference);require(a.shape == b.shape, 'Camera control extent mismatch')
    maximum = int(np.abs(a-b).max(initial=0));require(maximum == 0, 'Visible framing differs from independently authored literal camera control')
    return dict(passed=True, maximumByteDelta=maximum, pixelCount=int(a.shape[0]*a.shape[1]), evidence='Complete decoded native RGB raster pixels')


def visible(record):
    a = pixels(record);r, g, b = a[..., 0], a[..., 1], a[..., 2]
    counts = [int(np.count_nonzero((r > g+20) & (r > b+20))), int(np.count_nonzero((g > r+20) & (g > b+20))),
              int(np.count_nonzero((b > r+20) & (b > g+20)))]
    require(min(counts) > 50, 'Literal camera control is blank/offscreen or missing asymmetric RGB surfaces')
    return dict(passed=True, dominantRgbPixelCounts=counts, evidence='Independent positive native visibility control')


def changed(before, after):
    a, b = pixels(before), pixels(after);require(a.shape == b.shape, 'Positive framing comparison changed extent')
    delta = np.abs(a-b);count = int(np.count_nonzero(delta.max(axis=2) > 5));mean = float(delta.mean())
    require(count > 300 and mean > .3, 'Camera lens/selection positive pixel witness is vacuous')
    return dict(passed=True, changedPixels=count, meanByteDelta=mean, evidence='Native nonvacuous framing change')


def local_matrix(node):
    if 'matrix' in node: return np.asarray(node['matrix'], dtype=float).reshape(4, 4).T
    x, y, z, w = node.get('rotation', [0., 0., 0., 1.]);r = np.array([
        [1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
        [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
        [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]])
    m = np.eye(4);m[:3, :3] = r@np.diag(node.get('scale', [1., 1., 1.]));m[:3, 3] = node.get('translation', [0., 0., 0.]);return m


def inspect_export(path, expected, *, animated=False):
    model, buffers = load_asset(path);require(finite_json(model), 'Nonfinite serialized camera asset')
    cameras = model.get('cameras', []);require(len(cameras) == 2, 'Portable export lost camera definitions')
    for actual, wanted in zip(cameras, expected['authored']):
        require(actual['name'] == wanted['name'] and actual['type'] == wanted['type'], 'Serialized camera identity/type changed')
        require(actual.get('extras') == camera_json(wanted)['extras'], 'Serialized camera opaque metadata changed')
        if wanted['type'] == 'perspective':
            lens = actual['perspective'];require(all(k in lens for k in ('yfov', 'znear')), 'Missing required perspective fields')
            close([lens['yfov'], lens.get('aspectRatio', 0.), lens['znear'], lens.get('zfar', 0.)], wanted['values'][:4], 'serialized perspective')
            require((wanted['values'][1] != 0 or 'aspectRatio' not in lens) and (wanted['values'][3] != 0 or 'zfar' not in lens), 'Optional camera omission was materialized')
        else:
            lens = actual['orthographic'];require(all(k in lens for k in ('xmag', 'ymag', 'znear', 'zfar')), 'Missing required orthographic fields')
            close([lens[k] for k in ('xmag', 'ymag', 'znear', 'zfar')], wanted['values'][4:], 'serialized independent orthographic magnitudes')
        require(lens.get('extras') == camera_json(wanted)[wanted['type']]['extras'], 'Serialized lens metadata changed')
    nodes = model['nodes'];require(len(nodes) <= 64 and len(model['scenes']) == 2, 'Portable camera hierarchy/scenes changed')
    # The source fixture binds each mesh directly to its named Surface owner.
    # The production exporter retains that owner and emits its single runtime
    # primitive as an identity child. Resolve only these two exact forms.
    owners = {};geometry_nodes = {};incoming = [[] for _ in nodes]
    for parent_index, node in enumerate(nodes):
        for child in node.get('children', []):
            require(type(child) is int and 0 <= child < len(nodes), 'Invalid exported child reference')
            incoming[child].append(parent_index)
    canonical_count = 0
    for identity in range(len(QUADS)):
        name = 'Surface '+str(identity);found = [i for i, node in enumerate(nodes) if node.get('name') == name]
        require(len(found) == 1, 'Export lost or duplicated a geometry owner')
        owner_index = found[0];owner = nodes[owner_index];owners[identity] = owner_index
        require(not incoming[owner_index] and 'camera' not in owner, 'Export reparented a geometry owner')
        close(local_matrix(owner), np.eye(4), 'portable geometry owner transform', 0.)
        if 'mesh' in owner:
            require(not owner.get('children'), 'Flat geometry owner acquired children')
            mesh_node = owner_index
        else:
            children = owner.get('children', []);require(len(children) == 1, 'Canonical geometry owner needs exactly one primitive')
            mesh_node = children[0];child = nodes[mesh_node];canonical_count += 1
            require(child.get('name') == 'Primitive '+str(identity) and 'mesh' in child and
                    not child.get('children') and 'camera' not in child and incoming[mesh_node] == [owner_index],
                    'Canonical geometry primitive identity/parent changed')
            require(all(mesh_node not in scene['nodes'] for scene in model['scenes']), 'Primitive became an independent scene root')
            close(local_matrix(child), np.eye(4), 'portable primitive local transform', 0.)
        geometry_nodes[mesh_node] = identity
        for scene in model['scenes']:
            require(scene['nodes'].count(owner_index) == 1, 'Scene lost or duplicated a geometry owner root')
    require(canonical_count in (0, len(QUADS)), 'Export mixed geometry owner representations')
    require(len(model.get('meshes', [])) == len(QUADS) and
            {i for i, node in enumerate(nodes) if 'mesh' in node} == set(geometry_nodes), 'Export added or lost a geometry primitive')
    require({i for i, node in enumerate(nodes) if node.get('name', '').startswith('Surface ')} == set(owners.values()),
            'Export added or renamed a geometry owner')
    require({i for i, node in enumerate(nodes) if node.get('name', '').startswith('Primitive ')} ==
            (set(geometry_nodes) if canonical_count else set()), 'Export added or renamed a geometry primitive')
    editor_roots = [i for i, node in enumerate(nodes) if node.get('name') == 'Scene Root']
    require(len(editor_roots) <= 1, 'Export duplicated the optional editor root')
    for index in editor_roots:
        require(not incoming[index] and not nodes[index].get('children') and 'mesh' not in nodes[index] and
                'camera' not in nodes[index] and all(index not in scene['nodes'] for scene in model['scenes']),
                'Optional editor root became an authored scene/geometry owner')
        close(local_matrix(nodes[index]), np.eye(4), 'portable optional editor root', 0.)
    worlds = {};parents = {};geometry_materials = {}
    for scene in model['scenes']:
        stack = [(i, np.eye(4), None) for i in scene['nodes']];seen = set();scene_geometry = set()
        while stack:
            index, parent, parent_index = stack.pop();require(type(index) is int and 0 <= index < len(nodes) and index not in seen, 'Invalid/cyclic exported scene forest')
            seen.add(index);node = nodes[index];world = parent@local_matrix(node)
            if node.get('name') in NAMES.values():
                if node['name'] in worlds: close(worlds[node['name']], world, 'cross-scene camera world')
                worlds[node['name']] = world;parents[node['name']] = nodes[parent_index]['name'] if parent_index is not None else None
                selected = next(k for k, v in NAMES.items() if v == node['name']);require(node.get('camera') == (0 if selected in (1, 2) else 1), 'Camera association lost sharing')
                require(node.get('extras') == dict(nodeIdentity={1: 102, 2: 103, 4: 105}[selected]), 'Camera node metadata changed')
            if 'camera' in node: require(type(node['camera']) is int and 0 <= node['camera'] < len(cameras), 'Invalid serialized node.camera')
            if 'mesh' in node:
                require(type(node['mesh']) is int and 0 <= node['mesh'] < len(model['meshes']), 'Invalid exported mesh reference')
                mesh = model['meshes'][node['mesh']]
                identity = geometry_nodes[index];require(identity not in scene_geometry, 'Scene duplicated a geometry primitive');scene_geometry.add(identity)
                close(world, np.eye(4), 'portable geometry world transform', 0.)
                require(mesh.get('name') == ('Primitive ' if canonical_count else 'Geometry ')+str(identity) and
                        mesh.get('extras') == dict(geometryIdentity=identity) and len(mesh['primitives']) == 1, 'Serialized geometry identity changed')
                primitive = mesh['primitives'][0];require(primitive.get('mode', 4) == 4 and
                    type(primitive['material']) is int and 0 <= primitive['material'] < len(model['materials']), 'Invalid material/primitive reference')
                close(decode_accessor(path, primitive['attributes']['POSITION']), np.asarray(positions(identity), dtype=np.float32).astype(float), 'portable geometry positions', 0.)
                close(decode_accessor(path, primitive['attributes']['NORMAL']), [[0., 0., 1.]]*4, 'portable geometry normals', 0.)
                close(decode_accessor(path, primitive['attributes']['TANGENT']), [[1., 0., 0., 1.]]*4, 'portable geometry tangents', 0.)
                close(np.asarray(decode_accessor(path, primitive['indices'])).reshape(-1), INDICES, 'portable geometry indices', 0.)
                binding.inspect_record(model['materials'][primitive['material']],binding.material('Surface '+str(identity),COLORS[identity],sided=True),serialized=True)
                require(identity not in geometry_materials or geometry_materials[identity] == primitive['material'],
                        'Cross-scene geometry material association changed')
                geometry_materials[identity] = primitive['material']
                for accessor in [*primitive['attributes'].values(), primitive['indices']]: require(type(accessor) is int and 0 <= accessor < len(model['accessors']), 'Invalid accessor reference')
            stack.extend((child, world, index) for child in node.get('children', []))
        require(scene_geometry == set(range(len(QUADS))), 'Scene lost a geometry primitive')
    require(set(geometry_materials) == set(range(len(QUADS))) and len(set(geometry_materials.values())) == len(QUADS),
            'Portable geometry lost its distinct material identities')
    for identity, material in geometry_materials.items():
        require([i for i, row in enumerate(model['materials']) if row.get('name') == 'Surface '+str(identity)] == [material],
                'Portable material identity is missing, duplicated or detached from its geometry owner')
    require(set(worlds) == set(NAMES.values()), 'Portable export lost a camera node')
    for selected, name in NAMES.items():
        close(worlds[name], camera_world(selected), 'portable parented roll/translation')
        require(parents[name] == ('Perspective rolled parent' if selected in (1, 2) else 'Orthographic rolled parent'), 'Portable camera parent link changed')
    for accessor in model.get('accessors', []):
        if 'bufferView' in accessor: require(0 <= accessor['bufferView'] < len(model['bufferViews']), 'Invalid accessor bufferView')
    for view in model.get('bufferViews', []):
        require(0 <= view['buffer'] < len(buffers) and view.get('byteOffset', 0)+view['byteLength'] <= len(buffers[view['buffer']]), 'Invalid serialized buffer range')
    clips = model.get('animations', []);require(len(clips) == int(animated), 'Portable animation inventory changed')
    if animated:
        clip = clips[0];require(len(clip['channels']) == 2 and clip.get('extras') == dict(curveMetadata='retain original keys'), 'Portable camera/unrelated animation metadata changed')
        # The shared exporter projects source material IDs into its runtime
        # material table. Resolve the original Surface 0 through the exact
        # geometry/material record already checked above, never a numeric guess.
        targets = ['/cameras/0/perspective/yfov', '/materials/'+str(geometry_materials[0])+'/pbrMetallicRoughness/roughnessFactor']
        for channel, target, samples in zip(clip['channels'], targets, [[.6, 1.], [.2, .8]]):
            require(channel['target']['extensions'][POINTER]['pointer'] == target, 'Portable animation target was rebound')
            require(0 <= channel['sampler'] < len(clip['samplers']), 'Invalid animation sampler reference');sampler = clip['samplers'][channel['sampler']]
            require(sampler.get('interpolation', 'LINEAR') == 'LINEAR', 'Portable interpolation changed')
            close(np.asarray(decode_accessor(path, sampler['input'])).reshape(-1), [0., 2.], 'portable animation times', 0.)
            close(np.asarray(decode_accessor(path, sampler['output'])).reshape(-1), np.asarray(samples, dtype=np.float32).astype(float), 'portable unchanged curve keys', 0.)
    return dict(passed=True, path=str(path), sha256=sha(path), cameraCount=2, sharedDefinitions=True,
                parentedRollPreserved=True, geometryPreserved=True, animated=animated)


def dependencies(): return binding.dependencies()


def run_native(args, output, plan, report):
    require(args.renderer.is_file() and args.manifest.is_file(), 'Missing camera renderer/frozen manifest')
    require(not any(output.glob('captures/*')) and not any(output.glob('exports/**/*')) and not any(output.glob('portable/**/*')), 'Camera native evidence directories must be fresh')
    if (output/'results.json').exists():
        old = json.loads((output/'results.json').read_text());require(old.get('generatedOnly') and not old.get('rendererExecuted'), 'Refusing to overwrite native camera evidence')
    manifest_hash = sha(args.manifest)
    def persist():
        for key in ('captures', 'processes', 'stateChecks', 'imageChecks', 'exports'): report[key+'Count'] = len(report[key])
        (output/'results.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    def runtime_files(): return sorted([*args.renderer.parent.glob('*.dll'), *(args.renderer.parent/'spv_shaders').glob('*.spv')])
    def verify():
        require(sha(args.manifest) == manifest_hash and dependencies() == report['sourceDependencies'], 'Frozen camera manifest/helper changed')
        result = verify_manifest(args.manifest)
        require(sha(output/'launch-plan.json') == report['launchPlanSha256'], 'Camera launch plan changed')
        require((output/'vk_layer_settings.txt').read_bytes() == b'khronos_validation.validate_sync = true\n', 'Camera synchronization-validation settings changed')
        for entry in plan['immutableInputs']:
            p = Path(entry['path']);require(p.stat().st_size == entry['byteLength'] and sha(p) == entry['sha256'], 'Camera fixture/script changed')
        if 'runtimeInputs' in report: require([str(p.resolve()) for p in runtime_files()] == report['runtimeInputs'], 'Resident camera DLL/shader inventory changed')
        for copied in report['portableCopies']:
            require(all(sha(Path(f['path'])) == f['sha256'] for f in copied['files']), 'Portable camera export changed')
        return result
    def check(kind, technique, name, result): report[kind].append(dict(technique=technique, check=name, **result));persist()
    try:
        report['nativeManifestBefore'] = verify_manifest(args.manifest)
        frozen = {Path(p).resolve() for p in json.loads(args.manifest.read_text(encoding='utf-8-sig'))['files']}
        runtime = runtime_files();require(any(p.suffix == '.spv' for p in runtime), 'No resident camera shader inputs')
        require({args.renderer, *map(Path, report['sourceDependencies']), *map(Path.resolve, runtime)} <= frozen, 'Camera manifest omits renderer/helper/DLL/SPIR-V inputs')
        report.update(rendererSha256=sha(args.renderer), runtimeInputs=[str(p.resolve()) for p in runtime])
        immutable(output/'vk_layer_settings.txt', b'khronos_validation.validate_sync = true\n');persist();verify()
        for technique in args.technique:
            controls = {}
            for control in plan['controls']:
                expected = control['step'];record = binding.launch(args, output, report, persist, verify, technique, control['name'],
                    Path(control['source']), [expected], script=Path(control['script']), initial=expected)['control']
                check('stateChecks', technique, control['name'], check_state(record['state'], expected))
                check('imageChecks', technique, control['name']+'-visible', visible(record));controls[control['name']] = record
            cohorts = {}
            for run in plan['runs']:
                source = Path(plan['sources']['animated' if run.get('animated') else 'source'])
                save = output/'exports'/technique/run['name']/('scene.'+run['save']) if 'save' in run else None
                captures = binding.launch(args, output, report, persist, verify, technique, run['name'], source, run['steps'],
                    script=Path(run['script']), save=save, animation_time=.5 if run.get('animated') else None)
                cohorts[run['name']] = captures;baseline = captures['before']['state'];temporal_check(captures, run['steps'])
                log = Path(captures['before']['log']).read_text(encoding='utf-8', errors='replace')
                for field, marker in [('expectCameraProjectionFailure', 'Expected capture camera projection failure: '),
                                      ('expectCameraSelectionFailure', 'Expected capture camera selection failure: ')]:
                    require(log.count(marker) == sum(bool(e.get(field)) for e in run['events']), 'Missing explicit expected camera rejection diagnostics')
                for expected in run['steps']:
                    record = captures[expected['label']]
                    # Scene switching intentionally replaces active roots/history;
                    # stable geometry rows/tables are still checked independently.
                    check('stateChecks', technique, run['name']+'/'+expected['label'], check_state(record['state'], expected, baseline))
                    require(record['state']['gui']['selectedMeshNodeId'] == controls[expected['control']]['state']['gui']['selectedMeshNodeId'],
                            'Literal camera control has different mesh selection/outline context')
                    check('imageChecks', technique, run['name']+'/'+expected['label'], compare_images(record, controls[expected['control']]))
                    if expected['failureAs']:
                        earlier = captures[expected['failureAs']]
                        require(failure_signature(record['state']) == failure_signature(earlier['state']), 'Rejected camera action changed clocks/source/history/resources/render inputs')
                        check('imageChecks', technique, run['name']+'/'+expected['label']+'-failure-unchanged', compare_images(record, earlier))
                    if expected['unchangedAs']:
                        check('imageChecks', technique, run['name']+'/'+expected['label']+'-sample-unchanged',
                              compare_images(record, captures[expected['unchangedAs']]))
                if save:
                    last = run['steps'][-1];report['exports'].append(dict(technique=technique, **inspect_export(save, last, animated=run.get('animated', False))));persist()
                    copied, files = portable_copy(save, output/'portable'/technique/run['name']);report['portableCopies'].append(dict(files=files));persist()
                    reloads = binding.launch(args, output, report, persist, verify, technique, run['name']+'-reload', copied,
                        run['reloadSteps'], script=Path(run['reloadScript']), initial=run['reloadSteps'][0])
                    for expected in run['reloadSteps']:
                        record = reloads[expected['label']]
                        reference = captures['seek-animated-default'] if expected['clip'] == 0 else captures[last['label']]
                        check('stateChecks', technique, run['name']+'/'+expected['label'],
                              check_state(record['state'], expected, reload=True))
                        check('imageChecks', technique, run['name']+'/'+expected['label'], compare_images(record, reference))
            core, ortho, animation = (cohorts[n] for n in ('shared-perspective', 'orthographic-scene', 'animated-rest'))
            for name, a, b in [('perspective-lens-positive', core['before'], core['lens']),
                               ('shared-node-pose-positive', core['redo'], core['shared']),
                               ('orthographic-xy-positive', ortho['before'], ortho['lens']),
                               ('animated-fov-positive', animation['near'], animation['seek']),
                               ('animated-default-rest-positive', animation['clear-clip'], animation['undo-animated-rest']),
                               ('animated-default-redo-positive', animation['undo-animated-rest'], animation['redo-animated-rest']),
                               ('near-clipping-positive', animation['before'], animation['near']),
                               ('infinite-far-clipping-positive', core['auto-aspect'], core['infinite-far'])]:
                check('imageChecks', technique, name, changed(a, b))
        multiplier = len(args.technique)
        require(len(report['captures']) == plan['expectedCaptureCount']*multiplier and len(report['processes']) == plan['expectedProcessCount']*multiplier,
                'Incomplete native camera capture/process inventory')
        require(len(report['stateChecks']) == len(report['captures']) and len(report['imageChecks']) == plan['expectedImageCheckCount']*multiplier and
                len(report['exports']) == plan['expectedExportCount']*multiplier, 'Incomplete native camera proof inventory')
        report.update(passed=True, nativeValidationPerformed=True);verify()
    except Exception as error: report['failure'] = str(error);raise
    finally:
        if report.get('nativeManifestBefore'):
            try: report['nativeManifestAfter'] = verify()
            except Exception as error: report.update(passed=False, afterManifestFailure=str(error));report.setdefault('failure', str(error))
        persist()
    require(report['passed'], 'Native camera provenance failed after execution')


def synthetic(expected):
    camera = dict(asset=9, revision=10+expected['pubs'], publications=expected['pubs'], selectedSourceNode=expected['selected'],
        truncated=False, authored=copy.deepcopy(expected['authored']), evaluated=copy.deepcopy(expected['evaluated']), rests=copy.deepcopy(expected['rests']), cameras=[])
    rows = []
    for index, name in NAMES.items():
        graph = index+1;kind = 0 if index in (1, 2) else 1
        rows.append(dict(graphNode=graph, sourceNode=index, name=name, active=True, retired=False, worldColumns=camera_world(index).T.tolist()))
        camera['cameras'].append(dict(graphNode=graph, sourceNode=index, authoredCamera=kind, evaluatedCamera=kind,
            sourceNamespace=7, incarnation=100+kind, graphIdentity=6, graphRevision=1))
    if expected['selected'] >= 0:
        lens_value = expected['evaluated'][0 if expected['selected'] in (1, 2) else 1]
        camera.update(view=np.linalg.inv(camera_world(expected['selected'])).T.reshape(-1).tolist(),
            projection=projection_matrix(lens_value, expected['resolution']).T.reshape(-1).tolist(),
            shadowProjection=projection_matrix(lens_value, expected['resolution'], True).T.reshape(-1).tolist())
    state = dict(gltfCameraAuthoring=camera, gltfAnimation=dict(selectedCameraNode=expected['selected'], selectedClip=expected['clip'] or 0,
        time=expected['time'] or 0., speed=1., loop=True, playing=False), resolution=expected['resolution'], msaaSamples=1,
        scene=dict(primaryObjectCount=len(QUADS), primaryVertexCount=4*len(QUADS), primaryIndexCount=6*len(QUADS)),
        gltfMerge=dict(graphIdentity=6, graphNodeCount=len(rows), authoringNodesTruncated=False, authoringNodes=rows, sourceTables={}),
        gltfScenes=dict(selected=expected['scene'], editHistoryDepth=sum(expected['history'])),
        gltfMaterialVariants=dict(undoCount=expected['history'][0], redoCount=expected['history'][1]),
        taa=dict(epoch=100+expected['pubs'], resetReason='synthetic camera control'))
    if expected['clip'] == 0:
        a, b = map(float, np.asarray([.2, .8], dtype=np.float32));roughness = a+(b-a)*expected['time']/2
        state['gltfMaterialBindings'] = dict(materials=[dict(sourceMaterial=0, evaluated=dict(pbrMetallicRoughness=dict(roughnessFactor=roughness)))])
    return state


def glb_oracle(source, destination):
    model, buffers = load_asset(source);require(len(buffers) == 1, 'Pure camera GLB oracle requires one independent buffer')
    model = copy.deepcopy(model);model['buffers'][0].pop('uri', None)
    payload = json.dumps(model, separators=(',', ':'), allow_nan=False).encode('utf-8');payload += b' '*(-len(payload) % 4)
    binary = buffers[0];binary += b'\0'*(-len(binary) % 4)
    total = 12+8+len(payload)+8+len(binary)
    immutable(destination, struct.pack('<III', 0x46546c67, 2, total)+struct.pack('<II', len(payload), 0x4e4f534a)+payload+
        struct.pack('<II', len(binary), 0x004e4942)+binary)


def self_check():
    checks = []
    def reject(name, action):
        try: action()
        except (RuntimeError, ValueError, KeyError, TypeError, IndexError): checks.append(name);return
        raise RuntimeError('Camera checker accepted damaged evidence: '+name)
    with tempfile.TemporaryDirectory(prefix='camera-projection-selfcheck-') as directory:
        output = Path(directory);plan = generate(output)
        require([plan[key]*2 for key in ('expectedCaptureCount', 'expectedProcessCount', 'expectedImageCheckCount', 'expectedExportCount')] ==
                [152, 52, 188, 6], 'Camera cohort changed its exact declared bounds')
        controls_by_name = {c['name']: c for c in plan['controls']}
        for run in plan['runs']:
            script = json.loads(Path(run['script']).read_text());frames = [e['frame'] for e in script['events']]
            require(frames == sorted(set(frames)) and frames[-1] <= script['frames'], 'Camera events are not strictly bounded/ordered')
            checks.append(run['name']+'-bounded-input')
            for expected in run['steps']:
                check_state(synthetic(expected), expected);checks.append(run['name']+'/'+expected['label']+'-state-oracle')
                check_literal_context(expected, controls_by_name[expected['control']])
                if expected['unchangedAs']:
                    previous = next(s for s in run['steps'] if s['label'] == expected['unchangedAs'])
                    require(visual_key(expected) == visual_key(previous), 'Animated default changed the sampled-view oracle')
                    checks.append(run['name']+'/'+expected['label']+'-unchanged-sample-oracle')
            temporal = {};epoch = 100
            for expected in run['steps']:
                if not expected['failureAs']: epoch += 1
                state = synthetic(expected);state['taa']['epoch'] = epoch;temporal[expected['label']] = dict(state=state)
            temporal_check(temporal, run['steps']);checks.append(run['name']+'-temporal-protocol')
        for control in plan['controls']:
            expected = control['step'];check_state(synthetic(expected), expected)
            inspect_export(Path(control['source']), expected);checks.append(control['name']+'-independent-literal-asset')
            check_literal_context(expected, control);checks.append(control['name']+'-scene-selection-context')
        for selected in (-1, 4):
            scene_zero = step('context', selected=selected);scene_one = copy.deepcopy(scene_zero);scene_one['scene'] = 1
            require(visual_key(scene_zero) != visual_key(scene_one), 'Literal key merged scenes with different selection overlays')
            checks.append('scene-separated-'+('viewport' if selected < 0 else 'orthographic')+'-control-key')
        switched = next(c for c in plan['controls'] if c['step']['scene'] == 1 and c['step']['selected'] == 4)
        wrong_context = copy.deepcopy(switched);wrong_context['step']['scene'] = 0
        reject('wrong-literal-scene-context', lambda: check_literal_context(switched['step'], wrong_context))
        for name, events in [('missing-literal-scene-switch', [dict(frame=1, cameraSelection=dict(nodeName='Ortho camera'))]),
                             ('reordered-literal-scene-selection', [dict(frame=1, cameraSelection=dict(nodeName='Ortho camera')), dict(frame=2, gltfScene=1)])]:
            broken_script = output/(name+'.json');document(broken_script, dict(events=events))
            wrong_control = copy.deepcopy(switched);wrong_control['script'] = str(broken_script)
            reject(name, lambda control=wrong_control: check_literal_context(switched['step'], control))
        before = plan['runs'][0]['steps'][0];state = synthetic(before)
        mutations = [
            ('missing-row', lambda s: s['gltfCameraAuthoring']['authored'].pop()),
            ('wrong-lens', lambda s: s['gltfCameraAuthoring']['evaluated'][0]['values'].__setitem__(0, .9)),
            ('wrong-rest', lambda s: s['gltfCameraAuthoring']['rests'][0]['values'].__setitem__(2, .2)),
            ('lost-sharing', lambda s: s['gltfCameraAuthoring']['cameras'][1].__setitem__('incarnation', 999)),
            ('selected-rebound', lambda s: s['gltfCameraAuthoring'].__setitem__('selectedSourceNode', 2)),
            ('missing-namespace', lambda s: s['gltfCameraAuthoring']['cameras'][0].__setitem__('sourceNamespace', 0)),
            ('wrong-view', lambda s: s['gltfCameraAuthoring']['view'].__setitem__(12, 0.)),
            ('wrong-projection', lambda s: s['gltfCameraAuthoring']['projection'].__setitem__(0, 0.)),
            ('projection-y-flip', lambda s: s['gltfCameraAuthoring']['projection'].__setitem__(5, -s['gltfCameraAuthoring']['projection'][5])),
            ('wrong-shadow', lambda s: s['gltfCameraAuthoring']['shadowProjection'].__setitem__(10, 0.)),
            ('extra-publication', lambda s: s['gltfCameraAuthoring'].__setitem__('publications', 1)),
            ('wrong-history-cursor', lambda s: s['gltfMaterialVariants'].__setitem__('redoCount', 1)),
            ('wrong-scene', lambda s: s['gltfScenes'].__setitem__('selected', 1)),
            ('nonfinite-state', lambda s: s['gltfCameraAuthoring']['projection'].__setitem__(0, float('nan')))]
        for name, mutate in mutations:
            wrong = copy.deepcopy(state);mutate(wrong);reject(name, lambda wrong=wrong: check_state(wrong, before))
        wrong = copy.deepcopy(state)
        for row in wrong['gltfCameraAuthoring']['cameras']: row['sourceNamespace'] += 1
        reject('same-rows-new-namespace', lambda: check_state(wrong, before, state))
        wrong = copy.deepcopy(state)
        for row in wrong['gltfCameraAuthoring']['cameras']: row['incarnation'] += 1
        reject('same-numbers-new-incarnation', lambda: check_state(wrong, before, state))
        geometry_rejections_checked = False
        for run in plan['runs']:
            if 'save' in run:
                expected = run['steps'][-1];asset = output/(run['name']+'-oracle.gltf')
                write_fixture(asset, cameras=expected['authored'], animated=run.get('animated', False));inspect_export(asset, expected, animated=run.get('animated', False));checks.append(run['name']+'-portable-structure')
                binary = asset.with_suffix('.glb');glb_oracle(asset, binary);inspect_export(binary, expected, animated=run.get('animated', False));checks.append(run['name']+'-portable-glb-structure')
                broken, _ = load_asset(asset);broken['nodes'][1]['camera'] = 1
                wrong = output/(run['name']+'-wrong-camera.gltf');document(wrong, broken)
                reject(run['name']+'-wrong-portable-association', lambda wrong=wrong, expected=expected, run=run:
                    inspect_export(wrong, expected, animated=run.get('animated', False)))
                canonical, _ = load_asset(asset)
                owner_indices = [];child_indices = []
                for identity in range(len(QUADS)):
                    owner_index = next(i for i, node in enumerate(canonical['nodes']) if node.get('name') == 'Surface '+str(identity))
                    owner = canonical['nodes'][owner_index];mesh = owner.pop('mesh');child_index = len(canonical['nodes'])
                    owner['children'] = [child_index];canonical['nodes'].append(dict(name='Primitive '+str(identity), mesh=mesh))
                    canonical['meshes'][mesh]['name'] = 'Primitive '+str(identity)
                    owner_indices.append(owner_index);child_indices.append(child_index)
                canonical['nodes'].append(dict(name='Scene Root'))
                canonical_path = output/(run['name']+'-canonical.gltf');document(canonical_path, canonical)
                inspect_export(canonical_path, expected, animated=run.get('animated', False));checks.append(run['name']+'-canonical-portable-structure')
                canonical_binary = canonical_path.with_suffix('.glb');glb_oracle(canonical_path, canonical_binary)
                inspect_export(canonical_binary, expected, animated=run.get('animated', False));checks.append(run['name']+'-canonical-portable-glb-structure')
                if run.get('animated', False):
                    remapped = copy.deepcopy(canonical)
                    remapped['materials'] = [dict(name='Runtime default 0'), dict(name='Runtime default 1'), *remapped['materials']]
                    for mesh in remapped['meshes']:
                        for primitive in mesh['primitives']: primitive['material'] += 2
                    remapped['animations'][0]['channels'][1]['target']['extensions'][POINTER]['pointer'] = '/materials/2/pbrMetallicRoughness/roughnessFactor'
                    remapped_path = output/'remapped-material-camera.gltf';document(remapped_path, remapped)
                    inspect_export(remapped_path, expected, animated=True);checks.append('semantic-material-remap-gltf')
                    remapped_binary = remapped_path.with_suffix('.glb');glb_oracle(remapped_path, remapped_binary)
                    inspect_export(remapped_binary, expected, animated=True);checks.append('semantic-material-remap-glb')
                    material_mutations = [
                        ('stale-source-material-target', lambda m: m['animations'][0]['channels'][1]['target']['extensions'][POINTER].__setitem__('pointer', '/materials/0/pbrMetallicRoughness/roughnessFactor')),
                        ('wrong-source-material-target', lambda m: m['animations'][0]['channels'][1]['target']['extensions'][POINTER].__setitem__('pointer', '/materials/3/pbrMetallicRoughness/roughnessFactor')),
                        ('duplicate-material-identity', lambda m: m['materials'].append(copy.deepcopy(m['materials'][2]))),
                        ('wrong-geometry-material-link', lambda m: m['meshes'][0]['primitives'][0].__setitem__('material', 3))]
                    for label, mutate in material_mutations:
                        damaged = copy.deepcopy(remapped);mutate(damaged);wrong_material = output/(label+'.gltf');document(wrong_material, damaged)
                        reject(label+'-gltf', lambda path=wrong_material: inspect_export(path, expected, animated=True))
                        wrong_binary = wrong_material.with_suffix('.glb');glb_oracle(wrong_material, wrong_binary)
                        reject(label+'-glb', lambda path=wrong_binary: inspect_export(path, expected, animated=True))
                if not geometry_rejections_checked:
                    geometry_rejections_checked = True
                    first_owner, next_owner = owner_indices[:2];first_child, next_child = child_indices[:2]
                    def reparent_child(model):
                        model['nodes'][first_owner]['children'] = [next_child];model['nodes'][next_owner]['children'] = [first_child]
                    def cancel_transforms(model):
                        model['nodes'][first_owner]['translation'] = [1., 0., 0.];model['nodes'][first_child]['translation'] = [-1., 0., 0.]
                    def extra_primitive(model): model['nodes'].append(dict(name='Primitive extra', mesh=0))
                    def reparent_owner(model):
                        for scene in model['scenes']: scene['nodes'].remove(first_owner)
                        model['nodes'][1]['children'] = [first_owner]
                    def duplicate_owner(model): model['nodes'].append(copy.deepcopy(model['nodes'][first_owner]))
                    def lost_geometry(model):
                        model['meshes'][0]['primitives'][0]['attributes']['POSITION'] = model['meshes'][1]['primitives'][0]['attributes']['POSITION']
                    geometry_mutations = [
                        ('wrong-primitive-parent', reparent_child),
                        ('canceled-owner-local-transform', cancel_transforms),
                        ('wrong-primitive-transform', lambda m: m['nodes'][first_child].__setitem__('translation', [.1, 0., 0.])),
                        ('wrong-owner-name', lambda m: m['nodes'][first_owner].__setitem__('name', 'Unrelated owner')),
                        ('wrong-primitive-name', lambda m: m['nodes'][first_child].__setitem__('name', 'Unrelated primitive')),
                        ('missing-primitive-mesh', lambda m: m['nodes'][first_child].pop('mesh')),
                        ('extra-primitive', extra_primitive),
                        ('missing-scene-owner', lambda m: m['scenes'][1]['nodes'].remove(first_owner)),
                        ('reparented-owner', reparent_owner),
                        ('missing-mesh', lambda m: m['meshes'].pop()),
                        ('duplicate-owner', duplicate_owner),
                        ('wrong-geometry', lost_geometry),
                        ('wrong-primitive-mode', lambda m: m['meshes'][0]['primitives'][0].__setitem__('mode', 1)),
                        ('extra-primitive-count', lambda m: m['meshes'][0]['primitives'].append(copy.deepcopy(m['meshes'][0]['primitives'][0])))]
                    for label, mutate in geometry_mutations:
                        damaged = copy.deepcopy(canonical);mutate(damaged);wrong_geometry = output/('wrong-geometry-'+label+'.gltf');document(wrong_geometry, damaged)
                        reject(label, lambda path=wrong_geometry: inspect_export(path, expected, animated=run.get('animated', False)))
        world = camera_world(1);require(abs(world[0, 1]) > .3 and world[0, 3] != .15 and world[2, 3] == 6., 'Parent/roll fixture is vacuous');checks.append('independent-parent-roll')
        ortho = camera_records()[1];m = projection_matrix(ortho, [800, 360]);require(m[0, 0] != m[1, 1], 'Independent ortho magnitudes collapsed');checks.append('independent-orthographic-xy')
        auto = projection_matrix(camera_records()[0], [800, 360]);fixed_camera = camera_records()[0];fixed_camera['values'][1] = 1.5
        require(auto[0, 0] != projection_matrix(fixed_camera, [800, 360])[0, 0], 'Aspect resize control is vacuous');checks.append('auto-fixed-aspect-separation')
        for z, wanted in [(-.1, 1.), (-1e9, 0.)]:
            clip = projection_matrix(camera_records()[0], [640, 360])@np.array([0., 0., z, 1.]);require(abs(clip[2]/clip[3]-wanted) < 1e-8, 'Infinite reverse-Z oracle failed')
        checks.append('independent-infinite-reverse-z')
        def inside(camera, point):
            clip = projection_matrix(camera, [800, 360])@np.linalg.inv(camera_world(1))@np.array([*point, 1.])
            return clip[3] > 0 and -clip[3] < clip[0] < clip[3] and -clip[3] < clip[1] < clip[3] and 0 < clip[2] < clip[3]
        near_point = [QUADS[4][0], QUADS[4][1], QUADS[4][4]]
        require(inside(camera_records()[0], near_point) and not inside(records(near=True)[0], near_point), 'Near-depth fixture is vacuous')
        checks.append('independent-near-clipping-layer')
        far_point = [QUADS[3][0], QUADS[3][1], QUADS[3][4]]
        require(not inside(records(edited=True, auto=True)[0], far_point) and
                inside(records(edited=True, auto=True, infinite=True)[0], far_point), 'Far-depth fixture is vacuous')
        checks.append('independent-far-clipping-layer')
        a = np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8)
        for i, rgb in enumerate(([160, 20, 10], [20, 160, 30], [20, 30, 160])): a[100:150, 100+i*90:150+i*90] = rgb
        pa, pb = output/'pixels-a.png', output/'pixels-b.png';Image.fromarray(a).save(pa);b = np.roll(a, 25, axis=1);Image.fromarray(b).save(pb)
        first = dict(image=str(pa), resolution=[WIDTH, HEIGHT]);second = dict(image=str(pb), resolution=[WIDTH, HEIGHT])
        visible(first);changed(first, second);compare_images(first, first);checks.append('nonvacuous-pixel-oracles')
        reject('wrong-native-framing', lambda: compare_images(first, second))
        blank = output/'blank.png';Image.fromarray(np.zeros_like(a)).save(blank);reject('blank-positive-control', lambda: visible(dict(image=str(blank), resolution=[WIDTH, HEIGHT])))
    return dict(passed=True, checkCount=len(checks), checks=checks, rendererExecuted=False, nativeValidationPerformed=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path);parser.add_argument('--renderer', type=Path);parser.add_argument('--manifest', type=Path)
    parser.add_argument('--technique', choices=TECHNIQUES, action='append');parser.add_argument('--timeout', type=float, default=600.)
    parser.add_argument('--self-check', action='store_true');parser.add_argument('--generate-only', action='store_true');args = parser.parse_args()
    if args.self_check: print(json.dumps(self_check(), indent=2));return
    if args.output is None: parser.error('--output is required')
    if not math.isfinite(args.timeout) or args.timeout <= 0: parser.error('--timeout must be finite and positive')
    if not args.generate_only and (args.renderer is None or args.manifest is None): parser.error('Native camera run requires --renderer and --manifest')
    args.technique = tuple(dict.fromkeys(args.technique or TECHNIQUES))
    if args.renderer: args.renderer = args.renderer.resolve()
    if args.manifest: args.manifest = args.manifest.resolve()
    output = args.output.resolve();plan = generate(output)
    report = dict(schemaVersion=1, passed=None if args.generate_only else False, generatedOnly=args.generate_only,
        rendererExecuted=False, nativeValidationPerformed=False, sourceDependencies=dependencies(), launchPlanSha256=sha(output/'launch-plan.json'),
        expectedCaptureCount=plan['expectedCaptureCount']*len(args.technique), expectedProcessCount=plan['expectedProcessCount']*len(args.technique),
        expectedImageCheckCount=plan['expectedImageCheckCount']*len(args.technique), expectedExportCount=plan['expectedExportCount']*len(args.technique),
        processes=[], captures=[], stateChecks=[], imageChecks=[], exports=[], portableCopies=[], unverified=UNVERIFIED)
    if args.generate_only: document(output/'results.json', report);print('GENERATED', report['expectedCaptureCount'], 'camera captures; renderer not executed')
    else:
        run_native(args, output, plan, report)
        print('PASS', len(report['captures']), 'camera captures;', len(report['stateChecks']), 'CPU/source checks;', len(report['imageChecks']), 'image checks')


if __name__ == '__main__': main()
