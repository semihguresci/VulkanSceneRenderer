#!/usr/bin/env python3
"""Read robotics IFC and IFCX exports with OpenUSD and optionally exercise Isaac PhysX."""

import argparse
import importlib.util
import math
from pathlib import Path
import subprocess
import sys
import tempfile
from time import monotonic


FIXTURE = Path(__file__).resolve().parents[1] / "fixtures/robotics/lidar-corner.ifc"
SOURCE_NAMESPACE = "usd-corner-check"
TOLERANCE = 2e-5
# Expected source class and direct spatial parent, keyed by the IFC STEP ID.
PRODUCTS = {
    3: ("IFCPROJECT", None),
    17: ("IFCSLAB", 40),
    27: ("IFCWALL", 40),
    37: ("IFCWALL", 40),
    40: ("IFCBUILDINGSTOREY", 41),
    41: ("IFCBUILDING", 42),
    42: ("IFCSITE", 3),
}
# Authored millimetre fixture converted independently to REP-103 metres.
BOUNDS = {
    17: ((-6, -6, -.2), (6, 6, 0)),
    27: ((4, -2, 0), (4.25, 3.25, 3)),
    37: ((-2, 3, 0), (4, 3.25, 3)),
}
ORIGINS = {17: (0, 0, -.2), 27: (4.125, .625, 0), 37: (1, 3.125, 0)}
RAYS = ((27, (1., 0., 0.), 4.), (37, (0., 1., 0.), 3.))


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def check_vector(value, expected, label):
    require(value is not None and len(value) == len(expected) and all(
        math.isclose(actual, target, abs_tol=TOLERANCE, rel_tol=0.)
        for actual, target in zip(value, expected)
    ), f"{label}: expected {expected}, got {value}")


def export_stage(exporter, source, destination, namespace, *options):
    run = subprocess.run(
        [str(exporter), str(source), str(destination), namespace, *options],
        capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120,
    )
    require(run.returncode == 0,
            f"Export {destination.name} failed ({run.returncode}):\n{run.stdout}{run.stderr}")
    return destination


def export_corner(exporter, directory):
    # Rotate the Y wall's product frame about Z and swap the profile dimensions
    # to keep its analytic bounds. Give the X wall an authored, opaque red style.
    fixture = directory / FIXTURE.name
    source = FIXTURE.read_text(encoding="utf-8").replace(
        "#31=IFCAXIS2PLACEMENT3D(#30,#5,#6);", "#31=IFCAXIS2PLACEMENT3D(#30,#5,#50);"
    ).replace(
        "#33=IFCRECTANGLEPROFILEDEF(.AREA.,$,$,250.,6000.);",
        "#33=IFCRECTANGLEPROFILEDEF(.AREA.,$,$,6000.,250.);"
    ).replace("ENDSEC;\nEND-ISO-10303-21;", """#50=IFCDIRECTION((0.,1.,0.));
#51=IFCCOLOURRGB($,1.,0.,0.);
#52=IFCSURFACESTYLESHADING(#51,0.);
#53=IFCSURFACESTYLE('Red wall',.BOTH.,(#52));
#54=IFCSTYLEDITEM(#24,(#53),$);
ENDSEC;
END-ISO-10303-21;""")
    fixture.write_text(source, encoding="utf-8")

    stages = []
    for name, scale in (("original", "1"), ("scaled", "2.5"), ("repeat", "2.5")):
        stages.append(export_stage(exporter, fixture, directory / f"{name}.usda", SOURCE_NAMESPACE,
                                   "--import-scale", scale))
    require(stages[1].read_bytes() == stages[2].read_bytes(),
            "Identical IFC exports differ byte-for-byte")
    return stages


def mesh_triangles(prim, transforms):
    from pxr import Gf, UsdGeom

    mesh = UsdGeom.Mesh(prim)
    points = mesh.GetPointsAttr().Get()
    counts = mesh.GetFaceVertexCountsAttr().Get()
    indices = mesh.GetFaceVertexIndicesAttr().Get()
    path = prim.GetPath()
    require(points and counts and indices and all(count == 3 for count in counts),
            f"{path} is missing its triangle mesh")
    valid, reason = UsdGeom.Mesh.ValidateTopology(indices, counts, len(points))
    require(valid, f"{path} has invalid topology: {reason}")
    require(mesh.GetSubdivisionSchemeAttr().Get() == "none", f"{path} applies subdivision")
    world_from_mesh = transforms.GetLocalToWorldTransform(prim)
    vertices = [world_from_mesh.Transform(Gf.Vec3d(p)) for p in points]
    return mesh, vertices, indices


