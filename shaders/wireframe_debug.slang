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
#include "object_index_common.slang"
#include "pbr_material_common.slang"

[[vk::push_constant]]
ConstantBuffer<WireframePushConstants> pc;

struct VSInput
{
    [[vk::location(0)]] float3 inPosition;
    [[vk::location(2)]] float2 inTexCoord;
    [[vk::location(3)]] float3 inNormal;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float pointSize : SV_PointSize;
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] nointerpolation float4 objectColor;
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
        material.heightScale,
        material.heightOffset);
    float4 worldPos = mul(obj.model, float4(objectPosition, 1.0));
    output.pos = mul(uCamera.viewProj, worldPos);
    output.pointSize = max(pc.lineWidth, 1.0);
    output.worldPos = worldPos.xyz;
    output.objectColor = material.color;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0
{
    if (SceneClipPlanesClip(input.worldPos, pc.sectionPlaneEnabled, pc.sectionPlane))
    {
        discard;
    }

    float4 color = pc.useObjectColor != 0
        ? input.objectColor : float4(pc.colorIntensity.rgb, 1.0);
    return float4(color.rgb, saturate(color.a * pc.colorIntensity.a));
}



