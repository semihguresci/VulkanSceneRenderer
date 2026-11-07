#pragma once

#include "Container/geometry/OpenVdbRayQueries.h"

namespace container::geometry::vdb {

enum class SurfaceProjection { Perspective, Orthographic };
enum class SurfaceShading { Matte, WorldNormal, TwoSidedHeadlight };
struct SurfaceReferenceCamera {
  SurfaceProjection projection{SurfaceProjection::Perspective};
  std::array<double, 3> eye{0, 0, 3}, target{0, 0, 0}, up{0, 1, 0};
  double verticalFovDegrees{45}; // Perspective only, [0.1,150].
  double orthographicHeight{
      2}; // Orthographic only, finite positive WORLD height.
  // Euclidean WORLD lengths, NOT the native camera's view-plane depths.
  double minimumDistance{0}, maximumDistance{1000};
};
struct SurfaceReferenceOptions {
  uint32_t width{32}, height{32}; // Each [1,2048]; row0 top, X fastest.
  uint64_t maxPixels{
      262144}; // [1,262144], also bounded by rays.limits.maxRays.
  SurfaceShading shading{SurfaceShading::TwoSidedHeadlight};
  std::array<double, 3> color{1, 1, 1}, background{0, 0, 0}; // Each [0,1].
  std::array<double, 3> invalidNormalColor{1, 0, 1};
  // An image submits a full pixel batch; ordinary ray-query defaults stay
  // smaller. Explicit caller work caps remain authoritative.
  LevelSetRayOptions rays = [] {
    LevelSetRayOptions options;
    options.limits.maxWorkItems = 2'000'000'000;
    return options;
  }();
};
struct SurfaceReferenceImage {
  uint32_t width{}, height{};
  SurfaceReferenceCamera camera;
  SurfaceReferenceOptions options;
  LevelSetRayBatch geometry; // Full Double hit/depth/residual/normal status.
  // Actual native Float shader RGB promoted to Double, independent hit coverage
  // A.
  std::vector<std::array<double, 4>> rgba;
  uint64_t hitCount{}, unavailableNormalCount{}, preparationWork{}, work{};
  uint64_t roundedColorValues{};
  double maximumColorAbsoluteError{};
  uint64_t maxImageBytes{};
};
struct SurfaceReferenceExportResult {
  uint64_t roundedValues{}, encodedBytes{};
  double maximumAbsoluteError{}, maximumRelativeError{};
};

// Source-preserving native CPU SDF surface reference, one deterministic center
// ray per pixel. Real SDK perspective/orthographic cameras and constant
// Matte/Normal/ two-sided headlight shaders surround ONE checked
// QueryLevelSetRays batch. All source/map/band/type/coordinate, approximate
// HDDA/chord and validity limitations of that API apply. Float and Double
// inputs retain Double geometry. The shader response is native Float (audited,
// not physical radiance): headlight is color*abs(dot(normal,rayDirection)),
// normal is .5*color*(normal+1).
//
// SDK camera pose is conditioned at origin before WORLD eye translation. Each
// camera ray is checked against an independent intended basis. Public ray times
// override native depth-plane clipping and are Euclidean WORLD distances. Eye,
// target, up and the active camera scale must be finite; coincident/parallel
// poses reject. No AA, shadow rays, material/color grids, fog or scattering/SE
// is implied. Miss A=0, hit A=1 independently of Film alpha.
// Missing/degenerate/not-requested normals retain explicit status;
// normal-dependent modes use invalidNormalColor, not a fabricated normal or a
// miss. Colors/background [0,1] must retain nonzero Float values; meaningful
// shader underflow/overflow rejects, other rounding is counted. Approximate
// misses/tangencies/plateaus are not geometry certificates.
//
// Actual retained Film/ray/image capacities and bounded camera/shader work are
// deducted from local shared byte/work caps BEFORE the native batch. Only
// shared Processing maxInputBytes/maxVoxelCount apply; generation fields are
// ignored. One worker, no source modification. Large images can exceed native
// traversal work caps; there is no automatic quality reduction. OFF fails
// availability before paths/options/allocation. Logical payload limits are not
// an RSS sandbox.
[[nodiscard]] SurfaceReferenceImage
RenderSurfaceReference(const std::filesystem::path &input,
                       const SurfaceReferenceCamera &camera = {},
                       const SurfaceReferenceOptions &options = {},
                       const std::string &grid = {},
                       const ProcessingOptions &processing = {});

// Exclusive Unicode-safe uncompressed linear FLOAT EXR: R,G,B,A,Z,N.X,N.Y,N.Z,
// N.VALID,N.STATUS,HIT.KIND (11 channels). Z is radial WORLD depth. Miss depth
// and unavailable normals have zero placeholders; masks/status determine
// validity. N.STATUS codes match RayNormalStatus0..4; HIT.KIND matches
// SdfRayHitKind0..2. No Monte-Carlo uncertainty channels. Double sampled
// residual remains in memory. All retained result capacities,
// planar/encoded/TinyEXR workspace and metadata share
// min(maxBytes,image.maxImageBytes); finite/range/status checks and complete
// serialization precede publication. Float overflow/nonzero-to-zero conversion
// rejects; other rounding is reported. Existing output/source bytes are
// preserved, failure removes only a newly created owned file. OFF reports
// availability FIRST, even for a caller-constructed image (unlike
// OpenVDB-independent transport EXR).
[[nodiscard]] SurfaceReferenceExportResult
WriteSurfaceReferenceExr(const std::filesystem::path &output,
                         const SurfaceReferenceImage &image,
                         uint64_t maxBytes = 512ull * 1024 * 1024);

} // namespace container::geometry::vdb
