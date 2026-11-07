"""Independent #106 merge/history/failure and portable-export native acceptance.

This first static cohort plans 24 captures per technique: four independently
authored baked controls, sixteen merge/history/failure states, and two portable
save/reload pairs. Both input assets start their resource indices at zero and
use the same relative image/buffer basenames with different resident bytes.

--generate-only and --self-check never execute a renderer. Native execution
requires an explicit renderer and a verified frozen-input manifest. No generated
result claims a native pass. No production merger builds an oracle.
"""
from __future__ import annotations

import argparse
import base64
import copy
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from urllib.parse import unquote, urlsplit

import numpy as np
from PIL import Image

from gltf_accessor_runtime_regression import Asset, decode_accessor, load_asset, png_bytes, require
from gltf_extended_materials_runtime_regression import sha, validation_log_error
from gltf_tangent_uv_runtime_regression import finite_json
from gltf_texture_formats_runtime_regression import verify_manifest


TECHNIQUES = ('deferred-raster', 'forward-raster')
UNLIT = 'KHR_materials_unlit'
TRANSFORM = 'KHR_texture_transform'
WIDTH, HEIGHT = 640, 360
MAX_SCENE_OBJECTS = 4096
CAMERA = [0., 0., 6.]
LOCAL_QUAD = [[-.5, -.5, 0.], [.5, -.5, 0.], [.5, .5, 0.], [-.5, .5, 0.]]
UV = [[0., 0.], [1., 0.], [1., 1.], [0., 1.]]
B_UV1 = [[0., 0.], [2., 0.], [2., 2.], [0., 2.]]
INDICES = [0, 1, 2, 0, 2, 3]
PLACEMENTS = ([1.5, .5, 0.], [1.5, -1., 0.])
COLORS = {
    'A': [[[240, 30, 20, 255], [30, 220, 45, 255]],
          [[200, 60, 15, 255], [30, 140, 55, 255]]],
    'B': [[[20, 45, 240, 255], [235, 210, 20, 255]],
          [[55, 25, 170, 255], [170, 170, 25, 255]]],
}
# These are literal independent world vertices, not copied or transformed from
# a merge result. Every arithmetic operand is exactly representable in binary32.
BAKED = {
    'A': [[-1.125, .5, 0.], [-2.125, .5, 0.], [-2.125, -.5, 0.], [-1.125, -.5, 0.]],
    'sentinel': [[-2.625, -1.375, 0.], [-2.375, -1.375, 0.],
                 [-2.375, -1.125, 0.], [-2.625, -1.125, 0.]],
    'B1': [[1.875, 1.125, 0.], [1.375, 1.125, 0.], [1.375, .125, 0.], [1.875, .125, 0.]],
    'B2': [[1.875, -.375, 0.], [1.375, -.375, 0.], [1.375, -1.375, 0.], [1.875, -1.375, 0.]],
}


def immutable(path, payload):
    """Do not replace inputs or diagnostic evidence with different bytes."""
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        require(path.read_bytes() == payload, 'Refusing to replace a different generated input: '+str(path))
    else:
        path.write_bytes(payload)


def document(path, value):
    immutable(path, (json.dumps(value, indent=2, allow_nan=False)+'\n').encode('utf-8'))


def placement(translation):
    return [1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0., *translation, 1.]


def add_resource(asset, folder, origin, *, image_name):
    image_index = len(asset.model.setdefault('images', []))
    sampler_index = len(asset.model.setdefault('samplers', []))
    texture_index = len(asset.model.setdefault('textures', []))
    material_index = len(asset.model['materials'])
    immutable(folder/image_name, png_bytes(COLORS[origin]))
    asset.model['images'].append(dict(name='same image', uri=image_name,
        extras=dict(fixtureOrigin=origin, opaqueImageIndex=0)))
    asset.model['samplers'].append(dict(name='same sampler', minFilter=9728, magFilter=9728,
        wrapS=33071 if origin == 'A' else 33648, wrapT=33071 if origin == 'A' else 10497,
        extras=dict(fixtureOrigin=origin, opaqueSamplerIndex=0)))
    asset.model['textures'].append(dict(name='same texture', source=image_index, sampler=sampler_index,
        extras=dict(fixtureOrigin=origin, opaqueSourceIndex=0)))
    info = dict(index=texture_index, texCoord=0, extras=dict(fixtureOrigin=origin))
    if origin == 'B':
        info['extensions'] = {TRANSFORM: dict(texCoord=1, offset=[.125, .25], scale=[.75, .5],
            rotation=0., extras=dict(fixtureOrigin='B', opaqueTextureIndex=0))}
    asset.model['materials'].append(dict(name='same material',
        pbrMetallicRoughness=dict(baseColorFactor=[1., 1., 1., 1.], baseColorTexture=info,
            metallicFactor=0., roughnessFactor=1.),
        doubleSided=False, extensions={UNLIT: {}},
        extras=dict(fixtureOrigin=origin, fooTexture=dict(index=977))))
    return material_index


def add_quad(asset, positions, material, tag, *, origin):
    primitive = dict(attributes=dict(POSITION=asset.accessor('VEC3', positions),
        NORMAL=asset.accessor('VEC3', [[0., 0., 1.]]*4),
        TEXCOORD_0=asset.accessor('VEC2', UV if origin == 'A' else [[.25, .25]]*4),
        TEXCOORD_1=asset.accessor('VEC2', UV if origin == 'A' else B_UV1)),
        indices=asset.accessor('SCALAR', INDICES, 5123, indices=True), material=material, mode=4,
        extras=dict(fixtureTag=tag, textureLike=dict(index=991)))
    result = len(asset.model['meshes'])
    asset.model['meshes'].append(dict(name=tag, primitives=[primitive], extras=dict(fixtureTag=tag)))
    return result


def write_asset(path, asset):
    model = asset.model
    model['asset']['generator'] = 'Container independent static merge controls'
    model['extensionsUsed'] = [UNLIT] + ([TRANSFORM] if len(model.get('textures', [])) > 1 or
        any(TRANSFORM in m['pbrMetallicRoughness']['baseColorTexture'].get('extensions', {})
            for m in model['materials']) else [])
    model['buffers'] = [dict(uri='mesh.bin', byteLength=len(asset.data))]
    immutable(path.parent/'mesh.bin', bytes(asset.data))
    document(path, model)
    return model


def source_asset(path, origin):
    asset = Asset()
    material = add_resource(asset, path.parent, origin, image_name='texture.png')
    add_quad(asset, LOCAL_QUAD, material, origin+' panel', origin=origin)
    if origin == 'A':
        add_quad(asset, [[-.125, -.125, 0.], [.125, -.125, 0.],
                        [.125, .125, 0.], [-.125, .125, 0.]], material, 'A sentinel', origin='A')
        asset.model['nodes'] = [
            dict(name='A retained parent', translation=[-1.5, 0., 0.], rotation=[0., 0., 1., 0.],
                 children=[1], extras=dict(fixtureTag='A-parent')),
            dict(name='A retained panel', translation=[.125, 0., 0.], mesh=0, extras=dict(fixtureTag='A-panel')),
            dict(name='A unselected sentinel', translation=[-2.5, -1.25, 0.], mesh=1,
                 extras=dict(fixtureTag='A-sentinel')),
        ]
        roots = [0, 2]
    else:
        asset.model['nodes'] = [
            dict(name='B authored parent', translation=[.25, .125, 0.], rotation=[0., 0., 1., 0.],
                 scale=[.5, 1., 1.], children=[1], extras=dict(fixtureTag='B-parent')),
            dict(name='B authored panel', translation=[.25, 0., 0.], mesh=0, extras=dict(fixtureTag='B-panel')),
        ]
        roots = [0]
    asset.model['scenes'] = [dict(name='same scene', nodes=roots, extras=dict(fixtureOrigin=origin))]
    return write_asset(path, asset)


def baked_oracle(path, version):
    asset = Asset()
    material_a = add_resource(asset, path.parent, 'A', image_name='A_texture.png')
    material_b = add_resource(asset, path.parent, 'B', image_name='B_texture.png') if version in ('merged1', 'merged2') else None
    # Oracles contain explicit world-space vertex streams and ordinary roots.
    # They do not reference the source documents, call a merger, or share graph
    # traversal/evaluation logic with the production or transaction path.
    parts = [('A', material_a, 'A')]
    if version == 'original':
        parts.append(('sentinel', material_a, 'A'))
    if version in ('merged1', 'merged2'):
        parts.append(('B1', material_b, 'B'))
    if version == 'merged2':
        parts.append(('B2', material_b, 'B'))
    asset.model['nodes'] = []
    for tag, material, origin in parts:
        mesh_index = add_quad(asset, BAKED[tag], material, tag, origin=origin)
        asset.model['nodes'].append(dict(name='A retained panel' if tag == 'A' else tag,
            mesh=mesh_index, extras=dict(fixtureTag=tag)))
    asset.model['scenes'] = [dict(nodes=list(range(len(parts))))]
    return write_asset(path, asset)


def capacity_asset(path):
    asset = Asset()
    material = add_resource(asset, path.parent, 'B', image_name='texture.png')
    add_quad(asset, LOCAL_QUAD, material, 'capacity shared triangle mesh', origin='B')
    asset.model['nodes'] = [dict(name='Capacity '+str(i), mesh=0) for i in range(MAX_SCENE_OBJECTS+1)]
    asset.model['scenes'] = [dict(nodes=list(range(MAX_SCENE_OBJECTS+1)))]
    return write_asset(path, asset)


def expected_state(imports, *, sentinel=False, undo=1, redo=0, outcome=None, parent=None):
    return dict(importCount=imports, sentinelVisible=sentinel,
        activeRenderableCount=1+int(sentinel)+imports,
        sourceTables=dict(nodes=3+5*imports, meshes=2+imports, materials=1+imports,
            images=1+imports, textures=1+imports, samplers=1+imports, buffers=1+imports, scenes=1+imports),
        undoCount=undo, redoCount=redo, lastRequestOutcome=outcome,
        retainedGraphIdentity='equals first transaction sample',
        retainedSourceIds=[0, 1, 2], currentScene=0, inheritedImportScale=1.,
        lastParentName=parent, preservedSelection='same retained A panel; no outline mismatch allowed')


