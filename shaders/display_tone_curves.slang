// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2022-2026 NVIDIA CORPORATION
// SPDX-FileCopyrightText: 2024 The Khronos Group, Inc.
//
// Adapted from nvpro_core2's tonemap_functions.h.slang and Khronos ToneMapping.
// See docs/third-party-notices.md for source revisions and license.
// This adaptation returns display-linear Rec. 709 for grading and an sRGB target.

float3 DecodeToneCurveSrgb(float3 encoded)
{
    encoded = saturate(encoded);
    return lerp(encoded / 12.92, pow((encoded + 0.055) / 1.055, 2.4),
                float3(encoded > 0.04045));
}

float3 FilmicDisplayLinear(float3 color)
{
    float3 x = max(color - 0.004, 0.0.xxx);
    return DecodeToneCurveSrgb(x * (6.2 * x + 0.5) /
                               (x * (6.2 * x + 1.7) + 0.06));
}

float3 UnchartedCurve(float3 x)
{
    return (x * (0.15 * x + 0.05) + 0.004) /
           (x * (0.15 * x + 0.50) + 0.06) - (0.02 / 0.30);
}

float3 UnchartedDisplayLinear(float3 color)
{
    float3 normalized = saturate(UnchartedCurve(2.0 * color) / UnchartedCurve(11.2.xxx));
    return DecodeToneCurveSrgb(pow(normalized, 1.0 / 2.2));
}

float3 AgXDisplayLinear(float3 color)
{
    const float3x3 inset = float3x3(
        0.8424790623, 0.0784336000, 0.0792237451,
        0.0423282423, 0.8784686365, 0.0791661275,
        0.0423756549, 0.0784336000, 0.8791429738);
    const float3x3 outset = float3x3(
         1.1968790051, -0.0980208811, -0.0990297441,
        -0.0528968518,  1.1519031299, -0.0989611768,
        -0.0529716355, -0.0980434501,  1.1510736726);
    float3 x = saturate((log2(max(mul(inset, color), exp2(-12.47393))) + 12.47393) / 16.5);
    float3 sigmoid = (((((15.5 * x - 40.14) * x + 31.96) * x - 6.868) * x
                         + 0.4298) * x + 0.1191) * x - 0.0023;
    return DecodeToneCurveSrgb(mul(outset, sigmoid));
}

float3 PbrNeutralDisplayLinear(float3 color)
{
    float minimum = min(color.r, min(color.g, color.b));
    color -= minimum < 0.08 ? minimum * (1.0 - 6.25 * minimum) : 0.04;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < 0.76)
        return color;
    float compressed = 1.0 - 0.0576 / (peak - 0.52);
    float desaturation = 1.0 - rcp(1.0 + 0.15 * (peak - compressed));
    return lerp(color * (compressed / peak), compressed.xxx, desaturation);
}

float3 ApplyWhiteBalance(float3 color, float3 gain)
{
    if (all(gain == 1.0))
        return color;
    const float3x3 toCone = float3x3(
        0.39858179, 0.51147528, 0.03823013,
        0.08651038, 0.90263008, 0.04376704,
        0.02423435, 0.13363512, 0.92889263);
    const float3x3 fromCone = float3x3(
         2.86209568, -1.61563711, -0.04166966,
        -0.27259146,  1.26953249, -0.04859816,
        -0.03545430, -0.14049009,  1.08462940);
    return max(mul(fromCone, mul(toCone, color) * gain), 0.0.xxx);
}
