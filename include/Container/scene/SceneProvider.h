#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>

namespace container::scene {

enum class SceneProviderKind {
  Mesh,
  Bim,
  GaussianSplatting,
  RadianceField,
};

struct SceneProviderId {
  std::string value{};

  [[nodiscard]] bool operator==(const SceneProviderId &other) const {
    return value == other.value;
  }
};

struct SceneProviderRevision {
  uint64_t geometry{0};
  uint64_t materials{0};
  uint64_t instances{0};
};

struct SceneProviderBounds {
  glm::vec3 min{0.0f};
  glm::vec3 max{0.0f};
  bool valid{false};
};

struct SceneProviderTriangleBatch {
  uint32_t firstIndex{0};
  uint32_t indexCount{0};
  int32_t materialIndex{-1};
  bool doubleSided{false};
  bool transparent{false};
  uint32_t instanceCount{1};
};

// Topology is published once and shared by providers, snapshots and extraction.
// Retained snapshots own their data even after the source model is unloaded.
class ImmutableSceneTriangleBatches {
public:
  ImmutableSceneTriangleBatches() = default;
  ImmutableSceneTriangleBatches(
      std::vector<SceneProviderTriangleBatch> batches) {
    if (!batches.empty()) {
      batches_ =
          std::make_shared<const std::vector<SceneProviderTriangleBatch>>(
              std::move(batches));
    }
  }
  ImmutableSceneTriangleBatches(
      std::initializer_list<SceneProviderTriangleBatch> batches)
      : ImmutableSceneTriangleBatches(
            std::vector<SceneProviderTriangleBatch>(batches)) {}

  [[nodiscard]] std::size_t size() const {
    return batches_ ? batches_->size() : 0u;
  }
  [[nodiscard]] bool empty() const { return size() == 0u; }
  [[nodiscard]] const SceneProviderTriangleBatch *data() const {
    return batches_ ? batches_->data() : nullptr;
  }
  [[nodiscard]] const SceneProviderTriangleBatch *begin() const {
    return data();
  }
  [[nodiscard]] const SceneProviderTriangleBatch *end() const {
    return batches_ ? batches_->data() + batches_->size() : nullptr;
  }
  [[nodiscard]] const SceneProviderTriangleBatch &
  operator[](std::size_t index) const {
    return (*batches_)[index];
  }
  [[nodiscard]] const SceneProviderTriangleBatch &front() const {
    return batches_->front();
  }
  [[nodiscard]] std::span<const SceneProviderTriangleBatch> view() const {
    return {data(), size()};
  }

private:
  std::shared_ptr<const std::vector<SceneProviderTriangleBatch>> batches_{};
};

struct MeshSceneAsset {
  std::size_t primitiveCount{0};
  std::size_t materialCount{0};
  std::size_t instanceCount{0};
  ImmutableSceneTriangleBatches triangleBatches{};
  SceneProviderBounds bounds{};
};

struct BimSceneAsset {
  std::size_t elementCount{0};
  std::size_t meshPrimitiveCount{0};
  std::size_t meshOpaqueBatchCount{0};
  std::size_t meshTransparentBatchCount{0};
  std::size_t nativePointRangeCount{0};
  std::size_t nativeCurveRangeCount{0};
  std::size_t nativePointOpaqueRangeCount{0};
  std::size_t nativePointTransparentRangeCount{0};
  std::size_t nativeCurveOpaqueRangeCount{0};
  std::size_t nativeCurveTransparentRangeCount{0};
  ImmutableSceneTriangleBatches triangleBatches{};
  SceneProviderBounds bounds{};
};

struct GaussianSplatSceneAsset {
  std::size_t splatCount{0};
  uint32_t sphericalHarmonicCoefficientCount{0};
  SceneProviderBounds bounds{};
};

struct RadianceFieldSceneAsset {
  uint32_t fieldCount{0};
  bool hasOccupancyData{false};
  SceneProviderBounds bounds{};
};

struct SceneProviderSnapshot {
  SceneProviderKind kind{SceneProviderKind::Mesh};
  SceneProviderId id{};
  std::string displayName{};
  SceneProviderRevision revision{};
  SceneProviderBounds bounds{};
  std::size_t elementCount{0};
  std::size_t primitiveCount{0};
  std::size_t materialCount{0};
  std::size_t instanceCount{0};
  std::size_t opaqueBatchCount{0};
  std::size_t transparentBatchCount{0};
  ImmutableSceneTriangleBatches triangleBatches{};
  std::size_t nativePointRangeCount{0};
  std::size_t nativeCurveRangeCount{0};
  std::size_t nativePointOpaqueRangeCount{0};
  std::size_t nativePointTransparentRangeCount{0};
  std::size_t nativeCurveOpaqueRangeCount{0};
  std::size_t nativeCurveTransparentRangeCount{0};
  std::size_t splatCount{0};
  uint32_t sphericalHarmonicCoefficientCount{0};
  uint32_t fieldCount{0};
  bool hasOccupancyData{false};
};

class IRenderSceneProvider {
public:
  virtual ~IRenderSceneProvider() = default;

