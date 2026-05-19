#include "brdf_common.slang"
#include "area_light_common.slang"
#include "local_shadow_common.slang"
#include "shadow_common.slang"
#include "oit_common.slang"
#include "push_constants_common.slang"

#include "object_data_common.slang"
#include "material_data_common.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer> uObjects;
[[vk::binding(2, 0)]] SamplerState materialSamplers[MATERIAL_SAMPLER_DESCRIPTOR_COUNT];
[[vk::binding(3, 0)]] StructuredBuffer<GpuMaterial> uMaterials;
[[vk::binding(4, 0)]] StructuredBuffer<GpuTextureMetadata> uTextureMetadata;
[[vk::binding(5, 0)]] Texture2D materialTextures[MATERIAL_TEXTURE_DESCRIPTOR_COUNT];
[[vk::binding(0, 1)]] ConstantBuffer<LightingBuffer> uLighting;
[[vk::binding(1, 1)]] StructuredBuffer<PointLightData> uPointLights;
[[vk::binding(2, 1)]] StructuredBuffer<AreaLightData> uAreaLights;
[[vk::binding(0, 2)]] RWTexture2D<uint> headPointerImage;
[[vk::binding(1, 2)]] RWStructuredBuffer<TransparentNode> transparentNodes;
[[vk::binding(2, 2)]] RWStructuredBuffer<uint> nodeCounter;
[[vk::binding(3, 2)]] ConstantBuffer<OitMetadataBuffer> uOit;
[[vk::binding(7, 3)]] ConstantBuffer<ShadowBuffer> uShadow;
[[vk::binding(8, 3)]] Texture2DArray<float> gShadowAtlas;
[[vk::binding(9, 3)]] SamplerComparisonState gShadowSampler;
[[vk::binding(10, 3)]] TextureCube<float4> gIrradianceCubemap;
[[vk::binding(11, 3)]] TextureCube<float4> gPrefilteredCubemap;
[[vk::binding(12, 3)]] Texture2D<float2> gBrdfLUT;
[[vk::binding(13, 3)]] SamplerState envSampler;
[[vk::binding(14, 3)]] SamplerState brdfSampler;
[[vk::binding(18, 3)]] ConstantBuffer<LocalShadowBuffer> uLocalShadow;
[[vk::binding(19, 3)]] Texture2DArray<float> gLocalShadowAtlas;
[[vk::binding(20, 3)]] SamplerComparisonState gLocalShadowSampler;
[[vk::binding(3, 3)]] Texture2D<float4> gSceneNormalTexture;
[[vk::binding(6, 3)]] Texture2D<float> gSceneDepthTexture;

#include "scene_clip_common.slang"
#include "object_index_common.slang"
#include "pbr_material_common.slang"
#include "surface_normal_common.slang"

[[vk::push_constant]]
ConstantBuffer<BindlessPushConstants> pc;

static const uint kObjectFlagAlphaCutoff = 0x1u;
static const uint kObjectFlagTransparentOnly = 0x2u;
static const uint kObjectFlagDoubleSided = 0x4u;

float TransparentFiniteOr(float value, float fallback)
{
    return isfinite(value) ? value : fallback;
}

float2 TransparentFinite2Or(float2 value, float2 fallback)
{
    return all(isfinite(value)) ? value : fallback;
}

float3 TransparentFinite3Or(float3 value, float3 fallback)
{
    return all(isfinite(value)) ? value : fallback;
}

static const float kTransparentAmbientFloor = 0.025;

float3 ComputeTransparentAmbientFloor(float3 baseColor,
                                      float occlusion,
                                      float environmentIntensity)
{
    float disabledEnvironment =
        saturate(1.0 - max(TransparentFiniteOr(environmentIntensity, 0.0), 0.0));
    float diffuseOcclusion = saturate(TransparentFiniteOr(occlusion, 1.0));
    return max(TransparentFinite3Or(baseColor, 0.0.xxx), 0.0.xxx) *
        (kTransparentAmbientFloor * disabledEnvironment * diffuseOcclusion);
}

