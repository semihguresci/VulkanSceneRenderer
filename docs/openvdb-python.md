# Python data bridge

`container_openvdb` exposes typed arrays and point columns through Container's
compiled `OpenVdbTool`. Python 3.10 or newer is required. The package has no
runtime dependencies; NumPy and the upstream `pyopenvdb` extension are optional
and are not used internally.

Install from the repository:

```powershell
python -m pip install ./python
$env:CONTAINER_OPENVDB_TOOL = 'F:/Projects/Container/out/build/visual-studio/Release/OpenVdbTool.exe'
```

For development, set `PYTHONPATH` to the repository's `python` directory instead
of installing. Supply an executable explicitly, set `CONTAINER_OPENVDB_TOOL`,
or put `OpenVdbTool` on `PATH`. Runtime DLLs must already be staged next to the
compiled executable. The package does not search build directories or download
native dependencies.

```python
from container_openvdb import ToolClient

vdb = ToolClient()                 # Or ToolClient('/path/to/OpenVdbTool')
print(vdb.capabilities.version)
print(vdb.capabilities.protocols)
```

Discovery calls `--capabilities`, checking schema version 1, tool identity and
protocol versions. File operations require their own protocol and an enabled
OpenVDB runtime before accessing workflow paths or validating data. An OFF build
reports an actionable `ENABLE_OPENVDB=ON` error. A newer command is never guessed
from a version string. The current protocols are `denseArray`, `pointAttributes`,
`pointSelection` and `fileAuthoringSpec`, each at version 1 when supported.

## Dense scalar and vector arrays

`DenseVolume` contains an `array('f')` or `array('d')`, plus a separate activity
byte for every voxel. X varies fastest, then Y, then Z. Vector values are
interleaved XYZ components. The `origin` is an absolute integer index coordinate;
the row-major `linear` matrix and `translation` define
`world = linear * index + translation`.

```python
from array import array
from container_openvdb import DenseType, DenseVolume, GridClass

volume = DenseVolume(
    DenseType.FLOAT32, (2, 2, 1),
    array('f', [0.0, -0.0, 2.0, 4.0]),
    bytearray([0, 0, 1, 1]),
    origin=(-1, 0, 0),
    linear=(0.2, 0, 0, 0, 0.2, 0, 0, 0, 0.2),
    grid_name='density', grid_class=GridClass.FOG_VOLUME,
)
vdb.write_dense(volume, 'density.vdb')
crop = vdb.read_dense('density.vdb', (-1, 0, 0), (2, 2, 1), grid='density')
assert crop.values.tobytes() == volume.values.tobytes()
assert crop.active == volume.active
print(crop.index_to_world((-1, 0, 0)))
```

Supported `DenseType` values are `FLOAT32`, `FLOAT64`, `VEC3_FLOAT32` and
`VEC3_FLOAT64`. A memoryview of `volume.values` exposes a contiguous native buffer;
for example, `numpy.frombuffer(volume.values, dtype=numpy.float32)` can share a
Float buffer. Python arrays use host endianness; binary files always use little
endianness. Masks use one byte per cell and accept only zero or one. Background,
grid class, vector interpretation and world-space flags are explicit fields.
Class tags are retained labels; a cropped level set need not be a valid SDF.

The pure Python `read_dense_array(path)` and
`write_dense_array(path, volume)` functions read/write **CVDBARR v1** without any
native runtime. Native IEEE value bits, subnormal values, signed zero and masks
are preserved. Nonfinite numeric values are rejected by this interchange API.
Grid names and omitted-metadata diagnostic keys preserve arbitrary native bytes
through Python's `surrogateescape`; point JSON strings instead require UTF-8.

CVDBARR v1 has a 152-byte fixed prefix (`<8s6I3i3I9d3d`), typed background,
48-byte tail (`<IIQIIQQQ`), grid name, length-prefixed diagnostic keys, native
interleaved values and activity bytes. The magic is `CVDBARR\0`; lengths, reserved
fields, type tags and total file size are validated before payload allocation.
The tail reports active voxels and stored nonbackground values outside the crop;
`omitted_metadata` names metadata that cannot be represented by a dense array.
Dense export/import does not claim to preserve arbitrary VDB metadata, sparse
topology or unrepresented cells. Use file authoring for a lossless native copy.

