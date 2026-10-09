#include "brdf_common.slang"
#include "depth_normal_common.slang"
#include "area_light_common.slang"
#include "area_light_ltc.slang"
#include "material_data_common.slang"
#include "local_shadow_common.slang"
#include "screen_space_light_shadow_common.slang"
#include "shadow_common.slang"
#include "environment_sky_common.slang"
#include "ray_shadow_read.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(2, 0)]] Texture2D<float4> gAlbedoTexture;
[[vk::binding(3, 0)]] Texture2D<float4> gNormalTexture;
[[vk::binding(4, 0)]] Texture2D<float4> gMaterialTexture;
[[vk::binding(5, 0)]] Texture2D<float4> gEmissiveTexture;
[[vk::binding(6, 0)]] Texture2D<float> gDepthTexture;
[[vk::binding(7, 0)]] ConstantBuffer<ShadowBuffer> uShadow;
[[vk::binding(8, 0)]] Texture2DArray<float> gShadowAtlas;
[[vk::binding(9, 0)]] SamplerComparisonState gShadowSampler;
[[vk::binding(10, 0)]] TextureCube<float4> gIrradianceCubemap;
[[vk::binding(11, 0)]] TextureCube<float4> gPrefilteredCubemap;
[[vk::binding(12, 0)]] Texture2D<float2> gBrdfLUT;
[[vk::binding(13, 0)]] SamplerState envSampler;
[[vk::binding(14, 0)]] SamplerState brdfSampler;
[[vk::binding(15, 0)]] Texture2D<float> gAOTexture;
[[vk::binding(16, 0)]] SamplerState aoSampler;
[[vk::binding(17, 0)]] Texture2D<float4> gSpecularTexture;
[[vk::binding(18, 0)]] ConstantBuffer<LocalShadowBuffer> uLocalShadow;
[[vk::binding(19, 0)]] Texture2DArray<float> gLocalShadowAtlas;
[[vk::binding(20, 0)]] SamplerComparisonState gLocalShadowSampler;
[[vk::binding(21, 0)]] Texture2D<float4> gMaterialLayersTexture;
[[vk::binding(22, 0)]] Texture2D<float4> gMaterialSheenTexture;
[[vk::binding(0, 1)]] ConstantBuffer<LightingBuffer> uLighting;
[[vk::binding(2, 1)]] StructuredBuffer<AreaLightData> uAreaLights;
[[vk::binding(3, 2)]] StructuredBuffer<GpuMaterial> uMaterials;

float DeferredFiniteOr(float value, float fallback)
{
    return isfinite(value) ? value : fallback;
}

float2 DeferredFinite2Or(float2 value, float2 fallback)
{
    return all(isfinite(value)) ? value : fallback;
}

float3 DeferredFinite3Or(float3 value, float3 fallback)
{
    return all(isfinite(value)) ? value : fallback;
}

static const float kDiffuseAmbientFloor = 0.04;
static const float DIRECTIONAL_CONTACT_SHADOW_LIT_THRESHOLD = 0.02;
static const float DIRECTIONAL_CONTACT_SHADOW_RADIANCE_THRESHOLD = 1.0e-4;

float3 ComputeDiffuseAmbientFloor(float3 albedo,
                                  float occlusion,
                                  float ssao,
                                  float environmentIntensity,
                                  float bounceIntensity)
{
    float environmentFloorScale =
        saturate(max(DeferredFiniteOr(environmentIntensity, 0.0), 0.0));
    float bounceFloorScale =
        saturate(max(DeferredFiniteOr(bounceIntensity, 0.0), 0.0));
    float ambientFloorScale = max(environmentFloorScale, bounceFloorScale);
    float diffuseOcclusion =
        saturate(DeferredFiniteOr(occlusion, 1.0)) *
        lerp(1.0, saturate(DeferredFiniteOr(ssao, 1.0)), 0.65);
    return max(DeferredFinite3Or(albedo, 0.0.xxx), 0.0.xxx) *
        (kDiffuseAmbientFloor * ambientFloorScale * diffuseOcclusion);
}

