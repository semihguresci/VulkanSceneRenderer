"""Validate the shared scene-node and light journal against actual Vulkan frames."""

import argparse
from pathlib import Path

import numpy as np

from gltf_accessor_runtime_regression import Asset
from light_editor_history_runtime import action, edit, exact, history, point, run, visibly_changed, write_json


A = np.array([[0, -.8, 0, -.4], [1.25, 0, 0, .3], [0, 0, 1, 0], [0, 0, 0, 1]], dtype=np.float32)
B = np.array([[0, 1.2, 0, .5], [-.85, 0, 0, -.35], [0, 0, 1, .02], [0, 0, 0, 1]], dtype=np.float32)
REST = np.array([[-.85, .17, 0, -.12], [0, 1, 0, .18], [0, 0, 1, .24], [0, 0, 0, 1]], dtype=np.float32)
EDITED = REST.copy()
EDITED[0, 3] = .26
EDITED[1, 3] = -.52
LIGHT = dict(type='point', source='imported', nodeName='Point owner')
INVALID_NODE = 2**32 - 1


def columns(matrix):
    return np.asarray(matrix).T.reshape(16).tolist()


def matrix(value):
    return np.asarray(value, dtype=np.float64).reshape((4, 4), order='F')


def node(frame, operation, source=2, **values):
    target = dict(sourceNode=source)
    if operation == 'transform':
        return dict(frame=frame, nodeTransform=dict(node=target, matrix=values['matrix']))
    if operation == 'visibility':
        return dict(frame=frame, nodeFlags=dict(sourceNode=source, visible=values['visible']))
    if operation == 'reparent':
        return dict(frame=frame, nodeReparent=dict(node=target, parent=dict(sourceNode=values['parent'])))
    assert operation in ('select', 'begin', 'update', 'end', 'cancel'), operation
    return dict(frame=frame, nodeGizmo=dict(node=target, operation=operation, **values))


def build_fixture(path, local=REST, *, two_lights=False):
    asset = Asset()
    model = asset.model
    model['nodes'] = [
        dict(name='Parent A', matrix=columns(A), children=[3, 2, 4]),
        dict(name='Parent B', matrix=columns(B)),
        dict(name='Edited mirror', mesh=0, matrix=columns(local)),
        dict(name='Sibling before', mesh=0, translation=[.88, 0, 0], scale=[.52, .52, .52]),
        dict(name='Point owner', translation=[.1, .3, 3], extensions={'KHR_lights_punctual': dict(light=0)}),
        dict(name='Receiver', mesh=1, translation=[0, 0, -.2]),
    ]
    model['scenes'][0]['nodes'] = [0, 1, 5]
    model['extensionsUsed'] = ['KHR_lights_punctual']
    model['extensions'] = {'KHR_lights_punctual': dict(lights=[
        dict(type='point', color=[1, .8, .55], intensity=24, range=8)])}
    if two_lights:
        model['nodes'][1]['children'] = [6]
        model['nodes'].append(dict(name='Second point', translation=[.4, .2, 2],
                                   extensions={'KHR_lights_punctual': dict(light=1)}))
        model['extensions']['KHR_lights_punctual']['lights'].append(
            dict(type='point', color=[.2, .4, 1], intensity=12, range=7))
    model['materials'] = [dict(pbrMetallicRoughness=dict(baseColorFactor=color,
        metallicFactor=0, roughnessFactor=.57), doubleSided=True)
        for color in ([.72, .14, .24, 1], [.35, .43, .6, 1])]
    model['meshes'] = []
    for material, (w, h) in enumerate(((.48, .36), (2.1, 1.45))):
        attrs = dict(POSITION=asset.accessor('VEC3', [[-w,-h,0], [w,-h,0], [w,h,0], [-w,h,0]]),
                     NORMAL=asset.accessor('VEC3', [[0,0,1]] * 4))
        indices = asset.accessor('SCALAR', [0,1,2,0,2,3], 5123, indices=True)
        model['meshes'].append(dict(primitives=[dict(attributes=attrs, indices=indices, material=material)]))
    asset.write(path)


