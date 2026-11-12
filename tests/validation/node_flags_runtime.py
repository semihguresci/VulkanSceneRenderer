"""Exercise node flags, light history, and real viewport pick routes in Vulkan."""
import argparse
import math
import os
from pathlib import Path

from edited_scene_save_runtime import document
from light_editor_history_runtime import PUNCTUAL, action, edit, exact, fixture, history, lights, run, write_json


VISIBLE = 'KHR_node_visibility'
SELECTABLE = 'KHR_node_selectability'
HOVERABLE = 'KHR_node_hoverability'
ELIGIBLE, PANEL, HIDDEN, NEIGHBOR, CAMERA, HIDDEN_PANEL, BLOCKED = 2, 3, 4, 5, 6, 7, 8
PROBE = [320, 180]
# Project the center of the hidden panel at import scale 2 using the fixed camera.
HIDDEN_PROBE = [int(320 + 180 * (2 * 1.4) / ((6 - 2 * .2) * math.tan(math.pi / 8))), 180]
BLOCKED_PROBE = [int(320 - 180 * 1.4 / ((6 - .7) * math.tan(math.pi / 8))), 180]


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
        dict(name='Hidden group', children=[1, CAMERA, HIDDEN_PANEL], extensions={VISIBLE: dict(visible=False)}),
        dict(name='Neighbor light', translation=[1, 1, 3], extensions={PUNCTUAL: dict(light=1)}),
        dict(name='Hidden camera', camera=0, translation=[0, 0, 6]),
        dict(name='Hidden panel', mesh=0, scale=[.18, .18, 1], translation=[1.4, 0, .2]),
        dict(name='Blocked scene root', mesh=0, scale=[.35, .35, 1], translation=[0, 0, .7],
             extensions={SELECTABLE: dict(selectable=False), HOVERABLE: dict(hoverable=False)}),
    ]
    model['scenes'] = [dict(name='Flags', nodes=[0, ELIGIBLE, HIDDEN, NEIGHBOR]),
                       dict(name='Blocked root', nodes=[0, HIDDEN, NEIGHBOR, BLOCKED])]
    write_json(path, model)
    return model


def flags(frame, source, **values):
    return dict(frame=frame, nodeFlags=dict(sourceNode=source, **values))


def authored(sample):
    return {light['sourceNode']: light['intensity'] for light in lights(sample) if light['source'] == 1}


def probe(sample, expected, pixel=PROBE):
    depth = sample[1]['depthProbe']
    assert depth['pixel'] == pixel, (sample[0], depth['pixel'], pixel)
    values = depth['sceneInteraction']
    assert type(values['rawGpuId']) is int and values['rawGpuId'] > 0, (sample[0], values)
    for kind in ('cpu', 'gpu'):
        row = values[kind]
        actual = tuple(row[key]['name'] if row[key] else None for key in ('hit', 'selection', 'hover'))
        assert actual == expected, (sample[0], kind, actual, expected)
    assert values['cpu'] == values['gpu'], (sample[0], values)


def exact_view(first, second):
    assert first[1]['camera'] == second[1]['camera'], (first[0], second[0])
    assert first[1]['gui']['selectedMeshNodeId'] == second[1]['gui']['selectedMeshNodeId'], (first[0], second[0])
    exact(first, second)


def capture(renderer, output, name, source, technique, sample_frames, events=(), extra=()):
    states = run(renderer, output, name, source, technique, sample_frames, events, extra, fix_camera=True)
    log = output / (name + '.log')
    assert 'Insert instance layer "VK_LAYER_KHRONOS_validation"' in log.read_text(encoding='utf-8', errors='replace'), log
    assert all(state['validationEnabled'] for _, state in states.values()), log
    return states


