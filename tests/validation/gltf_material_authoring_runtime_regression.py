"""Native material editing, current-clock history and portable-save acceptance.

This file generates independent literal material controls. It never calls the
production authoring planner or shader evaluator. Generation/self-check launch
no renderer. Native runs require a frozen executable, SPIR-V and transitive
Python manifest, checked before/after every process and on failure.

CPU authored/evaluated records and the CPU hash of the uploaded packed entry
are identified as CPU evidence. Images and named GPU timestamps are separate
evidence; this harness does not claim GPU material-buffer readback.
"""
from __future__ import annotations

import argparse
import copy
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

from gltf_accessor_runtime_regression import Asset, decode_accessor, load_asset, require
from gltf_extended_materials_runtime_regression import sha, validation_log_error
from gltf_merge_runtime_regression import document, immutable, portable_copy
from gltf_tangent_uv_runtime_regression import finite_json
from gltf_texture_formats_runtime_regression import verify_manifest
from scene_node_authoring_runtime_regression import history_cursor

TECHNIQUES = ('deferred-raster', 'forward-raster')
COHORTS = ('core', 'animation', 'gesture', 'ray-ddgi', 'invalid')
WIDTH, HEIGHT, INVALID = 640, 360, 2**32-1
PREFIX = 'extensions/KHR_materials_'
TRANSMISSION, ANISOTROPY, DIFFUSE, UNLIT = [
    'KHR_materials_'+name for name in ('transmission', 'anisotropy', 'diffuse_transmission', 'unlit')]
POINTER = 'KHR_animation_pointer'
CORE = [('before', 13), ('factors', 26), ('undo', 39), ('redo', 52), ('mask', 65), ('blend', 78),
    ('sided', 91), ('zero', 104), ('tiny', 117), ('anisotropy', 130), ('undo-anisotropy', 143),
    ('redo-anisotropy', 156), ('unlit', 169), ('undo-unlit', 182)]
ANIMATION = [('before', 13), ('edit', 26), ('seek', 39), ('undo', 52), ('redo', 65),
    ('rest', 78), ('undo-rest', 91), ('redo-rest', 104), ('reselect', 117)]
GESTURE = [('before', 13), ('begin', 26), ('update', 39), ('end', 52), ('undo', 65),
    ('redo', 78), ('cancel-begin', 91), ('cancel', 104), ('undo-final', 117),
    ('remove', 130), ('recreate', 143), ('boundary-end', 156), ('boundary-undo', 169), ('boundary-redo', 182)]
RAY = [('before', 26), ('edited', 52), ('undo', 78), ('redo', 104)]
ROI = [(230, 163, 248, 197), (392, 163, 410, 197)]
UNVERIFIED = [
    'Generated inputs and self-checks are not native renderer acceptance.',
    'Material records, render classifications and uploadedMaterialHash are CPU publication evidence; no direct GPU buffer readback is claimed.',
    'Interior image controls exclude selection outlines; no image predicate infers hidden GPU resource ownership.',
    'Rejected capture requests prove an error before the requested mutation; no postfailure live-state claim is made without an actual after-capture.',
    'Texture rebinding, material duplication, procedural materialization and arbitrary external material formats are outside this existing-source edit cohort.',
    'The ray/DDGI cohort proves execution with zero GI intensity, not indirect-light image accuracy.']


def close(actual, expected, label, tolerance=3e-6):
    a, b = np.asarray(actual, dtype=float), np.asarray(expected, dtype=float)
    require(a.shape == b.shape and np.isfinite(a).all() and np.isfinite(b).all() and
        np.max(np.abs(a-b), initial=0.) <= tolerance, 'Incorrect material '+label)


def base_material():
    return dict(name='Shared source material', extras=dict(authoringFixture='shared-material', opaqueIndex=16777217,
        nested=dict(textureLike=dict(index=977))), pbrMetallicRoughness=dict(baseColorFactor=[.8, .2, .1, 1.],
        baseColorTexture=dict(index=0, extras=dict(opaqueTextureIndex=991)), metallicFactor=0., roughnessFactor=.6),
        normalTexture=dict(index=1, scale=1., extras=dict(normalTag='retained')), emissiveFactor=[0., 0., 0.],
        alphaMode='OPAQUE', alphaCutoff=.5, doubleSided=False, extensions={
            TRANSMISSION: dict(transmissionFactor=0., transmissionTexture=dict(index=0, extras=dict(slotTag='transmission'))),
            'KHR_materials_clearcoat': dict(clearcoatFactor=0., clearcoatRoughnessFactor=.25,
                clearcoatTexture=dict(index=0, extras=dict(slotTag='clearcoat')), extras=dict(vendorParameter=733))})


# Literal values are used independently for direct source controls and expected
# records. The checker never asks the renderer's planner to construct an oracle.
FACTOR_VALUES = {
    'pbrMetallicRoughness/baseColorFactor': [.15, .75, .25, 1.],
    'pbrMetallicRoughness/metallicFactor': .15,
    'pbrMetallicRoughness/roughnessFactor': .35,
    'normalTexture/scale': -.25,
    'emissiveFactor': [.04, .01, .02],
    PREFIX+'emissive_strength/emissiveStrength': 2.,
    PREFIX+'ior/ior': 1.33,
    PREFIX+'dispersion/dispersion': .2,
    PREFIX+'transmission/transmissionFactor': 0.,
    PREFIX+'volume/thicknessFactor': .2,
    PREFIX+'volume/attenuationDistance': 3.,
    PREFIX+'volume/attenuationColor': [.8, .9, 1.],
    PREFIX+'clearcoat/clearcoatFactor': .6,
    PREFIX+'clearcoat/clearcoatRoughnessFactor': .2,
    PREFIX+'sheen/sheenColorFactor': [.1, .2, .3],
    PREFIX+'sheen/sheenRoughnessFactor': .4,
    PREFIX+'specular/specularFactor': .35,
    PREFIX+'specular/specularColorFactor': [.9, .7, .4],
    PREFIX+'iridescence/iridescenceFactor': .25,
    PREFIX+'iridescence/iridescenceIor': 1.4,
    PREFIX+'iridescence/iridescenceThicknessMinimum': 100.,
    PREFIX+'iridescence/iridescenceThicknessMaximum': 400.}


def direct_factors():
    value = base_material(); value['name'] = 'Edited source material \u6771'
    value['pbrMetallicRoughness'].update(baseColorFactor=[.15, .75, .25, 1.], metallicFactor=.15, roughnessFactor=.35)
    value['normalTexture']['scale'] = -.25; value['emissiveFactor'] = [.04, .01, .02]
    value['extensions'].update({
        'KHR_materials_emissive_strength': dict(emissiveStrength=2.), 'KHR_materials_ior': dict(ior=1.33),
        'KHR_materials_dispersion': dict(dispersion=.2), 'KHR_materials_volume': dict(thicknessFactor=.2,
            attenuationDistance=3., attenuationColor=[.8, .9, 1.]),
        'KHR_materials_sheen': dict(sheenColorFactor=[.1, .2, .3], sheenRoughnessFactor=.4),
        'KHR_materials_specular': dict(specularFactor=.35, specularColorFactor=[.9, .7, .4]),
        'KHR_materials_iridescence': dict(iridescenceFactor=.25, iridescenceIor=1.4,
            iridescenceThicknessMinimum=100., iridescenceThicknessMaximum=400.)})
    value['extensions']['KHR_materials_clearcoat'].update(clearcoatFactor=.6, clearcoatRoughnessFactor=.2)
    return value


def expected_material(label):
    if label in ('before', 'undo'): return base_material()
    value = direct_factors()
    if label in ('factors', 'redo'): return value
    if label == 'mask':
        value['alphaMode'] = 'MASK'; value['alphaCutoff'] = .6
        value['pbrMetallicRoughness']['baseColorFactor'][3] = .4
        return value
    if label in ('blend', 'sided'):
        value['alphaMode'] = 'BLEND'; value['alphaCutoff'] = .6
        value['pbrMetallicRoughness']['baseColorFactor'][3] = .4
        value['doubleSided'] = label == 'sided'; return value
    value['alphaCutoff'] = .6; value['doubleSided'] = True
    if label in ('tiny', 'undo-anisotropy'):
        value['extensions'][TRANSMISSION]['transmissionFactor'] = .0005; return value
    if label == 'zero': return value
    value['extensions'][ANISOTROPY] = dict(anisotropyStrength=.7, anisotropyRotation=math.pi/4.)
    value['extensions'][DIFFUSE] = dict(diffuseTransmissionFactor=0., diffuseTransmissionColorFactor=[0., 0., 0.])
    if label == 'unlit':
        value['extensions'] = {UNLIT: {}}
    return value


