"""Independent #92 material controls, coverage, animation and portable exports.

The default essential cohort is bounded to 22 captures per raster technique,
including sloped 4x-MSAA mixed flagged/unflagged ownership.
Optional cohorts expand texture/UV identity, signed frames, mixed MSAA ownership,
environment, area lights, shadows, transparent draws, deformation and export.
--generate-only writes immutable fixture inputs and an explicit comparison plan;
it never launches the renderer and never reports native checks as passed.

Exact comparisons use independent authored representations, rather than captured
golden files. Small angle/frame comparisons allow a fixed one-byte percentile
envelope for CPU-float vs shader trig and half-float auxiliary storage. Relative
diffuse checks invert the declared display curve and account for PNG rounding.
No threshold or reference is learned from renderer output.
"""
from __future__ import annotations

import argparse
import base64
import copy
import hashlib
import io
import json
import math
import os
from pathlib import Path
import re
import subprocess

import numpy as np
from PIL import Image

from gltf_accessor_runtime_regression import Asset, load_asset, mesh, require
from gltf_tangent_uv_runtime_regression import finite_json
from gltf_texture_formats_runtime_regression import verify_manifest


TECHNIQUES = ('deferred-raster', 'forward-raster')
COHORTS = ('essential', 'uv-mirror', 'coverage', 'environment', 'area',
           'shadows', 'transparent', 'deformation', 'animation-export')
