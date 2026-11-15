#include "brdf_common.slang"
#include "lighting_structs.slang"
#include "push_constants_common.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] SamplerState gBufferSampler;
[[vk::binding(2, 0)]] Texture2D<float4> gAlbedoTexture;
[[vk::binding(3, 0)]] Texture2D<float4> gNormalTexture;
[[vk::binding(4, 0)]] Texture2D<float4> gMaterialTexture;
[[vk::binding(5, 0)]] Texture2D<float4> gEmissiveTexture;
[[vk::binding(6, 0)]] Texture2D<float> gDepthTexture;
[[vk::binding(17, 0)]] Texture2D<float4> gSpecularTexture;

[[vk::push_constant]]
ConstantBuffer<GBufferCompositePushConstants> pc;

struct VSOutput
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float2 texCoord;
};

[shader("vertex")]
VSOutput vertMain(uint vertexIndex : SV_VertexID)
{
    static const float2 kPositions[3] = {
        float2(-1.0, -1.0),
        float2(3.0, -1.0),
        float2(-1.0, 3.0),
    };

    static const float2 kTexCoords[3] = {
        float2(0.0, 0.0),
        float2(2.0, 0.0),
        float2(0.0, 2.0),
    };

    VSOutput output;
    output.pos = float4(kPositions[vertexIndex], 0.0, 1.0);
    output.texCoord = kTexCoords[vertexIndex];
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0
{
    uint width = 0u;
    uint height = 0u;
    gDepthTexture.GetDimensions(width, height);
    uint2 pixelCoord = uint2(input.pos.xy);
    pixelCoord = min(pixelCoord, uint2(max(width, 1u) - 1u, max(height, 1u) - 1u));

    uint outputMode = pc.outputMode;
    if (outputMode == 1u)
    {
        float4 albedo = gAlbedoTexture.Load(int3(pixelCoord, 0));
        albedo.rgb = max(Finite3Or(albedo.rgb, 0.0.xxx), 0.0.xxx);
        albedo.a = saturate(FiniteScalarOr(albedo.a, 1.0));
        return albedo;
    }

    if (outputMode == 2u)
    {
        float3 encodedNormal = gNormalTexture.Load(int3(pixelCoord, 0)).xyz;
        float3 normal = SafeNormalize(encodedNormal * 2.0 - 1.0, float3(0.0, 1.0, 0.0));
        return float4(normal * 0.5 + 0.5, 1.0);
    }

    if (outputMode == 3u)
    {
        float4 material = gMaterialTexture.Load(int3(pixelCoord, 0));
        material.xyz = Finite3Or(material.xyz, 0.0.xxx);
        return float4(material.xyz, 1.0);
    }

    if (outputMode == 4u)
    {
        float depth = gDepthTexture.Load(int3(pixelCoord, 0));
        if (!isfinite(depth) || depth <= 0.0 || depth > 1.0)
        {
            return float4(0.0.xxx, 1.0);
        }
        float depthVisual = 1.0 - pow(saturate(depth), 48.0);
        return float4(depthVisual.xxx, 1.0);
    }

    if (outputMode == 5u)
    {
        float3 emissive =
            Finite3Or(gEmissiveTexture.Load(int3(pixelCoord, 0)).rgb,
                      0.0.xxx);
        return float4(emissive, 1.0);
    }

    float depth = gDepthTexture.Load(int3(pixelCoord, 0));
    float4 albedo = gAlbedoTexture.Load(int3(pixelCoord, 0));
    albedo.rgb = max(Finite3Or(albedo.rgb, 0.0.xxx), 0.0.xxx);
    albedo.a = saturate(FiniteScalarOr(albedo.a, 0.0));
    if (!isfinite(depth) || depth <= 1e-6 || depth > 1.0 || albedo.a <= 0.0)
    {
        float3 emissive =
            Finite3Or(gEmissiveTexture.Load(int3(pixelCoord, 0)).rgb,
                      0.0.xxx);
        return float4(emissive, 1.0);
    }

    float3 encodedNormal = gNormalTexture.Load(int3(pixelCoord, 0)).xyz;
    float3 normal = SafeNormalize(encodedNormal * 2.0 - 1.0, float3(0.0, 1.0, 0.0));
    float4 material = gMaterialTexture.Load(int3(pixelCoord, 0));
    float4 emissiveSample = gEmissiveTexture.Load(int3(pixelCoord, 0));
    material.xyz = Finite3Or(material.xyz, float3(0.0, 1.0, 1.0));
    float3 emissive = Finite3Or(emissiveSample.rgb, 0.0.xxx);
    float3 dielectricF0Color = gSpecularTexture.Load(int3(pixelCoord, 0)).rgb;
    float dielectricF0 = saturate(MaxComponent3(Finite3Or(dielectricF0Color, 0.04.xxx)));

    float2 uv = (float2(pixelCoord) + 0.5.xx) / float2(max(width, 1u), max(height, 1u));
    float3 worldPosition = 0.0.xxx;
    if (!TryReconstructWorldPosition(uv, depth, uCamera.inverseViewProj,
                                     worldPosition))
    {
        return float4(emissive, 1.0);
    }
    float3 V = SafeNormalize(uCamera.cameraWorldPosition.xyz - worldPosition, float3(0.0, 0.0, 1.0));
    float3 L = SafeNormalize(float3(0.45, 0.65, 1.0), float3(0.0, 1.0, 0.0));
    float3 lightColor = float3(4.0, 3.8, 3.5);

    float metallic = saturate(FiniteScalarOr(material.x, 0.0));
    float roughness = clamp(FiniteScalarOr(material.y, 1.0), 0.045, 1.0);
    float occlusion = saturate(FiniteScalarOr(material.z, 1.0));

    float3 F0 = EvaluatePbrF0FromDielectricF0(
        albedo.rgb, metallic, dielectricF0);
    float3 directLighting = EvaluatePbrDirectLight(
        albedo.rgb, F0, metallic, roughness, normal, V, L, lightColor);
    float3 ambientLighting = albedo.rgb * 0.03 * occlusion;
    float3 finalColor =
        Finite3Or(ambientLighting + directLighting + emissive, emissive);

    return float4(finalColor, 1.0);
}
