#include "Container/renderer/core/ComfyUiBridge.h"

#include "Container/utility/AtomicFile.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <random>
#include <string_view>
#include <utility>

namespace container::renderer {
namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;
constexpr size_t maxAssetBytes = 64 * 1024 * 1024;
constexpr size_t maxStatusBytes = 8 * 1024;
constexpr size_t maxPromptBytes = 2048;
constexpr uint32_t maxPreviewDimension = 1024;
constexpr auto jobTimeout = 20min;

bool filePresent(const std::filesystem::path& path) {
  return std::filesystem::exists(std::filesystem::symlink_status(path));
}

std::vector<std::byte> readBytes(const std::filesystem::path& path, size_t limit) {
  if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path)) ||
      std::filesystem::canonical(path) != path)
    throw std::runtime_error("Bridge file is not a contained regular file");
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  const auto size = input.tellg();
  if (size <= 0 || static_cast<uintmax_t>(size) > limit)
    throw std::runtime_error("Bridge file is empty or exceeds its size limit");
  std::vector<std::byte> result(static_cast<size_t>(size));
  input.seekg(0);
  if (!input.read(reinterpret_cast<char*>(result.data()), size) ||
      input.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("Bridge file changed while being read");
  return result;
}

std::string_view asText(const std::vector<std::byte>& bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void publish(const std::filesystem::path& path, const Json& value) {
  container::util::writeFileAtomically(path, value.dump() + '\n');
}

void shrinkPreview(container::material::TextureMipLevel& image) {
  const auto longest = std::max(image.width, image.height);
  if (longest <= maxPreviewDimension) return;
  const auto width = std::max(1u, image.width * maxPreviewDimension / longest);
  const auto height = std::max(1u, image.height * maxPreviewDimension / longest);
  std::vector<std::byte> pixels(size_t{width} * height * 4);
  for (uint32_t y = 0; y < height; ++y) {
    const auto sourceY = (2 * y + 1) * image.height / (2 * height);
    for (uint32_t x = 0; x < width; ++x) {
      const auto sourceX = (2 * x + 1) * image.width / (2 * width);
      std::copy_n(image.bytes.data() + 4 * (sourceY * image.width + sourceX), 4,
                  pixels.data() + 4 * (y * width + x));
    }
  }
  image = {width, height, std::move(pixels)};
}

ComfyUiResult readResult(const std::filesystem::path& folder, const std::string& id,
                         ComfyUiJobKind kind, const std::string& filename) {
  const bool restyle = kind == ComfyUiJobKind::Restyle;
  if ((restyle && filename != "generated.png") ||
      (!restyle && filename != "generated.hdr" && filename != "generated.exr"))
    throw std::runtime_error("The adapter returned an unexpected output filename");
  auto path = folder / filename;
  auto encoded = std::make_shared<const std::vector<std::byte>>(readBytes(path, maxAssetBytes));
  const auto bytes = asText(*encoded);
  if (!restyle) {
    const bool recognizable = filename == "generated.exr" ? bytes.starts_with("\x76\x2f\x31\x01") :
        bytes.starts_with("#?RADIANCE\n") || bytes.starts_with("#?RGBE\n");
    if (!recognizable) throw std::runtime_error("The generated environment has an invalid HDR or EXR signature");
    return {id, kind, std::move(path), std::move(encoded), {}};
  }
  if (!bytes.starts_with("\x89PNG\r\n\x1a\n"))
    throw std::runtime_error("The generated preview is not a PNG image");
  auto preview = container::material::DecodeTextureImage(*encoded,
      {.isSrgb = true, .maxEncodedBytes = maxAssetBytes, .maxDecodedBytes = maxAssetBytes,
       .maxDimension = 4096, .maxMipLevels = 1, .allowedFormats = {VK_FORMAT_R8G8B8A8_SRGB}});
  shrinkPreview(preview.mips.front());
  return {id, kind, std::move(path), std::move(encoded), std::move(preview)};
}

}  // namespace