def verify_history(renderer, output, source, technique, suffix):
    destination = output / ('portable.' + suffix)
    other = 'forward-raster' if technique == 'deferred-raster' else 'deferred-raster'
    # Switching away and back clears the startup selection. Sample on the
    # next frame, after the fixed camera track has reapplied the viewport pose.
    events = [
        dict(frame=1, gltfScene=1), dict(frame=2, gltfScene=0),
        dict(frame=3, depthProbe=PROBE), flags(4, HIDDEN, visible=True), edit(5, dict(intensity=40)),
        flags(6, HIDDEN, visible=False), action(7, 'undo'), action(8, 'undo'),
        action(9, 'redo'), action(10, 'redo'), flags(11, PANEL, selectable=True, hoverable=False),
        dict(frame=12, gltfScene=1), dict(frame=13, depthProbe=PROBE),
        dict(frame=14, gltfScene=0), dict(frame=15, depthProbe=PROBE),
        dict(frame=16, technique=other), dict(frame=17, depthProbe=PROBE),
    ]
    for event in events:
        if 4 <= event['frame'] <= 11:
            event['depthProbe'] = PROBE if event['frame'] == 11 else HIDDEN_PROBE
    states = capture(renderer, output, technique, source, technique, [*range(3, 12), 13, 15, 17], events,
                     ('--no-ray-query', '--import-scale', '2', '--save-scene', str(destination)))
    hidden, default_light, edited_light = {NEIGHBOR: 9}, {1: 18, NEIGHBOR: 9}, {1: 40, NEIGHBOR: 9}
    for frame in (3, 6, 10, 11, 13, 15, 17):
        assert authored(states[frame]) == hidden, (frame, authored(states[frame]))
    for frame in (4, 8):
        assert authored(states[frame]) == default_light, (frame, authored(states[frame]))
    for frame in (5, 7, 9):
        assert authored(states[frame]) == edited_light, (frame, authored(states[frame]))
    assert states[3][1]['scene']['primaryObjectCount'] == 3  # Includes the hidden authored primitive.
    for frame, undo, redo in [(4, 1, 0), (5, 2, 0), (6, 3, 0), (7, 2, 1), (8, 1, 2),
                              (9, 2, 1), (10, 3, 0), (11, 4, 0), (13, 0, 0), (15, 0, 0)]:
        history(states[frame], undo, redo)
    for first, second in [(3, 6), (3, 10), (4, 8), (5, 7), (5, 9)]:
        exact_view(states[first], states[second])
    probe(states[3], ('Front panel', 'Eligible group', 'Front panel'))
    for frame in (4, 5, 7, 8, 9):
        probe(states[frame], ('Hidden panel',) * 3, HIDDEN_PROBE)
    for frame in (6, 10):
        probe(states[frame], ('Accessor probe',) * 3, HIDDEN_PROBE)
    probe(states[13], ('Blocked scene root', None, None))
    assert states[13][1]['gltfScenes']['selected'] == 1
    assert states[15][1]['gltfScenes']['selected'] == 0
    assert states[17][1]['technique'] == other
    for frame in (11, 15, 17):
        probe(states[frame], ('Front panel', 'Front panel', 'Eligible group'))
    saved = document(destination)
    panel = next(node for node in saved['nodes'] if node.get('name') == 'Front panel')
    assert panel['extensions'][SELECTABLE] == dict(selectable=True, extras=dict(key='retained'), future=7)
    assert panel['extensions'][HOVERABLE]['hoverable'] is False
    group = next(index for index, node in enumerate(saved['nodes']) if node.get('name') == 'Hidden group')
    assert saved['nodes'][group]['extensions'][VISIBLE]['visible'] is False
    point_owner = next(node for node in saved['nodes'] if node.get('name') == 'Point owner')
    point_index = point_owner['extensions'][PUNCTUAL]['light']
    assert saved['extensions'][PUNCTUAL]['lights'][point_index]['intensity'] == 40
    reloaded = capture(renderer, output, technique + '-reload', destination, technique, [3, 4, 6],
                       [dict(frame=1, gltfScene=1), dict(frame=2, gltfScene=0), flags(4, group, visible=True),
                        dict(frame=5, gltfScene=1), dict(frame=6, depthProbe=PROBE)], ('--no-ray-query',))
    exact_view(states[15], reloaded[3])
    exact_view(states[9], reloaded[4])
    probe(reloaded[6], ('Primitive 0', None, None))
    print(f'PASS {technique} flags, {suffix.upper()}, light undo and scene changes', flush=True)


