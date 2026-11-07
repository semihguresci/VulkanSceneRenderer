"""Native rename/empty-child/history and portable scene-membership regression.

The fixture uses literal affine transforms and a single unlit quad. Controls
are metamorphic: metadata edits cannot change its fixed interior/background
pixels. Selection deliberately changes after creation, so those fixed analytic
ROIs exclude the outline; scenes with no renderables compare complete images.
No production renderer or animation evaluator generates a reference.

--generate-only and --self-check never launch a renderer. Native execution
requires --renderer and a manifest binding every imported local Python helper
as well as the executable/runtime files. Existing native evidence is preserved.
"""
from __future__ import annotations

import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile

import numpy as np
from PIL import Image

from gltf_accessor_runtime_regression import Asset, decode_accessor, load_asset, require
from gltf_extended_materials_runtime_regression import sha, validation_log_error
from gltf_merge_runtime_regression import document, forest_worlds, immutable, portable_copy
from gltf_tangent_uv_runtime_regression import finite_json
from gltf_texture_formats_runtime_regression import verify_manifest

TECHNIQUES = ('deferred-raster', 'forward-raster')
WIDTH, HEIGHT, INVALID = 640, 360, 2**32-1
OLD_NAME, NEW_NAME = 'Retained authored group', 'Renamed authored \u6771'
OWNER_NAME = 'Retained mesh owner'
HISTORY_EMPTY, CONTEXT_EMPTY = 'Empty child \u03b2', 'Empty scene context \u03b2'
POSITIONS = [[-1., -.8, 0.], [1., -.8, 0.], [1., .8, 0.], [-1., .8, 0.]]
INDICES = [0, 1, 2, 0, 2, 3]
PARENT_WORLD = [[1., .25, 0., -.25], [0., 1., 0., .125], [0., 0., 1., 0.], [0., 0., 0., 1.]]
OWNER_WORLD = [[1., .25, 0., -.03125], [0., 1., 0., 0.], [0., 0., 1., 0.], [0., 0., 0., 1.]]
# Projected quad: x in approximately [230,405], y in [122,238]. The
# receiver rectangle stays well inside every sheared edge and the 5px outline.
RECEIVER_ROI, BACKGROUND_ROI = [270, 150, 365, 210], [16, 16, 80, 80]
TABLES = dict(nodes=2, meshes=1, materials=1, images=0, textures=0, samplers=0, buffers=1, scenes=2)
HISTORY = [
    ('before', 13, False, False, 0, 0, 2),
    ('rename', 26, True, False, 1, 0, 2),
    ('create', 39, True, True, 2, 0, 4),
    ('undo-empty', 52, True, False, 1, 1, 2),
    ('undo-rename', 65, False, False, 0, 2, 2),
    ('redo-rename', 78, True, False, 1, 1, 2),
    ('redo-empty', 91, True, True, 2, 0, 4)]
MEMBERSHIP = [('before', 13, 1, False, 0, INVALID), ('create', 26, 1, True, 1, 4),
              ('filled', 39, 0, True, 0, INVALID), ('back-empty', 52, 1, True, 0, INVALID)]
UNVERIFIED = [
    'This cohort exercises rename and creation of an empty child; subtree duplicate/delete are outside its scope.',
    'Interior/background pixel equality excludes the intentionally changed selection outline. Empty scenes compare full RGBA images.',
    'No physics, ray-query/DDGI, GPU picking/readback, failure injection or resource-lifetime/leak result is claimed.',
    'Portable reload uses fresh loader selection; editor selection is not serialized into glTF.',
    'Generation and pure self-check do not establish native rendering or GPU execution.']


def fixture(path):
    asset = Asset()
    asset.model['materials'] = [dict(name='Unlit witness', extensions={'KHR_materials_unlit': {}},
        pbrMetallicRoughness=dict(baseColorFactor=[1., .25, .125, 1.], metallicFactor=0., roughnessFactor=1.),
        extras=dict(opaqueMaterialIndex=977))]
    primitive = dict(attributes=dict(POSITION=asset.accessor('VEC3', POSITIONS),
        NORMAL=asset.accessor('VEC3', [[0., 0., 1.]]*4)),
        indices=asset.accessor('SCALAR', INDICES, 5123, indices=True), material=0, mode=4,
        extras=dict(opaquePrimitiveIndex=991))
    asset.model['meshes'] = [dict(name='Shared witness quad', primitives=[primitive], extras=dict(fixtureTag='mesh'))]
    asset.model['nodes'] = [dict(name=OLD_NAME, matrix=np.asarray(PARENT_WORLD).T.reshape(-1).tolist(),
        children=[1], extras=dict(fixtureTag='retained-parent', opaqueNodeIndex=977)),
        dict(name=OWNER_NAME, translation=[.25, -.125, 0.], mesh=0,
             extras=dict(fixtureTag='retained-owner', opaqueNodeIndex=991))]
    asset.model['scenes'] = [dict(name='Filled scene', nodes=[0], extras=dict(fixtureTag='filled')),
        dict(name='Empty scene', nodes=[], extras=dict(fixtureTag='empty'))]
    asset.model['extensionsUsed'] = ['KHR_materials_unlit']
    asset.model['asset']['generator'] = 'Independent node authoring witness'
    # Write through a temporary path because Asset.write is an ordinary writer.
    with tempfile.TemporaryDirectory(prefix='node-authoring-writer-') as temporary:
        generated = Path(temporary)/'scene.gltf'; asset.write(generated)
        immutable(path, generated.read_bytes())


def event(frame, action, name, **selector):
    return dict(frame=frame, nodeAuthoring=dict(action=action, name=name, **selector))


def script(frames, samples, events):
    return dict(schemaVersion=1, frames=frames, sampleFrames=samples, events=events,
        camera=[dict(frame=1, position=[0., 0., 6.], target=[0., 0., 0.])])


