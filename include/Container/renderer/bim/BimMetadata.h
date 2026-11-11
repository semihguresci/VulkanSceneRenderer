#pragma once

#include "Container/common/CommonMath.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace container::renderer {

struct BimElementBounds {
  bool valid{false};
  glm::vec3 min{0.0f};
  glm::vec3 max{0.0f};
  glm::vec3 center{0.0f};
  glm::vec3 size{0.0f};
  float radius{0.0f};
  float floorElevation{0.0f};
};

enum class BimGeometryKind : uint32_t { Mesh = 0, Points = 1, Curves = 2 };

struct BimElementProperty {
  std::string set{};
  std::string name{};
  std::string value{};
  std::string category{};
};

// Owning import/test DTO. Runtime publications use BimMetadataStorage instead.
struct BimElementMetadata {
  uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t sourceElementIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t meshId{0};
  uint32_t sourceMaterialIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t materialIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t semanticTypeId{std::numeric_limits<uint32_t>::max()};
  uint32_t productIdentityId{0};
  glm::vec4 sourceColor{0.8f, 0.82f, 0.86f, 1.0f};
  std::string guid{};
  std::string type{};
  std::string displayName{};
  std::string objectType{};
  std::string storeyName{};
  std::string storeyId{};
  std::string materialName{};
  std::string materialCategory{};
  std::string discipline{};
  std::string phase{};
  std::string fireRating{};
  std::string loadBearing{};
  std::string status{};
  std::string sourceId{};
  std::vector<BimElementProperty> properties{};
  bool transparent{false};
  bool doubleSided{false};
  BimElementBounds bounds{};
  BimGeometryKind geometryKind{BimGeometryKind::Mesh};
};

// A resolved row is cheap to copy and never owns model text. Views remain valid
// only while the source DTO/storage is alive and its publication is unchanged.
struct BimElementMetadataView {
  uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t sourceElementIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t meshId{0};
  uint32_t sourceMaterialIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t materialIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t semanticTypeId{std::numeric_limits<uint32_t>::max()};
  uint32_t productIdentityId{0};
  glm::vec4 sourceColor{0.8f, 0.82f, 0.86f, 1.0f};
  std::string_view guid{};
  std::string_view type{};
  std::string_view displayName{};
  std::string_view objectType{};
  std::string_view storeyName{};
  std::string_view storeyId{};
  std::string_view materialName{};
  std::string_view materialCategory{};
  std::string_view discipline{};
  std::string_view phase{};
  std::string_view fireRating{};
  std::string_view loadBearing{};
  std::string_view status{};
  std::string_view sourceId{};
  std::span<const BimElementProperty> properties{};
  bool transparent{false};
  bool doubleSided{false};
  BimElementBounds bounds{};
  BimGeometryKind geometryKind{BimGeometryKind::Mesh};

  BimElementMetadataView() = default;
  BimElementMetadataView(const BimElementMetadata &metadata) noexcept;
};

class BimMetadataTableView;

// Model-scoped storage: eleven exact-text dictionary IDs per row and packed
// GUID/name/source-ID bytes. IDs never escape their owning model. Hashes only
// select candidates; full text equality always decides dictionary identity.
class BimMetadataStorage {
public:
  BimMetadataStorage() noexcept;
  ~BimMetadataStorage();
  BimMetadataStorage(const BimMetadataStorage &other);
  BimMetadataStorage &operator=(const BimMetadataStorage &other);
  BimMetadataStorage(BimMetadataStorage &&other) noexcept;
  BimMetadataStorage &operator=(BimMetadataStorage &&other) noexcept;

  void clear() noexcept;
  void reserve(size_t count);
  void push_back(BimElementMetadata metadata);
  void pop_back() noexcept;
  // Discard the construction-only lookup map after publishing immutable rows.
  void seal() noexcept;
  // Bind the runtime material after installing the prepared row's source material.
  void setMaterialIndex(size_t index, uint32_t materialIndex);
  // Build a complete replacement before retiring this model's publication.
  void replace(BimMetadataTableView metadata);
  void swap(BimMetadataStorage &other) noexcept;

