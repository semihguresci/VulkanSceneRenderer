"""Bounded compiled-tool discovery and typed file workflows (never a shell)."""
from dataclasses import dataclass
from pathlib import Path
import math
import os
import shutil
import subprocess
import tempfile
import threading

from ._core import DEFAULT_LIMITS, FIXED, BridgeError, CapabilityError, DataError, LimitError, Limits, decode_json, integer, new_output, read_json, require_keys, utf8
from .dense import _shape, read_dense_array, write_dense_array
from .points import PointCloud, PointColumn, PointEdits, PointGroup, PointSchema


class ToolError(BridgeError):
    def __init__(self, argv, returncode, stdout, stderr):
        self.argv, self.returncode = tuple(argv), returncode
        self.stdout, self.stderr = stdout.decode("utf-8", "replace"), stderr.decode("utf-8", "replace")
        diagnostic = self.stderr.strip() or self.stdout.strip() or f"exit status{self.returncode}; check runtime DLL staging"
        super().__init__(f"OpenVdbTool failed: {diagnostic}")


@dataclass(frozen=True)
class Capabilities:
    available: bool
    version: object
    protocols: dict

    @classmethod
    def from_document(cls, document):
        require_keys(document, {"schemaVersion", "tool", "openvdbAvailable", "openvdbVersion", "protocols"}, {"pointTypes", "pointTraversal"}, what="capability report")
        if type(document["schemaVersion"]) is not int or document["schemaVersion"] != 1 or document["tool"] != "OpenVdbTool":
            raise CapabilityError("unsupported OpenVdbTool capability schema/tool identity")
        if type(document["openvdbAvailable"]) is not bool or (document["openvdbVersion"] is not None and type(document["openvdbVersion"]) is not str):
            raise CapabilityError("invalid OpenVDB availability/version report")
        protocols = document["protocols"]
        if type(protocols) is not dict or any(type(key) is not str or type(value) is not int or value < 0 for key, value in protocols.items()):
            raise CapabilityError("invalid OpenVDB protocol versions")
        if "pointTraversal" in document and document["pointTraversal"] != "leaf_origin_xyz_then_local_index":
            raise CapabilityError("unsupported native point traversal")
        if "pointTypes" in document and (type(document["pointTypes"]) is not list or document["pointTypes"] != ["bool", "int32", "int64", "float", "double", "vec3f", "vec3d", "string"]):
            raise CapabilityError("unsupported native point-type catalog")
        return cls(document["openvdbAvailable"], document["openvdbVersion"], dict(protocols))


@dataclass(frozen=True)
class PointSelection:
    any_of: tuple = ()
    all_of: tuple = ()
    none_of: tuple = ()

    def arguments(self, limits=DEFAULT_LIMITS):
        arguments = []
        count = 0
        for option, names in (("--any-group", self.any_of), ("--all-group", self.all_of), ("--exclude-group", self.none_of)):
            if not isinstance(names, (tuple, list)):
                raise DataError("group selectors require sized name sequences")
            for name in names:
                count += 1
                if count > 3 * limits.max_groups:
                    raise LimitError("point selection exceeds group-name limit")
                utf8(name, "selection group", 256, name=True)
                arguments.extend((option, name))
        return arguments


@dataclass(frozen=True)
class PointCounts:
    total: int
    selected: int
    selected_active: int
    selected_inactive: int
    archived_schema: bool
    grid_name: str

    @classmethod
    def from_document(cls, value, limits=DEFAULT_LIMITS):
        require_keys(value, {"total", "selected", "selectedActive", "selectedInactive", "archivedSchema", "gridName"}, what="point count report")
        for key in ("total", "selected", "selectedActive", "selectedInactive"):
            integer(value[key], 0, limits.max_points, key)
        if value["selected"] > value["total"] or value["selectedActive"]+value["selectedInactive"] != value["selected"] or type(value["archivedSchema"]) is not bool:
            raise DataError("inconsistent point counts/flags")
        utf8(value["gridName"], "point grid name")
        return cls(value["total"], value["selected"], value["selectedActive"], value["selectedInactive"], value["archivedSchema"], value["gridName"])


