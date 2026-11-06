#include "Container/renderer/bim/BimMetadata.h"

#include <array>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace container::renderer {

BimElementMetadataView::BimElementMetadataView(
    const BimElementMetadata &metadata) noexcept
    : objectIndex(metadata.objectIndex),
      sourceElementIndex(metadata.sourceElementIndex), meshId(metadata.meshId),
      sourceMaterialIndex(metadata.sourceMaterialIndex),
      materialIndex(metadata.materialIndex), semanticTypeId(metadata.semanticTypeId),
      productIdentityId(metadata.productIdentityId), sourceColor(metadata.sourceColor),
      guid(metadata.guid), type(metadata.type), displayName(metadata.displayName),
      objectType(metadata.objectType), storeyName(metadata.storeyName),
      storeyId(metadata.storeyId), materialName(metadata.materialName),
      materialCategory(metadata.materialCategory), discipline(metadata.discipline),
      phase(metadata.phase), fireRating(metadata.fireRating),
      loadBearing(metadata.loadBearing), status(metadata.status), sourceId(metadata.sourceId),
      properties(metadata.properties), transparent(metadata.transparent),
      doubleSided(metadata.doubleSided), bounds(metadata.bounds),
      geometryKind(metadata.geometryKind) {}

namespace {

enum SharedField : size_t {
  Type, ObjectType, StoreyName, StoreyId, MaterialName, MaterialCategory,
  Discipline, Phase, FireRating, LoadBearing, Status, SharedFieldCount
};

struct PackedRange {
  size_t begin{0};
  size_t end{0};
};

struct MetadataRecord {
  uint32_t objectIndex{0};
  uint32_t sourceElementIndex{0};
  uint32_t meshId{0};
  uint32_t sourceMaterialIndex{0};
  uint32_t materialIndex{0};
  uint32_t semanticTypeId{0};
  uint32_t productIdentityId{0};
  glm::vec4 sourceColor{0.0f};
  std::array<uint32_t, SharedFieldCount> shared{};
  // GUID, display name and source ID are adjacent, independently bounded.
  std::array<size_t, 4> textOffsets{};
  std::vector<BimElementProperty> properties{};
  bool transparent{false};
  bool doubleSided{false};
  BimElementBounds bounds{};
  BimGeometryKind geometryKind{BimGeometryKind::Mesh};
};

struct ExactTextHash {
  using is_transparent = void;
  [[nodiscard]] size_t operator()(std::string_view value) const noexcept {
    return std::hash<std::string_view>{}(value);
  }
  [[nodiscard]] size_t operator()(const std::string &value) const noexcept {
    return (*this)(std::string_view{value});
  }
};

size_t propertyCapacityBytes(const std::vector<BimElementProperty> &properties) {
  size_t bytes = properties.capacity() * sizeof(BimElementProperty);
  for (const auto &property : properties) {
    bytes += property.set.capacity() + property.name.capacity() +
             property.value.capacity() + property.category.capacity() + 4u;
  }
  return bytes;
}

std::string_view packedText(const std::vector<char> &text, size_t begin,
                            size_t end) noexcept {
  return begin == end ? std::string_view{}
                      : std::string_view{text.data() + begin, end - begin};
}

// DTO materialization is confined to explicit replacement/import work. No
// resolved runtime row retains an owning mirror of these strings.
BimElementMetadata owningMetadata(BimElementMetadataView view) {
  return {.objectIndex = view.objectIndex,
          .sourceElementIndex = view.sourceElementIndex,
          .meshId = view.meshId,
          .sourceMaterialIndex = view.sourceMaterialIndex,
          .materialIndex = view.materialIndex,
          .semanticTypeId = view.semanticTypeId,
          .productIdentityId = view.productIdentityId,
          .sourceColor = view.sourceColor,
          .guid = std::string{view.guid},
          .type = std::string{view.type},
          .displayName = std::string{view.displayName},
          .objectType = std::string{view.objectType},
          .storeyName = std::string{view.storeyName},
          .storeyId = std::string{view.storeyId},
          .materialName = std::string{view.materialName},
          .materialCategory = std::string{view.materialCategory},
          .discipline = std::string{view.discipline},
          .phase = std::string{view.phase},
          .fireRating = std::string{view.fireRating},
          .loadBearing = std::string{view.loadBearing},
          .status = std::string{view.status},
          .sourceId = std::string{view.sourceId},
          .properties = {view.properties.begin(), view.properties.end()},
          .transparent = view.transparent,
          .doubleSided = view.doubleSided,
          .bounds = view.bounds,
          .geometryKind = view.geometryKind};
}

} // namespace

