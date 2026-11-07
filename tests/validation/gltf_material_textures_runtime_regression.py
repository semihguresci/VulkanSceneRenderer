"""Native texture-slot transactions, sampler policies, history and portable reload.

Generation/self-check never launches the renderer. Native mode requires frozen
renderer, resident DLL/SPIR-V and transitive local Python inputs. A separate
read-only preflight proves initial source/catalog addresses before scripted edits.
Literal glTF controls are generated independently of the production planner.
"""
from __future__ import annotations

import argparse
import base64
import copy
import io
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
from gltf_material_authoring_runtime_regression import deep_contains, serialized_material_record
from gltf_merge_runtime_regression import document, immutable, portable_copy, image_pixels
from gltf_tangent_uv_runtime_regression import finite_json
from gltf_texture_formats_runtime_regression import verify_manifest
from scene_node_authoring_runtime_regression import history_cursor

TECHNIQUES = binding.TECHNIQUES
COHORTS = ('core', 'history', 'resize', 'portable', 'invalid', 'slots', 'failure')
WIDTH, HEIGHT, UNLIT, SG, TRANSFORM = 640, 360, 'KHR_materials_unlit', 'KHR_materials_pbrSpecularGlossiness', 'KHR_texture_transform'
UV0 = [[-1., -1.], [3., -1.], [3., 3.], [-1., 3.]]
UV1 = [[0., 1.], [0., 0.], [1., 0.], [1., 1.]]
REPEAT = dict(minFilter=9728, magFilter=9728, wrapS=10497, wrapT=10497)
CLAMP = dict(minFilter=9987, magFilter=9729, wrapS=33071, wrapT=33071)
MAPPING = dict(texCoord=1, offset=[.125, -.25], scale=[.5, .75], rotation=.25)
SLOTS = (
    ('baseColor', 'pbrMetallicRoughness/baseColorTexture', True, False),
    ('normal', 'normalTexture', False, False), ('occlusion', 'occlusionTexture', False, False),
    ('emissive', 'emissiveTexture', True, False),
    ('metallicRoughness', 'pbrMetallicRoughness/metallicRoughnessTexture', False, False),
    ('roughness', 'roughnessTexture', False, True), ('metalness', 'metalnessTexture', False, True),
    ('specular', 'extensions/KHR_materials_specular/specularTexture', False, False),
    ('height', 'heightTexture', False, True), ('opacity', 'opacityTexture', False, True),
    ('transmission', 'extensions/KHR_materials_transmission/transmissionTexture', False, False),
    ('specularColor', 'extensions/KHR_materials_specular/specularColorTexture', True, False),
    ('clearcoat', 'extensions/KHR_materials_clearcoat/clearcoatTexture', False, False),
    ('clearcoatRoughness', 'extensions/KHR_materials_clearcoat/clearcoatRoughnessTexture', False, False),
    ('clearcoatNormal', 'extensions/KHR_materials_clearcoat/clearcoatNormalTexture', False, False),
    ('thickness', 'extensions/KHR_materials_volume/thicknessTexture', False, False),
    ('sheenColor', 'extensions/KHR_materials_sheen/sheenColorTexture', True, False),
    ('sheenRoughness', 'extensions/KHR_materials_sheen/sheenRoughnessTexture', False, False),
    ('iridescence', 'extensions/KHR_materials_iridescence/iridescenceTexture', False, False),
    ('iridescenceThickness', 'extensions/KHR_materials_iridescence/iridescenceThicknessTexture', False, False),
    ('anisotropy', 'extensions/KHR_materials_anisotropy/anisotropyTexture', False, False),
    ('diffuseTransmission', 'extensions/KHR_materials_diffuse_transmission/diffuseTransmissionTexture', False, False),
    ('diffuseTransmissionColor', 'extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorTexture', True, False))
SLOT_INFO = {name: dict(path=path, srgb=srgb, legacy=legacy) for name, path, srgb, legacy in SLOTS}
IMAGE_NAMES = ('white', 'flat', 'checker', 'green', 'tilt')
END_COLOR = [.3, .7, .9, 1.]
PREVIEW_COLOR = [.2, .8, .3, 1.]
UNVERIFIED = [
    'Generation and self-checks are not native renderer acceptance.',
    'Source catalogs, slot rows, controller draw membership, packed hashes and VMA statistics are CPU evidence; no GPU buffer readback is claimed.',
    'Independent interior pixels cover base-color, normal, emissive and opacity fixtures. The all23-slot cohort proves source/role/mapping/sampler publication and export/reload, not all23 shader arithmetic paths.',
    'Stable source image identity and settled VMA counts during sampler changes do not alone prove that no transfer command ran. Allocation-lease and no-reupload guarantees need the separate GPU component tests.',
    'Undo/redo without file rereads after the input disappears is not exercised by this continuous-script harness; retained-resource GPU tests provide that separate proof.',
    'The fatal invalid cohort proves preceding captures plus explicit errors only. Parser-invalid requests may reject before rendering and have a separate read-only baseline. The separate failure cohort checks actual after-images/state using a capture-only expected-failure event flag against the same backend; it does not bypass or establish native GUI queue acceptance.',
    'These raster cohorts do not establish ray/DDGI, arbitrary external formats, native ImGui failure handling or reference-safe resource deletion acceptance.']


def pattern(name):
    if name == 'checker':
        result = np.empty((8, 8, 4), dtype=np.uint8)
        for y in range(8):
            for x in range(8): result[y, x] = [35, 75, 225, 64] if (x+y) % 2 == 0 else [215, 185, 185, 255]
        result[-1, -1] = [235, 35, 245, 255]
        return result
    colors = dict(white=[240, 240, 240, 255], flat=[128, 128, 255, 255], green=[30, 205, 65, 255], tilt=[191, 128, 238, 255])
    return np.tile(np.asarray(colors[name], dtype=np.uint8), (2, 2, 1))


def png(name):
    stream = io.BytesIO(); Image.fromarray(pattern(name)).save(stream, format='PNG', optimize=False)
    return stream.getvalue()


def tex(image, *, source=None, sampler=None, mapping=None):
    result = dict(image=image, source=source, sampler=copy.deepcopy(sampler or REPEAT),
        mapping=dict(texCoord=0, offset=[0., 0.], scale=[1., 1.], rotation=0.))
    if mapping: result['mapping'].update(copy.deepcopy(mapping))
    return result


def initial_slots(mode='unlit', *, right=False):
    if mode == 'physical-all' and not right: return {slot: tex('checker', source=2) for slot, *_ in SLOTS}
    result = dict(baseColor=tex('white', source=0), normal=tex('flat', source=1))
    if mode == 'sg': result.update(roughness=tex('white', source=0), specularColor=tex('white', source=0))
    return result


def state(label, frame, *, mode='unlit', slots=None, history=(0, 0), time=None, clip=None,
          width=WIDTH, height=HEIGHT, msaa=1, visual=True, resources_as=None, sampler_as=None, factor=None, pending=False):
    result=dict(label=label, frame=frame, mode=mode, slots=copy.deepcopy(slots if slots is not None else initial_slots(mode)),
        history=list(history), time=time, clip=clip, resolution=[width, height], msaa=msaa, visual=visual,
        resourcesAs=resources_as, samplerAs=sampler_as)
    if factor is not None:result['factor']=copy.deepcopy(factor)
    if pending:result['pendingGesture']=True
    return result


def edit(frame, slot, operation, **payload):
    return dict(frame=frame, materialTextureEdit=dict(sourceMaterial=0, slot=slot, operation=operation, **payload))


def change_slots(current, slot, value, *, mode='unlit'):
    result = copy.deepcopy(current)
    for name in (('roughness', 'specularColor') if mode == 'sg' and slot in ('roughness', 'specularColor') else (slot,)):
        if value is None: result.pop(name, None)
        else: result[name] = copy.deepcopy(value)
    return result


