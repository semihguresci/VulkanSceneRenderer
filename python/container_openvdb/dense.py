"""Native IEEE arrays, activity masks and CVDBARR version1 binary I/O."""
from array import array
from dataclasses import dataclass, field
from enum import IntEnum
import math
import os
from pathlib import Path
import struct
import sys

from ._core import DEFAULT_LIMITS, FIXED, DataError, LimitError, finite, integer, new_output


class DenseType(IntEnum):
    FLOAT32 = 0
    FLOAT64 = 1
    VEC3_FLOAT32 = 2
    VEC3_FLOAT64 = 3

    @property
    def typecode(self):
        return "f" if self in (self.FLOAT32, self.VEC3_FLOAT32) else "d"

    @property
    def components(self):
        return 1 if self < 2 else 3

    @property
    def width(self):
        return 4 if self.typecode == "f" else 8


class GridClass(IntEnum):
    UNKNOWN = 0
    LEVEL_SET = 1
    FOG_VOLUME = 2
    STAGGERED = 3


class VectorType(IntEnum):
    INVARIANT = 0
    COVARIANT = 1
    COVARIANT_NORMALIZE = 2
    CONTRAVARIANT_RELATIVE = 3
    CONTRAVARIANT_ABSOLUTE = 4


def _raw(text):
    if type(text) is not str:
        raise DataError("dense names/diagnostics must be strings")
    # Binary names need not be UTF-8. Surrogateescape preserves arbitrary native
    # bytes exactly; point JSON strings deliberately use a stricter policy.
    try:
        return text.encode("utf-8", "surrogateescape")
    except UnicodeError as error:
        raise DataError("dense string contains unsupported surrogates") from error


def _shape(dimensions, origin, limits):
    if len(dimensions) != 3 or len(origin) != 3:
        raise DataError("dense dimensions/origin require three integers")
    count = 1
    for n, start in zip(dimensions, origin):
        integer(n, 1, 2**31 - 1, "dense dimension")
        integer(start, -2**31 + 8, 2**31 - 9, "dense origin")
        if start + n - 1 > 2**31 - 9:
            raise DataError("dense region exceeds safe integer coordinates")
        count *= n
        if count > limits.max_voxels:
            raise LimitError("dense cell count exceeds max_voxels")
    return count


def _affine(linear, translation, origin, dimensions):
    if len(linear) != 9 or len(translation) != 3:
        raise DataError("dense affine requires9 row-major coefficients and3 translations")
    for value in (*linear, *translation):
        finite(value, "dense affine coefficient")
    axes = [tuple(linear[3 * row + col] for row in range(3)) for col in range(3)]
    sizes = [math.hypot(*axis) for axis in axes]
    if any(not 1e-6 <= size <= 1e6 for size in sizes):
        raise DataError("dense affine axes must span [1e-6,1e6] world units")
    a, b, c = axes
    determinant = (a[0] * (b[1]*c[2]-b[2]*c[1]) - a[1] * (b[0]*c[2]-b[2]*c[0])
                   + a[2] * (b[0]*c[1]-b[1]*c[0]))
    if not math.isfinite(determinant) or abs(determinant) < 3e-15 or abs(determinant / math.prod(sizes)) < 1e-12:
        raise DataError("dense affine is singular/ill-conditioned or below native inverse-map scale")
    def spacing(value):
        return max(math.nextafter(value, math.inf)-value, value-math.nextafter(value, -math.inf))
    tolerance = min(sizes) * 1e-4
    for corner in range(8):
        index = [origin[k] + (dimensions[k]-1 if corner & (1 << k) else 0) for k in range(3)]
        for row in range(3):
            error, partial = spacing(translation[row]), 0.0
            for col in range(3):
                term = linear[3*row+col] * index[col]
                partial += term
                error += spacing(term) + spacing(partial)
            error += spacing(partial + translation[row])
            if not math.isfinite(error) or error > tolerance:
                raise DataError("dense affine loses world precision; recenter the region")