def check_mesh(prim, transforms, source_id):
    from pxr import Gf

    mesh, vertices, indices = mesh_triangles(prim, transforms)
    path = prim.GetPath()
    minimum, maximum = BOUNDS[source_id]
    check_vector(tuple(min(p[i] for p in vertices) for i in range(3)), minimum, f"{path} minimum")
    check_vector(tuple(max(p[i] for p in vertices) for i in range(3)), maximum, f"{path} maximum")

    center = (Gf.Vec3d(*minimum) + Gf.Vec3d(*maximum)) / 2.
    winding = 1. if mesh.GetOrientationAttr().Get() == "rightHanded" else -1.
    for i in range(0, len(indices), 3):
        a, b, c = (vertices[indices[i + j]] for j in range(3))
        normal = Gf.Cross(b - a, c - a)
        require(winding * Gf.Dot(normal, (a + b + c) / 3. - center) > 0.,
                f"{path} has a degenerate or inward triangle at face {i // 3}")


def validate_stage(stage):
    from pxr import Gf, UsdGeom, UsdPhysics, UsdShade

    require(stage, "OpenUSD did not open the export")
    world = stage.GetDefaultPrim()
    require(world and str(world.GetPath()) == "/World", "Missing /World default prim")
    require(UsdGeom.StageHasAuthoredMetersPerUnit(stage) and UsdGeom.GetStageMetersPerUnit(stage) == 1.,
            "Expected authored stage metersPerUnit=1")
    require(stage.HasAuthoredMetadata("upAxis") and UsdGeom.GetStageUpAxis(stage) == UsdGeom.Tokens.z,
            "Expected authored Z up axis")
    require(world.GetAttribute("robotics:sourceNamespace").Get() == SOURCE_NAMESPACE,
            "Source namespace did not round trip")
    diagnostics = world.GetAttribute("robotics:diagnostics").Get()
    require(diagnostics and any("unsupported-semantic-class [#3]" in item for item in diagnostics),
            "Expected an explicit diagnostic for the project without navigation semantics")
    physics_scene = UsdPhysics.Scene.Get(stage, "/World/PhysicsScene")
    require(physics_scene, "Missing authored physics scene")

    products = {}
    identity = {}
    for prim in stage.Traverse():
        guid = prim.GetAttribute("ifc:guid").Get()
        if not guid:
            continue
        provenance = prim.GetAttribute("ifc:sourceId").Get()
        require(isinstance(provenance, str) and provenance.startswith("#") and provenance[1:].isdigit(),
                f"{prim.GetPath()} has invalid STEP provenance {provenance}")
        source_id = int(provenance[1:])
        require(source_id in PRODUCTS and source_id not in products and guid == f"{source_id:022d}",
                f"{prim.GetPath()} has duplicate or incorrect identity {provenance} / {guid}")
        products[source_id] = prim
        entity_id = prim.GetAttribute("robotics:entityId").Get()
        require(entity_id and prim.IsA(UsdGeom.Xform), f"{guid} lacks a semantic Xform or stable ID")
        identity[source_id] = (entity_id, str(prim.GetPath()))
        source_class, _ = PRODUCTS[source_id]
        actual_class = prim.GetAttribute("ifc:class").Get()
        require(actual_class and actual_class.upper() == source_class, f"{guid} lost IFC class")
        kind = prim.GetAttribute("robotics:kind").Get()
        labels = prim.GetAttribute("semantics:labels:class").Get()
        schemas = prim.GetMetadata("apiSchemas")
        require(kind and labels and schemas and "SemanticsLabelsAPI:class" in schemas.GetAppliedItems()
                and any(value.lower() in (kind.lower(), actual_class.lower()) for value in labels),
                f"{guid} lost its semantic kind or named class label")

    require(products.keys() == PRODUCTS.keys() and len({row[0] for row in identity.values()}) == len(PRODUCTS),
            "Missing IFC products or overlapping stable occurrence IDs")
    for source_id, (_, parent_id) in PRODUCTS.items():
        if parent_id is None:
            continue
        prim, parent = products[source_id], products[parent_id]
        require(prim.GetParent() == parent, f"{prim.GetPath()} lost its native spatial parent")
        relationship = "ifc:contains" if source_id in BOUNDS else "ifc:aggregates"
        require(prim.GetPath() in parent.GetRelationship(relationship).GetTargets(),
                f"{parent.GetPath()} lost {relationship} to {prim.GetPath()}")

    transforms = UsdGeom.XformCache()
    for source_id, origin in ORIGINS.items():
        world_from_source = transforms.GetLocalToWorldTransform(products[source_id])
        check_vector(world_from_source.Transform(Gf.Vec3d(0.)), origin, f"#{source_id} origin")
        basis = ((1, 0, 0), (0, 1, 0), (0, 0, 1)) if source_id == 37 else (
            (0, -1, 0), (1, 0, 0), (0, 0, 1)
        )
        for axis, expected in enumerate(basis):
            source_direction = Gf.Vec3d(0.)
            source_direction[axis] = 1000.
            check_vector(world_from_source.TransformDir(source_direction), expected,
                         f"#{source_id} original IFC axis {axis}")

    product_paths = {products[source_id].GetPath(): source_id for source_id in BOUNDS}
    visuals, colliders = {}, {}
    for prim in stage.Traverse():
        if not prim.IsA(UsdGeom.Mesh):
            continue
        path = prim.GetPath()
        collider = UsdPhysics.CollisionAPI(prim)
        if collider:
            targets = prim.GetRelationship("robotics:entity").GetTargets()
            require(len(targets) == 1 and targets[0] in product_paths, f"{path} lacks a collider owner")
            source_id = product_paths[targets[0]]
            require(str(path).startswith("/World/Colliders/") and source_id not in colliders,
                    f"{path} is outside the collider root or repeats an owner")
            require(prim.GetAttribute("robotics:entityId").Get() == identity[source_id][0],
                    f"{path} collider identity disagrees with its owner")
            require(collider.GetCollisionEnabledAttr().Get(), f"{path} collision is disabled")
            require(collider.GetSimulationOwnerRel().GetTargets() == [physics_scene.GetPath()],
                    f"{path} has no matching simulation owner")
            mesh_collider = UsdPhysics.MeshCollisionAPI(prim)
            require(mesh_collider and mesh_collider.GetApproximationAttr().Get() == "none",
                    f"{path} lacks an exact mesh collision schema")
            require(UsdGeom.Imageable(prim).ComputePurpose() == UsdGeom.Tokens.guide,
                    f"{path} is not authored as a non-rendering guide")
            colliders[source_id] = path
        else:
            parent = path.GetParentPath()
            while (parent.HasPrefix(world.GetPath()) and parent != world.GetPath()
                   and parent not in product_paths):
                parent = parent.GetParentPath()
            require(parent in product_paths, f"{path} lacks a visual product parent")
            source_id = product_paths[parent]
            require(source_id not in visuals, f"{path} repeats a visual mesh owner")
            material, binding = UsdShade.MaterialBindingAPI(prim).ComputeBoundMaterial()
            require(material and binding, f"{path} lacks a resolved material binding")
            shader, _, _ = material.ComputeSurfaceSource()
            require(shader and shader.GetIdAttr().Get() == "UsdPreviewSurface",
                    f"{path} lacks a connected UsdPreviewSurface shader")
            if source_id == 27:
                check_vector(shader.GetInput("diffuseColor").Get(), (1, 0, 0),
                             f"{path} authored IFC style")
            visuals[source_id] = path
        check_mesh(prim, transforms, source_id)
    require(visuals.keys() == colliders.keys() == BOUNDS.keys(), "Expected one visual and collider per fixture box")
    return identity, colliders