def generate(output):
    source = output/'sources'/'scene.gltf'; fixture(source)
    scripts = {
        'history': script(91, [row[1] for row in HISTORY], [
            event(14, 'rename', NEW_NAME, sourceNode=0), event(27, 'create-empty', HISTORY_EMPTY, sourceNode=1),
            dict(frame=40, sceneHistory='undo'), dict(frame=53, sceneHistory='undo'),
            dict(frame=66, sceneHistory='redo'), dict(frame=79, sceneHistory='redo')]),
        'history-save': script(26, [26], [event(2, 'rename', NEW_NAME, sourceNode=0),
            event(14, 'create-empty', HISTORY_EMPTY, sourceNode=1)]),
        'membership': script(52, [row[1] for row in MEMBERSHIP], [
            event(14, 'create-empty', CONTEXT_EMPTY, graphNode=3), dict(frame=27, gltfScene=0), dict(frame=40, gltfScene=1)]),
        'membership-save': script(13, [13], [event(2, 'create-empty', CONTEXT_EMPTY, graphNode=3)]),
        'membership-reload': script(39, [13, 26, 39], [dict(frame=14, gltfScene=0), dict(frame=27, gltfScene=1)])}
    for name, value in scripts.items(): document(output/(name+'.json'), value)
    immutable(output/'vk_layer_settings.txt', b'khronos_validation.validate_sync = true\n')
    files = [source, *(output/(name+'.json') for name in scripts), output/'vk_layer_settings.txt']
    plan = dict(schemaVersion=1, source=str(source), scripts={name: str(output/(name+'.json')) for name in scripts},
        immutableInputs=[dict(path=str(path), sha256=sha(path), byteLength=path.stat().st_size) for path in files],
        expectedCapturesPerTechnique=21, expectedProcessesPerTechnique=8, expectedStateChecksPerTechnique=21,
        expectedImageChecksPerTechnique=20, expectedExportInspectionsPerTechnique=4,
        historyStates=HISTORY, membershipStates=MEMBERSHIP, receiverRoi=RECEIVER_ROI, backgroundRoi=BACKGROUND_ROI,
        pixelPredicates=dict(maximumByteError=0, minimumReceiverBackgroundMeanDelta=10.),
        captureSettings=dict(resolution=[WIDTH, HEIGHT], cameraPosition=[0, 0, 6], cameraTarget=[0, 0, 0], fov=45,
            exposure=.35, importScale=1, msaa=1, taa=False, bloom=False, environmentIntensity=0, directionalIntensity=0,
            rayQueries=False, cameraReappliedEveryFrame=True),
        fixtureRoles=dict(sourceParentGraphNode=0, sourceOwnerGraphNode=1, primitiveGraphNode=2, editorGraphNode=3, firstNewGraphNode=4),
        contextTargetProof='baseline source0 parent=3; node3 is source-unmapped nonrenderable global root; operation also validates its actual editor tag',
        unverified=UNVERIFIED)
    document(output/'launch-plan.json', plan)
    return plan


def rows(state):
    merge = state['gltfMerge']
    require(not merge['retainedNodesTruncated'] and not state['gltfNodeFlags']['truncated'], 'Authoring node evidence is truncated')
    result = {row['graphNode']: row for row in merge['retainedNodes']}
    require(len(result) == len(merge['retainedNodes']) == merge['graphNodeCount'] == state['gltfScenes']['authoringNodeCount'],
            'Missing or duplicate graph-node telemetry')
    return result


def matrix(row):
    value = np.asarray(row['worldColumns'], dtype=float)
    require(value.shape == (4, 4) and np.isfinite(value).all(), 'Invalid authoring world matrix')
    return value.T


def common_state(state, scene, renderables):
    merge, scenes = state['gltfMerge'], state['gltfScenes']
    require(merge['graphIdentity'] == scenes['graphIdentity'] and merge['graphIdentity'] > 0,
            'Graph identity telemetry disagrees')
    require(scenes['selected'] == scene and scenes['names'] == ['Filled scene', 'Empty scene'], 'Wrong scene catalog/selection')
    require(scenes['activeRenderableCount'] == state['scene']['primaryObjectCount'] == renderables and
            state['gltfInstancing']['drawCount'] == renderables, 'Empty node changed rendered membership')
    require(not state['gui']['modelLoading']['pending'] and state['scene']['primaryImportScale'] == 1., 'Authoring did not settle')
    require(merge['requestId'] == merge['sourceReadCount'] == merge['generationPublications'] == 0 and
            merge['outcome'] == 'idle' and merge['error'] == '' and merge['commitGeneration'] == merge['descriptorGeneration'],
            'Node authoring accidentally used model merge/publication state')


def history_cursor(state, undo, redo, *, allow_uncreated=False):
    history = state['gltfMaterialVariants']
    present = tuple(key in history for key in ('undoCount', 'redoCount'))
    if present == (False, False):
        require(allow_uncreated and undo == redo == 0 and state['gltfScenes']['editHistoryDepth'] == 0,
                'Missing history counters outside an uncreated-history baseline')
    else:
        require(present == (True, True) and type(history['undoCount']) is int and type(history['redoCount']) is int and
                (history['undoCount'], history['redoCount']) == (undo, redo), 'Wrong or incomplete authoring history cursor')
    return present


def original_state(state, *, renamed, created, scene, undo, redo, selected, context=False, baseline=None, before=False):
    common_state(state, scene, int(scene == 0))
    merge = state['gltfMerge']
    require(merge['sourceTables'] == TABLES and merge['graphNodeCount'] == 4+int(created), 'Authoring changed source tables or wrong node count')
    require(not before or not renamed and not created and undo == redo == 0, 'Invalid initial authoring baseline allowance')
    history_cursor(state, undo, redo, allow_uncreated=before)
    require(state['gui']['selectedMeshNodeId'] == selected, 'Authoring selection/history restoration changed')
    expected_roots = [0] if scene == 0 else [4] if created else []
    require(merge['activeRoots'] == expected_roots, 'Exact active-root view changed')
    require(state['gltfScenes']['activeSourceNodes'] == ([0, 1] if scene == 0 else []), 'Inactive source membership changed')
    nodes = rows(state)
    for index, source, parent, owner, renderable, world in (
        (0, 0, 3, INVALID, False, PARENT_WORLD), (1, 1, 0, INVALID, False, OWNER_WORLD),
        (2, -1, 1, 1, True, OWNER_WORLD), (3, -1, INVALID, INVALID, False, np.eye(4))):
        row = nodes[index]
        require((row['sourceNode'], row['parentGraphNode'], row['meshOwnerGraphNode'], row['renderable']) ==
                (source, parent, owner, renderable), 'Retained graph/source/owner topology changed')
        require(row['active'] == (scene == 0 and index < 3), 'Retained active/context membership changed')
        require(np.array_equal(matrix(row), world), 'Retained world differs from literal independent affine expectation')
    require(nodes[0]['name'] == (NEW_NAME if renamed else OLD_NAME) and nodes[1]['name'] == OWNER_NAME,
            'Wrong rename target or old label restoration')
    if baseline is not None:
        require(merge['graphIdentity'] == baseline['gltfMerge']['graphIdentity'] and
                merge['commitGeneration'] == baseline['gltfMerge']['commitGeneration'], 'Authoring replaced the live graph/source generation')
        previous = rows(baseline)
        for index in range(4):
            expected = copy.deepcopy(previous[index]); actual = copy.deepcopy(nodes[index])
            for item in (expected, actual): item.pop('active')
            if index == 0: expected['name'] = NEW_NAME if renamed else OLD_NAME
            require(actual == expected, 'Retained payload changed beyond the requested name/view')
    if created:
        added = nodes[4]
        require(added['name'] == (CONTEXT_EMPTY if context else HISTORY_EMPTY) and
                added['sourceNode'] == -1 and added['parentGraphNode'] == (3 if context else 1) and
                added['meshOwnerGraphNode'] == INVALID and not added['renderable'] and added['active'] == (scene == (1 if context else 0)),
                'New empty node has wrong identity, parent, membership or geometry attachment')
        require(np.array_equal(matrix(added), np.eye(4) if context else OWNER_WORLD), 'New empty node inherited the wrong transform')
    flags = {row['graphNode']: row for row in state['gltfNodeFlags']['nodes']}
    require(all(flags[index]['localVisible'] and flags[index]['localSelectable'] and flags[index]['localHoverable']
                for index in nodes), 'Authoring unexpectedly changed local flags')
    return dict(passed=True, graphIdentity=merge['graphIdentity'], graphNodeCount=len(nodes), activeRoots=merge['activeRoots'],
        selectedMeshNodeId=selected, undoCount=undo, redoCount=redo, retainedWorldsExact=True, sourceTablesUnchanged=True)


