"""Native vertex RGBA coverage checks with independent alpha-texture oracles.

The source linearly interpolates corner alpha 0..1. The reference has RGB-only
vertices and samples a two-texel alpha ramp at u=.25+.5*original_u, reproducing
the same alpha exactly under bilinear interpolation. Layered blue receivers
expose incorrect MASK depth writes. Shadow receivers have independently chosen
blocked/lit regions. No production evaluator or shader helper supplies oracles.

GPU picking has no existing native capture action and is not asserted here.
--generate-only launches no renderer. Requires NumPy, Pillow and Vulkan for runs.
"""
from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path

import numpy as np

from gltf_accessor_runtime_regression import (Asset, NativeRun, TECHNIQUES,
    compare, decode_accessor, f32, load_asset, mesh, pixels, require, source_accessor_signature)

CASES = ('float-mask', 'u8-blend', 'u16-mask', 'float-opaque', 'deform-u16-blend',
         'opaque-transmission-zero', 'opaque-transmission-gradient')
FRONT = np.array([[-.9, -.8, .2], [.9, -.8, .2], [.9, .8, .2], [-.9, .8, .2]])
UV = np.array([[0., 1.], [1., 1.], [1., 0.], [0., 0.]])
ALPHA_TEXTURE = 128 / 255
MATERIAL_ALPHA = .75
CUTOFF = ALPHA_TEXTURE * MATERIAL_ALPHA * .5


def alpha_texture(reference=False, *, opaque=False, zero=False):
    rgba = np.tile([255, 255, 255, 128], (2, 2, 1)).astype(np.uint8)
    if reference and not opaque:
        rgba[:, 0, 3] = 0
    if zero:
        rgba[:, :, 3] = 0
    return rgba


def add_receiver(asset, *, shadow=False):
    # Larger than the front probe, so a discarded near fragment reveals a
    # real farther surface instead of an indistinguishable clear pixel.
    z, extent = (0., [1.6, 1.2]) if shadow else (-.4, [1.25, 1.15])
    p = [[-extent[0], -extent[1], z], [extent[0], -extent[1], z],
         [extent[0], extent[1], z], [-extent[0], extent[1], z]]
    primitive = dict(attributes=dict(POSITION=asset.accessor('VEC3', p),
                                      NORMAL=asset.accessor('VEC3', [[0., 0., 1.]]*4)),
                     indices=asset.accessor('SCALAR', [0, 1, 2, 0, 2, 3], 5123, indices=True), material=1)
    asset.model['meshes'].append(dict(primitives=[primitive]))
    asset.model['nodes'].append(dict(name='Independent farther receiver', mesh=1))
    asset.model['scenes'][0]['nodes'].append(len(asset.model['nodes'])-1)
    receiver = dict(pbrMetallicRoughness=dict(baseColorFactor=[.75, .75, .75, 1] if shadow else [.015, .04, 1, 1],
                                              metallicFactor=0, roughnessFactor=1))
    if not shadow:
        receiver['extensions'] = dict(KHR_materials_unlit={})
    asset.model['materials'].append(receiver)