bool ComfyUiJobStatus::busy() const {
  return phase >= ComfyUiJobPhase::Preparing && phase <= ComfyUiJobPhase::Running;
}

ComfyUiBridge::ComfyUiBridge(std::filesystem::path root)
    : root_(std::move(root)), worker_([this](std::stop_token stop) { run(stop); }) {}

ComfyUiBridge::~ComfyUiBridge() = default;

bool ComfyUiBridge::submit(ComfyUiJobKind kind, std::string prompt, uint64_t seed) {
  if (prompt.size() > maxPromptBytes || prompt.find_first_not_of(" \t\r\n") == std::string::npos)
    return false;
  const std::lock_guard lock(mutex_);
  if (status_.busy()) return false;
  const auto time = std::chrono::system_clock::now().time_since_epoch().count();
  const auto entropy = std::random_device{}();
  auto id = std::format("{:016x}{:08x}{:08x}", static_cast<uint64_t>(time), entropy,
                        static_cast<uint32_t>(++serial_));
  status_ = {id, ComfyUiJobPhase::Preparing, "Preparing generation request", 0.f};
  request_ = Request{std::move(id), kind, std::move(prompt), seed};
  capture_.reset();
  captureError_.reset();
  result_.reset();
  wake_.notify_all();
  return true;
}

void ComfyUiBridge::cancel() {
  const std::lock_guard lock(mutex_);
  if (!status_.busy()) return;
  status_.phase = ComfyUiJobPhase::Cancelled;
  status_.message = "Generation cancelled";
  capture_.reset();
  wake_.notify_all();
}

ComfyUiJobStatus ComfyUiBridge::status() const {
  const std::lock_guard lock(mutex_);
  return status_;
}

std::optional<ComfyUiCaptureRequest> ComfyUiBridge::takeCaptureRequest() {
  const std::lock_guard lock(mutex_);
  return std::exchange(capture_, {});
}

void ComfyUiBridge::completeCapture(const std::string& id, std::string error) {
  const std::lock_guard lock(mutex_);
  if (isCurrentLocked(id) && status_.phase == ComfyUiJobPhase::Capture) {
    captureError_ = std::move(error);
    wake_.notify_all();
  }
}

std::optional<ComfyUiResult> ComfyUiBridge::takeResult() {
  const std::lock_guard lock(mutex_);
  return std::exchange(result_, {});
}

bool ComfyUiBridge::isCurrentLocked(const std::string& id) const {
  return status_.id == id && status_.busy();
}

void ComfyUiBridge::update(const std::string& id, ComfyUiJobPhase phase, std::string message, float progress) {
  const std::lock_guard lock(mutex_);
  if (isCurrentLocked(id)) status_ = {id, phase, std::move(message), progress};
}

void ComfyUiBridge::run(std::stop_token stop) {
  while (!stop.stop_requested()) {
    std::unique_lock lock(mutex_);
    if (!wake_.wait(lock, stop, [&] { return request_.has_value(); })) return;
    auto request = std::move(*request_);
    request_.reset();
    if (!isCurrentLocked(request.id)) continue;
    lock.unlock();
    try {
      process(stop, request);
    } catch (const std::exception& error) {
      update(request.id, ComfyUiJobPhase::Failed, error.what(), 0.f);
    }
  }
}

