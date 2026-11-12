#include "lighting_structs.slang"
#include "push_constants_common.slang"
#include "object_data_common.slang"
#include "material_data_common.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer> uObjects;
[[vk::binding(2, 0)]] SamplerState materialSamplers[];
[[vk::binding(3, 0)]] StructuredBuffer<GpuMaterial> uMaterials;
[[vk::binding(4, 0)]] StructuredBuffer<GpuTextureMetadata> uTextureMetadata;
[[vk::binding(5, 0)]] Texture2D materialTextures[MATERIAL_TEXTURE_DESCRIPTOR_COUNT];

#include "scene_clip_common.slang"
#include "alpha_mask_common.slang"
#include "object_index_common.slang"
#include "pbr_material_common.slang"
#include "surface_normal_common.slang"
#include "pbr_extended_common.slang"

[[vk::push_constant]]
ConstantBuffer<BindlessPushConstants> pc;

static const uint kObjectFlagAlphaMask = 0x1u;
static const uint kObjectFlagTransparentOnly = 0x2u;
static const uint kObjectFlagDoubleSided = 0x4u;

// Keep this interface and vertMain identical to gbuffer.slang. In particular,
// height displacement, deformed Vertex input, COLOR_0 alpha, UV1, mirrored
// tangent handedness, and object-index indirection must have identical coverage.
struct VSInput
{
    [[vk::location(0)]] float3 inPosition;
    [[vk::location(1)]] float3 inColor;
    [[vk::location(2)]] float2 inTexCoord;
    [[vk::location(3)]] float3 inNormal;
    [[vk::location(4)]] float4 inTangent;
    [[vk::location(5)]] float2 inTexCoord1;
    [[vk::location(8)]] float inColorAlpha;
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
    [[vk::location(11)]] float untexturedMaskDistance;
    [[vk::location(12)]] float normalTextureScale;
    [[vk::location(13)]] float occlusionStrength;
    [[vk::location(14)]] uint flags;
    [[vk::location(15)]] float3 worldNormal;
    [[vk::location(16)]] float4 worldTangent;
    [[vk::location(17)]] uint roughnessTextureIndex;
    [[vk::location(18)]] uint metalnessTextureIndex;
    [[vk::location(19)]] uint specularTextureIndex;
    [[vk::location(20)]] uint opacityTextureIndex;
    [[vk::location(21)]] nointerpolation float flatVertexAlpha;
    [[vk::location(22)]] float specularFactor;
    [[vk::location(23)]] uint objectIndex;
    [[vk::location(24)]] float2 texCoord1;
};