def write_alpha_case(path, case, *, kind='source', shadow=False, clear=False, mode_override=None):
    require(kind in ('source', 'reference', 'dropped'), 'Unknown alpha fixture kind')
    asset = Asset()
    opaque = case == 'float-opaque' or case.startswith('opaque-transmission-')
    transmission = case.startswith('opaque-transmission-')
    zero_alpha = opaque and case != 'opaque-transmission-gradient'
    animated = case == 'deform-u16-blend'
    mode = 'OPAQUE' if opaque else 'MASK' if case.endswith('mask') else 'BLEND'
    if shadow:
        mode = 'MASK'
    if kind == 'dropped' and opaque:
        mode = 'MASK' if zero_alpha else 'BLEND'
    if mode_override:
        mode = mode_override
    component = 5123 if 'u16' in case else 5121 if case.startswith('u8') else 5126
    p = FRONT.copy()
    if shadow:
        p[:, 2] = .7
    delta = np.array([[0, 0, 0], [.25, .05, 0], [.3, .15, .22], [-.05, .05, 0]])
    if animated and kind != 'source':
        # Independent one-joint skin, after a .8-weight morph. The owner has an
        # unrelated translation that cancels in owner-inverse * joint skinning.
        p = np.asarray(f32(p)) + np.asarray(f32(delta)) * np.float32(.8) + np.asarray(f32([.15, .1, 0]))
    uv = UV.copy()
    if kind == 'reference' and mode != 'OPAQUE' and not zero_alpha:
        uv = .25 + .5 * uv
    alpha = np.zeros(4) if zero_alpha else UV[:, 0].copy()
    if kind == 'dropped' and not opaque:
        alpha[:] = 1
    rgba = np.column_stack((np.ones((4, 3)), alpha))
    color_kind = 'VEC4'
    normalize = component != 5126
    if kind == 'reference':
        rgba, component, color_kind, normalize = np.ones((4, 3)), 5126, 'VEC3', False
    elif normalize:
        rgba = np.rint(rgba * (255 if component == 5121 else 65535)).astype(int)
    color = asset.accessor(color_kind, rgba, component, normalize=normalize)
    primitive = mesh(asset, asset.accessor('VEC3', f32(p)),
        asset.accessor('VEC3', [[0., 0., 1.]]*4), asset.accessor('VEC2', f32(uv)), colors=color)
    material = dict(name='Vertex alpha probe', alphaMode=mode, alphaCutoff=CUTOFF,
        extensions=dict(KHR_materials_unlit={}),
        pbrMetallicRoughness=dict(baseColorFactor=[1, .025, .01, MATERIAL_ALPHA],
            baseColorTexture=dict(index=asset.image(alpha_texture(kind == 'reference', opaque=mode=='OPAQUE',
                zero=kind=='reference' and zero_alpha and mode!='OPAQUE'))),
            metallicFactor=0, roughnessFactor=1))
    if transmission:
        material['extensions'] = dict(KHR_materials_transmission=dict(transmissionFactor=.6,
            transmissionTexture=dict(index=material['pbrMetallicRoughness']['baseColorTexture']['index'])))
        material['pbrMetallicRoughness']['roughnessFactor'] = .25
    asset.model['materials'] = [material]
    asset.model['extensionsUsed'] = ['KHR_materials_unlit']
    if transmission:
        asset.model['extensionsUsed'].append('KHR_materials_transmission')
    if animated and kind == 'source':
        primitive['attributes'].pop('NORMAL')
        primitive['targets'] = [dict(POSITION=asset.accessor('VEC3', f32(delta)))]
        primitive['attributes']['JOINTS_0'] = asset.accessor('VEC4', [[0, 0, 0, 0]]*4, 5121)
        primitive['attributes']['WEIGHTS_0'] = asset.accessor('VEC4', [[65535, 0, 0, 0]]*4, 5123, normalize=True)
        asset.model['nodes'][0].update(skin=0, translation=[2, 0, 0])
        asset.model['nodes'].append(dict(name='Alpha skin joint'))
        asset.model['scenes'][0]['nodes'].append(1)
        asset.model['skins'] = [dict(joints=[1], inverseBindMatrices=asset.accessor('MAT4', [np.eye(4).T.ravel().tolist()]))]
        asset.model['meshes'][0]['weights'] = [0.]
        times = asset.accessor('SCALAR', [0., 1.])
        weights = asset.accessor('SCALAR', [0., .8])
        translation = asset.accessor('VEC3', [[0., 0., 0.], [.15, .1, 0.]])
        asset.model['animations'] = [dict(name='RGBA frame fallback then skin', samplers=[
            dict(input=times, output=weights, interpolation='LINEAR'), dict(input=times, output=translation, interpolation='LINEAR')],
            channels=[dict(sampler=0, target=dict(node=0, path='weights')), dict(sampler=1, target=dict(node=1, path='translation'))])]
    elif animated:
        primitive['attributes'].pop('NORMAL')
    add_receiver(asset, shadow=shadow)
    if clear:
        asset.model['scenes'][0]['nodes'] = [len(asset.model['nodes'])-1]
    asset.write(path)
    return dict(animated=animated and kind == 'source', mode=mode, colorAccessor=color)


def plane_points(record, z):
    # Independent inverse projection of pixel centres, intersecting a known
    # world plane. No renderer-provided coverage/picking data is used.
    matrix = np.linalg.inv(np.asarray(record['state']['camera']['unjitteredViewProjColumns']).T)
    yy, xx = np.mgrid[:360, :640]
    clip = np.column_stack((((xx.ravel()+.5)/640)*2-1, 1-((yy.ravel()+.5)/360)*2,
                            np.zeros(640*360), np.ones(640*360)))
    far = clip @ matrix.T
    far = far[:, :3]/far[:, 3:4]
    camera = np.asarray(record['state']['camera']['position'])
    direction = far-camera
    t = (z-camera[2])/direction[:, 2]
    return camera+direction*t[:, None]


