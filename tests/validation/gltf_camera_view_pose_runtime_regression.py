"""Native camera-node pose writeback, history and portable export regression.

Pure calibration/generation never starts the renderer. CPU source/graph checks,
literal raster controls and the separate Vulkan buffer tests are distinct proof.
"""
from __future__ import annotations

import argparse
import base64
import copy
import json
import math
from pathlib import Path
import struct
import tempfile

import numpy as np
from PIL import Image

import gltf_camera_projection_runtime_regression as projection
import gltf_material_binding_runtime_regression as binding
from gltf_accessor_runtime_regression import Asset, decode_accessor, load_asset, require
from gltf_extended_materials_runtime_regression import sha
from gltf_merge_runtime_regression import document, immutable, portable_copy
from gltf_texture_formats_runtime_regression import verify_manifest
from gltf_tangent_uv_runtime_regression import finite_json
from scene_node_authoring_runtime_regression import history_cursor

TECHNIQUES = binding.TECHNIQUES
WIDTH, HEIGHT = 640, 360
POINTER, LIGHT = 'KHR_animation_pointer', 'KHR_lights_punctual'
NAMES = {1: 'Main camera', 2: 'Shared camera', 4: 'Ortho camera'}
NAV_A = dict(position=[.5, 0., 6.], target=[.5, 0., 0.])
NAV_B = dict(position=[-.6, .2, 5.4], target=[-.6, .2, -.6])
UNVERIFIED = [
    'Pure calibration and generated inputs are not native rendering acceptance.',
    'Source/rest/graph/light telemetry is CPU evidence; separate Vulkan suites exercise actual GPU camera/object/light buffers.',
    'Literal full-RGB controls cover these bounded fixtures, not every provider, physics or ray/DDGI configuration.',
    'Capture actions are not native Windows ImGui interaction; separate widget tests exercise the actual button.',
    'Write viewport pose intentionally keeps the destination lens; it does not promise identical framing to the free viewport.']


def close(a, b, label, tolerance=5e-5):
    actual, expected = np.asarray(a, dtype=float), np.asarray(b, dtype=float)
    require(actual.shape == expected.shape and np.isfinite(actual).all() and np.isfinite(expected).all() and
            np.allclose(actual, expected, rtol=tolerance, atol=tolerance*.1), 'Incorrect analytical pose '+label)


def f32(value): return np.asarray(value, dtype=np.float32).astype(float)
def flat(matrix): return np.asarray(matrix).T.reshape(-1).tolist()


def node_matrix(node): return projection.local_matrix({key: value for key, value in node.items() if key not in ('matrix', 'translation', 'rotation', 'scale') or value})


def view_from_world(world):
    forward = -world[:3, 2];forward = forward/np.linalg.norm(forward)
    up = world[:3, 1];right = np.cross(forward, up);right = right/np.linalg.norm(right)
    up = np.cross(right, forward);up = up/np.linalg.norm(up)
    rigid = np.eye(4);rigid[:3, :3] = np.column_stack((right, up, -forward));rigid[:3, 3] = world[:3, 3]
    return np.linalg.inv(rigid)


def navigation_world(nav):
    eye, target = np.asarray(nav['position'], dtype=float), np.asarray(nav['target'], dtype=float)
    forward = target-eye;forward /= np.linalg.norm(forward)
    right = np.cross(forward, [0., 1., 0.]);right /= np.linalg.norm(right)
    up = np.cross(right, forward)
    result = np.eye(4);result[:3, :3] = np.column_stack((right, up, -forward));result[:3, 3] = eye;return result


def parent_record(mode, time=None):
    q = projection.quat_z(25)
    if mode == 'animated':
        x = .3 if time is None else float(f32(.3)+(f32(1.1)-f32(.3))*(time/2.))
        return dict(translation=[x, .2, 6.], rotation=q, scale=[1.1, .9, 1.2])
    rotation = node_matrix(dict(rotation=q))
    shear = np.eye(4);shear[:3, :3] = [[1.1, .2, .04], [0., .9, .08], [0., 0., 1.2]]
    matrix = rotation@shear;matrix[:3, 3] = [.3, .2, 6.]
    return dict(matrix=flat(matrix))


def original_main(): return dict(translation=[.15, -.1, 0.], scale=[1.2, .8, 1.1])


def main_pose(mode, endpoint):
    if endpoint == 0: return original_main()
    parent = f32(node_matrix(parent_record(mode, .5 if mode == 'animated' else None)))
    current = f32(node_matrix(original_main()))
    for nav in (NAV_A, NAV_B)[:endpoint]:
        world = f32(parent.astype(np.float32)@current.astype(np.float32))
        old_basis = np.linalg.inv(view_from_world(world))[:3, :3]
        residual = old_basis.T@world[:3, :3]
        desired = navigation_world(nav);desired[:3, :3] = desired[:3, :3]@residual
        current = f32(np.linalg.inv(parent)@desired)
    return dict(matrix=flat(current))


def lens_records(near=False):
    result = projection.camera_records()
    if near: result[0]['values'][2] = .2
    return result


def expected_nodes(mode, endpoint, time=None, *, evaluated=False):
    nodes = [
        dict(name='Perspective rolled parent', children=[1, 2], **parent_record(mode, time if evaluated else None)),
        dict(name='Main camera', camera=0, **main_pose(mode, endpoint)),
        dict(name='Shared camera', camera=0, translation=[-.8, .3, 0.]),
        dict(name='Orthographic rolled parent', translation=[-.2, -.15, 6.], rotation=projection.quat_z(-18), children=[4]),
        dict(name='Ortho camera', camera=1, translation=[.15, .1, 0.]),
        dict(name='Ambiguous camera', camera=1, translation=[0., 0., 6.]),
        dict(name='Ambiguous camera', camera=1, translation=[.2, 0., 6.]),
        *[dict(name='Surface '+str(i), mesh=i) for i in range(len(projection.QUADS))]]
    if mode == 'actor':
        nodes[1].update(mesh=5, children=[12])
        nodes.append(dict(name='Camera child light', translation=[.25, .15, -1.], extensions={LIGHT: dict(light=0)}))
    for i, node in enumerate(nodes): node['extras'] = dict(nodeIdentity=1000+i, retained=dict(annotation='camera pose metadata', sourceLike=991+i))
    return nodes


def world_matrices(nodes):
    parents = {}
    for i, node in enumerate(nodes):
        for child in node.get('children', []): require(child not in parents, 'Oracle contains shared child');parents[child] = i
    worlds = {}
    def visit(i):
        if i not in worlds: worlds[i] = (visit(parents[i]) if i in parents else np.eye(4))@node_matrix(nodes[i])
        return worlds[i]
    for i in range(len(nodes)): visit(i)
    return worlds


def positions(identity):
    return projection.positions(identity) if identity < 5 else [[-.4, -.7, -3.], [.4, -.7, -3.], [.4, -.1, -3.], [-.4, -.1, -3.]]


def material_record(identity):
    if identity < 5: return binding.material('Surface '+str(identity), projection.COLORS[identity], sided=True)
    return dict(name='Camera body material', doubleSided=True,
                pbrMetallicRoughness=dict(baseColorFactor=[.5, .5, .5, 1.], metallicFactor=0., roughnessFactor=.5),
                extensions={projection.UNLIT: {}}, extras=dict(bodyIdentity=17))


def write_fixture(path, mode, endpoint=0, near=False, *, literal_time=None, literal=False):
    asset = Asset();model = asset.model;count = 6 if mode == 'actor' else 5
    model['materials'] = [material_record(i) for i in range(count)]
    model['meshes'] = []
    for i in range(count):
        attrs = dict(POSITION=asset.accessor('VEC3', positions(i)), NORMAL=asset.accessor('VEC3', [[0., 0., 1.]]*4),
            TANGENT=asset.accessor('VEC4', [[1., 0., 0., 1.]]*4), TEXCOORD_0=asset.accessor('VEC2', [[0., 0.], [1., 0.], [1., 1.], [0., 1.]]))
        model['meshes'].append(dict(name='Geometry '+str(i), primitives=[dict(attributes=attrs,
            indices=asset.accessor('SCALAR', projection.INDICES, 5123, indices=True), material=i, mode=4)], extras=dict(geometryIdentity=i)))
    model['nodes'] = expected_nodes(mode, endpoint, literal_time, evaluated=literal_time is not None)
    model['cameras'] = [projection.camera_json(c) for c in lens_records(near)]
    surfaces = list(range(7, 12));model['scenes'] = [dict(name='All camera definitions', nodes=[0, 3, 5, 6, *surfaces]),
        dict(name='Orthographic scene', nodes=[3, 5, 6, *surfaces])];model['scene'] = 0;model['extensionsUsed'] = [projection.UNLIT]
    if mode == 'actor':
        model['extensionsUsed'].append(LIGHT);model['extensions'] = {LIGHT: dict(lights=[dict(name='Camera child point', type='point',
            color=[1., .7, .2], intensity=20., range=6., extras=dict(lightIdentity=31))])}
    if mode == 'animated' and literal_time is None and not literal:
        times = asset.accessor('SCALAR', [0., 2.]);translation = asset.accessor('VEC3', [[.3, .2, 6.], [1.1, .2, 6.]])
        rough = asset.accessor('SCALAR', [.2, .8]);blocked = asset.accessor('VEC3', [[-.8, .3, 0.], [-.6, .3, 0.]])
        model['extensionsUsed'].append(POINTER)
        model['animations'] = [dict(name='Parent and unrelated material', extras=dict(curveMetadata='preserve keys'),
            samplers=[dict(input=times, output=translation, interpolation='LINEAR', extras=dict(samplerIdentity=10)), dict(input=times, output=rough, interpolation='LINEAR', extras=dict(samplerIdentity=11))],
            channels=[dict(sampler=0, target=dict(node=0, path='translation'), extras=dict(channelIdentity=20)),
                      dict(sampler=1, target=dict(path='pointer', extensions={POINTER: dict(pointer='/materials/0/pbrMetallicRoughness/roughnessFactor')}), extras=dict(channelIdentity=21))]),
            dict(name='Unselected clip still owns shared-node TRS', extras=dict(blockerMetadata=19),
                 samplers=[dict(input=times, output=blocked, interpolation='LINEAR', extras=dict(samplerIdentity=12))],
                 channels=[dict(sampler=0, target=dict(node=2, path='translation'), extras=dict(channelIdentity=22))])]
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='camera-pose-fixture-') as directory:
        temp = Path(directory)/'fixture.gltf';asset.write(temp);immutable(path, temp.read_bytes())


def step(label, mode='static', endpoint=0, selected=1, pose=0, lens=0, history=(0, 0), destination=1,
         time=None, clip=-1, failure_as=None, nav=NAV_A):
    return dict(label=label, mode=mode, endpoint=endpoint, selected=selected, pose=pose, lens=lens,
                history=list(history), destination=destination, time=time, clip=clip, failureAs=failure_as,
                near=lens % 2 == 1, nav=copy.deepcopy(nav), resolution=[WIDTH, HEIGHT], msaa=1, scene=0)


def pose_action(source=1, expected=None):
    value = dict(sourceNode=source, fromViewport=True)
    if expected is not None: value['expectedCameraRevision'] = expected
    return dict(cameraPose=value)