def state(sample):
    telemetry = sample[1]
    row = next(item for item in telemetry['gltfMerge']['authoringNodes'] if item['sourceNode'] == 2)
    graph_node = row['graphNode']
    selected = telemetry['gui']['selectedMeshNodeId']
    flags = next(item for item in telemetry['gltfNodeFlags']['nodes'] if item['graphNode'] == graph_node)
    gizmo = telemetry.get('nodeGizmo', {})
    return dict(graphNode=graph_node, local=np.asarray(row['localColumns']).flatten().tolist(),
                world=np.asarray(row['worldColumns']).flatten().tolist(), parent=row['parentGraphNode'],
                sibling=gizmo.get('siblingIndex') if gizmo.get('graphNode') == graph_node else None,
                effectiveVisible=flags['effectiveVisible'], selected=selected == graph_node,
                selectedGraphNode=selected, gizmoActive=gizmo.get('active', False))


def near(a, b):
    np.testing.assert_allclose(a, b, rtol=3e-6, atol=3e-6)


def grab(frame, *, tool='translate', space='world', axis='x', delta=(0, 0)):
    return node(frame, 'begin', gizmo=dict(tool=tool, space=space, axis=axis), delta=list(delta))


def raster(renderer, output, technique, source, literal):
    commands = [
        node(1, 'select'), node(3, 'transform', matrix=columns(EDITED)),
        action(4, 'undo'), action(5, 'redo'), node(6, 'visibility', visible=False),
        action(7, 'undo'), action(8, 'redo'), action(9, 'undo'), node(10, 'reparent', parent=1),
        action(11, 'undo'), action(12, 'redo'),
        grab(13, delta=(30, 0)), node(14, 'update', delta=[30,0]), node(15, 'end'),
        action(16, 'undo'), action(17, 'redo'),
        grab(18, axis='y', delta=(0,25)), node(19, 'cancel'),
        action(20, 'undo'), grab(21, tool='rotate', space='local', axis='y', delta=(100,0)),
        node(22, 'end'), action(23, 'undo'), action(24, 'redo'),
        grab(25, tool='scale', space='local', delta=(40,0)), node(26, 'end'),
        action(27, 'undo'), action(28, 'redo'),
        node(29, 'select', 3), action(30, 'undo'), action(31, 'redo'),
        action(32, 'undo'), grab(33, delta=(30, 0)), node(34, 'update', delta=[-30,0]),
        node(35, 'end'), action(36, 'redo'),
    ]
    captures = run(renderer, output, technique + '-node', source, technique, list(range(2,37)),
                   commands, ('--no-ray-query',), fix_camera=True)
    baked = run(renderer, output, technique + '-node-baked', literal, technique, [2],
                [node(1, 'select')], ('--no-ray-query',), fix_camera=True)[2]
    near(matrix(state(captures[2])['world']), A @ REST)
    near(matrix(state(captures[3])['world']), A @ EDITED)
    assert state(captures[2])['selected']
    assert not state(captures[6])['effectiveVisible']
    assert state(captures[7])['effectiveVisible']
    assert state(captures[2])['sibling'] == state(captures[11])['sibling'] == 1
    assert state(captures[9])['parent'] == state(captures[11])['parent']
    assert state(captures[10])['parent'] == state(captures[12])['parent']
    assert state(captures[10])['parent'] != state(captures[9])['parent']
    near(matrix(state(captures[10])['world']), A @ EDITED)
    near(matrix(state(captures[10])['local']), np.linalg.inv(B) @ A @ EDITED)
    assert state(captures[11])['local'] == state(captures[9])['local']
    assert state(captures[10])['local'] == state(captures[12])['local']

    p = matrix(state(captures[12])['world'])
    delta1 = matrix(state(captures[13])['world'])[:3,3] - p[:3,3]
    delta2 = matrix(state(captures[14])['world'])[:3,3] - p[:3,3]
    assert .01 < delta1[0] < delta2[0], (delta1, delta2)
    near(delta2[1:], [0, 0])
    assert state(captures[14])['local'][:12] == state(captures[12])['local'][:12]
    history(captures[13], 2, 0)
    history(captures[14], 2, 0)
    history(captures[15], 3, 0)
    history(captures[16], 2, 1)
    history(captures[17], 3, 0)
    assert state(captures[19])['local'] == state(captures[17])['local']
    history(captures[19], 3, 0)
    history(captures[22], 3, 0)
    history(captures[26], 4, 0)
    for frame in (13, 14, 18, 21, 25, 33, 34):
        assert state(captures[frame])['gizmoActive'], frame
    for frame in (15, 19, 22, 26, 35):
        assert not state(captures[frame])['gizmoActive'], frame
    assert np.linalg.det(matrix(state(captures[26])['local'])[:3,:3]) < 0
    selected = state(captures[26])['graphNode']
    assert state(captures[29])['selectedGraphNode'] != selected
    assert state(captures[30])['selectedGraphNode'] == state(captures[31])['selectedGraphNode'] == selected
    assert state(captures[33])['local'] != state(captures[32])['local']
    assert state(captures[34])['local'] == state(captures[35])['local'] == state(captures[32])['local']
    history(captures[32], 3, 1)
    history(captures[33], 3, 1)
    history(captures[34], 3, 1)
    history(captures[35], 3, 1)
    history(captures[36], 4, 0)
    for a, b in [(2,4), (3,5), (5,7), (6,8), (9,11), (10,12), (14,15), (12,16),
                 (15,17), (17,19), (12,20), (20,23), (22,24), (24,27), (26,28), (24,30), (26,31),
                 (32,34), (32,35), (26,36)]:
        exact(captures[a], captures[b])
    exact(captures[3], baked)
    for a, b in [(2,3), (3,6), (12,15), (17,18), (20,22), (24,26)]:
        visibly_changed(captures[a], captures[b])
    return len(captures) + 1


