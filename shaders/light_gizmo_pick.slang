#include "lighting_structs.slang"
#include "push_constants_common.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(0, 1)]] Texture2DArray<float4> uLightGizmoIcons;
[[vk::binding(1, 1)]] SamplerState uLightGizmoSampler;

[[vk::push_constant]]
ConstantBuffer<LightPushConstants> pc;

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float iconLayer : TEXCOORD1;
};

[shader("vertex")]
VSOutput vertMain(uint vertexIndex : SV_VertexID)
{
    static const float2 kQuadCorners[6] = {
        float2(-1.0, -1.0),
        float2( 1.0, -1.0),
        float2( 1.0,  1.0),
        float2(-1.0, -1.0),
        float2( 1.0,  1.0),
        float2(-1.0,  1.0),
    };
    static const float2 kQuadUvs[6] = {
        float2(0.0, 1.0),
        float2(1.0, 1.0),
        float2(1.0, 0.0),
        float2(0.0, 1.0),
        float2(1.0, 0.0),
        float2(0.0, 0.0),
    };

    float gizmoRadius =
        PointLightHasFiniteRange(pc.positionRadius.w) ? pc.positionRadius.w : 1.0;
    float3 cameraForwardRaw = uCamera.cameraForward.xyz;
    float cameraForwardLen2 = dot(cameraForwardRaw, cameraForwardRaw);
    float3 cameraForward = cameraForwardLen2 > 0.0001
        ? cameraForwardRaw * rsqrt(cameraForwardLen2)
        : float3(0.0, 0.0, -1.0);
    float3 upSeed = abs(dot(cameraForward, float3(0.0, 1.0, 0.0))) > 0.95
        ? float3(1.0, 0.0, 0.0)
        : float3(0.0, 1.0, 0.0);
    float3 right = normalize(cross(upSeed, cameraForward));
    float3 up = normalize(cross(cameraForward, right));
    float2 corner = kQuadCorners[vertexIndex];
    float3 worldPosition =
        pc.positionRadius.xyz + (right * corner.x + up * corner.y) * gizmoRadius;

    VSOutput output;
    output.pos = mul(uCamera.temporalInfo.w != 0u ? uCamera.unjitteredViewProj : uCamera.viewProj, float4(worldPosition, 1.0));
    output.uv = kQuadUvs[vertexIndex];
    output.iconLayer = max(pc.coneOuterCosType.x, 0.0);
    return output;
}

[shader("fragment")]
uint fragMain(VSOutput input) : SV_Target0
{
    if (pc.padding2 == 0u)
    {
        discard;
    }

    float layer = floor(input.iconLayer + 0.5);
    float4 icon = uLightGizmoIcons.Sample(uLightGizmoSampler, float3(input.uv, layer));
    if (icon.a <= 0.01)
    {
        discard;
    }

    return pc.padding2;
}