void ComfyUiBridge::process(std::stop_token stop, const Request& request) {
  const auto root = std::filesystem::canonical(root_);
  const auto jobs = root / "jobs";
  std::filesystem::create_directory(jobs);
  if (!std::filesystem::is_directory(jobs) || std::filesystem::canonical(jobs) != jobs)
    throw std::runtime_error("Bridge jobs must be inside the configured directory");
  const auto folder = jobs / request.id;
  if (!std::filesystem::create_directory(folder))
    throw std::runtime_error("Could not reserve a unique ComfyUI job directory");

  const auto cancelPublished = [&] {
    try { publish(folder / "cancel.json", {{"version", 1}, {"id", request.id}}); }
    catch (const std::exception&) { /* The request also carries an expiry time. */ }
  };
  const bool restyle = request.kind == ComfyUiJobKind::Restyle;
  if (restyle) {
    std::unique_lock lock(mutex_);
    if (!isCurrentLocked(request.id)) return;
    status_ = {request.id, ComfyUiJobPhase::Capture, "Capturing the current viewport", 0.05f};
    capture_ = ComfyUiCaptureRequest{request.id, folder / "input.png"};
    const bool received = wake_.wait_for(lock, stop, 20s, [&] {
      return captureError_.has_value() || !isCurrentLocked(request.id);
    });
    if (stop.stop_requested() || !isCurrentLocked(request.id)) return;
    if (!received) {
      capture_.reset();
      throw std::runtime_error("Timed out waiting for the viewport capture");
    }
    if (!captureError_->empty()) throw std::runtime_error(*captureError_);
  }

  const auto expires = std::chrono::duration_cast<std::chrono::seconds>(
      (std::chrono::system_clock::now() + jobTimeout).time_since_epoch()).count();
  publish(folder / "request.json", {{"version", 1}, {"id", request.id},
      {"kind", restyle ? "restyle" : "hdri"}, {"prompt", request.prompt},
      {"seed", request.seed}, {"expiresAtUnix", expires}});
  update(request.id, ComfyUiJobPhase::Queued, "Waiting for the local adapter", 0.1f);
  const auto queued = std::chrono::steady_clock::now();
  bool acknowledged = false;
  const auto statusPath = folder / "status.json";
  try {
    for (;;) {
      std::unique_lock lock(mutex_);
      wake_.wait_for(lock, stop, 250ms, [&] { return !isCurrentLocked(request.id); });
      if (stop.stop_requested() || !isCurrentLocked(request.id)) {
        lock.unlock();
        cancelPublished();
        return;
      }
      lock.unlock();
      const auto elapsed = std::chrono::steady_clock::now() - queued;
      if (!acknowledged && elapsed >= 15s)
        throw std::runtime_error("ComfyUI adapter did not acknowledge the request");
      if (elapsed >= jobTimeout) throw std::runtime_error("ComfyUI job timed out");
      if (!filePresent(statusPath)) continue;
      const auto record = Json::parse(asText(readBytes(statusPath, maxStatusBytes)));
      if (record.at("version") != 1 || record.at("id") != request.id) continue;
      acknowledged = true;
      auto phase = record.at("phase").get<std::string>();
      auto message = record.value("message", std::string{});
      message.resize(std::min<size_t>(message.size(), 1024));
      const auto progress = record.value("progress", 0.f);
      const float fraction = std::isfinite(progress) ? std::clamp(progress, 0.f, 1.f) : 0.f;
      if (phase == "queued" || phase == "running") {
        update(request.id, phase == "queued" ? ComfyUiJobPhase::Queued : ComfyUiJobPhase::Running,
               std::move(message), fraction);
      } else if (phase == "succeeded") {
        auto result = readResult(folder, request.id, request.kind, record.at("output").get<std::string>());
        lock.lock();
        if (isCurrentLocked(request.id) && !stop.stop_requested()) {
          result_ = std::move(result);
          status_ = {request.id, ComfyUiJobPhase::Ready, "Generated result is ready", 1.f};
        }
        return;
      } else if (phase == "failed" || phase == "cancelled") {
        update(request.id, phase == "failed" ? ComfyUiJobPhase::Failed : ComfyUiJobPhase::Cancelled,
               std::move(message), 0.f);
        return;
      } else {
        throw std::runtime_error("Unrecognized ComfyUI adapter job state");
      }
    }
  } catch (...) {
    cancelPublished();
    throw;
  }
}

}  // namespace container::renderer
