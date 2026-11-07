"""Independent tiny MR scenes for reference convergence/camera-matched comparisons.

No renderer evaluator is imported. Asset is used only to serialize glTF accessors;
validation below decodes the serialized bytes independently.
"""
from __future__ import annotations

import argparse
import base64
import copy
import hashlib
import json
import math
from pathlib import Path
import struct

import numpy as np

from gltf_accessor_runtime_regression import Asset

NAMES = ("cornell", "area", "blocker", "roughness", "interior")
DOWN = [-math.sqrt(.5), 0., 0., math.sqrt(.5)]
ROUGHNESSES = (.1, .25, .5, .8)
EXPECTED_LIGHTS = dict(cornell=1, area=2, blocker=1, roughness=1, interior=2)
REQUIRED = dict(
    cornell=("room-back", "room-left", "room-right", "room-floor", "room-ceiling", "tall-box", "short-box"),
    area=("front-receiver",), blocker=("front-receiver", "penumbra-blocker"),
    roughness=("metal-sphere-0", "metal-sphere-1", "metal-sphere-2", "metal-sphere-3", "floor", "backdrop"),
    interior=("room-floor", "room-back", "partition-left", "partition-right", "doorway-lintel", "table-top", "table-leg", "bench"))


def require(value, message):
    if not value:
        raise ValueError(message)


