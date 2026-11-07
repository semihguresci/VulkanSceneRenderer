"""Compare optional GPU hierarchy propagation with the CPU path in real Vulkan frames.

Animated, mirrored EXT_mesh_gpu_instancing fixtures include static, skinned and
morphed geometry. Both raster paths toggle propagation, change MSAA/extent and
replace the scene. CPU mirrors and extraction remain, while ray profiles
require GPU-published TLAS inputs. Image checks do not establish an end-to-end
performance gain.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from gltf_extended_materials_runtime_regression import validation_log_error
from gltf_instancing_runtime_regression import fixture
from gltf_material_variants_runtime_regression import pixels


FRAMES = (4, 8, 12, 16, 20, 24, 28, 32, 36)


def verify_ray_execution(samples, asynchronous=True):
    witnesses = set()
    published = 0
    for sample in samples:
        state = json.loads(Path(sample['telemetry']).read_text())
        if not state['rayShadows']['supported'] or not state['rayShadows']['active']:
            raise RuntimeError('Ray-shadow acceptance requires an active supported Vulkan ray path')
        gi = state['ddgi']
        if not gi['supported'] or not gi['active'] or gi['asyncActive'] != asynchronous:
            raise RuntimeError('DDGI acceptance requires the requested active Vulkan path')
        published = max(published, gi['asyncPublishedUpdates'] if asynchronous else gi['updateFrame'])
        for timing in state['passes']:
            if timing.get('gpuTimed') and timing.get('gpuMs', 0) > 0:
                witnesses.add(timing['name'])
    if not published or not {'RayShadowTrace', 'DdgiTrace'} <= witnesses:
        raise RuntimeError('Missing positive ray/DDGI execution and publication witnesses')
    return {('asyncPublishedUpdates' if asynchronous else 'updateFrame'): published, 'timedPasses': sorted(witnesses)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', action='append', choices=('static', 'skin', 'morph'))
    parser.add_argument('--ray', action='store_true')
    parser.add_argument('--taa', action='store_true', help='Use continuous animation and TAA at MSAA 1x')
    parser.add_argument('--gi', action='store_true', help='Enable visible synchronous DDGI instead of the zero-intensity witness')
    args = parser.parse_args()
    if args.gi:
        args.ray = True
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / 'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n')
    report = dict(passed=False, executableSha256=hashlib.sha256(renderer.read_bytes()).hexdigest(),
                  cases=[], captures=0)
    report_path = output / 'results.json'

    def capture(model, technique, case, gpu):
        base = output / f'{technique}-{case}-{"gpu" if gpu else "cpu"}'
        events = [dict(frame=1, animationPlaying=args.taa, animationClip=0, animationTime=0),
                  dict(frame=6, animationTime=.7), dict(frame=10, gpuTransforms=False),
                  dict(frame=14, animationTime=.25), dict(frame=18, gpuTransforms=gpu, samples=1 if args.taa else 4),
                  dict(frame=22, resize=[512, 320]), dict(frame=26, animationTime=.9),
                  dict(frame=30, reload=str(model)),
                  dict(frame=34, animationPlaying=args.taa, animationClip=0, animationTime=.85)]
        sequence = base.with_suffix('.json')
        sequence.write_text(json.dumps(dict(schemaVersion=1, frames=FRAMES[-1], sampleFrames=FRAMES, events=events)))
        command = [str(renderer), '--hidden', '--no-ui', '--validation', '--taa' if args.taa else '--no-taa', '--no-bloom',
                   '--width', '640', '--height', '360', '--msaa', '1', '--fixed-dt', '.016666667',
                   '--model', str(model), '--import-scale', '1', '--render-technique', technique,
                   '--camera-position', '0', '0', '4', '--camera-target', '0', '0', '0',
                   '--environment-intensity', '0', '--directional-intensity', '3',
                   '--directional-direction', '0', '1', '-1', '--exposure', '1',
                   '--capture-sequence', str(sequence), '--screenshot', str(base.with_suffix('.png')),
                   '--screenshot-viewport']
        if gpu:
            command.append('--gpu-transforms')
        if args.ray:
            command += ['--ray-shadows', 'hard', '--no-ray-shadow-denoise', '--ddgi',
                        '--ddgi-counts', '3', '3', '3', '--ddgi-rays', '64' if args.gi else '4', '--ddgi-probes-per-frame', '27',
                        '--ddgi-ray-budget', '1728' if args.gi else '108', '--ddgi-intensity', '1' if args.gi else '0']
            if args.gi:
                command += ['--ddgi-origin','-1.5','-1.5','-1','--ddgi-spacing','1.5','1.5','1']
            else:
                command.append('--ddgi-async')
        for frame in FRAMES:
            stem = base if frame == FRAMES[-1] else Path(f'{base}.frame-{frame:04d}')
            Path(f'{stem}.png').unlink(missing_ok=True)
            Path(f'{stem}.telemetry.json').unlink(missing_ok=True)
        log = base.with_suffix('.log')
        with log.open('w', encoding='utf-8') as stream:
            process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                                     timeout=240, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                                     creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        if process.returncode or validation_log_error(log.read_text(encoding='utf-8', errors='replace')):
            raise RuntimeError(f'Vulkan capture failed: {log}')
        samples = []
        for frame in FRAMES:
            stem = base if frame == FRAMES[-1] else Path(f'{base}.frame-{frame:04d}')
            state = json.loads(Path(f'{stem}.telemetry.json').read_text())
            hierarchy = state['hierarchyTransforms']
            if state['taa']['enabled'] != args.taa:
                raise RuntimeError(f'Incorrect temporal path: {stem}')
            expected = gpu and (frame < 10 or frame >= 18)
            if hierarchy['active'] != expected or hierarchy['enabled'] != expected:
                raise RuntimeError(f'Incorrect live path at {stem}: {hierarchy}')
            if expected and (hierarchy['objects'] < 5 or hierarchy['levels'] < 3):
                raise RuntimeError(f'Incomplete hierarchy dispatch: {stem}')
            if expected and (hierarchy['cpuObjectUploadBytes'] or hierarchy['objectMetadataBytes']>112*hierarchy['objects']):
                raise RuntimeError(f'GPU object publication retained CPU writes or oversized metadata: {stem}')
            if args.ray:
                gpu_instances = state['rayShadows']['build']['gpuInstances']
                if (expected and gpu_instances < 5) or (not expected and gpu_instances):
                    raise RuntimeError(f'Incorrect GPU TLAS publication: {stem}: {gpu_instances}')
            samples.append(dict(frame=frame, image=f'{stem}.png', telemetry=f'{stem}.telemetry.json', hierarchy=hierarchy))
        if gpu and not any(s['hierarchy'].get('previousGpuMs', 0) > 0 for s in samples):
            raise RuntimeError(f'No actual GPU timing witness: {log}')
        report['captures'] += len(samples)
        return samples

    try:
        for case in args.case or ('static', 'skin', 'morph'):
            model = fixture(output / f'{case}.gltf', case, 'source', ray=args.ray, animated=True)
            for technique in ('deferred-raster', 'forward-raster'):
                cpu = capture(model, technique, case, False)
                gpu = capture(model, technique, case, True)
                comparisons = []
                for first, second in zip(cpu, gpu, strict=True):
                    difference = pixels(first['image'], second['image'])
                    if difference['meanAbsoluteError'] > .01 or difference['maximumError'] > 3:
                        raise RuntimeError(f'GPU/CPU mismatch: {second["image"]}: {difference}')
                    comparisons.append(dict(frame=first['frame'], **difference))
                motion = pixels(cpu[0]['image'], cpu[1]['image'])
                if motion['meanAbsoluteError'] < .01:
                    raise RuntimeError('The animated parent/owner fixture did not move')
                record = dict(case=case, technique=technique, cpu=cpu, gpu=gpu, comparisons=comparisons)
                if args.ray:
                    record['rayExecution'] = verify_ray_execution(gpu, not args.gi)
                report['cases'].append(record)
                print(f'{technique} {case}: {len(comparisons)} CPU/GPU pairs passed', flush=True)
                report_path.write_text(json.dumps(report, indent=2))
        if hashlib.sha256(renderer.read_bytes()).hexdigest() != report['executableSha256']:
            raise RuntimeError('Executable changed during validation')
        report['passed'] = True
    finally:
        report_path.write_text(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
