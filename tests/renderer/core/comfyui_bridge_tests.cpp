#include "Container/renderer/core/ComfyUiBridge.h"
#include "Container/utility/AtomicFile.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <limits>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace container::renderer;
using Json = nlohmann::json;

constexpr char image[] =
    "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52"
    "\x00\x00\x00\x02\x00\x00\x00\x01\x08\x06\x00\x00\x00\xf4\x22\x7f\x8a"
    "\x00\x00\x00\x11\x49\x44\x41\x54\x78\x9c\x63\x6c\x70\x50\xf8\xcf\xc0"
    "\xc0\xc0\x00\x00\x0b\x8d\x01\xe1\xd9\x3e\xac\x06\x00\x00\x00\x00\x49"
    "\x45\x4e\x44\xae\x42\x60\x82";

template<class Predicate>
bool waitFor(Predicate&& predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 4s;
  do {
    if (predicate()) return true;
    std::this_thread::sleep_for(15ms);
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

class ComfyUiBridgeTest : public testing::Test {
 protected:
  void SetUp() override {
    auto directory = std::filesystem::temp_directory_path() /
        ("container-comfyui-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    root = std::filesystem::canonical(directory);
  }

  void TearDown() override {
    if (!root.empty()) {
      std::error_code error;
      std::filesystem::remove_all(root, error);
    }
  }

  std::filesystem::path job(const ComfyUiBridge& bridge) const {
    return root / "jobs" / bridge.status().id;
  }

  static void publish(const std::filesystem::path& folder, const Json& record) {
    container::util::writeFileAtomically(folder / "status.json", record.dump());
  }

  static void publishImage(const std::filesystem::path& output) {
    container::util::writeFileAtomically(output, {image, sizeof(image) - 1});
  }

  std::filesystem::path root;
};

TEST_F(ComfyUiBridgeTest, RestyleCapturesFirstAndDecodesGeneratedPng) {
  ComfyUiBridge bridge(root);
  constexpr auto seed = std::numeric_limits<uint64_t>::max();
  ASSERT_TRUE(bridge.submit(ComfyUiJobKind::Restyle, "watercolor", seed));
  const auto folder = job(bridge);
  std::optional<ComfyUiCaptureRequest> capture;
  ASSERT_TRUE(waitFor([&] { return (capture = bridge.takeCaptureRequest()).has_value(); }));
  ASSERT_EQ(capture->path, folder / "input.png");
  EXPECT_FALSE(std::filesystem::exists(folder / "request.json"));
  publishImage(capture->path);
  bridge.completeCapture(capture->id);
  ASSERT_TRUE(waitFor([&] { return std::filesystem::exists(folder / "request.json"); }));
  std::ifstream requestFile(folder / "request.json");
  const auto request = Json::parse(requestFile);
  EXPECT_EQ(request.at("prompt"), "watercolor");
  EXPECT_EQ(request.at("seed").get<uint64_t>(), seed);

  publish(folder, {{"version", 1}, {"id", "previous-job"}, {"phase", "failed"}});
  std::this_thread::sleep_for(275ms);
  EXPECT_TRUE(bridge.status().busy());
  EXPECT_FALSE(bridge.takeResult());
  publish(folder, {{"version", 1}, {"id", capture->id}, {"phase", "running"}, {"progress", .75f}});
  ASSERT_TRUE(waitFor([&] { return bridge.status().phase == ComfyUiJobPhase::Running; }));
  EXPECT_FLOAT_EQ(bridge.status().progress, .75f);
  publishImage(folder / "generated.png");
  publish(folder, {{"version", 1}, {"id", capture->id}, {"phase", "succeeded"}, {"output", "generated.png"}});
  ASSERT_TRUE(waitFor([&] { return bridge.status().phase == ComfyUiJobPhase::Ready; }));
  auto result = bridge.takeResult();
  ASSERT_TRUE(result);
  EXPECT_EQ(result->encoded->size(), sizeof(image) - 1);
  ASSERT_EQ(result->preview.mips.size(), 1u);
  EXPECT_EQ(result->preview.mips[0].width, 2u);
  EXPECT_EQ(result->preview.mips[0].height, 1u);
  EXPECT_EQ(result->preview.mips[0].bytes[0], std::byte{128});
  EXPECT_FALSE(bridge.takeResult());
}

TEST_F(ComfyUiBridgeTest, CancelledAndInvalidResultsCannotBePublished) {
  ComfyUiBridge bridge(root);
  ASSERT_FALSE(bridge.submit(ComfyUiJobKind::Hdri, "  ", 1));
  ASSERT_TRUE(bridge.submit(ComfyUiJobKind::Hdri, "sunset", 1));
  const auto first = job(bridge);
  ASSERT_TRUE(waitFor([&] { return std::filesystem::exists(first / "request.json"); }));
  bridge.cancel();
  EXPECT_EQ(bridge.status().phase, ComfyUiJobPhase::Cancelled);
  ASSERT_TRUE(waitFor([&] { return std::filesystem::exists(first / "cancel.json"); }));

  ASSERT_TRUE(bridge.submit(ComfyUiJobKind::Hdri, "studio", 2));
  const auto second = job(bridge);
  ASSERT_NE(first, second);
  publish(first, {{"version", 1}, {"id", first.filename().string()}, {"phase", "succeeded"}, {"output", "generated.hdr"}});
  ASSERT_TRUE(waitFor([&] { return std::filesystem::exists(second / "request.json"); }));
  publish(second, {{"version", 1}, {"id", second.filename().string()}, {"phase", "succeeded"}, {"output", "../generated.hdr"}});
  ASSERT_TRUE(waitFor([&] { return bridge.status().phase == ComfyUiJobPhase::Failed; }));
  EXPECT_NE(bridge.status().message.find("unexpected output filename"), std::string::npos);
  EXPECT_FALSE(bridge.takeResult());
}

TEST_F(ComfyUiBridgeTest, CaptureErrorsDoNotReachTheAdapter) {
  ComfyUiBridge bridge(root);
  ASSERT_TRUE(bridge.submit(ComfyUiJobKind::Restyle, "study", 0));
  const auto folder = job(bridge);
  std::optional<ComfyUiCaptureRequest> capture;
  ASSERT_TRUE(waitFor([&] { return (capture = bridge.takeCaptureRequest()).has_value(); }));
  bridge.completeCapture(capture->id, "Viewport was unavailable");
  ASSERT_TRUE(waitFor([&] { return bridge.status().phase == ComfyUiJobPhase::Failed; }));
  EXPECT_EQ(bridge.status().message, "Viewport was unavailable");
  EXPECT_FALSE(std::filesystem::exists(folder / "request.json"));
}

}  // namespace
