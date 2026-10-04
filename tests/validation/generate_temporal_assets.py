"""Generate our MIT-licensed, self-contained temporal fixtures (stdlib only)."""
import base64
import copy
import json
import math
import pathlib
import struct
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEST = ROOT / "models/validation"


def write(name, value):
    (DEST / name).write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def checker():
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    rows = b"".join(b"\0" + b"".join(bytes((240, 240, 240, 255 if (x // 2 + y // 2) % 2 else 0)) for x in range(16)) for y in range(16))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 16, 16, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
    return "data:image/png;base64," + base64.b64encode(png).decode()


def box(center, extent):
    positions, normals, uvs, indices = [], [], [], []
    for normal, corners in [
        ((0, 0, 1), [(-1,-1,1),(1,-1,1),(1,1,1),(-1,1,1)]),
        ((0, 0,-1), [(1,-1,-1),(-1,-1,-1),(-1,1,-1),(1,1,-1)]),
        ((1, 0, 0), [(1,-1,1),(1,-1,-1),(1,1,-1),(1,1,1)]),
        ((-1,0, 0), [(-1,-1,-1),(-1,-1,1),(-1,1,1),(-1,1,-1)]),
        ((0, 1, 0), [(-1,1,1),(1,1,1),(1,1,-1),(-1,1,-1)]),
        ((0,-1, 0), [(-1,-1,-1),(1,-1,-1),(1,-1,1),(-1,-1,1)])]:
        start = len(positions)
        positions.extend([[center[a] + corner[a] * extent[a] * .5 for a in range(3)] for corner in corners])
        normals.extend([normal] * 4)
        uvs.extend([(0,0),(1,0),(1,1),(0,1)])
        indices.extend([start, start+1, start+2, start, start+2, start+3])
    return positions, normals, uvs, indices


def scene(transparent):
    data = bytearray()
    doc = {"asset":{"version":"2.0","generator":"VulkanSceneRenderer temporal fixture generator; MIT"}, "buffers":[], "bufferViews":[], "accessors":[], "meshes":[], "nodes":[], "scenes":[{"nodes":[0]}], "scene":0}
    doc["materials"] = [
        {"name":"Neutral", "pbrMetallicRoughness":{"baseColorFactor":[.4,.5,.62,1],"metallicFactor":0,"roughnessFactor":.7}},
        {"name":"Specular", "pbrMetallicRoughness":{"baseColorFactor":[.7,.18,.04,1],"metallicFactor":.65,"roughnessFactor":.15}},
        {"name":"Bright edges", "pbrMetallicRoughness":{"baseColorFactor":[.95,.95,.95,1],"metallicFactor":0,"roughnessFactor":.6}},
        {"name":"Mask", "alphaMode":"MASK", "alphaCutoff":.5, "doubleSided":True, "pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicFactor":0,"roughnessFactor":.5}},
        {"name":"Glass", "alphaMode":"BLEND", "doubleSided":True, "pbrMetallicRoughness":{"baseColorFactor":[.12,.75,.55,.35],"metallicFactor":0,"roughnessFactor":.3}},
        {"name":"Emitter", "emissiveFactor":[4,1,.1],"pbrMetallicRoughness":{"baseColorFactor":[.6,.2,.01,1],"metallicFactor":0,"roughnessFactor":.5}}
    ]
    doc["images"] = [{"uri":checker()}]
    doc["textures"] = [{"source":0}]
    doc["nodes"].append({"name":"Temporal root","children":[]})
    geometry = []
    def accessor(values, component, kind):
        while len(data) % 4: data.append(0)
        offset = len(data)
        flatten = [x for row in values for x in row] if isinstance(values[0], (list,tuple)) else values
        data.extend(struct.pack("<" + ("f" if component == 5126 else "I") * len(flatten), *flatten))
        view = len(doc["bufferViews"])
        doc["bufferViews"].append({"buffer":0,"byteOffset":offset,"byteLength":len(data)-offset})
        result = {"bufferView":view,"componentType":component,"count":len(values),"type":kind}
        if kind == "VEC3":
            result.update(min=[min(row[a] for row in values) for a in range(3)],max=[max(row[a] for row in values) for a in range(3)])
        doc["accessors"].append(result)
        return len(doc["accessors"])-1
    def add(name, center, extent, material=0, angle=0):
        p,n,uv,i = box(center,extent)
        if angle:
            def rotate(v): return [v[0]*math.cos(angle)-v[1]*math.sin(angle),v[0]*math.sin(angle)+v[1]*math.cos(angle),v[2]]
            p = [rotate(v) for v in p]; n = [rotate(v) for v in n]
        primitive = {"attributes":{"POSITION":accessor(p,5126,"VEC3"),"NORMAL":accessor(n,5126,"VEC3"),"TEXCOORD_0":accessor(uv,5126,"VEC2")}, "indices":accessor(i,5125,"SCALAR"),"material":material}
        doc["meshes"].append({"name":name,"primitives":[primitive]})
        doc["nodes"][0]["children"].append(len(doc["nodes"]))
        doc["nodes"].append({"name":name,"mesh":len(doc["meshes"])-1})
        geometry.append((name,p,i))
    add("Floor",(0,-.12,0),(7,.2,7))
    add("Back wall",(0,1.8,-2.2),(7,4,.15))
    add("Moving cube",(0,.6,0),(.9,1.2,.9),1)
    for j in range(12): add("Thin edge " + str(j),(-2+j*.12,.5,1),(.018,1.8,.04),2,.12)
    if transparent:
        add("Alpha checker",(1.35,.9,.3),(1.1,1.8,.01),3)
        add("Glass front",(.5,.9,1.3),(1.2,1.8,.01),4)
        add("Glass back",(.65,.9,1.1),(1.2,1.8,.01),4)
        add("Emitter",(2.4,1.7,-1),(.3,.3,.3),5)
    doc["buffers"] = [{"byteLength":len(data),"uri":"data:application/octet-stream;base64,"+base64.b64encode(data).decode()}]
    return doc, geometry


def main():
    DEST.mkdir(parents=True, exist_ok=True)
    doc, _ = scene(True); write("taa_scene.gltf", doc)
    coverage = copy.deepcopy(doc)
    cube = next(node for node in coverage["nodes"] if node.get("name") == "Moving cube")
    cube.update(scale=[-1,1,1], translation=[-1,0,0])
    material = copy.deepcopy(coverage["materials"][1]); material["doubleSided"] = True
    coverage["materials"].append(material)
    mesh = copy.deepcopy(coverage["meshes"][cube["mesh"]])
    mesh["primitives"][0]["material"] = len(coverage["materials"])-1
    coverage["meshes"].append(mesh)
    coverage["nodes"][0]["children"].append(len(coverage["nodes"]))
    coverage["nodes"].append({"name":"Double sided cube","mesh":len(coverage["meshes"])-1,"translation":[1,0,0]})
    write("taa_coverage.gltf",coverage)
    doc, geometry = scene(False); write("taa_equivalent.gltf", doc)
    bim = {"schema_version":"1.1.0","meshes":[],"elements":[],"info":{"georeference":{"sourceUpAxis":"Z"}}}
    usd = ['#usda 1.0','(upAxis = "Y" metersPerUnit = 1)', 'def Xform "TemporalRoot" {']
    for index,(name,points,indices) in enumerate(geometry):
        bim["meshes"].append({"mesh_id":index,"coordinates":[x for p in points for x in (p[0],-p[2],p[1])],"indices":indices})
        bim["elements"].append({"mesh_id":index,"guid":f"taa-{index:04}","type":"IfcBuildingElementProxy","vector":{"x":0,"y":0,"z":0},"rotation":{"qx":0,"qy":0,"qz":0,"qw":1},"color":{"r":102,"g":128,"b":158,"a":255},"info":{"Name":name}})
        usd.extend([f'def Mesh "mesh{index}" {{', 'point3f[] points = ['+', '.join('('+','.join(map(str,p))+')' for p in points)+']', 'int[] faceVertexCounts = ['+','.join(['3']*(len(indices)//3))+']', 'int[] faceVertexIndices = ['+','.join(map(str,indices))+']', 'uniform token subdivisionScheme = "none"','color3f[] primvars:displayColor = [(0.4,0.5,0.62)]','bool doubleSided = false','}'])
    usd.append('}')
    write("taa_equivalent.bim", bim)
    (DEST/"taa_equivalent.usda").write_text('\n'.join(usd)+'\n',encoding='utf-8')
    camera = [{"frame":1,"position":[0,1.8,6.5],"target":[0,.8,0]}]
    common = {"schemaVersion":1,"frames":64,"sampleFrames":[1,8,16,32,64],"camera":camera}
    write("taa_static.json",common)
    write("taa_reset.json",dict(common,frames=32,sampleFrames=[16,17,18,32],events=[{"frame":17,"reset":True},{"frame":18,"exposure":.5}]))
    pan = dict(common, sampleFrames=list(range(25,41))+[64], camera=[dict(camera[0],position=[-.35,1.8,6.5],target=[-.35,.8,0]),{"frame":64,"position":[.35,1.8,6.5],"target":[.35,.8,0]}])
    write("taa_pan.json",pan)
    motion = dict(common,objectName="Moving cube",sampleFrames=[1,16,24,32,40,48,56,64],objectTranslation=[{"frame":1,"translation":[-1,0,0]},{"frame":32,"translation":[1,0,0]},{"frame":48,"translation":[1,0,0]},{"frame":64,"translation":[-1,0,0]}])
    write("taa_object.json",motion)
    write("taa_transparency.json",dict(motion,objectName="Glass front"))
    write("taa_emissive.json",dict(motion,objectName="Emitter"))
    write("taa_provider_events.json",dict(common,events=[{"frame":12,"bimHiddenObject":2},{"frame":20,"bimHiddenObject":4294967295},{"frame":26,"bimLodBias":2},{"frame":34,"sectionPlane":[1,0,0,0]},{"frame":42,"sectionPlane":[1,0,0,4]},{"frame":50,"reset":True}],sampleFrames=[1,12,20,26,34,42,50,64]))
    write("taa_provider_motion.json",dict(common,sampleFrames=[1,16,32,48,64],bimTranslation=[{"frame":1,"translation":[-.5,0,0]},{"frame":32,"translation":[.5,0,0]},{"frame":64,"translation":[0,0,0]}]))
    events = [{"frame":4,"acquireOutOfDate":True},{"frame":6,"presentSuboptimal":True},{"frame":8,"skip":True},{"frame":9,"reset":True},{"frame":12,"resize":[800,450]},{"frame":15,"resize":[640,360]},{"frame":18,"minimized":True},{"frame":20,"minimized":False,"reset":True},{"frame":23,"taa":False},{"frame":26,"taa":True},{"frame":29,"technique":"forward-raster"},{"frame":32,"orthographic":True},{"frame":35,"orthographic":False},{"frame":38,"taa":False,"samples":4},{"frame":41,"samples":1,"taa":True},{"frame":44,"reload":"models/validation/taa_scene.gltf"}]
    write("taa_lifecycle.json",dict(common,frames=48,sampleFrames=[7,9,12,15,17,20,23,26,29,32,35,38,41,44,48],events=events))


if __name__ == "__main__": main()