def profiles(cohort, images):
    green = tex('green'); checker = tex('checker', source=2); clamped = tex('checker', sampler=CLAMP)
    mapped = tex('checker', sampler=CLAMP, mapping=MAPPING)
    if cohort == 'core':
        a = initial_slots(); b = change_slots(a, 'baseColor', green); replaced=change_slots(a,'baseColor',tex('tilt'));c = change_slots(a, 'baseColor', checker)
        d = change_slots(a, 'baseColor', clamped); e = change_slots(a, 'baseColor', mapped); f = change_slots(a, 'baseColor', None)
        runs = [dict(name='base-color', mode='unlit', steps=[state('before',13),state('import',26,slots=b,history=(1,0)),
            state('replace',39,slots=replaced,history=(2,0)),state('existing',52,slots=c,history=(3,0)),state('sampler',65,slots=d,history=(4,0),sampler_as='existing'),
            state('mapping',78,slots=e,history=(5,0)),state('clear',91,slots=f,history=(6,0)),
            state('undo-clear',104,slots=e,history=(5,1),resources_as='mapping')],
            events=[edit(14,'baseColor','importFile',path=images['green'],sampler=REPEAT),
                edit(27,'baseColor','importFile',path=images['tilt'],sampler=REPEAT),edit(40,'baseColor','bindExisting',sourceTexture=2),edit(53,'baseColor','editSampler',sampler=CLAMP),
                edit(66,'baseColor','editMapping',mapping=MAPPING),edit(79,'baseColor','clear'),dict(frame=92,sceneHistory='undo')], save='glb',
            positive=[dict(before='before',after='import',minimum=8.),dict(before='import',after='replace',minimum=8.),dict(before='existing',after='sampler',minimum=.25)])]
        for name, mode, slot, image, minimum in [('normal-role','normal','normal','tilt',1.),
                ('emissive-role','emissive','emissive','green',8.),('opacity-role','unlit','opacity','checker',1.)]:
            original = initial_slots(mode); replacement = tex(image, source=2 if image=='checker' else None)
            changed = change_slots(original,slot,replacement)
            event = edit(14,slot,'bindExisting',sourceTexture=2) if image=='checker' else edit(14,slot,'importFile',path=images[image],sampler=REPEAT)
            runs.append(dict(name=name,mode=mode,steps=[state('before',13,mode=mode),state('edit',26,mode=mode,slots=changed,history=(1,0)),
                state('undo',39,mode=mode,history=(0,1))],events=[event,dict(frame=27,sceneHistory='undo')],
                positive=[dict(before='before',after='edit',minimum=minimum)]))
        return runs
    if cohort == 'history':
        before=initial_slots(); imported=change_slots(before,'baseColor',green)
        sampler=change_slots(before,'baseColor',tex('green',sampler=CLAMP))
        values=[('before',13,before,(0,0),.25,0,None,None),('import',26,imported,(1,0),.25,0,None,None),
            ('seek',39,imported,(1,0),1.,0,None,None),('undo',52,before,(0,1),1.,0,None,None),
            ('redo',65,imported,(1,0),1.,0,'import',None),('rest',78,imported,(1,0),None,-1,'import',None),
            ('undo-rest',91,before,(0,1),None,-1,None,None),('redo-rest',104,imported,(1,0),None,-1,'import',None),
            ('reselect',117,imported,(1,0),.5,0,'import',None),('sampler',130,sampler,(2,0),.5,0,None,'reselect'),
            ('seek-sampler',143,sampler,(2,0),.75,0,'sampler',None),('undo-sampler',156,imported,(1,1),.75,0,'import',None),
            ('redo-sampler',169,sampler,(2,0),.75,0,'sampler',None)]
        return [dict(name='current-clock-history',mode='unlit',animated=True,
            steps=[state(n,f,slots=s,history=h,time=t,clip=c,resources_as=r,sampler_as=p) for n,f,s,h,t,c,r,p in values],
            events=[edit(14,'baseColor','importFile',path=images['green'],sampler=REPEAT),dict(frame=27,animationTime=1.),
                dict(frame=40,sceneHistory='undo'),dict(frame=53,sceneHistory='redo'),dict(frame=66,animationClip=-1),
                dict(frame=79,sceneHistory='undo'),dict(frame=92,sceneHistory='redo'),dict(frame=105,animationClip=0,animationTime=.5),
                edit(118,'baseColor','editSampler',sampler=CLAMP),dict(frame=131,animationTime=.75),
                dict(frame=144,sceneHistory='undo'),dict(frame=157,sceneHistory='redo')],save='glb',
            positive=[dict(before='before',after='import',minimum=8.)])]
    if cohort == 'resize':
        a=initial_slots(); b=change_slots(a,'baseColor',green);c=change_slots(a,'baseColor',checker)
        d=change_slots(a,'baseColor',clamped);e=change_slots(a,'baseColor',mapped)
        stages=[state('before',13),state('import',26,slots=b,history=(1,0)),state('checker',39,slots=c,history=(2,0))]
        for n,f,s,h,w,he,ms,r,p in [('resize',52,c,(2,0),800,450,1,None,None),('msaa',65,c,(2,0),800,450,4,None,None),
                ('sampler',78,d,(3,0),800,450,4,None,'msaa'),('mapping',91,e,(4,0),800,450,4,None,None),
                ('undo-map',104,d,(3,1),800,450,4,'sampler',None),('undo-sampler',117,c,(2,2),800,450,4,'msaa',None),
                ('redo-sampler',130,d,(3,1),800,450,4,'sampler',None),('redo-map',143,e,(4,0),800,450,4,'mapping',None),
                ('restore-size',156,e,(4,0),WIDTH,HEIGHT,4,'mapping',None),('restore-msaa',169,e,(4,0),WIDTH,HEIGHT,1,'mapping',None)]:
            stages.append(state(n,f,slots=s,history=h,width=w,height=he,msaa=ms,resources_as=r,sampler_as=p))
        return [dict(name='resize-msaa',mode='unlit',steps=stages,events=[edit(14,'baseColor','importFile',path=images['green'],sampler=REPEAT),
            edit(27,'baseColor','bindExisting',sourceTexture=2),dict(frame=40,resize=[800,450]),dict(frame=53,samples=4),
            edit(66,'baseColor','editSampler',sampler=CLAMP),edit(79,'baseColor','editMapping',mapping=MAPPING),
            *[dict(frame=f,sceneHistory=op) for f,op in [(92,'undo'),(105,'undo'),(118,'redo'),(131,'redo')]],
            dict(frame=144,resize=[WIDTH,HEIGHT]),dict(frame=157,samples=1)],positive=[dict(before='before',after='import',minimum=8.)])]
    if cohort == 'portable':
        a=initial_slots();b=change_slots(a,'baseColor',green);c=change_slots(a,'baseColor',checker)
        d=change_slots(a,'baseColor',clamped);e=change_slots(a,'baseColor',mapped);f=change_slots(e,'opacity',checker)
        steps=[state('before',13),state('import',26,slots=b,history=(1,0)),state('checker',39,slots=c,history=(2,0)),
            state('sampler',52,slots=d,history=(3,0),sampler_as='checker'),state('mapping',65,slots=e,history=(4,0)),
            state('opacity',78,slots=f,history=(5,0))]
        events=[edit(14,'baseColor','importFile',path=images['green'],sampler=REPEAT),edit(27,'baseColor','bindExisting',sourceTexture=2),
            edit(40,'baseColor','editSampler',sampler=CLAMP),edit(53,'baseColor','editMapping',mapping=MAPPING),
            edit(66,'opacity','bindExisting',sourceTexture=2)]
        return [dict(name='portable-'+suffix,mode='unlit',steps=copy.deepcopy(steps),events=copy.deepcopy(events),save=suffix,
            positive=[dict(before='before',after='import',minimum=8.)]) for suffix in ('glb','gltf')]
    if cohort == 'invalid':
        return [dict(name='missing-file',mode='unlit',steps=[state('before',13)],frames=26,
                    events=[edit(14,'baseColor','importFile',path=images['missing'])],rejection=r'texture.*(open|file|missing)|open.*texture'),
            dict(name='bad-image',mode='unlit',steps=[state('before',13)],frames=26,
                events=[edit(14,'normal','importFile',path=images['bad'])],rejection=r'(texture|image).*(decode|invalid|fail|unsupported)|(decode|invalid|fail|unsupported).*(texture|image|format)'),
            dict(name='unknown-source',mode='unlit',steps=[state('before',13)],frames=26,
                events=[edit(14,'baseColor','bindExisting',sourceTexture=9999)],rejection=r'texture.*(source|identity|catalog|range)|material source texture is unavailable'),
            dict(name='stale-resource-revision',mode='unlit',steps=[state('before',13)],frames=26,
                events=[edit(14,'baseColor','clear',expectedResourceRevision=18446744073709551615)],rejection=r'(catalog|revision|stale)'),
            dict(name='parser-slot',mode='unlit',steps=[],frames=26,events=[edit(14,'unknownSlot','clear')],
                rejection=r'Capture texture slot is unsupported',parserInvalid=True),
            dict(name='parser-mapping',mode='unlit',steps=[],frames=26,events=[edit(14,'normal','editMapping',mapping=dict(texCoord=2))],
                rejection=r'Capture texture mapping enum is invalid',parserInvalid=True)]
    if cohort == 'failure':
        before=state('before',13)
        preview=state('preview',26,factor=PREVIEW_COLOR,pending=True)
        after=state('after-failure',39,factor=PREVIEW_COLOR,pending=True)
        after.update(failureAs='preview',failurePattern=r'(texture|image).*(decode|invalid|fail|unsupported)|(decode|invalid|fail|unsupported).*(texture|image|format)')
        canceled=state('cancel-old-gesture',52);canceled.update(cancelAs='before',canceledPreview='after-failure')
        begin=dict(frame=14,materialEdit=dict(sourceMaterial=0,gesturePhase='begin',gestureId=701,
            values=[dict(property='pbrMetallicRoughness/baseColorFactor',value=PREVIEW_COLOR)]))
        failure=edit(27,'baseColor','importFile',path=images['bad']);failure['expectMaterialTextureFailure']=True
        cancel=dict(frame=40,materialEdit=dict(sourceMaterial=0,gesturePhase='cancel',gestureId=701))
        return [dict(name='failed-texture-retains-factor-gesture',mode='unlit',steps=[before,preview,after,canceled],events=[begin,failure,cancel],
            positive=[dict(before='before',after='preview',minimum=8.)],
            imagePairs=[dict(before='preview',after='after-failure'),dict(before='before',after='cancel-old-gesture')])]
    if cohort == 'slots':
        all_steps=[state('before',13,mode='physical',visual=True)];all_events=[];current=initial_slots('physical')
        for i,(slot,*_) in enumerate(SLOTS):
            frame=14+i*26;current=change_slots(current,slot,tex('checker',source=2),mode='physical')
            all_events.append(edit(frame,slot,'bindExisting',sourceTexture=2))
            all_steps.append(state('bind-'+slot,frame+12,mode='physical',slots=current,history=(i*2+1,0),visual=False))
            mapping=dict(texCoord=0,offset=[.25,-.125],scale=[.5,2.],rotation=.25)
            if SLOT_INFO[slot]['legacy']:mapping['channel']=dict(roughness=1,metalness=2,height=0,opacity=3)[slot]
            current=change_slots(current,slot,tex('checker',source=2,mapping=mapping),mode='physical')
            all_events.append(edit(frame+13,slot,'editMapping',mapping=mapping))
            all_steps.append(state('map-'+slot,frame+25,mode='physical',slots=current,history=(i*2+2,0),visual=False))
        all_steps[-1]['visual']=True
        clear_steps=[state('before',13,mode='physical-all',visual=True)];clear_events=[];current=initial_slots('physical-all')
        for i,(slot,*_) in enumerate(SLOTS):
            frame=14+i*13;current=change_slots(current,slot,None,mode='physical-all')
            clear_events.append(edit(frame,slot,'clear'));clear_steps.append(state('clear-'+slot,frame+12,mode='physical-all',slots=current,history=(i+1,0),visual=False))
        clear_steps[-1]['visual']=True
        sg0=initial_slots('sg');sg1=change_slots(sg0,'roughness',tex('checker',source=2),mode='sg')
        sg2=change_slots(sg1,'specularColor',tex('checker',source=2,mapping=dict(offset=[.25,-.125],scale=[.5,2.],rotation=.25)),mode='sg')
        sg3=change_slots(sg2,'roughness',None,mode='sg');sg4=change_slots(sg3,'baseColor',tex('checker',source=2),mode='sg')
        return [dict(name='all23-bind-map',mode='physical',steps=all_steps,events=all_events,save='glb',positive=[]),
            dict(name='all23-clear',mode='physical-all',steps=clear_steps,events=clear_events,save='gltf',positive=[]),
            dict(name='specular-glossiness-coupling',mode='sg',steps=[state('before',13,mode='sg'),state('coupled-bind',26,mode='sg',slots=sg1,history=(1,0)),
                state('coupled-map',39,mode='sg',slots=sg2,history=(2,0)),state('coupled-clear',52,mode='sg',slots=sg3,history=(3,0)),
                state('undo-clear',65,mode='sg',slots=sg2,history=(2,1),resources_as='coupled-map'),state('redo-clear',78,mode='sg',slots=sg3,history=(3,0)),
                state('diffuse-bind',91,mode='sg',slots=sg4,history=(4,0))],events=[edit(14,'roughness','bindExisting',sourceTexture=2),
                edit(27,'specularColor','editMapping',mapping=dict(offset=[.25,-.125],scale=[.5,2.],rotation=.25)),edit(40,'roughness','clear'),
                dict(frame=53,sceneHistory='undo'),dict(frame=66,sceneHistory='redo'),edit(79,'baseColor','bindExisting',sourceTexture=2)],save='glb',positive=[])]
    raise ValueError('Unknown cohort')


def slot_path(mode, slot):
    if mode == 'sg' and slot == 'baseColor': return 'extensions/'+SG+'/diffuseTexture'
    if mode == 'sg' and slot in ('roughness','specularColor'): return 'extensions/'+SG+'/specularGlossinessTexture'
    return SLOT_INFO[slot]['path']


def put_path(owner, path, value):
    parts=path.split('/')
    for key in parts[:-1]:owner=owner.setdefault(key,{})
    owner[parts[-1]]=value


def get_path(owner, path):
    for key in path.split('/'):
        if not isinstance(owner,dict) or key not in owner:return None
        owner=owner[key]
    return owner


def texture_info(material,mode,slot):
    # Project serialization writes the bounded legacy Material.values entries
    # inside PBR and additionalValues at the root. The former has precedence.
    if SLOT_INFO[slot]['legacy'] and not(mode=='sg' and slot=='roughness'):
        pbr=material.get('pbrMetallicRoughness',{});field=SLOT_INFO[slot]['path']
        if isinstance(pbr,dict) and field in pbr:return pbr[field]
    return get_path(material,slot_path(mode,slot))


def slot_srgb(mode,slot,*,shared=True):
    # SG RGB and glossiness alpha are sampled from the same color image view.
    # A separate legacy roughness fallback continues to use a data image view.
    return SLOT_INFO[slot]['srgb'] or mode=='sg' and slot=='roughness' and shared


def color_at(time):
    # Animation accessors are binary32, unlike literal JSON material factors.
    endpoints=np.asarray([[1.,1.,1.,1.],END_COLOR],dtype=np.float32).astype(float)
    return (endpoints[0]*(1.-time)+endpoints[1]*time).tolist()


def material_base(mode, side, *, time=None, factor=None):
    value=dict(name='Texture fixture '+side,pbrMetallicRoughness=dict(baseColorFactor=color_at(time) if time is not None and side=='left' else [1.,1.,1.,1.],
        metallicFactor=0.,roughnessFactor=.6,extras=dict(pbrOpaque=997)),emissiveFactor=[1.,1.,1.] if mode=='emissive' else [0.,0.,0.],
        alphaMode='OPAQUE',alphaCutoff=.5,doubleSided=False,
        extras=dict(textureFixture=side,integer=16777217,future=dict(imageLike=dict(index=977))))
    if mode=='emissive':value['pbrMetallicRoughness']['baseColorFactor']=[0.,0.,0.,1.]
    if mode=='unlit':value['extensions']={UNLIT:{}}
    if mode=='sg':value['extensions']={SG:dict(diffuseFactor=[1.,1.,1.,1.],specularFactor=[.4,.4,.4],glossinessFactor=.6)}
    if factor is not None and side=='left':value['pbrMetallicRoughness']['baseColorFactor']=copy.deepcopy(factor)
    return value


def info_for(slot, binding_value, index, mode):
    mapping=binding_value['mapping'];legacy=SLOT_INFO[slot]['legacy'] and not(mode=='sg' and slot=='roughness')
    result=dict(index=index,texCoord=mapping['texCoord'])
    if slot=='baseColor':result['extras']=dict(slotTag='retained base',opaqueTextureIndex=991)
    if slot=='normal':result.update(scale=0. if mode.startswith('physical') else 1.,extras=dict(normalTag='retained normal'))
    if legacy:
        result.update(offsetX=mapping['offset'][0],offsetY=mapping['offset'][1],scaleX=mapping['scale'][0],scaleY=mapping['scale'][1],rotation=mapping['rotation'])
        if 'channel' in mapping:result['channel']=mapping['channel']
    elif mapping['offset']!=[0.,0.] or mapping['scale']!=[1.,1.] or mapping['rotation']!=0.:
        result['extensions']={TRANSFORM:dict(offset=mapping['offset'],scale=mapping['scale'],rotation=mapping['rotation'])}
    return result


def write_fixture(path, images, *, mode='unlit', expected=None, animated=False):
    asset=Asset();left=expected['slots'] if expected else initial_slots(mode);right=initial_slots(mode,right=True)
    image_names=IMAGE_NAMES if expected else IMAGE_NAMES[:3]
    asset.model['images']=[dict(name=name,uri='data:image/png;base64,'+base64.b64encode(Path(images[name]).read_bytes()).decode('ascii')) for name in image_names]
    asset.model['samplers']=[copy.deepcopy(REPEAT)]
    asset.model['textures']=[dict(name='Source '+name,source=i,sampler=0) for i,name in enumerate(image_names[:3])]
    policies=[copy.deepcopy(REPEAT)];identities={(name,json.dumps(REPEAT,sort_keys=True)):i for i,name in enumerate(IMAGE_NAMES[:3])}
    def texture_index(value):
        identity=(value['image'],json.dumps(value['sampler'],sort_keys=True))
        if identity not in identities:
            if value['sampler'] not in policies:policies.append(copy.deepcopy(value['sampler']));asset.model['samplers'].append(copy.deepcopy(value['sampler']))
            identities[identity]=len(asset.model['textures'])
            asset.model['textures'].append(dict(name='Literal '+value['image'],source=image_names.index(value['image']),sampler=policies.index(value['sampler'])))
        return identities[identity]
    values=[]
    for side,slots in [('left',left),('right',right)]:
        material=material_base(mode,side,time=expected['time'] if expected else None,factor=expected.get('factor') if expected else None)
        for slot,value in slots.items():put_path(material,slot_path(mode,slot),info_for(slot,value,texture_index(value),mode))
        values.append(material)
    attrs=dict(POSITION=asset.accessor('VEC3',binding.POSITIONS),NORMAL=asset.accessor('VEC3',[[0.,0.,1.]]*4),
        TANGENT=asset.accessor('VEC4',[[1.,0.,0.,1.]]*4),TEXCOORD_0=asset.accessor('VEC2',UV0),TEXCOORD_1=asset.accessor('VEC2',UV1))
    indices=asset.accessor('SCALAR',binding.INDICES,5123,indices=True)
    asset.model['meshes']=[dict(primitives=[dict(attributes=copy.deepcopy(attrs),indices=indices,material=i,mode=4,
        extras=dict(textureGeometry='canonical unchanged'))]) for i in range(2)]
    asset.model['nodes']=[dict(name=side+' texture owner',mesh=i,translation=[x,0.,0.]) for i,(side,x) in enumerate([('Left',-1.1),('Right',1.1)])]
    asset.model['scenes']=[dict(name='Texture transaction acceptance',nodes=[0,1])];asset.model['materials']=values
    declarations=set()
    for material in values:
        declarations.update(material.get('extensions',{}))
        def visit(value):
            if isinstance(value,dict):
                declarations.update(value.get('extensions',{}))
                for child in value.values():visit(child)
            elif isinstance(value,list):
                for child in value:visit(child)
        visit(material)
    if animated:
        times=asset.accessor('SCALAR',[0.,1.]);colors=asset.accessor('VEC4',[[1.,1.,1.,1.],END_COLOR])
        asset.model['animations']=[dict(name='Retained factor curve',extras=dict(textureCurve='original'),samplers=[dict(input=times,output=colors,interpolation='LINEAR')],
            channels=[dict(sampler=0,target=dict(path='pointer',extensions={'KHR_animation_pointer':dict(pointer='/materials/0/pbrMetallicRoughness/baseColorFactor')}))])]
        declarations.add('KHR_animation_pointer')
    asset.model['extensionsUsed']=sorted(declarations);path.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='texture-fixture-') as folder:
        temporary=Path(folder)/'scene.gltf';asset.write(temporary);immutable(path,temporary.read_bytes())


