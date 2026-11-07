"""Point records, world-vector calculus and scalar Poisson CLI workflows."""

import argparse
import json
import math
import pathlib
import subprocess
import sys
import tempfile
import unittest


class OpenVdbFieldsCli(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-vdb-fields-")
        self.directory = pathlib.Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def run_tool(self, *arguments, succeeds=True):
        result = subprocess.run([str(self.tool), *map(str, arguments)], cwd=self.directory,
                                capture_output=True, text=True, timeout=60)
        if succeeds:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def require_enabled(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")

    def vector(self, source, position=(0, 0, 0), grid=None):
        args = ["--sample-vector", source, *position]
        if grid:
            args += ["--input-grid", grid]
        return json.loads(self.run_tool(*args).stdout)

    def test_help_and_incompatible_or_missing_parameters(self):
        help_text = self.run_tool("--help").stdout
        for operation in ("--points", "--export-points", "--gradient", "--divergence", "--curl",
                          "--laplacian", "--magnitude", "--normalize", "--poisson", "--sample-vector"):
            self.assertIn(operation, help_text)
        failures = [
            ["--points"], ["--sample-vector", "missing.vdb", "1", "2"],
            ["--poisson", "missing.vdb"], ["--gradient", "missing.vdb"],
            ["--points", "missing.json", "bad.vdb", "--max-points", "0"],
            ["--points", "missing.json", "bad.vdb", "--input-grid", "ignored"],
            ["--points", "missing.json", "bad.vdb", "--half-width", "3"],
            ["--gradient", "missing.vdb", "bad.vdb", "--voxel-size", ".2"],
            ["--gradient", "missing.vdb", "bad.vdb", "--max-points", "2"],
            ["--poisson", "missing.vdb", "bad.vdb", "--max-iterations", "0"],
            ["--poisson", "missing.vdb", "bad.vdb", "--max-iterations", "10001"],
            ["--poisson", "missing.vdb", "bad.vdb", "--tolerance", "nan"],
            ["--poisson", "missing.vdb", "bad.vdb", "--tolerance", "0"],
            ["--poisson", "missing.vdb", "bad.vdb", "--max-solver-voxels", "1000001"],
            ["--normalize", "missing.vdb", "bad.vdb", "--max-iterations", "2"],
            ["--sample-vector", "missing.vdb", "0", "0", "inf"],
        ]
        for arguments in failures:
            with self.subTest(arguments=arguments):
                self.run_tool(*arguments, succeeds=False)
                self.assertFalse((self.directory / "bad.vdb").exists())

    def test_disabled_operations_report_availability_before_reading(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        commands = [["--points", "missing.json", "bad.vdb"],
                    ["--export-points", "missing.vdb", "bad.json"],
                    ["--sample-vector", "missing.vdb", "0", "0", "0"],
                    ["--poisson", "missing.vdb", "bad.vdb"]]
        commands += [[name, "missing.vdb", "bad.vdb"] for name in
                     ("--gradient", "--divergence", "--curl", "--laplacian", "--magnitude", "--normalize")]
        for arguments in commands:
            result = self.run_tool(*arguments, succeeds=False)
            self.assertIn("ENABLE_OPENVDB=ON", result.stdout + result.stderr)
        self.assertFalse((self.directory / "bad.vdb").exists())
        self.assertFalse((self.directory / "bad.json").exists())

    def test_real_point_grid_json_roundtrip_and_inspection(self):
        self.require_enabled()
        records = [{"position": [1.125, -.25, 2], "velocity": [1, 2, 3], "radius": .4},
                   {"position": [-2, .5, .25]}]
        source = self.directory / "points.json"
        source.write_text(json.dumps(records), encoding="utf-8")
        original = source.read_bytes()
        self.run_tool("--points", source, "points.vdb", "--voxel-size", ".25")
        report = json.loads(self.run_tool("--inspect", "points.vdb").stdout)
        self.assertEqual(report["grids"][0]["name"], "points")
        self.assertEqual(report["grids"][0]["valueType"], "ptdataidx32")
        self.run_tool("--export-points", "points.vdb", "roundtrip.json", "--input-grid", "points")
        exported = json.loads((self.directory / "roundtrip.json").read_text(encoding="utf-8"))
        self.assertEqual(len(exported), 2)
        for actual, expected in zip(exported, sorted(records, key=lambda p: p["position"])):
            for a, b in zip(actual["position"], expected["position"]):
                self.assertAlmostEqual(a, b, delta=1e-6)
            self.assertEqual(actual["velocity"], expected.get("velocity", [0, 0, 0]))
            self.assertEqual(actual["radius"], expected.get("radius", 1))
        self.assertEqual(source.read_bytes(), original)
        self.run_tool("--points", source, "bad.vdb", "--max-points", "1", succeeds=False)
        self.run_tool("--export-points", "points.vdb", "roundtrip.json", succeeds=False)
        self.assertFalse((self.directory / "bad.vdb").exists())

    def test_vector_operators_preserve_constant_backgrounds_and_units(self):
        self.require_enabled()
        self.run_tool("--velocity", "wind.vdb", "--vector", "1", "2", "3")
        self.assertEqual(self.vector("wind.vdb", (15.25, -37, 1.75)), [1, 2, 3])
        self.run_tool("--magnitude", "wind.vdb", "speed.vdb")
        speed = float(self.run_tool("--sample", "speed.vdb", "0", "0", "0").stdout)
        self.assertAlmostEqual(speed, math.sqrt(14), places=10)
        self.run_tool("--normalize", "wind.vdb", "direction.vdb")
        for actual, expected in zip(self.vector("direction.vdb"), [v / math.sqrt(14) for v in (1, 2, 3)]):
            self.assertAlmostEqual(actual, expected, places=10)
        self.run_tool("--divergence", "wind.vdb", "div.vdb")
        self.assertEqual(float(self.run_tool("--sample", "div.vdb", "0", "0", "0").stdout), 0)
        self.run_tool("--curl", "wind.vdb", "curl.vdb")
        self.assertEqual(self.vector("curl.vdb"), [0, 0, 0])
        for name in ("speed.vdb", "direction.vdb", "div.vdb", "curl.vdb"):
            self.assertEqual(len(json.loads(self.run_tool("--inspect", name).stdout)["grids"]), 1)

    def test_scalar_gradient_composition_and_poisson_solution(self):
        self.require_enabled()
        self.run_tool("--sphere", "sphere.vdb", "--radius", "1", "--voxel-size", ".2")
        self.run_tool("--fog", "sphere.vdb", "density.vdb", "--grid-name", "density")
        original = (self.directory / "density.vdb").read_bytes()
        self.run_tool("--gradient", "density.vdb", "gradient.vdb", "--input-grid", "density")
        self.run_tool("--curl", "gradient.vdb", "curlgrad.vdb")
        self.assertEqual(self.vector("curlgrad.vdb"), [0, 0, 0])
        self.run_tool("--laplacian", "density.vdb", "laplacian.vdb")
        self.run_tool("--poisson", "density.vdb", "solution.vdb", "--input-grid", "density")
        center = float(self.run_tool("--sample", "solution.vdb", "0", "0", "0").stdout)
        self.assertLess(center, 0)
        self.run_tool("--poisson", "density.vdb", "bad.vdb", "--max-solver-voxels", "1", succeeds=False)
        self.assertFalse((self.directory / "bad.vdb").exists())
        self.assertEqual((self.directory / "density.vdb").read_bytes(), original)

    def test_invalid_schemas_types_limits_and_output_protection(self):
        self.require_enabled()
        source = self.directory / "bad.json"
        for text in ('[{"position":[0,0]}]', '[{"position":[0,0,0],"unknown":1}]',
                     '[{"position":[0,0,0],"radius":-1}]', '[{"position":[0,0,0],"position":[1,0,0]}]'):
            source.write_text(text, encoding="utf-8")
            self.run_tool("--points", source, "bad.vdb", succeeds=False)
            self.assertFalse((self.directory / "bad.vdb").exists())
        self.run_tool("--velocity", "wind.vdb", "--vector", "1", "0", "0")
        original = (self.directory / "wind.vdb").read_bytes()
        self.run_tool("--gradient", "wind.vdb", "bad.vdb", succeeds=False)
        self.run_tool("--magnitude", "wind.vdb", "wind.vdb", succeeds=False)
        self.run_tool("--normalize", "wind.vdb", "bad.vdb", "--max-input-bytes", "1", succeeds=False)
        self.assertEqual((self.directory / "wind.vdb").read_bytes(), original)
        self.assertFalse((self.directory / "bad.vdb").exists())

    def test_all_platonic_solids_and_strict_parameter_routing(self):
        for arguments in (["--platonic"],
                          ["--platonic", "bad.vdb", "--faces", "7"],
                          ["--platonic", "bad.vdb", "--radius", "1"],
                          ["--sphere", "bad.vdb", "--faces", "6"],
                          ["--sphere", "bad.vdb", "--scale", "1"]):
            self.run_tool(*arguments, succeeds=False)
        if not self.enabled:
            failure = self.run_tool("--platonic", "bad.vdb", succeeds=False)
            self.assertIn("ENABLE_OPENVDB=ON", failure.stdout + failure.stderr)
            return
        for faces in (4, 6, 8, 12, 20):
            name = f"solid-{faces}.vdb"
            self.run_tool("--platonic", name, "--faces", faces, "--scale", "1", "--center", "2.25", "-.5", "0")
            center = float(self.run_tool("--sample", name, "2.25", "-.5", "0").stdout)
            self.assertLess(center, 0)
            report = json.loads(self.run_tool("--inspect", name).stdout)
            self.assertEqual(report["grids"][0]["gridClass"], "level set")
        self.run_tool("--platonic", "bad.vdb", "--scale", ".1", "--voxel-size", ".1", succeeds=False)
        self.assertFalse((self.directory / "bad.vdb").exists())

    def test_world_surface_measurements_and_parameter_routing(self):
        for arguments in (["--measure"], ["--measure", "missing.vdb", "--voxel-size", ".1"],
                          ["--measure", "missing.vdb", "--grid-name", "wrong"],
                          ["--measure", "missing.vdb", "--max-triangles", "8"],
                          ["--measure", "missing.vdb", "--stats"]):
            self.run_tool(*arguments, succeeds=False)
        if not self.enabled:
            failure = self.run_tool("--measure", "missing.vdb", succeeds=False)
            self.assertIn("ENABLE_OPENVDB=ON", failure.stdout + failure.stderr)
            return
        self.run_tool("--sphere", "sphere.vdb", "--radius", "1", "--voxel-size", ".1")
        before = (self.directory / "sphere.vdb").read_bytes()
        measured = json.loads(self.run_tool("--measure", "sphere.vdb", "--input-grid", "surface").stdout)
        self.assertEqual(measured["gridName"], "surface")
        self.assertEqual(measured["estimatedEulerCharacteristic"], 2)
        self.assertAlmostEqual(measured["area"], 4 * math.pi, delta=.4)
        self.assertAlmostEqual(measured["volume"], 4 * math.pi / 3, delta=.3)
        self.assertAlmostEqual(measured["averageMeanCurvature"], 1, delta=.1)
        self.assertLess(measured["eulerRoundingError"], .1)
        self.assertIn("estimate", measured["topologyDiagnostic"])
        self.run_tool("--measure", "sphere.vdb", "--max-voxels", "1", succeeds=False)
        self.run_tool("--measure", "sphere.vdb", "--input-grid", "missing", succeeds=False)
        self.assertEqual((self.directory / "sphere.vdb").read_bytes(), before)

    def test_level_set_transport_morph_units_selectors_and_protection(self):
        for arguments in (["--advect-sdf","a","b","bad.vdb"],
                          ["--morph-sdf","a","b","bad.vdb","--time",1,"--velocity-grid","ignored"],
                          ["--advect-sdf","a","b","bad.vdb","--time",1,"--target-grid","ignored"],
                          ["--advect-sdf","a","b","bad.vdb","--time",1,"--steps",2],
                          ["--morph-sdf","a","b","bad.vdb","--time",1,"--normalization-iterations",9],
                          ["--morph-sdf","a","b","bad.vdb","--time",1,"--voxel-size",.1]):
            self.run_tool(*arguments,succeeds=False)
        if not self.enabled:
            for operation in ("--advect-sdf","--morph-sdf"):
                failure=self.run_tool(operation,"missing","missing","bad.vdb","--time",.1,succeeds=False)
                self.assertIn("ENABLE_OPENVDB=ON",failure.stdout+failure.stderr)
            return
        self.run_tool("--sphere","source.vdb","--voxel-size",.1)
        self.run_tool("--sphere","target.vdb","--voxel-size",.1,"--radius",1.2)
        self.run_tool("--velocity","wind.vdb","--vector",1,0,0)
        source=(self.directory/"source.vdb").read_bytes()
        report=json.loads(self.run_tool("--advect-sdf","source.vdb","wind.vdb","moved.vdb",
            "--time",.2,"--input-grid","surface","--velocity-grid","velocity","--grid-name","moving").stdout)
        self.assertGreater(report["cflSteps"],0)
        self.assertGreater(report["work"],0)
        self.assertLess(float(self.run_tool("--sample","moved.vdb",1.1,0,0,"--input-grid","moving").stdout),0)
        self.assertGreater(float(self.run_tool("--sample","moved.vdb",-.9,0,0,"--input-grid","moving").stdout),0)
        morphed=json.loads(self.run_tool("--morph-sdf","source.vdb","target.vdb","morphed.vdb",
            "--time",1,"--input-grid","surface","--target-grid","surface").stdout)
        self.assertGreater(morphed["cflSteps"],0)
        self.assertLess(float(self.run_tool("--sample","morphed.vdb",1.1,0,0).stdout),0)
        for extra in (("--max-cfl-steps",1),("--max-transport-work",1)):
            self.run_tool("--advect-sdf","source.vdb","wind.vdb","bad.vdb","--time",1,*extra,succeeds=False)
            self.assertFalse((self.directory/"bad.vdb").exists())
        self.run_tool("--morph-sdf","source.vdb","target.vdb","bad.vdb","--time",-.1,succeeds=False)
        self.run_tool("--advect-sdf","source.vdb","wind.vdb","source.vdb","--time",.2,succeeds=False)
        self.assertEqual((self.directory/"source.vdb").read_bytes(),source)

    def test_native_point_attributes_groups_exact_integer_json_and_routing(self):
        invalid=(["--point-schema","a","bad.json","--grid-name","ignored"],
                 ["--point-attribute","a","id","bad.json","--voxel-size",.2],
                 ["--edit-point-attributes","a","b","bad.vdb","--max-attributes",1025],
                 ["--points","a","bad.vdb","--max-groups",3])
        for arguments in invalid:
            self.run_tool(*arguments,succeeds=False)
        if not self.enabled:
            for arguments in (("--point-schema","a","bad.json"),("--point-attribute","a","id","bad.json"),
                              ("--point-group","a","selected","bad.json"),("--edit-point-attributes","a","b","bad.vdb")):
                failure=self.run_tool(*arguments,succeeds=False)
                self.assertIn("ENABLE_OPENVDB=ON",failure.stdout+failure.stderr)
            return
        (self.directory/"points.json").write_text(json.dumps([{"position":[0,0,0]},{"position":[.25,0,0]}]),encoding="utf-8")
        self.run_tool("--points","points.json","points.vdb")
        source=(self.directory/"points.vdb").read_bytes()
        columns={"flag":("bool",[True,False]),"small":("int32",[-2147483648,2147483647]),
                 "id":("int64",[-9223372036854775808,9007199254740993]),
                 "single":("float",[1.25,-2.5]),"large":("double",[1e200,-1e200]),
                 "vf":("vec3f",[[1,2,3],[-1,-2,-3]]),"vd":("vec3d",[[.25,.5,.75],[4,5,6]]),
                 "label":("string",["Ω\u0000end","é"]),"zero":("double",[0.0,-0.0])}
        edits={"version":1,"attributes":[{"op":"add","name":name,"type":kind,"values":values}
                 for name,(kind,values) in columns.items()],
               "groups":[{"op":"add","name":"selected","values":[True,False]}]}
        (self.directory/"edits.json").write_text(json.dumps(edits),encoding="utf-8")
        report=json.loads(self.run_tool("--edit-point-attributes","points.vdb","edits.json","edited.vdb",
            "--input-grid","points","--grid-name","native","--max-points",2).stdout)
        self.assertEqual(report["points"],2)
        self.assertEqual(report["groups"],1)
        self.run_tool("--point-schema","edited.vdb","schema.json","--input-grid","native")
        schema=json.loads((self.directory/"schema.json").read_text(encoding="utf-8"))
        self.assertEqual(schema["point_count"],2)
        self.assertEqual(schema["groups"],["selected"])
        for name,(kind,values) in columns.items():
            self.run_tool("--point-attribute","edited.vdb",name,name+".json","--input-grid","native")
            column=json.loads((self.directory/(name+".json")).read_text(encoding="utf-8"))
            self.assertEqual(column["type"],kind)
            self.assertEqual(column["values"],values)
            self.assertEqual(column["traversal"],"leaf_origin_xyz_then_local_index")
            if name=="zero":
                self.assertEqual([math.copysign(1,value) for value in column["values"]],[1,-1])
        self.run_tool("--point-group","edited.vdb","selected","group.json")
        self.assertEqual(json.loads((self.directory/"group.json").read_text())["values"],[True,False])
        preserved=(self.directory/"group.json").read_bytes()
        self.run_tool("--point-group","edited.vdb","selected","group.json",succeeds=False)
        self.assertEqual((self.directory/"group.json").read_bytes(),preserved)
        (self.directory/"duplicate.json").write_text('{"version":1,"version":1}',encoding="utf-8")
        self.run_tool("--edit-point-attributes","points.vdb","duplicate.json","bad.vdb",succeeds=False)
        self.run_tool("--point-attribute","edited.vdb","missing","bad.json",succeeds=False)
        self.assertFalse((self.directory/"bad.vdb").exists())
        self.assertFalse((self.directory/"bad.json").exists())
        self.assertEqual((self.directory/"points.vdb").read_bytes(),source)
        counts=json.loads(self.run_tool("--count-points","edited.vdb","--any-group","selected").stdout)
        self.assertEqual((counts["total"],counts["selected"],counts["selectedActive"],counts["selectedInactive"]),(2,1,1,0))
        statistics=json.loads(self.run_tool("--point-statistics","edited.vdb","id").stdout)
        self.assertEqual(statistics["minimum"],-9223372036854775808)
        self.assertEqual(statistics["maximum"],9007199254740993)
        self.assertEqual(statistics["componentCount"],1)
        self.assertGreater(statistics["populationStdDev"][0],0)
        kept=json.loads(self.run_tool("--select-points","edited.vdb","kept.vdb","--all-group","selected").stdout)
        self.assertEqual(kept["outputCount"],1)
        self.run_tool("--point-attribute","kept.vdb","id","kept-id.json")
        self.assertEqual(json.loads((self.directory/"kept-id.json").read_text())["values"],[-9223372036854775808])
        empty=json.loads(self.run_tool("--delete-points","edited.vdb","empty.vdb").stdout)
        self.assertEqual(empty["outputCount"],0)
        self.assertTrue(empty["archivedSchema"])
        archived=json.loads(self.run_tool("--count-points","empty.vdb").stdout)
        self.assertEqual(archived["total"],0)
        self.assertTrue(archived["archivedSchema"])
        excluded=json.loads(self.run_tool("--count-points","edited.vdb","--any-group","selected","--exclude-group","selected").stdout)
        self.assertEqual(excluded["selected"],0)
        self.run_tool("--select-points","edited.vdb","bad.vdb","--all-group","missing",succeeds=False)
        self.assertFalse((self.directory/"bad.vdb").exists())

    def test_dense_array_and_multiresolution_routes_and_capabilities(self):
        capabilities=json.loads(self.run_tool("--capabilities").stdout)
        self.assertEqual(capabilities["schemaVersion"],1)
        self.assertEqual(capabilities["openvdbAvailable"],self.enabled)
        self.assertEqual(capabilities["protocols"]["denseArray"],1)
        for arguments in (("--dense-export","a","bad.cvarr"),
                          ("--dense-export","a","bad.cvarr","--dense-origin",0,0,0,"--dense-dimensions",1,0,1),
                          ("--dense-import","a","bad.vdb","--input-grid","ignored"),
                          ("--multires-info","a","--grid-name","ignored"),
                          ("--multires","a","bad.vdb","--levels",9),
                          ("--multires-info","a","--level",0),
                          ("--sphere","bad.vdb","--any-group","ignored")):
            self.run_tool(*arguments,succeeds=False)
        if not self.enabled:
            for arguments in (("--dense-export","a","bad.cvarr","--dense-origin",0,0,0,"--dense-dimensions",1,1,1),
                              ("--dense-import","a","bad.vdb"),("--multires","a","bad.vdb"),
                              ("--multires-info","a"),("--multires-sample","a",0,0,0,.5),
                              ("--count-points","a"),("--point-statistics","a","id"),
                              ("--select-points","a","bad.vdb"),("--delete-points","a","bad.vdb")):
                failure=self.run_tool(*arguments,succeeds=False)
                self.assertIn("ENABLE_OPENVDB=ON",failure.stdout+failure.stderr)
            return
        self.run_tool("--velocity","wind.vdb","--vector",1,2,3,"--voxel-size",.25)
        source=(self.directory/"wind.vdb").read_bytes()
        report=json.loads(self.run_tool("--dense-export","wind.vdb","wind.cvarr","--input-grid","velocity",
            "--dense-origin",-1,-1,-1,"--dense-dimensions",2,2,2).stdout)
        self.assertEqual(report["valueType"],"vec3d")
        self.assertEqual(report["dimensions"],[2,2,2])
        self.assertEqual((self.directory/"wind.cvarr").read_bytes()[:8],b"CVDBARR\0")
        self.run_tool("--dense-import","wind.cvarr","restored.vdb")
        self.assertEqual(self.vector("restored.vdb",(17,-4,2)),[1,2,3])
        levels=json.loads(self.run_tool("--multires","wind.vdb","levels.vdb","--levels",3).stdout)
        self.assertEqual([level["level"] for level in levels],[0,1,2])
        for index,level in enumerate(levels):
            self.assertEqual(level["voxelSize"],[.25*2**index]*3)
        self.assertEqual(json.loads(self.run_tool("--multires-sample","wind.vdb",9,-7,4,1.5,"--levels",3).stdout),[1,2,3])
        self.run_tool("--multires","wind.vdb","one-level.vdb","--levels",3,"--level",1)
        inspection=json.loads(self.run_tool("--inspect","one-level.vdb").stdout)
        self.assertEqual(len(inspection["grids"]),1)
        self.assertEqual(inspection["grids"][0]["voxelSize"],[.5]*3)
        self.run_tool("--sphere","sphere.vdb","--voxel-size",.2)
        self.run_tool("--fog","sphere.vdb","fog.vdb")
        self.run_tool("--multires","fog.vdb","bad.vdb","--max-multires-work",1,succeeds=False)
        self.run_tool("--dense-import","wind.cvarr","bad.vdb","--max-input-bytes",1,succeeds=False)
        self.assertFalse((self.directory/"bad.vdb").exists())
        self.assertEqual((self.directory/"wind.vdb").read_bytes(),source)

    def test_unsigned_open_mesh_distance_and_native_triangle_ids(self):
        self.run_tool("--mesh", "missing", "bad.vdb", "--no-triangle-ids", succeeds=False)
        if not self.enabled:
            failure=self.run_tool("--mesh-distance", "missing", "bad.vdb", succeeds=False)
            self.assertIn("ENABLE_OPENVDB=ON",failure.stdout+failure.stderr)
            return
        fixture={"schema_version":"1.1.0","meshes":[{"mesh_id":0,
                 "coordinates":[-1,-1,0,1,-1,0,0,1,0],"indices":[0,1,2]}],
                 "elements":[{"mesh_id":0,"guid":"open","type":"open triangle",
                              "vector":{"x":2,"y":0,"z":0},
                              "rotation":{"qx":0,"qy":0,"qz":0,"qw":1},
                              "color":{"r":255,"g":255,"b":255,"a":255}}]}
        (self.directory/"open.bim").write_text(json.dumps(fixture),encoding="utf-8")
        original=(self.directory/"open.bim").read_bytes()
        report=json.loads(self.run_tool("--mesh-distance","open.bim","distance.vdb",
                                        "--triangle-id-name","closest").stdout)
        self.assertEqual(report["triangles"],1); self.assertEqual(report["distanceGrid"],"distance")
        self.assertEqual(report["triangleGrid"],"closest")
        # dotBIM's Z-up XY triangle is converted to the renderer's Y-up XZ plane.
        for y in (-.1,.1):
            sample=float(self.run_tool("--sample","distance.vdb",2,y,0,"--input-grid","distance").stdout)
            self.assertAlmostEqual(sample,.1,places=5)
        inspected=json.loads(self.run_tool("--inspect","distance.vdb").stdout)
        by_name={grid["name"]:grid for grid in inspected["grids"]}
        self.assertEqual(by_name["distance"]["gridClass"],"unknown")
        self.assertEqual(by_name["closest"]["valueType"],"int32")
        no_ids=json.loads(self.run_tool("--mesh-distance","open.bim","no-ids.vdb","--no-triangle-ids").stdout)
        self.assertEqual(no_ids["triangleGrid"],"")
        self.assertEqual(len(json.loads(self.run_tool("--inspect","no-ids.vdb").stdout)["grids"]),1)
        self.run_tool("--mesh-distance","open.bim","distance.vdb",succeeds=False)
        self.assertEqual((self.directory/"open.bim").read_bytes(),original)


    def test_general_field_filter_cli_mask_and_offsets(self):
        self.assertIn("--filter-field",self.run_tool("--help").stdout)
        for arguments in (
            ["--filter-field"],
            ["--filter-field","missing.vdb","bad.vdb","--filter","mean"],
            ["--filter-field","missing.vdb","bad.vdb","--field-offset","1","0","0"],
            ["--filter-field","missing.vdb","bad.vdb","--mask-min","0"],
            ["--filter-field","missing.vdb","bad.vdb","--field-filter","offset","--width","2"],
            ["--filter-field","missing.vdb","bad.vdb","--width","5"],
            ["--filter-field","missing.vdb","bad.vdb","--voxel-size",".1"],
            ["--filter-field","missing.vdb","bad.vdb","--max-triangles","1"],
            ["--sphere","bad.vdb","--field-filter","mean"],
        ):
            self.run_tool(*arguments,succeeds=False)
            self.assertFalse((self.directory/"bad.vdb").exists())
        if not self.enabled:
            result=self.run_tool("--filter-field","missing.vdb","bad.vdb",succeeds=False)
            self.assertIn("ENABLE_OPENVDB=ON",result.stdout+result.stderr)
            return
        from array import array
        sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[2]/"python"))
        from container_openvdb import DenseType,DenseVolume,write_dense_array
        volume=DenseVolume(DenseType.VEC3_FLOAT64,(1,1,1),array("d",[1,2,3]),bytearray([1]),grid_name="flow")
        write_dense_array(self.directory/"seed.cvarr",volume)
        self.run_tool("--dense-import","seed.cvarr","seed.vdb")
        original=(self.directory/"seed.vdb").read_bytes()
        shifted=json.loads(self.run_tool("--filter-field","seed.vdb","shifted.vdb","--field-filter","offset",
            "--field-offset","1","-2",".5","--iterations","2","--grid-name","shifted").stdout)
        self.assertEqual(shifted["gridName"],"shifted")
        self.assertEqual(shifted["activeVoxels"],1)
        self.assertEqual(self.vector("shifted.vdb"),[3,-2,4])
        self.assertEqual(self.vector("shifted.vdb",(100,100,100)),[0,0,0])
        mean=json.loads(self.run_tool("--filter-field","seed.vdb","mean.vdb","--field-filter","mean","--width","1").stdout)
        self.assertFalse(mean["maskUsed"])
        for actual,expected in zip(self.vector("mean.vdb"),[1/27,2/27,3/27]):
            self.assertAlmostEqual(actual,expected,delta=1e-7)
        mask=DenseVolume(DenseType.FLOAT64,(1,1,1),array("d",[0]),bytearray([0]),grid_name="alpha")
        write_dense_array(self.directory/"mask.cvarr",mask)
        self.run_tool("--dense-import","mask.cvarr","mask.vdb")
        masked=json.loads(self.run_tool("--filter-field","seed.vdb","masked.vdb","--field-filter","mean",
            "--filter-mask","mask.vdb","--filter-mask-grid","alpha").stdout)
        self.assertTrue(masked["maskUsed"])
        self.assertEqual(self.vector("masked.vdb"),[1,2,3])
        self.run_tool("--filter-field","seed.vdb","bad.vdb","--max-filter-work","1",succeeds=False)
        self.assertFalse((self.directory/"bad.vdb").exists())
        stored=(self.directory/"shifted.vdb").read_bytes()
        self.run_tool("--filter-field","seed.vdb","shifted.vdb",succeeds=False)
        self.assertEqual((self.directory/"shifted.vdb").read_bytes(),stored)
        self.assertEqual((self.directory/"seed.vdb").read_bytes(),original)

    def test_file_authoring_spec_cli_and_external_limits(self):
        self.assertIn("--author-file", self.run_tool("--help").stdout)
        capabilities=json.loads(self.run_tool("--capabilities").stdout)
        self.assertEqual(capabilities["protocols"]["fileAuthoringSpec"],1)
        invalid=[
            ["--author-file"],
            ["--author-file","missing.json","bad.vdb","--grid-name","ignored"],
            ["--author-file","missing.json","bad.vdb","--input-grid","ignored"],
            ["--author-file","missing.json","bad.vdb","--voxel-size",".2"],
            ["--author-file","missing.json","bad.vdb","--max-triangles","1"],
            ["--author-file","missing.json","bad.vdb","--max-string-bytes","1"],
            ["--author-file","missing.json","bad.vdb","--stats"],
            ["--author-file","missing.json","bad.vdb","--max-spec-depth","65"],
            ["--author-file","missing.json","bad.vdb","--max-spec-bytes","67108865"],
            ["--sphere","bad.vdb","--max-spec-depth","16"],
        ]
        for arguments in invalid:
            with self.subTest(arguments=arguments):
                self.run_tool(*arguments,succeeds=False)
                self.assertFalse((self.directory/"bad.vdb").exists())
        if not self.enabled:
            failure=self.run_tool("--author-file","missing.json","bad.vdb",succeeds=False)
            self.assertIn("ENABLE_OPENVDB=ON",failure.stdout+failure.stderr)
            return
        self.run_tool("--velocity","wind.vdb","--vector","1","2","3")
        points=self.directory/"points.json"
        points.write_text(json.dumps([{"position":[0,0,0]},{"position":[1,0,0]}]),encoding="utf-8")
        self.run_tool("--points",points,"points.vdb")
        source_bytes=(self.directory/"wind.vdb").read_bytes()
        specs=self.directory/"spécifications"
        specs.mkdir()
        spec=specs/"merge.json"
        spec.write_text(json.dumps({"version":1,"inputs":[
            {"path":"../wind.vdb","edits":[{"selector":"velocity","newName":"flow",
                "metadata":[{"name":"exact_id","operation":"set","type":"int64","value":9007199254740993}]}]},
            {"path":"../points.vdb"}],"compression":"zip",
            "metadata":[{"name":"artist","operation":"set","type":"string","value":"example"}]
        }),encoding="utf-8")
        result=json.loads(self.run_tool("--author-file",spec,"merged.vdb","--max-grids","2",
            "--max-points","2","--max-attributes","3","--max-groups","1","--max-edits","4").stdout)
        self.assertEqual(result["gridNames"],["flow","points"])
        self.assertEqual(result["inputGrids"],2)
        self.assertEqual(result["outputGrids"],2)
        self.assertEqual(result["renamedGrids"],1)
        self.assertEqual(result["points"],2)
        self.assertEqual(self.vector("merged.vdb",grid="flow"),[1,2,3])
        inspected=json.loads(self.run_tool("--inspect","merged.vdb").stdout)
        flow=next(grid for grid in inspected["grids"] if grid["name"]=="flow")
        exact=next(field for field in flow["metadata"] if field["name"]=="exact_id")
        self.assertEqual(exact["value"],"9007199254740993")
        self.assertEqual(json.loads(self.run_tool("--count-points","merged.vdb","--input-grid","points").stdout)["total"],2)
        for flag,limit in (("--max-spec-bytes","1"),("--max-spec-depth","1"),("--max-grids","1"),
                           ("--max-points","1"),("--max-edits","1"),("--max-metadata-bytes","1"),
                           ("--max-input-bytes","1")):
            with self.subTest(flag=flag):
                self.run_tool("--author-file",spec,"bad.vdb",flag,limit,succeeds=False)
                self.assertFalse((self.directory/"bad.vdb").exists())
        merged_bytes=(self.directory/"merged.vdb").read_bytes()
        self.run_tool("--author-file",spec,"merged.vdb",succeeds=False)
        self.assertEqual((self.directory/"merged.vdb").read_bytes(),merged_bytes)
        empty=specs/"empty.json"
        empty.write_text(json.dumps({"version":1,"inputs":[{"path":"../wind.vdb","remove":["velocity"]}]}),encoding="utf-8")
        removed=json.loads(self.run_tool("--author-file",empty,"empty.vdb").stdout)
        self.assertEqual(removed["outputGrids"],0)
        self.assertEqual(json.loads(self.run_tool("--inspect","empty.vdb").stdout)["grids"],[])
        self.assertEqual((self.directory/"wind.vdb").read_bytes(),source_bytes)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True, type=pathlib.Path)
    parser.add_argument("--enabled", required=True, choices=("0", "1"))
    args, remaining = parser.parse_known_args()
    OpenVdbFieldsCli.tool = args.tool.resolve()
    OpenVdbFieldsCli.enabled = args.enabled == "1"
    unittest.main(argv=[__file__, *remaining])
