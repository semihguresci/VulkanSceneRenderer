#pragma once

#include "Container/geometry/OpenVdbProcessing.h"

namespace container::geometry::vdb {

struct PlatonicOptions {
  uint32_t faceCount{6}; // Tetrahedron 4, cube 6, octahedron 8, dodecahedron 12, icosahedron 20.
  double scale{1.0};
  std::array<double,3> center{0.0,0.0,0.0};
};

// Generate upstream OpenVDB's canonical Platonic solids as FloatGrid SDFs.
// A cube's side length is scale; the other solids use upstream vertices inside
// [-scale,scale] (approximately unit circumradius at scale=1). Scale is in
// [1e-6,1e6] world units and must span at least five voxels. Half width is [3,64].
// The effective voxel range is [1.5e-5,1e6]: installed OpenVDB affine maps reject
// smaller isotropic determinants. Unsupported small voxels fail explicitly.
//
// Origin-centered voxel-unit generation avoids Float32 translation loss. Stored
// distances are rescaled into world units and an exact Double voxel/center map
// is installed. Canonical vertices/rasterization retain upstream Float32 error;
// center/world ULPs must fit 1e-4 voxel and canonical vertex ULPs 1e-3 voxel.
// Bounded coordinates, leaf/internal-node and meshToVolume workspace estimates
// precede upstream allocation in a one-worker arena. These conservative bounds
// are not a hard process-RSS cap. maxTriangles caps the triangulated input mesh
// (4/12/8/36/20 triangles). Metadata records faces, requested scale and center.
// Output must be new; failures leave no output. Disabled builds fail availability
// before accessing paths or validating options.
[[nodiscard]] ProcessingResult WritePlatonicLevelSet(
    const std::filesystem::path& output, const PlatonicOptions& primitive = {},
    const ProcessingOptions& processing = {});

} // namespace container::geometry::vdb
