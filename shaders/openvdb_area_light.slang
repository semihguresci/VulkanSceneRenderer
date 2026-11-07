// Bounded area-emitter single scattering. Included after the volume's medium
// transmittance and phase helpers; shared scene helpers define radiance/units.
#pragma once

#include "volume_area_light_common.slang"

[[vk::binding(15, 0)]] StructuredBuffer<AreaLightData> volumeAreaLights;

float3 AreaSource(float3 world, float3 viewDirection, float3 coefficients[4],
                 float3 mixtureAlbedo, bool multipleScattering)
{
    float3 result = 0.0.xxx;
    uint count = min(lighting.areaLightCount, VOLUME_MAX_AREA_LIGHTS);
    [loop] for (uint index = 0u; index < count; ++index)
    {
        AreaLightData light = volumeAreaLights[index];
        if (!AreaLightHasRenderableRadiance(light) || !all(isfinite(light.positionRange)) ||
            !all(isfinite(light.directionType)) || !all(isfinite(light.tangentHalfSize)) ||
            !all(isfinite(light.bitangentHalfSize))) continue;
        AreaLightFrame frame = BuildAreaLightFrame(light);
        if (!isfinite(frame.area) || frame.area <= 0.0) continue;
        float3 radiance = max(light.colorIntensity.rgb, 0.0.xxx) * max(light.colorIntensity.a, 0.0);
        float weights[4];
        float distances[4];
        float3 directions[4];
        float solidAngle = 0.0;
        [unroll] for (uint sample = 0u; sample < VOLUME_AREA_LIGHT_SAMPLES; ++sample)
        {
            weights[sample] = 0.0;
            distances[sample] = 0.0;
            directions[sample] = frame.normal;
            float3 position = VolumeAreaSamplePosition(frame, sample);
            float3 toLight;
            // dOmega = cos(theta_emitter) * dA / distance^2. There is no
            // receiving-surface cosine in a participating medium.
            float weight = AreaLightPositionContributionWeight(light, frame, world, position,
                1.0 / float(VOLUME_AREA_LIGHT_SAMPLES), toLight);
            float distance = length(position - world);
            if (!isfinite(weight) || weight <= 0.0 || !isfinite(distance) || distance <= 1e-5) continue;
            weights[sample] = weight;
            distances[sample] = distance;
            directions[sample] = toLight;
            solidAngle += weight;
        }
        if (!isfinite(solidAngle) || solidAngle <= 0.0) continue;
        // A planar one-sided emitter subtends at most a hemisphere. Four-point
        // quadrature can overshoot near an emitter sample; preserve the far
        // field and bound that otherwise singular near-field estimate.
        float angularScale = min(1.0, (2.0 * PI) / solidAngle);
        float geometryVisibility = VolumeAreaGeometryVisibility(world, index, light, frame);
        float phaseMass[4] = {0.0, 0.0, 0.0, 0.0};
        float3 higherOrderMass[4] = {0.0.xxx, 0.0.xxx, 0.0.xxx, 0.0.xxx};
        [unroll] for (uint sample = 0u; sample < VOLUME_AREA_LIGHT_SAMPLES; ++sample)
        {
            if (weights[sample] <= 0.0) continue;
            float3 toLight = directions[sample];
            float visibility = LightTransmittance(world, toLight, distances[sample], 4u);
            // PhaseWeight is HG relative to isotropic; divide by 4pi to
            // integrate the scene's emitted radiance over emitter solid angle.
            float weight = weights[sample] * angularScale * visibility / (4.0 * PI);
            [unroll] for (uint medium = 0u; medium < 4u; ++medium)
                if (medium < MediumCount()) {
                    phaseMass[medium] += weight * PhaseWeight(dot(viewDirection, toLight),
                                                              media[medium].scatteringAnisotropy.w);
                    if (multipleScattering)
                        higherOrderMass[medium] += weight *
                            VolumeMultipleScatteringGain(medium, mixtureAlbedo, visibility);
                }
        }
        [unroll] for (uint medium = 0u; medium < 4u; ++medium)
        {
            if (medium >= MediumCount()) continue;
            float visibility = (media[medium].flags.x & 16u) != 0u ? geometryVisibility : 1.0;
            // A normalized phase distribution integrates to one over the full
            // sphere. Cap a narrow HG lobe that the four samples cannot resolve.
            result += radiance * coefficients[medium] * saturate(phaseMass[medium]) * visibility;
            if (multipleScattering) {
                // Keep the existing unresolved-lobe cap. Additional orders may
                // fill unused phase mass but never reduce the direct result.
                float3 extra = min(higherOrderMass[medium], (1.0 - saturate(phaseMass[medium])).xxx);
                result += radiance * coefficients[medium] * max(extra, 0.0.xxx) * visibility;
            }
        }
    }
    return result;
}
