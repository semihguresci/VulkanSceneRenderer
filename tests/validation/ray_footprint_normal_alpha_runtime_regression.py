"""DDGI normal-mip and conservative alpha-cutout consumer checks for #82.

Independent authored RGBA8 mip levels, literal baked normal/strip geometry and
wrong-normal/full/empty controls. Candidate coverage intentionally stays at mip0;
normal shading uses ray gradients. --self-check/--generate-only never use a GPU.
Native inventory: seven profiles x two raster techniques x eight retired frames
= 14 processes / 112 EXRs. The source panel emits zero radiance in every profile.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import re
import statistics
import struct
import subprocess
import sys
import tempfile
import traceback

os.environ.setdefault('OPENCV_IO_ENABLE_OPENEXR', '1')
import cv2
import numpy as np

from gltf_accessor_runtime_regression import Asset

PROFILES=('normal-textured','normal-coarse','normal-base','normal-degenerate','alpha-textured','alpha-baked','empty')
TECHNIQUES=('forward-raster','deferred-raster')
FRAMES=tuple(range(14,30,2))
WIDTH,HEIGHT=256,192
UV_SCALE=16
BASE_NORMAL=(204,128,230,255)
COARSE_NORMAL=(128,204,230,255)
KTX_ID=b'\xabKTX 11\xbb\r\n\x1a\n'
LIGHT=np.array([.95,0.,-math.sqrt(1-.95**2)])
SOURCE=(.48,2.2,-1.2,1.2,.75)
SPECULAR='KHR_materials_specular'
THRESHOLDS=dict(normalNormalizedRmse=.035,normalRelativeMeanError=.025,
    alphaNormalizedRmse=.06,alphaRelativeMeanError=.04,negativeNormalSeparation=.20,
    alphaMinimumFullFraction=.30,alphaMaximumFullFraction=.70,
    emptyRelativeMaximum=.01,emptyAbsoluteMaximum=.0001,minimumFullMean=.01)


def require(value,message):
    if not value: raise RuntimeError(message)


def sha(path):
    with Path(path).open('rb') as stream: return hashlib.file_digest(stream,'sha256').hexdigest()


def write(path,value):
    Path(path).write_text(json.dumps(value,indent=2,allow_nan=False)+'\n',encoding='utf-8')


def normal(rgb):
    result=np.array(rgb[:3],dtype=float)/127.5-1
    result/=np.linalg.norm(result)
    # Source winding gives N=-Z, authored tangent +X, bitangent -Y.
    return result*np.array([1.,-1.,-1.])


def ktx(levels):
    height,width,_=levels[0].shape
    header=struct.pack('<13I',0x04030201,0x1401,1,0x1908,0x8058,0x1908,
                       width,height,0,0,1,len(levels),0)
    data=bytearray(KTX_ID+header)
    for pixels in levels:
        payload=np.asarray(pixels,dtype=np.uint8).tobytes()
        data.extend(struct.pack('<I',len(payload))+payload)
        data.extend(bytes((-len(payload))%4))
    return bytes(data)


def decode_ktx(path):
    data=Path(path).read_bytes()
    require(data[:12]==KTX_ID and len(data)>=64,'Invalid KTX identifier/header')
    header=struct.unpack_from('<13I',data,12)
    require(header[:6]==(0x04030201,0x1401,1,0x1908,0x8058,0x1908),'Texture must be linear uncompressed RGBA8')
    width,height=header[6:8]
    require(header[8:11]==(0,0,1) and header[11]==2 and header[12]==0,'Unexpected KTX dimensions/layers/mips')
    levels=[];offset=64
    for i in range(header[11]):
        require(offset+4<=len(data),'Missing KTX imageSize')
        length,=struct.unpack_from('<I',data,offset);offset+=4
        w,h=max(1,width>>i),max(1,height>>i)
        require(length==w*h*4 and offset+length<=len(data),'Truncated or malformed KTX level')
        levels.append(np.frombuffer(data[offset:offset+length],np.uint8).reshape(h,w,4).copy())
        offset+=length+(-length)%4
    require(offset==len(data),'Unexpected trailing KTX data')
    return levels


def add_texture(asset,name):
    image=len(asset.model.setdefault('images',[]))
    asset.model['images'].append(dict(uri=name,mimeType='image/ktx'))
    sampler=len(asset.model.setdefault('samplers',[]))
    asset.model['samplers'].append(dict(minFilter=9984,magFilter=9728,wrapS=10497,wrapT=10497))
    texture=len(asset.model.setdefault('textures',[]))
    asset.model['textures'].append(dict(source=image,sampler=sampler))
    return texture


def quad(asset,box,material,*,front=False,shading=None,uv=None):
    x0,x1,y0,y1,z=box
    normals=[0.,0.,1. if front else -1.] if shading is None else shading
    return dict(material=material,attributes=dict(
        POSITION=asset.accessor('VEC3',[[x0,y0,z],[x1,y0,z],[x1,y1,z],[x0,y1,z]]),
        NORMAL=asset.accessor('VEC3',[normals]*4),
        TANGENT=asset.accessor('VEC4',[[1.,0.,0.,1.]]*4),
        TEXCOORD_0=asset.accessor('VEC2',uv or [[0,0],[UV_SCALE,0],[UV_SCALE,UV_SCALE],[0,UV_SCALE]])),
        indices=asset.accessor('SCALAR',[0,1,2,0,2,3] if front else [0,2,1,0,3,2],5123,indices=True))


def generate(output):
    output=Path(output).resolve();output.mkdir(parents=True,exist_ok=True)
    normals=[np.tile(BASE_NORMAL,(128,128,1)).astype(np.uint8),np.tile(COARSE_NORMAL,(64,64,1)).astype(np.uint8)]
    alpha=np.full((128,128,4),255,np.uint8)
    # 128/255 is just above .5 linearly, but sRGB-decoding alpha would put it
    # below .5. Literal strip geometry independently witnesses the linear role.
    alpha[:,:,3]=128
    alpha[:,:32,3]=alpha[:,96:,3]=0
    opaque_coarse=np.full((64,64,4),255,np.uint8);opaque_coarse[:,:,3]=0
    (output/'normal.ktx').write_bytes(ktx(normals))
    (output/'coverage.ktx').write_bytes(ktx([alpha,opaque_coarse]))
    result={}
    for profile in PROFILES:
        asset=Asset()
        receiver=dict(name='Unlit-by-directional receiver',emissiveFactor=[0,0,0],
            pbrMetallicRoughness=dict(baseColorFactor=[.8,.8,.8,1],metallicFactor=0,roughnessFactor=1),
            extensions={SPECULAR:dict(specularFactor=0)})
        bounce=copy.deepcopy(receiver);bounce['name']='Zero-emission off-axis bounce panel'
        bounce['pbrMetallicRoughness']['baseColorFactor']=[.7,.7,.7,1]
        if profile in ('normal-textured','normal-degenerate','alpha-textured'):
            bounce['normalTexture']=dict(index=add_texture(asset,'normal.ktx'),scale=1)
        if profile=='alpha-textured':
            bounce.update(alphaMode='MASK',alphaCutoff=.5)
            bounce['pbrMetallicRoughness']['baseColorTexture']=dict(index=add_texture(asset,'coverage.ktx'))
        asset.model['materials']=[receiver,bounce]
        primitives=[quad(asset,(-1.5,1.5,-1.5,1.5,0),0,front=True)]
        if profile=='alpha-baked':
            x0,x1,y0,y1,z=SOURCE
            for i in range(UV_SCALE):
                # Exact mip-zero nearest coverage: fractional U in [.25,.75).
                a=x0+(x1-x0)*(i+.25)/UV_SCALE;b=x0+(x1-x0)*(i+.75)/UV_SCALE
                primitives.append(quad(asset,(a,b,y0,y1,z),1,shading=normal(COARSE_NORMAL).tolist()))
        elif profile!='empty':
            shading=normal(BASE_NORMAL if profile=='normal-base' else COARSE_NORMAL).tolist() if profile in ('normal-base','normal-coarse') else None
            uv=[[.5,.5]]*4 if profile=='normal-degenerate' else None
            primitives.append(quad(asset,SOURCE,1,shading=shading,uv=uv))
        asset.model['meshes']=[dict(name='Independent receiver and source',primitives=primitives)]
        asset.model['nodes']=[dict(mesh=0)]
        asset.model['scenes']=[dict(nodes=[0])]
        asset.model['extensionsUsed']=[SPECULAR]
        asset.model['extras']=dict(footprintProfile=profile,emission=0,candidateCoverageLod=0,
            expectedNormal='base' if profile in ('normal-base','normal-degenerate') else 'coarse')
        path=output/(profile+'.gltf');asset.write(path);result[profile]=str(path)
    return result


def validate(output,models):
    normal_levels=decode_ktx(output/'normal.ktx');alpha_levels=decode_ktx(output/'coverage.ktx')
    require([level.shape for level in normal_levels]==[(128,128,4),(64,64,4)],'Normal mip dimensions differ from independent LOD proof')
    require(np.all(normal_levels[0]==BASE_NORMAL) and np.all(normal_levels[1]==COARSE_NORMAL),'Authored normal mip roles changed')
    expected=np.full((128,128),128,np.uint8);expected[:,:32]=expected[:,96:]=0
    require(np.array_equal(alpha_levels[0][:,:,3],expected) and np.all(alpha_levels[1][:,:,3]==0),'Alpha coverage sentinel mips changed')
    require(np.all(alpha_levels[0][:,:,:3]==255) and np.all(alpha_levels[1][:,:,:3]==255),'Coverage RGB must stay constant white')
    summaries={}
    for profile,path in models.items():
        model=json.loads(Path(path).read_text())
        require(model['extras']['footprintProfile']==profile,'Fixture profile differs')
        require(all(m.get('emissiveFactor')==[0,0,0] for m in model['materials']),'Normal fixture must emit zero radiance')
        for i,material in enumerate(model['materials']):
            mr=material['pbrMetallicRoughness']
            require(mr['baseColorFactor']==([.8,.8,.8,1] if i==0 else [.7,.7,.7,1]) and
                mr['metallicFactor']==0 and mr['roughnessFactor']==1 and
                material['extensions']=={SPECULAR:dict(specularFactor=0)},'Fixture direct-material energy changed')
        bounce=model['materials'][1]
        if profile in ('normal-textured','normal-degenerate','alpha-textured'):
            require(bounce['normalTexture']==dict(index=0,scale=1),'Missing literal linear normal binding')
        if profile=='alpha-textured':
            require(bounce['alphaMode']=='MASK' and bounce['alphaCutoff']==.5 and
                bounce['pbrMetallicRoughness']['baseColorTexture']==dict(index=1),'Missing mip-zero alpha witness')
        primitives=model['meshes'][0]['primitives']
        require(len(primitives)==(1 if profile=='empty' else UV_SCALE+1 if profile=='alpha-baked' else 2),'Missing source geometry')
        # Decode independently rather than asking renderer geometry helpers.
        import base64
        data=base64.b64decode(model['buffers'][0]['uri'].split(',',1)[1])
        def accessor(index,width,code):
            item=model['accessors'][index];view=model['bufferViews'][item['bufferView']]
            offset=view.get('byteOffset',0)+item.get('byteOffset',0)
            stride=view.get('byteStride',struct.calcsize(code)*width)
            require(offset+stride*(item['count']-1)+width*struct.calcsize(code)<=len(data),'Accessor out of range')
            return np.array([struct.unpack_from('<'+code*width,data,offset+i*stride) for i in range(item['count'])])
        total_area=0.
        for i,primitive in enumerate(primitives):
            p=accessor(primitive['attributes']['POSITION'],3,'f');n=accessor(primitive['attributes']['NORMAL'],3,'f')
            uv=accessor(primitive['attributes']['TEXCOORD_0'],2,'f');indices=accessor(primitive['indices'],1,'H').ravel()
            require(np.isfinite(p).all() and np.isfinite(n).all() and np.isfinite(uv).all(),'Nonfinite geometry')
            require(np.allclose(np.linalg.norm(n,axis=1),1,atol=1e-6),'Nonunit authored normals')
            triangles=p[indices].reshape(-1,3,3);cross=np.cross(triangles[:,1]-triangles[:,0],triangles[:,2]-triangles[:,0])
            require(np.all(np.linalg.norm(cross,axis=1)>0),'Degenerate geometry')
            require(np.all(cross[:,2]>0) if i==0 else np.all(cross[:,2]<0),'Receiver/source winding changed')
            if i:
                if profile=='alpha-baked':
                    x0,x1,y0,y1,z=SOURCE;j=i-1
                    a=x0+(x1-x0)*(j+.25)/UV_SCALE;b=x0+(x1-x0)*(j+.75)/UV_SCALE
                    expected_p=[[a,y0,z],[b,y0,z],[b,y1,z],[a,y1,z]]
                else:
                    x0,x1,y0,y1,z=SOURCE;expected_p=[[x0,y0,z],[x1,y0,z],[x1,y1,z],[x0,y1,z]]
                require(np.allclose(p,expected_p,rtol=0,atol=2e-7),'Baked/source placement differs from literal cutout oracle')
                total_area+=float(np.linalg.norm(cross,axis=1).sum()/2)
                if profile in ('normal-base','normal-coarse','alpha-baked'):
                    expected_n=normal(BASE_NORMAL if profile=='normal-base' else COARSE_NORMAL)
                    require(np.allclose(n,expected_n,atol=1e-6),'Baked world normal differs from independent decoded normal')
                if profile=='normal-degenerate': require(np.all(uv==.5),'Invalid-footprint witness is not UV-degenerate')
                elif profile in ('normal-textured','alpha-textured'): require(np.array_equal(uv,[[0,0],[16,0],[16,16],[0,16]]),'UV scale changed')
        full_area=(SOURCE[1]-SOURCE[0])*(SOURCE[3]-SOURCE[2])
        if profile=='alpha-baked': require(abs(total_area/full_area-.5)<1e-6,'Baked cutout must cover exactly half the plane')
        summaries[profile]=dict(sourceArea=total_area,primitiveCount=len(primitives),emission=0,sha256=sha(path))
    # Smallest normal gap to configured probe Z planes is .15 world units.
    # Slant correction can only enlarge this lower bound. rho>2 guarantees
    # nearest-mip selection clamps to the authored coarse level, for every hit.
    spread=2*math.tan(math.acos(1-2/64))
    distance=min(abs(SOURCE[4]-z) for z in (-.2,.2,.6,1.))
    rho=spread*distance*min(UV_SCALE/(SOURCE[1]-SOURCE[0]),UV_SCALE/(SOURCE[3]-SOURCE[2]))*128
    require(rho>2,'Fixture does not guarantee a minified normal mip')
    return dict(profiles=summaries,independentMinimumTexelFootprint=rho,
        minimumUnclampedLod=math.log2(rho),authoredMaximumLod=1,
        sourceTextures={name:dict(bytes=(output/name).stat().st_size,format='RGBA8_UNORM authored two-level partial chain',
            dimensions=[[128,128],[64,64]],sha256=sha(output/name)) for name in ('normal.ktx','coverage.ktx')},
        degenerateUvFallbackLod=0,coverageCandidateLod=0,
        normalRgbRole='linear; decode 2*x-1 and normalize',
        coverageAlphaRole='linear; 128/255 survives nearest mip-zero cutoff .5, while erroneous sRGB decoding would reject it')


def metrics(captures):
    data=np.asarray(captures,dtype=float)
    require(data.shape==(len(FRAMES),63,55,3) and np.isfinite(data).all() and data.min()>=-1e-6,'Invalid receiver captures')
    return dict(mean=float(data.mean()),temporalRms=float(np.sqrt(np.var(data,axis=0).mean())))


def comparisons(frames):
    summary={name:metrics(values) for name,values in frames.items()}
    coarse=np.asarray(frames['normal-coarse']);scale=summary['normal-coarse']['mean']
    require(scale>THRESHOLDS['minimumFullMean'],'Receiver lacks a meaningful reflected-light signal')
    def equal(name,target,rmse_limit,mean_limit):
        a,b=np.asarray(frames[name]),np.asarray(frames[target]);den=summary[target]['mean']
        require(den>1e-5,'Baked target is black')
        rmse=float(np.sqrt(np.mean((a-b)**2))/den)
        mean=abs(summary[name]['mean']/den-1)
        require(rmse<=rmse_limit and mean<=mean_limit,'Filtered material does not match independent baked control: '+name)
        return dict(profile=name,bakedReference=target,normalizedRmse=rmse,relativeMeanError=mean)
    checks=[equal('normal-textured','normal-coarse',THRESHOLDS['normalNormalizedRmse'],THRESHOLDS['normalRelativeMeanError']),
            equal('normal-degenerate','normal-base',THRESHOLDS['normalNormalizedRmse'],THRESHOLDS['normalRelativeMeanError']),
            equal('alpha-textured','alpha-baked',THRESHOLDS['alphaNormalizedRmse'],THRESHOLDS['alphaRelativeMeanError'])]
    wrong=float(np.sqrt(np.mean((np.asarray(frames['normal-textured'])-np.asarray(frames['normal-base']))**2))/scale)
    require(wrong>THRESHOLDS['negativeNormalSeparation'],'Wrong base-mip normal is not a discriminating negative control')
    ratio=summary['alpha-baked']['mean']/scale
    require(THRESHOLDS['alphaMinimumFullFraction']<ratio<THRESHOLDS['alphaMaximumFullFraction'],'Cutout differs from expected partial visibility')
    require(summary['empty']['mean']<=THRESHOLDS['emptyRelativeMaximum']*scale+THRESHOLDS['emptyAbsoluteMaximum'],'Unexpected light leakage without bounce geometry')
    return dict(passed=True,profiles=summary,bakedChecks=checks,wrongBaseNormalSeparation=wrong,
        cutoutToFullMeanRatio=ratio,thresholds=THRESHOLDS,
        interpretation='Diffuse DDGI normal/coverage semantics; conservative mip-zero cutouts and authored closest-hit normal mips. Not general glossy reference filtering.')


def freeze(path,required=(),identity=None):
    require(identity is None or sha(path)==identity,'Freeze manifest identity changed')
    entries=json.loads(Path(path).read_text(encoding='utf-8-sig'))['files']
    files={Path(p).resolve():h.lower() for p,h in entries.items()}
    require(files and set(map(Path.resolve,required))<=files.keys(),'Freeze manifest omits consumed inputs')
    require(all(p.is_file() and sha(p)==h for p,h in files.items()),'Frozen runtime/helper/layer changed')
    return dict(path=str(path),sha256=sha(path),entryCount=len(files))


def layer_proof(log,library):
    paths=re.findall(r'Insert instance layer "VK_LAYER_KHRONOS_validation" \(([^\r\n]+)\)',log)
    require(any(Path(p).resolve()==library for p in paths),'Expected validation layer was not proven loaded')
    require(not re.search(r'VUID-|SYNC-HAZARD|Validation Error|\bhazard detected\b|'
                         r'\b(?:READ|WRITE)_AFTER_(?:READ|WRITE)\b[^\r\n]*\bhazard\b',log,re.I),'Strict Vulkan diagnostic')
    return dict(insertedLibrary=str(library))


def plan(output,models,renderer):
    result=[]
    for technique in TECHNIQUES:
        for profile in PROFILES:
            label=technique+'-'+profile;base=output/(label+'.exr');sequence=output/(label+'.sequence.json')
            write(sequence,dict(schemaVersion=1,frames=FRAMES[-1],sampleFrames=FRAMES,events=[]))
            command=[str(renderer),'--hidden','--no-ui','--validation','--no-bloom','--no-taa',
                '--width',str(WIDTH),'--height',str(HEIGHT),'--msaa','1','--fixed-dt','.016666667',
                '--model',models[profile],'--import-scale','1','--render-technique',technique,
                '--display-mode','lit','--exposure','1','--tone-map','reinhard',
                '--environment-intensity','0','--sky','plain','--sky-horizon','0','0','0',
                '--directional-intensity','8','--directional-direction',*map(str,-LIGHT),
                '--directional-color','1','1','1','--camera-position','0','0','4','--camera-target','0','0','0','--camera-fov','18',
                '--ddgi','--no-ddgi-async','--ddgi-origin','-1.5','-1.5','-.2','--ddgi-counts','6','6','4',
                '--ddgi-spacing','.6','.6','.4','--ddgi-rays','64','--ddgi-probes-per-frame','144','--ddgi-hysteresis','0',
                '--fsr','native','--dlss','native','--screen-reflections','off','--ray-shadows','hard',
                '--capture-sequence',str(sequence),'--screenshot',str(base),'--screenshot-viewport']
            images=[output/(label+(f'.frame-{f:04d}' if f!=FRAMES[-1] else '')+'.exr') for f in FRAMES]
            result.append(dict(name=label,technique=technique,profile=profile,command=command,images=list(map(str,images))))
    return dict(processCount=len(result),captureCount=sum(len(r['images']) for r in result),runs=result)


def self_check():
    scratch=Path(__file__).resolve().parents[2]/'build'/'codex-gltf-validation';scratch.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='normal-alpha-footprint-',dir=scratch) as temp:
        output=Path(temp);require(output.resolve().is_relative_to(scratch.resolve()),'Scratch escaped workspace')
        models=generate(output);positive=validate(output,models);rejected=[]
        def negative(name,callback):
            try: callback()
            except (RuntimeError,ValueError,AssertionError,KeyError,IndexError):rejected.append(name)
            else:raise RuntimeError('Oracle accepted corruption: '+name)
        original=(output/'normal.ktx').read_bytes();(output/'normal.ktx').write_bytes(original[:-4])
        negative('truncated-authored-mip',lambda:validate(output,models));(output/'normal.ktx').write_bytes(original)
        levels=decode_ktx(output/'normal.ktx');(output/'normal.ktx').write_bytes(ktx(levels[::-1]))
        negative('swapped-normal-mips',lambda:validate(output,models));(output/'normal.ktx').write_bytes(original)
        mask=(output/'coverage.ktx').read_bytes();levels=decode_ktx(output/'coverage.ktx');levels[1][:,:,3]=255
        (output/'coverage.ktx').write_bytes(ktx(levels));negative('coarse-alpha-sentinel',lambda:validate(output,models));(output/'coverage.ktx').write_bytes(mask)
        for label,profile,mutate in (
            ('emission-in-normal-fixture','normal-textured',lambda m:m['materials'][1].update(emissiveFactor=[1,1,1])),
            ('normal-binding','normal-textured',lambda m:m['materials'][1].pop('normalTexture')),
            ('changed-material-energy','normal-coarse',lambda m:m['materials'][1]['pbrMetallicRoughness'].update(baseColorFactor=[1,1,1,1])),
            ('alpha-mode','alpha-textured',lambda m:m['materials'][1].update(alphaMode='OPAQUE')),
            ('missing-cutout-strips','alpha-baked',lambda m:m['meshes'][0]['primitives'].pop()),
            ('source-missing','normal-coarse',lambda m:m['meshes'][0]['primitives'].pop())):
            path=Path(models[profile]);original=path.read_bytes();model=json.loads(original);mutate(model);write(path,model)
            negative(label,lambda:validate(output,models));path.write_bytes(original)
        rng=np.random.default_rng(820121);noise=rng.uniform(-.003,.003,(len(FRAMES),63,55,3))
        data={name:np.ones_like(noise)*(.04 if name in ('alpha-textured','alpha-baked') else .12 if name in ('normal-base','normal-degenerate') else 0 if name=='empty' else .08)+noise*(name!='empty') for name in PROFILES}
        stats=comparisons(data)
        for name,alter in (
            ('wrong-normal-lod',lambda d:d.__setitem__('normal-textured',d['normal-base'])),
            ('bad-invalid-gradient-fallback',lambda d:d.__setitem__('normal-degenerate',d['normal-coarse'])),
            ('alpha-holes-erased',lambda d:d.__setitem__('alpha-textured',d['normal-coarse'])),
            ('alpha-coarse-transparent',lambda d:d.__setitem__('alpha-textured',d['empty'])),
            ('environment-light-leak',lambda d:d.__setitem__('empty',d['normal-coarse'])),
            ('nonfinite-HDR',lambda d:d['normal-textured'].__setitem__((0,0,0,0),np.nan))):
            bad=copy.deepcopy(data);alter(bad);negative(name,lambda:comparisons(bad))
        inventory=plan(output,models,Path('renderer.exe'))
        require(inventory['processCount']==14 and inventory['captureCount']==112,'Native inventory changed')
        return dict(passed=True,gpuUsed=False,positive=positive,corruptionsRejected=rejected,
            syntheticReceiverChecks=stats,processCount=14,captureCount=112,thresholds=THRESHOLDS)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer',type=Path);parser.add_argument('--manifest',type=Path)
    parser.add_argument('--layer-manifest',type=Path);parser.add_argument('--output',type=Path)
    parser.add_argument('--self-check',action='store_true');parser.add_argument('--generate-only',action='store_true')
    parser.add_argument('--timeout',type=int,default=300)
    args=parser.parse_args()
    if args.self_check:
        receipt=self_check()
        if args.output:args.output.mkdir(parents=True,exist_ok=False);write(args.output/'self-check.json',receipt)
        print(json.dumps(receipt,indent=2));return
    require(args.output is not None,'--output is required');output=args.output.resolve();output.mkdir(parents=True,exist_ok=False)
    models=generate(output/'fixtures');proof=validate(output/'fixtures',models)
    inventory=plan(output,models,(args.renderer or Path('VulkanSceneRenderer.exe')).resolve())
    write(output/'fixtures.json',proof);write(output/'launch-plan.json',inventory);write(output/'thresholds.json',THRESHOLDS)
    if args.generate_only:print(json.dumps(dict(gpuUsed=False,processCount=14,captureCount=112)));return
    require(args.renderer and args.manifest and args.layer_manifest,'Native execution requires renderer/freeze/layer manifests')
    renderer=args.renderer.resolve();manifest=args.manifest.resolve();layer_manifest=args.layer_manifest.resolve()
    layer=json.loads(layer_manifest.read_text(encoding='utf-8'))['layer']
    require(layer['name']=='VK_LAYER_KHRONOS_validation' and layer['api_version']=='1.4.363','Unexpected validation layer/version')
    library=(layer_manifest.parent/layer['library_path']).resolve()
    required=[renderer,*renderer.parent.glob('*.dll'),*(renderer.parent/'spv_shaders').glob('*.spv'),
        Path(__file__).resolve(),Path(sys.modules[Asset.__module__].__file__).resolve(),layer_manifest,library]
    before=freeze(manifest,required)
    generated=[*map(Path,models.values()),output/'fixtures'/'normal.ktx',output/'fixtures'/'coverage.ktx',
               output/'launch-plan.json',output/'fixtures.json',output/'thresholds.json',*output.glob('*.sequence.json')]
    settings=output/'vk_layer_settings.txt';settings.write_text('khronos_validation.validate_sync = true\nkhronos_validation.duplicate_message_limit = 1000\n')
    generated.append(settings);input_hashes={str(p):sha(p) for p in generated}
    environment=dict(os.environ,VK_LAYER_PATH=str(layer_manifest.parent),VK_LAYER_SETTINGS_PATH=str(output),
        VK_LOADER_DEBUG='error,warn,info,layer,driver,debug')
    report=dict(passed=False,manifestBefore=before,rendererSha256=sha(renderer),generatedSha256=input_hashes,
        sourceDependencies={str(p):sha(p) for p in required if p.suffix=='.py'},fixtures=proof,thresholds=THRESHOLDS,runs=[],
        bandwidth='Hardware bandwidth counters unavailable; texture/ROI/owned allocation sizes and GPU dispatch timings are recorded, not hardware bandwidth.',
        approximation='Diffuse probe cones; closest-hit normal LOD is independent of conservative mip-zero candidate/shadow coverage. Not glossy reference filtering.')
    captured={technique:{} for technique in TECHNIQUES}
    comparison_camera=None
    def verify():
        freeze(manifest,required,identity=before['sha256'])
        require(all(sha(Path(p))==h for p,h in input_hashes.items()),'Generated inputs/settings changed')
    try:
        for run in inventory['runs']:
            verify();log_path=output/(run['name']+'.log')
            with log_path.open('w',encoding='utf-8') as stream:
                process=subprocess.run(run['command'],cwd=renderer.parent,env=environment,stdout=stream,
                    stderr=subprocess.STDOUT,timeout=args.timeout,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            require(process.returncode==0,'Native process failed: '+str(log_path))
            loaded=layer_proof(log_path.read_text(encoding='utf-8',errors='replace'),library)
            arrays=[];timings=[];artifacts=[log_path];samples=[]
            for image in map(Path,run['images']):
                pixels=cv2.imdecode(np.frombuffer(image.read_bytes(),np.uint8),cv2.IMREAD_UNCHANGED)
                require(pixels is not None and pixels.dtype==np.float32 and pixels.shape==(HEIGHT,WIDTH,3) and np.isfinite(pixels).all(),'Invalid native HDR image')
                arrays.append(pixels[65:128,80:135,::-1].astype(float))
                state_path=image.with_suffix('.telemetry.json');metadata_path=Path(str(image)+'.capture.json')
                state=json.loads(state_path.read_text());metadata=json.loads(metadata_path.read_text())
                require(state['technique']==run['technique'] and state['ddgi']['active'] and
                    state['ddgi']['tracedRays']==144*64,'Actual DDGI transport differs from plan')
                require(state['resolution']==[WIDTH,HEIGHT] and state['msaaSamples']==1 and not state['taa']['enabled'],'Native dimensions/temporal state differ')
                np.testing.assert_allclose(state['camera']['position'],[0,0,4],rtol=0,atol=1e-6)
                require(all(abs(v)<1e-8 for v in state['camera']['jitterUv']),'Comparison camera is jittered')
                matrix=state['camera']['unjitteredViewProjColumns']
                if comparison_camera is None:comparison_camera=matrix
                np.testing.assert_allclose(matrix,comparison_camera,rtol=0,atol=1e-6)
                require(all(not state[b]['active'] and state[b]['mode']=='native' for b in ('fsr','dlss')),'Upscaling entered raw comparison')
                require(metadata['source']=='raster-scene-composite' and metadata['colorSpace']=='linear-sRGB' and
                    not metadata['includesUi'] and not metadata['exposureApplied'] and not metadata['toneMappingApplied'],'Capture is not raw HDR')
                for key in ('gpu','driverVersion','vulkanApiVersion'):
                    if key in report:require(report[key]==state[key],'Hardware/driver changed')
                    report[key]=state[key]
                timing=next(p['gpuMs'] for p in state['passes'] if p['name']=='DdgiTrace')
                require(timing>0 and math.isfinite(timing),'Missing DDGI trace GPU timestamp');timings.append(timing)
                samples.append(dict(image=str(image),ddgi=state['ddgi'],scene=state['scene'],camera=state['camera'],traceGpuMs=timing))
                artifacts.extend((image,state_path,metadata_path))
            captured[run['technique']][run['profile']]=arrays
            report['runs'].append(dict(name=run['name'],technique=run['technique'],profile=run['profile'],layer=loaded,samples=samples,
                medianTraceGpuMs=statistics.median(timings),artifacts={str(p):sha(p) for p in artifacts}))
            write(output/'results.json',report);print(run['name']+': eight strict-clean raw frames',flush=True)
        verify()
        report['comparisons']={technique:comparisons(values) for technique,values in captured.items()}
        require(all(sha(Path(p))==h for r in report['runs'] for p,h in r['artifacts'].items()),'Native artifacts changed')
        report['manifestAfter']=freeze(manifest,required,identity=before['sha256'])
        report.update(passed=True,processes=len(report['runs']),captures=sum(len(r['samples']) for r in report['runs']),frozenInputsMatched=True)
    except Exception:
        report['failure']=traceback.format_exc();raise
    finally:write(output/'results.json',report)
    print('PASS 112 normal/alpha-footprint captures in 14 processes',flush=True)


if __name__=='__main__':main()
