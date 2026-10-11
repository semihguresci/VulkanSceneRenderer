#pragma once

#include "brdf_common.slang"
#include "push_constants_common.slang"

#ifndef DDGI_SET
#define DDGI_SET 4
#endif
#ifndef DDGI_DISABLED
#define DDGI_DISABLED 0
#endif

struct DdgiGpuVolume
{
    float4 originBlend; // origin.xyz, smooth edge band in cells
    float4 spacingNormalBias;
    uint4 countsOffset; // lattice xyz, first packed probe index
    float4 bias; // world view bias, visibility bias, reserved
};
static const uint DDGI_MAX_VOLUMES = 4u;

// Mirrors DdgiGpuData.h. Atlas interiors exclude their
// one-texel octahedral seam borders; probe indices are X-fast.
struct DdgiGpuData
{
    float4 originNormalBias;
    float4 spacingViewBias;
    uint4 counts; // xyz lattice dimensions, w total probes
    uint4 irradianceLayout; // interior, padded tile, tiles/row, atlas width
    uint4 visibilityLayout;
    uint4 extents; // irradiance height, visibility height, update count, rays/probe
    float4 trace; // finite maximum distance, hysteresis, moment exponent, visibility power
    uint4 frame; // frame number, enabled, read history valid, section plane enabled
    float4 intensityBias; // intensity, visibility bias, volume count, transport flags
    DdgiGpuVolume volumes[4];
};

uint DdgiVolumeCount(DdgiGpuData data)
{
    return data.intensityBias.z > 0.0 ? min(uint(data.intensityBias.z), DDGI_MAX_VOLUMES) : 1u;
}
DdgiGpuVolume DdgiVolumeAt(DdgiGpuData data, uint index)
{
    if (data.intensityBias.z > 0.0) return data.volumes[index];
    // Legacy CPU fixtures retain the original single-volume field contract.
    DdgiGpuVolume volume;
    volume.originBlend = float4(data.originNormalBias.xyz, 1.0);
    volume.spacingNormalBias = float4(data.spacingViewBias.xyz, data.originNormalBias.w);
    volume.countsOffset = uint4(data.counts.xyz, 0u);
    volume.bias = float4(data.spacingViewBias.w, data.intensityBias.y, 0, 0);
    return volume;
}
uint DdgiVolumeProbeCount(DdgiGpuVolume volume)
{
    return volume.countsOffset.x * volume.countsOffset.y * volume.countsOffset.z;
}
DdgiGpuVolume DdgiProbeVolumeForIndex(DdgiGpuData data, uint probeIndex)
{
    uint count = DdgiVolumeCount(data);
    for (uint index = 0u; index < count; ++index)
    {
        DdgiGpuVolume volume = DdgiVolumeAt(data, index);
        if (probeIndex >= volume.countsOffset.w &&
            probeIndex - volume.countsOffset.w < DdgiVolumeProbeCount(volume)) return volume;
    }
    return DdgiVolumeAt(data, 0u);
}

struct DdgiTraceSceneData
{
    float4 sectionPlane;
    BoxClipState boxClip;
    uint4 limits; // exact instance count, UV triangle count, light cap, area samples
};

// Matches DdgiProbeVolume.h; classification is independent of radiance validity.
struct DdgiProbeState
{
    float3 relocation;
    uint classification;
    uint lastUpdatedFrame;
    uint validRayCount;
    uint flags;
    uint reserved; // content epoch of this probe's submitted ray set
};
struct DdgiRaySample
{
    float4 radianceAndDistance;
    // w: 0 miss; 1 supported/two-sided hit; -1 single-sided back hit;
    // 2 unsupported closest geometry. Nonfinite payloads never enter history.
    float4 directionAndReserved;
};
struct DdgiProbeUpdate
{
    uint probeIndex;
    uint firstRay;
    uint rayCount;
    uint reserved; // manager-owned content epoch; preserves the 16-byte ABI
};

static const uint DDGI_PROBE_UNINITIALIZED = 0u;
static const uint DDGI_PROBE_ACTIVE = 1u;
static const uint DDGI_PROBE_INACTIVE = 2u;
static const uint DDGI_PROBE_RELOCATED = 1u;
static const uint DDGI_PROBE_UNSUPPORTED_HIT = 2u;
static const uint DDGI_PROBE_IRRADIANCE_CLAMPED = 4u;
static const uint DDGI_MAX_RAYS_PER_PROBE = 4096u;