def profiles():
    select = lambda i: dict(cameraSelection=dict(sourceNode=i)) if i >= 0 else dict(cameraSelection=dict(viewport=True))
    static = [
        (step('frame-one-written', endpoint=1, selected=-1, pose=1, history=(1, 0)), pose_action()),
        (step('view-written', endpoint=1, pose=1, history=(1, 0)), select(1)),
        (step('lens', endpoint=1, pose=1, lens=1, history=(2, 0)), dict(cameraProjection=dict(sourceNode=1, values=[dict(property='perspective/znear', value=.2)]))),
        (step('viewport', endpoint=1, selected=-1, pose=1, lens=1, history=(2, 0)), select(-1)),
        (step('same-frame-track-write', endpoint=2, selected=-1, pose=2, lens=1, history=(3, 0), nav=NAV_B), pose_action()),
        (step('shared', endpoint=2, selected=2, pose=2, lens=1, history=(3, 0), destination=2, nav=NAV_B), select(2)),
        (step('undo-second', endpoint=1, selected=2, pose=3, lens=1, history=(2, 1), destination=2, nav=NAV_B), dict(sceneHistory='undo')),
        (step('undo-lens', endpoint=1, selected=2, pose=3, lens=2, history=(1, 2), destination=2, nav=NAV_B), dict(sceneHistory='undo')),
        (step('undo-first', selected=2, pose=4, lens=2, history=(0, 3), destination=2, nav=NAV_B), dict(sceneHistory='undo')),
        (step('wrong-mode', selected=2, pose=4, lens=2, history=(0, 3), destination=2, failure_as='undo-first', nav=NAV_B), dict(**pose_action(), expectCameraPoseFailure=True)),
        (step('viewport-redo', selected=-1, pose=4, lens=2, history=(0, 3), destination=2, nav=NAV_B), select(-1)),
        (step('stale-rejected', selected=-1, pose=4, lens=2, history=(0, 3), destination=2, failure_as='viewport-redo', nav=NAV_B), dict(**pose_action(expected=9007199254740991), expectCameraPoseFailure=True)),
        (step('missing-rejected', selected=-1, pose=4, lens=2, history=(0, 3), destination=2, failure_as='viewport-redo', nav=NAV_B), dict(**pose_action(999), expectCameraPoseFailure=True)),
        (step('redo-first', endpoint=1, selected=-1, pose=5, lens=2, history=(1, 2), destination=2, nav=NAV_B), dict(sceneHistory='redo')),
        (step('redo-lens', endpoint=1, selected=-1, pose=5, lens=3, history=(2, 1), destination=2, nav=NAV_B), dict(sceneHistory='redo')),
        (step('redo-second', endpoint=2, selected=-1, pose=6, lens=3, history=(3, 0), destination=2, nav=NAV_B), dict(sceneHistory='redo')),
        (step('main-again', endpoint=2, pose=6, lens=3, history=(3, 0), nav=NAV_B), select(1)),
        (step('undo-repeat', endpoint=1, pose=7, lens=3, history=(2, 1), nav=NAV_B), dict(sceneHistory='undo')),
        (step('redo-repeat', endpoint=2, pose=8, lens=3, history=(3, 0), nav=NAV_B), dict(sceneHistory='redo'))]
    animated = [
        (step('before', 'animated', time=.5, clip=0), select(1)),
        (step('viewport', 'animated', selected=-1, time=.5, clip=0), select(-1)),
        (step('write', 'animated', endpoint=1, selected=-1, pose=1, history=(1, 0), time=.5, clip=0), pose_action()),
        (step('view-written', 'animated', endpoint=1, pose=1, history=(1, 0), time=.5, clip=0), select(1)),
        (step('seek', 'animated', endpoint=1, pose=1, history=(1, 0), time=1.5, clip=0), dict(animationTime=1.5)),
        (step('undo-current-time', 'animated', pose=2, history=(0, 1), time=1.5, clip=0), dict(sceneHistory='undo')),
        (step('viewport-redo', 'animated', selected=-1, pose=2, history=(0, 1), time=1.5, clip=0), select(-1)),
        (step('stale-animated-target-rejected', 'animated', selected=-1, pose=2, history=(0, 1), time=1.5, clip=0, failure_as='viewport-redo'), dict(**pose_action(2, expected=9007199254740991), expectCameraPoseFailure=True)),
        (step('redo-current-time', 'animated', endpoint=1, selected=-1, pose=3, history=(1, 0), time=1.5, clip=0), dict(sceneHistory='redo')),
        (step('main-current', 'animated', endpoint=1, pose=3, history=(1, 0), time=1.5, clip=0), select(1)),
        (step('clear-clip', 'animated', endpoint=1, pose=3, history=(1, 0), time=0., clip=-1), dict(animationClip=-1))]
    actor = [
        (step('before', 'actor'), select(1)),
        (step('viewport', 'actor', selected=-1), select(-1)),
        (step('write', 'actor', endpoint=1, selected=-1, pose=1, history=(1, 0)), pose_action()),
        (step('view-written', 'actor', endpoint=1, pose=1, history=(1, 0)), select(1)),
        (step('shared', 'actor', endpoint=1, selected=2, pose=1, history=(1, 0), destination=2), select(2)),
        (step('undo-shared', 'actor', selected=2, pose=2, history=(0, 1), destination=2), dict(sceneHistory='undo')),
        (step('redo-shared', 'actor', endpoint=1, selected=2, pose=3, history=(1, 0), destination=2), dict(sceneHistory='redo')),
        (step('main-again', 'actor', endpoint=1, pose=3, history=(1, 0)), select(1)),
        (step('viewport-again', 'actor', endpoint=1, selected=-1, pose=3, history=(1, 0)), select(-1)),
        (step('undo-free', 'actor', selected=-1, pose=4, history=(0, 1)), dict(sceneHistory='undo')),
        (step('missing-rejected', 'actor', selected=-1, pose=4, history=(0, 1), failure_as='undo-free'), dict(**pose_action(999), expectCameraPoseFailure=True)),
        (step('redo-free', 'actor', endpoint=1, selected=-1, pose=5, history=(1, 0)), dict(sceneHistory='redo')),
        (step('main-final', 'actor', endpoint=1, pose=5, history=(1, 0)), select(1))]
    result = []
    for name, rows, fmt in [('static-shared', static, 'gltf'), ('animated-parent', animated, 'glb'), ('mesh-child-light', actor, 'glb')]:
        steps, events = [], []
        for i, (value, event) in enumerate(rows):
            value['frame'] = 13*(i+1);steps.append(value)
            if event: events.append(dict(frame=13*i+1, **event))
        result.append(dict(name=name, steps=steps, events=events, save=fmt, mode=steps[0]['mode']))
    return result


def visual_key(value):
    return json.dumps({k: value[k] for k in ('mode', 'endpoint', 'selected', 'near', 'time', 'clip', 'nav', 'scene')}, sort_keys=True)


def generate(output):
    output.mkdir(parents=True, exist_ok=True);sources = {}
    for mode in ('static', 'animated', 'actor'):
        sources[mode] = output/'sources'/(mode+'.gltf');write_fixture(sources[mode], mode)
    runs = profiles();controls = [];by_key = {};inputs = list(sources.values())
    for run in runs:
        for value in run['steps']:
            key = visual_key(value)
            if key not in by_key:
                name = 'literal-'+str(len(controls));source = output/'sources'/(name+'.gltf')
                # Literal parent poses are baked independently; no animation
                # library is run to manufacture the expected image.
                write_fixture(source, value['mode'], value['endpoint'], value['near'],
                    literal_time=value['time'] if value['clip'] == 0 else None, literal=True)
                script = output/'scripts'/(name+'.json')
                events = [] if value['selected'] < 0 else [dict(frame=1, cameraSelection=dict(nodeName=NAMES[value['selected']]))]
                document(script, dict(schemaVersion=1, frames=13, sampleFrames=[13], camera=[dict(frame=1, **value['nav'])], events=events))
                expected = copy.deepcopy(value);expected.update(label='control', frame=13, pose=0, lens=0, history=[0, 0], time=None, clip=-1, failureAs=None,
                    literalTime=value['time'] if value['clip'] == 0 else None)
                controls.append(dict(name=name, source=str(source), script=str(script), step=expected, literalTime=value['time'] if value['clip'] == 0 else None));by_key[key] = name;inputs += [source, script]
            value['control'] = by_key[key]
        script = output/'scripts'/(run['name']+'.json');keys = [dict(frame=1, **NAV_A)]
        if run['name'] == 'static-shared': keys.extend([dict(frame=52, **NAV_A), dict(frame=53, **NAV_B)])
        document(script, dict(schemaVersion=1, frames=run['steps'][-1]['frame'], sampleFrames=[s['frame'] for s in run['steps']], camera=keys, events=run['events']))
        run['script'] = str(script);inputs.append(script)
        reload_script = output/'scripts'/(run['name']+'-reload.json');last = run['steps'][-1]
        document(reload_script, dict(schemaVersion=1, frames=13, sampleFrames=[13], camera=[dict(frame=1, **last['nav'])],
            events=[dict(frame=1, cameraSelection=dict(nodeName=NAMES[last['selected']]))]));run['reloadScript'] = str(reload_script);inputs.append(reload_script)
    captures = len(controls)+sum(len(r['steps'])+1 for r in runs)
    plan = dict(schemaVersion=1, sources={k: str(v) for k, v in sources.items()}, runs=runs, controls=controls,
        expectedCaptureCount=captures, expectedProcessCount=len(controls)+len(runs)*2,
        expectedImageCheckCount=captures+sum(bool(s['failureAs']) for r in runs for s in r['steps'])+6,
        expectedExportCount=len(runs), immutableInputs=[dict(path=str(p), byteLength=p.stat().st_size, sha256=sha(p)) for p in inputs])
    require(plan['expectedCaptureCount']*2 <= 160, 'Pose camera native cohort exceeded its declared bound')
    document(output/'launch-plan.json', plan);return plan


def pose_record(node):
    return dict(name=node['name'], camera=node.get('camera', -1),
                **{key: node.get(key, []) for key in ('matrix', 'translation', 'rotation', 'scale')})


def check_pose_record(actual, node, label, *, canonical=False):
    require(actual['name'] == node['name'] and actual['camera'] == node.get('camera', -1), 'Pose record identity/association changed: '+label)
    require(set(actual) == {'name', 'nameTruncated', 'camera', 'matrix', 'translation', 'rotation', 'scale'} and
            actual['nameTruncated'] is False, 'Pose record schema/truncation changed')
    if canonical: close(node_matrix(actual), node_matrix(node), label+' canonical local')
    else:
        for key in ('matrix', 'translation', 'rotation', 'scale'):
            if key == 'matrix' and node.get(key): close(actual[key], node[key], label+' published float matrix')
            else: require(actual[key] == node.get(key, []), 'Pose changed an exact original optional field: '+label+'/'+key)


