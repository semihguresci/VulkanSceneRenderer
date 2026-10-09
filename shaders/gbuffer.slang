#include "lighting_structs.slang"
#include "push_constants_common.slang"
#include "object_data_common.slang"
#include "material_data_common.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer> uObjects;
[[vk::binding(2, 0)]] SamplerState materialSamplers[MATERIAL_SAMPLER_DESCRIPTOR_COUNT];
[[vk::binding(3, 0)]] StructuredBuffer<GpuMaterial> uMaterials;
[[vk::binding(4, 0)]] StructuredBuffer<GpuTextureMetadata> uTextureMetadata;
[[vk::binding(5, 0)]] Texture2D materialTextures[MATERIAL_TEXTURE_DESCRIPTOR_COUNT];

#include "scene_clip_common.slang"
#include "alpha_mask_common.slang"
#include "object_index_common.slang"
#include "pbr_material_common.slang"
#include "surface_normal_common.slang"

[[vk::push_constant]]
ConstantBuffer<BindlessPushConstants> pc;

static const uint kObjectFlagAlphaMask = 0x1u;
static const uint kObjectFlagTransparentOnly = 0x2u;
static const uint kObjectFlagDoubleSided = 0x4u;

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
    [[vk::location(21)]] float opacityFactor;
    [[vk::location(22)]] float specularFactor;
    [[vk::location(23)]] uint objectIndex;
    [[vk::location(24)]] float2 texCoord1;
};

struct PSOutput
{
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
    float4 material : SV_Target2;
    float4 emissive : SV_Target3;
    float4 specular : SV_Target4;
    uint pickId : SV_Target5;
    float4 materialLayers : SV_Target6;
    float4 materialSheen : SV_Target7;
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
    output.opacityFactor = material.opacityFactor;
    output.specularFactor = material.specularFactor;
    output.objectIndex = objectIndex;
    return output;
}