def export_ifcx_fixture(exporter, directory, name, source):
    from pxr import Usd

    fixture = directory / f"{name}.ifcx"
    fixture.write_text(source, encoding="utf-8")
    output = export_stage(exporter, fixture, directory / f"{name}.usda", name,
                          "--metres-per-renderer-unit", "1")
    stage = Usd.Stage.Open(str(output))
    require(stage, f"OpenUSD did not open {output}")
    return stage


def check_ifcx_triangle(prim, transforms, expected):
    _, vertices, indices = mesh_triangles(prim, transforms)
    require(len(indices) == 3, f"{prim.GetPath()} should contain exactly one triangle")
    actual = sorted(tuple(vertices[index]) for index in indices)
    for position, witness in zip(actual, sorted(expected)):
        check_vector(position, witness, f"{prim.GetPath()} triangle")


def validate_ifcx_affine(exporter, directory):
    from pxr import Gf, Usd, UsdGeom, UsdPhysics

    stage = export_ifcx_fixture(exporter, directory, "ifcx-nested-affine", """
{"data": [
  {"path": "root", "children": {"First": "one", "Second": "two"},
   "attributes": {"usd::xformop": {"transform": [[-1,0,0,0],[0,1,0,0],[0,0,1,0],[10000000.125,2,3,1]]}}},
  {"path": "one", "inherits": ["type"],
   "attributes": {"usd::xformop": {"transform": [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0.25,4,5,1]]}}},
  {"path": "two", "inherits": ["type"],
   "attributes": {"usd::xformop": {"transform": [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0.5,6,7,1]]}}},
  {"path": "type", "children": {"Body": "mesh"},
   "attributes": {"bsi::ifc::class": "IfcSlab", "bsi::ifc::globalId": "shared-authored-guid"}},
  {"path": "mesh", "attributes": {
    "usd::usdgeom::mesh": {"faceVertexIndices": [0,1,2], "points": [[0,0,0],[1,0,0],[0,1,0]]}}}
]}
""")
    transforms = UsdGeom.XformCache()
    bounds = UsdGeom.BBoxCache(Usd.TimeCode.Default(), [UsdGeom.Tokens.default_, UsdGeom.Tokens.guide])
    products = [p for p in stage.Traverse() if p.GetAttribute("ifc:guid").Get() == "shared-authored-guid"]
    by_source = {p.GetAttribute("ifc:sourceId").Get(): p for p in products}
    # Converting the draw matrix to float would erase the 0.25 m difference
    # between the two reflected occurrences at this 10,000,000 m translation.
    witnesses = {"root/one": (6, -9999999.875, 8), "root/two": (8, -9999999.625, 10)}
    require(len(products) == 2 and by_source.keys() == witnesses.keys() and
            len({p.GetAttribute("robotics:entityId").Get() for p in products}) == 2,
            "IFCX shared-GUID instances lost distinct semantic occurrences")
    colliders = [p for p in stage.Traverse() if UsdPhysics.CollisionAPI(p)]
    require(len(colliders) == 2, "Expected one IFCX collider per occurrence")
    for source_id, origin in witnesses.items():
        product = by_source[source_id]
        world_from_product = transforms.GetLocalToWorldTransform(product)
        check_vector(world_from_product.ExtractTranslation(), origin, f"{source_id} IFCX origin")
        for axis, target in enumerate(((0, 1, 0), (1, 0, 0), (0, 0, 1))):
            direction = Gf.Vec3d(0.)
            direction[axis] = 1.
            check_vector(world_from_product.TransformDir(direction), target, f"{source_id} reflected axis {axis}")
        visuals = [p for p in Usd.PrimRange(product) if p.IsA(UsdGeom.Mesh)]
        owned = [p for p in colliders if p.GetRelationship("robotics:entity").GetTargets() == [product.GetPath()]]
        require(len(visuals) == len(owned) == 1, f"{source_id} lost a visual or collider")
        x, y, z = origin
        expected = (origin, (x, y + 1., z), (x + 1., y, z))
        check_ifcx_triangle(visuals[0], transforms, expected)
        check_ifcx_triangle(owned[0], transforms, expected)
        world_bounds = bounds.ComputeWorldBound(owned[0]).ComputeAlignedRange()
        check_vector(world_bounds.GetMin(), origin, f"{source_id} collider lower bound")
        check_vector(world_bounds.GetMax(), (x + 1., y + 1., z), f"{source_id} collider upper bound")


