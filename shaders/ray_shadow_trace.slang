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
[[vk::binding(4, 2)]] Texture2D<float> gDepth;
[[vk::binding(5, 2)]] RWTexture2DArray<float> gRaw;
[[vk::binding(6, 2)]] RWTexture2D<float4> gSurface;

uint RayHash(uint value)
{
    value ^= value >> 16u; value *= 0x7feb352du;
    value ^= value >> 15u; value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}
float RayRandom(uint seed) { return float(RayHash(seed) >> 8u) / 16777216.0; }

float3 SurfaceDerivative(uint2 pixel, int2 axis, uint2 extent, float3 world)
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
        float3 delta = (RayShadowWorld(uint2(p), depth, uRayTrace.camera) - world) * float(sign);
        float lengthSquared = dot(delta, delta);
        if (lengthSquared < bestLength) { best = delta; bestLength = lengthSquared; }
    }
    return best;
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
    float3 world = 0.0.xxx, normal = float3(0, 1, 0);
    if (valid)
    {
        world = RayShadowWorld(tid.xy, depth, uRayTrace.camera);
        float3 dx = SurfaceDerivative(tid.xy, int2(1, 0), extent, world);
        float3 dy = SurfaceDerivative(tid.xy, int2(0, 1), extent, world);
        normal = SafeNormalize(cross(dx, dy), float3(0, 1, 0));
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
            AreaLightData light = uAreaLights[tid.z - 5u];
            AreaLightFrame frame = BuildAreaLightFrame(light);
            uint samples = uRayTrace.read.options.z != 0u ? uRayTrace.read.limits.w : 1u;
            float sum = 0.0, weights = 0.0;
            uint seed = RayHash(tid.x + tid.y * extent.x + tid.z * 97531u + uRayTrace.frame.y * 12347u);
            for (uint i = 0; i < samples; ++i)
            {
                float2 uv = float2((float(i) + RayRandom(seed + i * 2u)) / float(samples),
                                  frac(float(i) * 0.61803398875 + RayRandom(seed + i * 2u + 1u)));
                float2 offset = uv * 2.0 - 1.0;
                if (abs(frame.type - AREA_LIGHT_TYPE_DISK) < 0.5)
                    offset = sqrt(uv.x) * float2(cos(uv.y * 2.0 * PI), sin(uv.y * 2.0 * PI));
                if (uRayTrace.read.options.z == 0u) offset = 0.0.xx;
                float3 origin = frame.position + frame.tangent * offset.x * frame.halfSize.x +
                                frame.bitangent * offset.y * frame.halfSize.y;
                float3 direction;
                float weight = AreaLightPositionContributionWeight(light, frame, world, origin, 1.0, direction);
                if (weight > 0.0)
                {
                    sum += weight * TraceRayVisibility(world, normal, direction, length(origin - world));
                    weights += weight;
                }
            }
            visibility = weights > 1e-8 ? sum / weights : 1.0;
        }
    }
    gRaw[tid] = visibility;
    if (tid.z == 0u) gSurface[tid.xy] = float4(normal, valid ? depth : 0.0);
}
