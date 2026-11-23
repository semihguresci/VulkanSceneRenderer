"""Native vertex RGBA coverage checks with independent alpha-texture oracles.

The source linearly interpolates corner alpha 0..1. The reference has RGB-only
vertices and samples a two-texel alpha ramp at u=.25+.5*original_u, reproducing
the same alpha exactly under bilinear interpolation. Layered blue receivers
expose incorrect MASK depth writes. Shadow receivers have independently chosen
blocked/lit regions. No production evaluator or shader helper supplies oracles.

The opt-in cutoff suite checks constant MASK coverage in the flat interior.
The authoring-shadows suite checks live alpha edits, history and portable reload
against independently authored directional and point-light shadow controls.
--generate-only launches no renderer. Requires NumPy, Pillow and Vulkan for runs.
"""
from __future__ import annotations

import argparse
import copy
import json
import math
import os
from pathlib import Path
import struct
import tempfile

import numpy as np
from PIL import Image

from gltf_accessor_runtime_regression import (Asset, NativeRun, TECHNIQUES,
    compare, decode_accessor, f32, finite_json, load_asset, mesh, pixels, require, source_accessor_signature)

CASES = ('float-mask', 'u8-blend', 'u16-mask', 'float-opaque', 'deform-u16-blend',
         'opaque-transmission-zero', 'opaque-transmission-gradient')
FRONT = np.array([[-.9, -.8, .2], [.9, -.8, .2], [.9, .8, .2], [-.9, .8, .2]])
UV = np.array([[0., 1.], [1., 1.], [1., 0.], [0., 0.]])
ALPHA_TEXTURE = 128 / 255
MATERIAL_ALPHA = .75
CUTOFF = ALPHA_TEXTURE * MATERIAL_ALPHA * .5
CUTOFF_CASES = (('below', .49, .5), ('equal', .5, .5), ('above', .51, .5), ('zero-equal', 0., 0.))
POINT_CUTOFF_CASES = CUTOFF_CASES[:3] + (
    ('one-ulp-below', float(np.nextafter(np.float32(.5), np.float32(0))), .5),
    ('one-ulp-above', float(np.nextafter(np.float32(.5), np.float32(1))), .5),
)
CUTOFF_PIXEL = (129, 128)  # The old Bayer test rejected equality at this parity.
CUTOFF_EDGE_PIXEL = (128, 131)  # Highest Bayer threshold in the transition column.
CUTOFF_ROI = np.s_[112:144, 112:144]
CUTOFF_ARGS = ('--width', '256', '--height', '256', '--camera-fov', '45', '--import-scale', '1',
               '--sky', 'plain', '--sky-horizon', '0', '0', '0', '--screenshot-viewport')
AUTHORING_SHADOW_CONTROLS = {'covered': ('MASK', .3), 'discarded': ('MASK', .6),
                             'opaque': ('OPAQUE', .6), 'blend': ('BLEND', .6)}
AUTHORING_SHADOW_STEPS = (
    ('initial', 'covered', (0, 0)), ('discarded', 'discarded', (1, 0)),
    ('undo-cutoff', 'covered', (0, 1)), ('redo-cutoff', 'discarded', (1, 0)),
    ('opaque', 'opaque', (2, 0)), ('undo-opaque', 'discarded', (1, 1)),
    ('redo-opaque', 'opaque', (2, 0)), ('blend', 'blend', (3, 0)),
    ('undo-blend', 'opaque', (2, 1)), ('redo-blend', 'blend', (3, 0)),
    ('final-mask', 'discarded', (4, 0)),
)


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


