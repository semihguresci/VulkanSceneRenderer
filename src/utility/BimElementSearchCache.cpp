#include "Container/utility/BimElementSearchCache.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace container::ui {
namespace {

using NormalizedCharacters = std::array<char, 256>;
// Discovery is temporary and bounded even when every categorical value is
// unique. Such metadata uses the ordinary packed-field representation.
constexpr size_t kMaximumInternedLabels = 262'144u;
constexpr size_t kMaximumInternedTextBytes = 64u * 1024u * 1024u;

std::string normalizedSearchText(std::string_view value) {
  std::string normalized(value);
  std::ranges::transform(normalized, normalized.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return normalized;
}

std::array<std::string_view, 6>
searchFields(const renderer::BimElementMetadataView &element) {
  return {element.guid,         element.sourceId,   element.type,
          element.displayName, element.storeyName, element.materialName};
}

std::array<std::string_view, 3>
identityFields(const renderer::BimElementMetadataView &element) {
  return {element.guid, element.sourceId, element.displayName};
}

std::array<std::string_view, 3>
categoricalFields(const renderer::BimElementMetadataView &element) {
  return {element.type, element.storeyName, element.materialName};
}

size_t checkedAdd(size_t left, size_t right,
                  size_t limit = std::numeric_limits<size_t>::max()) {
  if (right > limit - left)
    throw std::length_error("BIM search storage is too large");
  return left + right;
}

void appendNormalized(std::vector<char> &text, std::string_view value,
                      const NormalizedCharacters &characters) {
  for (const unsigned char character : value)
    text.push_back(characters[character]);
}

// The construction-only keys reference immutable input metadata. Their hash
// and equality both fold bytes, so differing case shares an ID and hash
// collisions still compare complete, length-aware values. No keys point into a
// growing arena and no string allocations are required for discovery.
struct NormalizedStringHash {
  const NormalizedCharacters *characters;
  size_t operator()(std::string_view value) const noexcept {
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char character : value) {
      hash ^= static_cast<unsigned char>((*characters)[character]);
      hash *= 1099511628211ull;
    }
    return static_cast<size_t>(hash);
  }
};

struct NormalizedStringEqual {
  const NormalizedCharacters *characters;
  bool operator()(std::string_view left, std::string_view right) const noexcept {
    if (left.size() != right.size())
      return false;
    for (size_t index = 0; index < left.size(); ++index)
      if ((*characters)[static_cast<unsigned char>(left[index])] !=
          (*characters)[static_cast<unsigned char>(right[index])])
        return false;
    return true;
  }
};

} // namespace