def mixed_history(renderer, output, source):
    moved = np.identity(4)
    moved[:3,3] = [-.3, .1, 3]
    commands = [
        node(1,'select',4), node(3,'transform',4,matrix=columns(moved)),
        action(4,'undo'), action(5,'redo'), edit(6,dict(intensity=45),target=LIGHT),
        node(7,'transform',2,matrix=columns(EDITED)),
        action(8,'undo'), action(9,'undo'), action(10,'undo'),
        action(11,'redo'), action(12,'redo'), action(13,'redo'),
        action(14,'undo'), dict(frame=15,reload=str(source)), node(16, 'select', 4),
    ]
    frames = run(renderer, output, 'mixed-history', source, 'deferred-raster', list(range(2,17)),
                 commands, ('--no-ray-query',), fix_camera=True)
    near(point(frames[3])['position'], (A @ moved)[:3,3])
    near(point(frames[4])['position'], point(frames[2])['position'])
    assert point(frames[6])['intensity'] == point(frames[7])['intensity'] == 45
    assert point(frames[9])['intensity'] == 24
    history(frames[10], 0, 3)
    history(frames[13], 3, 0)
    history(frames[14], 2, 1)
    assert frames[15][1].get('sceneEditHistory', dict(undo=0, redo=0)) == dict(undo=0, redo=0)
    assert 'nodeGizmo' not in frames[15][1]
    assert frames[15][1]['gltfMerge']['graphIdentity'] != frames[2][1]['gltfMerge']['graphIdentity']
    for a,b in [(2,4),(3,5),(6,8),(5,9),(2,10),(5,11),(6,12),(7,13),(6,14),(2,16)]:
        exact(frames[a],frames[b])
    visibly_changed(frames[2],frames[3])
    visibly_changed(frames[6],frames[7])
    return len(frames)


def stable_light_bindings(renderer, output):
    source = output/'two-lights.gltf'
    build_fixture(source, two_lights=True)
    commands = [node(1, 'select', 4), edit(3, dict(intensity=45), target=LIGHT),
                node(4, 'reparent', 4, parent=1), action(5, 'undo'), action(6, 'undo'),
                action(7, 'redo'), action(8, 'redo')]
    frames = run(renderer, output, 'light-binding-order', source, 'deferred-raster',
                 list(range(2,9)), commands, ('--no-ray-query',))
    def rows(frame):
        return {item['sourceNode']: item for item in frames[frame][1]['lightEdits']['lights']
                if item['source'] == 1 and item['type'] == 1}
    for frame in frames:
        first, second = (rows(frame)[source] for source in (4, 6))
        assert (first['index'], second['index']) == ((1, 0) if frame in (4, 8) else (0, 1))
        assert second['intensity'] == 12
        near(first['position'], rows(2)[4]['position'])
    assert rows(2)[4]['intensity'] == rows(6)[4]['intensity'] == 24
    assert rows(3)[4]['intensity'] == rows(4)[4]['intensity'] == rows(8)[4]['intensity'] == 45
    history(frames[8], 2, 0)
    for a,b in [(2,6),(3,5),(3,7),(4,8)]:
        exact(frames[a],frames[b])
    return len(frames)