def prepare_pointer_source(path):
    model = prepare_source(path)
    model['nodes'][BLOCKED]['translation'] = [-1.4, 0, .7]
    model['scenes'][0]['nodes'].append(BLOCKED)

    # Keep counters offscreen, with a separate channel for every valid event
    # and one channel that records any event on an ineligible route.
    counter_material = len(model['materials'])
    model['materials'].append(dict(name='Viewport event counters',
                                   pbrMetallicRoughness=dict(baseColorFactor=[0, 0, 0, 0])))
    target = dict(pointer=dict(value=[f'/materials/{counter_material}/pbrMetallicRoughness/baseColorFactor']),
                  type=dict(value=[0]))
    nodes = [dict(declaration=3, configuration=target)]
    for event, source, channel in [(0, ELIGIBLE, 0), (1, PANEL, 1), (2, PANEL, 2),
                                   (0, PANEL, 3), (0, BLOCKED, 3), (1, BLOCKED, 3),
                                   (0, 0, 3), (1, 0, 3)]:
        handler = len(nodes)
        increment = [0, 0, 0, 0]
        increment[channel] = .125
        nodes.extend([
            dict(declaration=event, configuration=dict(nodeIndex=dict(value=[source])),
                 flows=dict(out=dict(node=handler + 2))),
            dict(declaration=4, values=dict(a=dict(node=0), b=dict(type=0, value=increment))),
            dict(declaration=5, configuration=target, values=dict(value=dict(node=handler + 1))),
        ])
    hover_flag = dict(pointer=dict(value=[f'/nodes/{PANEL}/extensions/{HOVERABLE}/hoverable']), type=dict(value=[1]))
    read_flag = len(nodes)
    nodes.extend([
        dict(declaration=3, configuration=hover_flag),
        dict(declaration=6, configuration=dict(event=dict(value=[0])), flows=dict(out=dict(node=read_flag + 2))),
        dict(declaration=7, values=dict(condition=dict(node=read_flag)), flows={'false': dict(node=read_flag + 3)}),
        dict(declaration=5, configuration=hover_flag, values=dict(value=dict(type=1, value=[True]))),
    ])
    model['extensionsUsed'].append('KHR_interactivity')
    model['extensionsRequired'].append('KHR_interactivity')
    model['extensions']['KHR_interactivity'] = dict(graph=0, graphs=[dict(
        types=[dict(signature='float4'), dict(signature='bool')],
        events=[dict(id='restorePanelHover')],
        declarations=[dict(op='event/onSelect', extension=SELECTABLE),
                      dict(op='event/onHoverIn', extension=HOVERABLE),
                      dict(op='event/onHoverOut', extension=HOVERABLE),
                      dict(op='pointer/get'), dict(op='math/add'), dict(op='pointer/set'),
                      dict(op='event/receive'), dict(op='flow/branch')],
        nodes=nodes)])
    write_json(path, model)
    return counter_material


def pointer(frame, action, pixel=PROBE, **event):
    return dict(frame=frame, viewportPointer=dict(action=action, pixel=pixel), **event)