def export_inspection(path, *, context):
    model, _ = load_asset(path); worlds, _ = forest_worlds(model)
    require((path.read_bytes()[:4] == b'glTF') == (path.suffix == '.glb'), 'Wrong authoring export container')
    require(len(model['scenes']) == 2 and model['scene'] == int(context) and
            [scene['name'] for scene in model['scenes']] == ['Filled scene', 'Empty scene'] and
            [scene.get('extras') for scene in model['scenes']] == [dict(fixtureTag='filled'), dict(fixtureTag='empty')],
            'Export lost scene catalog/default/metadata')
    tagged_rows = [(node.get('extras', {}).get('fixtureTag'), i) for i, node in enumerate(model['nodes'])
                  if node.get('extras', {}).get('fixtureTag') in ('retained-parent', 'retained-owner')]
    tagged = dict(tagged_rows)
    require(len(tagged_rows) == 2 and set(tagged) == {'retained-parent', 'retained-owner'}, 'Source node metadata was lost or duplicated')
    parent, owner = tagged['retained-parent'], tagged['retained-owner']
    require(model['nodes'][parent]['name'] == (OLD_NAME if context else NEW_NAME) and
            model['nodes'][owner]['name'] == OWNER_NAME and owner in model['nodes'][parent]['children'], 'Export changed names/source hierarchy')
    require(model['nodes'][parent]['extras'] == dict(fixtureTag='retained-parent', opaqueNodeIndex=977) and
            model['nodes'][owner]['extras'] == dict(fixtureTag='retained-owner', opaqueNodeIndex=991), 'Export rewrote opaque node extras')
    require(np.array_equal(worlds[parent], PARENT_WORLD) and np.array_equal(worlds[owner], OWNER_WORLD), 'Export changed source affine worlds')
    empty = [i for i, node in enumerate(model['nodes']) if node.get('name') == (CONTEXT_EMPTY if context else HISTORY_EMPTY)]
    require(len(empty) == 1, 'Export lost or duplicated empty node'); empty = empty[0]
    empty_node = model['nodes'][empty]
    identity = np.eye(4).T.reshape(-1).tolist()
    require(empty_node.get('matrix', identity) == identity and empty_node.get('translation', [0, 0, 0]) == [0, 0, 0] and
            empty_node.get('rotation', [0, 0, 0, 1]) == [0, 0, 0, 1] and empty_node.get('scale', [1, 1, 1]) == [1, 1, 1] and
            not ('matrix' in empty_node and any(field in empty_node for field in ('translation', 'rotation', 'scale'))),
            'Export changed empty node identity local transform')
    require(not empty_node.get('children') and not any(field in empty_node for field in ('mesh', 'skin', 'camera')) and
            'EXT_mesh_gpu_instancing' not in empty_node.get('extensions', {}) and
            'KHR_lights_punctual' not in empty_node.get('extensions', {}) and
            np.array_equal(worlds[empty], np.eye(4) if context else OWNER_WORLD), 'Empty export acquired geometry/children or wrong world')
    for index in (parent, owner, empty):
        for extension, field in (('KHR_node_visibility', 'visible'), ('KHR_node_selectability', 'selectable'), ('KHR_node_hoverability', 'hoverable')):
            require(model['nodes'][index].get('extensions', {}).get(extension, {}).get(field, True) is True,
                    'Export changed authored/empty local flags')
    members = []
    for scene in model['scenes']:
        values = list(scene.get('nodes', []))
        for index in values: values.extend(model['nodes'][index].get('children', []))
        members.append(set(values))
    require(parent in members[0] and owner in members[0] and empty in members[int(context)] and
            empty not in members[1-int(context)] and not members[1].intersection({parent, owner}),
            'Empty node leaked into another scene or source scene roots changed')
    if context: require(empty in model['scenes'][1]['nodes'], 'Context-created empty is not an independent scene view root')
    else: require(empty in model['nodes'][owner]['children'], 'Empty child lost its authored parent')
    mesh_nodes = [i for i, node in enumerate(model['nodes']) if 'mesh' in node]
    require(len(mesh_nodes) == 1 and mesh_nodes[0] in members[0], 'Empty authoring added/removed renderable geometry')
    primitive = model['meshes'][model['nodes'][mesh_nodes[0]]['mesh']]['primitives'][0]
    require(primitive.get('extras') == dict(opaquePrimitiveIndex=991), 'Export changed opaque primitive metadata')
    require(np.array_equal(decode_accessor(path, primitive['attributes']['POSITION']), np.asarray(POSITIONS, dtype=np.float32)) and
            np.array_equal(decode_accessor(path, primitive['attributes']['NORMAL']), np.asarray([[0., 0., 1.]]*4, dtype=np.float32)) and
            np.array_equal(decode_accessor(path, primitive['indices']).reshape(-1), INDICES), 'Export changed source vertex/normal/index bytes')
    require(np.array_equal(worlds[mesh_nodes[0]], OWNER_WORLD), 'Export changed renderable world')
    material = model['materials'][primitive['material']]
    require(material.get('extras') == dict(opaqueMaterialIndex=977) and material.get('extensions', {}).get('KHR_materials_unlit') == {} and
            material['pbrMetallicRoughness']['baseColorFactor'] == [1., .25, .125, 1.], 'Export changed material resource')
    return dict(passed=True, path=str(path), sha256=sha(path), context=context, sourceNodeCount=len(model['nodes']),
        emptySourceNode=empty, parentSourceNode=parent, ownerSourceNode=owner, meshSourceNode=mesh_nodes[0],
        worldMatricesExact=True, geometryExact=True, opaqueMetadataExact=True, sceneMembershipExact=True)


