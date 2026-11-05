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
    [[vk::location(0)]] float3 worldPos;
};

float3 safeNormalize(float3 value, float3 fallback)
{
    const float lenSq = dot(value, value);
    return lenSq > 1.0e-8 ? value * rsqrt(lenSq) : fallback;
}

float2 safeNormalize2(float2 value, float2 fallback)
{
    const float lenSq = dot(value, value);
    return lenSq > 1.0e-8 ? value * rsqrt(lenSq) : fallback;
}

[shader("vertex")]
VSOutput vertMain(VSInput input, uint instanceID : SV_InstanceID,
                  uint startInstance : SV_StartInstanceLocation)
{
    VSOutput output;
    const uint objectIndex =
        ResolveObjectIndex(pc.objectIndex, instanceID, startInstance);
    const ObjectBuffer obj = uObjects[objectIndex];
    const GpuMaterial material = uMaterials[obj.objectInfo.x];
    const float3 objectPosition = ApplyHeightDisplacement(
        input.inPosition, input.inNormal, input.inTexCoord,
        material.heightTextureIndex, material.heightTextureTransform,
        material.heightScale,
        material.heightOffset);

    const float4 worldPos = mul(obj.model, float4(objectPosition, 1.0));
    const float3 worldNormal = safeNormalize(
        float3(dot(obj.normalMatrix0.xyz, input.inNormal),
               dot(obj.normalMatrix1.xyz, input.inNormal),
               dot(obj.normalMatrix2.xyz, input.inNormal)),
        float3(0.0, 1.0, 0.0));
    const float4 clipPos = mul(uCamera.viewProj, worldPos);
    const float4 normalClip =
        mul(uCamera.viewProj, float4(worldPos.xyz + worldNormal, 1.0));

    const float2 viewport =
        max(float2(pc.padding7, pc.padding8), float2(1.0, 1.0));
    const float2 clipNdc = clipPos.xy / max(abs(clipPos.w), 1.0e-6);
    const float4 centerClip =
        mul(uCamera.viewProj, float4(obj.boundingSphere.xyz, 1.0));
    const float2 centerNdc =
        centerClip.xy / max(abs(centerClip.w), 1.0e-6);
    const float2 normalNdc =
        normalClip.xy / max(abs(normalClip.w), 1.0e-6);
    const float2 normalDir =
        safeNormalize2(normalNdc - clipNdc, float2(0.0, 1.0));
    const float2 fallbackDir = safeNormalize2(clipNdc - centerNdc, normalDir);
    const float2 outlineDir =
        safeNormalize2(clipNdc - centerNdc, fallbackDir);
    const float outlinePixels = max(pc.lineWidth, 1.0);

    output.pos = clipPos;
    output.pos.xy += outlineDir * (outlinePixels * 2.0 / viewport) *
                     max(abs(clipPos.w), 1.0e-6);
    output.worldPos = worldPos.xyz;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0
{
    if (SceneClipPlanesClip(input.worldPos, pc.sectionPlaneEnabled,
                            pc.sectionPlane))
    {
        discard;
    }

    return float4(pc.colorIntensity.rgb, saturate(pc.colorIntensity.a));
}
