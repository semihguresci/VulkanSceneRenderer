"""Exercise primary-only export and protect the live BIM source and its aliases."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil

from gltf_accessor_runtime_regression import Asset, TECHNIQUES, load_asset, mesh, require
from gltf_compression_runtime_regression import write_glb
from gltf_inactive_scene_capacity_runtime_regression import verify_runtime_manifest
from gltf_material_variants_runtime_regression import pixels, run


PRIMARY = 'Primary panel'
EDITED = 'Edited primary panel'
AUXILIARY = 'BIM sidecar panel'
OPTIONS = ('--import-scale', '1', '--sky', 'plain', '--no-ray-query')
PROTECTION = 'Scene export cannot replace the loaded auxiliary model'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def panel(path, name, x, color):
    asset = Asset()
    mesh(asset, asset.accessor('VEC3', [[-.35, -.5, 0], [.35, -.5, 0], [.35, .5, 0], [-.35, .5, 0]]),
         asset.accessor('VEC3', [[0, 0, 1]] * 4), asset.accessor('VEC2', [[0, 0], [1, 0], [1, 1], [0, 1]]))
    asset.model['nodes'][0].update(name=name, translation=[x, 0, 0])
    asset.model['extensionsUsed'] = ['KHR_materials_unlit']
    asset.model['materials'] = [dict(extensions={'KHR_materials_unlit': {}},
                                     pbrMetallicRoughness=dict(baseColorFactor=color))]
    if path.suffix == '.glb':
        asset.model['buffers'] = [dict(byteLength=len(asset.data))]
        write_glb(path, asset.model, bytes(asset.data))
    else:
        asset.write(path)


def fixture(output):
    source = output / 'sources'
    source.mkdir()
    primary, auxiliary = source / 'primary.gltf', source / 'auxiliary.glb'
    panel(primary, PRIMARY, -.65, [1, .3, .1, 1])
    panel(auxiliary, AUXILIARY, .65, [.2, .8, .4, 1])
    alias = source / 'auxiliary-hard-link.glb'
    os.link(auxiliary, alias)
    sequence = output / 'edited.json'
    sequence.write_text(json.dumps(dict(schemaVersion=1, frames=4, sampleFrames=[4], events=[
        dict(frame=2, nodeAuthoring=dict(action='rename', sourceNode=0, name=EDITED))])), encoding='utf-8')
    return primary, auxiliary, alias, sequence


def check_scene(state, auxiliary_count):
    scene = state['scene']
    require(scene['primaryObjectCount'] == 1 and scene['auxiliaryObjectCount'] == auxiliary_count,
            f'Wrong primary or auxiliary draw count: {scene}')
    names = {node['name'] for node in state['gltfNodeFlags']['nodes'] if node['active']}
    require(EDITED in names and PRIMARY not in names and AUXILIARY not in names, f'Wrong primary hierarchy: {names}')


def expect_protected(renderer, output, primary, destination, prefix, sequence, options):
    previous = digest(destination)
    try:
        run(renderer, output, primary, prefix, TECHNIQUES[0], 'lit', [],
            sequence=sequence, save=destination, extra=options, require_validation=True)
    except RuntimeError:
        log = (output / f'{prefix}.log').read_text(encoding='utf-8', errors='replace')
        require(PROTECTION in log, f'Expected the loaded-sidecar diagnostic in {prefix}.log')
    else:
        raise RuntimeError('Primary export accepted the loaded auxiliary source as its destination')
    telemetry = output / f'{prefix}.telemetry.json'
    state = json.loads(telemetry.read_bytes())
    check_scene(state, 1)
    require(state['validationEnabled'] and digest(destination) == previous,
            'Rejected save changed the source or skipped validation')
    return dict(telemetry=str(telemetry), log=str(output / f'{prefix}.log'), preservedSha256=previous)


def portable_copy(path, output):
    output.mkdir()
    model, buffers = load_asset(path)
    require(len(buffers) == 1 and buffers[0], 'Exported geometry is empty')
    names = {node.get('name') for node in model['nodes']}
    require(EDITED in names and AUXILIARY not in names, f'Unexpected saved hierarchy: {names}')
    buffer, = model['buffers']
    if path.suffix == '.gltf':
        uri = Path(buffer['uri'])
        require(uri.name == str(uri) and uri.suffix == '.bin', 'ASCII export must use a relative adjacent dependency')
        shutil.copyfile(path.parent / uri, output / uri)
    else:
        require('uri' not in buffer, 'GLB must contain its geometry')
    copy = output / path.name
    shutil.copyfile(path, copy)
    return copy


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    primary, auxiliary, alias, sequence = fixture(output)
    if args.generate_only:
        return
    require(args.renderer is not None and args.manifest is not None, 'Native run requires --renderer and --manifest')
    renderer = args.renderer.resolve(strict=True)
    manifest = verify_runtime_manifest(args.manifest, renderer)
    (output / 'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n', encoding='utf-8')
    mixed = (*OPTIONS, '--bim-model', str(auxiliary))
    original = {path: digest(path) for path in (primary, auxiliary, alias)}
    rejected = [expect_protected(renderer, output, primary, target, label, sequence, mixed)
                for target, label in ((auxiliary, 'protect-direct'), (alias, 'protect-hard-link'))]
    records = []
    references = {}
    staging = output / 'exported'
    staging.mkdir()
    for technique, extension in zip(TECHNIQUES, ('.glb', '.gltf')):
        destination = staging / ('primary' + extension)
        live, = run(renderer, output, primary, 'mixed-' + technique, technique, 'lit', records,
                    sequence=sequence, save=destination, extra=mixed, require_validation=True)
        check_scene(live['state'], 1)
        reference, = run(renderer, output, primary, 'primary-' + technique, technique, 'lit', records,
                         sequence=sequence, extra=OPTIONS, require_validation=True)
        check_scene(reference['state'], 0)
        require(pixels(live['image'], reference['image'])['maximumError'] > 0,
                'The live sidecar made no visible difference')
        copy = portable_copy(destination, output / ('relocated-' + technique))
        references[technique] = (copy, reference['image'])

    in_place = primary.with_name('in-place.gltf')
    shutil.copyfile(primary, in_place)
    saved, = run(renderer, output, in_place, 'in-place', TECHNIQUES[0], 'lit', records,
                 sequence=sequence, save=in_place, extra=mixed, require_validation=True)
    check_scene(saved['state'], 1)
    portable_copy(in_place, output / 'relocated-in-place')
    require(all(digest(path) == sha for path, sha in original.items()), 'An original scene or auxiliary file changed')
    primary.parent.rename(output / 'offline-sources')
    staging.rename(output / 'offline-exports')
    for technique, (copy, reference) in references.items():
        restored, = run(renderer, output, copy, 'restored-' + technique, technique, 'lit', records,
                        extra=OPTIONS, require_validation=True)
        check_scene(restored['state'], 0)
        error = pixels(restored['image'], reference)
        require(error['maximumError'] == 0, f'Relocated primary render changed: {error}')
    require(len(records) == 7 and verify_runtime_manifest(args.manifest, renderer) == manifest,
            'Incomplete safety captures or changed runtime manifest')
    (output / 'results.json').write_text(json.dumps(dict(passed=True, rendererSha256=digest(renderer),
                                                       captures=records, rejected=rejected), indent=2) + '\n', encoding='utf-8')
    print('PASS loaded BIM source protection and relocated primary-only glTF/GLB')


if __name__ == '__main__':
    main()
