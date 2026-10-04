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
#include "temporal_common.slang"

[[vk::push_constant]]
ConstantBuffer<BindlessPushConstants> pc;

struct VSInput
{
    [[vk::location(0)]] float3 inPosition;
    [[vk::location(2)]] float2 inTexCoord;
    [[vk::location(3)]] float3 inNormal;
    [[vk::location(5)]] float2 inTexCoord1;
};

struct VSOutput
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float2 texCoord;
    [[vk::location(1)]] uint baseColorTextureIndex;
    [[vk::location(2)]] uint flags;
    [[vk::location(3)]] float alphaCutoff;
    [[vk::location(4)]] float baseColorAlpha;
    [[vk::location(5)]] uint opacityTextureIndex;
    [[vk::location(6)]] float opacityFactor;
    [[vk::location(7)]] float2 texCoord1;
    [[vk::location(8)]] uint objectIndex;
    [[vk::location(9)]] float3 worldPos;
    [[vk::location(10)]] float4 currentClip;
    [[vk::location(11)]] float4 previousClip;
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
        material.heightScale, material.heightOffset);
    float4 worldPos = mul(obj.model, float4(objectPosition, 1.0));
    output.pos = mul(uCamera.viewProj, worldPos);
    output.texCoord = input.inTexCoord;
    output.texCoord1 = input.inTexCoord1;
    output.baseColorTextureIndex = material.baseColorTextureIndex;
    output.flags = material.flags | obj.objectInfo.y;
    output.alphaCutoff = material.alphaCutoff;
    output.baseColorAlpha = material.color.a;
    output.opacityTextureIndex = material.opacityTextureIndex;
    output.opacityFactor = material.opacityFactor;
    output.objectIndex = objectIndex;
    output.worldPos = worldPos.xyz;
    output.currentClip = mul(uCamera.unjitteredViewProj, worldPos);
    output.previousClip = mul(uCamera.previousViewProj, mul(obj.previousModel, float4(objectPosition, 1.0)));
    return output;
}

struct VelocityOutput { float4 motion : SV_Target0; uint identity : SV_Target1; float reactive : SV_Target2; };

[shader("fragment")]
VelocityOutput fragMain(VSOutput input)
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
            input.texCoord, input.texCoord1,
            material.baseColorTextureTransform);
        alpha *= SamplePbrTexture(
            input.baseColorTextureIndex, baseColorTexCoord, 1.0.xxxx).a;
    }
    alpha = EvaluateOpacity(
        alpha, input.opacityTextureIndex, input.opacityFactor,
        ApplyGpuTextureTransform(input.texCoord, input.texCoord1,
                                 material.opacityTextureTransform),
        material.opacityTextureTransform);

    if ((input.flags & 0x1u) != 0u &&
        !AlphaMaskPass(alpha, input.alphaCutoff, input.pos))
    {
        // The depth prepass must apply the same alpha-test decision as shadow
        // and G-buffer passes, otherwise cutout materials can incorrectly hide
        // or reveal later lighting work.
        discard;
    }
    ObjectBuffer object = uObjects[input.objectIndex];
    TemporalMotionVector motion = temporalMotionFromUnjitteredClips(
        input.currentClip, input.previousClip,
        uCamera.temporalInfo.x != 0u && object.temporalInfo.y != 0u);
    VelocityOutput result;
    result.motion = float4(motion.uv, motion.previousReverseZDepth, motion.valid ? 1.0 : 0.0);
    result.identity = object.temporalInfo.x;
    // Emissive appearance has no lighting motion vector: use current shading.
    result.reactive = (any(material.emissiveColor > 0.0) || material.emissiveTextureIndex != 0xffffffffu) && material.emissiveStrength > 0.0 ? 1.0 : 0.0;
    return result;
}