def build_plan(output, techniques):
    paths = dict(A=output/'sources'/'A'/'scene.gltf', B=output/'sources'/'B'/'scene.gltf',
        malformed=output/'sources'/'malformed'/'scene.gltf',
        missing=output/'sources'/'missing'/'does-not-exist.gltf',
        capacity=output/'sources'/'capacity'/'scene.gltf')
    oracles = {name: output/'oracles'/name/'scene.gltf' for name in ('original', 'edited', 'merged1', 'merged2')}
    merge1 = dict(path=str(paths['B']), parentName='B placement 1', placementMatrix=placement(PLACEMENTS[0]))
    merge2 = dict(path=str(paths['B']), parentName='B placement 2', incomingScene=0, placementMatrix=placement(PLACEMENTS[1]))
    actions = [
        ('before', dict(nodeFlags=dict(sourceNode=2, visible=False)), 'edited', expected_state(0)),
        ('merge1', dict(merge=merge1), 'merged1', expected_state(1, undo=2, outcome='committed', parent='B placement 1')),
        ('undo1', dict(sceneHistory='undo'), 'edited', expected_state(0, undo=1, redo=1)),
        ('redo1', dict(sceneHistory='redo'), 'merged1', expected_state(1, undo=2)),
        ('merge2', dict(merge=merge2), 'merged2', expected_state(2, undo=3, outcome='committed', parent='B placement 2')),
        ('undo2', dict(sceneHistory='undo'), 'merged1', expected_state(1, undo=2, redo=1)),
        ('redo2', dict(sceneHistory='redo'), 'merged2', expected_state(2, undo=3)),
        ('missing', dict(merge=dict(path=str(paths['missing']), parentName='Must not publish')), 'merged2',
            expected_state(2, undo=3, outcome='failed')),
        ('malformed', dict(merge=dict(path=str(paths['malformed']), parentName='Must not publish')), 'merged2',
            expected_state(2, undo=3, outcome='failed')),
        ('capacity', dict(merge=dict(path=str(paths['capacity']), parentName='Must not publish')), 'merged2',
            expected_state(2, undo=3, outcome='failed')),
        ('undo2-after-failure', dict(sceneHistory='undo'), 'merged1', expected_state(1, undo=2, redo=1)),
        ('undo1-after-failure', dict(sceneHistory='undo'), 'edited', expected_state(0, undo=1, redo=2)),
        ('undo-old-visibility', dict(sceneHistory='undo'), 'original', expected_state(0, sentinel=True, undo=0, redo=3)),
        ('redo-old-visibility', dict(sceneHistory='redo'), 'edited', expected_state(0, undo=1, redo=2)),
        ('redo1-final', dict(sceneHistory='redo'), 'merged1', expected_state(1, undo=2, redo=1)),
        ('redo2-final', dict(sceneHistory='redo'), 'merged2', expected_state(2, undo=3)),
    ]
    sequence = dict(schemaVersion=1, frames=13*len(actions), sampleFrames=[],
        camera=[dict(frame=1, position=CAMERA, target=[0., 0., 0.])], events=[])
    states = []
    for number, (name, action, oracle, expected) in enumerate(actions):
        frame = 13*(number+1)
        event = dict(frame=2 if number == 0 else frame-11, **copy.deepcopy(action))
        if 'merge' in action:
            event['guiWaitForLoad'] = True
        sequence['events'].append(event)
        sequence['sampleFrames'].append(frame)
        states.append(dict(name=name, frame=frame, oracle=oracle, expected=expected,
            requestMustComplete='merge' in action,
            unchangedAfterFailure=['image', 'graphIdentity', 'sourceTables', 'historyCursor',
                'retainedNodeIds', 'descriptors', 'resourceOwnership'] if expected['lastRequestOutcome'] == 'failed' else []))
    captures = [dict(name='oracle-'+name, role='independent-control', source=str(path)) for name, path in oracles.items()]
    captures += [dict(name='transaction-'+entry['name'], role='transaction-state', frame=entry['frame'], source=str(paths['A']))
                 for entry in states]
    checks = [dict(name='transaction-'+entry['name']+'-equals-independent-control', kind='exact',
        images=['transaction-'+entry['name'], 'oracle-'+entry['oracle']], maximumByteError=0) for entry in states]
    exports = []
    for suffix in ('glb', 'gltf'):
        save_name, reload_name = 'save-'+suffix, 'reload-'+suffix
        export = output/'exports'/'{technique}'/('merged.'+suffix)
        captures += [dict(name=save_name, role='save-final-state', source=str(paths['A']), save=str(export)),
                     dict(name=reload_name, role='reload-portable-export', source=str(export))]
        checks += [dict(name=name+'-equals-independent-control', kind='exact',
            images=[name, 'oracle-merged2'], maximumByteError=0) for name in (save_name, reload_name)]
        checks.append(dict(name=suffix+'-reload-equals-save', kind='exact', images=[save_name, reload_name], maximumByteError=0))
        exports.append(dict(format=suffix, path=str(export), requiredInspections=[
            'complete scene catalog: three scenes including independent canonical B roots',
            'two named placement parents and correct local/world transforms',
            'all source references in range; global strict forest',
            'A/B/repeated-B image pixels, samplers, UV overrides and opaque extras preserved',
            'runtime numeric TRS equals exact source-to-binary32 expectation',
            'B source identities remain distinct even if resident image/geometry storage is shared',
            'fresh reload from copied export directory without source A/B files']))
    checks += [
        dict(name='old-visibility-control-is-visible', kind='contrast', images=['oracle-original', 'oracle-edited'],
            roi=[132, 264, 145, 277], minimumMeanAbsoluteByteDelta=3.),
        dict(name='first-import-control-is-visible', kind='contrast', images=['oracle-merged1', 'oracle-edited'],
            roi=[426, 118, 450, 152], minimumMeanAbsoluteByteDelta=3.),
        dict(name='repeat-import-control-is-visible', kind='contrast', images=['oracle-merged2', 'oracle-merged1'],
            roi=[426, 226, 450, 260], minimumMeanAbsoluteByteDelta=3.),
    ]
    export_sequence = dict(schemaVersion=1, frames=39, sampleFrames=[39],
        camera=copy.deepcopy(sequence['camera']), events=[
            dict(frame=2, nodeFlags=dict(sourceNode=2, visible=False)),
            dict(frame=14, merge=copy.deepcopy(merge1), guiWaitForLoad=True),
            dict(frame=27, merge=copy.deepcopy(merge2), guiWaitForLoad=True)])
    launches = [dict(name='oracle-'+name, source=str(path), sequence=None,
        captureNames=['oracle-'+name]) for name, path in oracles.items()]
    launches.append(dict(name='transaction', source=str(paths['A']),
        sequence=str(output/'transaction-sequence.json'),
        captureNames=['transaction-'+entry['name'] for entry in states]))
    for suffix in ('glb', 'gltf'):
        exported = output/'exports'/'{technique}'/('merged.'+suffix)
        launches += [dict(name='save-'+suffix, source=str(paths['A']), sequence=str(output/'save-sequence.json'),
                         save=str(exported), captureNames=['save-'+suffix]),
                     dict(name='reload-'+suffix, source=str(exported), sequence=None,
                         captureNames=['reload-'+suffix], requireSourceIndependentCopy=True)]
    return dict(schemaVersion=2, nativeReady=True,
        nativeExecutionRequirement='Explicit --renderer and --manifest; missing actual merge telemetry fails the run.',
        techniques=list(techniques), paths={key: str(path) for key, path in paths.items()},
        oracles={key: str(path) for key, path in oracles.items()}, captures=captures, imageChecks=checks, launches=launches,
        transactionStates=states, exports=exports, sequence=sequence, exportSequence=export_sequence,
        expectedCapturesPerTechnique=len(captures), expectedImageChecksPerTechnique=len(checks),
        expectedSessionStateChecksPerTechnique=len(states), expectedExportInspectionsPerTechnique=len(exports),
        expectedProcessesPerTechnique=len(launches),
        sessionPolicy=dict(importScale=1., incomingScale='inherits current session', destination='current selected scene',
            placement='independent affine named parent', defaultIncomingScene=0, autoPlayIncoming=False),
        requiredCapabilities=['public merge event and GUI transaction path', 'reversible source/catalog/resource publication',
            'old graph identity and history retention', 'failure cleanup completion', 'source-independent portable export'],
        fixedCaptureSettings=dict(width=WIDTH, height=HEIGHT, camera=CAMERA, cameraTarget=[0., 0., 0.], fov=45.,
            fixedDt=1/60, msaa=1, taa=False, bloom=False, exposure=.35, environmentIntensity=0., directionalIntensity=0.,
            renderMode='lit (unlit materials)', inheritedSessionImportScale=1., maxSceneObjects=MAX_SCENE_OBJECTS),
        prospectiveCommonArguments=['--hidden', '--no-ui', '--validation', '--no-bloom', '--no-taa',
            '--width', str(WIDTH), '--height', str(HEIGHT), '--msaa', '1', '--warmup-frames', '12',
            '--capture-frame', '13', '--fixed-dt', '.016666667', '--render-technique', '{technique}',
            '--display-mode', 'lit', '--exposure', '.35', '--camera-position', '0', '0', '6',
            '--camera-target', '0', '0', '0', '--camera-fov', '45', '--environment-intensity', '0',
            '--directional-intensity', '0', '--no-ray-query', '--ray-shadows', 'raster'],
        prospectivePerLaunchArguments=['--model', '{source}', '--screenshot', '{output}/captures/{technique}-{name}.png'],
        prospectiveSequenceArguments=['--capture-sequence', '{sequence}'],
        prospectiveSaveArguments=['--save-scene', '{save}'],
        requiredTelemetry=dict(existing=['gltfScenes.graphIdentity', 'gltfScenes.selected', 'gltfScenes.names',
            'scene.primaryObjectCount', 'gltfMaterialVariants.undoCount', 'gltfMaterialVariants.redoCount',
            'gltfNodeFlags.nodes', 'gltfNodeFlags.probes', 'gltfNodeFlags.truncated', 'gui.modelLoading.pending', 'passes'],
            merge=['gltfMerge.requestId/outcome/error/parentName', 'gltfMerge.commitGeneration',
                'gltfMerge.descriptorGeneration', 'gltfMerge.sourceTables', 'gltfMerge.importScale',
                'gltfMerge.retainedNodes[].graphNode/sourceNode/name/worldColumns',
                'gltfMerge.sourceReadCount', 'gltfMerge.generationPublications'],
            countSemantics='expected.activeRenderableCount is the visible count; actual active count includes the hidden sentinel'),
        validationLogContract='Reject VUID-, SYNC-HAZARD, Validation Error, hazard detected, and READ/WRITE_AFTER_READ/WRITE hazard wording.',
        unverified=['Generation-only results do not establish native rendering or GPU execution.',
            'Static input resources only: skin, morph, instancing, variants, cameras, lights and animation pointers need later cohorts.',
            'No procedural/manual runtime insertion is claimed; that needs a real capture creation action.',
            'CPU nearest-hit/ancestor probes do not prove GPU picking readback or interaction trace-through.',
            'No ray-query/DDGI work, physical lighting oracle, allocation-failure injection or leak-free native result is claimed.',
            'Descriptor generation is publication telemetry; this cohort does not introspect GPU descriptor contents.',
            'sourceReadCount proves merge-worker read-attempt deltas, not general filesystem or GPU-upload absence.',
            'Reported resource handles/ownership flags do not prove leak-free lifetime or all allocation ownership.'])