struct VSInput
{
    [[vk::location(0)]] float3 inPosition;
    [[vk::location(1)]] float3 inColor;
    [[vk::location(2)]] float2 inTexCoord;
    [[vk::location(3)]] float3 inNormal;
    [[vk::location(4)]] float4 inTangent;
    [[vk::location(5)]] float2 inTexCoord1;
};

struct VSOutput
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float4 baseColor;
    [[vk::location(1)]] float2 texCoord;
    [[vk::location(2)]] uint baseColorTextureIndex;
    [[vk::location(3)]] uint normalTextureIndex;
    [[vk::location(4)]] uint occlusionTextureIndex;
    [[vk::location(5)]] uint emissiveTextureIndex;
    [[vk::location(6)]] uint metallicRoughnessTextureIndex;
    [[vk::location(7)]] float3 emissiveColor;
    [[vk::location(8)]] float emissiveStrength;
    [[vk::location(9)]] float2 metallicRoughness;
    [[vk::location(10)]] float3 worldPos;
    [[vk::location(11)]] float alphaCutoff;
    [[vk::location(12)]] float normalTextureScale;
    [[vk::location(13)]] float occlusionStrength;
    [[vk::location(14)]] uint flags;
    [[vk::location(15)]] float3 worldNormal;
    [[vk::location(16)]] float4 worldTangent;
    [[vk::location(17)]] uint roughnessTextureIndex;
    [[vk::location(18)]] uint metalnessTextureIndex;
    [[vk::location(19)]] uint specularTextureIndex;
    [[vk::location(20)]] uint opacityTextureIndex;
    [[vk::location(21)]] uint transmissionTextureIndex;
    [[vk::location(22)]] float opacityFactor;
    [[vk::location(23)]] float specularFactor;
    [[vk::location(24)]] float transmissionFactor;
    [[vk::location(25)]] uint objectIndex;
    [[vk::location(26)]] float2 texCoord1;
};

[shader("vertex")]
VSOutput vertMain(VSInput input, uint instanceID : SV_InstanceID,
                  uint startInstance : SV_StartInstanceLocation)
{
    VSOutput output;
    uint objectIndex = ResolveObjectIndex(pc.objectIndex, instanceID, startInstance);
    ObjectBuffer obj = uObjects[objectIndex];
    GpuMaterial material = uMaterials[obj.objectInfo.x];
    float2 heightTexCoord = ApplyGpuTextureTransform(
        input.inTexCoord, input.inTexCoord1, material.heightTextureTransform);

    float3 objectPosition = ApplyHeightDisplacement(
        input.inPosition, input.inNormal, heightTexCoord,
        material.heightTextureIndex, material.heightTextureTransform,
        material.heightScale,
        material.heightOffset);
    float4 worldPos = mul(obj.model, float4(objectPosition, 1.0));
    float3x3 modelMatrix = (float3x3)obj.model;
    float3 normalVector =
        obj.normalMatrix0.xyz * input.inNormal.x +
        obj.normalMatrix1.xyz * input.inNormal.y +
        obj.normalMatrix2.xyz * input.inNormal.z;
    float3 worldNormal = SafeNormalize(normalVector, float3(0.0, 1.0, 0.0));
    float4 worldTangent = BuildWorldTangent(modelMatrix, worldNormal, input.inTangent);

    output.pos = mul(uCamera.viewProj, worldPos);
    output.baseColor = material.color * float4(input.inColor, 1.0);
    output.texCoord = input.inTexCoord;
    output.texCoord1 = input.inTexCoord1;
    output.baseColorTextureIndex = material.baseColorTextureIndex;
    output.normalTextureIndex = material.normalTextureIndex;
    output.occlusionTextureIndex = material.occlusionTextureIndex;
    output.emissiveTextureIndex = material.emissiveTextureIndex;
    output.metallicRoughnessTextureIndex =
        material.metallicRoughnessTextureIndex;
    output.emissiveColor = material.emissiveColor;
    output.emissiveStrength = material.emissiveStrength;
    output.metallicRoughness = material.metallicRoughness;
    output.worldPos = worldPos.xyz;
    output.alphaCutoff = material.alphaCutoff;
    output.normalTextureScale = material.normalTextureScale;
    output.occlusionStrength = material.occlusionStrength;
    output.flags = material.flags | obj.objectInfo.y;
    output.worldNormal = worldNormal;
    output.worldTangent = worldTangent;
    output.roughnessTextureIndex = material.roughnessTextureIndex;
    output.metalnessTextureIndex = material.metalnessTextureIndex;
    output.specularTextureIndex = material.specularTextureIndex;
    output.opacityTextureIndex = material.opacityTextureIndex;
    output.transmissionTextureIndex = material.transmissionTextureIndex;
    output.opacityFactor = material.opacityFactor;
    output.specularFactor = material.specularFactor;
    output.transmissionFactor = material.transmissionFactor;
    output.objectIndex = objectIndex;
    return output;
}

