"""Exercise the Vulkan reference viewport, raw radiance, and raster fallback."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

os.environ.setdefault('OPENCV_IO_ENABLE_OPENEXR', '1')
import cv2
import numpy as np
from PIL import Image

from gltf_accessor_runtime_regression import Asset, mesh


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(strict=True), args.output.resolve()
    output.mkdir(parents=True)
    (output/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n', encoding='utf-8')
    renderer_hash = hashlib.sha256(renderer.read_bytes()).hexdigest()
    asset = Asset()
    positions = asset.accessor('VEC3', [[-1,-1,0],[1,-1,0],[1,1,0],[-1,1,0]])
    normals = asset.accessor('VEC3', [[0,0,1]]*4)
    uv = asset.accessor('VEC2', [[0,0],[1,0],[1,1],[0,1]])
    mesh(asset, positions, normals, uv)
    material = dict(emissiveFactor=[.5,.25,.125],
                    extensions={'KHR_materials_emissive_strength': dict(emissiveStrength=8)},
                    pbrMetallicRoughness=dict(baseColorFactor=[0,0,0,1], metallicFactor=0, roughnessFactor=1))
    asset.model['extensionsUsed'] = ['KHR_materials_emissive_strength']
    asset.model['materials'] = [material]
    normal, blend, high = [output/(name+'.gltf') for name in ('opaque','blend','high')]
    asset.write(normal)
    material['alphaMode'] = 'BLEND'
    material['pbrMetallicRoughness']['baseColorFactor'][3] = .5
    asset.write(blend)
    material.pop('alphaMode')
    material['pbrMetallicRoughness']['baseColorFactor'][3] = 1
    material['extensions']['KHR_materials_emissive_strength']['emissiveStrength'] = 400000
    asset.write(high)

    results = []

    def capture(name, model, frames, events=(), *, suffix='.png', extra=()):
        stem = output/name
        sequence = stem.with_suffix('.json')
        sequence.write_text(json.dumps(dict(schemaVersion=1, frames=frames[-1], sampleFrames=frames, events=events)),
                            encoding='utf-8')
        command = [str(renderer), '--hidden', '--no-ui', '--validation', '--taa', '--no-bloom',
                   '--width', '320', '--height', '240', '--msaa', '1', '--model', str(model),
                   '--render-technique', 'path-tracing', '--display-mode', 'lit',
                   '--camera-position', '0','0','4', '--camera-target','0','0','0',
                   '--environment-intensity', '0', '--directional-intensity', '0',
                   '--path-spp', '4', '--path-samples', '13', '--path-bounces', '4', '--exposure', '1',
                   '--capture-sequence', str(sequence), '--screenshot', str(stem.with_suffix(suffix)), *extra]
        log = stem.with_suffix('.log')
        with log.open('x', encoding='utf-8') as stream:
            subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                           env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)), check=True, timeout=180,
                           creationflags=subprocess.CREATE_NO_WINDOW)
        assert not re.search(r'VUID-|SYNC-HAZARD|Validation Error', log.read_text(errors='replace'), re.I), log
        rows = []
        for frame in frames:
            name = str(stem) if frame == frames[-1] else f'{stem}.frame-{frame:04d}'
            image = Path(name + suffix)
            state = json.loads(Path(name+'.telemetry.json').read_text(encoding='utf-8'))
            if state['technique']=='path-tracing':
                metadata = json.loads(Path(str(image)+'.capture.json').read_text(encoding='utf-8'))
                path = state['referencePath']
                assert metadata['referencePath']['attemptedSamplesPerPixel']==path['attemptedSamples']
                assert path['active'] and path['unsupportedSamples']==path['rejectedSamples']==0, path
                passes = {p['name'] for p in state['passes'] if p['active']}
                assert 'Lighting' in passes
                assert not passes.intersection({'TemporalResolve','TemporalVelocity','DdgiTrace','DdgiUpdate',
                                               'RayShadowTrace','RayShadowFilter','OitResolve'})
            if suffix == '.exr':
                bgr = cv2.imdecode(np.frombuffer(image.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
                assert bgr is not None and bgr.dtype == np.float32 and np.isfinite(bgr).all(), image
                pixels = bgr[:,:,::-1]
            else:
                with Image.open(image) as opened:
                    pixels = np.asarray(opened.convert('RGB'))
            rows.append((state,pixels))
            results.append(dict(frame=frame, image=str(image), technique=state['technique'],
                                reference=state.get('referencePath'), log=str(log)))
        return rows

    progression = capture('sample-stop', normal, [1,2,3,4,6])
    assert [s['referencePath']['attemptedSamples'] for s,_ in progression]==[4,8,12,13,13]
    np.testing.assert_array_equal(progression[-1][1],progression[-2][1])

    raw = capture('raw-hdr', high, [4,8], [dict(frame=5,exposure=.125)], suffix='.exr')
    assert [s['referencePath']['historyResets'] for s,_ in raw]==[1,1]
    np.testing.assert_allclose(raw[0][1][120,160], [200000,100000,50000], atol=.02, rtol=0)
    np.testing.assert_array_equal(raw[0][1],raw[1][1])
    coverage = capture('alpha-coverage', blend, [4], suffix='.exr')[0][1]
    np.testing.assert_allclose(coverage[100:140,140:180].mean(axis=(0,1)), [2,1,.5], atol=.08, rtol=0)

    switched = capture('switch-and-resize', normal, [4,8,12,16,20,24], [
        dict(frame=5,technique='forward-raster'),dict(frame=9,technique='path-tracing'),
        dict(frame=13,technique='deferred-raster'),dict(frame=17,technique='path-tracing'),
        dict(frame=21,resize=[384,200])])
    assert [s['technique'] for s,_ in switched]==[
        'path-tracing','forward-raster','path-tracing','deferred-raster','path-tracing','path-tracing']
    for i in (0,2,4,5):
        assert switched[i][0]['referencePath']['attemptedSamples']==13
    for i in (2,4):
        np.testing.assert_array_equal(switched[0][1],switched[i][1])
    assert switched[-1][1].shape==(200,384,3)
    fallback = capture('query-disabled', normal, [4], extra=('--no-ray-query',))[0][0]
    assert fallback['technique']=='deferred-raster' and 'referencePath' not in fallback
    assert hashlib.sha256(renderer.read_bytes()).hexdigest()==renderer_hash
    (output/'results.json').write_text(json.dumps(dict(passed=True,rendererSha256=renderer_hash,captures=results),
                                                 indent=2)+'\n', encoding='utf-8')
    print(f'PASS {len(results)} reference viewport captures')


if __name__ == '__main__':
    main()
