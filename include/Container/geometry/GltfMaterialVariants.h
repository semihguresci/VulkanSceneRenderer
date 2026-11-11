#pragma once

#include <tiny_gltf.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace container::geometry::gltf {

struct MaterialVariantPrimitive {
  size_t sourceMesh{}, sourcePrimitive{};
  // -1 is glTF's implicit default material, not material zero.
  int32_t defaultMaterial{-1};
  // Sorted by variant index; identity is the source index, never its name.
  std::vector<std::pair<size_t,int32_t>> mappings;
};

struct MaterialVariantGrowthLimits {
  uint64_t maxMaterials{1'000'000};
  uint64_t maxMeshes{1'000'000};
  uint64_t maxPrimitives{1'000'000};
  uint64_t maxVariants{1'000'000};
  uint64_t maxMappings{4'000'000};
  // Conservative logical copied storage, including retained opaque metadata
  // and the temporary variant-identity validation table. Not allocator padding.
  uint64_t maxOwnedBytes{64ull * 1024 * 1024};
  uint64_t maxMetadataValues{1'000'000};
  uint64_t maxTraversalSteps{8'000'000};
  uint32_t maxMetadataDepth{64};
};

class MaterialVariantLibrary {
public:
  [[nodiscard]] bool hasVariants() const { return bool(rootExtension_); }
  [[nodiscard]] std::span<const std::string> names() const { return names_; }
  [[nodiscard]] std::span<const MaterialVariantPrimitive> primitives() const { return primitives_; }
  [[nodiscard]] size_t materialCount() const { return materialCount_; }
  [[nodiscard]] const MaterialVariantPrimitive& primitive(size_t mesh,size_t primitive) const;

  // Rebind only the appended source-material identity range. No material,
  // resource or Model is copied, and defaults/mappings never change. The caller
  // supplies its current source count to reject a stale library context. Shrink
  // requires separate tail-removal authority and is always refused here.
  // Equal counts validate but return nullopt, avoiding an unnecessary clone.
  // All topology/schema/storage/depth/work bounds are checked before copying.
  [[nodiscard]] std::optional<MaterialVariantLibrary> withExtendedMaterialCount(
      size_t expectedCurrentMaterialCount,size_t newMaterialCount,
      const MaterialVariantGrowthLimits& limits = {}) const;

  // Prepare metadata for removal of exactly the last source material. Refuses
  // zero/stale counts and any retained default/mapping reference to that tail.
  // This proves variant metadata compatibility only: the owner must separately
  // authorize resource/history ownership and all other material references.
  [[nodiscard]] MaterialVariantLibrary withRemovedLastMaterial(
      size_t expectedCurrentMaterialCount,
      const MaterialVariantGrowthLimits& limits = {}) const;

private:
  void validateMaterialCountChange(size_t expectedCurrentMaterialCount,
      size_t newMaterialCount,const MaterialVariantGrowthLimits&) const;
  friend MaterialVariantLibrary ParseMaterialVariants(const tinygltf::Model&, const std::stop_token&);
  friend void CopyMaterialVariantMetadataForExport(tinygltf::Model&,const MaterialVariantLibrary&,
      std::span<const int32_t>,std::span<const struct MaterialVariantExportPrimitive>);
  size_t materialCount_{};
  bool required_{false};
  std::vector<std::string> names_;
  std::vector<MaterialVariantPrimitive> primitives_;
  std::vector<size_t> meshOffsets_;
  // Immutable root payload sharing also keeps library move/swap independent of
  // TinyGLTF Value's allocating MSVC map-sentinel move constructor.
  std::shared_ptr<const tinygltf::Value> rootExtension_;
  std::vector<std::optional<tinygltf::Value>> primitiveExtensions_;
};

// Validates every primitive/default/mapping before returning an immutable
// library. Error messages include source mesh/primitive/mapping context. The
// source, material objects, resources and unknown metadata are never modified.
// Duplicate/empty variant names are legal; duplicate variant IDs in one
// primitive's mappings are invalid even when they name the same material.
[[nodiscard]] MaterialVariantLibrary ParseMaterialVariants(const tinygltf::Model& source,
                                                            const std::stop_token& stop = {});

struct MaterialVariantAssignment {
  size_t sourceMesh{}, sourcePrimitive{};
  int32_t sourceMaterial{-1};
  int32_t runtimeMaterial{-1};
};
struct MaterialVariantSelectionPlan {
  std::optional<size_t> selectedVariant;
  // Complete primitive assignment, including defaults for unmapped primitives.
  std::vector<MaterialVariantAssignment> assignments;
};

// Pure planning: validate selection and all material translations before the
// caller touches live draws. An empty source->runtime table uses identity IDs;
// otherwise it must cover the complete imported material table. The caller's
// implicit default ID resolves primitive.material==-1. Shared material IDs stay
// shared; selection never clones or resets material values/animation state.
[[nodiscard]] MaterialVariantSelectionPlan planMaterialVariantSelection(
    const MaterialVariantLibrary& library,std::optional<size_t> selectedVariant,
    std::span<const int32_t> sourceMaterialToRuntime = {},int32_t implicitDefaultRuntime = 0);

struct MaterialVariantExportPrimitive {
  size_t sourceMesh{}, sourcePrimitive{};
  size_t destinationMesh{}, destinationPrimitive{};
};

// Copies/remaps only variant extension metadata onto rebuilt exported meshes.
// Current destination primitive.material values are preserved: a saved selected
// appearance becomes the reloaded file's Default. Root names, mapping order and
// all unknown extras/extensions are retained. Multiple emitted instances may
// reference one source primitive. Validation completes before any mutation.
void CopyMaterialVariantMetadataForExport(
    tinygltf::Model& destination,const MaterialVariantLibrary& library,
    std::span<const int32_t> sourceMaterialToExport,
    std::span<const MaterialVariantExportPrimitive> primitives);

} // namespace container::geometry::gltf