def reload_state(state, inspected, *, scene, baseline=None):
    common_state(state, scene, int(scene == 0))
    nodes = rows(state)
    source = {row['sourceNode']: row for row in nodes.values() if row['sourceNode'] >= 0}
    require(len(source) == inspected['sourceNodeCount'] == state['gltfMerge']['sourceTables']['nodes'] and
            len(source) == sum(row['sourceNode'] >= 0 for row in nodes.values()) and
            set(source) == set(range(inspected['sourceNodeCount'])), 'Reload lost/duplicated complete serialized source nodes')
    parent, owner, empty = (source[inspected[key]] for key in ('parentSourceNode', 'ownerSourceNode', 'emptySourceNode'))
    context = inspected['context']
    require(parent['name'] == (OLD_NAME if context else NEW_NAME) and owner['name'] == OWNER_NAME and
            empty['name'] == (CONTEXT_EMPTY if context else HISTORY_EMPTY) and not empty['renderable'], 'Portable reload lost authored names/empty identity')
    require(np.array_equal(matrix(parent), PARENT_WORLD) and np.array_equal(matrix(owner), OWNER_WORLD) and
            np.array_equal(matrix(empty), np.eye(4) if context else OWNER_WORLD), 'Portable reload changed literal worlds')
    require(empty['active'] == (scene == int(context)) and parent['active'] == owner['active'] == (scene == 0),
            'Portable reload leaked scene membership')
    expected_roots = [parent['graphNode']] if scene == 0 else [empty['graphNode']] if context else []
    require(state['gltfMerge']['activeRoots'] == expected_roots, 'Portable reload changed exact scene roots')
    # This process only loads/switches scenes: it never creates an edit history.
    # Lazy no-UI telemetry may omit both counters; that shape must stay fixed.
    shape = history_cursor(state, 0, 0, allow_uncreated=True)
    if baseline is not None:
        initial = history_cursor(baseline, 0, 0, allow_uncreated=True)
        require(shape == initial, 'Portable reload unexpectedly changed lazy history-counter shape')
    flags = {row['graphNode']: row for row in state['gltfNodeFlags']['nodes']}
    require(all(flags[row['graphNode']]['localVisible'] and flags[row['graphNode']]['localSelectable'] and
                flags[row['graphNode']]['localHoverable'] for row in (parent, owner, empty)), 'Reload changed authored/empty local flags')
    if baseline is not None:
        require(state['gltfMerge']['graphIdentity'] == baseline['gltfMerge']['graphIdentity'], 'Scene switch replaced portable graph identity')
        before = rows(baseline)
        for index, row in nodes.items():
            old = copy.deepcopy(before[index]); current = copy.deepcopy(row)
            old.pop('active'); current.pop('active')
            require(old == current, 'Scene switching changed portable node payloads')
        require(state['gui']['selectedMeshNodeId'] == INVALID, 'Scene switch did not clear editor selection')
    elif scene == 0:
        selected = nodes.get(state['gui']['selectedMeshNodeId'])
        require(selected is not None and selected['renderable'] and selected['active'], 'Filled reload did not select its actual renderable')
    else: require(state['gui']['selectedMeshNodeId'] == INVALID, 'Empty reload selected a nonrenderable source')
    return dict(passed=True, scene=scene, graphIdentity=state['gltfMerge']['graphIdentity'], activeRoots=expected_roots,
        sourceIdentitiesComplete=True, emptyMembershipExact=True, literalWorldsExact=True)


def rgba(record):
    with Image.open(record['image']) as image:
        require(image.size == (WIDTH, HEIGHT), 'Wrong authoring capture dimensions')
        return np.asarray(image.convert('RGBA'), dtype=np.int16)


def image_check(first, second, *, full=False):
    a, b = rgba(first), rgba(second)
    regions = [[0, 0, WIDTH, HEIGHT]] if full else [RECEIVER_ROI, BACKGROUND_ROI]
    maxima = []
    for left, top, right, bottom in regions:
        maxima.append(int(np.abs(a[top:bottom, left:right]-b[top:bottom, left:right]).max()))
    require(max(maxima) == 0, f'Authoring changed fixed rendered pixels: max={maxima}')
    return dict(passed=True, fullImage=full, regions=regions, maximumByteError=0)


def nonvacuous(record):
    values = rgba(record)
    def mean(roi):
        left, top, right, bottom = roi
        return values[top:bottom, left:right, :3].mean(axis=(0, 1))
    delta = float(np.abs(mean(RECEIVER_ROI)-mean(BACKGROUND_ROI)).mean())
    require(delta >= 10., 'Surface/background authoring control is vacuous')
    return dict(passed=True, meanReceiverBackgroundDelta=delta, minimumDelta=10.)


def dependencies():
    directory = Path(__file__).resolve().parent
    result = {Path(__file__).resolve()}
    for module in tuple(sys.modules.values()):
        path = getattr(module, '__file__', None)
        if path and Path(path).resolve().parent == directory: result.add(Path(path).resolve())
    return {str(path): sha(path) for path in sorted(result)}


