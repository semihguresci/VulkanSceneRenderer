#include "lighting_structs.slang"
#include "push_constants_common.slang"
#include "object_data_common.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer> uObjects;

#include "scene_clip_common.slang"
#include "object_index_common.slang"

[[vk::push_constant]]
ConstantBuffer<BindlessPushConstants> pc;

struct VSInput
{
    [[vk::location(0)]] float3 inPosition;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float pointSize : SV_PointSize;
    [[vk::location(0)]] float3 color;
    [[vk::location(1)]] float3 worldPos;
};

struct PSInput
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 color;
    [[vk::location(1)]] float3 worldPos;
};

float3 hashColor(uint index)
{
    float scalar = float(index + 1u);
    float3 seed = float3(
        frac(sin(scalar * 12.9898) * 43758.5453),
        frac(sin(scalar * 78.2330) * 24634.6345),
        frac(sin(scalar * 39.4250) * 56445.2344));
    return 0.25.xxx + 0.75.xxx * seed;
}

[shader("vertex")]
VSOutput vertMain(VSInput input, uint instanceID : SV_InstanceID,
                  uint startInstance : SV_StartInstanceLocation)
{
    VSOutput output;
    uint objectIndex = ResolveObjectIndex(pc.objectIndex, instanceID, startInstance);
    ObjectBuffer obj = uObjects[objectIndex];

    float4 worldPos = mul(obj.model, float4(input.inPosition, 1.0));
    output.pos = mul(uCamera.viewProj, worldPos);
    output.pointSize = 4.0;
    output.color = hashColor(objectIndex);
    output.worldPos = worldPos.xyz;
    return output;
}

[shader("fragment")]
float4 fragMain(PSInput input) : SV_Target0
{
    if (SceneClipPlanesClip(input.worldPos, pc.sectionPlaneEnabled, pc.sectionPlane))
    {
        discard;
    }

    return float4(input.color, 1.0);
}
