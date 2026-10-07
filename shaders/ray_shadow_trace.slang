#include "material_data_common.slang"
[[vk::binding(2, 0)]] SamplerState materialSamplers[MATERIAL_SAMPLER_DESCRIPTOR_COUNT];
[[vk::binding(3, 0)]] StructuredBuffer<GpuMaterial> uMaterials;
[[vk::binding(4, 0)]] StructuredBuffer<GpuTextureMetadata> uTextureMetadata;
[[vk::binding(5, 0)]] Texture2D materialTextures[MATERIAL_TEXTURE_DESCRIPTOR_COUNT];
[[vk::binding(0, 1)]] ConstantBuffer<LightingBuffer> uLighting;
[[vk::binding(1, 1)]] StructuredBuffer<PointLightData> uPointLights;
[[vk::binding(2, 1)]] StructuredBuffer<AreaLightData> uAreaLights;
#include "ray_visibility_common.slang"
#include "area_light_common.slang"
#include "ray_shadow_receiver_common.slang"
[[vk::binding(4, 2)]] Texture2D<float> gDepth;
[[vk::binding(5, 2)]] RWTexture2DArray<float> gRaw;
[[vk::binding(6, 2)]] RWTexture2D<float4> gSurface;

float3 SurfaceDerivativeDirection(uint2 pixel, int2 axis, uint2 extent,
                                  float depth, float3 world, float2 sampleOffset)
{
    float3 best = 0.0.xxx;
    float bestLength = 1e30;
    // Choose the nearer world-space neighbour on each axis. This avoids taking
    // a derivative through a depth discontinuity or a background pixel.
    for (int sign = -1; sign <= 1; sign += 2)
    {
        int2 p = int2(pixel) + axis * sign;
        if (any(p < 0) || any(p >= int2(extent))) continue;
        float depth = gDepth.Load(int3(p, 0));
        if (!isfinite(depth) || depth <= 0.0) continue;
        bool neighbourReliable;
        float2 neighbourOffset;
        float3 neighbourWorld = RayShadowReceiverWorld(uint2(p), depth, gDepth,
            uRayTrace.camera, uRayTrace.read.options.w,
            neighbourReliable, neighbourOffset);
        if (RayShadowHasStandardMsaa(uRayTrace.read.options.w) && !neighbourReliable)
            continue;
        float3 delta = (neighbourWorld - world) * float(sign);
        float lengthSquared = dot(delta, delta);
        if (lengthSquared < bestLength) { best = delta; bestLength = lengthSquared; }
    }
    // A thin receiver can have background on both sides of an axis. Bound the
    // chosen neighbour by the local same-depth footprint rather than treating
    // that depth discontinuity as a geometric tangent. Very grazing surfaces
    // also use the conservative ray-direction bias instead of an uncertain
    // normal. Normalize each tangent before crossing to avoid a scale-dependent
    // normal fallback for small world-space pixels.
    float3 footprint = RayShadowWorldAtSample(pixel + uint2(axis), sampleOffset,
                                             depth, uRayTrace.camera) - world;
    float footprintLengthSquared = dot(footprint, footprint);
    if (!isfinite(bestLength) || bestLength <= 1e-30 ||
        !isfinite(footprintLengthSquared) || footprintLengthSquared <= 1e-30 ||
        bestLength > 64.0 * footprintLengthSquared)
        return 0.0.xxx;
    return best * rsqrt(bestLength);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    uint2 extent = uint2(uRayTrace.camera.renderExtent.xy);
    if (any(tid.xy >= extent) || tid.z >= RAY_SHADOW_LAYERS) return;
    float depth = gDepth.Load(int3(tid.xy, 0));
    float visibility = 1.0;
    bool valid = isfinite(depth) && depth > 0.0;
    float3 world = 0.0.xxx, normal = 0.0.xxx;
    if (valid)
    {
        bool reconstructionReliable;
        float2 sampleOffset;
        world = RayShadowReceiverWorld(tid.xy, depth, gDepth, uRayTrace.camera,
            uRayTrace.read.options.w, reconstructionReliable, sampleOffset);
        if (!RayShadowHasStandardMsaa(uRayTrace.read.options.w) || reconstructionReliable)
        {
            float3 dx = SurfaceDerivativeDirection(tid.xy, int2(1, 0), extent, depth, world, sampleOffset);
            float3 dy = SurfaceDerivativeDirection(tid.xy, int2(0, 1), extent, depth, world, sampleOffset);
            normal = SafeNormalize(cross(dx, dy), 0.0.xxx);
        }
        // A zero normal denotes an unreliable neighbourhood. It requests the
        // conservative trace bias and rejects normal-compatible filter history.
        if (tid.z == 0u && any(uLighting.directionalColorIntensity.rgb * uLighting.directionalColorIntensity.w > 0.0))
            visibility = TraceRayVisibility(world, normal,
                SafeNormalize(-uLighting.directionalDirection.xyz, float3(0, 1, 0)), 1e20);
        else if (tid.z >= 1u && tid.z <= uRayTrace.read.limits.y)
        {
            PointLightData light = uPointLights[tid.z - 1u];
            float3 delta = light.positionRadius.xyz - world;
            float distance = length(delta);
            if (PointLightHasRenderableIntensity(light) && distance > 1e-5 &&
                (!PointLightHasFiniteRange(light.positionRadius.w) || distance < light.positionRadius.w))
                visibility = TraceRayVisibility(world, normal, delta / distance, distance);
        }
        else if (tid.z >= 5u && tid.z < 5u + uRayTrace.read.limits.z)
        {
            visibility = TraceAreaRayVisibility(uAreaLights[tid.z - 5u], world,
                                               normal, tid.xy, tid.z);
        }
    }
    gRaw[tid] = visibility;
    if (tid.z == 0u) gSurface[tid.xy] = float4(normal, valid ? depth : 0.0);
}
