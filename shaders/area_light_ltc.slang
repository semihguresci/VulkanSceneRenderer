#pragma once
#include "area_light_common.slang"

// LTC integration adapts selfshadow/ltc_code (see materials/ltc/LICENSE.txt).
// Heitz, Dupuy, Hill and Neubelt, "Real-Time Polygonal-Light Shading with
// Linearly Transformed Cosines", ACM SIGGRAPH 2016.
// https://eheitzresearch.wordpress.com/415-2/
// These LUTs are fitted to this renderer's clamped GGX/Schlick equations,
// rather than substituting the reference implementation's Smith GGX model.
[[vk::binding(5, 1)]] Texture2D<float4> areaLtcMatrix;
[[vk::binding(6, 1)]] Texture2D<float4> areaLtcAmplitude;
[[vk::binding(7, 1)]] SamplerState areaLtcSampler;

static const float AREA_LTC_LUT_SIZE = 64.0;
static const uint AREA_LTC_DISK_SEGMENTS = 32u;
static const uint AREA_LTC_DISK_MAX_SUBDIVISIONS = 512u;
static const float AREA_LTC_DISK_CURVATURE_TOLERANCE = 0.002;

struct AreaLightDirectTerms
{
    float3 stable;
    float3 fresnel;
};

struct AreaLightLtcTerms
{
    AreaLightDirectTerms lighting;
    float3 diffuseCorrectionScale;
    float3 diffuseCorrectionLimit;
    float stableWeight;
    float fresnelWeight;
};

float2 AreaLtcUv(float roughness, float NdotV)
{
    float2 uv = float2(clamp(FiniteScalarOr(roughness, 1.0), 0.045, 1.0),
                       sqrt(1.0 - saturate(NdotV)));
    return uv * ((AREA_LTC_LUT_SIZE - 1.0) / AREA_LTC_LUT_SIZE) +
        (0.5 / AREA_LTC_LUT_SIZE).xx;
}

float3 AreaLtcTransformLocal(float3 local, float4 packedInverse)
{
    // Reference packing is GLM [column][row]; Slang's explicit rows differ.
    return float3(packedInverse.x * local.x + packedInverse.z * local.z,
                  local.y,
                  packedInverse.y * local.x + packedInverse.w * local.z);
}

float3 AreaLtcToLocal(float3 value, float3 T, float3 B, float3 N)
{
    return float3(dot(value, T), dot(value, B), dot(value, N));
}

float AreaLtcClosestChordDistance(float3 a, float3 b)
{
    float3 chord = b - a;
    float t = saturate(-dot(a, chord) / max(dot(chord, chord), 1.0e-20));
    return length(a + t * chord);
}

float AreaLtcIntegrateEdge(float3 a, float3 b)
{
    float lengthSqA = dot(a, a);
    float lengthSqB = dot(b, b);
    if (min(lengthSqA, lengthSqB) <= 1.0e-20) return 0.0;
    a *= rsqrt(lengthSqA);
    b *= rsqrt(lengthSqB);
    float x = clamp(dot(a, b), -1.0, 1.0);
    float3 edgeNormal = cross(a, b);
    if (x < -0.999)
    {
        // Dot can round to -1 while the cross product still describes a
        // valid near-antiparallel edge. Use its sine instead of 1-dot^2.
        float sine = length(edgeNormal);
        return sine > 1.0e-10 ?
            edgeNormal.z * atan2(sine, x) / (2.0 * PI * sine) : 0.0;
    }
    float y = abs(x);
    float numerator = 0.8543985 + (0.4965155 + 0.0145206 * y) * y;
    float denominator = 3.4175940 + (4.1616724 + y) * y;
    float v = numerator / denominator;
    // theta/(2*pi*sin(theta)); the resulting integral is cosine/pi.
    float edgeScale = x > 0.0 ? v :
        0.5 * rsqrt(max(1.0 - x * x, 1.0e-7)) - v;
    return edgeNormal.z * edgeScale;
}

void AreaLtcAccumulateClippedEdge(float3 previous, float3 current,
                                  inout float integral,
                                  inout float3 horizonEntry,
                                  inout float3 horizonExit,
                                  inout bool crossesHorizon)
{
    bool previousInside = previous.z > 0.0;
    bool currentInside = current.z > 0.0;
    if (previousInside && currentInside)
    {
        integral += AreaLtcIntegrateEdge(previous, current);
    }
    else if (previousInside != currentInside)
    {
        float3 intersection = previousInside ?
            previous.z * current - current.z * previous :
            current.z * previous - previous.z * current;
        intersection.z = 0.0;
        if (previousInside)
        {
            integral += AreaLtcIntegrateEdge(previous, intersection);
            horizonExit = intersection;
        }
        else
        {
            integral += AreaLtcIntegrateEdge(intersection, current);
            horizonEntry = intersection;
        }
        crossesHorizon = true;
    }
}

