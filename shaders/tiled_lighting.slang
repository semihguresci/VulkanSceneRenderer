// tiled_lighting.slang — Fullscreen pass that accumulates point light
// contributions using the tile light grid built by tile_light_cull.slang.
// Replaces the per-light stencil draw loop.

#include "brdf_common.slang"
#include "depth_normal_common.slang"
#include "lighting_structs.slang"
#include "material_data_common.slang"
#include "push_constants_common.slang"
#include "local_shadow_common.slang"
#define RAY_SHADOW_READ_SET 3
#include "ray_shadow_read.slang"
#include "screen_space_light_shadow_common.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(2, 0)]] Texture2D<float4> gAlbedoTexture;
[[vk::binding(3, 0)]] Texture2D<float4> gNormalTexture;
[[vk::binding(4, 0)]] Texture2D<float4> gMaterialTexture;
[[vk::binding(5, 0)]] Texture2D<float4> gEmissiveTexture;
[[vk::binding(6, 0)]] Texture2D<float> gDepthTexture;
[[vk::binding(17, 0)]] Texture2D<float4> gSpecularTexture;
[[vk::binding(18, 0)]] ConstantBuffer<LocalShadowBuffer> uLocalShadow;
[[vk::binding(19, 0)]] Texture2DArray<float> gLocalShadowAtlas;
[[vk::binding(20, 0)]] SamplerComparisonState gLocalShadowSampler;
[[vk::binding(21, 0)]] Texture2D<float4> gMaterialLayersTexture;
[[vk::binding(22, 0)]] Texture2D<float4> gMaterialSheenTexture;
// Bindings 7-9 remain reserved for directional shadow inputs.

// Set 1: tile resources.
[[vk::binding(0, 1)]] StructuredBuffer<PointLightData> uLights;
[[vk::binding(1, 1)]] StructuredBuffer<TileLightGrid> uTileGrid;
[[vk::binding(2, 1)]] StructuredBuffer<uint> uLightIndexList;
[[vk::binding(3, 2)]] StructuredBuffer<GpuMaterial> uMaterials;

[[vk::push_constant]]
ConstantBuffer<TiledLightingPushConstants> pc;

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

static const float LOCAL_CONTACT_SHADOW_RADIANCE_THRESHOLD = 1.0e-4;
static const float LOCAL_CONTACT_SHADOW_VISIBILITY_THRESHOLD = 0.02;

