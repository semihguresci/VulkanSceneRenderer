"""Native normal-map UV tangent checks against analytical authored frames.

Generated static and skin-only triangles have unchanged source vertex/index
order and opposite UV0/UV1 orientations. Missing tangents must match explicitly
authored T=-Y, w=+1, including KHR_texture_transform.texCoord overrides. A
deliberately wrong UV0 frame is rendered as a nonvacuous negative control.
Requires NumPy, Pillow, and the built Vulkan renderer; no downloaded assets.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import io
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess

import numpy as np
from PIL import Image


CASES = ('static-uv1', 'skin-uv1', 'static-override-uv1', 'skin-override-uv1')
TECHNIQUES = ('deferred-raster', 'forward-raster')
POSITIONS = ((-1., -.75, 0.), (1., -.75, 0.), (-1., .95, 0.))
UV0 = ((0., 0.), (0., 1.), (1., 0.))
UV1 = ((0., 0.), (0., 1.), (-1., 0.))
INDICES = (0, 1, 2)
TANGENT = (0., -1., 0., 1.)
WRONG_UV0_TANGENT = (0., 1., 0., -1.)
JOINT_ANGLE = .25


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def finite_json(value):
    if isinstance(value, float):
        return math.isfinite(value)
    if isinstance(value, dict):
        return all(finite_json(v) for v in value.values())
    if isinstance(value, list):
        return all(finite_json(v) for v in value)
    return True


def normal_png():
    # Positive tangent X and Z make the reversed tangent direction visible in
    # normal RGB and directional lighting. A constant map isolates the frame
    # from texture sampling/wrap behavior at the deliberately mirrored UVs.
    stream = io.BytesIO()
    Image.new('RGB', (2, 2), (217, 153, 217)).save(stream, format='PNG')
    return stream.getvalue()


def write_fixture(path, case, frame):
    data, views, accessors = bytearray(), [], []

    def accessor(values, kind, component=5126):
        while len(data) % 4:
            data.append(0)
        width = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[kind]
        flat = [v for row in values for v in row] if width > 1 else list(values)
        begin = len(data)
        data.extend(struct.pack('<' + {5121: 'B', 5123: 'H', 5126: 'f'}[component] * len(flat), *flat))
        views.append(dict(buffer=0, byteOffset=begin, byteLength=len(data)-begin))
        entry = dict(bufferView=len(views)-1, componentType=component, count=len(values), type=kind)
        if kind == 'VEC3':
            entry.update(min=[min(row[i] for row in values) for i in range(3)],
                         max=[max(row[i] for row in values) for i in range(3)])
        accessors.append(entry)
        return len(accessors)-1

    attributes = dict(POSITION=accessor(POSITIONS, 'VEC3'),
                      NORMAL=accessor(((0., 0., 1.),)*3, 'VEC3'),
                      TEXCOORD_0=accessor(UV0, 'VEC2'), TEXCOORD_1=accessor(UV1, 'VEC2'))
    if frame != 'generated':
        attributes['TANGENT'] = accessor((TANGENT if frame == 'analytical' else WRONG_UV0_TANGENT,)*3, 'VEC4')
    normal = dict(index=0, texCoord=0 if 'override' in case else 1)
    model = dict(asset=dict(version='2.0', generator='Container analytical normal-map UV tangent regression'),
                 scene=0, scenes=[dict(nodes=[0])], nodes=[dict(name=case, mesh=0)],
                 materials=[dict(name='Normal-map UV test', normalTexture=normal,
                                 pbrMetallicRoughness=dict(baseColorFactor=[.55, .65, .75, 1.],
                                                          metallicFactor=0., roughnessFactor=.65))],
                 textures=[dict(source=0, sampler=0)],
                 samplers=[dict(magFilter=9729, minFilter=9729, wrapS=33071, wrapT=33071)],
                 images=[dict(uri='data:image/png;base64,' + base64.b64encode(normal_png()).decode())])
    if 'override' in case:
        normal['extensions'] = {'KHR_texture_transform': dict(texCoord=1)}
        model['extensionsUsed'] = ['KHR_texture_transform']
    if case.startswith('skin'):
        attributes['JOINTS_0'] = accessor(((0, 0, 0, 0),)*3, 'VEC4', 5121)
        attributes['WEIGHTS_0'] = accessor(((1., 0., 0., 0.),)*3, 'VEC4')
        # glTF cancels the mesh owner's translation during skinning. The
        # independent nonuniform joint also exercises tangent transport.
        model['nodes'][0].update(skin=0, translation=[2., 0., 0.])
        model['nodes'].append(dict(name='Analytical joint', translation=[.1, .05, 0.],
                                   rotation=[0., 0., math.sin(JOINT_ANGLE/2), math.cos(JOINT_ANGLE/2)],
                                   scale=[1.1, .9, 1.]))
        model['scenes'][0]['nodes'].append(1)
        model['skins'] = [dict(joints=[1])]
    model['meshes'] = [dict(primitives=[dict(attributes=attributes, indices=accessor(INDICES, 'SCALAR', 5123),
                                            material=0, mode=4)])]
    model.update(bufferViews=views, accessors=accessors,
                 buffers=[dict(byteLength=len(data), uri='data:application/octet-stream;base64,' + base64.b64encode(data).decode())])
    path.write_text(json.dumps(model, indent=2), encoding='utf-8')
    return model


def verify_source_order(models):
    def payload(model, semantic):
        primitive = model['meshes'][0]['primitives'][0]
        index = primitive['indices'] if semantic == 'indices' else primitive['attributes'][semantic]
        accessor = model['accessors'][index]
        view = model['bufferViews'][accessor['bufferView']]
        data = base64.b64decode(model['buffers'][0]['uri'].split(',', 1)[1])
        return data[view['byteOffset']:view['byteOffset']+view['byteLength']]
    for semantic in ('POSITION', 'NORMAL', 'TEXCOORD_0', 'TEXCOORD_1', 'indices'):
        require(len({payload(model, semantic) for model in models}) == 1,
                f'Oracle changes source layout for {semantic}')
    require('TANGENT' not in models[0]['meshes'][0]['primitives'][0]['attributes'],
            'Generated fixture unexpectedly supplies tangents')
    # Independently verify the two constant analytical frames using the UV
    # Jacobian. No production loader, MikkTSpace, or deformation code is used.
    edge1, edge2 = np.asarray(POSITIONS[1])-POSITIONS[0], np.asarray(POSITIONS[2])-POSITIONS[0]
    for coordinates, expected in ((UV1, TANGENT), (UV0, WRONG_UV0_TANGENT)):
        a, b = np.asarray(coordinates[1])-coordinates[0], np.asarray(coordinates[2])-coordinates[0]
        determinant = a[0]*b[1]-a[1]*b[0]
        tangent = (edge1*b[1]-edge2*a[1])/determinant
        tangent /= np.linalg.norm(tangent)
        bitangent = (edge2*a[0]-edge1*b[0])/determinant
        sign = -1. if np.dot(np.cross([0.,0.,1.], tangent), bitangent) < 0 else 1.
        require(np.max(np.abs(np.append(tangent, sign)-expected)) < 1e-12,
                'Fixture analytical tangent does not match its UV Jacobian')


def capture(renderer, output, fixture, name, technique, display, timeout, records):
    image = output / (name + '.png')
    telemetry, log = image.with_suffix('.telemetry.json'), image.with_suffix('.log')
    image.unlink(missing_ok=True)
    telemetry.unlink(missing_ok=True)
    command = [str(renderer), '--hidden', '--no-ui', '--validation', '--width', '640', '--height', '360',
               '--msaa', '1', '--warmup-frames', '3', '--capture-frame', '4', '--fixed-dt', '.016666667',
               '--render-technique', technique, '--display-mode', display, '--no-bloom',
               '--environment-intensity', '0', '--directional-intensity', '3',
               '--directional-direction', '0', '1', '-1', '--exposure', '1',
               '--camera-position', '0', '0', '4', '--camera-target', '0', '0', '0', '--camera-fov', '45',
               '--model', str(fixture), '--screenshot', str(image)]
    with log.open('w', encoding='utf-8') as stream:
        process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                                 timeout=timeout, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                                 creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    text = log.read_text(encoding='utf-8', errors='replace')
    require(process.returncode == 0, f'Native tangent capture failed ({process.returncode}): {log}')
    require(not re.search(r'VUID-|SYNC-HAZARD|Validation Error', text, re.I), f'Vulkan validation failed: {log}')
    require(image.is_file() and telemetry.is_file(), f'Missing fresh native capture: {image}')
    state = json.loads(telemetry.read_text(encoding='utf-8'))
    require(finite_json(state), f'Nonfinite telemetry: {telemetry}')
    require(state['technique'] == technique, f'Wrong raster technique: {telemetry}')
    record = dict(image=str(image), telemetry=str(telemetry), log=str(log), command=command)
    records.append(record)
    return dict(**record, state=state)


def check_frame_state(record, case, frame):
    state = record['state']['gltfDeformation']
    if not case.startswith('skin'):
        require(not state['active'], 'Static tangent fixture unexpectedly deforms')
        return
    require(state['active'] and state['gpuReady'] and state['currentBoundToGpu'],
            'Skin-only tangent fixture did not bind GPU deformation')
    require(state['instanceCount'] == 1 and state['sourceVertexCount'] == 3 and state['expandedVertexCount'] == 6,
            'Skin-only source vertex order/count changed')
    require(len(state['instances']) == 1, 'Skin-only fixture lost its instance')
    instance = state['instances'][0]
    require(not instance['weights'], 'Skin-only fixture unexpectedly has morph weights')
    tangent = [math.sin(JOINT_ANGLE), -math.cos(JOINT_ANGLE), 0., 1.]
    if frame == 'wrong-uv0':
        tangent = [-tangent[0], -tangent[1], 0., -1.]
    require(np.max(np.abs(np.asarray(instance['firstTangent'])-tangent)) < 2e-4,
            f'Analytical skinned tangent differs: {instance["firstTangent"]}, expected {tangent}')
    require(np.max(np.abs(np.asarray(instance['firstNormal'])-[0., 0., 1.])) < 2e-4,
            f'Analytical skinned normal differs: {instance["firstNormal"]}')


def check_pixels(generated, reference, wrong):
    a, b, c = [np.asarray(Image.open(item['image']).convert('RGB'), dtype=float)
               for item in (generated, reference, wrong)]
    require(a.shape == b.shape == c.shape == (360, 640, 3), 'Wrong native capture dimensions')
    require(float(b.std()) > 3, 'Analytical reference image is vacuous')
    error = np.abs(a-b)
    mae, p999 = float(error.mean()), float(np.percentile(error, 99.9))
    require(mae <= .1 and p999 <= 2, f'Generated UV1 frame differs from analytical tangent: MAE={mae}, p99.9={p999}')
    negative = np.abs(b-c)
    changed = float(np.mean(np.max(negative, axis=2) >= 16))
    negative_mae = float(negative.mean())
    require(changed >= .005 and negative_mae >= .5,
            f'Wrong-UV0 control is not visibly different: changed={changed:.3%}, MAE={negative_mae}')
    return dict(meanAbsolutePixelError=mae, percentile99_9PixelError=p999,
                wrongUv0ChangedPixelFraction=changed, wrongUv0MeanAbsolutePixelError=negative_mae)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', action='append', choices=CASES)
    parser.add_argument('--technique', action='append', choices=TECHNIQUES)
    parser.add_argument('--display', action='append', choices=('lit', 'normals'))
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    require(renderer.is_file(), f'Missing renderer: {renderer}')
    output.mkdir(parents=True, exist_ok=True)
    (output/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n', encoding='utf-8')
    binary_hash = hashlib.sha256(renderer.read_bytes()).hexdigest()
    report = dict(schemaVersion=1, passed=False, generatedOnly=args.generate_only,
                  nativeValidationPerformed=False, renderer=str(renderer), rendererSha256=binary_hash,
                  captures=[], checks=[], unsupportedDisplays=[], generatedCases=[])

    def save():
        report.update(captureCount=len(report['captures']), referencePairCount=len(report['checks']),
                      negativeControlCount=len(report['checks']))
        (output/'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')

    save()
    try:
        for case in args.case or CASES:
            fixtures, models = {}, []
            for frame in ('generated', 'analytical', 'wrong-uv0'):
                path = output/f'{case}-{frame}.gltf'
                models.append(write_fixture(path, case, frame))
                fixtures[frame] = path
            verify_source_order(models)
            report['generatedCases'].append(dict(case=case, sourceVertexCount=3, sourceIndices=list(INDICES),
                                                  analyticalTangent=list(TANGENT), wrongUv0Tangent=list(WRONG_UV0_TANGENT),
                                                  fixtures={k: str(v) for k, v in fixtures.items()}))
            save()
            if args.generate_only:
                continue
            for technique in args.technique or TECHNIQUES:
                for display in args.display or ('lit', 'normals'):
                    if technique == 'forward-raster' and display == 'normals':
                        report['unsupportedDisplays'].append(dict(case=case, technique=technique, display=display,
                            reason='ForwardRasterTechnique does not advertise the G-buffer normal diagnostic'))
                        save()
                        continue
                    captures = {}
                    for frame, fixture in fixtures.items():
                        name = f'{case}-{technique}-{display}-{frame}'
                        captures[frame] = capture(renderer, output, fixture, name, technique, display, args.timeout, report['captures'])
                        report['nativeValidationPerformed'] = True
                        save()
                        check_frame_state(captures[frame], case, frame)
                    result = dict(case=case, technique=technique, display=display,
                                  **check_pixels(captures['generated'], captures['analytical'], captures['wrong-uv0']))
                    report['checks'].append(result)
                    save()
                    print('PASS', json.dumps(result), flush=True)
        require(hashlib.sha256(renderer.read_bytes()).hexdigest() == binary_hash, 'Renderer changed during native regression')
        require(args.generate_only or report['checks'], 'No supported native technique/display combination requested')
        report['passed'] = None if args.generate_only else True
    except Exception as error:
        report['failure'] = str(error)
        raise
    finally:
        save()
    if args.generate_only:
        print(f'GENERATED: {len(report["generatedCases"])} cases; native validation not run; {output}')
    else:
        print(f'PASS: {report["referencePairCount"]} analytical pairs, {report["negativeControlCount"]} negative controls, '
              f'{report["captureCount"]} native captures; {output}')


if __name__ == '__main__':
    main()