void AreaLtcAccumulateReceiverEdge(float3 previous, float3 current,
                                   float4 packedInverse,
                                   inout float integral,
                                   inout float3 horizonEntry,
                                   inout float3 horizonExit,
                                   inout bool crossesHorizon,
                                   inout float3 receiverEntry,
                                   inout float3 receiverExit,
                                   inout bool crossesReceiver)
{
    // A fitted LTC has a tilted hemisphere. Clip the physical receiver
    // hemisphere first so no fitted lobe can illuminate its back side.
    bool previousInside = previous.z > 0.0;
    bool currentInside = current.z > 0.0;
    if (previousInside && currentInside)
    {
        AreaLtcAccumulateClippedEdge(
            AreaLtcTransformLocal(previous, packedInverse),
            AreaLtcTransformLocal(current, packedInverse), integral,
            horizonEntry, horizonExit, crossesHorizon);
    }
    else if (previousInside != currentInside)
    {
        float3 intersection = previousInside ?
            previous.z * current - current.z * previous :
            current.z * previous - previous.z * current;
        intersection.z = 0.0;
        if (previousInside)
        {
            AreaLtcAccumulateClippedEdge(
                AreaLtcTransformLocal(previous, packedInverse),
                AreaLtcTransformLocal(intersection, packedInverse), integral,
                horizonEntry, horizonExit, crossesHorizon);
            receiverExit = intersection;
        }
        else
        {
            AreaLtcAccumulateClippedEdge(
                AreaLtcTransformLocal(intersection, packedInverse),
                AreaLtcTransformLocal(current, packedInverse), integral,
                horizonEntry, horizonExit, crossesHorizon);
            receiverEntry = intersection;
        }
        crossesReceiver = true;
    }
}

