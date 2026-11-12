"""Exercise procedural primitive history through the production Vulkan editor."""
import argparse
import json
from pathlib import Path

from gltf_deformation_runtime_regression import spec_for, write_fixture
from light_editor_history_runtime import (
    POINT, action, edit, exact, fixture, history, point, run, visibly_changed, write_json,
)
import numpy as np


INVALID = 0xFFFFFFFF
LIGHT = ('--environment-intensity', '1', '--directional-intensity', '2')
PATH = ('--path-spp', '32', '--path-samples', '32', '--path-bounces', '4')


def add(frame, kind):
    return dict(frame=frame, addPrimitive=dict(kind=kind, name=kind.capitalize()))


def scene(frame):
    return frame[1]['scene']


def topology(frame):
    return tuple(scene(frame)[key] for key in (
        'primaryNodeCount', 'primaryObjectCount', 'primaryVertexCount', 'primaryIndexCount',
    ))


def graph_nodes(frame):
    state = frame[1]['gltfMerge']
    assert not state['authoringNodesTruncated'], state['graphNodeCount']
    return {node['graphNode']: node for node in state['authoringNodes']}


def selected(frame):
    return frame[1]['gui']['selectedMeshNodeId']


def empty_scene(renderer, output, source, sidecar, technique):
    extras = PATH if technique == 'path-tracing' else ('--no-ray-query',)
    frames = run(renderer, output, technique + '-empty', source, technique, list(range(2, 14)), [
        add(3, 'cube'), add(4, 'torus'), action(5, 'undo'), action(6, 'undo'),
        action(7, 'redo'), action(8, 'redo'), action(9, 'undo'), action(10, 'undo'),
        add(11, 'sphere'), action(12, 'undo'), action(13, 'redo'),
    ], (*LIGHT, *extras, '--bim-model', str(sidecar)), fix_camera=True)
    base = topology(frames[2])
    baseline_nodes = graph_nodes(frames[2])
    assert base[0] == len(baseline_nodes) and base[1] == 0
    assert not any(node['active'] for node in baseline_nodes.values())
    assert sum(node['renderable'] for node in baseline_nodes.values()) == 1
    assert topology(frames[3])[2] > base[2] and topology(frames[3])[3] > base[3]
    first_slot = base[0]
    for frame in (2, 6, 10, 12):
        assert topology(frames[frame])[:2] == base[:2]
        assert graph_nodes(frames[frame]) == baseline_nodes
        assert frames[frame][1]['gltfMerge']['activeRoots'] == [] and selected(frames[frame]) == INVALID
        exact(frames[2], frames[frame])
    for frame in (5, 6, 7, 8, 9, 10):
        assert topology(frames[frame])[2:] == topology(frames[4])[2:]
    for frame in (12, 13):
        assert topology(frames[frame])[2:] == topology(frames[11])[2:]
    placements = {}
    for created, replayed in ((3, 7), (11, 13)):
        before, after = frames[created], frames[replayed]
        assert topology(before)[:2] == topology(after)[:2] == (base[0] + 1, 1)
        assert selected(before) == selected(after) == first_slot
        assert before[1]['gltfMerge']['activeRoots'] == after[1]['gltfMerge']['activeRoots'] == [first_slot]
        placements[created] = graph_nodes(before)[first_slot]
        assert placements[created] == graph_nodes(after)[first_slot] and placements[created]['active']
        exact(before, after)
        visibly_changed(frames[2], before)
    assert placements[3]['name'] == 'Cube' and placements[11]['name'] == 'Sphere'
    assert placements[3]['primitiveIndex'] != placements[11]['primitiveIndex']
    assert selected(frames[5]) == first_slot and graph_nodes(frames[5])[first_slot] == placements[3]
    assert topology(frames[4])[:2] == (base[0] + 2, 2)
    assert topology(frames[4]) == topology(frames[8])
    assert selected(frames[4]) == selected(frames[8]) == first_slot + 1
    assert graph_nodes(frames[4]) == graph_nodes(frames[8])
    exact(frames[3], frames[5])
    exact(frames[4], frames[8])
    history(frames[7], 1, 1)
    history(frames[8], 2, 0)
    history(frames[10], 0, 2)
    history(frames[11], 1, 0)
    if technique == 'path-tracing':
        for number, sample in frames.items():
            state = sample[1]
            path = state['referencePath']
            assert path['attemptedSamples'] == 32 and path['rejectedSamples'] == path['unsupportedSamples'] == 0, path
            assert state['rayShadows']['build']['instances'] == 1 + topology(sample)[1], number
    return len(frames)


