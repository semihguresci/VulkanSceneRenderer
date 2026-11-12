"""Verify graph-triggered glTF morph playback against independently baked meshes."""

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image

import gltf_deformation_runtime_regression as deformation
from gltf_accessor_runtime_regression import TECHNIQUES, require
from gltf_material_variants_runtime_regression import run


TIMESTEP = .125
HELD_TIME = 3 * TIMESTEP
# The graph starts the clip at frame 2. Stop is dispatched before sampling
# frame 6, so it retains the pose submitted at frame 5.
SAMPLES = {
    1: (0., 0), 2: (0., 1), 5: (HELD_TIME, 1), 7: (HELD_TIME, 0),
    9: (HELD_TIME, 0), 10: (0., 0), 12: (0., 0),
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inputs(output):
    spec = deformation.spec_for('morph')
    graph_path, sequence = output / 'graph-morph.gltf', output / 'graph-morph.sequence.json'
    baked = {0.: output / 'baked-rest.gltf', HELD_TIME: output / 'baked-held.gltf'}
    expected = {time: deformation.evaluate(spec, time) for time in baked}
    deformation.write_fixture(graph_path, spec)
    for time, path in baked.items():
        deformation.write_fixture(path, spec, expected[time])

    clip = {'animation': {'type': 0, 'value': ['/animations/0']}}
    graph = {
        'types': [{'signature': 'ref'}],
        'events': [{'id': 'startMorph'}, {'id': 'stopMorph'}],
        'declarations': [{'op': operation} for operation in ('event/receive', 'animation/start', 'animation/stop')],
        'nodes': [
            {'declaration': 0, 'configuration': {'event': {'value': [0]}}, 'flows': {'out': {'node': 1}}},
            {'declaration': 1, 'values': clip},
            {'declaration': 0, 'configuration': {'event': {'value': [1]}}, 'flows': {'out': {'node': 3}}},
            {'declaration': 2, 'values': clip},
        ],
    }
    for path in (graph_path, *baked.values()):
        document = json.loads(path.read_text(encoding='utf-8'))
        document['extensionsUsed'] = ['KHR_materials_unlit']
        document['materials'][0]['extensions'] = {'KHR_materials_unlit': {}}
        if path == graph_path:
            document['extensionsUsed'].append('KHR_interactivity')
            document['extensionsRequired'] = ['KHR_interactivity']
            document['extensions'] = {'KHR_interactivity': {'graph': 0, 'graphs': [graph]}}
        path.write_text(json.dumps(document, indent=2), encoding='utf-8')

    sequence.write_text(json.dumps({'schemaVersion': 1, 'frames': max(SAMPLES), 'sampleFrames': list(SAMPLES), 'events': [
        {'frame': 2, 'interactivityEvent': 'startMorph'},
        {'frame': 6, 'interactivityEvent': 'stopMorph'},
        {'frame': 10, 'interactivityReset': True},
    ]}, indent=2), encoding='utf-8')
    return spec, graph_path, sequence, baked, expected


def rgb(record):
    with Image.open(record['image']) as image:
        return np.asarray(image.convert('RGB'), dtype=np.int16)


def check_sequence(cohort, controls, spec, expected):
    samples, images = [], {}
    for (frame, (time, clip_count)), record in zip(SAMPLES.items(), cohort, strict=True):
        state = record['state']
        graph = state['gltfInteractivity']
        playing = frame < 10
        require(graph['available'] and not graph['diagnostic'] and graph['playing'] is playing and
                graph['started'] is playing and graph['activeClips'] == clip_count and
                graph['time'] == ((frame - 1) * TIMESTEP if playing else 0), f'Wrong graph state at frame {frame}: {graph}')
        animation = state['gltfAnimation']
        require(animation['clipCount'] == 1 and animation['selectedClip'] == -1 and not animation['playing'] and
                animation['diagnostics'] == 0, f'Graph deformation was not isolated from manual playback: {animation}')
        deformation.check_telemetry(record, expected[time], spec, moving=frame in (1, 2, 5, 10))
        actual = state['gltfDeformation']
        if frame == 5:
            previous = deformation.evaluate(spec, time - TIMESTEP)[0]['positions']
            delta = float(np.linalg.norm(expected[time][0]['positions'] - previous, axis=1).max())
            require(not actual['historyReset'] and delta > .005, 'The moving graph clip did not preserve submitted history')
            deformation.close(actual['maxPositionDelta'], delta, 'Graph morph current/previous pose delta', 3e-4)
        if frame == 10:
            require(actual['historyReset'] and actual['maxPositionDelta'] < 1e-5, 'Graph reset retained the deformed history')
        comparison = deformation.check_pixels(record, controls[time], expected[time])
        images[frame] = rgb(record)
        samples.append(dict(frame=frame, graphTime=graph['time'], activeClips=graph['activeClips'],
                            morphWeights=actual['instances'][0]['weights'], historyReset=actual['historyReset'],
                            maxPositionDelta=actual['maxPositionDelta'], pixelComparison=comparison))
    changed = int(np.count_nonzero(np.any(np.abs(images[5] - images[1]) > 12, axis=2)))
    require(changed > 100, f'The authored clip did not visibly deform the mesh: {changed} changed pixels')
    for frame, reference in ((2, 1), (7, 5), (9, 5), (10, 1), (12, 1)):
        np.testing.assert_array_equal(images[frame], images[reference], err_msg=f'Unexpected morph pixels at frame {frame}')
    return dict(changedPixels=changed, samples=samples)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / 'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n', encoding='utf-8')
    spec, graph, sequence, baked, expected = inputs(output)
    fixture_hashes = {path.name: sha(path) for path in (graph, sequence, *baked.values())}
    report = dict(schemaVersion=1, passed=False, nativeValidationPerformed=False, captures=[], results={},
                  fixturesSha256=fixture_hashes, captureCount=0, processCount=0)
    if not args.generate_only:
        binary_hash = sha(renderer)
        captures = report['captures']
        extra = ('--import-scale', '1', '--no-ray-query', '--ray-shadows', 'raster', '--no-ddgi',
                 '--sky', 'plain', '--sky-horizon', '0', '0', '0', '--fixed-dt', str(TIMESTEP),
                 '--camera-position', *map(str, spec['camera']), '--camera-target', *map(str, spec['target']))
        try:
            for technique in TECHNIQUES:
                cohort = run(renderer, output, graph, f'{technique}-graph-morph', technique, 'lit', captures,
                             sequence=sequence, extra=extra, require_validation=True)
                controls = {}
                for time, path in baked.items():
                    control, = run(renderer, output, path, f'{technique}-{path.stem}', technique, 'lit', captures,
                                   extra=extra, require_validation=True)
                    require(not control['state']['gltfDeformation']['active'] and
                            not control['state']['gltfInteractivity']['available'], 'Baked control is not static')
                    controls[time] = control
                report['results'][technique] = check_sequence(cohort, controls, spec, expected)
                print(f'PASS {technique}: graph starts, stops and resets a GPU morph clip', flush=True)
            require(sha(renderer) == binary_hash and all(sha(output / name) == value for name, value in fixture_hashes.items()),
                    'Renderer or authored deformation fixtures changed during validation')
            report.update(passed=True, nativeValidationPerformed=True)
        finally:
            report.update(rendererSha256=binary_hash, captureCount=len(captures),
                          processCount=len({capture['log'] for capture in captures}))
            (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        return
    (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
