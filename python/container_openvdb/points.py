"""Typed native columns/groups and world-space standard point buffers."""
from array import array
from dataclasses import dataclass, field, InitVar
import math
import struct

from ._core import DEFAULT_LIMITS, FIXED, DataError, LimitError, Limits, JsonOutput, finite, integer, require_keys, utf8

TRAVERSAL = "leaf_origin_xyz_then_local_index"
POINT_TYPES = ("bool", "int32", "int64", "float", "double", "vec3f", "vec3d", "string")
_CODES = {"bool": "B", "int32": "i", "int64": "q", "float": "f", "double": "d", "vec3f": "f", "vec3d": "d"}


def _integer_float32(value):
    """Round a Python integer directly to binary32, once, ties to even."""
    negative = value < 0
    magnitude = abs(value)
    exponent = magnitude.bit_length()-1
    if exponent < 24:
        return float(value)
    if exponent > 127:
        raise DataError("point integer overflows binary32")
    shift = exponent-23
    significand, remainder = divmod(magnitude, 1 << shift)
    halfway = 1 << (shift-1)
    if remainder > halfway or (remainder == halfway and significand & 1):
        significand += 1
    if significand == 1 << 24:
        significand >>= 1
        exponent += 1
    if exponent > 127:
        raise DataError("point integer overflows binary32")
    bits = (int(negative) << 31) | ((exponent+127) << 23) | (significand-(1 << 23))
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def _version(value, required):
    require_keys(value, {"version", "traversal", *required})
    if type(value["version"]) is not int or value["version"] != 1 or value["traversal"] != TRAVERSAL:
        raise DataError("unsupported point JSON version/native traversal")


def _name(value):
    utf8(value, "point name", 256, name=True)
    return value


@dataclass(frozen=True)
class AttributeDescription:
    name: str
    value_type: str
    codec: str
    stride: int
    group_storage: bool
    any_hidden: bool
    readable: bool

    @classmethod
    def from_document(cls, value):
        require_keys(value, {"name", "value_type", "codec", "stride", "group_storage", "any_hidden", "readable"}, what="attribute description")
        for key in ("name", "value_type", "codec"):
            utf8(value[key], key)
        integer(value["stride"], 0, 2**32-1, "attribute stride")
        if any(type(value[key]) is not bool for key in ("group_storage", "any_hidden", "readable")):
            raise DataError("attribute flags must be booleans")
        return cls(**value)