def edit(frame, values=None, *, source=0, extensions=None, phase=None, gesture=None, **metadata):
    payload = dict(sourceMaterial=source)
    if values is not None: payload['values'] = [dict(property=key, value=value) for key, value in values.items()]
    if extensions is not None: payload['extensions'] = [dict(extension=name, enabled=enabled) for name, enabled in extensions.items()]
    if phase is not None: payload['gesturePhase'] = phase
    if gesture is not None: payload['gestureId'] = gesture
    payload.update(metadata); return dict(frame=frame, materialEdit=payload)


def core_events():
    profile = edit(14, FACTOR_VALUES, name='Edited source material \u6771')
    incompatible = {name: False for name in expected_material('anisotropy')['extensions']}
    return [profile, dict(frame=27, sceneHistory='undo'), dict(frame=40, sceneHistory='redo'),
        edit(53, {'pbrMetallicRoughness/baseColorFactor': [.15, .75, .25, .4], 'alphaCutoff': .6}, alphaMode='MASK'),
        edit(66, alphaMode='BLEND'), edit(79, doubleSided=True),
        edit(92, {'pbrMetallicRoughness/baseColorFactor': [.15, .75, .25, 1.], PREFIX+'transmission/transmissionFactor': 0.}, alphaMode='OPAQUE'),
        edit(105, {PREFIX+'transmission/transmissionFactor': .0005}),
        edit(118, {PREFIX+'transmission/transmissionFactor': 0., PREFIX+'anisotropy/anisotropyStrength': .7,
            PREFIX+'anisotropy/anisotropyRotation': math.pi/4., PREFIX+'diffuse_transmission/diffuseTransmissionFactor': 0.,
            PREFIX+'diffuse_transmission/diffuseTransmissionColorFactor': [0., 0., 0.]}),
        dict(frame=131, sceneHistory='undo'), dict(frame=144, sceneHistory='redo'),
        edit(157, extensions={**incompatible, UNLIT: True}), dict(frame=170, sceneHistory='undo')]


def sequence(labels, events):
    return dict(schemaVersion=1, frames=labels[-1][1], sampleFrames=[frame for _, frame in labels], events=events,
        camera=[dict(frame=1, position=[0., 0., 6.], target=[0., 0., 0.])])


def write_fixture(path, material=None, *, animated=False):
    asset = Asset(); m = copy.deepcopy(material if material is not None else base_material())
    asset.image(np.tile(np.asarray([255, 255, 255, 255], dtype=np.uint8), (2, 2, 1)))
    asset.image(np.tile(np.asarray([128, 128, 255, 255], dtype=np.uint8), (2, 2, 1)))
    asset.model['materials'] = [m]
    points = [[-.65, -.8, 0.], [.65, -.8, 0.], [.65, .8, 0.], [-.65, .8, 0.]]
    attrs = dict(POSITION=asset.accessor('VEC3', points), NORMAL=asset.accessor('VEC3', [[0., 0., 1.]]*4),
        TANGENT=asset.accessor('VEC4', [[1., 0., 0., 1.]]*4),
        TEXCOORD_0=asset.accessor('VEC2', [[0., 0.], [1., 0.], [1., 1.], [0., 1.]]))
    primitive = dict(attributes=attrs, indices=asset.accessor('SCALAR', [0, 1, 2, 0, 2, 3], 5123, indices=True),
        material=0, mode=4, extras=dict(authoringPrimitive='shared-definition'))
    asset.model['meshes'] = [dict(primitives=[primitive], extras=dict(authoringMesh='shared-mesh'))]
    asset.model['nodes'] = [dict(name='Left shared owner', mesh=0, translation=[-1.1, 0., 0.]),
        dict(name='Right shared owner', mesh=0, translation=[1.1, 0., 0.])]
    asset.model['scenes'] = [dict(name='Shared bindings', nodes=[0, 1])]
    asset.model['extensionsUsed'] = list(m.get('extensions', {}))
    if TRANSMISSION in m.get('extensions', {}): asset.model['extensionsRequired'] = [TRANSMISSION]
    if animated:
        times = asset.accessor('SCALAR', [0., 1.]); asset.model['accessors'][times].update(min=[0.], max=[1.])
        output = asset.accessor('VEC4', [[.8, .2, .1, 1.], [.2, .4, .8, 1.]])
        metal = asset.accessor('SCALAR', [.1, .7])
        asset.model['animations'] = [dict(name='Absolute base-color curve', extras=dict(authoringCurve='unchanged'),
            samplers=[dict(input=times, output=output, interpolation='LINEAR'), dict(input=times, output=metal, interpolation='LINEAR')],
            channels=[dict(sampler=0, target=dict(path='pointer', extensions={POINTER:
                dict(pointer='/materials/0/pbrMetallicRoughness/baseColorFactor')})),
                dict(sampler=1, target=dict(path='pointer', extensions={POINTER:
                    dict(pointer='/materials/0/pbrMetallicRoughness/metallicFactor')}))])]
        asset.model['extensionsUsed'].append(POINTER)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='material-authoring-fixture-') as folder:
        generated = Path(folder)/'scene.gltf'; asset.write(generated); immutable(path, generated.read_bytes())


def generate(output, cohort):
    output.mkdir(parents=True, exist_ok=True)
    sources = {name: output/'sources'/(name+'.gltf') for name in ('source', 'animated', 'factors', 'zero', 'anisotropy', 'unlit')}
    write_fixture(sources['source']); write_fixture(sources['animated'], animated=True)
    for label in ('factors', 'zero', 'anisotropy', 'unlit'): write_fixture(sources[label], expected_material(label))
    for name, material in [('curve-control', animation_expected('reselect')[1]), ('rest-control', animation_expected('rest')[0])]:
        sources[name] = output/'sources'/(name+'.gltf'); write_fixture(sources[name], material)
    final_values = {**FACTOR_VALUES, 'alphaCutoff': .6, PREFIX+'anisotropy/anisotropyStrength': .7,
        PREFIX+'anisotropy/anisotropyRotation': math.pi/4., PREFIX+'diffuse_transmission/diffuseTransmissionFactor': 0.,
        PREFIX+'diffuse_transmission/diffuseTransmissionColorFactor': [0., 0., 0.]}
    animation_patch = {'pbrMetallicRoughness/baseColorFactor': [.1, .9, .2, 1.],
        'pbrMetallicRoughness/roughnessFactor': .2, 'emissiveFactor': [.04, .01, .02]}
    scripts = {
        'core': sequence(CORE, core_events()),
        'ascii': sequence([('ascii', 13)], [edit(2, final_values, name='Edited source material \u6771', doubleSided=True)]),
        'animation': sequence(ANIMATION, [edit(14, animation_patch), dict(frame=27, animationTime=1.),
            dict(frame=40, sceneHistory='undo'), dict(frame=53, sceneHistory='redo'), dict(frame=66, animationClip=-1),
            dict(frame=79, sceneHistory='undo'), dict(frame=92, sceneHistory='redo'),
            dict(frame=105, animationClip=0, animationTime=.5, animationPlaying=False)]),
        'gesture': sequence(GESTURE, [edit(14, {'pbrMetallicRoughness/roughnessFactor': .25}, phase='begin', gesture=101),
            edit(27, {'pbrMetallicRoughness/roughnessFactor': .4}, phase='update', gesture=101),
            edit(40, phase='end', gesture=101), dict(frame=53, sceneHistory='undo'), dict(frame=66, sceneHistory='redo'),
            edit(79, {'pbrMetallicRoughness/roughnessFactor': .15}, phase='begin', gesture=102),
            edit(92, phase='cancel', gesture=102), dict(frame=105, sceneHistory='undo'),
            edit(118, extensions={'KHR_materials_clearcoat': False}, phase='begin', gesture=103),
            edit(131, {PREFIX+'clearcoat/clearcoatFactor': .3, PREFIX+'clearcoat/clearcoatRoughnessFactor': .4},
                extensions={'KHR_materials_clearcoat': True}, phase='update', gesture=103),
            edit(144, phase='end', gesture=103), dict(frame=157, sceneHistory='undo'), dict(frame=170, sceneHistory='redo')]),
        'ray': sequence(RAY, [edit(27, {'pbrMetallicRoughness/baseColorFactor': [.2, .6, .9, 1.]}),
            dict(frame=53, sceneHistory='undo'), dict(frame=79, sceneHistory='redo')]),
        'invalid-range': sequence([('invalid', 26)], [edit(14, {'pbrMetallicRoughness/roughnessFactor': 1.1})]),
        'invalid-property': sequence([('invalid', 26)], [edit(14, {'unknownNumericProperty': .5})]),
        'invalid-conflict': sequence([('invalid', 26)], [dict(frame=14, animationTime=.5,
            materialEdit=edit(1, {'pbrMetallicRoughness/roughnessFactor': .2})['materialEdit'])]),
        'invalid-source': sequence([('before', 13), ('invalid', 26)], [edit(14, {'pbrMetallicRoughness/roughnessFactor': .2}, source=999)])}
    script_paths = {}
    for name, content in scripts.items():
        path = output/'scripts'/(name+'.json'); document(path, content); script_paths[name] = str(path)
    inputs = [*sources.values(), *map(Path, script_paths.values())]
    plan = dict(schemaVersion=1, cohort=cohort, sources={k: str(v) for k, v in sources.items()}, scripts=script_paths,
        immutableInputs=[dict(path=str(path), byteLength=path.stat().st_size, sha256=sha(path)) for path in inputs])
    document(output/'launch-plan.json', plan); return plan