  [[nodiscard]] size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept { return size() == 0u; }
  [[nodiscard]] const void *data() const noexcept;
  [[nodiscard]] BimElementMetadataView operator[](size_t index) const noexcept;
  [[nodiscard]] BimElementMetadataView front() const noexcept { return (*this)[0]; }
  [[nodiscard]] BimElementMetadataView back() const noexcept { return (*this)[size() - 1u]; }
  [[nodiscard]] BimMetadataTableView view() const noexcept;
  [[nodiscard]] auto begin() const noexcept;
  [[nodiscard]] auto end() const noexcept;
  [[nodiscard]] size_t estimatedBytes() const noexcept;
  [[nodiscard]] size_t sharedStringCount() const noexcept;
  [[nodiscard]] static size_t recordSize() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_{};
};

// Nonowning adapter for compact runtime storage and existing owning DTO
// fixtures. Iterators resolve a row by value; no temporary owning row is built.
class BimMetadataTableView {
public:
  class Iterator {
  public:
    using iterator_category = std::forward_iterator_tag;
    using iterator_concept = std::forward_iterator_tag;
    using value_type = BimElementMetadataView;
    using difference_type = std::ptrdiff_t;
    using reference = BimElementMetadataView;

    Iterator() = default;
    [[nodiscard]] BimElementMetadataView operator*() const noexcept;
    Iterator &operator++() noexcept { ++index_; return *this; }
    Iterator operator++(int) noexcept { auto copy = *this; ++*this; return copy; }
    friend bool operator==(const Iterator &, const Iterator &) = default;

  private:
    friend class BimMetadataTableView;
    Iterator(const BimElementMetadata *dto, const BimMetadataStorage *storage,
             size_t index) noexcept : dto_(dto), storage_(storage), index_(index) {}
    const BimElementMetadata *dto_{nullptr};
    const BimMetadataStorage *storage_{nullptr};
    size_t index_{0};
  };

  BimMetadataTableView() = default;
  BimMetadataTableView(const BimElementMetadata *metadata, size_t count) noexcept
      : dto_(metadata), count_(count) {}
  BimMetadataTableView(std::span<const BimElementMetadata> metadata) noexcept
      : dto_(metadata.data()), count_(metadata.size()) {}
  BimMetadataTableView(std::span<BimElementMetadata> metadata) noexcept
      : BimMetadataTableView(std::span<const BimElementMetadata>{metadata}) {}
  BimMetadataTableView(const std::vector<BimElementMetadata> &metadata) noexcept
      : BimMetadataTableView(std::span<const BimElementMetadata>{metadata}) {}
  template <size_t N>
  BimMetadataTableView(const std::array<BimElementMetadata, N> &metadata) noexcept
      : BimMetadataTableView(std::span<const BimElementMetadata>{metadata}) {}
  template <size_t N>
  BimMetadataTableView(const BimElementMetadata (&metadata)[N]) noexcept
      : BimMetadataTableView(std::span<const BimElementMetadata>{metadata}) {}
  BimMetadataTableView(const BimMetadataStorage &metadata) noexcept
      : storage_(&metadata), count_(metadata.size()) {}

  [[nodiscard]] size_t size() const noexcept { return count_; }
  [[nodiscard]] bool empty() const noexcept { return count_ == 0u; }
  [[nodiscard]] const void *data() const noexcept {
    return storage_ != nullptr ? storage_->data() : dto_;
  }
  [[nodiscard]] BimElementMetadataView operator[](size_t index) const noexcept {
    return storage_ != nullptr ? (*storage_)[index] : BimElementMetadataView{dto_[index]};
  }
  [[nodiscard]] BimElementMetadataView front() const noexcept { return (*this)[0]; }
  [[nodiscard]] BimElementMetadataView back() const noexcept { return (*this)[count_ - 1u]; }
  [[nodiscard]] Iterator begin() const noexcept { return {dto_, storage_, 0u}; }
  [[nodiscard]] Iterator end() const noexcept { return {dto_, storage_, count_}; }

private:
  const BimElementMetadata *dto_{nullptr};
  const BimMetadataStorage *storage_{nullptr};
  size_t count_{0};
};

inline BimElementMetadataView
BimMetadataTableView::Iterator::operator*() const noexcept {
  return storage_ != nullptr ? (*storage_)[index_] : BimElementMetadataView{dto_[index_]};
}

inline auto BimMetadataStorage::begin() const noexcept { return view().begin(); }
inline auto BimMetadataStorage::end() const noexcept { return view().end(); }

} // namespace container::renderer
