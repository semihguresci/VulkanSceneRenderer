#pragma once

#include "Container/renderer/bim/BimMetadata.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace container::ui {

// Search metadata is immutable between completed model publications. Keep the
// six fields separate so a query cannot match across neighboring field values.
class BimElementSearchCache {
public:
  // The publication revision must advance when searchable metadata is replaced
  // or edited, including replacements that reuse the same source/storage.
  // A null source clears the cache. Returns true when metadata was rebuilt.
  bool refresh(const void *source, uint64_t publicationRevision,
               renderer::BimMetadataTableView metadata);
  void clear();

  // Matches preserve metadata order. Unchanged metadata and case-equivalent
  // queries reuse the result without rescanning elements. Returned spans remain
  // valid until a metadata refresh, clear, or a different normalized query.
  [[nodiscard]] std::span<const size_t> matches(std::string_view query);

  // Cumulative counts survive clear, allowing lifecycle and steady-frame
  // checks.
  [[nodiscard]] uint64_t metadataBuildCount() const {
    return metadataBuildCount_;
  }
  [[nodiscard]] uint64_t matchBuildCount() const { return matchBuildCount_; }
  [[nodiscard]] bool usesInternedLabels() const { return internedLabels_; }
  [[nodiscard]] size_t internedLabelCount() const {
    return labelOffsets_.size() > 1u ? labelOffsets_.size() - 2u : 0u;
  }
  // Owned capacity estimate, excluding allocator overhead. Query capacities
  // include characters held inside their string objects. Constant time.
  [[nodiscard]] size_t estimatedBytes() const {
    return text_.capacity() + fieldOffsets_.capacity() * sizeof(size_t) +
           labelText_.capacity() + labelOffsets_.capacity() * sizeof(size_t) +
           labelIds_.capacity() * sizeof(LabelIds) +
           labelMatchBits_.capacity() * sizeof(uint64_t) +
           (allIndices_.capacity() + matches_.capacity()) * sizeof(size_t) +
           rawQuery_.capacity() + normalizedQuery_.capacity() + 2u;
  }

private:
  static constexpr size_t kSearchFieldCount = 6u;
  static constexpr size_t kIdentityFieldCount = 3u;
  using LabelIds = std::array<uint32_t, 3>;

  const void *source_{nullptr};
  uint64_t publicationRevision_{0};
  const void *metadata_{nullptr};
  size_t metadataCount_{0};
  // GUID/source/name remain packed, length-aware text. Repeated categorical
  // fields use 32-bit IDs into a shared normalized pool when that is smaller;
  // high-cardinality metadata retains all six fields in the packed arena.
  std::vector<char> text_{};
  std::vector<size_t> fieldOffsets_{};
  std::vector<char> labelText_{};
  std::vector<size_t> labelOffsets_{};
  std::vector<LabelIds> labelIds_{};
  std::vector<uint64_t> labelMatchBits_{};
  std::vector<size_t> allIndices_{};
  std::vector<size_t> matches_{};
  std::string rawQuery_{};
  std::string normalizedQuery_{};
  bool rawQueryValid_{false};
  bool matchesValid_{false};
  bool internedLabels_{false};
  uint64_t metadataBuildCount_{0};
  uint64_t matchBuildCount_{0};
};

} // namespace container::ui