def launch(args, output, plan, report, persist, verify, technique, name, source, *, scene, labels, sequence=None, save=None):
    verify()
    target = output/'captures'/(technique+'-'+name)
    target.parent.mkdir(parents=True, exist_ok=True)
    require(not any(target.parent.glob(target.name+'*')), 'Refusing to overwrite authoring native evidence')
    image, log = target.with_suffix('.png'), target.with_suffix('.log')
    command = [str(args.renderer.resolve()), '--hidden', '--no-ui', '--validation', '--no-bloom', '--no-taa',
        '--width', str(WIDTH), '--height', str(HEIGHT), '--msaa', '1', '--warmup-frames', '12', '--capture-frame', '13',
        '--fixed-dt', '.016666667', '--render-technique', technique, '--display-mode', 'lit', '--exposure', '.35',
        '--camera-position', '0', '0', '6', '--camera-target', '0', '0', '0', '--camera-fov', '45',
        '--environment-intensity', '0', '--directional-intensity', '0', '--no-ray-query', '--ray-shadows', 'raster',
        '--import-scale', '1', '--gltf-scene', str(scene), '--model', str(source), '--screenshot', str(image)]
    if sequence: command += ['--capture-sequence', str(sequence)]
    if save:
        save.parent.mkdir(parents=True, exist_ok=True); require(not save.exists(), 'Refusing to overwrite native authoring export')
        command += ['--save-scene', str(save)]
    process_row = dict(name=name, technique=technique, command=command, log=str(log), completed=False)
    report['processes'].append(process_row); report['rendererExecuted'] = True; persist()
    with log.open('x', encoding='utf-8') as stream:
        process = subprocess.run(command, cwd=args.renderer.resolve().parent, stdout=stream, stderr=subprocess.STDOUT,
            timeout=args.timeout, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    process_row.update(returnCode=process.returncode, completed=True, logSha256=sha(log)); persist()
    require(process.returncode == 0, 'Native authoring process failed: '+str(log))
    require(not validation_log_error(log.read_text(encoding='utf-8', errors='replace')), 'Vulkan validation failed: '+str(log))
    records = {}
    for label, frame in labels:
        sampled = image if frame == labels[-1][1] else image.with_name(image.stem+f'.frame-{frame:04d}.png')
        telemetry = sampled.with_suffix('.telemetry.json')
        require(sampled.is_file() and telemetry.is_file(), 'Missing authoring capture image/telemetry')
        state = json.loads(telemetry.read_text(encoding='utf-8-sig'))
        require(finite_json(state) and state['technique'] == technique and state['resolution'] == [WIDTH, HEIGHT] and
                state['msaaSamples'] == 1 and not state['taa']['enabled'], 'Wrong authoring capture settings')
        require(state['lighting']['environmentIntensity'] == state['lighting']['directionalIntensity'] == 0 and
                any(row['active'] and row['gpuTimed'] for row in state['passes']), 'Wrong lighting or no GPU-timed frame')
        row = dict(name=label, technique=technique, frame=frame, image=str(sampled), telemetry=str(telemetry), log=str(log),
            imageSha256=sha(sampled), telemetrySha256=sha(telemetry), logSha256=sha(log), validationClean=True)
        report['captures'].append(row); records[label] = {**row, 'state': state}
    expected = {Path(row['image']).resolve() for row in records.values()}
    require({path.resolve() for path in target.parent.glob(target.name+'*.png')} == expected, 'Wrong native authoring sample count')
    persist(); verify()
    return records


def run_native(args, output, plan, report):
    require(args.renderer.is_file() and args.manifest.is_file(), 'Missing explicit renderer/frozen manifest')
    require(not any(output.glob('captures/*')) and not any(output.glob('exports/**/*')) and not any(output.glob('portable/**/*')),
            'Native authoring evidence folders must be fresh')
    if (output/'results.json').exists():
        previous = json.loads((output/'results.json').read_text(encoding='utf-8'))
        require(previous.get('generatedOnly') and not previous.get('rendererExecuted'), 'Refusing to overwrite native results')
    manifest_hash = sha(args.manifest)
    def persist():
        report.update(captureCount=len(report['captures']), stateCheckCount=len(report['stateChecks']),
            imageCheckCount=len(report['imageChecks']), exportInspectionCount=len(report['exports']), processCount=len(report['processes']))
        (output/'results.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    def verify():
        require(sha(args.manifest) == manifest_hash, 'Authoring manifest changed')
        verified = verify_manifest(args.manifest)
        require(dependencies() == report['sourceDependencies'], 'Authoring harness/imported Python changed')
        require(sha(output/'launch-plan.json') == report['launchPlanSha256'], 'Authoring plan changed')
        if 'runtimeShaderInputs' in report:
            current = sorted(str(path.resolve()) for path in (args.renderer.resolve().parent/'spv_shaders').glob('*.spv'))
            require(current == report['runtimeShaderInputs'], 'Runtime shader inventory changed')
        for item in plan['immutableInputs']:
            path = Path(item['path'])
            require(path.stat().st_size == item['byteLength'] and sha(path) == item['sha256'], 'Authoring fixture/script changed')
        return verified
    def state_record(name, technique, result):
        report['stateChecks'].append(dict(name=name, technique=technique, **result)); persist()
    def pixel_record(name, technique, result):
        report['imageChecks'].append(dict(name=name, technique=technique, **result)); persist()
    try:
        report['nativeManifestBefore'] = verify_manifest(args.manifest)
        frozen = {Path(path).resolve() for path in json.loads(args.manifest.read_text(encoding='utf-8-sig'))['files']}
        require(args.renderer.resolve() in frozen, 'Manifest does not bind selected authoring executable')
        require({Path(path).resolve() for path in report['sourceDependencies']} <= frozen,
                'Manifest must bind every transitive local Python helper, including this harness')
        shaders = list((args.renderer.resolve().parent/'spv_shaders').glob('*.spv'))
        require(shaders and {path.resolve() for path in shaders} <= frozen, 'Manifest omits resident runtime SPIR-V inputs')
        report['runtimeShaderInputs'] = sorted(str(path.resolve()) for path in shaders)
        report['rendererSha256'] = sha(args.renderer); persist(); verify()
        for technique in args.technique:
            source, scripts = Path(plan['source']), {name: Path(path) for name, path in plan['scripts'].items()}
            history, membership = {}, {}
            for cohort in ('history', 'membership'):
                inspected = {}
                for suffix in ('glb', 'gltf'):
                    exported = output/'exports'/technique/cohort/('scene.'+suffix)
                    if cohort == 'history':
                        labels = [('history-'+row[0], row[1]) for row in HISTORY] if suffix == 'glb' else [('history-save-gltf', 26)]
                        sequence = scripts['history' if suffix == 'glb' else 'history-save']; scene = 0
                    else:
                        labels = [('membership-'+row[0], row[1]) for row in MEMBERSHIP] if suffix == 'glb' else [('membership-save-gltf', 13)]
                        sequence = scripts['membership' if suffix == 'glb' else 'membership-save']; scene = 1
                    captured = launch(args, output, plan, report, persist, verify, technique, cohort+'-'+suffix,
                        source, scene=scene, labels=labels, sequence=sequence, save=exported)
                    (history if cohort == 'history' else membership).update(captured)
                    if cohort == 'history' and suffix == 'glb':
                        baseline = captured['history-before']['state']
                        for name, _, renamed, created, undo, redo, selected in HISTORY:
                            state_record('history-'+name, technique, original_state(captured['history-'+name]['state'],
                                renamed=renamed, created=created, scene=0, undo=undo, redo=redo, selected=selected, baseline=baseline, before=name == 'before'))
                    elif cohort == 'history':
                        state_record('history-save-gltf', technique, original_state(captured['history-save-gltf']['state'],
                            renamed=True, created=True, scene=0, undo=2, redo=0, selected=4))
                    elif suffix == 'glb':
                        baseline = captured['membership-before']['state']
                        for name, _, selected_scene, created, undo, selected in MEMBERSHIP:
                            state_record('membership-'+name, technique, original_state(captured['membership-'+name]['state'],
                                renamed=False, created=created, scene=selected_scene, undo=undo, redo=0, selected=selected,
                                context=True, baseline=baseline, before=name == 'before'))
                    else:
                        state_record('membership-save-gltf', technique, original_state(captured['membership-save-gltf']['state'],
                            renamed=False, created=True, scene=1, undo=1, redo=0, selected=4, context=True))
                    inspected[suffix] = export_inspection(exported, context=cohort == 'membership')
                    report['exports'].append(dict(technique=technique, cohort=cohort, format=suffix, **inspected[suffix])); persist()
                for suffix in ('glb', 'gltf'):
                    exported = output/'exports'/technique/cohort/('scene.'+suffix)
                    copied, files = portable_copy(exported, output/'portable'/technique/cohort/suffix)
                    report['portableCopies'].append(dict(technique=technique, cohort=cohort, format=suffix,
                        document=str(copied), files=files, allFileReferencesConfinedToCopy=True))
                    labels = [(cohort+'-reload-'+suffix, 13)] if cohort == 'history' else [
                        ('membership-reload-'+suffix+'-'+name, frame) for name, frame in (('empty', 13), ('filled', 26), ('back-empty', 39))]
                    captured = launch(args, output, plan, report, persist, verify, technique, cohort+'-reload-'+suffix,
                        copied, scene=int(cohort == 'membership'), labels=labels,
                        sequence=scripts['membership-reload'] if cohort == 'membership' else None)
                    (history if cohort == 'history' else membership).update(captured)
                    baseline = next(iter(captured.values()))['state']
                    for index, (name, _) in enumerate(labels):
                        selected_scene = 0 if cohort == 'history' or index == 1 else 1
                        state_record(name, technique, reload_state(captured[name]['state'], inspected[suffix],
                            scene=selected_scene, baseline=baseline if index else None))
            before = history['history-before']
            for name, record in history.items():
                if name != 'history-before': pixel_record(name+'-surface-unchanged', technique, image_check(record, before))
            pixel_record('visible-surface-control', technique, nonvacuous(before))
            blank = membership['membership-before']
            for name, record in membership.items():
                if name == 'membership-before': continue
                empty_scene = record['state']['gltfScenes']['selected'] == 1
                pixel_record(name+'-pixels', technique, image_check(record, blank if empty_scene else before, full=empty_scene))
        for actual, expected in (('captures', 'expectedCaptureCount'), ('stateChecks', 'expectedStateCheckCount'),
                ('imageChecks', 'expectedImageCheckCount'), ('exports', 'expectedExportInspectionCount'), ('processes', 'expectedProcessCount')):
            require(len(report[actual]) == report[expected], 'Incomplete authoring cohort: '+actual)
        report['passed'] = report['nativeValidationPerformed'] = True
    except Exception as error:
        report['passed'] = False; report['failure'] = str(error)
        raise
    finally:
        try:
            report['nativeManifestAfter'] = verify(); report['afterFreezeVerified'] = True
        except Exception as error:
            report['afterFreezeVerified'] = False; report['freezeAuditFailure'] = str(error)
            if 'failure' not in report:
                report['passed'] = False; report['failure'] = 'After-run freeze audit failed: '+str(error)
                persist(); raise
        persist()


def synthetic_state(*, renamed=False, created=False, scene=0, undo=0, redo=0, selected=2, context=False):
    """Verifier-only telemetry; never counted as native evidence."""
    nodes = []
    for index, source, parent, owner, renderable, name, world in (
        (0, 0, 3, INVALID, False, NEW_NAME if renamed else OLD_NAME, PARENT_WORLD),
        (1, 1, 0, INVALID, False, OWNER_NAME, OWNER_WORLD),
        (2, -1, 1, 1, True, 'Primitive 0', OWNER_WORLD),
        (3, -1, INVALID, INVALID, False, 'Scene Root', np.eye(4))):
        nodes.append(dict(graphNode=index, sourceNode=source, parentGraphNode=parent, meshOwnerGraphNode=owner,
            renderable=renderable, name=name, worldColumns=np.asarray(world).T.tolist(), active=scene == 0 and index < 3))
    if created:
        nodes.append(dict(graphNode=4, sourceNode=-1, parentGraphNode=3 if context else 1, meshOwnerGraphNode=INVALID,
            renderable=False, name=CONTEXT_EMPTY if context else HISTORY_EMPTY,
            worldColumns=np.asarray(np.eye(4) if context else OWNER_WORLD).T.tolist(), active=scene == int(context)))
    return dict(gltfMerge=dict(retainedNodes=nodes, retainedNodesTruncated=False, graphNodeCount=len(nodes), graphIdentity=19,
        activeRoots=[0] if scene == 0 else [4] if created else [], sourceTables=copy.deepcopy(TABLES), requestId=0,
        sourceReadCount=0, generationPublications=0, outcome='idle', error='', commitGeneration=0, descriptorGeneration=0),
        gltfScenes=dict(graphIdentity=19, authoringNodeCount=len(nodes), selected=scene, names=['Filled scene', 'Empty scene'],
            activeRenderableCount=int(scene == 0), activeSourceNodes=[0, 1] if scene == 0 else [], editHistoryDepth=undo+redo),
        gltfNodeFlags=dict(truncated=False, nodes=[dict(graphNode=node['graphNode'], localVisible=True,
            localSelectable=True, localHoverable=True) for node in nodes]),
        gltfMaterialVariants=dict(undoCount=undo, redoCount=redo), gltfInstancing=dict(drawCount=int(scene == 0)),
        scene=dict(primaryObjectCount=int(scene == 0), primaryImportScale=1.),
        gui=dict(selectedMeshNodeId=selected, modelLoading=dict(pending=False)))


def synthetic_export(source, path, *, context):
    model, buffers = load_asset(source)
    model['nodes'][0]['name'] = OLD_NAME if context else NEW_NAME
    model['nodes'][1].pop('mesh'); model['nodes'][1]['children'] = [2]+([] if context else [4])
    model['nodes'].extend([dict(name='Primitive 0', mesh=0), dict(name='Scene Root'),
        dict(name=CONTEXT_EMPTY if context else HISTORY_EMPTY)])
    model['scenes'][1]['nodes'] = [4] if context else []
    model['scene'] = int(context)
    model['buffers'] = [dict(byteLength=len(buffers[0]), uri='mesh.bin')]
    immutable(path.parent/'mesh.bin', buffers[0])
    if path.suffix == '.glb':
        encoded = json.dumps(model).encode(); encoded += b' '*((-len(encoded)) % 4)
        payload = struct.pack('<4sII', b'glTF', 2, 20+len(encoded))+struct.pack('<II', len(encoded), 0x4E4F534A)+encoded
        immutable(path, payload)
    else: document(path, model)
    return model


def synthetic_reload(model, inspected, *, scene, selected):
    # Hand-written identities of the deliberately tiny exported hierarchy:
    # source mesh node2 gets one synthetic primitive3; the source-less outer
    # editor is6. Source node3 is the old orphan editor, not the new context.
    context = inspected['context']
    mapping = {0: 0, 1: 1, 2: 2, 3: 4 if context else 5, 4: 5 if context else 4}
    worlds, _ = forest_worlds(model)
    parents = {child: parent for parent, node in enumerate(model['nodes']) for child in node.get('children', [])}
    active = {0, 1, 2, 3} if scene == 0 else {mapping[4]} if context else set()
    if scene == 0 and not context: active.add(mapping[4])
    nodes = []
    for source, index in mapping.items():
        nodes.append(dict(graphNode=index, sourceNode=source, parentGraphNode=mapping[parents[source]] if source in parents else 6,
            meshOwnerGraphNode=INVALID, renderable=False, name=model['nodes'][source].get('name', ''),
            worldColumns=worlds[source].T.tolist(), active=index in active))
    nodes.extend([dict(graphNode=3, sourceNode=-1, parentGraphNode=2, meshOwnerGraphNode=2, renderable=True,
        name='Primitive 0', worldColumns=np.asarray(OWNER_WORLD).T.tolist(), active=scene == 0),
        dict(graphNode=6, sourceNode=-1, parentGraphNode=INVALID, meshOwnerGraphNode=INVALID, renderable=False,
            name='Scene Root', worldColumns=np.eye(4).T.tolist(), active=False)])
    state = synthetic_state(scene=scene, selected=selected)
    state['gltfMerge'].update(retainedNodes=nodes, graphNodeCount=7,
        activeRoots=[0] if scene == 0 else [mapping[4]] if context else [])
    state['gltfMerge']['sourceTables']['nodes'] = 5
    state['gltfScenes']['authoringNodeCount'] = 7
    state['gltfNodeFlags']['nodes'] = [dict(graphNode=node['graphNode'], localVisible=True,
        localSelectable=True, localHoverable=True) for node in nodes]
    return state


def roi_proof():
    focal = HEIGHT/(2*np.tan(np.pi/8))
    inverse = np.linalg.inv(np.asarray(OWNER_WORLD))
    left, top, right, bottom = RECEIVER_ROI
    for x, y in ((left-8, top-8), (right+8, top-8), (left-8, bottom+8), (right+8, bottom+8)):
        point = inverse@[(x-WIDTH/2)*6/focal, (HEIGHT/2-y)*6/focal, 0., 1.]
        require(-1 < point[0] < 1 and -.8 < point[1] < .8, 'Fixed receiver ROI is not safely inside literal projected geometry')
    require(BACKGROUND_ROI[2] < 150 and BACKGROUND_ROI[3] < 100, 'Fixed background ROI overlaps the receiver')
    return dict(analyticCameraProjection=True, receiverInsetMoreThanEightPixels=True, masksLearnedFromOutput=False)


def self_check():
    with tempfile.TemporaryDirectory(prefix='node-authoring-self-check-') as temporary:
        output = Path(temporary); plan = generate(output)
        require(plan == generate(output), 'Authoring generation is not immutable/idempotent')
        require(len(plan['immutableInputs']) == 7, 'Authoring input count changed')
        roi = roi_proof()
        baseline = synthetic_state()
        lazy = synthetic_state(); lazy['gltfMaterialVariants'].clear()
        original_state(lazy, renamed=False, created=False, scene=0, undo=0, redo=0, selected=2, before=True)
        for _, _, renamed, created, undo, redo, selected in HISTORY:
            original_state(synthetic_state(renamed=renamed, created=created, undo=undo, redo=redo, selected=selected),
                renamed=renamed, created=created, scene=0, undo=undo, redo=redo, selected=selected, baseline=baseline)
        empty = synthetic_state(scene=1, selected=INVALID, context=True)
        for _, _, scene, created, undo, selected in MEMBERSHIP:
            state = synthetic_state(scene=scene, created=created, undo=undo, selected=selected, context=True)
            original_state(state, renamed=False, created=created, scene=scene, undo=undo, redo=0,
                selected=selected, context=True, baseline=empty)
        rejected = []
        for name, state in (('missing-post-authoring-history', synthetic_state(renamed=True, undo=1)),
                            ('missing-post-operation-zero-history', synthetic_state(created=True, scene=1, selected=INVALID, context=True)),
                            ('partial-baseline-history', synthetic_state())):
            state['gltfMaterialVariants'].pop('undoCount')
            if name != 'partial-baseline-history': state['gltfMaterialVariants'].pop('redoCount')
            try:
                original_state(state, renamed=name == 'missing-post-authoring-history', created=name == 'missing-post-operation-zero-history',
                    scene=1 if name == 'missing-post-operation-zero-history' else 0, undo=1 if name == 'missing-post-authoring-history' else 0,
                    redo=0, selected=INVALID if name == 'missing-post-operation-zero-history' else 2,
                    context=name == 'missing-post-operation-zero-history', before=name == 'partial-baseline-history')
            except RuntimeError: rejected.append(name)
            else: raise RuntimeError('Authoring verifier accepted invalid missing history: '+name)
        for name, mutate in (
            ('wrong-node-renamed', lambda s: s['gltfMerge']['retainedNodes'][1].__setitem__('name', NEW_NAME)),
            ('changed-retained-world', lambda s: s['gltfMerge']['retainedNodes'][0]['worldColumns'][3].__setitem__(0, 3.)),
            ('empty-acquired-geometry', lambda s: s['gltfMerge']['retainedNodes'][4].__setitem__('renderable', True)),
            ('wrong-source-identity', lambda s: s['gltfMerge']['retainedNodes'][4].__setitem__('sourceNode', 1)),
            ('lost-selection', lambda s: s['gui'].__setitem__('selectedMeshNodeId', 2)),
            ('wrong-active-view', lambda s: s['gltfMerge'].__setitem__('activeRoots', [3])),
            ('wrong-history', lambda s: s['gltfMaterialVariants'].__setitem__('undoCount', 1))):
            state = synthetic_state(renamed=True, created=True, undo=2, selected=4); mutate(state)
            try: original_state(state, renamed=True, created=True, scene=0, undo=2, redo=0, selected=4, baseline=baseline)
            except RuntimeError: rejected.append(name)
            else: raise RuntimeError('Authoring verifier accepted changed synthetic state: '+name)
        inspections = reload_checks = 0
        for context in (False, True):
            for suffix in ('glb', 'gltf'):
                exported = output/'synthetic'/str(context)/suffix/('scene.'+suffix)
                model = synthetic_export(Path(plan['source']), exported, context=context)
                inspected = export_inspection(exported, context=context); inspections += 1
                initial = synthetic_reload(model, inspected, scene=int(context), selected=INVALID if context else 3)
                reload_state(initial, inspected, scene=int(context)); reload_checks += 1
                lazy_reload = copy.deepcopy(initial); lazy_reload['gltfMaterialVariants'].clear()
                reload_state(lazy_reload, inspected, scene=int(context))
                if context and suffix == 'gltf':
                    switched = synthetic_reload(model, inspected, scene=0, selected=INVALID)
                    try: reload_state(switched, inspected, scene=0, baseline=lazy_reload)
                    except RuntimeError: rejected.append('reload-history-shape-changed')
                    else: raise RuntimeError('Reload verifier accepted changed lazy history shape')
                    bad_depth = copy.deepcopy(lazy_reload); bad_depth['gltfScenes']['editHistoryDepth'] = 1
                    try: reload_state(bad_depth, inspected, scene=1)
                    except RuntimeError: rejected.append('reload-missing-history-nonzero-depth')
                    else: raise RuntimeError('Reload verifier accepted missing nonempty history')
                if context:
                    for scene in (0, 1):
                        switched = synthetic_reload(model, inspected, scene=scene, selected=INVALID)
                        reload_state(switched, inspected, scene=scene, baseline=initial); reload_checks += 1
                copied, files = portable_copy(exported, output/'copy'/str(context)/suffix)
                export_inspection(copied, context=context)
                require(len(files) == 2, 'Portable external buffer closure not copied')
                if suffix == 'gltf':
                    model = json.loads(exported.read_text()); model['nodes'][4]['name'] = 'Lost empty'
                    exported.write_text(json.dumps(model), encoding='utf-8')
                    try: export_inspection(exported, context=context)
                    except RuntimeError: rejected.append('lost-empty-export-'+str(context))
                    else: raise RuntimeError('Export verifier accepted a lost empty node')
        first, second = output/'pixels-a.png', output/'pixels-b.png'
        pixels = np.zeros((HEIGHT, WIDTH, 4), dtype=np.uint8); pixels[:, :, 3] = 255
        pixels[150:210, 270:365, :3] = [180, 70, 40]
        Image.fromarray(pixels).save(first); Image.fromarray(pixels).save(second)
        a, b = dict(image=str(first)), dict(image=str(second))
        image_check(a, b); nonvacuous(a)
        pixels[160, 280, 3] = 254; Image.fromarray(pixels).save(second)
        try: image_check(a, b)
        except RuntimeError: rejected.append('one-byte-surface-change')
        else: raise RuntimeError('Exact authoring image predicate accepted changed alpha')
        require(validation_log_error('WRITE_AFTER_WRITE hazard detected') and not validation_log_error('No hazards detected'),
                'Strict scanner changed')
        return dict(pureSelfCheck=True, nativeValidationPerformed=False, generatedInputCount=7,
            historyStatesChecked=7, membershipStatesChecked=4, syntheticExportsInspected=inspections,
            portableClosureChecks=4, syntheticReloadStatesChecked=reload_checks, rejectedMutations=rejected, roiProof=roi,
            expectedCapturesPerTechnique=21, expectedImageChecksPerTechnique=20,
            expectedStateChecksPerTechnique=21, expectedExportInspectionsPerTechnique=4, expectedProcessesPerTechnique=8)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path); parser.add_argument('--manifest', type=Path)
    parser.add_argument('--output', type=Path); parser.add_argument('--technique', action='append', choices=TECHNIQUES)
    parser.add_argument('--timeout', type=float, default=300.)
    parser.add_argument('--generate-only', action='store_true'); parser.add_argument('--self-check', action='store_true')
    args = parser.parse_args()
    if args.self_check: print(json.dumps(self_check(), indent=2)); return
    if args.output is None: parser.error('--output is required')
    if not args.generate_only and (args.renderer is None or args.manifest is None): parser.error('Native execution requires explicit --renderer and --manifest')
    if not np.isfinite(args.timeout) or args.timeout <= 0: parser.error('--timeout must be finite and positive')
    args.technique = tuple(dict.fromkeys(args.technique or TECHNIQUES))
    output = args.output.resolve(); plan = generate(output)
    report = dict(schemaVersion=1, passed=None if args.generate_only else False, generatedOnly=args.generate_only,
        rendererExecuted=False, nativeValidationPerformed=False, sourceDependencies=dependencies(),
        renderer=str(args.renderer.resolve()) if args.renderer else None, manifest=str(args.manifest.resolve()) if args.manifest else None,
        launchPlanSha256=sha(output/'launch-plan.json'), immutableInputs=plan['immutableInputs'], roiProof=roi_proof(),
        expectedCaptureCount=21*len(args.technique), expectedStateCheckCount=21*len(args.technique),
        expectedImageCheckCount=20*len(args.technique), expectedExportInspectionCount=4*len(args.technique), expectedProcessCount=8*len(args.technique),
        captures=[], stateChecks=[], imageChecks=[], exports=[], portableCopies=[], processes=[], unverified=UNVERIFIED)
    if args.generate_only:
        document(output/'results.json', report)
        print('GENERATED', len(plan['immutableInputs']), 'immutable inputs;', report['expectedCaptureCount'], 'planned captures; renderer not executed')
    else:
        run_native(args, output, plan, report)
        print('PASS', report['captureCount'], 'captures;', report['stateCheckCount'], 'state checks;',
            report['imageCheckCount'], 'image checks;', report['exportInspectionCount'], 'export inspections')


if __name__ == '__main__': main()