float AreaLtcPolygonIntegral(AreaLightFrame frame, float3 worldPosition,
                             float3 N, float3 V, float4 packedInverse)
{
    float3 T = SafeNormalize(V - N * dot(N, V), AreaLightFallbackTangent(N));
    float3 B = cross(N, T);
    float3 center = AreaLtcToLocal(frame.position - worldPosition, T, B, N);
    float3 axisX = AreaLtcToLocal(frame.tangent * frame.halfSize.x, T, B, N);
    float3 axisY = AreaLtcToLocal(frame.bitangent * frame.halfSize.y, T, B, N);
    float integral = 0.0;
    float3 entry = 0.0.xxx;
    float3 exit = 0.0.xxx;
    bool crossesHorizon = false;
    float3 receiverEntry = 0.0.xxx;
    float3 receiverExit = 0.0.xxx;
    bool crossesReceiver = false;
    if (abs(frame.type - AREA_LIGHT_TYPE_DISK) < 0.5)
    {
        // An inscribed 32-gon alone can lose a third of the response near a
        // disk rim. Refine each sector until its circular-arc deviation is
        // small relative to the receiver distance in BOTH integration spaces.
        // This keeps the cosine/constant-F0 response analytic near the rim.
        float radius = frame.halfSize.x;
        float3 diskNormal = SafeNormalize(cross(axisX, axisY), N);
        float planeDistance = dot(center, diskNormal);
        float radialDistance = length(center - diskNormal * planeDistance);
        float radialGap = radialDistance - radius;
        float boundaryDistance = sqrt(planeDistance * planeDistance + radialGap * radialGap);
        float3 transformedX = AreaLtcTransformLocal(axisX, packedInverse);
        float3 transformedY = AreaLtcTransformLocal(axisY, packedInverse);
        // The largest singular value of the transformed disk axes bounds the
        // arc deviation. Avoid a looser Frobenius bound that refines ordinary
        // distant disks unnecessarily.
        float xx = dot(transformedX, transformedX);
        float yy = dot(transformedY, transformedY);
        float xy = dot(transformedX, transformedY);
        float transformedRadiusBound = sqrt(0.5 * (xx + yy +
            sqrt((xx - yy) * (xx - yy) + 4.0 * xy * xy))) * 1.000001;
        float determinant = packedInverse.x * packedInverse.w -
                            packedInverse.y * packedInverse.z;
        // M contains a 2x2 block and an identity Y axis. Use the product of
        // squared singular values to avoid subtracting close eigenvalues.
        float transformTrace = dot(packedInverse, packedInverse);
        float largestTransformEigenvalue = 0.5 * (transformTrace + sqrt(max(
            transformTrace * transformTrace - 4.0 * determinant * determinant, 0.0)));
        float minimumTransformScale = min(1.0, abs(determinant) /
            sqrt(max(largestTransformEigenvalue, 1.0e-20))) * 0.999999;
        float transformedBoundaryDistance = boundaryDistance * minimumTransformScale;
        // 1-cos(pi/(32*n)) <= (pi/32)^2/(2*n^2), without cancellation.
        float curvatureBound = radius * 0.004819142773969;
        float transformedCurvatureBound = transformedRadiusBound * 0.004819142773969;
        float globalRelativeCurvature = max(curvatureBound / max(boundaryDistance, 1.0e-20),
            transformedCurvatureBound / max(transformedBoundaryDistance, 1.0e-20));
        const float2 sectorRotation = float2(0.980785280403230, 0.195090322016128);
        [branch]
        if (globalRelativeCurvature <= AREA_LTC_DISK_CURVATURE_TOLERANCE)
        {
            // Keep the ordinary distant-disk kernel free of nested adaptive
            // loops. The global bound proves every coarse sector is safe.
            float2 offset = float2(1.0, 0.0);
            float3 previous = center + axisX;
            [loop]
            for (uint i = 1u; i <= AREA_LTC_DISK_SEGMENTS; ++i)
            {
                offset = i == AREA_LTC_DISK_SEGMENTS ? float2(1.0, 0.0) :
                    float2(sectorRotation.x * offset.x - sectorRotation.y * offset.y,
                           sectorRotation.y * offset.x + sectorRotation.x * offset.y);
                float3 current = center + axisX * offset.x + axisY * offset.y;
                AreaLtcAccumulateReceiverEdge(previous, current, packedInverse,
                    integral, entry, exit, crossesHorizon,
                    receiverEntry, receiverExit, crossesReceiver);
                previous = current;
            }
        }
        else
        {
            float2 sectorStart = float2(1.0, 0.0);
            float3 previous = center + axisX;
            [loop]
            for (uint i = 1u; i <= AREA_LTC_DISK_SEGMENTS; ++i)
            {
                float2 sectorEnd = i == AREA_LTC_DISK_SEGMENTS ? float2(1.0, 0.0) :
                    float2(sectorRotation.x * sectorStart.x - sectorRotation.y * sectorStart.y,
                           sectorRotation.y * sectorStart.x + sectorRotation.x * sectorStart.y);
                float3 coarseEnd = center + axisX * sectorEnd.x + axisY * sectorEnd.y;
                // A chord and its arc differ by at most curvatureBound. Their
                // nearest distances therefore differ by no more than that bound.
                float relativeCurvature = globalRelativeCurvature;
                [branch]
                if (relativeCurvature > AREA_LTC_DISK_CURVATURE_TOLERANCE)
                {
                    float minimumDistance = max(boundaryDistance,
                        AreaLtcClosestChordDistance(previous, coarseEnd) - curvatureBound);
                    float minimumTransformedDistance = max(transformedBoundaryDistance,
                        AreaLtcClosestChordDistance(AreaLtcTransformLocal(previous, packedInverse),
                            AreaLtcTransformLocal(coarseEnd, packedInverse)) - transformedCurvatureBound);
                    relativeCurvature = max(curvatureBound / max(minimumDistance, 1.0e-20),
                        transformedCurvatureBound / max(minimumTransformedDistance, 1.0e-20));
                }
                uint subdivisions = 1u;
                [loop]
                while (relativeCurvature > AREA_LTC_DISK_CURVATURE_TOLERANCE &&
                       subdivisions < AREA_LTC_DISK_MAX_SUBDIVISIONS)
                {
                    subdivisions *= 2u;
                    relativeCurvature *= 0.25;
                }
                // Do not silently integrate an unbounded approximation when the
                // source size/transform exceeds this finite refinement budget.
                if (!isfinite(relativeCurvature) ||
                    relativeCurvature > AREA_LTC_DISK_CURVATURE_TOLERANCE) return -1.0;
                float2 rotation = sectorRotation;
                if (subdivisions > 1u)
                {
                    float angle = 2.0 * PI / float(AREA_LTC_DISK_SEGMENTS * subdivisions);
                    rotation = float2(cos(angle), sin(angle));
                }
                float2 offset = sectorStart;
                [loop]
                for (uint j = 1u; j <= subdivisions; ++j)
                {
                    offset = j == subdivisions ? sectorEnd :
                        float2(rotation.x * offset.x - rotation.y * offset.y,
                               rotation.y * offset.x + rotation.x * offset.y);
                    float3 current = center + axisX * offset.x + axisY * offset.y;
                    AreaLtcAccumulateReceiverEdge(previous, current, packedInverse,
                        integral, entry, exit, crossesHorizon,
                        receiverEntry, receiverExit, crossesReceiver);
                    previous = current;
                }
                sectorStart = sectorEnd;
            }
        }
    }
    else
    {
        static const float2 offsets[4] = {
            float2(-1.0, -1.0), float2(1.0, -1.0),
            float2(1.0, 1.0), float2(-1.0, 1.0),
        };
        float3 previous = center - axisX + axisY;
        [unroll]
        for (uint i = 0u; i < 4u; ++i)
        {
            float3 current = center + axisX * offsets[i].x + axisY * offsets[i].y;
            AreaLtcAccumulateReceiverEdge(previous, current, packedInverse,
                integral, entry, exit, crossesHorizon,
                receiverEntry, receiverExit, crossesReceiver);
            previous = current;
        }
    }
    if (crossesReceiver)
        AreaLtcAccumulateClippedEdge(
            AreaLtcTransformLocal(receiverExit, packedInverse),
            AreaLtcTransformLocal(receiverEntry, packedInverse), integral,
            entry, exit, crossesHorizon);
    if (crossesHorizon)
        integral += AreaLtcIntegrateEdge(exit, entry);
    // Source sidedness is tested separately against the authored normal;
    // tangent/bitangent handedness must not change emitted radiance.
    return saturate(abs(FiniteScalarOr(integral, 0.0)));
}