class Builder:
    def __init__(self, name):
        self.asset = Asset()
        self.model = self.asset.model
        self.model.update(scene=0, scenes=[dict(name=name, nodes=[])], nodes=[],
                          extras=dict(referenceConvergenceFixture=name))
        self.model["asset"]["generator"] = "Container independent convergence geometry"
        self.model["extensionsUsed"] = ["KHR_lights_punctual"]
        self.model["extensions"] = dict(KHR_lights_punctual=dict(lights=[]))

    def material(self, name, color, metallic=0., roughness=.7):
        result = len(self.model["materials"])
        self.model["materials"].append(dict(name=name, pbrMetallicRoughness=dict(
            baseColorFactor=[*color, 1.], metallicFactor=metallic, roughnessFactor=roughness)))
        return result

    def primitive(self, name, positions, normals, indices, material):
        positions, normals = np.asarray(positions), np.asarray(normals)
        primitive = dict(mode=4, material=material, extras=dict(semantic=name), attributes=dict(
            POSITION=self.asset.accessor("VEC3", positions),
            NORMAL=self.asset.accessor("VEC3", normals),
            TEXCOORD_0=self.asset.accessor("VEC2", np.zeros((len(positions), 2)))),
            indices=self.asset.accessor("SCALAR", indices, 5123, indices=True))
        mesh = len(self.model["meshes"])
        self.model["meshes"].append(dict(name=name, primitives=[primitive]))
        self.model["scenes"][0]["nodes"].append(len(self.model["nodes"]))
        self.model["nodes"].append(dict(name=name, mesh=mesh))

    def quad(self, name, center, u, v, material):
        center, u, v = map(lambda row: np.asarray(row, dtype=float), (center, u, v))
        normal = np.cross(u, v); normal /= np.linalg.norm(normal)
        positions = [center-u-v, center+u-v, center+u+v, center-u+v]
        self.primitive(name, positions, [normal]*4, [0, 1, 2, 0, 2, 3], material)

    def box(self, name, center, half, material, yaw=0.):
        x, y, z = half
        rotation = np.array([[math.cos(yaw), 0, math.sin(yaw)], [0, 1, 0], [-math.sin(yaw), 0, math.cos(yaw)]])
        for label, offset, u, v in (
            ("right", (x,0,0), (0,0,-z), (0,y,0)), ("left", (-x,0,0), (0,0,z), (0,y,0)),
            ("top", (0,y,0), (x,0,0), (0,0,-z)), ("bottom", (0,-y,0), (x,0,0), (0,0,z)),
            ("front", (0,0,z), (x,0,0), (0,y,0)), ("back", (0,0,-z), (-x,0,0), (0,y,0))):
            self.quad(name+"-"+label, np.asarray(center)+rotation@offset, rotation@u, rotation@v, material)

    def sphere(self, name, center, radius, material):
        # Smooth vertex normals, explicit nondegenerate polar caps.
        segments, rings = 24, 12
        normals = [[0., 1., 0.]]
        for j in range(1, rings):
            theta = math.pi*j/rings
            for k in range(segments):
                phi = 2*math.pi*k/segments
                normals.append([math.sin(theta)*math.cos(phi), math.cos(theta), math.sin(theta)*math.sin(phi)])
        normals.append([0., -1., 0.]); bottom = len(normals)-1
        triangles = []
        for k in range(segments):
            triangles.append([0, 1+k, 1+(k+1)%segments])
        for j in range(rings-2):
            for k in range(segments):
                a=1+j*segments+k; b=1+j*segments+(k+1)%segments
                triangles.extend(([a,b,b+segments], [a,b+segments,a+segments]))
        start=1+(rings-2)*segments
        for k in range(segments): triangles.append([bottom,start+(k+1)%segments,start+k])
        normals=np.asarray(normals); positions=np.asarray(center)+radius*normals
        for triangle in triangles:
            p=positions[triangle]
            if np.dot(np.cross(p[1]-p[0],p[2]-p[0]),normals[triangle].sum(axis=0))<0:
                triangle[1],triangle[2]=triangle[2],triangle[1]
        self.primitive(name,positions,normals,np.asarray(triangles).reshape(-1),material)

    def light(self, name, position, *, shape="rect", size=(.65,.45), intensity=10., color=(1.,1.,1.), down=False):
        lights=self.model["extensions"]["KHR_lights_punctual"]["lights"]
        area=dict(shape=shape,width=size[0],height=size[1],range=0.)
        if shape=="disk": area["radius"]=size[0]/2
        lights.append(dict(name=name,type="point",color=list(color),intensity=intensity,extras=dict(areaLight=area)))
        node=dict(name=name,translation=list(position),extensions=dict(KHR_lights_punctual=dict(light=len(lights)-1)))
        if down: node["rotation"]=DOWN
        self.model["scenes"][0]["nodes"].append(len(self.model["nodes"]))
        self.model["nodes"].append(node)

    def room(self, half, material, left=None, right=None):
        x,y,z=half
        self.quad("room-back",(0,0,-z),(x,0,0),(0,y,0),material)
        self.quad("room-left",(-x,0,0),(0,0,-z),(0,y,0),material if left is None else left)
        self.quad("room-right",(x,0,0),(0,0,z),(0,y,0),material if right is None else right)
        self.quad("room-floor",(0,-y,0),(x,0,0),(0,0,-z),material)
        self.quad("room-ceiling",(0,y,0),(x,0,0),(0,0,z),material)


