"""Reproducible scenes, calibrated sensors, and motion for the BIM robotics benchmark."""

from pathlib import Path


TEKLA_SHA256 = "1e0e087b7b5a9a8aaaccb2896ad9317c5a1cf1a86da30236b04a3ead8d9eac71"
TEKLA_RELATIVE_PATH = Path("buildingSMART-IFC5-development/examples/Tekla House/TeklaHouse.ifc")
DEFAULT_SYNTHETIC_SIDE = 32
MAX_SYNTHETIC_SIDE = 48
TICKS_PER_SECOND = 60
SENSOR_RATE_MILLIHZ = 10_000
MOUNT_HEIGHT_METERS = 0.72

_SCENE_FEET = {"tekla": [6.5, -20.75, 12.45], "synthetic": [0, 0, 0]}

# The repeated 12 m rooms have clear, connected floors and a 2.2 m
# obstacle-free radius around the robot's route in the first room.
# Layout positions and box sizes use millimetres in REP-103 axes.
_BOX_TYPES = {
    "slab": ((12_000, 12_000, 200), (0.55, 0.58, 0.63), "IFCSLAB", "FLOOR"),
    "end-wall": ((220, 4_100, 2_500), (0.75, 0.70, 0.60), "IFCWALL", "NOTDEFINED"),
    "side-wall": ((4_100, 220, 2_500), (0.60, 0.70, 0.74), "IFCWALL", "NOTDEFINED"),
    "barrier": ((600, 450, 880), (0.78, 0.40, 0.14), "IFCWALL", "NOTDEFINED"),
    "column": ((480, 480, 2_850), (0.22, 0.43, 0.50), "IFCCOLUMN", "COLUMN"),
}
_MODULE = (
    ("slab", (0, 0, -200)),
    ("end-wall", (-4_750, 0, 0)),
    ("end-wall", (4_750, 0, 0)),
    ("side-wall", (0, -4_750, 0)),
    ("side-wall", (0, 4_750, 0)),
    ("barrier", (-3_500, -1_700, 0)),
    ("barrier", (-3_500, 1_700, 0)),
    ("barrier", (3_500, -1_700, 0)),
    ("barrier", (3_500, 1_700, 0)),
    ("column", (-3_680, -3_100, 0)),
    ("column", (3_680, -3_100, 0)),
    ("column", (3_680, 3_100, 0)),
)


def source_namespace(scene):
    if scene not in _SCENE_FEET:
        raise ValueError(f"Unknown benchmark scene {scene!r}")
    return f"benchmark-{scene}"


def simulation_scenario(scene, ticks, *, lidar=True):
    source_namespace(scene)
    scenario = {
        "version": 1,
        "initialFeet": list(_SCENE_FEET[scene]),
        "initialYaw": 0.0,
        "ticks": ticks,
        "ticksPerSecond": TICKS_PER_SECOND,
        # This two-wheel motion has a radius of about 1.05 m and stays
        # within the same supported clearance region for long runs.
        "commands": [{"tick": 0, "linear": 0.40, "angular": 0.38}],
        "driveSensors": {"samplesPerSecondMilliHz": 20_000},
    }
    if lidar:
        scenario["lidar"] = {
            "scansPerSecondMilliHz": SENSOR_RATE_MILLIHZ,
            "azimuthSamples": 256,
            "elevationAngles": [-0.12, 0, 0.12],
            "minRangeMeters": 0.05,
            "maxRangeMeters": 30.0,
            "mount": {"xyz": [0, 0, MOUNT_HEIGHT_METERS]},
        }
    return scenario


def camera_spec(ticks, width, height):
    focal_length = width * 0.75
    return {
        "schema": "container.sensor-camera",
        "version": 1,
        "intrinsics": {
            "width": width, "height": height,
            "fx": focal_length, "fy": focal_length, "cx": width * 0.5, "cy": height * 0.5,
            "nearMeters": 0.08, "farMeters": 40.0,
        },
        "baseFromCamera": {"xyz": [0.12, 0, MOUNT_HEIGHT_METERS], "rpy": [0, 0.06, 0]},
        "timing": {"ticksPerSecond": TICKS_PER_SECOND, "framesPerSecondMilliHz": SENSOR_RATE_MILLIHZ,
                   "firstTick": 0, "lastTick": ticks},
    }