@dataclass(frozen=True)
class PointStatistics:
    selected_count: int
    has_values: bool
    component_count: int
    value_type: str
    minimum: object
    maximum: object
    mean: tuple
    population_stddev: tuple

    @classmethod
    def from_document(cls, value, limits=DEFAULT_LIMITS):
        require_keys(value, {"selectedCount", "hasValues", "componentCount", "valueType", "minimum", "maximum", "mean", "populationStdDev"}, what="point statistics report")
        count = integer(value["selectedCount"], 0, limits.max_points, "selected count")
        components = integer(value["componentCount"], 1, 3, "statistics component count")
        if components not in (1, 3) or type(value["hasValues"]) is not bool or value["hasValues"] != (count != 0):
            raise DataError("inconsistent point statistics count/flags")
        expected_components = {"bool": 1, "int32": 1, "int64": 1, "float": 1, "double": 1, "vec3s": 3, "vec3d": 3}
        value_type = value["valueType"]
        if type(value_type) is not str or expected_components.get(value_type) != components:
            raise DataError("unsupported statistics value type/component count")
        def finite_number(x):
            if type(x) not in (int, float):
                return False
            try:
                return math.isfinite(x)
            except OverflowError:
                return False
        for key in ("mean", "populationStdDev"):
            if type(value[key]) is not list or len(value[key]) != components or any(not finite_number(x) for x in value[key]):
                raise DataError("invalid point statistics component diagnostics")
        if any(x < 0 for x in value["populationStdDev"]):
            raise DataError("population standard deviation must be nonnegative")
        for key in ("minimum", "maximum"):
            extrema = value[key]
            if not count:
                if extrema is not None:
                    raise DataError("empty point statistics extrema must be null")
            else:
                scalars = extrema if components == 3 else [extrema]
                if type(scalars) is not list or len(scalars) != components:
                    raise DataError("invalid typed point statistics extrema")
                for scalar in scalars:
                    if value_type == "bool":
                        if type(scalar) is not bool:
                            raise DataError("bool statistics extrema require booleans")
                    elif value_type in ("int32", "int64"):
                        bits = 32 if value_type == "int32" else 64
                        integer(scalar, -(1 << (bits-1)), (1 << (bits-1))-1, "statistics integer extremum")
                    elif not finite_number(scalar):
                        raise DataError("floating statistics extrema require finite numbers")
                    elif value_type in ("float", "vec3s") and abs(scalar) > 3.4028234663852886e38:
                        raise DataError("Float statistics extremum exceeds native range")
        return cls(count, value["hasValues"], components, value["valueType"], value["minimum"], value["maximum"], tuple(value["mean"]), tuple(value["populationStdDev"]))


