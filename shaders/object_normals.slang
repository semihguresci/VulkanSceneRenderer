#include "lighting_structs.slang"
#include "push_constants_common.slang"
#include "object_data_common.slang"
#include "material_data_common.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer> uObjects;

#include "object_index_common.slang"
[[vk::binding(2, 0)]] SamplerState materialSamplers[MATERIAL_SAMPLER_DESCRIPTOR_COUNT];
[[vk::binding(3, 0)]] StructuredBuffer<GpuMaterial> uMaterials;
[[vk::binding(4, 0)]] StructuredBuffer<GpuTextureMetadata> uTextureMetadata;
[[vk::binding(5, 0)]] Texture2D materialTextures[MATERIAL_TEXTURE_DESCRIPTOR_COUNT];

#include "scene_clip_common.slang"
#include "alpha_mask_common.slang"
#include "pbr_material_common.slang"

[[vk::push_constant]]
ConstantBuffer<BindlessPushConstants> pc;

struct VSInput
{
    [[vk::location(0)]] float3 inPosition;
    [[vk::location(1)]] float3 inColor;
    [[vk::location(2)]] float2 inTexCoord;
    [[vk::location(3)]] float3 inNormal;
    [[vk::location(4)]] float4 inTangent;
    [[vk::location(8)]] float inColorAlpha;
};

struct VSOutput
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 objectNormal;
    [[vk::location(1)]] float2 texCoord;
    [[vk::location(2)]] uint baseColorTextureIndex;
    [[vk::location(3)]] uint flags;
    [[vk::location(4)]] float alphaCutoff;
    [[vk::location(5)]] float baseColorAlpha;
    [[vk::location(6)]] uint opacityTextureIndex;
    [[vk::location(7)]] float opacityFactor;
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

    float3 objectPosition = ApplyHeightDisplacement(
        input.inPosition, input.inNormal, input.inTexCoord,
        material.heightTextureIndex, material.heightTextureTransform,
        material.heightScale, material.heightOffset);
    float4 worldPos = mul(obj.model, float4(objectPosition, 1.0));
    output.pos = mul(uCamera.viewProj, worldPos);
    output.objectNormal = normalize(input.inNormal);
    output.texCoord = input.inTexCoord;
    output.baseColorTextureIndex = material.baseColorTextureIndex;
    output.flags = material.flags | obj.objectInfo.y;
    output.alphaCutoff = material.alphaCutoff;
    output.baseColorAlpha = material.color.a * input.inColorAlpha;
    output.opacityTextureIndex = material.opacityTextureIndex;
    output.opacityFactor = material.opacityFactor;
    output.objectIndex = objectIndex;
    output.worldPos = worldPos.xyz;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0
{
    if ((input.flags & 0x2u) != 0u)
    {
        discard;
    }
    if (SceneClipPlanesClip(input.worldPos, pc.sectionPlaneEnabled, pc.sectionPlane))
    {
        discard;
    }

    GpuMaterial material = uMaterials[uObjects[input.objectIndex].objectInfo.x];
    float alpha = input.baseColorAlpha;
    if (input.baseColorTextureIndex != 0xffffffff)
    {
        float2 baseColorTexCoord = ApplyGpuTextureTransform(
            input.texCoord, input.texCoord, material.baseColorTextureTransform);
        alpha *= SamplePbrTexture(
            input.baseColorTextureIndex, baseColorTexCoord, 1.0.xxxx).a;
    }
    alpha = EvaluateOpacity(
        alpha, input.opacityTextureIndex, input.opacityFactor,
        ApplyGpuTextureTransform(input.texCoord, input.texCoord,
                                 material.opacityTextureTransform),
        material.opacityTextureTransform);
    alpha = ResolveGpuMaterialAlpha(alpha, material.flags);

    if ((input.flags & 0x1u) != 0u &&
        !AlphaMaskPass(alpha, input.alphaCutoff, input.pos))
    {
        discard;
    }

    float3 objectNormal = normalize(input.objectNormal);
    if (!all(isfinite(objectNormal)))
    {
        objectNormal = float3(0.0, 1.0, 0.0);
    }

    return float4(saturate(objectNormal * 0.5 + 0.5), 1.0);
}
