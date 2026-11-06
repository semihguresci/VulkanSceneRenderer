#pragma once

#include <Container/geometry/Model.h>
#include <Container/geometry/GltfCapabilities.h>
#include <Container/geometry/GltfDeformation.h>
#include <Container/geometry/GltfCompression.h>
#include <Container/geometry/GltfInstancing.h>
#include <Container/geometry/GltfOpacityMicromaps.h>

#include <tiny_gltf.h>

#include <string>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace container::geometry {

struct GltfLoadResult {
  Model model;
  tinygltf::Model gltfModel;
  std::vector<gltf::CapabilityDiagnostic> capabilityDiagnostics;
  std::optional<gltf::DeformationData> deformation;
  gltf::GltfInstancingData instancing;
  gltf::GltfOpacityMicromapAssets opacityMicromaps;
};

namespace gltf {

struct LoadOptions {
  bool decodeImages{true};
  // Finite values bound aggregate encoded reads and preflight source, declared
  // buffer and converted geometry bytes before allocating the asset buffers.
  uintmax_t maxInputBytes{std::numeric_limits<uintmax_t>::max()};
  // Decoding happens on a detached source model, before publishing geometry.
  // Decoder output/worker bounds apply independently of encoded input reads.
  CompressionLimits compression;
  // Runtime texture consumers retain the exact encoded container/precision.
  // decodeImages=false always keeps the legacy empty-image contract, even
  // when this flag is true. Ordinary default PNG/JPEG loads stay decoded RGBA.
  bool retainEncodedImages{false};
  // Explicitly enable the URI-only linked asset subset of the pinned 2.1 draft.
  bool enableLinkedAssets{false};
  // Use the pinned retroreflection and scatter material draft definitions.
  bool enableExperimentalMaterials{false};
  bool enableOpacityMicromaps{false};
  InstancingLimits instancing;
  // A source parser can support more extensions than a particular scene
  // consumer. Restrict that consumer without weakening decoder requirements.
  std::vector<std::string> unavailableRuntimeExtensions;
  // Remote import keeps embedded buffers and data URIs, and rejects file URIs.
  bool requireSelfContained{false};
};

// Source-only consumers (for example material import) share the same version
// and extension gate, even when the asset has no renderable geometry.
tinygltf::Model LoadSourceModel(const std::string& path,
                               const LoadOptions& options = {});

// Responsible solely for reading glTF assets and converting them into
// runtime geometry structures. Keeps file I/O away from Model.
Model LoadModelFromFile(const std::string& path);

GltfLoadResult LoadModelWithSource(const std::string& path);
GltfLoadResult LoadModelWithSource(const std::string& path,
                                  const LoadOptions& options);

// Regenerates only missing authored frames with the same MikkTSpace/fallback
// path as initial import. UV selections follow source mesh-major primitives;
// procedural meshes appended after those primitives are preserved.
Model RegenerateGltfBindTangents(
    const tinygltf::Model& source, const Model& originalBindGeometry,
    std::span<const uint32_t> normalTextureTexCoords);

}  // namespace gltf
}  // namespace container::geometry