## Standard points and typed columns

Create a real native PointDataGrid from world-space position, velocity and radius
buffers. Default velocity is zero and default radius is one world unit.

```python
from container_openvdb import PointCloud

points = PointCloud(
    array('d', [0, 0, 0, 1, 0, 0]),
    velocities=array('d', [1, 0, 0, 0, 1, 0]),
    radii=array('d', [0.1, 0.2]),
)
vdb.write_points(points, 'points.vdb', voxel_size=0.1)
readback = vdb.read_points('points.vdb')
```

Positions use native voxel-localized Float32 storage, so readback has
voxel-relative quantization. The native writer rejects world coordinates that
cannot meet its `voxel_size * 1e-4` precision policy. It deterministically sorts
complete input records by position, velocity and radius, then partitions points
into voxels. Do not assume original input order after writing.

All native column/group APIs use traversal
`leaf_origin_xyz_then_local_index`, including points in inactive voxels. First
read the native layout or columns, then supply values in that order. A single
edit value broadcasts; otherwise its count must match the native point count.

```python
from container_openvdb import AttributeEdit, GroupEdit, PointColumn, PointEdits

schema = vdb.point_schema('points.vdb')
ids = PointColumn.from_values('int64', [9007199254740993, 9223372036854775807])
edits = PointEdits(
    attributes=(AttributeEdit('add', 'id', ids),),
    groups=(GroupEdit('add', 'selected', [True, False]),),
)
vdb.edit_points('points.vdb', 'with_ids.vdb', edits)
column = vdb.read_column('with_ids.vdb', 'id')
assert column.at(1) == 9223372036854775807
assert vdb.read_group('with_ids.vdb', 'selected').booleans() == (True, False)
```

Column types are `bool`, `int32`, `int64`, `float`, `double`, `vec3f`, `vec3d`
and `string`. Numeric values use typed native arrays; vectors are flat
interleaved buffers. `PointColumn.from_values` accepts sized scalar sequences
or sequences of three-component vectors. Int64 values never pass through
Double. Integer-to-Float conversion rounds directly to binary32, ties to even,
avoiding Double rounding. Float/Double overflow or nonzero underflow is an error.
Strings require valid UTF-8 and may contain embedded NUL values. Group exports
use byte buffers and serialize as JSON booleans. Edits support `add`, `set`,
`rename` (`new_name=...`) and `drop`. Every mutation of `P` is rejected because
moving positions requires voxel repartitioning.

Unselected registered native attributes/codecs, descriptor metadata, hidden flags
and group bits remain intact in native edits/selections. Unsupported columns
remain visible in the schema with `readable=False` and fail typed readback
explicitly. The standard `read_points` route accepts only the standard P/v/pscale
layout; it rejects extra attributes/groups rather than silently losing them.
Use `read_column`, `read_group` and `point_schema` for richer native layouts.

## Selection, counts and statistics

```python
from container_openvdb import PointSelection

selected = PointSelection(any_of=('selected',), none_of=('hidden',))
# Every referenced group must exist. Empty any_of selects all before other rules.
counts = vdb.count_points('with_ids.vdb', PointSelection(any_of=('selected',)))
vdb.select_points('with_ids.vdb', 'kept.vdb', PointSelection(any_of=('selected',)))
vdb.select_points('with_ids.vdb', 'deleted.vdb', delete=True)  # Delete all points.
statistics = vdb.point_statistics('with_ids.vdb', 'id')
print(statistics.minimum, statistics.maximum, statistics.mean)
```

