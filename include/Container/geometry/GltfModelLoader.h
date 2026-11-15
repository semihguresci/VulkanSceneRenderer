#pragma once

#include <Container/geometry/Model.h>
#include <Container/geometry/GltfCapabilities.h>
#include <Container/geometry/GltfDeformation.h>
#include <Container/geometry/GltfCompression.h>
#include <Container/geometry/GltfInstancing.h>
#include <Container/geometry/GltfOpacityMicromaps.h>
#include <Container/utility/ContentFingerprint.h>

#include <tiny_gltf.h>

#include <string>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace container::geometry {

namespace gltf { struct GltfLinkedCompositionInfo; }

struct GltfLoadResult {
  Model model;
  tinygltf::Model gltfModel;
  std::vector<gltf::CapabilityDiagnostic> capabilityDiagnostics;
  std::optional<gltf::DeformationData> deformation;
  gltf::GltfInstancingData instancing;
  gltf::GltfOpacityMicromapAssets opacityMicromaps;
  std::shared_ptr<const gltf::GltfLinkedCompositionInfo> linkedComposition;
  std::optional<container::util::ContentFingerprint> sourceFingerprint;
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
  // Reject required features unavailable to the consumer and omit their
  // declared optional payloads before consuming the core fallback.
  std::vector<std::string> unavailableRuntimeExtensions;
  // Remote import keeps embedded buffers and data URIs, and rejects file URIs.
  bool requireSelfContained{false};
  // The detached source is an incoming scene composition operand and may have no meshes.
  bool mergeInput{false};
  std::stop_token stop;
  // Identify the exact primary file read by the parser before source rewriting.
  bool fingerprintPrimarySource{false};
};

// Capability policy for consumers of static mesh primitives without materials
// or runtime scene features. Geometry codecs and accessor formats remain active.
[[nodiscard]] LoadOptions StaticGeometryLoadOptions();

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

struct GltfPrimitiveTangentVariant {
  Mesh mesh;
  // Empty for unchanged topology. Otherwise the original vertices keep their
  // indices, and each appended vertex refers back to its original attributes.
  std::vector<uint32_t> sourceVertexMap;
};

inline constexpr uint64_t kMaxLiveNormalFrameCorners = 8192;

// Builds a detached render stream for a live normal-UV edit. When seam
// splitting is enabled, distinct corner frames can share source attributes
// but need separate tangent vertices. Triangle order and count stay intact.
[[nodiscard]] GltfPrimitiveTangentVariant RegenerateGltfPrimitiveTangents(
    const Mesh&, uint32_t texCoord, bool splitTangentSeams);
[[nodiscard]] uint32_t GltfSourceNormalTexCoord(const tinygltf::Model&,
                                                const tinygltf::Primitive&);

}  // namespace gltf
}  // namespace container::geometry

