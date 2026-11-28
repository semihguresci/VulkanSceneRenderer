"""Parse saved ``vulkaninfo --text --show-formats`` output without loading Vulkan.

The complete original report belongs beside the returned inventory. Values which
cannot be interpreted remain strings, and absent features remain ``None``. GPU
indices are vulkaninfo indices, not a promise about the renderer's enumeration.
Capture matching therefore verifies the device actually used by the application.
"""

from __future__ import annotations

from copy import deepcopy
import hashlib
import math
import re
import uuid


_DEVICE = re.compile(r"^GPU(\d+):\s*$", re.MULTILINE)
_STRUCT = re.compile(r"^(Vk[A-Za-z0-9_]+):\s*$", re.MULTILINE)
_ASSIGNMENT = re.compile(r"^\s*([A-Za-z][A-Za-z0-9_ ]*)\s*=\s*(.*?)\s*$")
_ARRAY = re.compile(r"^(\s*)([A-Za-z][A-Za-z0-9_ ]*):\s*(?:count\s*=\s*(\d+))?\s*$")
_FORMAT = re.compile(r"\b(?:VK_)?FORMAT_(?!FEATURE)[A-Z0-9_]+\b")
_FEATURE_NAMES = (
    "timelineSemaphore", "rayQuery", "accelerationStructure", "rayTracingPipeline",
    "bufferDeviceAddress", "synchronization2", "dynamicRendering",
)
_IDENTITY_NAMES = {
    "deviceName": "deviceName", "vendorID": "vendorId", "deviceID": "deviceId",
    "apiVersion": "apiVersion", "driverVersion": "driverVersion",
    "deviceUUID": "deviceUUID", "driverUUID": "driverUUID",
}


def _scalar(value):
    value = value.strip()
    if value in ("true", "false"):
        return value == "true"
    if re.fullmatch(r"[+-]?(?:0[xX][0-9a-fA-F]+|\d+)", value):
        return int(value, 16 if "x" in value.lower() else 10)
    if re.fullmatch(r"[+-]?(?:\d+\.\d*|\.\d+)(?:[eE][+-]?\d+)?|[+-]?\d+[eE][+-]?\d+", value):
        parsed = float(value)
        if math.isfinite(parsed):
            return parsed
    return value


def _indent(line):
    expanded = line.expandtabs(8)
    return len(expanded) - len(expanded.lstrip())


def _fields(text):
    """Retain scalars and indented arrays, including flags and numeric ranges."""
    lines = text.splitlines()
    result = {}
    for index, line in enumerate(lines):
        match = _ASSIGNMENT.match(line)
        if match:
            result[match[1].strip()] = _scalar(match[2])
            continue
        match = _ARRAY.match(line)
        if not match:
            continue
        values = []
        for child in lines[index + 1:]:
            if not child.strip():
                continue
            if _indent(child) <= _indent(line):
                break
            if _ASSIGNMENT.match(child) or _ARRAY.match(child):
                break
            if not re.fullmatch(r"[-=]+", child.strip()):
                values.append(_scalar(child))
        if values or match[3] == "0":
            result[match[2]] = values
    return result


def _uint(value, label):
    parsed = _scalar(str(value)) if not isinstance(value, bool) else None
    if not isinstance(parsed, int) or not 0 <= parsed <= 0xFFFFFFFF:
        raise ValueError(f"{label} must be a uint32")
    return parsed


def _packed_version(value, *, api=False):
    raw = str(value).strip()
    parenthesized = re.search(r"\((0[xX][0-9a-fA-F]+|\d+)\)\s*$", raw)
    if parenthesized:
        return _uint(parenthesized[1], "version")
    if re.fullmatch(r"0[xX][0-9a-fA-F]+|\d+", raw):
        return _uint(raw, "version")
    # API versions have Vulkan's defined packing. Driver dotted versions are
    # vendor-specific and must never be converted with the API version formula.
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", raw) if api else None
    if match:
        major, minor, patch = map(int, match.groups())
        if major <= 127 and minor <= 1023 and patch <= 4095:
            return (major << 22) | (minor << 12) | patch
    return None