def validate_alpha_fixture(files, case):
    source, _ = load_asset(files['source'])
    reference, _ = load_asset(files['reference'])
    left = source['meshes'][0]['primitives'][0]
    right = reference['meshes'][0]['primitives'][0]
    alpha = decode_accessor(files['source'], left['attributes']['COLOR_0'])[:, 3]
    uv = decode_accessor(files['reference'], right['attributes']['TEXCOORD_0'])
    # Bilinear x = u*width-.5, with alpha texels [0,128/255].
    texture_alpha = (uv[:, 0]*2-.5)*np.float32(ALPHA_TEXTURE)
    if case == 'float-opaque' or case == 'opaque-transmission-zero':
        require(np.all(alpha == 0), 'Opaque control must author zero vertex alpha')
    elif case == 'opaque-transmission-gradient':
        require(np.array_equal(alpha, UV[:, 0]), 'Opaque glass must retain its authored vertex alpha ramp')
    else:
        require(np.max(np.abs(alpha*np.float32(ALPHA_TEXTURE)-texture_alpha)) < 1e-7,
                'Reference alpha texture does not reproduce source corner alpha')
        for triangle in ([0, 1, 2], [0, 2, 3]):
            bary = np.array([.2, .35, .45])
            actual = (alpha[triangle] @ bary)*ALPHA_TEXTURE*MATERIAL_ALPHA
            ref_u = uv[triangle, 0] @ bary
            expected = (ref_u*2-.5)*ALPHA_TEXTURE*MATERIAL_ALPHA
            require(abs(actual-expected) < 1e-7, 'Interior alpha interpolation oracle is inconsistent')


def check_mask_or_opaque(record, opaque):
    point = plane_points(record, .2)
    image = pixels(record).reshape((-1, 3))
    interior = (np.abs(point[:, 0]) < .84) & (np.abs(point[:, 1]) < .74)
    red = image[:, 0] > image[:, 2]+30
    blue = image[:, 2] > image[:, 0]+30
    left, right = interior & (point[:, 0] < -.06), interior & (point[:, 0] > .06)
    require(left.sum() > 1000 and right.sum() > 1000, 'Alpha oracle projects too few interior pixels')
    left_correct, right_correct = float(np.mean(red[left] if opaque else blue[left])), float(np.mean(red[right]))
    require(left_correct > .995 and right_correct > .995,
            f'Wrong analytic {"OPAQUE" if opaque else "MASK"} coverage: {left_correct}, {right_correct}')
    return dict(leftPixels=int(left.sum()), rightPixels=int(right.sum()), leftCorrectFraction=left_correct, rightCorrectFraction=right_correct)


