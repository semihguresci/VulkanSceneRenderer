"""Native glTF filtering checks for both raster paths, without editor dependencies."""
import argparse,base64,copy,hashlib,json,os,struct,subprocess,zlib
from pathlib import Path
import numpy as np
from PIL import Image
from display_capture_test_support import validation_log_error
FILTERS=dict(nearest=9728,linear=9729,nearest_mipmap_nearest=9984,linear_mipmap_nearest=9985,nearest_mipmap_linear=9986,linear_mipmap_linear=9987)
def require(condition, message):
    if not condition:
        raise RuntimeError(message)

def pattern_png(size, alpha_mode):
    rows = bytearray()
    for y in range(size):
        rows.append(0)
        for x in range(size):
            check = (x // (2 if size > 4 else 1) + y // (2 if size > 4 else 1)) % 2
            rows.extend((255 if check else 0, 0 if check else 255, 0,
                         0 if alpha_mode == 'MASK' and not check else 255))
    def chunk(kind, payload):
        return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(bytes(rows))) + chunk(b'IEND', b''))

def write_fixture(path, min_filter, mag_filter, repeat=1, alpha_mode='OPAQUE', edited=False, shared=False):
    data, views, accessors = bytearray(), [], []
    def accessor(kind, values, component=5126):
        while len(data) % 4:
            data.append(0)
        offset = len(data)
        flat = [x for value in values for x in value] if isinstance(values[0], tuple) else values
        data.extend(struct.pack('<' + ('f' if component == 5126 else 'H') * len(flat), *flat))
        views.append(dict(buffer=0, byteOffset=offset, byteLength=len(data)-offset))
        result = dict(bufferView=len(views)-1, componentType=component, count=len(values), type=kind)
        if kind == 'VEC3':
            result.update(min=[min(value[i] for value in values) for i in range(3)],
                          max=[max(value[i] for value in values) for i in range(3)])
        elif kind == 'SCALAR' and component == 5126:
            result.update(min=[min(values)], max=[max(values)])
        accessors.append(result)
        return len(accessors)-1
    positions = accessor('VEC3', [(-1., -1., 0.), (1., -1., 0.), (1., 1., 0.), (-1., 1., 0.)])
    normals = accessor('VEC3', [(0., 0., 1.)] * 4)
    uv = accessor('VEC2', [(0., float(repeat)), (float(repeat), float(repeat)), (float(repeat), 0.), (0., 0.)])
    indices = accessor('SCALAR', [0, 1, 2, 0, 2, 3], 5123)
    material = dict(name='Authored filter material', extensions=dict(KHR_materials_unlit={}),
                    alphaMode=alpha_mode, alphaCutoff=.3,
                    pbrMetallicRoughness=dict(baseColorFactor=[1, 1, 1, .65 if alpha_mode == 'BLEND' else 1],
                                             baseColorTexture=dict(index=0), metallicFactor=0, roughnessFactor=1))
    model = dict(asset=dict(version='2.0', generator='Container sampler filter regression'),
                 extensionsUsed=['KHR_materials_unlit'], scene=0, scenes=[dict(nodes=[0])],
                 nodes=[dict(name='Filter probe', mesh=0)], materials=[material],
                 meshes=[dict(primitives=[dict(attributes=dict(POSITION=positions, NORMAL=normals, TEXCOORD_0=uv),
                                              indices=indices, material=0)])],
                 images=[dict(uri='data:image/png;base64,' + base64.b64encode(pattern_png(64 if repeat > 1 else 4, alpha_mode)).decode())],
                 samplers=[dict(minFilter=min_filter, magFilter=mag_filter,
                                wrapS=10497 if repeat > 1 else 33071, wrapT=10497 if repeat > 1 else 33071)],
                 textures=[dict(source=0, sampler=0)])
    if shared:
        # Same decoded image, two different sampler policies, visible together.
        model['nodes'] = [dict(name='Nearest shared-image probe', mesh=0, translation=[-.5, 0, 0], scale=[.5, 1, 1]),
                          dict(name='Linear shared-image probe', mesh=1, translation=[.5, 0, 0], scale=[.5, 1, 1])]
        model['scenes'][0]['nodes'] = [0, 1]
        model['meshes'].append(copy.deepcopy(model['meshes'][0]))
        model['meshes'][1]['primitives'][0]['material'] = 1
        model['materials'].append(copy.deepcopy(material))
        model['materials'][1]['pbrMetallicRoughness']['baseColorTexture']['index'] = 1
        model['samplers'].append(dict(model['samplers'][0], magFilter=9729))
        model['textures'].append(dict(source=0, sampler=1))
    if edited:
        times = accessor('SCALAR', [0., 1.])
        colors = accessor('VEC4', [(1., 1., 1., 1.), (.5, .5, .5, 1.)])
        model['extensionsUsed'].append('KHR_animation_pointer')
        model['animations'] = [dict(name='Factor edit retains sampler',
            samplers=[dict(input=times, output=colors, interpolation='LINEAR')],
            channels=[dict(sampler=0, target=dict(path='pointer', extensions=dict(KHR_animation_pointer=dict(
                pointer='/materials/0/pbrMetallicRoughness/baseColorFactor'))))])]
    model.update(bufferViews=views, accessors=accessors,
                 buffers=[dict(byteLength=len(data), uri='data:application/octet-stream;base64,' + base64.b64encode(data).decode())])
    path.write_text(json.dumps(model, indent=2), encoding='utf-8')