def deep_contains(actual, expected, label):
    require(isinstance(actual, dict), 'Missing material object '+label)
    for key, value in expected.items():
        require(key in actual, 'Missing material metadata '+label+'/'+key)
        if key == 'extras': require(actual[key] == value, 'Changed opaque material metadata '+label+'/'+key)
        elif isinstance(value, dict): deep_contains(actual[key], value, label+'/'+key)
        elif type(value) is int: require(type(actual[key]) is int and actual[key] == value, 'Changed exact material integer '+label+'/'+key)
        elif isinstance(value, list):
            require(isinstance(actual[key], list) and all(type(item) in (int, float) for item in actual[key]), 'Malformed material vector '+label+'/'+key)
            close(actual[key], value, label+'/'+key)
        elif isinstance(value, float):
            require(type(actual[key]) in (int, float), 'Malformed material number '+label+'/'+key)
            close(actual[key], value, label+'/'+key)
        else: require(actual[key] == value, 'Changed material metadata '+label+'/'+key)


def inspect_record(actual, expected):
    deep_contains(actual, expected, 'record')
    # Known extension removal must not be masked by subset comparison.
    known = lambda value: {name for name in value.get('extensions', {}) if name.startswith('KHR_materials_')}
    require(known(actual) == known(expected), 'Unexpected/retained removed material extension')
    def texture_paths(value):
        result = {key for key in ('normalTexture', 'occlusionTexture', 'emissiveTexture') if key in value}
        result.update('pbrMetallicRoughness/'+key for key in ('baseColorTexture', 'metallicRoughnessTexture') if key in value.get('pbrMetallicRoughness', {}))
        for name, extension in value.get('extensions', {}).items():
            if name.startswith('KHR_materials_'):
                result.update('extensions/'+name+'/'+key for key in extension if key.endswith('Texture'))
        return result
    require(texture_paths(actual) == texture_paths(expected), 'Unexpected/lost material texture container')
    return dict(passed=True, name=actual['name'], sourceTextureIndices=[actual['pbrMetallicRoughness']['baseColorTexture']['index'],
        actual['normalTexture']['index']], metadataPreserved=True)


def common_state(state):
    require(finite_json(state) and state['scene']['primaryObjectCount'] == 2, 'Material edit lost a shared geometry binding')
    merge = state['gltfMerge']; rows = merge.get('authoringNodes', [])
    require(not merge.get('authoringNodesTruncated', True) and len(rows) == merge['graphNodeCount'], 'Missing complete authoring graph telemetry')
    renderables = [row for row in rows if row.get('renderable') and not row.get('retired', False)]
    require(len(renderables) == 2 and len({row['materialIndex'] for row in renderables}) == 1,
        'Shared source material no longer has two bindings')
    return merge, rows, renderables


def graph_identity(state):
    merge, rows, _ = common_state(state)
    # These fields describe retained actual graph slots, not material values.
    return dict(graphIdentity=merge['graphIdentity'], graphNodeCount=merge['graphNodeCount'], authoringNodes=rows,
        activeRoots=merge['activeRoots'], sourceTables=merge['sourceTables'],
        vertexCount=state['scene']['primaryVertexCount'], indexCount=state['scene']['primaryIndexCount'])


def expected_properties(material):
    ext = material.get('extensions', {})
    return dict(transparent=material.get('alphaMode', 'OPAQUE') == 'BLEND' or
        ext.get(TRANSMISSION, {}).get('transmissionFactor', 0.) > 0.,
        alphaMasked=material.get('alphaMode', 'OPAQUE') == 'MASK', doubleSided=material.get('doubleSided', False),
        unlit=UNLIT in ext, specularGlossiness=False, heightScale=0.)


def material_state(state, before, authored, evaluated, *, history, pending=False, initial=False):
    if before is not None:
        require(graph_identity(state) == graph_identity(before), 'Material-only publication changed graph or geometry counts')
    history_cursor(state, *history, allow_uncreated=initial)
    telemetry = state['gltfMaterialAuthoring']
    require(all(type(telemetry[key]) is int and telemetry[key] >= 0 for key in ('publications', 'materialRevision', 'generationEpoch')),
        'Material publication counters must be unsigned integer telemetry')
    require(type(telemetry['pendingGesture']) is bool and telemetry['pendingGesture'] == pending, 'Wrong pending material gesture state')
    if initial:
        require(telemetry['publications'] == 0 and telemetry['sourceMaterial'] == -1 and telemetry['runtimeMaterial'] == INVALID,
            'Unedited material publication baseline is not empty')
        return dict(passed=True, baseline=True, evidence='CPU graph and publication telemetry')
    require(telemetry['sourceMaterial'] == 0 and type(telemetry['runtimeMaterial']) is int and telemetry['runtimeMaterial'] < INVALID,
        'Source/runtime material identity differs')
    runtime = {row['materialIndex'] for row in common_state(state)[2]}
    require(runtime == {telemetry['runtimeMaterial']}, 'Published runtime material is not the actual shared binding')
    inspect_record(telemetry['authored'], authored); inspect_record(telemetry['evaluated'], evaluated)
    for key, value in expected_properties(evaluated).items():
        require(telemetry['materialProperties'][key] == value, 'Wrong material render property '+key)
    transparent = expected_properties(evaluated)['transparent']
    assignments = state['gltfMaterialVariants']['assignments']
    require(len(assignments) == 2 and all(row['transparent'] == transparent and
        row['alphaMasked'] == expected_properties(evaluated)['alphaMasked'] and
        row['doubleSided'] == expected_properties(evaluated)['doubleSided'] for row in assignments), 'Wrong actual graph-binding material route')
    require(type(telemetry['uploadedMaterialHash']) is int and telemetry['uploadedMaterialHash'] > 0,
        'Missing CPU hash of uploaded packed material entry')
    if before is not None:
        require(telemetry['materialRevision'] > before['gltfMaterialAuthoring']['materialRevision'] and
            telemetry['generationEpoch'] > before['gltfMaterialAuthoring']['generationEpoch'], 'Material generations did not advance')
    return dict(passed=True, sourceMaterial=0, runtimeMaterial=telemetry['runtimeMaterial'], history=list(history),
        pendingGesture=pending, uploadedMaterialHash=telemetry['uploadedMaterialHash'], hashEvidence='CPU packed-entry cache',
        sharedBindings=2, retainedGraphAndGeometryCountsVerified=before is not None)


