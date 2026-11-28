"""Read and compare the renderer's uncompressed robotics camera EXR frames."""

import math
from pathlib import Path
import struct


_RGB = ("R", "G", "B")
_EXACT = ("Z", "SemanticID", "InstanceID")
_PIXEL_TYPES = {"B": 2, "G": 2, "InstanceID": 0, "R": 2, "SemanticID": 0, "Z": 2}
_MAX_HEADER_BYTES = 64 * 1024
_SAMPLE_BYTES = 4
_CHUNK_HEADER = struct.Struct("<iI")


def _name(data, offset, path, field):
    # Version 2 without the long-names flag permits up to 31-byte names.
    end = data.find(b"\0", offset, offset + 32)
    if end < 0:
        raise ValueError(f"{path}: unterminated or oversized EXR {field}")
    try:
        return data[offset:end].decode("ascii"), end + 1
    except UnicodeDecodeError as error:
        raise ValueError(f"{path}: non-ASCII EXR {field}") from error


def _header_attributes(prefix, path):
    if len(prefix) < 8 or struct.unpack_from("<II", prefix) != (20000630, 2):
        raise ValueError(f"{path}: expected OpenEXR magic and scanline version 2 without flags")

    position = 8
    attributes = {}
    while True:
        name, position = _name(prefix, position, path, "attribute name")
        if not name:
            return attributes, position
        if name in attributes:
            raise ValueError(f"{path}: duplicate EXR attribute {name}")
        kind, position = _name(prefix, position, path, f"{name} attribute type")
        if len(prefix) - position < 4:
            raise ValueError(f"{path}: missing length for EXR attribute {name}")
        size, = struct.unpack_from("<I", prefix, position)
        position += 4
        end = position + size
        if end > len(prefix):
            raise ValueError(f"{path}: truncated or oversized EXR attribute {name}")
        attributes[name] = kind, prefix[position:end]
        position = end


def _channel_positions(data, path, channel_bytes):
    position = 0
    channels = {}
    while True:
        name, position = _name(data, position, path, "channel name")
        if not name:
            if position != len(data):
                raise ValueError(f"{path}: unexpected bytes after EXR channel list")
            break
        if name not in _PIXEL_TYPES or name in channels:
            raise ValueError(f"{path}: unexpected or duplicated EXR channel {name}")
        if len(data) - position < 16:
            raise ValueError(f"{path}: truncated EXR channel {name}")
        pixel_type, linear, reserved, x_sampling, y_sampling = struct.unpack_from("<iB3sii", data, position)
        if pixel_type != _PIXEL_TYPES[name]:
            expected = "UINT" if _PIXEL_TYPES[name] == 0 else "FLOAT"
            raise ValueError(f"{path}: EXR channel {name} must use {expected} pixels")
        if linear not in (0, 1) or reserved != bytes(3) or (x_sampling, y_sampling) != (1, 1):
            raise ValueError(f"{path}: unsupported sampling or flags for EXR channel {name}")
        channels[name] = _CHUNK_HEADER.size + len(channels) * channel_bytes
        position += 16
    if missing := _PIXEL_TYPES.keys() - channels.keys():
        raise ValueError(f"{path}: missing EXR channels {', '.join(sorted(missing))}")
    return channels