struct BimMetadataStorage::Impl {
  using Lookup = std::unordered_map<std::string, uint32_t, ExactTextHash,
                                    std::equal_to<>>;
  std::vector<MetadataRecord> records{};
  std::vector<char> text{};
  std::vector<char> sharedText{};
  std::vector<PackedRange> sharedRanges{};
  Lookup sharedIds{};
  bool lookupReady{true};
  size_t propertyBytes{0};
  size_t lookupTextBytes{0};

  void ensureLookup() {
    if (lookupReady)
      return;
    Lookup replacement;
    replacement.reserve(sharedRanges.size());
    size_t replacementTextBytes = 0u;
    for (size_t index = 0u; index < sharedRanges.size(); ++index) {
      const auto range = sharedRanges[index];
      auto [entry, inserted] = replacement.emplace(
          packedText(sharedText, range.begin, range.end),
          static_cast<uint32_t>(index + 1u));
      (void)inserted;
      replacementTextBytes += entry->first.capacity() + 1u;
    }
    sharedIds.swap(replacement);
    lookupTextBytes = replacementTextBytes;
    lookupReady = true;
  }

  uint32_t intern(std::string_view value) {
    if (value.empty())
      return 0u;
    ensureLookup();
    if (const auto existing = sharedIds.find(value); existing != sharedIds.end())
      return existing->second;
    if (sharedRanges.size() >= std::numeric_limits<uint32_t>::max())
      throw std::length_error("BIM shared string ID count is too large");
    if (value.size() > sharedText.max_size() - sharedText.size())
      throw std::length_error("BIM shared metadata text is too large");
    const size_t oldTextSize = sharedText.size();
    const size_t oldRangeSize = sharedRanges.size();
    const auto id = static_cast<uint32_t>(oldRangeSize + 1u);
    try {
      sharedText.insert(sharedText.end(), value.begin(), value.end());
      sharedRanges.push_back({oldTextSize, sharedText.size()});
      auto [entry, inserted] = sharedIds.emplace(std::string{value}, id);
      (void)inserted;
      lookupTextBytes += entry->first.capacity() + 1u;
    } catch (...) {
      sharedText.resize(oldTextSize);
      sharedRanges.resize(oldRangeSize);
      throw;
    }
    return id;
  }

  std::string_view shared(uint32_t id) const noexcept {
    if (id == 0u)
      return {};
    const auto range = sharedRanges[id - 1u];
    return packedText(sharedText, range.begin, range.end);
  }
};

BimMetadataStorage::BimMetadataStorage() noexcept = default;
BimMetadataStorage::~BimMetadataStorage() = default;
BimMetadataStorage::BimMetadataStorage(const BimMetadataStorage &other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}
BimMetadataStorage &BimMetadataStorage::operator=(const BimMetadataStorage &other) {
  if (this != &other) {
    BimMetadataStorage replacement{other};
    swap(replacement);
  }
  return *this;
}
BimMetadataStorage::BimMetadataStorage(BimMetadataStorage &&other) noexcept = default;
BimMetadataStorage &BimMetadataStorage::operator=(BimMetadataStorage &&other) noexcept = default;

void BimMetadataStorage::clear() noexcept { impl_.reset(); }

void BimMetadataStorage::reserve(size_t count) {
  if (!impl_)
    impl_ = std::make_unique<Impl>();
  impl_->records.reserve(count);
  // Dictionary capacity follows actual unique labels, never the row count.
}

void BimMetadataStorage::push_back(BimElementMetadata metadata) {
  if (!impl_)
    impl_ = std::make_unique<Impl>();
  MetadataRecord record;
  record.objectIndex = metadata.objectIndex;
  record.sourceElementIndex = metadata.sourceElementIndex;
  record.meshId = metadata.meshId;
  record.sourceMaterialIndex = metadata.sourceMaterialIndex;
  record.materialIndex = metadata.materialIndex;
  record.semanticTypeId = metadata.semanticTypeId;
  record.productIdentityId = metadata.productIdentityId;
  record.sourceColor = metadata.sourceColor;
  const std::array<std::string_view, SharedFieldCount> fields{
      metadata.type, metadata.objectType, metadata.storeyName, metadata.storeyId,
      metadata.materialName, metadata.materialCategory, metadata.discipline,
      metadata.phase, metadata.fireRating, metadata.loadBearing, metadata.status};
  for (size_t index = 0u; index < fields.size(); ++index)
    record.shared[index] = impl_->intern(fields[index]);
  const std::array<std::string_view, 3> uniqueFields{
      metadata.guid, metadata.displayName, metadata.sourceId};
  size_t bytes = 0u;
  for (const auto value : uniqueFields) {
    if (value.size() > impl_->text.max_size() - impl_->text.size() - bytes)
      throw std::length_error("BIM packed metadata text is too large");
    bytes += value.size();
  }
  record.properties = std::move(metadata.properties);
  const size_t propertyBytes = propertyCapacityBytes(record.properties);
  record.transparent = metadata.transparent;
  record.doubleSided = metadata.doubleSided;
  record.bounds = metadata.bounds;
  record.geometryKind = metadata.geometryKind;
  const size_t oldTextSize = impl_->text.size();
  try {
    record.textOffsets[0] = oldTextSize;
    for (size_t index = 0u; index < uniqueFields.size(); ++index) {
      const auto value = uniqueFields[index];
      if (!value.empty())
        impl_->text.insert(impl_->text.end(), value.begin(), value.end());
      record.textOffsets[index + 1u] = impl_->text.size();
    }
    impl_->records.push_back(std::move(record));
    impl_->propertyBytes += propertyBytes;
  } catch (...) {
    impl_->text.resize(oldTextSize);
    throw;
  }
}

