#pragma once
#include "ddgi_data.slang"

struct DdgiLookupResult
{
    float3 irradiance; // physical incident E; no receiver albedo or exposure
    float coverage; // 0 missing/invalid support; 1 valid response, including occluded black
};

#if defined(DDGI_DISABLED) && DDGI_DISABLED
DdgiLookupResult LookupDdgiIrradiance(float3 world, float3 geometricNormal,
                                     float3 shadingNormal, float3 viewDirection)
{
    return (DdgiLookupResult)0;
}
#else
[[vk::binding(0, DDGI_SET)]] ConstantBuffer<DdgiGpuData> uDdgi;
[[vk::binding(1, DDGI_SET)]] Texture2D<float4> gDdgiIrradiance;
[[vk::binding(2, DDGI_SET)]] Texture2D<float2> gDdgiVisibility;
[[vk::binding(3, DDGI_SET)]] SamplerState gDdgiSampler;
[[vk::binding(4, DDGI_SET)]] StructuredBuffer<DdgiProbeState> uDdgiStates;
[[vk::binding(11, DDGI_SET)]] StructuredBuffer<float4> uDdgiBasePositions;

// Spatial vectors can be far smaller than unit normals. Rescale before
// squaring so a valid small lattice keeps its direction and distance instead
// of using SafeNormalize's unit-vector tolerance or underflowing length().
float3 DdgiNormalizeDisplacement(float3 value, float3 fallback)
{
    float scale = max(max(abs(value.x), abs(value.y)), abs(value.z));
    if (!all(isfinite(value)) || scale <= 0.0) return fallback;
    float3 scaled = value / scale;
    return scaled * rsqrt(dot(scaled, scaled));
}
float DdgiDisplacementLength(float3 value)
{
    float scale = max(max(abs(value.x), abs(value.y)), abs(value.z));
    if (scale <= 0.0) return 0.0;
    float3 scaled = value / scale;
    return scale * sqrt(dot(scaled, scaled));
}

// Binary-search the actual CPU-rounded nodes instead of origin/spacing alone.
// This preserves an exact probe-center contribution at large BIM coordinates.
void DdgiAxisNeighbors(DdgiGpuVolume volume, float position, uint axis, out uint lower, out uint upper,
                       out float fraction)
{
    uint count = volume.countsOffset[axis];
    lower = 0u; upper = 0u; fraction = 0.0;
    if (count <= 1u) return;
    uint stride = axis == 0u ? 1u : axis == 1u ? volume.countsOffset.x :
                                    volume.countsOffset.x * volume.countsOffset.y;
    uint offset = volume.countsOffset.w;
    uint begin = 0u, end = count - 1u;
    while (end - begin > 1u)
    {
        uint middle = begin + (end - begin) / 2u;
        if (uDdgiBasePositions[offset + middle * stride][axis] <= position) begin = middle;
        else end = middle;
    }
    lower = begin; upper = end;
    float a = uDdgiBasePositions[offset + lower * stride][axis];
    float b = uDdgiBasePositions[offset + upper * stride][axis];
    // CPU validation guarantees distinct finite neighboring nodes. Preserve
    // tiny valid lattices rather than introducing a fixed world-unit epsilon.
    fraction = b > a ? saturate((position - a) / (b - a)) : 0.0;
}