bool ShouldTracePointLightContactShadow(float3 radiance, float lightVisibility)
{
    float3 finiteRadiance =
        all(isfinite(radiance)) ? max(radiance, 0.0.xxx) : 0.0.xxx;
    float radianceMagnitude =
        max(max(finiteRadiance.x, finiteRadiance.y), finiteRadiance.z);
    return isfinite(lightVisibility) &&
        radianceMagnitude > LOCAL_CONTACT_SHADOW_RADIANCE_THRESHOLD &&
        lightVisibility > LOCAL_CONTACT_SHADOW_VISIBILITY_THRESHOLD;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0
{
    if (RayDirectionalSelected() && uRayShadowRead.options.y != 0u) return 0.0.xxxx;
    uint width = 0u;
    uint height = 0u;
    gDepthTexture.GetDimensions(width, height);
    uint2 pixelCoord = uint2(input.pos.xy);
    pixelCoord = min(pixelCoord, uint2(max(width, 1u) - 1u, max(height, 1u) - 1u));

    float depth = gDepthTexture.Load(int3(pixelCoord, 0));
    if (!isfinite(depth) || depth <= 0.0 || depth > 1.0)
    {
        return 0.0.xxxx;
    }

    // Look up the screen/depth cluster for this pixel.
    uint tileCountX = max(pc.tileCountX, 1u);
    uint tileCountY = max(pc.tileCountY, 1u);
    uint2 tileCoord = min(pixelCoord / TILE_SIZE,
                          uint2(tileCountX - 1u, tileCountY - 1u));
    uint safeDepthSliceCount = max(pc.depthSliceCount, 1u);
    uint sliceZ = DepthToClusterSlice(depth, pc.cameraNear, pc.cameraFar,
                                      safeDepthSliceCount);
    uint clusterIndex =
        (sliceZ * tileCountY + tileCoord.y) * tileCountX + tileCoord.x;
    TileLightGrid tile = uTileGrid[clusterIndex];
    uint lightCount = min(tile.count, MAX_LIGHTS_PER_TILE);
    if (lightCount == 0u)
    {
        return 0.0.xxxx;
    }

    float4 albedo = gAlbedoTexture.Load(int3(pixelCoord, 0));
    if (albedo.a <= 0.0)
    {
        return 0.0.xxxx;
    }

    float3 normal = SafeNormalize(gNormalTexture.Load(int3(pixelCoord, 0)).xyz * 2.0 - 1.0,
                                  float3(0.0, 1.0, 0.0));
    float4 material = gMaterialTexture.Load(int3(pixelCoord, 0));
    float4 specularSample = gSpecularTexture.Load(int3(pixelCoord, 0));
    uint materialIndex = DecodeGBufferMaterialIndex(material.w);
    GpuMaterial materialData = uMaterials[materialIndex];
    DeferredMaterialLayerTerms layerTerms =
        DecodeDeferredMaterialLayerTerms(materialData,
            gMaterialLayersTexture.Load(int3(pixelCoord, 0)),
            gMaterialSheenTexture.Load(int3(pixelCoord, 0)));

    float2 uv = (float2(pixelCoord) + 0.5.xx) / float2(max(width, 1u), max(height, 1u));
    float3 worldPosition;
    if (!TryReconstructWorldPosition(uv, depth, uCamera.inverseViewProj,
                                     worldPosition))
    {
        return 0.0.xxxx;
    }
    float3 geometricNormal = ReconstructDepthGeometricNormal(
        pixelCoord, uint2(width, height), worldPosition, normal,
        gDepthTexture, uCamera.inverseViewProj);

    float metallic = saturate(material.x);
    float roughness = clamp(material.y, 0.045, 1.0);
    bool thinSurface = IsThinSurfaceGpuMaterial(materialData.flags);
    float3 V = SafeNormalize(uCamera.cameraWorldPosition.xyz - worldPosition, float3(0.0, 0.0, 1.0));
    float NdotV = max(dot(normal, V), 0.0);
    float iridescenceThickness = layerTerms.iridescenceThickness;
    float3 F0 = EvaluateDeferredPbrF0FromDielectricColor(
        albedo.rgb, metallic, specularSample.rgb,
        layerTerms.iridescenceFactor, materialData.iridescenceIor,
        iridescenceThickness, NdotV);
    float transmissionWeight =
        saturate(layerTerms.transmissionFactor * (1.0 - metallic));

    float3 totalLighting = 0.0.xxx;

    for (uint i = 0; i < lightCount; ++i)
    {
        uint lightIdx = uLightIndexList[tile.offset + i];
        PointLightData light = uLights[lightIdx];
        if (!PointLightHasRenderableIntensity(light))
        {
            continue;
        }

        float3 lightVector = light.positionRadius.xyz - worldPosition;
        float lightRadius = light.positionRadius.w;
        float distanceSq = dot(lightVector, lightVector);
        if (PointLightHasFiniteRange(lightRadius) &&
            distanceSq >= lightRadius * lightRadius)
        {
            continue;
        }

        float invDistance = rsqrt(max(distanceSq, 1e-8));
        float3 L = lightVector * invDistance;
        float attenuation = PointLightAttenuation(distanceSq, lightRadius);
        float coneAttenuation = SpotLightConeAttenuation(
            light.directionInnerCos, light.coneOuterCosType, L);
        float3 radiance =
            light.colorIntensity.rgb * light.colorIntensity.a * attenuation *
            coneAttenuation;
        float3 unshadowedRadiance = radiance;
        float lightVisibility = 1.0;
        bool useLocalShadowMap =
            LocalShadowMapsEnabled(pc.localShadowEnabled, uLocalShadow) &&
            PointLightHasLocalShadowLayers(light, uLocalShadow);
        if (!RayPointSelected(lightIdx) && useLocalShadowMap)
        {
            lightVisibility *= LocalPointLightShadowVisibility(
                light, worldPosition, geometricNormal, L, uLocalShadow,
                gLocalShadowAtlas, gLocalShadowSampler);
        }
        if (!RayPointSelected(lightIdx) && pc.contactVisibilityEnabled != 0u &&
            ShouldTracePointLightContactShadow(radiance, lightVisibility))
        {
            lightVisibility *= ScreenSpacePointLightContactVisibility(
                worldPosition, geometricNormal, light.positionRadius.xyz, gDepthTexture,
                gNormalTexture, uCamera.viewProj, uCamera.inverseViewProj);
        }
        if (RayPointSelected(lightIdx)) lightVisibility = RayShadowVisibility(input.pos, 1u + lightIdx);
        radiance *= lightVisibility;
        totalLighting += EvaluateDeferredLayeredDirectLight(
            albedo.rgb, F0, metallic, roughness, normal, V, L, radiance,
            layerTerms.clearcoatFactor, layerTerms.clearcoatRoughness,
            layerTerms.sheenColor, layerTerms.sheenRoughness);
        totalLighting += EvaluateDiffuseBounceLight(
            albedo.rgb, metallic, normal, geometricNormal, V, L, unshadowedRadiance,
            lightVisibility, saturate(material.z), pc.bounceIntensity);
        if (thinSurface || transmissionWeight > 0.0)
        {
            float transmissionAlpha =
                thinSurface ? lerp(albedo.a, 0.0, transmissionWeight) : 0.0;
            float transmissionScale = thinSurface ? 1.0 : transmissionWeight;
            totalLighting += EvaluateThinSurfaceTransmission(
                albedo.rgb, normal, V, L, radiance, transmissionAlpha, false) *
                transmissionScale;
        }
    }

    return float4(totalLighting, 1.0);
}
