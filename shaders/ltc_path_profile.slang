#ifndef CONTAINER_LTC_PATH_PROFILE_SLANG
#define CONTAINER_LTC_PATH_PROFILE_SLANG

// The opt-in profile stores one presence mask per shaded receiver in HDR
// alpha. Values from 1024 through 1535 are exact in the FP16 scene target.
static const uint LTC_PROFILE_MARKER = 1024u;
static const uint LTC_PROFILE_MASK_SIZE = 512u;
static const uint LTC_PROFILE_COARSE_DISK = 1u << 0;
static const uint LTC_PROFILE_ADAPTIVE_DISK = 1u << 1;
static const uint LTC_PROFILE_SUBDIVIDED_DISK = 1u << 2;
static const uint LTC_PROFILE_HORIZON_CLIP = 1u << 3;
static const uint LTC_PROFILE_SAMPLED_BLEND = 1u << 4;
static const uint LTC_PROFILE_DIFFUSE_FRESNEL = 1u << 5;
static const uint LTC_PROFILE_SAMPLED_FALLBACK = 1u << 6;
static const uint LTC_PROFILE_RECTANGLE = 1u << 7;
static const uint LTC_PROFILE_MATERIAL_RESIDUAL = 1u << 8;

#endif