struct PSOutput
{
    float4 anisotropy : SV_Target0;
    float4 diffuseTransmission : SV_Target1;
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
    output.baseColor = material.color * float4(input.inColor, input.inColorAlpha);
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
    output.untexturedMaskDistance = UntexturedAlphaMaskDistance(input.inColorAlpha, material);
    output.normalTextureScale = material.normalTextureScale;
    output.occlusionStrength = material.occlusionStrength;
    output.flags = material.flags | obj.objectInfo.y;
    output.worldNormal = worldNormal;
    output.worldTangent = worldTangent;
    output.roughnessTextureIndex = material.roughnessTextureIndex;
    output.metalnessTextureIndex = material.metalnessTextureIndex;
    output.specularTextureIndex = material.specularTextureIndex;
    output.opacityTextureIndex = material.opacityTextureIndex;
    output.flatVertexAlpha = input.inColorAlpha;
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
    float2 opacityTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.opacityTextureTransform);

    float3 baseColor = vertIn.baseColor.rgb;
    float baseAlpha = RasterBaseAlpha(vertIn.baseColor.a, vertIn.flatVertexAlpha, material);

    if (vertIn.baseColorTextureIndex != 0xffffffff)
    {
        float4 texColor = SamplePbrTexture(
            vertIn.baseColorTextureIndex, baseColorTexCoord, 1.0.xxxx);
        baseColor *= texColor.rgb;
        baseAlpha *= texColor.a;
    }
    baseAlpha = EvaluateOpacity(
        baseAlpha, vertIn.opacityTextureIndex, material.opacityFactor,
        opacityTexCoord, material.opacityTextureTransform);
    baseAlpha = ResolveGpuMaterialAlpha(baseAlpha, material.flags);

    if ((vertIn.flags & kObjectFlagAlphaMask) != 0u &&
        !AlphaMaskPass(baseAlpha, vertIn.untexturedMaskDistance, material, vertIn.pos))
    {
        discard;
    }

    PSOutput output;
    output.anisotropy = 0.0.xxxx;
    output.diffuseTransmission = 0.0.xxxx;
    // Semantic visualization and unlit shading suppress physical material
    // effects, exactly as the G-buffer suppresses its clearcoat/sheen layers.
    // Write zero rather than discard: later equal-depth unflagged surfaces
    // must replace earlier flagged ownership in the original opaque draw order.
    if (pc.semanticColorMode != kBimSemanticColorModeOff ||
        (vertIn.flags & kPbrObjectFlagUnlit) != 0u)
    {
        return output;
    }

    float faceSign =
        ((vertIn.flags & kObjectFlagDoubleSided) != 0u && !isFrontFace)
            ? -1.0 : 1.0;
    SurfaceNormalState surfaceNormal = EvaluateSurfaceNormal(
        vertIn.worldNormal, vertIn.worldTangent, normalTexCoord,
        vertIn.normalTextureIndex, vertIn.normalTextureScale, faceSign);

    // Anisotropy is packed linear-data RG direction (encoded [-1,1]) and B
    // strength. No texture means authored +T, before the CCW rotation factor.
    float4 anisotropySample = SamplePbrTexture(
        material.anisotropyTextureIndex,
        ApplyGpuTextureTransform(vertIn.texCoord, vertIn.texCoord1,
                                 material.anisotropyTextureTransform),
        float4(1.0, 0.5, 1.0, 1.0));
    float anisotropyStrength =
        material.anisotropyDiffuseFactors.x * anisotropySample.b;
    float3 anisotropyTangent = BuildAnisotropyTangent(
        surfaceNormal, vertIn.worldTangent, anisotropySample.rg,
        material.anisotropyDiffuseFactors.y);

    // Strict multiplication preserves authored zero/black factors even when a
    // texture exists. The scalar texture defaults to A; channel 4 selects the
    // slot default, while 0-3 retain the engine's explicit channel override.
    float4 diffuseFactorSample = SamplePbrTexture(
        material.diffuseTransmissionTextureIndex,
        ApplyGpuTextureTransform(vertIn.texCoord, vertIn.texCoord1,
                                 material.diffuseTransmissionTextureTransform),
        1.0.xxxx);
    float diffuseFactor = material.anisotropyDiffuseFactors.z *
        SelectGpuTextureChannel(diffuseFactorSample,
                                 material.diffuseTransmissionTextureTransform,
                                 3u);
    // Color resources use sRGB Vulkan formats, so sampling already returns
    // scene-linear RGB. Do not apply another transfer-function conversion.
    float3 diffuseColor = material.diffuseTransmissionColorFactor.rgb *
        SamplePbrTexture(
            material.diffuseTransmissionColorTextureIndex,
            ApplyGpuTextureTransform(
                vertIn.texCoord, vertIn.texCoord1,
                material.diffuseTransmissionColorTextureTransform),
            1.0.xxxx).rgb;
    // This pass stores only the two auxiliary terms. Lighting combines them
    // with its existing sampled specular-transmission input independently.
    ExtendedPbrTerms terms = MakeExtendedPbrTerms(
        anisotropyTangent, anisotropyStrength, diffuseColor, diffuseFactor, 0.0);
    if (terms.anisotropyStrength > 0.0)
    {
        output.anisotropy = float4(terms.anisotropyTangent,
                                   terms.anisotropyStrength);
    }
    if (terms.diffuseTransmissionFactor > 0.0)
    {
        output.diffuseTransmission = EncodeExtendedPbrDiffuseStorage(terms);
    }
    return output;
}