def _uuid(value):
    try:
        parsed = uuid.UUID(str(value).strip().strip("{}"))
        return str(parsed) if parsed.int else None
    except (ValueError, AttributeError):
        return None


def _flag_names(value, prefix):
    text = " | ".join(str(item) for item in value) if isinstance(value, list) else str(value)
    return sorted(set(re.findall(r"\b(?:VK_)?(" + prefix + r"[A-Z0-9_]+)\b", text)))


def _format_rows(body):
    marker = re.search(r"^Format Properties:\s*$", body, re.MULTILINE)
    if not marker:
        return {}
    text = body[marker.end():]
    # Current Vulkan-Tools groups formats with identical properties. Older
    # reports can instead give one FORMAT_* heading per property block.
    groups = list(re.finditer(
        r"^\s*(?:Common Format Group(?:\[\d+\])?|(?:VK_)?FORMAT_(?!FEATURE)[A-Z0-9_]+|Unsupported Formats):[^\S\r\n]*(?:count\s*=\s*\d+)?[^\S\r\n]*$",
        text, re.MULTILINE))
    result = {}
    for index, match in enumerate(groups):
        block = text[match.start():groups[index + 1].start() if index + 1 < len(groups) else len(text)]
        names_part = block.split("Properties:", 1)[0]
        names = _FORMAT.findall(names_part)
        fields = _fields(block)
        unsupported = match[0].lstrip().startswith("Unsupported Formats:")
        values = {}
        for key in ("linearTilingFeatures", "optimalTilingFeatures", "bufferFeatures"):
            values[key] = [] if unsupported else (
                _flag_names(fields[key], r"FORMAT_FEATURE(?:_2)?_") if key in fields else None)
        values["raw"] = {key: fields[key] for key in values if key in fields}
        values["reportedUnsupported"] = unsupported
        for name in names:
            canonical = name if name.startswith("VK_") else "VK_" + name
            row = deepcopy(values)
            if canonical in result and result[canonical] != row:
                raise ValueError(f"Conflicting format properties for {canonical}")
            result[canonical] = row
    return result


