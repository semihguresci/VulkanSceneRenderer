#pragma once

// Reference transport building blocks, independent of raster lighting, LTC,
// DDGI feedback and display transforms. Directions point AWAY from the surface
// in an orthonormal, right-handed shading frame whose normal is +Z.
// GGX visible-normal sampling follows Heitz, JCGT 7(4), 2018:
// https://jcgt.org/published/0007/04/01/
static const float PT_PI = 3.14159265358979323846;
static const uint PT_MAX_ACCUMULATED_SAMPLES = 16777216u;

struct PtBsdf
{
    float3 diffuse;
    float roughness; // glTF perceptual roughness; alpha = roughness squared
    float3 f0;
    float specularScale;
};
struct PtBsdfEvaluation { float3 value; float pdf; uint valid; };
struct PtBsdfSample
{
    float3 direction;
    float pdf; // solid angle density, or discrete probability for a delta
    float3 weight; // complete f * abs(cosine) / mixture PDF
    uint valid;
    uint delta;
    uint rejected; // invalid/unrepresentable input; distinct from a null sample
};
struct PtCameraRay
{
    float3 origin;
    float3 direction;
    float minimum;
    float maximum;
    uint valid;
};

float3 PtNormalize(float3 value)
{
    if (!all(isfinite(value))) return 0.0.xxx;
    float scale = max(max(abs(value.x), abs(value.y)), abs(value.z));
    if (scale == 0.0) return 0.0.xxx;
    value /= scale;
    return value * rsqrt(dot(value, value));
}
float PtLength(float3 value)
{
    float scale = max(max(abs(value.x), abs(value.y)), abs(value.z));
    if (scale == 0.0) return 0.0;
    return scale * length(value / scale);
}
float2 PtCameraNdc(float2 pixel, uint2 extent)
{
    // Negative-height Vulkan viewport: the top image row maps to NDC +Y.
    return float2(2.0 * pixel.x / float(extent.x) - 1.0,
                  1.0 - 2.0 * pixel.y / float(extent.y));
}
PtCameraRay PtGenerateCameraRay(float2 pixel, uint2 extent,
    float4x4 inverseProjection, float4x4 inverseView, bool orthographic)
{
    PtCameraRay result = (PtCameraRay)0;
    if (any(extent == uint2(0, 0)) || !all(isfinite(pixel)) || any(pixel < 0.0.xx) ||
        any(pixel >= float2(extent))) return result;
    for (uint row = 0u; row < 4u; ++row)
        if (!all(isfinite(inverseProjection[row])) || !all(isfinite(inverseView[row]))) return result;
    if (inverseView[3][0] != 0.0 || inverseView[3][1] != 0.0 ||
        inverseView[3][2] != 0.0 || inverseView[3][3] != 1.0) return result;
    // Pixel positions already contain the subpixel sample. Reverse-Z near/far
    // depths are 1/0, with no projection Y flip or extra half-pixel.
    float2 ndc = PtCameraNdc(pixel, extent);
    float4 nearView = mul(inverseProjection, float4(ndc, 1.0, 1.0));
    if (!all(isfinite(nearView)) || nearView.w == 0.0) return result;
    float3 viewOrigin = 0.0.xxx;
    // Work in view space first. Subtracting a world-space near point from the
    // camera loses ray direction precision in large-coordinate scenes.
    float3 viewDirection = nearView.xyz * (nearView.w < 0.0 ? -1.0 : 1.0);
    float4 farView = mul(inverseProjection, float4(ndc, 0.0, 1.0));
    if (!all(isfinite(farView))) return result;
    [branch] if (orthographic)
    {
        if (farView.w == 0.0) return result;
        viewOrigin = nearView.xyz / nearView.w;
        viewDirection = farView.xyz / farView.w - viewOrigin;
        result.minimum = 0.0;
        result.maximum = PtLength(mul(inverseView, float4(viewDirection, 0.0)).xyz);
    }
    else
    {
        result.minimum = PtLength(mul(inverseView, float4(nearView.xyz / nearView.w, 0.0)).xyz);
        // Infinite-far reverse-Z projection has a homogeneous far W of zero.
        [branch] if (farView.w == 0.0)
            result.maximum = 3.402823466e+38;
        else
            result.maximum = PtLength(mul(inverseView, float4(farView.xyz / farView.w, 0.0)).xyz);
    }
    result.origin = mul(inverseView, float4(viewOrigin, 1.0)).xyz;
    result.direction = PtNormalize(mul(inverseView, float4(PtNormalize(viewDirection), 0.0)).xyz);
    if (!all(isfinite(result.origin)) || !PtValidDirection(result.direction) ||
        !isfinite(result.minimum) || !isfinite(result.maximum) ||
        result.minimum < 0.0 || result.maximum <= result.minimum) return (PtCameraRay)0;
    result.valid = 1u;
    return result;
}