def mixed_history(renderer, output):
    source = output / 'receiver.gltf'
    fixture(source)
    select_light = dict(frame=1, lightSelection=POINT)
    base = run(renderer, output, 'mesh-baseline', source, 'deferred-raster', [2],
               [select_light], ('--no-ray-query',), fix_camera=True)[2]
    slot = scene(base)['primaryNodeCount']
    transform = np.eye(4)
    transform[0, 3] = 1.15
    frames = run(renderer, output, 'mixed-history', source, 'deferred-raster', list(range(2, 12)), [
        select_light, edit(3, dict(intensity=45)), add(4, 'cube'),
        dict(frame=5, nodeTransform=dict(node=dict(graphNode=slot), matrix=transform.T.reshape(-1).tolist())),
        action(6, 'undo'), action(7, 'undo'), action(8, 'undo'),
        action(9, 'redo'), action(10, 'redo'), action(11, 'redo'),
    ], ('--no-ray-query',), fix_camera=True)
    exact(base, frames[2])
    for a, b in ((2, 8), (3, 7), (3, 9), (4, 6), (4, 10), (5, 11)):
        exact(frames[a], frames[b])
    assert selected(frames[7]) == selected(frames[8]) == selected(base) == INVALID
    assert point(frames[7])['selected'] and point(frames[8])['selected'] and point(base)['selected']
    assert selected(frames[4]) == selected(frames[10]) == selected(frames[11]) == slot
    assert graph_nodes(frames[4])[slot] == graph_nodes(frames[10])[slot]
    assert graph_nodes(frames[5])[slot] == graph_nodes(frames[11])[slot]
    assert topology(frames[8])[:2] == topology(base)[:2]
    assert topology(frames[8])[2:] == topology(frames[5])[2:]
    assert point(frames[5])['intensity'] == point(frames[11])['intensity'] == 45
    assert point(frames[2])['intensity'] == point(frames[8])['intensity'] == 18
    history(frames[8], 0, 3)
    history(frames[11], 3, 0)
    return len(frames) + 1


def deformation(renderer, output):
    source = output / 'combined.gltf'
    write_fixture(source, spec_for('combined'))
    frames = run(renderer, output, 'skin-morph-history', source, 'deferred-raster', [2, 3, 4, 5], [
        add(3, 'sphere'), action(4, 'undo'), action(5, 'redo'),
    ], (*LIGHT, '--animation-clip', '0', '--animation-time', '.25',
        '--animation-paused', '--no-ray-query'), fix_camera=True)
    exact(frames[2], frames[4])
    exact(frames[3], frames[5])
    visibly_changed(frames[2], frames[3])
    instance_count = frames[2][1]['gltfDeformation']['instanceCount']
    assert instance_count > 0
    for frame in frames.values():
        assert frame[1]['gltfAnimation']['time'] == .25
        state = frame[1]['gltfDeformation']
        assert state['gpuReady'] and state['currentBoundToGpu']
        assert state['instanceCount'] == instance_count
    return len(frames)