def check_state(state, expected, baseline=None, *, reload=False, literal_time=None, control=False):
    require(finite_json(state), 'Nonfinite pose telemetry')
    poses, cameras = state['gltfCameraPoses'], state['gltfCameraAuthoring']
    require(not poses['truncated'] and not cameras['truncated'], 'Truncated camera pose proof')
    require(type(poses['viewRevision']) is int and poses['viewRevision'] > 0, 'Missing current unjittered navigation revision')
    require(poses['publications'] == expected['pose'] and cameras['publications'] == expected['lens'], 'Wrong independent pose/lens publication count')
    require(poses['viewportActive'] == (expected['selected'] < 0), 'Write changed the active viewport mode')
    if not control and not reload: require(poses['destinationSourceNode'] == expected['destination'], 'Weak pose authoring destination was changed or lost')
    require(state['resolution'] == expected['resolution'] and state['msaaSamples'] == 1, 'Wrong pose raster dimensions')
    require(state['gltfScenes']['selected'] == 0, 'Pose action changed the active scene')
    history_cursor(state, *expected['history'], allow_uncreated=expected['history'] == [0, 0])
    count = 6 if expected['mode'] == 'actor' else 5
    require(state['scene']['primaryObjectCount'] == count and state['scene']['primaryVertexCount'] == count*4 and
            state['scene']['primaryIndexCount'] == count*6, 'Pose publication changed geometry inventory')
    current_time = literal_time if control else expected['time'] if expected['clip'] == 0 else None
    authored = expected_nodes(expected['mode'], expected['endpoint'])
    evaluated = expected_nodes(expected['mode'], expected['endpoint'], current_time, evaluated=True)
    worlds = world_matrices(evaluated);rows = projection.graph_rows(state)
    pose_by_name = {}
    for row in poses['nodes']:
        require(type(row['nodeIncarnation']) is int and row['nodeIncarnation'] > 0 and row['sourceNode'] >= 0, 'Pose binding has no original node birth')
        pose_by_name.setdefault(row['authored']['name'], []).append(row)
    require(len(poses['nodes']) == 5, 'Pose snapshot lost camera-bearing source nodes')
    for key in ('graphNode', 'sourceNode', 'nodeIncarnation'):
        require(len({row[key] for row in poses['nodes']}) == 5, 'Pose snapshot duplicated a node identity')
    for source, name in NAMES.items():
        matching = pose_by_name.get(name, []);require(len(matching) == 1, 'Pose camera node name is ambiguous/missing')
        row = matching[0]
        if not reload: require(row['sourceNode'] == source, 'Pose node was rebound to another source index')
        check_pose_record(row['authored'], authored[source], 'authored '+name, canonical=reload)
        check_pose_record(row['evaluated'], evaluated[source], 'evaluated '+name, canonical=reload)
        check_pose_record(row['rest'], authored[source], 'rest '+name, canonical=reload)
        require(row['authored'] == row['evaluated'] == row['rest'], 'Pose camera local source/rest domains diverged')
        graph = next((r for r in rows if r['graphNode'] == row['graphNode']), None)
        require(graph is not None and graph['active'] and not graph['retired'], 'Pose graph binding is unavailable')
        close(np.asarray(graph['worldColumns']).T, worlds[source], name+' world')
        if baseline is not None:
            old = next(r for r in baseline['gltfCameraPoses']['nodes'] if r['authored']['name'] == name)
            require(all(row[k] == old[k] for k in ('graphNode', 'sourceNode', 'nodeIncarnation')), 'Pose history replaced a source node lifetime')
    require(pose_by_name['Main camera'][0]['nodeIncarnation'] != pose_by_name['Shared camera'][0]['nodeIncarnation'], 'Shared lens was confused with node lifetime')
    ambiguous = pose_by_name.get('Ambiguous camera', []);require(len(ambiguous) == 2, 'Pose changed unrelated ambiguous-name camera nodes')
    remaining = list(ambiguous)
    for source in (5, 6):
        matching = [row for row in remaining if np.allclose(node_matrix(row['authored']), node_matrix(authored[source]), rtol=0, atol=5e-5)]
        require(len(matching) == 1, 'Pose replaced an unrelated camera local endpoint');row = matching[0];remaining.remove(row)
        if not reload: require(row['sourceNode'] == source, 'Ambiguous camera was rebound to a different source node')
        for domain, wanted in [('authored', authored), ('evaluated', evaluated), ('rest', authored)]:
            check_pose_record(row[domain], wanted[source], domain+' unrelated camera', canonical=reload)
        graph = next((r for r in rows if r['graphNode'] == row['graphNode']), None)
        require(graph is not None and graph['active'] and not graph['retired'], 'Unrelated camera graph was lost')
        close(np.asarray(graph['worldColumns']).T, worlds[source], 'unrelated ambiguous camera world')
    for source in [0, 3, *range(7, len(evaluated))]:
        source_rows = [row for row in rows if row['name'] == evaluated[source]['name'] and row['active'] and not row['retired']]
        require(len(source_rows) == 1, 'Pose changed an unrelated source owner')
        close(np.asarray(source_rows[0]['worldColumns']).T, worlds[source], 'unchanged/derived owner '+evaluated[source]['name'])
    mappings = cameras['cameras'];require(len(mappings) == 5, 'Lost camera definition bindings')
    for pose in poses['nodes']:
        match = [row for row in mappings if row['graphNode'] == pose['graphNode'] and row['sourceNode'] == pose['sourceNode']]
        require(len(match) == 1 and match[0]['authoredCamera'] == pose['authored']['camera'] and
                match[0]['evaluatedCamera'] == pose['evaluated']['camera'], 'Pose node and definition namespace bindings diverged')
    shared = [row for row in mappings if row['graphNode'] in [pose_by_name[name][0]['graphNode'] for name in ('Main camera', 'Shared camera')]]
    require(len(shared) == 2 and len({(row['sourceNamespace'], row['incarnation'], row['authoredCamera'], row['evaluatedCamera']) for row in shared}) == 1,
            'Pose duplicated or rebound the shared lens definition')
    for row in mappings:
        require(all(type(row[key]) is int and row[key] > 0 for key in ('sourceNamespace', 'incarnation', 'graphIdentity')), 'Missing camera namespace authority')
        if baseline is not None:
            old = next((r for r in baseline['gltfCameraAuthoring']['cameras'] if r['graphNode'] == row['graphNode']), None)
            require(old is not None and all(row[key] == old[key] for key in
                ('graphNode', 'sourceNode', 'authoredCamera', 'evaluatedCamera', 'sourceNamespace', 'incarnation', 'graphIdentity')), 'Pose changed camera source identity')
    wanted_lenses = lens_records(expected['near'])
    for domain in ('authored', 'evaluated', 'rests'):
        require(len(cameras[domain]) == 2, 'Pose edit lost camera definitions')
        for actual, wanted in zip(cameras[domain], wanted_lenses):
            require(actual['name'] == wanted['name'] and actual['type'] == wanted['type'], 'Pose changed lens definition identity/type')
            require(actual['values'] == wanted['values'], 'Pose changed exact '+domain+' lens values')
    selected = expected['selected'];selected_source = -1 if selected < 0 else pose_by_name[NAMES[selected]][0]['sourceNode']
    if reload:
        destination = expected['destination'];destination_source = -1 if destination < 0 else pose_by_name[NAMES[destination]][0]['sourceNode']
        require(poses['destinationSourceNode'] == destination_source, 'Portable weak destination was rebound to a different semantic node')
    require(cameras['selectedSourceNode'] == selected_source and state['gltfAnimation']['selectedCameraNode'] == selected_source,
            'Pose write or replay changed authored-camera selection')
    if selected >= 0:
        lens = wanted_lenses[0 if selected in (1, 2) else 1];view = view_from_world(worlds[selected])
        close(cameras['view'], flat(view), 'selected world eye/forward/up/roll')
        close(cameras['projection'], flat(projection.projection_matrix(lens, expected['resolution'])), 'unchanged selected projection')
        close(cameras['shadowProjection'], flat(projection.projection_matrix(lens, expected['resolution'], True)), 'unchanged selected shadow projection')
        close(state['camera']['position'], worlds[selected][:3, 3], 'actual current camera eye')
    else:
        require(not any(key in cameras for key in ('view', 'projection', 'shadowProjection')), 'Free viewport retained an authored view')
        close(state['camera']['position'], expected['nav']['position'], 'free viewport eye')
        free_lens = dict(type='perspective', values=[math.pi/4, 0., .05, 500., 0., 0., 0., 0.])
        close(np.asarray(state['camera']['unjitteredViewProjColumns']).T,
              projection.projection_matrix(free_lens, expected['resolution'])@np.linalg.inv(navigation_world(expected['nav'])), 'actual free unjittered camera')
    if expected['time'] is not None and not control:
        player = state['gltfAnimation'];require(player['selectedClip'] == expected['clip'] and not player['playing'] and player['loop'], 'Pose history rewound animation controls')
        close(player['time'], expected['time'], 'current clock');close(player['speed'], 1., 'playback speed')
        if expected['clip'] == 0:
            first = next(m for m in state['gltfMaterialBindings']['materials'] if m['authored']['name'] == 'Surface 0')
            a, b = map(float, f32([.2, .8]));close(first['evaluated']['pbrMetallicRoughness']['roughnessFactor'], a+(b-a)*expected['time']/2, 'unrelated sampled roughness')
    point_lights = state['gltfAnimation']['pointLights'];light_count = 1 if expected['mode'] == 'actor' else 0
    require(type(point_lights) is list and len(point_lights) == light_count and
            state['lighting']['pointLightCount'] == light_count and state['lighting']['areaLightCount'] == 0,
            'Pose changed the exact authored local light inventory')
    if expected['mode'] == 'actor':
        light_rows = [r for r in rows if r['name'] == 'Camera child light' and r['sourceNode'] >= 0]
        require(len(light_rows) == 1, 'Camera child light source was lost')
        close(np.asarray(light_rows[0]['worldColumns']).T, worlds[12], 'child light graph world')
        light = point_lights[0];require(set(light) == {'position', 'color', 'intensity', 'range'}, 'Malformed normal-capture point light telemetry')
        close(light['color'], f32([1., .7, .2]), 'unchanged child light RGB identity', 0.)
        require(light['intensity'] == 20. and light['range'] == 6., 'Pose changed child light intensity/range')
        close(light['position'], worlds[12][:3, 3], 'derived child light payload position')
    if baseline is not None:
        offset = baseline['gltfCameraAuthoring']['revision']-baseline['gltfCameraPoses']['publications']-baseline['gltfCameraAuthoring']['publications']
        require(cameras['revision'] == offset+expected['pose']+expected['lens'], 'Pose/lens publication skipped or double-counted authoring revision')
        require(cameras['asset'] == baseline['gltfCameraAuthoring']['asset'], 'Pose replaced its asset')
        require(state['gltfMerge']['sourceTables'] == baseline['gltfMerge']['sourceTables'], 'Pose changed source table inventory')
        structural = lambda source: [{key: value for key, value in row.items() if key not in ('localColumns', 'worldColumns')} for row in projection.graph_rows(source)]
        require(structural(state) == structural(baseline), 'Pose changed graph birth/topology/source/geometry/material bindings')
    return dict(passed=True, posePublications=poses['publications'], lensPublications=cameras['publications'],
                viewportActive=poses['viewportActive'], viewRevision=poses['viewRevision'], evidence='CPU source/rest/node-birth/graph/light proof, not GPU readback')


