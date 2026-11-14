[[vk::binding(0, 0)]] Texture2DArray<float> atlas;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> sampledDepths;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    sampledDepths[id.x] = atlas.Load(int4(3, 4, id.x, 0));
}
