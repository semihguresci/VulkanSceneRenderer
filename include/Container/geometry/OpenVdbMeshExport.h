#pragma once

#include "Container/geometry/OpenVdbLoader.h"

#include <vector>

namespace container::geometry::vdb {

struct MeshExportResult {
  size_t gridCount{0};
  size_t vertexCount{0};
  size_t triangleCount{0};
  size_t skippedEmptyGrids{0};
  std::vector<std::string> diagnostics;
};

// Export the selected scalar isosurfaces as indexed, world-space Wavefront OBJ
// positions/normals and triangle faces (v//vn). Grid selection, default/explicit
// isovalues, adaptivity, voxel/input budgets and total triangle cap are inherited
// from LoadFromFile. Positions retain that loader's Float32 extraction precision.
// Meshing runs in a single-worker arena; as in the loader, the triangle cap is
// checked after extraction and maxInputBytes is an input/topology budget, not a
// bound on every meshing/output allocation.
// Unsupported or incomplete selected grids fail. Empty surfaces may be skipped
// only alongside valid surfaces, with diagnostics returned and written as OBJ
// comments; an entirely empty selection fails. Output must be a new path; errors
// do not leave a partially written file. Disabled builds fail before file access.
[[nodiscard]] MeshExportResult WriteObjSurface(
    const std::filesystem::path& input, const std::filesystem::path& output,
    const ImportOptions& options = {});

} // namespace container::geometry::vdb