def write_cutoff_fixture(path, alpha, cutoff, *, shadow=None, transmission=False, clear=False,
                         textured_vertex_alpha=None):
    asset = Asset()
    asset.model['nodes'][0]['name'] = 'Constant alpha panel'
    colors = (asset.accessor('VEC4', [[1, 1, 1, textured_vertex_alpha]] * 4)
              if textured_vertex_alpha is not None else None)
    mesh(asset, asset.accessor('VEC3', [[-1, -1, 0], [1, -1, 0], [1, 1, 0], [-1, 1, 0]]),
         asset.accessor('VEC3', [[0, 0, 1]] * 4), asset.accessor('VEC2', UV), colors=colors)
    asset.model['materials'] = [dict(doubleSided=True, alphaMode='MASK', alphaCutoff=cutoff,
        pbrMetallicRoughness=dict(baseColorFactor=[0, 1, 0, alpha], metallicFactor=0, roughnessFactor=1),
        extensions=dict(KHR_materials_unlit={}))]
    asset.model['extensionsUsed'] = ['KHR_materials_unlit']
    if textured_vertex_alpha is not None:
        texture = asset.image(np.full((2, 2, 4), 255, dtype=np.uint8))
        asset.model['materials'][0]['pbrMetallicRoughness']['baseColorTexture'] = dict(index=texture)
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
            cases = POINT_CUTOFF_CASES if light == 'point' else CUTOFF_CASES[:3]
            for label, alpha, cutoff in cases:
                model = run.output / f'cutoff-shadow-{light}-{label}.gltf'
                write_cutoff_fixture(model, alpha, cutoff, shadow=light)
                shadow_models[light][label] = model
                if light == 'point' and label in ('equal', 'one-ulp-below', 'one-ulp-above'):
                    textured = f'textured-vertex-{label}'
                    model = run.output / f'cutoff-shadow-{light}-{textured}.gltf'
                    write_cutoff_fixture(model, 1., cutoff, shadow=light, textured_vertex_alpha=alpha)
                    shadow_models[light][textured] = model
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
            check = dict(suite='cutoff', check='raster-shadow', technique=technique, light=light,
                         belowMean=lit_mean, equalMean=equal_mean, aboveMean=shadow_mean)
            if light == 'point':
                for reference in ('below', 'above'):
                    adjacent = shadows[f'one-ulp-{reference}']
                    require(np.array_equal(adjacent, shadows[reference]),
                            f'{technique} point shadow one ULP {reference} the cutoff differs from its control')
                check.update(oneUlpBelowMean=float(shadows['one-ulp-below'].mean()),
                             oneUlpAboveMean=float(shadows['one-ulp-above'].mean()))
                for reference in ('equal', 'one-ulp-below', 'one-ulp-above'):
                    textured = shadows[f'textured-vertex-{reference}']
                    require(np.array_equal(textured, shadows[reference]),
                            f'{technique} textured vertex point shadow differs from untextured {reference} control')
                check.update(texturedVertexEqualMean=float(shadows['textured-vertex-equal'].mean()),
                             texturedVertexOneUlpBelowMean=float(shadows['textured-vertex-one-ulp-below'].mean()),
                             texturedVertexOneUlpAboveMean=float(shadows['textured-vertex-one-ulp-above'].mean()))
            run.report['checks'].append(check)

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


def authoring_shadow_pixels(actual, reference):
    images = [np.asarray(Image.open(record['image']).convert('RGB'), dtype=np.int16)
              for record in (actual, reference)]
    require(all(image.shape == (256, 256, 3) for image in images), 'Wrong authoring shadow capture extent')
    delta = np.abs(images[0] - images[1])
    error = float(delta.mean())
    require(error <= .03, f'Live alpha/shadow image differs from its literal control: {error}')
    return dict(meanAbsolutePixelError=error, receiverMean=float(images[0][CUTOFF_ROI].mean()))


