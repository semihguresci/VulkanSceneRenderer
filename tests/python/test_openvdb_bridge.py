"""Pure-data tests always run; native roundtrips opt in with runtime path env."""
from array import array
import copy
from decimal import Decimal, localcontext
import json
import math
import os
from pathlib import Path
import random
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]/"python"))
from container_openvdb import (
    AttributeEdit, BridgeError, Capabilities, CapabilityError, DataError, DenseType, DenseVolume,
    GridClass, GroupEdit, LimitError, Limits, PointCloud, PointColumn, PointCounts,
    PointEdits, PointGroup, PointSchema, PointSelection, PointStatistics, ToolClient,
    ToolError, TRAVERSAL, VectorType, read_dense_array, write_dense_array)
from container_openvdb._core import decode_json


def bits32(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def column_document(dtype="int64", values=None, native="int64"):
    return {"version": 1, "traversal": TRAVERSAL, "attribute": {
        "name": "id", "value_type": native, "codec": "null", "stride": 1,
        "group_storage": False, "any_hidden": False, "readable": True},
        "type": dtype, "values": values if values is not None else [2**63-1]}


def independent_dense_fixture():
    # Independently hand-encode the published layout, not the bridge writer.
    name, omitted = b"raw\xff", b"artist\xfe"
    values = struct.pack("<3I", 0, 0x80000000, 1)
    mask = b"\x00\x01\x01"
    prefix = struct.pack("<8s6I3i3I9d3d", b"CVDBARR\0", 1, 0, 2, 4, 1, 0,
                         -2, 3, 4, 3, 1, 1, .2, 0, 0, 0, .3, 0, 0, 0, .4, 1, 2, 3)
    size = 200+4+len(name)+4+len(omitted)+len(values)+len(mask)
    tail = struct.pack("<IIQIIQQQ", len(name), 1, 12, 1, 0, 3, 15, size)
    return prefix+struct.pack("<I", 0x80000000)+tail+name+struct.pack("<I", len(omitted))+omitted+values+mask


class TemporaryTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="container-python-tests-")
        self.root = Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def path(self, name):
        return self.root/name