def validate_ifcx_malformed_child(exporter, directory):
    from pxr import UsdGeom, UsdPhysics

    stage = export_ifcx_fixture(exporter, directory, "ifcx-malformed-child", """
{"data": [
  {"path": "owner", "children": {"Malformed": "bad", "Valid": "good"},
   "attributes": {"bsi::ifc::class": "IfcWall", "bsi::ifc::globalId": "wall-guid",
     "usd::xformop": {"transform": [[1,0,0,0],[0,1,0,0],[0,0,1,0],[10,0,0,1]]}}},
  {"path": "bad", "attributes": {
    "usd::xformop": {"transform": [[1,0],[0,1]]},
    "usd::usdgeom::mesh": {"faceVertexIndices": [0,1,2], "points": [[0,0,0],[1,0,0],[0,1,0]]}}},
  {"path": "good", "attributes": {
    "usd::xformop": {"transform": [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,2,0,1]]},
    "usd::usdgeom::mesh": {"faceVertexIndices": [0,1,2], "points": [[0,0,0],[1,0,0],[0,1,0]]}}}
]}
""")
    products = [p for p in stage.Traverse() if p.GetAttribute("ifc:guid").Get() == "wall-guid"]
    require(len(products) == 1, "The IFCX wall's semantic product should survive its malformed draw child")
    meshes = [p for p in stage.Traverse() if p.IsA(UsdGeom.Mesh)]
    colliders = [p for p in meshes if UsdPhysics.CollisionAPI(p)]
    require(len(meshes) == 2 and len(colliders) == 1, "Exported invalid IFCX geometry or dropped its valid sibling")
    require(colliders[0].GetRelationship("robotics:entity").GetTargets() == [products[0].GetPath()],
            "The valid IFCX collider lost its semantic wall owner")
    transforms = UsdGeom.XformCache()
    for mesh in meshes:
        require(mesh.GetAttribute("robotics:representationId").Get() == "owner/good",
                f"{mesh.GetPath()} is not the valid IFCX sibling")
        check_ifcx_triangle(mesh, transforms, ((2, -10, 0), (2, -11, 0), (3, -10, 0)))
    diagnostics = stage.GetDefaultPrim().GetAttribute("robotics:diagnostics").Get()
    require(diagnostics and any("visual-metric-placement-unavailable [owner/bad]" in item for item in diagnostics),
            "Missing diagnostic for the malformed IFCX drawable placement")