def pixels(record):
    with Image.open(record['image']) as image:
        values = np.asarray(image.convert('RGB'), dtype=float)
    require(values.shape == (360, 640, 3), 'Wrong sampler capture extent')
    # Pinhole camera z3/fov60 projects the quad to x216..424, y76..284.
    # Exclude geometric edges so antialiasing/coverage cannot mimic filtering.
    values = values[84:276, 224:416]
    require(values.std() > 3, 'Sampler comparison is vacuous')
    return values

def difference(first, second):
    delta = np.abs(pixels(first)-pixels(second))
    return dict(meanAbsolutePixelError=float(delta.mean()),
                changedPixelFraction=float(np.mean(np.max(delta, axis=2) >= 8)))

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('renderer',type=Path);parser.add_argument('output',type=Path)
    parser.add_argument('--ray-smoke',action='store_true')
    args=parser.parse_args();exe=args.renderer.resolve();root=args.output.resolve()
    root.mkdir(parents=True,exist_ok=False)
    digest=hashlib.sha256(exe.read_bytes()).hexdigest();records=[];checks=[]
    (root/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n',encoding='utf-8')
    def capture(source,name,technique,extra=()):
        image=root/(name+'.png');log=image.with_suffix('.log')
        command=[str(exe),'--model',str(source),'--hidden','--no-ui','--validation',
            '--width','640','--height','360','--msaa','1','--no-taa','--no-bloom',
            '--environment-intensity','0','--directional-intensity','0','--exposure','1',
            '--camera-position','0','0','3','--camera-target','0','0','0','--camera-fov','60',
            '--warmup-frames','3','--capture-frame','4','--fixed-dt','.016666667',
            '--render-technique',technique,'--display-mode','lit','--screenshot',str(image)]+list(extra)
        with log.open('w',encoding='utf-8') as stream:
            result=subprocess.run(command,cwd=exe.parent,stdout=stream,stderr=subprocess.STDOUT,
                timeout=120,env=dict(os.environ,VK_LAYER_SETTINGS_PATH=str(root)),creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        require(result.returncode==0,f'Renderer failed: {log}')
        require(not validation_log_error(log.read_text(encoding='utf-8',errors='replace')),f'Validation failed: {log}')
        telemetry=image.with_suffix('.telemetry.json');state=json.loads(telemetry.read_text(encoding='utf-8-sig'))
        require(state['technique']==technique and state['scene']['primaryObjectCount']>=1,'Wrong scene or renderer')
        record=dict(image=str(image),telemetry=str(telemetry),log=str(log),command=command)
        records.append(record);pixels(record);return record
    for case in ['mag','min','shared','transparent','mask']:
        for repeat in ([4,8] if case=='min' else [1]):
            policies=list(FILTERS.items()) if case=='min' else [('nearest',9987),('linear',9987)]
            if case=='shared':policies=[('shared',9987)]
            fixtures={}
            for name,min_filter in policies:
                path=root/f'{case}-{repeat}-{name}.gltf'
                write_fixture(path,min_filter,9728 if case=='min' or name in ('nearest','shared') else 9729,repeat,
                    'BLEND' if case=='transparent' else 'MASK' if case=='mask' else 'OPAQUE',shared=case=='shared')
                fixtures[name]=path
            for technique in ['deferred-raster','forward-raster']:
                captures={name:capture(path,f'{technique}-{case}-{repeat}-{name}',technique) for name,path in fixtures.items()}
                if case=='shared':
                    values=pixels(captures['shared']);left,right=np.split(values,2,axis=1)
                    mixed=lambda a:float(np.mean((a[:,:,0]>70)&(a[:,:,1]>70)))
                    a,b=mixed(left),mixed(right);require(a<.03 and b>.15,'Shared images aliased distinct policies')
                    checks.append(dict(case=case,technique=technique,nearestMixedFraction=a,linearMixedFraction=b))
                elif case in ('mag','transparent'):
                    mixed=lambda a:float(np.mean((a[:,:,0]>70)&(a[:,:,1]>70)))
                    a,b=mixed(pixels(captures['nearest'])),mixed(pixels(captures['linear']))
                    require(a<.03 and b>.15,'Magnification did not retain nearest and bilinear edges')
                    checks.append(dict(case=case,technique=technique,nearestMixedFraction=a,linearMixedFraction=b))
                elif case=='mask':
                    delta=difference(captures['nearest'],captures['linear']);require(delta['changedPixelFraction']>.01,'MASK has no filtering effect')
                    checks.append(dict(case=case,technique=technique,**delta))
                else:
                    pairs=[('nearest','nearest_mipmap_nearest',False),('linear','linear_mipmap_nearest',False)] if repeat==4 else [
                        ('nearest_mipmap_nearest','nearest_mipmap_linear',True),('linear_mipmap_nearest','linear_mipmap_linear',True),
                        ('linear','linear_mipmap_linear',True),('nearest','linear',True)]
                    for a,b,distinct in pairs:
                        delta=difference(captures[a],captures[b]);require(
                            delta['meanAbsolutePixelError']>.5 and delta['changedPixelFraction']>.01 if distinct else delta['meanAbsolutePixelError']<=.1,
                            f'Unexpected minification {a}/{b}: {delta}')
                        checks.append(dict(case=case,repeat=repeat,technique=technique,first=a,second=b,distinct=distinct,**delta))
                print('PASS',case,repeat,technique,flush=True)
    # Twelve authored policies plus the default exercise real shader slots
    # beyond the former nine-entry wrap prefix. Magnification makes the oracle
    # depend only on each tile's authored magFilter, independently of minFilter.
    path=root/'twelve-policies.gltf';write_fixture(path,9987,9728)
    model=json.loads(path.read_text(encoding='utf-8'));prototype=copy.deepcopy(model['materials'][0])
    mesh=copy.deepcopy(model['meshes'][0]);model['materials']=[];model['meshes']=[]
    model['textures']=[];model['samplers']=[];model['nodes']=[]
    policies=[(minimum,magnification) for minimum in FILTERS.values() for magnification in [9728,9729]]
    for i,(minimum,magnification) in enumerate(policies):
        material=copy.deepcopy(prototype);material['pbrMetallicRoughness']['baseColorTexture']['index']=i
        model['materials'].append(material);primitive=copy.deepcopy(mesh);primitive['primitives'][0]['material']=i
        model['meshes'].append(primitive);model['textures'].append(dict(source=0,sampler=i))
        model['samplers'].append(dict(minFilter=minimum,magFilter=magnification,wrapS=33071,wrapT=33071))
        model['nodes'].append(dict(mesh=i,translation=[-.75+(i%4)*.5,2/3-(i//4)*2/3,0],scale=[.25,1/3,1]))
    model['scenes'][0]['nodes']=list(range(12));path.write_text(json.dumps(model),encoding='utf-8')
    for technique in ['deferred-raster','forward-raster']:
        record=capture(path,technique+'-twelve-policies',technique)
        state=json.loads(Path(record['telemetry']).read_text(encoding='utf-8-sig'))
        catalog=state['gltfMaterialTextures']
        require(state['scene']['primaryObjectCount']==len(policies) and not catalog['resourcesTruncated']
                and catalog['archiveImageCount']==1, 'Dense fixture did not publish all tiles on one source image')
        textures={row['sourceTexture']: row for row in catalog['textures']}
        metadata={row['runtimeTexture']: row for row in catalog['runtimeResources']}
        image=np.asarray(Image.open(record['image']).convert('RGB'))
        slots=set()
        for i,(minification,magnification) in enumerate(policies):
            texture=textures[i]
            require((texture['image'],texture['minFilter'],texture['magFilter'],texture['wrapS'],texture['wrapT'])
                    ==(0,minification,magnification,33071,33071), f'Authored policy changed in tile {i}')
            sampler=metadata[texture['colorResource']]
            require(sampler['samplerCount']==len(policies)+1 and 0<sampler['samplerSlot']<sampler['samplerCount'],
                    f'Tile {i} did not publish a nondefault sampler slot')
            slots.add(sampler['samplerSlot'])
            x=round(320+103.923048*(-.75+(i%4)*.5));y=round(180-103.923048*(2/3-(i//4)*2/3))
            tile=image[y-24:y+24,x-18:x+18]
            mixed=float(np.mean((tile[:,:,0]>70)&(tile[:,:,1]>70)))
            require(mixed<.03 if magnification==9728 else mixed>.15,f'Wrong sampler in policy tile {i}: {mixed}')
            checks.append(dict(case='twelve-policies',technique=technique,texture=i,minFilter=minification,
                magFilter=magnification,samplerSlot=sampler['samplerSlot'],samplerCount=sampler['samplerCount'],mixedFraction=mixed))
        require(len(slots)==len(policies), 'Distinct authored policies shared a dense sampler slot')
    if args.ray_smoke:
        path=root/'ray-alpha.gltf';write_fixture(path,9986,9728,4,'MASK',shared=True)
        for technique in ['deferred-raster','forward-raster']:
            record=capture(path,technique+'-ray-alpha',technique,[
                '--directional-intensity','3','--ray-shadows','hard','--no-ray-shadow-denoise','--ddgi','--ddgi-fit-scene',
                '--ddgi-fit-probes','64','--ddgi-rays','32','--ddgi-probes-per-frame','64','--ddgi-ray-budget','2048','--ddgi-hysteresis','0'])
            state=json.loads(Path(record['telemetry']).read_text(encoding='utf-8-sig'))
            ray,ddgi=state['rayShadows'],state['ddgi']
            require(ray['supported'] and ray['active'],'Hard ray-shadow sampler smoke did not execute')
            require(ddgi['supported'] and ddgi['active'] and ddgi['tracedRays']>0,'DDGI sampler smoke did not trace rays')
            checks.append(dict(case='ray-alpha',technique=technique,rayShadowActive=True,ddgiTracedRays=ddgi['tracedRays']))
    require(hashlib.sha256(exe.read_bytes()).hexdigest()==digest,'Renderer changed during verification')
    (root/'results.json').write_text(json.dumps(dict(passed=True,rendererSha256=digest,captures=len(records),records=records,checks=checks),indent=2)+'\n',encoding='utf-8')
    print('PASS',len(records),'captures',len(checks),'checks',flush=True)
if __name__=='__main__':main()
