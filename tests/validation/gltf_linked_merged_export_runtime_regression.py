"""Merged linked manifests: independent raster controls and portable offline reload.

Two input roots deliberately reuse URI basenames and alias keys with different
buffer/image contents. Both raster paths exercise merge/undo/redo, glTF/GLB save,
and reload after the original roots are moved away. Native work requires a
frozen renderer/helper/DLL/SPIR-V manifest. --self-check/--generate-only use no GPU.
Expected native inventory: 24 successful captures in 12 processes, plus two
expected-failure original-input protection processes whose images are excluded.
"""
from __future__ import annotations

import argparse
import base64
import copy
import json
from pathlib import Path
import struct
import sys
import tempfile
import traceback

import numpy as np
from PIL import Image

from gltf_accessor_runtime_regression import Asset, decode_accessor, load_asset, png_bytes, require
from gltf_extended_materials_runtime_regression import sha, validation_log_error
from gltf_material_variants_runtime_regression import run
from gltf_merge_runtime_regression import document, forest_worlds, image_pixels, immutable, placement, portable_copy
from gltf_texture_formats_runtime_regression import verify_manifest

TECHNIQUES = ('deferred-raster', 'forward-raster')
UNLIT = 'KHR_materials_unlit'
INDICES = [0, 1, 2, 0, 2, 3]
UV = [[0., 0.], [1., 0.], [1., 1.], [0., 1.]]
LOCAL = dict(A=[[-.375, -.25, 0.], [.375, -.25, 0.], [.375, .25, 0.], [-.375, .25, 0.]],
             B=[[-.25, -.375, 0.], [.25, -.375, 0.], [.25, .375, 0.], [-.25, .375, 0.]])
COLORS = dict(A=[[[240, 30, 20, 255], [220, 150, 30, 255]], [[170, 20, 40, 255], [245, 90, 25, 255]]],
              B=[[[20, 60, 240, 255], [30, 200, 210, 255]], [[45, 25, 170, 255], [20, 140, 235, 255]]])
CENTERS = dict(A=[[-1., .5, 0.], [-1., -.5, 0.]], B=[[1., .25, 0.], [1., -.75, 0.]])
# Literal independent world vertices; no production merge or export builds an oracle.
BAKED = dict(A=[
    [[-1.375, .25, 0.], [-.625, .25, 0.], [-.625, .75, 0.], [-1.375, .75, 0.]],
    [[-1.375, -.75, 0.], [-.625, -.75, 0.], [-.625, -.25, 0.], [-1.375, -.25, 0.]]], B=[
    [[.75, -.125, 0.], [1.25, -.125, 0.], [1.25, .625, 0.], [.75, .625, 0.]],
    [[.75, -1.125, 0.], [1.25, -1.125, 0.], [1.25, -.375, 0.], [.75, -.375, 0.]]])
COMMON = ('--import-scale', '1', '--no-ray-query', '--ray-shadows', 'raster', '--no-ddgi',
          '--sky', 'plain', '--sky-horizon', '0', '0', '0', '--directional-intensity', '0',
          '--fsr', 'native', '--dlss', 'native')


def add_material(asset, owner):
    image = asset.image(np.asarray(COLORS[owner], dtype=np.uint8))
    asset.model['samplers'][image].update(minFilter=9728, magFilter=9728)
    index = len(asset.model['materials'])
    asset.model['materials'].append(dict(name=owner+' panel material', extras=dict(fixtureOrigin=owner),
        pbrMetallicRoughness=dict(baseColorFactor=[1., 1., 1., 1.],
            baseColorTexture=dict(index=image), metallicFactor=0., roughnessFactor=1.), extensions={UNLIT: {}}))
    asset.model.setdefault('extensionsUsed', [UNLIT])
    return index


def add_quad(asset, positions, material):
    primitive = dict(attributes=dict(POSITION=asset.accessor('VEC3', positions),
        NORMAL=asset.accessor('VEC3', [[0., 0., 1.]]*4), TEXCOORD_0=asset.accessor('VEC2', UV)),
        indices=asset.accessor('SCALAR', INDICES, 5123, indices=True), material=material)
    index = len(asset.model['meshes'])
    asset.model['meshes'].append(dict(primitives=[primitive]))
    return index