def generate(output: Path) -> dict:
    output=Path(output).resolve(); output.mkdir(parents=True,exist_ok=True)
    result={}
    for name in NAMES:
        path=output/f"{name}.gltf"
        if path.exists(): raise FileExistsError(path)
        b=Builder(name)
        white=b.material("matte-white",(.68,.68,.68),roughness=.8)
        red=b.material("matte-red",(.62,.055,.04),roughness=.8)
        green=b.material("matte-green",(.055,.42,.085),roughness=.8)
        wood=b.material("matte-ochre",(.4,.22,.09),roughness=.65)
        camera=[0.,.05,3.8]; target=[0.,-.05,0.]
        if name=="cornell":
            b.room((1.,1.,1.),white,red,green)
            b.box("tall-box",(-.4,-.4,-.25),(.24,.6,.24),white,.3)
            b.box("short-box",(.4,-.68,.3),(.27,.32,.27),wood,-.25)
            b.light("ceiling-rect",(0,.985,0),down=True,size=(.65,.5),intensity=14.)
            description="Open-front colored five-wall Cornell room, two closed boxes, ceiling rectangle."
        elif name=="area":
            b.quad("front-receiver",(0,0,0),(1.65,0,0),(0,1.15,0),white)
            b.light("front-rect",(-.7,.48,1.35),size=(.65,.45),intensity=7.,color=(1.,.84,.67))
            b.light("front-disk",(.72,-.32,1.05),shape="disk",size=(.65,.65),intensity=5.,color=(.65,.8,1.))
            description="Front-facing diffuse receiver illuminated by distinct rectangular and circular emitters."
        elif name=="blocker":
            b.quad("front-receiver",(0,0,0),(1.65,0,0),(0,1.15,0),white)
            b.box("penumbra-blocker",(.12,-.08,.47),(.22,.38,.14),wood,.2)
            b.light("oblique-rect",(-.85,.67,1.45),size=(.85,.6),intensity=10.)
            description="Obliquely placed finite rectangle and closed box create umbra/penumbra on a receiver."
        elif name=="roughness":
            for i,r in enumerate(ROUGHNESSES):
                material=b.material(f"metal-r{r:g}",(.82,.64,.3),metallic=1.,roughness=r)
                b.sphere(f"metal-sphere-{i}",(-1.17+.78*i,.02,0.),.32,material)
            b.quad("floor",(0,-.3,0),(1.7,0,0),(0,0,-1.05),white)
            b.quad("backdrop",(0,.45,-.7),(1.7,0,0),(0,.75,0),white)
            b.light("highlight-rect",(-.35,1.,1.25),size=(.9,.45),intensity=12.)
            camera=[0.,.25,4.]; target=[0.,.05,0.]
            description="Four smooth gold spheres with roughness .1/.25/.5/.8 and spatially varying highlights."
        else:
            b.room((1.55,1.1,1.5),white)
            # A partition crosses the room, leaving a real central doorway.
            b.box("partition-left",(-1.02,-.05,-.35),(.53,1.05,.07),wood)
            b.box("partition-right",(1.02,-.05,-.35),(.53,1.05,.07),wood)
            b.box("doorway-lintel",(0,.8,-.35),(.49,.3,.07),wood)
            b.box("table-top",(-.35,-.35,.55),(.56,.055,.32),wood)
            for i,(x,z) in enumerate(((-.78,.32),(.08,.32),(-.78,.78),(.08,.78))):
                b.box(f"table-leg-{i}",(x,-.745,z),(.045,.35,.045),wood)
            b.box("bench",(.95,-.78,.75),(.2,.32,.5),green)
            b.box("back-cabinet",(-.85,-.7,-1.15),(.35,.4,.2),red)
            b.light("ceiling-front",(-.45,1.08,.65),down=True,size=(.65,.5),intensity=12.)
            b.light("ceiling-back",(.45,1.08,-.95),down=True,size=(.6,.45),intensity=10.)
            camera=[.2,.12,4.6]; target=[0.,-.12,-.15]
            description="Open-front interior with partition doorway, table, four legs, bench, cabinet and two ceiling emitters."
        b.asset.write(path)
        result[name]=dict(path=str(path),cameraPosition=camera,cameraTarget=target,cameraFov=45,
                          expectedAreaLights=EXPECTED_LIGHTS[name],description=description)
        validate(path,name)
    return result