def unedited_state(state, expected):
    common_state(state); history_cursor(state, 0, 0, allow_uncreated=True)
    require(state['gltfMaterialAuthoring']['publications'] == 0 and state['gltfMaterialAuthoring']['sourceMaterial'] == -1,
        'Unedited control/reload accidentally performed material authoring')
    assignments = state['gltfMaterialVariants']['assignments']; properties = expected_properties(expected)
    require(len(assignments) == 2 and all(row['transparent'] == properties['transparent'] and
        row['alphaMasked'] == properties['alphaMasked'] and row['doubleSided'] == properties['doubleSided'] for row in assignments),
        'Incorrect control/reload graph-binding classification')
    return dict(passed=True, sharedBindings=2, expectedGraphBindingProperties=properties, historyDepth=0)


def images(first, second, *, contrast=False, minimum=.25):
    arrays = [np.asarray(Image.open(row['image']).convert('RGB'), dtype=float) for row in (first, second)]
    require(all(array.shape == (HEIGHT, WIDTH, 3) for array in arrays), 'Wrong material image extent')
    results = []
    for x0, y0, x1, y1 in ROI:
        a, b = (array[y0:y1, x0:x1] for array in arrays); delta = np.abs(a-b)
        results.append(dict(mean=float(delta.mean()), maximum=float(delta.max()),
            changedFraction=float(np.mean(np.max(delta, axis=2) >= 4.))))
    if contrast: require(all(row['mean'] >= minimum for row in results), 'Material positive image control is vacuous')
    else: require(all(row['maximum'] == 0 for row in results), 'Material independent/restored image differs')
    return dict(passed=True, regions=ROI, contrast=contrast, measurements=results)


def visible(record):
    array = np.asarray(Image.open(record['image']).convert('RGB'), dtype=float)
    background = array[25:40, 25:40].mean(axis=(0, 1)); differences = []
    for x0, y0, x1, y1 in ROI: differences.append(float(np.abs(array[y0:y1, x0:x1].mean(axis=(0, 1))-background).mean()))
    require(min(differences) > 8., 'Shared material image witness is not visibly different from background')
    return dict(passed=True, backgroundContrast=differences)


def serialized_material_record(material):
    # TinyGLTF omits schema-default core leaves when writing glTF/GLB. This
    # adapter is ONLY for serialized export checks: telemetry stays exact, and
    # extension objects, texture containers/indices and opaque metadata are
    # never synthesized or normalized.
    require(isinstance(material, dict), 'Malformed serialized material')
    result = copy.deepcopy(material)
    if 'doubleSided' in result:
        require(type(result['doubleSided']) is bool, 'Malformed serialized doubleSided')
    for key, default in [('alphaMode', 'OPAQUE'), ('alphaCutoff', .5),
            ('doubleSided', False), ('emissiveFactor', [0., 0., 0.])]:
        result.setdefault(key, default)
    pbr = result.setdefault('pbrMetallicRoughness', {})
    require(isinstance(pbr, dict), 'Malformed serialized metallic-roughness object')
    for key, default in [('baseColorFactor', [1., 1., 1., 1.]), ('metallicFactor', 1.), ('roughnessFactor', 1.)]:
        pbr.setdefault(key, default)
    for owner, key, extra in [(pbr, 'baseColorTexture', None), (pbr, 'metallicRoughnessTexture', None),
            (result, 'normalTexture', ('scale', 1.)), (result, 'occlusionTexture', ('strength', 1.)),
            (result, 'emissiveTexture', None)]:
        if key in owner:
            require(isinstance(owner[key], dict), 'Malformed serialized texture container '+key)
            owner[key].setdefault('texCoord', 0)
            if extra: owner[key].setdefault(*extra)
    return result


def inspect_export(path, expected, *, animated=False, retained_unlit=False):
    model, buffers = load_asset(path); roots = model['scenes'][model.get('scene', 0)]['nodes']
    pending, seen, bound = list(reversed(roots)), set(), []
    while pending:
        index = pending.pop(); require(index not in seen and 0 <= index < len(model['nodes']), 'Malformed exported material forest')
        seen.add(index); node = model['nodes'][index]; pending.extend(reversed(node.get('children', [])))
        if node.get('mesh', -1) >= 0:
            for primitive in model['meshes'][node['mesh']]['primitives']:
                require('material' in primitive, 'Exported binding lost its material'); bound.append(primitive['material'])
    require(len(bound) == 2 and len(set(bound)) == 1, 'Export did not preserve two shared material bindings')
    material = model['materials'][bound[0]]; inspect_record(serialized_material_record(material), expected)
    for extension in expected.get('extensions', {}): require(extension in model.get('extensionsUsed', []), 'Export declaration missing')
    require(TRANSMISSION in model.get('extensionsRequired', []), 'Export dropped a required input extension declaration')
    if retained_unlit: require(UNLIT in model.get('extensionsUsed', []), 'Undo removed an append-only extension declaration')
    result = dict(passed=True, path=str(path), sha256=sha(path), boundMaterial=bound[0], sharedBindings=len(bound),
        textureCount=len(model.get('textures', [])), metadataPreserved=True)
    if animated:
        original = material_curve(Path(path), bound[0]); result['curve'] = original
    return result


def material_curve(path, material=0):
    model, _ = load_asset(path); prefix = f'/materials/{material}/pbrMetallicRoughness/'; matches = {}
    expected = dict(baseColorFactor=[[.8, .2, .1, 1.], [.2, .4, .8, 1.]], metallicFactor=[[.1], [.7]])
    for clip in model.get('animations', []):
        for channel in clip['channels']:
            pointer = channel['target'].get('extensions', {}).get(POINTER, {}).get('pointer')
            if pointer in {prefix+key for key in expected}:
                key = pointer[len(prefix):]; require(key not in matches, 'Original material pointer curve was duplicated')
                sampler = clip['samplers'][channel['sampler']]
                require(sampler.get('interpolation', 'LINEAR') == 'LINEAR', 'Material curve interpolation changed')
                for accessor, kind in [(sampler['input'], 'SCALAR'), (sampler['output'], 'VEC4' if key == 'baseColorFactor' else 'SCALAR')]:
                    record = model['accessors'][accessor]
                    require(record['componentType'] == 5126 and record['type'] == kind and record['count'] == 2,
                        'Original material FLOAT curve storage type/count changed')
                times = np.asarray(decode_accessor(path, sampler['input']), dtype=float).reshape(-1)
                values = np.asarray(decode_accessor(path, sampler['output']), dtype=float)
                close(times, [0., 1.], 'original curve key times', 0.)
                close(values, np.asarray(expected[key], dtype=np.float32).astype(float), 'original curve values', 0.)
                matches[key] = dict(times=times.tolist(), values=values.tolist(), extras=clip.get('extras', {}))
    require(set(matches) == set(expected) and all(value['extras'] == dict(authoringCurve='unchanged') for value in matches.values()),
        'Original edited/unrelated material pointer curve was lost')
    return matches


def dependencies():
    directory = Path(__file__).resolve().parent; paths = {Path(__file__).resolve()}
    for module in tuple(sys.modules.values()):
        source = getattr(module, '__file__', None)
        if source and Path(source).resolve().parent == directory: paths.add(Path(source).resolve())
    return {str(path): sha(path) for path in sorted(paths)}


def artifacts(target):
    pattern = re.compile(re.escape(target.name)+r'(?:\.frame-[0-9]{4,})?\.(?:png|log|telemetry\.json)\Z')
    return [path for path in target.parent.iterdir() if pattern.fullmatch(path.name)]


