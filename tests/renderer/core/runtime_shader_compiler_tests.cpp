#include "Container/renderer/pipeline/RuntimeShaderCompiler.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using container::renderer::compileShaderCandidate;

void writeText(const fs::path& path, std::string_view text) {
  std::ofstream stream(path, std::ios::binary);
  stream << text;
  if (!stream) throw std::runtime_error("Could not write shader compiler fixture");
}

std::string quoted(const fs::path& path) {
  return "[==[" + path.generic_string() + "]==]";
}

class RuntimeShaderCompiler : public testing::Test {
protected:
  void SetUp() override {
    const auto id = std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = fs::temp_directory_path() / ("shader compiler test " + std::to_string(id));
    ASSERT_TRUE(fs::create_directory(root_));
    writeText(root_ / "runtime_shader_reload_tool.txt", CONTAINER_TEST_CMAKE);
  }

  void TearDown() override {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }

  void manifest(std::string_view script) {
    writeText(root_ / "runtime_shader_reload.cmake", script);
  }

  void installStalledCompiler() {
    writeText(root_ / "child.cmake",
              "file(WRITE " + quoted(root_ / "started") + " \"1\")\n" +
              "execute_process(COMMAND \"${CMAKE_COMMAND}\" -E sleep 4)\n" +
              "file(WRITE " + quoted(root_ / "late-write") + " \"1\")\n");
    manifest("file(WRITE \"${RELOAD_DIRECTORY}/current-stage.txt\" \"stall.frag.spv\")\n"
             "file(WRITE " + quoted(root_ / "candidate-path") + " \"${RELOAD_DIRECTORY}\")\n" +
             "execute_process(COMMAND \"${CMAKE_COMMAND}\" -P " + quoted(root_ / "child.cmake") + ")\n");
  }

  void expectCandidateRemoved() const {
    std::ifstream stream(root_ / "candidate-path");
    std::string candidate;
    ASSERT_TRUE(std::getline(stream, candidate));
    EXPECT_FALSE(fs::exists(fs::path(candidate)));
  }

  fs::path root_;
};

TEST_F(RuntimeShaderCompiler, CandidateLifetimeOwnsTemporaryDirectory) {
  manifest("file(WRITE \"${RELOAD_DIRECTORY}/compiled.txt\" \"test.frag.spv\\n\")\n");
  auto candidate = compileShaderCandidate(root_);
  EXPECT_EQ(candidate->compiledStages, 1u);
  const auto directory = candidate->directory;
  EXPECT_TRUE(fs::is_directory(directory));
  candidate.reset();
  EXPECT_FALSE(fs::exists(directory));
}

TEST_F(RuntimeShaderCompiler, ReportsCompilerFailureAndRemovesCandidate) {
  manifest("file(WRITE " + quoted(root_ / "candidate-path") + " \"${RELOAD_DIRECTORY}\")\n" +
           "file(WRITE \"${RELOAD_DIRECTORY}/diagnostics.txt\" \"Malformed Slang\")\n"
           "message(FATAL_ERROR \"compile failed\")\n");
  try {
    static_cast<void>(compileShaderCandidate(root_));
    ADD_FAILURE() << "Expected compiler failure";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "Malformed Slang");
  }
  expectCandidateRemoved();
}

TEST_F(RuntimeShaderCompiler, DeadlineTerminatesCompilerDescendants) {
  installStalledCompiler();
  const auto start = std::chrono::steady_clock::now();
  try {
    static_cast<void>(compileShaderCandidate(root_, {}, 2s));
    ADD_FAILURE() << "Expected compiler timeout";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "Shader compilation timed out while compiling stall.frag.spv");
  }
  EXPECT_LT(std::chrono::steady_clock::now() - start, 4s);
  ASSERT_TRUE(fs::exists(root_ / "started"));
  expectCandidateRemoved();
  std::this_thread::sleep_until(start + 5s);
  EXPECT_FALSE(fs::exists(root_ / "late-write"));
}

TEST_F(RuntimeShaderCompiler, CancellationStopsAnInFlightCompiler) {
  installStalledCompiler();
  std::stop_source source;
  auto result = std::async(std::launch::async, [this, stop = source.get_token()] {
    return compileShaderCandidate(root_, stop);
  });
  const auto start = std::chrono::steady_clock::now();
  while (!fs::exists(root_ / "started") && std::chrono::steady_clock::now() - start < 2s)
    std::this_thread::sleep_for(10ms);
  const bool childStarted = fs::exists(root_ / "started");
  source.request_stop();
  ASSERT_TRUE(childStarted);
  EXPECT_EQ(result.wait_for(2s), std::future_status::ready);
  try {
    static_cast<void>(result.get());
    ADD_FAILURE() << "Expected compiler cancellation";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "Shader compilation was cancelled while compiling stall.frag.spv");
  }
  expectCandidateRemoved();
  std::this_thread::sleep_until(start + 5s);
  EXPECT_FALSE(fs::exists(root_ / "late-write"));
}

} // namespace
