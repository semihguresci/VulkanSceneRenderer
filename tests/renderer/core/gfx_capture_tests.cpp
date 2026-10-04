#include "Container/app/GfxCapture.h"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
using namespace container::capture;
TEST(GfxCaptureConfig, PositiveOrderedFrameRangesHaveExplicitEnd) {
  EXPECT_EQ(validateFrameRanges("9-12,20,30-31"), 31u);
  EXPECT_EQ(validateFrameRanges("4294967295"), 4294967295u);
}
TEST(GfxCaptureConfig, InvalidRangesCannotSilentlyCaptureAllFrames) {
  for (const std::string value : {"", "0", "-1", "1-0", "10-8", "1,1", "1-3,3-5",
                                  "2,1", "1,", "1--2", "1, 2", "4294967296", "1foo"})
    EXPECT_THROW(validateFrameRanges(value), std::invalid_argument) << value;
}
TEST(GfxCaptureConfig, LegacySessionsCannotArmReservedDebugHotkeys) {
  const auto folder = std::filesystem::temp_directory_path() /
      ("container-gfx-hotkey-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(folder);
  std::vector<std::string> keys{"F6", "F7", "F8"};
#if defined(_WIN32)
  keys.push_back("F12");
#endif
  const auto path = folder / "session.json";
  for (const auto& key : keys) {
    const nlohmann::json session{{"schemaVersion", 1}, {"outputDirectory", folder.string()},
        {"mode", "hotkey"}, {"frames", ""}, {"trigger", key}, {"toolVersion", "1.0.5"},
        {"stopAfterPresent", 0}, {"environment", nlohmann::json::object()}};
    { std::ofstream stream(path); stream << session.dump(); }
    EXPECT_THROW(loadSession(path), std::invalid_argument) << key;
  }
  std::filesystem::remove_all(folder);
}
TEST(GfxCaptureJournal, TicksAcquireFailuresSubmitsAndPresentsAreIndependent) {
  const auto folder = std::filesystem::temp_directory_path() /
      ("container-gfx-journal-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(folder);
  Config config;
  config.sessionPath = folder / "session.json";
  config.outputDirectory = folder;
  {
    Journal journal;
    journal.open(config, {{"schemaVersion", 1}});
    journal.tick(1, true);
    journal.tick(2, false);
    journal.acquireFailed(-1000001004);
    journal.submitted();
    EXPECT_EQ(journal.presentCalls(), 0u);
    journal.presented(0, 0, 2, 0, {{"taa", {{"submittedFrame", 1}}}});
    journal.tick(3, false);
    journal.submitted();
    journal.presented(-1000001004, -1000001004, 0, 1, {});
    EXPECT_EQ(journal.presentCalls(), 2u);
  }
  std::ifstream stream(folder / "frames.jsonl");
  std::vector<nlohmann::json> events;
  for (std::string line; std::getline(stream, line);) events.push_back(nlohmann::json::parse(line));
  ASSERT_EQ(events.size(), 8u);
  EXPECT_EQ(events[2]["type"], "acquireFailed");
  EXPECT_EQ(events[2]["successfulSubmissions"], 0);
  EXPECT_EQ(events[4]["tick"], 2);
  EXPECT_EQ(events[4]["presentCalls"], 1);
  EXPECT_EQ(events[4]["successfulSubmissions"], 1);
  EXPECT_EQ(events[4]["imageIndex"], 2);
  EXPECT_EQ(events[7]["tick"], 3);
  EXPECT_EQ(events[7]["successfulSubmissions"], 2);
  EXPECT_EQ(events[7]["presentCalls"], 2);
  Journal second;
  EXPECT_THROW(second.open(config, {}), std::invalid_argument);
  stream.close();
  std::filesystem::remove_all(folder);
}
TEST(GfxCaptureJournal, DisabledModeCreatesNoFilesAndAdvancesNoCounters) {
  Journal journal;
  journal.open(Config{}, {});
  journal.tick(10, false);
  journal.acquireFailed(-1);
  journal.submitted();
  journal.presented(0, 0, 1, 0, {});
  EXPECT_FALSE(journal.enabled());
  EXPECT_EQ(journal.presentCalls(), 0u);
}
} // namespace
