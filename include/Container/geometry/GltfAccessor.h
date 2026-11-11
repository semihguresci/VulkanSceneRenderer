#pragma once

#include <tiny_gltf.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

namespace container::geometry::gltf {

enum class AccessorUsage { Generic, VertexAttribute, Indices };

struct AccessorReadOptions {
  AccessorUsage usage{AccessorUsage::Generic};
  // Experimental binary64 is enabled only with this flag AND the asset's
  // explicit extensionsRequired declaration. It is never normalized.
  bool allowFloat64{false};
  // Preserve the static importer's established repair of invalid FLOAT
  // normals/tangents. This exception never permits unsafe DOUBLE conversion.
  bool allowNonFiniteFloatFrames{false};
};

[[nodiscard]] bool HasRequiredExtension(const tinygltf::Model& source,
                                        std::string_view extension);

// Immutable view of one checked accessor, with no count-sized scratch array. The source model
// and its buffers must outlive this view and must not be structurally modified.
// Sparse overrides are resolved against validated raw unsigned indices; source
// buffers/accessors are never rewritten or densified. Matrices read in glTF
// column-major component order, respecting padding between encoded columns.
class CheckedAccessor {
public:
  CheckedAccessor(const tinygltf::Model& source, int index,
                  std::string_view label, AccessorReadOptions options = {},
                  const std::stop_token& stop = {});
  [[nodiscard]] size_t count() const { return accessor_->count; }
  [[nodiscard]] size_t components() const { return components_; }
  [[nodiscard]] const tinygltf::Accessor& accessor() const { return *accessor_; }
  void readFloats(size_t element, std::span<float> destination) const;
  void readDoubles(size_t element, std::span<double> destination) const;
  // Unsigned, unnormalized source components only; no floating-point roundtrip.
  void readUnsigned(size_t element, std::span<uint32_t> destination) const;
  [[nodiscard]] size_t encodedElementSize() const { return encodedStride_; }
  // Exact stored components, including normalized integer extrema and IDs;
  // sparse/zero bases are resolved and matrix padding is zero-filled.
  void readEncodedElement(size_t element, std::span<unsigned char> destination) const;

private:
  [[noreturn]] void fail(std::string_view message) const;
  [[nodiscard]] const unsigned char* elementBytes(size_t element) const;
  [[nodiscard]] double component(const unsigned char* bytes, size_t component) const;
  const tinygltf::Accessor* accessor_{};
  std::string label_;
  AccessorReadOptions options_;
  size_t components_{};
  size_t componentWidth_{};
  size_t encodedStride_{};
  std::array<size_t,16> componentOffsets_{};
  const unsigned char* dense_{};
  size_t denseStride_{};
  const unsigned char* sparseIndices_{};
  const unsigned char* sparseValues_{};
  size_t sparseCount_{};
  size_t sparseIndexWidth_{};
};

} // namespace container::geometry::gltf
