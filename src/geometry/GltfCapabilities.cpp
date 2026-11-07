#include <Container/geometry/GltfCapabilities.h>
#include <Container/geometry/GltfCompression.h>
#include <Container/geometry/GltfExtendedMaterials.h>
#include <Container/geometry/GltfIesProfiles.h>
#include <Container/geometry/GltfOpacityMicromaps.h>
#include <Container/geometry/GltfXmpMetadata.h>
#include <Container/utility/TextureImageData.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <set>
#include <utility>

namespace container::geometry::gltf {
namespace {

using enum ExtensionSupport;
constexpr std::string_view kExperimentalMaterialFlag = "--experimental-gltf-materials";
const std::array kExtensions{
    ExtensionCapability{"KHR_texture_transform", Supported,
                        "Per-texture offset, rotation, scale and UV0/UV1 selection"},
    ExtensionCapability{"KHR_node_visibility", Supported,
                        "Authored local visual visibility, inherited through the scene hierarchy without disabling cameras"},
    ExtensionCapability{"KHR_node_selectability", Partial,
                        "Authored inherited eligibility, nearest-hit ancestor routing and graph selection events; no trace-through of excluded geometry"},
    ExtensionCapability{"KHR_node_hoverability", Partial,
                        "Authored inherited eligibility, nearest-hit ancestor routing and graph hover events; no trace-through of excluded geometry"},
    ExtensionCapability{"KHR_materials_unlit", Supported, "Unlit material shading"},
    ExtensionCapability{"KHR_materials_specular", Supported,
                        "Specular factor/color and textures"},
    ExtensionCapability{"KHR_materials_ior", Supported, "Material index of refraction"},
    ExtensionCapability{"KHR_materials_dispersion", Supported,
                        "Material dispersion in transmission shading"},
    ExtensionCapability{"KHR_materials_emissive_strength", Supported,
                        "Emissive intensity multiplier"},
    ExtensionCapability{"KHR_materials_transmission", Supported,
                        "Transmission factor and texture"},
    ExtensionCapability{"KHR_materials_volume", Supported,
                        "Thickness texture/factor and attenuation"},
    ExtensionCapability{"KHR_materials_clearcoat", Supported,
                        "Clearcoat factor, roughness and normal textures"},
    ExtensionCapability{"KHR_materials_sheen", Supported,
                        "Sheen color/roughness factors and textures"},
    ExtensionCapability{"KHR_materials_anisotropy", Partial,
                        "Tangent-frame reflection with legacy GGX masking/cap and bent-normal IBL; deferred MSAA resolves material response; mixed probe dielectrics remain unsupported"},
    ExtensionCapability{"KHR_materials_diffuse_transmission", Partial,
                        "Release-candidate thin-surface diffuse response with independent color, alpha-channel factor texture and opposite-side irradiance; raster/probe approximation without volumetric or MIS transport"},
    ExtensionCapability{kGltfRetroreflection, Partial,
                        "Experimental mirrored-view base reflection in reference path tracing", kExperimentalMaterialFlag},
    ExtensionCapability{kGltfScatter, Partial,
                        "Experimental thin-surface forward and backward scattering in reference path tracing; participating media are unsupported", kExperimentalMaterialFlag},
    ExtensionCapability{kGltfLegacyScatter, Partial,
                        "Earlier scatter draft alias with its original field defaults; only thin surfaces render", kExperimentalMaterialFlag},
    ExtensionCapability{"KHR_materials_iridescence", Partial,
                        "Iridescence factors and thickness textures use a cosine-tint approximation rather than thin-film interference"},
    ExtensionCapability{"KHR_materials_pbrSpecularGlossiness", Partial,
                        "Specular-glossiness is converted to the runtime metallic-roughness approximation"},
    ExtensionCapability{"KHR_lights_punctual", Supported,
                        "Point, directional and spot lights, including range and spot cones"},
    ExtensionCapability{kGltfIesExtension, Partial,
                        "LM-63 Type C measured point fixtures, including azimuthal distributions; only TILT=NONE"},
    ExtensionCapability{"KHR_lights_area", Partial,
                        "Area lights use the engine's supported area-light dialect and shading approximation"},
    ExtensionCapability{"EXT_lights_area", Partial,
                        "Area lights use the engine's supported area-light dialect and shading approximation"},
    ExtensionCapability{"WEBGI_lights_area", Partial,
                        "Area lights use the engine's supported area-light dialect and shading approximation"},
    ExtensionCapability{"KHR_draco_mesh_compression", HasDracoDecoder() ? Partial : Unsupported,
                        HasDracoDecoder()
                          ? "Triangle geometry decoded in a bounded worker; unsupported primitive modes are diagnosed"
                          : "Draco decoding is disabled; configure ENABLE_GLTF_DRACO with a supported bounded worker"},
    ExtensionCapability{"EXT_meshopt_compression", HasMeshoptDecoder() ? Supported : Unsupported,
                        HasMeshoptDecoder()
                          ? "Checked buffer-view decoding with attribute, triangle and index modes and EXT filters"
                          : "Meshopt decoding is disabled; configure ENABLE_GLTF_MESHOPT"},
    ExtensionCapability{"KHR_meshopt_compression", HasMeshoptDecoder() ? Partial : Unsupported,
                        HasMeshoptDecoder()
                          ? "Release-candidate meshopt extension with v0/v1 streams and COLOR filtering"
                          : "Meshopt decoding is disabled; configure ENABLE_GLTF_MESHOPT"},
    ExtensionCapability{"KHR_texture_basisu", container::material::HasKtxTextureCodec() ? Supported : Unsupported,
                        container::material::HasKtxTextureCodec()
                          ? "Validated ETC1S/UASTC KTX2 material textures with role-aware transcoding and authored mips"
                          : "BasisU transcoding is disabled; configure ENABLE_TEXTURE_KTX"},
    ExtensionCapability{"EXT_texture_webp", container::material::HasWebpTextureCodec() ? Supported : Unsupported,
                        container::material::HasWebpTextureCodec()
                          ? "Static WebP material textures with separate color and data views"
                          : "WebP decoding is disabled; configure ENABLE_TEXTURE_WEBP"},
    ExtensionCapability{"MSFT_texture_dds", Partial,
                        "Draft DDS alternate sources; supported 2D DDS formats retain authored mips with device-aware fallback diagnostics"},
    ExtensionCapability{"EXT_texture_avif", Unsupported,
                        "The runtime image decoder does not decode AVIF"},
    ExtensionCapability{"KHR_mesh_quantization", Partial,
                        "Declared integer vertex and POSITION/NORMAL/TANGENT morph attributes are decoded; unsupported morph semantics are diagnosed explicitly"},
    ExtensionCapability{"KHR_accessor_float64", Partial,
                        "Experimental required accessor extension: geometry converts to binary32 with checked range and precision loss; animation retains binary64 sampling"},
    ExtensionCapability{"EXT_mesh_gpu_instancing", Partial,
                        "Shared geometry with per-instance scene transforms, picking and ray placement; singular transforms require geometry baking"},
    ExtensionCapability{"KHR_animation_pointer", Partial,
                        "Implemented numeric node, material, texture-transform, punctual-light and camera targets and STEP node flags are evaluated; arbitrary Object Model properties are diagnosed as unsupported"},
    ExtensionCapability{"KHR_interactivity", Partial,
                        "Bounded behavior graph execution with a documented operation set and numeric Object Model targets"},
    ExtensionCapability{kGltfOpacityMicromapExtension, Partial,
                        "Experimental optional ray acceleration for static alpha-mask primitives; sampled alpha remains available"},
    ExtensionCapability{"KHR_materials_variants", Supported,
                        "Validated material mappings, undoable scene selection and portable variant export"},
    ExtensionCapability{kGltfXmpExtension, Supported,
                        "Read-only XMP packet inspection and portable metadata export"},
    ExtensionCapability{"OMI_environment_sky", Unsupported,
                        "Draft optional plain and gradient sky preview requires explicit opt-in; required extension is unsupported"},
};

struct Version {
  uint32_t major{};
  uint32_t minor{};
};

std::optional<Version> parseVersion(std::string_view value) {
  const auto dot = value.find('.');
  if (dot == 0 || dot == std::string_view::npos || dot + 1 == value.size())
    return std::nullopt;
  // Reject signs, whitespace, suffixes and extra components explicitly.
  if (!std::ranges::all_of(value, [](char c) {
        return (c >= '0' && c <= '9') || c == '.';
      }))
    return std::nullopt;
  Version result;
  const auto major = std::from_chars(value.data(), value.data() + dot, result.major);
  const auto minor = std::from_chars(value.data() + dot + 1,
                                     value.data() + value.size(), result.minor);
  if (major.ec != std::errc{} || major.ptr != value.data() + dot ||
      minor.ec != std::errc{} || minor.ptr != value.data() + value.size())
    return std::nullopt;
  return result;
}

}  // namespace

std::span<const ExtensionCapability> extensionCapabilities() { return kExtensions; }

const ExtensionCapability* extensionCapability(std::string_view name) {
  const auto found = std::ranges::find(kExtensions, name, &ExtensionCapability::name);
  return found == kExtensions.end() ? nullptr : &*found;
}

bool CapabilityValidation::supported() const {
  return std::ranges::none_of(diagnostics, [](const auto& diagnostic) {
    return diagnostic.severity == CapabilitySeverity::Error;
  });
}

CapabilityValidation validateCapabilities(const AssetRequirements& requirements,
                                           std::string_view assetPath,
                                           std::span<const std::string> unavailableRuntimeExtensions,
                                           std::span<const std::string_view> enabledExperimentalExtensions) {
  CapabilityValidation result;
  auto add = [&](CapabilitySeverity severity, std::string code, std::string message) {
    result.diagnostics.push_back(
        {severity, std::move(code), std::string(assetPath), std::move(message)});
  };
  const auto version = parseVersion(requirements.version);
  if (!version) {
    add(CapabilitySeverity::Error, "invalid_version",
        "asset.version '" + requirements.version + "' must be a major.minor version");
  } else if (version->major != 2) {
    add(CapabilitySeverity::Error, "unsupported_version",
        "asset.version '" + requirements.version + "' is unsupported; this runtime supports glTF 2.0");
  }

  bool minimumSupported = true;
  if (requirements.minVersion) {
    const auto minimum = parseVersion(*requirements.minVersion);
    if (!minimum) {
      minimumSupported = false;
      add(CapabilitySeverity::Error, "invalid_min_version",
          "asset.minVersion '" + *requirements.minVersion + "' must be a major.minor version");
    } else if (minimum->major != 2 || minimum->minor != 0) {
      minimumSupported = false;
      add(CapabilitySeverity::Error, "unsupported_min_version",
          "asset.minVersion '" + *requirements.minVersion + "' is unsupported; this runtime supports glTF 2.0");
    } else if (version && (minimum->major > version->major ||
               (minimum->major == version->major && minimum->minor > version->minor))) {
      minimumSupported = false;
      add(CapabilitySeverity::Error, "inconsistent_min_version",
          "asset.minVersion exceeds asset.version");
    }
  }
  if (version && version->major == 2 && version->minor > 0 && minimumSupported) {
    add(CapabilitySeverity::Warning, "newer_minor_version",
        "asset.version '" + requirements.version +
        "' is newer than glTF 2.0; importing its compatible 2.0 features");
  }

  const std::set<std::string> required(requirements.extensionsRequired.begin(),
                                       requirements.extensionsRequired.end());
  std::set<std::string> seen;
  auto checkExtension = [&](const std::string& name, bool isRequired) {
    if (!seen.insert(name).second) return;
    if (std::ranges::find(unavailableRuntimeExtensions, name) != unavailableRuntimeExtensions.end()) {
      add(isRequired ? CapabilitySeverity::Error : CapabilitySeverity::Warning,
          isRequired ? "unsupported_required_extension" : "unsupported_optional_extension",
          std::string(isRequired ? "Required" : "Optional") + " extension '" + name +
              "' is unavailable in the active scene consumer" +
              (isRequired ? "" : "; importing the asset's fallback, which may omit this feature"));
      return;
    }
    const auto* capability = extensionCapability(name);
    if (capability && !capability->experimentalEnableHint.empty() &&
        std::ranges::find(enabledExperimentalExtensions, name) == enabledExperimentalExtensions.end()) {
      add(isRequired ? CapabilitySeverity::Error : CapabilitySeverity::Warning,
          isRequired ? "unsupported_required_extension" : "unsupported_optional_extension",
          std::string(isRequired ? "Required" : "Optional") + " extension '" + name +
              "' needs explicit opt-in with " + std::string(capability->experimentalEnableHint) +
              (isRequired ? "" : "; importing the asset's fallback"));
    } else if (!capability || capability->support == Unsupported) {
      std::string message = (isRequired ? "Required extension '" : "Optional extension '") +
          name + "' is unsupported";
      if (capability) message += ": " + std::string(capability->description);
      if (!isRequired) message += "; importing the asset's fallback, which may omit this feature";
      add(isRequired ? CapabilitySeverity::Error : CapabilitySeverity::Warning,
          isRequired ? "unsupported_required_extension" : "unsupported_optional_extension",
          std::move(message));
    } else if (capability->support == Partial) {
      add(CapabilitySeverity::Warning, "partial_extension_support",
          "Extension '" + name + "': " + std::string(capability->description));
    }
  };
  // Required wins if the same extension is also in extensionsUsed. Do not
  // assume malformed files faithfully list required entries in both arrays.
  for (const auto& name : requirements.extensionsRequired) checkExtension(name, true);
  for (const auto& name : requirements.extensionsUsed)
    checkExtension(name, required.contains(name));
  return result;
}

std::string formatCapabilityDiagnostic(const CapabilityDiagnostic& diagnostic) {
  return "glTF asset '" + diagnostic.assetPath + "': " + diagnostic.code + ": " + diagnostic.message;
}

}  // namespace container::geometry::gltf
