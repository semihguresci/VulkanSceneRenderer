"""Verify named camera selection after scene node rename, history and save."""

import argparse
import json
from pathlib import Path

from gltf_accessor_runtime_regression import load_asset, require
from gltf_inactive_scene_capacity_runtime_regression import verify_runtime_manifest
from gltf_material_variants_runtime_regression import pixels, run
from scene_node_authoring_runtime_regression import TECHNIQUES, fixture


SOURCE_CAMERA_NODE = 2
ORIGINAL = 'Original camera node'
RENAMED = 'Renamed camera 東'
CAPTURE_FRAMES = (3, 9, 15, 21)
SETTINGS = ('--sky', 'plain', '--directional-intensity', '0', '--import-scale', '1', '--no-ray-query')


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')


def select(frame, name=None, *, reject=False):
    action = dict(frame=frame, cameraSelection=dict(nodeName=name) if name else dict(viewport=True))
    if reject:
        action['expectCameraSelectionFailure'] = True
    return action


def sequence(path, frames, events):
    write_json(path, dict(schemaVersion=1, frames=frames[-1], sampleFrames=frames, events=events))


def make_fixture(source, history, reload):
    fixture(source)
    model = json.loads(source.read_text(encoding='utf-8'))
    model['cameras'] = [dict(name='Shared lens', type='perspective', perspective=dict(yfov=0.7, znear=0.1))]
    model['nodes'][0]['children'].append(SOURCE_CAMERA_NODE)
    model['nodes'].append(dict(name=ORIGINAL, camera=0, translation=[0.2, 0.1, 4.3]))
    write_json(source, model)
    sequence(history, CAPTURE_FRAMES, [
        select(1, ORIGINAL),
        dict(frame=4, nodeAuthoring=dict(action='rename', sourceNode=SOURCE_CAMERA_NODE, name=RENAMED)),
        select(5), select(6, RENAMED), select(7, ORIGINAL, reject=True),
        dict(frame=10, sceneHistory='undo'), select(11), select(12, ORIGINAL), select(13, RENAMED, reject=True),
        dict(frame=16, sceneHistory='redo'), select(17), select(18, RENAMED), select(19, ORIGINAL, reject=True),
    ])
    sequence(reload, [3], [select(1, RENAMED)])


def check_capture(row, name):
    state = row['state']
    camera = state['gltfCameraAuthoring']
    require(len(camera['cameras']) == 1 and 'view' in camera and 'projection' in camera,
            f'No active authored camera: {row["telemetry"]}')
    binding, = camera['cameras']
    graph_node, = [node for node in state['gltfMerge']['authoringNodes'] if node['graphNode'] == binding['graphNode']]
    require(camera['selectedSourceNode'] == binding['sourceNode'] == graph_node['sourceNode'] and
            graph_node['name'] == name and graph_node['active'], f'Wrong camera identity: {row["telemetry"]}')
    return camera, graph_node


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--technique', action='append', choices=TECHNIQUES)
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    techniques = tuple(dict.fromkeys(args.technique or TECHNIQUES))
    report = dict(schemaVersion=1, passed=False, generatedOnly=args.generate_only, nativeValidationPerformed=False,
                  expectedCaptureCount=5 * len(techniques), captures=[], checks=[])
    try:
        source, history, reload = (output / name for name in ('source.gltf', 'history.json', 'reload.json'))
        make_fixture(source, history, reload)
        if args.generate_only:
            report['passed'] = True
            return
        require(args.renderer is not None and args.manifest is not None, 'Native validation requires renderer and manifest')
        renderer, manifest = args.renderer.resolve(), args.manifest.resolve()
        report['nativeFreeze'] = verify_runtime_manifest(manifest, renderer)
        (output / 'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n', encoding='utf-8')

        def capture(technique, label, model, script, *, save=None):
            return run(renderer, output, model, f'{technique}-{label}', technique, 'lit', report['captures'],
                       sequence=script, save=save, extra=SETTINGS, require_validation=True, use_authored_sun=True)

        for technique in techniques:
            extension = 'gltf' if technique == 'deferred-raster' else 'glb'
            destination = output / technique / f'edited.{extension}'
            destination.parent.mkdir()
            rows = capture(technique, 'history', source, history, save=destination)
            require(len(rows) == len(CAPTURE_FRAMES), f'Incomplete history captures: {technique}')
            baseline, owner = check_capture(rows[0], ORIGINAL)
            log = Path(rows[0]['log']).read_text(encoding='utf-8', errors='replace')
            require(log.count('Expected capture camera selection failure:') == 3,
                    f'Old or undone camera name remained selectable: {technique}')
            for row, name, undo, redo in zip(rows, (ORIGINAL, RENAMED, ORIGINAL, RENAMED), (0, 1, 0, 1), (0, 0, 1, 0), strict=True):
                camera, current = check_capture(row, name)
                state = row['state']
                require(current['graphNode'] == owner['graphNode'] and current['sourceNode'] == SOURCE_CAMERA_NODE and
                        state.get('sceneEditHistory', dict(undo=0, redo=0)) == dict(undo=undo, redo=redo) and
                        camera['view'] == baseline['view'] and camera['projection'] == baseline['projection'],
                        f'Rename/history changed camera identity, pose, lens or history: {row["telemetry"]}')
                metrics = pixels(rows[0]['image'], row['image'])
                require(metrics['maximumError'] == 0, f'Camera rename changed the rendered view: {technique} {metrics}')
                report['checks'].append(dict(technique=technique, name=name, undo=undo, redo=redo, **metrics))

            model, _ = load_asset(destination)
            require([node.get('name') for node in model['nodes'] if 'camera' in node] == [RENAMED],
                    f'Export did not keep the current camera node name: {destination}')
            saved, = capture(technique, 'reload', destination, reload)
            camera, _ = check_capture(saved, RENAMED)
            metrics = pixels(rows[-1]['image'], saved['image'])
            require(camera['view'] == baseline['view'] and camera['projection'] == baseline['projection'] and
                    metrics['maximumError'] == 0, f'Renamed camera reload changed the view: {technique} {metrics}')
            report['checks'].append(dict(technique=technique, name=RENAMED, exported=extension, **metrics))
        require(report['nativeFreeze'] == verify_runtime_manifest(manifest, renderer) and
                len(report['captures']) == report['expectedCaptureCount'], 'Incomplete or changed camera-rename run')
        report.update(passed=True, nativeValidationPerformed=True)
    finally:
        report.update(captureCount=len(report['captures']), checkCount=len(report['checks']))
        write_json(output / 'results.json', report)


if __name__ == '__main__':
    main()