def generate(plan):
    paths = {key: Path(value) for key, value in plan['paths'].items()}
    source_asset(paths['A'], 'A')
    source_asset(paths['B'], 'B')
    for name, path in plan['oracles'].items():
        baked_oracle(Path(path), name)
    capacity_asset(paths['capacity'])
    document(paths['malformed'], dict(asset=dict(version='2.0'), scene=0,
        scenes=[dict(nodes=[0])], nodes=[dict(children=[2147483648])],
        buffers=[dict(uri='never-open.bin', byteLength=4)]))
    require(not paths['missing'].exists() and not (paths['malformed'].parent/'never-open.bin').exists(),
            'Missing-resource failure fixture unexpectedly exists')
    output = paths['A'].parents[2]
    document(output/'transaction-sequence.json', plan['sequence'])
    document(output/'save-sequence.json', plan['exportSequence'])
    immutable(output/'vk_layer_settings.txt', b'khronos_validation.validate_sync = true\n')
    inputs = []
    for folder in (output/'sources', output/'oracles'):
        inputs.extend(path for path in folder.rglob('*') if path.is_file())
    inputs += [output/'transaction-sequence.json', output/'save-sequence.json', output/'vk_layer_settings.txt']
    return [dict(path=str(path), sha256=sha(path), byteLength=path.stat().st_size) for path in sorted(inputs)]


def exact_positions(actual, expected, label):
    require(np.array_equal(np.asarray(actual, dtype=np.float32), np.asarray(expected, dtype=np.float32)),
            'Independent baked world geometry differs: '+label)


def geometry_proof(plan):
    a_path, b_path = Path(plan['paths']['A']), Path(plan['paths']['B'])
    a, _ = load_asset(a_path)
    b, _ = load_asset(b_path)
    a_positions = decode_accessor(a_path, a['meshes'][0]['primitives'][0]['attributes']['POSITION'])
    b_positions = decode_accessor(b_path, b['meshes'][0]['primitives'][0]['attributes']['POSITION'])
    # Independent analytic evaluation of these deliberately exact 180-degree
    # source transforms, not a generic graph traversal or production evaluator.
    a_world = a_positions*np.array([-1., -1., 1.], dtype=np.float32)+[-1.625, 0., 0.]
    b_local = b_positions*np.array([-.5, -1., 1.], dtype=np.float32)+[.125, .125, 0.]
    exact_positions(a_world, BAKED['A'], 'A')
    for index, tag in enumerate(('B1', 'B2')):
        exact_positions(b_local+np.asarray(PLACEMENTS[index]), BAKED[tag], tag)
    sentinel = decode_accessor(a_path, a['meshes'][1]['primitives'][0]['attributes']['POSITION'])+[-2.5, -1.25, 0.]
    exact_positions(sentinel, BAKED['sentinel'], 'sentinel')
    for name, path in plan['oracles'].items():
        model, _ = load_asset(Path(path))
        for entry in model['meshes']:
            actual = decode_accessor(Path(path), entry['primitives'][0]['attributes']['POSITION'])
            exact_positions(actual, BAKED[entry['name']], name+'/'+entry['name'])
    return dict(literalBakedVerticesEqualIndependentAnalyticTransforms=True, maximumPositionError=0.,
                allOperandsExactlyRepresentableInBinary32=True)


def roi_proof(plan):
    tags = ('sentinel', 'B1', 'B2')
    controls = [entry for entry in plan['imageChecks'] if entry['kind'] == 'contrast']
    require(len(controls) == len(tags), 'Nonvacuous merge controls changed')
    focal_pixels = HEIGHT/(2*np.tan(np.pi/8))
    for control, tag in zip(controls, tags):
        vertices = np.asarray(BAKED[tag])
        pixels = np.column_stack((WIDTH/2+focal_pixels*vertices[:, 0]/(CAMERA[2]-vertices[:, 2]),
            HEIGHT/2-focal_pixels*vertices[:, 1]/(CAMERA[2]-vertices[:, 2])))
        left, top, right, bottom = control['roi']
        require(left > min(pixels[:, 0]) and right < max(pixels[:, 0]) and
                top > min(pixels[:, 1]) and bottom < max(pixels[:, 1]),
                'Fixed negative-control ROI is not strictly inside independent geometry: '+tag)
    return dict(fixedContrastRoisInsideIndependentProjectedQuads=True, projection='analytic fixed camera; no renderer output')


def identity_proof(plan):
    assets = {name: load_asset(Path(plan['paths'][name]))[0] for name in ('A', 'B')}
    for name, model in assets.items():
        require(model['buffers'][0]['uri'] == 'mesh.bin' and model['images'][0]['uri'] == 'texture.png',
                'Source collision basenames changed')
        require(model['textures'][0]['source'] == 0 and model['textures'][0]['sampler'] == 0 and
                model['materials'][0]['pbrMetallicRoughness']['baseColorTexture']['index'] == 0 and
                model['meshes'][0]['primitives'][0]['material'] == 0 and model['nodes'][1]['mesh'] == 0,
                'Source resource references must independently begin at index0')
        with Image.open(Path(plan['paths'][name]).parent/'texture.png') as decoded:
            require(np.array_equal(np.asarray(decoded.convert('RGBA')), np.asarray(COLORS[name], dtype=np.uint8)),
                    'Source external PNG differs from its independently declared pixels')
    require(COLORS['A'] != COLORS['B'], 'Source image aliasing control is vacuous')
    require(assets['A']['samplers'][0] != assets['B']['samplers'][0], 'Sampler collision control is vacuous')
    b_primitive = assets['B']['meshes'][0]['primitives'][0]
    b_uv = decode_accessor(Path(plan['paths']['B']), b_primitive['attributes']['TEXCOORD_1'])
    require(np.array_equal(b_uv, np.asarray(B_UV1, dtype=np.float32)) and
            max(b_uv[:, 0]*.75+.125) > 1 and max(b_uv[:, 1]*.5+.25) > 1,
            'B UV1 control must cross both authored wrapping boundaries')
    for name in ('merged1', 'merged2'):
        model, _ = load_asset(Path(plan['oracles'][name]))
        require([material['extras']['fixtureOrigin'] for material in model['materials']] == ['A', 'B'],
                'Independent oracle lost distinct source material associations')
        for mesh in model['meshes']:
            primitive = mesh['primitives'][0]
            require(primitive['material'] == int(mesh['name'].startswith('B')),
                    'Independent oracle aliases B to A material')
        info = model['materials'][1]['pbrMetallicRoughness']['baseColorTexture']
        require(info['index'] == 1 and model['textures'][1]['source'] == 1 and model['textures'][1]['sampler'] == 1 and
                info['extensions'][TRANSFORM]['texCoord'] == 1,
                'Independent oracle resource/UV association differs')
    return dict(collidingSourceBasenames=True, independentIndexZeroReferences=True, uv1CrossesSamplerWrappingBoundaries=True,
                differentImagePixelsAndSamplerPolicies=True, independentOracleMaterialAssociations=True)


def dependencies():
    return {str(path.resolve()): sha(path) for path in (Path(__file__),
        Path(__file__).with_name('gltf_accessor_runtime_regression.py'),
        Path(__file__).with_name('gltf_extended_materials_runtime_regression.py'),
        Path(__file__).with_name('gltf_tangent_uv_runtime_regression.py'),
        Path(__file__).with_name('gltf_texture_formats_runtime_regression.py'))}


PUBLICATIONS = [0, 1, 2, 3, 4, 5, 6, 6, 6, 6, 7, 8, 8, 8, 9, 10]
READ_ATTEMPTS = [0, 1, 1, 1, 2, 2, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5]


def integer(value, label):
    require(type(value) is int and value >= 0, 'Invalid unsigned integer: '+label)
    return value


def columns(value, label):
    matrix = np.asarray(value, dtype=np.float64)
    require(matrix.shape == (4, 4) and np.isfinite(matrix).all(), 'Invalid finite matrix: '+label)
    require(np.array_equal(matrix[:, 3], [0., 0., 0., 1.]), 'Non-affine column-major matrix: '+label)
    return matrix.T