class DenseDataTests(TemporaryTest):
    def test_independent_binary_fixture_and_raw_names(self):
        fixture = independent_dense_fixture()
        self.path("fixture.arr").write_bytes(fixture)
        volume = read_dense_array(self.path("fixture.arr"))
        self.assertEqual(volume.origin, (-2, 3, 4))
        self.assertEqual(volume.dimensions, (3, 1, 1))
        self.assertEqual(volume.values.tobytes(), struct.pack("<3I", 0, 0x80000000, 1))
        self.assertEqual(volume.background.tobytes(), struct.pack("<I", 0x80000000))
        self.assertEqual(volume.grid_name.encode("utf-8", "surrogateescape"), b"raw\xff")
        self.assertEqual(volume.omitted_metadata[0].encode("utf-8", "surrogateescape"), b"artist\xfe")
        self.assertEqual(volume.index_to_world((-1, 3, 4)), (.8, 2.9, 4.6))
        self.assertTrue(volume.active_at((-1, 3, 4)))
        self.assertFalse(volume.active_at((-2, 3, 4)))
        self.assertEqual(volume.active_voxels_outside, 12)
        self.assertTrue(volume.stored_nonbackground_outside)
        write_dense_array(self.path("rewritten.arr"), volume)
        self.assertEqual(self.path("rewritten.arr").read_bytes(), fixture)

    def test_all_four_native_buffers_preserve_ieee_bits(self):
        for dtype in DenseType:
            with self.subTest(dtype=dtype):
                width = dtype.width
                raw = struct.pack("<4I", 0, 0x80000000, 1, 0x7f7fffff) if width == 4 else struct.pack("<4Q", 0, 1 << 63, 1, 0x7fefffffffffffff)
                values = array(dtype.typecode);values.frombytes(raw*dtype.components)
                volume = DenseVolume(dtype, (2, 2, 1), values, bytearray((0, 1, 1, 0)),
                                     origin=(-1, 2, 3), linear=(.2, .1, 0, 0, .3, 0, 0, 0, .4),
                                     translation=(2, -3, 4), grid_name="typed", grid_class=GridClass.FOG_VOLUME,
                                     vector_type=VectorType.COVARIANT, world_space=False)
                path = self.path(f"{dtype}.arr")
                write_dense_array(path, volume)
                recovered = read_dense_array(path)
                self.assertEqual(recovered.values.tobytes(), values.tobytes())
                self.assertEqual(recovered.active, volume.active)
                self.assertEqual(recovered.linear, volume.linear)
                self.assertEqual(recovered.vector_type, VectorType.COVARIANT)
                self.assertFalse(recovered.world_space)
                self.assertEqual(memoryview(recovered.values).nbytes, len(values)*width)
                self.assertEqual(recovered.value_at((0, 3, 3)), volume.value_at((0, 3, 3)))

    def test_corrupt_headers_counts_payload_and_nonfinite_values_reject(self):
        fixture = independent_dense_fixture()
        mutations = [(0, b"X"), (8, struct.pack("<I", 2)), (12, struct.pack("<I", 20)),
                     (16, struct.pack("<I", 20)), (24, struct.pack("<I", 2)), (28, b"\1"),
                     (44, struct.pack("<I", 0)), (156, struct.pack("<I", 257)),
                     (160, struct.pack("<I", 4097)), (172, struct.pack("<I", 2)),
                     (176, b"\1"), (180, struct.pack("<Q", 4)),
                     (188, struct.pack("<Q", 14)), (196, struct.pack("<Q", len(fixture)+1)),
                     (152, struct.pack("<I", 0x7f800000)), (len(fixture)-1, b"\2"),
                     (len(fixture)-15, struct.pack("<I", 0x7fc00000))]
        for n, (offset, replacement) in enumerate(mutations):
            with self.subTest(offset=offset):
                raw = bytearray(fixture);raw[offset:offset+len(replacement)] = replacement
                path = self.path(f"invalid{n}.arr");path.write_bytes(raw)
                with self.assertRaises(DataError):
                    read_dense_array(path)
        for n, raw in enumerate((fixture[:-1], fixture+b"x", fixture[:20])):
            path = self.path(f"short{n}.arr");path.write_bytes(raw)
            with self.assertRaises(DataError):
                read_dense_array(path)

    def test_budgets_precision_mutation_and_exclusive_output(self):
        volume = DenseVolume(DenseType.FLOAT64, (2, 1, 1), array("d", (0, -0.)), bytearray((0, 1)))
        with self.assertRaises(LimitError):
            write_dense_array(self.path("small.arr"), volume, Limits(max_bytes=1024))
        self.assertFalse(self.path("small.arr").exists())
        with self.assertRaises(LimitError):
            volume.validate(Limits(max_voxels=1))
        volume.values[0] = math.nan
        with self.assertRaises(DataError):
            volume.validate()
        volume.values[0] = 0;volume.world_space = 1
        with self.assertRaises(DataError):
            volume.validate()
        volume.world_space = True
        self.path("existing").write_bytes(b"keep")
        with self.assertRaises(FileExistsError):
            write_dense_array(self.path("existing"), volume)
        self.assertEqual(self.path("existing").read_bytes(), b"keep")
        volume.origin=(1_000_000_000, 0, 0)
        volume.linear=(math.nextafter(1e6, 0), 0, 0, 0, 1e-6, 0, 0, 0, 1)
        volume.translation=(-1e15, 0, 0)
        with self.assertRaisesRegex(DataError, "precision"):
            volume.validate()


