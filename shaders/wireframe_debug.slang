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

#if defined(BIM_FLOOR_PLAN_DISPLAY_DEPTH)
[[vk::binding(6, 1)]] Texture2D<float> floorPlanSceneDepth;
#endif
#if defined(BIM_NATIVE_OIT)
#define OIT_WRITE_SET 1
#include "oit_write_common.slang"
#endif

#include "scene_clip_common.slang"
#include "object_index_common.slang"
#include "pbr_material_common.slang"

[[vk::push_constant]]
ConstantBuffer<WireframePushConstants> pc;

[[vk::constant_id(0)]] const uint kUseLargePoints = 1u;

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

struct PSInput
{
    float4 pos : SV_Position;
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
    output.pos = mul(pc.useUnjitteredCamera != 0 && uCamera.temporalInfo.w != 0u
                         ? uCamera.unjitteredViewProj : uCamera.viewProj, worldPos);
    output.pointSize = kUseLargePoints != 0u ? max(pc.lineWidth, 1.0) : 1.0;
    output.worldPos = worldPos.xyz;
    output.objectColor = material.color;
    return output;
}

#if defined(BIM_NATIVE_OIT)
// A fragment behind opaque geometry must fail depth before it inserts a node.
[earlydepthstencil]
#endif
[shader("fragment")]
float4 fragMain(PSInput input) : SV_Target0
{
#if defined(BIM_FLOOR_PLAN_DISPLAY_DEPTH)
    // The swapchain scope has no depth attachment. Read the resolved reverse-Z
    // prepass depth at the fragment's framebuffer coordinates.
    if (input.pos.z + pc.floorPlanDepthTolerance <
        floorPlanSceneDepth.Load(int3(int2(input.pos.xy), 0)))
    {
        discard;
    }
#endif
    if (SceneClipPlanesClip(input.worldPos, pc.sectionPlaneEnabled, pc.sectionPlane))
    {
        discard;
    }

    float4 color = pc.useObjectColor != 0
        ? input.objectColor : float4(pc.colorIntensity.rgb, 1.0);
    color.a = saturate(color.a * pc.colorIntensity.a);
#if defined(BIM_NATIVE_OIT)
    if (color.a > 0.0)
    {
        uint2 pixel = uint2(input.pos.xy);
        uint nodeIndex = AllocateOitNode(pixel);
        if (nodeIndex != INVALID_NODE_INDEX)
            AppendOitNode(pixel, nodeIndex, input.pos.z, color);
    }
    return 0.0.xxxx;
#else
    return color;
#endif
}