def isaac_raycasts(app, collider_paths):
    import carb
    import omni.physx
    import omni.timeline
    timeline = omni.timeline.get_timeline_interface()
    timeline.play()
    query = omni.physx.get_physx_scene_query_interface()
    origin = carb.Float3(0., 0., 1.)
    try:
        deadline = monotonic() + 30.
        pending = list(RAYS)
        while pending and monotonic() < deadline:
            app.update()
            for source_id, direction, distance in pending[:]:
                hit = query.raycast_closest(origin, carb.Float3(*direction), 8.)
                if not hit["hit"]:
                    continue
                path = hit.get("collision") or hit.get("rigidBody")
                require(str(path) == str(collider_paths[source_id]),
                        f"Isaac PhysX ray for #{source_id} hit {path}")
                check_vector((hit["distance"],), (distance,), f"Isaac PhysX ray for #{source_id}")
                pending.remove((source_id, direction, distance))
        require(not pending, f"Isaac PhysX did not hit fixture walls: {[r[0] for r in pending]}")
    finally:
        timeline.stop()


def round_trip(exporter, directory, isaac):
    stages = export_corner(exporter, directory)
    app = None
    if isaac:
        from isaacsim import SimulationApp

        app = SimulationApp({"headless": True, "open_usd": str(stages[1]), "fast_shutdown": False})
    try:
        from pxr import Usd

        identities = [validate_stage(Usd.Stage.Open(str(path)))[0] for path in stages]
        require(identities[0] == identities[1] == identities[2],
                "Stable IDs or prim paths changed with renderer import scale")
        validate_ifcx_affine(exporter, directory)
        validate_ifcx_malformed_child(exporter, directory)
        print(f"OpenUSD {Usd.GetVersion()} validated IFC and IFCX exports in {directory}", flush=True)
        if app:
            import omni.usd

            stage = omni.usd.get_context().get_stage()
            require(stage and Path(stage.GetRootLayer().realPath).resolve() == stages[1].resolve(),
                    "Isaac Sim did not open the requested stage")
            _, collider_paths = validate_stage(stage)
            isaac_raycasts(app, collider_paths)
            print("Isaac Sim opened the stage and hit both exact walls with PhysX rays", flush=True)
    finally:
        if app:
            app.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exporter", type=Path, required=True, help="BimRoboticsUsd executable")
    parser.add_argument("--output-dir", type=Path, help="Keep generated fixtures in a new directory")
    parser.add_argument("--isaac", action="store_true", help="Also validate in headless Isaac Sim")
    args = parser.parse_args()
    dependency = "isaacsim" if args.isaac else "pxr"
    if importlib.util.find_spec(dependency) is None:
        print(f"SKIP: {dependency} is unavailable in this Python environment", file=sys.stderr)
        return 77
    try:
        exporter = args.exporter.resolve(strict=True)
        if args.output_dir:
            directory = args.output_dir.resolve()
            directory.mkdir(parents=True, exist_ok=False)
            round_trip(exporter, directory, args.isaac)
        else:
            with tempfile.TemporaryDirectory(prefix="bim-usd-roundtrip-") as directory:
                round_trip(exporter, Path(directory), args.isaac)
        return 0
    except Exception as error:
        print(f"robotics USD round trip: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
