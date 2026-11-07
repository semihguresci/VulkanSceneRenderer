#include "Container/geometry/OpenVdbVolumeSequence.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace container::geometry {
namespace {

std::string utf8(const std::filesystem::path& path) {
  const auto value = path.u8string();
  return {value.begin(), value.end()};
}

char lowerAscii(char value) {
  return value >= 'A' && value <= 'Z' ? char(value - 'A' + 'a') : value;
}

bool digit(char value) { return value >= '0' && value <= '9'; }

bool naturalLess(std::string_view a, std::string_view b) {
  size_t left = 0, right = 0;
  while (left < a.size() && right < b.size()) {
    if (digit(a[left]) && digit(b[right])) {
      size_t leftEnd = left, rightEnd = right;
      while (leftEnd < a.size() && digit(a[leftEnd])) ++leftEnd;
      while (rightEnd < b.size() && digit(b[rightEnd])) ++rightEnd;
      size_t leftNumber = left, rightNumber = right;
      while (leftNumber < leftEnd && a[leftNumber] == '0') ++leftNumber;
      while (rightNumber < rightEnd && b[rightNumber] == '0') ++rightNumber;
      const size_t leftDigits = leftEnd - leftNumber;
      const size_t rightDigits = rightEnd - rightNumber;
      if (leftDigits != rightDigits) return leftDigits < rightDigits;
      const int comparison = a.substr(leftNumber, leftDigits).compare(
          b.substr(rightNumber, rightDigits));
      if (comparison != 0) return comparison < 0;
      if (leftEnd - left != rightEnd - right)
        return leftEnd - left < rightEnd - right;
      left = leftEnd;
      right = rightEnd;
    } else {
      const auto first = lowerAscii(a[left]), second = lowerAscii(b[right]);
      if (first != second) return static_cast<unsigned char>(first) < static_cast<unsigned char>(second);
      ++left;
      ++right;
    }
  }
  if (left != a.size() || right != b.size()) return left == a.size();
  return a < b;
}

void validateSpeed(double speed) {
  if (!std::isfinite(speed) || speed < 0.0 || speed > 1000.0)
    throw std::invalid_argument("OpenVDB sequence speed must be finite and in [0,1000]");
}

void validateVolumeOptions(const OpenVdbVolumeOptions& options) {
  if (options.maxResolution < 16 || options.maxResolution > 256 ||
      !std::isfinite(options.importScale) || options.importScale <= 0.0f ||
      !options.maxActiveVoxels || !options.maxInputBytes)
    throw std::invalid_argument("Invalid OpenVDB sequence volume resolution, scale or source budgets");
}

} // namespace

double OpenVdbSequenceManifest::durationSeconds() const {
  if (frames.empty() || !std::isfinite(framesPerSecond) || framesPerSecond <= 0.0)
    throw std::invalid_argument("OpenVDB sequence requires frames and a positive finite FPS");
  const double duration = double(frames.size()) / framesPerSecond;
  if (!std::isfinite(duration) || duration <= 0.0)
    throw std::invalid_argument("OpenVDB sequence duration is outside the finite clock range");
  return duration;
}

OpenVdbSequenceManifest discoverOpenVdbVolumeSequence(
    const std::filesystem::path& directory, double framesPerSecond,
    const OpenVdbSequenceDiscoveryOptions& options) {
  if (!std::isfinite(framesPerSecond) || framesPerSecond <= 0.0 || !options.maxFrames)
    throw std::invalid_argument("OpenVDB sequence FPS and frame budget must be positive and finite");
  if (!std::filesystem::is_directory(directory))
    throw std::invalid_argument("OpenVDB sequence input is not a directory: " + utf8(directory));
  OpenVdbSequenceManifest manifest;
  manifest.framesPerSecond = framesPerSecond;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (!entry.is_regular_file()) continue;
    const auto filename = utf8(entry.path().filename());
    if (!filename.starts_with(options.filenamePrefix)) continue;
    auto extension = utf8(entry.path().extension());
    std::transform(extension.begin(), extension.end(), extension.begin(), lowerAscii);
    if (extension != ".vdb") continue;
    if (manifest.frames.size() >= options.maxFrames)
      throw std::invalid_argument("OpenVDB sequence exceeds the frame-count budget");
    manifest.frames.push_back(std::filesystem::absolute(entry.path()).lexically_normal());
  }
  std::sort(manifest.frames.begin(), manifest.frames.end(), [](const auto& a, const auto& b) {
    return naturalLess(utf8(a.filename()), utf8(b.filename()));
  });
  (void)manifest.durationSeconds();
  return manifest;
}