def write_static(path, owners=('A', 'B'), *, baked=True):
    asset = Asset(); asset.model['nodes'] = []
    for owner in owners:
        material = add_material(asset, owner)
        shared = None if baked else add_quad(asset, LOCAL[owner], material)
        for index, positions in enumerate(BAKED[owner]):
            node = dict(name=owner+' static '+str(index), mesh=add_quad(asset, positions, material) if baked else shared)
            if not baked: node['translation'] = CENTERS[owner][index]
            asset.model['nodes'].append(node)
    asset.model['scenes'] = [dict(nodes=list(range(len(asset.model['nodes']))))]
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='linked-control-', dir=path.parent) as temporary:
        candidate = Path(temporary)/'control.gltf'; asset.write(candidate); immutable(path, candidate.read_bytes())


def write_linked(root, owner):
    asset = Asset(); material = add_material(asset, owner)
    add_quad(asset, LOCAL[owner], material)
    asset.model['nodes'] = [dict(name=owner+' linked panel', mesh=0, extras=dict(fixtureOrigin=owner))]
    asset.model['buffers'] = [dict(byteLength=len(asset.data), uri='virtual%20mesh.bin')]
    asset.model['images'][0] = dict(uri='virtual%20albedo.png', extras=dict(fixtureOrigin=owner))
    document(root/'parts/Shared panel.gltf', asset.model)
    immutable(root/'Shared mesh.bin', bytes(asset.data))
    immutable(root/'Shared image.png', png_bytes(COLORS[owner]))
    translations = CENTERS['A'] if owner == 'A' else [[.5, .5, 0.], [.5, -.5, 0.]]
    source = root/'linked.gltf'
    document(source, dict(asset=dict(version='2.1', minVersion='2.1'),
        files=[dict(uri='parts/Shared%20panel.gltf', mimeType='model/gltf+json', aliases=[
                    dict(alias='virtual%20mesh.bin', file=1), dict(alias='virtual%20albedo.png', file=2)]),
               dict(uri='Shared%20mesh.bin', mimeType='application/octet-stream'),
               dict(uri='Shared%20image.png', mimeType='image/png')],
        externalAssets=[dict(file=0)], nodes=[dict(name=f'{owner} carrier {i}', externalAsset=0, translation=t)
            for i, t in enumerate(translations)], scenes=[dict(nodes=[0, 1])], scene=0))
    return source


def generate(output):
    sources = {owner: write_linked(output/'sources'/owner, owner) for owner in ('A', 'B')}
    controls = {name: output/'controls'/(name+'.gltf') for name in ('original', 'merged')}
    write_static(controls['original'], ('A',)); write_static(controls['merged'])
    request = dict(path=str(sources['B']), parentName='Merged linked B', placementMatrix=placement([.5, -.25, 0.]))
    scripts = dict(control=dict(schemaVersion=1, frames=2, sampleFrames=[2]),
        merge=dict(schemaVersion=1, frames=8, sampleFrames=[2, 4, 6, 8], events=[
            dict(frame=3, merge=request, guiWaitForLoad=True), dict(frame=5, sceneHistory='undo'), dict(frame=7, sceneHistory='redo')]),
        protect=dict(schemaVersion=1, frames=2, sampleFrames=[2], events=[dict(frame=1, merge=request, guiWaitForLoad=True)]))
    paths = {}
    for name, script in scripts.items():
        paths[name] = output/'scripts'/(name+'.json'); document(paths[name], script)
    inputs = sorted([*(output/'sources').rglob('*'), *controls.values(), *paths.values()])
    plan = dict(schemaVersion=1, sources={k: str(v) for k, v in sources.items()}, controls={k: str(v) for k, v in controls.items()},
        scripts={k: str(v) for k, v in paths.items()}, expectedCaptureCount=24, expectedSuccessfulProcesses=12,
        expectedProtectionProcesses=2, immutableInputs=[dict(path=str(p), byteLength=p.stat().st_size, sha256=sha(p)) for p in inputs if p.is_file()])
    document(output/'launch-plan.json', plan)
    return plan


def signature(vertices):
    return tuple(sorted(map(tuple, np.asarray(vertices).reshape((-1, 3)).tolist())))


