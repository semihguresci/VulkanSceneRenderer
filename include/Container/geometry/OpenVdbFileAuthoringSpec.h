#pragma once

#include "Container/geometry/OpenVdbFileAuthoring.h"

namespace container::geometry::vdb {

struct FileAuthoringSpecOptions {
  uintmax_t maxSpecBytes{1024ull *
                         1024ull}; // [1,64MiB], immutable encoded JSON.
  size_t maxDepth{16};             // [1,64], before JSON token allocation.
  // Authoritative native limits. The specification has no limit fields.
  size_t maxGrids{256}, maxMetadataBytes{1024ull * 1024ull}, maxEdits{1024};
  uint64_t maxPoints{1'000'000};
  size_t maxPointAttributes{1024}, maxPointGroups{4096};
};

// Strict UTF-8 JSON object, version exactly integer1. Required inputs is an
// array of 1..16 objects {path,select?,remove?,edits?}. Each edit is
// {selector,newName?,metadata?}. Metadata entries are
// {name,operation:"set",type,value} or {name,operation:"remove"}.
// Types: bool, int32, int64, float, double, string, vec3s, vec3d. vec3s means
// three binary32 components; vec3d means three binary64 components. Integer
// metadata requires integer tokens and preserves exact int64 endpoints.
// Optional root fields: metadata, metadataConflict (reject/preferFirst/
// preferLast), compression (zip/blosc/none), activeMaskCompression and
// saveFloatAsHalf. Defaults are the native API's lossless settings.
//
// Unknown/duplicate fields, wrong types, malformed JSON, unsupported versions,
// inappropriate metadata fields, numeric overflow or nonzero underflow fail.
// No filesystem paths are expanded from environment variables. Relative input
// paths are resolved against the ORIGINAL spec directory, not the process
// working directory or a temporary snapshot directory.
//
// Reads one bounded immutable string snapshot, checks depth/raw token lengths
// before the SAX decoder and builds only the native schema. Parser snapshot,
// decoded model and conservative scratch reserves are subtracted from the
// shared maxInputBytes before native authoring. External limits cannot be
// raised by JSON. Inputs/existing outputs remain intact and invalid specs
// create no output. OFF reports availability before paths, options or parsing.
[[nodiscard]] FileAuthoringResult
WriteAuthoredVdbSpec(const std::filesystem::path &specification,
                     const std::filesystem::path &output,
                     const FileAuthoringSpecOptions &options = {},
                     const ProcessingOptions &processing = {});

} // namespace container::geometry::vdb