class PointDataTests(TemporaryTest):
    def test_all_eight_types_and_exact_int64_buffers(self):
        fixtures = {"bool": [True, False], "int32": [-2**31, 2**31-1],
                    "int64": [-2**63, 2**63-1, 2**53+1], "float": [0., -0., 1e-40],
                    "double": [5e-324, -0., 1e300], "vec3f": [(1, -0., 3), (4, 5, 6)],
                    "vec3d": [(1e-300, 2, 1e300)], "string": ["blue", "蓝", "a\0b"]}
        for dtype, values in fixtures.items():
            with self.subTest(dtype=dtype):
                column = PointColumn.from_values(dtype, values).validate()
                self.assertEqual(column.count, len(values))
                self.assertEqual(column.at(0), tuple(values[0]) if dtype.startswith("vec3") else values[0])
                if dtype != "string":
                    self.assertGreater(memoryview(column.values).nbytes, 0)
                if dtype == "int64":
                    self.assertEqual(column.at(1), 9223372036854775807)
                    self.assertEqual(column.at(2), 9007199254740993)
                if dtype in ("float", "double"):
                    self.assertEqual(math.copysign(1, column.at(1)), -1)
        group = PointGroup.from_document({"version": 1, "traversal": TRAVERSAL, "group": "g", "values": [True, False]})
        self.assertEqual(group.membership, b"\1\0")
        self.assertEqual(group.booleans(), (True, False))
        with self.assertRaises(DataError):
            group.at(-1)

    def test_integer_float32_single_rounding_and_random_midpoints(self):
        midpoint = 1152921573326323712
        values = [midpoint+1, -midpoint-1, midpoint, midpoint-1]
        scalar = PointColumn.from_values("float", values)
        self.assertEqual([bits32(x) for x in scalar.values], [0x5d800001, 0xdd800001, 0x5d800000, 0x5d800000])
        vector = PointColumn.from_values("vec3f", [values[:3]])
        self.assertEqual([bits32(x) for x in vector.values], [0x5d800001, 0xdd800001, 0x5d800000])
        # Independent exact decimal distances between neighboring IEEE values.
        rng = random.Random(731)
        for _ in range(200):
            exponent = rng.randrange(55, 125)
            significand = rng.randrange(1 << 23, (1 << 24)-1)
            lower = significand << (exponent-23)
            upper = (significand+1) << (exponent-23)
            halfway = (lower+upper)//2
            number = halfway+rng.choice((-1, 0, 1))
            candidate = PointColumn.from_values("float", [number]).at(0)
            with localcontext() as context:
                context.prec = 200
                dl, du = abs(Decimal(number)-Decimal(lower)), abs(Decimal(number)-Decimal(upper))
            expected = lower if dl < du or (dl == du and significand % 2 == 0) else upper
            self.assertEqual(int(candidate), expected)
        with self.assertRaises(DataError):
            PointColumn.from_values("float", [1 << 128])

    def test_typed_range_schema_json_and_shared_budgets(self):
        for dtype, values in (("bool", [1]), ("int64", [1.0]), ("int32", [2**31]),
                              ("float", [1e-46]), ("float", [1e40]), ("double", [math.inf]),
                              ("vec3d", [[1, 2]]), ("string", ["\udcff"])):
            with self.subTest(dtype=dtype, values=values), self.assertRaises((DataError, UnicodeError)):
                PointColumn.from_values(dtype, values)
        with self.assertRaises(LimitError):
            PointColumn.from_values("int64", [1, 2], Limits(max_points=1))
        with self.assertRaises(LimitError):
            PointColumn.from_values("string", ["four"], Limits(max_string_bytes=3))
        with self.assertRaises(LimitError):
            PointColumn.from_values("double", [1], Limits(max_bytes=1024))
        column = PointColumn.from_document(column_document())
        self.assertEqual(column.at(0), 2**63-1)
        for change in ({"version": 2}, {"traversal": "active_only"}, {"extra": 1}):
            value = column_document();value.update(change)
            with self.assertRaises(DataError):
                PointColumn.from_document(value)
        for type_value in (None, [], {}, True, "unsupported"):
            bad=column_document();bad["type"]=type_value
            with self.assertRaises(DataError):
                PointColumn.from_document(bad)
        mismatch=column_document();mismatch["attribute"]["value_type"]="double"
        with self.assertRaises(DataError):
            PointColumn.from_document(mismatch)
        for raw in (b'{"x":1,"x":2}', b'{"v":NaN}', b'{"v":1e-999}', b'{"v":1e999}', b'"\xff"', b'{"v":'+b'1'*129+b'}'):
            with self.assertRaises(DataError):
                decode_json(raw)
        with self.assertRaises(LimitError):
            decode_json(b'"'+b'a'*100+b'"', Limits(max_bytes=1024))
        column = PointColumn.from_values("double", [1]);column.values[0] = math.nan
        with self.assertRaises(DataError):
            column.validate()

    def test_streamed_edits_exact_integers_boolean_groups_and_position_protection(self):
        edits = PointEdits((AttributeEdit("add", "id", PointColumn.from_values("int64", [2**63-1, 2**53+1])),
                            AttributeEdit("add", "label", PointColumn.from_values("string", ["a\0b", "蓝"]))),
                           (GroupEdit("add", "g", [True, False]),))
        edits.write(self.path("edits.json"))
        raw = self.path("edits.json").read_bytes();document = json.loads(raw)
        self.assertIn(b"9223372036854775807", raw)
        self.assertEqual(document["attributes"][0]["values"], [2**63-1, 2**53+1])
        self.assertIs(document["groups"][0]["values"][0], True)
        self.assertEqual(document["attributes"][1]["values"][0], "a\0b")
        with self.assertRaises(FileExistsError):
            edits.write(self.path("edits.json"))
        self.assertEqual(self.path("edits.json").read_bytes(), raw)
        for op in ("add", "set", "drop", "rename"):
            with self.assertRaisesRegex(DataError, "protected"):
                AttributeEdit(op, "P").validate()
        with self.assertRaises(DataError):
            GroupEdit("add", "g", [1]).validate()
        with self.assertRaises(DataError):
            PointEdits((object(),), ()).validate()
        with self.assertRaises(LimitError):
            edits.validate(Limits(max_edits=1))

    def test_standard_point_defaults_and_statistics_types(self):
        cloud = PointCloud(array("d", [0, 1, 2, 3, 4, 5])).validate()
        self.assertEqual(cloud.velocities, array("d", [0]*6))
        self.assertEqual(cloud.radii, array("d", [1, 1]))
        cloud.write(self.path("standard.json"))
        recovered = PointCloud.from_document(json.loads(self.path("standard.json").read_text()))
        self.assertEqual(recovered.positions, cloud.positions)
        with self.assertRaises(LimitError):
            PointCloud(array("d", [0]*6), limits=Limits(max_points=1))
        cloud.radii[0] = 0
        with self.assertRaises(DataError):
            cloud.validate()
        stats = {"selectedCount": 2, "hasValues": True, "componentCount": 1, "valueType": "int64",
                 "minimum": 2**63-2, "maximum": 2**63-1, "mean": [float(2**63-1)], "populationStdDev": [.5]}
        parsed = PointStatistics.from_document(stats)
        self.assertEqual(parsed.maximum, 2**63-1)
        for value_type, minimum in (("int64", True), ("double", False), ("bool", 1), ("string", "a"), ("int32", 2**31)):
            bad = dict(stats, valueType=value_type, minimum=minimum)
            with self.assertRaises(DataError):
                PointStatistics.from_document(bad)
        with self.assertRaises(DataError):
            PointCounts.from_document({"total": 1, "selected": 2, "selectedActive": 2, "selectedInactive": 0,
                                       "archivedSchema": False, "gridName": "points"})