ANISO = 'KHR_materials_anisotropy'
DIFFUSE = 'KHR_materials_diffuse_transmission'
TRANSFORM = 'KHR_texture_transform'
SPECULAR = 'KHR_materials_specular'
WIDTH, HEIGHT = 640, 360
ROI = (slice(112, 248), slice(242, 398))
COLOR = [.08, .25, .85]
SAMPLE_TIMES = (0., 1.)
POINTER_VALUES = {
    f'/materials/0/extensions/{ANISO}/anisotropyStrength': [0., .8],
    f'/materials/0/extensions/{ANISO}/anisotropyRotation': [-.6, .6],
    f'/materials/0/extensions/{DIFFUSE}/diffuseTransmissionFactor': [0., .7],
    f'/materials/0/extensions/{DIFFUSE}/diffuseTransmissionColorFactor':
        [[1., .2, .1], [.1, .3, 1.]],
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validation_log_error(text):
    # Newer validation-layer builds also print synchronization errors without
    # the legacy SYNC-HAZARD prefix or the words "Validation Error".
    return bool(re.search(r'VUID-|SYNC-HAZARD|Validation Error|\bhazard detected\b|'
        r'\b(?:READ|WRITE)_AFTER_(?:READ|WRITE)\b[^\r\n]*\bhazard\b', text, re.I))


def extension_material(*, anisotropy=None, diffuse=None, color=COLOR,
                       base=(.7, .12, .04), metallic=0., alpha=1., specular=0.):
    extensions = {SPECULAR: dict(specularFactor=specular)}
    if anisotropy is not None:
        extensions[ANISO] = dict(anisotropyStrength=anisotropy,
                                  anisotropyRotation=0.)
    if diffuse is not None:
        extensions[DIFFUSE] = dict(diffuseTransmissionFactor=diffuse,
            diffuseTransmissionColorFactor=list(color))
    material = dict(name='Independent extended-material receiver',
        pbrMetallicRoughness=dict(baseColorFactor=[*base, alpha],
            metallicFactor=metallic, roughnessFactor=.35 if metallic else .8),
        doubleSided=False, extensions=extensions)
    if alpha < 1:
        material['alphaMode'] = 'BLEND'
    return material


def texture(asset, rgba, *, sampler=None):
    values = np.asarray(rgba, dtype=np.uint8)
    if values.ndim == 1:
        values = np.tile(values, (2, 2, 1))
    index = asset.image(values)
    asset.model['samplers'][-1] = sampler or dict(
        minFilter=9728, magFilter=9728, wrapS=33071, wrapT=33071)
    return index


def fixture(path, spec):
    """Build a +Z receiver; reference frames are authored independently."""
    asset = Asset()
    positions = np.array([[-1., -.8, 0], [1., -.8, 0],
                          [1., .8, 0], [-1., .8, 0]], dtype=float)
    indices = [0, 1, 2, 0, 2, 3]
    normal = np.array([0., 0., -1. if spec.get('opposite') else 1.])
    tangent = np.array(spec.get('tangent', [1., 0., 0., 1.]), dtype=float)
    if spec.get('opposite'):
        tangent = np.array([-1., 0., 0., 1.])
        indices = [0, 2, 1, 0, 3, 2]
    if spec.get('slope'):
        positions[:, 2] = positions[:, 0] * .125
        normal = np.array([-.125, 0., 1.]); normal /= np.linalg.norm(normal)
        tangent = np.array([1., 0., .125, 1.]); tangent[:3] /= np.linalg.norm(tangent[:3])
    if spec.get('mirror') == 'baked':
        positions[:, 0] *= -1
        tangent[0] *= -1
        tangent[3] *= -1
        indices = [0, 2, 1, 0, 3, 2]
    delta = np.tile([.125, .0625, 0.], (4, 1))
    if spec.get('deformation') == 'baked':
        positions += delta
    uv0 = [[.25, .5]] * 4 if spec.get('constant_uv') else [[0, 0], [1, 0], [1, 1], [0, 1]]
    uv1 = [[.75, .5]] * 4
    primitive = mesh(asset, asset.accessor('VEC3', positions),
        asset.accessor('VEC3', np.tile(normal, (4, 1))),
        asset.accessor('VEC2', uv0),
        tangent=asset.accessor('VEC4', np.tile(tangent, (4, 1))),
        indices=asset.accessor('SCALAR', indices, 5123, indices=True))
    primitive['attributes']['TEXCOORD_1'] = asset.accessor('VEC2', uv1)
    material = copy.deepcopy(spec['material'])
    asset.model['materials'] = [material]
    for slot, rgba in spec.get('textures', {}).items():
        extension, member = slot.split('/')
        info = dict(index=texture(asset, rgba,
            sampler=spec.get('slot_samplers', {}).get(slot, spec.get('sampler'))))
        info.update(copy.deepcopy(spec.get('texture_info', {}).get(slot, {})))
        info.setdefault('extras', {})['sourceSlot'] = slot
        material['extensions'][extension][member] = info
    if spec.get('deformation') == 'morph':
        primitive['targets'] = [dict(POSITION=asset.accessor('VEC3', delta))]
        asset.model['meshes'][0]['weights'] = [1.]
    if spec.get('mirror') == 'node':
        asset.model['nodes'][0]['scale'] = [-1., 1., 1.]
    if spec.get('coverage'):
        # Every asset uses the same partitioned geometry and first selected
        # left primitive. Only the source adds a flagged right duplicate. The
        # plain right owner alone is double-sided: the existing CPU NoCull route
        # records after all primary/GPU draws, regardless of EnTT iteration or
        # atomic indirect compaction order. Both triangles face the camera, so
        # this route choice does not change their front-face BSDF. Duplicate
        # vertices/triangles are identical, giving exact sample-depth equality.
        require(spec.get('slope'), 'Coverage fixture must exercise sloped MSAA depth')
        plain = extension_material(base=(.35, .35, .35), metallic=1., specular=1.)
        plain['doubleSided'] = True
        asset.model['materials'].append(plain)
        partition = []
        for side in ('left', 'right'):
            clipped = positions.copy()
            # The interior boundary is oblique and crosses pixel interiors;
            # an axis-aligned x=0 edge could land exactly between sample grids.
            clipped[:, 0] = [-1, -.25, .375, -1] if side == 'left' else [-.25, 1, 1, .375]
            clipped[:, 2] = clipped[:, 0] * .125
            extra = copy.deepcopy(primitive)
            extra['attributes']['POSITION'] = asset.accessor('VEC3', clipped)
            extra['material'] = 0 if side == 'left' else 1
            partition.append(extra)
        left, right = partition
        right_flagged = copy.deepcopy(right)
        right_flagged['material'] = 0
        if spec['coverage'] == 'source':
            selected = [left, right_flagged, right]
        elif spec['coverage'] == 'oracle':
            selected = [left, right]
        elif spec['coverage'] == 'wrong':
            selected = [left, right_flagged]
        else:
            raise RuntimeError('Unknown coverage fixture')
        # Keep all original/full/partition accessors, buffers and materials,
        # including unused entries, in all three documents.
        asset.model['meshes'][0]['primitives'] = selected
    if spec.get('blocker'):
        # A hidden-from-camera opaque back-side blocker, halfway to the lamp.
        p = np.array([[-.22, -.22, -1], [.22, -.22, -1],
                      [.22, .22, -1], [-.22, .22, -1]])
        blocker = dict(attributes=dict(POSITION=asset.accessor('VEC3', p),
            NORMAL=asset.accessor('VEC3', [[0, 0, -1]] * 4)),
            indices=asset.accessor('SCALAR', [0, 2, 1, 0, 3, 2], 5123, indices=True),
            material=len(asset.model['materials']))
        asset.model['materials'].append(extension_material(base=(0, 0, 0)))
        asset.model['meshes'].append(dict(primitives=[blocker]))
        asset.model['nodes'].append(dict(name='Analytical back blocker', mesh=1))
    lighting = spec.get('lighting', 'front')
    light = dict(name='Independent isolated lamp', type='point', color=[1, 1, 1],
                 intensity=0 if lighting in ('environment', 'directional') else 18.)
    if spec.get('finite_shadow_light'):
        # The raster local-shadow allocator requires a finite positive range.
        light['range'] = 5.
    light_position = [.7, .45, 2.] if lighting in ('front', 'both') else [0., 0., -2.]
    lamp = dict(name='Authored lamp', translation=light_position,
                extensions=dict(KHR_lights_punctual=dict(light=0)))
    if lighting == 'area':
        light['intensity'] = 6.
        light['extras'] = dict(areaLight=dict(shape='rect', width=.8, height=.6))
        lamp.update(rotation=[0., 1., 0., 0.], translation=[.2, .1, -1.5])
    asset.model['nodes'].append(lamp)
    asset.model['extensions'] = dict(KHR_lights_punctual=dict(lights=[light]))
    if lighting == 'both':
        asset.model['extensions']['KHR_lights_punctual']['lights'].append(copy.deepcopy(light))
        asset.model['nodes'].append(dict(name='Independent back lamp', translation=[0., 0., -2.],
            extensions=dict(KHR_lights_punctual=dict(light=1))))
    used = {SPECULAR, 'KHR_lights_punctual'}
    for entry in asset.model['materials']:
        used.update(entry.get('extensions', {}))
        for extension in entry.get('extensions', {}).values():
            if any(TRANSFORM in info.get('extensions', {}) for info in extension.values() if isinstance(info, dict)):
                used.add(TRANSFORM)
    if spec.get('animated'):
        used.add('KHR_animation_pointer')
        times = asset.accessor('SCALAR', [0., 1.])
        samplers, channels = [], []
        for pointer, values in POINTER_VALUES.items():
            kind = 'VEC3' if isinstance(values[0], list) else 'SCALAR'
            output = asset.accessor(kind, values)
            samplers.append(dict(input=times, output=output, interpolation='LINEAR'))
            channels.append(dict(sampler=len(samplers)-1,
                target=dict(path='pointer', extensions=dict(KHR_animation_pointer=dict(pointer=pointer)))))
        asset.model['animations'] = [dict(name='All extended numeric pointers', samplers=samplers, channels=channels)]
    asset.model['extensionsUsed'] = sorted(used)
    asset.model['scenes'][0]['nodes'] = list(range(len(asset.model['nodes'])))
    asset.model['asset']['generator'] = 'Container independent #92 metamorphic fixtures'
    path.parent.mkdir(parents=True, exist_ok=True)
    asset.write(path)
    return asset.model


def build_plan(output, cohorts):
    captures, checks, fixture_specs = {}, [], {}

    def capture(name, material, *, cohort, **settings):
        options = {key: settings.pop(key) for key in list(settings)
                   if key in ('msaa', 'area_mode', 'ray', 'time', 'save', 'reload', 'generic_path')}
        spec = dict(material=material, **settings)
        path = output / 'fixtures' / name
        path = path / ('animated.gltf' if options.get('generic_path') else 'models/validation/cornell_box_local_light.gltf')
        fixture_specs[name] = dict(path=str(path), spec=spec)
        captures[name] = dict(name=name, cohort=cohort, fixture=name, **options,
            lighting=spec.get('lighting', 'front'), opposite=spec.get('opposite', False))
        return name

    def check(kind, name, *images, **settings):
        checks.append(dict(kind=kind, name=name, images=list(images), **settings))

    if 'essential' in cohorts:
        a = lambda **kw: extension_material(base=(.35, .35, .35), metallic=1., specular=1., **kw)
        capture('a-none', a(), cohort='essential')
        capture('a-zero', a(anisotropy=0.), cohort='essential',
                textures={f'{ANISO}/anisotropyTexture': [0, 255, 255, 255]})
        capture('a-blue-zero', a(anisotropy=1.), cohort='essential',
                textures={f'{ANISO}/anisotropyTexture': [255, 0, 0, 255]})
        positive = a(anisotropy=.85); positive['extensions'][ANISO]['anisotropyRotation'] = math.pi / 4
        negative = copy.deepcopy(positive); negative['extensions'][ANISO]['anisotropyRotation'] *= -1
        capture('a-positive', positive, cohort='essential')
        capture('a-negative', negative, cohort='essential')
        tex = a(anisotropy=.85); tex['extensions'][ANISO]['anisotropyRotation'] = -math.pi / 2
        capture('a-texture-equivalent', tex, cohort='essential',
                textures={f'{ANISO}/anisotropyTexture': [0, 255, 255, 255]})
        capture('a-tangent-equivalent', a(anisotropy=.85), cohort='essential',
                tangent=[math.sqrt(.5), math.sqrt(.5), 0., 1.])
        check('exact', 'authored-anisotropy-zero', 'a-zero', 'a-none')
        check('exact', 'sampled-anisotropy-zero', 'a-blue-zero', 'a-none')
        check('near', 'packed-direction-plus-negative-rotation', 'a-texture-equivalent', 'a-positive')
        check('near', 'independent-authored-tangent', 'a-tangent-equivalent', 'a-positive')
        check('contrast', 'anisotropy-is-visible', 'a-positive', 'a-none')
        check('contrast', 'rotation-sign-is-visible', 'a-positive', 'a-negative')

        capture('f-none', extension_material(), cohort='essential')
        capture('f-zero', extension_material(diffuse=0.), cohort='essential',
                textures={f'{DIFFUSE}/diffuseTransmissionTexture': [0, 0, 0, 255]})
        capture('f-half', extension_material(diffuse=.5), cohort='essential')
        capture('f-full', extension_material(diffuse=1.), cohort='essential')
        capture('f-black-reference', extension_material(base=(0, 0, 0)), cohort='essential')
        check('exact', 'authored-diffuse-zero-with-texture', 'f-zero', 'f-none')
        check('near', 'full-diffuse-removes-only-front-base', 'f-full', 'f-black-reference')
        check('linear', 'half-front-diffuse-split', 'f-half', 'f-none', 'f-full', fraction=.5)
        check('contrast', 'front-diffuse-removal-is-visible', 'f-none', 'f-full')
        for name, material, textures in (
            ('b-none', extension_material(), {}),
            ('b-zero', extension_material(diffuse=0.), {f'{DIFFUSE}/diffuseTransmissionTexture': [0, 0, 0, 255]}),
            ('b-alpha-zero', extension_material(diffuse=1.), {f'{DIFFUSE}/diffuseTransmissionTexture': [255, 255, 255, 0]}),
            ('b-half', extension_material(diffuse=.5), {f'{DIFFUSE}/diffuseTransmissionTexture': [0, 0, 0, 255]}),
            ('b-full', extension_material(diffuse=1.), {}),
            ('b-independent-base', extension_material(diffuse=1., base=(.04, .8, .05)), {}),
            ('b-black', extension_material(diffuse=1., color=(0, 0, 0)), {})):
            capture(name, material, cohort='essential', lighting='back', textures=textures)
        check('exact', 'back-authored-factor-zero', 'b-zero', 'b-none')
        check('exact', 'back-alpha-channel-zero', 'b-alpha-zero', 'b-none')
        check('exact', 'black-independent-transmission', 'b-black', 'b-none')
        check('near', 'back-transmission-independent-of-base', 'b-independent-base', 'b-full')
        check('linear', 'half-back-diffuse-split', 'b-half', 'b-full', 'b-none', fraction=.5)
        check('contrast', 'back-transmission-is-visible', 'b-full', 'b-none')
        check('blue', 'independent-blue-transmission-color', 'b-full')

    if 'uv-mirror' in cohorts:
        mat = extension_material(anisotropy=.85, metallic=1., specular=1.)
        mat['extensions'][ANISO]['anisotropyRotation'] = .65
        # Anisotropy identifies an axis: opposite RG vectors are equivalent.
        # These texels decode to orthogonal axes rather than opposite vectors.
        packed = [[[255, 0, 255, 255], [255, 255, 255, 255]]] * 2
        verify_uv_anisotropy_axes(packed)
        slot = f'{ANISO}/anisotropyTexture'
        info = dict(texCoord=0, extensions={TRANSFORM: dict(offset=[.5, 0], scale=[1, 1],
                    rotation=0., texCoord=0, extras=dict(uvSource='retain-transform'))})
        capture('uv-transform', mat, cohort='uv-mirror', constant_uv=True,
                textures={slot: packed}, texture_info={slot: info})
        capture('uv-one', mat, cohort='uv-mirror', constant_uv=True,
                textures={slot: packed}, texture_info={slot: dict(texCoord=1)})
        capture('uv-wrong-zero', mat, cohort='uv-mirror', constant_uv=True,
                textures={slot: packed})
        check('exact', 'uv-transform-equals-independent-uv1', 'uv-transform', 'uv-one')
        check('contrast', 'uv-selection-negative-control', 'uv-one', 'uv-wrong-zero')
        capture('mirror-node', mat, cohort='uv-mirror', mirror='node')
        capture('mirror-baked', mat, cohort='uv-mirror', mirror='baked')
        capture('mirror-wrong-sign', mat, cohort='uv-mirror', mirror='baked', tangent=[1., 0., 0., -1.])
        check('near', 'mirrored-node-equals-baked-signed-frame', 'mirror-node', 'mirror-baked')
        check('contrast', 'mirrored-handedness-negative-control', 'mirror-baked', 'mirror-wrong-sign')
        factor_slot = f'{DIFFUSE}/diffuseTransmissionTexture'
        color_slot = f'{DIFFUSE}/diffuseTransmissionColorTexture'
        diffuse_texels = {factor_slot: [[[255, 0, 0, 0], [0, 0, 0, 128]]] * 2,
                          color_slot: [[[255, 0, 0, 255], [64, 128, 192, 255]]] * 2}
        capture('uv-diffuse-textured', extension_material(diffuse=1., color=(1, 1, 1)),
            cohort='uv-mirror', lighting='back', constant_uv=True, textures=diffuse_texels,
            texture_info={factor_slot: info, color_slot: dict(texCoord=1)},
            slot_samplers={color_slot: dict(minFilter=9728, magFilter=9728, wrapS=10497, wrapT=33648)})
        srgb = np.array([64, 128, 192], dtype=float)/255
        linear_color = np.where(srgb <= .04045, srgb/12.92, ((srgb+.055)/1.055)**2.4).tolist()
        capture('uv-diffuse-independent', extension_material(diffuse=128/255, color=linear_color),
            cohort='uv-mirror', lighting='back')
        capture('uv-diffuse-wrong-zero', extension_material(diffuse=1., color=(1, 1, 1)),
            cohort='uv-mirror', lighting='back', constant_uv=True, textures=diffuse_texels)
        check('near', 'diffuse-alpha-linear-and-color-srgb-uv-roles', 'uv-diffuse-textured', 'uv-diffuse-independent')
        check('contrast', 'diffuse-texture-uv-negative-control', 'uv-diffuse-textured', 'uv-diffuse-wrong-zero')

    if 'coverage' in cohorts or 'essential' in cohorts:
        mat = extension_material(anisotropy=.9, diffuse=.7, metallic=.15, specular=1.)
        for msaa in ((1, 4) if 'coverage' in cohorts else (4,)):
            for version in ('source', 'oracle', 'wrong'):
                capture(f'coverage-{msaa}-{version}', mat,
                        cohort='coverage' if 'coverage' in cohorts else 'essential',
                        slope=True, coverage=version, msaa=msaa, lighting='both')
            check('near', f'msaa{msaa}-deterministic-duplicate-ownership',
                  f'coverage-{msaa}-source', f'coverage-{msaa}-oracle')
            check('contrast', f'msaa{msaa}-unflagged-owner-is-visible',
                  f'coverage-{msaa}-source', f'coverage-{msaa}-wrong')

    if 'transparent' in cohorts:
        for name, mat in (
            ('transparent-none', extension_material(alpha=.6)),
            ('transparent-zero', extension_material(alpha=.6, anisotropy=0., diffuse=0.)),
            ('transparent-full', extension_material(alpha=.6, diffuse=1.)),
            ('transparent-independent-base', extension_material(alpha=.6, diffuse=1., base=(.04, .8, .05)))):
            capture(name, mat, cohort='transparent', lighting='back')
        check('exact', 'transparent-zero-is-legacy', 'transparent-zero', 'transparent-none')
        check('near', 'transparent-color-independent-of-base', 'transparent-independent-base', 'transparent-full')
        check('contrast', 'transparent-diffuse-is-visible', 'transparent-full', 'transparent-none')

    if 'shadows' in cohorts:
        for name, block in (('shadow-open', False), ('shadow-blocked', True)):
            capture(name, extension_material(diffuse=1.), cohort='shadows', lighting='back',
                    blocker=block, finite_shadow_light=True)
        check('shadow', 'back-punctual-occluder-casts-shadow', 'shadow-blocked', 'shadow-open')

    if 'area' in cohorts:
        mat = extension_material(anisotropy=.8, diffuse=.7)
        for mode in ('sampled', 'ltc'):
            capture('area-'+mode, mat, cohort='area', lighting='area', area_mode=mode)
        check('exact', 'active-extensions-bypass-isotropic-ltc', 'area-ltc', 'area-sampled')
        check('blue', 'area-backside-response-is-nonvacuous', 'area-sampled')

    if 'environment' in cohorts:
        for side in ('back', 'opposite-front'):
            for color in ('blue', 'black'):
                mat = extension_material(diffuse=1. if side == 'back' else None,
                    color=COLOR if color == 'blue' else [0, 0, 0],
                    base=COLOR if side == 'opposite-front' and color == 'blue' else [0, 0, 0])
                capture(f'env-{side}-{color}', mat, cohort='environment', lighting='environment',
                        opposite=side == 'opposite-front')
        check('environment', 'back-hemisphere-independent-front-reference',
              'env-back-blue', 'env-back-black', 'env-opposite-front-blue', 'env-opposite-front-black')
        check('contrast', 'opposite-environment-is-visible', 'env-back-blue', 'env-back-black')

    if 'deformation' in cohorts:
        mat = extension_material(anisotropy=.8, metallic=1., specular=1.)
        for mode in ('morph', 'baked'):
            capture('deformation-'+mode, mat, cohort='deformation', deformation=mode)
        check('near', 'gpu-morph-equals-independent-baked-geometry', 'deformation-morph', 'deformation-baked')

    if 'animation-export' in cohorts:
        mat = extension_material(anisotropy=0., diffuse=0.)
        mat['extensions'][ANISO]['anisotropyRotation'] = -.6
        mat['extensions'][DIFFUSE]['diffuseTransmissionColorFactor'] = [1., .2, .1]
        slot_textures = {f'{ANISO}/anisotropyTexture': [255, 0, 255, 255],
            f'{DIFFUSE}/diffuseTransmissionTexture': [0, 0, 0, 255],
            f'{DIFFUSE}/diffuseTransmissionColorTexture': [255, 255, 255, 255]}
        infos = {slot: dict(texCoord=1, extensions={TRANSFORM: dict(offset=[.125, .25],
            scale=[.75, 1.25], rotation=.2, extras=dict(sourceSlot=slot))}) for slot in slot_textures}
        for time in SAMPLE_TIMES:
            capture(f'animated-time{time:g}', mat, cohort='animation-export',
                lighting='back', animated=True, time=time, generic_path=True,
                textures=slot_textures, texture_info=infos,
                sampler=dict(minFilter=9728, magFilter=9728, wrapS=10497, wrapT=33648))
        check('contrast', 'all-pointer-endpoints-are-visible', 'animated-time0', 'animated-time1')
        # The source path has the ordinary viewer defaults for every format;
        # avoid the special local-light .gltf suffix changing GLB bounce defaults.
        for suffix in ('glb', 'gltf'):
            capture('animated-save-'+suffix, mat, cohort='animation-export',
                lighting='back', animated=True, time=.5, generic_path=True, save=suffix,
                textures=slot_textures, texture_info=infos,
                sampler=dict(minFilter=9728, magFilter=9728, wrapS=10497, wrapT=33648))
            for time in SAMPLE_TIMES:
                name = f'animated-{suffix}-time{time:g}'
                captures[name] = dict(name=name, cohort='animation-export', reload=suffix,
                    fixture='animated-save-'+suffix, time=time, lighting='back', opposite=False)
                check('near', f'{suffix}-animated-endpoint{time:g}-roundtrip', name, f'animated-time{time:g}')

    return dict(schemaVersion=1, cohorts=list(cohorts), captures=list(captures.values()),
                checks=checks, fixtures=fixture_specs,
                expectedCapturesPerTechnique=len(captures), expectedChecksPerTechnique=len(checks),
                thresholds=dict(exactMaximumByteError=0, nearInteriorMean=.12, nearInteriorP99=1.,
                    nearMaximum=4., contrastInteriorMean=.15, contrastChangedFraction=.005,
                    diffuseRelativeError=.04, diffuseAbsoluteRadianceError=.025),
                oracleNotes=[
                    'Essential zero-factor controls compare complete images exactly, including sampled B=0/A=0.',
                    'Rotation direction is checked against independently authored tangents and a packed diagonal plus negative rotation.',
                    'Diffuse interpolation is linear in radiance after removing fixed reflected specular, never in display bytes.',
                    'Black transmission remains a positive factor that removes front diffuse; independent base color cannot tint backside transmission.',
                    'Coverage source adds a single-sided flagged duplicate of the exact right partition triangles; oracle omits that duplicate.',
                    'Only the plain owner is double-sided, routing its unique right draw through CPU NoCull after all flagged primary/GPU draws; authored order is not assumed.',
                    'Coverage buffers/accessors/material tables are identical, and every fixture selects the same first left primitive.',
                    'Environment reference faces the opposite hemisphere; its declared .04 deferred/.025 forward ambient floor is subtracted analytically.',
                    'PNG quantization and CPU/shader trig are the only declared small equivalence envelopes; thresholds are fixed before capture.'])


def image(record, *, crop=True):
    with Image.open(record['image']) as png:
        values = np.asarray(png.convert('RGB'), dtype=np.float64)
    require(values.shape == (HEIGHT, WIDTH, 3), 'Unexpected native capture size')
    return values[ROI] if crop else values


def radiance(values, exposure=.35):
    require(float(values.max()) < 254, 'Radiance oracle is clipped; preserve diagnostics and investigate lighting')
    value = values / 255.
    mapped = np.where(value <= .04045, value / 12.92, ((value + .055) / 1.055) ** 2.4)
    a, b, c = 2.43 * mapped - 2.51, .59 * mapped - .03, .14 * mapped
    return (-b - np.sqrt(np.maximum(b*b-4*a*c, 0))) / (2*a) / exposure


def compare(check, captures, thresholds):
    records = [captures[name] for name in check['images']]
    kind = check['kind']
    if kind in ('exact', 'near', 'contrast'):
        delta = np.abs(image(records[0], crop=kind != 'exact') - image(records[1], crop=kind != 'exact'))
        metrics = dict(meanAbsoluteByteError=float(delta.mean()), p99ByteError=float(np.quantile(delta, .99)),
            maximumByteError=float(delta.max()), changedFraction=float(np.any(delta >= 4, axis=2).mean()))
        if kind == 'exact':
            require(metrics['maximumByteError'] == 0, f'Exact oracle failed: {check["name"]}: {metrics}')
        elif kind == 'near':
            metrics['fullImageMaximumByteError'] = float(np.abs(
                image(records[0], crop=False)-image(records[1], crop=False)).max())
            require(metrics['meanAbsoluteByteError'] <= thresholds['nearInteriorMean'] and
                metrics['p99ByteError'] <= thresholds['nearInteriorP99'] and
                metrics['fullImageMaximumByteError'] <= thresholds['nearMaximum'],
                f'Fixed precision envelope exceeded: {check["name"]}: {metrics}')
        else:
            require(metrics['meanAbsoluteByteError'] >= thresholds['contrastInteriorMean'] and
                metrics['changedFraction'] >= thresholds['contrastChangedFraction'],
                f'Vacuous visual control: {check["name"]}: {metrics}')
        return metrics
    if kind == 'blue':
        means = image(records[0]).mean(axis=(0, 1))
        require(means[2] > means[0] + 3 and means[2] > 8, f'Independent blue response absent: {means}')
        return dict(interiorChannelMeans=means.tolist())
    if kind == 'linear':
        actual, high, low = [radiance(image(record)) for record in records]
        expected = low + check['fraction'] * (high-low)
        error = float(np.mean(np.abs(actual-expected)))
        signal = float(np.mean(np.abs(high-low)))
        require(signal > .03, f'Vacuous radiance interpolation: {check["name"]}')
        require(error <= max(thresholds['diffuseAbsoluteRadianceError'], signal*thresholds['diffuseRelativeError']),
                f'Diffuse linear split failed: {check["name"]}: error {error}, signal {signal}')
        return dict(meanAbsoluteRadianceError=error, independentSignal=signal, fraction=check['fraction'])
    if kind == 'shadow':
        blocked, opened = [image(record) for record in records]
        center = (slice(51, 85), slice(61, 95))
        left = (slice(20, 116), slice(0, 15))
        shadow, clear = float(blocked[center].mean()), float(opened[center].mean())
        require(clear > 8 and shadow < clear*.5, f'Back light bypassed blocker: {shadow}/{clear}')
        require(float(blocked[left].mean()) > 3, 'Shadow control has no visible unoccluded receiver')
        return dict(blockedCenterMean=shadow, openCenterMean=clear, ratio=shadow/clear)
    if kind == 'environment':
        back, black_back, front, black_front = [radiance(image(record)) for record in records]
        # Opposite camera reverses screen X. The receiver is spatially uniform,
        # but explicitly remap the reference rather than assuming symmetry.
        actual = back-black_back
        floors = {'deferred-raster': .04, 'forward-raster': .025}
        require(records[0]['technique'] in floors, 'Environment oracle needs an explicit raster technique')
        ambient_floor = floors[records[0]['technique']]
        expected = np.flip(front-black_front, axis=1) - ambient_floor*np.array(COLOR)
        error, signal = float(np.mean(abs(actual-expected))), float(np.mean(abs(actual)))
        require(signal > .02 and error <= max(.03, signal*.05), f'Opposite-hemisphere oracle failed: {error}/{signal}')
        return dict(meanAbsoluteRadianceError=error, independentSignal=signal,
                    frontAmbientFloorSubtracted=ambient_floor, frontAoAppliedToBack=False)
    raise RuntimeError('Unknown comparison '+kind)


def check_record(technique, check, result):
    # An expected parameter may also be a measured-result label (fraction in
    # the linear oracle). A dict constructor with both **mappings raises even
    # when their values agree; allow that overlap without replacing expectations.
    for key in check.keys() & result.keys():
        require(check[key] == result[key], 'Comparison result changed expected field: '+key)
    return {'technique': technique, **check, **result, 'passed': True}


def verify_uv_anisotropy_axes(packed):
    directions = np.asarray(packed, dtype=float)[0, :2, :2]/255*2-1
    lengths = np.linalg.norm(directions, axis=1)
    require(np.all(lengths > 0), 'UV negative control contains a degenerate anisotropy direction')
    axes = directions/lengths[:, None]
    dot = float(abs(np.dot(axes[0], axes[1])))
    require(dot < .01, 'UV negative control must use distinct anisotropy axes, including sign equivalence')
    return dict(absoluteAxisDot=dot, axesIndependentOfVectorSign=True)


def shadow_execution_evidence(state, hard_ray):
    passes = {entry['name']: entry for entry in state['passes']}
    name = 'RayShadowTrace' if hard_ray else 'LocalShadowDepth'
    execution = passes.get(name, {})
    require(execution.get('active') and execution.get('gpuTimed') and execution.get('gpuMs', 0) > 0,
            'Shadow visibility has no active GPU timestamp: '+name)
    if hard_ray:
        ray = state['rayShadows']
        build = ray.get('build', {})
        require(ray['supported'] and ray['active'] and ray['mode'] == 1 and not ray['denoise'],
                'Hard ray-shadow visibility did not execute')
        require(build.get('instances', 0) > 0 and build.get('blasBuilt', 0)+build.get('blasReused', 0) > 0 and
                build.get('asAllocatedBytes', 0) > 0 and build.get('inputAllocatedBytes', 0) > 0,
                'Hard ray-shadow visibility has no positive TLAS/BLAS resources')
        return dict(mode='hard-ray', activeGpuPass=name, gpuMilliseconds=execution['gpuMs'],
                    instances=build['instances'], accelerationStructureBytes=build['asAllocatedBytes'])
    require(state['lighting']['pointLightCount'] == 1 and state['lighting']['activeLocalShadowLayers'] == 6,
            'Raster point shadow fixture did not allocate exactly its six cube faces')
    return dict(mode='raster', activeGpuPass=name, gpuMilliseconds=execution['gpuMs'], activeLocalShadowLayers=6)


def verify_coverage_models(models):
    """Prove sample-depth input and selection identity, independent of Vulkan."""
    require(set(models) == {'source', 'oracle', 'wrong'}, 'Coverage needs all three authored controls')
    source, oracle, wrong = (models[name] for name in ('source', 'oracle', 'wrong'))
    source_primitives, oracle_primitives, wrong_primitives = (
        model['meshes'][0]['primitives'] for model in (source, oracle, wrong))
    require(len(source['materials']) == 2 and source['materials'][0].get('doubleSided') is False and
            source['materials'][1].get('doubleSided') is True,
            'Coverage requires single-sided flagged material then a distinct late CPU NoCull plain owner')
    require([len(source_primitives), len(oracle_primitives), len(wrong_primitives)] == [3, 2, 2],
            'Coverage controls lost their explicit draw order')
    require(source_primitives[0] == oracle_primitives[0] == wrong_primitives[0],
            'Coverage first primitive/selection outline differs')
    require(source_primitives[1] == wrong_primitives[1] and source_primitives[2] == oracle_primitives[1],
            'Coverage reference uses different right triangles or materials')
    flagged, plain = copy.deepcopy(source_primitives[1]), copy.deepcopy(source_primitives[2])
    require(flagged.pop('material') == 0 and plain.pop('material') == 1 and flagged == plain,
            'Coverage overwrite must differ only in material, with identical attributes/indices/mode')
    # Compare every retained metadata table and byte-encoded source buffer.
    # Only the ordered primitive lists may differ between these documents.
    metadata = []
    for model in (source, oracle, wrong):
        normalized = copy.deepcopy(model)
        normalized['meshes'][0]['primitives'] = []
        metadata.append(normalized)
    require(metadata[0] == metadata[1] == metadata[2],
            'Coverage source/reference metadata, accessors, buffers or materials differ')
    require(source_primitives[0]['attributes']['POSITION'] != source_primitives[1]['attributes']['POSITION'],
            'Coverage partition has no independent left/right vertex streams')
    require(source_primitives[1]['indices'] == source_primitives[2]['indices'],
            'Coverage duplicate changed triangle indexing')
    return dict(sharedRasterInputs=True, retainedMetadataIdentical=True,
                commonFirstSelectedPrimitive=True, orderedPrimitiveCounts=[3, 2, 2],
                overwriteChangesOnlyMaterial=True,
                flaggedRoute='primary/GPU', plainOwnerRoute='late CPU NoCull',
                reliesOnAuthoredOrCompactedDrawOrder=False)


def verify_generated_coverage(plan):
    groups = {}
    for name, entry in plan['fixtures'].items():
        if entry['spec'].get('coverage'):
            key = name.rsplit('-', 1)[0]
            groups.setdefault(key, {})[entry['spec']['coverage']] = load_asset(Path(entry['path']))[0]
    return [dict(name=name, **verify_coverage_models(models)) for name, models in sorted(groups.items())]


def report_merge_self_check():
    """Exercise every real compare result shape, without images or a renderer.

    Synthetic in-memory values isolate report construction. Image loading and
    display inversion are intentionally replaced here; this is a smoke check
    for comparison/report plumbing, not native rendering or oracle accuracy.
    """
    from unittest.mock import patch
    shape = (136, 156, 3)
    constant = lambda rgb: np.broadcast_to(np.array(rgb, dtype=float), shape).copy()
    blocked = constant([64, 64, 64])
    blocked[51:85, 61:95] = 0
    captures = {name: {'values': values, 'technique': 'forward-raster'} for name, values in {
        'black': constant([0, 0, 0]), 'blue': constant([0, 0, 64]),
        'half': constant([16, 16, 16]), 'full': constant([32, 32, 32]),
        'blocked': blocked, 'open': constant([64, 64, 64]),
        'env-back': constant(np.array(COLOR)*.2),
        'env-front': constant(np.array(COLOR)*.225),
    }.items()}
    specs = [
        dict(kind='exact', images=['black', 'black']),
        dict(kind='near', images=['black', 'black']),
        dict(kind='contrast', images=['blue', 'black']),
        dict(kind='blue', images=['blue']),
        dict(kind='linear', images=['half', 'full', 'black'], fraction=.5),
        dict(kind='shadow', images=['blocked', 'open']),
        dict(kind='environment', images=['env-back', 'black', 'env-front', 'black']),
    ]
    thresholds = build_plan(Path('self-check-does-not-write'), ('essential',))['thresholds']
    records = []
    with patch.dict(compare.__globals__, image=lambda record, **kwargs: record['values'],
                    radiance=lambda values: values):
        for spec in specs:
            check = {**spec, 'name': 'report-merge-'+spec['kind']}
            result = compare(check, captures, thresholds)
            record = check_record('pure-report-self-check', check, result)
            require(all(record[key] == value for key, value in check.items()), 'Expected check fields were overwritten')
            require(all(record[key] == value for key, value in result.items()), 'Comparison metrics were omitted')
            require(record['passed'] and record['technique'] == 'pure-report-self-check', 'Report identity was lost')
            json.dumps(record, allow_nan=False)
            records.append(record)
        environment_check = dict(kind='environment', name='deferred-environment-floor',
            images=['env-back', 'black', 'env-front', 'black'])
        deferred_captures = copy.deepcopy(captures)
        for record in deferred_captures.values():
            record['technique'] = 'deferred-raster'
        deferred_captures['env-front']['values'] = constant(np.array(COLOR)*.24)
        deferred_environment = compare(environment_check, deferred_captures, thresholds)
        require(deferred_environment['frontAmbientFloorSubtracted'] == .04 and
                deferred_environment['meanAbsoluteRadianceError'] < 1e-12,
                'Deferred environment oracle did not subtract its declared .04 floor')
    linear = next(record for record in records if record['kind'] == 'linear')
    require(linear['fraction'] == .5, 'Duplicate fraction was lost')
    rejected_conflict = False
    try:
        check_record('pure-report-self-check', {'fraction': .5}, {'fraction': .75})
    except RuntimeError:
        rejected_conflict = True
    require(rejected_conflict, 'Conflicting result silently replaced an expected fraction')
    invalid_logs = ['VUID-vkCmdPipelineBarrier2-01234', 'SYNC-HAZARD-WRITE-AFTER-WRITE',
        'Validation Error: invalid image',
        'validation layer: vkCmdPipelineBarrier2(): WRITE_AFTER_WRITE hazard detected.',
        'READ_AFTER_WRITE synchronization hazard', 'WRITE_AFTER_READ hazard',
        'READ_AFTER_READ hazard', 'Image hazard detected']
    valid_logs = ['Renderer initialized', 'No synchronization hazards detected',
        'Image hazard detector initialized', 'WRITE_AFTER_WRITE symbolic access enumerated']
    require(all(validation_log_error(text) for text in invalid_logs) and
            not any(validation_log_error(text) for text in valid_logs),
            'Validation log scanner missed an actual hazard or rejected benign status')
    uv_plan = build_plan(Path('self-check-does-not-write'), ('uv-mirror',))
    axis_proof = verify_uv_anisotropy_axes(
        uv_plan['fixtures']['uv-transform']['spec']['textures'][f'{ANISO}/anisotropyTexture'])
    opposite_axes_rejected = False
    try:
        verify_uv_anisotropy_axes([[[255, 0, 255, 255], [0, 255, 255, 255]]]*2)
    except RuntimeError:
        opposite_axes_rejected = True
    require(opposite_axes_rejected, 'UV axis proof accepted equivalent opposite directions')
    raster_shadow = dict(lighting=dict(pointLightCount=1, activeLocalShadowLayers=6), passes=[
        dict(name='LocalShadowDepth', active=True, gpuTimed=True, gpuMs=.01)])
    hard_shadow = dict(rayShadows=dict(supported=True, active=True, mode=1, denoise=False,
        build=dict(instances=2, blasBuilt=2, blasReused=0, asAllocatedBytes=4096, inputAllocatedBytes=256)),
        passes=[dict(name='RayShadowTrace', active=True, gpuTimed=True, gpuMs=.01)])
    shadow_execution_evidence(raster_shadow, False)
    shadow_execution_evidence(hard_shadow, True)
    rejected_shadow_mutations = []
    for mutation in ('raster-layers', 'raster-timestamp', 'ray-AS', 'ray-timestamp'):
        hard = mutation.startswith('ray')
        changed = copy.deepcopy(hard_shadow if hard else raster_shadow)
        if mutation == 'raster-layers':
            changed['lighting']['activeLocalShadowLayers'] = 0
        elif mutation == 'ray-AS':
            changed['rayShadows']['build']['asAllocatedBytes'] = 0
        else:
            changed['passes'][0]['gpuTimed'] = False
        try:
            shadow_execution_evidence(changed, hard)
        except RuntimeError:
            rejected_shadow_mutations.append(mutation)
    require(len(rejected_shadow_mutations) == 4, 'Shadow execution proof accepted requested-only or missing GPU work')
    # Export inspection currently returns no technique key; dictionary expansion
    # also keeps that report merge robust to an identical future label.
    exported = {'technique': 'pure-report-self-check', **dict(
        portablePointerCount=4, liveExtendedTextureInfos=3)}
    require(exported['portablePointerCount'] == 4 and exported['liveExtendedTextureInfos'] == 3,
            'Export inspection metrics were omitted')
    # The coverage contract is tested with actual generated accessor/buffer
    # documents. Temporary fixtures never invoke the renderer or touch native
    # evidence. A deliberate index mutation must invalidate the proof.
    import tempfile
    with tempfile.TemporaryDirectory(prefix='container-pbr92-coverage-') as temporary:
        coverage_plan = build_plan(Path(temporary), ('coverage',))
        for entry in coverage_plan['fixtures'].values():
            fixture(Path(entry['path']), entry['spec'])
        coverage_proofs = verify_generated_coverage(coverage_plan)
        first_group = {version: load_asset(Path(coverage_plan['fixtures'][f'coverage-4-{version}']['path']))[0]
                       for version in ('source', 'oracle', 'wrong')}
        first_group['source']['meshes'][0]['primitives'][1]['indices'] = -1
        rejected_geometry_mutation = False
        try:
            verify_coverage_models(first_group)
        except RuntimeError:
            rejected_geometry_mutation = True
        require(rejected_geometry_mutation, 'Coverage identity proof accepted changed triangle indices')
        route_group = {version: load_asset(Path(coverage_plan['fixtures'][f'coverage-4-{version}']['path']))[0]
                       for version in ('source', 'oracle', 'wrong')}
        for model in route_group.values():
            model['materials'][1]['doubleSided'] = False
        rejected_route_mutation = False
        try:
            verify_coverage_models(route_group)
        except RuntimeError:
            rejected_route_mutation = True
        require(rejected_route_mutation, 'Coverage identity proof accepted an unordered primary-route owner')
        export_plan = build_plan(Path(temporary), ('animation-export',))
        source_entry = export_plan['fixtures']['animated-save-glb']
        source_path = Path(source_entry['path'])
        source_model = fixture(source_path, source_entry['spec'])
        synthetic_export = copy.deepcopy(source_model)
        for extension, member in EXTENDED_TEXTURE_INFOS:
            info = synthetic_export['materials'][0]['extensions'][extension][member]
            info.update(expected_export_texture_metadata(info))
        synthetic_path = Path(temporary)/'synthetic-runtime-float32-export.gltf'
        synthetic_path.write_text(json.dumps(synthetic_export), encoding='utf-8')
        inspected_export = inspect_export(source_path, synthetic_path)
        require(inspected_export['liveExtendedTextureInfos'] == 3,
                'Serialized float32 export inspection omitted texture slots')
        rejected_export_mutations = []
        for mutation in ('rotation', 'extras', 'extension'):
            changed = copy.deepcopy(synthetic_export)
            info = changed['materials'][0]['extensions'][ANISO]['anisotropyTexture']
            if mutation == 'rotation':
                transform = info['extensions'][TRANSFORM]
                transform['rotation'] = float(np.nextafter(np.float32(transform['rotation']), np.float32(1.)))
            elif mutation == 'extras':
                info['extras']['sourceSlot'] = 'changed source identity'
            else:
                info['extensions'][TRANSFORM]['unexpectedMetadata'] = True
            synthetic_path.write_text(json.dumps(changed), encoding='utf-8')
            try:
                inspect_export(source_path, synthetic_path)
            except RuntimeError:
                rejected_export_mutations.append(mutation)
        require(rejected_export_mutations == ['rotation', 'extras', 'extension'],
                'Export inspection accepted changed binary32 rotation or exact metadata')
    return dict(pureReportSmokeCheck=True, nativeValidationPerformed=False,
                resultShapes=[record['kind'] for record in records],
                resultShapeCount=len(records), conflictingExpectationRejected=True,
                coverageIdentityProofs=coverage_proofs, changedTriangleIndicesRejected=True,
                unorderedOwnerRouteRejected=True,
                serializedFloat32ExportInspected=True, exportMutationsRejected=rejected_export_mutations,
                uvAxisProof=axis_proof, equivalentOppositeAxesRejected=opposite_axes_rejected,
                requestedOnlyShadowMutationsRejected=rejected_shadow_mutations,
                validationLogErrorsRejected=len(invalid_logs), benignValidationStatusAccepted=len(valid_logs),
                environmentFloorCases={'deferred-raster': .04, 'forward-raster': .025})


EXTENDED_TEXTURE_INFOS = ((ANISO, 'anisotropyTexture'), (DIFFUSE, 'diffuseTransmissionTexture'),
                          (DIFFUSE, 'diffuseTransmissionColorTexture'))


def expected_export_texture_metadata(info):
    """Model runtime binary32 TRS and effective UV selection without a tolerance.

    The exporter writes runtime float fields as JSON doubles. All remaining
    extension fields and extras retain exact recursive source equality.
    Resource indices are checked through sampler/image identity separately.
    """
    expected = copy.deepcopy(info)
    expected.pop('index', None)
    expected['texCoord'] = expected.get('texCoord', 0)
    transform = expected.get('extensions', {}).get(TRANSFORM)
    if transform is not None:
        expected['texCoord'] = transform.pop('texCoord', expected['texCoord'])
        for field, default in (('offset', [0., 0.]), ('scale', [1., 1.]), ('rotation', 0.)):
            value = transform.get(field, default)
            transform[field] = [float(np.float32(v)) for v in value] if isinstance(value, list) else float(np.float32(value))
    return expected


def inspect_export(source, destination):
    before, before_buffers = load_asset(source)
    after, after_buffers = load_asset(destination)
    bound = {primitive.get('material', -1) for entry in after['meshes'] for primitive in entry['primitives']}
    matches = [(index, after['materials'][index]) for index in bound if index >= 0 and
               after['materials'][index].get('name') == before['materials'][0]['name']]
    require(len(matches) == 1, 'Live exported extended material binding was lost')
    bound_index, bound_material = matches[0]
    actual, expected = bound_material['extensions'], before['materials'][0]['extensions']

    def decoded_pixels(model, buffers, texture_index, path):
        entry = model['images'][model['textures'][texture_index]['source']]
        if 'bufferView' in entry:
            view = model['bufferViews'][entry['bufferView']]
            offset = view.get('byteOffset', 0)
            payload = buffers[view['buffer']][offset:offset+view['byteLength']]
        else:
            uri = entry['uri']
            payload = base64.b64decode(uri.split(',', 1)[1]) if uri.startswith('data:') else (path.parent/uri).read_bytes()
        with Image.open(io.BytesIO(payload)) as decoded:
            return np.asarray(decoded.convert('RGBA'))
    for extension, member in EXTENDED_TEXTURE_INFOS:
        info, wanted = actual[extension][member], expected[extension][member]
        metadata = copy.deepcopy(info)
        metadata.pop('index', None)
        require(metadata == expected_export_texture_metadata(wanted),
                'Export changed extended UV/transform/source metadata: '+member)
        texture_index, source_index = info['index'], wanted['index']
        sampler = after['samplers'][after['textures'][texture_index]['sampler']]
        wanted_sampler = before['samplers'][before['textures'][source_index]['sampler']]
        require(sampler == wanted_sampler, 'Export changed an extended source sampler')
        require(np.array_equal(decoded_pixels(after, after_buffers, texture_index, destination),
                               decoded_pixels(before, before_buffers, source_index, source)),
                'Export aliased or changed an extended source image: '+member)
    pointers = [channel['target']['extensions']['KHR_animation_pointer']['pointer']
                for clip in after.get('animations', []) for channel in clip['channels']]
    expected_pointers = {pointer.replace('/materials/0/', f'/materials/{bound_index}/', 1) for pointer in POINTER_VALUES}
    require(len(pointers) == 4 and set(pointers) == expected_pointers, 'Export lost/remapped extended pointer tracks away from the live binding')
    require(all(extension in after.get('extensionsUsed', []) for extension in (ANISO, DIFFUSE, TRANSFORM, 'KHR_animation_pointer')),
            'Export lost required extension declarations')
    return dict(portablePointerCount=4, liveExtendedTextureInfos=3, uvTransformMetadataPreserved=True,
                transformNumericContract='exact source-to-runtime binary32 conversion',
                sourceSamplerIdentitiesPreserved=True, sourceImagePixelsPreserved=True,
                path=str(destination), sha256=sha(destination))


def launch(renderer, output, planned, fixtures, technique, captures, timeout, *, hard_ray=False):
    name = technique+'-'+planned['name']
    target = output / 'captures' / name
    target.parent.mkdir(parents=True, exist_ok=True)
    image_path, telemetry, log = target.with_suffix('.png'), target.with_suffix('.telemetry.json'), target.with_suffix('.log')
    image_path.unlink(missing_ok=True); telemetry.unlink(missing_ok=True)
    if planned.get('reload'):
        source = output / 'exports' / technique / ('animated.'+planned['reload'])
    else:
        source = Path(fixtures[planned['fixture']]['path'])
    command = [str(renderer), '--hidden', '--no-ui', '--validation', '--no-bloom', '--no-taa',
        '--width', str(WIDTH), '--height', str(HEIGHT), '--msaa', str(planned.get('msaa', 1)),
        '--warmup-frames', '12', '--capture-frame', '13', '--fixed-dt', '.016666667',
        '--render-technique', technique, '--display-mode', 'lit', '--exposure', '.35',
        '--camera-position', '0', '0', '-4' if planned.get('opposite') else '4',
        '--camera-target', '0', '0', '0', '--camera-fov', '45',
        '--environment-intensity', '1' if planned['lighting'] == 'environment' else '0',
        '--directional-intensity', '3' if planned['lighting'] == 'directional' else '0',
        '--directional-direction', '0', '0', '-1', '--area-lighting', planned.get('area_mode', 'sampled'),
        '--area-light-samples', '25', '--model', str(source), '--screenshot', str(image_path)]
    if hard_ray:
        command += ['--ray-shadows', 'hard', '--no-ray-shadow-denoise']
    else:
        command += ['--no-ray-query', '--ray-shadows', 'raster']
    if 'time' in planned:
        command += ['--animation-clip', '0', '--animation-time', str(planned['time']), '--animation-paused']
    if planned.get('save'):
        destination = output / 'exports' / technique / ('animated.'+planned['save'])
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.unlink(missing_ok=True)
        command += ['--save-scene', str(destination)]
    with log.open('w', encoding='utf-8') as stream:
        process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
            timeout=timeout, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    text = log.read_text(encoding='utf-8', errors='replace')
    require(process.returncode == 0, f'Native capture failed ({process.returncode}): {log}')
    require(not validation_log_error(text), f'Vulkan validation failed: {log}')
    require(image_path.is_file() and telemetry.is_file(), 'Missing fresh PNG/telemetry: '+str(log))
    state = json.loads(telemetry.read_text(encoding='utf-8'))
    require(finite_json(state) and state['technique'] == technique, 'Invalid/wrong-technique telemetry')
    require(state['msaaSamples'] == planned.get('msaa', 1), 'Requested MSAA path did not execute')
    require(state['scene']['primaryObjectCount'] >= 1, 'Fixture has no published receiver')
    require(state['lighting']['environmentIntensity'] == int(planned['lighting'] == 'environment'), 'Environment isolation changed')
    require(state['lighting']['directionalIntensity'] == (3 if planned['lighting'] == 'directional' else 0), 'Directional isolation changed')
    if planned['cohort'] != 'animation-export':
        require(state['lighting']['bounceIntensity'] == 0, 'Fixture lost isolated local-light scene defaults')
    shadow_evidence = shadow_execution_evidence(state, hard_ray) if hard_ray or planned['cohort'] == 'shadows' else None
    if planned['cohort'] == 'area':
        require(state['lighting']['areaLightCount'] == 1 and state['lighting']['pointLightCount'] == 0,
                'Area fixture did not publish exactly its authored area emitter')
        require(state['lighting']['areaLightingRequestedMode'] == int(planned.get('area_mode') == 'ltc'), 'Area mode toggle ignored')
    if planned['name'] == 'deformation-morph':
        require(state['gltfDeformation']['active'] and state['gltfDeformation']['gpuReady'], 'Morph test did not execute GPU deformation')
    if 'time' in planned:
        animation = state['gltfAnimation']
        require(animation['selectedClip'] == 0 and not animation['playing'] and animation['diagnostics'] == 0 and
                abs(animation['time']-planned['time']) < 1e-6, 'Extended animation seek/track acceptance failed')
        values = {row['property']: row['value'] for row in animation['targets']}
        for pointer, endpoints in POINTER_VALUES.items():
            property_name = pointer.split('/materials/0/')[1]
            expected = np.asarray(endpoints[0])*(1-planned['time'])+np.asarray(endpoints[1])*planned['time']
            require(property_name in values and np.allclose(np.asarray(values[property_name]).reshape(-1),
                np.asarray(expected).reshape(-1), atol=2e-6, rtol=0), 'Animated extended target value is stale: '+pointer)
    record = dict(name=planned['name'], technique=technique, image=str(image_path), telemetry=str(telemetry),
        log=str(log), command=command, imageSha256=sha(image_path), telemetrySha256=sha(telemetry),
        logSha256=sha(log), validationClean=True)
    if shadow_evidence:
        record['shadowExecution'] = shadow_evidence
    captures.append(record)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--technique', action='append', choices=TECHNIQUES)
    parser.add_argument('--cohort', action='append', choices=(*COHORTS, 'all'))
    parser.add_argument('--hard-ray', action='store_true', help='Run selected cohort with hard ray shadows instead of raster shadows')
    parser.add_argument('--timeout', type=float, default=240)
    parser.add_argument('--generate-only', action='store_true')
    parser.add_argument('--self-check', action='store_true', help='Check report shapes and temporary coverage fixture identities without a renderer')
    args = parser.parse_args()
    if args.self_check:
        print(json.dumps(report_merge_self_check(), indent=2))
        return
    if args.renderer is None or args.output is None:
        parser.error('--renderer and --output are required unless --self-check is selected')
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    cohorts = COHORTS if 'all' in (args.cohort or []) else tuple(dict.fromkeys(args.cohort or ['essential']))
    techniques = tuple(dict.fromkeys(args.technique or TECHNIQUES))
    plan = build_plan(output, cohorts)
    report = dict(schemaVersion=1, passed=None if args.generate_only else False,
        generatedOnly=args.generate_only, nativeValidationPerformed=False, renderer=str(renderer),
        rendererSha256=None, cohorts=list(cohorts), captures=[], checks=[], exports=[], fixtures=[],
        expectedCaptureCount=len(plan['captures'])*len(techniques),
        expectedImageCheckCount=len(plan['checks'])*len(techniques),
        expectedExportChecks=2*len(techniques) if 'animation-export' in cohorts else 0,
        sourceSha256=sha(Path(__file__)), hardRay=args.hard_ray,
        unverified=['No GPU material-buffer or picking readback is claimed.',
            'Anisotropic IBL is a bent-normal approximation; direct shared math has its separate independent GPU contract.',
            'Transparent environment uses its existing forward path; backside DDGI transport is not exercised here.',
            'No skinning case in this bounded harness; the deformation cohort isolates GPU morph ownership.',
            'No volume/subsurface diffusion or spectral rendering oracle is claimed.'])

    def persist():
        report.update(captureCount=len(report['captures']), checkCount=len(report['checks']), exportCheckCount=len(report['exports']))
        (output/'results.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')

    try:
        for name, entry in plan['fixtures'].items():
            path = Path(entry['path'])
            fixture(path, entry['spec'])
            report['fixtures'].append(dict(name=name, path=str(path), sha256=sha(path)))
        report['coverageFixtureIdentityProofs'] = verify_generated_coverage(plan)
        (output/'launch-plan.json').write_text(json.dumps(plan, indent=2)+'\n', encoding='utf-8')
        report['launchPlanSha256'] = sha(output/'launch-plan.json')
        persist()
        if args.generate_only:
            return
        require(renderer.is_file() and args.manifest is not None, 'Native execution requires renderer and frozen manifest')
        report['rendererSha256'] = sha(renderer)
        report['nativeManifestBefore'] = verify_manifest(args.manifest)
        frozen = json.loads(args.manifest.read_text(encoding='utf-8-sig'))['files']
        require(any(Path(name).resolve() == renderer for name in frozen), 'Manifest does not bind this executable')
        required_spv = {'extended_material_aux.frag.spv', 'forward_opaque.frag.spv', 'forward_transparent.frag.spv'}
        require(required_spv <= {Path(name).name for name in frozen}, 'Manifest omits extended material shader consumers')
        if 'environment' in cohorts:
            environment = renderer.parent/'hdr/citrus_orchard_road_puresky_4k.exr'
            require(environment.is_file(), 'Missing pinned default environment input')
            report['environment'] = dict(path=str(environment), sha256=sha(environment))
        (output/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n', encoding='utf-8')
        for technique in techniques:
            records = {}
            for capture in plan['captures']:
                records[capture['name']] = launch(renderer, output, capture, plan['fixtures'], technique,
                    report['captures'], args.timeout, hard_ray=args.hard_ray)
                if capture.get('save'):
                    source = Path(plan['fixtures'][capture['fixture']]['path'])
                    destination = output/'exports'/technique/('animated.'+capture['save'])
                    report['exports'].append({'technique': technique, **inspect_export(source, destination)})
                persist()
            for check in plan['checks']:
                result = compare(check, records, plan['thresholds'])
                report['checks'].append(check_record(technique, check, result))
                persist()
        require(len(report['captures']) == report['expectedCaptureCount'] and
            len(report['checks']) == report['expectedImageCheckCount'] and
            len(report['exports']) == report['expectedExportChecks'], 'Native regression matrix is incomplete')
        require(sha(renderer) == report['rendererSha256'], 'Renderer changed during native work')
        require(sha(Path(__file__)) == report['sourceSha256'] and
                sha(output/'launch-plan.json') == report['launchPlanSha256'], 'Harness or expected plan changed during native work')
        require(all(sha(Path(entry['path'])) == entry['sha256'] for entry in report['fixtures']), 'Source fixture changed during native work')
        if 'environment' in report:
            require(sha(Path(report['environment']['path'])) == report['environment']['sha256'], 'Environment input changed during native work')
        report['nativeManifestAfter'] = verify_manifest(args.manifest)
        report['nativeValidationPerformed'] = True
        report['passed'] = True
    except Exception as error:
        report['failure'] = str(error)
        if report.get('nativeManifestBefore') and args.manifest:
            try:
                report['nativeManifestAfter'] = verify_manifest(args.manifest)
            except Exception as manifest_error:
                report['manifestAfterFailure'] = str(manifest_error)
        raise
    finally:
        persist()
        print('GENERATED' if args.generate_only else 'NATIVE', report['captureCount'], 'captures,',
              report['checkCount'], 'image checks,', report['exportCheckCount'], 'export checks;',
              'planned', report['expectedCaptureCount'], 'captures', flush=True)


if __name__ == '__main__':
    main()
