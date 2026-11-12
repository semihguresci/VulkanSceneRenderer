"""Exercise the published light editor commands through the native Vulkan viewport."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess

from PIL import Image, ImageChops

from gltf_accessor_runtime_regression import Asset


PUNCTUAL = 'KHR_lights_punctual'
POINTER = 'KHR_animation_pointer'
POINT = dict(type='point', source='imported', nodeName='Point owner')


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def fixture(path, *, intensity=18, position=(-1, 1, 3), animated=False):
    asset = Asset()
    model = asset.model
    points = [[-2, -1.5, 0], [2, -1.5, 0], [2, 1.5, 0], [-2, 1.5, 0]]
    vertices = dict(POSITION=asset.accessor('VEC3', points),
                    NORMAL=asset.accessor('VEC3', [[0, 0, 1]] * 4))
    indices = asset.accessor('SCALAR', [0, 1, 2, 0, 2, 3], 5123, indices=True)
    model['meshes'] = [dict(primitives=[dict(attributes=vertices, indices=indices, material=0, mode=4)])]
    model['materials'] = [dict(pbrMetallicRoughness=dict(baseColorFactor=[.6, .7, .9, 1],
                             metallicFactor=0, roughnessFactor=.55), doubleSided=True)]
    model['nodes'].append(dict(name=POINT['nodeName'], translation=list(position),
                               extensions={PUNCTUAL: dict(light=0)}))
    model['scenes'][0]['nodes'].append(1)
    model['extensionsUsed'] = [PUNCTUAL]
    model['extensions'] = {PUNCTUAL: dict(lights=[dict(type='point', color=[1, .5, .2],
                                                      intensity=intensity, range=8)])}
    if animated:
        model['extensionsUsed'].append(POINTER)
        times = asset.accessor('SCALAR', [0, 2])
        intensity_track = asset.accessor('SCALAR', [18, 26])
        roughness_track = asset.accessor('SCALAR', [.2, .8])
        tracks = (intensity_track, roughness_track)
        pointers = (f'/extensions/{PUNCTUAL}/lights/0/intensity',
                    '/materials/0/pbrMetallicRoughness/roughnessFactor')
        model['animations'] = [dict(
            samplers=[dict(input=times, output=track, interpolation='LINEAR') for track in tracks],
            channels=[dict(sampler=i, target=dict(path='pointer', extensions={POINTER: dict(pointer=ptr)}))
                      for i, ptr in enumerate(pointers)])]
    asset.write(path)


def edit(frame, values=None, *, phase='commit', gesture=None, offset=None, target=POINT):
    request = dict(target=target, phase=phase)
    if values is not None:
        request['values'] = values
    if gesture is not None:
        request['gesture'] = gesture
    if offset is not None:
        request['gizmo'] = dict(tool='translate', space='world', axis='x', amount=[offset, 0, 0])
    return dict(frame=frame, lightEdit=request)


def action(frame, value):
    return dict(frame=frame, sceneHistory=value)


def lights(sample):
    return sample[1]['lightEdits']['lights']


def point(sample):
    return next(light for light in lights(sample) if light['type'] == 1 and light['source'] == 1)


def history(sample, undo, redo):
    assert sample[1]['sceneEditHistory'] == dict(undo=undo, redo=redo), sample[1]['sceneEditHistory']


def difference(first, second):
    with Image.open(first[0]) as a, Image.open(second[0]) as b:
        assert a.size == b.size
        return ImageChops.difference(a.convert('RGB'), b.convert('RGB'))


def exact(first, second):
    assert difference(first, second).getbbox() is None, (first[0], second[0])


def visibly_changed(first, second):
    delta = difference(first, second)
    pixels = delta.tobytes()
    assert sum(max(pixels[i:i + 3]) > 5 for i in range(0, len(pixels), 3)) >= 250, (first[0], second[0])


def run(renderer, output, name, source, technique, sample_frames, events=(), extra=(), *, fix_camera=False, with_ui=False):
    prefix = output / name
    sequence = prefix.with_suffix('.json')
    settings = dict(schemaVersion=1, frames=sample_frames[-1], sampleFrames=sample_frames, events=events)
    if fix_camera:
        settings['camera'] = [dict(frame=1, position=[0, 0, 6], target=[0, 0, 0])]
    write_json(sequence, settings)
    image = prefix.with_suffix('.png')
    log = prefix.with_suffix('.log')
    window = ('--width', '1280', '--height', '720') if with_ui else (
        '--no-ui', '--screenshot-viewport', '--width', '640', '--height', '360')
    command = [str(renderer), '--model', str(source), '--hidden', '--validation', *window,
               '--render-technique', technique, '--display-mode', 'lit', '--no-bloom', '--no-taa',
               '--msaa', '1', '--camera-position', '0', '0', '6',
               '--camera-target', '0', '0', '0', '--camera-fov', '45', '--environment-intensity', '0',
               '--directional-intensity', '0', '--exposure', '.35', '--ray-shadows', 'raster',
               '--capture-sequence', str(sequence), '--screenshot', str(image),
               *extra]
    with log.open('x', encoding='utf-8') as stream:
        result = subprocess.run(command, cwd=output if with_ui else renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                                timeout=180, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    assert result.returncode == 0, log
    assert not re.search(r'VUID-|SYNC-HAZARD|Validation Error|hazard detected',
                         log.read_text(encoding='utf-8', errors='replace'), re.I), log
    images = [image.with_name(f'{image.stem}.frame-{frame:04d}.png') for frame in sample_frames[:-1]] + [image]
    if with_ui:
        assert all(json.loads(path.with_suffix('.png.capture.json').read_text())['includesUi'] for path in images)
    return {frame: (path, json.loads(path.with_suffix('.telemetry.json').read_text(encoding='utf-8')))
            for frame, path in zip(sample_frames, images)}


def raster_cases(renderer, output, technique, source, baked):
    events = [dict(frame=1, lightSelection=POINT),
              edit(3, dict(intensity=35), phase='begin', gesture=100),
              edit(4, dict(intensity=45), phase='update', gesture=100),
              edit(5, phase='end', gesture=100), action(6, 'undo'), action(7, 'redo'),
              edit(8, phase='begin', gesture=101, offset=.5),
              edit(9, phase='update', gesture=101, offset=1.25),
              edit(10, phase='end', gesture=101), action(11, 'undo'), action(12, 'redo'),
              edit(13, phase='begin', gesture=102, offset=-.75),
              edit(14, phase='cancel', gesture=102), action(15, 'undo'),
              edit(16, dict(range=6)), dict(frame=17, lightAdd=dict(type='point')),
              action(18, 'undo'), action(19, 'redo'),
              dict(frame=20, lightEdit=dict(target=POINT, phase='commit', values=dict(intensity=99),
                                          expectedLightRevision=9007199254740991),
                   expectLightEditFailure=True),
              action(21, 'undo'), dict(frame=22, reload=str(source))]
    captures = run(renderer, output, technique, source, technique, list(range(2, 23)), events,
                   ('--no-ray-query',), fix_camera=True)
    literal = run(renderer, output, technique + '-baked', baked, technique, [2],
                  [dict(frame=1, lightSelection=POINT)], ('--no-ray-query',), fix_camera=True)[2]
    assert point(captures[3])['intensity'] == 35 and point(captures[4])['intensity'] == 45
    history(captures[4], 0, 0)
    history(captures[5], 1, 0)
    history(captures[6], 0, 1)
    history(captures[9], 1, 0)
    history(captures[10], 2, 0)
    history(captures[14], 2, 0)
    history(captures[16], 2, 0)
    history(captures[17], 3, 0)
    history(captures[18], 2, 1)
    history(captures[19], 3, 0)
    history(captures[20], 3, 0)
    history(captures[21], 2, 1)
    history(captures[22], 0, 0)
    assert point(captures[8])['position'][0] == -.5
    assert point(captures[9])['position'][0] == .25
    assert point(captures[14])['position'][0] == .25
    assert not any(light['source'] == 2 for light in lights(captures[18]))
    assert any(light['source'] == 2 for light in lights(captures[19]))
    assert captures[20][1]['lightEdits']['error']
    assert not captures[22][1]['lightEdits']['error']
    assert point(captures[22])['intensity'] == 18 and point(captures[22])['range'] == 8
    assert not any(light['source'] == 2 for light in lights(captures[22]))
    for a, b in [(2, 6), (2, 22), (5, 7), (7, 11), (7, 15), (9, 10), (10, 12),
                 (12, 14), (16, 18), (16, 21), (17, 19), (19, 20)]:
        exact(captures[a], captures[b])
    exact(captures[10], literal)
    visibly_changed(captures[2], captures[5])
    visibly_changed(captures[5], captures[10])
    return len(captures) + 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--raster-only', action='store_true')
    args = parser.parse_args()
    renderer = args.renderer.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True)
    (output / 'vk_layer_settings.txt').write_bytes(b'khronos_validation.validate_sync = true\n')
    source, baked, animated = (output / name for name in ('source.gltf', 'baked.gltf', 'animated.gltf'))
    fixture(source)
    fixture(baked, intensity=45, position=(.25, 1, 3))
    fixture(animated, animated=True)
    counts = {}
    for technique in ('deferred-raster', 'forward-raster'):
        counts[technique] = raster_cases(renderer, output, technique, source, baked)
        print(f'PASS {technique} edit, coalesced gizmo, cancel, replay, stale rejection and reload', flush=True)
    states = run(renderer, output, 'animated-clock', animated, 'deferred-raster', [2, 3, 4, 5, 6],
                 [edit(3, dict(intensity=45)), dict(frame=4, animationTime=1.5), action(5, 'undo'), action(6, 'redo')],
                 ('--animation-clip', '0', '--animation-time', '.5', '--animation-paused', '--no-ray-query'))
    assert point(states[2])['intensity'] == 20
    assert point(states[3])['intensity'] == 45
    assert point(states[5])['intensity'] == 24 and point(states[6])['intensity'] == 45
    assert states[5][1]['gltfAnimation']['time'] == states[6][1]['gltfAnimation']['time'] == 1.5
    visibly_changed(states[4], states[5])
    exact(states[4], states[6])
    counts['animated-clock'] = len(states)
    gui = run(renderer, output, 'editor-controls', source, 'deferred-raster', [2, 4, 6],
              [edit(3, dict(intensity=45)), action(5, 'undo')], ('--no-ray-query',), with_ui=True)
    history(gui[2], 0, 0)
    history(gui[4], 1, 0)
    history(gui[6], 0, 1)
    counts['editor-controls'] = len(gui)
    if not args.raster_only:
        states = run(renderer, output, 'reference', source, 'path-tracing', [2, 4, 6, 8],
                     [edit(3, dict(intensity=45)), action(5, 'undo'), action(7, 'redo')],
                     ('--path-spp', '16', '--path-samples', '32', '--path-bounces', '3', '--path-seed', '7'))
        for image, state in states.values():
            path = state['referencePath']
            assert path['attemptedSamples'] == 32 and path['unsupportedSamples'] == path['rejectedSamples'] == 0, image
        exact(states[2], states[6])
        exact(states[4], states[8])
        visibly_changed(states[2], states[4])
        counts['path-tracing'] = len(states)
        states = run(renderer, output, 'temporal', source, 'deferred-raster', [2, 4, 6, 8],
                     [edit(3, dict(intensity=45)), action(5, 'undo'), action(7, 'redo')],
                     ('--taa', '--no-ray-query'))
        temporal = [state['taa'] for _, state in states.values()]
        assert all(row['enabled'] for row in temporal), temporal
        epochs = [row['epoch'] for row in temporal]
        assert all(a < b for a, b in zip(epochs, epochs[1:])), epochs
        assert all(row['resetReason'] == 'light edit or history' for row in temporal[1:]), temporal
        counts['taa'] = len(states)
        states = run(renderer, output, 'ddgi', source, 'deferred-raster', [2, 4, 6, 8],
                     [dict(frame=3, lightAdd=dict(type='point')), action(5, 'undo'), action(7, 'redo')],
                     ('--ddgi', '--ddgi-async', '--ddgi-counts', '3', '3', '3', '--ddgi-rays', '16'))
        gi = [state['ddgi'] for _, state in states.values()]
        assert all(row['supported'] and row['active'] for row in gi), gi
        resets = [row['historyResets'] for row in gi]
        assert all(a < b for a, b in zip(resets, resets[1:])), resets
        counts['ddgi'] = len(states)
    write_json(output / 'results.json', dict(passed=True, captures=counts, renderer=str(renderer)))
    print(f'PASS {sum(counts.values())} native captures', flush=True)


if __name__ == '__main__':
    main()
