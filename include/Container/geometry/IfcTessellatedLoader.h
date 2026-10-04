#pragma once

#include "Container/geometry/DotBimLoader.h"

#include <filesystem>
#include <string_view>

namespace container::geometry::ifc {

// Reuses the BIM mesh/interior instance representation while parsing IFC STEP
// files with triangulated/polygonal face sets, planar faceted BReps, and
// polyline-profile extrusions. Model::importReport records omitted shapes.
using Model = container::geometry::dotbim::Model;

[[nodiscard]] Model LoadFromFile(const std::filesystem::path& path,
                                 float importScale = 1.0f);
[[nodiscard]] Model LoadFromStep(std::string_view stepText,
                                 float importScale = 1.0f);

}  // namespace container::geometry::ifc
