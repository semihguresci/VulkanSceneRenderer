[[vk::binding(0, 0)]] Texture2D<float> nativeDepth;
[[vk::binding(1, 0)]] Texture2D<float4> nativeMotion;
[[vk::binding(2, 0)]] Texture2D<float4> composite;
[[vk::binding(3, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> fsrDepth;
[[vk::binding(4, 0)]] [[vk::image_format("rg16f")]] RWTexture2D<float2> fsrMotion;
[[vk::binding(5, 0)]] [[vk::image_format("r8")]] RWTexture2D<float> fsrReactive;
[[vk::binding(6, 0)]] [[vk::image_format("r8")]] RWTexture2D<float> fsrTransparency;
[[vk::binding(7, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> diagnostics;

struct FsrInputConstants { uint2 extent; };
[[vk::push_constant]] ConstantBuffer<FsrInputConstants> pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= pc.extent)) return;
    int3 samplePoint = int3(id.xy, 0);
    float depth = nativeDepth.Load(samplePoint);
    float4 motion = nativeMotion.Load(samplePoint);
    float coverage = composite.Load(samplePoint).a;
    bool validMotion = motion.w > 0.5 && all(isfinite(motion.xy));
    // Native velocity is current-to-previous unjittered upper-left UV. Sky,
    // new surfaces and HDR overlays do not expose a matching previous sample.
    fsrDepth[id.xy] = isfinite(depth) ? saturate(depth) : 0.0;
    fsrMotion[id.xy] = validMotion ? clamp(motion.xy, -2.0.xx, 2.0.xx) : 0.0.xx;
    float reactive = validMotion && isfinite(coverage) ? min(saturate(coverage), 0.9) : 0.9;
    fsrReactive[id.xy] = reactive;
    fsrTransparency[id.xy] = validMotion && isfinite(coverage) ? saturate(coverage) : 1.0;
    diagnostics[id.xy] = float4(0.0, 0.0, 0.0, reactive);
}
