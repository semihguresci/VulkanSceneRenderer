"""Validate XMP import diagnostics, viewport-selected bindings and portable saves."""

import argparse
import copy
import hashlib
import json
from pathlib import Path

from gltf_accessor_runtime_regression import Asset, TECHNIQUES, load_asset, mesh, require
from gltf_inactive_scene_capacity_runtime_regression import verify_runtime_manifest
from gltf_material_variants_runtime_regression import pixels, run


XMP = 'KHR_xmp_json_ld'
SCENE_NAME = 'XMP studio'
PANELS = ('Copper panel', 'Cobalt panel')
MATERIALS = ('Copper surface', 'Cobalt surface')
LABELS = ('Studio asset', SCENE_NAME, PANELS[0], 'Copper mesh', MATERIALS[0],
          PANELS[1], 'Cobalt mesh', MATERIALS[1])
CONTEXT = {'dc': 'http://purl.org/dc/elements/1.1/', 'xmp': 'http://ns.adobe.com/xap/1.0/',
           'fixture': 'https://example.invalid/xmp-fixture#'}
# At a distance of four units the fixed 45-degree vertical FOV projects the
# two panel centers at x=260 and x=380 in the 640 by 360 viewport.
PICK_PIXELS = ([260, 180], [380, 180])
SAMPLE_FRAMES = (3, 6)
WARNINGS = (
    'packets[0]: XMP packet must be an object with an @context object',
    'asset: XMP packet must be an object with an @context object',
    'nodes[0]: XMP packet reference must be an object with a nonnegative integer packet index',
    'meshes[0]: XMP packet index is outside the packets array',
)