def visual_key(step):
    value={k:step[k] for k in ('mode','slots','time','resolution','msaa')}
    if 'factor' in step:value['factor']=step['factor']
    return json.dumps(value,sort_keys=True)


def generate(output,cohort):
    output.mkdir(parents=True,exist_ok=True);images={name:str(output/'images'/(name+'.png')) for name in IMAGE_NAMES}
    for name,path in images.items():immutable(Path(path),png(name))
    images.update(bad=str(output/'images/bad.png'),missing=str(output/'images/missing.png'))
    immutable(Path(images['bad']),b'This is deliberately not an image.\n')
    runs=profiles(cohort,images);inputs=[Path(path) for key,path in images.items() if key!='missing'];preflights=[];sources={};controls=[];known={}
    for run in runs:
        flavor=run['mode']+('-animated' if run.get('animated') else '')
        if flavor not in sources:
            source=output/'sources'/(flavor+'.gltf');write_fixture(source,images,mode=run['mode'],animated=run.get('animated',False));sources[flavor]=str(source);inputs.append(source)
            before=state('preflight',13,mode=run['mode'],time=.25 if run.get('animated') else None,clip=0 if run.get('animated') else None)
            preflights.append(dict(name='preflight-'+flavor,source=str(source),step=before,animated=run.get('animated',False)))
        run['source']=sources[flavor]
        for step in run['steps']:
            if cohort!='invalid' and step['visual']:
                key=visual_key(step)
                if key not in known:
                    name='literal-'+str(len(controls));path=output/'controls'/(name+'.gltf')
                    write_fixture(path,images,mode=step['mode'],expected=step);inputs.append(path)
                    known[key]=name;controls.append(dict(name=name,source=str(path),step=copy.deepcopy(step)))
                step['control']=known[key]
        frames=run['frames'] if 'frames' in run else run['steps'][-1]['frame'];path=output/'scripts'/(run['name']+'.json')
        script=dict(schemaVersion=1,frames=frames,sampleFrames=[step['frame'] for step in run['steps']] or [frames],
            camera=[dict(frame=1,position=[0.,0.,6.],target=[0.,0.,0.])],events=run['events'])
        document(path,script);run['script']=str(path);inputs.append(path)
    saves=sum('save' in run for run in runs)
    captures=len(preflights)+len(controls)+sum(len(run['steps']) for run in runs)+saves
    processes=len(preflights)+len(controls)+len(runs)+saves
    comparisons=sum('control' in step for run in runs for step in run['steps'])
    positives=sum(len(run.get('positive',[]))+len(run.get('imagePairs',[])) for run in runs)
    plan=dict(schemaVersion=1,cohort=cohort,images=images,sources=sources,preflights=preflights,controls=controls,runs=runs,
        expectedCaptureCount=captures,expectedProcessCount=processes,expectedImageCheckCount=len(controls)+comparisons+saves+positives,
        expectedExportCount=saves,expectedStateCheckCount=captures,
        initialCatalog=dict(sourceMaterials=2,runtimeMaterials=4,sourceToRuntime=[2,3],archiveTextures=3,archiveImages=3,archiveSamplers=1,
            graphNodes=[1,3],runtimeBindings=[2,3],primitiveIndices=[0,1]),
        absentInputs=[images['missing']],immutableInputs=[dict(path=str(path),byteLength=path.stat().st_size,sha256=sha(path)) for path in inputs])
    document(output/'launch-plan.json',plan);return plan


def policy(row):
    return {name:row.get(name,default) for name,default in [('minFilter',9987),('magFilter',9729),('wrapS',10497),('wrapT',10497)]}


def mapping_info(info, slot, mode):
    require(isinstance(info,dict) and type(info.get('index')) is int and info['index']>=0,'Malformed texture source info')
    legacy=SLOT_INFO[slot]['legacy'] and not(mode=='sg' and slot=='roughness')
    result=dict(texCoord=info.get('texCoord',0),channel=info.get('channel',3 if slot=='diffuseTransmission' else 4),
        offset=[0.,0.],scale=[1.,1.],rotation=0.)
    if legacy:
        result.update(offset=[info.get('offsetX',0.),info.get('offsetY',0.)],scale=[info.get('scaleX',1.),info.get('scaleY',1.)],rotation=info.get('rotation',0.))
    else:
        transform=info.get('extensions',{}).get(TRANSFORM,{})
        for field in ('texCoord','offset','scale','rotation'):
            if field in transform:result[field]=transform[field]
    return result


def expect_mapping(actual, expected, slot, label):
    for field,default in [('texCoord',0),('channel',3 if slot=='diffuseTransmission' else 4),('offset',[0.,0.]),('scale',[1.,1.]),('rotation',0.)]:
        binding.close(actual[field],expected.get(field,default),label+'/'+field)


def expected_properties(mode, slots):
    return dict(transparent='opacity' in slots,alphaMasked=False,doubleSided=False,unlit=mode=='unlit',specularGlossiness=mode=='sg',heightScale=0.)


def inspect_material(actual, step, side, *, serialized=False):
    record=serialized_material_record(actual) if serialized else actual
    expected=material_base(step['mode'],side,time=step['time'],factor=step.get('factor'))
    if 'alphaMode' in step:expected['alphaMode']=step['alphaMode']
    deep_contains(record,expected,'texture material '+side)
    require(record.get('extras')==expected['extras'] and record['pbrMetallicRoughness'].get('extras')==expected['pbrMetallicRoughness']['extras'],
        'Opaque material/PBR metadata changed')
    slots=step['slots'] if side=='left' else initial_slots(step['mode'],right=True)
    for slot,*_ in SLOTS:
        info=texture_info(record,step['mode'],slot)
        # Live Material telemetry intentionally omits TinyGLTF legacy maps;
        # the independent complete slot telemetry proves those live bindings.
        if SLOT_INFO[slot]['legacy'] and not(step['mode']=='sg' and slot=='roughness') and not serialized:continue
        if slot not in slots:
            require(info is None,'Cleared texture source field survived: '+slot);continue
        require(info is not None,'Missing source texture field: '+slot)
        expect_mapping(mapping_info(info,slot,step['mode']),slots[slot]['mapping'],slot,'source texture '+slot)
        if slot=='baseColor':require(info.get('extras')==dict(slotTag='retained base',opaqueTextureIndex=991),'Base texture opaque metadata changed')
        if slot=='normal':
            require(info.get('extras')==dict(normalTag='retained normal'),'Normal texture opaque metadata changed')
            binding.close(info.get('scale',1.),0. if step['mode'].startswith('physical') else 1.,'normal scale')


def resource_catalog(state):
    value=state['gltfMaterialTextures'];require(not value['resourcesTruncated'],'Texture telemetry was truncated')
    for field in ('resourceRevision','archiveImageCount','archiveTextureCount','archiveSamplerCount','evaluatedImageCount',
            'evaluatedTextureCount','evaluatedSamplerCount','runtimeTextureCount'):
        require(type(value[field]) is int and value[field]>=int(field=='resourceRevision'),'Invalid texture catalog count/revision')
    textures={row['sourceTexture']:row for row in value['textures']}
    require(len(textures)==len(value['textures'])==value['archiveTextureCount'] and set(textures)==set(range(value['archiveTextureCount'])),
        'Texture source catalog is incomplete or ambiguous')
    for source,row in textures.items():
        require(type(row['evaluatedTexture']) is int and 0<=row['evaluatedTexture']<value['evaluatedTextureCount'] and
            type(row['image']) is int and 0<=row['image']<value['archiveImageCount'] and
            type(row['sampler']) is int and 0<=row['sampler']<value['archiveSamplerCount'],'Texture catalog has invalid source/resource references')
        require(policy(row)['minFilter'] in (9728,9729,9984,9985,9986,9987) and policy(row)['magFilter'] in (9728,9729) and
            policy(row)['wrapS'] in (10497,33071,33648) and policy(row)['wrapT'] in (10497,33071,33648),'Invalid retained sampler policy')
        for role in ('colorResource','dataResource'):
            require(row.get(role) is None or type(row[role]) is int and 0<=row[role]<value['runtimeTextureCount'],'Invalid explicit color/data resource role')
    catalog=state['gltfMaterialBindings'];source_count=catalog['sourceMaterialCount'];material_map=catalog['sourceToRuntime']
    require(type(source_count) is int and source_count>0 and len(material_map)==source_count,'Invalid source material catalog shape')
    slots={(row['sourceMaterial'],row['slot']):row for row in value['slots']}
    require(len(slots)==len(value['slots'])==source_count*len(SLOTS) and
        set(slots)=={(source,slot) for source in range(source_count) for slot,*_ in SLOTS},
        'Slot telemetry does not cover every source material and all23 roles exactly once')
    metadata={row['runtimeTexture']:row for row in value['runtimeResources']}
    count=max(1,value['runtimeTextureCount'])
    require(len(metadata)==len(value['runtimeResources'])==count and set(metadata)==set(range(count)),
        'Runtime texture metadata does not cover the actual resource array')
    require(all(type(row['samplerCount']) is int and type(row['samplerSlot']) is int and 0<=row['samplerSlot']<row['samplerCount']
        for row in metadata.values()) and len({row['samplerCount'] for row in metadata.values()})==1,
        'Runtime texture has an invalid dense sampler descriptor slot/count')
    for (source,slot),row in slots.items():
        authored_material=catalog['materials'][source]['authored'];mode='sg' if SG in authored_material.get('extensions',{}) else 'physical'
        shared=get_path(authored_material,'extensions/'+SG+'/specularGlossinessTexture') is not None or row['authoredTexture']==-1
        require(row['runtimeMaterial']==material_map[source] and type(row['srgb']) is bool and row['srgb']==slot_srgb(mode,slot,shared=shared),
            'Slot row disagrees with its source/runtime material or color/data role')
        require(type(row['texCoord']) is int and row['texCoord'] in (0,1) and type(row['channel']) is int and 0<=row['channel']<=4 and
            all(isinstance(row[field],list) and len(row[field])==2 and all(type(x) in (int,float) and math.isfinite(x) for x in row[field])
                for field in ('offset','scale')) and type(row['rotation']) in (int,float) and math.isfinite(row['rotation']),
            'Slot row has malformed UV/channel/transform metadata')
        authored,evaluated=row['authoredTexture'],row['evaluatedTexture']
        require(type(authored) is int and type(evaluated) is int,'Slot source identities are not exact integers')
        if authored==-1:
            require(evaluated==-1 and row.get('runtimeTexture') in (None,2**32-1),'Absent source slot retained an evaluated/runtime binding')
            expect_mapping(row,{},slot,'absent source slot '+slot)
        else:
            require(authored in textures and evaluated==textures[authored]['evaluatedTexture'],'Slot row has an invalid authored/evaluated source pair')
            role='colorResource' if row['srgb'] else 'dataResource'
            require(type(row.get('runtimeTexture')) is int and row['runtimeTexture'] in metadata and row['runtimeTexture']==textures[authored].get(role),
                'Slot row does not use its explicit source resource role')
        for side,index in (('authored',authored),('evaluated',evaluated)):
            material=catalog['materials'][source][side];mode='sg' if SG in material.get('extensions',{}) else 'physical'
            info=get_path(material,slot_path(mode,slot))
            if SLOT_INFO[slot]['legacy'] and not(mode=='sg' and slot=='roughness' and (info is not None or index==-1)):continue
            require((info is None and index==-1) or isinstance(info,dict) and info.get('index')==index,
                'Complete slot telemetry disagrees with its structured source record')
    for source in range(source_count):
        material=catalog['materials'][source]['authored']
        shared=get_path(material,'extensions/'+SG+'/specularGlossinessTexture')
        if shared is None:continue
        roughness,color=slots[(source,'roughness')],slots[(source,'specularColor')]
        fields=('authoredTexture','evaluatedTexture','runtimeTexture','texCoord','channel','offset','scale','rotation','srgb')
        require(all(roughness[field]==color[field] for field in fields) and roughness['srgb'] and
            type(roughness['runtimeTexture']) is int and roughness['authoredTexture']>=0,
            'Bound SG roughness/specular aliases must use the exact same source, mapping and color resource')
        a,b=textures[roughness['authoredTexture']],textures[color['authoredTexture']]
        require(a['sampler']==b['sampler'] and policy(a)==policy(b),'SG shared source sampler descriptions disagree')
    return value,textures,slots


def resources_signature(state):
    value=state['gltfMaterialTextures']
    return {key:value[key] for key in ('archiveImageCount','archiveTextureCount','archiveSamplerCount',
        'evaluatedImageCount','evaluatedTextureCount','evaluatedSamplerCount','runtimeTextureCount')}


def retained_geometry(state):
    merge,_,_=binding.bindings_and_catalog(state)
    return dict(graph=merge['graphIdentity'],nodes=merge['authoringNodes'],roots=merge['activeRoots'],
        vertices=state['scene']['primaryVertexCount'],indices=state['scene']['primaryIndexCount'])


def texture_resources(state):
    return {key:copy.deepcopy(value) for key,value in state['gltfMaterialTextures'].items() if key not in ('lastError','errorSourceMaterial')}


def history_state(state):
    result=copy.deepcopy(state['gltfMaterialVariants'])
    # Beginning a preview may create the history owner with an empty cursor;
    # this is the same zero-history state accepted by history_cursor().
    result.setdefault('undoCount',0);result.setdefault('redoCount',0)
    return result


def failure_signature(state):
    return dict(resources=texture_resources(state),materials=state['gltfMaterialBindings'],authoring=state['gltfMaterialAuthoring'],
        history=history_state(state),historyDepth=state['gltfScenes']['editHistoryDepth'],geometry=retained_geometry(state),
        vma={key:state['gltfMerge'][key] for key in ('vmaAllocationCount','vmaAllocationBytes')})