  [[nodiscard]] virtual SceneProviderSnapshot snapshot() const = 0;
};

class MeshSceneProvider final : public IRenderSceneProvider {
public:
  explicit MeshSceneProvider(SceneProviderId id,
                             std::string displayName = "Mesh scene")
      : id_(std::move(id)), displayName_(std::move(displayName)) {}

  void update(MeshSceneAsset asset, SceneProviderRevision revision,
              std::string displayName = {}) {
    asset_ = std::move(asset);
    revision_ = revision;
    if (!displayName.empty()) {
      displayName_ = std::move(displayName);
    }
  }

  [[nodiscard]] SceneProviderSnapshot snapshot() const override {
    return {
        .kind = SceneProviderKind::Mesh,
        .id = id_,
        .displayName = displayName_,
        .revision = revision_,
        .bounds = asset_.bounds,
        .elementCount = asset_.primitiveCount,
        .primitiveCount = asset_.primitiveCount,
        .materialCount = asset_.materialCount,
        .instanceCount = asset_.instanceCount,
        .opaqueBatchCount = asset_.primitiveCount,
        .transparentBatchCount = 0,
        .triangleBatches = asset_.triangleBatches,
    };
  }

  [[nodiscard]] const MeshSceneAsset &asset() const { return asset_; }

private:
  SceneProviderId id_{};
  std::string displayName_{};
  MeshSceneAsset asset_{};
  SceneProviderRevision revision_{};
};

class BimSceneProvider final : public IRenderSceneProvider {
public:
  explicit BimSceneProvider(SceneProviderId id,
                            std::string displayName = "BIM scene")
      : id_(std::move(id)), displayName_(std::move(displayName)) {}

  void update(BimSceneAsset asset, SceneProviderRevision revision,
              std::string displayName = {}) {
    asset_ = std::move(asset);
    revision_ = revision;
    if (!displayName.empty()) {
      displayName_ = std::move(displayName);
    }
  }

  [[nodiscard]] SceneProviderSnapshot snapshot() const override {
    return {
        .kind = SceneProviderKind::Bim,
        .id = id_,
        .displayName = displayName_,
        .revision = revision_,
        .bounds = asset_.bounds,
        .elementCount = asset_.elementCount,
        .primitiveCount = asset_.meshPrimitiveCount,
        .instanceCount = asset_.elementCount,
        .opaqueBatchCount = asset_.meshOpaqueBatchCount,
        .transparentBatchCount = asset_.meshTransparentBatchCount,
        .triangleBatches = asset_.triangleBatches,
        .nativePointRangeCount = asset_.nativePointRangeCount,
        .nativeCurveRangeCount = asset_.nativeCurveRangeCount,
        .nativePointOpaqueRangeCount = asset_.nativePointOpaqueRangeCount,
        .nativePointTransparentRangeCount =
            asset_.nativePointTransparentRangeCount,
        .nativeCurveOpaqueRangeCount = asset_.nativeCurveOpaqueRangeCount,
        .nativeCurveTransparentRangeCount =
            asset_.nativeCurveTransparentRangeCount,
    };
  }