def inspect_export(path, expected, *, literal_time=None, literal=False, authored_source=False):
    require(path.stat().st_size <= 16*1024*1024, 'Portable pose fixture exceeded its finite file bound')
    model, buffers = load_asset(path);require(finite_json(model), 'Nonfinite portable pose model')
    require(sum(map(len, buffers)) <= 16*1024*1024, 'Portable pose binary exceeded its finite bound')
    original = expected_nodes(expected['mode'], expected['endpoint'], literal_time, evaluated=literal_time is not None);count = 6 if expected['mode'] == 'actor' else 5
    nodes = model['nodes'];require(len(nodes) <= 96 and len(model['scenes']) == 2, 'Portable pose scene/hierarchy inventory changed')
    for node in nodes:
        require(not ('matrix' in node and any(key in node for key in ('translation', 'rotation', 'scale'))), 'Portable node mixes matrix and TRS representations')
        for key, length in [('matrix', 16), ('translation', 3), ('rotation', 4), ('scale', 3)]:
            require(key not in node or len(node[key]) == length, 'Portable node has a malformed transform field')
        if 'matrix' in node:
            require([node['matrix'][i] for i in (3, 7, 11, 15)] == [0., 0., 0., 1.], 'Portable raw node matrix has a noncanonical affine homogeneous row')
    mapping = {}
    for i, source in enumerate(original):
        found = [j for j, row in enumerate(nodes) if row.get('extras', {}).get('nodeIdentity') == 1000+i]
        require(len(found) == 1, 'Portable original source node identity is missing/duplicated');mapping[i] = found[0]
        actual = nodes[found[0]];require(actual.get('name') == source['name'] and actual.get('extras') == source['extras'], 'Portable node metadata changed')
        require(actual.get('camera', -1) == source.get('camera', -1), 'Portable camera node association changed')
        close(node_matrix(actual), node_matrix(source), 'portable local endpoint')
    source_ids = set(mapping.values());primitive_nodes = {};materials = {};incoming = [[] for _ in nodes];light_carrier = None
    for i, node in enumerate(nodes):
        for child in node.get('children', []):
            require(type(child) is int and 0 <= child < len(nodes), 'Invalid portable child reference');incoming[child].append(i)
    for i, source in enumerate(original):
        actual = nodes[mapping[i]];children = actual.get('children', [])
        require([j for j in children if j in source_ids] == [mapping[c] for c in source.get('children', [])], 'Portable authored parent/sibling links changed')
        extra = [j for j in children if j not in source_ids]
        if 'mesh' not in source:
            if expected['mode'] == 'actor' and i == 12 and not (authored_source or literal):
                require('mesh' not in actual and LIGHT not in actual.get('extensions', {}) and len(extra) == 1,
                        'Portable point light owner did not transfer its exact binding to one carrier')
                light_carrier = extra[0];carrier = nodes[light_carrier]
                require(set(carrier) == {'name', 'matrix', 'extensions'} and carrier['name'] == 'Imported Point 0' and
                        carrier['extensions'] == {LIGHT: dict(light=1)} and incoming[light_carrier] == [mapping[12]],
                        'Portable imported point carrier identity/parent/binding changed')
                continue
            require('mesh' not in actual and not extra, 'Portable nonmesh owner acquired a primitive');continue
        identity = source['mesh']
        if 'mesh' in actual:
            require(not extra, 'Portable flat mesh owner acquired another primitive');mesh_node = mapping[i]
        else:
            require(len(extra) == 1, 'Portable geometry owner lost/duplicated its primitive');mesh_node = extra[0]
            child = nodes[mesh_node];require(child.get('name') == 'Primitive '+str(identity) and incoming[mesh_node] == [mapping[i]] and
                not child.get('children') and 'camera' not in child, 'Portable primitive parent/identity changed')
            close(node_matrix(child), np.eye(4), 'portable primitive local', 0.)
        primitive_nodes[identity] = mesh_node;mesh_id = nodes[mesh_node]['mesh']
        require(type(mesh_id) is int and 0 <= mesh_id < len(model['meshes']), 'Invalid portable mesh reference');mesh = model['meshes'][mesh_id]
        require(mesh.get('extras') == dict(geometryIdentity=identity) and len(mesh['primitives']) == 1, 'Portable primitive metadata/count changed')
        primitive = mesh['primitives'][0];material = primitive['material'];materials[identity] = material
        require(type(material) is int and 0 <= material < len(model['materials']) and primitive.get('mode', 4) == 4, 'Invalid portable primitive/material reference')
        for attribute, wanted in [('POSITION', f32(positions(identity))), ('NORMAL', [[0., 0., 1.]]*4),
            ('TANGENT', [[1., 0., 0., 1.]]*4), ('TEXCOORD_0', [[0., 0.], [1., 0.], [1., 1.], [0., 1.]])]:
            close(decode_accessor(path, primitive['attributes'][attribute]), wanted, 'portable '+attribute, 0.)
        close(np.asarray(decode_accessor(path, primitive['indices'])).reshape(-1), projection.INDICES, 'portable indices', 0.)
        wanted = material_record(identity)
        if identity < 5: binding.inspect_record(model['materials'][material], wanted, serialized=True)
        else: binding.deep_contains(model['materials'][material], wanted, 'portable camera body material')
        require([j for j, m in enumerate(model['materials']) if m.get('name') == wanted['name']] == [material], 'Portable material identity was duplicated/rebound')
    require(len(model['meshes']) == count and set(primitive_nodes) == set(range(count)) and
        {i for i, n in enumerate(nodes) if 'mesh' in n} == set(primitive_nodes.values()), 'Portable geometry inventory changed')
    extras = set(range(len(nodes)))-source_ids-set(primitive_nodes.values())-({light_carrier} if light_carrier is not None else set())
    require(len(extras) <= 1, 'Portable export added unexplained hierarchy nodes')
    for i in extras: require(nodes[i].get('name') == 'Scene Root' and not nodes[i].get('children') and not incoming[i] and
        all(i not in scene['nodes'] for scene in model['scenes']), 'Editor root leaked into portable authored scenes')
    expected_roots = [[0, 3, 5, 6, *range(7, 12)], [3, 5, 6, *range(7, 12)]]
    for scene, roots in zip(model['scenes'], expected_roots): require(scene['nodes'] == [mapping[i] for i in roots], 'Portable scene roots changed')
    require(model.get('scene', 0) == 0, 'Portable active scene changed')
    lenses = lens_records(expected['near']);require(len(model['cameras']) == 2, 'Portable lens inventory changed')
    for actual, camera in zip(model['cameras'], lenses):
        wanted = projection.camera_json(camera);binding.deep_contains(actual, wanted, 'portable unchanged lens')
        lens = actual[camera['type']]
        if camera['type'] == 'perspective': require('aspectRatio' not in lens and 'zfar' not in lens, 'Pose export materialized optional lens omission')
    clips = model.get('animations', []);require(len(clips) == (2 if expected['mode'] == 'animated' and not literal else 0), 'Portable raw clip inventory changed')
    if clips:
        expected_clips = [([('node', 0, 'translation'), ('material', 0, 'roughnessFactor')], [[.3, .2, 6.], [1.1, .2, 6.]], [.2, .8]),
                          ([('node', 2, 'translation')], [[-.8, .3, 0.], [-.6, .3, 0.]], None)]
        restores = [[('node', 2, 'translation', [-.8, .3, 0.])],
                    [('node', 0, 'translation', [.3, .2, 6.]), ('material', 0, 'roughnessFactor', [.7])]]
        def target_for(domain, source, prop):
            return dict(node=mapping[source], path=prop) if domain == 'node' else dict(path='pointer',
                extensions={POINTER: dict(pointer='/materials/'+str(materials[source])+'/pbrMetallicRoughness/'+prop)})
        def accessor(index, kind, count, label):
            require(type(index) is int and 0 <= index < len(model['accessors']), 'Invalid '+label+' accessor index')
            value = model['accessors'][index]
            require(value['type'] == kind and value['count'] == count and value['componentType'] == 5126 and
                    not value.get('normalized', False) and 'sparse' not in value, 'Wrong '+label+' FLOAT accessor shape')
        for index, (clip, (targets, vectors, scalars)) in enumerate(zip(clips, expected_clips)):
            added = [] if authored_source else restores[index];total = len(targets)+len(added)
            require(len(clip['channels']) == total and len(clip['samplers']) == total, 'Portable channel/sampler count changed')
            require(clip.get('extras') == (dict(curveMetadata='preserve keys') if index == 0 else dict(blockerMetadata=19)), 'Portable clip metadata changed')
            for ordinal, (channel, (domain, source, prop)) in enumerate(zip(clip['channels'][:len(targets)], targets)):
                identity = ordinal if index == 0 else 2
                require(set(channel) == {'sampler', 'target', 'extras'} and channel.get('extras') == dict(channelIdentity=20+identity) and channel['sampler'] == ordinal,
                        'Portable channel metadata/sampler association changed')
                require(channel['target'] == target_for(domain, source, prop), 'Portable original curve retargeted')
                sampler = clip['samplers'][channel['sampler']];require(set(sampler) == {'input', 'output', 'interpolation', 'extras'} and sampler.get('interpolation') == 'LINEAR' and
                    sampler.get('extras') == dict(samplerIdentity=10+identity), 'Portable sampler metadata/interpolation changed')
                accessor(sampler['input'], 'SCALAR', 2, 'original curve input')
                accessor(sampler['output'], 'SCALAR' if domain == 'material' else 'VEC3', 2, 'original curve output')
                close(np.asarray(decode_accessor(path, sampler['input'])).reshape(-1), [0., 2.], 'portable original times', 0.)
                samples = scalars if domain == 'material' else vectors
                actual = np.asarray(decode_accessor(path, sampler['output']));wanted = f32(samples)
                close(actual.reshape(-1), wanted.reshape(-1), 'portable original curve keys', 0.)
            for ordinal, (domain, source, prop, values) in enumerate(added, start=len(targets)):
                channel = clip['channels'][ordinal]
                require(channel == dict(sampler=ordinal, target=target_for(domain, source, prop)), 'Unexpected/retargeted portable restore channel')
                sampler = clip['samplers'][ordinal]
                require(set(sampler) == {'input', 'output', 'interpolation'} and sampler['interpolation'] == 'STEP', 'Portable restore is not an exact STEP sampler')
                accessor(sampler['input'], 'SCALAR', 1, 'restore input')
                accessor(sampler['output'], 'SCALAR' if domain == 'material' else 'VEC3', 1, 'restore output')
                close(np.asarray(decode_accessor(path, sampler['input'])).reshape(-1), [0.], 'portable restore single time zero', 0.)
                close(np.asarray(decode_accessor(path, sampler['output'])).reshape(-1), f32(values).reshape(-1), 'portable exact authored restore value', 0.)
    if expected['mode'] == 'actor':
        light = model['extensions'][LIGHT]['lights']
        wanted = dict(name='Camera child point', type='point', color=[1., .7, .2], intensity=20., range=6., extras=dict(lightIdentity=31))
        direct = authored_source or literal
        require(len(light) == (1 if direct else 2) and light[0] == wanted, 'Portable original source light definition changed')
        target = mapping[12] if direct else light_carrier;definition = 0 if direct else 1
        require(target is not None, 'Portable point light carrier is missing')
        if not direct:
            require(light[1] == wanted, 'Portable renderer point light values or retained metadata changed')
            worlds = world_matrices(nodes);authored_world = world_matrices(original)[12]
            desired = np.eye(4);desired[:3, 3] = authored_world[:3, 3]
            close(worlds[target], desired, 'portable point carrier world/scale-shear cancellation')
            close(node_matrix(nodes[target]), np.linalg.inv(worlds[mapping[12]])@desired, 'portable point carrier local inverse-parent transform')
        refs = [(i, row['extensions'][LIGHT]) for i, row in enumerate(nodes) if LIGHT in row.get('extensions', {})]
        require(refs == [(target, dict(light=definition))], 'Portable light binding is duplicated, lost or retargeted')
        for ordinal, scene in enumerate(model['scenes']):
            reached = set();pending = list(scene['nodes'])
            while pending:
                node = pending.pop();require(node not in reached, 'Portable light scene contains repeated node ownership');reached.add(node)
                pending.extend(nodes[node].get('children', []))
            active = [(i, ref) for i, ref in refs if i in reached]
            require(active == (refs if ordinal == 0 else []), 'Portable light scene membership changed')
    for accessor in model['accessors']:
        if 'bufferView' in accessor: require(0 <= accessor['bufferView'] < len(model['bufferViews']), 'Invalid portable accessor view')
    for view in model['bufferViews']: require(0 <= view['buffer'] < len(buffers) and 0 <= view.get('byteOffset', 0) <= len(buffers[view['buffer']]) and
        view['byteLength'] <= len(buffers[view['buffer']])-view.get('byteOffset', 0), 'Invalid portable binary range')
    require(not model.get('images') and not model.get('textures') and not model.get('skins'), 'Pose fixture acquired unrelated resource references')
    for buffer in model['buffers']:
        if 'uri' in buffer: require(not Path(buffer['uri']).is_absolute() and not buffer['uri'].startswith(('file:', 'http:', 'https:')), 'Portable buffer points outside export')
    return dict(passed=True, path=str(path), sha256=sha(path), originalNodes=len(original), geometryCount=count,
                exactLensPreserved=True, semanticAnimationTargets=True, evidence='Raw portable source/geometry/reference inspection')


