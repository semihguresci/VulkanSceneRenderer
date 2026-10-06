#include "ray_shadow_data.slang"
#include "temporal_common.slang"
[[vk::binding(0, 0)]] ConstantBuffer<RayShadowTraceData> uRayTrace;
[[vk::binding(1, 0)]] Texture2DArray<float> gCurrent;
[[vk::binding(2, 0)]] Texture2DArray<float> gPrevious;
[[vk::binding(3, 0)]] Texture2D<float4> gSurface;
[[vk::binding(4, 0)]] Texture2D<float4> gPreviousSurface;
[[vk::binding(5, 0)]] RWTexture2DArray<float> gOutput;
[[vk::push_constant]] ConstantBuffer<uint> uSpatial;

bool CompatibleSurface(float4 a, float4 b)
{
    return a.w > 0.0 && b.w > 0.0 && dot(a.xyz, b.xyz) > 0.9 &&
           abs(a.w - b.w) < max(1e-5, a.w * 0.01);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    uint2 extent = uint2(uRayTrace.camera.renderExtent.xy);
    if (any(tid.xy >= extent) || tid.z >= RAY_SHADOW_LAYERS) return;
    float current = gCurrent.Load(int4(tid, 0));
    float4 surface = gSurface.Load(int3(tid.xy, 0));
    // Hard directional/point/spot visibility is exact; filtering it would erase
    // thin blockers. Only sampled emitters use independent visibility history.
    bool soft = tid.z >= 5u && uRayTrace.read.options.z != 0u &&
                uRayTrace.read.options.x != 0u && surface.w > 0.0;
    float result = current;
    if (soft && uSpatial == 0u && uRayTrace.frame.x != 0u)
    {
        float3 world = RayShadowWorld(tid.xy, surface.w, uRayTrace.camera);
        TemporalProjectedPoint previous = temporalProjectSceneClip(mul(uRayTrace.previousViewProj, float4(world, 1.0)));
        if (previous.valid && temporalInsideImage(previous.uv))
        {
            uint2 pixel = min(uint2(previous.uv * float2(extent)), extent - 1u);
            float4 previousSurface = gPreviousSurface.Load(int3(pixel, 0));
            float4 expected = float4(surface.xyz, previous.reverseZDepth);
            if (CompatibleSurface(expected, previousSurface))
            {
                float history = gPrevious.Load(int4(pixel, tid.z, 0));
                float low = current, high = current;
                for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x)
                {
                    int2 p = clamp(int2(tid.xy) + int2(x, y), int2(0), int2(extent) - 1);
                    if (CompatibleSurface(surface, gSurface.Load(int3(p, 0))))
                    {
                        float value = gCurrent.Load(int4(p, tid.z, 0));
                        low = min(low, value); high = max(high, value);
                    }
                }
                result = lerp(current, clamp(history, low, high), 0.8);
            }
        }
    }
    else if (soft && uSpatial != 0u)
    {
        float sum = current, weights = 1.0;
        for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x)
        {
            if (x == 0 && y == 0) continue;
            int2 p = clamp(int2(tid.xy) + int2(x, y), int2(0), int2(extent) - 1);
            if (CompatibleSurface(surface, gSurface.Load(int3(p, 0))))
            {
                float value = gCurrent.Load(int4(p, tid.z, 0));
                float weight = exp(-abs(value - current) * 4.0) * 0.5;
                sum += weight * value; weights += weight;
            }
        }
        result = sum / weights;
    }
    gOutput[tid] = saturate(result);
}
