"""Inactive over-capacity scenes remain portable while admitted views render.

The 4,097 inactive draws share one source primitive. An attached punctual light
forces all-scenes light export to evaluate that inactive view using authoring,
rather than GPU publication, limits. Native execution requires a frozen manifest;
--generate-only never launches the renderer or claims native validation.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

import numpy as np
from PIL import Image

from gltf_accessor_runtime_regression import Asset, load_asset, mesh, require
from gltf_material_variants_runtime_regression import pixels, run
from gltf_texture_formats_runtime_regression import verify_manifest


TECHNIQUES = ('deferred-raster', 'forward-raster')
INACTIVE_DRAWS = 4097
SCENE_NAMES = ['Admitted active scene', 'Inactive over GPU capacity']
LIGHTS = [
    dict(name='Active point', type='point', color=[1, .25, .1], intensity=2.5, range=4.5),
    dict(name='Inactive attached point', type='point', color=[.2, .7, .4], intensity=11.5, range=7.25),
]
CAPACITY_DIAGNOSTIC = 'selected scene exceeds the renderable object budget'


def fixture(source: Path, oracle: Path):
    asset = Asset()
    mesh(asset, asset.accessor('VEC3', [[-.7, -.6, 0], [.7, -.6, 0], [.7, .6, 0], [-.7, .6, 0]]),
         asset.accessor('VEC3', [[0, 0, 1]] * 4),
         asset.accessor('VEC2', [[0, 0], [1, 0], [1, 1], [0, 1]]),
         tangent=asset.accessor('VEC4', [[1, 0, 0, 1]] * 4),
         indices=asset.accessor('SCALAR', [0, 1, 2, 0, 2, 3], 5123, indices=True))
    model = asset.model
    model['materials'] = [dict(pbrMetallicRoughness=dict(
        baseColorFactor=[.035, .3, .85, 1], metallicFactor=0, roughnessFactor=1),
        extensions=dict(KHR_materials_unlit={}))]
    model['extensionsUsed'] = ['KHR_materials_unlit', 'KHR_node_visibility', 'KHR_lights_punctual']
    model['extensions'] = dict(KHR_lights_punctual=dict(lights=copy.deepcopy(LIGHTS)))
    model['nodes'] = [
        dict(name='Visible active mesh', mesh=0),
        dict(name='Active light node', translation=[-1, 1, 2],
             extensions=dict(KHR_lights_punctual=dict(light=0))),
        dict(name='Inactive large root', children=list(range(3, 3 + INACTIVE_DRAWS))),
    ]
    for index in range(INACTIVE_DRAWS):
        model['nodes'].append(dict(name=f'Inactive draw {index}', mesh=0,
            translation=[20 + (index % 65) * .1, (index // 65) * .1, 0],
            extras=dict(capacitySourceIdentity=index)))
    model['nodes'][3]['extensions'] = dict(KHR_node_visibility=dict(
        visible=False, extras=dict(capacityFixture='hidden-source-node')))
    model['nodes'][-1]['extensions'] = dict(KHR_lights_punctual=dict(light=1))
    model['scenes'] = [dict(name=SCENE_NAMES[0], nodes=[0, 1], extras=dict(capacityScene=0)),
                       dict(name=SCENE_NAMES[1], nodes=[2], extras=dict(capacityScene=1))]
    model['scene'] = 0
    asset.write(source)
    # The oracle has no inactive node, scene or light and does not use the
    # tested active-root selection to exclude oversized authoring content.
    reference = copy.deepcopy(model)
    reference['nodes'] = copy.deepcopy(model['nodes'][:2])
    reference['scenes'] = [copy.deepcopy(model['scenes'][0])]
    reference['extensions']['KHR_lights_punctual']['lights'] = [copy.deepcopy(LIGHTS[0])]
    reference['extensionsUsed'].remove('KHR_node_visibility')
    oracle.write_text(json.dumps(reference, indent=2) + '\n', encoding='utf-8')
    return model


def scene_nodes(model, selected):
    require(0 <= selected < len(model['scenes']), 'Missing scene in saved asset')
    visited, pending = set(), list(model['scenes'][selected].get('nodes', []))
    while pending:
        index = pending.pop()
        require(isinstance(index, int) and 0 <= index < len(model['nodes']), 'Invalid saved node index')
        require(index not in visited, 'Saved scene contains a cycle or repeated node')
        visited.add(index)
        pending.extend(model['nodes'][index].get('children', []))
    return visited


def inspect_asset(path: Path, *, complete=True):
    model, _ = load_asset(path)
    require(model['scene'] == 0, 'Save changed the selected default')
    require(len(model['scenes']) == (2 if complete else 1), 'Save lost or invented scenes')
    require([scene.get('name') for scene in model['scenes']] == SCENE_NAMES[:len(model['scenes'])],
            'Scene names changed')
    incoming = [0] * len(model['nodes'])
    for node in model['nodes']:
        for child in node.get('children', []):
            require(isinstance(child, int) and 0 <= child < len(incoming), 'Invalid child index')
            incoming[child] += 1
    require(max(incoming, default=0) <= 1 and
            all(incoming[root] == 0 for scene in model['scenes'] for root in scene.get('nodes', [])),
            'Saved output violates the global strict forest')
    pending = [index for index, count in enumerate(incoming) if count == 0]
    visited_count = 0
    while pending:
        index = pending.pop()
        visited_count += 1
        pending.extend(model['nodes'][index].get('children', []))
    require(visited_count == len(model['nodes']), 'Saved forest contains a disconnected cycle')
    counts, ranges = [], []
    light_definitions = model['extensions']['KHR_lights_punctual']['lights']
    for selected, scene in enumerate(model['scenes']):
        require(scene.get('extras', {}).get('capacityScene') == selected, 'Scene extras changed')
        active = scene_nodes(model, selected)
        draws = sum(len(model['meshes'][model['nodes'][index]['mesh']]['primitives'])
                    for index in active if 'mesh' in model['nodes'][index])
        require(draws == (1 if selected == 0 else INACTIVE_DRAWS), 'Saved scene lost mesh placements')
        counts.append(draws)
        lights = [light_definitions[model['nodes'][index]['extensions']['KHR_lights_punctual']['light']]
                  for index in active if 'KHR_lights_punctual' in model['nodes'][index].get('extensions', {})]
        require(len(lights) == 1, 'Saved scene lost or duplicated its attached light')
        actual, expected = lights[0], LIGHTS[selected]
        require(actual['type'] == 'point' and
                np.allclose(actual.get('color', [1, 1, 1]), expected['color'], atol=1e-5, rtol=0) and
                abs(actual['intensity'] - expected['intensity']) < 1e-5 and
                abs(actual['range'] - expected['range']) < 1e-5,
                'Inactive light color/intensity/range changed during all-scenes export')
        ranges.append(actual['range'])
    if complete:
        matches = [node for node in model['nodes'] if node.get('name') == 'Inactive draw 0']
        require(len(matches) == 1, 'Saved source group identity changed')
        hidden = matches[0]['extensions']['KHR_node_visibility']
        require(hidden['visible'] is False and hidden.get('extras', {}).get('capacityFixture') == 'hidden-source-node',
                'Saved inactive source visibility or extension metadata changed')
        identities = [node.get('extras', {}).get('capacitySourceIdentity') for node in model['nodes']]
        require(sorted(value for value in identities if value is not None) == list(range(INACTIVE_DRAWS)),
                'Saved inactive source-node metadata changed')
    return dict(sceneDrawCounts=counts, sceneLightRanges=ranges,
                nodeCount=len(model['nodes']))


def assert_active(state, *, source):
    view = state['gltfScenes']
    require(view['selected'] == 0 and view['names'] == SCENE_NAMES, 'Wrong admitted scene')
    require(view['activeRenderableCount'] == 1 and state['scene']['primaryObjectCount'] == 1,
            'Inactive oversized scene reached GPU object publication')
    require(view['pointLightCount'] == 1 and len(state['gltfAnimation']['pointLights']) == 1,
            'Inactive attached light reached active publication')
    require(view['authoringNodeCount'] > INACTIVE_DRAWS, 'Inactive authoring graph was discarded')
    if source:
        require(state['gltfDeformation']['sourceVertexCount'] == 4,
                'Repeated source placements duplicated the shared input geometry')


def reject_oversized(renderer, output, source, technique, timeout):
    image = output / f'{technique}-rejected.png'
    image.unlink(missing_ok=True)
    log = output / f'{technique}-rejected.log'
    command = [str(renderer), '--hidden', '--no-ui', '--validation', '--model', str(source),
               '--gltf-scene', '1', '--render-technique', technique, '--width', '64', '--height', '64',
               '--msaa', '1', '--no-taa', '--warmup-frames', '1', '--capture-frame', '2',
               '--screenshot', str(image)]
    with log.open('w', encoding='utf-8') as stream:
        result = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
            timeout=timeout, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    text = log.read_text(encoding='utf-8', errors='replace')
    require(result.returncode != 0 and CAPACITY_DIAGNOSTIC in text,
            f'Oversized CLI scene did not reject with its admission diagnostic: {log}')
    require(not image.exists(), 'Rejected scene produced a success capture')
    require(not re.search(r'VUID-|SYNC-HAZARD|Validation Error', text, re.I), f'Validation error: {log}')
    return dict(technique=technique, check='oversized-cli-preflight-rejection', returnCode=result.returncode,
                diagnostic=CAPACITY_DIAGNOSTIC, log=str(log), command=command)


def verify_runtime_manifest(path, renderer):
    manifest = path.read_bytes()
    verified = verify_manifest(path)
    require(verified['sha256'] == hashlib.sha256(manifest).hexdigest(), 'Native freeze manifest changed while verifying')
    frozen = {Path(name).resolve() for name in json.loads(manifest)['files']}
    shaders = {shader.resolve() for shader in (renderer.parent / 'spv_shaders').rglob('*.spv') if shader.is_file()}
    require(shaders, 'Renderer runtime contains no SPIR-V shaders to freeze')
    missing = ({renderer.resolve()} | shaders) - frozen
    require(not missing, f'Native freeze manifest omits the executable or shaders: {sorted(map(str, missing))}')
    return dict(verified, shaderCount=len(shaders))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--technique', action='append', choices=TECHNIQUES)
    parser.add_argument('--timeout', type=float, default=240)
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    techniques = args.technique or TECHNIQUES
    report = dict(schemaVersion=1, passed=False, nativeValidationPerformed=False, generatedOnly=args.generate_only,
                  renderer=str(renderer), rendererSha256=None, fixtures=[], captures=[], checks=[],
                  expectedCaptureCount=3 * len(techniques), expectedCheckCount=5 * len(techniques),
                  expectedRejectedLaunches=len(techniques),
                  unverified=['No interactive UI capacity rejection or GPU picking readback is exercised'])
    def save():
        report.update(captureCount=len(report['captures']), checkCount=len(report['checks']))
        (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    try:
        source, oracle = output / 'inactive-over-capacity.gltf', output / 'active-only-oracle.gltf'
        model = fixture(source, oracle)
        require(len(model['meshes']) == 1, 'Fixture does not share one source primitive')
        source_metadata = inspect_asset(source)
        inspect_asset(oracle, complete=False)
        report['fixtures'].append(dict(source=str(source), oracle=str(oracle),
            sourceSha256=hashlib.sha256(source.read_bytes()).hexdigest(),
            oracleSha256=hashlib.sha256(oracle.read_bytes()).hexdigest(), **source_metadata))
        if args.generate_only:
            report['passed'] = None
            return
        require(renderer.is_file() and args.manifest is not None, 'Native execution requires renderer and frozen manifest')
        report['rendererSha256'] = hashlib.sha256(renderer.read_bytes()).hexdigest()
        report['nativeManifestBefore'] = verify_runtime_manifest(args.manifest, renderer)
        (output / 'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n', encoding='utf-8')
        for technique in techniques:
            prefix = technique + '-capacity'
            destination, ascii_path = output / (prefix + '.glb'), output / (prefix + '.gltf')
            destination.unlink(missing_ok=True)
            ascii_path.unlink(missing_ok=True)
            reference = run(renderer, output, oracle, prefix + '-oracle', technique, 'lit',
                            report['captures'], timeout=args.timeout, gltf_scene=0)[0]
            rgb = np.asarray(Image.open(reference['image']).convert('RGB'), dtype=np.int16)
            require(int(np.count_nonzero((rgb[:, :, 2] > rgb[:, :, 0] + 20) & (rgb[:, :, 2] > 50))) > 1000,
                    'Active-only oracle has no substantial blue surface')
            actual = run(renderer, output, source, prefix + '-source', technique, 'lit',
                         report['captures'], timeout=args.timeout, gltf_scene=0, save=destination)[0]
            assert_active(actual['state'], source=True)
            error = pixels(actual['image'], reference['image'])
            require(error['maximumError'] == 0, f'Inactive authoring content changed admitted view: {error}')
            report['checks'].append(dict(technique=technique, check='source-active-only-oracle', **error))
            report['checks'].append(dict(technique=technique, check='all-scenes-glb-metadata', **inspect_asset(destination)))
            reload = run(renderer, output, destination, prefix + '-reload', technique, 'lit',
                         report['captures'], timeout=args.timeout, gltf_scene=0, save=ascii_path)[0]
            assert_active(reload['state'], source=False)
            error = pixels(reload['image'], reference['image'])
            require(error['maximumError'] == 0, f'GLB reload changed admitted view: {error}')
            report['checks'].append(dict(technique=technique, check='glb-active-only-oracle', **error))
            report['checks'].append(dict(technique=technique, check='all-scenes-ascii-metadata', **inspect_asset(ascii_path)))
            report['checks'].append(reject_oversized(renderer, output, source, technique, args.timeout))
            save()
            print('PASS inactive capacity', technique, flush=True)
        require(len(report['captures']) == report['expectedCaptureCount'] and
                len(report['checks']) == report['expectedCheckCount'], 'Incomplete native capacity matrix')
        require(hashlib.sha256(renderer.read_bytes()).hexdigest() == report['rendererSha256'], 'Renderer changed during validation')
        report['nativeManifestAfter'] = verify_runtime_manifest(args.manifest, renderer)
        require(report['nativeManifestAfter'] == report['nativeManifestBefore'], 'Frozen native manifest changed during validation')
        report['nativeValidationPerformed'] = True
        report['passed'] = True
    except Exception as error:
        report['failure'] = str(error)
        raise
    finally:
        save()
        print('GENERATED' if args.generate_only else 'NATIVE', report['captureCount'], 'captures',
              report['checkCount'], 'checks', flush=True)


if __name__ == '__main__':
    main()