def write_restore_oracle(source, destination):
    """Independent bounded fixture for the three known cross-clip rest leaves."""
    model, buffers = load_asset(source);require(len(buffers) == 1, 'Restore fixture requires one original buffer')
    model = copy.deepcopy(model);binary = bytearray(buffers[0]);binary.extend(b'\0'*(-len(binary) % 4))
    def values(data, kind):
        offset = len(binary);encoded = struct.pack('<'+'f'*len(data), *data);binary.extend(encoded)
        model['bufferViews'].append(dict(buffer=0, byteOffset=offset, byteLength=len(encoded)))
        model['accessors'].append(dict(bufferView=len(model['bufferViews'])-1, byteOffset=0, componentType=5126, count=1, type=kind))
        return len(model['accessors'])-1
    time = values([0.], 'SCALAR');model['accessors'][time].update(min=[0.], max=[0.])
    parent = next(i for i, row in enumerate(model['nodes']) if row.get('extras', {}).get('nodeIdentity') == 1000)
    shared = next(i for i, row in enumerate(model['nodes']) if row.get('extras', {}).get('nodeIdentity') == 1002)
    surface = next(i for i, row in enumerate(model['materials']) if row.get('name') == 'Surface 0')
    targets = [[(dict(node=shared, path='translation'), [-.8, .3, 0.], 'VEC3')],
               [(dict(node=parent, path='translation'), [.3, .2, 6.], 'VEC3'),
                (dict(path='pointer', extensions={POINTER: dict(pointer='/materials/'+str(surface)+'/pbrMetallicRoughness/roughnessFactor')}), [.7], 'SCALAR')]]
    require([len(clip['channels']) for clip in model['animations']] == [2, 1], 'Restore oracle requires untouched authored channels')
    for clip, missing in zip(model['animations'], targets):
        for target, data, kind in missing:
            output = values(data, kind);sampler = len(clip['samplers'])
            clip['samplers'].append(dict(input=time, output=output, interpolation='STEP'))
            clip['channels'].append(dict(sampler=sampler, target=target))
    require(bytes(binary[:len(buffers[0])]) == buffers[0], 'Restore fixture changed original encoded bytes')
    model['buffers'][0].update(byteLength=len(binary), uri='data:application/octet-stream;base64,'+base64.b64encode(binary).decode('ascii'))
    document(destination, model)


def write_point_carrier_oracle(source, destination):
    """Independent point-light overlay fixture; original source row stays intact."""
    model, _ = load_asset(source);model = copy.deepcopy(model)
    owner = next(i for i, row in enumerate(model['nodes']) if row.get('extras', {}).get('nodeIdentity') == 1012)
    require(model['nodes'][owner]['extensions'][LIGHT] == dict(light=0) and len(model['extensions'][LIGHT]['lights']) == 1,
            'Point carrier oracle requires the untouched authored light binding')
    worlds = world_matrices(model['nodes']);desired = np.eye(4);desired[:3, 3] = f32(worlds[owner][:3, 3])
    local = f32(np.linalg.inv(worlds[owner])@desired)
    light = copy.deepcopy(model['extensions'][LIGHT]['lights'][0])
    model['extensions'][LIGHT]['lights'].append(light)
    model['nodes'][owner]['extensions'].pop(LIGHT)
    if not model['nodes'][owner]['extensions']: model['nodes'][owner].pop('extensions')
    model['nodes'][owner].setdefault('children', []).append(len(model['nodes']))
    model['nodes'].append(dict(name='Imported Point 0', matrix=flat(local), extensions={LIGHT: dict(light=1)}))
    document(destination, model)


def dependencies():
    result = dict(projection.dependencies())
    for path in (Path(__file__).resolve(), Path(projection.__file__).resolve()): result[str(path)] = sha(path)
    return dict(sorted(result.items()))


def failure_signature(state):
    result = projection.failure_signature(state);result['gltfCameraPoses'] = copy.deepcopy(state['gltfCameraPoses'])
    result['camera'] = {key: state['camera'][key] for key in ('position', 'unjitteredViewProjColumns')}
    result['lights'] = {key: state['lighting'][key] for key in ('pointLightCount', 'areaLightCount')}
    return result


def check_literal_context(expected, control):
    reference = control['step']
    require(all(reference[key] == expected[key] for key in ('mode', 'endpoint', 'selected', 'near', 'nav', 'resolution', 'msaa', 'scene')),
            'Pose literal control has a different source/lens/selection/viewport context')
    require(control['literalTime'] == expected.get('literalTime', expected['time'] if expected['clip'] == 0 else None), 'Pose literal baked the wrong parent clock')
    script = json.loads(Path(control['script']).read_text(encoding='utf-8'))
    expected_events = [] if expected['selected'] < 0 else [dict(frame=1, cameraSelection=dict(nodeName=NAMES[expected['selected']]))]
    require(script['events'] == expected_events and script['camera'] == [dict(frame=1, **expected['nav'])] and
            script['frames'] == 13 and script['sampleFrames'] == [13], 'Pose literal has a different selection/overlay or navigation event')