def retained_nodes(state):
    merge, flags = state['gltfMerge'], state['gltfNodeFlags']
    require(not merge.get('retainedNodesTruncated', False) and not flags['truncated'],
            'Retained source/flag evidence was truncated')
    by_source = {}
    for row in merge['retainedNodes']:
        integer(row['graphNode'], 'retained graphNode')
        columns(row['worldColumns'], 'retained world')
        if row['sourceNode'] in (0, 1, 2):
            require(row['sourceNode'] not in by_source, 'Duplicate retained source binding')
            by_source[row['sourceNode']] = row
    require(set(by_source) == {0, 1, 2}, 'Retained A source bindings are missing')
    return by_source


def nearest_authored_source(flag_rows, selected):
    """Synthetic primitive children retain their authored owner's identity.

    The flag snapshot is bounded; reject missing parents/cycles rather than
    depending on optional primitive/accessor telemetry for this proof.
    """
    visited = set()
    while selected in flag_rows and selected not in visited:
        visited.add(selected)
        row = flag_rows[selected]
        source = row['sourceIndex']
        require(type(source) is int and source >= -1, 'Invalid authored source identity in selection ancestry')
        if source >= 0:
            return source
        parent = row['parentGraphNode']
        require(type(parent) is int and parent >= -1, 'Invalid parent in selection ancestry')
        selected = parent
    raise RuntimeError('Selection ancestry is missing, cyclic or has no authored source')


def session_check(state, baseline, entry, index):
    """Compare actual publication evidence against a fixed transaction schedule."""
    expected = entry['expected']
    merge, initial = state['gltfMerge'], baseline['gltfMerge']
    scene, history = state['gltfScenes'], state['gltfMaterialVariants']
    for field in ('requestId', 'sourceReadCount', 'generationPublications', 'commitGeneration', 'descriptorGeneration'):
        integer(merge[field], 'gltfMerge.'+field)
        integer(initial[field], 'baseline gltfMerge.'+field)
    require(merge['requestId']-initial['requestId'] == READ_ATTEMPTS[index] and
            merge['sourceReadCount']-initial['sourceReadCount'] == READ_ATTEMPTS[index],
            'Merge requests/read attempts changed during history or did not complete: '+entry['name'])
    require(merge['generationPublications']-initial['generationPublications'] == PUBLICATIONS[index] and
            merge['commitGeneration']-initial['commitGeneration'] == PUBLICATIONS[index] and
            merge['descriptorGeneration'] == merge['commitGeneration'],
            'Generation publication/descriptors changed incorrectly: '+entry['name'])
    outcome = 'idle' if index == 0 else 'committed' if index < 7 else 'failed'
    require(merge['outcome'] == outcome and isinstance(merge['error'], str),
            'Unexpected/pending merge request outcome: '+entry['name'])
    require(bool(merge['error']) == (outcome == 'failed'), 'Failure diagnostic missing or stale')
    parent = '' if index == 0 else 'B placement 1' if index < 4 else 'B placement 2' if index < 7 else 'Must not publish'
    # Idle parent text has no semantic meaning; every submitted request does.
    require(index == 0 or merge['parentName'] == parent, 'Wrong completed merge parent name')
    require(merge['sourceTables'] == expected['sourceTables'], 'Live source tables changed incorrectly: '+entry['name'])
    require(merge['importScale'] == 1. == state['scene']['primaryImportScale'], 'Session import scale changed')
    require(scene['graphIdentity'] == baseline['gltfScenes']['graphIdentity'] and
            scene['selected'] == 0 and len(scene['names']) == 1+expected['importCount'],
            'Merge replaced graph identity or corrupted scene catalog/current selection')
    require(history['undoCount'] == expected['undoCount'] and history['redoCount'] == expected['redoCount'],
            'Merge/failure lost the retained history cursor: '+entry['name'])
    active_count = 2+expected['importCount']
    require(scene['activeRenderableCount'] == active_count == state['scene']['primaryObjectCount'],
            'Active draw membership changed (hidden sentinel remains active)')
    require(state['gltfInstancing']['drawCount'] == expected['activeRenderableCount'],
            'Visible controller draw count changed')
    active_sources = [0, 1, 2]+[source+5*number for number in range(expected['importCount']) for source in (5, 6, 7)]
    require(sorted(scene['activeSourceNodes']) == active_sources, 'Canonical inactive B or wrong placement source nodes became active')
    instances = state['gltfInstancing']['instances']
    retained, original = retained_nodes(state), retained_nodes(baseline)
    require(retained == original, 'Retained A graph IDs, names or world matrices changed')
    # These independent source matrices also catch a wrong baseline/reference pair.
    a_parent = np.diag([-1., -1., 1., 1.]); a_parent[:3, 3] = [-1.5, 0., 0.]
    a_panel = a_parent.copy(); a_panel[0, 3] = -1.625
    sentinel = np.eye(4); sentinel[:3, 3] = [-2.5, -1.25, 0.]
    for source, matrix in enumerate((a_parent, a_panel, sentinel)):
        require(np.array_equal(columns(retained[source]['worldColumns'], str(source)), matrix),
                'Retained world differs from independent source arithmetic')
    flag_rows = {row['graphNode']: row for row in state['gltfNodeFlags']['nodes']}
    require(len(flag_rows) == len(state['gltfNodeFlags']['nodes']), 'Duplicate graph ID in selection/flag evidence')
    for source, row in retained.items():
        flag = flag_rows[row['graphNode']]
        require(flag['sourceIndex'] == source and flag['active'] and
                flag['localSelectable'] and flag['localHoverable'] and
                flag['localVisible'] == (expected['sentinelVisible'] if source == 2 else True),
                'Retained source activity/local flags changed')
    require(state['gui']['selectedMeshNodeId'] == baseline['gui']['selectedMeshNodeId'] and
            state['gui']['selectedMeshNodeId'] in flag_rows and not state['gui']['modelLoading']['pending'],
            'Selection changed or GUI transaction has not settled')
    selected = state['gui']['selectedMeshNodeId']
    require(nearest_authored_source(flag_rows, selected) == 1,
            'Selection is not the retained A panel')
    # This legacy accessor telemetry is optional for source-mapped runtime
    # primitives. Exact images, source activity and retained worlds above are
    # mandatory; exported geometry is independently decoded below.
    instance_geometry_checked = len(instances) == active_count
    if instance_geometry_checked:
        require(sum(row['visible'] for row in instances) == expected['activeRenderableCount'], 'Instance visibility changed')
        expected_tags = ['A', 'sentinel']+['B'+str(i+1) for i in range(expected['importCount'])]
        actual_geometry = []
        for row in instances:
            matrix = columns(row['modelColumns'], 'instance world')
            local = np.asarray(LOCAL_QUAD, dtype=np.float64)*(.25 if row['sourceNode'] == 2 else 1.)
            transformed = (matrix@np.column_stack((local, np.ones(4))).T).T[:, :3]
            actual_geometry.append(tuple(transformed.reshape(-1)))
        wanted = [tuple(np.asarray(BAKED[tag]).reshape(-1)) for tag in expected_tags]
        require(sorted(actual_geometry) == sorted(wanted), 'Instance matrices differ from independent baked positions')
    if 'objectBuffers' in merge:
        require(merge['objectBuffers'] and all(integer(row['handle'], 'object buffer') > 0 and
                type(row['controllerOwned']) is bool for row in merge['objectBuffers']),
                'Published object buffer handles/ownership evidence are invalid')
    return dict(name=entry['name'], frame=entry['frame'], passed=True,
        sourceTables=merge['sourceTables'], graphIdentity=scene['graphIdentity'],
        activeRenderableCount=active_count, visibleRenderableCount=expected['activeRenderableCount'],
        commitGeneration=merge['commitGeneration'], descriptorGeneration=merge['descriptorGeneration'],
        requestId=merge['requestId'], outcome=merge['outcome'], sourceReadCount=merge['sourceReadCount'],
        generationPublications=merge['generationPublications'], undoCount=history['undoCount'], redoCount=history['redoCount'],
        retainedNodeWorldsUnchanged=True, selectionUnchanged=True, instanceGeometryChecked=instance_geometry_checked,
        objectBuffers=merge.get('objectBuffers'), retainedGenerationCount=merge.get('retainedGenerationCount'))


def compare_images(check, records):
    images = []
    for name in check['images']:
        with Image.open(records[name]['image']) as source:
            require(source.size == (WIDTH, HEIGHT), 'Wrong capture dimensions: '+name)
            images.append(np.asarray(source.convert('RGBA'), dtype=np.int16))
    delta = np.abs(images[0]-images[1])
    if check['kind'] == 'exact':
        maximum = int(delta.max())
        require(maximum <= check['maximumByteError'], f"Exact merge image mismatch: {check['name']}; max={maximum}")
        return dict(maximumByteError=maximum, meanAbsoluteByteDelta=float(delta.mean()), passed=True)
    left, top, right, bottom = check['roi']
    mean = float(delta[top:bottom, left:right, :3].mean())
    require(mean >= check['minimumMeanAbsoluteByteDelta'], f"Vacuous merge control: {check['name']}; mean={mean}")
    return dict(meanAbsoluteByteDelta=mean, passed=True)


