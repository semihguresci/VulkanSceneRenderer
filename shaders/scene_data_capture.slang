#include "brdf_common.slang"

struct SceneDataCapturePushConstants
{
    column_major float4x4 inverseProjection;
    uint width;
    uint height;
    float depthScale;
    uint bimSensor;
};

// Keep this tightly packed to match the host's 20-byte readback record.
struct SceneDataPixel
{
    float depth;
    float normalX;
    float normalY;
    float normalZ;
    uint objectId;
};

[[vk::binding(0, 0)]] Texture2D<float> sceneDepth;
[[vk::binding(1, 0)]] Texture2D<float4> shadingNormals;
[[vk::binding(2, 0)]] Texture2D<uint> opaqueObjectIds;
[[vk::binding(3, 0)]] RWStructuredBuffer<SceneDataPixel> capturePixels;
[[vk::push_constant]] ConstantBuffer<SceneDataCapturePushConstants> pc;

[numthreads(8, 8, 1)]
[shader("compute")]
void computeMain(uint3 dispatchId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchId.xy;
    if (any(pixel >= uint2(pc.width, pc.height)))
        return;

    int3 texel = int3(pixel, 0);
    SceneDataPixel data = {};
    uint objectId = opaqueObjectIds.Load(texel);
    // Source bits match the host pick-ID layout. Each capture selects one
    // provider because its object IDs are resolved through that provider.
    uint provider = objectId & 0xc0000000u;
    uint index = objectId & 0x3fffffffu;
    if (index != 0u && provider == (pc.bimSensor != 0u ? 0x80000000u : 0u))
    {
        float2 uv = (float2(pixel) + 0.5.xx) / float2(pc.width, pc.height);
        // The scene viewport flips Y, while reverse-Z depth is already in NDC.
        float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
        float4 view = mul(pc.inverseProjection, float4(ndc, sceneDepth.Load(texel), 1.0));
        // Camera forward is -Z. Decode the same world-space shading normal used by lighting.
        data.depth = view.w > 0.0 ? max(FiniteScalarOr(-view.z / view.w * pc.depthScale, 0.0), 0.0) : 0.0;
        float3 normal = SafeNormalize(shadingNormals.Load(texel).xyz * 2.0 - 1.0,
                                      float3(0.0, 1.0, 0.0));
        data.normalX = normal.x;
        data.normalY = normal.y;
        data.normalZ = normal.z;
        data.objectId = objectId;
    }

    capturePixels[pixel.y * pc.width + pixel.x] = data;
}