def run_native(args, output, plan, report):
    require(args.renderer.is_file() and args.manifest.is_file(), 'Missing pose renderer/frozen manifest')
    require(not any(output.glob('captures/*')) and not any(output.glob('exports/**/*')) and not any(output.glob('portable/**/*')), 'Pose evidence directories must be fresh')
    if (output/'results.json').exists():
        previous = json.loads((output/'results.json').read_text());require(previous.get('generatedOnly') and not previous.get('rendererExecuted'), 'Refusing to overwrite native pose evidence')
    manifest_hash = sha(args.manifest)
    def persist():
        for key in ('captures', 'processes', 'stateChecks', 'imageChecks', 'exports'): report[key+'Count'] = len(report[key])
        (output/'results.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    def runtime_files(): return sorted([*args.renderer.parent.glob('*.dll'), *(args.renderer.parent/'spv_shaders').glob('*.spv')])
    def verify():
        require(sha(args.manifest) == manifest_hash and dependencies() == report['sourceDependencies'], 'Frozen pose manifest/helper changed')
        result = verify_manifest(args.manifest)
        require(sha(output/'launch-plan.json') == report['launchPlanSha256'], 'Pose launch plan changed')
        require((output/'vk_layer_settings.txt').read_bytes() == b'khronos_validation.validate_sync = true\n', 'Pose synchronization validation settings changed')
        for entry in plan['immutableInputs']:
            path = Path(entry['path']);require(path.stat().st_size == entry['byteLength'] and sha(path) == entry['sha256'], 'Pose input fixture/script changed')
        if 'runtimeInputs' in report: require([str(path.resolve()) for path in runtime_files()] == report['runtimeInputs'], 'Resident pose DLL/shader inventory changed')
        for copied in report['portableCopies']:
            require(all(sha(Path(row['path'])) == row['sha256'] for row in copied['files']), 'Portable pose files changed')
        return result
    def check(kind, technique, name, result): report[kind].append(dict(technique=technique, check=name, **result));persist()
    try:
        report['nativeManifestBefore'] = verify_manifest(args.manifest)
        frozen = {Path(path).resolve() for path in json.loads(args.manifest.read_text(encoding='utf-8-sig'))['files']}
        runtime = runtime_files();require(any(path.suffix == '.spv' for path in runtime), 'No resident pose shader inputs')
        require({args.renderer, *map(Path, report['sourceDependencies']), *map(Path.resolve, runtime)} <= frozen, 'Pose manifest omits renderer/transitive helper/DLL/SPIR-V inputs')
        report.update(rendererSha256=sha(args.renderer), runtimeInputs=[str(path.resolve()) for path in runtime])
        immutable(output/'vk_layer_settings.txt', b'khronos_validation.validate_sync = true\n');persist();verify()
        for technique in args.technique:
            controls = {}
            for control in plan['controls']:
                expected = control['step'];check_literal_context(expected, control)
                record = binding.launch(args, output, report, persist, verify, technique, control['name'], Path(control['source']),
                    [expected], script=Path(control['script']), initial=expected)['control']
                check('stateChecks', technique, control['name'], check_state(record['state'], expected, control=True, literal_time=control['literalTime']))
                check('imageChecks', technique, control['name']+'-visible', projection.visible(record));controls[control['name']] = record
            cohorts = {}
            for run in plan['runs']:
                save = output/'exports'/technique/run['name']/('scene.'+run['save'])
                captures = binding.launch(args, output, report, persist, verify, technique, run['name'], Path(plan['sources'][run['mode']]),
                    run['steps'], script=Path(run['script']), save=save, animation_time=.5 if run['mode'] == 'animated' else None)
                cohorts[run['name']] = captures;baseline = captures[run['steps'][0]['label']]['state']
                projection.temporal_check(captures, run['steps'])
                log = Path(captures[run['steps'][0]['label']]['log']).read_text(encoding='utf-8', errors='replace')
                require(log.count('Expected capture camera pose failure: ') == sum(bool(event.get('expectCameraPoseFailure')) for event in run['events']),
                        'Missing explicit expected camera pose rejection diagnostics')
                for expected in run['steps']:
                    record = captures[expected['label']];reference = controls[expected['control']]
                    check('stateChecks', technique, run['name']+'/'+expected['label'], check_state(record['state'], expected, baseline))
                    require(record['state']['gui']['selectedMeshNodeId'] == reference['state']['gui']['selectedMeshNodeId'], 'Literal pose has different selection/outline context')
                    check('imageChecks', technique, run['name']+'/'+expected['label'], projection.compare_images(record, reference))
                    if expected['failureAs']:
                        old = captures[expected['failureAs']]
                        require(failure_signature(record['state']) == failure_signature(old['state']), 'Rejected pose changed source/rest/graph/lights/history/clock/render state')
                        check('imageChecks', technique, run['name']+'/'+expected['label']+'-failure-unchanged', projection.compare_images(record, old))
                last = run['steps'][-1];report['exports'].append(dict(technique=technique, **inspect_export(save, last)));persist()
                copied, files = portable_copy(save, output/'portable'/technique/run['name']);report['portableCopies'].append(dict(files=files));persist()
                expected = copy.deepcopy(last);expected.update(label='reload', frame=13, pose=0, lens=0, history=[0, 0])
                record = binding.launch(args, output, report, persist, verify, technique, run['name']+'-reload', copied, [expected],
                    script=Path(run['reloadScript']), initial=expected)['reload']
                check('stateChecks', technique, run['name']+'-portable-reload', check_state(record['state'], expected, reload=True))
                check('imageChecks', technique, run['name']+'-portable-reload', projection.compare_images(record, captures[last['label']]))
            static, animated, actor = (cohorts[name] for name in ('static-shared', 'animated-parent', 'mesh-child-light'))
            for name, first, second in [
                ('second-viewport-pose-positive', static['view-written'], static['main-again']),
                ('shared-definition-independent-node-positive', static['shared'], static['main-again']),
                ('animated-parent-original-to-written-positive', animated['before'], animated['view-written']),
                ('current-parent-clock-positive', animated['view-written'], animated['seek']),
                ('camera-bearing-mesh-pose-positive', actor['before'], actor['view-written']),
                ('derived-descendant-geometry-positive', actor['undo-shared'], actor['redo-shared'])]:
                check('imageChecks', technique, name, projection.changed(first, second))
        multiplier = len(args.technique)
        require(len(report['captures']) == plan['expectedCaptureCount']*multiplier and len(report['processes']) == plan['expectedProcessCount']*multiplier,
                'Incomplete native pose capture/process inventory')
        require(len(report['stateChecks']) == len(report['captures']) and len(report['imageChecks']) == plan['expectedImageCheckCount']*multiplier and
                len(report['exports']) == plan['expectedExportCount']*multiplier, 'Incomplete native pose proof inventory')
        report['nativeManifestAfter'] = verify();report.update(passed=True, nativeValidationPerformed=True)
    except Exception as error: report.update(passed=False, failure=str(error));persist();raise
    finally:
        if report['rendererExecuted']:
            try: report['nativeManifestAfter'] = verify()
            except Exception as error: report.update(passed=False, afterManifestFailure=str(error));report.setdefault('failure', str(error))
        persist()
    require(report['passed'], 'Native pose provenance failed after execution')


def synthetic(expected, *, literal_time=None, control=False):
    current_time = literal_time if control else expected['time'] if expected['clip'] == 0 else None
    authored = expected_nodes(expected['mode'], expected['endpoint'])
    evaluated = expected_nodes(expected['mode'], expected['endpoint'], current_time, evaluated=True);worlds = world_matrices(evaluated)
    camera = dict(asset=9, revision=10+expected['pose']+expected['lens'], publications=expected['lens'], selectedSourceNode=expected['selected'],
        truncated=False, authored=copy.deepcopy(lens_records(expected['near'])), evaluated=copy.deepcopy(lens_records(expected['near'])),
        rests=copy.deepcopy(lens_records(expected['near'])), cameras=[])
    poses = dict(publications=expected['pose'], viewRevision=7, viewportActive=expected['selected'] < 0,
                 destinationSourceNode=expected['destination'], truncated=False, nodes=[])
    rows = []
    def record(node): return dict(nameTruncated=False, **pose_record(node))
    for i, node in enumerate(evaluated):
        rows.append(dict(graphNode=i+1, sourceNode=i, name=node['name'], active=True, retired=False,
                         localColumns=node_matrix(node).T.tolist(), worldColumns=worlds[i].T.tolist()))
        if 'camera' in node:
            poses['nodes'].append(dict(graphNode=i+1, sourceNode=i, nodeIncarnation=1000+i,
                authored=record(authored[i]), evaluated=record(node), rest=record(authored[i])))
            camera['cameras'].append(dict(graphNode=i+1, sourceNode=i, authoredCamera=node['camera'], evaluatedCamera=node['camera'],
                sourceNamespace=7, incarnation=100+node['camera'], graphIdentity=6, graphRevision=1))
    selected = expected['selected']
    if selected >= 0:
        view = view_from_world(worlds[selected]);lens = lens_records(expected['near'])[0 if selected in (1, 2) else 1]
        proj = projection.projection_matrix(lens, expected['resolution']);position = worlds[selected][:3, 3]
        camera.update(view=flat(view), projection=flat(proj), shadowProjection=flat(projection.projection_matrix(lens, expected['resolution'], True)))
    else:
        view = np.linalg.inv(navigation_world(expected['nav']));position = expected['nav']['position']
        proj = projection.projection_matrix(dict(type='perspective', values=[math.pi/4, 0., .05, 500., 0., 0., 0., 0.]), expected['resolution'])
    count = 6 if expected['mode'] == 'actor' else 5
    state = dict(gltfCameraPoses=poses, gltfCameraAuthoring=camera,
        gltfAnimation=dict(selectedCameraNode=selected, selectedClip=expected['clip'], time=expected['time'] or 0., playing=False, loop=True, speed=1., pointLights=[]),
        camera=dict(position=np.asarray(position).tolist(), unjitteredViewProjColumns=(proj@view).T.tolist()),
        resolution=[WIDTH, HEIGHT], msaaSamples=1,
        scene=dict(primaryObjectCount=count, primaryVertexCount=count*4, primaryIndexCount=count*6),
        gltfMerge=dict(graphIdentity=6, graphNodeCount=len(rows), authoringNodesTruncated=False, authoringNodes=rows, sourceTables={}),
        gltfScenes=dict(selected=0, editHistoryDepth=sum(expected['history'])), gltfMaterialVariants=dict(undoCount=expected['history'][0], redoCount=expected['history'][1]),
        taa=dict(epoch=100+expected['pose']+expected['lens'], resetReason='synthetic pose control'), lighting=dict(pointLightCount=0, areaLightCount=0))
    if expected['mode'] == 'actor':
        state['lighting']['pointLightCount'] = 1
        state['gltfAnimation']['pointLights'] = [dict(position=worlds[12][:3, 3].tolist(), color=f32([1., .7, .2]).tolist(), intensity=20., range=6.)]
    rough = .7
    if expected['clip'] == 0 and expected['time'] is not None:
        a, b = map(float, f32([.2, .8]));rough = a+(b-a)*expected['time']/2
    state['gltfMaterialBindings'] = dict(materials=[dict(authored=dict(name='Surface 0'), evaluated=dict(pbrMetallicRoughness=dict(roughnessFactor=rough)))])
    return state


def self_check():
    checks = []
    def reject(name, action):
        try: action()
        except (RuntimeError, ValueError, KeyError, TypeError, IndexError, StopIteration, np.linalg.LinAlgError): checks.append(name);return
        raise RuntimeError('Pose checker accepted damaged evidence: '+name)
    with tempfile.TemporaryDirectory(prefix='camera-view-pose-selfcheck-') as directory:
        output = Path(directory);plan = generate(output)
        require([plan[key]*2 for key in ('expectedCaptureCount', 'expectedProcessCount', 'expectedImageCheckCount', 'expectedExportCount')] ==
                [150, 70, 172, 6], 'Pose native cohort changed its exact declared bounds')
        controls = {row['name']: row for row in plan['controls']}
        for run in plan['runs']:
            script = json.loads(Path(run['script']).read_text());frames = [row['frame'] for row in script['events']]
            require(frames == sorted(set(frames)) and frames[-1] <= script['frames'] <= 256 and
                    script['sampleFrames'] == [row['frame'] for row in run['steps']], 'Pose events/captures are not strictly bounded and ordered')
            checks.append(run['name']+'-bounded-script')
            baseline = synthetic(run['steps'][0]);temporal = {};epoch = 100
            for expected in run['steps']:
                state = synthetic(expected);check_state(state, expected, baseline);checks.append(run['name']+'/'+expected['label']+'-state-oracle')
                check_literal_context(expected, controls[expected['control']])
                if not expected['failureAs']: epoch += 1
                state['taa']['epoch'] = epoch;temporal[expected['label']] = dict(state=state)
            projection.temporal_check(temporal, run['steps']);checks.append(run['name']+'-temporal-protocol')
            for expected in run['steps']:
                if expected['failureAs']:
                    require(failure_signature(temporal[expected['label']]['state']) == failure_signature(temporal[expected['failureAs']]['state']),
                            'Synthetic failed action changed its expected endpoint');checks.append(run['name']+'/'+expected['label']+'-exact-failure-state')
        static_script = json.loads(Path(plan['runs'][0]['script']).read_text())
        require(static_script['events'][0]['frame'] == 1 and static_script['events'][0]['cameraPose']['fromViewport'] is True,
                'Pose fixture no longer tests a valid first-frame write')
        write_frame = next(row['frame'] for row in static_script['events'] if row.get('cameraPose') and row['frame'] > 1 and not row.get('expectCameraPoseFailure'))
        require(static_script['camera'][-2:] == [dict(frame=write_frame-1, **NAV_A), dict(frame=write_frame, **NAV_B)],
                'Pose fixture no longer proves current-frame navigation is applied before write')
        checks.append('frame-one-and-same-frame-track-contract')
        for control in plan['controls']:
            expected = control['step'];check_literal_context(expected, control)
            check_state(synthetic(expected, literal_time=control['literalTime'], control=True), expected, literal_time=control['literalTime'], control=True)
            inspect_export(Path(control['source']), expected, literal_time=control['literalTime'], literal=True)
            checks.append(control['name']+'-independent-literal-control')
        expected = plan['runs'][0]['steps'][1];state = synthetic(expected)
        def wrong_world(s): s['gltfMerge']['authoringNodes'][1]['worldColumns'][3][0] += .3
        mutations = [
            ('missing-selected-pose', lambda s: s['gltfCameraPoses']['nodes'].pop(0)),
            ('wrong-node-birth', lambda s: s['gltfCameraPoses']['nodes'][0].__setitem__('nodeIncarnation', 0)),
            ('truncated-pose-source', lambda s: s['gltfCameraPoses'].__setitem__('truncated', True)),
            ('truncated-pose-name', lambda s: s['gltfCameraPoses']['nodes'][0]['authored'].__setitem__('nameTruncated', True)),
            ('wrong-authored-local', lambda s: s['gltfCameraPoses']['nodes'][0]['authored']['matrix'].__setitem__(12, .3)),
            ('wrong-evaluated-local', lambda s: s['gltfCameraPoses']['nodes'][0]['evaluated']['matrix'].__setitem__(12, .3)),
            ('wrong-rest-local', lambda s: s['gltfCameraPoses']['nodes'][0]['rest']['matrix'].__setitem__(12, .3)),
            ('wrong-world-pose', wrong_world),
            ('unrelated-sibling-pose', lambda s: s['gltfCameraPoses']['nodes'][1]['authored']['translation'].__setitem__(0, 7.)),
            ('unrelated-ambiguous-pose', lambda s: s['gltfCameraPoses']['nodes'][3]['rest']['translation'].__setitem__(0, 7.)),
            ('lost-definition-sharing', lambda s: s['gltfCameraAuthoring']['cameras'][1].__setitem__('incarnation', 999)),
            ('wrong-weak-destination', lambda s: s['gltfCameraPoses'].__setitem__('destinationSourceNode', 2)),
            ('wrong-viewport-mode', lambda s: s['gltfCameraPoses'].__setitem__('viewportActive', True)),
            ('extra-pose-publication', lambda s: s['gltfCameraPoses'].__setitem__('publications', 2)),
            ('double-camera-revision', lambda s: s['gltfCameraAuthoring'].__setitem__('revision', 99)),
            ('wrong-current-view', lambda s: s['gltfCameraAuthoring']['view'].__setitem__(12, .3)),
            ('wrong-shadow-camera', lambda s: s['gltfCameraAuthoring']['shadowProjection'].__setitem__(0, .3)),
            ('pose-copied-viewport-lens', lambda s: s['gltfCameraAuthoring']['authored'][0]['values'].__setitem__(0, math.pi/4)),
            ('wrong-history-redo', lambda s: s['gltfMaterialVariants'].__setitem__('redoCount', 1)),
            ('wrong-geometry-inventory', lambda s: s['scene'].__setitem__('primaryObjectCount', 4)),
            ('nonfinite-actual-view', lambda s: s['camera']['position'].__setitem__(0, float('nan')))]
        for name, mutate in mutations:
            bad = copy.deepcopy(state);mutate(bad);reject(name, lambda bad=bad: check_state(bad, expected, state))
        original = plan['runs'][0]['steps'][8];original_state = synthetic(original)
        original_state['gltfCameraPoses']['nodes'][0]['authored']['translation'][0] = math.nextafter(.15, 1.)
        reject('undo-original-TRS-one-ulp-change', lambda: check_state(original_state, original))
        altered = copy.deepcopy(state)
        for row in altered['gltfCameraAuthoring']['cameras']: row['sourceNamespace'] += 1
        reject('same-source-numbers-new-namespace', lambda: check_state(altered, expected, state))
        altered = copy.deepcopy(state);altered['gltfCameraPoses']['nodes'][0]['nodeIncarnation'] += 1
        reject('same-payload-new-node-birth', lambda: check_state(altered, expected, state))
        animated = plan['runs'][1]['steps'][4];animated_state = synthetic(animated)
        animated_state['gltfAnimation']['time'] = .5
        reject('history-rewound-current-parent-clock', lambda: check_state(animated_state, animated))
        animated_state = synthetic(animated);animated_state['gltfMaterialBindings']['materials'][0]['evaluated']['pbrMetallicRoughness']['roughnessFactor'] = .2
        reject('history-overwrote-unrelated-material-animation', lambda: check_state(animated_state, animated))
        actor = plan['runs'][2]['steps'][3];actor_state = synthetic(actor)
        actor_state['gltfAnimation']['pointLights'][0]['position'][0] += 1
        reject('stale-derived-child-light', lambda: check_state(actor_state, actor))
        normal = synthetic(actor)
        require('pointLights' not in normal['lighting'] and set(normal['gltfAnimation']['pointLights'][0]) == {'position', 'color', 'intensity', 'range'},
                'Synthetic normal capture regressed to the gfxrecon-only light schema')
        check_state(normal, actor);checks.append('normal-capture-animation-point-light-schema')
        wrong_schema = copy.deepcopy(normal);wrong_schema['gltfAnimation'].pop('pointLights')
        wrong_schema['lighting']['pointLights'] = [dict(positionRadius=[0., 0., 0., 6.], colorIntensity=[1., .7, .2, 20.])]
        reject('old-gfxrecon-only-point-light-schema', lambda: check_state(wrong_schema, actor))
        for name, mutate in [
            ('wrong-local-light-count', lambda s: s['lighting'].__setitem__('pointLightCount', 0)),
            ('wrong-area-light-count', lambda s: s['lighting'].__setitem__('areaLightCount', 1)),
            ('extra-local-light-payload', lambda s: s['gltfAnimation']['pointLights'].append(copy.deepcopy(s['gltfAnimation']['pointLights'][0]))),
            ('wrong-child-light-rgb', lambda s: s['gltfAnimation']['pointLights'][0]['color'].__setitem__(1, .8)),
            ('wrong-child-light-intensity', lambda s: s['gltfAnimation']['pointLights'][0].__setitem__('intensity', 21.)),
            ('wrong-child-light-range', lambda s: s['gltfAnimation']['pointLights'][0].__setitem__('range', 7.))]:
            bad = copy.deepcopy(normal);mutate(bad);reject(name, lambda bad=bad: check_state(bad, actor))
        free = plan['runs'][0]['steps'][3];free_state = synthetic(free)
        free_state['camera']['unjitteredViewProjColumns'][3][0] += .1
        reject('write-changed-free-viewport', lambda: check_state(free_state, free))
        first_control = copy.deepcopy(plan['controls'][0]);first_control['step']['selected'] = 2
        reject('literal-selection-overlay-mismatch', lambda: check_literal_context(plan['controls'][0]['step'], first_control))
        scene_zero = plan['runs'][0]['steps'][0];scene_one = copy.deepcopy(scene_zero);scene_one['scene'] = 1
        require(visual_key(scene_zero) != visual_key(scene_one), 'Literal pose key merged distinct scene/overlay contexts');checks.append('literal-scene-context-key')
        reload_expected = copy.deepcopy(plan['runs'][0]['steps'][-1]);reload_expected.update(pose=0, lens=0, history=[0, 0])
        remapped_state = synthetic(reload_expected)
        for row in remapped_state['gltfCameraPoses']['nodes']:
            row['sourceNode'] += 7
            for domain in ('authored', 'evaluated', 'rest'):
                value = row[domain];matrix = flat(node_matrix(value));value.update(matrix=matrix, translation=[], rotation=[], scale=[])
        for row in remapped_state['gltfCameraAuthoring']['cameras']: row['sourceNode'] += 7
        for row in remapped_state['gltfMerge']['authoringNodes']: row['sourceNode'] += 7
        remapped_state['gltfCameraPoses']['destinationSourceNode'] += 7
        remapped_state['gltfCameraAuthoring']['selectedSourceNode'] += 7
        remapped_state['gltfAnimation']['selectedCameraNode'] += 7
        check_state(remapped_state, reload_expected, reload=True);checks.append('portable-canonical-trs-and-semantic-node-remap')
        wrong_reload = copy.deepcopy(remapped_state);wrong_reload['gltfCameraPoses']['destinationSourceNode'] = 1
        reject('portable-stale-source-number-destination', lambda: check_state(wrong_reload, reload_expected, reload=True))
        wrong_reload = copy.deepcopy(remapped_state);wrong_reload['gltfAnimation']['selectedCameraNode'] = 1
        reject('portable-stale-source-number-selection', lambda: check_state(wrong_reload, reload_expected, reload=True))
        for run in plan['runs']:
            last = run['steps'][-1];source = output/(run['name']+'-portable.gltf')
            write_fixture(source, run['mode'], last['endpoint'], last['near'])
            inspect_export(source, last, authored_source=True);checks.append(run['name']+'-authored-flat-gltf')
            binary = source.with_suffix('.glb');projection.glb_oracle(source, binary);inspect_export(binary, last, authored_source=True);checks.append(run['name']+'-authored-flat-glb')
            if run['mode'] == 'animated':
                reject('portable-missing-all-cross-clip-restores-gltf', lambda source=source, last=last: inspect_export(source, last))
                reject('portable-missing-all-cross-clip-restores-glb', lambda binary=binary, last=last: inspect_export(binary, last))
                restored = output/(run['name']+'-portable-restores.gltf');write_restore_oracle(source, restored)
                inspect_export(restored, last);checks.append('independent-cross-clip-restores-gltf')
                restored_binary = restored.with_suffix('.glb');projection.glb_oracle(restored, restored_binary)
                inspect_export(restored_binary, last);checks.append('independent-cross-clip-restores-glb')
                reject('authored-source-cannot-hide-extra-restores', lambda: inspect_export(restored, last, authored_source=True))
                source = restored
            if run['mode'] == 'actor':
                reject('portable-missing-point-carrier-gltf', lambda source=source, last=last: inspect_export(source, last))
                reject('portable-missing-point-carrier-glb', lambda binary=binary, last=last: inspect_export(binary, last))
                overlaid = output/(run['name']+'-point-carrier.gltf');write_point_carrier_oracle(source, overlaid)
                inspect_export(overlaid, last);checks.append('independent-point-carrier-gltf')
                overlaid_binary = overlaid.with_suffix('.glb');projection.glb_oracle(overlaid, overlaid_binary)
                inspect_export(overlaid_binary, last);checks.append('independent-point-carrier-glb')
                reject('authored-source-cannot-hide-light-carrier', lambda: inspect_export(overlaid, last, authored_source=True))
                source = overlaid
            model, _ = load_asset(source);canonical = copy.deepcopy(model);owners = [];children = []
            for i, node in enumerate(model['nodes']):
                if 'mesh' not in node: continue
                owner = canonical['nodes'][i];identity = owner.pop('mesh');child = len(canonical['nodes'])
                owner.setdefault('children', []).append(child);canonical['nodes'].append(dict(name='Primitive '+str(identity), mesh=identity))
                canonical['meshes'][identity]['name'] = 'Primitive '+str(identity);owners.append(i);children.append(child)
            canonical['nodes'].append(dict(name='Scene Root'))
            canonical['materials'] = [dict(name='Runtime default 0'), dict(name='Runtime default 1'), *canonical['materials']]
            for mesh in canonical['meshes']:
                for primitive in mesh['primitives']: primitive['material'] += 2
            if run['mode'] == 'animated':
                for animation in canonical['animations']:
                    for channel in animation['channels']:
                        pointer = channel['target'].get('extensions', {}).get(POINTER)
                        if pointer and pointer['pointer'] == '/materials/0/pbrMetallicRoughness/roughnessFactor':
                            pointer['pointer'] = '/materials/2/pbrMetallicRoughness/roughnessFactor'
            canonical_path = output/(run['name']+'-canonical.gltf');document(canonical_path, canonical)
            inspect_export(canonical_path, last);checks.append(run['name']+'-canonical-semantic-remap-gltf')
            require(any('matrix' in row for row in canonical['nodes']) and
                    all([row['matrix'][i] for i in (3, 7, 11, 15)] == [0., 0., 0., 1.] for row in canonical['nodes'] if 'matrix' in row),
                    'Independent portable fixture lost exact affine homogeneous rows')
            checks.append(run['name']+'-independent-exact-raw-affine-rows')
            canonical_binary = canonical_path.with_suffix('.glb');projection.glb_oracle(canonical_path, canonical_binary)
            inspect_export(canonical_binary, last);checks.append(run['name']+'-canonical-semantic-remap-glb')
            def wrong_parent(m): m['nodes'][owners[0]]['children'].remove(children[0]);m['nodes'][0].setdefault('children', []).append(children[0])
            def compensated_transform(m):
                m['nodes'][owners[0]]['translation'] = [1., 0., 0.];m['nodes'][children[0]]['translation'] = [-1., 0., 0.]
            mutations = [
                ('wrong-camera-association', lambda m: m['nodes'][1].__setitem__('camera', 1)),
                ('noncanonical-raw-matrix-m44-one-float-ulp', lambda m: m['nodes'][1]['matrix'].__setitem__(15, float(np.nextafter(np.float32(1.), np.float32(0.))))),
                ('noncanonical-raw-matrix-m44-one-double-ulp', lambda m: m['nodes'][1]['matrix'].__setitem__(15, math.nextafter(1., 0.))),
                ('nonzero-raw-matrix-homogeneous-x', lambda m: m['nodes'][1]['matrix'].__setitem__(3, float(np.nextafter(np.float32(0.), np.float32(1.))))),
                ('nonzero-raw-matrix-homogeneous-y', lambda m: m['nodes'][1]['matrix'].__setitem__(7, float(np.nextafter(np.float32(0.), np.float32(1.))))),
                ('nonzero-raw-matrix-homogeneous-z', lambda m: m['nodes'][1]['matrix'].__setitem__(11, float(np.nextafter(np.float32(0.), np.float32(1.))))),
                ('lost-node-metadata', lambda m: m['nodes'][1]['extras']['retained'].__setitem__('sourceLike', 0)),
                ('duplicate-node-identity', lambda m: m['nodes'].append(copy.deepcopy(m['nodes'][1]))),
                ('wrong-source-owner-name', lambda m: m['nodes'][owners[0]].__setitem__('name', 'Replacement')),
                ('wrong-primitive-parent', wrong_parent),
                ('compensated-owner-child-transform', compensated_transform),
                ('extra-primitive', lambda m: m['nodes'].append(dict(name='Primitive extra', mesh=0))),
                ('wrong-geometry-material', lambda m: m['meshes'][0]['primitives'][0].__setitem__('material', 3)),
                ('duplicate-material-identity', lambda m: m['materials'].append(copy.deepcopy(m['materials'][2]))),
                ('lost-camera-shared-use', lambda m: m['nodes'][2].__setitem__('camera', 1)),
                ('perspective-far-materialized', lambda m: m['cameras'][0]['perspective'].__setitem__('zfar', 0.)),
                ('moved-scene-root', lambda m: m['scenes'][1]['nodes'].append(0))]
            if run['mode'] == 'animated':
                def change_curve_bytes(model, accessor_id, data):
                    target = model['accessors'][accessor_id];view = model['bufferViews'][target['bufferView']];buffer = model['buffers'][view['buffer']]
                    header, payload = buffer['uri'].split(',', 1);raw = bytearray(base64.b64decode(payload));offset = view.get('byteOffset', 0)+target.get('byteOffset', 0)
                    encoded = struct.pack('<'+'f'*len(data), *data);raw[offset:offset+len(encoded)] = encoded;buffer['uri'] = header+','+base64.b64encode(raw).decode('ascii')
                def missing_restore(model): model['animations'][0]['channels'].pop();model['animations'][0]['samplers'].pop()
                def extra_restore(model):
                    clip = model['animations'][0];channel = copy.deepcopy(clip['channels'][-1]);channel['sampler'] = len(clip['samplers'])
                    clip['channels'].append(channel);clip['samplers'].append(copy.deepcopy(clip['samplers'][-1]))
                mutations += [('stale-material-curve-target', lambda m: m['animations'][0]['channels'][1]['target']['extensions'][POINTER].__setitem__('pointer', '/materials/0/pbrMetallicRoughness/roughnessFactor')),
                    ('retargeted-parent-curve', lambda m: m['animations'][0]['channels'][0]['target'].__setitem__('node', 1)),
                    ('lost-channel-metadata', lambda m: m['animations'][0]['channels'][0]['extras'].__setitem__('channelIdentity', 99)),
                    ('lost-sampler-metadata', lambda m: m['animations'][0]['samplers'][0]['extras'].__setitem__('samplerIdentity', 99)),
                    ('missing-required-restore', missing_restore), ('duplicate-extra-restore', extra_restore),
                    ('wrong-restore-node', lambda m: m['animations'][0]['channels'][2]['target'].__setitem__('node', 1)),
                    ('wrong-restore-material-remap', lambda m: m['animations'][1]['channels'][2]['target']['extensions'][POINTER].__setitem__('pointer', '/materials/0/pbrMetallicRoughness/roughnessFactor')),
                    ('nonstep-restore', lambda m: m['animations'][0]['samplers'][2].__setitem__('interpolation', 'LINEAR')),
                    ('extra-restore-metadata', lambda m: m['animations'][0]['samplers'][2].__setitem__('extras', dict(unexpected=True))),
                    ('wrong-restore-time', lambda m: change_curve_bytes(m, m['animations'][0]['samplers'][2]['input'], [1.])),
                    ('wrong-restore-value', lambda m: change_curve_bytes(m, m['animations'][0]['samplers'][2]['output'], [-.7, .3, 0.])),
                    ('wrong-original-curve-byte', lambda m: change_curve_bytes(m, m['animations'][0]['samplers'][0]['output'], [.9])),
                    ('wrong-restore-output-shape', lambda m: m['accessors'][m['animations'][0]['samplers'][2]['output']].__setitem__('type', 'SCALAR'))]
            if run['mode'] == 'actor':
                carrier = next(i for i, row in enumerate(canonical['nodes']) if row.get('name') == 'Imported Point 0')
                def extra_carrier(m):
                    m['nodes'][12]['children'].append(len(m['nodes']));m['nodes'].append(copy.deepcopy(m['nodes'][carrier]))
                def reparent_carrier(m):
                    m['nodes'][12]['children'].remove(carrier);m['nodes'][1]['children'].append(carrier)
                mutations += [
                    ('noncanonical-carrier-m44-one-float-ulp', lambda m: m['nodes'][carrier]['matrix'].__setitem__(15, float(np.nextafter(np.float32(1.), np.float32(0.))))),
                    ('nonzero-carrier-homogeneous-x', lambda m: m['nodes'][carrier]['matrix'].__setitem__(3, float(np.nextafter(np.float32(0.), np.float32(1.))))),
                    ('missing-light-carrier', lambda m: m['nodes'][12]['children'].clear()),
                    ('duplicate-light-carrier', extra_carrier), ('reparented-light-carrier', reparent_carrier),
                    ('wrong-light-carrier-name', lambda m: m['nodes'][carrier].__setitem__('name', 'Unrelated light')),
                    ('wrong-light-carrier-position', lambda m: m['nodes'][carrier]['matrix'].__setitem__(12, .5)),
                    ('wrong-light-carrier-residual', lambda m: m['nodes'][carrier]['matrix'].__setitem__(0, 1.5)),
                    ('light-carrier-acquired-mesh', lambda m: m['nodes'][carrier].__setitem__('mesh', 0)),
                    ('wrong-carrier-light-definition', lambda m: m['nodes'][carrier]['extensions'][LIGHT].__setitem__('light', 0)),
                    ('source-owner-binding-not-transferred', lambda m: m['nodes'][12].__setitem__('extensions', {LIGHT: dict(light=0)})),
                    ('raw-unused-light-definition-changed', lambda m: m['extensions'][LIGHT]['lights'][0]['color'].__setitem__(1, .8)),
                    ('rendered-light-definition-changed', lambda m: m['extensions'][LIGHT]['lights'][1]['color'].__setitem__(1, .8)),
                    ('rendered-light-metadata-changed', lambda m: m['extensions'][LIGHT]['lights'][1]['extras'].__setitem__('lightIdentity', 0)),
                    ('rendered-light-range-changed', lambda m: m['extensions'][LIGHT]['lights'][1].__setitem__('range', 7.)),
                    ('extra-light-definition', lambda m: m['extensions'][LIGHT]['lights'].append(copy.deepcopy(m['extensions'][LIGHT]['lights'][1]))),
                    ('unused-original-light-became-active', lambda m: m['nodes'][2].__setitem__('extensions', {LIGHT: dict(light=0)})),
                    ('light-carrier-leaked-into-other-scene', lambda m: m['scenes'][1]['nodes'].append(carrier))]
            for name, mutate in mutations:
                bad = copy.deepcopy(canonical);mutate(bad);path = output/(run['name']+'-'+name+'.gltf');document(path, bad)
                reject(run['name']+'/'+name+'-gltf', lambda path=path, last=last: inspect_export(path, last))
                binary = path.with_suffix('.glb');projection.glb_oracle(path, binary)
                reject(run['name']+'/'+name+'-glb', lambda binary=binary, last=last: inspect_export(binary, last))
        for mode in ('static', 'animated', 'actor'):
            parent = node_matrix(parent_record(mode, .5 if mode == 'animated' else None));local = node_matrix(main_pose(mode, 1));world = parent@local
            close(view_from_world(world), np.linalg.inv(navigation_world(NAV_A)), 'independent solved eye/forward/up/roll')
            require(not np.allclose(local, navigation_world(NAV_A), atol=1e-4), 'Pose fixture failed to distinguish local from world')
            before = parent@node_matrix(original_main());require(not np.allclose(before[:3, :3].T@before[:3, :3], np.eye(3), atol=1e-3), 'Pose fixture lost scale/shear residual')
            close(world[:3, :3].T@world[:3, :3], before[:3, :3].T@before[:3, :3], 'scale/shear metric retained')
            checks.append(mode+'-independent-parent-inverse-roll-residual')
        actor_before = world_matrices(expected_nodes('actor', 0))[12][:3, 3];actor_after = world_matrices(expected_nodes('actor', 1))[12][:3, 3]
        require(np.linalg.norm(actor_after-actor_before) > .1, 'Derived light descendant fixture is vacuous');checks.append('nonvacuous-child-light-motion')
        image = np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8)
        for i, rgb in enumerate(([160, 20, 10], [20, 160, 30], [20, 30, 160])): image[100:150, 100+i*90:150+i*90] = rgb
        a, b = output/'pixels-a.png', output/'pixels-b.png';Image.fromarray(image).save(a);Image.fromarray(np.roll(image, 25, axis=1)).save(b)
        first, second = dict(image=str(a), resolution=[WIDTH, HEIGHT]), dict(image=str(b), resolution=[WIDTH, HEIGHT])
        projection.visible(first);projection.changed(first, second);projection.compare_images(first, first);checks.append('independent-full-rgb-positive-oracles')
        reject('wrong-pose-raster-framing', lambda: projection.compare_images(first, second))
        reject('vacuous-positive-pose-witness', lambda: projection.changed(first, first))
        require(str(Path(__file__).resolve()) in dependencies() and str(Path(projection.__file__).resolve()) in dependencies(), 'Pose manifest misses direct checker sources')
        require(set(projection.dependencies()) <= set(dependencies()), 'Pose manifest misses transitive projection helpers');checks.append('transitive-frozen-checker-dependencies')
    return dict(passed=True, checkCount=len(checks), checks=checks, rendererExecuted=False, nativeValidationPerformed=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path);parser.add_argument('--renderer', type=Path);parser.add_argument('--manifest', type=Path)
    parser.add_argument('--technique', choices=TECHNIQUES, action='append');parser.add_argument('--timeout', type=float, default=600.)
    parser.add_argument('--self-check', action='store_true');parser.add_argument('--generate-only', action='store_true');args = parser.parse_args()
    if args.self_check: print(json.dumps(self_check(), indent=2));return
    if args.output is None: parser.error('--output is required')
    if not math.isfinite(args.timeout) or args.timeout <= 0: parser.error('--timeout must be finite and positive')
    if not args.generate_only and (args.renderer is None or args.manifest is None): parser.error('Native pose run requires --renderer and --manifest')
    args.technique = tuple(dict.fromkeys(args.technique or TECHNIQUES))
    if args.renderer: args.renderer = args.renderer.resolve()
    if args.manifest: args.manifest = args.manifest.resolve()
    output = args.output.resolve();plan = generate(output)
    report = dict(schemaVersion=1, passed=None if args.generate_only else False, generatedOnly=args.generate_only,
        rendererExecuted=False, nativeValidationPerformed=False, sourceDependencies=dependencies(), launchPlanSha256=sha(output/'launch-plan.json'),
        expectedCaptureCount=plan['expectedCaptureCount']*len(args.technique), expectedProcessCount=plan['expectedProcessCount']*len(args.technique),
        expectedImageCheckCount=plan['expectedImageCheckCount']*len(args.technique), expectedExportCount=plan['expectedExportCount']*len(args.technique),
        processes=[], captures=[], stateChecks=[], imageChecks=[], exports=[], portableCopies=[], unverified=UNVERIFIED)
    if args.generate_only: document(output/'results.json', report);print('GENERATED', report['expectedCaptureCount'], 'pose captures; renderer not executed')
    else:
        run_native(args, output, plan, report)
        print('PASS', len(report['captures']), 'pose captures;', len(report['stateChecks']), 'CPU/source checks;', len(report['imageChecks']), 'image checks')


if __name__ == '__main__': main()