def authoring_shadow_state(state, control, light, history, publications, *, baseline=None):
    from scene_node_authoring_runtime_regression import history_cursor

    mode, cutoff = AUTHORING_SHADOW_CONTROLS[control]
    require(finite_json(state) and state['resolution'] == [256, 256] and
            state['msaaSamples'] == 1 and not state['taa']['enabled'] and
            state['scene']['primaryObjectCount'] == 2, 'Wrong alpha authoring scene or render configuration')
    history_cursor(state, *history, allow_uncreated=publications == 0)
    active = {item['name'] for item in state['passes'] if item['active']}
    prefix = 'ShadowCascade' if light == 'directional' else 'LocalShadowDepth'
    require(not state['rayShadows']['active'] and any(name.startswith(prefix) for name in active),
            f'Missing {light} raster shadow pass')
    if light == 'point':
        require(state['lighting']['pointLightCount'] == 1 and state['lighting']['activeLocalShadowLayers'] == 6,
                'Point authoring fixture needs six active shadow cube faces')
    require(('OitResolve' in active) == (mode == 'BLEND'), 'Alpha authoring did not update transparent lighting')
    assignments = {row['mesh']: row for row in state['gltfMaterialVariants']['assignments']}
    require(set(assignments) == {0, 1}, 'Missing caster or independent receiver assignment')
    caster, receiver = assignments[0], assignments[1]
    require(caster['transparent'] == (mode == 'BLEND') and caster['alphaMasked'] == (mode == 'MASK') and
            caster['doubleSided'] and not receiver['transparent'] and not receiver['alphaMasked'] and
            not receiver['doubleSided'], 'Alpha edit changed the wrong material route')
    authoring = state['gltfMaterialAuthoring']
    require(authoring['publications'] == publications and not authoring['pendingGesture'],
            'Wrong alpha material publication count or unfinished gesture')
    if publications:
        require(authoring['sourceMaterial'] == 0 and authoring['runtimeMaterial'] == caster['material'] and
                authoring['uploadedMaterialHash'] > 0, 'Wrong published source/runtime alpha material')
        for record in (authoring['authored'], authoring['evaluated']):
            require(record['alphaMode'] == mode and abs(record['alphaCutoff'] - cutoff) < 1e-6 and
                    record['doubleSided'] and record['extensions'].get('KHR_materials_unlit') == {} and
                    np.allclose(record['pbrMetallicRoughness']['baseColorFactor'], [0, 1, 0, .8], rtol=0, atol=1e-6) and
                    record['pbrMetallicRoughness']['baseColorTexture']['index'] == 0,
                    'Live alpha state lost authored factors, texture binding or material properties')
        properties = authoring['materialProperties']
        require(properties['transparent'] == caster['transparent'] and
                properties['alphaMasked'] == caster['alphaMasked'] and properties['doubleSided'],
                'Published alpha material properties disagree with graph routing')
    else:
        require(authoring['sourceMaterial'] == -1, 'Static reference unexpectedly performed material authoring')
    if baseline is not None:
        for key in ('graphIdentity', 'graphNodeCount', 'activeRoots', 'sourceTables', 'authoringNodes'):
            require(state['gltfMerge'][key] == baseline['gltfMerge'][key], 'Alpha edit changed graph/source identity '+key)
        for key in ('primaryObjectCount', 'primaryVertexCount', 'primaryIndexCount'):
            require(state['scene'][key] == baseline['scene'][key], 'Alpha edit changed geometry '+key)
        require(receiver == {row['mesh']: row for row in baseline['gltfMaterialVariants']['assignments']}[1],
                'Alpha edit changed the independent receiver binding')
    return dict(mode=mode, cutoff=cutoff, publications=publications, history=list(history))


