"""Native vertex RGBA coverage checks with independent alpha-texture oracles.

The source linearly interpolates corner alpha 0..1. The reference has RGB-only
vertices and samples a two-texel alpha ramp at u=.25+.5*original_u, reproducing
the same alpha exactly under bilinear interpolation. Layered blue receivers
expose incorrect MASK depth writes. Shadow receivers have independently chosen
blocked/lit regions. No production evaluator or shader helper supplies oracles.

The opt-in cutoff suite checks constant MASK coverage in the flat interior.
--generate-only launches no renderer. Requires NumPy, Pillow and Vulkan for runs.
"""
from __future__ import annotations

import argparse
import copy
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image

from gltf_accessor_runtime_regression import (Asset, NativeRun, TECHNIQUES,
    compare, decode_accessor, f32, load_asset, mesh, pixels, require, source_accessor_signature)

CASES = ('float-mask', 'u8-blend', 'u16-mask', 'float-opaque', 'deform-u16-blend',
         'opaque-transmission-zero', 'opaque-transmission-gradient')
FRONT = np.array([[-.9, -.8, .2], [.9, -.8, .2], [.9, .8, .2], [-.9, .8, .2]])
UV = np.array([[0., 1.], [1., 1.], [1., 0.], [0., 0.]])
ALPHA_TEXTURE = 128 / 255
MATERIAL_ALPHA = .75
CUTOFF = ALPHA_TEXTURE * MATERIAL_ALPHA * .5
CUTOFF_CASES = (('below', .49, .5), ('equal', .5, .5), ('above', .51, .5), ('zero-equal', 0., 0.))
CUTOFF_PIXEL = (129, 128)  # The old Bayer test rejected equality at this parity.
CUTOFF_EDGE_PIXEL = (128, 131)  # Highest Bayer threshold in the transition column.
CUTOFF_ROI = np.s_[112:144, 112:144]
CUTOFF_ARGS = ('--width', '256', '--height', '256', '--camera-fov', '45', '--import-scale', '1',
               '--sky', 'plain', '--sky-horizon', '0', '0', '0', '--screenshot-viewport')


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


def write_cutoff_fixture(path, alpha, cutoff, *, shadow=None, transmission=False, clear=False):
    asset = Asset()
    asset.model['nodes'][0]['name'] = 'Constant alpha panel'
    mesh(asset, asset.accessor('VEC3', [[-1, -1, 0], [1, -1, 0], [1, 1, 0], [-1, 1, 0]]),
         asset.accessor('VEC3', [[0, 0, 1]] * 4), asset.accessor('VEC2', UV))
    asset.model['materials'] = [dict(doubleSided=True, alphaMode='MASK', alphaCutoff=cutoff,
        pbrMetallicRoughness=dict(baseColorFactor=[0, 1, 0, alpha], metallicFactor=0, roughnessFactor=1),
        extensions=dict(KHR_materials_unlit={}))]
    asset.model['extensionsUsed'] = ['KHR_materials_unlit']
    if shadow:
        # A receiver ray from the center toward the light meets the panel
        # center, while the camera's center ray sees only the receiver.
        asset.model['nodes'][0]['translation'] = [1.35, 0, 1]
        add_receiver(asset, shadow=True)
        if shadow == 'point':
            light = 'KHR_lights_punctual'
            asset.model['extensionsUsed'].append(light)
            asset.model['extensions'] = {light: dict(lights=[dict(type='point', intensity=60, range=12)])}
            asset.model['nodes'].append(dict(name='Local cutoff shadow light', translation=[2.7, 0, 2],
                                             extensions={light: dict(light=0)}))
            asset.model['scenes'][0]['nodes'].append(len(asset.model['nodes']) - 1)
    if transmission:
        material = asset.model['materials'][0]
        material['extensions'] = dict(KHR_materials_transmission=dict(transmissionFactor=.5))
        asset.model['extensionsUsed'].append('KHR_materials_transmission')
        add_receiver(asset)
        if clear:
            asset.model['scenes'][0]['nodes'].remove(0)
    asset.write(path)