DdgiLookupResult LookupDdgiVolume(DdgiGpuVolume volume, float3 world, float3 geometricNormal,
                                  float3 shadingNormal, float3 viewDirection)
{
    DdgiLookupResult result = (DdgiLookupResult)0;
    if (uDdgi.frame.y == 0u || uDdgi.frame.z == 0u || uDdgi.counts.w == 0u ||
        any(volume.countsOffset.xyz == 0u) || !all(isfinite(world))) return result;
    uint offset = volume.countsOffset.w;
    uint count = DdgiVolumeProbeCount(volume);
    if (offset >= uDdgi.counts.w || count > uDdgi.counts.w - offset) return result;
    float3 minimum = uDdgiBasePositions[offset].xyz;
    float3 maximum = uDdgiBasePositions[offset + count - 1u].xyz;
    // Rasterized depth and inverse projection can reconstruct a boundary
    // surface beyond its actual plane by more than a few float ULPs. Allow a
    // bounded coverage margin of 1/1024 of a cell, then clamp interpolation
    // coordinates. This is an intentional tiny region, not a claim of pure
    // arithmetic roundoff. Visibility retains the actual receiver and biases.
    float3 allowance = abs(volume.spacingNormalBias.xyz) / 1024.0;
    if (any(minimum - world > allowance) || any(world - maximum > allowance)) return result;
    float3 interpolationWorld = clamp(world, minimum, maximum);
    geometricNormal = SafeNormalize(geometricNormal, float3(0, 1, 0));
    shadingNormal = SafeNormalize(shadingNormal, geometricNormal);
    viewDirection = SafeNormalize(viewDirection, geometricNormal);
    float3 biasedWorld = world + geometricNormal * max(volume.spacingNormalBias.w, 0.0) +
        viewDirection * max(volume.bias.x, 0.0);
    uint3 lower, upper; float3 fraction;
    DdgiAxisNeighbors(volume, interpolationWorld.x, 0u, lower.x, upper.x, fraction.x);
    DdgiAxisNeighbors(volume, interpolationWorld.y, 1u, lower.y, upper.y, fraction.y);
    DdgiAxisNeighbors(volume, interpolationWorld.z, 2u, lower.z, upper.z, fraction.z);
    float weightSum = 0.0;
    bool hasValidFacingSupport = false;
    for (uint corner = 0u; corner < 8u; ++corner)
    {
        bool3 high = bool3((corner & 1u) != 0u, (corner & 2u) != 0u, (corner & 4u) != 0u);
        uint3 node = uint3(high.x ? upper.x : lower.x, high.y ? upper.y : lower.y,
                          high.z ? upper.z : lower.z);
        float3 axisWeight = float3(high.x ? fraction.x : 1.0 - fraction.x,
                                  high.y ? fraction.y : 1.0 - fraction.y,
                                  high.z ? fraction.z : 1.0 - fraction.z);
        float weight = axisWeight.x * axisWeight.y * axisWeight.z;
        if (weight <= 0.0) continue;
        uint probeIndex = offset + DdgiLinearIndex(node, volume.countsOffset.xyz);
        DdgiProbeState state = uDdgiStates[probeIndex];
        if (state.classification != DDGI_PROBE_ACTIVE || state.validRayCount == 0u ||
            !all(isfinite(state.relocation))) continue;
        float3 probe = uDdgiBasePositions[probeIndex].xyz + state.relocation;
        float3 delta = biasedWorld - probe;
        float distance = DdgiDisplacementLength(delta);
        if (!isfinite(distance) || distance > uDdgi.trace.x) continue;
        float3 probeToWorld = DdgiNormalizeDisplacement(delta, geometricNormal);
        float2 moments = gDdgiVisibility.SampleLevel(gDdgiSampler,
            DdgiAtlasUv(probeIndex, probeToWorld, uDdgi.visibilityLayout, uDdgi.extents.y), 0.0);
        float3 irradiance = gDdgiIrradiance.SampleLevel(gDdgiSampler,
            DdgiAtlasUv(probeIndex, shadingNormal, uDdgi.irradianceLayout, uDdgi.extents.x), 0.0).rgb;
        if (!all(isfinite(moments)) || !all(isfinite(irradiance)) || moments.x < 0.0) continue;
        float difference = max(distance - moments.x - max(volume.bias.y, 0.0), 0.0);
        float visibility = 1.0;
        if (difference > 0.0)
        {
            // Chebyshev is invariant under a change of world units. Evaluate
            // its ratio in dimensionless coordinates instead of introducing a
            // fixed world-distance-squared floor. Divide the second moment in
            // two steps so tiny scale squared cannot underflow the divisor.
            float momentScale = max(max(distance, moments.x), sqrt(max(moments.y, 0.0)));
            float mean = moments.x / momentScale;
            float second = (moments.y / momentScale) / momentScale;
            float variance = max(second - mean * mean, 0.0);
            float normalizedDifference = difference / momentScale;
            float denominator = variance + normalizedDifference * normalizedDifference;
            visibility = denominator > 0.0 ? variance / denominator : 0.0;
        }
        visibility = pow(saturate(visibility), max(uDdgi.trace.w, 1.0));
        float3 surfaceToProbe = DdgiNormalizeDisplacement(probe - world, geometricNormal);
        float normalWeight = saturate((dot(surfaceToProbe, geometricNormal) + 0.2) / 1.2);
        normalWeight *= normalWeight;
        // The exact center has no preferred direction; avoid rejecting its own
        // data just because the fallback direction happens to oppose the normal.
        if (all(probe == world)) normalWeight = 1.0;
        if (normalWeight > 0.0) hasValidFacingSupport = true;
        weight *= visibility * normalWeight;
        result.irradiance += max(irradiance, 0.0.xxx) * weight;
        weightSum += weight;
    }
    if (weightSum > 1e-8 && all(isfinite(result.irradiance)))
    {
        result.irradiance /= weightSum;
        result.coverage = 1.0;
    }
    else
    {
        result.irradiance = 0.0.xxx;
        // Occlusion is a valid black GI response, not missing probe data. A
        // zero visibility weight must not reactivate the bright diffuse sky
        // and ambient-floor fallback through a closed wall.
        result.coverage = hasValidFacingSupport ? 1.0 : 0.0;
    }
    return result;
}