def authoring_shadow_export(path):
    from gltf_material_authoring_runtime_regression import deep_contains, serialized_material_record
    from gltf_merge_runtime_regression import forest_worlds

    model, _ = load_asset(path)
    require(path.read_bytes()[:4] == b'glTF', 'Alpha authoring export is not GLB')
    require(len(model['meshes']) == 2 and all(len(mesh['primitives']) == 1 for mesh in model['meshes']),
            'Alpha authoring export lost its two primitive meshes')
    worlds, visible = forest_worlds(model)
    pending = list(model['scenes'][model['scene']]['nodes'])
    represented = {}
    for index in pending:
        node = model['nodes'][index]
        pending.extend(node.get('children', []))
        if 'mesh' in node:
            mesh_index = node['mesh']
            require(mesh_index in (0, 1) and mesh_index not in represented and visible[index],
                    'Alpha authoring export changed its represented caster/receiver set')
            represented[mesh_index] = index
    require(set(represented) == {0, 1}, 'Alpha authoring export lost its represented receiver or caster')
    bound = [model['meshes'][index]['primitives'][0].get('material', -1) for index in (0, 1)]
    # Source fixture GLBs bind source slots 0/1. Renderer exports retain two
    # unused schema-default runtime rows before the represented materials.
    require(bound in ([0, 1], [2, 3]) and len(model['materials']) == bound[-1] + 1,
            'Alpha authoring export changed its distinct material bindings')
    default = serialized_material_record({})
    require(all(serialized_material_record(row) == default for row in model['materials'][:bound[0]]),
            'Alpha authoring export added an unbound nondefault material')
    caster, receiver = [serialized_material_record(model['materials'][index]) for index in bound]
    deep_contains(caster, dict(alphaMode='MASK', alphaCutoff=.6, doubleSided=True, emissiveFactor=[0., 0., 0.],
        extensions=dict(KHR_materials_unlit={}), pbrMetallicRoughness=dict(baseColorFactor=[0., 1., 0., .8],
            metallicFactor=0., roughnessFactor=1., baseColorTexture=dict(index=0, texCoord=0))), 'saved caster')
    deep_contains(receiver, dict(alphaMode='OPAQUE', alphaCutoff=.5, doubleSided=False, emissiveFactor=[0., 0., 0.],
        pbrMetallicRoughness=dict(baseColorFactor=[.75, .75, .75, 1.], metallicFactor=0., roughnessFactor=1.)), 'saved receiver')
    require(set(caster.get('extensions', {})) == {'KHR_materials_unlit'} and not receiver.get('extensions') and
            not any(key.endswith('Texture') for key in receiver) and
            not any(key.endswith('Texture') for key in receiver['pbrMetallicRoughness']),
            'Alpha authoring export changed its receiver material')
    expected_positions = [np.array([[-1, -1, 0], [1, -1, 0], [1, 1, 0], [-1, 1, 0]]),
        np.array([[-1.6, -1.2, 0], [1.6, -1.2, 0], [1.6, 1.2, 0], [-1.6, 1.2, 0]])]
    for index in (0, 1):
        primitive = model['meshes'][index]['primitives'][0]
        positions = decode_accessor(path, primitive['attributes']['POSITION'])
        normals = decode_accessor(path, primitive['attributes']['NORMAL'])
        indices = decode_accessor(path, primitive['indices']).reshape(-1)
        require(primitive.get('mode', 4) == 4 and positions.shape == normals.shape == (4, 3) and
                np.allclose(positions, expected_positions[index], rtol=0, atol=1e-6) and
                np.array_equal(normals, np.tile([0, 0, 1], (4, 1))) and
                np.array_equal(indices, [0, 1, 2, 0, 2, 3]), 'Portable export changed caster/receiver geometry')
        expected_world = np.eye(4)
        if index == 0:
            expected_world[:3, 3] = [1.35, 0, 1]
        require(np.allclose(worlds[represented[index]], expected_world, rtol=0, atol=1e-6),
                'Portable export changed caster/receiver placement')
    primitive = model['meshes'][0]['primitives'][0]
    colors = decode_accessor(path, primitive['attributes']['COLOR_0'])
    require(colors.shape == (4, 4) and np.array_equal(colors[:, 3], np.full(4, .5)),
            'Portable export lost vertex alpha')
    require(np.array_equal(decode_accessor(path, primitive['attributes']['TEXCOORD_0']), UV),
            'Portable export changed the alpha texture coordinates')
    texture = caster['pbrMetallicRoughness']['baseColorTexture']['index']
    require(len(model['textures']) == len(model['images']) == 1 and texture == 0 and model['textures'][texture]['source'] == 0,
            'Portable export lost the alpha texture resource')
    return dict(materials=len(model['materials']), representedMaterialBindings=bound, vertexAlpha=.5,
                savedMode='MASK', savedCutoff=.6, receiverAndCasterGeometryVerified=True)