[shader("fragment")]
PSOutput fragMain(VSOutput vertIn, bool isFrontFace : SV_IsFrontFace)
{
    if ((vertIn.flags & kObjectFlagTransparentOnly) != 0u)
    {
        discard;
    }
    if (SceneClipPlanesClip(vertIn.worldPos, pc.sectionPlaneEnabled, pc.sectionPlane))
    {
        discard;
    }

    uint materialIndex = uObjects[vertIn.objectIndex].objectInfo.x;
    GpuMaterial material = uMaterials[materialIndex];
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
    float2 specularColorTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.specularColorTextureTransform);

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

    if ((vertIn.flags & kObjectFlagAlphaMask) != 0u &&
        !AlphaMaskPass(baseAlpha, vertIn.alphaCutoff, vertIn.pos))
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
    float3 specularColor = EvaluateSpecularColor(
        material.specularColorFactor.rgb, material.specularColorTextureIndex,
        specularColorTexCoord, vertIn.specularTextureIndex, specularTexCoord);
    float3 dielectricF0Color = EvaluateDielectricF0Color(
        specularFactor, specularColor, material.ior);
    float dielectricF0 = EvaluateDielectricF0(
        specularFactor, specularColor, material.ior);

    float faceSign =
        ((vertIn.flags & kObjectFlagDoubleSided) != 0u && !isFrontFace)
            ? -1.0 : 1.0;
    SurfaceNormalState surfaceNormal = EvaluateSurfaceNormal(
        vertIn.worldNormal, vertIn.worldTangent, normalTexCoord,
        vertIn.normalTextureIndex, vertIn.normalTextureScale, faceSign);
    const bool thinSurface = IsThinSurfaceGpuMaterial(vertIn.flags);
    const float materialMetadata =
        EncodeGBufferMaterialMetadata(materialIndex, thinSurface);

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

    // Store sampled layer inputs separately from the authored base material.
    // Every deferred light and the IBL pass consume these same values.
    float clearcoat = EvaluateClearcoatFactor(
        material.clearcoatFactor, material.clearcoatTextureIndex,
        ApplyGpuTextureTransform(vertIn.texCoord, vertIn.texCoord1,
                                 material.clearcoatTextureTransform),
        material.clearcoatTextureTransform);
    float clearcoatRoughness = EvaluateClearcoatRoughness(
        material.clearcoatRoughnessFactor, material.clearcoatRoughnessTextureIndex,
        ApplyGpuTextureTransform(vertIn.texCoord, vertIn.texCoord1,
                                 material.clearcoatRoughnessTextureTransform),
        material.clearcoatRoughnessTextureTransform);
    float3 sheenColor = EvaluateSheenColor(
        material.sheenColorFactor.rgb, material.sheenColorTextureIndex,
        ApplyGpuTextureTransform(vertIn.texCoord, vertIn.texCoord1,
                                 material.sheenColorTextureTransform));
    float sheenRoughness = EvaluateSheenRoughness(
        material.sheenRoughnessFactor, material.sheenRoughnessTextureIndex,
        ApplyGpuTextureTransform(vertIn.texCoord, vertIn.texCoord1,
                                 material.sheenRoughnessTextureTransform),
        material.sheenRoughnessTextureTransform);
    float iridescence = EvaluateIridescenceFactor(
        material.iridescenceFactor, material.iridescenceTextureIndex,
        ApplyGpuTextureTransform(vertIn.texCoord, vertIn.texCoord1,
                                 material.iridescenceTextureTransform),
        material.iridescenceTextureTransform);
    float iridescenceThickness = EvaluateIridescenceThickness(
        material.iridescenceThicknessMinimum, material.iridescenceThicknessMaximum,
        material.iridescenceThicknessTextureIndex,
        ApplyGpuTextureTransform(vertIn.texCoord, vertIn.texCoord1,
                                 material.iridescenceThicknessTextureTransform),
        material.iridescenceThicknessTextureTransform);

    if (pc.semanticColorMode != kBimSemanticColorModeOff)
    {
        baseColor = EvaluateBimSemanticColor(
            uObjects[vertIn.objectIndex].objectInfo.w, pc.semanticColorMode);
        sampledMetallic = 0.0;
        sampledRoughness = 0.74;
        emissive = 0.0.xxx;
        dielectricF0Color = 0.04.xxx;
        dielectricF0 = 0.04;
        clearcoat = 0.0;
        sheenColor = 0.0.xxx;
        iridescence = 0.0;
    }

    if ((vertIn.flags & kPbrObjectFlagUnlit) != 0u)
    {
        PSOutput unlitOutput;
        unlitOutput.albedo = float4(0.0, 0.0, 0.0, baseAlpha);
        unlitOutput.normal =
            float4(surfaceNormal.shadingNormal * 0.5 + 0.5, 1.0);
        unlitOutput.material = float4(0.0, 1.0, 1.0, materialMetadata);
        unlitOutput.emissive = float4(baseColor + emissive, 0.0);
        unlitOutput.specular = 0.0.xxxx;
        unlitOutput.materialLayers = float4(0.0, 1.0, 0.0, 0.0);
        unlitOutput.materialSheen = float4(0.0, 0.0, 0.0, 1.0);
        unlitOutput.pickId =
            uObjects[vertIn.objectIndex].objectInfo.z | (vertIn.objectIndex + 1u);
        return unlitOutput;
    }

    PSOutput output;
    output.albedo = float4(baseColor, baseAlpha);
    output.normal = float4(surfaceNormal.shadingNormal * 0.5 + 0.5, 1.0);
    output.material = float4(sampledMetallic, sampledRoughness, occlusion,
                             materialMetadata);
    output.emissive = float4(emissive, dielectricF0);
    output.specular = float4(dielectricF0Color, dielectricF0);
    output.materialLayers = float4(clearcoat, clearcoatRoughness,
                                   iridescence, iridescenceThickness);
    output.materialSheen = float4(sheenColor, sheenRoughness);
    output.pickId =
        uObjects[vertIn.objectIndex].objectInfo.z | (vertIn.objectIndex + 1u);
    return output;
}