@dataclass(frozen=True)
class PointSchema:
    grid_name: str
    point_count: int
    leaf_count: int
    attributes: tuple
    groups: tuple

    @classmethod
    def from_document(cls, document, limits=DEFAULT_LIMITS):
        _version(document, {"grid_name", "point_count", "leaf_count", "attributes", "groups"})
        utf8(document["grid_name"], "point grid name")
        integer(document["point_count"], 0, limits.max_points, "point count")
        integer(document["leaf_count"], 0, (limits.max_voxels+511)//512, "point leaf count")
        attrs, groups = document["attributes"], document["groups"]
        if type(attrs) is not list or len(attrs) > limits.max_attributes or type(groups) is not list or len(groups) > limits.max_groups:
            raise LimitError("point schema attribute/group limits exceeded")
        attributes = tuple(AttributeDescription.from_document(value) for value in attrs)
        if len({value.name for value in attributes}) != len(attributes):
            raise DataError("duplicate native attribute names")
        for group in groups:
            utf8(group, "group name")
        if len(set(groups)) != len(groups):
            raise DataError("duplicate native group names")
        return cls(document["grid_name"], document["point_count"], document["leaf_count"], attributes, tuple(groups))


@dataclass
class PointColumn:
    """Exact native typed values, not JavaScript-style numeric JSON values.

    Numeric values use flat array buffers; vector components are interleaved.
    String values are a tuple of strict UTF-8 strings, including embedded NULs.
    Bool values use0/1 bytes and are serialized as JSON booleans.
    """
    dtype: str
    values: object
    attribute: AttributeDescription = None

    @property
    def components(self):
        return 3 if self.dtype in ("vec3f", "vec3d") else 1

    @property
    def count(self):
        return len(self.values) // self.components

    @property
    def memory_bytes(self):
        if self.dtype == "string":
            return sum(4*len(utf8(value))+128 for value in self.values) + 256
        return len(self.values)*self.values.itemsize + 256

    @classmethod
    def from_values(cls, dtype, values, limits=DEFAULT_LIMITS, attribute=None):
        if dtype not in POINT_TYPES:
            raise DataError(f"unsupported native point type: {dtype}")
        vector = dtype in ("vec3f", "vec3d")
        # Inputs are concrete sized sequences; accepting unbounded generators
        # would allocate before a count/memory preflight.
        if isinstance(values, (str, bytes, bytearray)) or not hasattr(values, "__len__"):
            raise DataError("point values require a sized sequence")
        if len(values) > limits.max_points:
            raise LimitError("point column exceeds max_points")
        limits.charge(96*len(values)+FIXED, "typed point column conversion")
        if dtype == "string":
            total = 0
            for value in values:
                total += len(utf8(value, "point string"))
                if total > limits.max_string_bytes:
                    raise LimitError("decoded point strings exceed max_string_bytes")
                limits.charge(3*(4*total+128*len(values)+256)+FIXED, "decoded point string column")
            return cls(dtype, tuple(values), attribute)
        native = array(_CODES[dtype])
        expected = {"B":1,"i":4,"q":8,"f":4,"d":8}[native.typecode]
        if native.itemsize != expected:
            raise DataError("this Python platform does not provide the required native numeric widths")
        for value in values:
            if vector:
                if not isinstance(value, (list, tuple, array)) or len(value) != 3:
                    raise DataError("point vectors require exactly three components")
                items = value
            else:
                items = (value,)
            for item in items:
                if dtype == "bool":
                    if type(item) is not bool:
                        raise DataError("point bool values must be booleans")
                    native.append(int(item))
                elif dtype in ("int32", "int64"):
                    width = 32 if dtype == "int32" else 64
                    native.append(integer(item, -2**(width-1), 2**(width-1)-1, dtype))
                else:
                    number = _integer_float32(item) if dtype in ("float", "vec3f") and type(item) is int else finite(item, "point numeric value")
                    try:
                        native.append(number)
                    except OverflowError as error:
                        raise DataError("point value overflows its native type") from error
                    if not math.isfinite(native[-1]) or (number != 0 and native[-1] == 0):
                        raise DataError("point value overflows/underflows its native type")
        return cls(dtype, native, attribute)

    @classmethod
    def from_document(cls, document, limits=DEFAULT_LIMITS):
        _version(document, {"attribute", "type", "values"})
        if type(document["type"]) is not str or document["type"] not in POINT_TYPES:
            raise DataError("unsupported point column JSON type")
        description = AttributeDescription.from_document(document["attribute"])
        if not description.readable or description.group_storage or description.stride != 1 or type(document["values"]) is not list:
            raise DataError("point column is not a supported readable stride-one attribute")
        expected_native = {"bool": "bool", "int32": "int32", "int64": "int64", "float": "float", "double": "double",
                           "vec3f": "vec3s", "vec3d": "vec3d", "string": "uint32"}
        expected_codec = "str" if document["type"] == "string" else "null"
        if expected_native.get(document["type"]) != description.value_type or description.codec != expected_codec:
            raise DataError("point column JSON type disagrees with its native attribute description")
        return cls.from_values(document["type"], document["values"], limits, description)

    def at(self, index):
        integer(index, 0, self.count-1, "point index")
        if self.components == 3:
            return tuple(self.values[3*index:3*index+3])
        value = self.values[index]
        return bool(value) if self.dtype == "bool" else value

    def validate(self, limits=DEFAULT_LIMITS):
        # Buffers are intentionally mutable, so validate before every transfer.
        if type(self.dtype) is not str or self.dtype not in POINT_TYPES or not isinstance(self.values, (list, tuple, array)):
            raise DataError("invalid point type/native buffer")
        if len(self.values) % self.components or self.count > limits.max_points:
            raise DataError("invalid point type/component/count")
        if self.dtype == "string":
            if type(self.values) not in (tuple, list) or sum(len(utf8(v)) for v in self.values) > limits.max_string_bytes:
                raise LimitError("point string column exceeds limits")
        else:
            if type(self.values) is not array or self.values.typecode != _CODES[self.dtype] or self.values.itemsize != {"B":1,"i":4,"q":8,"f":4,"d":8}[_CODES[self.dtype]]:
                raise DataError("point numeric buffer type mismatch")
            for value in self.values:
                if self.dtype == "bool" and value not in (0, 1):
                    raise DataError("boolean point buffer requires0/1")
                finite(value, "point numeric value")
        limits.charge(3*self.memory_bytes+FIXED, "point column buffer/workspace")
        return self


@dataclass
class PointGroup:
    name: str
    membership: bytearray

    @classmethod
    def from_document(cls, document, limits=DEFAULT_LIMITS):
        _version(document, {"group", "values"})
        _name(document["group"])
        values = document["values"]
        if type(values) is not list or len(values) > limits.max_points or any(type(value) is not bool for value in values):
            raise DataError("group values must be bounded JSON booleans")
        return cls(document["group"], bytearray(values))

    def at(self, index):
        integer(index, 0, len(self.membership)-1, "group point index")
        return bool(self.membership[index])

    def booleans(self):
        self.validate()
        return tuple(bool(value) for value in self.membership)

    def validate(self, limits=DEFAULT_LIMITS):
        _name(self.name)
        if type(self.membership) is not bytearray or len(self.membership) > limits.max_points or any(value not in (0, 1) for value in self.membership):
            raise DataError("point group requires a bounded0/1 bytearray")
        limits.charge(3*len(self.membership)+FIXED, "point group buffers")
        return self


@dataclass(frozen=True)
class AttributeEdit:
    op: str
    name: str
    column: PointColumn = None
    new_name: str = None

    def validate(self, limits=DEFAULT_LIMITS):
        _name(self.name)
        if self.name == "P" or self.new_name == "P":
            raise DataError("P is protected; position edits require point repartitioning")
        if self.op in ("add", "set"):
            if self.new_name is not None or not isinstance(self.column, PointColumn):
                raise DataError("Add/Set requires a typed column, without new_name")
            self.column.validate(limits)
        elif self.op == "rename":
            if self.column is not None:
                raise DataError("Rename must not contain values")
            _name(self.new_name)
        elif self.op == "drop":
            if self.column is not None or self.new_name is not None:
                raise DataError("Drop must not contain values/new_name")
        else:
            raise DataError("unknown attribute edit operation")


@dataclass(frozen=True)
class GroupEdit:
    op: str
    name: str
    membership: object = None
    new_name: str = None

    def validate(self, limits=DEFAULT_LIMITS):
        _name(self.name)
        if self.op in ("add", "set"):
            if self.new_name is not None or not isinstance(self.membership, (list, tuple)) or len(self.membership) > limits.max_points or any(type(v) is not bool for v in self.membership):
                raise DataError("group Add/Set requires bounded boolean membership, without new_name")
        elif self.op == "rename":
            if self.membership is not None:
                raise DataError("group Rename must not contain values")
            _name(self.new_name)
        elif self.op == "drop":
            if self.membership is not None or self.new_name is not None:
                raise DataError("group Drop must not contain values/new_name")
        else:
            raise DataError("unknown group operation")


@dataclass(frozen=True)
class PointEdits:
    attributes: tuple = field(default_factory=tuple)
    groups: tuple = field(default_factory=tuple)

    @property
    def memory_bytes(self):
        return sum(edit.column.memory_bytes if edit.column is not None else 512 for edit in self.attributes) + sum(8*len(edit.membership) if edit.membership is not None else 512 for edit in self.groups) + FIXED

    def validate(self, limits=DEFAULT_LIMITS):
        if not isinstance(self.attributes, (list, tuple)) or not isinstance(self.groups, (list, tuple)) or any(not isinstance(edit, AttributeEdit) for edit in self.attributes) or any(not isinstance(edit, GroupEdit) for edit in self.groups):
            raise DataError("point edit batch requires typed sized edit sequences")
        if len(self.attributes)+len(self.groups) > limits.max_edits:
            raise LimitError("point edit batch exceeds max_edits")
        for edit in (*self.attributes, *self.groups):
            edit.validate(limits)
        strings = sum(sum(len(utf8(v)) for v in edit.column.values) for edit in self.attributes if edit.column is not None and edit.column.dtype == "string")
        if strings > limits.max_string_bytes:
            raise LimitError("point edit strings exceed aggregate max_string_bytes")
        limits.charge(3*self.memory_bytes, "point edit buffers/workspace")
        return self

    def write(self, path, limits=DEFAULT_LIMITS):
        self.validate(limits)
        with JsonOutput(path, limits) as output:
            output.write('{"version":1,"attributes":[')
            for index, edit in enumerate(self.attributes):
                if index:
                    output.write(",")
                output.write('{"op":');output.value(edit.op);output.write(',"name":');output.value(edit.name)
                if edit.new_name is not None:
                    output.write(',"new_name":');output.value(edit.new_name)
                if edit.column is not None:
                    output.write(',"type":');output.value(edit.column.dtype);output.write(',"values":[')
                    for n in range(edit.column.count):
                        if n:
                            output.write(",")
                        output.value(edit.column.at(n))
                    output.write("]")
                output.write("}")
            output.write('],"groups":[')
            for index, edit in enumerate(self.groups):
                if index:
                    output.write(",")
                output.write('{"op":');output.value(edit.op);output.write(',"name":');output.value(edit.name)
                if edit.new_name is not None:
                    output.write(',"new_name":');output.value(edit.new_name)
                if edit.membership is not None:
                    output.write(',"values":[')
                    for n, value in enumerate(edit.membership):
                        if n:
                            output.write(",")
                        output.value(value)
                    output.write("]")
                output.write("}")
            output.write("]}\n")


@dataclass
class PointCloud:
    """Standard world-space P/v/pscale records in contiguous Double buffers.

    Readback uses localized Float32 P internally; expect voxel-relative native
    quantization. Generic attributes/groups require the separate column APIs.
    """
    positions: array
    velocities: array = None
    radii: array = None
    limits: InitVar[Limits] = DEFAULT_LIMITS

    def __post_init__(self, limits):
        if type(self.positions) is not array or self.positions.typecode != "d" or len(self.positions) % 3:
            raise DataError("point positions require a complete array('d') Vec3 buffer")
        count = len(self.positions)//3
        if count > limits.max_points:
            raise LimitError("standard point defaults exceed max_points")
        limits.charge(3*56*count+FIXED, "standard point default buffers")
        if self.velocities is None:
            self.velocities = array("d", [0.0])*len(self.positions)
        if self.radii is None:
            self.radii = array("d", [1.0])*count

    @property
    def count(self):
        return len(self.positions)//3

    @property
    def memory_bytes(self):
        return 8*(len(self.positions)+len(self.velocities)+len(self.radii))

    def validate(self, limits=DEFAULT_LIMITS):
        if any(type(values) is not array or values.typecode != "d" or values.itemsize != 8 for values in (self.positions, self.velocities, self.radii)):
            raise DataError("standard point buffers require array('d')")
        if len(self.positions) % 3 or len(self.velocities) != len(self.positions) or len(self.radii) != self.count or self.count > limits.max_points:
            raise DataError("standard point buffer lengths/count disagree")
        limits.charge(3*self.memory_bytes+FIXED, "standard point buffer workspace")
        for values in (self.positions, self.velocities, self.radii):
            for value in values:
                finite(value, "point record value")
        if any(radius <= 0 for radius in self.radii):
            raise DataError("standard point radii must be positive")
        return self

    def write(self, path, limits=DEFAULT_LIMITS):
        self.validate(limits)
        with JsonOutput(path, limits) as output:
            output.write("[")
            for n in range(self.count):
                if n:
                    output.write(",")
                output.value({"position": list(self.positions[3*n:3*n+3]), "velocity": list(self.velocities[3*n:3*n+3]), "radius": self.radii[n]})
            output.write("]\n")

    @classmethod
    def from_document(cls, document, limits=DEFAULT_LIMITS):
        if type(document) is not list or len(document) > limits.max_points:
            raise DataError("standard point JSON must be a bounded record array")
        limits.charge(3*56*len(document)+FIXED, "standard point conversion")
        positions, velocities, radii = array("d"), array("d"), array("d")
        for record in document:
            require_keys(record, {"position"}, {"velocity", "radius"}, "point record")
            for name, target in (("position", positions), ("velocity", velocities)):
                values = record.get(name, [0., 0., 0.])
                if type(values) is not list or len(values) != 3:
                    raise DataError("point position/velocity requires exactly three numbers")
                target.extend(finite(value) for value in values)
            radii.append(finite(record.get("radius", 1.)))
        return cls(positions, velocities, radii, limits).validate(limits)