float3 PtThinLensDirection(float3 direction, float3 forward, float3 lensOffset, float focusDistance)
{
    // The focal plane is perpendicular to the camera axis, at a distance
    // measured along that axis. Scale by cos(theta) before normalizing to
    // avoid forming a distant world-space point or subtracting large positions.
    return PtNormalize(direction * focusDistance - lensOffset * dot(direction, forward));
}

bool PtApplyThinLens(inout PtCameraRay ray, float4x4 inverseView, float2 random,
    float apertureRadius, float focusDistance, out float3 lensOffset)
{
    const float angle = (2.0 * PT_PI) * random.x;
    const float radius = apertureRadius * sqrt(random.y);
    lensOffset = mul(inverseView, float4(radius * cos(angle), radius * sin(angle), 0.0, 0.0)).xyz;
    const float3 forward = PtNormalize(mul(inverseView, float4(0.0, 0.0, -1.0, 0.0)).xyz);
    const float3 direction = PtThinLensDirection(ray.direction, forward, lensOffset, focusDistance);
    const float axial = dot(direction, forward);
    if (!PtValidDirection(direction) || axial <= 0.0) return false;

    // Keep the original camera near and far planes. Their ray parameters
    // change with the lens direction, while infinite far remains infinite.
    const float scale = dot(ray.direction, forward) / axial;
    const float infiniteFar = 3.402823466e+38;
    ray.origin += lensOffset;
    ray.direction = direction;
    ray.minimum *= scale;
    if (ray.maximum < infiniteFar) ray.maximum = min(ray.maximum * scale, infiniteFar);
    return all(isfinite(ray.origin)) && isfinite(ray.minimum) &&
        ray.minimum >= 0.0 && ray.maximum > ray.minimum;
}

// Returns world-space width and angular spread for a valid camera ray.
float2 PtCameraRayFootprint(float2 pixel, uint2 extent,
    float4x4 inverseProjection, float4x4 inverseView, bool orthographic, float3 direction,
    float3 lensOffset, float focusDistance)
{
    float4 nearView = mul(inverseProjection, float4(PtCameraNdc(pixel, extent), 1.0, 1.0));
    float2 ndcStep = float2(2.0, -2.0) / float2(extent);
    float2 footprint = 0.0.xx;
    float3 forward = focusDistance > 0.0
        ? PtNormalize(mul(inverseView, float4(0.0, 0.0, -1.0, 0.0)).xyz) : 0.0.xxx;
    // Extrapolate the projection by a pixel in both axes, including at the
    // viewport edges and for one-pixel extents.
    [unroll] for (uint axis = 0u; axis < 2u; ++axis)
    {
        float2 step = axis == 0u ? float2(ndcStep.x, 0.0) : float2(0.0, ndcStep.y);
        float4 nearStep = mul(inverseProjection, float4(step, 0.0, 0.0));
        float4 adjacent = nearView + nearStep;
        if (!all(isfinite(adjacent)) || adjacent.w == 0.0) continue;
        if (orthographic)
        {
            // Difference of the two unprojected points before world translation.
            float3 viewStep = (nearStep.xyz - (nearView.xyz / nearView.w) * nearStep.w) / adjacent.w;
            float width = PtLength(mul(inverseView, float4(viewStep, 0.0)).xyz);
            if (isfinite(width)) footprint.x = max(footprint.x, width);
        }
        else
        {
            float3 viewDirection = PtNormalize(adjacent.xyz * (adjacent.w < 0.0 ? -1.0 : 1.0));
            float3 worldDirection = PtNormalize(mul(inverseView, float4(viewDirection, 0.0)).xyz);
            if (focusDistance > 0.0)
                worldDirection = PtThinLensDirection(worldDirection, forward, lensOffset, focusDistance);
            if (PtValidDirection(worldDirection))
                footprint.y = max(footprint.y, PtLength(worldDirection - direction));
        }
    }
    return footprint;
}

