// Optional bounded, local light-path return closure. This is a bespoke preview
// approximation, not a spatial multiple-scattering solver. Included after the
// combined-medium extinction and direct-scattering helpers.
#pragma once

bool VolumeMultipleScatteringEnabled()
{
    [unroll] for (uint index = 0u; index < 4u; ++index)
        if (index < MediumCount() && (media[index].flags.x & 1u) != 0u &&
            media[index].temperatureControls.z > 0.0) return true;
    return false;
}

float3 VolumeMultipleScatteringGain(uint index, float3 mixtureAlbedo, float lightTransmittance)
{
    float strength = saturate(media[index].temperatureControls.z);
    if (strength <= 0.0) return 0.0.xxx;
    // A light-path collision probability is used as a local return proxy. The
    // combined medium's RGB albedo supplies the fraction available to scatter
    // again. Each subsequent order is treated as isotropic. In each channel:
    // q <= 1-T, so T * sum(n=0..N, q^n) <= 1. Strength scales only extra orders.
    // This is a bound on the closure's path gain, not a global flux estimate.
    float3 q = saturate(mixtureAlbedo) * (1.0 - saturate(lightTransmittance));
    float3 power = q;
    float3 gain = 0.0.xxx;
    uint orders = clamp(media[index].flags.y, 1u, 4u);
    [unroll] for (uint order = 0u; order < 4u; ++order)
    {
        if (order < orders) gain += power;
        power *= q;
    }
    return strength * gain;
}

float3 VolumeMultipleScatteringSource(float3 coefficients[4], float3 mixtureAlbedo,
                                      float lightTransmittance, float geometryVisibility)
{
    float3 result = 0.0.xxx;
    [unroll] for (uint index = 0u; index < 4u; ++index)
    {
        if (index >= MediumCount()) continue;
        float visibility = (media[index].flags.x & 16u) != 0u ? geometryVisibility : 1.0;
        result += coefficients[index] * visibility *
                  VolumeMultipleScatteringGain(index, mixtureAlbedo, lightTransmittance);
    }
    return result;
}