def validate(path: Path, name: str) -> dict:
    """Independently decode serialized geometry; reject corrupt or weakened fixtures."""
    require(name in NAMES,"Unknown convergence fixture")
    path=Path(path).resolve(); model=json.loads(path.read_text(encoding="utf-8"))
    require(model.get("asset",{}).get("version")=="2.0","Expected glTF 2.0")
    require(model.get("extras",{}).get("referenceConvergenceFixture")==name,"Fixture identity mismatch")
    require(set(model.get("extensionsUsed",[]))=={"KHR_lights_punctual"},"Unexpected material/runtime extension")
    require(not any(model.get(key) for key in ("textures","images","animations","skins")),"Fixture must stay texture/animation/layer free")
    buffers=[]
    for row in model["buffers"]:
        uri=row["uri"]; require(uri.startswith("data:application/octet-stream;base64,"),"Expected embedded buffer")
        payload=base64.b64decode(uri.split(",",1)[1],validate=True)
        require(len(payload)==row["byteLength"],"Buffer byteLength mismatch"); buffers.append(payload)

    def accessor(index, kind, component):
        a=model["accessors"][index]
        require(a["type"]==kind and a["componentType"]==component and not a.get("normalized") and not a.get("sparse"),"Unexpected accessor encoding")
        width={"SCALAR":1,"VEC2":2,"VEC3":3}[kind]; code,size={5123:("H",2),5126:("f",4)}[component]
        v=model["bufferViews"][a["bufferView"]]; payload=buffers[v["buffer"]]
        offset=v.get("byteOffset",0)+a.get("byteOffset",0); stride=v.get("byteStride",width*size)
        count=a["count"]; end=offset+(count-1)*stride+width*size
        require(count>0 and stride>=width*size and offset>=v.get("byteOffset",0) and end<=v.get("byteOffset",0)+v["byteLength"]<=len(payload),"Accessor outside buffer view")
        values=np.array([struct.unpack_from("<"+code*width,payload,offset+i*stride) for i in range(count)])
        require(np.isfinite(values).all(),"Non-finite accessor")
        if kind=="VEC3":
            require(np.allclose(a.get("min"),values.min(axis=0),atol=1e-6,rtol=0) and np.allclose(a.get("max"),values.max(axis=0),atol=1e-6,rtol=0),"Accessor bounds mismatch")
        return values

    materials=model["materials"]
    for material in materials:
        require(not material.get("extensions") and material.get("alphaMode","OPAQUE")=="OPAQUE" and not material.get("doubleSided",False),"Unsupported material behavior")
        mr=material["pbrMetallicRoughness"]
        values=np.array([*mr["baseColorFactor"],mr["metallicFactor"],mr["roughnessFactor"]])
        require(np.isfinite(values).all() and np.all((values>=0)&(values<=1)) and values[3]==1,"Invalid MR factors")
        require(not any("Texture" in field for field in mr),"Unexpected texture")
    rootnodes=model["scenes"][model["scene"]]["nodes"]
    require(len(rootnodes)==len(model["nodes"]) and len(set(rootnodes))==len(rootnodes),"All fixture nodes must be active roots")
    require(all(not node.get("children") for node in model["nodes"]),"Unexpected scene hierarchy")
    semantics=[]; positions=[]; triangle_count=0; mesh_nodes=[]; sphere_materials=[]; geometry={}
    for node in model["nodes"]:
        if "mesh" not in node: continue
        require(not any(key in node for key in ("matrix","translation","rotation","scale")),"Geometry must use explicit world coordinates")
        mesh_nodes.append(node["mesh"])
        for primitive in model["meshes"][node["mesh"]]["primitives"]:
            semantic=primitive.get("extras",{}).get("semantic","")
            require(semantic==node.get("name") and semantic,"Missing semantic primitive name"); semantics.append(semantic)
            require(primitive.get("mode",4)==4,"Expected triangles")
            p=accessor(primitive["attributes"]["POSITION"],"VEC3",5126)
            n=accessor(primitive["attributes"]["NORMAL"],"VEC3",5126)
            uv=accessor(primitive["attributes"]["TEXCOORD_0"],"VEC2",5126)
            index=accessor(primitive["indices"],"SCALAR",5123).reshape(-1)
            require(len(p)==len(n)==len(uv) and np.allclose(np.linalg.norm(n,axis=1),1,atol=2e-6,rtol=0),"Normals must be finite unit vectors")
            require(len(index)%3==0 and int(index.max())<len(p),"Invalid triangle indices")
            index=index.reshape((-1,3)); crosses=np.cross(p[index[:,1]]-p[index[:,0]],p[index[:,2]]-p[index[:,0]])
            require(np.all(np.linalg.norm(crosses,axis=1)>1e-8),"Degenerate triangle")
            require(np.all(np.einsum("ij,ij->i",crosses,n[index].sum(axis=1))>1e-8),"Winding disagrees with shading normals")
            require(0<=primitive["material"]<len(materials),"Invalid primitive material")
            if semantic.startswith("metal-sphere-"):
                mr=materials[primitive["material"]]["pbrMetallicRoughness"]
                require(mr["metallicFactor"]==1 and np.all(np.ptp(n,axis=0)>1.9),"Roughness objects must be smooth metal spheres")
                sphere_materials.append(mr["roughnessFactor"])
            positions.append(p); triangle_count+=len(index)
            geometry[semantic]=(p,index)
    require(len(mesh_nodes)==len(model["meshes"])==len(set(mesh_nodes)),"Every mesh must be used exactly once")
    require(len(semantics)==len(set(semantics)),"Duplicate semantic primitives")
    for expected in REQUIRED[name]:
        require(any(s==expected or s.startswith(expected+"-") for s in semantics),"Missing semantic geometry "+expected)
    faces=("right","left","top","bottom","front","back")
    boxes=sorted({s.rsplit("-",1)[0] for s in semantics if s.rsplit("-",1)[-1] in faces and not s.startswith("room-")})
    for box in boxes:
        require(all(box+"-"+face in geometry for face in faces),"Closed box missing face: "+box)
        edges={}; volume=0.
        for face in faces:
            p,indices=geometry[box+"-"+face]
            for triangle in indices:
                xyz=p[triangle]
                volume+=float(np.dot(xyz[0],np.cross(xyz[1],xyz[2])))/6
                vertices=[tuple(np.round(row,6)) for row in xyz]
                for a,b in zip(vertices,vertices[1:]+vertices[:1]): edges[(a,b)]=edges.get((a,b),0)+1
        require(volume>1e-6 and all(count==1 and edges.get((b,a))==1 for (a,b),count in edges.items()),"Box must be a closed outward manifold: "+box)
    all_positions=np.concatenate(positions)
    extents=np.ptp(all_positions,axis=0)
    require(np.max(np.abs(all_positions))<5 and np.count_nonzero(extents>0)>=(2 if name=="area" else 3),"Unbounded or flattened scene")
    if name=="roughness": require(sorted(sphere_materials)==list(ROUGHNESSES),"Missing authored roughness class")
    lights=model["extensions"]["KHR_lights_punctual"]["lights"]
    require(len(lights)==EXPECTED_LIGHTS[name],"Incorrect area-light count")
    light_nodes=[node for node in model["nodes"] if "KHR_lights_punctual" in node.get("extensions",{})]
    require(sorted(node["extensions"]["KHR_lights_punctual"]["light"] for node in light_nodes)==list(range(len(lights))),"Light instances must be one-to-one")
    shapes=[]
    for node in light_nodes:
        light=lights[node["extensions"]["KHR_lights_punctual"]["light"]]; area=light["extras"]["areaLight"]
        require(light["type"]=="point" and light["intensity"]>0 and np.isfinite(light["intensity"]),"Invalid area-light carrier")
        require(area["shape"] in ("rect","disk") and area["range"]==0 and area["width"]>0 and area["height"]>0,"Invalid area-light dimensions")
        require(np.isfinite([*node["translation"],area["width"],area["height"],*light["color"]]).all(),"Non-finite light")
        if name in ("cornell","interior"):
            require(np.allclose(node.get("rotation"),DOWN,atol=1e-12,rtol=0),"Ceiling lights must face down -Y")
        else: require("rotation" not in node,"Front lights must retain default -Z orientation")
        shapes.append(area["shape"])
    if name=="area": require(sorted(shapes)==["disk","rect"],"Area scene must cover both emitter shapes")
    return dict(name=name,path=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
        primitiveCount=len(semantics),triangleCount=triangle_count,areaLightCount=len(lights),
        closedBoxes=boxes,
        boundsMin=all_positions.min(axis=0).tolist(),boundsMax=all_positions.max(axis=0).tolist(),
        roughnessClasses=sorted(sphere_materials),geometryValidated=True)


