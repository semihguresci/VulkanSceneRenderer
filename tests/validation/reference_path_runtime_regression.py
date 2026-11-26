"""Exercise the real Vulkan reference viewport, history and float32 export."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

os.environ.setdefault('OPENCV_IO_ENABLE_OPENEXR', '1')
import cv2
import numpy as np
from PIL import Image

from bim_floor_plan_runtime_regression import panel
from gltf_extended_materials_runtime_regression import fixture, validation_log_error


def write_environment(path, rgb):
    pixels = np.full((32, 64, 3), rgb[::-1], dtype=np.float32)
    if not cv2.imwrite(str(path), pixels):
        raise RuntimeError(f'Could not write constant HDR fixture {path}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--visibility-only', action='store_true', help='Run the scene visibility and provider reload sequence')
    modes.add_argument('--shadow-budget-only', action='store_true', help='Run the raster shadow budget history check')
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / 'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n')
    report = dict(passed=False, executableSha256=hashlib.sha256(renderer.read_bytes()).hexdigest(), runs=[], captures=0)
    result_path = output / 'results.json'
    material = dict(emissiveFactor=[.5, .25, .125],
        extensions={'KHR_materials_emissive_strength': dict(emissiveStrength=8),
                    'KHR_materials_specular': dict(specularFactor=0)},
        pbrMetallicRoughness=dict(baseColorFactor=[0, 0, 0, 1], metallicFactor=0, roughnessFactor=1))
    model = output / 'radiance.gltf'
    asset = fixture(model, dict(material=material))
    model.write_text(json.dumps(asset))
    environment = output / 'constant.hdr'
    sky_rgb = [1., .5, .25]
    write_environment(environment, sky_rgb)
    environment_flags = ('--environment', str(environment), '--environment-intensity', '1')

    def capture(name, *, model_path=model, frames=(6,), events=(), cameras=(), suffix='.png', flags=(), ui=False):
        base = output / name
        sequence = base.with_suffix('.sequence.json')
        sequence.write_text(json.dumps(dict(schemaVersion=1, frames=max(frames), sampleFrames=list(frames),
            events=list(events), camera=list(cameras))))
        command = [str(renderer), '--hidden', '--validation', '--taa', '--no-bloom',
            '--width', '320', '--height', '240', '--msaa', '1', '--fixed-dt', '.016666667',
            '--model', str(model_path), '--import-scale', '1', '--render-technique', 'path-tracing',
            '--camera-position', '0', '0', '4', '--camera-target', '0', '0', '0',
            '--environment-intensity', '0', '--directional-intensity', '0', '--display-mode', 'lit',
            '--path-spp', '4', '--path-samples', '13', '--path-bounces', '4', '--exposure', '1',
            '--capture-sequence', str(sequence), '--screenshot', str(base.with_suffix(suffix)), *flags]
        if not ui:
            command += ['--no-ui', '--screenshot-viewport']
        log = base.with_suffix('.log')
        with log.open('w', encoding='utf-8') as stream:
            process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                timeout=240, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        if process.returncode or validation_log_error(log.read_text(encoding='utf-8', errors='replace')):
            raise RuntimeError(f'Vulkan capture failed: {log}')
        samples = []
        for frame in frames:
            stem = base if frame == max(frames) else Path(f'{base}.frame-{frame:04d}')
            state = json.loads(Path(f'{stem}.telemetry.json').read_text())
            image = Path(f'{stem}{suffix}')
            metadata = json.loads(Path(f'{image}.capture.json').read_text())
            if state['technique'] == 'path-tracing':
                assert metadata['referencePath']['attemptedSamplesPerPixel'] == state['referencePath']['attemptedSamples']
            if suffix == '.exr':
                rgb = cv2.imdecode(np.frombuffer(image.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
                if rgb is None or rgb.dtype != np.float32 or not np.isfinite(rgb).all():
                    raise RuntimeError(f'Invalid float radiance: {image}')
                rgb = rgb[:, :, ::-1]
            else:
                rgb = np.asarray(Image.open(image).convert('RGB'))
            samples.append(dict(frame=frame, state=state, pixels=rgb, image=str(image)))
        report['captures'] += len(samples)
        report['runs'].append(dict(name=name, log=str(log), samples=[dict(frame=s['frame'],
            image=s['image'], technique=s['state']['technique'], referencePath=s['state'].get('referencePath')) for s in samples]))
        result_path.write_text(json.dumps(report, indent=2))
        return samples

    def reference(sample, *, samples=13, resets=None):
        state = sample['state']
        path = state['referencePath']
        assert state['technique'] == 'path-tracing' and path['active'], sample
        assert path['attemptedSamples'] == samples, (sample['frame'], path)
        if resets is not None:
            assert path['historyResets'] == resets, (sample['frame'], path)
        assert path['unsupportedSamples'] == 0 and path['rejectedSamples'] == 0, path
        assert any(p['name'] == 'Lighting' and p['active'] for p in state['passes']), state['passes']
        assert all(not p['active'] for p in state['passes'] if p['name'] in
            ('TemporalResolve', 'TemporalVelocity', 'DdgiTrace', 'DdgiUpdate', 'RayShadowTrace', 'RayShadowFilter'))
        return path

    def check_restart_checkpoints(captures):
        reference(captures[0], resets=1)
        for resets, (first, full) in enumerate(zip(captures[1::2], captures[2::2], strict=True), start=2):
            reference(first, samples=4, resets=resets)
            reference(full, resets=resets)
        return captures[::2]

    def check_visibility():
        bim = output / 'provider-panel.bim'
        bim.write_text(json.dumps(dict(schema_version='1.1.0', meshes=[panel(1, (-1, -.8, 1, .8), 0)],
            elements=[dict(mesh_id=1, guid='reference-panel', type='IfcWall',
                           color=dict(r=32, g=64, b=128, a=255))])), encoding='utf-8')
        visibility = capture('visibility-provider-history', suffix='.exr', flags=environment_flags,
            frames=(4, 5, 8, 9, 12, 13, 16, 17, 20, 21, 24, 25, 28),
            cameras=(dict(frame=1, position=[0, 0, 4], target=[0, 0, 0]),), events=(
                dict(frame=5, nodeFlags=dict(sourceNode=0, visible=False)),
                dict(frame=9, sceneHistory='undo'),
                dict(frame=13, sceneHistory='redo'),
                dict(frame=17, reload=str(bim)),
                dict(frame=21, bimHiddenObject=0),
                dict(frame=25, reload=str(model))))
        original, hidden, restored, hidden_again, auxiliary, filtered, reloaded = check_restart_checkpoints(visibility)
        assert Path(auxiliary['state']['scene']['auxiliary']).resolve() == bim
        assert auxiliary['state']['rayShadows']['build']['instances'] == 1
        assert Path(reloaded['state']['scene']['primary']).resolve() == model
        np.testing.assert_allclose(original['pixels'][120, 160], [4, 2, 1], atol=.002, rtol=0)
        assert np.max(np.abs(auxiliary['pixels'][120, 160] - sky_rgb)) > .1
        for sample in (restored, reloaded):
            np.testing.assert_array_equal(sample['pixels'], original['pixels'])
        sky = np.broadcast_to(sky_rgb, hidden['pixels'].shape)
        np.testing.assert_allclose(hidden['pixels'], sky, atol=.002, rtol=0)
        for sample in (hidden_again, filtered):
            np.testing.assert_array_equal(sample['pixels'], hidden['pixels'])

    def point_light_fixture(name, *, light_range=None):
        path = output / name
        scene = json.loads(model.read_text())
        scene['materials'][0]['emissiveFactor'] = [0, 0, 0]
        scene['materials'][0]['pbrMetallicRoughness']['baseColorFactor'] = [1, 1, 1, 1]
        light = dict(type='point', intensity=4)
        if light_range is not None:
            light['range'] = light_range
        extension = 'KHR_lights_punctual'
        scene.setdefault('extensions', {})[extension] = dict(lights=[light])
        scene['nodes'][1] = dict(name='Reference point', translation=[0, 0, 2],
                                 extensions={extension: dict(light=0)})
        path.write_text(json.dumps(scene))
        return path

    def check_shadow_budget_history():
        point_model = point_light_fixture('finite-point.gltf', light_range=8)
        target = dict(type='point', source='imported', sourceNode=1)
        initial_budget = dict(frame=1, localShadowLayerBudget=8)
        light_edit = dict(frame=9, lightEdit=dict(target=target, phase='commit', values=dict(intensity=8)))
        frames = (4, 5, 8, 9, 12)
        changed = capture('raster-shadow-budget-history', model_path=point_model, suffix='.exr',
                          frames=frames, events=(initial_budget, dict(frame=5, localShadowLayerBudget=0),
                                                dict(frame=7, localShadowLayerBudget=8), light_edit))
        control = capture('raster-shadow-budget-control', model_path=point_model, suffix='.exr',
                          frames=frames, events=(initial_budget, light_edit))
        assert [sample['state']['lighting']['localShadowLayerBudget'] for sample in changed] == [8, 0, 8, 8, 8]
        assert [sample['state']['lighting']['activeLocalShadowLayers'] for sample in changed] == [6, 0, 6, 6, 6]
        for sample, baseline, count, resets in zip(changed, control, (13, 13, 13, 4, 13), (1, 1, 1, 2, 2), strict=True):
            assert sample['state']['validationEnabled']
            reference(sample, samples=count, resets=resets)
            reference(baseline, samples=count, resets=resets)
            np.testing.assert_array_equal(sample['pixels'], baseline['pixels'])
        before, after = changed[0]['pixels'][120, 160], changed[-1]['pixels'][120, 160]
        assert np.min(before) > .05, before
        np.testing.assert_allclose(after, before * 2, atol=.001, rtol=0)

    def check_viewport():
        progress = capture('sample-stop', frames=(1, 2, 3, 4, 6))
        for sample, count in zip(progress, (4, 8, 12, 13, 13), strict=True):
            reference(sample, samples=count, resets=1)
        raw = capture('linear', suffix='.exr')[0]
        reference(raw, resets=1)
        np.testing.assert_allclose(raw['pixels'][120, 160], [4, 2, 1], atol=.002, rtol=0)
        changed = capture('linear-display-changed', suffix='.exr', flags=('--exposure', '.125', '--tone-map', 'reinhard'))[0]
        reference(changed, resets=1)
        np.testing.assert_array_equal(raw['pixels'], changed['pixels'])
        bloom_raw = capture('bloom-linear', suffix='.exr', flags=('--bloom',))[0]
        reference(bloom_raw, resets=1)
        np.testing.assert_array_equal(raw['pixels'], bloom_raw['pixels'])
        bloom_display = capture('bloom-display', flags=('--bloom',))[0]
        reference(bloom_display, resets=1)
        assert any(p['name'] == 'Bloom' and p['active'] for p in bloom_display['state']['passes'])
        assert np.any(bloom_display['pixels'] != progress[-1]['pixels'])
        hdr = output / 'high-radiance.gltf'
        asset['materials'][0]['extensions']['KHR_materials_emissive_strength']['emissiveStrength'] = 400000
        hdr.write_text(json.dumps(asset))
        high = capture('float32-range', model_path=hdr, suffix='.exr')[0]
        reference(high, resets=1)
        np.testing.assert_allclose(high['pixels'][120, 160], [200000, 100000, 50000], atol=.02, rtol=0)

        empty = output / 'empty.gltf'
        empty_asset = json.loads(model.read_text())
        empty_asset['scenes'] = [dict(nodes=[])]
        empty_asset['scene'] = 0
        empty.write_text(json.dumps(empty_asset))
        background = capture('empty-scene', model_path=empty, suffix='.exr')[0]
        reference(background, resets=1)
        np.testing.assert_array_equal(background['pixels'], 0)
        replacement_environment = output / 'replacement.hdr'
        replacement_rgb = [.125, .25, 2.]
        write_environment(replacement_environment, replacement_rgb)
        sky_frames = capture('environment-replacement', model_path=empty, suffix='.exr',
            flags=environment_flags, frames=(4, 5, 6, 7, 8, 9, 12), events=(
                dict(frame=5, environment=str(replacement_environment)),
                dict(frame=6, referenceDisplay='standard-error'),
                dict(frame=7, referenceDisplay='beauty'),
                dict(frame=9, environment=str(environment))))
        for sample, count, resets in zip(sky_frames, (13, 4, 8, 12, 13, 4, 13), (1, 2, 2, 2, 2, 3, 3), strict=True):
            path = reference(sample, samples=count, resets=resets)
            frame = sample['frame']
            assert path['display'] == ('standard-error' if frame == 6 else 'beauty'), path
            rgb = replacement_rgb if 5 <= frame < 9 else sky_rgb
            np.testing.assert_allclose(sample['pixels'], np.broadcast_to(rgb, sample['pixels'].shape),
                                       atol=.002, rtol=0)

        texture = output / 'red-emissive.png'
        Image.new('RGBA', (4, 4), (255, 0, 0, 255)).save(texture)
        textures = capture('material-texture-history', suffix='.exr', frames=(4, 5, 8, 9, 12, 13, 16), events=(
            dict(frame=5, materialTextureEdit=dict(sourceMaterial=0, slot='emissive', operation='importFile',
                path=str(texture), sampler=dict(minFilter=9728, magFilter=9728))),
            dict(frame=9, sceneHistory='undo'),
            dict(frame=13, sceneHistory='redo')))
        original, edited, restored, replayed = check_restart_checkpoints(textures)
        np.testing.assert_allclose(edited['pixels'][120, 160], [4, 0, 0], atol=.002, rtol=0)
        np.testing.assert_array_equal(original['pixels'], raw['pixels'])
        np.testing.assert_array_equal(restored['pixels'], original['pixels'])
        np.testing.assert_array_equal(replayed['pixels'], edited['pixels'])

        displaced = output / 'unsupported-displacement.gltf'
        displaced_asset = json.loads(model.read_text())
        displaced_asset.update(images=[dict(uri=texture.name)], textures=[dict(source=0)])
        displaced_asset['materials'][0].update(heightTexture=dict(index=0), heightScale=.125)
        displaced.write_text(json.dumps(displaced_asset), encoding='utf-8')
        displaced_state = capture('displaced-surface', model_path=displaced)[0]['state']
        path = displaced_state['referencePath']
        assert path['active'] and path['attemptedSamples'] == 13, path
        assert path['unsupportedSamples'] == 320*240*13 and path['rejectedSamples'] == 0, path
        assert 'ray scene is incompatible with active geometry' in path['status'], path

        blend = output / 'unsupported-blend.gltf'
        blend_asset=json.loads(model.read_text())
        blend_asset['materials'][0]['alphaMode'] = 'BLEND'
        blend_asset['materials'][0]['pbrMetallicRoughness']['baseColorFactor'][3] = .5
        blend.write_text(json.dumps(blend_asset))
        blended = capture('alpha-coverage', model_path=blend, suffix='.exr')[0]
        reference(blended, resets=1)
        np.testing.assert_allclose(blended['pixels'][100:140,140:180].mean(axis=(0,1)),[2,1,.5],atol=.05,rtol=0)
        atmosphere = capture('unsupported-atmosphere', model_path=empty, flags=('--sky', 'atmosphere'))[0]
        path = atmosphere['state']['referencePath']
        assert path['unsupportedSamples'] == 320*240*13 and path['rejectedSamples'] == 0, path
        assert 'atmosphere' in path['status']

        changes = capture('camera-material-history', frames=(4, 8, 12, 16, 20, 24),
            events=(dict(frame=5, exposure=.25),
                dict(frame=13, materialEdit=dict(sourceMaterial=0, values=[dict(property='emissiveFactor', value=[.25,.5,.125])])),
                dict(frame=17, sceneHistory='undo'), dict(frame=21, sceneHistory='redo')),
            cameras=(dict(frame=1, position=[0,0,4], target=[0,0,0]),
                dict(frame=8, position=[0,0,4], target=[0,0,0]),
                dict(frame=9, position=[.4,0,4], target=[.4,0,0])))
        for sample, resets in zip(changes, (1, 1, 2, 3, 4, 5), strict=True):
            reference(sample, resets=resets)
        np.testing.assert_array_equal(changes[2]['pixels'], changes[4]['pixels'])
        np.testing.assert_array_equal(changes[3]['pixels'], changes[5]['pixels'])
        assert np.any(changes[2]['pixels'] != changes[3]['pixels'])

        point_model = point_light_fixture('point-light.gltf')
        target = dict(type='point', source='imported', sourceNode=1)
        lights = capture('light-history-gizmo', model_path=point_model, suffix='.exr', frames=(4,8,12,16,20,24,28), events=(
            dict(frame=5, lightEdit=dict(target=target, phase='commit', values=dict(intensity=8))),
            dict(frame=9, sceneHistory='undo'), dict(frame=13, sceneHistory='redo'),
            dict(frame=17, lightEdit=dict(target=target, phase='commit', gizmo=dict(tool='translate', amount=[0,0,2]))),
            dict(frame=21, sceneHistory='undo'), dict(frame=25, sceneHistory='redo')))
        for sample, resets in zip(lights, range(1,8), strict=True): reference(sample, resets=resets)
        np.testing.assert_allclose(lights[0]['pixels'][120,160], [1/np.pi]*3, atol=.001, rtol=0)
        np.testing.assert_allclose(lights[1]['pixels'], lights[0]['pixels']*2, atol=.00001, rtol=0)
        for first, second in ((0,2),(1,3),(3,5),(4,6)):
            np.testing.assert_array_equal(lights[first]['pixels'], lights[second]['pixels'])
        np.testing.assert_allclose(lights[4]['pixels'][120,160], lights[3]['pixels'][120,160]*.25, atol=.001, rtol=0)

        switches = capture('switch-resize', frames=(4,8,12,16,20,24), events=(
            dict(frame=5, technique='forward-raster'), dict(frame=9, technique='path-tracing'),
            dict(frame=13, technique='deferred-raster'), dict(frame=17, technique='path-tracing'),
            dict(frame=21, resize=[384,200])))
        for index in (0,2,4,5): reference(switches[index])
        assert switches[1]['state']['technique'] == 'forward-raster'
        assert switches[3]['state']['technique'] == 'deferred-raster'
        assert switches[-1]['pixels'].shape == (200,384,3)
        fallback = capture('query-unavailable', flags=('--no-ray-query',))[0]
        assert fallback['state']['technique'] == 'deferred-raster'
        gpu = capture('gpu-hierarchy', flags=('--gpu-transforms',))[0]
        reference(gpu, resets=1)
        assert gpu['state']['hierarchyTransforms']['active']
        reference(capture('simple-ui', ui=True, flags=('--width','1280','--height','800'))[0], resets=1)

    try:
        if args.shadow_budget_only:
            check_shadow_budget_history()
        else:
            check_visibility()
            if not args.visibility_only:
                check_viewport()
                check_shadow_budget_history()
        if hashlib.sha256(renderer.read_bytes()).hexdigest() != report['executableSha256']:
            raise RuntimeError('Executable changed during validation')
        report['passed'] = True
    finally:
        result_path.write_text(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