  [[nodiscard]] const BimSceneAsset &asset() const { return asset_; }

private:
  SceneProviderId id_{};
  std::string displayName_{};
  BimSceneAsset asset_{};
  SceneProviderRevision revision_{};
};

class GaussianSplatSceneProvider final : public IRenderSceneProvider {
public:
  explicit GaussianSplatSceneProvider(
      SceneProviderId id, std::string displayName = "Gaussian splat scene")
      : id_(std::move(id)), displayName_(std::move(displayName)) {}

  void update(GaussianSplatSceneAsset asset, SceneProviderRevision revision,
              std::string displayName = {}) {
    asset_ = asset;
    revision_ = revision;
    if (!displayName.empty()) {
      displayName_ = std::move(displayName);
    }
  }

  [[nodiscard]] SceneProviderSnapshot snapshot() const override {
    return {
        .kind = SceneProviderKind::GaussianSplatting,
        .id = id_,
        .displayName = displayName_,
        .revision = revision_,
        .bounds = asset_.bounds,
        .elementCount = asset_.splatCount,
        .splatCount = asset_.splatCount,
        .sphericalHarmonicCoefficientCount =
            asset_.sphericalHarmonicCoefficientCount,
    };
  }

  [[nodiscard]] const GaussianSplatSceneAsset &asset() const { return asset_; }

private:
  SceneProviderId id_{};
  std::string displayName_{};
  GaussianSplatSceneAsset asset_{};
  SceneProviderRevision revision_{};
};

class RadianceFieldSceneProvider final : public IRenderSceneProvider {
public:
  explicit RadianceFieldSceneProvider(
      SceneProviderId id, std::string displayName = "Radiance field scene")
      : id_(std::move(id)), displayName_(std::move(displayName)) {}

  void update(RadianceFieldSceneAsset asset, SceneProviderRevision revision,
              std::string displayName = {}) {
    asset_ = asset;
    revision_ = revision;
    if (!displayName.empty()) {
      displayName_ = std::move(displayName);
    }
  }

  [[nodiscard]] SceneProviderSnapshot snapshot() const override {
    return {
        .kind = SceneProviderKind::RadianceField,
        .id = id_,
        .displayName = displayName_,
        .revision = revision_,
        .bounds = asset_.bounds,
        .elementCount = asset_.fieldCount,
        .fieldCount = asset_.fieldCount,
        .hasOccupancyData = asset_.hasOccupancyData,
    };
  }

  [[nodiscard]] const RadianceFieldSceneAsset &asset() const { return asset_; }

private:
  SceneProviderId id_{};
  std::string displayName_{};
  RadianceFieldSceneAsset asset_{};
  SceneProviderRevision revision_{};
};

class SceneProviderRegistry {
public:
  void registerProvider(const IRenderSceneProvider &provider) {
    const SceneProviderSnapshot providerSnapshot = provider.snapshot();
    if (providerSnapshot.id.value.empty()) {
      throw std::invalid_argument("scene provider id must not be empty");
    }
    if (find(providerSnapshot.id) != nullptr) {
      throw std::invalid_argument("scene provider is already registered");
    }
    providers_.push_back(&provider);
  }

  [[nodiscard]] const IRenderSceneProvider *
  find(const SceneProviderId &id) const {
    for (const IRenderSceneProvider *provider : providers_) {
      if (provider != nullptr && provider->snapshot().id == id) {
        return provider;
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::vector<const IRenderSceneProvider *>
  providersForKind(SceneProviderKind kind) const {
    std::vector<const IRenderSceneProvider *> matches;
    for (const IRenderSceneProvider *provider : providers_) {
      if (provider != nullptr && provider->snapshot().kind == kind) {
        matches.push_back(provider);
      }
    }
    return matches;
  }

  [[nodiscard]] std::vector<SceneProviderSnapshot> snapshots() const {
    std::vector<SceneProviderSnapshot> result;
    result.reserve(providers_.size());
    for (const IRenderSceneProvider *provider : providers_) {
      if (provider != nullptr) {
        result.push_back(provider->snapshot());
      }
    }
    return result;
  }

  [[nodiscard]] std::span<const IRenderSceneProvider *const> providers() const {
    return {providers_.data(), providers_.size()};
  }

  void clear() { providers_.clear(); }

private:
  std::vector<const IRenderSceneProvider *> providers_{};
};

} // namespace container::scene
