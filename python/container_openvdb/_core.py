"""Shared bounds and strict JSON primitives. No native/Python extension needed."""
from dataclasses import dataclass
import json
import math
import os
from pathlib import Path


class BridgeError(RuntimeError):
    """Bridge/runtime error with an actionable diagnostic."""


class DataError(ValueError):
    """Malformed or unsupported typed interchange data."""


class LimitError(DataError):
    """A caller-supplied logical budget was exceeded."""


class CapabilityError(BridgeError):
    """The compiled runtime or required protocol is unavailable."""


@dataclass(frozen=True)
class Limits:
    max_bytes: int = 512 * 1024 * 1024
    max_voxels: int = 16_000_000
    max_points: int = 1_000_000
    max_attributes: int = 64
    max_groups: int = 256
    max_edits: int = 64
    max_string_bytes: int = 1024 * 1024
    max_report_bytes: int = 1024 * 1024

    def __post_init__(self):
        for name, value in vars(self).items():
            if type(value) is not int or value <= 0:
                raise DataError(f"{name} must be a positive integer")
        for name, maximum in (("max_bytes", 2**64 - 1), ("max_voxels", 2**64 - 1),
                              ("max_points", 2**63 - 1), ("max_attributes", 1024),
                              ("max_groups", 4096), ("max_edits", 1024)):
            if getattr(self, name) > maximum:
                raise DataError(f"{name} exceeds {maximum}")

    def charge(self, size, what="data"):
        if size > self.max_bytes:
            raise LimitError(f"{what} exceeds the shared logical byte budget")


DEFAULT_LIMITS = Limits()
FIXED = 128 * 1024


def require_keys(value, required, optional=(), what="document"):
    if type(value) is not dict or not set(required) <= value.keys() or value.keys() - set(required) - set(optional):
        raise DataError(f"invalid/missing/unknown {what} keys")


def integer(value, minimum, maximum, what):
    if type(value) is not int or not minimum <= value <= maximum:
        raise DataError(f"{what} must be an integer in [{minimum},{maximum}]")
    return value


def finite(value, what="value"):
    if type(value) not in (int, float):
        raise DataError(f"{what} must be numeric")
    try:
        result = float(value)
    except OverflowError as error:
        raise DataError(f"{what} is outside Double range") from error
    if not math.isfinite(result):
        raise DataError(f"{what} must be finite")
    return result


def utf8(value, what="string", maximum=None, name=False):
    if type(value) is not str:
        raise DataError(f"{what} must be a UTF-8 string")
    try:
        raw = value.encode("utf-8", errors="strict")
    except UnicodeError as error:
        raise DataError(f"{what} contains invalid UTF-8/surrogates") from error
    if maximum is not None and len(raw) > maximum:
        raise LimitError(f"{what} exceeds its byte limit")
    if name and (not raw or b"\0" in raw):
        raise DataError(f"{what} must be nonempty without NUL")
    return raw


def new_output(path):
    result = Path(os.path.abspath(os.fspath(path)))
    if os.path.lexists(result):
        raise FileExistsError(f"output already exists: {result}")
    if not result.parent.is_dir():
        raise FileNotFoundError(f"output directory does not exist: {result.parent}")
    return result


def _pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise DataError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _int(token):
    if len(token) > 128:
        raise LimitError("JSON numeric token exceeds128 bytes")
    return int(token)


def _float(token):
    if len(token) > 128:
        raise LimitError("JSON numeric token exceeds128 bytes")
    value = float(token)
    nonzero = any(c in "123456789" for c in token.lower().split("e")[0])
    if not math.isfinite(value) or (value == 0 and nonzero):
        raise DataError("JSON numeric token overflows/underflows Double range")
    return value


def decode_json(raw, limits=DEFAULT_LIMITS):
    # Python JSON containers/numbers have larger storage than their encoding.
    # Reserve64x input plus fixed scratch before building any decoded tree.
    # Short malformed objects can use more storage than numeric columns.
    limits.charge(64 * len(raw) + FIXED, "JSON decoded/parser workspace")
    try:
        text = raw.decode("utf-8", errors="strict")
        return json.loads(text, object_pairs_hook=_pairs, parse_int=_int, parse_float=_float,
                          parse_constant=lambda token: (_ for _ in ()).throw(DataError(f"invalid JSON number: {token}")))
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise DataError(f"invalid JSON/UTF-8: {error}") from error


def read_json(path, limits=DEFAULT_LIMITS):
    with open(path, "rb") as stream:
        size = os.fstat(stream.fileno()).st_size
        limits.charge(64 * size + FIXED, "JSON decoded/parser workspace")
        raw = stream.read(size + 1)
        if len(raw) != size:
            raise DataError("JSON input changed size while being read")
    return decode_json(raw, limits)


class JsonOutput:
    """Exclusive streaming JSON sink; no whole-document string copy."""
    def __init__(self, path, limits=DEFAULT_LIMITS):
        self.path = new_output(path)
        self.limits = limits
        self.count = 0
        self.stream = None

    def __enter__(self):
        self.stream = open(self.path, "xb")
        return self

    def write(self, text):
        raw = text.encode("utf-8", errors="strict")
        if self.count + len(raw) > self.limits.max_bytes:
            raise LimitError("encoded JSON output exceeds byte budget")
        self.stream.write(raw)
        self.count += len(raw)

    def value(self, value):
        self.write(json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":")))

    def __exit__(self, kind, error, traceback):
        try:
            self.stream.close()
        except BaseException:
            self.path.unlink(missing_ok=True)
            raise
        if kind:
            self.path.unlink(missing_ok=True)