def _device(index, body):
    warnings = []
    groups = {}
    raw_groups = {}
    headers = list(_STRUCT.finditer(body))
    for ordinal, header in enumerate(headers):
        section = body[header.end():headers[ordinal + 1].start() if ordinal + 1 < len(headers) else len(body)]
        fields = _fields(section)
        if header[1] in groups and groups[header[1]] != fields:
            raise ValueError(f"GPU{index}: conflicting {header[1]} blocks")
        groups[header[1]] = fields
        raw_groups[header[1]] = {match[1].strip(): match[2] for line in section.splitlines()
                                 if (match := _ASSIGNMENT.match(line))}
    properties = groups.get("VkPhysicalDeviceProperties")
    if properties is None:
        raise ValueError(f"GPU{index}: full VkPhysicalDeviceProperties report required")
    raw_identity = {}
    for group_name, fields in raw_groups.items():
        if "Properties" in group_name:
            for key in _IDENTITY_NAMES:
                if key in fields:
                    value = fields[key]
                    if key in raw_identity and raw_identity[key] != value:
                        raise ValueError(f"GPU{index}: conflicting {key}")
                    raw_identity[key] = value
    for key in ("deviceName", "vendorID", "deviceID", "apiVersion", "driverVersion"):
        if key not in raw_identity:
            raise ValueError(f"GPU{index}: missing {key}")
    identity = {
        "deviceName": str(raw_identity["deviceName"]),
        "vendorId": _uint(raw_identity["vendorID"], "vendorID"),
        "deviceId": _uint(raw_identity["deviceID"], "deviceID"),
        "apiVersion": _packed_version(raw_identity["apiVersion"], api=True),
        "driverVersion": _packed_version(raw_identity["driverVersion"]),
        "deviceUUID": _uuid(raw_identity.get("deviceUUID")),
        "driverUUID": _uuid(raw_identity.get("driverUUID")),
        "raw": raw_identity,
    }
    # Driver identity is useful even when a tool version does not print the
    # packed driver version. Preserve its original spelling without decoding a
    # vendor-specific dotted version or replacing it with an OS driver version.
    for fields in raw_groups.values():
        for key in ("deviceType", "pipelineCacheUUID", "driverID", "driverName", "driverInfo", "conformanceVersion"):
            if key in fields:
                raw_identity[key] = fields[key]
    if identity["apiVersion"] is None or not identity["deviceName"].strip():
        raise ValueError(f"GPU{index}: invalid device identity")
    if identity["driverVersion"] is None:
        warnings.append("Packed driver version unavailable; application driver attribution cannot be verified")
    feature_groups = {key: value for key, value in groups.items() if "Features" in key}
    features = {}
    for name in _FEATURE_NAMES:
        values = [group[name] for group in feature_groups.values() if name in group]
        valid = values and all(isinstance(value, bool) for value in values) and len(set(values)) == 1
        features[name] = values[0] if valid else None
        if values and not valid:
            warnings.append(f"Ambiguous or unrecognized {name} feature values")
    queues = []
    queue_headers = list(re.finditer(r"^\s*queueProperties\[(\d+)\]:\s*$", body, re.MULTILINE))
    for ordinal, header in enumerate(queue_headers):
        block = body[header.end():queue_headers[ordinal + 1].start() if ordinal + 1 < len(queue_headers) else len(body)]
        # A blank separator ends each queue's base and chained properties in
        # current Vulkan-Tools output, before the following feature blocks.
        fields = _fields(block.split("\n\n\n", 1)[0])
        count = fields.get("queueCount")
        if isinstance(count, bool) or not isinstance(count, int) or count <= 0:
            raise ValueError(f"GPU{index}: invalid queueCount")
        flags = _flag_names(fields.get("queueFlags", ""), "QUEUE_")
        queues.append({"index": int(header[1]), "queueCount": count, "queueFlags": flags,
                       "timestampValidBits": fields.get("timestampValidBits"),
                       "presentSupport": fields.get("present support"), "raw": fields})
    if len({queue["index"] for queue in queues}) != len(queues):
        raise ValueError(f"GPU{index}: duplicate queue family indices")
    formats = _format_rows(body)
    if not formats:
        warnings.append("Format properties absent or unrecognized; use vulkaninfo --text --show-formats")
    elif any(row[key] is None for row in formats.values()
             for key in ("linearTilingFeatures", "optimalTilingFeatures", "bufferFeatures")):
        warnings.append("Some reported format properties could not be interpreted")
    if "VkPhysicalDeviceLimits" not in groups:
        warnings.append("VkPhysicalDeviceLimits absent")
    if not queues:
        warnings.append("Queue family properties absent")
    extensions = sorted(set(re.findall(r"^\s*(VK_[A-Za-z0-9_]+)\s*:\s*extension revision\s+\d+", body, re.MULTILINE)))
    return {"index": index, "identity": identity, "limits": groups.get("VkPhysicalDeviceLimits", {}),
            "features": features, "featureGroups": feature_groups,
            "propertyGroups": {key: value for key, value in groups.items() if "Properties" in key},
            "queueFamilies": queues, "formats": formats, "extensions": extensions, "warnings": warnings}


def parse_vulkaninfo(text: str) -> dict:
    """Return a JSON-compatible inventory from a full text report.

    The SHA-256 covers the supplied text encoded as UTF-8, including its original
    newlines. The caller should also fingerprint the saved raw output bytes.
    """
    if not isinstance(text, str) or not text.strip() or len(text) > 64 * 1024 * 1024:
        raise ValueError("Expected a nonempty Vulkan text report of at most 64 MiB")
    raw_hash = hashlib.sha256(text.encode("utf-8")).hexdigest()
    clean = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", text).lstrip("\ufeff").replace("\r\n", "\n")
    headers = list(_DEVICE.finditer(clean))
    if not headers or len(headers) > 256:
        raise ValueError("Expected full vulkaninfo GPU sections (at most 256)")
    devices = [_device(int(header[1]), clean[header.end():headers[index + 1].start() if index + 1 < len(headers) else len(clean)])
               for index, header in enumerate(headers)]
    if len({device["index"] for device in devices}) != len(devices):
        raise ValueError("Duplicate vulkaninfo GPU indices")
    instance = re.search(r"^Vulkan Instance Version:\s*(.+)$", clean, re.MULTILINE)
    return {"schema": "container.vulkan-device-capabilities", "version": 1,
            "instanceVersion": instance[1].strip() if instance else None,
            "devices": devices, "warnings": [], "rawTextSha256": raw_hash}


