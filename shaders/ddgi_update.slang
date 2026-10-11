#define DDGI_SET 0
#include "ddgi_data.slang"

[[vk::binding(0, DDGI_SET)]] ConstantBuffer<DdgiGpuData> uDdgi;
[[vk::binding(1, DDGI_SET)]] Texture2D<float4> gDdgiIrradiance;
[[vk::binding(2, DDGI_SET)]] Texture2D<float2> gDdgiVisibility;
[[vk::binding(4, DDGI_SET)]] RWStructuredBuffer<DdgiProbeState> uDdgiStates;
[[vk::binding(5, DDGI_SET)]] StructuredBuffer<DdgiRaySample> uDdgiRays;
[[vk::binding(6, DDGI_SET)]] StructuredBuffer<DdgiProbeUpdate> uDdgiUpdates;
[[vk::binding(7, DDGI_SET)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> gDdgiIrradianceWrite;
[[vk::binding(8, DDGI_SET)]] [[vk::image_format("rg32f")]] RWTexture2D<float2> gDdgiVisibilityWrite;

groupshared DdgiProbeUpdate gUpdate;
groupshared DdgiProbeState gState;
groupshared uint gCanBlend;
groupshared uint gClamped;
groupshared float3 gSpacing;

bool DdgiValidRay(DdgiRaySample sample)
{
    float directionLengthSq = dot(sample.directionAndReserved.xyz, sample.directionAndReserved.xyz);
    return all(isfinite(sample.radianceAndDistance)) && all(isfinite(sample.directionAndReserved)) &&
        sample.radianceAndDistance.w >= 0.0 && sample.radianceAndDistance.w <= uDdgi.trace.x &&
        directionLengthSq > 0.99 && directionLengthSq < 1.01;
}
void DdgiClassifyProbe()
{
    DdgiProbeState previous = uDdgiStates[gUpdate.probeIndex];
    gState = previous;
    gState.validRayCount = 0u;
    gState.flags = 0u;
    uint backCount = 0u;
    float nearestBack = uDdgi.trace.x, nearestFront = uDdgi.trace.x;
    float3 backDirection = 0.0.xxx, frontDirection = 0.0.xxx;
    for (uint ray = 0u; ray < gUpdate.rayCount; ++ray)
    {
        DdgiRaySample sample = uDdgiRays[gUpdate.firstRay + ray];
        if (!DdgiValidRay(sample)) continue;
        ++gState.validRayCount;
        float status = sample.directionAndReserved.w;
        if (status < -0.5)
        {
            ++backCount;
            if (sample.radianceAndDistance.w < nearestBack)
            {
                nearestBack = sample.radianceAndDistance.w;
                backDirection = sample.directionAndReserved.xyz;
            }
        }
        else if (status > 0.5)
        {
            if (status > 1.5) gState.flags |= DDGI_PROBE_UNSUPPORTED_HIT;
            if (sample.radianceAndDistance.w < nearestFront)
            {
                nearestFront = sample.radianceAndDistance.w;
                frontDirection = sample.directionAndReserved.xyz;
            }
        }
    }
    bool inside = gState.validRayCount != 0u && backCount > gState.validRayCount / 2u;
    float clearance = 0.02 * min(min(gSpacing.x, gSpacing.y), gSpacing.z);
    float3 relocation = previous.relocation;
    if (inside && nearestBack < uDdgi.trace.x)
        relocation += backDirection * (nearestBack + clearance);
    else if (nearestFront < clearance)
        relocation -= frontDirection * (clearance - nearestFront);
    // Preserve lattice association; never relocate beyond half a cell. A
    // probe that cannot escape an enclosing solid remains inactive.
    relocation = clamp(relocation, -gSpacing * 0.45, gSpacing * 0.45);
    // Compare movement in cell coordinates. Squaring a fixed world-space
    // epsilon prevents any relocation in sufficiently small valid lattices;
    // normalize first to preserve the same relative threshold at every scale.
    float3 cellMovement = (relocation - previous.relocation) / gSpacing;
    bool moved = dot(cellMovement, cellMovement) > 1e-12;
    if (moved)
    {
        gState.relocation = relocation;
        gState.flags |= DDGI_PROBE_RELOCATED;
        // The ray set describes the old origin. New-origin history becomes
        // visible only after a later complete bounded ray set is submitted.
        gState.validRayCount = 0u;
    }
    gState.classification = inside || moved || gState.validRayCount == 0u ?
        DDGI_PROBE_INACTIVE : DDGI_PROBE_ACTIVE;
    gState.lastUpdatedFrame = uDdgi.frame.x;
    gState.reserved = gUpdate.reserved;
    gCanBlend = uDdgi.frame.z != 0u && previous.classification == DDGI_PROBE_ACTIVE &&
        previous.validRayCount != 0u && previous.reserved == gUpdate.reserved &&
        gState.classification == DDGI_PROBE_ACTIVE && !moved ? 1u : 0u;
}
float3 DdgiIntegrateIrradiance(float3 direction)
{
    float3 irradiance = 0.0.xxx;
    // Uniform sphere PDF=1/(4*pi). Normalize by all generated directions;
    // backface/unsupported/invalid samples contribute zero, never renormalize
    // the surviving hemisphere (which would amplify the energy).
    float normalization = 4.0 * PI / float(max(gUpdate.rayCount, 1u));
    for (uint ray = 0u; ray < gUpdate.rayCount; ++ray)
    {
        DdgiRaySample sample = uDdgiRays[gUpdate.firstRay + ray];
        if (!DdgiValidRay(sample) || sample.directionAndReserved.w < -0.5 ||
            sample.directionAndReserved.w > 1.5) continue;
        float cosine = max(dot(direction, sample.directionAndReserved.xyz), 0.0);
        // Apply normalization before summing and cap each channel at the
        // representable atlas ceiling. Finite huge input cannot overflow the
        // accumulator or create half-float infinities.
        float channelLimit = 65504.0 / max(normalization * cosine, 1e-10);
        if (any(sample.radianceAndDistance.rgb > channelLimit.xxx)) InterlockedOr(gClamped, 1u);
        float3 contribution = min(max(sample.radianceAndDistance.rgb, 0.0.xxx),
            channelLimit.xxx) * (normalization * cosine);
        if (any(irradiance + contribution > 65504.0.xxx)) InterlockedOr(gClamped, 1u);
        irradiance = min(irradiance + contribution, 65504.0.xxx);
    }
    return irradiance;
}
float2 DdgiIntegrateMoments(float3 direction)
{
    float2 moments = 0.0.xx;
    float weights = 0.0;
    float exponent = max(uDdgi.trace.z, 1.0);
    for (uint ray = 0u; ray < gUpdate.rayCount; ++ray)
    {
        DdgiRaySample sample = uDdgiRays[gUpdate.firstRay + ray];
        if (!DdgiValidRay(sample)) continue;
        float weight = pow(max(dot(direction, sample.directionAndReserved.xyz), 0.0), exponent);
        float distance = sample.radianceAndDistance.w;
        // Including backfaces and unsupported hits is essential: lighting
        // rejection must not replace their finite occluder distance with sky.
        moments += float2(distance, distance * distance) * weight;
        weights += weight;
    }
    return weights > 1e-10 ? moments / weights : float2(uDdgi.trace.x, uDdgi.trace.x * uDdgi.trace.x);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    if (group.x >= uDdgi.extents.z) return;
    DdgiProbeUpdate update = uDdgiUpdates[group.x];
    if (update.probeIndex >= uDdgi.counts.w || update.rayCount == 0u ||
        update.rayCount > DDGI_MAX_RAYS_PER_PROBE) return;
    if (lane == 0u)
    {
        gUpdate = update;
        gSpacing = DdgiProbeVolumeForIndex(uDdgi, update.probeIndex).spacingNormalBias.xyz;
        gClamped = 0u;
        DdgiClassifyProbe();
    }
    GroupMemoryBarrierWithGroupSync();
    float hysteresis = gCanBlend != 0u ? clamp(uDdgi.trace.y, 0.0, 0.9999) : 0.0;
    bool active = gState.classification == DDGI_PROBE_ACTIVE;
    uint irradianceTile = uDdgi.irradianceLayout.y;
    uint2 irradianceOrigin = DdgiTileOrigin(update.probeIndex, uDdgi.irradianceLayout);
    for (uint texel = lane; texel < irradianceTile * irradianceTile; texel += 64u)
    {
        uint2 local = uint2(texel % irradianceTile, texel / irradianceTile);
        uint2 folded = DdgiFoldBorder(local, uDdgi.irradianceLayout.x);
        float3 direction = DdgiOctDecode((float2(folded) - 0.5) / float(uDdgi.irradianceLayout.x));
        float3 current = active ? DdgiIntegrateIrradiance(direction) : 0.0.xxx;
        float3 previous = gDdgiIrradiance.Load(int3(irradianceOrigin + folded, 0)).rgb;
        if (!all(isfinite(previous))) previous = current;
        float3 blended = clamp(lerp(current, previous, hysteresis), 0.0.xxx, 65504.0.xxx);
        gDdgiIrradianceWrite[irradianceOrigin + local] = float4(blended, active ? 1.0 : 0.0);
    }
    uint visibilityTile = uDdgi.visibilityLayout.y;
    uint2 visibilityOrigin = DdgiTileOrigin(update.probeIndex, uDdgi.visibilityLayout);
    for (uint texel = lane; texel < visibilityTile * visibilityTile; texel += 64u)
    {
        uint2 local = uint2(texel % visibilityTile, texel / visibilityTile);
        uint2 folded = DdgiFoldBorder(local, uDdgi.visibilityLayout.x);
        float3 direction = DdgiOctDecode((float2(folded) - 0.5) / float(uDdgi.visibilityLayout.x));
        float2 current = DdgiIntegrateMoments(direction);
        float2 previous = gDdgiVisibility.Load(int3(visibilityOrigin + folded, 0));
        if (!all(isfinite(previous))) previous = current;
        float2 blended = max(lerp(current, previous, hysteresis), 0.0.xx);
        blended.y = max(blended.y, blended.x * blended.x);
        gDdgiVisibilityWrite[visibilityOrigin + local] = blended;
    }
    // Borders are computed from exactly the same folded interior coordinates
    // and history addresses, avoiding cross-invocation image read/write races.
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0u)
    {
        if (gClamped != 0u) gState.flags |= DDGI_PROBE_IRRADIANCE_CLAMPED;
        uDdgiStates[update.probeIndex] = gState;
    }
}
