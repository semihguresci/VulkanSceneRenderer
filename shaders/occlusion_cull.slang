// occlusion_cull.slang - Compute shader for Hi-Z occlusion culling.
// Tests each object's bounding sphere against the Hi-Z pyramid generated
// from the depth prepass. Objects that pass the occlusion test are written
// to the indirect draw buffer for the G-Buffer pass.

#include "lighting_structs.slang"
#include "push_constants_common.slang"
#include "draw_indirect_common.slang"

#include "object_data_common.slang"
#include "occlusion_sphere_common.slang"

[[vk::push_constant]]
ConstantBuffer<CullPushConstants> pc;

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer>                 uCamera;
[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer>              uObjects;
[[vk::binding(2, 0)]] StructuredBuffer<DrawIndexedIndirectCommand> uInputDraws;
[[vk::binding(3, 0)]] RWStructuredBuffer<DrawIndexedIndirectCommand> uOutputDraws;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint>                     uDrawCount;
[[vk::binding(5, 0)]] Texture2D<float>                             gHiZPyramid;
[[vk::binding(6, 0)]] StructuredBuffer<uint>                       uInputDrawCount;
[[vk::binding(7, 0)]] SamplerState                                 gHiZSampler;

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 dtid : SV_DispatchThreadID)
{
    uint idx = dtid.x;
    uint frustumCulledCount = uInputDrawCount[0];
    if (idx >= frustumCulledCount) return;

    DrawIndexedIndirectCommand draw = uInputDraws[idx];
    uint objectIndex = draw.firstInstance;
    ObjectBuffer obj = uObjects[objectIndex];
    float4 bs = obj.boundingSphere;
    float3 center = bs.xyz;
    float radius = bs.w;

    bool visible = true;

    if (radius > 0.0)
    {
        float2 minUV, maxUV;
        float closestDepth;
        if (ProjectSphere(uCamera.viewProj, center, radius, minUV, maxUV, closestDepth))
        {
            uint hizW, hizH;
            gHiZPyramid.GetDimensions(hizW, hizH);

            float2 sizePixels = (maxUV - minUV) * float2(hizW, hizH);
            float maxExtent = max(sizePixels.x, sizePixels.y);

            // Large projected bounds are poor occlusion-cull candidates with a
            // current-frame Hi-Z pyramid: wall reliefs and other attached
            // details can share screen space with the occluder that was just
            // written by the depth prepass. Keep those visible and let the
            // hardware depth test reject fragments instead.
            const float largeProjectedExtentPixels = 96.0;
            if (maxExtent <= largeProjectedExtentPixels)
            {
                float mipLevel = max(0.0, ceil(log2(max(1.0, maxExtent))) - 1.0);
                float2 centerUV = (minUV + maxUV) * 0.5;
                float2 minMaxXUV = float2(maxUV.x, minUV.y);
                float2 maxMinXUV = float2(minUV.x, maxUV.y);
                float2 midTopUV = float2(centerUV.x, minUV.y);
                float2 midBottomUV = float2(centerUV.x, maxUV.y);
                float2 midLeftUV = float2(minUV.x, centerUV.y);
                float2 midRightUV = float2(maxUV.x, centerUV.y);

                // Reverse-Z Hi-Z stores the minimum depth in the sampled
                // region, i.e. the farthest visible surface. Use several
                // samples over the projected bounds so elongated or edge-aligned
                // meshes are not rejected by a single optimistic texel.
                float conservativeHiZ = gHiZPyramid.SampleLevel(
                    gHiZSampler, centerUV, mipLevel);
                conservativeHiZ = min(conservativeHiZ,
                    gHiZPyramid.SampleLevel(gHiZSampler, minUV, mipLevel));
                conservativeHiZ = min(conservativeHiZ,
                    gHiZPyramid.SampleLevel(gHiZSampler, maxUV, mipLevel));
                conservativeHiZ = min(conservativeHiZ,
                    gHiZPyramid.SampleLevel(gHiZSampler, minMaxXUV, mipLevel));
                conservativeHiZ = min(conservativeHiZ,
                    gHiZPyramid.SampleLevel(gHiZSampler, maxMinXUV, mipLevel));
                conservativeHiZ = min(conservativeHiZ,
                    gHiZPyramid.SampleLevel(gHiZSampler, midTopUV, mipLevel));
                conservativeHiZ = min(conservativeHiZ,
                    gHiZPyramid.SampleLevel(gHiZSampler, midBottomUV, mipLevel));
                conservativeHiZ = min(conservativeHiZ,
                    gHiZPyramid.SampleLevel(gHiZSampler, midLeftUV, mipLevel));
                conservativeHiZ = min(conservativeHiZ,
                    gHiZPyramid.SampleLevel(gHiZSampler, midRightUV, mipLevel));

                // Keep the test biased toward visibility. A strict comparison
                // can falsely reject thin reliefs or objects embedded near a
                // wall after the depth prepass has populated Hi-Z with nearby
                // surfaces.
                const float visibilityBias = 0.005;
                if (closestDepth + visibilityBias < conservativeHiZ)
                {
                    visible = false;
                }
            }
        }
    }

    if (visible)
    {
        uint outIdx;
        InterlockedAdd(uDrawCount[0], 1, outIdx);
        uOutputDraws[outIdx] = draw;
    }
}