OpenVdbSequencePlayback::OpenVdbSequencePlayback(
    const OpenVdbSequenceManifest& manifest,
    const OpenVdbSequencePlaybackOptions& options)
    : frameCount_(manifest.frames.size()), framesPerSecond_(manifest.framesPerSecond),
      duration_(manifest.durationSeconds()), options_(options) {
  validateSpeed(options_.speed);
}

void OpenVdbSequencePlayback::advance(double elapsedSeconds) {
  if (!std::isfinite(elapsedSeconds) || elapsedSeconds < 0.0)
    throw std::invalid_argument("OpenVDB sequence elapsed time must be finite and nonnegative");
  if (options_.paused || options_.speed == 0.0 || elapsedSeconds == 0.0) return;
  if (!options_.loop) {
    const double remaining = duration_ - position_;
    position_ = elapsedSeconds >= remaining / options_.speed
        ? duration_ : position_ + elapsedSeconds * options_.speed;
    return;
  }
  // Reduce before multiplication so even a very large finite time delta cannot
  // overflow. Avoid adding two values near DBL_MAX when the period is huge.
  const double increment = std::fmod(elapsedSeconds, duration_ / options_.speed) * options_.speed;
  const double remaining = duration_ - position_;
  position_ = increment >= remaining ? increment - remaining : position_ + increment;
  if (position_ >= duration_) position_ = 0.0;
}

void OpenVdbSequencePlayback::seekSeconds(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0.0)
    throw std::invalid_argument("OpenVDB sequence seek time must be finite and nonnegative");
  position_ = options_.loop ? std::fmod(seconds, duration_) : std::min(seconds, duration_);
}

void OpenVdbSequencePlayback::setLoop(bool loop) {
  options_.loop = loop;
  if (loop && position_ >= duration_) position_ = 0.0;
}

void OpenVdbSequencePlayback::setSpeed(double speed) {
  validateSpeed(speed);
  options_.speed = speed;
}

size_t OpenVdbSequencePlayback::frameIndex() const noexcept {
  if (position_ >= duration_) return frameCount_ - 1;
  const double index = std::floor(std::nextafter(position_ * framesPerSecond_,
                                               std::numeric_limits<double>::infinity()));
  return std::min(static_cast<size_t>(std::max(index, 0.0)), frameCount_ - 1);
}

OpenVdbVolumeSequence::OpenVdbVolumeSequence(
    OpenVdbSequenceManifest manifest, const OpenVdbVolumeOptions& volumeOptions,
    const OpenVdbSequenceCacheOptions& cacheOptions)
    : manifest_(std::move(manifest)), volumeOptions_(volumeOptions), cacheOptions_(cacheOptions) {
  (void)manifest_.durationSeconds();
  validateVolumeOptions(volumeOptions_);
  if (!cacheOptions_.maxCachedFrames || cacheOptions_.maxCachedFrames > 3 || !cacheOptions_.maxCachedBytes)
    throw std::invalid_argument("OpenVDB sequence cache requires 1 to 3 frames and a positive byte budget");
  const uint64_t resolution = volumeOptions_.maxResolution;
  maximumFrameBytes_ = volumeOptions_.sparse ? volumeOptions_.maxInputBytes :
      resolution * resolution * resolution * sizeof(float) *
          (volumeOptions_.temperatureGridName.empty() ? 1 : 2);
  if (maximumFrameBytes_ > cacheOptions_.maxCachedBytes)
    throw std::invalid_argument("OpenVDB sequence cache budget cannot fit one frame at the requested resolution/channels");
}

void OpenVdbVolumeSequence::evictOldest() noexcept {
  cachedBytes_ -= cache_.back().bytes;
  cache_.pop_back();
}

bool OpenVdbVolumeSequence::isCached(size_t index) const noexcept {
  return std::any_of(cache_.begin(), cache_.end(), [index](const auto& entry) { return entry.index == index; });
}

