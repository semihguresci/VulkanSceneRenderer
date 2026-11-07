#include "Container/renderer/core/RendererMcpServer.h"
#include "Container/utility/AtomicFile.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>

namespace {

using container::renderer::RendererMcpMeasurement;
using namespace std::chrono_literals;

TEST(RendererMcpMeasurement, DistinctRetiredFramesAreWarmedUpAndMeasuredExactlyOnce) {
  const auto now = RendererMcpMeasurement::Clock::time_point{};
  RendererMcpMeasurement series(100, 2, 4, now + 1s);
  series.observe(100, 99, now);
  series.observe(101, 99, now);
  series.observe(101, 99, now);
  EXPECT_EQ(series.report(now).at("warmupRemaining"), 1);
  series.observe(102, 99, now);
  series.observe(103, std::nullopt, now);
  series.observe(103, 99, now);
  series.observe(104, std::numeric_limits<double>::infinity(), now);
  series.observe(105, 1, now);
  series.observe(106, 3, now);
  series.observe(105, 99, now);
  series.observe(107, 5, now);
  EXPECT_EQ(series.status(now), RendererMcpMeasurement::Status::Running);
  series.observe(108, 7, now);
  series.observe(109, 99, now);

  const auto result = series.report(now + 2s);
  EXPECT_EQ(result.at("status"), "complete");
  EXPECT_EQ(result.at("samples"), 4);
  EXPECT_EQ(result.at("firstRecordingSerial"), 105);
  EXPECT_EQ(result.at("lastRecordingSerial"), 108);
  const auto& stats = result.at("milliseconds");
  EXPECT_EQ(stats.at("mean"), 4.0);
  EXPECT_EQ(stats.at("median"), 4.0);
  EXPECT_EQ(stats.at("min"), 1.0);
  EXPECT_EQ(stats.at("max"), 7.0);
  EXPECT_DOUBLE_EQ(stats.at("stddev").get<double>(), std::sqrt(5.0));
}

TEST(RendererMcpMeasurement, TimedOutAndCancelledSessionsDoNotCollectLateResults) {
  const auto now = RendererMcpMeasurement::Clock::time_point{};
  RendererMcpMeasurement timedOut(0, 0, 2, now + 100ms);
  timedOut.observe(1, 1.2, now);
  timedOut.observe(2, 1.3, now + 100ms);
  timedOut.cancel(now + 100ms);
  const auto timedReport = timedOut.report(now + 100ms);
  EXPECT_EQ(timedReport.at("status"), "timedOut");
  EXPECT_EQ(timedReport.at("samples"), 1);
  EXPECT_FALSE(timedReport.contains("milliseconds"));

  RendererMcpMeasurement cancelled(1, 0, 1, now + 100ms);
  cancelled.cancel(now);
  cancelled.observe(2, 2, now);
  const auto cancelledReport = cancelled.report(now);
  EXPECT_EQ(cancelledReport.at("status"), "cancelled");
  EXPECT_EQ(cancelledReport.at("samples"), 0);
  EXPECT_FALSE(cancelledReport.contains("milliseconds"));
}

TEST(RendererMcpMeasurement, RejectsUnboundedOrEmptySampleRequests) {
  const auto deadline = RendererMcpMeasurement::Clock::time_point{} + 1s;
  EXPECT_THROW(RendererMcpMeasurement(0, 241, 1, deadline), std::invalid_argument);
  EXPECT_THROW(RendererMcpMeasurement(0, 0, 0, deadline), std::invalid_argument);
  EXPECT_THROW(RendererMcpMeasurement(0, 0, 241, deadline), std::invalid_argument);
}

TEST(RendererMcpFiles, ExclusivePublicationKeepsExistingBytes) {
  using container::util::AtomicFilePublication;
  using container::util::writeFileAtomically;
  const auto directory = std::filesystem::temp_directory_path() /
      ("container-mcp-atomic-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto output = directory / "capture.png";
  struct Cleanup {
    std::filesystem::path output, directory;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove(output, ignored);
      std::filesystem::remove(directory, ignored);
    }
  } cleanup{output, directory};
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  ASSERT_NO_THROW(writeFileAtomically(output, "initial", AtomicFilePublication::CreateNew));
  EXPECT_THROW(writeFileAtomically(output, "second", AtomicFilePublication::CreateNew), std::runtime_error);
  std::ifstream file(output, std::ios::binary);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), "initial");
}

} // namespace