class _CameraFrame:
    def __init__(self, stream, path, width, height):
        if (type(width) is not int or type(height) is not int or
                not 0 < width <= 8192 or not 0 < height <= 8192 or width * height > 8_388_608):
            raise ValueError(f"{path}: invalid robotics camera dimensions {width}×{height}")
        self.stream = stream
        self.path = path
        self.channel_bytes = width * _SAMPLE_BYTES
        self.row_bytes = self.channel_bytes * len(_PIXEL_TYPES)
        self.chunk_bytes = _CHUNK_HEADER.size + self.row_bytes
        actual_size = stream.seek(0, 2)
        stream.seek(0)
        attributes, table_start = _header_attributes(stream.read(_MAX_HEADER_BYTES), path)
        window = struct.pack("<iiii", 0, 0, width - 1, height - 1)
        for name in ("dataWindow", "displayWindow"):
            if attributes.get(name) != ("box2i", window):
                raise ValueError(f"{path}: EXR {name} does not match camera dimensions {width}×{height}")
        if attributes.get("compression") != ("compression", b"\0"):
            raise ValueError(f"{path}: expected uncompressed EXR scanlines")
        if attributes.get("lineOrder") != ("lineOrder", b"\0"):
            raise ValueError(f"{path}: expected EXR scanlines in increasing Y order")
        kind, channels = attributes.get("channels", (None, b""))
        if kind != "chlist":
            raise ValueError(f"{path}: missing or invalid EXR channel list")
        self.channels = _channel_positions(channels, path, self.channel_bytes)

        table_bytes = height * 8
        self.first_row = table_start + table_bytes
        expected_size = self.first_row + height * self.chunk_bytes
        if actual_size != expected_size:
            raise ValueError(f"{path}: expected EXR scanline payload to end at byte {expected_size}, "
                             f"file has {actual_size} bytes")
        stream.seek(table_start)
        table = stream.read(table_bytes)
        if len(table) != table_bytes:
            raise ValueError(f"{path}: truncated EXR scanline offset table")
        for row, (offset,) in enumerate(struct.iter_unpack("<Q", table)):
            if offset != self.first_row + row * self.chunk_bytes:
                raise ValueError(f"{path}: invalid EXR offset for row {row}: {offset}")

    def read_row(self, row):
        self.stream.seek(self.first_row + row * self.chunk_bytes)
        chunk = self.stream.read(self.chunk_bytes)
        if len(chunk) != self.chunk_bytes:
            raise ValueError(f"{self.path}: truncated EXR row {row}")
        stored_row, byte_count = _CHUNK_HEADER.unpack_from(chunk)
        if (stored_row, byte_count) != (row, self.row_bytes):
            raise ValueError(f"{self.path}: invalid EXR row {row}: header contains row {stored_row} "
                             f"with {byte_count} bytes, expected {self.row_bytes}")
        return chunk

    def channel(self, chunk, name):
        start = self.channels[name]
        return memoryview(chunk)[start:start + self.channel_bytes]


def compare_camera_frames(reference_path, candidate_path, width, height) -> float:
    """Require byte-identical axial Z and IDs, then compare finite linear RGB."""
    reference_path, candidate_path = Path(reference_path), Path(candidate_path)
    maximum_difference = 0.0
    with reference_path.open("rb") as reference_stream, candidate_path.open("rb") as candidate_stream:
        reference = _CameraFrame(reference_stream, reference_path, width, height)
        candidate = _CameraFrame(candidate_stream, candidate_path, width, height)
        for row in range(height):
            first, second = reference.read_row(row), candidate.read_row(row)
            for name in _EXACT:
                expected, actual = reference.channel(first, name), candidate.channel(second, name)
                if actual != expected:
                    column = next(i // _SAMPLE_BYTES for i, (a, b) in enumerate(zip(expected, actual)) if a != b)
                    raise ValueError(f"{candidate_path}: EXR {name} differs from {reference_path} "
                                     f"at row {row}, column {column}")
            for name in _RGB:
                expected = struct.iter_unpack("<f", reference.channel(first, name))
                actual = struct.iter_unpack("<f", candidate.channel(second, name))
                for column, ((a,), (b,)) in enumerate(zip(expected, actual)):
                    if not math.isfinite(a) or not math.isfinite(b):
                        path = reference_path if not math.isfinite(a) else candidate_path
                        raise ValueError(f"{path}: nonfinite EXR {name} at row {row}, column {column}")
                    if not math.isclose(a, b, rel_tol=1e-3, abs_tol=1e-5):
                        raise ValueError(f"{candidate_path}: EXR {name} differs from {reference_path} "
                                         f"at row {row}, column {column}: {b} vs {a}")
                    maximum_difference = max(maximum_difference, abs(a - b))
    return maximum_difference


def camera_pixel(path, width, height, x, y) -> dict:
    """Read the typed FLOAT and UINT values of a single zero-based camera pixel."""
    path = Path(path)
    with path.open("rb") as stream:
        frame = _CameraFrame(stream, path, width, height)
        if type(x) is not int or type(y) is not int or not 0 <= x < width or not 0 <= y < height:
            raise ValueError(f"{path}: camera pixel ({x}, {y}) is outside {width}×{height}")
        chunk = frame.read_row(y)
        result = {}
        for name, pixel_type in _PIXEL_TYPES.items():
            format_code = "<I" if pixel_type == 0 else "<f"
            value, = struct.unpack_from(format_code, chunk, frame.channels[name] + x * _SAMPLE_BYTES)
            if name in _RGB and not math.isfinite(value):
                raise ValueError(f"{path}: nonfinite EXR {name} at row {y}, column {x}")
            result[name] = value
    return result