def document(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def package():
    return {'packets': [{
        '@context': CONTEXT,
        'xmp:Label': label,
        'dc:title': {'@value': f'{label} · édition française 日本語', '@language': 'fr'},
        'dc:creator': {'@list': ['Studio team', {'@value': 'Künstler', '@language': 'de'}]},
        'fixture:details': {'identifier': 2**54 + 3 + index, 'approved': True, 'empty': {},
                            'values': [None, [], 0, False]},
    } for index, label in enumerate(LABELS)]}


def attach(record, index):
    record.setdefault('extensions', {})[XMP] = {'packet': index}


def attachment(record):
    return record.get('extensions', {}).get(XMP)


def check_attachment(record, index):
    reference = attachment(record)
    require(reference == {'packet': index} and type(reference['packet']) is int,
            f'Wrong XMP attachment for {LABELS[index]}: {reference}')


def fixtures(output):
    asset = Asset()
    model = asset.model
    mesh(asset, asset.accessor('VEC3', [[-.35, -.5, 0], [.35, -.5, 0], [.35, .5, 0], [-.35, .5, 0]]),
         asset.accessor('VEC3', [[0, 0, 1]] * 4), asset.accessor('VEC2', [[0, 0], [1, 0], [1, 1], [0, 1]]))
    model['meshes'].append(copy.deepcopy(model['meshes'][0]))
    model['meshes'][1]['primitives'][0]['material'] = 1
    model['meshes'][0]['name'], model['meshes'][1]['name'] = LABELS[3], LABELS[6]
    model['nodes'] = [dict(name=name, mesh=index, translation=[offset, 0, 0])
                      for index, (name, offset) in enumerate(zip(PANELS, (-.55, .55)))]
    model['scenes'] = [dict(name=SCENE_NAME, nodes=[0, 1])]
    model['materials'] = [dict(name=name, pbrMetallicRoughness=dict(baseColorFactor=color),
                               extensions={'KHR_materials_unlit': {}})
                          for name, color in zip(MATERIALS, ([1, .16, .04, 1], [.04, .22, 1, 1]))]
    model['extensionsUsed'] = [XMP, 'KHR_materials_unlit']
    model['extensions'] = {XMP: package()}
    for index, record in enumerate((model['asset'], model['scenes'][0], model['nodes'][0], model['meshes'][0],
                                    model['materials'][0], model['nodes'][1], model['meshes'][1], model['materials'][1])):
        attach(record, index)
    valid = output / 'XMP valid.gltf'
    asset.write(valid)

    damaged = copy.deepcopy(model)
    damaged['extensions'][XMP]['packets'][0] = None
    attach(damaged['nodes'][0], 'not an integer')
    attach(damaged['meshes'][0], 99)
    del damaged['materials'][0]['extensions'][XMP]
    malformed = output / 'XMP malformed.gltf'
    document(malformed, damaged)

    sequence = output / 'xmp-viewport-selection.json'
    events = []
    for frame, pixel in zip(SAMPLE_FRAMES, PICK_PIXELS):
        events.extend((dict(frame=frame - 2, viewportPointer=dict(action='select', pixel=pixel)),
                       dict(frame=frame, depthProbe=pixel)))
    document(sequence, dict(schemaVersion=1, frames=SAMPLE_FRAMES[-1], sampleFrames=SAMPLE_FRAMES,
                            camera=[dict(frame=1, position=[0, 0, 4], target=[0, 0, 0])], events=events))
    return valid, malformed, sequence


def panel_binding(model, name):
    nodes = model['nodes']
    matching = [index for index, node in enumerate(nodes) if node.get('name') == name]
    require(len(matching) == 1, f'Expected one named source node for {name}')
    authored_node = matching[0]
    # Portable export can put the primitive on a generated child of the
    # retained authored node. Resolve the live mesh owner from that subtree.
    pending, visited, owners = [authored_node], set(), []
    while pending:
        index = pending.pop()
        require(0 <= index < len(nodes) and index not in visited, f'Invalid exported subtree for {name}')
        visited.add(index)
        node = nodes[index]
        if 'mesh' in node:
            owners.append(index)
        pending.extend(node.get('children', []))
    require(len(owners) == 1, f'Expected one mesh owner in the subtree of {name}')
    mesh_owner = owners[0]
    source_mesh = nodes[mesh_owner]['mesh']
    primitives = model['meshes'][source_mesh]['primitives']
    require(len(primitives) == 1, f'Expected one primitive in the mesh of {name}')
    return dict(authoredNode=authored_node, meshOwner=mesh_owner, sourceMesh=source_mesh,
                sourceMaterial=primitives[0]['material'])


def check_package(model):
    require(model['extensionsUsed'].count(XMP) == 1, 'Missing or duplicate XMP extension declaration')
    # Canonical JSON compares value types as well as content. Python's native
    # equality otherwise treats an integer 0 and a boolean false as equal.
    expected = json.dumps(package(), sort_keys=True)
    require(json.dumps(model['extensions'][XMP], sort_keys=True) == expected,
            'Portable XMP package changed its packet order or JSON-LD values')
    require(len(model['scenes']) == 1 and model['scene'] == 0 and model['scenes'][0]['name'] == SCENE_NAME,
            'Portable file changed the scene identity')
    check_attachment(model['asset'], 0)
    check_attachment(model['scenes'][0], 1)
    for panel, name in enumerate(PANELS):
        binding = panel_binding(model, name)
        node_packet = 2 + 3 * panel
        records = (model['nodes'][binding['authoredNode']], model['meshes'][binding['sourceMesh']],
                   model['materials'][binding['sourceMaterial']])
        for offset, record in enumerate(records):
            check_attachment(record, node_packet + offset)
        # The selected surface can belong to a generated mesh node. Its
        # packet is explicit so the inspector can resolve it directly.
        check_attachment(model['nodes'][binding['meshOwner']], node_packet)


def check_selection(record, model, panel):
    state = record['state']
    name, pixel = PANELS[panel], PICK_PIXELS[panel]
    source = panel_binding(model, name)
    probe = state['depthProbe']
    require(probe['pixel'] == pixel and probe['opaque'] > 0, f'No current opaque depth at {name}: {probe}')
    interaction = probe['sceneInteraction']
    raw_id = interaction['rawGpuId']
    require(type(raw_id) is int and raw_id > 0 and interaction['gpu'] is not None,
            f'No actual GPU pick result for {name}: {interaction}')
    hit = interaction['gpu']['hit']
    require(hit is not None and interaction['cpu']['hit'] == hit, f'CPU and GPU hit disagree at {name}: {interaction}')
    require(interaction['gpu']['selection'] == interaction['cpu']['selection'] == hit and
            state['gui']['selectedMeshNodeId'] == hit['graphNode'], f'Viewport did not select the GPU hit for {name}')
    require((hit['sourceNode'], hit['sourceMesh'], hit['sourcePrimitive'], hit['sourceInstance']) ==
            (source['meshOwner'], source['sourceMesh'], 0, -1), f'GPU hit the wrong source primitive for {name}: {hit}')
    require(state['gltfScenes']['selected'] == model['scene'] and state['gltfScenes']['names'] == [SCENE_NAME] and
            state['scene']['primaryObjectCount'] == 2, f'Unexpected active view for {name}')

    bindings = state['gltfMaterialBindings']
    draws = [draw for draw in bindings['draws'] if draw['graphNode'] == hit['graphNode']]
    require(len(draws) == 1 and not bindings['drawsTruncated'], f'No unique draw material binding for {name}')
    materials = [material for material in bindings['materials']
                 if material['runtimeMaterial'] == draws[0]['runtimeMaterial']]
    require(len(materials) == 1 and not bindings['materialsTruncated'], f'No unique live material row for {name}')
    live = materials[0]
    expected = model['materials'][source['sourceMaterial']]
    require(live['active'] and live['sourceMaterial'] == source['sourceMaterial'] and
            live['authored']['name'] == expected['name'] == MATERIALS[panel] and
            attachment(live['authored']) == attachment(expected), f'Wrong live source material XMP binding for {name}')
    return dict(name=name, pixel=pixel, rawGpuId=raw_id, graphNode=hit['graphNode'],
                nodeAttachment=attachment(model['nodes'][hit['sourceNode']]),
                runtimeMaterial=live['runtimeMaterial'], materialAttachment=attachment(expected), **source)


def diagnostics(log, source, *, malformed):
    marker = 'xmp_metadata: '
    lines = [line for line in Path(log).read_text(encoding='utf-8', errors='replace').splitlines() if marker in line]
    messages = [line.partition(marker)[2] for line in lines]
    require(all(f"{source.name}': " in line and "glTF asset '" in line for line in lines),
            f'XMP warnings must identify the source asset: {lines}')
    require(sorted(messages) == sorted(WARNINGS if malformed else ()), f'Unexpected XMP warnings: {lines}')
    return messages


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--technique', choices=TECHNIQUES, action='append')
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    inputs = output / 'fixtures'
    inputs.mkdir(parents=True, exist_ok=True)
    techniques = args.technique or TECHNIQUES
    report = dict(passed=False, generatedOnly=args.generate_only, nativeValidationPerformed=False,
                  captures=[], checks=[], diagnostics={}, expectedCaptureCount=8 * len(techniques),
                  expectedCheckCount=10 * len(techniques))
    try:
        valid, malformed, sequence = fixtures(inputs)
        fixture_hashes = {str(path): sha(path) for path in (valid, malformed, sequence)}
        report['fixtureSha256'] = fixture_hashes
        original, _ = load_asset(valid)
        check_package(original)
        if args.generate_only:
            report['passed'] = None
            return
        require(args.renderer is not None and args.manifest is not None, 'Native execution requires --renderer and --manifest')
        renderer = args.renderer.resolve(strict=True)
        report['rendererSha256'] = sha(renderer)
        report['nativeManifestBefore'] = verify_runtime_manifest(args.manifest, renderer)
        (output / 'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n', encoding='utf-8')
        for technique in techniques:
            portable = output / technique
            portable.mkdir(exist_ok=True)
            glb, gltf = portable / 'portable.glb', portable / 'portable.gltf'
            for path in (glb, gltf):
                path.unlink(missing_ok=True)
            reference = None
            for case, path, save in (('source', valid, glb), ('glb-reload', glb, gltf),
                                     ('gltf-reload', gltf, None), ('malformed', malformed, None)):
                model, _ = load_asset(path)
                records = run(renderer, output, path, f'{technique}-xmp-{case}', technique, 'lit', report['captures'],
                              sequence=sequence, save=save, require_validation=True, extra=('--no-ray-query',))
                require(len(records) == len(PANELS), f'Missing viewport selection captures for {case}')
                warnings = diagnostics(records[0]['log'], path, malformed=case == 'malformed')
                report['diagnostics'][f'{technique}/{case}'] = warnings
                selections = [check_selection(record, model, panel) for panel, record in enumerate(records)]
                for key in ('rawGpuId', 'graphNode', 'runtimeMaterial'):
                    require(selections[0][key] != selections[1][key], f'Both panels resolved to the same {key} in {case}')
                if reference is None:
                    reference = records
                    require(pixels(records[0]['image'], records[1]['image'])['maximumError'] > 0,
                            'The two viewport selections did not change the rendered image')
                for frame, record, oracle, selected in zip(SAMPLE_FRAMES, records, reference, selections):
                    error = pixels(record['image'], oracle['image'])
                    require(error['maximumError'] == 0, f'{case} changed the rendered selection at frame {frame}: {error}')
                    report['checks'].append(dict(technique=technique, case=case, frame=frame, **selected, **error))
                if save is not None:
                    check_package(load_asset(save)[0])
                    report['checks'].append(dict(technique=technique, case='portable-package', path=str(save),
                                                 packetCount=len(LABELS), sha256=sha(save)))
            print(f'PASS {technique} XMP selection, diagnostics and portable reloads', flush=True)
        report['nativeManifestAfter'] = verify_runtime_manifest(args.manifest, renderer)
        require(report['nativeManifestAfter'] == report['nativeManifestBefore'] and sha(renderer) == report['rendererSha256'],
                'Native executable or manifest changed during capture')
        require(all(sha(Path(path)) == expected for path, expected in fixture_hashes.items()),
                'A generated input changed during native capture')
        require(len(report['captures']) == report['expectedCaptureCount'] and
                len(report['checks']) == report['expectedCheckCount'], 'Incomplete XMP validation matrix')
        report.update(passed=True, nativeValidationPerformed=True)
    except Exception as error:
        report['failure'] = str(error)
        raise
    finally:
        report.update(captureCount=len(report['captures']), checkCount=len(report['checks']))
        document(output / 'results.json', report)


if __name__ == '__main__':
    main()