const OpenVdbVolume& OpenVdbVolumeSequence::frame(size_t index) {
  if (index >= manifest_.frames.size())
    throw std::out_of_range("OpenVDB sequence frame index is out of range");
  for (auto entry = cache_.begin(); entry != cache_.end(); ++entry)
    if (entry->index == index) {
      cache_.splice(cache_.begin(), cache_, entry);
      return cache_.front().volume;
    }
  // Make room before loading; a miss never temporarily retains a fourth dense
  // payload or exceeds the configured preview-cache byte budget.
  while (!cache_.empty() &&
         (cache_.size() >= cacheOptions_.maxCachedFrames ||
          maximumFrameBytes_ > cacheOptions_.maxCachedBytes - cachedBytes_))
    evictOldest();
  try {
    auto volume = loadOpenVdbVolume(manifest_.frames[index], volumeOptions_);
    if (haveSignature_ && (volume.gridName != densityGridName_ ||
                           volume.temperatureGridName != temperatureGridName_))
      throw std::runtime_error("selected density/temperature grid names differ from the sequence's first loaded frame");
    const uint64_t bytes = uint64_t(volume.density.capacity()) * sizeof(float) +
                           uint64_t(volume.temperature.capacity()) * sizeof(float) +
                           uint64_t(volume.sparseDensity.capacity()) * sizeof(uint32_t) +
                           uint64_t(volume.sparseTemperature.capacity()) * sizeof(uint32_t);
    if (bytes > maximumFrameBytes_ || bytes > cacheOptions_.maxCachedBytes - cachedBytes_)
      throw std::runtime_error("decoded frame exceeds the sequence preview-cache byte budget");
    if (!haveSignature_) {
      densityGridName_ = volume.gridName;
      temperatureGridName_ = volume.temperatureGridName;
      haveSignature_ = true;
    }
    cache_.push_front({index, std::move(volume), bytes});
    cachedBytes_ += bytes;
    return cache_.front().volume;
  } catch (const std::exception& error) {
    throw std::runtime_error("OpenVDB sequence frame " + std::to_string(index) + " ('" +
                             utf8(manifest_.frames[index]) + "'): " + error.what());
  }
}

OpenVdbSequenceBounds OpenVdbVolumeSequence::scanUnionBounds() {
  OpenVdbSequenceBounds bounds;
  bounds.minimum.fill(std::numeric_limits<float>::max());
  bounds.maximum.fill(std::numeric_limits<float>::lowest());
  for (size_t index = 0; index < manifest_.frames.size(); ++index) {
    const auto& volume = frame(index);
    for (size_t axis = 0; axis < 3; ++axis) {
      bounds.minimum[axis] = std::min(bounds.minimum[axis], volume.boundsMin[axis]);
      bounds.maximum[axis] = std::max(bounds.maximum[axis], volume.boundsMax[axis]);
      if (!std::isfinite(bounds.maximum[axis] - bounds.minimum[axis]))
        throw std::runtime_error("OpenVDB sequence union bounds exceed the finite float range");
    }
  }
  return bounds;
}

void OpenVdbVolumeSequence::clearCache() noexcept {
  cache_.clear();
  cachedBytes_ = 0;
}

namespace {
struct AsyncBudget {
  std::mutex mutex;
  std::condition_variable_any changed;
  size_t frames{0}, maximumFrames{0};
  uint64_t bytes{0}, maximumBytes{0};
};

struct AsyncFrameStorage {
  OpenVdbVolume volume;
  std::shared_ptr<AsyncBudget> budget;
  uint64_t bytes{0};
  bool accounted{false};
  ~AsyncFrameStorage() {
    // Free the arrays before advertising capacity to the waiting worker.
    volume = {};
    if (accounted) {
      { std::lock_guard lock(budget->mutex); --budget->frames; budget->bytes -= bytes; }
      budget->changed.notify_all();
    }
  }
};

struct AsyncReservation {
  std::shared_ptr<AsyncBudget> budget;
  uint64_t bytes{0};
  bool active{false};
  ~AsyncReservation() {
    if (active) {
      { std::lock_guard lock(budget->mutex); --budget->frames; budget->bytes -= bytes; }
      budget->changed.notify_all();
    }
  }
};

uint64_t payloadBytes(const OpenVdbVolume& volume) {
  return uint64_t(volume.density.capacity()) * sizeof(float) +
         uint64_t(volume.temperature.capacity()) * sizeof(float) +
         uint64_t(volume.sparseDensity.capacity()) * sizeof(uint32_t) +
         uint64_t(volume.sparseTemperature.capacity()) * sizeof(uint32_t);
}

std::shared_ptr<const OpenVdbVolume> trackedVolume(OpenVdbVolume volume,
                                                AsyncReservation& reservation) {
  const uint64_t bytes = payloadBytes(volume);
  if (bytes > reservation.bytes)
    throw std::runtime_error("OpenVDB async frame exceeds its reserved payload budget");
  auto storage = std::make_shared<AsyncFrameStorage>();
  storage->volume = std::move(volume);
  storage->budget = reservation.budget;
  storage->bytes = bytes;
  {
    std::lock_guard lock(reservation.budget->mutex);
    reservation.budget->bytes -= reservation.bytes - bytes;
    storage->accounted = true;
    reservation.active = false;
  }
  reservation.budget->changed.notify_all();
  const auto* pointer = &storage->volume;
  return std::shared_ptr<const OpenVdbVolume>(std::move(storage), pointer);
}
} // namespace

