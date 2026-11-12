#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace container::util {

[[noreturn]] inline void atomicFileError(std::string_view action, unsigned long error) {
  throw std::runtime_error("Atomic file write: " + std::string(action) +
                           " failed (system error " + std::to_string(error) +
                           ")");
}

inline std::shared_ptr<std::mutex>
replacementMutex(const std::filesystem::path &path) {
  // Resolve parent aliases without following a destination-file symlink: an
  // atomic save replaces that link itself. Keep filenames case-insensitive on
  // Windows, matching ordinary Win32 path lookup.
  const auto absolute = std::filesystem::absolute(path).lexically_normal();
  std::error_code error;
  auto parent =
      std::filesystem::weakly_canonical(absolute.parent_path(), error);
  if (error)
    parent = absolute.parent_path();
  const auto encoded = (parent / absolute.filename()).generic_u8string();
  std::string key(reinterpret_cast<const char *>(encoded.data()),
                  encoded.size());
#ifdef _WIN32
  for (char &ch : key)
    if (ch >= 'A' && ch <= 'Z')
      ch = static_cast<char>(ch + ('a' - 'A'));
#endif
  static std::mutex registryMutex;
  static std::map<std::string, std::weak_ptr<std::mutex>> registry;
  const std::lock_guard registryLock(registryMutex);
  std::erase_if(registry,
                [](const auto &entry) { return entry.second.expired(); });
  auto &entry = registry[key];
  if (auto writer = entry.lock())
    return writer;
  auto writer = std::make_shared<std::mutex>();
  entry = writer;
  return writer;
}

enum class AtomicFilePublication { ReplaceExisting, CreateNew };

// The caller supplies a complete file on the destination filesystem.
inline void replaceFileAtomically(const std::filesystem::path &source,
                                  const std::filesystem::path &destination) {
#ifdef _WIN32
  for (unsigned attempt = 0;; ++attempt) {
    if (MoveFileExW(source.c_str(), destination.c_str(),
                    MOVEFILE_WRITE_THROUGH | MOVEFILE_REPLACE_EXISTING))
      return;
    const DWORD renameError = GetLastError();
    const DWORD attributes = GetFileAttributesW(destination.c_str());
    const bool permanentDestination =
        attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY));
    if (attempt >= 20 || permanentDestination ||
        (renameError != ERROR_SHARING_VIOLATION && renameError != ERROR_ACCESS_DENIED))
      atomicFileError("replacing destination", renameError);
    // A concurrent reader or antivirus inspection can briefly hold a file open.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
#else
  if (::rename(source.c_str(), destination.c_str()) != 0)
    atomicFileError("replacing destination", errno);
#endif
}

inline void writeFileAtomically(const std::filesystem::path &path, std::string_view contents,
    AtomicFilePublication publication = AtomicFilePublication::ReplaceExisting) {
  // Simultaneous writers in this process share a destination lock. Other
  // destinations remain independent, and the OS rename is still atomic for
  // readers and writers in other processes.
  const auto writerMutex = replacementMutex(path);
  const std::lock_guard writerLock(*writerMutex);
  static std::atomic<uint64_t> serial{0};
  std::filesystem::path temporary;
#ifdef _WIN32
  HANDLE file = INVALID_HANDLE_VALUE;
#else
  int file = -1;
#endif
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  for (unsigned attempt = 0; attempt < 64; ++attempt) {
    temporary = path;
    temporary += ".tmp-" + std::to_string(stamp) + "-" +
                 std::to_string(serial.fetch_add(1));
#ifdef _WIN32
    file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
      break;
    if (GetLastError() != ERROR_FILE_EXISTS &&
        GetLastError() != ERROR_ALREADY_EXISTS)
      atomicFileError("creating temporary file", GetLastError());
#else
    file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (file >= 0)
      break;
    if (errno != EEXIST)
      atomicFileError("creating temporary file", errno);
#endif
  }
#ifdef _WIN32
  if (file == INVALID_HANDLE_VALUE)
    throw std::runtime_error(
        "Atomic file write: cannot create unique temporary file");
#else
  if (file < 0)
    throw std::runtime_error(
        "Atomic file write: cannot create unique temporary file");
#endif
  try {
#ifdef _WIN32
    DWORD written = 0;
    if (!WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()),
                   &written, nullptr) ||
        written != contents.size())
      atomicFileError("writing temporary file", GetLastError());
    if (!FlushFileBuffers(file))
      atomicFileError("flushing temporary file", GetLastError());
    if (!CloseHandle(file)) {
      file = INVALID_HANDLE_VALUE;
      atomicFileError("closing temporary file", GetLastError());
    }
    file = INVALID_HANDLE_VALUE;
    if (publication == AtomicFilePublication::CreateNew) {
      if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH))
        atomicFileError("publishing a new destination", GetLastError());
    } else {
      replaceFileAtomically(temporary, path);
    }
#else
    size_t offset = 0;
    while (offset < contents.size()) {
      const auto written =
          ::write(file, contents.data() + offset, contents.size() - offset);
      if (written < 0 && errno == EINTR)
        continue;
      if (written <= 0)
        atomicFileError("writing temporary file", errno);
      offset += static_cast<size_t>(written);
    }
    if (::fsync(file) != 0)
      atomicFileError("flushing temporary file", errno);
    const int closeResult = ::close(file);
    file = -1;
    if (closeResult != 0)
      atomicFileError("closing temporary file", errno);
    if (publication == AtomicFilePublication::CreateNew) {
      if (::link(temporary.c_str(), path.c_str()) != 0)
        atomicFileError("publishing a new destination", errno);
      if (::unlink(temporary.c_str()) != 0)
        atomicFileError("removing the temporary hard link", errno);
    } else {
      replaceFileAtomically(temporary, path);
    }
#endif
  } catch (...) {
#ifdef _WIN32
    if (file != INVALID_HANDLE_VALUE)
      CloseHandle(file);
#else
    if (file >= 0)
      ::close(file);
#endif
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}

} // namespace container::util