bool DeferredValidSceneDepth(float depth)
{
    return isfinite(depth) && depth > 0.0 && depth <= 1.0;
}

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

float3 SampleBackgroundSky(float2 uv, float environmentIntensity)
{
    return SampleEnvironmentBackground(uv, uCamera.inverseViewProj,
        environmentIntensity, gPrefilteredCubemap, envSampler);
}

float3 EvaluateDeferredAreaLight(uint areaIndex, AreaLightData light,
                                 float3 albedo,
                                 float3 F0, float metallic, float roughness,
                                 float3 geometricNormal, float3 normal, float3 V,
                                 float3 worldPosition,
                                 DeferredMaterialLayerTerms layerTerms,
                                 bool thinSurface, float alpha,
                                 float occlusion, float bounceIntensity, float4 pixelPosition)
{
    if (!AreaLightHasRenderableRadiance(light)) return 0.0.xxx;
    AreaLightFrame frame = BuildAreaLightFrame(light);
    float3 directLighting = 0.0.xxx;
    float areaShadowVisibility = 1.0;
    uint areaShadowRef = PackedLocalShadowAreaRef(uLocalShadow, areaIndex);
    if (!RayAreaSelected(areaIndex) && LocalShadowMapsEnabled(uLighting, uLocalShadow) &&
        areaShadowRef != 0u)
    {
        // Integrate visibility from the emitter origins rendered into cube maps.
        // The budget determines how many emitter samples are available.
        areaShadowVisibility = 0.0;
        float visibilityWeightSum = 0.0;
        uint visibilitySampleCount =
            LocalAreaShadowSampleCount(areaShadowRef, uLocalShadow);
        [loop]
        for (uint visibilitySample = 0u;
             visibilitySample < visibilitySampleCount;
             ++visibilitySample)
        {
            float3 samplePosition = LocalAreaShadowSamplePosition(
                    areaShadowRef, visibilitySample, uLocalShadow);
            float3 visibilitySurfaceToLight;
            float weight = AreaLightPositionContributionWeight(
                light, frame, worldPosition, samplePosition,
                1.0 / float(visibilitySampleCount),
                visibilitySurfaceToLight);
            if (weight <= 0.0)
            {
                continue;
            }
            areaShadowVisibility += weight * LocalAreaLightShadowVisibility(
                areaIndex, light, worldPosition, geometricNormal, visibilitySample,
                uLocalShadow, gLocalShadowAtlas, gLocalShadowSampler);
            visibilityWeightSum += weight;
        }
        areaShadowVisibility =
            visibilityWeightSum > 1.0e-4
                ? areaShadowVisibility / visibilityWeightSum
                : 1.0;
    }

    if (RayAreaSelected(areaIndex))
        areaShadowVisibility = RayShadowVisibility(pixelPosition, 5u + areaIndex);
    float transmissionWeight =
        saturate(DeferredFiniteOr(layerTerms.transmissionFactor, 0.0) *
                 (1.0 - metallic));

    AreaLightLtcTerms ltcTerms;
    bool useLtc = TryEvaluateAreaLightLtc(uLighting, light, frame,
        worldPosition, albedo, F0, metallic, roughness, normal, V,
        layerTerms.clearcoatFactor, layerTerms.clearcoatRoughness,
        layerTerms.sheenColor, layerTerms.sheenRoughness,
        thinSurface || transmissionWeight > 0.0, ltcTerms);
    bool correctDiffuseFresnel = useLtc && any(ltcTerms.diffuseCorrectionScale > 0.0.xxx);
    bool sampleDirectTerms = useLtc &&
        (ltcTerms.stableWeight < 1.0 || ltcTerms.fresnelWeight < 1.0);
    if (useLtc && !sampleDirectTerms && bounceIntensity <= 0.0 && !correctDiffuseFresnel)
    {
        directLighting += (ltcTerms.lighting.stable + ltcTerms.lighting.fresnel) *
            areaShadowVisibility;
        return directLighting;
    }

    uint integrationSampleCount = AreaLightIntegrationSampleCount(uLighting);
    float3 diffuseCorrection = 0.0.xxx;
    AreaLightDirectTerms sampledTerms;
    sampledTerms.stable = 0.0.xxx;
    sampledTerms.fresnel = 0.0.xxx;
    [loop]
    for (uint sampleIndex = 0u;
         sampleIndex < integrationSampleCount; ++sampleIndex)
    {
        float3 L;
        float3 radiance = EvaluateAreaLightSampleRadiance(
            light, frame, worldPosition, sampleIndex, integrationSampleCount, L);
        float lightVisibility = areaShadowVisibility;
        float3 directRadiance = radiance * lightVisibility;
        if (!useLtc)
            directLighting += EvaluateDeferredLayeredDirectLight(
                albedo, F0, metallic, roughness, normal, V, L, directRadiance,
                layerTerms.clearcoatFactor, layerTerms.clearcoatRoughness,
                layerTerms.sheenColor, layerTerms.sheenRoughness);
        else if (sampleDirectTerms)
        {
            AreaLightDirectTerms sample = EvaluateAreaLightSampledDirectTerms(
                albedo, F0, metallic, roughness, normal, V, L, directRadiance,
                layerTerms.clearcoatFactor, layerTerms.clearcoatRoughness,
                layerTerms.sheenColor, layerTerms.sheenRoughness);
            sampledTerms.stable += sample.stable;
            sampledTerms.fresnel += sample.fresnel;
        }
        if (bounceIntensity > 0.0)
            directLighting += EvaluateDiffuseBounceLight(
                albedo, metallic, normal, geometricNormal, V, L, radiance,
                lightVisibility, occlusion, bounceIntensity);
        if (correctDiffuseFresnel)
            diffuseCorrection += EvaluateAreaLightDiffuseFresnelCorrection(
                ltcTerms.diffuseCorrectionScale, normal, V, L, directRadiance);

        if (thinSurface || transmissionWeight > 0.0)
        {
            float transmissionAlpha =
                thinSurface ? lerp(alpha, 0.0, transmissionWeight) : 0.0;
            float transmissionScale = thinSurface ? 1.0 : transmissionWeight;
            directLighting += EvaluateThinSurfaceTransmission(
                albedo, normal, V, L, directRadiance, transmissionAlpha, false) *
                transmissionScale;
        }
    }

    if (useLtc)
    {
        float3 ltcContribution;
        if (!TryCombineAreaLightLtcTerms(ltcTerms, areaShadowVisibility,
                diffuseCorrection, sampledTerms, ltcContribution))
        {
            // A coarse quadrature correction must not remove more energy
            // than the analytic diffuse term. Recover the sampled BRDF.
            ltcContribution = sampleDirectTerms ?
                sampledTerms.stable + sampledTerms.fresnel :
                EvaluateAreaLightSampledLayeredDirect(light,
                    frame, worldPosition, integrationSampleCount, albedo, F0,
                    metallic, roughness, normal, V, layerTerms.clearcoatFactor,
                    layerTerms.clearcoatRoughness, layerTerms.sheenColor,
                    layerTerms.sheenRoughness) * areaShadowVisibility;
        }
        directLighting += max(Finite3Or(ltcContribution, 0.0.xxx), 0.0.xxx);
    }

    return directLighting;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0
{
    uint width = 0u;
    uint height = 0u;
    gDepthTexture.GetDimensions(width, height);
    // Use integer pixel loads for the G-buffer to avoid filtering material IDs,
    // normal encodings, and depth across geometry edges.
    uint2 pixelCoord = uint2(input.pos.xy);
    pixelCoord = min(pixelCoord, uint2(max(width, 1u) - 1u, max(height, 1u) - 1u));
    float2 uv = (float2(pixelCoord) + 0.5.xx) / float2(max(width, 1u), max(height, 1u));

    float depth = gDepthTexture.Load(int3(pixelCoord, 0));
    float environmentIntensity =
        max(DeferredFiniteOr(uLighting.environmentIntensity, 0.0), 0.0);
    float bounceIntensity =
        max(DeferredFiniteOr(uLighting.bounceIntensity, 0.0), 0.0);
    float3 emissiveTexel =
        DeferredFinite3Or(gEmissiveTexture.Load(int3(pixelCoord, 0)).rgb,
                          0.0.xxx);

    if (!DeferredValidSceneDepth(depth))
    {
        float3 skyColor = SampleBackgroundSky(uv, environmentIntensity);
        return float4(skyColor + emissiveTexel, 1.0);
    }

    float4 albedo = gAlbedoTexture.Load(int3(pixelCoord, 0));
    if (RayDirectionalSelected() && uRayShadowRead.options.y != 0u)
        return float4(RayShadowDebug(input.pos), 1.0);
    albedo.rgb = max(DeferredFinite3Or(albedo.rgb, 0.0.xxx), 0.0.xxx);
    albedo.a = saturate(DeferredFiniteOr(albedo.a, 0.0));
    if (albedo.a <= 0.0)
    {
        float3 skyColor = SampleBackgroundSky(uv, environmentIntensity);
        return float4(skyColor + emissiveTexel, 1.0);
    }

    float3 normal = SafeNormalize(gNormalTexture.Load(int3(pixelCoord, 0)).xyz * 2.0 - 1.0,
                                  float3(0.0, 1.0, 0.0));
    float4 material = gMaterialTexture.Load(int3(pixelCoord, 0));
    float4 emissiveSample = gEmissiveTexture.Load(int3(pixelCoord, 0));
    float4 specularSample = gSpecularTexture.Load(int3(pixelCoord, 0));
    material.xyz = DeferredFinite3Or(material.xyz, float3(0.0, 1.0, 1.0));
    material.w = DeferredFiniteOr(material.w, 0.0);
    specularSample.rgb = DeferredFinite3Or(specularSample.rgb, 0.04.xxx);
    float3 emissive = DeferredFinite3Or(emissiveSample.rgb, emissiveTexel);
    uint materialIndex = DecodeGBufferMaterialIndex(material.w);
    GpuMaterial materialData = uMaterials[materialIndex];
    DeferredMaterialLayerTerms layerTerms =
        DecodeDeferredMaterialLayerTerms(materialData,
            gMaterialLayersTexture.Load(int3(pixelCoord, 0)),
            gMaterialSheenTexture.Load(int3(pixelCoord, 0)));
    layerTerms.transmissionFactor =
        saturate(DeferredFiniteOr(layerTerms.transmissionFactor, 0.0));
    layerTerms.clearcoatFactor =
        saturate(DeferredFiniteOr(layerTerms.clearcoatFactor, 0.0));
    layerTerms.clearcoatRoughness =
        clamp(DeferredFiniteOr(layerTerms.clearcoatRoughness, 1.0),
              0.045, 1.0);
    layerTerms.sheenColor =
        max(DeferredFinite3Or(layerTerms.sheenColor, 0.0.xxx), 0.0.xxx);
    layerTerms.sheenRoughness =
        clamp(DeferredFiniteOr(layerTerms.sheenRoughness, 1.0), 0.045, 1.0);
    layerTerms.iridescenceFactor =
        saturate(DeferredFiniteOr(layerTerms.iridescenceFactor, 0.0));

    float metallic = saturate(DeferredFiniteOr(material.x, 0.0));
    float roughness = clamp(DeferredFiniteOr(material.y, 1.0), 0.045, 1.0);
    float occlusion = saturate(DeferredFiniteOr(material.z, 1.0));
    bool thinSurface = IsThinSurfaceGpuMaterial(materialData.flags);
    float3 worldPosition;
    if (!TryReconstructWorldPosition(uv, depth, uCamera.inverseViewProj,
                                     worldPosition))
    {
        return float4(emissive, 1.0);
    }
    float3 geometricNormal = ReconstructDepthGeometricNormal(
        pixelCoord, uint2(width, height), worldPosition, normal,
        gDepthTexture, uCamera.inverseViewProj);
    float3 V = SafeNormalize(uCamera.cameraWorldPosition.xyz - worldPosition, float3(0.0, 0.0, 1.0));
    float3 L = SafeNormalize(-uLighting.directionalDirection.xyz, float3(0.0, 1.0, 0.0));
    float3 lightColor =
        max(DeferredFinite3Or(uLighting.directionalColorIntensity.rgb, 0.0.xxx),
            0.0.xxx) *
        max(DeferredFiniteOr(uLighting.directionalColorIntensity.a, 0.0), 0.0);
    float directionalRadiance = max(max(lightColor.r, lightColor.g), lightColor.b);
    float NdotV = max(dot(normal, V), 0.0);

    // Positive view-axis depth selects the cascade. Camera forward is uploaded
    // once per frame; reconstructing it here would cost two matrix transforms
    // for every lit pixel.
    float3 cameraForward =
        SafeNormalize(uCamera.cameraForward.xyz, float3(0.0, 0.0, -1.0));
    float viewDepth =
        max(dot(worldPosition - uCamera.cameraWorldPosition.xyz, cameraForward), 0.0);

    // Shadow evaluation.
    float shadowFactor = 1.0;
    float contactVisibility = 1.0;
    float NdotLForShadow = dot(geometricNormal, L);
    if (!RayDirectionalSelected() && uLighting.shadowEnabled != 0u &&
        (NdotLForShadow > 0.0 || thinSurface))
    {
        shadowFactor = ComputeShadowFactor(
            worldPosition, geometricNormal, uLighting.directionalDirection.xyz,
            viewDepth, uShadow, gShadowAtlas, gShadowSampler);
        if (shadowFactor > DIRECTIONAL_CONTACT_SHADOW_LIT_THRESHOLD &&
            directionalRadiance > DIRECTIONAL_CONTACT_SHADOW_RADIANCE_THRESHOLD &&
            ShadowFiniteOr(uShadow.contactShadowSettings.x, 0.0) > 0.0)
        {
            contactVisibility = ScreenSpaceDirectionalContactVisibility(
                worldPosition, geometricNormal, L,
                uShadow.contactShadowSettings.y,
                uShadow.contactShadowSettings.z,
                uShadow.contactShadowSettings.w,
                gDepthTexture, gNormalTexture,
                uCamera.viewProj, uCamera.inverseViewProj);
        }
    }

    float iridescenceThickness = layerTerms.iridescenceThickness;
    float3 F0 = EvaluateDeferredPbrF0FromDielectricColor(
        albedo.rgb, metallic, specularSample.rgb,
        layerTerms.iridescenceFactor, materialData.iridescenceIor,
        iridescenceThickness, NdotV);
    if (RayDirectionalSelected()) shadowFactor = RayShadowVisibility(input.pos, 0u);
    float directShadowVisibility = shadowFactor * contactVisibility;
    float3 directLighting = EvaluateDeferredLayeredDirectLight(
        albedo.rgb, F0, metallic, roughness, normal, V, L, lightColor,
        layerTerms.clearcoatFactor, layerTerms.clearcoatRoughness,
        layerTerms.sheenColor, layerTerms.sheenRoughness) * directShadowVisibility;
    directLighting += EvaluateDiffuseBounceLight(
        albedo.rgb, metallic, normal, geometricNormal, V, L, lightColor,
        shadowFactor, occlusion, bounceIntensity);

    float transmissionWeight =
        saturate(DeferredFiniteOr(layerTerms.transmissionFactor, 0.0) *
                 (1.0 - metallic));
    if (thinSurface || transmissionWeight > 0.0)
    {
        float transmissionAlpha =
            thinSurface ? lerp(albedo.a, 0.0, transmissionWeight) : 0.0;
        float transmissionScale = thinSurface ? 1.0 : transmissionWeight;
        directLighting += EvaluateThinSurfaceTransmission(
            albedo.rgb, normal, V, L, lightColor, transmissionAlpha, false) *
            transmissionScale * directShadowVisibility;
    }

    uint areaLightCount = min(uLighting.areaLightCount, MAX_AREA_LIGHTS);
    for (uint areaIndex = 0u; areaIndex < areaLightCount; ++areaIndex)
    {
        directLighting += EvaluateDeferredAreaLight(
            areaIndex, uAreaLights[areaIndex], albedo.rgb, F0, metallic, roughness,
            geometricNormal, normal, V, worldPosition, layerTerms, thinSurface,
            albedo.a, occlusion, bounceIntensity, input.pos);
    }

    // IBL ambient lighting (split-sum approximation).
    float3 kS_ibl = FresnelSchlickRoughness(NdotV, F0, roughness);
    float3 kD_ibl = (1.0 - kS_ibl) * (1.0 - metallic);

    float3 irradiance =
        DeferredFinite3Or(gIrradianceCubemap.Sample(envSampler, normal).rgb,
                          0.0.xxx) *
        environmentIntensity;
    float3 diffuseIBL = irradiance * albedo.rgb;

    float3 R = reflect(-V, normal);
    float maxMipLevel = max(float(uLighting.prefilteredMipCount) - 1.0, 0.0);
    float3 prefilteredColor =
        DeferredFinite3Or(
            gPrefilteredCubemap.SampleLevel(envSampler, R,
                                            roughness * maxMipLevel).rgb,
            0.0.xxx) *
        environmentIntensity;
    float2 brdf = max(
        DeferredFinite2Or(gBrdfLUT.Sample(brdfSampler, float2(NdotV, roughness)),
                          0.0.xx),
        0.0.xx);
    float3 specularIBL =
        EvaluateSpecularIblMultipleScattering(prefilteredColor, F0, brdf);

    // Screen-space AO (GTAO).
    float ssao = 1.0;
    if (uLighting.gtaoEnabled != 0u)
    {
        ssao = saturate(DeferredFiniteOr(gAOTexture.Sample(aoSampler, uv), 1.0));
    }

    float diffuseAO = occlusion * ssao;
    float specularOcclusion = saturate(pow(NdotV + diffuseAO, exp2(-16.0 * roughness - 1.0)) - 1.0 + diffuseAO);
    specularOcclusion = lerp(1.0, specularOcclusion, saturate(1.0 - roughness));
    float3 ambientLighting = kD_ibl * diffuseIBL * diffuseAO + specularIBL * specularOcclusion;
    ambientLighting += ComputeDiffuseAmbientFloor(
        albedo.rgb, occlusion, ssao, environmentIntensity, bounceIntensity);
    // Layer the environment response exactly once, using the geometry pass's
    // sampled factors. Direct lighting already evaluates these layers above.
    if (layerTerms.clearcoatFactor > 0.0)
    {
        float clearcoatRoughness = layerTerms.clearcoatRoughness;
        float3 clearcoatPrefiltered = DeferredFinite3Or(
            gPrefilteredCubemap.SampleLevel(envSampler, R,
                clearcoatRoughness * maxMipLevel).rgb, 0.0.xxx) * environmentIntensity;
        float2 clearcoatBrdf = max(DeferredFinite2Or(
            gBrdfLUT.Sample(brdfSampler, float2(NdotV, clearcoatRoughness)),
            0.0.xx), 0.0.xx);
        float3 clearcoatIbl = EvaluateSpecularIblMultipleScattering(
            clearcoatPrefiltered, IorToF0(1.5).xxx, clearcoatBrdf);
        ambientLighting = ambientLighting *
            DeferredClearcoatBaseEnergyScale(layerTerms.clearcoatFactor) +
            clearcoatIbl * layerTerms.clearcoatFactor;
    }
    if (MaxComponent3(layerTerms.sheenColor) > 0.0)
    {
        float sheenScale = pow(1.0 - NdotV, 2.0) *
            (1.0 - 0.35 * layerTerms.sheenRoughness);
        ambientLighting = ambientLighting *
            DeferredSheenBaseEnergyScale(layerTerms.sheenColor) +
            layerTerms.sheenColor * irradiance * sheenScale * 0.35;
    }
    float3 finalColor =
        DeferredFinite3Or(ambientLighting + directLighting + emissive, 0.0.xxx);

    return float4(finalColor, 1.0);
}
