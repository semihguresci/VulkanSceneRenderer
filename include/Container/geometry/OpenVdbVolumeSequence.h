#pragma once

#include "Container/geometry/OpenVdbVolume.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace container::geometry {

struct OpenVdbAsyncSequenceImpl;

struct OpenVdbSequenceDiscoveryOptions {
  size_t maxFrames{10'000};
  // Optional literal filename prefix, e.g. smoke. Only regular .vdb files in
  // this directory are discovered; subdirectories are never traversed.
  std::string filenamePrefix{};
};

struct OpenVdbSequenceManifest {
  std::vector<std::filesystem::path> frames;
  double framesPerSecond{24.0};
  [[nodiscard]] double durationSeconds() const;
};

// Numeric filename runs sort as numbers without integer overflow, so 2 precedes
// 10. Discovery reads directory entries only, never volume payloads. An explicit
// positive finite FPS is required, and empty/over-budget sequences fail.
[[nodiscard]] OpenVdbSequenceManifest discoverOpenVdbVolumeSequence(
    const std::filesystem::path& directory, double framesPerSecond,
    const OpenVdbSequenceDiscoveryOptions& options = {});

struct OpenVdbSequencePlaybackOptions {
  bool loop{true};
  bool paused{false};
  double speed{1.0}; // Nonnegative finite multiplier in [0,1000].
};

// A CPU-only clock; advance() receives real elapsed seconds. Paused clocks and
// speed zero hold their position. Nonlooping playback holds the final frame.
// Seek is explicit and remains available while paused.
class OpenVdbSequencePlayback {
public:
  explicit OpenVdbSequencePlayback(
      const OpenVdbSequenceManifest& manifest,
      const OpenVdbSequencePlaybackOptions& options = {});
  void advance(double elapsedSeconds);
  void seekSeconds(double seconds);
  void setPaused(bool paused) noexcept { options_.paused = paused; }
  void setLoop(bool loop);
  void setSpeed(double speed);
  [[nodiscard]] size_t frameIndex() const noexcept;
  [[nodiscard]] double positionSeconds() const noexcept { return position_; }
  [[nodiscard]] double durationSeconds() const noexcept { return duration_; }
  [[nodiscard]] const OpenVdbSequencePlaybackOptions& options() const noexcept {
    return options_;
  }

private:
  friend struct OpenVdbAsyncSequenceImpl;
  size_t frameCount_{0};
  double framesPerSecond_{0.0}, duration_{0.0}, position_{0.0};
  OpenVdbSequencePlaybackOptions options_{};
};

struct OpenVdbSequenceCacheOptions {
  size_t maxCachedFrames{2}; // Bounded to [1,3].
  uint64_t maxCachedBytes{512ull * 1024ull * 1024ull};
};

struct OpenVdbSequenceBounds {
  std::array<float, 3> minimum{}, maximum{};
};

// On-demand bounded CPU LRU. frame() never silently reuses another frame on an
// error. Successful frames must retain the first loaded frame's selected grid
// names/channel presence, while transforms, bounds and dimensions may change.
// Returned references remain valid until a cache miss or clearCache(); callers
// must finish uploads before requesting another frame. This class is not shared
// across threads. Cache budget excludes the existing loader's source-grid
// budget and must fit one worst-case dense payload at the requested resolution,
// or volumeOptions.maxInputBytes for native sparse payloads. Native reservations
// are conservative; fitting multiple frames can require raising this budget.
class OpenVdbVolumeSequence {
public:
  explicit OpenVdbVolumeSequence(
      OpenVdbSequenceManifest manifest,
      const OpenVdbVolumeOptions& volumeOptions = {},
      const OpenVdbSequenceCacheOptions& cacheOptions = {});
  [[nodiscard]] const OpenVdbVolume& frame(size_t index);
  // Optional full sequence inspection, loading/evicting one frame at a time.
  // Suitable for stable camera framing. Each GPU frame keeps its own lattice;
  // this does not pad/resample every frame into a fixed union texture.
  [[nodiscard]] OpenVdbSequenceBounds scanUnionBounds();
  void clearCache() noexcept;
  [[nodiscard]] const OpenVdbSequenceManifest& manifest() const noexcept { return manifest_; }
  [[nodiscard]] size_t cachedFrameCount() const noexcept { return cache_.size(); }
  [[nodiscard]] uint64_t cachedBytes() const noexcept { return cachedBytes_; }
  [[nodiscard]] bool isCached(size_t index) const noexcept;

private:
  friend struct OpenVdbAsyncSequenceImpl;
  struct Entry { size_t index; OpenVdbVolume volume; uint64_t bytes; };
  void evictOldest() noexcept;
  OpenVdbSequenceManifest manifest_;
  OpenVdbVolumeOptions volumeOptions_;
  OpenVdbSequenceCacheOptions cacheOptions_;
  std::list<Entry> cache_;
  uint64_t cachedBytes_{0}, maximumFrameBytes_{0};
  bool haveSignature_{false};
  std::string densityGridName_, temperatureGridName_;
};

struct OpenVdbAsyncSequenceOptions {
  size_t maxCachedFrames{2}; // [1,3]; cached ready/prefetched payloads.
  size_t maxResidentFrames{3}; // [1,3], including caller-held frames/in-flight.
  uint64_t maxResidentBytes{0}; // Zero inherits the synchronous cache byte cap.
};

enum class OpenVdbAsyncSequenceState {
  Idle, Pending, Loading, WaitingForMemory, Ready, Failed, Stopped
};

struct OpenVdbAsyncSequenceFrame {
  size_t frameIndex{0};
  uint64_t requestId{0};
  std::shared_ptr<const OpenVdbVolume> volume{};
  std::string error{};
};

struct OpenVdbAsyncSequenceStatus {
  OpenVdbAsyncSequenceState state{OpenVdbAsyncSequenceState::Idle};
  bool hasRequest{false};
  size_t requestedFrame{0};
  uint64_t requestId{0};
  std::string error{};
  size_t cachedFrames{0}, residentFrames{0};
  uint64_t cachedBytes{0}, residentBytes{0};
  std::string prefetchError{};
};

// One CPU worker, one latest foreground request, and at most one speculative
// prefetch. Request/poll/status methods never load files or copy voxel arrays.
// Shared frame leases count against the resident budgets even after delivery;
// callers should release them after upload. Exhausted leases yield explicit
// WaitingForMemory until a lease is released. Superseded work never reports an
// old frame as the latest success. Errors are returned explicitly in completions.
// requestStop() wakes pending waits; destruction joins an ongoing OpenVDB load
// before releasing the worker. No detached thread can outlive this wrapper.
class OpenVdbAsyncVolumeSequence {
public:
  explicit OpenVdbAsyncVolumeSequence(
      std::unique_ptr<OpenVdbVolumeSequence> warmedSequence,
      const OpenVdbAsyncSequenceOptions& options = {});
  ~OpenVdbAsyncVolumeSequence();
  OpenVdbAsyncVolumeSequence(const OpenVdbAsyncVolumeSequence&) = delete;
  OpenVdbAsyncVolumeSequence& operator=(const OpenVdbAsyncVolumeSequence&) = delete;
  [[nodiscard]] uint64_t request(size_t frame);
  void prefetch(size_t frame);
  [[nodiscard]] std::optional<OpenVdbAsyncSequenceFrame> tryTakeFrame();
  [[nodiscard]] OpenVdbAsyncSequenceStatus status() const;
  [[nodiscard]] uint64_t cachedBytes() const;
  [[nodiscard]] const OpenVdbSequenceManifest& manifest() const noexcept;
  void requestStop() noexcept;

private:
  std::unique_ptr<OpenVdbAsyncSequenceImpl> impl_;
};

} // namespace container::geometry
