#include "lighting_structs.slang"
#include "temporal_common.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] SamplerState colorSampler;
[[vk::binding(2, 0)]] Texture2D<float4> currentColor;
[[vk::binding(3, 0)]] Texture2D<float4> currentMotion;
[[vk::binding(4, 0)]] Texture2D<uint> currentIdentity;
[[vk::binding(5, 0)]] Texture2D<float> currentDepth;
[[vk::binding(6, 0)]] Texture2D<float4> previousColor;
[[vk::binding(7, 0)]] Texture2D<float2> previousDepth;
[[vk::binding(8, 0)]] Texture2D<uint> previousIdentity;
[[vk::binding(9, 0)]] RWTexture2D<float4> outputColor;
[[vk::binding(10, 0)]] RWTexture2D<float2> outputDepth;
[[vk::binding(11, 0)]] RWTexture2D<uint> outputIdentity;
[[vk::binding(12, 0)]] RWTexture2D<float4> diagnostics;

struct ResolveConstants
{
    uint width; uint height; uint historyValid; uint orthographic;
    float historyWeight; float absoluteTolerance; float relativeTolerance; float varianceGamma;
    float nearPlane; float farPlane; float previousNear; float previousFar;
};
[[vk::push_constant]] ConstantBuffer<ResolveConstants> pc;

float3 safeColor(float3 value) { return all(isfinite(value)) ? clamp(value, 0.0.xxx, 65000.0.xxx) : 0.0.xxx; }
float distanceFromDepth(float depth)
{
    return pc.orthographic != 0u ? pc.previousFar - depth * (pc.previousFar - pc.previousNear) :
        pc.previousNear * pc.previousFar / (depth * (pc.previousFar - pc.previousNear) + pc.previousNear);
}
int2 pixelAt(float2 uv) { return clamp(int2(uv * float2(pc.width, pc.height)), int2(0), int2(pc.width - 1, pc.height - 1)); }

