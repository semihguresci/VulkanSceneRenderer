#pragma once

// Shared binary-shadow metadata. Matches RaySceneExtraction.h. Optional
// closest-hit attributes live in separate buffers in ray_surface_common.slang.
struct RayTriangleData { float4 uv[3]; };
struct RayInstanceData { uint4 info; };