class RuntimeControlTests(TemporaryTest):
    def fixture(self, *, available=True, mode="normal", protocols=None):
        report = {"schemaVersion": 1, "tool": "OpenVdbTool", "openvdbAvailable": available,
                  "openvdbVersion": "12.0.1" if available else None,
                  "protocols": protocols if protocols is not None else {"denseArray": 1, "pointAttributes": 1, "pointSelection": 1, "fileAuthoringSpec": 0}}
        script = self.path("runtime.py")
        script.write_text("import json,sys,time\n"+
                          f"report={report!r}\nmode={mode!r}\n"+
                          "if mode=='timeout':time.sleep(5)\n"+
                          "if mode=='overflow':print('x'*100000)\n"+
                          "elif '--capabilities' in sys.argv: print(json.dumps(report))\n"+
                          "elif mode=='write_overflow':\n open(sys.argv[3],'wb').write(b'private artifact');print('x'*100000)\n"+
                          "else: print('intentional tool failure',file=sys.stderr);sys.exit(7)\n", encoding="utf-8")
        return [sys.executable, script]

    def test_off_and_missing_protocol_gate_before_paths_or_data(self):
        client = ToolClient(self.fixture(available=False))
        for operation in (lambda: client.write_dense(None, self.path("missing/out")),
                          lambda: client.edit_points("missing", "missing/out", None),
                          lambda: client.count_points("missing"),
                          lambda: client.author_file("missing", "missing/out")):
            with self.assertRaisesRegex(CapabilityError, "ENABLE_OPENVDB=ON"):
                operation()
        client = ToolClient(self.fixture(protocols={"denseArray": 0}))
        with self.assertRaises(CapabilityError):
            client.read_dense("missing", (0, 0, 0), (1, 1, 1))
        with self.assertRaises(CapabilityError):
            ToolClient(self.path("missing.exe"))

    def test_timeout_report_cap_identity_and_process_failure(self):
        with self.assertRaisesRegex(CapabilityError, "timeout"):
            ToolClient(self.fixture(mode="timeout"), timeout=.1)
        with self.assertRaisesRegex(CapabilityError, "max_report_bytes"):
            ToolClient(self.fixture(mode="overflow"), limits=Limits(max_report_bytes=1024))
        client = ToolClient(self.fixture())
        with self.assertRaises(ToolError) as caught:
            client.count_points("missing")
        self.assertEqual(caught.exception.returncode, 7)
        self.assertIn("intentional tool failure", caught.exception.stderr)
        with self.assertRaises(DataError):
            PointSelection(any_of=("",)).arguments()
        with self.assertRaises(DataError):
            ToolClient(self.fixture(), timeout=0)

    def test_malformed_capabilities_and_typed_reports_reject(self):
        baseline={"schemaVersion":1,"tool":"OpenVdbTool","openvdbAvailable":True,
                  "openvdbVersion":"12.0.1","protocols":{"denseArray":1}}
        for change in ({"schemaVersion":2},{"schemaVersion":True},{"tool":"other"},
                       {"openvdbAvailable":1},{"openvdbVersion":12},{"protocols":{"denseArray":True}},
                       {"protocols":[]},{"pointTraversal":"active_only"},{"pointTypes":["double"]},{"extra":0}):
            value=dict(baseline,**change)
            with self.subTest(change=change),self.assertRaises((CapabilityError,DataError)):
                Capabilities.from_document(value)

    def test_failed_private_artifact_is_never_published(self):
        client=ToolClient(self.fixture(mode="write_overflow"),limits=Limits(max_report_bytes=1024))
        with self.assertRaisesRegex(LimitError,"max_report_bytes"):
            client.select_points("missing-input",self.path("output.vdb"))
        self.assertFalse(self.path("output.vdb").exists())


