"""Exercise node flags, light history, and real viewport pick routes in Vulkan."""
import argparse
import math
from pathlib import Path

from edited_scene_save_runtime import document
from light_editor_history_runtime import PUNCTUAL, action, edit, exact, fixture, history, lights, run, write_json


VISIBLE = 'KHR_node_visibility'
SELECTABLE = 'KHR_node_selectability'
HOVERABLE = 'KHR_node_hoverability'
PANEL, HIDDEN, NEIGHBOR, CAMERA, BLOCKED = 3, 4, 5, 6, 8
PROBE = [320, 180]


def prepare_source(path):
    fixture(path)
    model = document(path)
    model['extensionsUsed'] += [VISIBLE, SELECTABLE, HOVERABLE]
    model['extensionsRequired'] = [VISIBLE]
    model['extensions'][PUNCTUAL]['lights'].append(dict(type='point', range=6, intensity=9))
    model['cameras'] = [dict(type='perspective', perspective=dict(yfov=math.pi / 4, znear=.1))]
    model['nodes'] += [
        dict(name='Eligible group', children=[PANEL]),
        dict(name='Front panel', mesh=0, scale=[.35, .35, 1], translation=[0, 0, .7],
             extensions={SELECTABLE: dict(selectable=False, extras=dict(key='retained'), future=7),
                         HOVERABLE: dict(hoverable=True)}),
        dict(name='Hidden group', children=[1, CAMERA, 7], extensions={VISIBLE: dict(visible=False)}),
        dict(name='Neighbor light', translation=[1, 1, 3], extensions={PUNCTUAL: dict(light=1)}),
        dict(name='Hidden camera', camera=0, translation=[0, 0, 6]),
        dict(name='Hidden panel', mesh=0, scale=[.18, .18, 1], translation=[1.4, 0, .2]),
        dict(name='Blocked scene root', mesh=0, scale=[.35, .35, 1], translation=[0, 0, .7],
             extensions={SELECTABLE: dict(selectable=False), HOVERABLE: dict(hoverable=False)}),
    ]
    model['scenes'] = [dict(name='Flags', nodes=[0, 2, HIDDEN, NEIGHBOR]),
                       dict(name='Blocked root', nodes=[0, HIDDEN, NEIGHBOR, BLOCKED])]
    write_json(path, model)
    return model


def flags(frame, source, **values):
    return dict(frame=frame, nodeFlags=dict(sourceNode=source, **values))


def authored(sample):
    return {light['sourceNode']: light['intensity'] for light in lights(sample) if light['source'] == 1}


def probe(sample, expected):
    values = sample[1]['depthProbe']['sceneInteraction']
    for kind in ('cpu', 'gpu'):
        row = values[kind]
        actual = tuple(row[key]['name'] if row[key] else None for key in ('hit', 'selection', 'hover'))
        assert actual == expected, (sample[0], kind, actual, expected)


