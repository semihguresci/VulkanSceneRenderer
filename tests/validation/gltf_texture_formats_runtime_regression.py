"""Native typed texture formats, precision, mips, alternates, alpha and reload.

Ten generated format cases give 60 captures across both raster techniques.
The default hard-ray/DDGI alpha witness adds 6. --roundtrip adds 12 captures for
three selected cases, with GLB then ASCII reload after owned external images
are deleted. --generate-only never starts the renderer. NumPy/Pillow required.

PNG16 precision uses dark samples and emissive amplification; its independent
reference is analytical linear RGBA32F KTX2. Normal-map precision uses tiny
16-bit offsets around 0.5 and an independent float reference. Authored mip
levels have unrelated colors, so regenerating from the base level fails.
Official ETC1S/UASTC cyan fixtures have a constant-cyan PNG oracle, with bounded
transcode tolerance. No production decoder/evaluator produces a reference.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import math
from pathlib import Path
import struct
import zlib

import numpy as np

from gltf_accessor_runtime_regression import (Asset, NativeRun, TECHNIQUES,
    load_asset, mesh, pixels, png_bytes, require)
from gltf_vertex_alpha_runtime_regression import add_receiver, shadow_metrics

CASES = ('png16-srgb', 'png16-normal', 'hdr', 'dds-mips', 'ktx1-mips', 'ktx2-mips',
         'webp-mask', 'webp-blend', 'basis-etc1s', 'basis-uastc')
ROUNDTRIP_CASES = ('png16-srgb', 'webp-mask', 'basis-uastc')
RGBA = np.array([[[48, 128, 192, 65535], [192, 48, 128, 65535]],
                 [[128, 192, 48, 65535], [96, 160, 224, 65535]]], dtype=np.uint16)
NORMAL16 = np.array([[[32780, 32896, 32768, 65535], [32896, 32780, 32768, 65535]],
                     [[32704, 32864, 32769, 65535], [32864, 32704, 32769, 65535]]], dtype=np.uint16)
HDR = np.array([[[8., 2., .5, 1.], [.25, 4., 1., 1.]],
                [[1., .5, 4., 1.], [4., .25, 2., 1.]]], dtype=np.float32)
WEBP = bytes.fromhex('5249464620000000574542505650384c130000002f010000100f3067b3357f5319f0a84044ff0300')
# WebP1.6.0 lossless encoder input {(17,29,53,0),(91,103,127,149)}.
# Transparent RGB is canonicalized to zero by the encoder; visible RGB/alpha
# below is independently known and never obtained by calling the tested codec.
WEBP_RGBA = np.array([[[0, 0, 0, 0], [91, 103, 127, 149]]], dtype=np.uint8)
CYAN = np.tile([0, 255, 255, 255], (2, 2, 1)).astype(np.uint8)
FALLBACK = np.tile([235, 25, 15, 255], (2, 2, 1)).astype(np.uint8)
KTX1_ID = b'\xabKTX 11\xbb\r\n\x1a\n'
KTX2_ID = b'\xabKTX 20\xbb\r\n\x1a\n'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def verify_manifest(path):
    document = json.loads(path.read_text(encoding='utf-8-sig'))
    files = document.get('files',{})
    require(isinstance(files,dict) and files, 'Native freeze manifest has no files')
    for name,expected in files.items():
        item = Path(name)
        require(item.is_file() and sha(item.read_bytes())==expected.lower(), f'Frozen native input changed: {item}')
    return dict(path=str(path.resolve()),sha256=sha(path.read_bytes()),entryCount=len(files),verified=True)


def png16_bytes(values):
    a = np.asarray(values, dtype='>u2')
    require(a.ndim == 3 and a.shape[2] in (1, 3, 4), 'PNG16 fixture channel count')
    def chunk(kind, payload):
        return struct.pack('>I', len(payload))+kind+payload+struct.pack('>I', zlib.crc32(kind+payload))
    rows = b''.join(b'\0'+row.tobytes() for row in a)
    return (b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>IIBBBBB', a.shape[1], a.shape[0],
        16, {1: 0, 3: 2, 4: 6}[a.shape[2]], 0, 0, 0))+chunk(b'IDAT', zlib.compress(rows))+chunk(b'IEND', b''))


def eotf(values):
    a = np.asarray(values, dtype=np.float32)
    return np.where(a <= np.float32(.04045), a/np.float32(12.92),
                    ((a+np.float32(.055))/np.float32(1.055))**np.float32(2.4)).astype(np.float32)


def hdr_bytes(values):
    a = np.asarray(values, dtype=np.float32)
    out = bytearray(f'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {a.shape[0]} +X {a.shape[1]}\n'.encode())
    # Old/raw RGBE is valid for scanlines narrower than 8; all selected values
    # are exactly representable powers of two, avoiding a circular codec oracle.
    for p in a.reshape((-1, 4)):
        maximum = float(np.max(p[:3]))
        mantissa, exponent = math.frexp(maximum)
        scale = mantissa*256/maximum
        out.extend([int(float(v)*scale) for v in p[:3]]+[exponent+128])
    return bytes(out)


def metadata(key, value):
    text = key.encode()+b'\0'+value.encode()+b'\0'
    return struct.pack('<I', len(text))+text+bytes((-len(text)) % 4)


def ktx2_bytes(levels, *, floating=False, swizzle=None, data_role=False):
    levels = [np.asarray(level, dtype='<f4' if floating else np.uint8) for level in levels]
    h, w, channels = levels[0].shape
    require(channels == 4 and all(level.shape == (max(1, h >> i), max(1, w >> i), 4)
            for i, level in enumerate(levels)), 'Invalid independent KTX2 mip dimensions')
    scalar, texel = (4, 16) if floating else (1, 4)
    samples = bytearray()
    for c in range(4):
        channel = (15 if c == 3 else c) | (0xc0 if floating else 0) | (0x10 if c == 3 else 0)
        samples.extend(struct.pack('<HBB4BII', c*scalar*8, scalar*8-1, channel, 0, 0, 0, 0,
                                   0, 0x3f800000 if floating else 255))
    basic = struct.pack('<IHH4B4B8B', 0, 2, 24+len(samples), 1, 0 if data_role else 1,
                        1 if floating or data_role else 2, 0, 0, 0, 0, 0, texel, 0, 0, 0, 0, 0, 0, 0)+samples
    dfd = struct.pack('<I', 4+len(basic))+basic
    dfd_offset = 80+24*len(levels)
    kv = metadata('KTXswizzle', swizzle) if swizzle else b''
    kv_offset = dfd_offset+len(dfd) if kv else 0
    out = bytearray(KTX2_ID+struct.pack('<13I2Q', 109 if floating else 43, scalar, w, h, 0, 0, 1,
        len(levels), 0, dfd_offset, len(dfd), kv_offset, len(kv), 0, 0)+bytes(24*len(levels))+dfd+kv)
    for i in reversed(range(len(levels))):
        out.extend(bytes((-len(out)) % texel))
        payload = levels[i].tobytes()
        struct.pack_into('<3Q', out, 80+24*i, len(out), len(payload), len(payload))
        out.extend(payload)
    return bytes(out)


def bc1_level(size, color565):
    return struct.pack('<HHI', color565, 0, 0)*(((size+3)//4)**2)


def dds_mips():
    header = bytearray(128)
    for offset, value in ((0, 0x20534444), (4, 124), (8, 0xa1007), (12, 64), (16, 64),
                          (20, 2048), (28, 3), (76, 32), (80, 4), (84, 0x31545844), (108, 0x401008)):
        struct.pack_into('<I', header, offset, value)
    return bytes(header)+b''.join(bc1_level(size, color) for size, color in ((64, 0xf800), (32, 0x07e0), (16, 0x001f)))


def ktx1_mips():
    kv = metadata('KTXorientation', 'S=r,T=d')+metadata('KTXswizzle', 'gbr1')
    out = bytearray(KTX1_ID+struct.pack('<13I', 0x04030201, 0, 1, 0, 0x83f1, 0x1908,
                                      64, 64, 0, 0, 1, 3, len(kv))+kv)
    for size, color in ((64, 0xf800), (32, 0x07e0), (16, 0x001f)):
        payload = bc1_level(size, color)
        out.extend(struct.pack('<I', len(payload))+payload)
    return bytes(out)


def pattern(color, size=2):
    return np.tile(color, (size, size, 1)).astype(np.uint8)


def texture_spec(case, *, enabled=True):
    role, strength, mode = 'base', 1., 'OPAQUE'
    if case == 'png16-srgb':
        linear = RGBA.astype(np.float32)/np.float32(65535)
        linear[:, :, :3] = eotf(linear[:, :, :3])
        source = png16_bytes(RGBA)
        reference = ktx2_bytes([linear], floating=True)
        dropped = png_bytes((RGBA >> 8).astype(np.uint8))
        role, strength = 'emissive', 256.
        return dict(source=(source, 'png', 'image/png'), reference=(reference, 'ktx2', 'image/ktx2'),
                    dropped=(dropped, 'png', 'image/png'), role=role, strength=strength, mode=mode, tolerance=.1)
    if case == 'png16-normal':
        source, reference = png16_bytes(NORMAL16), ktx2_bytes([NORMAL16.astype(np.float32)/np.float32(65535)], floating=True, data_role=True)
        return dict(source=(source, 'png', 'image/png'), reference=(reference, 'ktx2', 'image/ktx2'),
                    dropped=(png_bytes((NORMAL16 >> 8).astype(np.uint8)), 'png', 'image/png'), role='normal', strength=1., mode=mode, tolerance=.12)
    if case == 'hdr':
        return dict(source=(hdr_bytes(HDR), 'hdr', 'image/vnd.radiance'),
                    reference=(ktx2_bytes([HDR], floating=True), 'ktx2', 'image/ktx2'),
                    dropped=(png_bytes(np.rint(np.clip(HDR, 0, 1)*255).astype(np.uint8)), 'png', 'image/png'),
                    role='emissive', strength=.125, mode=mode, tolerance=.1)
    if case.endswith('-mips'):
        if case == 'dds-mips': source, ext, mime = dds_mips(), 'dds', 'image/vnd-ms.dds'
        elif case == 'ktx1-mips': source, ext, mime = ktx1_mips(), 'ktx', 'image/ktx'
        else:
            source, ext, mime = ktx2_bytes([pattern([255,0,0,255],64), pattern([0,255,0,255],32), pattern([0,0,255,255],16)]), 'ktx2', 'image/ktx2'
        # KTX1's gbr1 swizzle maps authored blue→green and base red→blue.
        last = [0,255,0,255] if case == 'ktx1-mips' else [0,0,255,255]
        first = [0,0,255,255] if case == 'ktx1-mips' else [255,0,0,255]
        return dict(source=(source,ext,mime), reference=(png_bytes(pattern(last)), 'png','image/png'),
                    dropped=(png_bytes(pattern(first)), 'png','image/png'), role=role, strength=strength, mode=mode,
                    extension='MSFT_texture_dds' if case == 'dds-mips' else None, repeat=32, tolerance=.03)
    if case.startswith('webp-'):
        actual = png_bytes(WEBP_RGBA)
        fallback = png_bytes(FALLBACK)
        reference = (actual if enabled else fallback,'png','image/png')
        blend = case.endswith('blend')
        if blend and enabled:
            # Independent opaque composite oracle: one transparent surface over
            # the known unlit blue receiver. This bypasses BLEND in the reference
            # and catches the same alpha mistake in two textured BLEND shaders.
            alpha = WEBP_RGBA[:, :, 3:4].astype(np.float32)/np.float32(255)
            rgb = eotf(WEBP_RGBA[:, :, :3].astype(np.float32)/np.float32(255))
            mixed = rgb*alpha+np.array([.015,.04,1.],dtype=np.float32)*(1-alpha)
            opaque = np.concatenate((mixed,np.ones((*mixed.shape[:2],1),dtype=np.float32)),axis=2)
            reference = (ktx2_bytes([opaque],floating=True),'ktx2','image/ktx2')
        return dict(source=(WEBP,'webp','image/webp'), reference=reference,
                    dropped=(fallback if enabled else actual,'png','image/png'), role=role, strength=strength,
                    mode='BLEND' if blend else 'MASK', referenceMode='OPAQUE' if blend and enabled else None,
                    # Deferred opaque albedo is UNORM8; transparent composition
                    # is HDR. The independent opaque oracle can differ by one
                    # display code after that intermediate quantization.
                    extension='EXT_texture_webp', tolerance=1. if blend else .05)
    require(case in ('basis-etc1s','basis-uastc'), f'Unknown format fixture {case}')
    fixture = Path(__file__).resolve().parents[1]/'fixtures'/'texture-codecs'/('cyan_rgb_reference_basis.ktx2' if case.endswith('etc1s') else 'cyan_rgb_reference_uastc.ktx2')
    # These official fixtures encode constant cyan. The independently authored
    # uncompressed cyan_rgba_reference_u.ktx2 in the same upstream release has
    # every RGBA texel(0,255,255,255), verified without invoking a codec.
    return dict(source=(fixture.read_bytes(),'ktx2','image/ktx2'),
                reference=(png_bytes(CYAN if enabled else FALLBACK),'png','image/png'),
                dropped=(png_bytes(FALLBACK if enabled else CYAN),'png','image/png'), role=role, strength=strength,
                mode=mode, extension='KHR_texture_basisu', tolerance=2. if case.endswith('etc1s') else 1.)


def write_asset(path, spec, kind, *, shadow=False, clear=False):
    a = Asset()
    positions = [[-.9,-.8,.7],[.9,-.8,.7],[.9,.8,.7],[-.9,.8,.7]] if shadow else [[-1.2,-.9,0],[1.2,-.9,0],[1.2,.9,0],[-1.2,.9,0]]
    repeat = spec.get('repeat', 1)
    mesh(a, a.accessor('VEC3', positions), a.accessor('VEC3', [[0.,0.,1.]]*4),
         a.accessor('VEC2', [[0.,repeat],[repeat,repeat],[repeat,0.],[0.,0.]]),
         tangent=a.accessor('VEC4', [[1.,0.,0.,1.]]*4))
    encoded, extension, mime = spec[kind]
    external = []
    if kind == 'source':
        image_path = path.with_suffix('.'+extension)
        image_path.write_bytes(encoded)
        external.append(image_path)
        image = dict(uri=image_path.name, mimeType=mime)
    else: image = dict(uri=f'data:{mime};base64,'+base64.b64encode(encoded).decode())
    a.model['images'] = [image]
    a.model['samplers'] = [dict(minFilter=9984 if repeat>1 else 9728, magFilter=9728,
                                wrapS=10497 if repeat>1 else 33071, wrapT=10497 if repeat>1 else 33071)]
    a.model['textures'] = [dict(source=0, sampler=0)]
    used = []
    if kind == 'source' and spec.get('extension'):
        # Deliberately different core source: matching cyan/cutout must come
        # from the selected alternate, not accidental fallback or a missing map.
        a.model['images'] = [dict(uri='data:image/png;base64,'+base64.b64encode(png_bytes(FALLBACK)).decode()), image]
        a.model['textures'][0]['extensions'] = {spec['extension']: dict(source=1)}
        used.append(spec['extension'])
    mode = spec.get('referenceMode') if kind=='reference' and not shadow else None
    material = dict(name='Typed image probe', alphaMode='MASK' if shadow else mode or spec['mode'], alphaCutoff=.3,
                    pbrMetallicRoughness=dict(baseColorFactor=[1,1,1,1], metallicFactor=0, roughnessFactor=1))
    pbr = material['pbrMetallicRoughness']
    if spec['role'] == 'emissive':
        pbr['baseColorFactor'] = [0,0,0,1]
        material.update(emissiveFactor=[1,1,1], emissiveTexture=dict(index=0),
                        extensions=dict(KHR_materials_emissive_strength=dict(emissiveStrength=spec['strength'])))
        used.append('KHR_materials_emissive_strength')
    elif spec['role'] == 'normal': material['normalTexture'] = dict(index=0)
    else:
        pbr['baseColorTexture'] = dict(index=0)
        material['extensions'] = dict(KHR_materials_unlit={})
        used.append('KHR_materials_unlit')
    a.model['materials'] = [material]
    a.model['extensionsUsed'] = used
    if shadow or spec['mode'] in ('MASK','BLEND'):
        add_receiver(a, shadow=shadow)
        if not shadow and spec['mode'] == 'BLEND':
            # The independent opaque oracle bakes this blue receiver behind
            # every probe texel. Enlarge it in X to cover the nearer quad after
            # perspective projection, including fully transparent edge texels.
            a.model['nodes'][-1]['scale'] = [1.2, 1., 1.]
        if clear: a.model['scenes'][0]['nodes'] = [len(a.model['nodes'])-1]
    a.write(path)
    return dict(path=str(path), external=[str(p) for p in external], encodedSha256=sha(encoded),
                encodedBytes=len(encoded), mime=mime)


def difference(a, b):
    left, right = pixels(a), pixels(b)
    # Camera z3/fov60 and authored quad give a large stable interior. Keep
    # geometric edges/background out of the precision and codec tolerances.
    crop = np.abs(left[100:260,208:432]-right[100:260,208:432])
    return dict(meanAbsolutePixelError=float(np.abs(left-right).mean()), interiorMeanAbsolutePixelError=float(crop.mean()),
                interiorP99PixelError=float(np.quantile(crop,.99)), changedPixelFraction=float(np.mean(np.max(crop,axis=2)>=8)))


def compare_format(actual, reference, tolerance, *, negative=False):
    result = difference(actual, reference)
    if negative:
        require(result['interiorMeanAbsolutePixelError'] > 1. and result['changedPixelFraction'] > .02,
                f'Format negative control did not visibly differ: {result}')
    else:
        require(result['meanAbsolutePixelError'] <= max(.03,tolerance) and
                result['interiorMeanAbsolutePixelError'] <= tolerance and result['interiorP99PixelError'] <= max(2.,tolerance*3),
                f'Typed image differs from independent reference: {result}')
    return result


def mask_coverage(record):
    image = pixels(record)
    left, right = image[115:245,220:305], image[115:245,335:420]
    blue = lambda values: (values[:,:,2]>values[:,:,0]+60)&(values[:,:,2]>values[:,:,1]+60)
    clear_fraction, covered_fraction = float(blue(left).mean()), float((~blue(right)).mean())
    require(clear_fraction>.995 and covered_fraction>.995,
            f'Compressed MASK failed independent left-clear/right-covered coverage: {clear_fraction}, {covered_fraction}')
    return dict(leftClearFraction=clear_fraction,rightCoveredFraction=covered_fraction)


def retained_image_hashes(path):
    model, buffers = load_asset(path)
    hashes = []
    for image in model.get('images', []):
        if 'bufferView' in image:
            view = model['bufferViews'][image['bufferView']]
            at = view.get('byteOffset',0)
            encoded = buffers[view['buffer']][at:at+view['byteLength']]
        else:
            uri = image.get('uri','')
            require(uri.startswith('data:'), 'Saved image still references an external resource')
            encoded = base64.b64decode(uri.split(',',1)[1])
        hashes.append(sha(encoded))
    return hashes


def delete_owned_external(paths, output):
    deleted = []
    for text in sorted(set(paths)):
        path = Path(text).resolve()
        require(path.is_relative_to(output) and path.parent == output and path.is_file(),
                f'Refusing to delete a resource outside this fixture directory: {path}')
        path.unlink()
        require(not path.exists(), f'External source resource survived deletion: {path}')
        deleted.append(str(path))
    return deleted


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--suite', action='append', choices=('formats','alpha-ray'))
    parser.add_argument('--case', action='append', choices=CASES)
    parser.add_argument('--technique', action='append', choices=TECHNIQUES)
    parser.add_argument('--optional-codecs', choices=('enabled','disabled'), default='enabled')
    parser.add_argument('--roundtrip', action='store_true')
    parser.add_argument('--roundtrip-case', action='append', choices=CASES)
    parser.add_argument('--manifest',type=Path,help='Verify the frozen executable/SPIRV manifest before and after native work')
    parser.add_argument('--generate-only', action='store_true')
    parser.add_argument('--timeout', type=float, default=180)
    args = parser.parse_args()
    run = NativeRun(args.renderer,args.output,args.timeout)
    run.report.update(mode='generate-only' if args.generate_only else 'native', optionalCodecs=args.optional_codecs,
                      deletedExternalImages=[], unverified=['GPU picking has no native capture action'])
    if args.manifest: run.report['nativeManifestBefore']=verify_manifest(args.manifest)
    suites = args.suite or ('formats','alpha-ray')
    techniques = args.technique or TECHNIQUES
    def checkpoint():
        run.report.update(captureCount=len(run.report['captures']), checkCount=len(run.report['checks']))
        run.save()
    try:
        if 'formats' in suites:
            for case in args.case or CASES:
                spec = texture_spec(case,enabled=args.optional_codecs=='enabled')
                files = {kind:run.output/f'{case}-{kind}.gltf' for kind in ('source','reference','dropped')}
                info = {kind:write_asset(path,spec,kind) for kind,path in files.items()}
                run.report['fixtures'].append(dict(case=case,info=info,tolerance=spec['tolerance']))
                if args.generate_only: continue
                rounds = []
                for technique in techniques:
                    stem = technique+'-'+case
                    roundtrip = args.roundtrip and case in (args.roundtrip_case or ROUNDTRIP_CASES)
                    destination = run.output/(stem+'-saved.glb') if roundtrip else None
                    lit = spec['role']=='normal'
                    # Forward has no normal G-buffer debug view. Its directional
                    # lighting still gives an independent normal-map witness.
                    extra = ['--display-mode','normals'] if lit and technique=='deferred-raster' else []
                    actual = run.capture(files['source'],stem+'-source',technique,destination=destination,extra=extra,lit=lit)
                    reference = run.capture(files['reference'],stem+'-reference',technique,extra=extra,lit=lit)
                    control = run.capture(files['dropped'],stem+'-dropped',technique,extra=extra,lit=lit)
                    run.report['checks'].append(dict(case=case,technique=technique,check='independent-format-reference',
                        **compare_format(actual,reference,spec['tolerance'])))
                    run.report['checks'].append(dict(case=case,technique=technique,check='format-negative-control',
                        **compare_format(actual,control,spec['tolerance'],negative=True)))
                    if case=='webp-mask' and args.optional_codecs=='enabled':
                        run.report['checks'].append(dict(case=case,technique=technique,check='analytical-mask-coverage',**mask_coverage(actual)))
                    if roundtrip:
                        require(info['source']['encodedSha256'] in retained_image_hashes(destination),
                                'GLB export changed/lost the exact encoded source image')
                        rounds.append((technique,actual,destination,extra,lit))
                    checkpoint()
                    print('PASS formats',case,technique,flush=True)
                if rounds:
                    run.report['deletedExternalImages'] += delete_owned_external(info['source']['external'],run.output)
                    for technique,actual,glb,extra,lit in rounds:
                        ascii_path = glb.with_suffix('.gltf')
                        reload_glb = run.capture(glb,glb.stem+'-reload',technique,destination=ascii_path,extra=extra,lit=lit)
                        require(info['source']['encodedSha256'] in retained_image_hashes(ascii_path),
                                'ASCII export changed/lost the exact encoded source image')
                        reload_ascii = run.capture(ascii_path,ascii_path.stem+'-ascii-reload',technique,extra=extra,lit=lit)
                        for format_name,record in (('glb',reload_glb),('gltf',reload_ascii)):
                            run.report['checks'].append(dict(case=case,technique=technique,check='retained-'+format_name+'-reload-after-external-delete',
                                **compare_format(actual,record,.03)))
                        checkpoint()
        if 'alpha-ray' in suites and (not args.case or any(case.startswith('webp-') for case in args.case)):
            if args.optional_codecs == 'disabled':
                run.report['unverified'].append('Compressed-alpha ray witness requires the enabled WebP codec')
            else:
                spec = texture_spec('webp-mask')
                files = {kind:run.output/f'webp-alpha-ray-{kind}.gltf' for kind in ('source','reference','clear')}
                info = {kind:write_asset(path,spec,'reference' if kind=='clear' else kind,shadow=True,clear=kind=='clear')
                        for kind,path in files.items()}
                run.report['fixtures'].append(dict(case='webp-alpha-ray',info=info))
                if not args.generate_only:
                    extra = ['--ray-shadows','hard','--no-ray-shadow-denoise','--directional-intensity','3',
                             '--directional-direction','-.7','0','-1','--ddgi','--ddgi-intensity','0',
                             '--ddgi-fit-scene','--ddgi-fit-probes','64','--ddgi-rays','32',
                             '--ddgi-probes-per-frame','64','--ddgi-ray-budget','2048','--ddgi-hysteresis','0']
                    for technique in techniques:
                        records = {kind:run.capture(path,f'{technique}-webp-alpha-ray-{kind}',technique,extra=extra,lit=True)
                                   for kind,path in files.items()}
                        state = records['source']['state']
                        require(state['rayShadows']['supported'] and state['rayShadows']['active'],'Compressed-alpha hard ray shadows inactive')
                        require(state['ddgi']['supported'] and state['ddgi']['active'] and state['ddgi']['tracedRays']>0,
                                'Compressed-alpha DDGI candidate traversal did not execute')
                        run.report['checks'].append(dict(case='webp-alpha-ray',technique=technique,check='independent-alpha-image-and-ray-receiver',
                            **compare_format(records['source'],records['reference'],.08),
                            receiver=shadow_metrics(records['source'],records['clear']),ddgiTracedRays=state['ddgi']['tracedRays']))
                        checkpoint()
                        print('PASS compressed-alpha ray',technique,flush=True)
        if args.manifest: run.report['nativeManifestAfter']=verify_manifest(args.manifest)
        if args.generate_only: run.report['passed']=True
        else: run.finish()
    except Exception as error:
        run.report['failure']=str(error)
        raise
    finally: checkpoint()
    print(f"PASS: {len(run.report['checks'])} checks, {len(run.report['captures'])} captures; {run.output}")


if __name__ == '__main__':
    main()
