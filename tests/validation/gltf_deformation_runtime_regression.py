"""Opt-in native deformation regression against independently baked glTF meshes.

Every asset (including its binary buffers) is generated here. No downloaded test
assets or renderer-side deformation evaluator are used by the numerical oracle.
Requires NumPy, Pillow, a Vulkan display/runtime, and a built native renderer.
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

CAPTURES = []


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def unit(values):
    values = np.asarray(values, dtype=np.float64)
    return values / np.linalg.norm(values, axis=-1, keepdims=True)


def f32(values):
    return np.asarray(values, dtype=np.float32).astype(np.float64)


def rotation(axis, angle):
    result = np.eye(4)
    a, b = ((1, 2), (2, 0), (0, 1))[axis]
    result[a, a] = result[b, b] = math.cos(angle)
    result[a, b], result[b, a] = -math.sin(angle), math.sin(angle)
    return result


def transform(translation=(0, 0, 0), scale=(1, 1, 1), rotate=None):
    result = np.eye(4)
    result[:3, 3] = translation
    if rotate is not None:
        result[:3, :3] = rotate[:3, :3]
    result[:3, :3] = result[:3, :3] @ np.diag(scale)
    return result


def cube(rotated=False):
    x, y, z = .35, .45, .25
    faces = [
        ((0, 0, 1), [(-x, -y, z), (x, -y, z), (x, y, z), (-x, y, z)]),
        ((0, 0, -1), [(x, -y, -z), (-x, -y, -z), (-x, y, -z), (x, y, -z)]),
        ((1, 0, 0), [(x, -y, z), (x, -y, -z), (x, y, -z), (x, y, z)]),
        ((-1, 0, 0), [(-x, -y, -z), (-x, -y, z), (-x, y, z), (-x, y, -z)]),
        ((0, 1, 0), [(-x, y, z), (x, y, z), (x, y, -z), (-x, y, -z)]),
        ((0, -1, 0), [(-x, -y, -z), (x, -y, -z), (x, -y, z), (-x, -y, z)]),
    ]
    positions = np.array([v for _, vs in faces for v in vs])
    normals = np.array([n for n, vs in faces for _ in vs])
    tangents = np.array([unit(np.subtract(vs[1], vs[0])) for _, vs in faces for _ in vs])
    if rotated:
        r = (rotation(1, .35) @ rotation(2, .2))[:3, :3]
        positions, normals, tangents = (values @ r.T for values in (positions, normals, tangents))
    tangents = np.column_stack((tangents, np.ones(24)))
    shape = np.diag([1.35, 1.15, .8])
    targets = [dict(positions=np.tile([.65, .15, 0], (24, 1)), normals=np.zeros((24, 3)), tangents=np.zeros((24, 3))),
               dict(positions=positions @ (shape - np.eye(3)).T,
                    normals=unit(normals @ np.linalg.inv(shape)) - normals,
                    tangents=unit(tangents[:, :3] @ shape.T) - tangents[:, :3])]
    return dict(positions=f32(positions), normals=f32(normals), tangents=f32(tangents),
                uv=f32([(0, 0), (1, 0), (1, 1), (0, 1)] * 6),
                indices=np.array([face * 4 + i for face in range(6) for i in (0, 1, 2, 0, 2, 3)]),
                targets=[{key: f32(value) for key, value in target.items()} for target in targets],
                weights=[.2, .4], influences=[], material=0)


def spec_for(case):
    mesh = cube(case in ('mirror', 'combined'))
    spec = dict(meshes=[mesh], nodes=[], skins=[], tracks=[], camera=[0, .6, 6], target=[0, 0, 0])
    if case in ('fallback-flat', 'fallback-tangent'):
        flat = case == 'fallback-flat'
        p = f32([[-.6, -.5, 0], [.6, -.5, 0], [.6, .5, 0], [-.6, .5, 0]] if flat else
                [[-.6, -.5, 0], [.6, -.5, 0], [-.5, .6, 0]])
        count = len(p)
        uv = f32([[0, 0], [1, 0], [1, 1], [0, 1]] if flat else [[0, 0], [1, 0], [0, 1]])
        a, b = np.zeros((count, 3)), np.zeros((count, 3))
        a[:] = [.45, .12, 0]
        a[2] += [0, 0, .9] if flat else [.8, .2, .5]
        b[0], b[1] = [.2, 0, .4], [0, .1, -.2]
        if flat:
            b[3] = [0, 0, -.3]
        mesh = dict(positions=p, normals=f32([[0, 0, 1]]*count), tangents=f32([[0, 1, 0, -1]]*count),
                    uv=uv, uv1=uv[:, ::-1].copy(), indices=np.array([0, 1, 2, 0, 2, 3] if flat else [0, 1, 2]),
                    targets=[dict(positions=f32(delta), normals=np.zeros((count, 3)),
                                  tangents=f32([[0, 3, 2]]*count)) for delta in (a, b)],
                    weights=[.3, .1], influences=[], material=0,
                    omitNormal=flat, omitTangent=not flat, frameUv=0 if flat else 1)
        spec['meshes'] = [mesh]
        spec['camera'], spec['target'] = [0, .4, 4], [0, 0, 0]
        spec['nodes'] = [dict(name=case, mesh=0, weights=[-.2, .6])]
        spec['tracks'] = [(0, 'weights', [-.2, .6], [.8, -.4])]
        if flat:
            mesh['influences'] = [(np.zeros((count, 4), dtype=int), np.tile([1., 0, 0, 0], (count, 1)), False)]
            spec['nodes'][0].update(skin=0, translation=[2, 0, 0])
            spec['nodes'].append(dict(name='Fallback nonuniform joint', translation=[-.2, .1, 0],
                                      rotation=[0, math.sin(.2/2), 0, math.cos(.2/2)], scale=[1.1, .8, 1.2]))
            spec['skins'] = [dict(joints=[1], inverseBind=[np.eye(4)])]
        else:
            spec['normalTextureUv'] = 1
    elif case == 'defaults':
        spec['nodes'] = [dict(name='Mesh defaults', mesh=0, translation=[-1.4, 0, 0]),
                         dict(name='Node override', mesh=0, translation=[0, 0, 0], weights=[.8, .1]),
                         dict(name='Zero override', mesh=0, translation=[1.3, 0, 0], weights=[0, 0])]
    elif case == 'morph':
        spec['nodes'] = [dict(name='Morph animated', mesh=0, weights=[0, 0])]
        spec['tracks'] = [(0, 'weights', [0, 0], [1, .65])]
    elif case == 'skin':
        mesh['targets'] = []
        mesh['weights'] = []
        mesh['influences'] = [(np.tile([0, 1, 0, 0], (24, 1)), np.tile([.35, .65, 0, 0], (24, 1)), False)]
        parent = transform((-.5, 0, 0), rotate=rotation(2, .35))
        child = parent @ transform((.5, 0, 0))
        spec['nodes'] = [dict(name='Skin owner offset', mesh=0, skin=0, translation=[3, 0, 0]),
                         dict(name='Nested root joint', matrix=parent.T.ravel().tolist(), children=[2]),
                         dict(name='Nested moving joint', translation=[.5, 0, 0])]
        spec['skins'] = [dict(joints=[1, 2], inverseBind=[np.linalg.inv(parent), np.linalg.inv(child)])]
        spec['tracks'] = [(2, 'translation', [.5, 0, 0], [1.4, .45, 0])]
    elif case == 'shared':
        mesh['influences'] = [(np.zeros((24, 4), dtype=int), np.tile([64, 0, 0, 0], (24, 1)), True),
                              (np.tile([1, 0, 0, 0], (24, 1)), np.tile([191, 0, 0, 0], (24, 1)), True)]
        spec['nodes'] = [dict(name='Shared skin A', mesh=0, skin=0, translation=[4, 0, 0], weights=[.1, .2]),
                         dict(name='Shared skin B', mesh=0, skin=1, translation=[-4, 0, 0], weights=[.8, .3]),
                         dict(name='Joint A root', translation=[-1.4, 0, 0], children=[4]),
                         dict(name='Joint B root', translation=[1.1, 0, 0], children=[5]),
                         dict(name='Joint A child', translation=[0, 0, 0]),
                         dict(name='Joint B child', translation=[0, 0, 0])]
        spec['skins'] = [dict(joints=[2, 4], inverseBind=[np.eye(4)] * 2),
                         dict(joints=[3, 5], inverseBind=[np.eye(4)] * 2)]
        spec['tracks'] = [(4, 'translation', [0, 0, 0], [.3, .4, 0]),
                          (5, 'translation', [0, 0, 0], [-.2, -.4, 0]),
                          (0, 'weights', [.1, .2], [.7, .4]), (1, 'weights', [.8, .3], [.2, -.3])]
    elif case in ('mirror', 'combined', 'ray', 'ray-mirror'):
        if case == 'ray-mirror':
            # An open, single-sided front quad catches a wrong TLAS facing flag;
            # a closed cube could still occlude the ray with its exit face.
            for key in ('positions', 'normals', 'tangents', 'uv'):
                mesh[key] = mesh[key][:4]
            for target in mesh['targets']:
                for key in target:
                    target[key] = target[key][:4]
            mesh['indices'] = np.array([0, 1, 2, 0, 2, 3])
        mesh['influences'] = [(np.zeros((len(mesh['positions']), 4), dtype=int), np.tile([1., 0, 0, 0], (len(mesh['positions']), 1)), False)]
        owner = transform((2, 0, 0), (.8, 1.3, .9))
        mirrored = case in ('mirror', 'ray-mirror')
        joint = transform((-.1, .1, 0), (-1.3, .75, 1.1) if mirrored else (1.3, .75, 1.1), rotation(1, .25))
        spec['nodes'] = [dict(name=case + ' owner', mesh=0, skin=0, matrix=owner.T.ravel().tolist(), weights=[.3, .2]),
                         dict(name=case + ' joint', translation=joint[:3, 3].tolist(),
                              rotation=[0, math.sin(.25 / 2), 0, math.cos(.25 / 2)],
                              scale=[-1.3 if mirrored else 1.3, .75, 1.1])]
        spec['skins'] = [dict(joints=[1], inverseBind=[np.eye(4)])]
        spec['tracks'] = [(1, 'translation', [-.1, .1, 0], [.3, -.2, 0]),
                          (0, 'weights', [.3, .2], [.8, .6])]
        if case.startswith('ray'):
            floor = dict(positions=f32([[-4, -.7, 3], [4, -.7, 3], [4, -.7, -3], [-4, -.7, -3]]),
                         normals=f32([[0, 1, 0]]*4), tangents=f32([[1, 0, 0, 1]]*4),
                         uv=f32([[0, 0], [1, 0], [1, 1], [0, 1]]), indices=np.array([0, 1, 2, 0, 2, 3]),
                         targets=[], weights=[], influences=[], material=1)
            spec['meshes'].append(floor)
            # Single-sided casters face the light. Visibility rays travel from
            # the floor toward +Z, opposite the raster light-camera direction.
            light_position = [2, 3, 3]
            spec['nodes'].extend([dict(name='Static shadow receiver', mesh=1),
                                  dict(name='Authored shadow light', translation=light_position,
                                       extensions={'KHR_lights_punctual': dict(light=0)})])
            spec['lights'] = [dict(type='point', color=[1, 1, 1], intensity=120, range=20)]
            spec['camera'], spec['target'] = [0, 2, 6], [0, -.2, 0]
    else:
        raise ValueError(case)
    return spec


def local_matrix(node):
    if 'matrix' in node:
        return np.array(node['matrix']).reshape(4, 4).T
    result = transform(node.get('translation', [0, 0, 0]), node.get('scale', [1, 1, 1]))
    if 'rotation' in node:
        x, y, z, w = node['rotation']
        result[:3, :3] = np.array([[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
                                   [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
                                   [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]]) @ np.diag(node.get('scale', [1, 1, 1]))
    return result


def evaluate(spec, time):
    nodes = copy.deepcopy(spec['nodes'])
    for node, path, before, after in spec['tracks']:
        nodes[node][path] = f32(np.array(before) * (1-time) + np.array(after) * time).tolist()
    parents = {child: parent for parent, node in enumerate(nodes) for child in node.get('children', [])}
    worlds = {}

    def world(index):
        if index not in worlds:
            worlds[index] = (world(parents[index]) if index in parents else np.eye(4)) @ local_matrix(nodes[index])
        return worlds[index]

    result = []
    for source, node in enumerate(nodes):
        if 'mesh' not in node:
            continue
        mesh = spec['meshes'][node['mesh']]
        weights = f32(node.get('weights', mesh['weights']))
        p, n, t = (mesh[key].copy() for key in ('positions', 'normals', 'tangents'))
        for weight, target in zip(weights, mesh['targets']):
            p += weight * target['positions']
            if not mesh.get('omitNormal'):
                n += weight * target['normals']
            if not mesh.get('omitNormal') and not mesh.get('omitTangent'):
                t[:, :3] += weight * target['tangents']
        if mesh.get('omitNormal') or mesh.get('omitTangent'):
            corners = mesh['indices']
            p, n, t = p[corners], n[corners], t[corners]
            expanded = dict(mesh)
            for key in ('positions', 'normals', 'tangents', 'uv', 'uv1'):
                if key in mesh:
                    expanded[key] = mesh[key][corners]
            expanded['indices'] = np.arange(len(corners))
            expanded['influences'] = [(j[corners], w[corners], normalized) for j, w, normalized in mesh['influences']]
            mesh = expanded
            if mesh.get('omitNormal'):
                faces = p.reshape(-1, 3, 3)
                n = np.repeat(unit(np.cross(faces[:, 1]-faces[:, 0], faces[:, 2]-faces[:, 0])), 3, axis=0)
            n = unit(n)
            uv = mesh['uv1'] if mesh['frameUv'] == 1 else mesh['uv']
            for triangle in mesh['indices'].reshape(-1, 3):
                a, b, c = triangle
                e1, e2, d1, d2 = p[b]-p[a], p[c]-p[a], uv[b]-uv[a], uv[c]-uv[a]
                determinant = d1[0]*d2[1]-d1[1]*d2[0]
                require(abs(determinant) > 1e-8, 'Analytical fallback fixture requires nondegenerate UVs')
                tangent = (e1*d2[1]-e2*d1[1])/determinant
                bitangent = (e2*d1[0]-e1*d2[0])/determinant
                for vertex in triangle:
                    projected = unit(tangent-n[vertex]*np.dot(n[vertex], tangent))
                    t[vertex] = [*projected, np.sign(np.dot(np.cross(n[vertex], projected), bitangent))]
        sign = np.ones(len(p))
        if 'skin' in node:
            skin = spec['skins'][node['skin']]
            palette = [np.linalg.inv(world(source)) @ world(joint) @ f32(bind)
                       for joint, bind in zip(skin['joints'], skin['inverseBind'])]
            sums = np.zeros((len(p), 4, 4))
            total = np.zeros(len(p))
            for joints, influences, normalized in mesh['influences']:
                influences = f32(influences / 255 if normalized else influences)
                for v in range(len(p)):
                    for j, weight in zip(joints[v], influences[v]):
                        if weight:
                            sums[v] += weight * palette[j]
                            total[v] += weight
            sums /= total[:, None, None]
            p = np.einsum('vij,vj->vi', sums, np.column_stack((p, np.ones(len(p)))))[:, :3]
            linear = sums[:, :3, :3]
            n = np.einsum('vji,vj->vi', np.linalg.inv(linear), n)
            t[:, :3] = np.einsum('vij,vj->vi', linear, t[:, :3])
            sign = np.sign(np.linalg.det(linear))
        n = unit(n)
        t[:, :3] = unit(t[:, :3] - n * np.sum(n*t[:, :3], axis=1)[:, None])
        t[:, 3] *= sign
        model = world(source)
        result.append(dict(sourceNodeIndex=source, sourcePrimitive=node['mesh'], weights=weights.tolist(),
                           positions=p, normals=n, tangents=t, model=model, mesh=mesh,
                           worldPositions=(np.column_stack((p, np.ones(len(p)))) @ model.T)[:, :3],
                           deformed=bool(mesh['targets'] or 'skin' in node),
                           orientation='reversed' if np.all(sign < 0) else 'preserved'))
    return result


def write_fixture(path, spec, baked=None):
    data, views, accessors = bytearray(), [], []

    def accessor(values, kind, component=5126, normalized=False):
        values = np.asarray(values)
        while len(data) % 4:
            data.append(0)
        offset = len(data)
        data.extend(values.astype({5126: '<f4', 5123: '<u2', 5121: 'u1'}[component]).tobytes())
        views.append(dict(buffer=0, byteOffset=offset, byteLength=len(data)-offset))
        result = dict(bufferView=len(views)-1, componentType=component, count=len(values), type=kind)
        if normalized:
            result['normalized'] = True
        if kind in ('VEC3', 'SCALAR') and component == 5126:
            shaped = values.reshape(len(values), -1)
            result.update(min=shaped.min(axis=0).tolist(), max=shaped.max(axis=0).tolist())
        accessors.append(result)
        return len(accessors)-1

    nodes, meshes = copy.deepcopy(spec['nodes']), []
    mesh_inputs = spec['meshes'] if baked is None else [entry['mesh'] for entry in baked]
    for index, original in enumerate(mesh_inputs):
        mesh = original if baked is None else dict(original, **{key: baked[index][key] for key in ('positions', 'normals', 'tangents')})
        attrs = {semantic: accessor(mesh[key], kind) for semantic, key, kind in
                 [('POSITION', 'positions', 'VEC3'), ('NORMAL', 'normals', 'VEC3'), ('TANGENT', 'tangents', 'VEC4'), ('TEXCOORD_0', 'uv', 'VEC2')]}
        if baked is None:
            if mesh.get('omitNormal'):
                attrs.pop('NORMAL')
            if mesh.get('omitTangent'):
                attrs.pop('TANGENT')
        if 'uv1' in mesh:
            attrs['TEXCOORD_1'] = accessor(mesh['uv1'], 'VEC2')
        primitive = dict(attributes=attrs, indices=accessor(mesh['indices'], 'SCALAR', 5123), material=mesh['material'])
        definition = dict(primitives=[primitive])
        if baked is None:
            for influence, (joints, weights, normalized) in enumerate(mesh['influences']):
                attrs['JOINTS_' + str(influence)] = accessor(joints, 'VEC4', 5121 if influence == 0 else 5123)
                attrs['WEIGHTS_' + str(influence)] = accessor(weights, 'VEC4', 5121 if normalized else 5126, normalized)
            if mesh['targets']:
                semantics = [('POSITION', 'positions')]
                if not mesh.get('omitNormal'):
                    semantics.append(('NORMAL', 'normals'))
                if not mesh.get('omitTangent'):
                    semantics.append(('TANGENT', 'tangents'))
                primitive['targets'] = [{semantic: accessor(target[key], 'VEC3') for semantic, key in semantics}
                                        for target in mesh['targets']]
                definition['weights'] = mesh['weights']
        meshes.append(definition)
    if baked is not None:
        for index, entry in enumerate(baked):
            node = nodes[entry['sourceNodeIndex']]
            node['mesh'] = index
            node.pop('skin', None)
            node.pop('weights', None)
    roots = [i for i in range(len(nodes)) if not any(i in node.get('children', []) for node in nodes)]
    model = dict(asset=dict(version='2.0', generator='Container independent deformation regression'),
                 scenes=[dict(nodes=roots)], scene=0, nodes=nodes, meshes=meshes,
                 materials=[dict(pbrMetallicRoughness=dict(baseColorFactor=[.8, .2, .08, 1], metallicFactor=0, roughnessFactor=.6))])
    if 'lights' in spec:
        model['extensionsUsed'] = ['KHR_lights_punctual']
        model['extensions'] = {'KHR_lights_punctual': dict(lights=spec['lights'])}
        model['materials'].append(dict(pbrMetallicRoughness=dict(baseColorFactor=[.65, .65, .65, 1], metallicFactor=0, roughnessFactor=.8)))
    if 'normalTextureUv' in spec:
        texture = io.BytesIO()
        Image.new('RGBA', (1, 1), (209, 51, 236, 255)).save(texture, format='PNG')
        model['images'] = [dict(uri='data:image/png;base64,'+base64.b64encode(texture.getvalue()).decode())]
        model['textures'] = [dict(source=0)]
        model['materials'][0]['normalTexture'] = dict(index=0, texCoord=spec['normalTextureUv'])
    if baked is None:
        if spec['skins']:
            model['skins'] = [dict(joints=skin['joints'], inverseBindMatrices=accessor(np.array(skin['inverseBind']).transpose(0, 2, 1).reshape(-1, 16), 'MAT4'))
                              for skin in spec['skins']]
        if spec['tracks']:
            times = accessor([0., 1.], 'SCALAR')
            samplers, channels = [], []
            for node, path_name, before, after in spec['tracks']:
                output = accessor(np.array([before, after]).reshape(-1) if path_name == 'weights' else [before, after],
                                  'SCALAR' if path_name == 'weights' else 'VEC3')
                channels.append(dict(sampler=len(samplers), target=dict(node=node, path=path_name)))
                samplers.append(dict(input=times, output=output, interpolation='LINEAR'))
            model['animations'] = [dict(name='Deformation motion', samplers=samplers, channels=channels)]
    model.update(bufferViews=views, accessors=accessors,
                 buffers=[dict(byteLength=len(data), uri='data:application/octet-stream;base64,' + base64.b64encode(data).decode())])
    path.write_text(json.dumps(model, indent=2), encoding='utf-8')


def finite_json(value):
    if isinstance(value, float):
        return math.isfinite(value)
    if isinstance(value, dict):
        return all(finite_json(v) for v in value.values())
    if isinstance(value, list):
        return all(finite_json(v) for v in value)
    return True


def capture(renderer, output, fixture, name, technique, spec, time, timeout, *, animated=True, display='lit', moving=False, ray=False, ray_mode='hard', raster_shadow=False):
    image = output / (name + '.png')
    telemetry = image.with_suffix('.telemetry.json')
    image.unlink(missing_ok=True)
    telemetry.unlink(missing_ok=True)
    command = [str(renderer), '--hidden', '--no-ui', '--validation', '--width', '640', '--height', '360',
               '--msaa', '1', '--warmup-frames', '8' if moving else '3', '--capture-frame', '9' if moving else '4',
               '--fixed-dt', '0.033333333' if moving else '0.016666667', '--display-mode', display, '--no-bloom',
               '--environment-intensity', '0' if raster_shadow else '.1' if ray else '.35', '--directional-intensity', '0' if ray else '3', '--exposure', '.5' if ray else '1',
               '--camera-position', *map(str, spec['camera']), '--camera-target', *map(str, spec['target']),
               '--camera-fov', '45', '--render-technique', technique, '--model', str(fixture), '--screenshot', str(image)]
    if animated and spec['tracks']:
        command.extend(['--animation-clip', '0', '--animation-time', str(time)])
        if not moving:
            command.append('--animation-paused')
    if moving or display == 'taa-velocity':
        command.extend(['--taa', '--taa-jitter-seed', '0'])
    if ray:
        command.extend(['--ray-shadows', ray_mode, '--no-ray-shadow-denoise'])
    if raster_shadow:
        command.extend(['--ray-shadows', 'raster', '--directional-direction', '-.45', '-1', '-.3'])
    log = image.with_suffix('.log')
    with log.open('w', encoding='utf-8') as stream:
        process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                                 timeout=timeout, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                                 creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    text = log.read_text(encoding='utf-8', errors='replace')
    require(process.returncode == 0, f'Native renderer failed ({process.returncode}): {log}')
    require(not re.search(r'VUID-|SYNC-HAZARD|Validation Error', text, re.I), f'Vulkan validation failed: {log}')
    require(image.is_file() and telemetry.is_file(), f'Missing fresh native capture: {image}')
    state = json.loads(telemetry.read_text(encoding='utf-8'))
    require(finite_json(state), f'Nonfinite telemetry: {telemetry}')
    require(state['technique'] == technique, f'Wrong technique: {telemetry}')
    record = dict(image=str(image), telemetry=str(telemetry), log=str(log), command=command, state=state)
    CAPTURES.append({key: record[key] for key in ('image', 'telemetry', 'log', 'command')})
    return record


def close(actual, expected, label, tolerance=2e-4):
    error = float(np.max(np.abs(np.asarray(actual)-np.asarray(expected))))
    require(error <= tolerance, f'{label}: max error {error:.7g}; actual={actual}, expected={np.asarray(expected).tolist()}')


def check_telemetry(record, expected, spec, *, moving=False):
    expected = [entry for entry in expected if entry['deformed']]
    state = record['state']['gltfDeformation']
    require(state['active'] and state['gpuReady'], 'Native GPU deformation is not active')
    require(state['currentBoundToGpu'] and state['previousBoundToGpu'], 'Draws did not bind both GPU deformation streams')
    require(state['instanceCount'] == len(expected), 'Wrong deformed instance count')
    require(len(state['instances']) == len(expected), 'Missing per-instance deformation telemetry')
    source_vertices = sum(len(mesh['positions']) for mesh in spec['meshes'])
    require(state['sourceVertexCount'] == source_vertices, 'Unexpected source vertex layout')
    require(state['expandedVertexCount'] == source_vertices + sum(len(entry['positions']) for entry in expected), 'Dynamic vertices were not appended per instance')
    source_indices = sum(len(mesh['indices']) for mesh in spec['meshes'])
    require(state['expandedIndexCount'] == source_indices + sum(len(entry['mesh']['indices']) for entry in expected), 'Indices were not remapped per instance')
    require(state['gpuVertexCount'] >= state['expandedVertexCount'], 'GPU stream cannot cover the dynamic vertex ranges')
    ranges = []
    for entry in expected:
        actual = next((i for i in state['instances'] if i['sourceNodeIndex'] == entry['sourceNodeIndex']), None)
        require(actual is not None, f'Missing source instance {entry["sourceNodeIndex"]}')
        close(actual['weights'], entry['weights'], 'Morph weights') if entry['weights'] else require(not actual['weights'], 'Skin-only unexpectedly has weights')
        for key, values in [('firstPosition', entry['positions'][0]), ('firstNormal', entry['normals'][0]), ('firstTangent', entry['tangents'][0]),
                            ('boundsMin', entry['positions'].min(axis=0)), ('boundsMax', entry['positions'].max(axis=0))]:
            close(actual[key], values, key)
        close(actual['modelColumns'], entry['model'].T, 'Instance world matrix')
        require(actual['boundsValid'] and actual['orientation'] == entry['orientation'], 'Incorrect deformed bounds/orientation classification')
        require(actual['vertexOffset'] >= source_vertices and actual['vertexCount'] == len(entry['positions']), 'Wrong per-instance vertex range')
        require(actual['firstIndex'] >= source_indices and actual['indexCount'] == len(entry['mesh']['indices']), 'Wrong per-instance index range')
        ranges.append((actual['vertexOffset'], actual['vertexOffset']+actual['vertexCount']))
        pick = actual['pick']
        require(pick['projected'] and pick['hit'] and pick['hitNode'] == actual['graphNode'], f'Actual CPU pick missed deformed instance: {pick}')
        close(pick['worldPosition'], pick['worldProbe'], 'CPU pick world hit', 3e-3)
    require(all(a[1] <= b[0] for a, b in zip(sorted(ranges), sorted(ranges)[1:])), 'Independent instances share output vertices')
    if not moving:
        require(state['maxPositionDelta'] < 1e-5 and not state['historyReset'], 'Paused deformation did not settle its previous stream')


def project(points, state):
    matrix = np.array(state['camera']['unjitteredViewProjColumns']).T
    clip = np.column_stack((points, np.ones(len(points)))) @ matrix.T
    ndc = clip[:, :3] / clip[:, 3:4]
    # Scene negative-height viewport maps positive NDC Y toward the image top.
    return np.column_stack(((ndc[:, 0]*.5+.5)*640, (.5-ndc[:, 1]*.5)*360))


def check_pixels(dynamic, reference, expected):
    before = np.array(Image.open(dynamic['image']).convert('RGB')).astype(float)
    after = np.array(Image.open(reference['image']).convert('RGB')).astype(float)
    require(before.shape == after.shape == (360, 640, 3), 'Unexpected image dimensions')
    mask = np.zeros((360, 640), dtype=bool)
    for entry in expected:
        screen = project(entry['worldPositions'], dynamic['state'])
        lo = np.maximum(np.floor(screen.min(axis=0)-2).astype(int), [0, 0])
        hi = np.minimum(np.ceil(screen.max(axis=0)+2).astype(int), [640, 360])
        mask[lo[1]:hi[1], lo[0]:hi[0]] = True
    require(mask.sum() > 100, 'Fixture projects outside the camera')
    require(after[mask].max()-after[mask].min() > 20, 'Reference capture has no visible geometry contrast')
    difference = np.abs(before-after)
    mean = float(difference[mask].mean())
    bad = float((difference[mask].max(axis=1) > 12).mean())
    require(mean <= 1.5 and bad <= .025, f'Deformed raster differs from independent baked geometry: MAE={mean:.5f}, bad={bad:.3%}')
    return dict(roiPixels=int(mask.sum()), meanAbsolutePixelError=mean, badPixelFraction=bad,
                imageSha256=[hashlib.sha256(Path(record['image']).read_bytes()).hexdigest() for record in (dynamic, reference)])


def linear_image(path):
    value = np.asarray(Image.open(path).convert('RGB'), dtype=np.float64)/255
    return np.where(value <= .04045, value/12.92, ((value+.055)/1.055)**2.4)


def check_velocity(moving, paused, spec):
    time = moving['state']['gltfAnimation']['time']
    require(0.05 < time < .9 and moving['state']['gltfAnimation']['playing'], 'Velocity fixture did not advance without a timeline wrap')
    current, previous = evaluate(spec, time), evaluate(spec, time-1/30)
    check_telemetry(moving, current, spec, moving=True)
    check_telemetry(paused, evaluate(spec, time), spec)
    state = moving['state']['gltfDeformation']
    require(not state['historyReset'], 'Moving deformation reset previous-pose history')
    delta = max(float(np.linalg.norm(a['positions']-b['positions'], axis=1).max())
                for a, b in zip(current, previous) if a['deformed'])
    close(state['maxPositionDelta'], delta, 'Current/previous CPU pose delta', 3e-4)
    require(delta > .005, 'Velocity fixture lacks measurable local deformation')
    moving_pixels, paused_pixels = linear_image(moving['image']), linear_image(paused['image'])
    samples = []
    camera = np.array(spec['camera'])
    for now, old in zip(current, previous):
        if not now['deformed']:
            continue
        normals = now['normals'] @ np.linalg.inv(now['model'][:3, :3])
        for triangle in now['mesh']['indices'].reshape(-1, 3):
            point = now['worldPositions'][triangle].mean(axis=0)
            if np.dot(normals[triangle].sum(axis=0), camera-point) <= 0:
                continue
            screen = project(np.array([point]), moving['state'])[0]
            old_point = old['worldPositions'][triangle].mean(axis=0)
            expected = project(np.array([old_point]), moving['state'])[0]-screen
            x, y = np.floor(screen).astype(int)
            require(2 <= x < 638 and 2 <= y < 358, 'Velocity probe lies outside the viewport')
            # The native debug view encodes previousUV-currentUV in 16-pixel units.
            observed = (np.median(moving_pixels[y-1:y+2, x-1:x+2, :2], axis=(0, 1))-.5)*16
            still = (np.median(paused_pixels[y-1:y+2, x-1:x+2, :2], axis=(0, 1))-.5)*16
            close(observed, expected, 'Native deformation velocity in pixels', .3)
            require(np.linalg.norm(observed) > .25, f'Deformation velocity is missing: {observed.tolist()}')
            require(np.linalg.norm(still) < .16, f'Paused deformation has spurious motion: {still.tolist()}')
            samples.append(dict(pixel=[int(x), int(y)], expectedPixels=expected.tolist(), observedPixels=observed.tolist(), pausedPixels=still.tolist()))
            break
    require(samples, 'No visible front triangle for velocity validation')
    return dict(animationTime=time, maxPositionDelta=state['maxPositionDelta'], samples=samples)


def check_ray_telemetry(record):
    ray = record['state']['rayShadows']
    require(ray['supported'] and ray['active'], 'Native hard ray shadows are not supported/active')
    require(ray['build']['instances'] >= 2 and ray['build']['blasBuilt'] + ray['build']['blasReused'] >= 2,
            'Ray geometry omitted the deformed caster or static receiver')
    return dict(instances=ray['build']['instances'], blasBuilt=ray['build']['blasBuilt'], blasReused=ray['build']['blasReused'])


def segment_intersections(points, destination, entries):
    """Independent segment visibility for the fixture's single-sided surfaces.

    A caster must face the destination (the light or camera), matching raster
    back-face culling. Visibility queries travel in the opposite direction.
    Skin/model mirrors change triangle cross products but retain outward
    normals through inverse transpose and the runtime facing correction.
    """
    origin = np.asarray(points)
    direction = np.asarray(destination)-origin
    # Intersections use a fractional segment parameter. Keep endpoint epsilon
    # in world units so a distant directional-light endpoint does not discard
    # every nearby caster as a near-origin hit.
    epsilon = 1e-5/np.linalg.norm(direction, axis=1)
    result = np.zeros(len(points), dtype=bool)
    for entry in entries:
        if not entry['deformed']:
            continue
        for indices in entry['mesh']['indices'].reshape(-1, 3):
            a, b, c = entry['worldPositions'][indices]
            e1, e2 = b-a, c-a
            outward = np.cross(e1, e2)
            outward *= -1 if entry['orientation'] == 'reversed' else 1
            outward *= np.sign(np.linalg.det(entry['model'][:3, :3]))
            if np.dot(outward, np.asarray(destination)-a) <= 0:
                continue
            p = np.cross(direction, e2)
            determinant = p @ e1
            with np.errstate(divide='ignore', invalid='ignore'):
                u = np.sum((origin-a)*p, axis=1)/determinant
                q = np.cross(origin-a, e1)
                v = np.sum(direction*q, axis=1)/determinant
                distance = (q @ e2)/determinant
            result |= (np.abs(determinant) > 1e-10) & (u >= 0) & (v >= 0) & (u+v <= 1) & (distance > epsilon) & (distance < 1-epsilon)
    return result


def check_ray_receiver(shadowed, clear, expected, spec, *, directional=None):
    """Require visible floor shadows at independently intersected locations."""
    state = shadowed['state']
    matrix = np.linalg.inv(np.array(state['camera']['unjitteredViewProjColumns']).T)
    yy, xx = np.mgrid[:360, :640]
    clip = np.column_stack((((xx.ravel()+.5)/640)*2-1, 1-((yy.ravel()+.5)/360)*2, np.zeros(640*360), np.ones(640*360)))
    far = clip @ matrix.T
    far = far[:, :3]/far[:, 3:4]
    camera = np.asarray(spec['camera'])
    direction = far-camera
    distance = (-.7-camera[1])/direction[:, 1]
    points = camera+direction*distance[:, None]
    floor = (distance > 0) & (np.abs(points[:, 0]) < 3.9) & (np.abs(points[:, 2]) < 2.9)
    selection = np.flatnonzero(floor)
    points = points[selection]
    exposed = ~segment_intersections(points, camera, expected)
    light = (np.asarray(directional)*1e6 if directional is not None else
             next(node['translation'] for node in spec['nodes'] if 'extensions' in node))
    blocked = segment_intersections(points, light, expected)
    shadow_pixels = selection[exposed & blocked]
    lit_pixels = selection[exposed & ~blocked]
    native = np.asarray(Image.open(shadowed['image']).convert('RGB'), dtype=float).reshape(-1, 3)
    baseline = np.asarray(Image.open(clear['image']).convert('RGB'), dtype=float).reshape(-1, 3)
    darkening = (baseline-native).mean(axis=1)
    require(len(shadow_pixels) >= 50 and len(lit_pixels) >= 500, 'Ray fixture lacks a measurable shadow and lit receiver')
    median_shadow = float(np.median(darkening[shadow_pixels]))
    lit_error = float(np.quantile(np.abs(darkening[lit_pixels]), .95))
    require(median_shadow > 8, f'Native rays do not shadow the independently predicted receiver: {median_shadow:.3f}')
    require(lit_error < 3, f'Native rays shadow an independently unoccluded receiver: {lit_error:.3f}')
    return dict(shadowPixels=len(shadow_pixels), litPixels=len(lit_pixels), medianShadowDarkening=median_shadow, litErrorP95=lit_error)


def run_directional_shadows(args, renderer, output):
    """Separate CSM/current-GPU-stream check; outside the 92-capture matrix."""
    binary_hash = hashlib.sha256(renderer.read_bytes()).hexdigest()
    results = []
    for case in args.case or ('ray', 'ray-mirror'):
        require(case in ('ray', 'ray-mirror'), 'Directional raster shadow suite requires a generated floor/caster case')
        spec = spec_for(case)
        spec.pop('lights')
        for node in spec['nodes']:
            node.pop('extensions', None)
        fixture = output/f'directional-{case}.gltf'
        write_fixture(fixture, spec)
        for time in args.time or (1.,):
            require(0 <= time <= 1, 'Fixture seek time must be in [0, 1]')
            expected = evaluate(spec, time)
            baked_fixture = output/f'directional-{case}-baked-{time:g}.gltf'
            write_fixture(baked_fixture, spec, expected)
            clear_spec = copy.deepcopy(spec)
            for entry in expected:
                if entry['deformed']:
                    for field in ('mesh', 'skin', 'weights'):
                        clear_spec['nodes'][entry['sourceNodeIndex']].pop(field, None)
            clear_fixture = output/f'directional-{case}-receiver-only-{time:g}.gltf'
            write_fixture(clear_fixture, clear_spec, [entry for entry in expected if not entry['deformed']])
            if args.generate_only:
                continue
            for technique in args.technique or ('deferred-raster', 'forward-raster'):
                name = f'{technique}-directional-{case}-{time:g}'
                dynamic = capture(renderer, output, fixture, name, technique, spec, time, args.timeout, raster_shadow=True)
                check_telemetry(dynamic, expected, spec)
                state = dynamic['state']
                require(not state['rayShadows']['active'], 'Directional raster test unexpectedly enabled ray shadows')
                require(any(p['active'] and p['name'].startswith('ShadowCascade') for p in state['passes']), 'Directional raster shadow passes did not run')
                close(state['lighting']['directionalDirection'], unit([-.45, -1, -.3]), 'Directional shadow light direction')
                reference = capture(renderer, output, baked_fixture, name+'-baked', technique, spec, time, args.timeout, animated=False, raster_shadow=True)
                clear = capture(renderer, output, clear_fixture, name+'-unshadowed', technique, spec, time, args.timeout, animated=False, raster_shadow=True)
                result = dict(case=case, time=time, technique=technique, display='lit',
                              **check_pixels(dynamic, reference, expected),
                              rasterReceiver=check_ray_receiver(dynamic, clear, expected, spec, directional=[.45, 1, .3]))
                results.append(result)
                report = dict(renderer=str(renderer), rendererSha256=binary_hash, suite='raster-shadows',
                              referencePairCount=len(results), rasterReceiverCheckCount=len(results),
                              captureCount=len(CAPTURES), validationErrors=0, results=results, captures=CAPTURES)
                (output/'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
                print('PASS', json.dumps(result), flush=True)
    require(hashlib.sha256(renderer.read_bytes()).hexdigest() == binary_hash, 'Renderer binary changed during native validation')
    print(f'PASS: {len(results)} directional raster shadow checks, {len(CAPTURES)} native captures; artifacts: {output}')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--technique', action='append', choices=('deferred-raster', 'forward-raster'))
    parser.add_argument('--case', action='append', choices=('defaults', 'morph', 'skin', 'shared', 'mirror', 'combined', 'ray', 'ray-mirror', 'fallback-flat', 'fallback-tangent'))
    parser.add_argument('--suite', choices=('geometry', 'velocity', 'rays', 'raster-shadows', 'fallback', 'all'), default='all')
    parser.add_argument('--time', action='append', type=float)
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    require(renderer.is_file(), f'Renderer does not exist: {renderer}')
    output.mkdir(parents=True, exist_ok=True)
    (output/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n', encoding='utf-8')
    if args.suite == 'raster-shadows':
        return run_directional_shadows(args, renderer, output)
    binary_hash = hashlib.sha256(renderer.read_bytes()).hexdigest()
    results = []

    def save_results():
        report = dict(renderer=str(renderer), rendererSha256=binary_hash, suite=args.suite,
                      referencePairCount=sum('imageSha256' in result for result in results),
                      velocityCheckCount=sum(result['display'] == 'taa-velocity' for result in results),
                      rayReceiverCheckCount=sum('rayReceiver' in result for result in results),
                      captureCount=len(CAPTURES), validationErrors=0, results=results, captures=CAPTURES)
        (output/'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')

    cases = args.case or (('fallback-flat', 'fallback-tangent') if args.suite == 'fallback' else
                          ('ray', 'ray-mirror') if args.suite == 'rays' else ('morph', 'skin', 'combined') if args.suite == 'velocity'
                          else ('defaults', 'morph', 'skin', 'shared', 'mirror', 'combined', 'ray', 'ray-mirror') if args.suite == 'all'
                          else ('defaults', 'morph', 'skin', 'shared', 'mirror', 'combined'))
    for case in cases:
        spec = spec_for(case)
        fixture = output/(case+'.gltf')
        write_fixture(fixture, spec)
        ray = case.startswith('ray')
        for time in args.time or ((.5,) if case.startswith('fallback') else (0,) if case == 'defaults' else (0., 1.)):
            require(0 <= time <= 1, 'Fixture seek time must be in [0, 1]')
            expected = evaluate(spec, time)
            reference_fixture = output/f'{case}-baked-{time:g}.gltf'
            write_fixture(reference_fixture, spec, expected)
            if args.generate_only:
                continue
            if args.suite == 'velocity' or (args.suite == 'rays' and not ray):
                continue
            for technique in args.technique or ('deferred-raster', 'forward-raster'):
                name = f'{technique}-{case}-{time:g}'
                dynamic = capture(renderer, output, fixture, name, technique, spec, time, args.timeout, ray=ray)
                check_telemetry(dynamic, expected, spec)
                reference = capture(renderer, output, reference_fixture, name+'-baked', technique, spec, time, args.timeout, animated=False, ray=ray)
                require(not reference['state']['gltfDeformation']['active'], 'Baked fixture unexpectedly deforms')
                result = dict(case=case, time=time, technique=technique, display='lit', **check_pixels(dynamic, reference, expected))
                if ray:
                    result['rayGeometry'] = check_ray_telemetry(dynamic)
                    check_ray_telemetry(reference)
                    clear_spec = copy.deepcopy(spec)
                    for entry in expected:
                        if entry['deformed']:
                            node = clear_spec['nodes'][entry['sourceNodeIndex']]
                            for field in ('mesh', 'skin', 'weights'):
                                node.pop(field, None)
                    clear_fixture = output/f'{case}-receiver-only-{time:g}.gltf'
                    write_fixture(clear_fixture, clear_spec, [entry for entry in expected if not entry['deformed']])
                    clear = capture(renderer, output, clear_fixture, name+'-unshadowed', technique, spec, time, args.timeout,
                                    animated=False, ray=True, ray_mode='raster')
                    result['rayReceiver'] = check_ray_receiver(dynamic, clear, expected, spec)
                results.append(result)
                save_results()
                print('PASS', json.dumps(result), flush=True)
                # ForwardRasterTechnique does not advertise the G-buffer normal
                # diagnostic; its fallback texture would make this check vacuous.
                if ((case in ('mirror', 'combined') and time == 1) or case.startswith('fallback')) and technique == 'deferred-raster':
                    dynamic_normals = capture(renderer, output, fixture, name+'-normals', technique, spec, time, args.timeout, display='normals')
                    reference_normals = capture(renderer, output, reference_fixture, name+'-normals-baked', technique, spec, time, args.timeout, animated=False, display='normals')
                    result = dict(case=case, time=time, technique=technique, display='normals', **check_pixels(dynamic_normals, reference_normals, expected))
                    results.append(result)
                    save_results()
                    print('PASS', json.dumps(result), flush=True)
        if not args.generate_only and args.suite in ('velocity', 'fallback', 'all') and spec['tracks'] and not ray:
            for technique in args.technique or ('deferred-raster', 'forward-raster'):
                name = f'{technique}-{case}-velocity'
                moving = capture(renderer, output, fixture, name, technique, spec, 0, args.timeout, display='taa-velocity', moving=True)
                time = moving['state']['gltfAnimation']['time']
                paused = capture(renderer, output, fixture, name+'-paused', technique, spec, time, args.timeout, display='taa-velocity')
                result = dict(case=case, technique=technique, display='taa-velocity', **check_velocity(moving, paused, spec))
                results.append(result)
                save_results()
                print('PASS', json.dumps(result), flush=True)
    require(hashlib.sha256(renderer.read_bytes()).hexdigest() == binary_hash, 'Renderer binary changed during native validation')
    save_results()
    print(f'PASS: {len(results)} deformation checks, {len(CAPTURES)} native captures; artifacts: {output}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