def select_device(inventory: dict, intended_device: int | str) -> dict:
    """Select a unique GPU index, UUID, or exact device name without dispatch."""
    devices = inventory.get("devices", [])
    if isinstance(intended_device, bool) or not isinstance(intended_device, (int, str)):
        raise ValueError("Intended device must be a GPU index, UUID, or exact device name")
    selector = intended_device.strip() if isinstance(intended_device, str) else intended_device
    numeric = re.fullmatch(r"(?:GPU)?(\d+)", selector) if isinstance(selector, str) else None
    if isinstance(selector, int) or numeric:
        wanted = selector if isinstance(selector, int) else int(numeric[1])
        matches = [device for device in devices if device["index"] == wanted]
    else:
        wanted_uuid = _uuid(selector)
        matches = [device for device in devices if
                   (wanted_uuid is not None and device["identity"].get("deviceUUID") == wanted_uuid) or
                   device["identity"].get("deviceName") == selector]
    if len(matches) != 1:
        raise ValueError(f"Intended adapter {intended_device!r} resolved to {len(matches)} devices")
    return matches[0]


def match_capture_device(inventory: dict, telemetry: dict,
                         intended_device: int | str | None = None) -> dict:
    """Verify actual application identity; reject indistinguishable adapters.

    An explicit index alone cannot distinguish two identical physical adapters
    when capture telemetry omits a UUID. Such attribution is rejected even if
    vulkaninfo reported a distinct UUID for each adapter.
    """
    aliases = {"gpu": "deviceName", "deviceName": "deviceName", "vendorId": "vendorId",
               "vendorID": "vendorId", "deviceId": "deviceId", "deviceID": "deviceId",
               "vulkanApiVersion": "apiVersion", "apiVersion": "apiVersion",
               "driverVersion": "driverVersion", "deviceUUID": "deviceUUID", "deviceUuid": "deviceUUID"}
    raw = {key: deepcopy(telemetry[key]) for key in aliases if key in telemetry}
    normalized = {}
    for key, value in raw.items():
        name = aliases[key]
        if name in ("vendorId", "deviceId", "apiVersion", "driverVersion"):
            value = _uint(value, f"Capture {key}")
        elif name == "deviceUUID":
            value = _uuid(value)
            if value is None:
                raise ValueError(f"Capture {key} is not a nonzero UUID")
        elif not isinstance(value, str) or not value.strip():
            raise ValueError(f"Capture {key} must be a device name")
        if name in normalized and normalized[name] != value:
            raise ValueError(f"Conflicting capture identity aliases for {name}")
        normalized[name] = value
    required = ("deviceName", "vendorId", "deviceId", "apiVersion", "driverVersion")
    if any(key not in normalized for key in required):
        raise ValueError("Capture must identify gpu, vendorId, deviceId, vulkanApiVersion and driverVersion")
    matches = []
    for device in inventory.get("devices", []):
        identity = device["identity"]
        # Missing information cannot rule another physical adapter out. First
        # count all possible matches, then require complete evidence for the
        # sole candidate; otherwise an incomplete second row creates false
        # uniqueness for a fully reported adapter.
        if all(identity.get(key) is None or identity[key] == value for key, value in normalized.items()):
            matches.append(device)
    if len(matches) != 1:
        raise ValueError(f"Capture identity matched {len(matches)} inventory adapters; attribution is not unique")
    selected = matches[0]
    unavailable = [key for key in normalized if selected["identity"].get(key) is None]
    if unavailable:
        raise ValueError(f"Cannot verify capture identity fields: {', '.join(unavailable)}")
    if intended_device is not None and select_device(inventory, intended_device)["index"] != selected["index"]:
        raise ValueError(f"Application used GPU{selected['index']} instead of intended adapter {intended_device!r}")
    matched_by = [key for key in normalized if selected["identity"].get(key) is not None]
    return {"deviceIndex": selected["index"], "deviceIdentity": deepcopy(selected["identity"]),
            "captureIdentity": raw, "matchedBy": matched_by}
