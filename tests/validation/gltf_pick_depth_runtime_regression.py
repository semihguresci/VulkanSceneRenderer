"""Probe production Vulkan depth readback for an animated transparent mesh."""

import argparse
import json
import os
from pathlib import Path
import subprocess

import numpy as np

from gltf_accessor_runtime_regression import Asset, require
from gltf_extended_materials_runtime_regression import validation_log_error


def write_scene(path):
    asset = Asset()
    model = asset.model
    model['nodes'] = [dict(name='Opaque receiver', mesh=0),
                      dict(name='Animated transparent surface', mesh=1)]
    model['scenes'][0]['nodes'] = [0, 1]
    model['materials'] = [
        dict(doubleSided=True, pbrMetallicRoughness=dict(baseColorFactor=[.8, .1, .1, 1])),
        dict(doubleSided=True, alphaMode='BLEND', pbrMetallicRoughness=dict(baseColorFactor=[.1, .8, .1, .6])),
    ]
    positions = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]
    indices = asset.accessor('SCALAR', [0, 1, 2, 0, 2, 3], 5123, indices=True)
    normals = asset.accessor('VEC3', [(0, 0, 1)] * 4)
    for material, z in enumerate((0, 1)):
        position = asset.accessor('VEC3', [(x, y, z) for x, y, _ in positions])
        primitive = dict(attributes=dict(POSITION=position, NORMAL=normals),
                         indices=indices, material=material)
        mesh = dict(primitives=[primitive])
        if material == 1:
            primitive['targets'] = [dict(POSITION=asset.accessor('VEC3', [(0, 0, 1)] * 4))]
            mesh['weights'] = [0]
        model['meshes'].append(mesh)
    times = asset.accessor('SCALAR', [0, 1])
    weights = asset.accessor('SCALAR', [0, 1])
    model['animations'] = [dict(
        samplers=[dict(input=times, output=weights, interpolation='LINEAR')],
        channels=[dict(sampler=0, target=dict(node=1, path='weights'))],
    )]
    asset.write(path)


def projected_depth(telemetry, z):
    columns = np.asarray(telemetry['camera']['unjitteredViewProjColumns'], dtype=float).reshape(4, 4)
    clip = columns.T @ [0, 0, z, 1]
    return float(clip[2] / clip[3])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--msaa', type=int, choices=(1, 2, 4, 8), default=1)
    args = parser.parse_args()
    renderer = args.renderer.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True)
    scene, sequence = output / 'planes.gltf', output / 'probe.json'
    write_scene(scene)
    samples = [(3, (160, 90), (0, 1)), (6, (160, 90), (0, 1.5)),
               (9, (1, 1), 'background'), (11, (192, 108), (0, 1.5)),
               (12, (384, 108), 'out-of-bounds')]
    final_frame = 15
    events = [dict(frame=1, animationClip=0, animationPlaying=False),
              dict(frame=6, animationTime=.5), dict(frame=10, resize=[384, 216])]
    by_frame = {event['frame']: event for event in events}
    for frame, pixel, _ in samples:
        by_frame.setdefault(frame, dict(frame=frame))['depthProbe'] = list(pixel)
    events = [by_frame[frame] for frame in sorted(by_frame)]
    sequence.write_text(json.dumps(dict(schemaVersion=1, frames=final_frame,
                                        sampleFrames=[frame for frame, _, _ in samples] + [final_frame], events=events)))
    (output / 'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n')
    results = []
    for technique in ('deferred-raster', 'forward-raster'):
        image = output / f'{technique}.png'
        log_path = output / f'{technique}.log'
        command = [str(renderer), '--hidden', '--no-ui', '--validation', '--no-taa',
                   '--no-ray-query', '--no-bloom', '--display-mode', 'lit', '--width', '320', '--height', '180',
                   '--msaa', str(args.msaa), '--camera-position', '0', '0', '6', '--camera-target', '0', '0', '0',
                   '--import-scale', '1', '--render-technique', technique, '--model', str(scene),
                   '--capture-sequence', str(sequence), '--screenshot', str(image)]
        with log_path.open('wb') as log:
            subprocess.run(command, cwd=renderer.parent, stdout=log, stderr=subprocess.STDOUT,
                           env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)), check=True, timeout=120)
        require(not validation_log_error(log_path.read_text(errors='replace')), f'{technique} Vulkan validation failed')
        for frame, pixel, expected_surface in samples:
            stem = f'{technique}.frame-{frame:04d}'
            telemetry = json.loads((output / f'{stem}.telemetry.json').read_text())
            require(telemetry['msaaSamples'] == args.msaa, f'{technique} did not use requested MSAA')
            probe = telemetry.get('depthProbe')
            require(probe is not None and probe['pixel'] == list(pixel), f'{technique} missing probe at frame {frame}')
            values = [probe['opaque'], probe['pick']]
            if expected_surface == 'out-of-bounds':
                expected = [None, None]
                require(values == expected, f'{technique} sampled a pixel outside the framebuffer')
            else:
                expected = [0, 0] if expected_surface == 'background' else [
                    projected_depth(telemetry, z) for z in expected_surface]
                require(all(v is not None for v in values) and np.allclose(values, expected, rtol=0, atol=2e-6),
                        f'{technique} frame {frame} depth {values}, expected {expected}')
            results.append(dict(technique=technique, msaa=args.msaa, frame=frame, pixel=pixel, depth=values, expected=expected))
        final = json.loads((output / f'{technique}.telemetry.json').read_text())
        require('depthProbe' not in final, f'{technique} retained a probe from an earlier frame')
    (output / 'results.json').write_text(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
