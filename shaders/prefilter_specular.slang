// prefilter_specular.slang — Compute shader for split-sum pre-filtered
// specular environment cubemap.  Dispatched per face per mip level.
// EnvironmentManager supplies the output size and roughness levels (0.0→1.0).

static const float PI = 3.14159265359;
static const uint  SAMPLE_COUNT = 1024u;

#include "push_constants_common.slang"

[[vk::push_constant]]
ConstantBuffer<PrefilterPushConstants> pc;

[[vk::binding(0, 0)]] TextureCube<float4>      envCubemap;
[[vk::binding(1, 0)]] SamplerState             envSampler;
[[vk::binding(2, 0)]] RWTexture2DArray<float4>  outPrefiltered;

float RadicalInverse_VdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

float2 Hammersley(uint i, uint N)
{
    return float2(float(i) / float(N), RadicalInverse_VdC(i));
}

float3 ImportanceSampleGGX(float2 Xi, float roughness, float3 N)
{
    float a = roughness * roughness;
    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    float3 H;
    H.x = sinTheta * cos(phi);
    H.y = sinTheta * sin(phi);
    H.z = cosTheta;

    float3 up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);

    return tangent * H.x + bitangent * H.y + N * H.z;
}

float DistributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-6);
}

float3 CubeTexelToDir(uint face, float2 uv)
{
    float2 st = uv * 2.0 - 1.0;
    float3 dir;
    switch (face)
    {
    case 0: dir = float3( 1.0, -st.y, -st.x); break;
    case 1: dir = float3(-1.0, -st.y,  st.x); break;
    case 2: dir = float3( st.x,  1.0,  st.y); break;
    case 3: dir = float3( st.x, -1.0, -st.y); break;
    case 4: dir = float3( st.x, -st.y,  1.0); break;
    case 5: dir = float3(-st.x, -st.y, -1.0); break;
    default: dir = float3(0.0, 0.0, 1.0); break;
    }
    return normalize(dir);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void computeMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= pc.faceSize || dtid.y >= pc.faceSize) return;

    float2 uv = (float2(dtid.xy) + 0.5) / float(pc.faceSize);
    float3 N = CubeTexelToDir(pc.faceIndex, uv);
    // The zero-roughness GGX limit is a delta at the reflected direction.
    // Integrating its singular distribution can introduce numerical angular
    // spread; preserve source detail directly and avoid redundant samples.
    if (pc.roughness == 0.0)
    {
        outPrefiltered[uint3(dtid.xy, pc.faceIndex)] =
            float4(envCubemap.SampleLevel(envSampler, N, 0.0).rgb, 1.0);
        return;
    }
    float3 R = N;
    float3 V = R;

    float totalWeight = 0.0;
    float3 prefilteredColor = float3(0.0, 0.0, 0.0);
    float sourceResolution = exp2(max(float(pc.sourceMipCount), 1.0) - 1.0);
    float sourceTexelSolidAngle = 4.0 * PI / max(6.0 * sourceResolution * sourceResolution, 1.0);
    float maxSourceMip = max(float(pc.sourceMipCount) - 1.0, 0.0);

    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        float2 Xi = Hammersley(i, SAMPLE_COUNT);
        float3 H = ImportanceSampleGGX(Xi, pc.roughness, N);
        float3 L = 2.0 * dot(V, H) * H - V;

        float NdotL = saturate(dot(N, L));
        if (NdotL > 0.0)
        {
            float NdotH = saturate(dot(N, H));
            float VdotH = saturate(dot(V, H));
            float pdf = DistributionGGX(NdotH, pc.roughness) * NdotH /
                        max(4.0 * VdotH, 1e-4);
            float sampleSolidAngle = 1.0 / max(float(SAMPLE_COUNT) * pdf, 1e-6);
            float sourceMip = pc.roughness <= 0.001
                ? 0.0
                : 0.5 * log2(sampleSolidAngle / sourceTexelSolidAngle);
            sourceMip = clamp(sourceMip, 0.0, maxSourceMip);
            prefilteredColor += envCubemap.SampleLevel(envSampler, L, sourceMip).rgb * NdotL;
            totalWeight += NdotL;
        }
    }

    prefilteredColor /= max(totalWeight, 1e-4);
    outPrefiltered[uint3(dtid.xy, pc.faceIndex)] = float4(prefilteredColor, 1.0);
}