def authoring_shadow_self_test(output):
    rejected = []

    def reject(label, check):
        try:
            check()
        except RuntimeError:
            rejected.append(label)
        else:
            raise RuntimeError('Alpha shadow oracle accepted ' + label)

    with tempfile.TemporaryDirectory(prefix='alpha-shadow-oracle-', dir=output) as folder:
        root = Path(folder)
        source = root / 'fixture.gltf'
        write_cutoff_fixture(source, .8, .6, shadow='directional', textured_vertex_alpha=.5)
        model = json.loads(source.read_text(encoding='utf-8'))

        def export(value):
            # A GLB JSON chunk may contain the fixture's embedded data URIs.
            data = json.dumps(value).encode('utf-8')
            data += b' ' * (-len(data) % 4)
            path = root / 'saved.glb'
            path.write_bytes(struct.pack('<4sII', b'glTF', 2, 20 + len(data)) +
                             struct.pack('<I4s', len(data), b'JSON') + data)
            return path

        authoring_shadow_export(export(model))
        wrong = copy.deepcopy(model)
        wrong['materials'][0]['alphaCutoff'] = .3
        reject('lost exported cutoff', lambda: authoring_shadow_export(export(wrong)))
        wrong = copy.deepcopy(model)
        wrong['materials'][0]['alphaMode'] = 'OPAQUE'
        reject('lost exported mask mode', lambda: authoring_shadow_export(export(wrong)))
        wrong = copy.deepcopy(model)
        wrong['scenes'][0]['nodes'] = [0]
        reject('missing represented receiver', lambda: authoring_shadow_export(export(wrong)))
        wrong = copy.deepcopy(model)
        wrong['meshes'][1]['primitives'][0]['material'] = 0
        reject('aliased caster receiver binding', lambda: authoring_shadow_export(export(wrong)))
        runtime = copy.deepcopy(model)
        runtime['materials'] = [{}, {}] + runtime['materials']
        for mesh in runtime['meshes']:
            mesh['primitives'][0]['material'] += 2
        authoring_shadow_export(export(runtime))
        runtime['materials'][0] = dict(pbrMetallicRoughness=dict(baseColorFactor=[1, 0, 0, 1]))
        reject('unbound nondefault prefix material', lambda: authoring_shadow_export(export(runtime)))
        expected = dict(image=str(root / 'expected.png'))
        actual = dict(image=str(root / 'actual.png'))
        image = np.full((256, 256, 3), 80, dtype=np.uint8)
        Image.fromarray(image).save(expected['image'])
        Image.fromarray(image).save(actual['image'])
        authoring_shadow_pixels(actual, expected)
        image[CUTOFF_ROI] = 0
        Image.fromarray(image).save(actual['image'])
        reject('stale receiver pixels', lambda: authoring_shadow_pixels(actual, expected))
        material = model['materials'][0]
        state = dict(resolution=[256, 256], msaaSamples=1, taa=dict(enabled=False),
            scene=dict(primaryObjectCount=2), passes=[dict(name='ShadowCascade[0]', active=True)],
            rayShadows=dict(active=False), gltfMaterialVariants=dict(undoCount=1, redoCount=0, assignments=[
                dict(mesh=0, material=2, transparent=False, alphaMasked=True, doubleSided=True),
                dict(mesh=1, material=3, transparent=False, alphaMasked=False, doubleSided=False)]),
            gltfMaterialAuthoring=dict(publications=1, pendingGesture=False, sourceMaterial=0, runtimeMaterial=2,
                uploadedMaterialHash=11, authored=material, evaluated=material,
                materialProperties=dict(transparent=False, alphaMasked=True, doubleSided=True)))
        authoring_shadow_state(state, 'discarded', 'directional', (1, 0), 1)
        wrong_state = copy.deepcopy(state)
        wrong_state['gltfMaterialVariants']['assignments'][0]['alphaMasked'] = False
        reject('stale mask classification', lambda: authoring_shadow_state(wrong_state, 'discarded', 'directional', (1, 0), 1))
        wrong_state = copy.deepcopy(state)
        wrong_state['gltfMaterialVariants']['undoCount'] = 0
        reject('lost edit history', lambda: authoring_shadow_state(wrong_state, 'discarded', 'directional', (1, 0), 1))
    return dict(check='oracle-negative-controls', suite='authoring-shadows', rejected=rejected)