def failure_preservation(state,step,peers):
    previous=peers[step['failureAs']]['state'];error=state['gltfMaterialTextures']['lastError']
    require(type(error) is str and 0<len(error)<=4096 and re.search(step['failurePattern'],error,re.I) and
        state['gltfMaterialTextures']['errorSourceMaterial']==0,'Expected texture backend error is absent or identifies another material')
    require(previous['gltfMaterialAuthoring']['pendingGesture'] and state['gltfMaterialAuthoring']['pendingGesture'],
        'Rejected texture action finished the pre-existing factor gesture')
    require(failure_signature(state)==failure_signature(previous),
        'Rejected texture action changed materials, resources, routing, generation, publications or history')
    return dict(passed=True,preservedAs=step['failureAs'],error=error,errorSourceMaterial=0,
        pendingGesture=True,evidence='Actual AFTER telemetry compared with the factor-preview capture; same backend, capture-only expected-failure flag')


def cancel_restoration(state,step,peers):
    before=peers[step['cancelAs']]['state'];preview=peers[step['canceledPreview']]['state']
    require(texture_resources(state)==texture_resources(before) and state['gltfMaterialBindings']==before['gltfMaterialBindings'],
        'Cancel of the old factor gesture did not restore the original complete material/resource state')
    a,b=state['gltfMaterialAuthoring'],preview['gltfMaterialAuthoring']
    require(not a['pendingGesture'] and a['publications']==b['publications']+1 and a['materialRevision']>b['materialRevision'] and
        a['generationEpoch']>b['generationEpoch'],'Old factor Cancel did not publish exactly one restoring generation')
    require(history_state(state)==history_state(before) and state['gltfScenes']['editHistoryDepth']==before['gltfScenes']['editHistoryDepth'],
        'Failed texture action or old factor Cancel changed history')
    return dict(passed=True,restoredAs=step['cancelAs'],oldGestureCanceled=True,evidence='Actual post-Cancel material/resource/history records and generation counters')


def state_check(state, step, *, preflight=False, reference=False, reference_layout=None, animated=False, baseline=None, aliases=None, peers=None):
    merge,rows,catalog=binding.bindings_and_catalog(state);value,textures,slots=resource_catalog(state)
    require(state['resolution']==step['resolution'] and state['msaaSamples']==step['msaa'],'Wrong extent/MSAA state')
    if reference:
        require(reference_layout is not None and catalog['sourceMaterialCount']==reference_layout['sourceMaterialCount'] and
            merge['sourceTables']['materials']==reference_layout['sourceMaterialCount'],'Reload/control material catalog differs from the independently decoded source')
        for side,row in zip(('left','right'),rows):
            expected=reference_layout[side]
            require(row['sourceMesh']==expected['sourceMesh'] and row['sourcePrimitive']==expected['sourcePrimitive'],
                'Reload/control surface differs from its independently decoded mesh/primitive binding')
    else:
        require([r['graphNode'] for r in rows]==[1,3] and [r['primitiveIndex'] for r in rows]==[0,1] and [r['materialIndex'] for r in rows]==[2,3],
            'Fixture instance/material addresses changed; refusing to address another surface')
        require(catalog['sourceMaterialCount']==2 and catalog['runtimeMaterialCount']==4 and catalog['sourceToRuntime']==[2,3] and merge['sourceTables']['materials']==2,
            'Texture operation changed material identities/counts')
    require(not catalog['drawsTruncated'],'Actual controller draws were truncated')
    graph_nodes={row['graphNode'] for row in rows}
    draws={r['graphNode']:r for r in catalog['draws']};require(len(draws)==len(catalog['draws'])==2 and set(draws)==graph_nodes,'Actual draw membership is ambiguous')
    routes={row['node']:row for row in state['gltfMaterialVariants']['assignments']}
    require(len(routes)==len(state['gltfMaterialVariants']['assignments'])==2 and set(routes)==graph_nodes,'Graph material routing is ambiguous')
    aliases={} if aliases is None else aliases;bindings=[];sampler_slots={};sampler_policies={}
    metadata={row['runtimeTexture']:row for row in value['runtimeResources']}
    for side_index,side in enumerate(('left','right')):
        source=reference_layout[side]['sourceMaterial'] if reference_layout else side_index
        require(type(source) is int and 0<=source<catalog['sourceMaterialCount'],'Independent bound source material is outside the live catalog')
        runtime=catalog['sourceToRuntime'][source]
        expected=step['slots'] if side_index==0 else initial_slots(step['mode'],right=True)
        entry=catalog['materials'][source]
        # A reload's authored defaults come from the independently inspected
        # file. An animated save stores E at save time, while evaluation may
        # already be sampling the unchanged original curve at another time.
        authored_step={**step,'time':None}
        if reference:
            authored_step.update(factor=reference_layout[side]['baseColorFactor'],alphaMode=reference_layout[side]['alphaMode'])
        inspect_material(entry['authored'],authored_step,side)
        evaluated_step={**step,'alphaMode':reference_layout[side]['alphaMode']} if reference else step
        inspect_material(entry['evaluated'],evaluated_step,side)
        properties=expected_properties(step['mode'],expected);require(entry['materialProperties']==properties,'Texture routing/flags differ from evaluated source')
        draw=draws[rows[side_index]['graphNode']]
        route=routes[rows[side_index]['graphNode']]
        require(route['material']==runtime and all(route[key]==properties[key] for key in ('transparent','alphaMasked','doubleSided')),
            'Graph material routing differs from the evaluated texture source')
        require(draw['runtimeMaterial']==runtime and rows[side_index]['materialIndex']==runtime and draw['primitiveIndex']==rows[side_index]['primitiveIndex'] and draw['indexCount']==6 and
            draw['transparent']==properties['transparent'] and not draw['doubleSided'] and not draw['objectDoubleSided'],'Actual draw classification/binding differs')
        for slot,*_ in SLOTS:
            srgb=slot_srgb(step['mode'],slot)
            row=slots[(source,slot)];require(row['runtimeMaterial']==runtime and row['srgb']==srgb,'Wrong slot material or color/data role')
            if slot not in expected:
                require(row['authoredTexture']==row['evaluatedTexture']==-1 and row.get('runtimeTexture') in (None,2**32-1),
                    'Cleared slot retained a source/runtime binding: '+slot)
                expect_mapping(row,{},slot,'absent slot '+slot);continue
            intended=expected[slot];authored=row['authoredTexture'];require(type(authored) is int and authored in textures,'Bound slot has no authored source identity')
            texture=textures[authored];require(row['evaluatedTexture']==texture['evaluatedTexture'],'Authored/evaluated source identity map disagrees')
            role='colorResource' if srgb else 'dataResource'
            require(type(row.get('runtimeTexture')) is int and row['runtimeTexture']==texture.get(role),'Runtime slot does not use its explicit source role')
            sampler_slot=metadata[row['runtimeTexture']]['samplerSlot'];key=tuple(policy(texture).values())
            if key in sampler_slots:require(sampler_slots[key]==sampler_slot,'Identical source policies use different dense sampler descriptors')
            if sampler_slot in sampler_policies:require(sampler_policies[sampler_slot]==key,'Different source policies alias one dense sampler descriptor')
            sampler_slots[key]=sampler_slot;sampler_policies[sampler_slot]=key
            expect_mapping(row,intended['mapping'],slot,'live slot '+slot)
            require(policy(texture)==intended['sampler'],'Bound source sampler does not match the requested policy')
            if reference:
                if reference_layout:
                    layout=reference_layout[side]['slots'][slot]
                    require(authored==layout['sourceTexture'] and texture['image']==layout['image'] and texture['sampler']==layout['sampler'],
                        'Loaded source/catalog binding differs from the independently inspected literal/export file')
                else:require(texture['image']==IMAGE_NAMES.index(intended['image']),'Literal control bound a different independent source image')
            elif intended['source'] is not None:
                require(authored==intended['source'] and row['evaluatedTexture']==intended['source'],'A retained source texture was remapped or confused with a runtime handle')
                require(texture['image']==intended['source'],'Retained texture/image source identity changed')
            else:
                require(authored>=3,'A new image/sampler operation overwrote a retained source texture')
                identity=(intended['image'],json.dumps(intended['sampler'],sort_keys=True))
                actual=(authored,row['evaluatedTexture'],texture['image'],texture['sampler'],row['runtimeTexture'])
                if identity in aliases:require(aliases[identity]==actual,'Undo/redo recreated or changed an owned source/resource identity')
                else:aliases[identity]=actual
                if intended['image'] in IMAGE_NAMES[:3]:require(texture['image']==IMAGE_NAMES.index(intended['image']),'Sampler-only edit cloned/replaced the source image')
                else:
                    require(texture['image']>=3,'Image import overwrote a retained source image')
                    prior=(intended['image'],json.dumps(REPEAT,sort_keys=True))
                    if prior in aliases:require(texture['image']==aliases[prior][2],'Sampler-only edit replaced its imported source image')
            # Known structured source infos must point at the same explicit IDs.
            for key,index in [('authored',authored),('evaluated',row['evaluatedTexture'])]:
                info=texture_info(entry[key],step['mode'],slot)
                if info is not None:require(info['index']==index,'Material record and actual slot source IDs disagree')
            bindings.append(dict(sourceMaterial=source,slot=slot,authoredTexture=authored,evaluatedTexture=row['evaluatedTexture'],
                runtimeTexture=row['runtimeTexture'],image=texture['image'],sampler=texture['sampler'],samplerDescriptorSlotCPU=sampler_slot))
    if preflight:
        require(resources_signature(state)|{}==dict(archiveImageCount=3,archiveTextureCount=3,archiveSamplerCount=1,
            evaluatedImageCount=3,evaluatedTextureCount=3,evaluatedSamplerCount=1,runtimeTextureCount=value['runtimeTextureCount']),
            'Initial resource catalog layout changed before mutation')
        for index in range(3):
            require(textures[index]['evaluatedTexture']==index and textures[index]['image']==index and textures[index]['sampler']==0 and policy(textures[index])==REPEAT,
                'Initial catalog IDs/policies are not the independent fixture addresses')
    if not reference:
        history_cursor(state,*step['history'],allow_uncreated=step['history']==[0,0])
        require(state['gltfMaterialAuthoring']['pendingGesture']==step.get('pendingGesture',False),'Wrong pending factor gesture state')
    else:history_cursor(state,0,0,allow_uncreated=True)
    if step['clip'] is not None and not reference:
        player=state['gltfAnimation'];require(not player['playing'] and player['selectedClip']==step['clip'],'Texture history changed the selected animation/transport')
        if step['time'] is not None:binding.close(player['time'],step['time'],'current texture-history animation clock')
    if baseline is not None:require(retained_geometry(state)==retained_geometry(baseline),'Texture edit changed graph/geometry identities')
    if peers is not None and step['resourcesAs']:
        require(resources_signature(state)==resources_signature(peers[step['resourcesAs']]['state']),'Undo/redo or resize grew/replaced resource table counts')
    sampler_observation=None
    if peers is not None and step['samplerAs']:
        previous=peers[step['samplerAs']]['state'];old,_,_=resource_catalog(previous)
        require(value['archiveImageCount']==old['archiveImageCount'] and value['evaluatedImageCount']==old['evaluatedImageCount'],
            'Sampler-only edit added source images')
        a,b=state['gltfMerge'],previous['gltfMerge']
        require(a['vmaAllocationCount']==b['vmaAllocationCount'],'Sampler-only edit increased settled VMA allocation count')
        sampler_observation=dict(sourceImages=value['archiveImageCount'],vmaAllocationCount=a['vmaAllocationCount'],
            beforeVmaBytes=b['vmaAllocationBytes'],afterVmaBytes=a['vmaAllocationBytes'],evidence='CPU source identities and settled whole-app VMA counters; not transfer-command proof')
    failure=failure_preservation(state,step,peers) if peers is not None and 'failureAs' in step and not reference else None
    canceled=cancel_restoration(state,step,peers) if peers is not None and 'cancelAs' in step and not reference else None
    return dict(passed=True,bindings=bindings,resources=resources_signature(state),resourceRevision=value['resourceRevision'],history=step['history'],
        samplerObservation=sampler_observation,failurePreservation=failure,cancelRestoration=canceled,
        evidence='CPU source/resource catalogs and actual controller draw membership, not GPU readback')


def positive_change(before,after,minimum):
    a,regions=binding.pixels(before);b,other=binding.pixels(after);require(regions==other and a.shape==b.shape,'Positive image witness changed extent')
    delta=[float(np.abs(a[y0:y1,x0:x1]-b[y0:y1,x0:x1]).mean()) for x0,y0,x1,y1 in regions]
    require(delta[0]>minimum and delta[1]==0.,'Texture positive image witness is vacuous or changed the other material')
    return dict(passed=True,regionMeanDelta=delta,minimum=minimum,evidence='Native independent-material interior pixels')


def unchanged_image(before,after):
    a,regions=binding.pixels(before);b,other=binding.pixels(after)
    require(regions==other and a.shape==b.shape and np.array_equal(a,b),'Failed texture action or old Cancel changed the complete captured image')
    return dict(passed=True,maximumDelta=0.,evidence='Actual complete decoded native images before/after failure or old-gesture Cancel')