float AreaLightClosestDistanceSq(AreaLightFrame frame, float3 worldPosition)
{
    float3 delta = worldPosition - frame.position;
    float2 projected = float2(dot(delta, frame.tangent), dot(delta, frame.bitangent));
    float2 closest;
    if (abs(frame.type - AREA_LIGHT_TYPE_DISK) < 0.5)
        closest = projected * min(1.0, frame.halfSize.x /
            max(length(projected), 1.0e-8));
    else
        closest = clamp(projected, -frame.halfSize, frame.halfSize);
    float3 closestDelta = delta - frame.tangent * closest.x - frame.bitangent * closest.y;
    return dot(closestDelta, closestDelta);
}

float AreaLtcProjectedAngularConfidence(AreaLightFrame frame,
                                        float3 worldPosition)
{
    float3 receiverDelta = worldPosition - frame.position;
    float distanceSq = dot(receiverDelta, receiverDelta);
    if (!isfinite(distanceSq) || distanceSq <= 0.0) return 0.0;
    float inverseDistance = rsqrt(distanceSq);
    float minimumHalfSize = min(frame.halfSize.x, frame.halfSize.y);
    float maximumHalfSize = max(frame.halfSize.x, frame.halfSize.y);
    if (!isfinite(minimumHalfSize) || !isfinite(maximumHalfSize) ||
        minimumHalfSize <= 0.0) return 0.0;
    float harmonicHalfSize = minimumHalfSize /
        (1.0 + minimumHalfSize / maximumHalfSize);
    float sourceCosine = max(dot(frame.normal, receiverDelta) * inverseDistance, 0.0);
    // Lower bound on the narrow angular scale of the source projection's
    // Jacobian at its center. Product/ratio form includes foreshortening
    // without subtracting nearly equal Gram eigenvalues. Tiny polygons lose precision
    // when opposite edge integrals cancel, even for a constant-F0 lobe.
    float angularExtent = harmonicHalfSize * inverseDistance * sourceCosine;
    if (!isfinite(angularExtent)) return 0.0;
    return smoothstep(1.0e-4, 2.0e-4, angularExtent);
}

bool AreaLtcRangeScale(AreaLightData light, AreaLightFrame frame,
                       float3 worldPosition, out float scale,
                       out float confidence)
{
    scale = 1.0;
    confidence = 1.0;
    float range = light.positionRange.w;
    if (!isfinite(range)) return false;
    if (!PointLightHasFiniteRange(range)) return true;
    float distance = length(frame.position - worldPosition);
    float radius = abs(frame.type - AREA_LIGHT_TYPE_DISK) < 0.5 ?
        frame.halfSize.x : length(frame.halfSize);
    float nearest = max(distance - radius, 0.0);
    float furthest = distance + radius;
    float maximumFade = SmoothRangeAttenuation(nearest * nearest, range);
    float minimumFade = SmoothRangeAttenuation(furthest * furthest, range);
    // Center fade is bounded to 1% of the largest fade across the emitter.
    // Lights near their cutoff retain the configured quadrature path.
    float relativeSpread = maximumFade > 0.0 ?
        (maximumFade - minimumFade) / maximumFade : 0.0;
    if (relativeSpread >= 0.01) return false;
    confidence = 1.0 - smoothstep(0.005, 0.01, relativeSpread);
    scale = SmoothRangeAttenuation(distance * distance, range);
    return true;
}

AreaLightDirectTerms AreaLtcSpecular(AreaLightFrame frame, float3 worldPosition,
                      float3 N, float3 V, float3 F0, float roughness,
                      float NdotV, float2 fresnelInterval,
                      bool3 activeChannels,
                      out bool geometrySafe,
                      out float diffuseFresnelMoment,
                      out float relativeFresnelError,
                      out float3 wholeHemisphereFresnelError)
{
    float2 uv = AreaLtcUv(roughness, NdotV);
    float4 inverse = areaLtcMatrix.SampleLevel(areaLtcSampler, uv, 0.0);
    float4 amplitude = areaLtcAmplitude.SampleLevel(areaLtcSampler, uv, 0.0);
    diffuseFresnelMoment = saturate(FiniteScalarOr(amplitude.z, 0.0));
    float meanFresnel = amplitude.x > 1.0e-20 ? amplitude.y / amplitude.x : 0.0;
    // A positive BRDF-weighted Fresnel mean over this emitter must be inside
    // its directional interval. The hemisphere mean can lie far outside it.
    float boundedMean = clamp(FiniteScalarOr(meanFresnel, 0.0),
        fresnelInterval.x, fresnelInterval.y);
    float momentError = max(abs(boundedMean - fresnelInterval.x),
                            abs(boundedMean - fresnelInterval.y));
    float3 fresnelError = (1.0.xxx - F0) * momentError;
    float3 minimumFresnel = F0 + (1.0.xxx - F0) * fresnelInterval.x;
    float3 ratios = fresnelError / max(minimumFresnel, 1.0e-20.xxx);
    ratios = select(activeChannels, ratios, 0.0.xxx);
    relativeFresnelError = MaxComponent3(ratios);
    // Unit-F single-scattering GGX hemispherical energy is <= 1. The
    // engine's D/direct-denominator floors only reduce it, and its Schlick
    // masking is no larger than Smith masking (k >= alpha/2). Use this
    // universal upper bound; an interpolated fitted energy is an estimate.
    wholeHemisphereFresnelError = fresnelError;
    float integral = AreaLtcPolygonIntegral(frame, worldPosition, N, V, inverse);
    geometrySafe = integral >= 0.0;
    float unitFIntegral = integral * max(amplitude.x, 0.0);
    AreaLightDirectTerms terms;
    terms.stable = max(Finite3Or(unitFIntegral * F0, 0.0.xxx), 0.0.xxx);
    terms.fresnel = max(Finite3Or(unitFIntegral * (1.0.xxx - F0) *
        boundedMean, 0.0.xxx), 0.0.xxx);
    return terms;
}