def inspect_export(path, *, shared=True):
    model, buffers = load_asset(path)
    require(model['asset']['version'] == '2.0' and 'files' not in model and 'externalAssets' not in model and
        all('externalAsset' not in n for n in model['nodes']), 'Portable export retains linked references')
    worlds, _ = forest_worlds(model)
    pending = list(model['scenes'][model['scene']]['nodes']); active = []
    for index in pending:
        require(index not in active, 'Duplicate selected-scene node'); active.append(index)
        pending.extend(model['nodes'][index].get('children', []))
    wanted = {owner: [signature(np.asarray(p)[INDICES]) for p in BAKED[owner]] for owner in ('A', 'B')}
    seen = dict(A=[], B=[]); resources = dict(A=set(), B=set())
    for index in active:
        node = model['nodes'][index]
        if 'mesh' not in node: continue
        for primitive in model['meshes'][node['mesh']]['primitives']:
            require(primitive.get('mode', 4) == 4, 'Changed linked primitive mode')
            material = model['materials'][primitive['material']]
            owner = material.get('extras', {}).get('fixtureOrigin')
            require(owner in wanted and UNLIT in material.get('extensions', {}), 'Lost linked material identity/model')
            info = material['pbrMetallicRoughness']
            require(info.get('baseColorFactor', [1.]*4) == [1.]*4, 'Changed literal material color')
            texture = model['textures'][info['baseColorTexture']['index']]
            require(np.array_equal(image_pixels(path, model, buffers, texture['source']), np.asarray(COLORS[owner], dtype=np.uint8)),
                'Overlapping alias resolved to the wrong source image')
            positions = decode_accessor(path, primitive['attributes']['POSITION'])
            indices = decode_accessor(path, primitive['indices']).reshape(-1).astype(int)
            require(len(indices) == 6 and np.all(indices >= 0) and np.all(indices < len(positions)), 'Changed linked triangle indices')
            world = (worlds[index]@np.column_stack((positions[indices], np.ones(len(indices)))).T).T[:, :3]
            record = signature(world); require(record in wanted[owner], 'Linked world geometry differs from literal control')
            wanted[owner].remove(record); seen[owner].append(node['mesh']); resources[owner].add(texture['source'])
    require(all(not rows for rows in wanted.values()) and all(len(rows) == 2 for rows in seen.values()), 'Lost/duplicated merged linked placements')
    if shared: require(all(len(set(rows)) == 1 for rows in seen.values()) and seen['A'][0] != seen['B'][0], 'Lost within-source mesh sharing or merged distinct sources')
    require(resources['A'].isdisjoint(resources['B']), 'Distinct aliased images lost source identity')
    return dict(passed=True, activePlacements=4, distinctMeshResources=len(set(seen['A']+seen['B'])),
        decodedGeometryExact=True, decodedImagesExact=True, sourceFree=True)


def same_image(actual, expected):
    with Image.open(actual['image']) as a, Image.open(expected['image']) as b:
        left, right = np.asarray(a.convert('RGB')), np.asarray(b.convert('RGB'))
    require(left.shape == right.shape == (360, 640, 3) and np.array_equal(left, right), 'Independent static image control differs')
    return dict(passed=True, pixelExact=True)


def check_state(state, count, *, linked, history=None):
    require(state['resolution'] == [640, 360] and state['msaaSamples'] == 1 and not state['taa']['enabled'], 'Wrong raster capture settings')
    require(state['gltfScenes']['activeRenderableCount'] == count, 'Wrong active placement membership')
    flags = state['gltfNodeFlags']; require(not flags['truncated'], 'Truncated linked node evidence')
    payloads = [row for row in flags['nodes'] if row['active'] and row.get('name') in ('A linked panel', 'B linked panel') and row['sourceIndex'] >= 0]
    if linked:
        require(len(payloads) == count and all(row['linkedReadOnly'] for row in payloads), 'Import/merge lost linked source locks')
    else: require(not any(row['linkedReadOnly'] for row in flags['nodes']), 'Portable reload recreated authoritative linked locks')
    if history is not None:
        cursor = state['gltfMaterialVariants']
        require((cursor.get('undoCount', 0), cursor.get('redoCount', 0)) == history, 'Merge history cursor differs')
    return dict(passed=True, activePlacements=count, linkedReadOnly=linked, history=history)


def dependencies():
    directory = Path(__file__).resolve().parent
    return {str(Path(module.__file__).resolve()): sha(Path(module.__file__).resolve()) for module in tuple(sys.modules.values())
        if getattr(module, '__file__', None) and Path(module.__file__).resolve().parent == directory}