def verify_viewport_pointer(renderer, output):
    source = output / 'viewport-pointer.gltf'
    counter = prepare_pointer_source(source)
    events = [
        dict(frame=1, interactivityPlaying=True),
        pointer(2, 'select', depthProbe=PROBE), pointer(3, 'hover', depthProbe=PROBE),
        pointer(4, 'hover'), pointer(5, 'hover'),
        pointer(6, 'hover', interactivityPlaying=False), pointer(7, 'hover'),
        pointer(8, 'hover', interactivityPlaying=True), pointer(9, 'hover'),
        pointer(10, 'hover', BLOCKED_PROBE, depthProbe=BLOCKED_PROBE),
        pointer(11, 'select', BLOCKED_PROBE, depthProbe=BLOCKED_PROBE), pointer(12, 'hover', BLOCKED_PROBE),
        dict(flags(14, PANEL, hoverable=False), depthProbe=PROBE),
        dict(action(15, 'undo'), depthProbe=PROBE), dict(action(16, 'redo'), depthProbe=PROBE),
        dict(frame=17, interactivityEvent='restorePanelHover', depthProbe=PROBE),
        pointer(18, 'hover'),
    ]
    counters = [
        (0, 0, 0, 0), (0, 0, 0, 0), (1, 0, 0, 0), (1, 1, 0, 0), (1, 1, 0, 0),
        (1, 1, 0, 0), (1, 1, 0, 0), (1, 1, 0, 0), (1, 2, 0, 0), (1, 2, 0, 0),
        (1, 2, 1, 0), (1, 2, 1, 0), (1, 2, 1, 0),
        (1, 2, 1, 0), (1, 2, 1, 0), (1, 2, 1, 0), (1, 2, 1, 0), (1, 2, 1, 0), (1, 3, 1, 0),
    ]
    for technique in ('deferred-raster', 'forward-raster'):
        states = capture(renderer, output, technique + '-viewport-pointer', source, technique,
                         list(range(1, len(counters) + 1)), events, ('--no-ray-query',))
        for frame, expected in enumerate(counters, 1):
            image, state = states[frame]
            graph = state['gltfInteractivity']
            assert graph['available'] and not graph['diagnostic'] and graph['playing'] == (frame not in (6, 7)), (image, graph)
            materials = state['gltfMaterialBindings']['materials']
            marker = next(entry for entry in materials if entry['sourceMaterial'] == counter)
            values = marker['evaluated']['pbrMetallicRoughness']['baseColorFactor']
            assert values == [value / 8 for value in expected], (image, values, expected)
        for frame in (2, 3):
            probe(states[frame], ('Front panel', 'Eligible group', 'Front panel'))
        for frame in (10, 11):
            probe(states[frame], ('Blocked scene root', None, None), BLOCKED_PROBE)
        for frame, enabled, undo, redo in [(14, False, 1, 0), (15, True, 0, 1), (16, False, 1, 0), (17, True, 1, 0)]:
            nodes = states[frame][1]['gltfNodeFlags']['nodes']
            panel = next(node for node in nodes if node['sourceIndex'] == PANEL)
            assert panel['localHoverable'] is enabled and panel['effectiveHoverable'] is enabled, (frame, panel)
            history(states[frame], undo, redo)
            probe(states[frame], ('Front panel', 'Eligible group', 'Front panel' if enabled else 'Eligible group'))
        print(f'PASS {technique} viewport routes, hover pause/resume and editor flag behavior', flush=True)


def verify_rays(renderer, output, source):
    events = [flags(3, HIDDEN, visible=True), flags(5, HIDDEN, visible=False)]
    raster = capture(renderer, output, 'hard-ray-and-probes', source, 'forward-raster', [2, 4, 6], events,
                 ('--ray-shadows', 'hard', '--no-ray-shadow-denoise', '--ddgi', '--ddgi-counts', '3', '3', '3',
                  '--ddgi-rays', '4', '--ddgi-probes-per-frame', '27', '--ddgi-ray-budget', '108',
                  '--ddgi-intensity', '0'))
    for frame, instances in ((2, 2), (4, 3), (6, 2)):
        state = raster[frame][1]
        assert state['rayShadows']['active'] and state['rayShadows']['build']['instances'] == instances
        assert state['ddgi']['active'] and state['ddgi']['updatedProbes'] > 0 and state['ddgi']['tracedRays'] > 0
    exact(raster[2], raster[6])
    traced = capture(renderer, output, 'reference-path', source, 'path-tracing', [2, 4, 6], events,
                     ('--path-spp', '16', '--path-samples', '32', '--path-bounces', '3', '--path-seed', '7'))
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
    os.environ['VK_LOADER_DEBUG'] = 'layer'
    renderer, output = args.renderer.resolve(strict=True), args.output.resolve()
    output.mkdir(parents=True)
    (output / 'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n')
    source = output / 'source.gltf'
    prepare_source(source)
    verify_history(renderer, output, source, 'deferred-raster', 'glb')
    verify_history(renderer, output, source, 'forward-raster', 'gltf')
    camera = capture(renderer, output, 'hidden-camera', source, 'deferred-raster', [1], (),
                     ('--no-ray-query', '--gltf-camera-node', str(CAMERA)))[1]
    assert camera[1]['gltfAnimation']['selectedCameraNode'] == CAMERA
    verify_viewport_pointer(renderer, output)
    if args.ray:
        verify_rays(renderer, output, source)
    write_json(output / 'results.json', dict(passed=True, rasterTechniques=2, gpuPickRoutes=True,
        hiddenCameraAvailable=True, portableFormats=2, viewportPointerBehavior=True, editorFlagBehavior=True, ray=args.ray))


if __name__ == '__main__':
    main()
