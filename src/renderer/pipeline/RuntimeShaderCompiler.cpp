#include "Container/renderer/pipeline/RuntimeShaderCompiler.h"
#include "Container/renderer/pipeline/RayEffectPipelines.h"
#include "Container/utility/FileLoader.h"
#include "Container/utility/Platform.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace container::renderer {
namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

class CompilerDeadline {
public:
  CompilerDeadline(std::stop_token stop, std::chrono::milliseconds timeout)
      : stop_(stop), timeout_(timeout) {}

  std::chrono::milliseconds nextWait() const {
    if (stop_.stop_requested())
      throw std::runtime_error("Shader compilation was cancelled");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_);
    const auto remaining = timeout_ - elapsed;
    if (remaining <= 0ms)
      throw std::runtime_error("Shader compilation timed out");
    return std::min(25ms, remaining);
  }

private:
  std::stop_token stop_;
  std::chrono::milliseconds timeout_;
  std::chrono::steady_clock::time_point started_{std::chrono::steady_clock::now()};
};

#ifdef _WIN32
struct CloseWinHandle {
  void operator()(HANDLE handle) const noexcept { CloseHandle(handle); }
};
using WinHandle = std::unique_ptr<void, CloseWinHandle>;
#else
struct CompilerProcessGroup {
  pid_t leader;

  ~CompilerProcessGroup() {
    if (leader <= 0) return;
    // Keep the leader unreaped while signalling its process group. This keeps
    // the group ID from being reused by an unrelated process.
    (void)kill(-leader, SIGKILL);
    int status{};
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto result = waitpid(leader, &status, WNOHANG);
      if (result == leader || (result < 0 && errno != EINTR)) return;
      std::this_thread::sleep_for(10ms);
    }
    // A child stuck in kernel I/O can remain alive after SIGKILL. Reap it when
    // it exits without making renderer shutdown wait for that I/O.
    try {
      std::thread([pid = leader] {
        int status{};
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
      }).detach();
    } catch (...) {
      // The system will reap the child after the renderer exits.
    }
  }
};
#endif

std::string readText(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

bool runCompiler(const fs::path& cmake, const fs::path& script,
                 const fs::path& output, const CompilerDeadline& deadline) {
  (void)deadline.nextWait();
#ifdef _WIN32
  // Windows paths cannot contain quotes. The generated paths end in filenames
  // or a staging folder name, so no trailing backslash escapes a closing quote.
  auto command = L"\"" + cmake.native() + L"\" \"-DRELOAD_DIRECTORY=" +
      output.native() + L"\" -P \"" + script.native() + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  // Enroll the suspended process before CMake can start Slang. Closing the
  // job terminates both processes on timeout, cancellation, or failure.
  WinHandle job(CreateJobObjectW(nullptr, nullptr));
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                        &limits, sizeof(limits)))
    throw std::system_error(GetLastError(), std::system_category(),
                            "Could not create the shader compiler job");
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(cmake.c_str(), command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr,
                      &startup, &process))
    throw std::system_error(GetLastError(), std::system_category(),
                            "Could not launch the build-tree CMake shader compiler");
  WinHandle leader(process.hProcess);
  WinHandle thread(process.hThread);
  if (!AssignProcessToJobObject(job.get(), leader.get())) {
    const auto error = GetLastError();
    (void)TerminateProcess(leader.get(), 1);
    (void)WaitForSingleObject(leader.get(), 1000);
    throw std::system_error(error, std::system_category(),
                            "Could not supervise the shader compiler process");
  }
  if (ResumeThread(thread.get()) == static_cast<DWORD>(-1))
    throw std::system_error(GetLastError(), std::system_category(),
                            "Could not start the shader compiler process");

  try {
    for (;;) {
      const DWORD waited = WaitForSingleObject(
          leader.get(), static_cast<DWORD>(deadline.nextWait().count()));
      if (waited == WAIT_TIMEOUT) continue;
      DWORD status{};
      if (waited != WAIT_OBJECT_0 || !GetExitCodeProcess(leader.get(), &status))
        throw std::system_error(GetLastError(), std::system_category(),
                                "Could not read the shader compiler exit status");
      return status == 0;
    }
  } catch (...) {
    job.reset();
    (void)WaitForSingleObject(leader.get(), 1000);
    throw;
  }