struct OpenVdbAsyncSequenceImpl {
  struct Entry { size_t index; std::shared_ptr<const OpenVdbVolume> volume; };
  std::unique_ptr<OpenVdbVolumeSequence> sequence;
  OpenVdbAsyncSequenceOptions options;
  std::shared_ptr<AsyncBudget> budget{std::make_shared<AsyncBudget>()};
  mutable std::mutex mutex;
  std::condition_variable_any workChanged;
  std::atomic<uint64_t> revision{0};
  uint64_t servedRevision{0};
  OpenVdbAsyncSequenceStatus snapshot;
  std::optional<OpenVdbAsyncSequenceFrame> ready;
  std::optional<size_t> queuedPrefetch, activePrefetch;
  std::optional<std::pair<size_t, std::string>> failedPrefetch;
  bool terminated{false};
  std::set<size_t> cachedIndices;
  std::list<Entry> cache; // Worker-owned after construction.
  std::jthread worker;

  explicit OpenVdbAsyncSequenceImpl(std::unique_ptr<OpenVdbVolumeSequence> warmed,
                                   const OpenVdbAsyncSequenceOptions& configuration)
      : sequence(std::move(warmed)), options(configuration) {
    if (!sequence) throw std::invalid_argument("OpenVDB async sequence must own a sequence");
    if (!options.maxCachedFrames || options.maxCachedFrames > 3 ||
        !options.maxResidentFrames || options.maxResidentFrames > 3 ||
        options.maxCachedFrames > options.maxResidentFrames)
      throw std::invalid_argument("OpenVDB async cache/resident frame counts must be in [1,3], cache <= resident");
    budget->maximumFrames = options.maxResidentFrames;
    budget->maximumBytes = options.maxResidentBytes ? options.maxResidentBytes : sequence->cacheOptions_.maxCachedBytes;
    if (budget->maximumBytes < sequence->maximumFrameBytes_)
      throw std::invalid_argument("OpenVDB async resident byte budget must fit one worst-case frame");
    while (!sequence->cache_.empty() &&
           (sequence->cache_.size() > options.maxCachedFrames ||
            sequence->cache_.size() > options.maxResidentFrames ||
            sequence->cachedBytes_ > budget->maximumBytes))
      sequence->evictOldest();
    // Transfer warmed arrays into tracked leases. No voxel array is copied.
    for (auto& entry : sequence->cache_) {
      AsyncReservation reservation{budget, entry.bytes, true};
      { std::lock_guard lock(budget->mutex); ++budget->frames; budget->bytes += entry.bytes; }
      cache.push_back({entry.index, trackedVolume(std::move(entry.volume), reservation)});
    }
    sequence->clearCache();
    publishCache();
    worker = std::jthread([this](std::stop_token stop) { run(stop); });
  }

  ~OpenVdbAsyncSequenceImpl() {
    stop();
    if (worker.joinable()) worker.join();
    ready.reset();
    cache.clear();
  }

  void stop() noexcept {
    worker.request_stop();
    workChanged.notify_all();
    budget->changed.notify_all();
  }

  void publishCache() {
    std::lock_guard lock(mutex);
    cachedIndices.clear();
    snapshot.cachedBytes = 0;
    for (const auto& entry : cache) {
      cachedIndices.insert(entry.index);
      snapshot.cachedBytes += payloadBytes(*entry.volume);
    }
    snapshot.cachedFrames = cache.size();
  }