def reused_geometry(renderer, output):
    source = output / 'reuse-receiver.gltf'
    fixture(source)
    options = (*LIGHT, '--no-ray-query')
    control = run(renderer, output, 'reuse-baseline', source, 'deferred-raster',
                  [2], extra=options, fix_camera=True)[2]
    base = topology(control)
    births = list(range(3, 18, 2))
    undos = [frame + 1 for frame in births[:-1]]
    events = [dict(frame=frame, addPrimitive=dict(kind='torus', name=f'Torus {frame}'))
              for frame in births]
    events.extend(action(frame, 'undo') for frame in undos)
    events.append(dict(frame=18, addPrimitive=dict(kind='torus', name='Second torus')))
    move = np.eye(4)
    move[0, 3] = 1.4
    events.extend([
        dict(frame=19, nodeTransform=dict(node=dict(graphNode=base[0] + 1), matrix=move.T.reshape(-1).tolist())),
        action(20, 'undo'), action(21, 'undo'), action(22, 'redo'), action(23, 'redo'),
    ])
    frames = run(renderer, output, 'reused-procedural-geometry', source, 'deferred-raster',
                 list(range(2, 24)), sorted(events, key=lambda event: event['frame']),
                 options, fix_camera=True)
    assert topology(frames[2]) == base
    exact(control, frames[2])
    single = topology(frames[3])
    assert single[:2] == (base[0] + 1, base[1] + 1)
    assert single[2] > base[2] and single[3] > base[3], (base, single)
    visibly_changed(frames[2], frames[3])
    for frame in births:
        assert topology(frames[frame]) == single
        assert frames[frame][1]['gltfScenes']['editHistoryDepth'] == 1
        exact(frames[3], frames[frame])
    for frame in undos:
        assert topology(frames[frame]) == (*base[:2], *single[2:])
        exact(frames[2], frames[frame])

    # The simultaneous placements have one geometry allocation and distinct
    # picks. Moving one changes the image, with exact undo and redo.
    assert topology(frames[19]) == (base[0] + 2, base[1] + 2, *single[2:])
    toruses = [item for item in frames[19][1]['gltfNodeFlags']['probes']
               if item['sourceName'] in ('Torus 17', 'Second torus')]
    assert len(toruses) == 2 and toruses[0]['graphNode'] != toruses[1]['graphNode']
    assert all(item['projected'] and item['hit'] and
               item['resolvedSelection'] == item['nearestHitGraphNode'] == item['graphNode']
               for item in toruses), toruses
    assert topology(frames[21]) == single
    exact(frames[17], frames[21])
    exact(frames[18], frames[20])
    exact(frames[18], frames[22])
    exact(frames[19], frames[23])
    visibly_changed(frames[18], frames[19])
    return len(frames) + 1


def temporal_lighting(renderer, output):
    frames = run(renderer, output, 'temporal-lighting', output / 'receiver.gltf',
                 'deferred-raster', [2, 4, 6, 8], [
                     add(3, 'cube'), action(5, 'undo'), action(7, 'redo'),
                 ], ('--taa', '--ddgi', '--ddgi-async', '--ddgi-counts',
                     '3', '3', '3', '--ddgi-rays', '16'), fix_camera=True)
    gi = [state['ddgi'] for _, state in frames.values()]
    taa = [state['taa'] for _, state in frames.values()]
    assert all(row['active'] and row['supported'] for row in gi)
    assert all(row['enabled'] for row in taa)
    for rows, key in ((gi, 'historyResets'), (taa, 'epoch')):
        values = [row[key] for row in rows]
        assert all(a < b for a, b in zip(values, values[1:])), values
    return len(frames)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reuse-only', action='store_true')
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(strict=True), args.output.resolve()
    output.mkdir(parents=True)
    (output / 'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n')
    results = {'reuse': reused_geometry(renderer, output)}
    print('PASS shared primitive geometry across divergent branches and replay', flush=True)
    if args.reuse_only:
        (output / 'results.json').write_text(json.dumps(dict(passed=True, captures=results), indent=2))
        return
    # Keep one non-primary triangle in the ray scene while editing an empty
    # primary view. The triangle sits beyond the camera frustum.
    source, sidecar = output / 'empty-primary.gltf', output / 'sidecar.ifcx'
    fixture(source)
    model = json.loads(source.read_text(encoding='utf-8'))
    # Retain source geometry in an inactive view so the loader can admit an
    # asset whose selected scene starts with zero renderables.
    model['nodes'] = [dict(name='Inactive source mesh', mesh=0)]
    model['scenes'] = [dict(name='Empty active view', nodes=[]), dict(nodes=[0])]
    model.pop('extensions')
    model.pop('extensionsUsed')
    write_json(source, model)
    write_json(sidecar, {'data': [{'path': 'wall', 'attributes': {
        'bsi::ifc::class': {'code': 'IfcWall'},
        'usd::usdgeom::mesh': {'points': [[20, 0, 0], [21, 0, 0], [20, 0, 1]],
                               'faceVertexIndices': [0, 1, 2]},
    }}]})
    for technique in ('deferred-raster', 'forward-raster', 'path-tracing'):
        results[technique] = empty_scene(renderer, output, source, sidecar, technique)
        print(f'PASS {technique} empty scene, selection, and divergent history', flush=True)
    results['mixed'] = mixed_history(renderer, output)
    print('PASS shared mesh, transform, and light history', flush=True)
    results['deformation'] = deformation(renderer, output)
    print('PASS GPU skinning and morphing across add, undo and redo', flush=True)
    results['temporal'] = temporal_lighting(renderer, output)
    print('PASS TAA and asynchronous probe-lighting history resets', flush=True)
    (output / 'results.json').write_text(json.dumps(dict(passed=True, captures=results), indent=2))


if __name__ == '__main__':
    main()
