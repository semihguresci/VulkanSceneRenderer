#pragma once

// Scattering in a spherical atmosphere. Distances are in kilometres.
// Model and Earth coefficients follow Hillaire, EGSR 2020.
static const float kAtmospherePi = 3.14159265359;
static const float3 kRayleigh = float3(0.005802, 0.013558, 0.033100);
static const float3 kOzone = float3(0.000650, 0.001881, 0.000085);
static const float kMieScattering = 0.003996;
static const float kMieExtinction = 0.004440;

float RadialDelta(float3 p, float radius)
{
    // |p|² - r² without subtracting two planet-sized squares near the surface.
    return dot(p.xz, p.xz) + (p.y - radius) * (p.y + radius);
}

float AtmosphereExit(float3 p, float3 d, float radius)
{
    float b = dot(p, d);
    float c = RadialDelta(p, radius);
    float root = sqrt(max(b * b - c, 0.0));
    return max(b > 0.0 ? -c / max(b + root, 1e-6) : root - b, 0.0);
}

float GroundDistance(float3 p, float3 d, float radius)
{
    float b = dot(p, d);
    float c = RadialDelta(p, radius);
    float discriminant = b * b - c;
    if (b >= 0.0 || discriminant < 0.0)
        return -1.0;
    return max(c / (-b + sqrt(discriminant)), 0.0);
}

float3 AtmosphereDensity(float3 p, float planetRadius)
{
    float h = max(RadialDelta(p, planetRadius) / (length(p) + planetRadius), 0.0);
    return float3(exp(-h / 8.0), exp(-h / 1.2), saturate(1.0 - abs(h - 25.0) / 15.0));
}

float3 AtmosphereExtinction(float3 density)
{
    return kRayleigh * density.x + kMieExtinction * density.y + kOzone * density.z;
}

float3 SunTransmittance(float3 p, float3 sun, float planetRadius, float topRadius,
                        float3 densityScale, uint samples)
{
    if (GroundDistance(p, sun, planetRadius) >= 0.0)
        return 0.0.xxx;
    float distance = AtmosphereExit(p, sun, topRadius);
    float3 column = 0.0.xxx;
    float previous = 0.0;
    for (uint i = 0; i < samples; ++i)
    {
        float u = float(i + 1) / float(samples);
        float next = distance * u * u;
        column += AtmosphereDensity(p + sun * (0.5 * (previous + next)), planetRadius) *
                  (next - previous);
        previous = next;
    }
    return exp(-AtmosphereExtinction(column * densityScale));
}

static const uint kAtmosphereLutWidth = 16;
static const uint kAtmosphereLutHeight = 8;

float2 AtmosphereLutPosition(float3 p, float3 sun, float radius, float topRadius)
{
    float height = RadialDelta(p, radius) / (length(p) + radius);
    float mu = clamp(dot(normalize(p), sun), -1.0, 1.0);
    return float2(0.5 + 0.5 * sign(mu) * sqrt(abs(mu)),
                  sqrt(saturate(height / (topRadius - radius)))) *
           float2(kAtmosphereLutWidth - 1, kAtmosphereLutHeight - 1);
}

interface IAtmosphereLut
{
    float3 lookup(float2 position);
}

float3 IntegrateAtmospherePath<Lut : IAtmosphereLut>(float3 origin, float3 direction, float viewDistance,
                               bool endsAtGround, float3 sun, float planetRadius,
                               float topRadius, float3 densityScale, uint samples,
                               uint solarSamples, bool multipleScattering, Lut lut,
                               out float3 transmittance)
{
    float mu = clamp(dot(direction, sun), -1.0, 1.0);
    float rayleighPhase = 3.0 * (1.0 + mu * mu) / (16.0 * kAtmospherePi);
    const float g = 0.8;
    float miePhase = (1.0 - g * g) / (4.0 * kAtmospherePi * pow(1.0 + g * g - 2.0 * g * mu, 1.5));
    float3 opticalDepth = 0.0.xxx;
    float3 scattered = 0.0.xxx;
    float previous = 0.0;
    for (uint i = 0; i < samples; ++i)
    {
        // Resolve the thin aerosol layer near the lower endpoint.
        float u = float(i + 1) / float(samples);
        float next = viewDistance * (endsAtGround ? u * (2.0 - u) : u * u);
        float stepSize = next - previous;
        float3 p = origin + direction * (0.5 * (previous + next));
        float3 density = AtmosphereDensity(p, planetRadius) * densityScale;
        float3 segment = AtmosphereExtinction(density) * stepSize;
        float3 view = exp(-opticalDepth - 0.5 * segment);
        float3 light = SunTransmittance(p, sun, planetRadius, topRadius, densityScale, solarSamples);
        float3 scatter = kRayleigh * (density.x * rayleighPhase) +
                         kMieScattering * (density.y * miePhase);
        float3 source = light * scatter;
        if (multipleScattering)
        {
            float3 multiple = lut.lookup(AtmosphereLutPosition(p, sun, planetRadius, topRadius));
            source += multiple * (kRayleigh * density.x + kMieScattering * density.y);
        }
        scattered += view * source * stepSize;
        opticalDepth += segment;
        previous = next;
    }
    transmittance = exp(-opticalDepth);
    return scattered;
}

float3 EvaluateAtmosphere<Lut : IAtmosphereLut>(float3 direction, float3 groundAlbedo, float4 sun,
                          float3 geometry, float4 density, Lut lut)
{
    float3 densityScale = density.xyz;
    float planetRadius = geometry.x;
    float topRadius = planetRadius + geometry.y;
    float3 origin = float3(0.0, planetRadius + geometry.z, 0.0);
    float groundDistance = GroundDistance(origin, direction, planetRadius);
    float viewDistance = groundDistance >= 0.0 ? groundDistance
        : AtmosphereExit(origin, direction, topRadius);

    float3 transmittance;
    float3 scattered = IntegrateAtmospherePath(origin, direction, viewDistance,
        groundDistance >= 0.0, sun.xyz, planetRadius, topRadius, densityScale,
        24, 12, density.w > 0.0, lut, transmittance);
    if (groundDistance >= 0.0)
    {
        float3 normal = normalize(origin + direction * groundDistance);
        float3 light = SunTransmittance(normal * (planetRadius + 0.002), sun.xyz,
                                        planetRadius, topRadius, densityScale, 12);
        scattered += transmittance * light * groundAlbedo *
                     (max(dot(normal, sun.xyz), 0.0) / kAtmospherePi);
    }
    return scattered * sun.w;
}