// Catmull-Rom reconstructs jittered samples without the repeated tent-filter
// blur of bilinear current sampling. Clamp ringing to the local HDR envelope.
float4 cubicWeights(float t) {
    float t2 = t * t, t3 = t2 * t;
    return float4(-0.5*t + t2 - 0.5*t3, 1.0 - 2.5*t2 + 1.5*t3,
                  0.5*t + 2.0*t2 - 1.5*t3, -0.5*t2 + 0.5*t3);
}
float3 reconstructCurrent(float2 rasterUv, int2 center) {
    float2 position = rasterUv * float2(pc.width, pc.height) - 0.5;
    int2 base = int2(floor(position));
    float4 wx = cubicWeights(frac(position.x)), wy = cubicWeights(frac(position.y));
    float3 color = 0.0.xxx, low = 65000.0.xxx, high = 0.0.xxx;
    for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) {
        int2 tap = clamp(base + int2(x-1,y-1), int2(0), int2(pc.width-1, pc.height-1));
        color += safeColor(currentColor.Load(int3(tap,0)).rgb) * wx[x] * wy[y];
    }
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
        int2 tap = clamp(center + int2(x,y), int2(0), int2(pc.width-1,pc.height-1));
        float3 sample = safeColor(currentColor.Load(int3(tap,0)).rgb);
        low = min(low, sample); high = max(high, sample);
    }
    return clamp(safeColor(color), low, high);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.width || id.y >= pc.height) return;
    uint2 pixel = id.xy;
    float2 size = float2(pc.width, pc.height);
    float2 uv = (float2(pixel) + 0.5) / size;
    float2 rasterUv = uv + uCamera.jitterUv.xy;
    int2 center = pixelAt(rasterUv);
    float3 color = reconstructCurrent(rasterUv, center);
    float4 motion = currentMotion.Load(int3(center, 0));
    uint identity = currentIdentity.Load(int3(center, 0));
    float depth = currentDepth.Load(int3(center, 0));
    float reactive = saturate(currentColor.Load(int3(center, 0)).a);
    // Dilate reactive coverage over the reconstruction footprint. Transparent
    // edges must not borrow opaque velocity from a neighboring sample.
    int2 reactiveBase = int2(floor(rasterUv * size - 0.5));
    for (int y = -1; y <= 2; ++y)
    for (int x = -1; x <= 2; ++x)
        reactive = max(reactive, saturate(currentColor.Load(int3(clamp(reactiveBase+int2(x,y), int2(0), int2(pc.width-1, pc.height-1)),0)).a));
    float weight = 0.0;
    float age = 1.0;
    uint reason = 0u; // accepted, first/reset, uncovered, invalid, offscreen, identity, depth, reactive
    float2 historyUv = uv + motion.xy;
    float2 depthUv = historyUv + uCamera.jitterUv.zw;
    if (pc.historyValid == 0u || uCamera.temporalInfo.x == 0u) reason = 1u;
    else if (!isfinite(depth) || depth <= 0.0 || identity == 0u) reason = 2u;
    else if (!all(isfinite(motion)) || motion.w < 0.5 || motion.z < 0.0 || motion.z > 1.0) reason = 3u;
    else if (!temporalInsideImage(rasterUv) || !temporalInsideImage(historyUv) || !temporalInsideImage(depthUv)) reason = 4u;
    else
    {
        int2 oldPixel = pixelAt(depthUv);
        float2 oldDepth = previousDepth.Load(int3(oldPixel, 0));
        uint oldIdentity = previousIdentity.Load(int3(oldPixel, 0));
        float expectedDistance = distanceFromDepth(motion.z);
        float sampledDistance = distanceFromDepth(oldDepth.x);
        // Bilinear color taps must all belong to this surface. Raw metadata
        // lives on the old jittered grid, so address each color tap there.
        bool footprintCompatible = true;
        int2 historyBase = int2(floor(historyUv * size - 0.5));
        float2 fraction = frac(historyUv * size - 0.5);
        for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) {
            float tapWeight = (x == 0 ? 1.0-fraction.x : fraction.x) * (y == 0 ? 1.0-fraction.y : fraction.y);
            if (tapWeight < 0.01) continue;
            float2 tapUv = (float2(historyBase + int2(x,y)) + 0.5) / size + uCamera.jitterUv.zw;
            int2 metadataPixel = pixelAt(tapUv);
            float2 metadata = previousDepth.Load(int3(metadataPixel,0));
            footprintCompatible = footprintCompatible && temporalInsideImage(tapUv) &&
                previousIdentity.Load(int3(metadataPixel,0)) == identity && all(isfinite(metadata)) &&
                metadata.x > 0.0 && metadata.y <= 0.01;
        }
        if (identity != oldIdentity) reason = 5u;
        else if (!all(isfinite(oldDepth)) || oldDepth.x <= 0.0 ||
                 !isfinite(expectedDistance) || !isfinite(sampledDistance) ||
                 abs(expectedDistance - sampledDistance) > pc.absoluteTolerance + pc.relativeTolerance * expectedDistance)
            reason = 6u;
        else if (reactive > 0.01 || oldDepth.y > 0.01) reason = 7u;
        else if (!footprintCompatible) reason = 5u;
        else
        {
            float4 oldColor = previousColor.SampleLevel(colorSampler, historyUv, 0);
            if (!all(isfinite(oldColor))) reason = 3u;
            else
            {
                // Variance clipping in a bounded HDR neighborhood. Accumulate
                // normalized values to avoid overflow from bright emitters.
                float3 low = color, high = color, mean = 0.0.xxx, square = 0.0.xxx;
                for (int y = -1; y <= 1; ++y)
                for (int x = -1; x <= 1; ++x)
                {
                    int2 tap = clamp(center + int2(x, y), int2(0), int2(pc.width - 1, pc.height - 1));
                    float3 sample = safeColor(currentColor.Load(int3(tap, 0)).rgb);
                    low = min(low, sample); high = max(high, sample);
                    float3 scaled = sample / 65000.0;
                    mean += scaled / 9.0; square += scaled * scaled / 9.0;
                }
                float3 sigma = sqrt(max(square - mean * mean, 0.0.xxx)) * 65000.0;
                mean *= 65000.0;
                low = max(low, mean - pc.varianceGamma * sigma);
                high = min(high, mean + pc.varianceGamma * sigma);
                float3 history = clamp(safeColor(oldColor.rgb), min(low, high), max(low, high));
                age = min(max(oldColor.a, 1.0) + 1.0, 64.0);
                float motionPixels = length(motion.xy * size);
                weight = min(pc.historyWeight, (age - 1.0) / age) * (1.0 - saturate(motionPixels / 64.0) * 0.5);
                // Rapid shading changes (moving highlights/lights) lower
                // confidence even when geometric reprojection is compatible.
                float currentLuma = dot(color, float3(0.2126,0.7152,0.0722));
                float historyLuma = dot(history, float3(0.2126,0.7152,0.0722));
                float shadingChange = abs(currentLuma-historyLuma) / max(max(currentLuma,historyLuma),0.01);
                weight *= 1.0 - saturate(shadingChange * 0.5);
                color = safeColor(lerp(color, history, weight));
            }
        }
    }
    outputColor[pixel] = float4(color, age);
    // Retain raw raster depth/identity/reactivity, NOT resampled output metadata.
    float rawDepth = currentDepth.Load(int3(pixel, 0));
    outputDepth[pixel] = float2(isfinite(rawDepth) ? rawDepth : 0.0, saturate(currentColor.Load(int3(pixel, 0)).a));
    outputIdentity[pixel] = currentIdentity.Load(int3(pixel, 0));
    diagnostics[pixel] = float4(age / 64.0, float(reason) / 7.0, weight, reactive);
}