def launch(args, output, report, persist, verify, technique, name, source, labels, *, script=None, save=None,
           animated=False, ray=False, rejection=None):
    verify(); target = output/'captures'/(technique+'-'+name); target.parent.mkdir(parents=True, exist_ok=True)
    require(not artifacts(target), 'Refusing to overwrite native material evidence')
    image, log = target.with_suffix('.png'), target.with_suffix('.log')
    command = [str(args.renderer.resolve()), '--hidden', '--no-ui', '--validation', '--no-bloom', '--no-taa',
        '--width', str(WIDTH), '--height', str(HEIGHT), '--msaa', '1', '--warmup-frames', '12', '--capture-frame', '13',
        '--fixed-dt', '.016666667', '--render-technique', technique, '--display-mode', 'lit', '--exposure', '.35',
        '--camera-position', '0', '0', '6', '--camera-target', '0', '0', '0', '--camera-fov', '45',
        '--environment-intensity', '0', '--directional-intensity', '3', '--directional-direction', '-.6', '.3', '-1',
        '--directional-color', '.2', '.7', '.4', '--import-scale', '1', '--model', str(source), '--screenshot', str(image)]
    if ray: command += ['--ray-shadows', 'hard', '--no-ray-shadow-denoise', '--ddgi', '--ddgi-counts', '3', '3', '3',
        '--ddgi-rays', '4', '--ddgi-probes-per-frame', '27', '--ddgi-ray-budget', '108', '--ddgi-intensity', '0']
    else: command += ['--no-ray-query', '--ray-shadows', 'raster']
    if animated: command += ['--animation-clip', '0', '--animation-time', '.5', '--animation-paused']
    if script: command += ['--capture-sequence', str(script)]
    if save:
        save.parent.mkdir(parents=True, exist_ok=True); require(not save.exists(), 'Refusing to overwrite material export')
        command += ['--save-scene', str(save)]
    row = dict(name=name, technique=technique, command=command, log=str(log), completed=False)
    report['processes'].append(row); report['rendererExecuted'] = True; persist()
    with log.open('x', encoding='utf-8') as stream:
        process = subprocess.run(command, cwd=args.renderer.resolve().parent, stdout=stream, stderr=subprocess.STDOUT,
            timeout=args.timeout, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    text = log.read_text(encoding='utf-8', errors='replace')
    row.update(returnCode=process.returncode, completed=True, logSha256=sha(log)); persist()
    require(not validation_log_error(text), 'Strict Vulkan validation failure: '+str(log))
    if rejection:
        require(process.returncode != 0 and re.search(rejection, text, re.I), 'Expected material request was not explicitly rejected: '+str(log))
        row['expectedRejection'] = True
    else: require(process.returncode == 0, 'Native material process failed: '+str(log))
    records = {}
    for label, frame in labels:
        sampled = image if frame == labels[-1][1] and not rejection else image.with_name(image.stem+f'.frame-{frame:04d}.png')
        telemetry = sampled.with_suffix('.telemetry.json')
        require(sampled.is_file() and telemetry.is_file(), 'Missing material capture image/telemetry: '+str(sampled))
        state = json.loads(telemetry.read_text(encoding='utf-8-sig'))
        require(finite_json(state) and state['technique'] == technique and state['resolution'] == [WIDTH, HEIGHT] and
            state['msaaSamples'] == 1 and not state['taa']['enabled'] and state['lighting']['environmentIntensity'] == 0 and
            any(item['active'] and item['gpuTimed'] for item in state['passes']), 'Wrong capture configuration/no timed GPU frame')
        close(state['lighting']['directionalIntensity'], 3., 'retained directional intensity')
        close(state['lighting']['directionalColor'], [.2, .7, .4], 'retained directional color')
        item = dict(name=label, technique=technique, process=name, frame=frame, image=str(sampled), telemetry=str(telemetry),
            log=str(log), imageSha256=sha(sampled), telemetrySha256=sha(telemetry), validationClean=True)
        report['captures'].append(item); records[label] = {**item, 'state': state}
    require({path.resolve() for path in artifacts(target) if path.suffix == '.png'} ==
        {Path(item['image']).resolve() for item in records.values()}, 'Unexpected material process capture inventory')
    persist(); verify(); return records


def animation_expected(label):
    edited = label not in ('before', 'undo', 'undo-rest')
    value = base_material()
    if edited:
        value['pbrMetallicRoughness'].update(baseColorFactor=[.1, .9, .2, 1.], roughnessFactor=.2)
        value['emissiveFactor'] = [.04, .01, .02]
    rest = label in ('rest', 'undo-rest', 'redo-rest'); time = .5 if label in ('before', 'edit', 'reselect') else 1.
    evaluated = copy.deepcopy(value)
    if not rest:
        start, end = np.asarray([.8, .2, .1, 1.], dtype=np.float32), np.asarray([.2, .4, .8, 1.], dtype=np.float32)
        evaluated['pbrMetallicRoughness']['baseColorFactor'] = (start*(1-time)+end*time).astype(float).tolist()
        metal = np.asarray([.1, .7], dtype=np.float32)
        evaluated['pbrMetallicRoughness']['metallicFactor'] = float(metal[0]*(1-time)+metal[1]*time)
    return value, evaluated, time, rest


def gesture_expected(label):
    value = base_material()
    roughness = dict(begin=.25, update=.4, end=.4, redo=.4, **{'cancel-begin': .15, 'cancel': .4}).get(label, .6)
    value['pbrMetallicRoughness']['roughnessFactor'] = roughness
    if label == 'remove': del value['extensions']['KHR_materials_clearcoat']
    if label in ('recreate', 'boundary-end', 'boundary-redo'):
        value['extensions']['KHR_materials_clearcoat'] = dict(clearcoatFactor=.3, clearcoatRoughnessFactor=.4)
    return value


def ray_witness(state, previous):
    ray, gi = state['rayShadows'], state['ddgi']; build = ray.get('build', {})
    require(ray['supported'] and ray['active'] and ray['mode'] == 1 and not ray['denoise'] and build.get('instances') == 2 and
        build.get('blasBuilt', 0)+build.get('blasReused', 0) > 0, 'No actual material ray scene')
    require(gi['supported'] and gi['enabled'] and gi['active'] and gi['contentEpoch'] > 0 and gi['updatedProbes'] > 0 and
        gi['tracedRays'] > 0 and gi['settings']['intensity'] == 0 and gi['settings']['counts'] == [3, 3, 3] and
        gi['settings']['raysPerProbe'] == 4, 'No actual material DDGI trace/update')
    require(previous is None or ray['generation'] > previous[0] and gi['contentEpoch'] > previous[1], 'Material replay kept stale ray/DDGI content')
    passes = {item['name']: item for item in state['passes']}; times = {}
    for name in ('RayShadowTrace', 'DdgiTrace', 'DdgiUpdate'):
        item = passes.get(name, {}); require(item.get('active') and item.get('gpuTimed') and item.get('gpuMs', 0.) > 0., 'Missing actual GPU pass '+name)
        times[name] = item['gpuMs']
    return dict(passed=True, rayGeneration=ray['generation'], ddgiContentEpoch=gi['contentEpoch'],
        activeGpuPassMilliseconds=times, evidence='execution only; GI intensity zero')


COUNTS = {'core': (21, 8), 'animation': (12, 4), 'gesture': (14, 1), 'ray-ddgi': (4, 1), 'invalid': (1, 4)}
CHECK_COUNTS = {'core': (22, 13, 2), 'animation': (11, 8, 1), 'gesture': (14, 7, 0), 'ray-ddgi': (8, 0, 0), 'invalid': (1, 0, 0)}


def run_native(args, output, plan, report):
    require(args.renderer.is_file() and args.manifest.is_file(), 'Missing renderer/frozen manifest')
    require(not any(output.glob('captures/*')) and not any(output.glob('exports/**/*')) and not any(output.glob('portable/**/*')),
        'Native material evidence directories must be fresh')
    if (output/'results.json').exists():
        previous = json.loads((output/'results.json').read_text(encoding='utf-8'))
        require(previous.get('generatedOnly') and not previous.get('rendererExecuted'), 'Refusing to overwrite native material result')
    manifest_hash = sha(args.manifest)
    def persist():
        for key in ('captures', 'stateChecks', 'imageChecks', 'exports', 'processes'): report[key+'Count'] = len(report[key])
        (output/'results.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    def verify():
        require(sha(args.manifest) == manifest_hash, 'Frozen material manifest changed')
        result = verify_manifest(args.manifest)
        require(dependencies() == report['sourceDependencies'] and sha(output/'launch-plan.json') == report['launchPlanSha256'], 'Material harness/dependencies/plan changed')
        for item in plan['immutableInputs']:
            path = Path(item['path']); require(path.stat().st_size == item['byteLength'] and sha(path) == item['sha256'], 'Material input changed')
        if 'runtimeShaderInputs' in report:
            require(sorted(str(path.resolve()) for path in (args.renderer.parent/'spv_shaders').glob('*.spv')) == report['runtimeShaderInputs'], 'Runtime SPIR-V inventory changed')
        for copied in report['portableCopies']:
            require(all(sha(Path(item['path'])) == item['sha256'] for item in copied['files']), 'Portable material resource changed')
        return result
    def check(name, technique, result, *, pixels=False):
        report['imageChecks' if pixels else 'stateChecks'].append(dict(check=name, technique=technique, **result)); persist()
    def exported(path, expected, technique, **kwargs):
        result = inspect_export(path, expected, **kwargs); report['exports'].append(dict(technique=technique, **result)); persist(); return result
    try:
        report['nativeManifestBefore'] = verify_manifest(args.manifest)
        frozen = {Path(path).resolve() for path in json.loads(args.manifest.read_text(encoding='utf-8-sig'))['files']}
        require(args.renderer.resolve() in frozen and {Path(path).resolve() for path in report['sourceDependencies']} <= frozen,
            'Manifest omits renderer or transitive local Python helper')
        shaders = list((args.renderer.parent/'spv_shaders').glob('*.spv'))
        require(shaders and {path.resolve() for path in shaders} <= frozen, 'Manifest omits resident runtime SPIR-V')
        report.update(rendererSha256=sha(args.renderer), runtimeShaderInputs=sorted(str(path.resolve()) for path in shaders))
        (output/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n', encoding='utf-8'); persist(); verify()
        sources = {key: Path(value) for key, value in plan['sources'].items()}; scripts = {key: Path(value) for key, value in plan['scripts'].items()}
        for technique in args.technique:
            if args.cohort == 'invalid':
                for label in ('range', 'property', 'conflict', 'source'):
                    reasons = dict(range=r'Capture material factor/color must be in \[0,1\]',
                        property=r'Capture material property is not in the numeric registry',
                        conflict=r'Capture material edits and model/scene/history/timeline/physics mutations need separate events',
                        source=r'Material edit requires an existing authored source material')
                    records = launch(args, output, report, persist, verify, technique, 'invalid-'+label, sources['source'],
                        [('before', 13)] if label == 'source' else [], script=scripts['invalid-'+label], rejection=reasons[label])
                    if label == 'source': check('before-rejected-source', technique, material_state(records['before']['state'], records['before']['state'],
                        base_material(), base_material(), history=(0, 0), initial=True))
                continue
            if args.cohort == 'core':
                controls = {name: launch(args, output, report, persist, verify, technique, 'control-'+name, sources[name], [(name, 13)])[name]
                    for name in ('factors', 'zero', 'anisotropy', 'unlit')}
                for label in controls: check('control-'+label, technique, unedited_state(controls[label]['state'], expected_material(label)))
                destination = output/'exports'/technique/'binary'/'materials.glb'
                records = launch(args, output, report, persist, verify, technique, 'core', sources['source'], CORE, script=scripts['core'], save=destination)
                cursor = [(0, 0), (1, 0), (0, 1), (1, 0), (2, 0), (3, 0), (4, 0), (5, 0), (6, 0), (7, 0), (6, 1), (7, 0), (8, 0), (7, 1)]
                for (label, _), history in zip(CORE, cursor):
                    expected = expected_material(label)
                    check(label, technique, material_state(records[label]['state'], records['before']['state'], expected, expected,
                        history=history, initial=label == 'before'))
                    if label not in ('before',):
                        counter = records[label]['state']['gltfMaterialAuthoring']['publications']
                        require(counter == [item[0] for item in CORE].index(label), 'Wrong material core publication count')
                for a, b in [('factors', 'redo'), ('before', 'undo'), ('tiny', 'undo-anisotropy'), ('anisotropy', 'redo-anisotropy'), ('anisotropy', 'undo-unlit')]:
                    check(a+'-'+b, technique, images(records[a], records[b]), pixels=True)
                    if a != 'before': require(records[a]['state']['gltfMaterialAuthoring']['uploadedMaterialHash'] ==
                        records[b]['state']['gltfMaterialAuthoring']['uploadedMaterialHash'], 'Restored packed-entry CPU hash differs')
                for name in controls: check(name+'-literal-control', technique, images(records[name], controls[name]), pixels=True)
                if technique == 'deferred-raster':
                    passes = {row['name']: row for row in records['anisotropy']['state']['passes']}
                    auxiliary = passes.get('PbrAuxiliary', {})
                    require(auxiliary.get('active') and auxiliary.get('gpuTimed') and auxiliary.get('gpuMs', 0.) > 0.,
                        'First anisotropy publication did not execute its actual deferred auxiliary pass')
                check('first-extended-activation', technique, dict(passed=True,
                    actualDeferredAuxiliaryTimestampRequired=technique == 'deferred-raster',
                    uploadedHashEvidence='CPU packed-entry cache; images are independent evidence'))
                check('factors-positive-control', technique, images(records['before'], controls['factors'], contrast=True), pixels=True)
                check('shared-visible', technique, visible(controls['zero']), pixels=True)
                ascii_path = output/'exports'/technique/'ascii'/'materials.gltf'
                ascii_record = launch(args, output, report, persist, verify, technique, 'ascii', sources['source'], [('ascii', 13)], script=scripts['ascii'], save=ascii_path)['ascii']
                check('ascii-final', technique, material_state(ascii_record['state'], None, expected_material('anisotropy'), expected_material('anisotropy'), history=(1, 0)))
                for suffix, path in [('glb', destination), ('gltf', ascii_path)]:
                    exported(path, expected_material('anisotropy'), technique, retained_unlit=suffix == 'glb')
                    copied, files = portable_copy(path, output/'portable'/technique/suffix); report['portableCopies'].append(dict(files=files)); persist()
                    reload = launch(args, output, report, persist, verify, technique, 'reload-'+suffix, copied, [('reload', 13)])['reload']
                    check('reload-'+suffix, technique, unedited_state(reload['state'], expected_material('anisotropy')))
                    check('reload-'+suffix+'-literal', technique, images(reload, controls['anisotropy']), pixels=True)
                continue
            if args.cohort == 'animation':
                controls = {name: launch(args, output, report, persist, verify, technique, name, sources[name], [(name, 13)])[name]
                    for name in ('curve-control', 'rest-control')}
                check('curve-control', technique, unedited_state(controls['curve-control']['state'], animation_expected('reselect')[1]))
                check('rest-control', technique, unedited_state(controls['rest-control']['state'], animation_expected('rest')[0]))
                destination = output/'exports'/technique/'binary'/'animated.glb'
                records = launch(args, output, report, persist, verify, technique, 'animation', sources['animated'], ANIMATION,
                    script=scripts['animation'], save=destination, animated=True)
                cursor = [(0, 0), (1, 0), (1, 0), (0, 1), (1, 0), (1, 0), (0, 1), (1, 0), (1, 0)]
                for (label, _), history in zip(ANIMATION, cursor):
                    a, e, time, rest = animation_expected(label); state = records[label]['state']
                    check(label, technique, material_state(state, records['before']['state'], a, e, history=history, initial=label == 'before'))
                    player = state['gltfAnimation']; require(not player['playing'] and player['selectedClip'] == (-1 if rest else 0), 'Material edit changed playback selection/state')
                    if not rest: close(player['time'], time, 'current-clock material replay')
                for a, b in [('seek', 'redo'), ('rest', 'redo-rest'), ('edit', 'reselect')]: check(a+'-'+b, technique, images(records[a], records[b]), pixels=True)
                check('tracked-curves-visible', technique, images(records['edit'], records['seek'], contrast=True), pixels=True)
                check('edited-rest-visible', technique, images(records['rest'], records['undo-rest'], contrast=True), pixels=True)
                check('curve-literal', technique, images(records['edit'], controls['curve-control']), pixels=True)
                check('rest-literal', technique, images(records['rest'], controls['rest-control']), pixels=True)
                # File defaults are the captured evaluated pose; original curves
                # and edited authored rest are tested separately through clip -1.
                saved = animation_expected('reselect')[1]; exported(destination, saved, technique, animated=True)
                copied, files = portable_copy(destination, output/'portable'/technique/'animation'); report['portableCopies'].append(dict(files=files)); persist()
                reload = launch(args, output, report, persist, verify, technique, 'reload-animation', copied, [('reload', 13)], animated=True)['reload']
                check('reload-current-curve', technique, images(reload, records['reselect']), pixels=True)
                continue
            if args.cohort == 'gesture':
                records = launch(args, output, report, persist, verify, technique, 'gesture', sources['source'], GESTURE, script=scripts['gesture'])
                cursors = [(0, 0), (0, 0), (0, 0), (1, 0), (0, 1), (1, 0), (1, 0), (1, 0), (0, 1), (0, 1), (0, 1), (1, 0), (0, 1), (1, 0)]
                pending = {'begin', 'update', 'cancel-begin', 'remove', 'recreate'}
                publications = [0, 1, 2, 2, 3, 4, 5, 6, 7, 8, 9, 9, 10, 11]
                for (label, _), history, counter in zip(GESTURE, cursors, publications):
                    expected = gesture_expected(label); state = records[label]['state']
                    check(label, technique, material_state(state, records['before']['state'], expected, expected,
                        history=history, pending=label in pending, initial=label == 'before'))
                    require(state['gltfMaterialAuthoring']['publications'] == counter, 'Wrong gesture publication count')
                for a, b in [('update', 'end'), ('end', 'redo'), ('end', 'cancel'), ('before', 'undo-final'),
                    ('before', 'boundary-undo'), ('recreate', 'boundary-end'), ('recreate', 'boundary-redo')]:
                    check(a+'-'+b, technique, images(records[a], records[b]), pixels=True)
                continue
            records = launch(args, output, report, persist, verify, technique, 'ray', sources['source'], RAY, script=scripts['ray'], ray=True)
            previous = None
            for label, _ in RAY:
                expected = base_material()
                if label in ('edited', 'redo'): expected['pbrMetallicRoughness']['baseColorFactor'] = [.2, .6, .9, 1.]
                check(label, technique, material_state(records[label]['state'], records['before']['state'], expected, expected,
                    history={'before': (0, 0), 'edited': (1, 0), 'undo': (0, 1), 'redo': (1, 0)}[label], initial=label == 'before'))
                witness = ray_witness(records[label]['state'], previous); previous = witness['rayGeneration'], witness['ddgiContentEpoch']
                check(label+'-gpu-execution', technique, witness)
        require(len(report['captures']) == report['expectedCaptureCount'] and len(report['processes']) == report['expectedProcessCount'], 'Incomplete material native cohort')
        expected = CHECK_COUNTS[args.cohort]
        require(tuple(len(report[key]) for key in ('stateChecks', 'imageChecks', 'exports')) ==
            tuple(count*len(args.technique) for count in expected), 'Incomplete material verification cohort')
        report.update(passed=True, nativeValidationPerformed=True); verify()
    except Exception as error:
        report['failure'] = str(error); raise
    finally:
        if report.get('nativeManifestBefore'):
            try: report['nativeManifestAfter'] = verify()
            except Exception as error:
                report['afterManifestFailure'] = str(error); report['passed'] = False
                if 'failure' not in report: report['failure'] = str(error)
        persist()
    require(report['passed'], 'Native material provenance failed after execution')


def self_check():
    checks = []
    def rejected(name, action):
        try: action()
        except (RuntimeError, KeyError, TypeError, ValueError): checks.append(name); return
        raise RuntimeError('Self-check accepted mutation: '+name)
    original = base_material(); inspect_record(copy.deepcopy(original), original)
    for name, mutate in [('texture-alias', lambda m: m['normalTexture'].update(index=0)),
        ('unknown-metadata', lambda m: m['extras'].update(opaqueIndex=16777218)),
        ('zero-factor', lambda m: m['extensions'][TRANSMISSION].update(transmissionFactor=1.)),
        ('signed-normal', lambda m: m['normalTexture'].update(scale=.1)),
        ('missing-extension', lambda m: m['extensions'].pop('KHR_materials_clearcoat'))]:
        changed = copy.deepcopy(original); mutate(changed); rejected(name, lambda changed=changed: inspect_record(changed, original))
    require(expected_properties(expected_material('zero'))['transparent'] is False and
        expected_properties(expected_material('tiny'))['transparent'] is True, 'Nonzero transmission routing oracle is wrong')
    a, e, time, rest = animation_expected('undo'); require(time == 1. and not rest and a != e, 'Current-clock animation oracle is vacuous')
    require(animation_expected('edit')[1]['pbrMetallicRoughness']['baseColorFactor'] != [.1, .9, .2, 1.], 'Absolute animation override oracle is missing')
    rows = [dict(graphNode=i, renderable=True, retired=False, materialIndex=2, primitiveIndex=0) for i in (0, 1)]
    before = dict(scene=dict(primaryObjectCount=2, primaryVertexCount=4, primaryIndexCount=6),
        gltfMerge=dict(graphNodeCount=2, graphIdentity=71, authoringNodes=rows, authoringNodesTruncated=False,
            activeRoots=[0, 1], sourceTables=dict(nodes=2, materials=1, meshes=1)),
        gltfScenes=dict(editHistoryDepth=0), gltfMaterialVariants=dict(assignments=[dict(node=i, material=2,
            transparent=False, alphaMasked=False, doubleSided=False) for i in (0, 1)]),
        gltfMaterialAuthoring=dict(publications=0, sourceMaterial=-1, runtimeMaterial=INVALID, pendingGesture=False,
            materialRevision=1, generationEpoch=1))
    material_state(before, before, original, original, history=(0, 0), initial=True)
    after = copy.deepcopy(before); after['gltfScenes']['editHistoryDepth'] = 1
    after['gltfMaterialVariants'].update(undoCount=1, redoCount=0)
    after['gltfMaterialAuthoring'].update(publications=1, sourceMaterial=0, runtimeMaterial=2, materialRevision=2,
        generationEpoch=2, uploadedMaterialHash=42, authored=direct_factors(), evaluated=direct_factors(),
        materialProperties=expected_properties(direct_factors()))
    material_state(after, before, direct_factors(), direct_factors(), history=(1, 0))
    for name, mutate in [('replaced-graph-identity', lambda s: s['gltfMerge'].update(graphIdentity=72)),
        ('rebound-shared-owner', lambda s: s['gltfMerge']['authoringNodes'][1].update(materialIndex=3)),
        ('wrong-runtime-material', lambda s: s['gltfMaterialAuthoring'].update(runtimeMaterial=3)),
        ('wrong-draw-route', lambda s: s['gltfMaterialVariants']['assignments'][0].update(transparent=True)),
        ('pending-gesture', lambda s: s['gltfMaterialAuthoring'].update(pendingGesture=True)),
        ('missing-history', lambda s: s['gltfMaterialVariants'].pop('undoCount')),
        ('changed-source-factor', lambda s: s['gltfMaterialAuthoring']['authored']['pbrMetallicRoughness'].update(roughnessFactor=.9)),
        ('stale-packed-hash', lambda s: s['gltfMaterialAuthoring'].update(uploadedMaterialHash=0))]:
        changed = copy.deepcopy(after); mutate(changed)
        rejected(name, lambda changed=changed: material_state(changed, before, direct_factors(), direct_factors(), history=(1, 0)))
    animated = copy.deepcopy(after); a, e, _, _ = animation_expected('undo')
    animated['gltfMaterialAuthoring'].update(authored=a, evaluated=e, materialProperties=expected_properties(e))
    animated['gltfMaterialVariants'].update(undoCount=0, redoCount=1)
    material_state(animated, before, a, e, history=(0, 1))
    stale_clock = copy.deepcopy(animated)
    stale_clock['gltfMaterialAuthoring']['evaluated']['pbrMetallicRoughness']['baseColorFactor'] = animation_expected('before')[1]['pbrMetallicRoughness']['baseColorFactor']
    rejected('rewound-material-clock', lambda: material_state(stale_clock, before, a, e, history=(0, 1)))
    whole = gesture_expected('boundary-undo'); inspect_record(copy.deepcopy(whole), base_material())
    damaged = copy.deepcopy(whole); damaged['extensions']['KHR_materials_clearcoat'].pop('clearcoatTexture')
    rejected('lost-toggle-texture', lambda: inspect_record(damaged, base_material()))
    with tempfile.TemporaryDirectory(prefix='material-authoring-selfcheck-') as folder:
        root = Path(folder); plan = generate(root, 'core')
        material_curve(Path(plan['sources']['animated']))
        inspect_export(Path(plan['sources']['source']), base_material())
        serialized, _ = load_asset(Path(plan['sources']['source']))
        omitted = copy.deepcopy(serialized)
        record = omitted['materials'][0]
        for key in ('alphaMode', 'alphaCutoff', 'doubleSided', 'emissiveFactor'): record.pop(key)
        record['normalTexture'].pop('scale')
        omitted_path = root/'omitted-defaults.gltf'; document(omitted_path, omitted)
        inspect_export(omitted_path, base_material())
        require(all(key not in record for key in ('alphaMode', 'alphaCutoff', 'doubleSided', 'emissiveFactor')) and
            'scale' not in record['normalTexture'], 'Export checker mutated serialized defaults')
        rejected('telemetry-default-omission-stays-strict', lambda: inspect_record(record, base_material()))
        default_pbr = copy.deepcopy(serialized)
        expected_pbr = base_material()
        expected_pbr['pbrMetallicRoughness'].update(baseColorFactor=[1., 1., 1., 1.], metallicFactor=1., roughnessFactor=1.)
        for key in ('baseColorFactor', 'metallicFactor', 'roughnessFactor'): default_pbr['materials'][0]['pbrMetallicRoughness'].pop(key)
        default_pbr_path = root/'omitted-pbr-defaults.gltf'; document(default_pbr_path, default_pbr)
        inspect_export(default_pbr_path, expected_pbr)
        for name, expected, mutate in [
                ('export-lost-nondefault-alpha', dict(base_material(), alphaMode='MASK'), lambda m: m.pop('alphaMode')),
                ('export-lost-nondefault-cutoff', dict(base_material(), alphaCutoff=.6), lambda m: m.pop('alphaCutoff')),
                ('export-lost-nondefault-factor', base_material(), lambda m: m['pbrMetallicRoughness'].pop('roughnessFactor')),
                ('export-malformed-boolean', base_material(), lambda m: m.update(doubleSided=0)),
                ('export-malformed-number', base_material(), lambda m: m.update(alphaCutoff='0.5')),
                ('export-malformed-normal-scale', base_material(), lambda m: m['normalTexture'].update(scale=True)),
                ('export-malformed-texture-container', base_material(), lambda m: m.update(normalTexture=[])),
                ('export-lost-texture-container', base_material(), lambda m: m.pop('normalTexture')),
                ('export-lost-texture-index', base_material(), lambda m: m['normalTexture'].pop('index')),
                ('export-lost-extension-factor', base_material(), lambda m: m['extensions'][TRANSMISSION].pop('transmissionFactor')),
                ('export-changed-opaque-extras', base_material(), lambda m: m['extras'].update(opaqueIndex=16777218))]:
            damaged = copy.deepcopy(serialized); mutate(damaged['materials'][0])
            path = root/(name+'.gltf'); document(path, damaged)
            rejected(name, lambda path=path, expected=expected: inspect_export(path, expected))
        checks.extend(['serialized-core-default-omission', 'serialized-pbr-default-omission', 'serialized-input-immutability'])
        bad_curve, _ = load_asset(Path(plan['sources']['animated']))
        bad_curve['animations'][0]['channels'][0]['target']['extensions'][POINTER]['pointer'] = '/materials/999/pbrMetallicRoughness/baseColorFactor'
        bad_path = root/'wrong-pointer.gltf'; document(bad_path, bad_curve)
        rejected('forged-material-pointer-target', lambda: material_curve(bad_path))
        for name, path in plan['scripts'].items():
            data = json.loads(Path(path).read_text()); frames = [item['frame'] for item in data['events']]
            require(frames == sorted(set(frames)) and all(1 <= frame <= data['frames'] for frame in frames), 'Duplicate/outside material event frame')
        target = root/'captures'/'animated'; target.parent.mkdir(); (target.parent/'animated-control.png').write_bytes(b'neighbor')
        require(not artifacts(target), 'Control neighbor conflicts with exact material output namespace')
        for suffix in ('.png', '.log', '.telemetry.json', '.frame-0013.png', '.frame-0013.telemetry.json', '.frame-10000.png'):
            path = target.parent/(target.name+suffix); path.write_bytes(b'own'); require(path in artifacts(target), 'Exact material artifact is not protected'); path.unlink()
        source, _ = load_asset(Path(plan['sources']['source'])); require(len(source['nodes']) == 2 and source['nodes'][0]['mesh'] == source['nodes'][1]['mesh'], 'Fixture does not share geometry/material')
        pixels = np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8)
        for x0, y0, x1, y1 in ROI: pixels[y0:y1, x0:x1] = [30, 150, 70]
        reference, changed = root/'reference.png', root/'changed.png'; Image.fromarray(pixels).save(reference)
        visible(dict(image=str(reference))); images(dict(image=str(reference)), dict(image=str(reference)))
        rejected('vacuous-positive-image', lambda: images(dict(image=str(reference)), dict(image=str(reference)), contrast=True))
        pixels[ROI[0][1], ROI[0][0]] = [70, 80, 40]; Image.fromarray(pixels).save(changed)
        rejected('changed-reference-pixel', lambda: images(dict(image=str(reference)), dict(image=str(changed))))
        gpu = dict(rayShadows=dict(supported=True, active=True, mode=1, denoise=False, generation=3,
            build=dict(instances=2, blasBuilt=1, blasReused=0)), ddgi=dict(supported=True, enabled=True, active=True,
            contentEpoch=4, updatedProbes=27, tracedRays=108, settings=dict(intensity=0, counts=[3, 3, 3], raysPerProbe=4)),
            passes=[dict(name=name, active=True, gpuTimed=True, gpuMs=.1) for name in ('RayShadowTrace', 'DdgiTrace', 'DdgiUpdate')])
        ray_witness(gpu, (2, 3)); stale_gpu = copy.deepcopy(gpu); stale_gpu['passes'][2]['gpuMs'] = 0.
        rejected('missing-ddgi-dispatch', lambda: ray_witness(stale_gpu, (2, 3)))
        rejected('stale-ddgi-content', lambda: ray_witness(gpu, (3, 4)))
        checks.extend(['independent-current-clock', 'absolute-pointer-override', 'nonzero-transmission-route',
            'strict-event-schedules', 'native-output-namespace', 'shared-bindings', 'original-pointer-curve'])
    return dict(passed=True, checks=checks, checkCount=len(checks), rendererExecuted=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path); parser.add_argument('--renderer', type=Path); parser.add_argument('--manifest', type=Path)
    parser.add_argument('--technique', choices=TECHNIQUES, action='append'); parser.add_argument('--cohort', choices=COHORTS, default='core')
    parser.add_argument('--timeout', type=float, default=600.); parser.add_argument('--generate-only', action='store_true'); parser.add_argument('--self-check', action='store_true')
    args = parser.parse_args()
    if args.self_check: print(json.dumps(self_check(), indent=2)); return
    if args.output is None: parser.error('--output is required')
    if not args.generate_only and (args.renderer is None or args.manifest is None): parser.error('Native execution requires explicit --renderer and --manifest')
    if not math.isfinite(args.timeout) or args.timeout <= 0.: parser.error('--timeout must be finite and positive')
    args.technique = tuple(dict.fromkeys(args.technique or TECHNIQUES))
    if args.renderer: args.renderer = args.renderer.resolve()
    if args.manifest: args.manifest = args.manifest.resolve()
    output = args.output.resolve(); plan = generate(output, args.cohort); captures, processes = COUNTS[args.cohort]
    report = dict(schemaVersion=1, cohort=args.cohort, passed=None if args.generate_only else False, generatedOnly=args.generate_only,
        rendererExecuted=False, nativeValidationPerformed=False, sourceDependencies=dependencies(), launchPlanSha256=sha(output/'launch-plan.json'),
        expectedCaptureCount=captures*len(args.technique), expectedProcessCount=processes*len(args.technique),
        captures=[], processes=[], stateChecks=[], imageChecks=[], exports=[], portableCopies=[], unverified=UNVERIFIED)
    if args.generate_only: document(output/'results.json', report); print('GENERATED', report['expectedCaptureCount'], 'planned captures; renderer not executed')
    else:
        run_native(args, output, plan, report)
        print('PASS', len(report['captures']), 'captures;', len(report['stateChecks']), 'CPU/publication checks;', len(report['imageChecks']), 'image checks')


if __name__ == '__main__': main()
