#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace container::gpu {

struct SceneAssetBytes {
  uint64_t residentBytes{};
  uint64_t uploadBytes{};

  bool operator==(const SceneAssetBytes&) const noexcept = default;
};

class SceneAssetBudgetExceeded : public std::length_error {
 public:
  using std::length_error::length_error;
};

// Render-thread accounting for resident scene assets and concurrent staging.
// A zero limit leaves the corresponding byte category uncapped.
class SceneAssetBudget {
 public:
  struct Snapshot {
    SceneAssetBytes limits{};
    SceneAssetBytes used{};
    SceneAssetBytes peak{};
    uint64_t rejectedReservations{};
  };

  class Reservation {
   public:
    Reservation() = default;
    ~Reservation() { reset(); }
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;

    Reservation(Reservation&& other) noexcept
        : state_(std::move(other.state_)),
          bytes_(std::exchange(other.bytes_, {})) {}

    Reservation& operator=(Reservation&& other) noexcept {
      if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        bytes_ = std::exchange(other.bytes_, {});
      }
      return *this;
    }

    // Call after completion of the GPU upload and retirement of its staging
    // storage. Keep the remaining reservation with the resident allocation.
    void releaseUpload() noexcept {
      if (state_) {
        state_->used.uploadBytes -= bytes_.uploadBytes;
        bytes_.uploadBytes = 0;
      }
    }

    void reset() noexcept {
      if (state_) {
        releaseUpload();
        state_->used.residentBytes -= bytes_.residentBytes;
        bytes_ = {};
        state_.reset();
      }
    }

   private:
    friend class SceneAssetBudget;
    Reservation(std::shared_ptr<Snapshot> state, SceneAssetBytes bytes) noexcept
        : state_(std::move(state)), bytes_(bytes) {}

    std::shared_ptr<Snapshot> state_;
    SceneAssetBytes bytes_;
  };

  explicit SceneAssetBudget(SceneAssetBytes limits = {})
      : state_(std::make_shared<Snapshot>(Snapshot{.limits = limits})) {}
  SceneAssetBudget(const SceneAssetBudget&) = delete;
  SceneAssetBudget& operator=(const SceneAssetBudget&) = delete;

  void reset(SceneAssetBytes limits = {}) {
    if (state_->used != SceneAssetBytes{})
      throw std::logic_error("Cannot reset the scene asset budget while bytes are reserved");
    *state_ = Snapshot{.limits = limits};
  }

  [[nodiscard]] Snapshot snapshot() const noexcept { return *state_; }

  // Test both limits before changing either charge or peak.
  void check(SceneAssetBytes request) {
    checkLimit("memory", request.residentBytes, state_->used.residentBytes,
               state_->limits.residentBytes);
    checkLimit("upload", request.uploadBytes, state_->used.uploadBytes,
               state_->limits.uploadBytes);
  }

  [[nodiscard]] Reservation reserve(SceneAssetBytes request) {
    check(request);
    auto& used = state_->used;
    auto& peak = state_->peak;
    used.residentBytes += request.residentBytes;
    used.uploadBytes += request.uploadBytes;
    peak.residentBytes = (std::max)(peak.residentBytes, used.residentBytes);
    peak.uploadBytes = (std::max)(peak.uploadBytes, used.uploadBytes);
    return Reservation(state_, request);
  }

 private:
  void checkLimit(const char* category, uint64_t request, uint64_t used, uint64_t limit) {
    constexpr auto maxBytes = (std::numeric_limits<uint64_t>::max)();
    const uint64_t available = (limit ? limit : maxBytes) - used;
    if (request <= available)
      return;
    if (state_->rejectedReservations != maxBytes)
      ++state_->rejectedReservations;
    throw SceneAssetBudgetExceeded("Scene asset " + std::string(category) +
        " budget exceeded (" + std::to_string(request) + " bytes requested, " +
        std::to_string(available) + " bytes available)");
  }

  std::shared_ptr<Snapshot> state_;
};

} // namespace container::gpu