@dataclass
class DenseVolume:
    """Mutable native scalar/vector buffer. X varies fastest, then Y, then Z.

    ``values`` is a flattened array('f'/'d'); vector components are interleaved.
    ``active`` is one explicit0/1 byte per cell, independent of background.
    Memoryviews of these arrays interoperate with buffer consumers such as NumPy.
    Classification is only a preserved tag: a crop need not be a valid SDF.
    """
    dtype: DenseType
    dimensions: tuple
    values: array
    active: bytearray
    origin: tuple = (0, 0, 0)
    linear: tuple = (1., 0., 0., 0., 1., 0., 0., 0., 1.)
    translation: tuple = (0., 0., 0.)
    background: array = None
    grid_name: str = "field"
    grid_class: GridClass = GridClass.UNKNOWN
    vector_type: VectorType = VectorType.INVARIANT
    world_space: bool = True
    active_voxels_outside: int = 0
    stored_nonbackground_outside: bool = False
    omitted_metadata: tuple = field(default_factory=tuple)

    def __post_init__(self):
        if any(type(value) is bool for value in (self.dtype, self.grid_class, self.vector_type)):
            raise DataError("dense enum tags must not be booleans")
        try:
            self.dtype, self.grid_class, self.vector_type = DenseType(self.dtype), GridClass(self.grid_class), VectorType(self.vector_type)
        except (ValueError, TypeError) as error:
            raise DataError("invalid dense type/class/vector tag") from error
        self.dimensions, self.origin = tuple(self.dimensions), tuple(self.origin)
        self.linear, self.translation = tuple(self.linear), tuple(self.translation)
        self.omitted_metadata = tuple(self.omitted_metadata)
        if self.background is None:
            self.background = array(self.dtype.typecode, [0] * self.dtype.components)

    @property
    def cell_count(self):
        return math.prod(self.dimensions)

    @property
    def payload_bytes(self):
        return self.cell_count * (self.dtype.components * self.dtype.width + 1)

    @property
    def memory_bytes(self):
        return self.payload_bytes + 4*len(_raw(self.grid_name)) + sum(4*len(_raw(key))+128 for key in self.omitted_metadata) + 4096

    def validate(self, limits=DEFAULT_LIMITS):
        if any(type(value) is bool for value in (self.dtype, self.grid_class, self.vector_type)):
            raise DataError("dense enum tags must not be booleans")
        try:
            self.dtype, self.grid_class, self.vector_type = DenseType(self.dtype), GridClass(self.grid_class), VectorType(self.vector_type)
        except (ValueError, TypeError) as error:
            raise DataError("invalid dense enum tag") from error
        count = _shape(self.dimensions, self.origin, limits)
        if type(self.values) is not array or self.values.typecode != self.dtype.typecode or self.values.itemsize != self.dtype.width or len(self.values) != count * self.dtype.components:
            raise DataError("dense native value buffer type/length does not match dimensions")
        if type(self.background) is not array or self.background.typecode != self.dtype.typecode or len(self.background) != self.dtype.components:
            raise DataError("dense background type/length does not match values")
        if not isinstance(self.active, (bytes, bytearray)) or len(self.active) != count or any(value > 1 for value in self.active):
            raise DataError("dense activity requires one0/1 byte per cell")
        if type(self.world_space) is not bool or type(self.stored_nonbackground_outside) is not bool:
            raise DataError("dense flags must be booleans")
        integer(self.active_voxels_outside, 0, 2**64 - 1, "outside active count")
        if not 1 <= len(_raw(self.grid_name)) <= 256 or "\0" in self.grid_name:
            raise DataError("dense grid name requires1..256 bytes without NUL")
        if len(self.omitted_metadata) > 4096 or sum(len(_raw(key)) for key in self.omitted_metadata) > 1024 * 1024:
            raise LimitError("dense omitted-metadata diagnostic exceeds4096 keys/1MiB")
        limits.charge(3*self.memory_bytes+FIXED, "dense public buffers/scratch")
        for value in self.values:
            finite(value, "dense value")
        for value in self.background:
            finite(value, "dense background")
        _affine(self.linear, self.translation, self.origin, self.dimensions)
        return self

    def _offset(self, index):
        if len(index) != 3:
            raise DataError("dense index requires three integers")
        local = []
        for value, start, size in zip(index, self.origin, self.dimensions):
            if type(value) is not int or not start <= value < start + size:
                raise IndexError("dense index lies outside the represented region")
            local.append(value-start)
        return (local[2]*self.dimensions[1]+local[1])*self.dimensions[0]+local[0]

    def value_at(self, index):
        offset = self._offset(index) * self.dtype.components
        return self.values[offset] if self.dtype.components == 1 else tuple(self.values[offset:offset+3])

    def active_at(self, index):
        return bool(self.active[self._offset(index)])

    def index_to_world(self, index):
        if len(index) != 3:
            raise DataError("index requires three coordinates")
        xyz = [finite(v, "index coordinate") for v in index]
        return tuple(math.fsum([self.translation[row], *(self.linear[3*row+col]*xyz[col] for col in range(3))]) for row in range(3))


_PREFIX = struct.Struct("<8s6I3i3I9d3d")
_TAIL = struct.Struct("<IIQIIQQQ")
_MAGIC = b"CVDBARR\0"


def _read(stream, size):
    result = stream.read(size)
    if len(result) != size:
        raise DataError("truncated dense array")
    return result


def _native_array(typecode, raw):
    result = array(typecode)
    result.frombytes(raw)
    if sys.byteorder != "little":
        result.byteswap()
    return result


