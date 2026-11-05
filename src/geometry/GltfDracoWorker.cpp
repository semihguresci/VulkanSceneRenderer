#include <Container/geometry/GltfDracoWorker.h>
#include <Container/geometry/GltfDracoProtocol.h>

#include <atomic>
#include <chrono>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace container::geometry::gltf {
namespace {
[[noreturn]] void fail(std::string_view reason) {
  throw std::runtime_error("Bounded Draco decode: " + std::string(reason));
}

#ifdef _WIN32
class Handle {
public:
  Handle() = default;
  explicit Handle(HANDLE value) : value_(value) {}
  ~Handle() { reset(); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  void reset(HANDLE value = nullptr) {
    if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    value_ = value;
  }
  HANDLE get() const { return value_; }
private:
  HANDLE value_{};
};

class TemporaryDirectory {
public:
  explicit TemporaryDirectory(const std::filesystem::path& requestedRoot) {
    const auto base = requestedRoot.empty() ? std::filesystem::temp_directory_path() : requestedRoot;
    root_ = std::filesystem::canonical(std::filesystem::absolute(base));
    if (!std::filesystem::is_directory(root_)) fail("temporary root is not a directory");
    static std::atomic<uint64_t> sequence{};
    std::random_device random;
    for (uint32_t attempt = 0; attempt < 32; ++attempt) {
      const auto name = "container-draco-" + std::to_string(GetCurrentProcessId()) + "-" +
          std::to_string(random()) + "-" + std::to_string(sequence.fetch_add(1));
      const auto candidate = (root_ / name).lexically_normal();
      if (candidate.parent_path() != root_) fail("temporary path escapes its root");
      if (!std::filesystem::create_directory(candidate)) continue;
      // Cleanup below removes only known files and the empty owned directory;
      // it never recursively traverses untrusted files or directory junctions.
      if (std::filesystem::canonical(candidate).parent_path() != root_)
        fail("temporary directory resolves outside its root");
      path_ = candidate;
      // Retain the exact directory identity without delete sharing, preventing
      // the worker from renaming/replacing this directory while it runs.
      directory_.reset(CreateFileW(path_.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
      if (directory_.get() == INVALID_HANDLE_VALUE ||
          !GetFileInformationByHandle(directory_.get(), &identity_) ||
          (identity_.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        directory_.reset();
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        fail("cannot retain the owned temporary directory identity");
      }
      return;
    }
    fail("cannot create a unique temporary directory");
  }
  ~TemporaryDirectory() {
    if (path_.empty() || path_.parent_path() != root_) return;
    BY_HANDLE_FILE_INFORMATION current{};
    if (!GetFileInformationByHandle(directory_.get(), &current) ||
        current.dwVolumeSerialNumber != identity_.dwVolumeSerialNumber ||
        current.nFileIndexHigh != identity_.nFileIndexHigh ||
        current.nFileIndexLow != identity_.nFileIndexLow) return;
    const DWORD attributes = GetFileAttributesW(path_.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      // A child may have converted an empty directory into a junction in-place.
      // Never traverse it to delete known filenames in the junction's target.
      directory_.reset();
      (void)RemoveDirectoryW(path_.c_str()); // Removes the directory link itself.
      return;
    }
    std::error_code ignored;
    const auto resolved = std::filesystem::canonical(path_, ignored);
    if (ignored || resolved != path_ || resolved.parent_path() != root_) return;
    std::filesystem::remove(request(), ignored);
    std::filesystem::remove(response(), ignored);
    directory_.reset();
    std::filesystem::remove(path_, ignored);
  }
  std::filesystem::path request() const { return path_ / "request.bin"; }
  std::filesystem::path response() const { return path_ / "response.bin"; }
private:
  std::filesystem::path root_, path_;
  Handle directory_;
  BY_HANDLE_FILE_INFORMATION identity_{};
};

std::filesystem::path workerPath(const DracoWorkerOptions& options) {
  std::filesystem::path executable = options.workerExecutable;
  if (executable.empty()) {
    std::wstring module(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (!length || length >= module.size()) fail("cannot discover the application directory");
    module.resize(length);
    executable = std::filesystem::path(module).parent_path() / L"ContainerDracoWorker.exe";
  }
  if (!executable.is_absolute()) fail("worker executable override must be absolute");
  if (!std::filesystem::is_regular_file(executable)) fail("optional ContainerDracoWorker.exe is unavailable");
  return std::filesystem::canonical(executable);
}

std::wstring quote(const std::filesystem::path& path) {
  const auto value = path.native();
  if (value.find(L'"') != std::wstring::npos) fail("invalid quote in worker path");
  return L"\"" + value + L"\"";
}

class Child {
public:
  Handle job, process, thread;
  bool finished{};
  ~Child() {
    if (!finished && process.get()) {
      if (job.get()) TerminateJobObject(job.get(), 1);
      TerminateProcess(process.get(), 1); // Also covers assignment/startup failure.
      (void)WaitForSingleObject(process.get(), 5000);
    }
  }
};

void runWorker(const std::filesystem::path& executable,
               const TemporaryDirectory& temporary, const DracoDecodeLimits& limits) {
  Child child;
  child.job.reset(CreateJobObjectW(nullptr, nullptr));
  if (!child.job.get()) fail("cannot create decoder memory-limit job");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
  jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY |
      JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
      JOB_OBJECT_LIMIT_PROCESS_TIME | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
  jobLimits.BasicLimitInformation.ActiveProcessLimit = 1;
  jobLimits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart =
      static_cast<LONGLONG>(limits.timeoutMs) * 10'000;
  jobLimits.ProcessMemoryLimit = static_cast<SIZE_T>(limits.processMemoryBytes);
  if (!SetInformationJobObject(child.job.get(), JobObjectExtendedLimitInformation,
                                &jobLimits, sizeof(jobLimits)))
    fail("cannot enforce decoder memory/CPU limits");

  std::wstring command = quote(executable) + L" " + quote(temporary.request()) + L" " + quote(temporary.response());
  STARTUPINFOW startup{}; startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION process{};
  // No decoder instruction can run before its suspended process joins the job.
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                      nullptr, executable.parent_path().c_str(), &startup, &process))
    fail("cannot launch the isolated decoder");
  child.process.reset(process.hProcess); child.thread.reset(process.hThread);
  if (!AssignProcessToJobObject(child.job.get(), child.process.get()))
    fail("cannot assign decoder to the bounded job; decode was not started");
  if (ResumeThread(child.thread.get()) == DWORD(-1)) fail("cannot start the bounded decoder");
  const DWORD wait = WaitForSingleObject(child.process.get(), limits.timeoutMs);
  if (wait == WAIT_TIMEOUT) fail("worker wall timeout exceeded");
  if (wait != WAIT_OBJECT_0) fail("cannot wait for the bounded decoder");
  child.finished = true;
  DWORD exit{};
  if (!GetExitCodeProcess(child.process.get(), &exit) || exit != 0)
    fail("worker terminated before a valid response (memory/CPU limit or decode failure)");
}
#endif
} // namespace

bool DracoWorkerPlatformSupported() noexcept {
#ifdef _WIN32
  return true;
#else
  return false;
#endif
}

DracoDecodedMesh DecodeDracoInWorker(std::span<const std::byte> compressed,
    std::span<const DracoAttributeRequest> attributes, const DracoDecodeLimits& limits,
    const DracoWorkerOptions& options) {
  draco_worker::ValidateRequest(compressed, attributes, limits);
#ifdef _WIN32
  const auto executable = workerPath(options);
  TemporaryDirectory temporary(options.tempRoot);
  draco_worker::WriteRequest(temporary.request(), compressed, attributes, limits);
  runWorker(executable, temporary, limits);
  return draco_worker::ReadResponse(temporary.response(), attributes, limits);
#else
  (void)options;
  fail("bounded worker unavailable on this platform; no in-process fallback is permitted");
#endif
}

} // namespace container::geometry::gltf