def viewport_selection_history(renderer, output, source):
    # The fixed camera projects the center of A @ REST inside this pixel's quad.
    pixel = [279, 169]
    commands = [dict(frame=2, viewportPointer=dict(action='select', pixel=pixel)),
                dict(frame=3, nodeFlags=dict(sourceNode=0, selectable=False)),
                action(4, 'undo'), action(5, 'redo'), action(6, 'undo')]
    for command in commands:
        command['depthProbe'] = pixel
    captures = run(renderer, output, 'viewport-selection-history', source, 'deferred-raster',
                   list(range(2, 7)), commands, ('--no-ray-query',), fix_camera=True)
    initial = captures[2][1]
    picked = initial['depthProbe']['sceneInteraction']['gpu']['hit']
    assert picked['sourceNode'] == 2 and picked['sourceMesh'] == 0, picked
    selected = picked['graphNode']
    for frame, capture in captures.items():
        telemetry = capture[1]
        eligible = frame in (2, 4, 6)
        expected = selected if eligible else INVALID_NODE
        assert telemetry['gui']['selectedMeshNodeId'] == expected, (frame, telemetry['gui'])
        interaction = telemetry['depthProbe']['sceneInteraction']
        assert interaction['rawGpuId'] > 0, (frame, interaction)
        for route in (interaction['cpu'], interaction['gpu']):
            assert route['hit']['graphNode'] == selected, (frame, route)
            assert (route['selection']['graphNode'] if route['selection'] else INVALID_NODE) == expected, (frame, route)
        if frame == 2:
            assert telemetry.get('sceneEditHistory', dict(undo=0, redo=0)) == dict(undo=0, redo=0)
        else:
            undo, redo = (0, 1) if eligible else (1, 0)
            history(capture, undo, redo)
    exact(captures[3], captures[5])
    exact(captures[4], captures[6])
    return len(captures)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--raster-only', action='store_true')
    args = parser.parse_args()
    renderer = args.renderer.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True)
    (output/'vk_layer_settings.txt').write_bytes(b'khronos_validation.validate_sync = true\n')
    source, literal = output/'source.gltf', output/'literal.gltf'
    build_fixture(source)
    build_fixture(literal, EDITED)
    counts = {}
    for technique in ('deferred-raster','forward-raster'):
        counts[technique] = raster(renderer, output, technique, source, literal)
        print(f'PASS {technique} node transform, visibility, hierarchy and viewport drag replay', flush=True)
    counts['mixed-history'] = mixed_history(renderer, output, source)
    counts['light-binding-order'] = stable_light_bindings(renderer, output)
    counts['viewport-selection-history'] = viewport_selection_history(renderer, output, source)
    if not args.raster_only:
        samples = run(renderer, output, 'node-path', source, 'path-tracing', [2,4,6,8],
                      [node(3,'visibility',visible=False),action(5,'undo'),action(7,'redo')],
                      ('--path-spp','16','--path-samples','32','--path-bounces','3','--path-seed','3'))
        for path, sample in samples.values():
            result = sample['referencePath']
            assert result['attemptedSamples'] == 32 and result['unsupportedSamples'] == result['rejectedSamples'] == 0, path
        exact(samples[2],samples[6])
        exact(samples[4],samples[8])
        visibly_changed(samples[2],samples[4])
        counts['path-tracing'] = len(samples)
        samples = run(renderer, output, 'node-ddgi', source, 'deferred-raster', [2,4,6,8],
                      [node(3,'visibility',visible=False),action(5,'undo'),action(7,'redo')],
                      ('--taa','--ray-shadows','hard','--ddgi','--ddgi-async','--ddgi-counts','3','3','3','--ddgi-rays','16'))
        gi = [sample['ddgi'] for _,sample in samples.values()]
        taa = [sample['taa'] for _,sample in samples.values()]
        ray = [sample['rayShadows'] for _,sample in samples.values()]
        assert all(row['active'] for row in gi)
        assert all(row['active'] for row in ray)
        assert [row['build']['instances'] for row in ray] == [3,2,3,2]
        assert all(a['historyResets'] < b['historyResets'] for a,b in zip(gi,gi[1:]))
        assert all(a['epoch'] < b['epoch'] for a,b in zip(taa,taa[1:]))
        counts['taa-ddgi'] = len(samples)
    write_json(output/'results.json', dict(passed=True,captures=counts,renderer=str(renderer)))
    print(f'PASS {sum(counts.values())} native scene node captures', flush=True)


if __name__ == '__main__':
    main()
