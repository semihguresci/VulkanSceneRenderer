#ifndef TEMPORAL_COMMON_SLANG
#define TEMPORAL_COMMON_SLANG

// Pure math mirrored in TemporalConventions.h; these local structs are not a
// storage-buffer ABI. See docs/temporal-rendering.md before consuming history.
struct TemporalJitter
{
    float2 uv;
    float2 ndc;
    bool valid;
};

struct TemporalProjectedPoint
{
    float2 uv;
    float reverseZDepth;
    bool valid;
};

struct TemporalMotionVector
{
    float2 uv;
    float previousReverseZDepth;
    bool valid;
};

struct TemporalReprojectedAddress
{
    float2 uv;
    bool valid;
};

struct TemporalExposureScale
{
    float value;
    bool valid;
};

// Finite input required. Scene negative-height viewport maps NDC +Y to UV -Y.
float2 temporalSceneNdcToUv(float2 ndc)
{
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

float2 temporalSceneUvToNdc(float2 uv)
{
    return float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
}

TemporalJitter temporalMakeJitter(float2 pixels, uint2 extent)
{
    TemporalJitter result = {};
    if (!all(isfinite(pixels)) || any(extent == uint2(0)))
        return result;
    float2 uv = pixels / float2(extent);
    float2 ndc = float2(2.0 * uv.x, -2.0 * uv.y);
    if (!all(isfinite(uv)) || !all(isfinite(ndc)))
        return result;
    result.uv = uv;
    result.ndc = ndc;
    result.valid = true;
    return result;
}

TemporalProjectedPoint temporalProjectSceneClip(float4 clip)
{
    TemporalProjectedPoint result = {};
    if (!all(isfinite(clip)) || clip.w <= 0.0)
        return result;
    float3 ndc = clip.xyz / clip.w;
    float2 uv = temporalSceneNdcToUv(ndc.xy);
    if (!all(isfinite(uv)) || !isfinite(ndc.z) || ndc.z < 0.0 || ndc.z > 1.0)
        return result;
    result.uv = uv;
    result.reverseZDepth = ndc.z;
    result.valid = true;
    return result;
}

TemporalMotionVector temporalMotionFromUnjitteredClips(
    float4 currentClip, float4 previousClip, bool previousSurfaceValid)
{
    TemporalMotionVector result = {};
    if (!previousSurfaceValid)
        return result;
    TemporalProjectedPoint current = temporalProjectSceneClip(currentClip);
    TemporalProjectedPoint previous = temporalProjectSceneClip(previousClip);
    float2 velocity = previous.uv - current.uv;
    if (!current.valid || !previous.valid || !all(isfinite(velocity)))
        return result;
    result.uv = velocity;
    result.previousReverseZDepth = previous.reverseZDepth;
    result.valid = true;
    return result;
}

bool temporalInsideImage(float2 uv)
{
    return all(isfinite(uv)) && all(uv >= 0.0) && all(uv < 1.0);
}

// Previous grid jitter: zero for resolved color, previous jitter for raw depth.
TemporalReprojectedAddress temporalReprojectToPreviousGrid(
    float2 currentRasterUv, TemporalMotionVector motion,
    float2 currentJitterUv, float2 previousGridJitterUv)
{
    TemporalReprojectedAddress result = {};
    if (!motion.valid || !all(isfinite(currentRasterUv)) ||
        !all(isfinite(motion.uv)) || !all(isfinite(currentJitterUv)) ||
        !all(isfinite(previousGridJitterUv)))
        return result;
    float2 uv = currentRasterUv + motion.uv - currentJitterUv + previousGridJitterUv;
    if (!temporalInsideImage(uv))
        return result;
    result.uv = uv;
    result.valid = true;
    return result;
}

TemporalExposureScale temporalHistoryExposureScale(
    float currentPreExposure, float previousPreExposure)
{
    TemporalExposureScale result = {};
    if (!isfinite(currentPreExposure) || !isfinite(previousPreExposure) ||
        currentPreExposure <= 0.0 || previousPreExposure <= 0.0)
        return result;
    float value = currentPreExposure / previousPreExposure;
    if (!isfinite(value) || value <= 0.0)
        return result;
    result.value = value;
    result.valid = true;
    return result;
}

#endif
