#pragma once

#include "Container/geometry/OpenVdbPointAttributes.h"

namespace container::geometry::vdb {

// Strict version1 JSON edit document:
// {"version":1,"attributes":[{"op":"add|set","name":"id",
//   "type":"bool|int32|int64|float|double|vec3f|vec3d|string","values":[...]}],
//  "groups":[{"op":"add|set","name":"selected","values":[true,false]}]}
// Both arrays are optional. Rename uses op/name/new_name; Drop uses op/name.
// No other keys, duplicates, nulls or implicit conversions are accepted.
// Members can appear in any order. Vec3 values contain exactly three numbers;
// int32/int64 require integer JSON tokens and retain exact integer precision.
// Strings are UTF-8 JSON (including escaped NUL in string values). Native byte
// strings that are not valid UTF-8 fail export explicitly, without replacement.
// Numeric tokens are limited to128 bytes. Other limits come from processing
// and attributeOptions, including aggregate decoded string and point/edit caps.
// A private bounded edit snapshot and streaming parser share the native P03
// memory budget; they do not each receive an independent full allocation.
[[nodiscard]] PointAttributeSchema ApplyPointAttributeEditsFile(
    const std::filesystem::path& input, const std::filesystem::path& editsJson,
    const std::filesystem::path& output, const std::string& inputGrid = {},
    const ProcessingOptions& processing = {}, const PointAttributeOptions& attributeOptions = {});

// Exclusive, bounded streaming exports with version1 and
// traversal="leaf_origin_xyz_then_local_index". Column exports add attribute
// (native description), type and values; group exports add group and boolean
// values. Schema exports contain grid_name, point_count, leaf_count, attributes
// and groups. All follow P03 native ordering, including inactive-voxel points.
// Exported attribute documents are readback documents, not edit documents.
// Output failure removes the owned partial file. Source files remain unchanged.
// OFF reports ENABLE_OPENVDB=ON before any input/options/output access.
[[nodiscard]] PointAttributeSchema ExportPointAttributeSchema(
    const std::filesystem::path& input, const std::filesystem::path& outputJson,
    const std::string& inputGrid = {}, const ProcessingOptions& processing = {},
    const PointAttributeOptions& attributeOptions = {});
void ExportPointAttribute(
    const std::filesystem::path& input, const std::string& attribute,
    const std::filesystem::path& outputJson, const std::string& inputGrid = {},
    const ProcessingOptions& processing = {}, const PointAttributeOptions& attributeOptions = {});
void ExportPointGroup(
    const std::filesystem::path& input, const std::string& group,
    const std::filesystem::path& outputJson, const std::string& inputGrid = {},
    const ProcessingOptions& processing = {}, const PointAttributeOptions& attributeOptions = {});

} // namespace container::geometry::vdb