def self_check(output: Path) -> dict:
    output=Path(output).resolve(); output.mkdir(parents=True,exist_ok=True)
    fixtures=generate(output/"positive")
    positive={name:validate(row["path"],name) for name,row in fixtures.items()}
    original=json.loads(Path(fixtures["roughness"]["path"]).read_text(encoding="utf-8"))
    negatives=[]
    def reject(label,mutate):
        model=copy.deepcopy(original); mutate(model); path=output/f"bad-{label}.gltf"
        path.write_text(json.dumps(model),encoding="utf-8")
        try: validate(path,"roughness")
        except (ValueError,KeyError,IndexError,TypeError,struct.error): negatives.append(label)
        else: raise AssertionError("Corrupt fixture accepted: "+label)
    reject("truncated-buffer",lambda m:m["buffers"][0].update(byteLength=m["buffers"][0]["byteLength"]+1))
    reject("accessor-range",lambda m:m["accessors"][0].update(count=1000000))
    reject("bounds",lambda m:m["accessors"][0].update(min=[-100,-100,-100]))
    reject("normal-type",lambda m:m["accessors"][1].update(componentType=5123))
    reject("missing-metal",lambda m:m["materials"][-1]["pbrMetallicRoughness"].update(metallicFactor=0))
    reject("roughness-class",lambda m:m["materials"][-1]["pbrMetallicRoughness"].update(roughnessFactor=.7))
    reject("unsupported-layer",lambda m:m["materials"][0].update(extensions={"KHR_materials_sheen":{}}))
    reject("semantic",lambda m:m["meshes"][0]["primitives"][0]["extras"].update(semantic="unknown"))
    reject("inactive-mesh",lambda m:m["scenes"][0]["nodes"].pop(0))
    reject("light-count",lambda m:m["extensions"]["KHR_lights_punctual"]["lights"].append(copy.deepcopy(m["extensions"]["KHR_lights_punctual"]["lights"][0])))
    reject("light-direction",lambda m:m["nodes"][-1].update(rotation=DOWN))
    def corrupt_geometry(m,kind):
        a=m["accessors"][0 if kind=="position" else 1 if kind=="normal" else 3]
        v=m["bufferViews"][a["bufferView"]]; payload=bytearray(base64.b64decode(m["buffers"][0]["uri"].split(",",1)[1]))
        offset=v.get("byteOffset",0)+a.get("byteOffset",0)
        if kind=="position": struct.pack_into("<f",payload,offset,float("nan"))
        elif kind=="normal": struct.pack_into("<fff",payload,offset,0.,0.,0.)
        elif kind=="winding":
            i,j,k=struct.unpack_from("<HHH",payload,offset); struct.pack_into("<HHH",payload,offset,i,k,j)
        else:
            i=struct.unpack_from("<H",payload,offset)[0]; struct.pack_into("<HHH",payload,offset,i,i,i)
        m["buffers"][0]["uri"]="data:application/octet-stream;base64,"+base64.b64encode(payload).decode()
    for label in ("position","normal","winding","degenerate"):
        reject(label,lambda m,label=label:corrupt_geometry(m,label))
    # Remove only one box face while retaining the root node/mesh tables: this
    # specifically exercises closure rather than active-node or bounds checks.
    blocker=json.loads(Path(fixtures["blocker"]["path"]).read_text(encoding="utf-8"))
    blocker["meshes"][1]["primitives"]=[]
    path=output/"bad-open-blocker.gltf"; path.write_text(json.dumps(blocker),encoding="utf-8")
    try: validate(path,"blocker")
    except ValueError: negatives.append("open-blocker")
    else: raise AssertionError("Open blocker accepted")
    return dict(passed=True,positive=positive,negativeCases=negatives,negativeCount=len(negatives),gpuUsed=False)


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output",type=Path); parser.add_argument("--self-check",action="store_true")
    args=parser.parse_args()
    print(json.dumps(self_check(args.output) if args.self_check else generate(args.output),indent=2))