def write_synthetic_ifc(path, tiles):
    """Write a square grid of modules and return the represented product count."""
    if type(tiles) is not int or not 1 <= tiles <= MAX_SYNTHETIC_SIDE:
        raise ValueError(f"Synthetic grid side must be an integer in [1,{MAX_SYNTHETIC_SIDE}]")

    with Path(path).open("x", encoding="ascii", newline="\n") as out:
        out.write("""ISO-10303-21;
HEADER;
FILE_DESCRIPTION(('ViewDefinition [ReferenceView_V1.2]'),'2;1');
FILE_NAME('robotics-benchmark.ifc','2026-01-01T00:00:00',(''),(''),'Container','Container','');
FILE_SCHEMA(('IFC4'));
ENDSEC;
DATA;
""")
        next_step_id = 1

        def entity(expression):
            nonlocal next_step_id
            reference = f"#{next_step_id}"
            out.write(f"{reference}={expression};\n")
            next_step_id += 1
            return reference

        def guid():
            # A deterministic 22-character IFC identifier. This fixture uses
            # the decimal subset of the compressed identifier alphabet.
            return f"'{next_step_id:022d}'"

        unit = entity("IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.)")
        units = entity(f"IFCUNITASSIGNMENT(({unit}))")
        zero = entity("IFCCARTESIANPOINT((0.,0.,0.))")
        up = entity("IFCDIRECTION((0.,0.,1.))")
        right = entity("IFCDIRECTION((1.,0.,0.))")
        axes = entity(f"IFCAXIS2PLACEMENT3D({zero},{up},{right})")
        origin = entity(f"IFCLOCALPLACEMENT($,{axes})")
        context = entity(f"IFCGEOMETRICREPRESENTATIONCONTEXT($,'Model',3,0.001,{axes},$)")
        project = entity(f"IFCPROJECT({guid()},$,'Robotics benchmark',$,$,$,$,({context}),{units})")
        site = entity(f"IFCSITE({guid()},$,'Site',$,$,{origin},$,$,.ELEMENT.,$,$,$,$,$)")
        building = entity(f"IFCBUILDING({guid()},$,'Benchmark floor',$,$,{origin},$,$,.ELEMENT.,$,$,$)")
        storey = entity(f"IFCBUILDINGSTOREY({guid()},$,'Ground floor',$,$,{origin},$,$,.ELEMENT.,0.)")
        for parent, child in ((project, site), (site, building), (building, storey)):
            entity(f"IFCRELAGGREGATES({guid()},$,$,$,{parent},({child}))")

        mapping_target = entity(f"IFCCARTESIANTRANSFORMATIONOPERATOR3D($,$,{zero},1.,$)")
        maps = {}
        for kind, ((dx, dy, dz), (r, g, b), _, _) in _BOX_TYPES.items():
            # IFC X points right and IFC Y points forward, whereas REP-103
            # X points forward and REP-103 Y points left.
            profile = entity(f"IFCRECTANGLEPROFILEDEF(.AREA.,$,$,{dy}.,{dx}.)")
            solid = entity(f"IFCEXTRUDEDAREASOLID({profile},{axes},{up},{dz}.)")
            color = entity(f"IFCCOLOURRGB($,{r},{g},{b})")
            shading = entity(f"IFCSURFACESTYLESHADING({color},0.)")
            style = entity(f"IFCSURFACESTYLE($,.BOTH.,({shading}))")
            entity(f"IFCSTYLEDITEM({solid},({style}),$)")
            shape = entity(f"IFCSHAPEREPRESENTATION({context},'Body','SweptSolid',({solid}))")
            maps[kind] = entity(f"IFCREPRESENTATIONMAP({axes},{shape})")

        products = []
        for row in range(tiles):
            for column in range(tiles):
                for kind, (local_x, local_y, z) in _MODULE:
                    x, y = local_x + 12_000 * column, local_y + 12_000 * row
                    point = entity(f"IFCCARTESIANPOINT(({-y}.,{x}.,{z}.))")
                    placement_axes = entity(f"IFCAXIS2PLACEMENT3D({point},{up},{right})")
                    placement = entity(f"IFCLOCALPLACEMENT({origin},{placement_axes})")
                    item = entity(f"IFCMAPPEDITEM({maps[kind]},{mapping_target})")
                    shape = entity(f"IFCSHAPEREPRESENTATION({context},'Body','MappedRepresentation',({item}))")
                    body = entity(f"IFCPRODUCTDEFINITIONSHAPE($,$,({shape}))")
                    _, _, ifc_class, predefined = _BOX_TYPES[kind]
                    products.append(entity(f"{ifc_class}({guid()},$,'{kind} {row},{column}',$,$,"
                                           f"{placement},{body},$,.{predefined}.)"))
        entity(f"IFCRELCONTAINEDINSPATIALSTRUCTURE({guid()},$,$,$,({','.join(products)}),{storey})")
        out.write("ENDSEC;\nEND-ISO-10303-21;\n")
    return len(products)