def local_matrix(node):
    if 'matrix' in node:
        values = np.asarray(node['matrix'], dtype=np.float64)
        require(values.shape == (16,) and np.isfinite(values).all(), 'Invalid export matrix')
        return columns(values.reshape(4, 4), 'export local')
    x, y, z, w = node.get('rotation', [0., 0., 0., 1.])
    rotation = np.array([[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
        [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
        [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]])
    result = np.eye(4)
    result[:3, :3] = rotation@np.diag(node.get('scale', [1., 1., 1.]))
    result[:3, 3] = node.get('translation', [0., 0., 0.])
    require(np.isfinite(result).all(), 'Invalid export TRS')
    return result


def forest_worlds(model):
    """Bounded iterative independent reference validation/evaluation."""
    nodes = model.get('nodes', [])
    require(0 < len(nodes) <= 100000, 'Unexpected exported node bound')
    parents = [-1]*len(nodes)
    for parent, node in enumerate(nodes):
        for child in node.get('children', []):
            require(type(child) is int and 0 <= child < len(nodes) and parents[child] == -1,
                    'Export has invalid, duplicate or multiply-parented child')
            parents[child] = parent
    pending = [index for index, parent in enumerate(parents) if parent == -1]
    worlds, visible = {}, {}
    for index in pending:
        parent = parents[index]
        worlds[index] = local_matrix(nodes[index]) if parent == -1 else worlds[parent]@local_matrix(nodes[index])
        local_visible = nodes[index].get('extensions', {}).get('KHR_node_visibility', {}).get('visible', True)
        require(type(local_visible) is bool, 'Export visibility is not boolean')
        visible[index] = local_visible and (parent == -1 or visible[parent])
        pending.extend(nodes[index].get('children', []))
    require(len(worlds) == len(nodes), 'Export has a disconnected cycle')
    for scene in model.get('scenes', []):
        roots = scene.get('nodes', [])
        require(len(roots) == len(set(roots)) and all(type(i) is int and 0 <= i < len(nodes) and parents[i] == -1 for i in roots),
                'Export scene references a non-root or invalid/duplicate node')
    require(type(model.get('scene')) is int and 0 <= model['scene'] < len(model['scenes']), 'Invalid exported default scene')
    return worlds, visible


def external_path(root, uri):
    require(isinstance(uri, str) and uri and not urlsplit(uri).scheme and not urlsplit(uri).netloc and
            not urlsplit(uri).query and not urlsplit(uri).fragment, 'Export resource URI is not a local relative file')
    relative = Path(unquote(uri))
    require(not relative.is_absolute() and ':' not in str(relative), 'Export resource URI is absolute')
    resolved = (root/relative).resolve()
    require(resolved.is_relative_to(root.resolve()) and resolved.is_file(), 'Export resource escapes its independent directory')
    return resolved


def portable_copy(exported, destination):
    model, _ = load_asset(exported)
    closure = {exported.resolve()}
    for table in ('buffers', 'images'):
        for row in model.get(table, []):
            uri = row.get('uri', '')
            if uri and not uri.startswith('data:'):
                closure.add(external_path(exported.parent, uri))
    require(not destination.exists(), 'Refusing to overwrite portable export evidence')
    destination.mkdir(parents=True)
    files = []
    for source in sorted(closure):
        relative = source.relative_to(exported.parent.resolve())
        target = destination/relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        require(sha(source) == sha(target), 'Portable copy changed exported bytes')
        files.append(dict(path=str(target), sha256=sha(target)))
    copied = destination/exported.name
    copied_model, _ = load_asset(copied)
    for table in ('buffers', 'images'):
        for row in copied_model.get(table, []):
            uri = row.get('uri', '')
            if uri and not uri.startswith('data:'):
                external_path(destination, uri)
    return copied, files


def image_pixels(path, model, buffers, index):
    integer(index, 'image index')
    require(index < len(model.get('images', [])), 'Invalid exported image source')
    image = model['images'][index]
    if image.get('uri', '').startswith('data:'):
        payload = base64.b64decode(image['uri'].split(',', 1)[1], validate=True)
    elif image.get('uri'):
        payload = external_path(path.parent, image['uri']).read_bytes()
    else:
        view_index = integer(image.get('bufferView'), 'image bufferView')
        require(view_index < len(model['bufferViews']), 'Image bufferView out of range')
        view = model['bufferViews'][view_index]
        payload = buffers[view['buffer']][view.get('byteOffset', 0):view.get('byteOffset', 0)+view['byteLength']]
    with Image.open(io.BytesIO(payload)) as decoded:
        return np.asarray(decoded.convert('RGBA'))


def texture_info_contract(info, *, source=False):
    """Normalize only the specified effective UV placement/default spelling.

    Export intentionally moves extension texCoord to the enclosing texture info.
    Numeric runtime fields use an independently cast binary32 expectation;
    extras and other extension members remain exact opaque JSON.
    """
    result = copy.deepcopy(info)
    result.pop('index', None)
    result.setdefault('texCoord', 0)
    transform = result.get('extensions', {}).get(TRANSFORM)
    if transform is not None:
        result['texCoord'] = transform.pop('texCoord', result['texCoord'])
        if source:
            for field in ('offset', 'scale'):
                if field in transform:
                    transform[field] = np.asarray(transform[field], dtype=np.float32).astype(float).tolist()
            if 'rotation' in transform:
                transform['rotation'] = float(np.float32(transform['rotation']))
    return result


def inspect_export(plan, path):
    require((path.read_bytes()[:4] == b'glTF') == (path.suffix.lower() == '.glb'), 'Wrong portable export container format')
    model, buffers = load_asset(path)
    require(finite_json(model) and len(model.get('scenes', [])) == 3 and model.get('scene') == 0,
            'Export lost complete scene catalog/default')
    worlds, visible = forest_worlds(model)
    require([scene.get('extras', {}).get('fixtureOrigin') for scene in model['scenes']] == ['A', 'B', 'B'],
            'Export lost colliding source scene metadata')
    for buffer, data in zip(model.get('buffers', []), buffers):
        require(data is not None and buffer['byteLength'] <= len(data), 'Export buffer is truncated')
    for view in model.get('bufferViews', []):
        b = integer(view['buffer'], 'bufferView buffer')
        require(b < len(buffers) and 0 <= view.get('byteOffset', 0) and
                view.get('byteOffset', 0)+view['byteLength'] <= len(buffers[b]), 'Invalid exported bufferView')
    for texture in model.get('textures', []):
        image, sampler = integer(texture['source'], 'texture source'), integer(texture['sampler'], 'texture sampler')
        require(image < len(model.get('images', [])) and sampler < len(model.get('samplers', [])),
                'Export contains an invalid unused texture reference')
    for i, accessor in enumerate(model.get('accessors', [])):
        require(np.isfinite(decode_accessor(path, i)).all(), 'Nonfinite/unreadable exported accessor')
    sources = {origin: load_asset(Path(plan['paths'][origin]))[0] for origin in ('A', 'B')}
    origins = []
    texture_ids, image_ids, sampler_ids = set(), set(), set()
    for material in model.get('materials', []):
        origin = material.get('extras', {}).get('fixtureOrigin')
        if origin not in sources:
            continue
        origins.append(origin)
        wanted = sources[origin]['materials'][0]
        require(material['extras'] == wanted['extras'] and material.get('extensions', {}).get(UNLIT) == {} and
                material.get('doubleSided', False) is False, 'Export changed source material metadata/unlit/culling')
        pbr = material['pbrMetallicRoughness']
        for field, default in (('baseColorFactor', [1., 1., 1., 1.]), ('metallicFactor', 1.), ('roughnessFactor', 1.)):
            require(pbr.get(field, default) == wanted['pbrMetallicRoughness'][field], 'Export changed material factor')
        info = pbr['baseColorTexture']
        texture_index = integer(info['index'], 'material texture')
        require(texture_info_contract(info) == texture_info_contract(wanted['pbrMetallicRoughness']['baseColorTexture'], source=True),
                'Export changed effective texture UV/transform/opaque metadata')
        require(texture_index < len(model['textures']), 'Exported material texture out of range')
        texture = model['textures'][texture_index]
        image_index, sampler_index = integer(texture['source'], 'texture image'), integer(texture['sampler'], 'texture sampler')
        require(sampler_index < len(model['samplers']), 'Exported texture sampler out of range')
        require(texture.get('extras') == sources[origin]['textures'][0]['extras'] and
                model['images'][image_index].get('extras') == sources[origin]['images'][0]['extras'] and
                model['samplers'][sampler_index] == sources[origin]['samplers'][0],
                'Export aliased source texture/image/sampler metadata')
        require(np.array_equal(image_pixels(path, model, buffers, image_index), np.asarray(COLORS[origin], dtype=np.uint8)),
                'Export changed colliding image pixels')
        texture_ids.add(texture_index); image_ids.add(image_index); sampler_ids.add(sampler_index)
    require(sorted(origins) == ['A', 'B', 'B'] and len(texture_ids) == len(image_ids) == len(sampler_ids) == 3,
            'Export collapsed distinct repeated source identities')
    for number in (1, 2):
        matches = [i for i, row in enumerate(model['nodes']) if row.get('name') == 'B placement '+str(number)]
        require(len(matches) == 1 and np.array_equal(local_matrix(model['nodes'][matches[0]]),
                np.asarray(placement(PLACEMENTS[number-1])).reshape(4, 4).T) and
                np.array_equal(worlds[matches[0]], local_matrix(model['nodes'][matches[0]])),
                'Named placement transform was changed/double-applied')
    selected = []
    pending = list(model['scenes'][0]['nodes'])
    for index in pending:
        selected.append(index); pending.extend(model['nodes'][index].get('children', []))
    geometry, hidden, node_resources = [], [], {}
    for index, node in enumerate(model['nodes']):
        if 'mesh' not in node:
            continue
        mesh = integer(node['mesh'], 'node mesh')
        require(mesh < len(model['meshes']), 'Export node mesh out of range')
        for primitive in model['meshes'][mesh]['primitives']:
            material = integer(primitive['material'], 'primitive material')
            require(material < len(model['materials']), 'Export primitive material out of range')
            positions = decode_accessor(path, primitive['attributes']['POSITION'])
            require(positions.shape == (4, 3), 'Export changed source primitive vertex identity')
            require(np.array_equal(decode_accessor(path, primitive['indices']).reshape(-1), INDICES), 'Export changed indices')
            origin = model['materials'][material]['extras']['fixtureOrigin']
            tag = primitive.get('extras', {}).get('fixtureTag')
            require(tag in (('A panel', 'A sentinel') if origin == 'A' else ('B panel',)) and
                    primitive['extras'] == dict(fixtureTag=tag, textureLike=dict(index=991)) and
                    model['meshes'][mesh].get('extras') == dict(fixtureTag=tag),
                    'Export primitive/mesh opaque metadata changed')
            node_resources.setdefault(index, set()).add(model['materials'][material]['pbrMetallicRoughness']['baseColorTexture']['index'])
            require(np.array_equal(decode_accessor(path, primitive['attributes']['TEXCOORD_0']),
                    np.asarray(UV if origin == 'A' else [[.25, .25]]*4, dtype=np.float32)) and
                    np.array_equal(decode_accessor(path, primitive['attributes']['TEXCOORD_1']),
                    np.asarray(UV if origin == 'A' else B_UV1, dtype=np.float32)), 'Export changed UV source streams')
            if index in selected:
                transformed = (worlds[index]@np.column_stack((positions, np.ones(4))).T).T[:, :3]
                (geometry if visible[index] else hidden).append(tuple(transformed.reshape(-1)))
    require(sorted(geometry) == sorted(tuple(np.asarray(BAKED[tag]).reshape(-1)) for tag in ('A', 'B1', 'B2')) and
            hidden == [tuple(np.asarray(BAKED['sentinel']).reshape(-1))],
            'Export selected visible/hidden geometry differs from independent literal oracle')
    require(set().union(*(node_resources.get(index, set()) for index in selected)) == texture_ids,
            'Selected placements do not retain every distinct source texture identity')
    canonical_resources = []
    canonical_roots = []
    canonical_vertices = np.asarray(LOCAL_QUAD)*[-.5, -1., 1.]+[.125, .125, 0.]
    for scene in model['scenes'][1:]:
        members = list(scene['nodes']); canonical_roots.append(set(members))
        for index in members:
            members.extend(model['nodes'][index].get('children', []))
        draws = [index for index in members if 'mesh' in model['nodes'][index]]
        require(len(draws) == 1 and visible[draws[0]], 'Canonical incoming scene lost its independent mesh')
        index = draws[0]
        primitive = model['meshes'][model['nodes'][index]['mesh']]['primitives'][0]
        positions = decode_accessor(path, primitive['attributes']['POSITION'])
        actual = (worlds[index]@np.column_stack((positions, np.ones(4))).T).T[:, :3]
        exact_positions(actual, canonical_vertices, 'export canonical incoming scene')
        resources = set().union(*(node_resources.get(node, set()) for node in members))
        require(len(resources) == 1 and model['materials'][primitive['material']]['extras']['fixtureOrigin'] == 'B',
                'Canonical scene references another source resource')
        canonical_resources.append(resources)
    require(not canonical_roots[0].intersection(canonical_roots[1]) and
            not canonical_resources[0].intersection(canonical_resources[1]), 'Repeated canonical scenes collapsed source identity')
    tags = [node.get('extras', {}).get('fixtureTag') for node in model['nodes']]
    require({tag: tags.count(tag) for tag in ('A-parent', 'A-panel', 'A-sentinel', 'B-parent', 'B-panel')} ==
            {'A-parent': 1, 'A-panel': 1, 'A-sentinel': 1, 'B-parent': 4, 'B-panel': 4},
            'Source node extras were lost or canonical/placement source identities collapsed')
    return dict(path=str(path), sha256=sha(path), passed=True, sceneCount=3,
        distinctFixtureMaterials=3, distinctTextureImageSamplerIdentities=3,
        globalForestValid=True, allAccessorsDecoded=True, exactLiteralWorldGeometry=True, canonicalScenesIndependentlyDecoded=True,
        hiddenSentinelPreserved=True, sourceMetadataAndPixelsPreserved=True)


def capture_state(path, technique):
    state = json.loads(path.read_text(encoding='utf-8-sig'))
    require(finite_json(state) and state['technique'] == technique and state['resolution'] == [WIDTH, HEIGHT] and
            state['msaaSamples'] == 1 and not state['taa']['enabled'], 'Wrong/invalid native capture settings')
    require(state['lighting']['environmentIntensity'] == 0 and state['lighting']['directionalIntensity'] == 0,
            'Native merge fixture lighting isolation changed')
    require(any(row['active'] and row['gpuTimed'] for row in state['passes']), 'No actual GPU-timed rendering pass')
    return state


def launch(renderer, output, plan, planned, technique, report, persist, timeout, *, source_override=None):
    name = planned['name']
    target = output/'captures'/(technique+'-'+name)
    target.parent.mkdir(parents=True, exist_ok=True)
    image, log = target.with_suffix('.png'), target.with_suffix('.log')
    require(not any(target.parent.glob(target.name+'*')), 'Refusing to overwrite native capture evidence: '+str(target))
    source = source_override or Path(planned['source'].replace('{technique}', technique))
    command = [str(renderer)]+[value.replace('{technique}', technique) for value in plan['prospectiveCommonArguments']]
    command += ['--model', str(source), '--screenshot', str(image)]
    if planned.get('sequence'):
        command += ['--capture-sequence', planned['sequence']]
    if planned.get('save'):
        destination = Path(planned['save'].replace('{technique}', technique))
        destination.parent.mkdir(parents=True, exist_ok=True)
        require(not destination.exists(), 'Refusing to overwrite a native export')
        command += ['--save-scene', str(destination)]
    attempt = dict(name=name, technique=technique, command=command, log=str(log), completed=False)
    report['processes'].append(attempt)
    report['rendererExecuted'] = report['nativeValidationPerformed'] = True
    persist()
    with log.open('x', encoding='utf-8') as stream:
        process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
            timeout=timeout, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    attempt.update(returnCode=process.returncode, completed=True, logSha256=sha(log))
    text = log.read_text(encoding='utf-8', errors='replace')
    attempt['validationClean'] = not bool(validation_log_error(text)); persist()
    require(process.returncode == 0, f'Native merge process failed ({process.returncode}): {log}')
    require(attempt['validationClean'], 'Vulkan validation failed: '+str(log))
    if name == 'transaction':
        frames = [(row['name'], row['frame']) for row in plan['transactionStates']]
        final_frame = plan['sequence']['frames']
    else:
        frames = [(name, 39 if planned.get('sequence') else 13)]
        final_frame = frames[0][1]
    records = {}
    expected_images = set()
    for label, frame in frames:
        sampled = image if frame == final_frame else image.with_name(image.stem+f'.frame-{frame:04d}.png')
        telemetry = sampled.with_suffix('.telemetry.json')
        require(sampled.is_file() and telemetry.is_file(), 'Missing fresh native image/telemetry: '+str(sampled))
        state = capture_state(telemetry, technique)
        record_name = 'transaction-'+label if name == 'transaction' else label
        record = dict(name=record_name, technique=technique, frame=frame, image=str(sampled), telemetry=str(telemetry),
            log=str(log), imageSha256=sha(sampled), telemetrySha256=sha(telemetry), logSha256=sha(log), validationClean=True)
        report['captures'].append(record)
        records[record_name] = {**record, 'state': state}
        expected_images.add(sampled.resolve())
    require({p.resolve() for p in target.parent.glob(target.name+'*.png')} == expected_images,
            'Unexpected/missing native sequence image count')
    persist()
    return records


def final_export_session(state):
    merge = state['gltfMerge']
    require(merge['outcome'] == 'committed' and merge['error'] == '' and merge['parentName'] == 'B placement 2' and
            merge['requestId'] == merge['sourceReadCount'] == merge['generationPublications'] == 2,
            'Save sequence did not complete exactly two merge requests/publications')
    require(merge['sourceTables'] == expected_state(2)['sourceTables'] and merge['importScale'] == 1. and
            merge['commitGeneration'] == merge['descriptorGeneration'], 'Save sequence published wrong source/descriptor state')
    require(state['gltfScenes']['selected'] == 0 and len(state['gltfScenes']['names']) == 3 and
            state['gltfScenes']['activeRenderableCount'] == 4 and
            state['gltfInstancing']['drawCount'] == 3 and
            state['gltfMaterialVariants']['undoCount'] == 3 and state['gltfMaterialVariants']['redoCount'] == 0 and
            not state['gui']['modelLoading']['pending'], 'Save sequence retained wrong catalog/history/active membership')


def run_native(args, output, plan, report):
    renderer, manifest = args.renderer.resolve(), args.manifest.resolve()
    require(renderer.is_file() and manifest.is_file(), 'Native execution requires an existing renderer and frozen manifest')
    require(not any(output.glob('captures/*')) and not any(output.glob('exports/**/*')) and
            not any(output.glob('portable/**/*')), 'Native evidence directories must be fresh')
    if (output/'results.json').exists():
        old = json.loads((output/'results.json').read_text(encoding='utf-8'))
        require(old.get('generatedOnly') is True and not old.get('nativeValidationPerformed'),
                'Refusing to overwrite an existing native result; choose a fresh output directory')

    def persist():
        report.update(captureCount=len(report['captures']), imageCheckCount=len(report['imageChecks']),
            sessionStateCheckCount=len(report['sessionStateChecks']), exportInspectionCount=len(report['exports']),
            processCount=len(report['processes']))
        (output/'results.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n', encoding='utf-8')

    def provenance():
        require(report['sourceDependencies'] == dependencies(), 'Harness/shared helper source changed during native execution')
        require(sha(output/'launch-plan.json') == report['launchPlanSha256'], 'Native launch plan changed')
        for entry in report['immutableInputs']:
            item = Path(entry['path'])
            require(item.is_file() and item.stat().st_size == entry['byteLength'] and sha(item) == entry['sha256'],
                    'Immutable merge input/oracle changed: '+str(item))
        verified = verify_manifest(manifest)
        require(verified['sha256'] == report['nativeManifestBefore']['sha256'], 'Frozen manifest itself changed')
        return verified

    try:
        report['nativeManifestBefore'] = verify_manifest(manifest)
        frozen = json.loads(manifest.read_text(encoding='utf-8-sig'))['files']
        require(any(Path(name).resolve() == renderer for name in frozen), 'Manifest does not bind this renderer')
        require({'gbuffer.frag.spv', 'forward_opaque.frag.spv'} <= {Path(name).name for name in frozen},
                'Manifest omits raster consumers used by this cohort')
        report['rendererSha256'] = sha(renderer)
        persist()
        for technique in plan['techniques']:
            records = {}
            for planned in plan['launches']:
                override = None
                if planned.get('requireSourceIndependentCopy'):
                    suffix = planned['name'].split('-', 1)[1]
                    exported = Path(planned['source'].replace('{technique}', technique))
                    override, files = portable_copy(exported, output/'portable'/technique/suffix)
                    report['portableCopies'].append(dict(technique=technique, format=suffix,
                        document=str(override), closure=files, sourceReferencesConfinedToCopiedDirectory=True))
                launched = launch(renderer, output, plan, planned, technique, report, persist, args.timeout,
                    source_override=override)
                records.update(launched)
                if planned['name'] == 'transaction':
                    baseline = records['transaction-before']['state']
                    for index, entry in enumerate(plan['transactionStates']):
                        state = records['transaction-'+entry['name']]['state']
                        result = session_check(state, baseline, entry, index)
                        report['sessionStateChecks'].append({'technique': technique, **result}); persist()
                if planned.get('save'):
                    final_export_session(launched[planned['name']]['state'])
                    exported = Path(planned['save'].replace('{technique}', technique))
                    report['exports'].append({'technique': technique, 'format': exported.suffix[1:],
                        **inspect_export(plan, exported)}); persist()
                if planned.get('requireSourceIndependentCopy'):
                    state = launched[planned['name']]['state']
                    require(len(state['gltfScenes']['names']) == 3 and state['gltfScenes']['selected'] == 0 and
                            state['gltfScenes']['activeRenderableCount'] == 4 and
                            state['gltfInstancing']['drawCount'] == 3,
                            'Portable reload lost catalog/hidden sentinel/active placement membership')
            require(len(records) == plan['expectedCapturesPerTechnique'], 'Native capture enumeration mismatch')
            for check in plan['imageChecks']:
                result = compare_images(check, records)
                report['imageChecks'].append({'technique': technique, **check, **result}); persist()
        for actual, expected in (('captures', 'expectedCaptureCount'), ('imageChecks', 'expectedImageCheckCount'),
                                 ('sessionStateChecks', 'expectedSessionStateCheckCount'), ('exports', 'expectedExportInspectionCount'),
                                 ('processes', 'expectedProcessCount')):
            require(len(report[actual]) == report[expected], 'Incomplete native cohort: '+actual)
        report['nativeManifestAfter'] = provenance()
        report['immutableInputsUnchanged'] = report['sourceDependenciesUnchanged'] = True
        report['passed'] = True; persist()
    except Exception as error:
        report['passed'] = False
        report['error'] = str(error)
        try:
            if 'nativeManifestBefore' in report:
                report['nativeManifestAfter'] = provenance()
                report['immutableInputsUnchanged'] = report['sourceDependenciesUnchanged'] = True
        except Exception as provenance_error:
            report['provenanceError'] = str(provenance_error)
        persist()
        raise


def synthetic_state(entry, index):
    """Literal telemetry-shaped data tests verifier branches, never native output."""
    expected = entry['expected']
    a_parent = np.diag([-1., -1., 1., 1.]); a_parent[:3, 3] = [-1.5, 0., 0.]
    a_panel = a_parent.copy(); a_panel[0, 3] = -1.625
    sentinel = np.eye(4); sentinel[:3, 3] = [-2.5, -1.25, 0.]
    retained = [dict(graphNode=source+2, sourceNode=source, name='retained '+str(source), worldColumns=matrix.T.tolist())
        for source, matrix in enumerate((a_parent, a_panel, sentinel))]
    instances = [dict(sourceNode=source, modelColumns=matrix.T.tolist(),
        visible=expected['sentinelVisible'] if source == 2 else True) for source, matrix in ((1, a_panel), (2, sentinel))]
    for number in range(expected['importCount']):
        matrix = np.diag([-.5, -1., 1., 1.]); matrix[:3, 3] = np.asarray(PLACEMENTS[number])+[.125, .125, 0.]
        instances.append(dict(sourceNode=6+5*number, modelColumns=matrix.T.tolist(), visible=True))
    return dict(gltfMerge=dict(requestId=READ_ATTEMPTS[index], sourceReadCount=READ_ATTEMPTS[index],
            generationPublications=PUBLICATIONS[index], commitGeneration=11+PUBLICATIONS[index],
            descriptorGeneration=11+PUBLICATIONS[index], outcome='idle' if index == 0 else 'committed' if index < 7 else 'failed',
            error='Expected failure' if index >= 7 else '',
            parentName='' if index == 0 else 'B placement 1' if index < 4 else 'B placement 2' if index < 7 else 'Must not publish',
            sourceTables=expected['sourceTables'], importScale=1., retainedNodes=retained,
            objectBuffers=[dict(handle=100+index, controllerOwned=True)]),
        gltfScenes=dict(graphIdentity=77, selected=0, names=['same scene']*(1+expected['importCount']),
            activeRenderableCount=2+expected['importCount'],
            activeSourceNodes=[0, 1, 2]+[source+5*number for number in range(expected['importCount']) for source in (5, 6, 7)]),
        gltfMaterialVariants=dict(undoCount=expected['undoCount'], redoCount=expected['redoCount']),
        gltfInstancing=dict(drawCount=expected['activeRenderableCount'], instances=instances),
        gltfNodeFlags=dict(truncated=False, nodes=[dict(graphNode=source+2, sourceIndex=source, active=True,
            parentGraphNode=2 if source == 1 else -1,
            localVisible=expected['sentinelVisible'] if source == 2 else True, localSelectable=True, localHoverable=True)
            for source in (0, 1, 2)]+[dict(graphNode=5, sourceIndex=-1, parentGraphNode=3, active=True,
                localVisible=True, localSelectable=True, localHoverable=True)]),
        gui=dict(selectedMeshNodeId=5, modelLoading=dict(pending=False)),
        scene=dict(primaryImportScale=1., primaryObjectCount=2+expected['importCount']))


def synthetic_export(path):
    """Hand-authored hierarchy exercises inspection; it is not a render oracle."""
    asset = Asset()
    materials = [add_resource(asset, path.parent, origin, image_name=str(i)+'.png')
        for i, origin in enumerate(('A', 'B', 'B'))]
    add_quad(asset, LOCAL_QUAD, materials[0], 'A panel', origin='A')
    add_quad(asset, np.asarray(LOCAL_QUAD)*.25, materials[0], 'A sentinel', origin='A')
    add_quad(asset, LOCAL_QUAD, materials[1], 'B panel', origin='B')
    add_quad(asset, LOCAL_QUAD, materials[2], 'B panel', origin='B')
    asset.model['nodes'] = [
        dict(name='A retained parent', translation=[-1.5, 0., 0.], rotation=[0., 0., 1., 0.], children=[1]),
        dict(name='A retained panel', translation=[.125, 0., 0.], mesh=0),
        dict(name='sentinel', translation=[-2.5, -1.25, 0.], mesh=1,
            extensions={'KHR_node_visibility': dict(visible=False)}),
        dict(name='canonical B1', translation=[.25, .125, 0.], rotation=[0., 0., 1., 0.], scale=[.5, 1., 1.], children=[4]),
        dict(translation=[.25, 0., 0.], mesh=2),
        dict(name='B placement 1', matrix=placement(PLACEMENTS[0]), children=[6]),
        dict(translation=[.25, .125, 0.], rotation=[0., 0., 1., 0.], scale=[.5, 1., 1.], children=[7]),
        dict(translation=[.25, 0., 0.], mesh=2),
        dict(name='canonical B2', translation=[.25, .125, 0.], rotation=[0., 0., 1., 0.], scale=[.5, 1., 1.], children=[9]),
        dict(translation=[.25, 0., 0.], mesh=3),
        dict(name='B placement 2', matrix=placement(PLACEMENTS[1]), children=[11]),
        dict(translation=[.25, .125, 0.], rotation=[0., 0., 1., 0.], scale=[.5, 1., 1.], children=[12]),
        dict(translation=[.25, 0., 0.], mesh=3)]
    asset.model['scenes'] = [dict(nodes=[0, 2, 5, 10], extras=dict(fixtureOrigin='A')),
        dict(nodes=[3], extras=dict(fixtureOrigin='B')), dict(nodes=[8], extras=dict(fixtureOrigin='B'))]
    for index, tag in ((0, 'A-parent'), (1, 'A-panel'), (2, 'A-sentinel'),
                       (3, 'B-parent'), (4, 'B-panel'), (6, 'B-parent'), (7, 'B-panel'),
                       (8, 'B-parent'), (9, 'B-panel'), (11, 'B-parent'), (12, 'B-panel')):
        asset.model['nodes'][index]['extras'] = dict(fixtureTag=tag)
    return write_asset(path, asset)


def self_check():
    with tempfile.TemporaryDirectory(prefix='container-merge-plan-') as temporary:
        plan = build_plan(Path(temporary), ('deferred-raster',))
        first = generate(plan)
        require(first == generate(plan), 'Idempotent generation changed immutable input bytes')
        geometry, identities, rois = geometry_proof(plan), identity_proof(plan), roi_proof(plan)
        require(len(plan['captures']) == 24 and len(plan['imageChecks']) == 25 and len(plan['transactionStates']) == 16,
                'Bounded merge plan counts changed')
        require(len(plan['launches']) == 9 and sum(len(entry['captureNames']) for entry in plan['launches']) == 24,
                'Prospective merge process/capture enumeration differs')
        require(all(entry['maximumByteError'] == 0 for entry in plan['imageChecks'] if entry['kind'] == 'exact'),
                'Exact merge image predicate changed')
        require(plan['nativeReady'] and 'manifest' in plan['nativeExecutionRequirement'], 'Native execution must require a frozen manifest')
        require(validation_log_error('validation layer: vkCmdPipelineBarrier2(): WRITE_AFTER_WRITE hazard detected.') and
                not validation_log_error('Renderer initialized; no synchronization hazards detected'),
                'Shared strict validation scanner changed')
        rejected = []
        changed = np.asarray(BAKED['B1'], dtype=np.float32).copy()
        changed[0, 0] = np.nextafter(changed[0, 0], np.float32(4.))
        try:
            exact_positions(changed, BAKED['B1'], 'one-ULP mutation')
        except RuntimeError:
            rejected.append('one-ULP-baked-position')
        try:
            immutable(Path(plan['paths']['A']).parent/'texture.png', b'changed image')
        except RuntimeError:
            rejected.append('immutable-input-overwrite')
        baseline = synthetic_state(plan['transactionStates'][0], 0)
        for index, entry in enumerate(plan['transactionStates']):
            session_check(synthetic_state(entry, index), baseline, entry, index)
        for name, mutate in (
            ('stale-generation', lambda s: s['gltfMerge'].__setitem__('descriptorGeneration', 1000)),
            ('redo-source-read', lambda s: s['gltfMerge'].__setitem__('sourceReadCount', 2)),
            ('changed-retained-world', lambda s: s['gltfMerge']['retainedNodes'][0]['worldColumns'][3].__setitem__(0, -1.6)),
            ('lost-history', lambda s: s['gltfMaterialVariants'].__setitem__('undoCount', 0)),
            ('inactive-canonical-draw', lambda s: s['gltfScenes'].__setitem__('activeRenderableCount', 4)),
            ('selected-primitive-adopted-by-sentinel', lambda s: s['gltfNodeFlags']['nodes'][-1].__setitem__('parentGraphNode', 4)),
            ('selected-primitive-parent-cycle', lambda s: s['gltfNodeFlags']['nodes'][-1].__setitem__('parentGraphNode', 5)),
            ('selected-primitive-missing-parent', lambda s: s['gltfNodeFlags']['nodes'][-1].__setitem__('parentGraphNode', 999)),
            ('null-object-buffer', lambda s: s['gltfMerge']['objectBuffers'][0].__setitem__('handle', 0))):
            state = synthetic_state(plan['transactionStates'][3], 3); mutate(state)
            try:
                session_check(state, baseline, plan['transactionStates'][3], 3)
            except RuntimeError:
                rejected.append(name)
            else:
                raise RuntimeError('Verifier accepted changed synthetic telemetry: '+name)
        exported = Path(temporary)/'synthetic-export'/'merged.gltf'
        model = synthetic_export(exported)
        inspect_export(plan, exported)
        copied, files = portable_copy(exported, Path(temporary)/'portable-copy')
        inspect_export(plan, copied)
        require(files and all(Path(row['path']).is_relative_to(copied.parent) for row in files), 'Portable closure escaped')
        for name, mutate in (
            ('aliased-export-sampler', lambda m: m['textures'][1].__setitem__('sampler', 0)),
            ('changed-opaque-extra', lambda m: m['materials'][1]['extras']['fooTexture'].__setitem__('index', 0)),
            ('changed-placement', lambda m: m['nodes'][5]['matrix'].__setitem__(12, 1.6)),
            ('duplicate-global-parent', lambda m: m['nodes'][5]['children'].append(4)),
            ('collapsed-export-source-identity', lambda m: m['materials'][2]['pbrMetallicRoughness']['baseColorTexture'].__setitem__('index', 1))):
            changed_model = copy.deepcopy(model); mutate(changed_model)
            exported.write_text(json.dumps(changed_model), encoding='utf-8')
            try:
                inspect_export(plan, exported)
            except RuntimeError:
                rejected.append(name)
            else:
                raise RuntimeError('Verifier accepted changed synthetic export: '+name)
        exported.write_text(json.dumps(model), encoding='utf-8')
        for uri in ('../outside.bin', 'F:/absolute.bin', 'https://example.com/image.png'):
            try:
                external_path(exported.parent, uri)
            except RuntimeError:
                rejected.append('portable-path-'+uri)
            else:
                raise RuntimeError('Verifier accepted nonportable resource URI')
        source_info = dict(index=0, texCoord=0, extras=dict(opaqueIndex=977), extensions={TRANSFORM:
            dict(texCoord=1, offset=[.125, .25], scale=[.75, .5], rotation=.2, extras=dict(index=991))})
        runtime_info = copy.deepcopy(source_info)
        runtime_info['texCoord'] = 1
        runtime_info['extensions'][TRANSFORM].pop('texCoord')
        runtime_info['extensions'][TRANSFORM]['rotation'] = float(np.float32(.2))
        require(texture_info_contract(runtime_info) == texture_info_contract(source_info, source=True),
                'Independent binary32/effective-UV export expectation rejected valid normalization')
        runtime_info['extensions'][TRANSFORM]['rotation'] = float(np.nextafter(np.float32(.2), np.float32(1.)))
        require(texture_info_contract(runtime_info) != texture_info_contract(source_info, source=True),
                'One-ULP exported texture rotation was accepted')
        first_image = Path(temporary)/'predicate-a.png'; second_image = Path(temporary)/'predicate-b.png'
        pixels = np.full((HEIGHT, WIDTH, 4), 255, dtype=np.uint8)
        Image.fromarray(pixels).save(first_image); Image.fromarray(pixels).save(second_image)
        records = dict(a=dict(image=str(first_image)), b=dict(image=str(second_image)))
        exact = dict(name='pure-exact-predicate', kind='exact', images=['a', 'b'], maximumByteError=0)
        compare_images(exact, records)
        pixels[0, 0, 3] = 254; Image.fromarray(pixels).save(second_image)
        try:
            compare_images(exact, records)
        except RuntimeError:
            rejected.append('one-byte-alpha-image-difference')
        else:
            raise RuntimeError('Exact image predicate accepted a changed alpha byte')
        contrast = dict(name='pure-contrast-predicate', kind='contrast', images=['a', 'b'],
            roi=[132, 264, 145, 277], minimumMeanAbsoluteByteDelta=3.)
        pixels[264:277, 132:145, :3] = 251; Image.fromarray(pixels).save(second_image)
        compare_images(contrast, records)
        pixels[264:277, 132:145, :3] = 253; Image.fromarray(pixels).save(second_image)
        try:
            compare_images(contrast, records)
        except RuntimeError:
            rejected.append('vacuous-image-contrast')
        else:
            raise RuntimeError('Contrast predicate accepted a vacuous control')
        require(len(rejected) == 21, 'Pure verification mutation coverage changed')
        capacity, _ = load_asset(Path(plan['paths']['capacity']))
        require(len(capacity['nodes']) == MAX_SCENE_OBJECTS+1 and
                capacity['scenes'][0]['nodes'] == list(range(MAX_SCENE_OBJECTS+1)) and
                all(node['mesh'] == 0 for node in capacity['nodes']), 'Capacity rejection fixture changed')
        return dict(pureSelfCheck=True, nativeValidationPerformed=False, generatedInputCount=len(first),
            expectedCapturesPerTechnique=24, expectedImageChecksPerTechnique=25,
            expectedSessionStateChecksPerTechnique=16, expectedExportInspectionsPerTechnique=2,
            expectedProcessesPerTechnique=9,
            geometry=geometry, sourceIdentities=identities, fixedContrastRois=rois, rejectedMutations=rejected,
            syntheticTelemetryStatesChecked=16, syntheticExportInspected=True, portableClosureChecked=True,
            syntheticPrimitiveSelectionAncestryChecked=True,
            exactAndNonvacuousImagePredicatesChecked=True, exactBinary32UvNormalizationChecked=True,
            nativeExecutionRequiresExplicitRendererAndManifest=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--renderer', type=Path, help='Explicit native executable; not executed with --generate-only')
    parser.add_argument('--manifest', type=Path, help='Required frozen executable/runtime-input manifest for native execution')
    parser.add_argument('--timeout', type=float, default=300., help='Maximum seconds per native process')
    parser.add_argument('--technique', action='append', choices=TECHNIQUES)
    parser.add_argument('--generate-only', action='store_true')
    parser.add_argument('--self-check', action='store_true')
    args = parser.parse_args()
    if args.self_check:
        print(json.dumps(self_check(), indent=2))
        return
    if args.output is None:
        parser.error('--output is required')
    if not args.generate_only and (args.renderer is None or args.manifest is None):
        parser.error('Native execution requires explicit --renderer and --manifest; use --generate-only for pure preparation')
    if args.timeout <= 0 or not np.isfinite(args.timeout):
        parser.error('--timeout must be positive and finite')
    output = args.output.resolve()
    techniques = tuple(dict.fromkeys(args.technique or ('deferred-raster',)))
    plan = build_plan(output, techniques)
    inputs = generate(plan)
    proofs = dict(geometry=geometry_proof(plan), sourceIdentities=identity_proof(plan), fixedContrastRois=roi_proof(plan))
    document(output/'launch-plan.json', plan)
    report = dict(schemaVersion=2, passed=None if args.generate_only else False, generatedOnly=args.generate_only, nativeValidationPerformed=False,
        nativeExecutionRequiresExplicitRendererAndManifest=True, renderer=str(args.renderer.resolve()) if args.renderer else None,
        manifest=str(args.manifest.resolve()) if args.manifest else None, rendererExecuted=False,
        sourceDependencies=dependencies(), immutableInputs=inputs,
        sourceSha256=sha(Path(__file__)),
        launchPlanSha256=sha(output/'launch-plan.json'), pureFixtureProofs=proofs,
        expectedCaptureCount=len(plan['captures'])*len(techniques),
        expectedImageCheckCount=len(plan['imageChecks'])*len(techniques),
        expectedSessionStateCheckCount=len(plan['transactionStates'])*len(techniques),
        expectedExportInspectionCount=len(plan['exports'])*len(techniques),
        expectedProcessCount=len(plan['launches'])*len(techniques),
        captures=[], imageChecks=[], sessionStateChecks=[], exports=[], processes=[], portableCopies=[],
        unverified=plan['unverified'])
    if args.generate_only:
        document(output/'results.json', report)
        print('GENERATED', len(inputs), 'immutable inputs;', report['expectedCaptureCount'],
              'planned captures,', report['expectedImageCheckCount'], 'planned image checks; renderer not executed')
    else:
        run_native(args, output, plan, report)
        print('PASS', report['captureCount'], 'captures;', report['imageCheckCount'], 'exact/control image checks;',
              report['sessionStateCheckCount'], 'session checks;', report['exportInspectionCount'], 'portable export inspections')


if __name__ == '__main__':
    main()
