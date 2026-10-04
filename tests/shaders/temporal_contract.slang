#include "temporal_common.slang"

// Compile probe: dynamic buffer inputs keep every contract helper live in
// SPIR-V. This checks compilation/validation, not GPU numerical parity.
struct TemporalProbeInput
{
    float4 currentClip;
    float4 previousClip;
    float4 jitterPixelsAndExtent;
    float4 rasterUvAndPreviousJitterUv;
    float4 exposureAndValidity;
};

[[vk::binding(0, 0)]] StructuredBuffer<TemporalProbeInput> inputs;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> results;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    TemporalProbeInput input = inputs[id.x];
    TemporalJitter jitter = temporalMakeJitter(
        input.jitterPixelsAndExtent.xy, uint2(input.jitterPixelsAndExtent.zw));
    TemporalProjectedPoint projected = temporalProjectSceneClip(input.currentClip);
    TemporalMotionVector motion = temporalMotionFromUnjitteredClips(
        input.currentClip, input.previousClip, input.exposureAndValidity.z > 0.0);
    TemporalReprojectedAddress history = temporalReprojectToPreviousGrid(
        input.rasterUvAndPreviousJitterUv.xy, motion, jitter.uv, float2(0.0));
    TemporalReprojectedAddress depth = temporalReprojectToPreviousGrid(
        input.rasterUvAndPreviousJitterUv.xy, motion, jitter.uv,
        input.rasterUvAndPreviousJitterUv.zw);
    TemporalExposureScale exposure = temporalHistoryExposureScale(
        input.exposureAndValidity.x, input.exposureAndValidity.y);
    uint base = id.x * 7;
    results[base] = float4(jitter.uv, jitter.ndc);
    results[base + 1] = float4(projected.uv, projected.reverseZDepth, projected.valid ? 1.0 : 0.0);
    results[base + 2] = float4(motion.uv, motion.previousReverseZDepth, motion.valid ? 1.0 : 0.0);
    results[base + 3] = float4(history.uv, history.valid ? 1.0 : 0.0, jitter.valid ? 1.0 : 0.0);
    results[base + 4] = float4(depth.uv, depth.valid ? 1.0 : 0.0, temporalInsideImage(projected.uv) ? 1.0 : 0.0);
    results[base + 5] = float4(exposure.value, exposure.valid ? 1.0 : 0.0, 0.0, 0.0);
    results[base + 6] = float4(
        temporalSceneNdcToUv(input.currentClip.xy),
        temporalSceneUvToNdc(input.rasterUvAndPreviousJitterUv.xy));
}