float2 DdgiSignNotZero(float2 value)
{
    return float2(value.x < 0.0 ? -1.0 : 1.0, value.y < 0.0 ? -1.0 : 1.0);
}
float2 DdgiOctEncode(float3 direction)
{
    direction = SafeNormalize(direction, float3(0, 0, 1));
    float2 projected = direction.xy / max(dot(abs(direction), 1.0.xxx), 1e-10);
    if (direction.z < 0.0)
        projected = (1.0.xx - abs(projected.yx)) * DdgiSignNotZero(projected);
    return projected * 0.5 + 0.5;
}
float3 DdgiOctDecode(float2 uv)
{
    float2 p = uv * 2.0 - 1.0;
    float3 direction = float3(p, 1.0 - abs(p.x) - abs(p.y));
    if (direction.z < 0.0)
        direction.xy = (1.0.xx - abs(direction.yx)) * DdgiSignNotZero(direction.xy);
    return SafeNormalize(direction, float3(0, 0, 1));
}
uint2 DdgiTileOrigin(uint probeIndex, uint4 layout)
{
    return uint2(probeIndex % max(layout.z, 1u), probeIndex / max(layout.z, 1u)) * layout.y;
}
float2 DdgiAtlasUv(uint probeIndex, float3 direction, uint4 layout, uint height)
{
    // UV=0 reaches the half-way point between the first interior and gutter
    // centers; the folded gutter makes bilinear filtering continuous at seams.
    return (float2(DdgiTileOrigin(probeIndex, layout)) + 1.0.xx +
        DdgiOctEncode(direction) * float(layout.x)) /
        float2(max(layout.w, 1u), max(height, 1u));
}
uint2 DdgiFoldBorder(uint2 local, uint interior)
{
    uint end = interior + 1u;
    bool left = local.x == 0u, right = local.x == end;
    bool top = local.y == 0u, bottom = local.y == end;
    if ((left || right) && (top || bottom))
        return uint2(left ? interior : 1u, top ? interior : 1u);
    if (left || right) return uint2(left ? 1u : interior, end - local.y);
    if (top || bottom) return uint2(end - local.x, top ? 1u : interior);
    return local;
}
uint DdgiLinearIndex(uint3 coordinate, uint3 counts)
{
    return coordinate.x + counts.x * (coordinate.y + counts.y * coordinate.z);
}
uint DdgiHash(uint value)
{
    value ^= value >> 16u; value *= 0x7feb352du;
    value ^= value >> 15u; value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}
float DdgiRandom(uint value) { return float(DdgiHash(value) >> 8u) / 16777216.0; }
uint DdgiSelectedLightIndex(uint stratum, uint sampleCount, uint lightCount, uint seed)
{
    if (lightCount <= sampleCount) return stratum;
    // Partition the full light-index interval into equally sized strata. One
    // uniform pick per stratum, weighted count/sampleCount by the caller, is
    // unbiased even when the counts do not divide evenly. Sources beyond the
    // per-ray cap remain represented as deterministic frame-varying noise.
    float coordinate = (float(stratum) + DdgiRandom(seed + stratum * 277803737u)) /
        float(max(sampleCount, 1u));
    return min(uint(coordinate * float(lightCount)), lightCount - 1u);
}
float3 DdgiRayDirection(uint rayIndex, uint rayCount, uint probeIndex, uint frame)
{
    // Equal solid-angle Fibonacci samples with a deterministic uniform SO(3)
    // rotation. The PDF is 1/(4*pi), including after rotation.
    float y = 1.0 - 2.0 * (float(rayIndex) + 0.5) / float(max(rayCount, 1u));
    float radius = sqrt(max(1.0 - y * y, 0.0));
    float angle = float(rayIndex) * 2.399963229728653;
    float3 direction = float3(radius * cos(angle), y, radius * sin(angle));
    uint seed = DdgiHash(probeIndex * 747796405u + frame * 2891336453u);
    float u = DdgiRandom(seed), a = 2.0 * PI * DdgiRandom(seed + 1u);
    float b = 2.0 * PI * DdgiRandom(seed + 2u);
    float4 rotation = float4(sqrt(1.0 - u) * sin(a), sqrt(1.0 - u) * cos(a),
                            sqrt(u) * sin(b), sqrt(u) * cos(b));
    direction += 2.0 * cross(rotation.xyz,
        cross(rotation.xyz, direction) + rotation.w * direction);
    return SafeNormalize(direction, float3(0, 1, 0));
}
