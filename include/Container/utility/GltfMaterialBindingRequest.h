#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace container::material {

enum class MaterialBindingEditOperation : uint8_t { Assign, MakeUnique };

// One guarded, one-shot primitive binding operation. Assign carries an explicit
// destination source/runtime pair; -1 keeps a runtime-only identity. MakeUnique
// carries the exact current pair to duplicate, not the future appended IDs.
// The scene owner separately proves live graph/catalog identities and resource
// provenance before staging any publication. Packed/deduplicated GPU resources
// are never used to reconstruct a source material.
struct GltfMaterialBindingRequest {
  uint32_t graphNode{UINT32_MAX}, expectedRuntimeMaterial{UINT32_MAX};
  MaterialBindingEditOperation operation{MaterialBindingEditOperation::Assign};
  int32_t sourceMaterial{-1};
  uint32_t runtimeMaterial{UINT32_MAX};
  std::optional<std::string> name;
};

} // namespace container::material