uint PtHash(uint value)
{
    // PCG permutation. Unsigned wraparound is intentional.
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float PtRandom(uint pixel, uint sampleIndex, uint bounce, uint dimension, uint seed)
{
    // Address every sample independently: changing frame batch size or dispatch
    // order never changes the random sequence. The 23-bit conversion is strictly
    // inside (0,1), including float32 rounding at both endpoints.
    uint value = PtHash(pixel ^ PtHash(seed));
    value = PtHash(value ^ sampleIndex);
    value = PtHash(value ^ bounce);
    value = PtHash(value ^ dimension);
    return (float(value >> 9u) + 0.5) * (1.0 / 8388608.0);
}
bool PtValidDirection(float3 value)
{
    return all(isfinite(value)) && abs(dot(value, value) - 1.0) < 0.002;
}
bool PtValidBsdf(PtBsdf bsdf)
{
    return all(isfinite(bsdf.diffuse)) && all(bsdf.diffuse >= 0.0.xxx) &&
        all(bsdf.diffuse <= 1.0.xxx) && all(isfinite(bsdf.f0)) &&
        all(bsdf.f0 >= 0.0.xxx) && all(bsdf.f0 <= 1.0.xxx) &&
        isfinite(bsdf.roughness) && bsdf.roughness >= 0.0 && bsdf.roughness <= 1.0 &&
        isfinite(bsdf.specularScale) && bsdf.specularScale >= 0.0 && bsdf.specularScale <= 1.0;
}
float3 PtFresnel(float3 f0, float cosine)
{
    float x = 1.0 - saturate(cosine);
    float x2 = x * x;
    return f0 + (1.0.xxx - f0) * (x2 * x2 * x);
}
float PtLuminance(float3 value) { return dot(value, float3(0.2126, 0.7152, 0.0722)); }
float PtSpecularProbability(PtBsdf bsdf, float3 view)
{
    if (bsdf.specularScale == 0.0) return 0.0;
    if (all(bsdf.diffuse == 0.0.xxx)) return 1.0;
    float specular = PtLuminance(PtFresnel(bsdf.f0, view.z)) * bsdf.specularScale;
    float diffuse = PtLuminance(bsdf.diffuse) * (1.0 - specular);
    // Both lobes need nonzero probability, even when Schlick F0 is zero.
    return clamp(specular / max(specular + diffuse, 1e-30), 0.05, 0.95);
}
float3 PtCosineHemisphere(float2 u)
{
    float radius = sqrt(u.x), angle = 2.0 * PT_PI * u.y;
    return float3(radius * cos(angle), radius * sin(angle), sqrt(max(0.0, 1.0 - u.x)));
}
float3 PtUniformSphere(float2 u)
{
    float z = 1.0 - 2.0 * u.x, radius = sqrt(max(0.0, 1.0 - z * z));
    float angle = 2.0 * PT_PI * u.y;
    return float3(radius * cos(angle), radius * sin(angle), z);
}
float PtGgxDistribution(float3 halfVector, float alpha)
{
    // Rearrangement avoids subtracting nearly equal values at the normal and
    // avoids squaring alpha twice for very small positive roughness.
    float q = dot(halfVector.xy, halfVector.xy) / alpha + alpha * halfVector.z * halfVector.z;
    return 1.0 / (PT_PI * q * q);
}
float3 PtSampleGgxVisibleNormal(float3 view, float alpha, float2 u)
{
    float3 stretched = normalize(float3(alpha * view.xy, view.z));
    float lengthSquared = dot(stretched.xy, stretched.xy);
    float3 tangent;
    [branch] if (lengthSquared > 0.0)
        tangent = float3(-stretched.y, stretched.x, 0.0) * rsqrt(lengthSquared);
    else
        tangent = float3(1.0, 0.0, 0.0);
    float3 bitangent = cross(stretched, tangent);
    float radius = sqrt(u.x), phi = 2.0 * PT_PI * u.y;
    float x = radius * cos(phi), y = radius * sin(phi);
    float blend = 0.5 * (1.0 + stretched.z);
    y = (1.0 - blend) * sqrt(max(0.0, 1.0 - x * x)) + blend * y;
    float3 normal = x * tangent + y * bitangent +
        sqrt(max(0.0, 1.0 - x * x - y * y)) * stretched;
    return normalize(float3(alpha * normal.xy, max(0.0, normal.z)));
}
PtBsdfEvaluation PtEvaluateBsdf(PtBsdf bsdf, float3 view, float3 light)
{
    PtBsdfEvaluation result = (PtBsdfEvaluation)0;
    if (!PtValidBsdf(bsdf) || !PtValidDirection(view) || !PtValidDirection(light)) return result;
    result.valid = 1u;
    if (view.z <= 0.0 || light.z <= 0.0) return result;
    float3 halfVector = normalize(view + light);
    float3 fresnel = PtFresnel(bsdf.f0, dot(view, halfVector)) * bsdf.specularScale;
    float probability = PtSpecularProbability(bsdf, view);
    result.value = (1.0.xxx - fresnel) * bsdf.diffuse / PT_PI;
    result.pdf = (1.0 - probability) * light.z / PT_PI;
    // Exact zero roughness is a delta lobe, not a roughness-clamped highlight.
    [branch] if (bsdf.roughness > 0.0 && bsdf.specularScale > 0.0)
    {
        float alpha = bsdf.roughness * bsdf.roughness;
        float lengthView = length(float3(alpha * view.xy, view.z));
        float lengthLight = length(float3(alpha * light.xy, light.z));
        float distribution = PtGgxDistribution(halfVector, alpha);
        // Height-correlated Smith masking; includes 1/(4 NoV NoL).
        float visibility = 0.5 / (light.z * lengthView + view.z * lengthLight);
        result.value += fresnel * distribution * visibility;
        result.pdf += probability * distribution / (2.0 * (view.z + lengthView));
    }
    if (!all(isfinite(result.value)) || !isfinite(result.pdf) || result.pdf < 0.0)
        return (PtBsdfEvaluation)0;
    return result;
}
PtBsdfSample PtRejectedBsdfSample()
{
    PtBsdfSample result = (PtBsdfSample)0;
    result.rejected = 1u;
    return result;
}
PtBsdfSample PtSampleBsdf(PtBsdf bsdf, float3 view, float3 u)
{
    PtBsdfSample result = (PtBsdfSample)0;
    if (!PtValidBsdf(bsdf) || !PtValidDirection(view) || view.z <= 0.0 ||
        !all(isfinite(u)) || any(u < 0.0.xxx) || any(u >= 1.0.xxx)) return PtRejectedBsdfSample();
    float probability = PtSpecularProbability(bsdf, view);
    [branch] if (u.x < probability)
    {
        [branch] if (bsdf.roughness == 0.0)
        {
            result.direction = float3(-view.xy, view.z);
            result.pdf = probability;
            result.weight = bsdf.specularScale * PtFresnel(bsdf.f0, view.z) / probability;
            result.valid = 1u;
            result.delta = 1u;
            return result;
        }
        float3 halfVector = PtSampleGgxVisibleNormal(view, bsdf.roughness * bsdf.roughness, u.yz);
        result.direction = reflect(-view, halfVector);
        // Below-surface VNDF outcomes are null events. Resampling them would
        // change the PDF and bias the estimator, particularly on rough metals.
        if (result.direction.z <= 0.0) return (PtBsdfSample)0;
    }
    else
        result.direction = PtCosineHemisphere(u.yz);
    PtBsdfEvaluation evaluated = PtEvaluateBsdf(bsdf, view, result.direction);
    if (evaluated.valid == 0u || evaluated.pdf <= 0.0) return PtRejectedBsdfSample();
    result.pdf = evaluated.pdf;
    result.weight = evaluated.value * (result.direction.z / result.pdf);
    if (!all(isfinite(result.weight))) return PtRejectedBsdfSample();
    result.valid = 1u;
    return result;
}
// glTF thin-walled transmission: the opposite-hemisphere GGX lobe is the
// reflection lobe mirrored across the tangent plane. No macroscopic bending or
// eta^2 radiance factor applies to a sheet with the same medium on both sides.
// Solid-volume dielectric interfaces require a separate refractive BSDF.
struct PtThinBsdf
{
    PtBsdf reflection;
    float3 transmission;
    float roughness;
};
bool PtValidThinBsdf(PtThinBsdf bsdf)
{
    return PtValidBsdf(bsdf.reflection) && all(isfinite(bsdf.transmission)) &&
        all(bsdf.transmission >= 0.0.xxx) && all(bsdf.transmission <= 1.0.xxx) &&
        isfinite(bsdf.roughness) && bsdf.roughness >= 0.0 && bsdf.roughness <= 1.0;
}
float PtThinProbability(PtThinBsdf bsdf, float3 view)
{
    if (all(bsdf.transmission == 0.0.xxx)) return 0.0;
    if (bsdf.reflection.specularScale == 0.0 && all(bsdf.reflection.diffuse == 0.0.xxx)) return 1.0;
    float fresnel = PtLuminance(PtFresnel(bsdf.reflection.f0, view.z)) * bsdf.reflection.specularScale;
    float reflected = fresnel + (1.0 - fresnel) * PtLuminance(bsdf.reflection.diffuse);
    float transmitted = (1.0 - fresnel) * PtLuminance(bsdf.transmission);
    // Preserve every active proposal, including a rough Fresnel lobe whose
    // normal-incidence estimate is zero but whose microfacet angles are not.
    return clamp(transmitted / max(reflected + transmitted, 1e-30), 0.05, 0.95);
}
PtBsdfEvaluation PtEvaluateThinBsdf(PtThinBsdf bsdf, float3 view, float3 light)
{
    PtBsdfEvaluation result = (PtBsdfEvaluation)0;
    if (!PtValidThinBsdf(bsdf) || !PtValidDirection(view) || !PtValidDirection(light)) return result;
    if (all(bsdf.transmission == 0.0.xxx)) return PtEvaluateBsdf(bsdf.reflection, view, light);
    result.valid = 1u;
    if (view.z <= 0.0 || light.z == 0.0) return result;
    float probability = PtThinProbability(bsdf, view);
    if (light.z > 0.0)
    {
        result = PtEvaluateBsdf(bsdf.reflection, view, light);
        result.pdf *= 1.0 - probability;
        return result;
    }
    // Zero roughness is a discrete straight-through event and has no density
    // with respect to solid angle; never put it into a continuous MIS PDF.
    if (bsdf.roughness == 0.0) return result;
    float3 mirrored = float3(light.xy, -light.z);
    float3 halfVector = PtNormalize(view + mirrored);
    float alpha = bsdf.roughness * bsdf.roughness;
    float lengthView = length(float3(alpha * view.xy, view.z));
    float lengthLight = length(float3(alpha * mirrored.xy, mirrored.z));
    float distribution = PtGgxDistribution(halfVector, alpha);
    float visibility = 0.5 / (mirrored.z * lengthView + view.z * lengthLight);
    float3 fresnel = PtFresnel(bsdf.reflection.f0, dot(view, halfVector)) * bsdf.reflection.specularScale;
    result.value = bsdf.transmission * (1.0.xxx - fresnel) * distribution * visibility;
    result.pdf = probability * distribution / (2.0 * (view.z + lengthView));
    if (!all(isfinite(result.value)) || !isfinite(result.pdf) || result.pdf < 0.0)
        return (PtBsdfEvaluation)0;
    return result;
}
PtBsdfSample PtSampleThinBsdf(PtThinBsdf bsdf, float3 view, float3 u)
{
    if (!PtValidThinBsdf(bsdf) || !PtValidDirection(view) || view.z <= 0.0 ||
        !all(isfinite(u)) || any(u < 0.0.xxx) || any(u >= 1.0.xxx)) return PtRejectedBsdfSample();
    if (all(bsdf.transmission == 0.0.xxx)) return PtSampleBsdf(bsdf.reflection, view, u);
    float probability = PtThinProbability(bsdf, view);
    PtBsdfSample result = (PtBsdfSample)0;
    if (u.x < probability)
    {
        if (bsdf.roughness == 0.0)
        {
            result.direction = -view;
            result.pdf = probability;
            result.weight = bsdf.transmission * (1.0.xxx -
                PtFresnel(bsdf.reflection.f0, view.z) * bsdf.reflection.specularScale) / probability;
            result.valid = 1u;
            result.delta = 1u;
            return result;
        }
        float3 halfVector = PtSampleGgxVisibleNormal(view, bsdf.roughness * bsdf.roughness, u.yz);
        float3 mirrored = reflect(-view, halfVector);
        if (mirrored.z <= 0.0) return result; // Ordinary VNDF null event, not a rejection.
        result.direction = float3(mirrored.xy, -mirrored.z);
    }
    else
    {
        // Remap into the conditional reflection/diffuse mixture. Bound the
        // rounded division below one without changing the random sequence.
        float choice = min((u.x - probability) / (1.0 - probability), asfloat(0x3f7fffffu));
        result = PtSampleBsdf(bsdf.reflection, view, float3(choice, u.yz));
        if (result.valid == 0u) return result;
        if (result.delta != 0u)
        {
            result.pdf *= 1.0 - probability;
            result.weight /= 1.0 - probability;
            return result;
        }
    }
    PtBsdfEvaluation evaluated = PtEvaluateThinBsdf(bsdf, view, result.direction);
    if (evaluated.valid == 0u || evaluated.pdf <= 0.0) return PtRejectedBsdfSample();
    result.pdf = evaluated.pdf;
    result.weight = evaluated.value * (abs(result.direction.z) / evaluated.pdf);
    if (!all(isfinite(result.weight))) return PtRejectedBsdfSample();
    result.valid = 1u;
    return result;
}
float PtPowerHeuristic(float sampledPdf, float otherPdf)
{
    if (!isfinite(sampledPdf) || sampledPdf <= 0.0 || !isfinite(otherPdf) || otherPdf < 0.0)
        return 0.0;
    float scale = max(sampledPdf, otherPdf);
    float a = sampledPdf / scale, b = otherPdf / scale;
    return a * a / (a * a + b * b);
}
bool PtRussianRoulette(inout float3 throughput, uint depth, uint minimumDepth, float u)
{
    if (!all(isfinite(throughput)) || any(throughput < 0.0.xxx) ||
        !isfinite(u) || u < 0.0 || u >= 1.0) return false;
    float maximum = max(max(throughput.x, throughput.y), throughput.z);
    if (maximum == 0.0) return false;
    if (depth < minimumDepth) return true;
    float survival = min(maximum, 0.95);
    if (u >= survival) return false;
    throughput /= survival;
    return all(isfinite(throughput));
}
bool PtAccumulate(inout float3 mean, inout float3 secondMoment, inout uint count, float3 sample)
{
    // Welford moments in scene-linear radiance. No firefly/radiance clamp or
    // denoising. Reject invalid input without advancing the sample count.
    if (count >= PT_MAX_ACCUMULATED_SAMPLES || !all(isfinite(sample)) || any(sample < 0.0.xxx))
        return false;
    [branch] if (count == 0u)
    {
        mean = sample;
        secondMoment = 0.0.xxx;
        count = 1u;
        return true;
    }
    if (!all(isfinite(mean)) || !all(isfinite(secondMoment)) ||
        any(mean < 0.0.xxx) || any(secondMoment < 0.0.xxx)) return false;
    float3 difference = sample - mean;
    float3 nextMean = mean + difference / float(count + 1u);
    float3 nextMoment = secondMoment + difference * (sample - nextMean);
    if (!all(isfinite(nextMean)) || !all(isfinite(nextMoment))) return false;
    mean = nextMean;
    secondMoment = nextMoment;
    ++count;
    return true;
}