float2 AreaLtcViewDotCone(float centerDot, float coneCosine, float coneSine)
{
    centerDot = clamp(centerDot, -1.0, 1.0);
    float centerSine = sqrt(max(1.0 - centerDot * centerDot, 0.0));
    return float2(centerDot <= -coneCosine ? -1.0 :
        centerDot * coneCosine - centerSine * coneSine,
        centerDot >= coneCosine ? 1.0 :
        centerDot * coneCosine + centerSine * coneSine);
}

float2 AreaLtcFresnelInterval(AreaLightFrame frame, float3 worldPosition,
                             float3 V, float NdotV)
{
    float3 centerVector = frame.position - worldPosition;
    float distanceSq = dot(centerVector, centerVector);
    float radius = abs(frame.type - AREA_LIGHT_TYPE_DISK) < 0.5 ?
        frame.halfSize.x : length(frame.halfSize);
    float minimumViewDot = -1.0;
    float maximumViewDot = 1.0;
    if (distanceSq > radius * radius)
    {
        float coneSine = radius * rsqrt(distanceSq);
        float coneCosine = sqrt(max(1.0 - coneSine * coneSine, 0.0));
        float2 interval = AreaLtcViewDotCone(
            dot(V, centerVector) * rsqrt(distanceSq), coneCosine, coneSine);
        // The sphere cone exists only outside its radius. Conservatively
        // widen it to the entire sphere at that boundary so activating it
        // cannot pop Fresnel confidence. Lerp only enlarges the interval.
        float sphereConfidence = smoothstep(1.0, 1.1, sqrt(distanceSq) / radius);
        interval = lerp(float2(-1.0, 1.0), interval, sphereConfidence);
        minimumViewDot = interval.x;
        maximumViewDot = interval.y;
    }
    // A second cone follows from the source plane and remains useful inside
    // the emitter's enclosing sphere, including broad near-field lights.
    float3 planeAxis = -frame.normal;
    float planeDistance = dot(centerVector, planeAxis);
    if (planeDistance > 0.0)
    {
        float planarRadius = length(centerVector - planeAxis * planeDistance) + radius;
        float inverseConeLength = rsqrt(planeDistance * planeDistance +
                                        planarRadius * planarRadius);
        float2 interval = AreaLtcViewDotCone(dot(V, planeAxis),
            planeDistance * inverseConeLength, planarRadius * inverseConeLength);
        minimumViewDot = max(minimumViewDot, interval.x);
        maximumViewDot = min(maximumViewDot, interval.y);
    }
    // Only the physical receiver hemisphere contributes to diffuse lighting.
    minimumViewDot = max(minimumViewDot, -sqrt(max(1.0 - NdotV * NdotV, 0.0)));
    if (minimumViewDot > maximumViewDot) return 0.0.xx;
    float minimumFresnel = Pow5(saturate(1.0 -
        sqrt(max(0.5 * (1.0 + maximumViewDot), 0.0))));
    float maximumFresnel = Pow5(saturate(1.0 -
        sqrt(max(0.5 * (1.0 + minimumViewDot), 0.0))));
    return float2(minimumFresnel, maximumFresnel);
}

bool AreaLtcDiffuseFresnelBound(float2 interval, float moment)
{
    float maximumError = max(abs(moment - interval.x), abs(moment - interval.y));
    return maximumError <= 0.01 * (1.0 - interval.y);
}

float3 EvaluateAreaLightDiffuseFresnelCorrection(float3 scale, float3 N,
                                                 float3 V, float3 L,
                                                 float3 radiance)
{
    float3 H = SafeNormalize(V + L, N);
    float q = Pow5(saturate(1.0 - dot(H, V)));
    return scale * max(Finite3Or(radiance, 0.0.xxx), 0.0.xxx) *
        (saturate(dot(N, L)) * q / PI);
}

bool AreaLightDiffuseFresnelCorrectionIsSafe(float3 correction, float3 limit)
{
    float3 tolerance = max(max(limit, correction) * 4.0e-6, 1.0e-12.xxx);
    return all(isfinite(correction)) && all(correction <= limit + tolerance);
}