def run_native(args, output, plan, report):
    require(args.renderer and args.manifest and args.renderer.is_file() and args.manifest.is_file(), 'Native work requires renderer and frozen manifest')
    require(not (output/'captures').exists() and not (output/'exports').exists() and not (output/'offline-sources').exists(), 'Native evidence directory must be fresh')
    runtime = sorted([*args.renderer.parent.glob('*.dll'), *(args.renderer.parent/'spv_shaders').glob('*.spv')])
    frozen = {Path(name).resolve() for name in json.loads(args.manifest.read_text(encoding='utf-8-sig'))['files']}
    require(runtime and any(p.suffix == '.spv' for p in runtime), 'Missing native shader inventory')
    require({args.renderer, *map(Path, report['sourceDependencies']), *map(Path.resolve, runtime)} <= frozen, 'Manifest omits renderer/helper/DLL/SPIR-V inputs')
    manifest_hash, plan_hash = sha(args.manifest), sha(output/'launch-plan.json'); moved = False
    settings = b'khronos_validation.validate_sync = true\nkhronos_validation.duplicate_message_limit = 1000\n'
    target = output/'captures'; target.mkdir(); immutable(target/'vk_layer_settings.txt', settings)
    def persist():
        report.update(captureCount=len(report['captures']), checkCount=len(report['checks']))
        (output/'results.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    def verify():
        require(sha(args.manifest) == manifest_hash and sha(output/'launch-plan.json') == plan_hash, 'Frozen manifest/plan changed')
        verify_manifest(args.manifest); require(dependencies() == report['sourceDependencies'], 'Frozen helper source changed')
        require(runtime == sorted([*args.renderer.parent.glob('*.dll'), *(args.renderer.parent/'spv_shaders').glob('*.spv')]), 'Native DLL/shader inventory changed')
        require((target/'vk_layer_settings.txt').read_bytes() == settings, 'Strict validation settings changed')
        for item in plan['immutableInputs']:
            path = Path(item['path'])
            if moved and path.is_relative_to(output/'sources'): path = output/'offline-sources'/path.relative_to(output/'sources')
            require(path.is_file() and path.stat().st_size == item['byteLength'] and sha(path) == item['sha256'], 'An original/generated input changed')
        for portable in report['portableCopies']:
            for item in portable['files']: require(sha(Path(item['path'])) == item['sha256'], 'Portable evidence changed')
    def capture(source, name, technique, script, *, save=None, linked=False):
        verify()
        try:
            records = run(args.renderer, target, source, name, technique, 'lit', report['captures'], sequence=script, save=save,
                timeout=args.timeout, extra=(*COMMON, *(('--experimental-gltf-linked-assets',) if linked else ())))
            require(not validation_log_error(Path(records[0]['log']).read_text(encoding='utf-8', errors='replace')), 'Strict Vulkan diagnostic')
            return records
        finally: verify(); persist()
    def check(kind, name, result): report['checks'].append(dict(kind=kind, name=name, **result)); persist()
    controls, exported = {}, []
    try:
        report['nativeManifestBefore'] = verify_manifest(args.manifest); report['rendererExecuted'] = True; persist()
        for technique in args.technique:
            controls[technique] = {}
            for name, source in plan['controls'].items():
                row = capture(Path(source), technique+'-control-'+name, technique, Path(plan['scripts']['control']))[0]
                check('state', technique+'-control-'+name, check_state(row['state'], 2 if name == 'original' else 4, linked=False))
                controls[technique][name] = row
            a, b = [np.asarray(Image.open(controls[technique][name]['image']).convert('RGB'), dtype=np.int16) for name in ('original', 'merged')]
            require(float(np.any(a != b, axis=2).mean()) > .01 and float(np.abs(a-b).mean()) > 1., 'Vacuous incoming geometry/image control')
            for suffix in ('gltf', 'glb'):
                name = technique+'-'+suffix; destination = output/'exports'/name/('scene.'+suffix); destination.parent.mkdir(parents=True)
                rows = capture(Path(plan['sources']['A']), name+'-source', technique, Path(plan['scripts']['merge']), save=destination, linked=True)
                require(len(rows) == 4, 'Wrong merge/history capture inventory')
                for row, count, history, control in zip(rows, (2, 4, 2, 4), ((0, 0), (1, 0), (0, 1), (1, 0)), ('original', 'merged', 'original', 'merged'), strict=True):
                    check('state', row['image'], check_state(row['state'], count, linked=True, history=history))
                    check('image', row['image'], same_image(row, controls[technique][control]))
                require(all(row['state']['gltfScenes']['graphIdentity'] == rows[0]['state']['gltfScenes']['graphIdentity'] for row in rows), 'Merge replaced graph identity')
                check('serialized', name, inspect_export(destination))
                copied, files = portable_copy(destination, output/'portable'/name)
                report['portableCopies'].append(dict(files=files)); exported.append((technique, name, copied)); persist()
        # Explicitly protect both the primary manifest and the incoming origin.
        for owner in ('A', 'B'):
            verify(); prefix = 'protect-original-'+owner
            try:
                run(args.renderer, target, Path(plan['sources']['A']), prefix, 'deferred-raster', 'lit', [],
                    sequence=Path(plan['scripts']['protect']), save=Path(plan['sources'][owner]), timeout=args.timeout,
                    extra=(*COMMON, '--experimental-gltf-linked-assets'))
            except RuntimeError:
                log = (target/(prefix+'.log')).read_text(encoding='utf-8', errors='replace')
                require('cannot replace a linked input file' in log and not validation_log_error(log), 'Protection failed for an unrelated reason')
            else: raise RuntimeError('Save replaced an original linked input')
            report['protectedInputs'].append(dict(owner=owner, passed=True, log=str(target/(prefix+'.log')))); verify(); persist()
        require((output/'sources').resolve().is_relative_to(output.resolve()) and not (output/'offline-sources').exists(), 'Unsafe source-free staging path')
        (output/'sources').rename(output/'offline-sources'); moved = True; verify()
        require(all(not Path(p).exists() for p in plan['sources'].values()), 'Original linked manifests remain reachable')
        for technique, name, source in exported:
            row = capture(source, name+'-offline-reload', technique, Path(plan['scripts']['control']))[0]
            check('state', name+'-offline', check_state(row['state'], 4, linked=False))
            check('image', name+'-offline', same_image(row, controls[technique]['merged']))
            check('serialized-offline', name, inspect_export(source))
        require(len(report['captures']) == 12*len(args.technique), 'Wrong successful native capture inventory')
        report['passed'] = report['nativeValidationPerformed'] = True
    finally:
        verify(); report['nativeManifestAfter'] = verify_manifest(args.manifest); persist()


def simulated_glb(source, destination):
    model, buffers = load_asset(source); data = bytearray(buffers[0])
    for image in model.get('images', []):
        payload = base64.b64decode(image.pop('uri').split(',', 1)[1]); data.extend(bytes((-len(data)) % 4))
        image['bufferView'] = len(model['bufferViews']); image['mimeType'] = 'image/png'
        model['bufferViews'].append(dict(buffer=0, byteOffset=len(data), byteLength=len(payload))); data.extend(payload)
    model['buffers'] = [dict(byteLength=len(data))]
    text = json.dumps(model).encode(); text += b' '*((-len(text)) % 4); data.extend(bytes((-len(data)) % 4))
    destination.write_bytes(struct.pack('<5I', 0x46546c67, 2, 28+len(text)+len(data), len(text), 0x4e4f534a)+text+
        struct.pack('<2I', len(data), 0x004e4942)+data)


def self_check():
    build = Path(__file__).resolve().parents[2]/'build'; build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='linked-merged-export-selfcheck-', dir=build) as temporary:
        root = Path(temporary); plan = generate(root)
        require(plan['expectedCaptureCount'] == 24, 'Unexpected capture matrix')
        a, b = [json.loads(Path(plan['sources'][owner]).read_text()) for owner in ('A', 'B')]
        require(a['files'] == b['files'] and a['externalAssets'] == b['externalAssets'], 'Aliased URI tables no longer collide')
        for filename in ('Shared mesh.bin', 'Shared image.png'):
            require((root/'sources/A'/filename).read_bytes() != (root/'sources/B'/filename).read_bytes(), 'Source resource witness became vacuous')
        for owner in ('A', 'B'):
            for local, center, baked in zip([LOCAL[owner]]*2, CENTERS[owner], BAKED[owner], strict=True):
                require(np.array_equal(np.asarray(local)+center, baked), 'Authored transform and independent literal control disagree')
        candidate = root/'simulated.gltf'; write_static(candidate, baked=False); inspect_export(candidate)
        binary = root/'simulated.glb'; simulated_glb(candidate, binary); inspect_export(binary)
        pristine = json.loads(candidate.read_text()); rejected = []
        mutations = dict(
            linked_file_table=lambda m: m.update(files=[]),
            linked_node_reference=lambda m: m['nodes'][0].update(externalAsset=0),
            missing_placement=lambda m: m['scenes'][0]['nodes'].pop(),
            duplicate_root=lambda m: m['scenes'][0]['nodes'].append(0),
            misplaced_incoming=lambda m: m['nodes'][2]['translation'].__setitem__(0, 1.25),
            wrong_aliased_image=lambda m: m['textures'][1].update(source=0),
            lost_owner=lambda m: m['materials'][1]['extras'].update(fixtureOrigin='A'),
            changed_material=lambda m: m['materials'][1]['pbrMetallicRoughness'].update(baseColorFactor=[0., 0., 0., 1.]))
        for name, mutation in mutations.items():
            altered = copy.deepcopy(pristine); mutation(altered); candidate.write_text(json.dumps(altered))
            try: inspect_export(candidate)
            except (RuntimeError, AssertionError): rejected.append(name)
            else: raise RuntimeError('Serialized oracle accepted '+name)
        candidate.write_text(json.dumps(pristine))
        flags = [dict(name='A linked panel', active=True, sourceIndex=i, linkedReadOnly=True) for i in (2, 3)]
        state = dict(resolution=[640, 360], msaaSamples=1, taa=dict(enabled=False), gltfScenes=dict(activeRenderableCount=2),
            gltfNodeFlags=dict(truncated=False, nodes=flags), gltfMaterialVariants=dict(undoCount=0, redoCount=0))
        check_state(state, 2, linked=True, history=(0, 0)); bad_state = copy.deepcopy(state); bad_state['gltfNodeFlags']['nodes'][0]['linkedReadOnly'] = False
        try: check_state(bad_state, 2, linked=True)
        except RuntimeError: rejected.append('lost_linked_lock')
        else: raise RuntimeError('State oracle accepted lost linked lock')
        for message in ('VUID-test', 'SYNC-HAZARD-WRITE-AFTER-WRITE', 'hazard detected', 'Undefined-Value-StorageImage-FormatMismatch-ImageView'):
            require(validation_log_error(message), 'Strict diagnostic gate accepted '+message)
        return dict(passed=True, expectedCaptureCount=24, serializedFormats=['gltf', 'glb'],
            rejectedCorruptions=rejected, overlappingAliasTables=True, distinctGeometryAndImages=True,
            nativeValidationPerformed=False, rendererExecuted=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path); parser.add_argument('--manifest', type=Path); parser.add_argument('--output', type=Path)
    parser.add_argument('--self-check', action='store_true'); parser.add_argument('--generate-only', action='store_true')
    parser.add_argument('--timeout', type=int, default=240); parser.add_argument('--technique', action='append', choices=TECHNIQUES)
    args = parser.parse_args()
    if args.self_check: print(json.dumps(self_check(), indent=2)); return
    require(args.output is not None, '--output is required')
    output = args.output.resolve(); build = Path(__file__).resolve().parents[2]/'build'
    require(output.is_relative_to(build), 'Generated artifacts must stay under workspace build/')
    output.mkdir(parents=True, exist_ok=True); args.technique = args.technique or list(TECHNIQUES)
    if args.renderer: args.renderer = args.renderer.resolve()
    if args.manifest: args.manifest = args.manifest.resolve()
    report = dict(schemaVersion=1, passed=False, generatedOnly=args.generate_only, rendererExecuted=False,
        nativeValidationPerformed=False, sourceDependencies=dependencies(), captures=[], checks=[], portableCopies=[], protectedInputs=[])
    try:
        plan = generate(output)
        if args.generate_only: report['passed'] = True
        else: run_native(args, output, plan, report)
    except BaseException:
        report['error'] = traceback.format_exc(); raise
    finally:
        report.update(captureCount=len(report['captures']), checkCount=len(report['checks']))
        (output/'results.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    print('PASS merged linked export '+('generation only' if args.generate_only else 'native portable offline reload'), flush=True)


if __name__ == '__main__': main()
