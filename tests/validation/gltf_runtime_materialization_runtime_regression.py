"""Native source-less material Make Unique, isolated editing and portable history.

Self-check/generate-only never execute the renderer. Each native technique first
captures and verifies the implicit-material identities in a separate process;
only then may a scripted process address those exact slots. CPU controller draw
membership and source records are reported separately from image evidence.
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

import gltf_material_binding_runtime_regression as binding
from gltf_accessor_runtime_regression import Asset, decode_accessor, load_asset, require
from gltf_extended_materials_runtime_regression import sha, validation_log_error
from gltf_material_authoring_runtime_regression import deep_contains, edit, serialized_material_record
from gltf_merge_runtime_regression import document, immutable, portable_copy
from gltf_tangent_uv_runtime_regression import finite_json
from gltf_texture_formats_runtime_regression import verify_manifest
from scene_node_authoring_runtime_regression import history_cursor

TECHNIQUES = binding.TECHNIQUES
COHORTS = ('core', 'resize', 'portable', 'invalid')
WIDTH, HEIGHT, UNLIT = binding.WIDTH, binding.HEIGHT, binding.UNLIT
BLUE = [.05, .2, .9, .45]
PHYSICAL = {
    'KHR_materials_ior': dict(ior=1.5),
    'KHR_materials_dispersion': dict(dispersion=0.),
    'KHR_materials_emissive_strength': dict(emissiveStrength=1.),
    'KHR_materials_specular': dict(specularFactor=1., specularColorFactor=[1., 1., 1.]),
    'KHR_materials_transmission': dict(transmissionFactor=0.),
    'KHR_materials_volume': dict(thicknessFactor=0., attenuationColor=[1., 1., 1.]),
    'KHR_materials_clearcoat': dict(clearcoatFactor=0., clearcoatRoughnessFactor=0.),
    'KHR_materials_sheen': dict(sheenColorFactor=[0., 0., 0.], sheenRoughnessFactor=0.),
    'KHR_materials_iridescence': dict(iridescenceFactor=0., iridescenceIor=1.3,
        iridescenceThicknessMinimum=100., iridescenceThicknessMaximum=400.),
    'KHR_materials_anisotropy': dict(anisotropyStrength=0., anisotropyRotation=0.),
    'KHR_materials_diffuse_transmission': dict(diffuseTransmissionFactor=0., diffuseTransmissionColorFactor=[1., 1., 1.])}
UNVERIFIED = [
    'Generation and self-checks are not native renderer acceptance.',
    'Graph bindings, material catalogs, packed-entry hashes and actual controller draw membership are CPU evidence, not GPU buffer readback.',
    'Interior pixels and independent literal controls prove rendered instance coverage, appearance preservation and edit isolation; they do not prove ray or DDGI behavior.',
    'This cohort covers the untextured implicit glTF default. Textured runtime-only imports, procedural/BIM producers, arbitrary external materials and native ImGui interaction need separate acceptance.',
    'Rejected requests terminate capture. Only preceding snapshots and explicit error text are checked; postfailure state/history preservation is not claimed.']


def material(edited=False):
    # This is an independent literal glTF definition, not a serializer result.
    # glTF's implicit metallic default is ONE, unlike Material{}'s zero.
    result = dict(pbrMetallicRoughness=dict(baseColorFactor=BLUE if edited else [1., 1., 1., 1.],
        metallicFactor=1., roughnessFactor=1.), emissiveFactor=[0., 0., 0.],
        alphaMode='BLEND' if edited else 'OPAQUE', alphaCutoff=.5, doubleSided=edited)
    if edited: result['extensions'] = {UNLIT: {}}
    return result


def request(frame, operation='makeUnique', *, graph=1, expected=2, source=-1, runtime=2):
    return binding.binding(frame, operation, graph=graph, expected=expected, source=source, runtime=runtime,
        name='Unique implicit left')


def edit_unique(frame):
    return edit(frame, {'pbrMetallicRoughness/baseColorFactor': BLUE}, source=0,
        extensions={**{name: False for name in PHYSICAL}, UNLIT: True}, alphaMode='BLEND', doubleSided=True)


def spec(label, frame, *, tail=False, edited=False, assigned=False, restored=False, history=(0, 0),
         width=WIDTH, height=HEIGHT, msaa=1):
    return dict(label=label, frame=frame, tail=tail, edited=edited, assigned=assigned, restored=restored,
        history=list(history), resolution=[width, height], msaa=msaa)


def pair_for(step):
    first = material(step['edited']) if step['tail'] and not step['restored'] else material()
    second = material(step['edited']) if step['assigned'] else material()
    return [first, second]


def profiles(cohort):
    if cohort == 'core':
        steps = [spec('before', 13), spec('unique', 26, tail=True, history=(1, 0)),
            spec('edit', 39, tail=True, edited=True, history=(2, 0)),
            spec('undo-edit', 52, tail=True, history=(1, 1)), spec('undo-unique', 65, history=(0, 2)),
            spec('redo-unique', 78, tail=True, history=(1, 1)), spec('redo-edit', 91, tail=True, edited=True, history=(2, 0)),
            spec('assign-existing', 104, tail=True, edited=True, assigned=True, history=(3, 0)),
            spec('undo-assign', 117, tail=True, edited=True, history=(2, 1)),
            spec('assign-runtime-only', 130, tail=True, edited=True, restored=True, history=(3, 0)),
            spec('undo-runtime-only', 143, tail=True, edited=True, history=(2, 1))]
        events = [request(14), edit_unique(27), *[dict(frame=f, sceneHistory=action) for f, action in
            [(40, 'undo'), (53, 'undo'), (66, 'redo'), (79, 'redo')]],
            request(92, 'assign', graph=3, source=0, runtime=3), dict(frame=105, sceneHistory='undo'),
            request(118, 'assign', expected=3), dict(frame=131, sceneHistory='undo')]
        return [dict(name='core', steps=steps, events=events, save='glb')]
    if cohort == 'resize':
        steps = [spec('before', 13), spec('unique', 26, tail=True, history=(1, 0)),
            spec('edit', 39, tail=True, edited=True, history=(2, 0))]
        for label, frame, tail, edited, history, width, height, samples in [
                ('resize', 52, True, True, (2, 0), 800, 450, 1), ('msaa', 65, True, True, (2, 0), 800, 450, 4),
                ('undo-edit', 78, True, False, (1, 1), 800, 450, 4), ('undo-unique', 91, False, False, (0, 2), 800, 450, 4),
                ('redo-unique', 104, True, False, (1, 1), 800, 450, 4), ('redo-edit', 117, True, True, (2, 0), 800, 450, 4),
                ('restore-size', 130, True, True, (2, 0), WIDTH, HEIGHT, 4), ('restore-msaa', 143, True, True, (2, 0), WIDTH, HEIGHT, 1)]:
            steps.append(spec(label, frame, tail=tail, edited=edited, history=history, width=width, height=height, msaa=samples))
        events = [request(14), edit_unique(27), dict(frame=40, resize=[800, 450]), dict(frame=53, samples=4),
            *[dict(frame=f, sceneHistory=action) for f, action in [(66, 'undo'), (79, 'undo'), (92, 'redo'), (105, 'redo')]],
            dict(frame=118, resize=[WIDTH, HEIGHT]), dict(frame=131, samples=1)]
        return [dict(name='resize', steps=steps, events=events)]
    if cohort == 'portable':
        steps = [spec('before', 13), spec('unique', 26, tail=True, history=(1, 0)),
            spec('edit', 39, tail=True, edited=True, history=(2, 0))]
        return [dict(name='portable-'+suffix, steps=copy.deepcopy(steps), events=[request(14), edit_unique(27)], save=suffix)
            for suffix in ('gltf', 'glb')]
    if cohort == 'invalid':
        return [dict(name='invalid-stale', steps=[spec('before', 13), spec('unique', 26, tail=True, history=(1, 0))],
            events=[request(14), request(27, 'assign', expected=2, source=0, runtime=3)], frames=39,
            rejection='The material surface binding changed before assignment'),
            dict(name='invalid-pair', steps=[spec('before', 13), spec('unique', 26, tail=True, history=(1, 0))],
                events=[request(14), request(27, 'assign', expected=3, source=0, runtime=2)], frames=39,
                rejection='The requested source/runtime material pair is stale'),
            dict(name='invalid-node', steps=[spec('before', 13)],
                events=[request(14, 'assign', graph=0)], frames=26,
                rejection='The material surface binding changed before assignment'),
            dict(name='invalid-unique-pair', steps=[spec('before', 13)],
                events=[request(14, runtime=0)], frames=26,
                rejection='Make Unique must use the surface.s current material pair')]
    raise ValueError('Unsupported cohort')


def visual_key(step):
    return json.dumps(dict(pair=pair_for(step), resolution=step['resolution'], msaa=step['msaa']), sort_keys=True)


def write_fixture(path, *, pair=None):
    asset = Asset(); asset.model.pop('materials')
    attrs = dict(POSITION=asset.accessor('VEC3', binding.POSITIONS), NORMAL=asset.accessor('VEC3', [[0., 0., 1.]]*4),
        TANGENT=asset.accessor('VEC4', [[1., 0., 0., 1.]]*4),
        TEXCOORD_0=asset.accessor('VEC2', [[0., 0.], [1., 0.], [1., 1.], [0., 1.]]))
    primitive = dict(attributes=attrs, indices=asset.accessor('SCALAR', binding.INDICES, 5123, indices=True), mode=4,
        extras=dict(runtimeMaterializationGeometry='unchanged topology'))
    asset.model['meshes'] = [dict(primitives=[primitive], extras=dict(sharedGeometry=True))]
    if pair is not None:
        asset.model['materials'] = copy.deepcopy(pair); primitive['material'] = 0
        other = copy.deepcopy(primitive); other['material'] = 1; asset.model['meshes'].append(dict(primitives=[other]))
        if any(UNLIT in m.get('extensions', {}) for m in pair): asset.model['extensionsUsed'] = [UNLIT]
    asset.model['nodes'] = [dict(name='Left implicit owner', mesh=0, translation=[-1.1, 0., 0.]),
        dict(name='Right implicit owner', mesh=1 if pair is not None else 0, translation=[1.1, 0., 0.])]
    asset.model['scenes'] = [dict(name='Runtime materialization acceptance', nodes=[0, 1])]
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='runtime-materialization-fixture-') as folder:
        generated = Path(folder)/'scene.gltf'; asset.write(generated); immutable(path, generated.read_bytes())


def generate(output, cohort):
    output.mkdir(parents=True, exist_ok=True); runs = profiles(cohort)
    source = output/'sources'/'implicit.gltf'; write_fixture(source)
    controls, by_visual, inputs = [], {}, [source]
    for run in runs:
        if cohort != 'invalid':
            for step in run['steps']:
                key = visual_key(step)
                if key not in by_visual:
                    name = 'literal-'+str(len(controls)); path = output/'sources'/(name+'.gltf')
                    pair = pair_for(step); write_fixture(path, pair=pair)
                    controls.append(dict(name=name, source=str(path), pair=pair, resolution=step['resolution'], msaa=step['msaa']))
                    by_visual[key] = name; inputs.append(path)
                step['control'] = by_visual[key]
        script = dict(schemaVersion=1, frames=run.get('frames', run['steps'][-1]['frame']),
            sampleFrames=[s['frame'] for s in run['steps']], camera=[dict(frame=1, position=[0., 0., 6.], target=[0., 0., 0.])],
            events=run['events'])
        path = output/'scripts'/(run['name']+'.json'); document(path, script); run['script'] = str(path); inputs.append(path)
    saves = sum('save' in r for r in runs)
    captures = 1+len(controls)+sum(len(r['steps']) for r in runs)+saves
    processes = 1+len(controls)+len(runs)+saves
    # One image check per control, sequence sample and reload; a preflight
    # literal/default preflight checks and two preservation/isolation checks follow.
    image_checks = 0 if cohort == 'invalid' else len(controls)+sum(len(r['steps']) for r in runs)+saves+4
    plan = dict(schemaVersion=1, cohort=cohort, source=str(source), controls=controls, runs=runs,
        expectedCaptureCount=captures, expectedProcessCount=processes, expectedImageCheckCount=image_checks,
        expectedExportCount=saves, implicitIdentityPreflight=dict(sourceMaterialCount=0, runtimeMaterialCount=3,
            graphNodes=[1, 3], runtimeBindings=[2, 2]),
        immutableInputs=[dict(path=str(p), byteLength=p.stat().st_size, sha256=sha(p)) for p in inputs])
    document(output/'launch-plan.json', plan); return plan


def inspect_record(actual, expected, *, serialized=False):
    value = serialized_material_record(actual) if serialized else actual
    deep_contains(value, expected, 'runtime materialization record')
    extensions = value.get('extensions', {})
    if UNLIT in expected.get('extensions', {}):
        require(set(extensions) == {UNLIT}, 'Unlit edit retained excluded physical extensions')
    else:
        require(set(extensions) <= set(PHYSICAL), 'Implicit material gained an unsupported workflow/extension')
        for name, extension in extensions.items():
            deep_contains(extension, PHYSICAL[name], 'implicit physical defaults/'+name)
            require(set(extension) == set(PHYSICAL[name]), 'Implicit default extension gained nondefault fields')
    def no_textures(owner):
        if isinstance(owner, dict):
            require(not any(k.endswith('Texture') for k in owner), 'Untextured implicit material gained a texture')
            for child in owner.values(): no_textures(child)
        elif isinstance(owner, list):
            for child in owner: no_textures(child)
    no_textures(value)


def properties(edited=False):
    return dict(transparent=edited, alphaMasked=False, doubleSided=edited, unlit=edited,
        specularGlossiness=False, heightScale=0.)


def state_check(state, expected, baseline=None):
    merge, rows, catalog = binding.bindings_and_catalog(state)
    require(state['resolution'] == expected['resolution'] and state['msaaSamples'] == expected['msaa'], 'Stale extent/MSAA configuration')
    require([r['graphNode'] for r in rows] == [1, 3] and {r['primitiveIndex'] for r in rows} == {0},
        'Implicit fixture primitive IDs changed; refusing to address another surface')
    count = int(expected['tail']); require(catalog['sourceMaterialCount'] == count and catalog['runtimeMaterialCount'] == count+3,
        'Source-less material tail was not appended/detached exactly once')
    require(catalog['sourceToRuntime'] == ([3] if count else []), 'New source record does not map to runtime3')
    require(merge['sourceTables']['materials'] == count, 'Evaluated model and material catalog disagree')
    runtime = [3 if expected['tail'] and not expected['restored'] else 2, 3 if expected['assigned'] else 2]
    require([r['materialIndex'] for r in rows] == runtime, 'Wrong implicit/unique surface binding')
    if count:
        entry = catalog['materials'][0]; wanted = material(expected['edited'])
        inspect_record(entry['authored'], wanted); inspect_record(entry['evaluated'], wanted)
        require(entry['materialProperties'] == properties(expected['edited']), 'Wrong unique material classification')
        if baseline is not None and baseline['gltfMaterialBindings']['sourceMaterialCount']:
            require(entry['authored'] == baseline['gltfMaterialBindings']['materials'][0]['authored'],
                'History replay did not restore the exact canonical source record')
    routes = {row['node']: row for row in state['gltfMaterialVariants']['assignments']}
    require(set(routes) == {1, 3}, 'Missing actual graph-binding routes')
    require(not catalog['drawsTruncated'], 'Controller draw inventory truncated')
    draws = {row['graphNode']: row for row in catalog['draws']}
    require(len(catalog['draws']) == len(draws) == 2 and set(draws) == {1, 3}, 'Controller draws do not cover both actual instances exactly once')
    for node, actual in zip((1, 3), runtime):
        flags = properties(expected['edited'] and actual == 3); route, draw = routes[node], draws[node]
        require(route['material'] == actual and all(route[k] == flags[k] for k in ('transparent', 'alphaMasked', 'doubleSided')),
            'Actual graph route does not follow runtime material')
        require(draw['runtimeMaterial'] == actual and draw['primitiveIndex'] == 0 and draw['indexCount'] == 6 and
            all(draw[k] == flags[k] for k in ('transparent', 'doubleSided')) and draw['objectDoubleSided'] == flags['doubleSided'],
            'Actual controller draw membership or object flags differ')
    history_cursor(state, *expected['history'], allow_uncreated=expected['history'] == [0, 0])
    require(not state['gltfMaterialAuthoring']['pendingGesture'], 'Material operation left a gesture pending')
    if baseline is not None:
        require(binding.retained_identity(state) == binding.retained_identity(baseline), 'Material authoring changed retained graph/geometry/resource tables')
    return dict(passed=True, bindings=runtime, sourceCount=count, runtimeCount=count+3, history=expected['history'],
        packedEntryHashes=[m['uploadedMaterialHash'] for m in catalog['materials']],
        evidence='CPU graph, source/runtime catalog and actual controller draw membership')


def reference_state(state, pair):
    _, rows, catalog = binding.bindings_and_catalog(state)
    require(not catalog['drawsTruncated'] and len(catalog['draws']) == 2, 'Incomplete reference controller draws')
    draws = {row['graphNode']: row for row in catalog['draws']}
    for row, expected in zip(rows, pair):
        entries = [m for m in catalog['materials'] if m['runtimeMaterial'] == row['materialIndex']]
        require(len(entries) == 1, 'Literal/reload surface has no explicit bound source record')
        inspect_record(entries[0]['authored'], expected); inspect_record(entries[0]['evaluated'], expected)
        flags = properties(UNLIT in expected.get('extensions', {}))
        require(entries[0]['materialProperties'] == flags, 'Reference/reload material classification differs')
        draw = draws.get(row['graphNode']); require(draw is not None and draw['runtimeMaterial'] == row['materialIndex'] and
            draw['indexCount'] == 6 and draw['transparent'] == flags['transparent'] and draw['doubleSided'] == flags['doubleSided'],
            'Reference/reload controller draw differs from bound source')
    history_cursor(state, 0, 0, allow_uncreated=True)
    return dict(passed=True, bindings=[r['materialIndex'] for r in rows], evidence='CPU explicit bound source records and controller draws')


def inspect_export(path, pair):
    model, _ = load_asset(path); nodes = model['nodes']; pending = [(i, 0.) for i in model['scenes'][model.get('scene', 0)]['nodes']]
    found, seen = [], set()
    while pending:
        index, parent_x = pending.pop(); require(index not in seen and 0 <= index < len(nodes), 'Invalid export node forest')
        seen.add(index); node = nodes[index]
        x = parent_x+(node['matrix'][12] if 'matrix' in node else node.get('translation', [0., 0., 0.])[0])
        pending.extend((child, x) for child in node.get('children', []))
        if 'mesh' in node:
            primitives = model['meshes'][node['mesh']]['primitives']; require(len(primitives) == 1, 'Export changed fixture primitive count')
            primitive = primitives[0]; require('material' in primitive, 'Export lost a materialized surface definition')
            found.append((x, primitive['material']))
            binding.close(decode_accessor(path, primitive['attributes']['POSITION']), np.asarray(binding.POSITIONS, dtype=np.float32).astype(float), 'export positions', 0.)
            binding.close(decode_accessor(path, primitive['attributes']['NORMAL']), [[0., 0., 1.]]*4, 'export normals', 0.)
            binding.close(np.asarray(decode_accessor(path, primitive['indices'])).reshape(-1), binding.INDICES, 'export indices', 0.)
    found.sort(); require(len(found) == 2 and found[0][1] != found[1][1], 'Export collapsed distinct implicit/unique bindings')
    binding.close([x for x, _ in found], [-1.1, 1.1], 'export placements')
    for (_, index), expected in zip(found, pair): inspect_record(model['materials'][index], expected, serialized=True)
    require(UNLIT in model.get('extensionsUsed', []) and not model.get('animations'), 'Export lost unlit declaration or invented animation')
    return dict(passed=True, path=str(path), sha256=sha(path), bindingMaterials=[i for _, i in found],
        evidence='Independent serialized material factors, scene bindings and decoded geometry')


def dependencies():
    directory = Path(__file__).resolve().parent; paths = {Path(__file__).resolve()}
    for module in tuple(sys.modules.values()):
        source = getattr(module, '__file__', None)
        if source and Path(source).resolve().parent == directory: paths.add(Path(source).resolve())
    return {str(path): sha(path) for path in sorted(paths)}


def launch(args, output, report, persist, verify, technique, name, source, steps, *, script=None, save=None, initial=None, rejection=None):
    verify(); target = output/'captures'/(technique+'-'+name); target.parent.mkdir(parents=True, exist_ok=True)
    require(not binding.artifacts(target), 'Refusing to overwrite runtime materialization evidence')
    initial = initial or dict(resolution=[WIDTH, HEIGHT], msaa=1); width, height = initial['resolution']
    image, log = target.with_suffix('.png'), target.with_suffix('.log')
    command = [str(args.renderer), '--hidden', '--no-ui', '--validation', '--no-bloom', '--no-taa',
        '--width', str(width), '--height', str(height), '--msaa', str(initial['msaa']), '--warmup-frames', '12', '--capture-frame', '13',
        '--fixed-dt', '.016666667', '--render-technique', technique, '--display-mode', 'lit', '--exposure', '.35',
        '--camera-position', '0', '0', '6', '--camera-target', '0', '0', '0', '--camera-fov', '45',
        '--environment-intensity', '0', '--directional-intensity', '3', '--directional-direction', '-.6', '.3', '-1',
        '--directional-color', '.2', '.7', '.4', '--import-scale', '1', '--model', str(source), '--screenshot', str(image),
        '--no-ray-query', '--ray-shadows', 'raster']
    if script: command += ['--capture-sequence', str(script)]
    if save:
        save.parent.mkdir(parents=True, exist_ok=True); require(not save.exists(), 'Refusing to overwrite materialization export')
        command += ['--save-scene', str(save)]
    row = dict(name=name, technique=technique, command=command, log=str(log), completed=False)
    report['processes'].append(row); report['rendererExecuted'] = True; persist()
    try:
        with log.open('x', encoding='utf-8') as stream:
            process = subprocess.run(command, cwd=args.renderer.parent, stdout=stream, stderr=subprocess.STDOUT, timeout=args.timeout,
                env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)), creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        text = log.read_text(encoding='utf-8', errors='replace'); row.update(returnCode=process.returncode, completed=True, logSha256=sha(log)); persist()
        require(not validation_log_error(text), 'Strict Vulkan validation error: '+str(log))
        if rejection:
            require(process.returncode != 0 and re.search(rejection, text, re.I), 'Request was not explicitly rejected: '+str(log)); row['expectedRejection'] = True
        else: require(process.returncode == 0, 'Native materialization process failed: '+str(log))
        final_frame = json.loads(script.read_text(encoding='utf-8'))['frames'] if script else 13
        records = {}
        for step in steps:
            frame = step['frame']; sampled = image if frame == final_frame and not rejection else image.with_name(image.stem+f'.frame-{frame:04d}.png')
            telemetry = sampled.with_suffix('.telemetry.json'); require(sampled.is_file() and telemetry.is_file(), 'Missing capture image/telemetry')
            state = json.loads(telemetry.read_text(encoding='utf-8-sig'))
            require(finite_json(state) and state['technique'] == technique and state['resolution'] == step['resolution'] and
                state['msaaSamples'] == step['msaa'] and not state['taa']['enabled'] and state['lighting']['environmentIntensity'] == 0 and
                any(p['active'] and p['gpuTimed'] for p in state['passes']), 'Wrong configuration or missing timed GPU frame')
            binding.close(state['lighting']['directionalIntensity'], 3., 'fixed directional intensity')
            binding.close(state['lighting']['directionalColor'], [.2, .7, .4], 'fixed directional color')
            item = dict(name=step['label'], process=name, technique=technique, frame=frame, resolution=step['resolution'], msaa=step['msaa'],
                image=str(sampled), telemetry=str(telemetry), log=str(log), imageSha256=sha(sampled), telemetrySha256=sha(telemetry), validationClean=True)
            report['captures'].append(item); records[step['label']] = {**item, 'state': state}
        require({p.resolve() for p in binding.artifacts(target) if p.suffix == '.png'} == {Path(r['image']).resolve() for r in records.values()},
            'Unexpected capture image inventory')
        persist(); return records
    finally: verify(); persist()


def run_native(args, output, plan, report):
    require(args.renderer.is_file() and args.manifest.is_file(), 'Missing renderer/frozen manifest')
    require(not any(output.glob('captures/*')) and not any(output.glob('exports/**/*')) and not any(output.glob('portable/**/*')), 'Native evidence directories must be fresh')
    if (output/'results.json').exists():
        old = json.loads((output/'results.json').read_text()); require(old.get('generatedOnly') and not old.get('rendererExecuted'), 'Refusing to overwrite native report')
    manifest_hash = sha(args.manifest)
    def persist():
        for key in ('captures', 'processes', 'stateChecks', 'imageChecks', 'exports'): report[key+'Count'] = len(report[key])
        (output/'results.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    def runtime_files():
        return sorted([*args.renderer.parent.glob('*.dll'), *(args.renderer.parent/'spv_shaders').glob('*.spv')])
    def verify():
        require(sha(args.manifest) == manifest_hash, 'Frozen manifest changed'); result = verify_manifest(args.manifest)
        require((output/'vk_layer_settings.txt').read_bytes() == b'khronos_validation.validate_sync = true\n', 'Validation settings changed')
        require(dependencies() == report['sourceDependencies'] and sha(output/'launch-plan.json') == report['launchPlanSha256'], 'Helper/dependency/launch plan changed')
        for entry in plan['immutableInputs']:
            path = Path(entry['path']); require(path.stat().st_size == entry['byteLength'] and sha(path) == entry['sha256'], 'Generated input changed')
        if 'runtimeInputs' in report: require([str(p.resolve()) for p in runtime_files()] == report['runtimeInputs'], 'Resident DLL/shader inventory changed')
        for copied in report['portableCopies']:
            require(all(sha(Path(f['path'])) == f['sha256'] for f in copied['files']), 'Portable copied bytes changed')
        return result
    def check(kind, technique, name, result): report[kind].append(dict(technique=technique, check=name, **result)); persist()
    try:
        report['nativeManifestBefore'] = verify_manifest(args.manifest)
        frozen = {Path(p).resolve() for p in json.loads(args.manifest.read_text(encoding='utf-8-sig'))['files']}
        require({args.renderer, *map(Path, report['sourceDependencies'])} <= frozen, 'Manifest omits renderer/transitive Python helpers')
        runtime = runtime_files(); require(any(p.suffix == '.spv' for p in runtime) and {p.resolve() for p in runtime} <= frozen, 'Manifest omits resident DLLs/SPIR-V')
        report.update(rendererSha256=sha(args.renderer), runtimeInputs=[str(p.resolve()) for p in runtime])
        settings = output/'vk_layer_settings.txt'; immutable(settings, b'khronos_validation.validate_sync = true\n')
        report['validationLayerSettings'] = dict(path=str(settings), sha256=sha(settings), verified=True); persist(); verify()
        for technique in args.technique:
            # This separate, read-only process completes BEFORE any scripted
            # material address is used. A changed runtime layout stops here.
            before = spec('preflight', 13)
            preflight = launch(args, output, report, persist, verify, technique, 'identity-preflight', Path(plan['source']), [before])['preflight']
            check('stateChecks', technique, 'identity-preflight', state_check(preflight['state'], before))
            controls = {}
            for control in plan['controls']:
                step = dict(label='control', frame=13, resolution=control['resolution'], msaa=control['msaa'])
                record = launch(args, output, report, persist, verify, technique, control['name'], Path(control['source']), [step], initial=control)['control']
                check('stateChecks', technique, control['name'], reference_state(record['state'], control['pair']))
                check('imageChecks', technique, control['name']+'-positive-visibility', binding.visible_reference(record)); controls[control['name']] = record
            if args.cohort != 'invalid':
                matches = [c for c in plan['controls'] if c['pair'] == [material(), material()] and
                    c['resolution'] == [WIDTH, HEIGHT] and c['msaa'] == 1]
                require(len(matches) == 1, 'Missing independent explicit default-MR preflight control')
                check('imageChecks', technique, 'implicit-default-literal-equivalence',
                    binding.compare_images(preflight, controls[matches[0]['name']]))
            first = None
            for run in plan['runs']:
                save = output/'exports'/technique/run['name']/('scene.'+run['save']) if 'save' in run else None
                records = launch(args, output, report, persist, verify, technique, run['name'], Path(plan['source']), run['steps'],
                    script=Path(run['script']), save=save, rejection=run.get('rejection'))
                if first is None: first = records
                baseline = records['before']['state']; rest = None
                for step in run['steps']:
                    record = records[step['label']]
                    check('stateChecks', technique, run['name']+'/'+step['label'], state_check(record['state'], step, baseline))
                    if step['tail'] and not step['edited']:
                        if rest is None: rest = record['state']['gltfMaterialBindings']['materials'][0]
                        else: require(record['state']['gltfMaterialBindings']['materials'][0] == rest, 'Undo/redo changed exact canonical rest record or packed-entry hash')
                    if 'control' in step: check('imageChecks', technique, run['name']+'/'+step['label'], binding.compare_images(record, controls[step['control']]))
                if save:
                    last = run['steps'][-1]; pair = pair_for(last)
                    report['exports'].append(dict(technique=technique, **inspect_export(save, pair))); persist()
                    copied, files = portable_copy(save, output/'portable'/technique/run['name']); report['portableCopies'].append(dict(files=files)); persist()
                    step = dict(label='reload', frame=13, resolution=last['resolution'], msaa=last['msaa'])
                    reload = launch(args, output, report, persist, verify, technique, run['name']+'-reload', copied, [step], initial=step)['reload']
                    check('stateChecks', technique, run['name']+'-reload', reference_state(reload['state'], pair))
                    check('imageChecks', technique, run['name']+'-reload', binding.compare_images(reload, records[last['label']]))
            if args.cohort != 'invalid':
                check('imageChecks', technique, 'separate-preflight-equivalence', binding.compare_images(preflight, first['before']))
                check('imageChecks', technique, 'materialization-preserves-interior-pixels', binding.compare_images(first['before'], first['unique']))
                check('imageChecks', technique, 'edit-isolates-left-positive-control', binding.isolate_change(first['before'], first['edit'], 0))
        multiplier = len(args.technique)
        require(len(report['captures']) == plan['expectedCaptureCount']*multiplier and len(report['processes']) == plan['expectedProcessCount']*multiplier,
            'Incomplete native capture/process cohort')
        require(len(report['stateChecks']) == len(report['captures']) and len(report['imageChecks']) == plan['expectedImageCheckCount']*multiplier and
            len(report['exports']) == plan['expectedExportCount']*multiplier, 'Incomplete native verification cohort')
        report.update(passed=True, nativeValidationPerformed=True); verify()
    except Exception as error: report['failure'] = str(error); raise
    finally:
        if report.get('nativeManifestBefore'):
            try: report['nativeManifestAfter'] = verify()
            except Exception as error: report.update(passed=False, afterManifestFailure=str(error)); report.setdefault('failure', str(error))
        persist()
    require(report['passed'], 'Native provenance failed after execution')


def synthetic_state(step):
    count = int(step['tail']); runtime = [3 if count and not step['restored'] else 2, 3 if step['assigned'] else 2]
    rows = [dict(graphNode=i, sourceNode=-1, renderable=True, retired=False, active=True, primitiveIndex=0, materialIndex=m,
        worldColumns=[[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [x, 0, 0, 1]]) for (i, x), m in zip([(1, -1.1), (3, 1.1)], runtime)]
    flags = [properties(step['edited'] and m == 3) for m in runtime]
    source = dict(sourceMaterial=0, runtimeMaterial=3, authored=material(step['edited']), evaluated=material(step['edited']),
        materialProperties=properties(step['edited']), uploadedMaterialHash=123)
    return dict(scene=dict(primaryObjectCount=2, primaryVertexCount=4, primaryIndexCount=6), resolution=step['resolution'], msaaSamples=step['msaa'],
        gltfMerge=dict(authoringNodes=rows, authoringNodesTruncated=False, graphNodeCount=2, graphIdentity=71,
            activeRoots=[1, 3], sourceTables=dict(materials=count, nodes=2)),
        gltfMaterialBindings=dict(sourceMaterialCount=count, runtimeMaterialCount=3+count, sourceToRuntime=[3] if count else [],
            materialsTruncated=False, materials=[source] if count else [], drawsTruncated=False,
            draws=[dict(graphNode=i, runtimeMaterial=m, primitiveIndex=0, indexCount=6, transparent=f['transparent'],
                doubleSided=f['doubleSided'], objectDoubleSided=f['doubleSided']) for i, m, f in zip((1, 3), runtime, flags)]),
        gltfMaterialAuthoring=dict(pendingGesture=False), gltfScenes=dict(editHistoryDepth=sum(step['history'])),
        gltfMaterialVariants=dict(undoCount=step['history'][0], redoCount=step['history'][1],
            assignments=[dict(node=i, material=m, transparent=f['transparent'], alphaMasked=False, doubleSided=f['doubleSided'])
                for i, m, f in zip((1, 3), runtime, flags)]))


def self_check():
    checks = []
    def reject(label, action):
        try: action()
        except (RuntimeError, KeyError, TypeError, ValueError, IndexError): checks.append(label); return
        raise RuntimeError('Self-check accepted damaged evidence: '+label)
    with tempfile.TemporaryDirectory(prefix='runtime-materialization-selfcheck-') as folder:
        root = Path(folder)
        for cohort in COHORTS:
            plan = generate(root/cohort, cohort)
            for run in plan['runs']:
                script = json.loads(Path(run['script']).read_text()); frames = [e['frame'] for e in script['events']]
                require(frames == sorted(set(frames)) and max(frames) <= script['frames'] and
                    script['sampleFrames'] == sorted(set(script['sampleFrames'])), 'Unbounded or unordered capture events')
                for step in run['steps']: state_check(synthetic_state(step), step)
            require(generate(root/cohort, cohort) == plan, 'Immutable generation is not repeatable')
            checks.append(cohort+'-bounded-immutable-plan-and-state-oracles')
        source, _ = load_asset(Path(plan['source'])); require(not source.get('materials') and
            'material' not in source['meshes'][0]['primitives'][0] and len(source['meshes']) == 1 and
            source['nodes'][0]['mesh'] == source['nodes'][1]['mesh'], 'Fixture has an explicit source material or unshared primitive')
        checks.append('actual-source-less-shared-primitive')
        base = spec('before', 13); state = synthetic_state(base); state_check(state, base)
        for label, mutate in [
                ('wrong-implicit-runtime-count', lambda s: s['gltfMaterialBindings'].update(runtimeMaterialCount=2)),
                ('wrong-implicit-binding', lambda s: s['gltfMerge']['authoringNodes'][0].update(materialIndex=0)),
                ('wrong-primitive-address', lambda s: s['gltfMerge']['authoringNodes'][0].update(graphNode=0)),
                ('wrong-source-count', lambda s: s['gltfMaterialBindings'].update(sourceMaterialCount=1)),
                ('missing-controller-draw', lambda s: s['gltfMaterialBindings']['draws'].pop()),
                ('duplicate-controller-draw', lambda s: s['gltfMaterialBindings']['draws'].append(copy.deepcopy(s['gltfMaterialBindings']['draws'][0]))),
                ('wrong-controller-material', lambda s: s['gltfMaterialBindings']['draws'][0].update(runtimeMaterial=0)),
                ('wrong-controller-route', lambda s: s['gltfMaterialBindings']['draws'][0].update(transparent=True)),
                ('wrong-object-sidedness', lambda s: s['gltfMaterialBindings']['draws'][0].update(objectDoubleSided=True)),
                ('truncated-draws', lambda s: s['gltfMaterialBindings'].update(drawsTruncated=True)),
                ('wrong-route', lambda s: s['gltfMaterialVariants']['assignments'][0].update(material=0)),
                ('pending-gesture', lambda s: s['gltfMaterialAuthoring'].update(pendingGesture=True))]:
            bad = copy.deepcopy(state); mutate(bad); reject(label, lambda bad=bad: state_check(bad, base))
        unique = spec('unique', 26, tail=True, history=(1, 0)); created = synthetic_state(unique)
        state_check(created, unique, state); checks.append('independent-materialized-defaults')
        for label, mutate in [
                ('wrong-source-runtime-map', lambda s: s['gltfMaterialBindings'].update(sourceToRuntime=[2])),
                ('lost-packed-hash', lambda s: s['gltfMaterialBindings']['materials'][0].update(uploadedMaterialHash=0)),
                ('wrong-metallic-default', lambda s: s['gltfMaterialBindings']['materials'][0]['authored']['pbrMetallicRoughness'].update(metallicFactor=0.)),
                ('wrong-unlit-classification', lambda s: s['gltfMaterialBindings']['materials'][0]['materialProperties'].update(unlit=True))]:
            bad = copy.deepcopy(created); mutate(bad); reject(label, lambda bad=bad: state_check(bad, unique, state))
        default_ext = material(); default_ext['extensions'] = copy.deepcopy(PHYSICAL); inspect_record(default_ext, material())
        bad = copy.deepcopy(default_ext); bad['extensions']['KHR_materials_transmission']['transmissionFactor'] = .2
        reject('nondefault-physical-extension', lambda: inspect_record(bad, material()))
        bad = material(True); bad['extensions']['KHR_materials_ior'] = dict(ior=1.5)
        reject('unlit-exclusion', lambda: inspect_record(bad, material(True)))
        reference = root/'reference.gltf'; pair = [material(True), material()]; write_fixture(reference, pair=pair); inspect_export(reference, pair)
        checks.append('independent-literal-export-oracle')
        model, _ = load_asset(reference); model['meshes'][0]['primitives'][0]['material'] = 1
        wrong = root/'collapsed.gltf'; document(wrong, model); reject('collapsed-export-bindings', lambda: inspect_export(wrong, pair))
        image = np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8)
        for a, b, c, d in binding.REGIONS: image[round(b*HEIGHT):round(d*HEIGHT), round(a*WIDTH):round(c*WIDTH)] = [40, 140, 80]
        path = root/'image.png'; Image.fromarray(image).save(path); record = dict(image=str(path), resolution=[WIDTH, HEIGHT])
        binding.visible_reference(record); binding.compare_images(record, record); reject('vacuous-image-isolation', lambda: binding.isolate_change(record, record, 0))
        changed = image.copy(); a, b, c, d = binding.REGIONS[0]
        changed[round(b*HEIGHT):round(d*HEIGHT), round(a*WIDTH):round(c*WIDTH)] = [130, 20, 90]
        other = root/'changed.png'; Image.fromarray(changed).save(other); altered = dict(image=str(other), resolution=[WIDTH, HEIGHT])
        binding.isolate_change(record, altered, 0); reject('wrong-literal-image', lambda: binding.compare_images(record, altered))
        checks.append('positive-interior-image-oracles')
    return dict(passed=True, checkCount=len(checks), checks=checks, rendererExecuted=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path); parser.add_argument('--renderer', type=Path); parser.add_argument('--manifest', type=Path)
    parser.add_argument('--technique', choices=TECHNIQUES, action='append'); parser.add_argument('--cohort', choices=COHORTS, default='core')
    parser.add_argument('--timeout', type=float, default=600.); parser.add_argument('--self-check', action='store_true'); parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    if args.self_check: print(json.dumps(self_check(), indent=2)); return
    if args.output is None: parser.error('--output is required')
    if not math.isfinite(args.timeout) or args.timeout <= 0: parser.error('--timeout must be finite and positive')
    if not args.generate_only and (args.renderer is None or args.manifest is None): parser.error('Native run requires explicit --renderer and --manifest')
    args.technique = tuple(dict.fromkeys(args.technique or TECHNIQUES))
    if args.renderer: args.renderer = args.renderer.resolve()
    if args.manifest: args.manifest = args.manifest.resolve()
    output = args.output.resolve(); plan = generate(output, args.cohort)
    report = dict(schemaVersion=1, cohort=args.cohort, passed=None if args.generate_only else False, generatedOnly=args.generate_only,
        rendererExecuted=False, nativeValidationPerformed=False, sourceDependencies=dependencies(), launchPlanSha256=sha(output/'launch-plan.json'),
        expectedCaptureCount=plan['expectedCaptureCount']*len(args.technique), expectedProcessCount=plan['expectedProcessCount']*len(args.technique),
        captures=[], processes=[], stateChecks=[], imageChecks=[], exports=[], portableCopies=[], unverified=UNVERIFIED)
    if args.generate_only: document(output/'results.json', report); print('GENERATED', report['expectedCaptureCount'], 'planned captures; renderer not executed')
    else:
        run_native(args, output, plan, report)
        print('PASS', len(report['captures']), 'captures;', len(report['stateChecks']), 'CPU checks;', len(report['imageChecks']), 'image checks')


if __name__ == '__main__': main()