def read_dense_array(path, limits=DEFAULT_LIMITS):
    """Read a versioned native array file without requiring OpenVdbTool."""
    with open(path, "rb") as stream:
        size = os.fstat(stream.fileno()).st_size
        limits.charge(size + FIXED, "dense encoded input")
        fields = _PREFIX.unpack(_read(stream, _PREFIX.size))
        magic, version, dtype, grid_class, vector_type, world, reserved = fields[:7]
        if magic != _MAGIC or version != 1 or reserved or world not in (0, 1):
            raise DataError("unsupported dense array magic/version/flags")
        try:
            dtype, grid_class, vector_type = DenseType(dtype), GridClass(grid_class), VectorType(vector_type)
        except ValueError as error:
            raise DataError("invalid dense native type/class/vector tag") from error
        origin, dimensions, linear, translation = fields[7:10], fields[10:13], fields[13:22], fields[22:25]
        count = _shape(dimensions, origin, limits)
        payload = count * (dtype.width * dtype.components + 1)
        limits.charge(size + 3 * payload + FIXED, "dense decoded/native array workspace")
        background = _native_array(dtype.typecode, _read(stream, dtype.width * dtype.components))
        for value in background:
            finite(value, "dense background")
        _affine(linear, translation, origin, dimensions)
        name_length, omitted_count, outside, stored, reserved, stored_count, stored_payload, total = _TAIL.unpack(_read(stream, _TAIL.size))
        if reserved or stored not in (0, 1) or stored_count != count or stored_payload != payload or total != size:
            raise DataError("dense header lengths/counts/flags disagree")
        if not 1 <= name_length <= 256 or omitted_count > 4096:
            raise DataError("dense name/metadata count out of range")
        string_reserve = 4*name_length+256*omitted_count+4096
        limits.charge(size+3*payload+FIXED+string_reserve, "dense decoded names/metadata workspace")
        name = _read(stream, name_length).decode("utf-8", "surrogateescape")
        if "\0" in name:
            raise DataError("dense grid name contains NUL")
        omitted, string_bytes = [], 0
        for _ in range(omitted_count):
            length = struct.unpack("<I", _read(stream, 4))[0]
            if length > 1024 * 1024 - string_bytes or length > size - stream.tell():
                raise LimitError("dense diagnostic string length exceeds limits")
            string_reserve += 4*length
            limits.charge(size+3*payload+FIXED+string_reserve, "dense decoded names/metadata workspace")
            omitted.append(_read(stream, length).decode("utf-8", "surrogateescape"))
            string_bytes += length
        if stream.tell() + payload != size:
            raise DataError("dense payload/trailing byte length disagrees")
        values = array(dtype.typecode)
        try:
            values.fromfile(stream, count * dtype.components)
        except EOFError as error:
            raise DataError("dense payload changed size/truncated") from error
        if sys.byteorder != "little":
            values.byteswap()
        active = bytearray(_read(stream, count))
        if stream.read(1):
            raise DataError("dense array has trailing bytes or changed size")
    return DenseVolume(dtype, dimensions, values, active, origin, linear, translation, background,
                       name, grid_class, vector_type, bool(world), outside, bool(stored), tuple(omitted)).validate(limits)


def write_dense_array(path, volume, limits=DEFAULT_LIMITS):
    """Exclusively write CVDBARR v1, preserving native IEEE bits exactly."""
    volume.validate(limits)
    name, keys = _raw(volume.grid_name), [_raw(key) for key in volume.omitted_metadata]
    total = 200 + volume.dtype.width*volume.dtype.components + len(name) + sum(4+len(key) for key in keys) + volume.payload_bytes
    limits.charge(total + 3*volume.payload_bytes + FIXED, "dense encoded/output workspace")
    output = new_output(path)
    owned = False
    try:
        with open(output, "xb") as stream:
            owned = True
            stream.write(_PREFIX.pack(_MAGIC, 1, int(volume.dtype), int(volume.grid_class), int(volume.vector_type), int(volume.world_space), 0,
                                      *volume.origin, *volume.dimensions, *volume.linear, *volume.translation))
            background, values = volume.background, volume.values
            if sys.byteorder != "little":
                background, values = array(background.typecode, background), array(values.typecode, values)
                background.byteswap()
                values.byteswap()
            background.tofile(stream)
            stream.write(_TAIL.pack(len(name), len(keys), volume.active_voxels_outside, int(volume.stored_nonbackground_outside), 0,
                                    volume.cell_count, volume.payload_bytes, total))
            stream.write(name)
            for key in keys:
                stream.write(struct.pack("<I", len(key)))
                stream.write(key)
            values.tofile(stream)
            stream.write(volume.active)
            if stream.tell() != total:
                raise DataError("dense written length disagrees with version1 schema")
    except BaseException:
        if owned:
            output.unlink(missing_ok=True)
        raise
    return output