[earlydepthstencil]
[shader("fragment")]
float4 fragMain(VSOutput vertIn, bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
    if (SceneClipPlanesClip(vertIn.worldPos, pc.sectionPlaneEnabled, pc.sectionPlane))
    {
        discard;
    }

    GpuMaterial material =
        uMaterials[uObjects[vertIn.objectIndex].objectInfo.x];
    float2 baseColorTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.baseColorTextureTransform);
    float2 normalTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.normalTextureTransform);
    float2 occlusionTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.occlusionTextureTransform);
    float2 emissiveTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.emissiveTextureTransform);
    float2 metallicRoughnessTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.metallicRoughnessTextureTransform);
    float2 roughnessTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.roughnessTextureTransform);
    float2 metalnessTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.metalnessTextureTransform);
    float2 specularTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.specularTextureTransform);
    float2 opacityTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.opacityTextureTransform);
    float2 transmissionTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.transmissionTextureTransform);
    float2 specularColorTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.specularColorTextureTransform);
    float2 clearcoatTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.clearcoatTextureTransform);
    float2 clearcoatRoughnessTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.clearcoatRoughnessTextureTransform);
    float2 clearcoatNormalTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.clearcoatNormalTextureTransform);
    float2 thicknessTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.thicknessTextureTransform);
    float2 sheenColorTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.sheenColorTextureTransform);
    float2 sheenRoughnessTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.sheenRoughnessTextureTransform);
    float2 iridescenceTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.iridescenceTextureTransform);
    float2 iridescenceThicknessTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.iridescenceThicknessTextureTransform);

    float3 baseColor = vertIn.baseColor.rgb;
    float baseAlpha = vertIn.baseColor.a;

    if (vertIn.baseColorTextureIndex != 0xffffffff)
    {
        float4 texColor = SamplePbrTexture(
            vertIn.baseColorTextureIndex, baseColorTexCoord, 1.0.xxxx);
        baseColor *= texColor.rgb;
        baseAlpha *= texColor.a;
    }
    baseAlpha = EvaluateOpacity(
        baseAlpha, vertIn.opacityTextureIndex, vertIn.opacityFactor,
        opacityTexCoord, material.opacityTextureTransform);

    if ((vertIn.flags & kObjectFlagAlphaCutoff) != 0u && baseAlpha < vertIn.alphaCutoff)
    {
        discard;
    }

    if (baseAlpha <= 0.01)
    {
        discard;
    }

    float sampledMetallic = EvaluateMetalness(
        vertIn.metallicRoughness.x, vertIn.metallicRoughnessTextureIndex,
        vertIn.metalnessTextureIndex, metallicRoughnessTexCoord,
        metalnessTexCoord, material.metalnessTextureTransform);
    float sampledRoughness = EvaluateRoughness(
        vertIn.metallicRoughness.y, vertIn.metallicRoughnessTextureIndex,
        vertIn.roughnessTextureIndex, metallicRoughnessTexCoord,
        roughnessTexCoord, material.roughnessTextureTransform, vertIn.flags);
    float specularFactor = EvaluateSpecularFactor(
        vertIn.specularFactor, vertIn.specularTextureIndex, specularTexCoord,
        material.specularTextureTransform);
    float transmissionFactor = EvaluateTransmissionFactor(
        vertIn.transmissionFactor, vertIn.transmissionTextureIndex,
        transmissionTexCoord, material.transmissionTextureTransform);
    float3 specularColor = EvaluateSpecularColor(
        material.specularColorFactor.rgb, material.specularColorTextureIndex,
        specularColorTexCoord, vertIn.specularTextureIndex, specularTexCoord);
    float clearcoat = EvaluateClearcoatFactor(
        material.clearcoatFactor, material.clearcoatTextureIndex,
        clearcoatTexCoord, material.clearcoatTextureTransform);
    float clearcoatRoughness = EvaluateClearcoatRoughness(
        material.clearcoatRoughnessFactor,
        material.clearcoatRoughnessTextureIndex, clearcoatRoughnessTexCoord,
        material.clearcoatRoughnessTextureTransform);
    float3 sheenColor = EvaluateSheenColor(
        material.sheenColorFactor.rgb, material.sheenColorTextureIndex,
        sheenColorTexCoord);
    float sheenRoughness = EvaluateSheenRoughness(
        material.sheenRoughnessFactor, material.sheenRoughnessTextureIndex,
        sheenRoughnessTexCoord, material.sheenRoughnessTextureTransform);
    float iridescence = EvaluateIridescenceFactor(
        material.iridescenceFactor, material.iridescenceTextureIndex,
        iridescenceTexCoord, material.iridescenceTextureTransform);
    float iridescenceThickness = EvaluateIridescenceThickness(
        material.iridescenceThicknessMinimum,
        material.iridescenceThicknessMaximum,
        material.iridescenceThicknessTextureIndex, iridescenceThicknessTexCoord,
        material.iridescenceThicknessTextureTransform);
    float volumeThickness = EvaluateThickness(
        material.thicknessFactor, material.thicknessTextureIndex,
        thicknessTexCoord, material.thicknessTextureTransform);

    float faceSign =
        ((vertIn.flags & kObjectFlagDoubleSided) != 0u && !isFrontFace)
            ? -1.0 : 1.0;
    SurfaceNormalState surfaceNormal = EvaluateSurfaceNormal(
        vertIn.worldNormal, vertIn.worldTangent, normalTexCoord,
        vertIn.normalTextureIndex, vertIn.normalTextureScale, faceSign);

    float occlusion = 1.0;
    if (vertIn.occlusionTextureIndex != 0xffffffff)
    {
        float sampledOcclusion = SamplePbrTexture(
            vertIn.occlusionTextureIndex, occlusionTexCoord, 1.0.xxxx).r;
        occlusion = lerp(1.0, sampledOcclusion, saturate(vertIn.occlusionStrength));
    }

    float3 emissive = vertIn.emissiveColor;
    if (vertIn.emissiveTextureIndex != 0xffffffff)
    {
        emissive *= SamplePbrTexture(
            vertIn.emissiveTextureIndex, emissiveTexCoord, 1.0.xxxx).rgb;
    }
    emissive *= vertIn.emissiveStrength;

    float metallic = sampledMetallic;
    float roughness = sampledRoughness;
    if (pc.semanticColorMode != kBimSemanticColorModeOff)
    {
        baseColor = EvaluateBimSemanticColor(
            uObjects[vertIn.objectIndex].objectInfo.w, pc.semanticColorMode);
        metallic = 0.0;
        roughness = 0.74;
        emissive = 0.0.xxx;
        transmissionFactor = 0.0;
        clearcoat = 0.0;
        sheenColor = 0.0.xxx;
        iridescence = 0.0;
        volumeThickness = 0.0;
    }
    float3 V = SafeNormalize(uCamera.cameraWorldPosition.xyz - vertIn.worldPos,
                             float3(0.0, 0.0, 1.0));
    float NdotV = max(dot(surfaceNormal.shadingNormal, V), 0.0);
    float3 iridescenceTint = ApproximateIridescenceTint(
        iridescence, material.iridescenceIor, iridescenceThickness, NdotV);
    float3 F0 = EvaluatePbrF0(
        baseColor, metallic, specularFactor, specularColor * iridescenceTint,
        material.ior);

    float3 F_ibl = FresnelSchlickRoughness(NdotV, F0, roughness);
    float3 kD_ibl = (1.0 - F_ibl) * (1.0 - metallic);
    float environmentIntensity =
        max(TransparentFiniteOr(uLighting.environmentIntensity, 0.0), 0.0);
    float3 irradiance =
        TransparentFinite3Or(
            gIrradianceCubemap.Sample(envSampler, surfaceNormal.shadingNormal).rgb,
            0.0.xxx) *
        environmentIntensity;
    float3 diffuseIBL = irradiance * baseColor;
    float3 R = reflect(-V, surfaceNormal.shadingNormal);
    float maxMipLevel = max(float(uLighting.prefilteredMipCount) - 1.0, 0.0);
    float3 prefilteredColor =
        TransparentFinite3Or(
            gPrefilteredCubemap.SampleLevel(envSampler, R,
                                            roughness * maxMipLevel).rgb,
            0.0.xxx) *
        environmentIntensity;
    float2 brdf = max(
        TransparentFinite2Or(
            gBrdfLUT.Sample(brdfSampler, float2(NdotV, roughness)),
            0.0.xx),
        0.0.xx);
    float3 specularIBL =
        EvaluateSpecularIblMultipleScattering(prefilteredColor, F0, brdf);

    float diffuseAO = occlusion;
    float specularOcclusion = saturate(pow(NdotV + diffuseAO, exp2(-16.0 * roughness - 1.0)) - 1.0 + diffuseAO);
    specularOcclusion = lerp(1.0, specularOcclusion, saturate(1.0 - roughness));
    float3 lighting = kD_ibl * diffuseIBL * diffuseAO + specularIBL * specularOcclusion;
    lighting += ComputeTransparentAmbientFloor(
        baseColor, occlusion, environmentIntensity);

    if ((vertIn.flags & kPbrObjectFlagUnlit) != 0u)
    {
        lighting = baseColor;
        baseAlpha *= lerp(1.0, 0.35, transmissionFactor);
    }

    float3 directionalL = SafeNormalize(-uLighting.directionalDirection.xyz,
                                        float3(0.0, 1.0, 0.0));
    float3 directionalRadiance =
        uLighting.directionalColorIntensity.rgb * uLighting.directionalColorIntensity.a;
    const bool thinSurface = IsThinSurfaceGpuMaterial(vertIn.flags);
    float shadowFactor = 1.0;
    if ((vertIn.flags & kPbrObjectFlagUnlit) == 0u)
    {
        float3 cameraForward =
            SafeNormalize(uCamera.cameraForward.xyz, float3(0.0, 0.0, -1.0));
        float viewDepth =
            max(dot(vertIn.worldPos - uCamera.cameraWorldPosition.xyz,
                    cameraForward), 0.0);
        float NdotLForShadow = dot(surfaceNormal.geometricNormal, directionalL);
        if (uLighting.shadowEnabled != 0u &&
            (NdotLForShadow > 0.0 || thinSurface))
        {
            shadowFactor = ComputeShadowFactor(
                vertIn.worldPos, surfaceNormal.geometricNormal,
                uLighting.directionalDirection.xyz, viewDepth,
                uShadow, gShadowAtlas, gShadowSampler);
        }
        lighting += EvaluatePbrDirectLight(
            baseColor, F0, metallic, roughness, surfaceNormal.shadingNormal, V,
            directionalL, directionalRadiance * shadowFactor);
        lighting += EvaluateDiffuseBounceLight(
            baseColor, metallic, surfaceNormal.shadingNormal,
            surfaceNormal.geometricNormal, V, directionalL,
            directionalRadiance * shadowFactor, shadowFactor, occlusion,
            uLighting.bounceIntensity);
    }
    if (thinSurface)
    {
        if ((vertIn.flags & kPbrObjectFlagUnlit) == 0u)
        {
            lighting += EvaluateThinSurfaceTransmission(
                baseColor, surfaceNormal.shadingNormal, V, directionalL,
                directionalRadiance * shadowFactor, saturate(baseAlpha),
                !isFrontFace);
        }
    }

    for (uint i = 0; i < uLighting.pointLightCount && i < MAX_POINT_LIGHTS; ++i)
    {
        PointLightData light = uPointLights[i];
        float3 lightVector = light.positionRadius.xyz - vertIn.worldPos;
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
        float lightVisibility = 1.0;
        if (LocalShadowMapsEnabled(uLighting, uLocalShadow))
        {
            lightVisibility = LocalPointLightShadowVisibility(
                light, vertIn.worldPos, surfaceNormal.geometricNormal, L,
                uLocalShadow, gLocalShadowAtlas, gLocalShadowSampler);
            radiance *= lightVisibility;
        }
        if ((vertIn.flags & kPbrObjectFlagUnlit) == 0u)
        {
            lighting += EvaluatePbrDirectLight(
                baseColor, F0, metallic, roughness, surfaceNormal.shadingNormal,
                V, L, radiance);
            lighting += EvaluateDiffuseBounceLight(
                baseColor, metallic, surfaceNormal.shadingNormal,
                surfaceNormal.geometricNormal, V, L, radiance,
                lightVisibility, occlusion, uLighting.bounceIntensity);
            if (thinSurface)
            {
                lighting += EvaluateThinSurfaceTransmission(
                    baseColor, surfaceNormal.shadingNormal, V, L, radiance,
                    saturate(baseAlpha), !isFrontFace);
            }
        }
    }

    if ((vertIn.flags & kPbrObjectFlagUnlit) == 0u)
    {
        uint areaLightCount = min(uLighting.areaLightCount, MAX_AREA_LIGHTS);
        for (uint i = 0u; i < areaLightCount; ++i)
        {
            AreaLightData light = uAreaLights[i];
            AreaLightFrame frame = BuildAreaLightFrame(light);
            float areaShadowVisibility = 1.0;
            uint areaShadowRef = PackedLocalShadowAreaRef(uLocalShadow, i);
            if (LocalShadowMapsEnabled(uLighting, uLocalShadow) &&
                areaShadowRef != 0u)
            {
                // The local-shadow PCF radius already models finite source
                // size; a reduced weighted shadow estimate avoids a full PCF
                // for every lighting integration sample.
                areaShadowVisibility = 0.0;
                float visibilityWeightSum = 0.0;
                [unroll]
                for (uint visibilitySample = 0u;
                     visibilitySample < AREA_LIGHT_SHADOW_VISIBILITY_SAMPLE_COUNT;
                     ++visibilitySample)
                {
                    uint areaSampleIndex =
                        AreaLightShadowVisibilitySampleIndex(visibilitySample);
                    float3 visibilitySurfaceToLight;
                    float weight = AreaLightSampleContributionWeight(
                        light, frame, vertIn.worldPos, areaSampleIndex,
                        visibilitySurfaceToLight);
                    if (weight <= 0.0)
                    {
                        continue;
                    }
                    float3 samplePosition =
                        AreaLightSamplePosition(frame, areaSampleIndex);
                    areaShadowVisibility += weight * LocalAreaLightShadowVisibility(
                        i, vertIn.worldPos, surfaceNormal.geometricNormal,
                        samplePosition, uLocalShadow, gLocalShadowAtlas,
                        gLocalShadowSampler);
                    visibilityWeightSum += weight;
                }
                areaShadowVisibility =
                    visibilityWeightSum > 1.0e-4
                        ? areaShadowVisibility / visibilityWeightSum
                        : 1.0;
            }
            [unroll]
            for (uint sampleIndex = 0u;
                 sampleIndex < AREA_LIGHT_INTEGRATION_SAMPLE_COUNT;
                 ++sampleIndex)
            {
                float3 L;
                float3 radiance = EvaluateAreaLightSampleRadiance(
                    light, frame, vertIn.worldPos, sampleIndex, L);
                float lightVisibility = areaShadowVisibility;
                radiance *= lightVisibility;
                lighting += EvaluatePbrDirectLight(
                    baseColor, F0, metallic, roughness,
                    surfaceNormal.shadingNormal, V, L, radiance);
                lighting += EvaluateDiffuseBounceLight(
                    baseColor, metallic, surfaceNormal.shadingNormal,
                    surfaceNormal.geometricNormal, V, L, radiance,
                    lightVisibility, occlusion, uLighting.bounceIntensity);
                if (thinSurface)
                {
                    lighting += EvaluateThinSurfaceTransmission(
                        baseColor, surfaceNormal.shadingNormal, V, L, radiance,
                        saturate(baseAlpha), !isFrontFace);
                }
            }
        }
    }

    if ((vertIn.flags & kPbrObjectFlagUnlit) == 0u)
    {
        if (clearcoat > 0.0)
        {
            float3 clearcoatNormal = surfaceNormal.shadingNormal;
            if (material.clearcoatNormalTextureIndex != kInvalidMaterialTexture)
            {
                clearcoatNormal = EvaluateSurfaceNormal(
                    vertIn.worldNormal, vertIn.worldTangent,
                    clearcoatNormalTexCoord,
                    material.clearcoatNormalTextureIndex,
                    material.clearcoatNormalTextureScale, faceSign).shadingNormal;
            }
            float clearcoatNdotV = max(dot(clearcoatNormal, V), 0.0);
            float3 clearcoatF0 = IorToF0(1.5).xxx;
            float3 clearcoatR = reflect(-V, clearcoatNormal);
            float3 clearcoatPrefiltered = TransparentFinite3Or(
                gPrefilteredCubemap.SampleLevel(
                    envSampler, clearcoatR,
                    clearcoatRoughness * maxMipLevel).rgb,
                0.0.xxx) *
                environmentIntensity;
            float2 clearcoatBrdf =
                max(TransparentFinite2Or(
                        gBrdfLUT.Sample(
                            brdfSampler,
                            float2(clearcoatNdotV, clearcoatRoughness)),
                        0.0.xx),
                    0.0.xx);
            float3 clearcoatIbl =
                EvaluateSpecularIblMultipleScattering(
                    clearcoatPrefiltered, clearcoatF0, clearcoatBrdf);
            lighting = lighting * (1.0 - clearcoat * 0.04) +
                       clearcoatIbl * clearcoat;
            lighting += EvaluatePbrDirectLight(
                            0.0.xxx, clearcoatF0, 1.0, clearcoatRoughness,
                            clearcoatNormal, V, directionalL,
                            directionalRadiance * shadowFactor) *
                        clearcoat;
        }

        float sheenWeight = MaxComponent(sheenColor);
        if (sheenWeight > 0.0)
        {
            float grazing = pow(1.0 - NdotV, 2.0);
            float sheenScale = grazing * (1.0 - 0.35 * sheenRoughness);
            lighting = lighting * (1.0 - saturate(sheenWeight * 0.25)) +
                       sheenColor *
                           (irradiance + directionalRadiance * shadowFactor) *
                           sheenScale * 0.35;
        }
    }

    lighting += emissive;

    float transmissionWeight = saturate(transmissionFactor * (1.0 - metallic));
    if (transmissionWeight > 0.0)
    {
        float3 transmittedColor = 0.0.xxx;
        if (volumeThickness > 0.0)
        {
            float safeIor = max(material.ior, 1.0);
            float halfSpread =
                (safeIor - 1.0) * 0.025 * max(material.dispersion, 0.0);
            float3 iors = max(float3(safeIor - halfSpread, safeIor,
                                     safeIor + halfSpread),
                              1.0.xxx);
            float etaR = isFrontFace ? (1.0 / iors.r) : iors.r;
            float etaG = isFrontFace ? (1.0 / iors.g) : iors.g;
            float etaB = isFrontFace ? (1.0 / iors.b) : iors.b;
            float3 refrR = refract(-V, surfaceNormal.shadingNormal, etaR);
            float3 refrG = refract(-V, surfaceNormal.shadingNormal, etaG);
            float3 refrB = refract(-V, surfaceNormal.shadingNormal, etaB);
            bool refrRValid = dot(refrR, refrR) > 1e-6 && all(isfinite(refrR));
            bool refrGValid = dot(refrG, refrG) > 1e-6 && all(isfinite(refrG));
            bool refrBValid = dot(refrB, refrB) > 1e-6 && all(isfinite(refrB));
            if (refrRValid)
            {
                transmittedColor.r = TransparentFiniteOr(
                    gPrefilteredCubemap.SampleLevel(
                        envSampler, refrR, roughness * maxMipLevel).r,
                    0.0);
            }
            if (refrGValid)
            {
                transmittedColor.g = TransparentFiniteOr(
                    gPrefilteredCubemap.SampleLevel(
                        envSampler, refrG, roughness * maxMipLevel).g,
                    0.0);
            }
            if (refrBValid)
            {
                transmittedColor.b = TransparentFiniteOr(
                    gPrefilteredCubemap.SampleLevel(
                        envSampler, refrB, roughness * maxMipLevel).b,
                    0.0);
            }
        }
        else
        {
            transmittedColor = TransparentFinite3Or(
                gPrefilteredCubemap.SampleLevel(
                    envSampler, -V, roughness * maxMipLevel).rgb,
                0.0.xxx);
        }
        transmittedColor *= environmentIntensity;

        if (dot(transmittedColor, transmittedColor) > 0.0 &&
            all(isfinite(transmittedColor)))
        {
            transmittedColor = ApplyBeerLambert(
                transmittedColor * baseColor, material.attenuationColor.rgb,
                material.attenuationDistance, volumeThickness);
            lighting = lerp(lighting, transmittedColor + emissive,
                            transmissionWeight * 0.75);
        }
        baseAlpha *= lerp(1.0, 0.35, transmissionWeight);
    }

    uint2 pixelCoord = uint2(vertIn.pos.xy);
    if (pixelCoord.x >= uOit.viewportWidth || pixelCoord.y >= uOit.viewportHeight)
    {
        return 0.0.xxxx;
    }

    uint nodeIndex = 0u;
    InterlockedAdd(nodeCounter[0], 1u, nodeIndex);
    if (nodeIndex >= uOit.nodeCapacity)
    {
        return 0.0.xxxx;
    }

    TransparentNode node;
    node.color = float4(lighting, saturate(baseAlpha));
    node.depth = saturate(vertIn.pos.z);
    node.next = INVALID_NODE_INDEX;
    node.padding0 = 0.0;
    node.padding1 = 0.0;

    uint previousHead = INVALID_NODE_INDEX;
    InterlockedExchange(headPointerImage[pixelCoord], nodeIndex, previousHead);
    node.next = previousHead;
    transparentNodes[nodeIndex] = node;

    return 0.0.xxxx;
}