def run_authoring_shadow_suite(run, techniques, *, generate_only):
    from gltf_merge_runtime_regression import portable_copy
    from gltf_extended_materials_runtime_regression import validation_log_error

    # Material alpha .8 * vertex alpha .5 * white texture alpha 1 = .4.
    # The .3/.6 thresholds are deliberately far from derivative/dither edges.
    require(.3 < .8 * .5 < .6, 'Alpha authoring coverage controls are vacuous')
    run.report['checks'].append(authoring_shadow_self_test(run.output))
    script = run.output / 'authoring-shadow-sequence.json'
    actions = [dict(materialEdit=dict(sourceMaterial=0, values=[dict(property='alphaCutoff', value=.6)])),
               dict(sceneHistory='undo'), dict(sceneHistory='redo'),
               dict(materialEdit=dict(sourceMaterial=0, alphaMode='OPAQUE')),
               dict(sceneHistory='undo'), dict(sceneHistory='redo'),
               dict(materialEdit=dict(sourceMaterial=0, alphaMode='BLEND')),
               dict(sceneHistory='undo'), dict(sceneHistory='redo'),
               dict(materialEdit=dict(sourceMaterial=0, alphaMode='MASK'))]
    frames = [13 * (index + 1) for index in range(len(AUTHORING_SHADOW_STEPS))]
    script.write_text(json.dumps(dict(schemaVersion=1, frames=frames[-1], sampleFrames=frames,
        events=[dict(frame=frames[index] + 1, **action) for index, action in enumerate(actions)])), encoding='utf-8')
    for light in ('directional', 'point'):
        controls = {}
        for control, (mode, cutoff) in AUTHORING_SHADOW_CONTROLS.items():
            path = run.output / f'authoring-shadow-{light}-{control}.gltf'
            write_cutoff_fixture(path, .8, cutoff, shadow=light, textured_vertex_alpha=.5)
            model = json.loads(path.read_text(encoding='utf-8'))
            model['materials'][0]['alphaMode'] = mode
            path.write_text(json.dumps(model), encoding='utf-8')
            colors = decode_accessor(path, model['meshes'][0]['primitives'][0]['attributes']['COLOR_0'])
            require(np.array_equal(colors[:, 3], np.full(4, .5)) and model['nodes'][0]['translation'] == [1.35, 0, 1],
                    'Alpha authoring fixture changed its vertex coverage or caster geometry')
            controls[control] = path
        run.report['fixtures'].append(dict(suite='authoring-shadows', light=light, script=str(script),
            paths={key: str(path) for key, path in controls.items()}, combinedAlpha=.4,
            receiverRoi=[112, 112, 144, 144]))
        if generate_only:
            continue
        light_args = ('--directional-direction', '-1.35', '0', '-1') if light == 'directional' else ()
        extra = (*CUTOFF_ARGS, *light_args, '--bounce-intensity', '0', '--ray-shadows', 'raster', '--no-ray-query')

        def capture(source, name, technique, **options):
            record = run.capture(source, name, technique, extra=extra + options.pop('extra', ()),
                                 lit=light == 'directional', **options)
            text = Path(record['log']).read_text(encoding='utf-8', errors='replace')
            require(not validation_log_error(text), 'Strict Vulkan validation failed: ' + record['log'])
            require('Insert instance layer "VK_LAYER_KHRONOS_validation"' in text,
                    'Khronos validation layer did not load: ' + record['log'])
            return record

        for technique in techniques:
            stem = f'{technique}-authoring-shadow-{light}'
            references = {control: capture(path, stem + '-reference-' + control, technique)
                          for control, path in controls.items()}
            for control, record in references.items():
                authoring_shadow_state(record['state'], control, light, (0, 0), 0)
            receiver = {key: cutoff_roi(record) for key, record in references.items()}
            darkening = float((receiver['discarded'] - receiver['covered']).mean())
            require(darkening > 20 and np.array_equal(receiver['covered'], receiver['opaque']) and
                    np.array_equal(receiver['discarded'], receiver['blend']),
                    f'Literal {light} alpha shadow receiver controls disagree: {darkening}')
            run.report['checks'].append(dict(suite='authoring-shadows', technique=technique, light=light,
                check='independent-receiver-controls', shadowDarkening=darkening, receiverPixels=1024))
            exported = run.output / (stem + '-edited.glb')
            final = capture(controls['covered'], stem + '-live', technique, destination=exported,
                            extra=('--capture-sequence', str(script)))
            records = {}
            for index, (label, control, history) in enumerate(AUTHORING_SHADOW_STEPS):
                image = Path(final['image'])
                if index + 1 < len(frames):
                    image = image.with_name(image.stem + f'.frame-{frames[index]:04d}.png')
                telemetry = image.with_suffix('.telemetry.json')
                require(image.is_file() and telemetry.is_file(), f'Missing live alpha checkpoint {image}')
                record = dict(image=str(image), telemetry=str(telemetry), log=final['log'],
                              state=json.loads(telemetry.read_text(encoding='utf-8-sig')))
                records[label] = record
                status = authoring_shadow_state(record['state'], control, light, history, index,
                    baseline=records['initial']['state'] if index else None)
                comparison = authoring_shadow_pixels(record, references[control])
                if index + 1 < len(frames):
                    run.report['captures'].append(dict(image=str(image), telemetry=str(telemetry), log=final['log'],
                        command=final['command'], frame=frames[index], checkpoint=label))
                run.report['checks'].append(dict(suite='authoring-shadows', technique=technique, light=light,
                    check='live-history', checkpoint=label, image=str(image), telemetry=str(telemetry), **status, **comparison))
            run.report['checks'].append(dict(suite='authoring-shadows', technique=technique, light=light,
                check='edited-export', **authoring_shadow_export(exported)))
            copied, closure = portable_copy(exported, run.output / (stem + '-portable'))
            reload = capture(copied, stem + '-reload', technique)
            authoring_shadow_state(reload['state'], 'discarded', light, (0, 0), 0)
            run.report['checks'].append(dict(suite='authoring-shadows', technique=technique, light=light,
                check='portable-reload', portableFiles=closure, **authoring_shadow_pixels(reload, references['discarded'])))
            run.save()
            print('PASS live alpha edits, history and portable shadow reload', light, technique, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--suite', action='append', choices=('coverage', 'shadows', 'cutoff', 'authoring-shadows'))
    parser.add_argument('--cutoff-color-only', action='store_true', help='Run only color, depth and picking in the cutoff suite')
    parser.add_argument('--case', action='append', choices=CASES)
    parser.add_argument('--technique', action='append', choices=TECHNIQUES)
    parser.add_argument('--shadow-mode', action='append', choices=('raster', 'hard'))
    parser.add_argument('--roundtrip', action='store_true')
    parser.add_argument('--generate-only', action='store_true')
    parser.add_argument('--managed-output', action='store_true', help='Keep captures in a fresh child of the output directory')
    parser.add_argument('--require-opt-in', action='store_true', help='Require CONTAINER_RUN_GPU_GLTF_MATERIAL_AUTHORING=1 for authoring-shadows')
    parser.add_argument('--timeout', type=float, default=180)
    args = parser.parse_args()
    authoring = 'authoring-shadows' in (args.suite or ())
    if authoring and args.require_opt_in and not args.generate_only and os.environ.get('CONTAINER_RUN_GPU_GLTF_MATERIAL_AUTHORING') != '1':
        print('Set CONTAINER_RUN_GPU_GLTF_MATERIAL_AUTHORING=1 to run the alpha authoring shadow regression.')
        return 77
    if args.managed_output:
        args.output.mkdir(parents=True, exist_ok=True)
        args.output = Path(tempfile.mkdtemp(prefix='run-', dir=args.output))
    if authoring and not args.generate_only:
        os.environ['VK_LOADER_DEBUG'] = os.environ.get('VK_LOADER_DEBUG', '') + ',layer'
    run = NativeRun(args.renderer, args.output, args.timeout)
    run.report['mode'] = 'generate-only' if args.generate_only else 'native'
    try:
        if 'authoring-shadows' in (args.suite or ()):
            run_authoring_shadow_suite(run, args.technique or TECHNIQUES, generate_only=args.generate_only)
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
    raise SystemExit(main())