class ToolClient:
    """A discovered OpenVdbTool runtime with typed, bounded file workflows.

    Supply its executable path, a trusted argv prefix, CONTAINER_OPENVDB_TOOL,
    or put OpenVdbTool on PATH. Never guesses build directories or downloads DLLs.
    Capabilities are queried before any workflow input/output access. Runtime
    limits are shared with retained Python buffers; they are logical payload
    bounds, not a combined process RSS sandbox.
    """
    def __init__(self, executable=None, *, limits=DEFAULT_LIMITS, timeout=60):
        if not isinstance(limits, Limits):
            raise DataError("tool limits must be a Limits instance")
        if type(timeout) not in (int, float) or not math.isfinite(timeout) or not 0 < timeout <= 3600:
            raise DataError("tool timeout must be in (0,3600] seconds")
        self.limits, self.timeout = limits, timeout
        if executable is None:
            executable = os.environ.get("CONTAINER_OPENVDB_TOOL") or shutil.which("OpenVdbTool")
        if not executable:
            raise CapabilityError("OpenVdbTool was not found; supply its path or CONTAINER_OPENVDB_TOOL")
        if isinstance(executable, (str, os.PathLike)):
            self.command = [os.fspath(executable)]
        else:
            if not isinstance(executable, (list, tuple)) or not 1 <= len(executable) <= 16:
                raise CapabilityError("trusted tool command prefix requires1..16 sized arguments")
            self.command = [os.fspath(value) for value in executable]
        if not self.command or len(self.command) > 16 or any(not value or "\0" in value for value in self.command):
            raise CapabilityError("invalid trusted OpenVdbTool command prefix")
        try:
            self.capabilities = Capabilities.from_document(self._json_command(["--capabilities"]))
        except (BridgeError, OSError, DataError) as error:
            raise CapabilityError(f"cannot discover OpenVdbTool capabilities: {error}") from error

    def require(self, protocol, version=1):
        if not self.capabilities.available:
            raise CapabilityError("OpenVDB is unavailable in this runtime; configure with -DENABLE_OPENVDB=ON")
        if self.capabilities.protocols.get(protocol) != version:
            raise CapabilityError(f"runtime requires supported {protocol} protocol{version}; reported{self.capabilities.protocols.get(protocol, 0)}")

    def _run(self, arguments):
        argv = self.command + [str(value) for value in arguments]
        try:
            process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell=False)
        except OSError as error:
            raise BridgeError(f"cannot start OpenVdbTool: {error}") from error
        outputs, overflow, failures = [bytearray(), bytearray()], threading.Event(), []
        def reader(pipe, target):
            try:
                while True:
                    chunk = pipe.read(65536)
                    if not chunk:
                        break
                    remaining = self.limits.max_report_bytes-len(target)
                    target.extend(chunk[:remaining])
                    if len(chunk) > remaining:
                        overflow.set()
                        process.kill()
                        break
            except OSError as error:
                failures.append(error)
        threads = [threading.Thread(target=reader, args=(pipe, target), daemon=True) for pipe, target in zip((process.stdout, process.stderr), outputs)]
        for thread in threads:
            thread.start()
        timed_out = False
        try:
            process.wait(timeout=self.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            process.kill()
            process.wait(timeout=5)
        for thread in threads:
            thread.join(timeout=2)
        if any(thread.is_alive() for thread in threads):
            raise BridgeError("OpenVdbTool output pipes did not close after process termination")
        process.stdout.close();process.stderr.close()
        if timed_out:
            raise BridgeError(f"OpenVdbTool exceeded timeout{self.timeout}s")
        if overflow.is_set():
            raise LimitError("OpenVdbTool report exceeds max_report_bytes")
        if failures:
            raise BridgeError(f"cannot read OpenVdbTool report: {failures[0]}")
        if process.returncode:
            raise ToolError(argv, process.returncode, *outputs)
        return bytes(outputs[0])

    def _json_command(self, arguments):
        return decode_json(self._run(arguments), self.limits)

    def _options(self, *, grid=None, retained=0, points=False, attributes=False):
        reserve = retained + 2*self.limits.max_report_bytes + FIXED
        self.limits.charge(reserve+1, "Python buffers/tool reports")
        result = ["--max-input-bytes", str(self.limits.max_bytes-reserve), "--max-voxels", str(self.limits.max_voxels)]
        if grid is not None:
            utf8(grid, "grid selector", 4096, name=True)
            result.extend(("--input-grid", grid))
        if points or attributes:
            result.extend(("--max-points", str(self.limits.max_points)))
        if attributes:
            for option, value in (("--max-attributes", self.limits.max_attributes),
                                  ("--max-groups", self.limits.max_groups), ("--max-edits", self.limits.max_edits),
                                  ("--max-string-bytes", self.limits.max_string_bytes)):
                result.extend((option, str(value)))
        return result

    def _publish(self, source, output, retained=0):
        # Compiled commands write only inside our private directory. Publish
        # afterward with exclusive creation, so timeout/report failures never
        # leave a user-visible half-written artifact or remove a racing file.
        size = source.stat().st_size
        self.limits.charge(size+retained+FIXED, "encoded native output/retained Python buffers")
        owned = False
        try:
            with open(source, "rb") as reader, open(output, "xb") as writer:
                owned = True
                remaining = size
                while remaining:
                    chunk = reader.read(min(65536, remaining))
                    if not chunk:
                        raise DataError("private native artifact changed size")
                    writer.write(chunk)
                    remaining -= len(chunk)
                if reader.read(1):
                    raise DataError("private native artifact changed size")
        except BaseException:
            if owned:
                output.unlink(missing_ok=True)
            raise
        return output

    def read_dense(self, input, origin, dimensions, *, grid=None):
        self.require("denseArray")
        if not isinstance(origin, (tuple, list)) or not isinstance(dimensions, (tuple, list)):
            raise DataError("dense origin/dimensions require sized three-integer sequences")
        _shape(dimensions, origin, self.limits)
        with tempfile.TemporaryDirectory(prefix="container-vdb-python-") as directory:
            output = Path(directory)/"array.cvarr"
            self._run(["--dense-export", Path(input).resolve(), output,
                                "--dense-origin", *origin, "--dense-dimensions", *dimensions, *self._options(grid=grid)])
            return read_dense_array(output, self.limits)

    def write_dense(self, volume, output):
        self.require("denseArray")
        output = new_output(output)
        volume.validate(self.limits)
        with tempfile.TemporaryDirectory(prefix="container-vdb-python-") as directory:
            source = Path(directory)/"array.cvarr"
            write_dense_array(source, volume, self.limits)
            native_output = Path(directory)/"output.vdb"
            self._run(["--dense-import", source, native_output, *self._options(retained=volume.memory_bytes+FIXED)])
            self._publish(native_output, output, volume.memory_bytes+FIXED)
        return output

    def point_schema(self, input, *, grid=None):
        self.require("pointAttributes")
        with tempfile.TemporaryDirectory(prefix="container-vdb-python-") as directory:
            output = Path(directory)/"schema.json"
            self._run(["--point-schema", Path(input).resolve(), output, *self._options(grid=grid, attributes=True)])
            return PointSchema.from_document(read_json(output, self.limits), self.limits)

    def read_column(self, input, name, *, grid=None):
        self.require("pointAttributes")
        utf8(name, "attribute name", 256, name=True)
        with tempfile.TemporaryDirectory(prefix="container-vdb-python-") as directory:
            output = Path(directory)/"column.json"
            self._run(["--point-attribute", Path(input).resolve(), name, output, *self._options(grid=grid, attributes=True)])
            return PointColumn.from_document(read_json(output, self.limits), self.limits)

    def read_group(self, input, name, *, grid=None):
        self.require("pointAttributes")
        utf8(name, "group name", 256, name=True)
        with tempfile.TemporaryDirectory(prefix="container-vdb-python-") as directory:
            output = Path(directory)/"group.json"
            self._run(["--point-group", Path(input).resolve(), name, output, *self._options(grid=grid, attributes=True)])
            return PointGroup.from_document(read_json(output, self.limits), self.limits)

    def edit_points(self, input, output, edits, *, grid=None, grid_name="points"):
        self.require("pointAttributes")
        if not isinstance(edits, PointEdits):
            raise DataError("edits must be a PointEdits batch")
        output = new_output(output)
        utf8(grid_name, "output grid name", 256, name=True)
        edits.validate(self.limits)
        with tempfile.TemporaryDirectory(prefix="container-vdb-python-") as directory:
            source = Path(directory)/"edits.json"
            edits.write(source, self.limits)
            native_output = Path(directory)/"output.vdb"
            self._run(["--edit-point-attributes", Path(input).resolve(), source, native_output, "--grid-name", grid_name,
                       *self._options(grid=grid, retained=edits.memory_bytes, attributes=True)])
            self._publish(native_output, output, edits.memory_bytes)
        return output

    def write_points(self, points, output, *, voxel_size=.1, grid_name="points"):
        self.require("pointAttributes")
        if not isinstance(points, PointCloud):
            raise DataError("standard points require PointCloud buffers")
        points.validate(self.limits)
        output = new_output(output)
        if type(voxel_size) not in (int, float) or not math.isfinite(voxel_size) or not 1.5e-5 <= voxel_size <= 1e6:
            raise DataError("point voxel_size requires [1.5e-5,1e6]")
        utf8(grid_name, "output grid name", 256, name=True)
        with tempfile.TemporaryDirectory(prefix="container-vdb-python-") as directory:
            source = Path(directory)/"points.json"
            points.write(source, self.limits)
            native_output = Path(directory)/"output.vdb"
            self._run(["--points", source, native_output, "--voxel-size", voxel_size, "--grid-name", grid_name,
                       *self._options(retained=points.memory_bytes+FIXED, points=True)])
            self._publish(native_output, output, points.memory_bytes+FIXED)
        return output

    def read_points(self, input, *, grid=None):
        self.require("pointAttributes")
        with tempfile.TemporaryDirectory(prefix="container-vdb-python-") as directory:
            output = Path(directory)/"points.json"
            self._run(["--export-points", Path(input).resolve(), output, *self._options(grid=grid, points=True)])
            return PointCloud.from_document(read_json(output, self.limits), self.limits)

    def count_points(self, input, selection=PointSelection(), *, grid=None):
        self.require("pointSelection")
        document = self._json_command(["--count-points", Path(input).resolve(), *selection.arguments(self.limits), *self._options(grid=grid, attributes=True)])
        return PointCounts.from_document(document, self.limits)

    def select_points(self, input, output, selection=PointSelection(), *, delete=False, grid=None, grid_name="points"):
        self.require("pointSelection")
        if type(delete) is not bool:
            raise DataError("delete must be an explicit boolean")
        output = new_output(output)
        utf8(grid_name, "output grid name", 256, name=True)
        with tempfile.TemporaryDirectory(prefix="container-vdb-python-") as directory:
            native_output = Path(directory)/"output.vdb"
            self._run(["--delete-points" if delete else "--select-points", Path(input).resolve(), native_output,
                       *selection.arguments(self.limits), "--grid-name", grid_name, *self._options(grid=grid, attributes=True)])
            self._publish(native_output, output)
        return output

    def point_statistics(self, input, attribute, selection=PointSelection(), *, grid=None):
        self.require("pointSelection")
        utf8(attribute, "attribute name", 256, name=True)
        document = self._json_command(["--point-statistics", Path(input).resolve(), attribute,
                                       *selection.arguments(self.limits), *self._options(grid=grid, attributes=True)])
        return PointStatistics.from_document(document, self.limits)

    def author_file(self, specification, output, *, max_spec_bytes=1024*1024, max_depth=16,
                    max_grids=256, max_metadata_bytes=1024*1024):
        """Apply native strict-v1 select/merge/rename/typed-metadata JSON.

        Paths inside the specification remain relative to its original directory.
        The native adapter validates and snapshots it; no Python JSON conversion
        can round integer metadata or expand its external limits.
        """
        self.require("fileAuthoringSpec")
        integer(max_spec_bytes, 1, 64*1024*1024, "max_spec_bytes")
        integer(max_depth, 1, 64, "max_depth")
        integer(max_grids, 1, 4096, "max_grids")
        integer(max_metadata_bytes, 1, 2**64-1, "max_metadata_bytes")
        output = new_output(output)
        with tempfile.TemporaryDirectory(prefix="container-vdb-python-") as directory:
            native_output = Path(directory)/"output.vdb"
            self._run(["--author-file", Path(specification).resolve(), native_output,
                       "--max-spec-bytes", max_spec_bytes, "--max-spec-depth", max_depth,
                       "--max-grids", max_grids, "--max-metadata-bytes", max_metadata_bytes,
                       "--max-attributes", self.limits.max_attributes, "--max-groups", self.limits.max_groups,
                       "--max-edits", self.limits.max_edits, *self._options(points=True)])
            self._publish(native_output, output)
        return output