void BimMetadataStorage::pop_back() noexcept {
  if (empty())
    return;
  const auto &record = impl_->records.back();
  impl_->propertyBytes -= propertyCapacityBytes(record.properties);
  impl_->text.resize(record.textOffsets[0]);
  impl_->records.pop_back();
  // Shared labels may also be used by earlier rows. The whole dictionary is
  // retired by clear(), after progressive row/property destruction completes.
}

void BimMetadataStorage::seal() noexcept {
  if (!impl_)
    return;
  Impl::Lookup{}.swap(impl_->sharedIds);
  impl_->lookupTextBytes = 0u;
  impl_->lookupReady = false;
}

void BimMetadataStorage::replace(BimMetadataTableView metadata) {
  BimMetadataStorage replacement;
  replacement.reserve(metadata.size());
  for (const auto row : metadata)
    replacement.push_back(owningMetadata(row));
  replacement.seal();
  swap(replacement);
}

void BimMetadataStorage::swap(BimMetadataStorage &other) noexcept {
  impl_.swap(other.impl_);
}

size_t BimMetadataStorage::size() const noexcept {
  return impl_ ? impl_->records.size() : 0u;
}

const void *BimMetadataStorage::data() const noexcept {
  return impl_ ? impl_->records.data() : nullptr;
}

BimElementMetadataView BimMetadataStorage::operator[](size_t index) const noexcept {
  const auto &record = impl_->records[index];
  BimElementMetadataView view;
  view.objectIndex = record.objectIndex;
  view.sourceElementIndex = record.sourceElementIndex;
  view.meshId = record.meshId;
  view.sourceMaterialIndex = record.sourceMaterialIndex;
  view.materialIndex = record.materialIndex;
  view.semanticTypeId = record.semanticTypeId;
  view.productIdentityId = record.productIdentityId;
  view.sourceColor = record.sourceColor;
  view.guid = packedText(impl_->text, record.textOffsets[0], record.textOffsets[1]);
  view.displayName = packedText(impl_->text, record.textOffsets[1], record.textOffsets[2]);
  view.sourceId = packedText(impl_->text, record.textOffsets[2], record.textOffsets[3]);
  view.type = impl_->shared(record.shared[Type]);
  view.objectType = impl_->shared(record.shared[ObjectType]);
  view.storeyName = impl_->shared(record.shared[StoreyName]);
  view.storeyId = impl_->shared(record.shared[StoreyId]);
  view.materialName = impl_->shared(record.shared[MaterialName]);
  view.materialCategory = impl_->shared(record.shared[MaterialCategory]);
  view.discipline = impl_->shared(record.shared[Discipline]);
  view.phase = impl_->shared(record.shared[Phase]);
  view.fireRating = impl_->shared(record.shared[FireRating]);
  view.loadBearing = impl_->shared(record.shared[LoadBearing]);
  view.status = impl_->shared(record.shared[Status]);
  view.properties = record.properties;
  view.transparent = record.transparent;
  view.doubleSided = record.doubleSided;
  view.bounds = record.bounds;
  view.geometryKind = record.geometryKind;
  return view;
}

BimMetadataTableView BimMetadataStorage::view() const noexcept { return {*this}; }

size_t BimMetadataStorage::estimatedBytes() const noexcept {
  if (!impl_)
    return 0u;
  const size_t lookupBytes = impl_->sharedIds.empty() ? 0u :
      impl_->sharedIds.bucket_count() * sizeof(void *) +
      impl_->sharedIds.size() * (sizeof(Impl::Lookup::value_type) + 2u * sizeof(void *)) +
      impl_->lookupTextBytes;
  return sizeof(Impl) + impl_->records.capacity() * sizeof(MetadataRecord) +
      impl_->text.capacity() + impl_->sharedText.capacity() +
      impl_->sharedRanges.capacity() * sizeof(PackedRange) +
      impl_->propertyBytes + lookupBytes;
}

size_t BimMetadataStorage::sharedStringCount() const noexcept {
  return impl_ ? impl_->sharedRanges.size() : 0u;
}

size_t BimMetadataStorage::recordSize() noexcept { return sizeof(MetadataRecord); }

} // namespace container::renderer
