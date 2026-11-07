"""Measure selected silhouette stroke width in framebuffer pixels."""
from pathlib import Path
import argparse
import numpy as np
from PIL import Image
from gltf_accessor_runtime_regression import Asset,NativeRun,mesh,require
from gltf_texture_formats_runtime_regression import verify_manifest
from gltf_extended_materials_runtime_regression import validation_log_error

CASES=[
 ('landscape',640,360,3,[1,1,1],1),
 ('wide',1280,360,3,[1,1,1],1),
 ('portrait',360,640,3,[1,1,1],1),
 ('square',512,512,3,[1,1,1],1),
 ('nonuniform',640,360,3,[1.6,.6,1],1),
 ('distant',640,360,5,[1,1,1],1),
 ('msaa4',640,360,3,[1,1,1],4)]

def fixture(path,scale):
 a=Asset()
 mesh(a,a.accessor('VEC3',[[-.65,-.45,0],[.65,-.45,0],[.65,.45,0],[-.65,.45,0]]),
      a.accessor('VEC3',[[0,0,1]]*4),a.accessor('VEC2',[[0,1],[1,1],[1,0],[0,0]]))
 a.model['nodes'][0]['scale']=scale
 a.model['materials']=[dict(pbrMetallicRoughness=dict(baseColorFactor=[.015,.04,1,1]),
                           extensions=dict(KHR_materials_unlit={}))]
 a.model['extensionsUsed']=['KHR_materials_unlit'];a.write(path)

def measure(record,w,h,msaa):
 require(record['state']['msaaSamples']==msaa,'Requested MSAA mode did not execute')
 a=np.asarray(Image.open(record['image']).convert('RGB'))
 require(a.shape==(h,w,3),'Wrong capture extent')
 r,g,b=a[:,:,0].astype(float),a[:,:,1].astype(float),a[:,:,2].astype(float)
 fill=(b>120)&(b>2*r)&(b>2*g)
 stroke=(r>120)&(g>60)&(b<100)
 yy,xx=np.where(fill);require(len(xx)>500,'Missing selected rectangle')
 y0,y1,x0,x1=int(yy.min()),int(yy.max()),int(xx.min()),int(xx.max())
 xc,yc=(x0+x1)//2,(y0+y1)//2
 # Median three interior scanlines to avoid rasterized corner samples.
 widths={}
 for name,rows in [('left',[stroke[yc+i,:x0] for i in (-1,0,1)]),
                   ('right',[stroke[yc+i,x1+1:] for i in (-1,0,1)]),
                   ('top',[stroke[:y0,xc+i] for i in (-1,0,1)]),
                   ('bottom',[stroke[y1+1:,xc+i] for i in (-1,0,1)])]:
  widths[name]=float(np.median([np.count_nonzero(row) for row in rows]))
 require(all(width==5 for width in widths.values()) if msaa==1 else
         max(widths.values())-min(widths.values())<=1 and all(4<=width<=6 for width in widths.values()),
         f'Unequal/non-five-pixel outline: {widths}')
 return dict(widthsPixels=widths,fillBounds=[x0,y0,x1,y1],extent=[w,h],msaa=record['state']['msaaSamples'])

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--renderer',required=True,type=Path);p.add_argument('--output',required=True,type=Path)
 p.add_argument('--manifest',type=Path);p.add_argument('--case',action='append',choices=[x[0] for x in CASES])
 p.add_argument('--technique',action='append',choices=['deferred-raster','forward-raster'])
 args=p.parse_args();run=NativeRun(args.renderer,args.output,180)
 if args.manifest:run.report['nativeManifestBefore']=verify_manifest(args.manifest)
 try:
  for technique in args.technique or ['deferred-raster','forward-raster']:
   for name,w,h,d,scale,msaa in CASES:
    if args.case and name not in args.case:continue
    path=run.output/(name+'.gltf');fixture(path,scale)
    actual=run.capture(path,technique+'-'+name,technique,extra=['--width',str(w),'--height',str(h),'--msaa',str(msaa),
        '--camera-position','0','0',str(d)])
    require(not validation_log_error(Path(actual['log']).read_text(encoding='utf-8',errors='replace')),
            f'Vulkan validation failed: {actual["log"]}')
    run.report['checks'].append(dict(case=name,technique=technique,check='equal-horizontal-vertical-stroke',**measure(actual,w,h,msaa)))
    run.save();print('PASS outline',technique,name,run.report['checks'][-1]['widthsPixels'],flush=True)
  if args.manifest:run.report['nativeManifestAfter']=verify_manifest(args.manifest)
  run.finish()
 except Exception as e:run.report['failure']=str(e);raise
 finally:
  run.report.update(captureCount=len(run.report['captures']),checkCount=len(run.report['checks']));run.save()
if __name__=='__main__':main()