def verify_history(renderer, output, source, technique, suffix):
    destination = output / ('portable.' + suffix)
    other = 'forward-raster' if technique == 'deferred-raster' else 'deferred-raster'
    events = [
        dict(frame=1), flags(2, HIDDEN, visible=True), edit(3, dict(intensity=40)),
        flags(4, HIDDEN, visible=False), action(5, 'undo'), action(6, 'undo'),
        action(7, 'redo'), action(8, 'redo'),
        flags(9, PANEL, selectable=True, hoverable=False),
        dict(frame=10, gltfScene=1), dict(frame=11, gltfScene=0), dict(frame=12, technique=other),
    ]
    for event in events:
        event['depthProbe'] = PROBE
    states = run(renderer, output, technique, source, technique, list(range(1, 13)), events,
                 ('--no-ray-query', '--import-scale', '2', '--save-scene', str(destination)), fix_camera=True)
    assert authored(states[1]) == authored(states[4]) == authored(states[8]) == {NEIGHBOR: 9}
    assert authored(states[2]) == authored(states[6]) == {1: 18, NEIGHBOR: 9}
    assert authored(states[3]) == authored(states[5]) == authored(states[7]) == {1: 40, NEIGHBOR: 9}
    assert states[1][1]['scene']['primaryObjectCount'] == 3  # Includes the hidden authored primitive.
    history(states[9], 4, 0)
    history(states[11], 0, 0)
    exact(states[1], states[4])
    exact(states[1], states[8])
    exact(states[3], states[5])
    exact(states[3], states[7])
    probe(states[1], ('Front panel', 'Eligible group', 'Front panel'))
    probe(states[9], ('Front panel', 'Front panel', 'Eligible group'))
    probe(states[10], ('Blocked scene root', None, None))
    probe(states[11], ('Front panel', 'Front panel', 'Eligible group'))
    assert states[12][1]['technique'] == other and authored(states[12]) == {NEIGHBOR: 9}
    probe(states[12], ('Front panel', 'Front panel', 'Eligible group'))
    saved = document(destination)
    panel = next(node for node in saved['nodes'] if node.get('name') == 'Front panel')
    assert panel['extensions'][SELECTABLE] == dict(selectable=True, extras=dict(key='retained'), future=7)
    assert panel['extensions'][HOVERABLE]['hoverable'] is False
    group = next(index for index, node in enumerate(saved['nodes']) if node.get('name') == 'Hidden group')
    reload = run(renderer, output, technique + '-reload', destination, technique, [1, 2, 3],
                 [dict(frame=1, depthProbe=PROBE), flags(2, group, visible=True),
                  dict(frame=3, gltfScene=1, depthProbe=PROBE)], ('--no-ray-query',), fix_camera=True)
    exact(states[11], reload[1])
    exact(states[7], reload[2])
    probe(reload[3], ('Primitive 0', None, None))
    print(f'PASS {technique} flags, {suffix.upper()}, light undo and scene changes', flush=True)


def verify_rays(renderer, output, source):
    events = [flags(3, HIDDEN, visible=True), flags(5, HIDDEN, visible=False)]
    raster = run(renderer, output, 'hard-ray-and-probes', source, 'forward-raster', [2, 4, 6], events,
                 ('--ray-shadows', 'hard', '--no-ray-shadow-denoise', '--ddgi', '--ddgi-counts', '3', '3', '3',
                  '--ddgi-rays', '4', '--ddgi-probes-per-frame', '27', '--ddgi-ray-budget', '108',
                  '--ddgi-intensity', '0'), fix_camera=True)
    for frame, instances in ((2, 2), (4, 3), (6, 2)):
        state = raster[frame][1]
        assert state['rayShadows']['active'] and state['rayShadows']['build']['instances'] == instances
        assert state['ddgi']['active'] and state['ddgi']['updatedProbes'] > 0 and state['ddgi']['tracedRays'] > 0
    exact(raster[2], raster[6])
    traced = run(renderer, output, 'reference-path', source, 'path-tracing', [2, 4, 6], events,
                 ('--path-spp', '16', '--path-samples', '32', '--path-bounces', '3', '--path-seed', '7'), fix_camera=True)
    for _, state in traced.values():
        counters = state['referencePath']
        assert counters['attemptedSamples'] == 32 and counters['unsupportedSamples'] == counters['rejectedSamples'] == 0
    exact(traced[2], traced[6])
    print('PASS hide/show refreshes ray shadows, probes, and path tracing', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--ray', action='store_true')
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(strict=True), args.output.resolve()
    output.mkdir(parents=True)
    (output / 'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n')
    source = output / 'source.gltf'
    prepare_source(source)
    verify_history(renderer, output, source, 'deferred-raster', 'glb')
    verify_history(renderer, output, source, 'forward-raster', 'gltf')
    camera = run(renderer, output, 'hidden-camera', source, 'deferred-raster', [1], (),
                 ('--no-ray-query', '--gltf-camera-node', str(CAMERA)), fix_camera=True)[1]
    assert camera[1]['gltfAnimation']['selectedCameraNode'] == CAMERA
    if args.ray:
        verify_rays(renderer, output, source)
    write_json(output / 'results.json', dict(passed=True, rasterTechniques=2, gpuPickRoutes=True,
        hiddenCameraAvailable=True, portableFormats=2, ray=args.ray))


if __name__ == '__main__':
    main()
