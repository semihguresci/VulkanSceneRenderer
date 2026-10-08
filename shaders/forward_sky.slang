#include "lighting_structs.slang"
#include "environment_sky_common.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(0, 1)]] ConstantBuffer<LightingBuffer> uLighting;
[[vk::binding(11, 3)]] TextureCube<float4> environment;
[[vk::binding(13, 3)]] SamplerState environmentSampler;

struct VSOutput { float4 pos : SV_Position; };
[shader("vertex")]
VSOutput vertMain(uint vertexIndex : SV_VertexID)
{
    static const float2 positions[3] = {
        float2(-1, -1), float2(3, -1), float2(-1, 3)
    };
    VSOutput output;
    // Clear reverse-Z depth is zero. The pipeline tests equality per MSAA
    // sample and never writes depth, preserving covered opaque samples.
    output.pos = float4(positions[vertexIndex], 0, 1);
    return output;
}
[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0
{
    float2 uv = (floor(input.pos.xy) + 0.5.xx) * uCamera.renderExtent.zw;
    return float4(SampleEnvironmentBackground(uv, uCamera.inverseViewProj,
        uLighting.environmentIntensity, environment, environmentSampler), 1);
}
