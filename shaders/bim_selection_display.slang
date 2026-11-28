[[vk::binding(17, 0)]] Texture2D<float> selectionStroke;

struct SelectionDisplayPushConstants
{
    float4 color;
};

[[vk::push_constant]]
ConstantBuffer<SelectionDisplayPushConstants> pc;

[shader("vertex")]
float4 vertMain(uint vertexIndex : SV_VertexID) : SV_Position
{
    static const float2 positions[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    return float4(positions[vertexIndex], 0, 1);
}

[shader("fragment")]
float4 fragMain(float4 position : SV_Position) : SV_Target0
{
    if (selectionStroke.Load(int3(int2(position.xy), 0)) < 0.5)
        discard;
    return pc.color;
}