def write_cutoff_edge_fixture(path, source):
    asset = Asset()
    half_view = 3 * math.tan(math.radians(22.5))
    # Aim the threshold at the center of column 128. A gradient has a
    # nonzero derivative here and should keep mixed Bayer coverage down it.
    cutoff = .5 + half_view / (2 * 256)
    uv = np.column_stack((.25 + .5 * UV[:, 0], UV[:, 1])) if source == 'texture' else UV
    color = asset.accessor('VEC4', np.column_stack((np.ones((4, 3)), UV[:, 0]))) if source == 'vertex' else None
    mesh(asset, asset.accessor('VEC3', [[-1, -1, 0], [1, -1, 0], [1, 1, 0], [-1, 1, 0]]),
         asset.accessor('VEC3', [[0, 0, 1]] * 4), asset.accessor('VEC2', uv), colors=color)
    pbr = dict(baseColorFactor=[0, 1, 0, 1], metallicFactor=0, roughnessFactor=1)
    if source == 'texture':
        texels = np.tile([[255, 255, 255, 0], [255, 255, 255, 255]], (2, 1, 1))
        pbr['baseColorTexture'] = dict(index=asset.image(texels))
    asset.model['materials'] = [dict(doubleSided=True, alphaMode='MASK', alphaCutoff=cutoff,
        pbrMetallicRoughness=pbr, extensions=dict(KHR_materials_unlit={}))]
    asset.model['extensionsUsed'] = ['KHR_materials_unlit']
    asset.write(path)


def cutoff_roi(record):
    image = np.asarray(Image.open(record['image']).convert('RGB'), dtype=np.int16)
    require(image.shape == (256, 256, 3), f"Wrong cutoff capture extent: {record['image']}")
    return image[CUTOFF_ROI]


def green_pixels(roi):
    r, g, b = np.moveaxis(roi, -1, 0)
    return int(((g > 128) & (g - np.maximum(r, b) > 64)).sum())


def cutoff_depth(state, z):
    if z is None:
        return 0.
    clip = np.asarray(state['camera']['unjitteredViewProjColumns']).T @ [0, 0, z, 1]
    return float(clip[2] / clip[3])


def check_cutoff_probe(record, pixel, *, opaque_z, hit_node, hit_z):
    state = record['state']
    probe = state['depthProbe']
    require(state['msaaSamples'] == 1 and tuple(probe['pixel']) == pixel, 'Wrong cutoff probe configuration')
    require({'DepthPrepass', 'TransparentPick'} <= {p['name'] for p in state['passes'] if p['active']},
            'Depth and picking passes must both execute')
    expected_depth = cutoff_depth(state, opaque_z)
    depth = probe['opaque']
    gpu = probe['sceneInteraction']['gpu']
    pick_id = probe['sceneInteraction']['rawGpuId']
    hit = gpu['hit'] if gpu else None
    pick_ok = (pick_id is not None and pick_id > 0 and hit is not None and hit['sourceNode'] == hit_node
               if hit_node is not None else pick_id == 0 and gpu is not None and hit is None)
    depth_ok = depth is not None and abs(depth - expected_depth) < 2e-6
    if probe['pick'] is not None or hit_z != opaque_z:
        depth_ok &= probe['pick'] is not None and abs(probe['pick'] - cutoff_depth(state, hit_z)) < 2e-6
    return dict(opaqueDepth=depth, expectedDepth=expected_depth, pickDepth=probe['pick'], rawGpuId=pick_id,
                gpuHit=hit, depthOk=depth_ok, pickOk=pick_ok)


def cutoff_sequence(path, pixel):
    path.write_text(json.dumps(dict(schemaVersion=1, frames=4, sampleFrames=[4],
        events=[dict(frame=4, depthProbe=pixel)])), encoding='utf-8')