#else
  auto executable = cmake.native();
  auto option = "-DRELOAD_DIRECTORY=" + output.native();
  auto manifest = script.native();
  char flag[] = "-P";
  char* args[]{executable.data(), option.data(), flag, manifest.data(), nullptr};
  posix_spawnattr_t attributes;
  int error = posix_spawnattr_init(&attributes);
  if (error != 0)
    throw std::system_error(error, std::generic_category(), "Could not prepare shader compiler launch");
  pid_t pid{};
  error = posix_spawnattr_setpgroup(&attributes, 0);
  if (error == 0) error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  if (error == 0)
    error = posix_spawn(&pid, executable.c_str(), nullptr, &attributes, args, environ);
  posix_spawnattr_destroy(&attributes);
  if (error != 0)
    throw std::system_error(error, std::generic_category(), "Could not launch the build-tree CMake shader compiler");

  CompilerProcessGroup child{pid};
  for (;;) {
    const auto delay = deadline.nextWait();
    siginfo_t exitInfo{};
    if (waitid(P_PID, pid, &exitInfo, WEXITED | WNOHANG | WNOWAIT) < 0) {
      if (errno == EINTR) continue;
      if (errno == ECHILD) child.leader = 0;
      throw std::system_error(errno, std::generic_category(), "Could not read the shader compiler exit status");
    }
    if (exitInfo.si_pid == pid)
      return exitInfo.si_code == CLD_EXITED && exitInfo.si_status == 0;
    std::this_thread::sleep_for(delay);
  }
#endif
}
} // namespace

vk::raii::Pipeline loadRayEffectComputePipeline(
    const vk::raii::Device& device, const fs::path& shader, vk::PipelineLayout layout) {
  const auto bytes = container::util::readFile(shader);
  if (bytes.empty() || bytes.size() % sizeof(uint32_t))
    throw std::runtime_error("Invalid compute shader SPIR-V: " + container::util::pathToUtf8(shader));
  std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
  std::memcpy(words.data(), bytes.data(), bytes.size());
  const vk::raii::ShaderModule module(
      device, vk::ShaderModuleCreateInfo({}, bytes.size(), words.data()));
  const vk::PipelineShaderStageCreateInfo stage(
      {}, vk::ShaderStageFlagBits::eCompute, *module, "computeMain");
  return vk::raii::Pipeline(device, nullptr, vk::ComputePipelineCreateInfo({}, stage, layout));
}

ShaderReloadCandidate::ShaderReloadCandidate(fs::path path) : directory(std::move(path)) {}

ShaderReloadCandidate::~ShaderReloadCandidate() {
  std::error_code ignored;
  fs::remove_all(directory, ignored);
}

std::shared_ptr<ShaderReloadCandidate> compileShaderCandidate(
    const fs::path& executable, std::stop_token stop,
    std::chrono::milliseconds timeout) {
  if (timeout <= 0ms)
    throw std::invalid_argument("Shader compiler timeout must be positive");
  const CompilerDeadline deadline(stop, timeout);
  (void)deadline.nextWait();
  auto script = executable / "runtime_shader_reload.cmake";
  if (!fs::is_regular_file(script))
    script = executable.parent_path() / script.filename();
  if (!fs::is_regular_file(script))
    throw std::runtime_error("Shader reload requires a source build with its CMake shader manifest");
  const auto compiler = container::util::pathFromUtf8(
      readText(script.parent_path() / "runtime_shader_reload_tool.txt"));
  if (compiler.empty() || !fs::is_regular_file(compiler))
    throw std::runtime_error("The CMake executable recorded by the source build is unavailable");

  const auto temp = fs::temp_directory_path();
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  for (int attempt = 0; attempt < 32; ++attempt) {
    auto directory = temp / ("container-shader-reload-" + std::to_string(stamp) + "-" +
                             std::to_string(attempt));
    if (!fs::create_directory(directory))
      continue;
    auto candidate = std::make_shared<ShaderReloadCandidate>(std::move(directory));
    bool compiled = false;
    try {
      compiled = runCompiler(compiler, script, candidate->directory, deadline);
    } catch (const std::runtime_error& error) {
      const auto stage = readText(candidate->directory / "current-stage.txt");
      if (!stage.empty())
        throw std::runtime_error(std::string(error.what()) + " while compiling " + stage);
      throw;
    }
    if (!compiled) {
      auto diagnostics = readText(candidate->directory / "diagnostics.txt");
      if (diagnostics.empty())
        diagnostics = "The candidate compiler exited before producing diagnostics";
      throw std::runtime_error(std::move(diagnostics));
    }
    candidate->compiledStages = static_cast<std::size_t>(
        std::ranges::count(readText(candidate->directory / "compiled.txt"), '\n'));
    return candidate;
  }
  throw std::runtime_error("Could not create a shader candidate directory");
}

} // namespace container::renderer