def shadow_metrics(actual, clear):
    point = plane_points(actual, 0)
    # Incoming sun direction is (-.7,0,-1). At caster z=.7, a receiver
    # visibility ray reaches x+0.49. A gradient MASK passes only caster x>=0.
    # Pixel rays see through the caster's x<0 half, exposing these receivers.
    blocked = (point[:, 0] > -.43) & (point[:, 0] < -.07) & (np.abs(point[:, 1]) < .65)
    lit = (point[:, 0] > -.83) & (point[:, 0] < -.61) & (np.abs(point[:, 1]) < .65)
    require(blocked.sum() > 500 and lit.sum() > 500, 'Shadow alpha oracle lacks visible receiver samples')
    image, baseline = pixels(actual).reshape((-1, 3)), pixels(clear).reshape((-1, 3))
    darkening = (baseline-image).mean(axis=1)
    blocked_darkening = float(np.median(darkening[blocked]))
    lit_error = float(np.quantile(np.abs(darkening[lit]), .95))
    require(blocked_darkening > 8 and lit_error < 4,
            f'Incorrect alpha shadow receiver: darkening={blocked_darkening}, litErrorP95={lit_error}')
    return dict(shadowPixels=int(blocked.sum()), litPixels=int(lit.sum()),
                medianShadowDarkening=blocked_darkening, litErrorP95=lit_error)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--suite', action='append', choices=('coverage', 'shadows'))
    parser.add_argument('--case', action='append', choices=CASES)
    parser.add_argument('--technique', action='append', choices=TECHNIQUES)
    parser.add_argument('--shadow-mode', action='append', choices=('raster', 'hard'))
    parser.add_argument('--roundtrip', action='store_true')
    parser.add_argument('--generate-only', action='store_true')
    parser.add_argument('--timeout', type=float, default=180)
    args = parser.parse_args()
    run = NativeRun(args.renderer, args.output, args.timeout)
    run.report['mode'] = 'generate-only' if args.generate_only else 'native'
    try:
        if 'coverage' in (args.suite or ('coverage', 'shadows')):
            for case in args.case or CASES:
                files = {kind: run.output/f'{case}-{kind}.gltf' for kind in ('source', 'reference', 'dropped')}
                info = {kind: write_alpha_case(path, case, kind=kind) for kind, path in files.items()}
                validate_alpha_fixture(files, case)
                transmission = case.startswith('opaque-transmission-')
                control_file = None
                if transmission:
                    mode = 'MASK' if case.endswith('zero') else 'BLEND'
                    control_file = run.output/f'{case}-{mode.lower()}-reference.gltf'
                    write_alpha_case(control_file, case, kind='reference', mode_override=mode)
                    files['controlReference'] = control_file
                run.report['fixtures'].append(dict(suite='coverage', case=case, paths={key: str(path) for key, path in files.items()}))
                if args.generate_only:
                    continue
                for technique in args.technique or TECHNIQUES:
                    stem = technique+'-'+case
                    destination = run.output/(stem+'-saved.glb') if args.roundtrip else None
                    actual = run.capture(files['source'], stem+'-source', technique, animated=info['source']['animated'], destination=destination, lit=transmission)
                    reference = run.capture(files['reference'], stem+'-reference', technique, lit=transmission)
                    dropped = run.capture(files['dropped'], stem+'-dropped', technique, lit=transmission)
                    # Hardware subtexel interpolation may quantize the texture
                    # ramp more coarsely than the raster's vertex interpolator.
                    result = dict(case=case, suite='coverage', technique=technique, check='independent-alpha-texture', **compare(actual, reference, tolerance=.15))
                    if case.endswith('mask') or case == 'float-opaque':
                        result['analyticCoverage'] = check_mask_or_opaque(actual, case == 'float-opaque')
                    run.report['checks'].append(result)
                    run.report['checks'].append(dict(case=case, suite='coverage', technique=technique, check='alpha-negative-control', **compare(actual, dropped, exact=False)))
                    if transmission:
                        mode = 'MASK' if case.endswith('zero') else 'BLEND'
                        control = run.capture(control_file, stem+'-'+mode.lower()+'-reference', technique, lit=True)
                        run.report['checks'].append(dict(case=case, suite='coverage', technique=technique,
                            check='transmission-'+mode.lower()+'-retains-alpha', **compare(dropped, control, tolerance=.15)))
                    if destination:
                        original = source_accessor_signature(files['source'])
                        require(source_accessor_signature(destination)[:len(original)] == original,
                                'Export altered original RGBA accessor or encoded source bytes')
                        reloaded = run.capture(destination, stem+'-reload', technique, animated=info['source']['animated'], lit=transmission)
                        run.report['checks'].append(dict(case=case, suite='coverage', technique=technique, check='rgba-export-reload', **compare(actual, reloaded)))
                    run.save()
                    print('PASS coverage', case, technique, flush=True)
        if 'shadows' in (args.suite or ('coverage', 'shadows')):
            files = {kind: run.output/f'alpha-shadow-{kind}.gltf' for kind in ('source', 'reference', 'clear')}
            for kind, path in files.items():
                write_alpha_case(path, 'u16-mask', kind='source' if kind == 'clear' else kind, shadow=True, clear=kind=='clear')
            run.report['fixtures'].append(dict(suite='shadows', paths={key: str(path) for key, path in files.items()}))
            if not args.generate_only:
                for mode in args.shadow_mode or ('raster', 'hard'):
                    for technique in args.technique or TECHNIQUES:
                        extra = ['--directional-intensity', '3', '--directional-direction', '-.7', '0', '-1',
                                 '--ray-shadows', mode, '--no-ray-shadow-denoise']
                        if mode == 'hard':
                            # Keep a warmed active field while disabling its
                            # lighting contribution to the direct-shadow oracle.
                            extra += ['--ddgi', '--ddgi-intensity', '0', '--ddgi-fit-scene', '--ddgi-fit-probes', '64',
                                      '--ddgi-rays', '32', '--ddgi-probes-per-frame', '64', '--ddgi-ray-budget', '2048', '--ddgi-hysteresis', '0']
                        captures = {kind: run.capture(path, f'{technique}-shadow-{mode}-{kind}', technique, extra=extra, lit=True)
                                    for kind, path in files.items()}
                        actual = captures['source']
                        state = actual['state']
                        if mode == 'hard':
                            require(state['rayShadows']['supported'] and state['rayShadows']['active'], 'Hard alpha ray shadows inactive')
                            require(state['ddgi']['supported'] and state['ddgi']['active'] and state['ddgi']['tracedRays'] > 0,
                                    'DDGI alpha candidate traversal did not execute')
                        else:
                            require(not state['rayShadows']['active'] and
                                    any(p['active'] and p['name'].startswith('ShadowCascade') for p in state['passes']),
                                    'Raster alpha shadow passes inactive')
                        run.report['checks'].append(dict(suite='shadows', mode=mode, technique=technique,
                            check='independent-alpha-texture', **compare(actual, captures['reference'], tolerance=.15),
                            receiver=shadow_metrics(actual, captures['clear']), ddgiTracedRays=state['ddgi']['tracedRays']))
                        run.save()
                        print('PASS shadows', mode, technique, flush=True)
        run.finish()
    except Exception as error:
        run.report['failure'] = str(error)
        raise
    finally:
        run.save()
    print(f"PASS: {len(run.report['checks'])} checks, {len(run.report['captures'])} captures; {run.output}")


if __name__ == '__main__':
    main()
