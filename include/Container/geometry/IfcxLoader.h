#pragma once

#include "Container/geometry/DotBimLoader.h"
#include "Container/utility/ContentFingerprint.h"

#include <filesystem>
#include <memory>
#include <stop_token>
#include <string_view>

namespace container::geometry::ifcx {

// IFC5 development examples are encoded as IFCX JSON with USD mesh attributes.
// The loader converts supported mesh payloads into the shared BIM model shape.
using Model = container::geometry::dotbim::Model;

struct SourceSnapshotData;

struct SourceLayer {
  std::filesystem::path path;
  util::ContentFingerprint fingerprint;
};

struct SourceSnapshot {
  std::vector<SourceLayer> layers;
  std::shared_ptr<const SourceSnapshotData> data;
};

// Resolves local composition without tessellating. Fingerprints correspond to
// the actual bytes consumed for each layer in the snapshot.
[[nodiscard]] SourceSnapshot PrepareSource(const std::filesystem::path& path,
                                           std::stop_token stop = {});
[[nodiscard]] Model LoadFromSnapshot(const SourceSnapshot& source,
                                     float importScale = 1.0f,
                                     std::stop_token stop = {});

[[nodiscard]] Model LoadFromFile(const std::filesystem::path& path,
                                 float importScale = 1.0f,
                                 std::stop_token stop = {});
[[nodiscard]] Model LoadFromJson(std::string_view jsonText,
                                 float importScale = 1.0f,
                                 std::stop_token stop = {});

}  // namespace container::geometry::ifcx