@unittest.skipUnless(os.environ.get("CONTAINER_OPENVDB_TOOL"), "set CONTAINER_OPENVDB_TOOL for native roundtrips")
class NativeRoundtripTests(TemporaryTest):
    @classmethod
    def setUpClass(cls):
        cls.client = ToolClient(os.environ["CONTAINER_OPENVDB_TOOL"])
        if not cls.client.capabilities.available:
            raise unittest.SkipTest("selected runtime has ENABLE_OPENVDB=OFF")

    def make_points(self):
        cloud = PointCloud(array("d", [.01, .02, .03, .01, .02, .03, .01, .02, .03]),
                           array("d", [1, 2, 3, -1, -2, -3, 4, 5, 6]), array("d", [.1, .2, .3]))
        self.client.write_points(cloud, self.path("points.vdb"), voxel_size=.1, grid_name="source")
        return cloud

    def test_all_dense_types_native_bits_mask_affine_and_tags(self):
        for dtype in DenseType:
            with self.subTest(dtype=dtype):
                raw = struct.pack("<4I", 0, 0x80000000, 1, 0x3f800000) if dtype.width == 4 else struct.pack("<4Q", 0, 1 << 63, 1, 0x3ff0000000000000)
                values = array(dtype.typecode);values.frombytes(raw*dtype.components)
                source = DenseVolume(dtype, (2, 2, 1), values, bytearray([0, 0, 1, 1]), origin=(-1, 2, 0),
                                     linear=(.2, .1, 0, 0, .3, 0, 0, 0, .4), translation=(1, 2, 3),
                                     background=array(dtype.typecode, [-0.]*dtype.components),
                                     grid_name="dense", grid_class=GridClass.FOG_VOLUME,
                                     vector_type=VectorType.COVARIANT, world_space=False)
                path=self.path(f"dense{dtype}.vdb")
                self.client.write_dense(source, path)
                result=self.client.read_dense(path, source.origin, source.dimensions, grid="dense")
                self.assertEqual(result.values.tobytes(), source.values.tobytes())
                self.assertEqual(result.active, source.active)
                self.assertEqual(result.background.tobytes(), source.background.tobytes())
                self.assertEqual(result.linear, source.linear)
                self.assertEqual(result.translation, source.translation)
                self.assertEqual(result.grid_class, source.grid_class)
                self.assertEqual(result.vector_type, source.vector_type)
                self.assertEqual(result.world_space, source.world_space)

    def test_standard_points_and_all_native_columns_groups_roundtrip(self):
        cloud=self.make_points();before=self.path("points.vdb").read_bytes()
        roundtrip=self.client.read_points(self.path("points.vdb"), grid="source")
        for actual, expected in zip(roundtrip.positions, cloud.positions):
            self.assertAlmostEqual(actual, expected, delta=1e-8)
        # Standard writer deterministically sorts complete records before native
        # voxel repartitioning. Native column edits use the readback traversal.
        actual = [(tuple(roundtrip.velocities[3*i:3*i+3]), roundtrip.radii[i]) for i in range(3)]
        expected = sorted((tuple(cloud.velocities[3*i:3*i+3]), cloud.radii[i]) for i in range(3))
        self.assertEqual(actual, expected)
        fixtures={"bool":[True,False,True],"int32":[-2**31,0,2**31-1],"int64":[-2**63,2**53+1,2**63-1],
                  "float":[0.,-0.,1.25],"double":[-0.,5e-324,1e300],
                  "vec3f":[(0.,-0.,3.),(4,5,6),(7,8,9)],
                  "vec3d":[(0.,-0.,3.),(1e-200,5,6),(7,8,1e200)],"string":["blue","蓝","a\0b"]}
        columns={dtype:PointColumn.from_values(dtype,values) for dtype,values in fixtures.items()}
        edits=PointEdits(tuple(AttributeEdit("add","a_"+dtype,column) for dtype,column in columns.items()),
                         (GroupEdit("add","selected",[True,False,True]),))
        self.client.edit_points(self.path("points.vdb"), self.path("edited.vdb"), edits, grid="source", grid_name="edited")
        schema=self.client.point_schema(self.path("edited.vdb"), grid="edited")
        self.assertEqual(schema.point_count,3)
        self.assertIn("selected",schema.groups)
        for dtype,column in columns.items():
            recovered=self.client.read_column(self.path("edited.vdb"),"a_"+dtype,grid="edited")
            self.assertEqual(recovered.dtype,dtype)
            self.assertEqual(recovered.values if dtype=="string" else recovered.values.tobytes(), column.values if dtype=="string" else column.values.tobytes())
        self.assertEqual(self.client.read_group(self.path("edited.vdb"),"selected").booleans(),(True,False,True))
        self.assertEqual(self.path("points.vdb").read_bytes(),before)
        with self.assertRaises(ToolError):
            self.client.read_points(self.path("edited.vdb"))

    def test_selection_statistics_exact_ids_empty_archive_and_protection(self):
        self.make_points();ids=PointColumn.from_values("int64",[2**63-3,2**63-2,2**63-1])
        edits=PointEdits((AttributeEdit("add","id",ids),),
                         (GroupEdit("add","a",[True,False,True]),GroupEdit("add","b",[False,True,True])))
        self.client.edit_points(self.path("points.vdb"),self.path("edited.vdb"),edits)
        original=self.path("edited.vdb").read_bytes()
        selection=PointSelection(any_of=("a",),all_of=("b",))
        counts=self.client.count_points(self.path("edited.vdb"),selection)
        self.assertEqual((counts.total,counts.selected,counts.selected_active,counts.selected_inactive),(3,1,1,0))
        stats=self.client.point_statistics(self.path("edited.vdb"),"id")
        self.assertEqual((stats.minimum,stats.maximum),(2**63-3,2**63-1))
        self.assertAlmostEqual(stats.population_stddev[0],math.sqrt(2/3))
        self.client.select_points(self.path("edited.vdb"),self.path("selected.vdb"),selection)
        self.assertEqual(self.client.read_column(self.path("selected.vdb"),"id").at(0),2**63-1)
        self.client.select_points(self.path("edited.vdb"),self.path("empty.vdb"),delete=True)
        empty=self.client.count_points(self.path("empty.vdb"))
        self.assertEqual(empty.total,0);self.assertTrue(empty.archived_schema)
        with self.assertRaises(FileExistsError):
            self.client.select_points(self.path("edited.vdb"),self.path("edited.vdb"))
        self.assertEqual(self.path("edited.vdb").read_bytes(),original)
        with self.assertRaises(ToolError):
            self.client.select_points(self.path("edited.vdb"),self.path("failed.vdb"),PointSelection(any_of=("missing",)))
        self.assertFalse(self.path("failed.vdb").exists())

    def test_file_authoring_capability_gate_or_real_relative_spec(self):
        if self.client.capabilities.protocols.get("fileAuthoringSpec") != 1:
            with self.assertRaises(CapabilityError):
                self.client.author_file("missing",self.path("missing/out"))
            return
        self.make_points();source=self.path("points.vdb").read_bytes()
        specification={"version":1,"inputs":[{"path":"points.vdb","edits":[{"selector":"source","newName":"renamed",
                        "metadata":[{"name":"exact_id","operation":"set","type":"int64","value":2**63-1}]}]}]}
        self.path("spec.json").write_text(json.dumps(specification),encoding="utf-8")
        self.client.author_file(self.path("spec.json"),self.path("authored.vdb"))
        self.assertEqual(self.client.count_points(self.path("authored.vdb"),grid="renamed").total,3)
        self.assertEqual(self.path("points.vdb").read_bytes(),source)

    def test_native_unicode_paths_grid_names_and_string_values(self):
        source=self.path("点云_é.vdb");edited=self.path("属性_é.vdb")
        cloud=PointCloud(array("d",[.1,.2,.3]))
        self.client.write_points(cloud,source,grid_name="粒子_é")
        self.assertEqual(self.client.count_points(source,grid="粒子_é").total,1)
        self.client.edit_points(source,edited,PointEdits(
            (AttributeEdit("add","id",PointColumn.from_values("int64",[2**63-1])),
             AttributeEdit("add","label",PointColumn.from_values("string",["标签_é"]))),
            (GroupEdit("add","selected",[True]),)),grid="粒子_é",grid_name="输出_é")
        self.assertEqual(self.client.read_column(edited,"id",grid="输出_é").at(0),2**63-1)
        self.assertEqual(self.client.read_column(edited,"label",grid="输出_é").at(0),"标签_é")
        self.assertEqual(self.client.read_group(edited,"selected",grid="输出_é").booleans(),(True,))


if __name__ == "__main__":
    unittest.main()
