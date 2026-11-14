// The display overlay uses the unjittered world-to-clip transform and the
// post-process pass's negative-height viewport. It does not enter scene HDR.
struct PushConstants
{
    float4x4 viewProjection;
    uint encodeSrgb;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> pc;

struct VertexInput
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 color;
};

struct VertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 color;
};

[shader("vertex")]
VertexOutput vertMain(VertexInput input)
{
    VertexOutput output;
    output.position = mul(pc.viewProjection, float4(input.position, 1.0));
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 fragMain(VertexOutput input) : SV_Target0
{
    const float3 srgb = saturate(input.color);
    const float3 linearColor = select(srgb <= 0.04045, srgb / 12.92, pow((srgb + 0.055) / 1.055, 2.4));
    return float4(pc.encodeSrgb != 0 ? srgb : linearColor, 0.76);
}