  std::shared_ptr<const OpenVdbVolume> cached(size_t index) {
    for (auto entry = cache.begin(); entry != cache.end(); ++entry)
      if (entry->index == index) {
        cache.splice(cache.begin(), cache, entry);
        return cache.front().volume;
      }
    return {};
  }

  bool reserve(uint64_t id, bool prefetch, std::stop_token stop, AsyncReservation& reservation) {
    auto fits = [&] {
      return budget->frames < budget->maximumFrames &&
             sequence->maximumFrameBytes_ <= budget->maximumBytes - budget->bytes;
    };
    for (;;) {
      if (stop.stop_requested() || revision.load() != id) return false;
      {
        std::lock_guard lock(budget->mutex);
        if (fits()) {
          ++budget->frames;
          budget->bytes += sequence->maximumFrameBytes_;
          reservation.budget = budget;
          reservation.bytes = sequence->maximumFrameBytes_;
          reservation.active = true;
          return true;
        }
      }
      if (!cache.empty()) {
        cache.pop_back();
        publishCache();
        continue;
      }
      if (!prefetch) {
        std::lock_guard lock(mutex);
        if (revision.load() == id) snapshot.state = OpenVdbAsyncSequenceState::WaitingForMemory;
      }
      std::unique_lock lock(budget->mutex);
      budget->changed.wait(lock, stop, [&] { return revision.load() != id || fits(); });
    }
  }

  OpenVdbVolume extract(size_t index) {
    (void)sequence->frame(index);
    auto entry = sequence->cache_.begin();
    OpenVdbVolume volume = std::move(entry->volume);
    sequence->cachedBytes_ -= entry->bytes;
    sequence->cache_.erase(entry);
    return volume;
  }

  void deliver(size_t index, uint64_t id, std::shared_ptr<const OpenVdbVolume> volume,
               std::string error, std::stop_token stop) {
    std::lock_guard lock(mutex);
    if (stop.stop_requested() || revision.load() != id) return;
    ready = OpenVdbAsyncSequenceFrame{index, id, std::move(volume), error};
    servedRevision = id;
    snapshot.state = error.empty() ? OpenVdbAsyncSequenceState::Ready : OpenVdbAsyncSequenceState::Failed;
    snapshot.error = std::move(error);
  }

  void run(std::stop_token stop) noexcept {
    try {
      while (!stop.stop_requested()) {
        size_t index = 0;
        uint64_t id = 0;
        bool prefetch = false;
        std::string previousError;
        {
          std::unique_lock lock(mutex);
          workChanged.wait(lock, stop, [&] { return revision.load() != servedRevision || queuedPrefetch.has_value(); });
          if (stop.stop_requested()) break;
          id = revision.load();
          prefetch = id == servedRevision;
          if (prefetch) {
            index = *queuedPrefetch;
            queuedPrefetch.reset();
            activePrefetch = index;
          } else {
            index = snapshot.requestedFrame;
            snapshot.state = OpenVdbAsyncSequenceState::Loading;
            if (failedPrefetch && failedPrefetch->first == index) {
              previousError = std::move(failedPrefetch->second);
              failedPrefetch.reset();
              snapshot.prefetchError.clear();
            }
          }
        }
        if (!previousError.empty()) { deliver(index, id, {}, std::move(previousError), stop); continue; }
        std::shared_ptr<const OpenVdbVolume> volume = cached(index);
        std::string error;
        if (!volume) {
          AsyncReservation reservation;
          if (!reserve(id, prefetch, stop, reservation)) {
            if (prefetch) { std::lock_guard lock(mutex); activePrefetch.reset(); }
            continue;
          }
          if (!prefetch) {
            std::lock_guard lock(mutex);
            if (revision.load() == id) snapshot.state = OpenVdbAsyncSequenceState::Loading;
          }
          try {
            volume = trackedVolume(extract(index), reservation);
          } catch (const std::exception& failure) {
            error = failure.what();
          }
          if (volume) {
            cache.push_front({index, volume});
            while (cache.size() > options.maxCachedFrames) cache.pop_back();
          }
        }
        publishCache();
        if (prefetch) {
          std::lock_guard lock(mutex);
          activePrefetch.reset();
          if (!error.empty()) {
            failedPrefetch = std::pair(index, error);
            snapshot.prefetchError = std::move(error);
          }
        } else {
          deliver(index, id, std::move(volume), std::move(error), stop);
        }
      }
    } catch (const std::exception& error) {
      std::lock_guard lock(mutex);
      terminated = true;
      const auto id = revision.load();
      snapshot.state = OpenVdbAsyncSequenceState::Failed;
      snapshot.error = std::string("OpenVDB async worker failed: ") + error.what();
      if (snapshot.hasRequest) {
        servedRevision = id;
        ready = OpenVdbAsyncSequenceFrame{snapshot.requestedFrame, id, {}, snapshot.error};
      }
    }
    if (stop.stop_requested()) {
      std::lock_guard lock(mutex);
      snapshot.state = OpenVdbAsyncSequenceState::Stopped;
    }
    { std::lock_guard lock(mutex); terminated = true; }
  }
};