The predicate is `(any_of is empty OR any matches) AND all_of matches AND none
of none_of matches`. `delete=False` keeps matches; `delete=True` deletes matches.
Counts distinguish selected active and inactive points. Numeric statistics
support seven scalar/vector types, return exact native extrema and Double-rounded
means/population standard deviations, and include inactive selected points.
Empty extrema are `None`; `has_values` is false. String statistics fail explicitly.

A zero-point filtered result is a valid empty native tree with a versioned opaque
schema archive in metadata. `count_points(...).archived_schema` reports this
representation. Ordinary OpenVDB tools see zero native points. Generic schema
restoration/repopulation is separate; the bridge does not deserialize foreign
opaque archives or claim ordinary leaf-based column exports on an empty tree.

## Native file authoring

`vdb.author_file('spec.json', 'combined.vdb')` applies the native strict version-1
specification. It selects/copies/removes/renames/merges native grids and edits
typed file/grid metadata, retaining registered point attributes and native
value bits. Input paths are relative to the **original specification directory**.
The native adapter handles schema validation and exact integer tokens.

```json
{
  "version": 1,
  "inputs": [{
    "path": "with_ids.vdb",
    "edits": [{
      "selector": "points", "newName": "particles",
      "metadata": [{"name": "frame_id", "operation": "set",
                    "type": "int64", "value": 9223372036854775807}]
    }]
  }],
  "compression": "zip"
}
```

Metadata types are `bool`, `int32`, `int64`, `float`, `double`, `string`,
`vec3s` and `vec3d`. Metadata edits use `operation: set` with type/value or
`operation: remove`. Optional per-input `select`/`remove` arrays and explicit
`descriptor:` selectors support native catalogs. Default ZIP output is lossless;
`activeMaskCompression` and `saveFloatAsHalf` are explicit lossy opt-ins. The
package does not enable them. A runtime reporting `fileAuthoringSpec: 0` raises
`CapabilityError` before workflow I/O.

## Limits and failure behavior

`Limits` sets shared logical byte/voxel/point/attribute/group/edit/string/report
budgets. Default bytes are 512 MiB and points are capped at one million. Counts
are upper bounds; sparse topology and parser workspace may reach a byte budget
before a count cap. Python JSON decoding conservatively reserves 64 times its
encoded size plus fixed scratch before building containers. Binary dense arrays
are more compact. These are logical payload/workspace policies, not a hostile-file
or combined-process RSS sandbox; upstream metadata and nonpaged native arrays
can allocate before all decoded sizes are available.

Native operations subtract retained Python buffers, bounded stdout/stderr and
scratch from their own shared byte cap. Numeric buffers are mutable, so transfers
revalidate them. Native topology/count/precision preflights still apply.
`ToolClient(timeout=60)` bounds each process run. Each report pipe is bounded by
`max_report_bytes`; excessive output or timeout terminates the tool and raises an
error. Commands use argument arrays without a shell. File-producing commands
write into a private temporary directory and publish the final file exclusively
with a bounded copy; invalid input or a failed process leaves no user output.
Existing files are never overwritten. Sources remain unchanged.

Errors are `DataError`/`LimitError` for malformed data and logical limits,
`CapabilityError` for discovery/protocol/OFF failures, and `ToolError` for native
command failures. `ToolError` retains bounded `argv`, `returncode`, `stdout` and
`stderr` diagnostics. Filesystem errors such as an existing destination retain
their standard Python exception types.

## Tests

```powershell
python -m unittest discover -s tests/python -p test_openvdb_bridge.py -v
# Add the native runtime to enable the real VDB roundtrip cases:
$env:CONTAINER_OPENVDB_TOOL = 'F:/Projects/Container/out/build/visual-studio/Release/OpenVdbTool.exe'
python -m unittest discover -s tests/python -p test_openvdb_bridge.py -v
```

Pure tests include an independently encoded CVDBARR fixture, raw IEEE bits,
malformed headers and JSON, budgets, int64 endpoints, exact binary32 midpoint
rounding, strict point schemas and bounded runtime failures. Native tests cover
all four dense types, all eight point columns/groups, world positions, selection,
exact IDs/statistics, empty archives and relative-path native authoring.