float DdgiVolumeEdgeWeight(DdgiGpuVolume volume, float3 world)
{
    if (volume.originBlend.w <= 0.0) return 1.0;
    uint count = DdgiVolumeProbeCount(volume);
    float3 minimum = uDdgiBasePositions[volume.countsOffset.w].xyz;
    float3 maximum = uDdgiBasePositions[volume.countsOffset.w + count - 1u].xyz;
    float weight = 1.0;
    for (uint axis = 0u; axis < 3u; ++axis)
    {
        if (volume.countsOffset[axis] <= 1u) continue;
        float distance = min(world[axis] - minimum[axis], maximum[axis] - world[axis]);
        weight = min(weight, saturate((distance / volume.spacingNormalBias[axis]) /
                                     volume.originBlend.w));
    }
    return weight * weight * (3.0 - 2.0 * weight);
}

DdgiLookupResult LookupDdgiIrradiance(float3 world, float3 geometricNormal,
                                     float3 shadingNormal, float3 viewDirection)
{
    DdgiLookupResult result = (DdgiLookupResult)0;
    if (uDdgi.frame.y == 0u || uDdgi.frame.z == 0u || uDdgi.counts.w == 0u ||
        !all(isfinite(world))) return result;
    uint count = DdgiVolumeCount(uDdgi);
    uint order[4];
    for (uint i = 0u; i < count; ++i) order[i] = i;
    // Four bounded descriptors keep sorting deterministic and permit a fine
    // room volume to blend into a coarse site volume independent of UI order.
    for (uint i = 0u; i < count; ++i)
        for (uint j = i + 1u; j < count; ++j)
        {
            float3 a = DdgiVolumeAt(uDdgi, order[i]).spacingNormalBias.xyz;
            float3 b = DdgiVolumeAt(uDdgi, order[j]).spacingNormalBias.xyz;
            float sa = max(max(a.x, a.y), a.z), sb = max(max(b.x, b.y), b.z);
            if (sa < sb || (sa == sb && order[i] < order[j]))
            {
                uint previous = order[i]; order[i] = order[j]; order[j] = previous;
            }
        }
    for (uint i = 0u; i < count; ++i)
    {
        DdgiGpuVolume volume = DdgiVolumeAt(uDdgi, order[i]);
        DdgiLookupResult sample = LookupDdgiVolume(volume, world, geometricNormal,
                                                   shadingNormal, viewDirection);
        if (sample.coverage == 0.0) continue;
        float weight = result.coverage > 0.0 ? DdgiVolumeEdgeWeight(volume, world) : 1.0;
        result.irradiance = lerp(result.irradiance, sample.irradiance, weight);
        // Valid occluded black remains support: never turn it into sky fallback.
        result.coverage = 1.0;
    }
    return result;
}
#endif