bool BimElementSearchCache::refresh(
    const void *source, uint64_t publicationRevision,
    renderer::BimMetadataTableView metadata) {
  if (source == nullptr) {
    clear();
    return false;
  }
  if (source_ == source && publicationRevision_ == publicationRevision &&
      metadata_ == metadata.data() && metadataCount_ == metadata.size()) {
    return false;
  }

  std::vector<char> text;
  std::vector<size_t> fieldOffsets;
  std::vector<char> labelText;
  std::vector<size_t> labelOffsets;
  std::vector<LabelIds> labelIds;
  std::vector<uint64_t> labelMatchBits;
  std::vector<size_t> allIndices;
  if (metadata.size() > (fieldOffsets.max_size() - 1u) / kSearchFieldCount ||
      metadata.size() > allIndices.max_size() ||
      metadata.size() > labelIds.max_size())
    throw std::length_error("BIM search element count is too large");

  // Preserve query normalization's unsigned-byte folding in the current C
  // locale, with one lookup per byte instead of repeated CRT locale work.
  NormalizedCharacters characters;
  for (size_t character = 0; character < characters.size(); ++character)
    characters[character] =
        static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  using LabelLookup =
      std::unordered_map<std::string_view, uint32_t, NormalizedStringHash,
                         NormalizedStringEqual>;
  LabelLookup labels{0u, NormalizedStringHash{&characters},
                     NormalizedStringEqual{&characters}};
  std::vector<std::string_view> uniqueLabels;
  labels.reserve(std::min(metadata.size(), size_t{4'096u}));
  uniqueLabels.reserve(std::min(metadata.size(), size_t{4'096u}));
  size_t textBytes = 0u;
  size_t categoricalTextBytes = 0u;
  size_t uniqueLabelBytes = 0u;
  bool discoveryAbandoned = false;
  for (const auto element : metadata) {
    for (const auto field : searchFields(element))
      textBytes = checkedAdd(textBytes, field.size(), text.max_size());
    for (const auto field : categoricalFields(element)) {
      categoricalTextBytes = checkedAdd(categoricalTextBytes, field.size());
      if (discoveryAbandoned || field.empty() || labels.contains(field))
        continue;
      if (uniqueLabels.size() >= kMaximumInternedLabels ||
          uniqueLabels.size() >= std::numeric_limits<uint32_t>::max() ||
          field.size() > kMaximumInternedTextBytes - uniqueLabelBytes) {
        discoveryAbandoned = true;
        labels.clear();
        std::vector<std::string_view>{}.swap(uniqueLabels);
        continue;
      }
      const auto id = static_cast<uint32_t>(uniqueLabels.size() + 1u);
      labels.emplace(field, id);
      uniqueLabels.push_back(field);
      uniqueLabelBytes += field.size();
    }
  }

  // The packed baseline needs three categorical offsets and all categorical
  // text per entity. Only retain a dictionary if its entire owned capacity,
  // including IDs and reusable query membership bits, is smaller.
  const size_t packedCategoricalBytes = checkedAdd(
      categoricalTextBytes, metadata.size() * 3u * sizeof(size_t));
  bool internedLabels = false;
  if (!discoveryAbandoned && !metadata.empty()) {
    const size_t bitWords = (uniqueLabels.size() + 1u + 63u) / 64u;
    size_t candidateBytes = checkedAdd(
        uniqueLabelBytes, (uniqueLabels.size() + 2u) * sizeof(size_t));
    candidateBytes =
        checkedAdd(candidateBytes, metadata.size() * sizeof(LabelIds));
    candidateBytes = checkedAdd(candidateBytes, bitWords * sizeof(uint64_t));
    if (candidateBytes < packedCategoricalBytes) {
      labelText.reserve(uniqueLabelBytes);
      labelOffsets.reserve(uniqueLabels.size() + 2u);
      labelIds.reserve(metadata.size());
      labelMatchBits.resize(bitWords);
      size_t actualBytes = checkedAdd(
          labelText.capacity(), labelOffsets.capacity() * sizeof(size_t));
      actualBytes =
          checkedAdd(actualBytes, labelIds.capacity() * sizeof(LabelIds));
      actualBytes = checkedAdd(
          actualBytes, labelMatchBits.capacity() * sizeof(uint64_t));
      internedLabels = actualBytes < packedCategoricalBytes;
    }
  }
  if (internedLabels) {
    labelOffsets.push_back(0u);
    labelOffsets.push_back(0u); // ID zero is the empty categorical value.
    for (const auto label : uniqueLabels) {
      appendNormalized(labelText, label, characters);
      labelOffsets.push_back(labelText.size());
    }
  } else {
    std::vector<char>{}.swap(labelText);
    std::vector<size_t>{}.swap(labelOffsets);
    std::vector<LabelIds>{}.swap(labelIds);
    std::vector<uint64_t>{}.swap(labelMatchBits);
  }

  const size_t packedFieldCount =
      internedLabels ? kIdentityFieldCount : kSearchFieldCount;
  text.reserve(internedLabels ? textBytes - categoricalTextBytes : textBytes);
  fieldOffsets.reserve(metadata.size() * packedFieldCount + 1u);
  allIndices.reserve(metadata.size());
  fieldOffsets.push_back(0u);
  for (size_t index = 0; index < metadata.size(); ++index) {
    const auto element = metadata[index];
    if (internedLabels) {
      for (const auto field : identityFields(element)) {
        appendNormalized(text, field, characters);
        fieldOffsets.push_back(text.size());
      }
      const auto categories = categoricalFields(element);
      LabelIds ids{};
      for (size_t field = 0; field < ids.size(); ++field)
        if (!categories[field].empty())
          ids[field] = labels.at(categories[field]);
      labelIds.push_back(ids);
    } else {
      for (const auto field : searchFields(element)) {
        appendNormalized(text, field, characters);
        fieldOffsets.push_back(text.size());
      }
    }
    allIndices.push_back(index);
  }

  // All work above uses local owning storage. Publish only after successful
  // construction; failed allocation/normalization retains the old cache/query.
  text_.swap(text);
  fieldOffsets_.swap(fieldOffsets);
  labelText_.swap(labelText);
  labelOffsets_.swap(labelOffsets);
  labelIds_.swap(labelIds);
  labelMatchBits_.swap(labelMatchBits);
  allIndices_.swap(allIndices);
  std::vector<size_t>{}.swap(matches_);
  source_ = source;
  publicationRevision_ = publicationRevision;
  metadata_ = metadata.data();
  metadataCount_ = metadata.size();
  internedLabels_ = internedLabels;
  matchesValid_ = false;
  ++metadataBuildCount_;
  return true;
}

void BimElementSearchCache::clear() {
  const auto metadataBuilds = metadataBuildCount_;
  const auto queryBuilds = matchBuildCount_;
  // Unload must release the large normalized index, including vector capacity.
  // Keep cumulative diagnostics while retiring all model/query-owned storage.
  *this = {};
  metadataBuildCount_ = metadataBuilds;
  matchBuildCount_ = queryBuilds;
}

std::span<const size_t> BimElementSearchCache::matches(std::string_view query) {
  if (!rawQueryValid_ || rawQuery_ != query) {
    std::string normalized = normalizedSearchText(query);
    rawQuery_.assign(query);
    rawQueryValid_ = true;
    if (normalizedQuery_ != normalized) {
      normalizedQuery_ = std::move(normalized);
      matchesValid_ = false;
    }
  }
  if (!matchesValid_) {
    matches_.clear();
    if (!normalizedQuery_.empty()) {
      if (internedLabels_) {
        std::ranges::fill(labelMatchBits_, uint64_t{0u});
        for (size_t id = 1u; id + 1u < labelOffsets_.size(); ++id) {
          const size_t begin = labelOffsets_[id];
          const size_t end = labelOffsets_[id + 1u];
          const std::string_view label{labelText_.data() + begin, end - begin};
          if (label.find(normalizedQuery_) != std::string_view::npos)
            labelMatchBits_[id / 64u] |= uint64_t{1u} << (id % 64u);
        }
      }
      const size_t packedFieldCount =
          internedLabels_ ? kIdentityFieldCount : kSearchFieldCount;
      for (size_t index = 0; index < metadataCount_; ++index) {
        if (internedLabels_ &&
            std::ranges::any_of(labelIds_[index], [this](uint32_t id) {
              return (labelMatchBits_[id / 64u] &
                      (uint64_t{1u} << (id % 64u))) != 0u;
            })) {
          matches_.push_back(index);
          continue;
        }
        const size_t firstField = index * packedFieldCount;
        for (size_t field = 0; field < packedFieldCount; ++field) {
          const size_t begin = fieldOffsets_[firstField + field];
          const size_t end = fieldOffsets_[firstField + field + 1u];
          if (begin == end)
            continue;
          const std::string_view value{text_.data() + begin, end - begin};
          if (value.find(normalizedQuery_) != std::string_view::npos) {
            matches_.push_back(index);
            break;
          }
        }
      }
    }
    matchesValid_ = true;
    ++matchBuildCount_;
  }
  return normalizedQuery_.empty() ? std::span<const size_t>{allIndices_}
                                  : std::span<const size_t>{matches_};
}

} // namespace container::ui