def inspect_export(path,step,*,animated=False,saved_pose=False):
    model,buffers=load_asset(path);require(finite_json(model),'Export contains nonfinite values')
    nodes=model['nodes'];pending=[(n,0.) for n in model['scenes'][model.get('scene',0)]['nodes']];found=[];seen=set()
    while pending:
        index,parent=pending.pop();require(index not in seen and 0<=index<len(nodes),'Export graph is invalid/cyclic');seen.add(index);node=nodes[index]
        x=parent+(node['matrix'][12] if 'matrix' in node else node.get('translation',[0.,0.,0.])[0]);pending.extend((c,x) for c in node.get('children',[]))
        if 'mesh' not in node:continue
        primitives=model['meshes'][node['mesh']]['primitives'];require(len(primitives)==1,'Export changed primitive count');primitive=primitives[0]
        found.append((x,primitive['material'],node['mesh']))
        binding.close(decode_accessor(path,primitive['attributes']['POSITION']),np.asarray(binding.POSITIONS,dtype=np.float32).astype(float),'export positions',0.)
        binding.close(decode_accessor(path,primitive['attributes']['NORMAL']),[[0.,0.,1.]]*4,'export normals',0.)
        binding.close(np.asarray(decode_accessor(path,primitive['indices'])).reshape(-1),binding.INDICES,'export indices',0.)
        for semantic,expected in [('TEXCOORD_0',UV0),('TEXCOORD_1',UV1)]:
            require(semantic in primitive['attributes'],'Export lost canonical UV set');binding.close(decode_accessor(path,primitive['attributes'][semantic]),expected,'export '+semantic,0.)
    found.sort();require(len(found)==2 and found[0][1]!=found[1][1],'Export collapsed independent material definitions')
    binding.close([row[0] for row in found],[-1.1,1.1],'export placements')
    layout=dict(sourceMaterialCount=len(model['materials']))
    for side,(_,index,mesh) in zip(('left','right'),found):
        actual=model['materials'][index]
        expected=step['slots'] if side=='left' else initial_slots(step['mode'],right=True)
        # Generated animated inputs retain their original authored rest. Scene
        # exports use the documented current-pose defaults and retain the keys.
        material_step={**step,'time':None} if animated and not saved_pose else step
        if saved_pose:
            # The legacy opacity binding promotes the converted runtime to
            # Blend. Export writes that canonical mode to preserve appearance;
            # live original A/E records remain strictly OPAQUE in these fixtures.
            material_step={**material_step,'alphaMode':'BLEND' if 'opacity' in expected else 'OPAQUE'}
        inspect_material(actual,material_step,side,serialized=True)
        defaults=serialized_material_record(actual)
        layout[side]=dict(sourceMaterial=index,sourceMesh=mesh,sourcePrimitive=0,
            baseColorFactor=copy.deepcopy(defaults['pbrMetallicRoughness']['baseColorFactor']),alphaMode=defaults['alphaMode'],slots={})
        for slot,intended in expected.items():
            info=texture_info(actual,step['mode'],slot);require(info is not None,'Export lost bound source field '+slot)
            texture=model['textures'][info['index']];source=texture['source'];sampler=model.get('samplers',[])[texture['sampler']] if 'sampler' in texture else {}
            require(policy(sampler)==intended['sampler'],'Export changed bound sampler policy')
            require(np.array_equal(image_pixels(path,model,buffers,source),pattern(intended['image'])),'Export changed/misbound image pixels for '+slot)
            layout[side]['slots'][slot]=dict(sourceTexture=info['index'],image=source,sampler=texture.get('sampler',-1))
    if animated:
        channels=[(clip,channel) for clip in model.get('animations',[]) for channel in clip['channels']]
        require(len(channels)==1,'Texture export changed original animation channels');clip,channel=channels[0]
        require(channel['target'].get('path')=='pointer' and 'node' not in channel['target'],
            'Texture animation requires the exact pointer target path without a node target')
        require(channel['target']['extensions']['KHR_animation_pointer']['pointer']==f'/materials/{found[0][1]}/pbrMetallicRoughness/baseColorFactor',
            'Texture export changed the original factor target')
        sampler=clip['samplers'][channel['sampler']];binding.close(np.asarray(decode_accessor(path,sampler['input'])).reshape(-1),[0.,1.],'export curve times',0.)
        binding.close(decode_accessor(path,sampler['output']),np.asarray([[1.,1.,1.,1.],END_COLOR],dtype=np.float32).astype(float),'export curve values',0.)
    else:require(not model.get('animations'),'Texture edit invented animation')
    return dict(passed=True,path=str(path),sha256=sha(path),boundMaterials=[row[1] for row in found],
        checkedSlots=len(step['slots'])+len(initial_slots(step['mode'],right=True)),sourceLayout=layout,
        evidence='Independent serialized bound images, samplers, mapping and canonical geometry')


def dependencies():
    directory=Path(__file__).resolve().parent;paths={Path(__file__).resolve()}
    for module in tuple(sys.modules.values()):
        source=getattr(module,'__file__',None)
        if source and Path(source).resolve().parent==directory:paths.add(Path(source).resolve())
    return {str(path):sha(path) for path in sorted(paths)}


def rejection_diagnostics(return_code,text,pattern,*,label='texture capture'):
    # Main may print an exception's plain diagnostic without an "error" prefix.
    # Keep the case-specific match and a genuine failure reason on the SAME
    # line; startup info/debug and validation warnings are never that evidence.
    benign=re.compile(r'^\s*(?:(?:\[[^\]\r\n]*\]\s*)*\[(?:trace|debug|info|warn(?:ing)?)\](?:\s|$)|(?:validation layer|trace|debug|info|warn(?:ing)?)\s*:)',re.I)
    severe=re.compile(r'^\s*(?:\[[^\]\r\n]*\]\s*)*\[(?:error|critical|fatal)\](?:\s|$)',re.I)
    reason=re.compile(r'\b(?:error|fatal|exception|failed|failure|invalid|malformed|unrecognized|unsupported|unavailable|stale|missing|denied|cannot|unable|rejected|exceeds?|exceeded)(?=\s|:|;|$|[.!?](?:\s|$))|'
        r'\bcould\s+not\b|\bnot\s+found\b|\bout\s+of\s+range\b|\b(?:catalog|revision|selection|scene|source)\s+(?:has\s+)?changed\b',re.I)
    lines=[line for line in text.splitlines() if re.search(pattern,line,re.I) and not benign.search(line) and (severe.search(line) or reason.search(line))]
    require(return_code!=0 and lines,'Texture request was not explicitly rejected: '+str(label))
    return lines