bool TryCombineAreaLightLtcTerms(AreaLightLtcTerms terms, float visibility,
                                float3 diffuseCorrection,
                                AreaLightDirectTerms sampled,
                                out float3 lighting)
{
    lighting = 0.0.xxx;
    float3 stable = terms.lighting.stable * visibility;
    if (any(terms.diffuseCorrectionScale > 0.0.xxx))
    {
        if (!AreaLightDiffuseFresnelCorrectionIsSafe(diffuseCorrection,
                terms.diffuseCorrectionLimit * visibility)) return false;
        stable -= diffuseCorrection;
    }
    // Geometry confidence applies to cosine and constant-F0 response; only
    // the uncertain Schlick q response uses the narrower Fresnel confidence.
    lighting = max(Finite3Or(stable, 0.0.xxx), 0.0.xxx) * terms.stableWeight +
        terms.lighting.fresnel * (visibility * terms.fresnelWeight) +
        sampled.stable * (1.0 - terms.stableWeight) +
        sampled.fresnel * (1.0 - terms.fresnelWeight);
    lighting = max(Finite3Or(lighting, 0.0.xxx), 0.0.xxx);
    return true;
}

AreaLightDirectTerms EvaluateAreaLightSampledDirectTerms(
    float3 albedo, float3 F0, float metallic, float roughness, float3 N,
    float3 V, float3 L, float3 radiance, float clearcoatFactor,
    float clearcoatRoughness, float3 sheenColor, float sheenRoughness)
{
    AreaLightDirectTerms terms;
    terms.stable = 0.0.xxx;
    terms.fresnel = 0.0.xxx;
    albedo = max(Finite3Or(albedo, 0.0.xxx), 0.0.xxx);
    F0 = saturate(Finite3Or(F0, 0.04.xxx));
    metallic = saturate(FiniteScalarOr(metallic, 0.0));
    roughness = clamp(FiniteScalarOr(roughness, 1.0), 0.045, 1.0);
    radiance = max(Finite3Or(radiance, 0.0.xxx), 0.0.xxx);
    N = SafeNormalize(N, float3(0.0, 1.0, 0.0));
    V = SafeNormalize(V, float3(0.0, 0.0, 1.0));
    L = SafeNormalize(L, float3(0.0, 1.0, 0.0));
    float NdotL = saturate(FiniteScalarOr(dot(N, L), 0.0));
    float NdotV = saturate(FiniteScalarOr(dot(N, V), 0.0));
    if (NdotL <= 0.0 || NdotV <= 0.0) return terms;

    float3 H = SafeNormalize(V + L, N);
    float q = Pow5(1.0 - saturate(dot(H, V)));
    float3 unitFSpecular = radiance * (NdotL *
        DistributionGGX(N, H, roughness) * GeometrySmith(N, V, L, roughness) /
        max(4.0 * NdotV * NdotL, 1.0e-4));
    // Split Schlick F=F0+(1-F0)*q without evaluating GGX twice. The constant
    // F0 lobe and cosine terms remain analytic when only the q fit is unsafe.
    terms.stable = unitFSpecular * F0 +
        albedo * (1.0 - metallic) * (1.0.xxx - F0) *
        ((1.0 - q) * NdotL / PI) * radiance;
    terms.fresnel = unitFSpecular * (1.0.xxx - F0) * q;
    float clearcoat = saturate(FiniteScalarOr(clearcoatFactor, 0.0));
    float baseScale = DeferredClearcoatBaseEnergyScale(clearcoat);
    terms.stable *= baseScale;
    terms.fresnel *= baseScale;
    if (clearcoat > 0.0)
    {
        float coatRoughness = clamp(FiniteScalarOr(clearcoatRoughness, 1.0),
                                    0.045, 1.0);
        float3 coatUnitFSpecular = radiance * (NdotL * clearcoat *
            DistributionGGX(N, H, coatRoughness) *
            GeometrySmith(N, V, L, coatRoughness) /
            max(4.0 * NdotV * NdotL, 1.0e-4));
        float coatF0 = IorToF0(1.5);
        terms.stable += coatUnitFSpecular * coatF0;
        terms.fresnel += coatUnitFSpecular * ((1.0 - coatF0) * q);
    }
    float sheenBaseScale = DeferredSheenBaseEnergyScale(sheenColor);
    terms.stable *= sheenBaseScale;
    terms.fresnel *= sheenBaseScale;
    terms.stable += EvaluateDeferredSheenDirectLight(
        sheenColor, sheenRoughness, N, V, L, radiance);
    return terms;
}

float3 EvaluateAreaLightSampledLayeredDirect(AreaLightData light,
                                            AreaLightFrame frame,
                                            float3 worldPosition,
                                            uint sampleCount, float3 albedo,
                                            float3 F0, float metallic,
                                            float roughness, float3 N,
                                            float3 V, float clearcoat,
                                            float clearcoatRoughness,
                                            float3 sheenColor,
                                            float sheenRoughness)
{
    float3 directLighting = 0.0.xxx;
    [loop]
    for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
    {
        float3 L;
        float3 radiance = EvaluateAreaLightSampleRadiance(light, frame,
            worldPosition, sampleIndex, sampleCount, L);
        directLighting += EvaluateDeferredLayeredDirectLight(albedo, F0,
            metallic, roughness, N, V, L, radiance, clearcoat,
            clearcoatRoughness, sheenColor, sheenRoughness);
    }
    return directLighting;
}