def run_cutoff_suite(run, techniques, *, generate_only, color_only):
    models = {}
    sequence = run.output / 'cutoff-depth.json'
    cutoff_sequence(sequence, CUTOFF_PIXEL)
    for label, alpha, cutoff in CUTOFF_CASES:
        model = run.output / f'cutoff-{label}.gltf'
        write_cutoff_fixture(model, alpha, cutoff)
        models[label] = model
        run.report['fixtures'].append(dict(suite='cutoff', case=label, alpha=alpha, cutoff=cutoff, path=str(model)))
    if not color_only:
        motion = run.output / 'cutoff-motion.json'
        motion.write_text(json.dumps(dict(schemaVersion=1, frames=4, sampleFrames=[4], objectName='Constant alpha panel',
            objectTranslation=[dict(frame=3, translation=[0, 0, 0]), dict(frame=4, translation=[.08, 0, 0])])),
            encoding='utf-8')
        shadow_models = {}
        for light in ('directional', 'point'):
            shadow_models[light] = {}
            for label, alpha, cutoff in CUTOFF_CASES[:3]:
                model = run.output / f'cutoff-shadow-{light}-{label}.gltf'
                write_cutoff_fixture(model, alpha, cutoff, shadow=light)
                shadow_models[light][label] = model
            run.report['fixtures'].append(dict(suite='cutoff', case=f'{light}-shadow',
                                                paths={k: str(v) for k, v in shadow_models[light].items()}))
        transmission_models = {}
        for label, alpha in (('equal', 0.), ('above', 1.), ('clear', 0.)):
            model = run.output / f'cutoff-transmission-{label}.gltf'
            write_cutoff_fixture(model, alpha, 0., transmission=True, clear=label == 'clear')
            transmission_models[label] = model
        run.report['fixtures'].append(dict(suite='cutoff', case='transmission',
                                            paths={k: str(v) for k, v in transmission_models.items()}))
        edge_models = {}
        edge_sequence = run.output / 'cutoff-edge-depth.json'
        cutoff_sequence(edge_sequence, CUTOFF_EDGE_PIXEL)
        for source in ('vertex', 'texture'):
            edge_models[source] = run.output / f'cutoff-edge-{source}.gltf'
            write_cutoff_edge_fixture(edge_models[source], source)
        run.report['fixtures'].append(dict(suite='cutoff', case='edge', paths={k: str(v) for k, v in edge_models.items()}))
    if generate_only:
        return
    failures = []
    for technique in techniques:
        for label, alpha, cutoff in CUTOFF_CASES:
            record = run.capture(models[label], f'{technique}-cutoff-{label}', technique,
                                 extra=(*CUTOFF_ARGS, '--capture-sequence', str(sequence)))
            roi = cutoff_roi(record)
            opaque = alpha >= cutoff
            z = 0 if opaque else None
            count = green_pixels(roi)
            expected = 1024 if opaque else 0
            check = check_cutoff_probe(record, CUTOFF_PIXEL, opaque_z=z, hit_node=0 if opaque else None, hit_z=z)
            check.update(greenPixels=count, expectedGreenPixels=expected,
                         colorOk=count == expected and (opaque or int(roi.max()) < 8))
            run.report['checks'].append(dict(suite='cutoff', check='color-depth-picking', technique=technique,
                                              case=label, alpha=alpha, cutoff=cutoff, **check))
            run.save()
            if not (check['colorOk'] and check['depthOk'] and check['pickOk']):
                failures.append(f"{technique} {label}: {check['greenPixels']}/{check['expectedGreenPixels']} green pixels, "
                                f"depth={check['opaqueDepth']}, pick={check['rawGpuId']}")
    require(not failures, '\n'.join(failures))
    print('PASS constant cutoff color, depth and GPU picking', flush=True)
    if color_only:
        return

    for label, alpha, cutoff in CUTOFF_CASES:
        ray = run.capture(models[label], f'path-tracing-cutoff-{label}', 'path-tracing', extra=(
            *CUTOFF_ARGS, '--path-spp', '4', '--path-samples', '4', '--path-bounces', '1'))
        count = green_pixels(cutoff_roi(ray))
        expected = 1024 if alpha >= cutoff else 0
        reference = ray['state']['referencePath']
        require(count == expected and reference['attemptedSamples'] == 4 and
                reference['unsupportedSamples'] == reference['rejectedSamples'] == 0,
                f'Incorrect ray cutoff coverage: {label}, {count} green pixels, {reference}')
        run.report['checks'].append(dict(suite='cutoff', check='ray-interior', case=label,
                                        greenPixels=count, expectedGreenPixels=expected))

    for technique in techniques:
        velocity = {}
        for label, alpha, cutoff in CUTOFF_CASES:
            record = run.capture(models[label], f'{technique}-cutoff-motion-{label}', technique,
                                 extra=(*CUTOFF_ARGS, '--taa', '--display-mode', 'taa-velocity',
                                        '--capture-sequence', str(motion)))
            require(any(p['active'] and p['name'] == 'TemporalVelocity' for p in record['state']['passes']),
                    f'{technique} velocity pass did not run')
            roi = velocity[label] = cutoff_roi(record)
            valid = np.any(roi != [255, 0, 255], axis=-1)
            moved = valid & (np.abs(roi[:, :, 0] - 188) > 20)
            expected = 1024 if alpha >= cutoff else 0
            require(int(valid.sum()) == int(moved.sum()) == expected,
                    f'{technique} {label}: wrong velocity coverage or motion ({valid.sum()}/{moved.sum()})')
            run.report['checks'].append(dict(suite='cutoff', check='motion-vector', technique=technique,
                                            case=label, validPixels=int(valid.sum()), movedPixels=int(moved.sum())))
        require(np.array_equal(velocity['equal'], velocity['above']) and np.array_equal(velocity['zero-equal'], velocity['above']),
                f'{technique} equality motion vectors differ from above-cutoff reference')

        for light, casters in shadow_models.items():
            shadows = {}
            shadow_pass = 'ShadowCascade' if light == 'directional' else 'LocalShadowDepth'
            light_args = ('--directional-direction', '-1.35', '0', '-1') if light == 'directional' else ()
            for label, model in casters.items():
                record = run.capture(model, f'{technique}-cutoff-shadow-{light}-{label}', technique,
                                     lit=light == 'directional', extra=(
                                         *CUTOFF_ARGS, *light_args, '--bounce-intensity', '0', '--ray-shadows', 'raster'))
                state = record['state']
                require(not state['rayShadows']['active'] and
                        any(p['active'] and p['name'].startswith(shadow_pass) for p in state['passes']),
                        f'{technique} {light} raster shadow pass did not run')
                if light == 'point':
                    require(state['lighting']['pointLightCount'] == 1 and state['lighting']['activeLocalShadowLayers'] == 6,
                            'Point cutoff shadow fixture did not allocate six cube faces')
                shadows[label] = cutoff_roi(record)
            lit_mean = float(shadows['below'].mean())
            shadow_mean = float(shadows['above'].mean())
            equal_mean = float(shadows['equal'].mean())
            require(lit_mean - shadow_mean > 20 and np.array_equal(shadows['equal'], shadows['above']),
                    f'{technique} {light} cutoff shadow: below={lit_mean}, equal={equal_mean}, above={shadow_mean}')
            run.report['checks'].append(dict(suite='cutoff', check='raster-shadow', technique=technique, light=light,
                                            belowMean=lit_mean, equalMean=equal_mean, aboveMean=shadow_mean))

        transmission = {}
        for label, model in transmission_models.items():
            record = run.capture(model, f'{technique}-cutoff-transmission-{label}', technique, lit=True,
                                 extra=(*CUTOFF_ARGS, '--directional-direction', '0', '0', '-1',
                                        '--capture-sequence', str(sequence)))
            clear = label == 'clear'
            if not clear:
                require(any(p['active'] and p['name'] == 'OitResolve' for p in record['state']['passes']),
                        f'{technique} MASK transmission did not exercise OIT')
            check = check_cutoff_probe(record, CUTOFF_PIXEL, opaque_z=-.4, hit_node=1 if clear else 0,
                                       hit_z=-.4 if clear else 0)
            require(check['depthOk'] and check['pickOk'], f'{technique} {label} MASK transmission probe: {check}')
            transmission[label] = cutoff_roi(record)
            run.report['checks'].append(dict(suite='cutoff', check='transmission-depth-picking', technique=technique,
                                            case=label, **check))
        receiver_change = float(np.abs(transmission['above'] - transmission['clear']).mean())
        require(receiver_change > 20 and np.array_equal(transmission['equal'], transmission['above']),
                f'{technique} zero-alpha MASK transmission disagrees with its covered reference (contrast {receiver_change})')
        run.report['checks'].append(dict(suite='cutoff', check='transmission-oit', technique=technique,
                                        receiverMeanAbsoluteDifference=receiver_change))

        for source, model in edge_models.items():
            record = run.capture(model, f'{technique}-cutoff-edge-{source}', technique,
                                 extra=(*CUTOFF_ARGS, '--capture-sequence', str(edge_sequence)))
            roi = cutoff_roi(record)
            left, edge, right = (green_pixels(roi[:, x:x+1]) for x in (14, 16, 18))
            require(left == 0 and right == 32 and 0 < edge < 32,
                    f'{technique} {source} alpha lost edge antialiasing: {left}/{edge}/{right}')
            x, y = (coordinate - 112 for coordinate in CUTOFF_EDGE_PIXEL)
            visible = green_pixels(roi[y:y+1, x:x+1]) == 1
            check = check_cutoff_probe(record, CUTOFF_EDGE_PIXEL, opaque_z=None, hit_node=None, hit_z=None)
            require(not visible and check['depthOk'] and check['pickOk'],
                    f'{technique} {source} dithered-out edge disagrees across color/depth/picking: {check}')
            run.report['checks'].append(dict(suite='cutoff', check='gradient-edge-aa', technique=technique,
                                            source=source, left=left, edge=edge, right=right, **check))
        run.save()
    print('PASS cutoff ray interiors, motion vectors, raster shadows, OIT and gradient edges', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--suite', action='append', choices=('coverage', 'shadows', 'cutoff'))
    parser.add_argument('--cutoff-color-only', action='store_true', help='Run only color, depth and picking in the cutoff suite')
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
        if 'cutoff' in (args.suite or ()):
            run_cutoff_suite(run, args.technique or TECHNIQUES,
                             generate_only=args.generate_only, color_only=args.cutoff_color_only)
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
