#include "lighting_structs.slang"
#include "object_data_common.slang"

struct BimMeshletGpuCluster
{
    uint4 drawRange;          // x meshId, y firstIndex, z indexCount, w triangleCount
    uint4 lodInfo;            // x lodLevel, y estimated, zw reserved
    float4 boundsCenterRadius;
};

struct BimMeshletGpuObjectLod
{
    uint4 objectInfo;         // x objectIndex, y sourceElementIndex, z meshId, w geometryKind
    uint4 clusterInfo;        // x firstCluster, y clusterCount, z maxLod, w estimated
};

struct BimMeshletResidencyEntry
{
    uint flags;               // bit 0 = resident
    uint selectedLod;
    uint firstCluster;
    uint clusterCount;
};

struct BimMeshletResidencyPushConstants
{
    uint objectCount;
    uint forceResident;
    uint drawBudgetMaxObjects;
    uint selectedObjectIndex;
    int lodBias;
    float screenErrorPixels;
    float viewportHeightPixels;
    uint flags;
};

[[vk::binding(0, 0)]] StructuredBuffer<BimMeshletGpuCluster> uMeshletClusters;
[[vk::binding(1, 0)]] StructuredBuffer<BimMeshletGpuObjectLod> uObjectLods;
[[vk::binding(2, 0)]] RWStructuredBuffer<BimMeshletResidencyEntry> uResidency;
[[vk::binding(3, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(4, 0)]] StructuredBuffer<ObjectBuffer> uObjects;

[[vk::push_constant]]
ConstantBuffer<BimMeshletResidencyPushConstants> pc;

static const uint BIM_MESHLET_RESIDENCY_AUTO_LOD = 1u << 0u;
static const uint BIM_MESHLET_RESIDENCY_DRAW_BUDGET = 1u << 1u;
static const uint BIM_MESHLET_RESIDENCY_KEEP_SELECTED = 1u << 2u;
static const uint BIM_GEOMETRY_KIND_POINTS = 1u;
static const uint BIM_GEOMETRY_KIND_CURVES = 2u;
static const uint BIM_MESHLET_RESIDENCY_PAUSED = 1u << 3u;

float projectedRadiusPixels(float3 center, float radius)
{
    if (!isfinite(radius) || radius <= 0.0)
    {
        return 0.0;
    }

    float4x4 stableViewProj = uCamera.temporalInfo.w != 0u ? uCamera.unjitteredViewProj : uCamera.viewProj;
    float4 clipCenter = mul(stableViewProj, float4(center, 1.0));
    if (abs(clipCenter.w) <= 1e-5)
    {
        return 0.0;
    }

    float3 cameraRight = float3(stableViewProj[0][0], stableViewProj[0][1], stableViewProj[0][2]);
    const float rightLengthSq = dot(cameraRight, cameraRight);
    if (rightLengthSq <= 1e-8)
    {
        cameraRight = float3(1.0, 0.0, 0.0);
    }
    else
    {
        cameraRight *= rsqrt(rightLengthSq);
    }

    float4 clipOffset = mul(stableViewProj,
                            float4(center + cameraRight * radius, 1.0));
    if (abs(clipOffset.w) <= 1e-5)
    {
        return 0.0;
    }

    const float centerX = clipCenter.x / clipCenter.w;
    const float offsetX = clipOffset.x / clipOffset.w;
    return abs(offsetX - centerX) * max(pc.viewportHeightPixels, 1.0) * 0.5;
}

uint selectLodLevel(BimMeshletGpuObjectLod lod, uint objectIndex)
{
    const uint maxLod = lod.clusterInfo.z;
    if ((pc.flags & BIM_MESHLET_RESIDENCY_AUTO_LOD) == 0u || maxLod == 0u)
    {
        return 0u;
    }

    const ObjectBuffer objectData = uObjects[objectIndex];
    const float4 sphere = objectData.boundingSphere;
    const float projectedPixels = projectedRadiusPixels(sphere.xyz, sphere.w);
    const float screenError = max(pc.screenErrorPixels, 0.25);
    const float ratio = max(screenError / max(projectedPixels, 1e-3), 1.0);
    int selectedLod = int(floor(log2(ratio))) + pc.lodBias;
    selectedLod = clamp(selectedLod, 0, int(maxLod));
    return uint(selectedLod);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint objectIndex = dispatchThreadId.x;
    if (objectIndex >= pc.objectCount)
    {
        return;
    }

    const BimMeshletGpuObjectLod lod = uObjectLods[objectIndex];
    const uint sceneObjectIndex = lod.objectInfo.x;
    const uint geometryKind = lod.objectInfo.w;
    const bool hasClusters = lod.clusterInfo.y > 0u;
    const bool nativePrimitive =
        geometryKind == BIM_GEOMETRY_KIND_POINTS ||
        geometryKind == BIM_GEOMETRY_KIND_CURVES;
    const bool selectedObject =
        (pc.flags & BIM_MESHLET_RESIDENCY_KEEP_SELECTED) != 0u &&
        sceneObjectIndex == pc.selectedObjectIndex;
    const bool budgetResident =
        (pc.flags & BIM_MESHLET_RESIDENCY_DRAW_BUDGET) == 0u ||
        sceneObjectIndex < pc.drawBudgetMaxObjects ||
        selectedObject;

    BimMeshletResidencyEntry entry;
    entry.flags = (pc.forceResident != 0u ||
                   ((hasClusters || nativePrimitive) && budgetResident)) ? 1u : 0u;
    entry.selectedLod = selectLodLevel(lod, sceneObjectIndex);
    entry.firstCluster = lod.clusterInfo.x;
    entry.clusterCount = lod.clusterInfo.y;
    uResidency[objectIndex] = entry;
}