def launch(args,output,report,persist,verify,technique,name,source,steps,*,script=None,save=None,initial=None,animated=False,rejection=None):
    verify();target=output/'captures'/(technique+'-'+name);target.parent.mkdir(parents=True,exist_ok=True)
    require(not binding.artifacts(target),'Refusing to overwrite texture capture evidence')
    initial=initial or dict(resolution=[WIDTH,HEIGHT],msaa=1);width,height=initial['resolution']
    image,log=target.with_suffix('.png'),target.with_suffix('.log')
    command=[str(args.renderer),'--hidden','--no-ui','--validation','--no-bloom','--no-taa',
        '--width',str(width),'--height',str(height),'--msaa',str(initial['msaa']),'--warmup-frames','12','--capture-frame','13',
        '--fixed-dt','.016666667','--render-technique',technique,'--display-mode','lit','--exposure','.35',
        '--camera-position','0','0','6','--camera-target','0','0','0','--camera-fov','45',
        '--environment-intensity','0','--directional-intensity','3','--directional-direction','-.6','.3','-1',
        '--directional-color','.8','.7','1','--import-scale','1','--model',str(source),'--screenshot',str(image),
        '--no-ray-query','--ray-shadows','raster']
    if animated:command+=['--animation-clip','0','--animation-time',str(initial.get('time') if initial.get('time') is not None else .25),'--animation-paused']
    if script:command+=['--capture-sequence',str(script)]
    if save:
        save.parent.mkdir(parents=True,exist_ok=True);require(not save.exists(),'Refusing to overwrite texture export');command+=['--save-scene',str(save)]
    row=dict(name=name,technique=technique,command=command,log=str(log),completed=False)
    report['processes'].append(row);report['rendererExecuted']=True;persist()
    try:
        with log.open('x',encoding='utf-8') as stream:
            process=subprocess.run(command,cwd=args.renderer.parent,stdout=stream,stderr=subprocess.STDOUT,timeout=args.timeout,
                env=dict(os.environ,VK_LAYER_SETTINGS_PATH=str(output)),creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        text=log.read_text(encoding='utf-8',errors='replace');row.update(returnCode=process.returncode,completed=True,logSha256=sha(log));persist()
        require(not validation_log_error(text),'Strict Vulkan validation error: '+str(log))
        if rejection:
            errors=rejection_diagnostics(process.returncode,text,rejection,label=log)
            row.update(expectedRejection=True,rejectionLines=errors)
        else:require(process.returncode==0,'Native texture process failed: '+str(log))
        last=json.loads(script.read_text(encoding='utf-8'))['frames'] if script else 13;records={}
        for step in steps:
            frame=step['frame'];sample=image if frame==last and not rejection else image.with_name(image.stem+f'.frame-{frame:04d}.png')
            telemetry=sample.with_suffix('.telemetry.json');require(sample.is_file() and telemetry.is_file(),'Missing texture capture/telemetry')
            state_value=json.loads(telemetry.read_text(encoding='utf-8-sig'))
            require(finite_json(state_value) and state_value['technique']==technique and state_value['resolution']==step['resolution'] and
                state_value['msaaSamples']==step['msaa'] and not state_value['taa']['enabled'] and state_value['lighting']['environmentIntensity']==0 and
                any(p['active'] and p['gpuTimed'] and p.get('gpuMs',0)>0 for p in state_value['passes']),'Wrong texture capture configuration or missing GPU work')
            binding.close(state_value['lighting']['directionalIntensity'],3.,'fixed texture directional intensity')
            binding.close(state_value['lighting']['directionalColor'],[.8,.7,1.],'fixed texture directional color')
            item=dict(name=step['label'],process=name,technique=technique,frame=frame,resolution=step['resolution'],msaa=step['msaa'],
                image=str(sample),telemetry=str(telemetry),log=str(log),imageSha256=sha(sample),telemetrySha256=sha(telemetry),validationClean=True)
            report['captures'].append(item);records[step['label']]={**item,'state':state_value}
        expected_files={log.resolve(),*(Path(record[key]).resolve() for record in records.values() for key in ('image','telemetry'))}
        require({p.resolve() for p in binding.artifacts(target)}==expected_files,'Unexpected/uncounted texture capture artifact inventory')
        persist();return records
    finally:verify();persist()


def run_native(args,output,plan,report):
    require(args.renderer.is_file() and args.manifest.is_file(),'Missing renderer/frozen texture manifest')
    require(not any(output.glob('captures/*')) and not any(output.glob('exports/**/*')) and not any(output.glob('portable/**/*')),
        'Texture evidence directories must be fresh')
    if (output/'results.json').exists():
        old=json.loads((output/'results.json').read_text());require(old.get('generatedOnly') and not old.get('rendererExecuted'),'Refusing to overwrite a native texture report')
    manifest_hash=sha(args.manifest)
    def persist():
        for key in ('captures','processes','stateChecks','imageChecks','exports'):report[key+'Count']=len(report[key])
        (output/'results.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf-8')
    def runtime_files():return sorted([*args.renderer.parent.glob('*.dll'),*(args.renderer.parent/'spv_shaders').glob('*.spv')])
    def verify():
        require(sha(args.manifest)==manifest_hash,'Frozen texture manifest changed');result=verify_manifest(args.manifest)
        require((output/'vk_layer_settings.txt').read_bytes()==b'khronos_validation.validate_sync = true\n','Validation settings changed')
        require(dependencies()==report['sourceDependencies'] and sha(output/'launch-plan.json')==report['launchPlanSha256'],'Texture helper/plan changed')
        for entry in plan['immutableInputs']:
            path=Path(entry['path']);require(path.stat().st_size==entry['byteLength'] and sha(path)==entry['sha256'],'Generated texture input changed')
        require(all(not Path(path).exists() for path in plan['absentInputs']),'A deliberately missing texture input appeared')
        if 'runtimeInputs' in report:require([str(p.resolve()) for p in runtime_files()]==report['runtimeInputs'],'Resident texture DLL/SPIR-V inventory changed')
        for group in report['portableCopies']:
            require(all(sha(Path(item['path']))==item['sha256'] for item in group['files']),'Portable texture copy changed')
        for item in report['exports']:require(sha(Path(item['path']))==item['sha256'],'Previously inspected texture export changed')
        for item in report['captures']:
            require(sha(Path(item['image']))==item['imageSha256'] and sha(Path(item['telemetry']))==item['telemetrySha256'],
                'Previously recorded texture capture changed')
        for item in report['processes']:
            if item.get('completed'):require(sha(Path(item['log']))==item['logSha256'],'Previously recorded texture process log changed')
        return result
    def check(kind,technique,name,result):report[kind].append(dict(technique=technique,check=name,**result));persist()
    try:
        report['nativeManifestBefore']=verify_manifest(args.manifest)
        frozen={Path(path).resolve() for path in json.loads(args.manifest.read_text(encoding='utf-8-sig'))['files']}
        require({args.renderer,*map(Path,report['sourceDependencies'])}<=frozen,'Manifest omits renderer/transitive Python helpers')
        runtime=runtime_files();require(any(path.suffix=='.spv' for path in runtime) and {p.resolve() for p in runtime}<=frozen,'Manifest omits resident DLL/SPIR-V inputs')
        report.update(rendererSha256=sha(args.renderer),runtimeInputs=[str(path.resolve()) for path in runtime])
        settings=output/'vk_layer_settings.txt';immutable(settings,b'khronos_validation.validate_sync = true\n')
        report['validationLayerSettings']=dict(path=str(settings),sha256=sha(settings),verified=True);persist();verify()
        for technique in args.technique:
            # Every input flavor completes its read-only identity/catalog proof
            # before an edit script may address any of its initial source IDs.
            for preflight in plan['preflights']:
                step=preflight['step']
                record=launch(args,output,report,persist,verify,technique,preflight['name'],Path(preflight['source']),[step],
                    initial=step,animated=preflight['animated'])['preflight']
                check('stateChecks',technique,preflight['name'],state_check(record['state'],step,preflight=True))
            controls={}
            for control in plan['controls']:
                step={**control['step'],'label':'control','frame':13}
                contract=inspect_export(Path(control['source']),step)
                record=launch(args,output,report,persist,verify,technique,control['name'],Path(control['source']),[step],initial=step)['control']
                check('stateChecks',technique,control['name'],state_check(record['state'],step,reference=True,reference_layout=contract['sourceLayout']))
                check('imageChecks',technique,control['name']+'-visible',binding.visible_reference(record));controls[control['name']]=record
            for run in plan['runs']:
                save=output/'exports'/technique/run['name']/('scene.'+run['save']) if 'save' in run else None
                records=launch(args,output,report,persist,verify,technique,run['name'],Path(run['source']),run['steps'],script=Path(run['script']),
                    save=save,animated=run.get('animated',False),rejection=run.get('rejection'))
                baseline=records[run['steps'][0]['label']]['state'] if run['steps'] else None;aliases={}
                for step in run['steps']:
                    record=records[step['label']]
                    check('stateChecks',technique,run['name']+'/'+step['label'],state_check(record['state'],step,baseline=baseline,aliases=aliases,peers=records))
                    if 'control' in step:check('imageChecks',technique,run['name']+'/'+step['label'],binding.compare_images(record,controls[step['control']]))
                for positive in run.get('positive',[]):
                    check('imageChecks',technique,run['name']+'/'+positive['before']+'-to-'+positive['after'],
                        positive_change(records[positive['before']],records[positive['after']],positive['minimum']))
                for pair in run.get('imagePairs',[]):
                    check('imageChecks',technique,run['name']+'/'+pair['before']+'-equals-'+pair['after'],
                        unchanged_image(records[pair['before']],records[pair['after']]))
                if save:
                    last=run['steps'][-1];report['exports'].append(dict(technique=technique,**inspect_export(save,last,animated=run.get('animated',False),saved_pose=True)));persist()
                    copied,files=portable_copy(save,output/'portable'/technique/run['name']);report['portableCopies'].append(dict(files=files));persist()
                    step={**last,'label':'reload','frame':13,'history':[0,0],'resourcesAs':None,'samplerAs':None}
                    contract=inspect_export(copied,step,animated=run.get('animated',False),saved_pose=True)
                    reload=launch(args,output,report,persist,verify,technique,run['name']+'-reload',copied,[step],initial=step,
                        animated=run.get('animated',False))['reload']
                    check('stateChecks',technique,run['name']+'-reload',state_check(reload['state'],step,reference=True,
                        reference_layout=contract['sourceLayout'],animated=run.get('animated',False)))
                    check('imageChecks',technique,run['name']+'-reload',binding.compare_images(reload,records[last['label']]))
        multiplier=len(args.technique)
        for key,expected in [('captures','expectedCaptureCount'),('processes','expectedProcessCount'),('stateChecks','expectedStateCheckCount'),
                ('imageChecks','expectedImageCheckCount'),('exports','expectedExportCount')]:
            require(len(report[key])==plan[expected]*multiplier,'Incomplete native texture '+key)
        report.update(passed=True,nativeValidationPerformed=True);verify()
    except Exception as error:report['failure']=str(error);raise
    finally:
        if report.get('nativeManifestBefore'):
            try:report['nativeManifestAfter']=verify()
            except Exception as error:report.update(passed=False,afterManifestFailure=str(error));report.setdefault('failure',str(error))
        persist()
    require(report['passed'],'Native texture input provenance failed after execution')


def self_check():
    """Exercise only fixture generation and independent evidence checkers."""
    checks=[];counts={}
    def reject(name,action):
        try:action()
        except (RuntimeError,KeyError,TypeError,ValueError,IndexError):checks.append(name);return
        raise RuntimeError('Self-check accepted damaged texture evidence: '+name)
    def synthetic(step):
        # Deliberately small CPU evidence, with explicit resource roles. The
        # runtime IDs are independently allocated here, never inferred from A/E.
        pairs=[('left',step['slots']),('right',initial_slots(step['mode'],right=True))]
        identities={(name,json.dumps(REPEAT,sort_keys=True)):i for i,name in enumerate(IMAGE_NAMES[:3])}
        policies=[copy.deepcopy(REPEAT)]
        textures=[dict(sourceTexture=i,evaluatedTexture=i,image=i,sampler=0,**REPEAT,colorResource=None,dataResource=None) for i in range(3)]
        images=list(IMAGE_NAMES[:3]);slots=[];materials=[];routes=[];draws=[];nodes=[];runtime=0;metadata=[]
        for source,(side,expected) in enumerate(pairs):
            authored=material_base(step['mode'],side,factor=step.get('factor'))
            evaluated=material_base(step['mode'],side,time=step['time'],factor=step.get('factor'))
            for slot,*_ in SLOTS:
                srgb=slot_srgb(step['mode'],slot)
                row=dict(sourceMaterial=source,runtimeMaterial=source+2,slot=slot,authoredTexture=-1,evaluatedTexture=-1,runtimeTexture=None,
                    texCoord=0,channel=3 if slot=='diffuseTransmission' else 4,offset=[0.,0.],scale=[1.,1.],rotation=0.,srgb=srgb)
                if slot in expected:
                    intended=expected[slot];identity=(intended['image'],json.dumps(intended['sampler'],sort_keys=True))
                    if identity not in identities:
                        if intended['image'] not in images:images.append(intended['image'])
                        if intended['sampler'] not in policies:policies.append(copy.deepcopy(intended['sampler']))
                        index=len(textures);identities[identity]=index
                        textures.append(dict(sourceTexture=index,evaluatedTexture=index,image=images.index(intended['image']),
                            sampler=policies.index(intended['sampler']),**intended['sampler'],colorResource=None,dataResource=None))
                    index=identities[identity];texture=textures[index];role='colorResource' if srgb else 'dataResource'
                    if texture[role] is None:
                        texture[role]=runtime;metadata.append(dict(runtimeTexture=runtime,samplerSlot=texture['sampler']));runtime+=1
                    row.update(authoredTexture=index,evaluatedTexture=index,runtimeTexture=texture[role],**intended['mapping'])
                    put_path(authored,slot_path(step['mode'],slot),info_for(slot,intended,index,step['mode']))
                    put_path(evaluated,slot_path(step['mode'],slot),info_for(slot,intended,index,step['mode']))
                slots.append(row)
            props=expected_properties(step['mode'],expected);graph=1+source*2
            materials.append(dict(sourceMaterial=source,runtimeMaterial=source+2,authored=authored,evaluated=evaluated,
                materialProperties=props,uploadedMaterialHash=123+source))
            routes.append(dict(node=graph,material=source+2,**{key:props[key] for key in ('transparent','alphaMasked','doubleSided')}))
            draws.append(dict(graphNode=graph,runtimeMaterial=source+2,primitiveIndex=source,indexCount=6,
                transparent=props['transparent'],doubleSided=False,objectDoubleSided=False))
            nodes.append(dict(graphNode=graph,sourceNode=source,sourceMesh=source,sourcePrimitive=0,renderable=True,retired=False,active=True,primitiveIndex=source,materialIndex=source+2,
                worldColumns=[[1.,0.,0.,0.],[0.,1.,0.,0.],[0.,0.,1.,0.],[-1.1 if source==0 else 1.1,0.,0.,1.]]))
        for row in metadata:row['samplerCount']=len(policies)
        return dict(resolution=step['resolution'],msaaSamples=step['msaa'],scene=dict(primaryObjectCount=2,primaryVertexCount=8,primaryIndexCount=12),
            gltfMerge=dict(graphIdentity=71,authoringNodes=nodes,authoringNodesTruncated=False,graphNodeCount=2,activeRoots=[1,3],
                sourceTables=dict(materials=2,nodes=2,meshes=2),vmaAllocationCount=17,vmaAllocationBytes=2048),
            gltfMaterialBindings=dict(sourceMaterialCount=2,runtimeMaterialCount=4,sourceToRuntime=[2,3],materialsTruncated=False,
                drawsTruncated=False,draws=draws,materials=materials),gltfMaterialVariants=dict(assignments=routes,undoCount=step['history'][0],redoCount=step['history'][1]),
            gltfMaterialAuthoring=dict(pendingGesture=step.get('pendingGesture',False),publications=0,materialRevision=1,generationEpoch=1),
            gltfScenes=dict(editHistoryDepth=sum(step['history'])),
            gltfAnimation=dict(selectedClip=step['clip'],time=step['time'],playing=False),
            gltfMaterialTextures=dict(resourceRevision=1,archiveImageCount=len(images),archiveTextureCount=len(textures),archiveSamplerCount=len(policies),
                evaluatedImageCount=len(images),evaluatedTextureCount=len(textures),evaluatedSamplerCount=len(policies),runtimeTextureCount=runtime,
                resourcesTruncated=False,textures=textures,slots=slots,runtimeResources=metadata))
    with tempfile.TemporaryDirectory(prefix='material-textures-selfcheck-') as temporary:
        root=Path(temporary);plans={}
        for cohort in COHORTS:
            output=root/cohort;plan=generate(output,cohort);plans[cohort]=plan
            require(generate(output,cohort)==plan,'Generated texture plan is not byte-repeatable')
            for entry in plan['immutableInputs']:
                require(Path(entry['path']).stat().st_size==entry['byteLength'] and sha(Path(entry['path']))==entry['sha256'],'Generated input hash is wrong')
            require(all(not Path(p).exists() for p in plan['absentInputs']),'Missing-file fixture exists')
            for run in plan['runs']:
                script=json.loads(Path(run['script']).read_text());frames=[event['frame'] for event in script['events']]
                require(frames==sorted(set(frames)) and all(1<=frame<=script['frames'] for frame in frames),'Events are not strictly ordered/bounded')
                samples=[step['frame'] for step in run['steps']]
                require(samples==sorted(set(samples)) and all(1<=frame<=script['frames'] for frame in samples),'Samples are not strictly ordered/bounded')
                require(script['sampleFrames']==(samples or [script['frames']]),'Script samples do not match the declared evidence count')
                if run.get('parserInvalid'):require(not samples,'Parser-invalid request claims an in-process before capture')
                else:require(run['steps'][0]['label']=='before' and samples[0]<frames[0],'Mutation precedes its actual before capture')
            for control in plan['controls']:inspect_export(Path(control['source']),control['step'])
            for preflight in plan['preflights']:
                inspect_export(Path(preflight['source']),preflight['step'],animated=preflight['animated'])
                source,_=load_asset(Path(preflight['source']))
                require(len(source['images'])==len(source['textures'])==3 and len(source['samplers'])==1 and len(source['materials'])==2,
                    'Initial catalog is not the independently specified fixed layout')
                require([p['material'] for m in source['meshes'] for p in m['primitives']]==[0,1],
                    'Two fixture surfaces do not use independent material definitions')
            counts[cohort]={key:plan[key] for key in ('expectedCaptureCount','expectedProcessCount','expectedStateCheckCount','expectedImageCheckCount','expectedExportCount')}
            checks.append(cohort+'-immutable-complete-plan')
        all_slots=plans['slots']['runs'][0]
        require({e['materialTextureEdit']['slot'] for e in all_slots['events']}==set(SLOT_INFO) and len(all_slots['events'])==46,
            'All23 bind/mapping matrix is incomplete')
        require(len(plans['slots']['runs'][1]['events'])==23 and {e['materialTextureEdit']['slot'] for e in plans['slots']['runs'][1]['events']}==set(SLOT_INFO),
            'All23 clear matrix is incomplete')
        sg=plans['slots']['runs'][2]
        require(sg['steps'][1]['slots']['roughness']==sg['steps'][1]['slots']['specularColor'] and
            sg['steps'][2]['slots']['roughness']==sg['steps'][2]['slots']['specularColor'] and
            'roughness' not in sg['steps'][3]['slots'] and 'specularColor' not in sg['steps'][3]['slots'],
            'SG source-coupling oracle is vacuous')
        checks.extend(['all23-bind-map-clear-matrix','specular-glossiness-coupled-source'])
        sg_step=sg['steps'][2];sg_state=synthetic(sg_step);state_check(sg_state,sg_step)
        sg_rows={(row['sourceMaterial'],row['slot']):row for row in sg_state['gltfMaterialTextures']['slots']}
        roughness,color=sg_rows[(0,'roughness')],sg_rows[(0,'specularColor')]
        require(roughness['srgb'] and color['srgb'] and type(roughness['runtimeTexture']) is int and
            roughness['runtimeTexture']==color['runtimeTexture'],'SG shared color/linear-alpha oracle is vacuous')
        checks.append('sg-rgb-and-linear-alpha-share-exact-color-source-resource')
        def roughness_row(value):return next(row for row in value['gltfMaterialTextures']['slots'] if row['sourceMaterial']==0 and row['slot']=='roughness')
        def aliased_source(value):
            table=value['gltfMaterialTextures'];clone=copy.deepcopy(table['textures'][2]);clone.update(sourceTexture=3,evaluatedTexture=3)
            table['textures'].append(clone);table['archiveTextureCount']=table['evaluatedTextureCount']=4
            roughness_row(value).update(authoredTexture=3,evaluatedTexture=3)
        for label,mutate in [
                ('sg-shared-roughness-cannot-use-data-role',lambda s:roughness_row(s).update(srgb=False)),
                ('sg-bound-alias-cannot-be-null',lambda s:roughness_row(s).update(runtimeTexture=None)),
                ('sg-bound-alias-cannot-use-another-resource',lambda s:roughness_row(s).update(runtimeTexture=1)),
                ('sg-shared-alias-authored-source-must-match',lambda s:roughness_row(s).update(authoredTexture=1)),
                ('sg-shared-alias-evaluated-source-must-match',lambda s:roughness_row(s).update(evaluatedTexture=1)),
                ('sg-shared-alias-offset-must-match',lambda s:roughness_row(s).update(offset=[0.,0.])),
                ('sg-shared-alias-channel-must-match',lambda s:roughness_row(s).update(channel=3)),
                ('sg-shared-alias-source-id-cannot-be-inferred-from-dedup-resource',aliased_source)]:
            damaged=copy.deepcopy(sg_state);mutate(damaged);reject(label,lambda damaged=damaged:state_check(damaged,sg_step))
        mr_step=next(step for step in all_slots['steps'] if step['label']=='bind-roughness');mr_state=synthetic(mr_step)
        row=roughness_row(mr_state);require(not row['srgb'] and row['runtimeTexture']==mr_state['gltfMaterialTextures']['textures'][row['authoredTexture']]['dataResource'],
            'Ordinary metallic-roughness roughness lost its data role')
        checks.append('ordinary-metallic-roughness-roughness-remains-data')
        for name in IMAGE_NAMES:
            with Image.open(io.BytesIO(png(name))) as image:require(np.array_equal(np.asarray(image),pattern(name)),'Independent PNG pattern changed')
        require(len({png(name) for name in IMAGE_NAMES})==len(IMAGE_NAMES) and len(np.unique(pattern('checker').reshape(-1,4),axis=0))==3,
            'Source image controls are not contrasting')
        checks.append('independent-png-patterns')
        invalid={run['name']:run for run in plans['invalid']['runs']}
        decoder='Texture decode: unrecognized or malformed ordinary image'
        for label,case,line in [
                ('plain-decoder-diagnostic','bad-image',decoder),
                ('plain-missing-file-diagnostic','missing-file','Failed to open material texture: missing.png'),
                ('plain-unavailable-source-diagnostic','unknown-source','Chosen material texture source identity is unavailable'),
                ('plain-wrapper-unavailable-source-diagnostic','unknown-source','Material source texture is unavailable'),
                ('plain-stale-catalog-diagnostic','stale-resource-revision','The texture catalog changed; refresh before editing'),
                ('plain-unsupported-slot-diagnostic','parser-slot','Capture texture slot is unsupported'),
                ('plain-invalid-mapping-diagnostic','parser-mapping','Capture texture mapping enum is invalid'),
                ('plain-source-range-diagnostic','unknown-source','Material texture source identity is out of range'),
                ('severe-source-diagnostic','unknown-source','[2026-10-29 01:00:00] [Renderer] [error] Requested material texture source identity 9999')]:
            require(rejection_diagnostics(1,line,invalid[case]['rejection'])==[line],'Rejected a genuine case-specific diagnostic')
            checks.append(label)
        for label,case,code,text in [
                ('diagnostic-cannot-replace-nonzero-exit','bad-image',0,decoder),
                ('timestamped-info-is-not-rejection','bad-image',1,'[2026-10-29 01:00:00] [Renderer] [info] '+decoder),
                ('timestamped-debug-is-not-rejection','bad-image',1,'[2026-10-29 01:00:00] [Renderer] [debug] '+decoder),
                ('timestamped-warning-is-not-rejection','bad-image',1,'[2026-10-29 01:00:00] [Renderer] [warning] '+decoder),
                ('validation-warning-is-not-rejection','bad-image',1,'validation layer: Texture decode: malformed format warning'),
                ('benign-texture-startup-is-not-rejection','bad-image',1,'Initializing texture decode support'),
                ('startup-filename-is-not-failure-reason','missing-file',1,'Texture startup: loaded missing.png'),
                ('unrelated-failure-cannot-pair-with-texture-startup','unknown-source',1,'Texture source catalog ready\nUnrelated network failure'),
                ('source-texture-startup-cannot-pair-with-failure','unknown-source',1,'Material source texture is ready\nUnrelated network failure'),
                ('unrelated-diagnostic-is-not-request-rejection','unknown-source',1,'The model source identity is unavailable')]:
            reject(label,lambda case=case,code=code,text=text:rejection_diagnostics(code,text,invalid[case]['rejection']))
        base=state('before',13);good=synthetic(base);state_check(good,base,preflight=True,baseline=good)
        checks.append('explicit-catalog-and-all23-slot-state')
        # Export may retain fallback/diagnostic source definitions before the
        # two bound materials and may add hierarchy wrappers. Decode those
        # source bindings independently rather than assuming import-time IDs.
        literal=root/'reload-layout-original.gltf';write_fixture(literal,plans['core']['images'],expected=base)
        reference_model,_=load_asset(literal);reference_model['materials']=[{},{}]+reference_model['materials']
        for mesh in reference_model['meshes']:mesh['primitives'][0]['material']+=2
        exported=root/'reload-layout-extra-materials.gltf';document(exported,reference_model)
        layout=inspect_export(exported,base)['sourceLayout'];reloaded=copy.deepcopy(good)
        catalog=reloaded['gltfMaterialBindings'];catalog.update(sourceMaterialCount=4,runtimeMaterialCount=6,sourceToRuntime=[2,3,4,5])
        for entry in catalog['materials']:entry['sourceMaterial']+=2;entry['runtimeMaterial']+=2
        fallback=serialized_material_record({})
        catalog['materials']=[dict(sourceMaterial=i,runtimeMaterial=i+2,authored=copy.deepcopy(fallback),evaluated=copy.deepcopy(fallback),
            materialProperties=expected_properties('physical',{}),uploadedMaterialHash=999+i) for i in range(2)]+catalog['materials']
        reloaded['gltfMerge']['sourceTables']['materials']=4;reloaded['gltfMerge']['activeRoots']=[2,5]
        for row,graph in zip(reloaded['gltfMerge']['authoringNodes'],(2,5)):row.update(graphNode=graph,materialIndex=row['materialIndex']+2)
        for row,graph in zip(catalog['draws'],(2,5)):row.update(graphNode=graph,runtimeMaterial=row['runtimeMaterial']+2)
        for row,graph in zip(reloaded['gltfMaterialVariants']['assignments'],(2,5)):row.update(node=graph,material=row['material']+2)
        for row in reloaded['gltfMaterialTextures']['slots']:row['sourceMaterial']+=2;row['runtimeMaterial']+=2
        empty_rows=[dict(sourceMaterial=source,runtimeMaterial=source+2,slot=slot,authoredTexture=-1,evaluatedTexture=-1,runtimeTexture=None,
            texCoord=0,channel=3 if slot=='diffuseTransmission' else 4,offset=[0.,0.],scale=[1.,1.],rotation=0.,srgb=srgb)
            for source in range(2) for slot,_,srgb,_ in SLOTS]
        reloaded['gltfMaterialTextures']['slots']=empty_rows+reloaded['gltfMaterialTextures']['slots']
        state_check(reloaded,base,reference=True,reference_layout=layout);checks.append('independent-reload-bound-ids-and-complete-extra-material-slots')
        def missing_source(value):
            value['gltfMaterialBindings'].update(sourceMaterialCount=3,runtimeMaterialCount=5,sourceToRuntime=[2,3,4])
            value['gltfMaterialBindings']['materials'].pop();value['gltfMerge']['sourceTables']['materials']=3
            value['gltfMaterialTextures']['slots']=[row for row in value['gltfMaterialTextures']['slots'] if row['sourceMaterial']!=3]
        for label,mutate in [
                ('reload-missing-extra-material-slot',lambda s:s['gltfMaterialTextures']['slots'].pop(0)),
                ('reload-duplicate-extra-material-slot',lambda s:s['gltfMaterialTextures']['slots'].append(copy.deepcopy(s['gltfMaterialTextures']['slots'][0]))),
                ('reload-wrong-extra-material-runtime',lambda s:s['gltfMaterialTextures']['slots'][0].update(runtimeMaterial=4)),
                ('reload-extra-slot-disagrees-with-source',lambda s:s['gltfMaterialTextures']['slots'][0].update(authoredTexture=0,evaluatedTexture=0,runtimeTexture=0)),
                ('reload-missing-bound-material-slot',lambda s:s['gltfMaterialTextures']['slots'].pop(2*len(SLOTS))),
                ('reload-wrong-bound-source-pair',lambda s:s['gltfMaterialTextures']['slots'][2*len(SLOTS)].update(evaluatedTexture=1)),
                ('reload-wrong-bound-source-image',lambda s:s['gltfMaterialTextures']['textures'][0].update(image=2)),
                ('reload-wrong-bound-source-mesh',lambda s:s['gltfMerge']['authoringNodes'][0].update(sourceMesh=1)),
                ('reload-dropped-independently-retained-source',missing_source)]:
            damaged=copy.deepcopy(reloaded);mutate(damaged)
            reject(label,lambda damaged=damaged:state_check(damaged,base,reference=True,reference_layout=layout))
        for label,mutate in [
                ('wrong-source-identity',lambda s:s['gltfMaterialTextures']['slots'][0].update(authoredTexture=2)),
                ('wrong-evaluated-identity',lambda s:s['gltfMaterialTextures']['slots'][0].update(evaluatedTexture=2)),
                ('wrong-runtime-role',lambda s:s['gltfMaterialTextures']['slots'][0].update(runtimeTexture=1)),
                ('wrong-color-role',lambda s:s['gltfMaterialTextures']['slots'][0].update(srgb=False)),
                ('wrong-source-image',lambda s:s['gltfMaterialTextures']['textures'][0].update(image=2)),
                ('wrong-filter',lambda s:s['gltfMaterialTextures']['textures'][0].update(minFilter=9729)),
                ('wrong-wrap',lambda s:s['gltfMaterialTextures']['textures'][0].update(wrapS=33071)),
                ('wrong-sampler-source',lambda s:s['gltfMaterialTextures']['textures'][0].update(sampler=1)),
                ('wrong-uv-set',lambda s:s['gltfMaterialTextures']['slots'][0].update(texCoord=1)),
                ('wrong-transform',lambda s:s['gltfMaterialTextures']['slots'][0].update(offset=[.2,0.])),
                ('wrong-channel',lambda s:s['gltfMaterialTextures']['slots'][0].update(channel=0)),
                ('missing-runtime-sampler-metadata',lambda s:s['gltfMaterialTextures']['runtimeResources'].pop()),
                ('runtime-sampler-outside-descriptors',lambda s:s['gltfMaterialTextures']['runtimeResources'][0].update(samplerSlot=1)),
                ('missing-slot',lambda s:s['gltfMaterialTextures']['slots'].pop()),
                ('duplicate-slot',lambda s:s['gltfMaterialTextures']['slots'].append(copy.deepcopy(s['gltfMaterialTextures']['slots'][0]))),
                ('missing-source-texture',lambda s:s['gltfMaterialTextures']['textures'].pop()),
                ('truncated-resources',lambda s:s['gltfMaterialTextures'].update(resourcesTruncated=True)),
                ('zero-resource-revision',lambda s:s['gltfMaterialTextures'].update(resourceRevision=0)),
                ('float-resource-count',lambda s:s['gltfMaterialTextures'].update(archiveImageCount=3.)),
                ('wrong-material-count',lambda s:s['gltfMaterialBindings'].update(runtimeMaterialCount=5)),
                ('wrong-material-map',lambda s:s['gltfMaterialBindings'].update(sourceToRuntime=[3,2])),
                ('wrong-material-record-source',lambda s:s['gltfMaterialBindings']['materials'][0]['evaluated']['pbrMetallicRoughness']['baseColorTexture'].update(index=2)),
                ('changed-opaque-metadata',lambda s:s['gltfMaterialBindings']['materials'][0]['authored']['extras'].update(integer=16777218)),
                ('wrong-graph-route',lambda s:s['gltfMaterialVariants']['assignments'][0].update(transparent=True)),
                ('wrong-controller-route',lambda s:s['gltfMaterialBindings']['draws'][0].update(transparent=True)),
                ('wrong-controller-binding',lambda s:s['gltfMaterialBindings']['draws'][0].update(runtimeMaterial=3)),
                ('missing-controller-draw',lambda s:s['gltfMaterialBindings']['draws'].pop()),
                ('truncated-controller-draws',lambda s:s['gltfMaterialBindings'].update(drawsTruncated=True)),
                ('changed-geometry-identity',lambda s:s['gltfMerge'].update(graphIdentity=72)),
                ('pending-factor-gesture',lambda s:s['gltfMaterialAuthoring'].update(pendingGesture=True)),
                ('wrong-history-cursor',lambda s:s['gltfMaterialVariants'].update(undoCount=1))]:
            damaged=copy.deepcopy(good);mutate(damaged);reject(label,lambda damaged=damaged:state_check(damaged,base,preflight=True,baseline=good))
        for cohort in ('core','history','resize','portable','slots','failure'):
            for run in plans[cohort]['runs']:
                for step in run['steps']:state_check(synthetic(step),step)
            checks.append(cohort+'-semantic-state-matrix')
        seek=plans['history']['runs'][0]['steps'][3];clock=synthetic(seek)
        clock['gltfMaterialBindings']['materials'][0]['evaluated']['pbrMetallicRoughness']['baseColorFactor']=color_at(.25)
        reject('texture-undo-rewound-factor-clock',lambda:state_check(clock,seek))
        current=plans['core']['runs'][0]['steps'][1];owned=synthetic(current);aliases={};state_check(owned,current,aliases=aliases)
        damaged=copy.deepcopy(owned);damaged['gltfMaterialTextures']['textures'][3]['image']=4;damaged['gltfMaterialTextures'].update(archiveImageCount=5,evaluatedImageCount=5)
        reject('redo-recreated-owned-source',lambda:state_check(damaged,current,aliases=aliases))
        sampler_step=next(step for step in plans['core']['runs'][0]['steps'] if step['label']=='sampler');sampler_state=synthetic(sampler_step)
        previous=synthetic(next(step for step in plans['core']['runs'][0]['steps'] if step['label']=='existing'));peers={'existing':dict(state=previous)}
        state_check(sampler_state,sampler_step,peers=peers);checks.append('sampler-source-and-settled-allocation-observation')
        sampler_state['gltfMerge']['vmaAllocationCount']+=1
        reject('sampler-increased-settled-allocations',lambda:state_check(sampler_state,sampler_step,peers=peers))
        failure_steps=plans['failure']['runs'][0]['steps'];failure_records={step['label']:dict(state=synthetic(step)) for step in failure_steps}
        before,preview,after,canceled=(failure_records[step['label']]['state'] for step in failure_steps)
        for value in (preview,after):value['gltfMaterialAuthoring'].update(publications=1,materialRevision=2,generationEpoch=2)
        after['gltfMaterialTextures'].update(lastError='Material image decode failed: bad image encoding',errorSourceMaterial=0)
        canceled['gltfMaterialAuthoring'].update(publications=2,materialRevision=3,generationEpoch=3)
        failure_preservation(after,failure_steps[2],failure_records);cancel_restoration(canceled,failure_steps[3],failure_records)
        state_check(after,failure_steps[2],peers=failure_records);state_check(canceled,failure_steps[3],peers=failure_records)
        checks.append('actual-after-failure-and-old-gesture-cancel-oracles')
        for label,mutate in [('failure-missing-error',lambda s:s['gltfMaterialTextures'].update(lastError='')),
                ('failure-wrong-error-source',lambda s:s['gltfMaterialTextures'].update(errorSourceMaterial=1)),
                ('failure-ended-old-gesture',lambda s:s['gltfMaterialAuthoring'].update(pendingGesture=False)),
                ('failure-published-generation',lambda s:s['gltfMaterialAuthoring'].update(generationEpoch=3)),
                ('failure-increased-publications',lambda s:s['gltfMaterialAuthoring'].update(publications=2)),
                ('failure-changed-resource-revision',lambda s:s['gltfMaterialTextures'].update(resourceRevision=2)),
                ('failure-changed-packed-material',lambda s:s['gltfMaterialBindings']['materials'][0].update(uploadedMaterialHash=999)),
                ('failure-changed-history',lambda s:s['gltfMaterialVariants'].update(undoCount=1))]:
            damaged=copy.deepcopy(after);mutate(damaged);reject(label,lambda damaged=damaged:failure_preservation(damaged,failure_steps[2],failure_records))
        damaged=copy.deepcopy(canceled);damaged['gltfMaterialBindings']=copy.deepcopy(preview['gltfMaterialBindings'])
        reject('old-gesture-cancel-left-preview',lambda:cancel_restoration(damaged,failure_steps[3],failure_records))
        damaged=copy.deepcopy(canceled);damaged['gltfMaterialAuthoring']['publications']=4
        reject('old-gesture-cancel-published-twice',lambda:cancel_restoration(damaged,failure_steps[3],failure_records))
        images=plans['core']['images'];export=root/'portable-contract.gltf'
        export_step=plans['slots']['runs'][0]['steps'][-1];write_fixture(export,images,mode=export_step['mode'],expected=export_step)
        inspect_export(export,export_step);checks.append('all23-independent-serialized-contract')
        original,_=load_asset(export)
        for label,mutate in [('export-lost-legacy-field',lambda m:m['materials'][0].pop('heightTexture')),
                ('export-wrong-legacy-channel',lambda m:m['materials'][0]['opacityTexture'].update(channel=2)),
                ('export-wrong-image',lambda m:m['textures'][m['materials'][0]['normalTexture']['index']].update(source=0)),
                ('export-wrong-policy',lambda m:m['samplers'][0].update(wrapT=33071)),
                ('export-lost-transform',lambda m:m['materials'][0]['normalTexture'].pop('extensions')),
                ('export-lost-uv',lambda m:m['meshes'][0]['primitives'][0]['attributes'].pop('TEXCOORD_1')),
                ('export-collapsed-materials',lambda m:m['meshes'][1]['primitives'][0].update(material=0))]:
            damaged=copy.deepcopy(original);mutate(damaged);path=root/(label+'.gltf');document(path,damaged)
            reject(label,lambda path=path:inspect_export(path,export_step))
        animated_path=Path(plans['history']['sources']['unlit-animated']);animated=plans['history']['runs'][0]['steps'][0]
        inspect_export(animated_path,animated,animated=True);bad_curve,_=load_asset(animated_path)
        for label,mutate in [('animation-missing-pointer-target-path',lambda target:target.pop('path')),
                ('animation-wrong-pointer-target-path',lambda target:target.update(path='translation')),
                ('animation-pointer-also-targets-node',lambda target:target.update(node=0))]:
            damaged=copy.deepcopy(bad_curve);mutate(damaged['animations'][0]['channels'][0]['target'])
            path=root/(label+'.gltf');document(path,damaged)
            reject(label,lambda path=path:inspect_export(path,animated,animated=True))
        bad_curve['animations'][0]['channels'][0]['target']['extensions']['KHR_animation_pointer']['pointer']='/materials/1/pbrMetallicRoughness/baseColorFactor'
        wrong=root/'wrong-animation.gltf';document(wrong,bad_curve);reject('export-retargeted-original-curve',lambda:inspect_export(wrong,animated,animated=True))
        saved_step=plans['history']['runs'][0]['steps'][-1];saved_path=root/'animated-saved-pose.gltf'
        write_fixture(saved_path,images,mode=saved_step['mode'],expected=saved_step,animated=True)
        saved_contract=inspect_export(saved_path,saved_step,animated=True,saved_pose=True)
        checks.append('animated-export-saved-evaluated-defaults-and-original-curve')
        saved_model,_=load_asset(saved_path);wrong_pose=copy.deepcopy(saved_model)
        wrong_pose['materials'][0]['pbrMetallicRoughness']['baseColorFactor']=[1.,1.,1.,1.]
        wrong_pose_path=root/'wrong-saved-pose.gltf';document(wrong_pose_path,wrong_pose)
        reject('animated-export-wrong-saved-pose',lambda:inspect_export(wrong_pose_path,saved_step,animated=True,saved_pose=True))
        changed_curve=copy.deepcopy(saved_model);clip=changed_curve['animations'][0]
        output_accessor=changed_curve['accessors'][clip['samplers'][0]['output']];view=changed_curve['bufferViews'][output_accessor['bufferView']]
        payload=bytearray(base64.b64decode(changed_curve['buffers'][view['buffer']]['uri'].split(',',1)[1],validate=True))
        first=view.get('byteOffset',0)+output_accessor.get('byteOffset',0);payload[first:first+4]=np.asarray([.125],dtype='<f4').tobytes()
        changed_curve['buffers'][view['buffer']]['uri']='data:application/octet-stream;base64,'+base64.b64encode(payload).decode('ascii')
        changed_curve_path=root/'changed-original-curve.gltf';document(changed_curve_path,changed_curve)
        reject('animated-export-changed-original-curve-values',lambda:inspect_export(changed_curve_path,saved_step,animated=True,saved_pose=True))
        # A different reload clock must affect E without replacing the saved
        # pose defaults in A. Those defaults come from the validated file.
        reload_step={**saved_step,'time':.5,'history':[0,0],'resourcesAs':None,'samplerAs':None}
        reload_state=synthetic(reload_step)
        for side,entry in zip(('left','right'),reload_state['gltfMaterialBindings']['materials']):
            entry['authored']['pbrMetallicRoughness']['baseColorFactor']=copy.deepcopy(saved_contract['sourceLayout'][side]['baseColorFactor'])
        state_check(reload_state,reload_step,reference=True,reference_layout=saved_contract['sourceLayout'],animated=True)
        checks.append('animated-reload-saved-authored-defaults-and-independent-current-clock')
        wrong_reload=copy.deepcopy(reload_state);wrong_reload['gltfMaterialBindings']['materials'][0]['authored']['pbrMetallicRoughness']['baseColorFactor']=[1.,1.,1.,1.]
        reject('animated-reload-wrong-authored-saved-defaults',lambda:state_check(wrong_reload,reload_step,reference=True,
            reference_layout=saved_contract['sourceLayout'],animated=True))
        opacity_step=plans['portable']['runs'][0]['steps'][-1];opacity_literal=root/'opacity-literal-source.gltf'
        write_fixture(opacity_literal,images,mode=opacity_step['mode'],expected=opacity_step)
        inspect_export(opacity_literal,opacity_step);live_opacity=synthetic(opacity_step);state_check(live_opacity,opacity_step)
        checks.append('opacity-live-authored-evaluated-and-literal-source-remain-opaque')
        for side in ('authored','evaluated'):
            damaged=copy.deepcopy(live_opacity);damaged['gltfMaterialBindings']['materials'][0][side]['alphaMode']='BLEND'
            reject('opacity-live-'+side+'-wrong-alpha',lambda damaged=damaged:state_check(damaged,opacity_step))
        opacity_model,_=load_asset(opacity_literal);opacity_model['materials'][0]['alphaMode']='BLEND'
        opacity_model['materials'][0]['pbrMetallicRoughness']['opacityTexture']=opacity_model['materials'][0].pop('opacityTexture')
        opacity_saved=root/'opacity-canonical-saved.gltf';document(opacity_saved,opacity_model)
        opacity_contract=inspect_export(opacity_saved,opacity_step,saved_pose=True)
        opacity_reload_step={**opacity_step,'history':[0,0],'resourcesAs':None,'samplerAs':None}
        opacity_reload=synthetic(opacity_reload_step)
        for side in ('authored','evaluated'):opacity_reload['gltfMaterialBindings']['materials'][0][side]['alphaMode']='BLEND'
        state_check(opacity_reload,opacity_reload_step,reference=True,reference_layout=opacity_contract['sourceLayout'])
        checks.append('opacity-export-and-reload-use-independently-validated-canonical-blend')
        for mode in ('OPAQUE','MASK'):
            damaged=copy.deepcopy(opacity_model);damaged['materials'][0]['alphaMode']=mode
            path=root/('opacity-export-wrong-'+mode+'.gltf');document(path,damaged)
            reject('opacity-export-wrong-alpha-'+mode,lambda path=path:inspect_export(path,opacity_step,saved_pose=True))
        damaged=copy.deepcopy(opacity_model);damaged['materials'][1]['alphaMode']='BLEND'
        path=root/'opacity-export-changed-other-material-alpha.gltf';document(path,damaged)
        reject('opacity-export-alpha-promotion-leaked-to-other-material',lambda:inspect_export(path,opacity_step,saved_pose=True))
        damaged=copy.deepcopy(opacity_reload);damaged['gltfMaterialBindings']['materials'][0]['evaluated']['alphaMode']='OPAQUE'
        reject('opacity-reload-alpha-differs-from-validated-file',lambda:state_check(damaged,opacity_reload_step,reference=True,
            reference_layout=opacity_contract['sourceLayout']))
        for label,mutate in [('opacity-export-lost-pbr-legacy-binding',lambda m:m['materials'][0]['pbrMetallicRoughness'].pop('opacityTexture')),
                ('opacity-export-wrong-pbr-legacy-image',lambda m:m['materials'][0]['pbrMetallicRoughness']['opacityTexture'].update(index=1))]:
            damaged=copy.deepcopy(opacity_model);mutate(damaged);path=root/(label+'.gltf');document(path,damaged)
            reject(label,lambda path=path:inspect_export(path,opacity_step,saved_pose=True))
        damaged=copy.deepcopy(opacity_model);damaged['materials'][0]['opacityTexture']={'index':2}
        damaged['materials'][0]['pbrMetallicRoughness']['opacityTexture']['index']=1
        path=root/'opacity-root-fallback-hides-wrong-pbr-binding.gltf';document(path,damaged)
        reject('opacity-root-fallback-cannot-hide-wrong-pbr-binding',lambda:inspect_export(path,opacity_step,saved_pose=True))
        pixels=np.zeros((HEIGHT,WIDTH,3),dtype=np.uint8)
        for a,b,c,d in binding.REGIONS:pixels[round(b*HEIGHT):round(d*HEIGHT),round(a*WIDTH):round(c*WIDTH)]=[40,140,80]
        image=root/'reference.png';Image.fromarray(pixels).save(image);record=dict(image=str(image),resolution=[WIDTH,HEIGHT])
        binding.visible_reference(record);binding.compare_images(record,record);checks.append('nonempty-independent-image-control')
        unchanged_image(record,record);checks.append('complete-after-failure-image-oracle')
        reject('vacuous-positive-image',lambda:positive_change(record,record,8.))
        changed=pixels.copy();a,b,c,d=binding.REGIONS[0];changed[round(b*HEIGHT):round(d*HEIGHT),round(a*WIDTH):round(c*WIDTH)]=[150,20,90]
        other=root/'changed.png';Image.fromarray(changed).save(other);other_record=dict(image=str(other),resolution=[WIDTH,HEIGHT])
        positive_change(record,other_record,8.);reject('wrong-literal-image',lambda:binding.compare_images(record,other_record))
        outside=pixels.copy();outside[0,0]=[255,255,255];outside_path=root/'outside-roi.png';Image.fromarray(outside).save(outside_path)
        reject('after-failure-image-changed-outside-regions',lambda:unchanged_image(record,dict(image=str(outside_path),resolution=[WIDTH,HEIGHT])))
        changed=pixels.copy()
        for a,b,c,d in binding.REGIONS:changed[round(b*HEIGHT):round(d*HEIGHT),round(a*WIDTH):round(c*WIDTH)]=[150,20,90]
        leaked=root/'leaked.png';Image.fromarray(changed).save(leaked)
        reject('texture-change-leaked-to-other-material',lambda:positive_change(record,dict(image=str(leaked),resolution=[WIDTH,HEIGHT]),8.))
        victim=Path(plans['core']['immutableInputs'][0]['path']);reject('immutable-input-overwrite',lambda:immutable(victim,b'different'))
        checks.append('source-input-immutability')
    return dict(passed=True,checkCount=len(checks),checks=checks,perTechniquePlanCounts=counts,sourceDependencies=dependencies(),rendererExecuted=False,
        nativeValidationPerformed=False,unverified=UNVERIFIED)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path);parser.add_argument('--renderer',type=Path);parser.add_argument('--manifest',type=Path)
    parser.add_argument('--technique',choices=TECHNIQUES,action='append');parser.add_argument('--cohort',choices=COHORTS,default='core')
    parser.add_argument('--timeout',type=float,default=600.);parser.add_argument('--self-check',action='store_true');parser.add_argument('--generate-only',action='store_true')
    args=parser.parse_args()
    if args.self_check:print(json.dumps(self_check(),indent=2,allow_nan=False));return
    if args.output is None:parser.error('--output is required')
    if not math.isfinite(args.timeout) or args.timeout<=0:parser.error('--timeout must be finite and positive')
    if not args.generate_only and (args.renderer is None or args.manifest is None):parser.error('Native execution requires explicit --renderer and --manifest')
    args.technique=tuple(dict.fromkeys(args.technique or TECHNIQUES))
    if args.renderer:args.renderer=args.renderer.resolve()
    if args.manifest:args.manifest=args.manifest.resolve()
    output=args.output.resolve();plan=generate(output,args.cohort)
    report=dict(schemaVersion=1,cohort=args.cohort,passed=None if args.generate_only else False,generatedOnly=args.generate_only,
        rendererExecuted=False,nativeValidationPerformed=False,sourceDependencies=dependencies(),launchPlanSha256=sha(output/'launch-plan.json'),
        **{key:plan[key]*len(args.technique) for key in ('expectedCaptureCount','expectedProcessCount','expectedStateCheckCount','expectedImageCheckCount','expectedExportCount')},
        captures=[],processes=[],stateChecks=[],imageChecks=[],exports=[],portableCopies=[],unverified=UNVERIFIED)
    if args.generate_only:
        document(output/'results.json',report);print('GENERATED',report['expectedCaptureCount'],'planned captures;',report['expectedProcessCount'],'planned processes; renderer not executed')
    else:
        run_native(args,output,plan,report)
        print('PASS',len(report['captures']),'captures;',len(report['stateChecks']),'CPU/catalog checks;',len(report['imageChecks']),'image checks')


if __name__=='__main__':main()