OpenVdbAsyncVolumeSequence::OpenVdbAsyncVolumeSequence(
    std::unique_ptr<OpenVdbVolumeSequence> warmedSequence,
    const OpenVdbAsyncSequenceOptions& options)
    : impl_(std::make_unique<OpenVdbAsyncSequenceImpl>(std::move(warmedSequence), options)) {}

OpenVdbAsyncVolumeSequence::~OpenVdbAsyncVolumeSequence() = default;

uint64_t OpenVdbAsyncVolumeSequence::request(size_t frame) {
  if (frame >= impl_->sequence->manifest().frames.size())
    throw std::out_of_range("OpenVDB async requested frame index is out of range");
  uint64_t requestedId = 0;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->worker.get_stop_token().stop_requested())
      throw std::logic_error("OpenVDB async worker has stopped");
    if (impl_->terminated)
      throw std::runtime_error(impl_->snapshot.error.empty() ? "OpenVDB async worker has terminated" : impl_->snapshot.error);
    if (impl_->snapshot.hasRequest && impl_->snapshot.requestedFrame == frame &&
        (impl_->revision.load() != impl_->servedRevision || impl_->ready))
      return impl_->revision.load();
    impl_->ready.reset();
    // Budget wait predicates also observe revision. Publish it under that
    // wait's mutex so superseding a memory-blocked request cannot lose a wake.
    uint64_t id = 0;
    { std::lock_guard budgetLock(impl_->budget->mutex);
      id = impl_->revision.fetch_add(1) + 1; }
    requestedId = id;
    impl_->snapshot.hasRequest = true;
    impl_->snapshot.requestedFrame = frame;
    impl_->snapshot.requestId = id;
    impl_->snapshot.state = OpenVdbAsyncSequenceState::Pending;
    impl_->snapshot.error.clear();
  }
  impl_->workChanged.notify_all();
  impl_->budget->changed.notify_all();
  return requestedId;
}

void OpenVdbAsyncVolumeSequence::prefetch(size_t frame) {
  if (frame >= impl_->sequence->manifest().frames.size())
    throw std::out_of_range("OpenVDB async prefetch frame index is out of range");
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->worker.get_stop_token().stop_requested() || impl_->terminated) return;
    if (impl_->cachedIndices.contains(frame) || impl_->queuedPrefetch == frame ||
        impl_->activePrefetch == frame ||
        (impl_->failedPrefetch && impl_->failedPrefetch->first == frame)) return;
    impl_->queuedPrefetch = frame;
  }
  impl_->workChanged.notify_all();
}

std::optional<OpenVdbAsyncSequenceFrame> OpenVdbAsyncVolumeSequence::tryTakeFrame() {
  std::lock_guard lock(impl_->mutex);
  auto result = std::move(impl_->ready);
  impl_->ready.reset();
  return result;
}

OpenVdbAsyncSequenceStatus OpenVdbAsyncVolumeSequence::status() const {
  std::lock_guard lock(impl_->mutex);
  auto result = impl_->snapshot;
  { std::lock_guard budgetLock(impl_->budget->mutex);
    result.residentFrames = impl_->budget->frames;
    result.residentBytes = impl_->budget->bytes; }
  return result;
}

uint64_t OpenVdbAsyncVolumeSequence::cachedBytes() const { return status().cachedBytes; }

const OpenVdbSequenceManifest& OpenVdbAsyncVolumeSequence::manifest() const noexcept {
  return impl_->sequence->manifest();
}

void OpenVdbAsyncVolumeSequence::requestStop() noexcept { impl_->stop(); }

} // namespace container::geometry
