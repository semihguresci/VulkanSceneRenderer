"""Container's typed, stdlib-only OpenVDB data bridge.

This package is a supported compiled-tool bridge, not the upstream pyopenvdb
extension. Binary dense arrays work without an installed native runtime.
"""
from ._core import BridgeError, CapabilityError, DataError, LimitError, Limits
from .dense import DenseType, DenseVolume, GridClass, VectorType, read_dense_array, write_dense_array
from .points import AttributeDescription, AttributeEdit, GroupEdit, PointCloud, PointColumn, PointEdits, PointGroup, PointSchema, TRAVERSAL
from .tool import Capabilities, PointCounts, PointSelection, PointStatistics, ToolClient, ToolError

__version__ = "0.1.0"
__all__ = ["AttributeDescription", "AttributeEdit", "BridgeError", "Capabilities", "CapabilityError",
           "DataError", "DenseType", "DenseVolume", "GridClass", "GroupEdit", "LimitError", "Limits",
           "PointCloud", "PointColumn", "PointCounts", "PointEdits", "PointGroup", "PointSchema",
           "PointSelection", "PointStatistics", "TRAVERSAL", "ToolClient", "ToolError", "VectorType",
           "read_dense_array", "write_dense_array"]
