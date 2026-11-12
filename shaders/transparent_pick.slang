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
#include "object_index_common.slang"
#include "pbr_material_common.slang"
#include "alpha_mask_common.slang"

[[vk::push_constant]]
ConstantBuffer<BindlessPushConstants> pc;

static const uint kObjectFlagAlphaCutoff = 0x1u;

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
    [[vk::location(2)]] float2 texCoord1;
    [[vk::location(3)]] uint baseColorTextureIndex;
    [[vk::location(4)]] uint opacityTextureIndex;
    [[vk::location(5)]] float opacityFactor;
    [[vk::location(6)]] float alphaCutoff;
    [[vk::location(7)]] uint flags;
    [[vk::location(8)]] uint objectIndex;
    [[vk::location(9)]] float3 worldPos;
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

    output.pos = mul(uCamera.viewProj, worldPos);
    output.baseColor = material.color * float4(input.inColor, input.inColorAlpha);
    output.texCoord = input.inTexCoord;
    output.texCoord1 = input.inTexCoord1;
    output.baseColorTextureIndex = material.baseColorTextureIndex;
    output.opacityTextureIndex = material.opacityTextureIndex;
    output.opacityFactor = material.opacityFactor;
    output.alphaCutoff = material.alphaCutoff;
    output.flags = material.flags | obj.objectInfo.y;
    output.objectIndex = objectIndex;
    output.worldPos = worldPos.xyz;
    return output;
}

[shader("fragment")]
uint fragMain(VSOutput vertIn) : SV_Target0
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
    float2 opacityTexCoord = ApplyGpuTextureTransform(
        vertIn.texCoord, vertIn.texCoord1,
        material.opacityTextureTransform);

    float baseAlpha = vertIn.baseColor.a;
    if (vertIn.baseColorTextureIndex != 0xffffffff)
    {
        baseAlpha *= SamplePbrTexture(
            vertIn.baseColorTextureIndex, baseColorTexCoord, 1.0.xxxx).a;
    }
    baseAlpha = EvaluateOpacity(
        baseAlpha, vertIn.opacityTextureIndex, vertIn.opacityFactor,
        opacityTexCoord, material.opacityTextureTransform);
    baseAlpha = ResolveGpuMaterialAlpha(baseAlpha, material.flags);

    const bool alphaMasked = (vertIn.flags & kObjectFlagAlphaCutoff) != 0u;
    if (alphaMasked && !AlphaMaskPass(baseAlpha, vertIn.alphaCutoff, vertIn.pos))
    {
        discard;
    }

    if (!alphaMasked && baseAlpha <= 0.01)
    {
        discard;
    }

    return uObjects[vertIn.objectIndex].objectInfo.z |
           (vertIn.objectIndex + 1u);
}