bool TryEvaluateAreaLightLtc(LightingBuffer lighting, AreaLightData light,
                             AreaLightFrame frame, float3 worldPosition,
                             float3 albedo, float3 F0, float metallic,
                             float roughness, float3 N, float3 V,
                             float clearcoatFactor, float clearcoatRoughness,
                             float3 sheenColor, float sheenRoughness,
                             bool unsupportedTransmission,
                             out AreaLightLtcTerms terms)
{
    terms.lighting.stable = 0.0.xxx;
    terms.lighting.fresnel = 0.0.xxx;
    terms.diffuseCorrectionScale = 0.0.xxx;
    terms.diffuseCorrectionLimit = 0.0.xxx;
    terms.stableWeight = 0.0;
    terms.fresnelWeight = 0.0;
    if (lighting.areaLightingMode != AREA_LIGHTING_MODE_LTC || unsupportedTransmission ||
        !all(isfinite(worldPosition)) || !all(isfinite(frame.position)) ||
        !all(isfinite(frame.halfSize)))
        return false;
    float closestDistanceSq = AreaLightClosestDistanceSq(frame, worldPosition);
    if (closestDistanceSq <= AREA_LIGHT_MIN_DISTANCE_SQ) return false;
    float minimumRoughness = clamp(FiniteScalarOr(roughness, 1.0), 0.045, 1.0);
    if (saturate(FiniteScalarOr(clearcoatFactor, 0.0)) > 0.0)
        minimumRoughness = min(minimumRoughness,
            clamp(FiniteScalarOr(clearcoatRoughness, 1.0), 0.045, 1.0));
    // The engine's NDF denominator floor flattens the low-roughness lobe
    // (it remains active at the peak until roughness ~= .274). One LTC
    // cannot reproduce that cap reliably. Preserve the sampled BRDF below
    // .28 and blend through .34 to avoid roughness-map transition seams.
    if (minimumRoughness <= 0.28) return false;
    terms.stableWeight = smoothstep(0.28, 0.34, minimumRoughness);
    // Preserve the source-distance floor while approaching LTC continuously
    // over closest source distances .1 -> .12 world units.
    terms.stableWeight = min(terms.stableWeight,
        smoothstep(0.01, 0.0144, closestDistanceSq));
    float rangeScale;
    float rangeConfidence;
    if (!AreaLtcRangeScale(light, frame, worldPosition, rangeScale,
            rangeConfidence)) return false;
    terms.stableWeight = min(terms.stableWeight, rangeConfidence);
    terms.fresnelWeight = terms.stableWeight;
    if (dot(frame.normal, worldPosition - frame.position) <= 0.0) return true;
    float angularConfidence = AreaLtcProjectedAngularConfidence(frame, worldPosition);
    if (angularConfidence <= 0.0) return false;
    terms.stableWeight = min(terms.stableWeight, angularConfidence);

    N = SafeNormalize(N, float3(0.0, 1.0, 0.0));
    V = SafeNormalize(V, float3(0.0, 0.0, 1.0));
    float NdotV = saturate(dot(N, V));
    // The grazing endpoint of the finite fit is intentionally conservative.
    if (NdotV <= 0.01) return false;
    terms.stableWeight = min(terms.stableWeight, smoothstep(0.01, 0.02, NdotV));
    terms.fresnelWeight = terms.stableWeight;
    albedo = max(Finite3Or(albedo, 0.0.xxx), 0.0.xxx);
    F0 = saturate(Finite3Or(F0, 0.04.xxx));
    metallic = saturate(FiniteScalarOr(metallic, 0.0));
    float3 emittedRadiance = max(Finite3Or(
        light.colorIntensity.rgb * light.colorIntensity.a, 0.0.xxx), 0.0.xxx);
    bool3 activeChannels = emittedRadiance > 0.0.xxx;
    float2 fresnelInterval = AreaLtcFresnelInterval(frame, worldPosition, V, NdotV);
    float3 diffuseScale = albedo * (1.0 - metallic) * (1.0.xxx - F0);
    sheenColor = max(Finite3Or(sheenColor, 0.0.xxx), 0.0.xxx);
    float cosineIntegral = 0.0;
    // The cosine polygon serves diffuse energy, its Fresnel lower bound and
    // sheen. A pure metal without sheen needs only the fitted specular lobe.
    [branch]
    if (any(diffuseScale > 0.0.xxx) || any(sheenColor > 0.0.xxx))
        cosineIntegral = AreaLtcPolygonIntegral(frame, worldPosition,
            N, V, float4(1.0, 0.0, 0.0, 1.0));
    if (cosineIntegral < 0.0) return false;
    AreaLightDirectTerms baseLighting;
    baseLighting.stable = 0.0.xxx;
    baseLighting.fresnel = 0.0.xxx;
    if (NdotV > 0.0)
    {
        float diffuseFresnelMoment;
        float baseFresnelError;
        float3 baseWholeFresnelError;
        bool baseGeometrySafe;
        AreaLightDirectTerms specular = AreaLtcSpecular(frame, worldPosition, N, V, F0,
            roughness, NdotV, fresnelInterval, activeChannels,
            baseGeometrySafe, diffuseFresnelMoment, baseFresnelError, baseWholeFresnelError);
        if (!baseGeometrySafe) return false;
        bool sampledDiffuseFresnel = any(diffuseScale > 0.0.xxx) &&
            !AreaLtcDiffuseFresnelBound(fresnelInterval, diffuseFresnelMoment);
        float3 diffuse = diffuseScale * cosineIntegral *
            (sampledDiffuseFresnel ? 1.0 : 1.0 - diffuseFresnelMoment);
        if (sampledDiffuseFresnel)
        {
            // Keep the analytic cosine term; a cheap sampled correction
            // preserves the emitter-dependent Schlick diffuse Fresnel.
            terms.diffuseCorrectionScale = diffuseScale *
                DeferredClearcoatBaseEnergyScale(clearcoatFactor) *
                DeferredSheenBaseEnergyScale(sheenColor);
        }
        float coatBaseScale = DeferredClearcoatBaseEnergyScale(clearcoatFactor);
        baseLighting.stable = (diffuse + specular.stable) * coatBaseScale;
        baseLighting.fresnel = specular.fresnel * coatBaseScale;
        float clearcoat = saturate(FiniteScalarOr(clearcoatFactor, 0.0));
        float coatFresnelError = 0.0;
        float3 coatWholeFresnelError = 0.0.xxx;
        if (clearcoat > 0.0)
        {
            float unusedMoment;
            bool coatGeometrySafe;
            AreaLightDirectTerms coatSpecular = AreaLtcSpecular(frame, worldPosition,
                N, V, IorToF0(1.5).xxx, clearcoatRoughness, NdotV,
                fresnelInterval, activeChannels, coatGeometrySafe, unusedMoment,
                coatFresnelError, coatWholeFresnelError);
            if (!coatGeometrySafe) return false;
            baseLighting.stable += clearcoat * coatSpecular.stable;
            baseLighting.fresnel += clearcoat * coatSpecular.fresnel;
        }
        float baseScale = DeferredClearcoatBaseEnergyScale(clearcoatFactor) *
            DeferredSheenBaseEnergyScale(sheenColor);
        float coatScale = clearcoat * DeferredSheenBaseEnergyScale(sheenColor);
        float3 wholeError = baseWholeFresnelError * baseScale +
            coatWholeFresnelError * coatScale;
        float3 diffuseLowerBound = diffuseScale *
            (cosineIntegral * (1.0 - fresnelInterval.y) * baseScale);
        float3 totalRatios = wholeError / max(diffuseLowerBound, 1.0e-20.xxx);
        totalRatios = select(activeChannels, totalRatios, 0.0.xxx);
        // Either every lobe has bounded relative specular error, or their
        // summed whole-hemisphere error is bounded by an exact diffuse lower
        // bound. The latter retains accurate LTC for diffuse near-field
        // emitters where coarse BRDF quadrature is especially unreliable.
        float fresnelError = min(max(baseFresnelError, coatFresnelError),
                                 MaxComponent3(totalRatios));
        if (!isfinite(fresnelError)) return false;
        // Proven 2% domains keep the fitted Schlick response. Recover its
        // q term continuously between 2% and 4% uncertainty while keeping
        // confidence * Fresnel error <= 2% throughout that band.
        float fresnelConfidence = min(1.0, 0.02 / max(fresnelError, 1.0e-20)) *
            (1.0 - smoothstep(0.02, 0.04, fresnelError));
        // Only the direction-dependent Schlick q term uses this confidence.
        // Sampling its uncertainty must not reintroduce coarse quadrature
        // into the analytic cosine response or the constant-F0 GGX lobe.
        terms.fresnelWeight = min(terms.stableWeight, fresnelConfidence);
    }
    float sheenBaseScale = DeferredSheenBaseEnergyScale(sheenColor);
    baseLighting.stable *= sheenBaseScale;
    baseLighting.fresnel *= sheenBaseScale;
    float safeSheenRoughness = clamp(FiniteScalarOr(sheenRoughness, 1.0), 0.045, 1.0);
    float grazing = (1.0 - NdotV) * (1.0 - NdotV) *
        (1.0 - 0.35 * safeSheenRoughness);
    baseLighting.stable += sheenColor * (PI * cosineIntegral * grazing * 0.35);
    // Intensity remains emitted scene-linear radiance. The solid-angle
    // integral already contains source area, source cosine and inverse-square.
    terms.lighting.stable = max(Finite3Or(
        baseLighting.stable * emittedRadiance * rangeScale, 0.0.xxx), 0.0.xxx);
    terms.lighting.fresnel = max(Finite3Or(
        baseLighting.fresnel * emittedRadiance * rangeScale, 0.0.xxx), 0.0.xxx);
    terms.diffuseCorrectionLimit = terms.diffuseCorrectionScale *
        (cosineIntegral * rangeScale) * emittedRadiance;
    return true;
}
