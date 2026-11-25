#include "Container/utility/DerivedAssetCache.h"

#include "Container/utility/AtomicFile.h"
#include "Container/utility/ContentFingerprint.h"
#include "Container/utility/ModelImportCancellation.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <span>

namespace container::util {
namespace {

constexpr std::string_view magic = "CNTDRVD1";
constexpr size_t digestBytes = 64;
constexpr size_t headerBytes = magic.size() + sizeof(uint32_t) + digestBytes +
                               sizeof(uint64_t) + digestBytes;
constexpr size_t ioChunkBytes = 64 * 1024;

std::filesystem::path cacheIoPath(const std::filesystem::path& file) {
#ifdef _WIN32
  // Include room for the atomic writer's temporary suffix even when the
  // eventual cache entry fits within the legacy Win32 path length.
  std::error_code error;
  auto path = file.is_absolute() ? file : std::filesystem::absolute(file, error);
  if (error)
    return {};
  path = path.lexically_normal();
  path.make_preferred();
  const auto& native = path.native();
  if (native.starts_with(LR"(\\?\)") || native.starts_with(LR"(\\.\)"))
    return path;
  if (native.starts_with(LR"(\\)"))
    return std::filesystem::path(LR"(\\?\UNC\)" + native.substr(2));
  return std::filesystem::path(LR"(\\?\)" + native);
#else
  return file;
#endif
}

bool validKey(std::string_view key) {
  return key.size() == digestBytes && std::ranges::all_of(key, [](char code) {
    return ('0' <= code && code <= '9') || ('a' <= code && code <= 'f');
  });
}

void appendInteger(std::string& buffer, uint64_t value, size_t bytes) {
  for (size_t i = 0; i < bytes; ++i)
    buffer.push_back(static_cast<char>(value >> (8 * i)));
}

uint64_t readInteger(std::string_view bytes) {
  uint64_t result = 0;
  for (size_t i = 0; i < bytes.size(); ++i)
    result |= uint64_t(static_cast<unsigned char>(bytes[i])) << (8 * i);
  return result;
}

std::string digest(std::string_view bytes, std::stop_token stop) {
  const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
  return FingerprintContent(std::span(data, bytes.size()), stop).digest;
}

} // namespace

std::string_view derivedAssetCacheStatusName(DerivedAssetCacheStatus status) noexcept {
  switch (status) {
    case DerivedAssetCacheStatus::Disabled: return "disabled";
    case DerivedAssetCacheStatus::NotCacheable: return "notCacheable";
    case DerivedAssetCacheStatus::Miss: return "miss";
    case DerivedAssetCacheStatus::Hit: return "hit";
    case DerivedAssetCacheStatus::Corrupt: return "corrupt";
    case DerivedAssetCacheStatus::IoError: return "ioError";
  }
  return "ioError";
}

DerivedAssetCacheRead readDerivedAssetCache(
    const std::filesystem::path& file, uint32_t formatVersion, std::string_view key,
    uint64_t maxPayloadBytes, std::stop_token stop, DerivedAssetCacheKeyMismatch keyMismatch) {
  checkModelImportCancellation(stop);
  if (file.empty())
    return {.status = DerivedAssetCacheStatus::Disabled};
  if (!validKey(key))
    return {.status = DerivedAssetCacheStatus::IoError};

  const auto path = cacheIoPath(file);
  if (path.empty())
    return {.status = DerivedAssetCacheStatus::IoError};
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (status.type() == std::filesystem::file_type::not_found ||
      error == std::errc::no_such_file_or_directory)
    return {};
  if (error || !std::filesystem::is_regular_file(status))
    return {.status = DerivedAssetCacheStatus::IoError};

  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input)
    return {.status = DerivedAssetCacheStatus::IoError};
  const auto fileSize = input.tellg();
  if (fileSize < 0)
    return {.status = DerivedAssetCacheStatus::IoError};
  if (fileSize < static_cast<std::streamoff>(headerBytes) ||
      static_cast<uint64_t>(fileSize) - headerBytes > maxPayloadBytes)
    return {.status = DerivedAssetCacheStatus::Corrupt};

  input.seekg(0);
  std::array<char, headerBytes> header{};
  input.read(header.data(), static_cast<std::streamsize>(header.size()));
  const auto headerRead = static_cast<uint64_t>(input.gcount());
  if (!input || headerRead != headerBytes)
    return {.status = input.bad() ? DerivedAssetCacheStatus::IoError : DerivedAssetCacheStatus::Corrupt,
            .bytesRead = headerRead};

  std::string_view fields(header.data(), header.size());
  if (!fields.starts_with(magic))
    return {.status = DerivedAssetCacheStatus::Corrupt, .bytesRead = headerRead};
  fields.remove_prefix(magic.size());
  if (readInteger(fields.substr(0, sizeof(formatVersion))) != formatVersion)
    return {.status = DerivedAssetCacheStatus::Corrupt, .bytesRead = headerRead};
  fields.remove_prefix(sizeof(formatVersion));
  const auto storedKey = fields.substr(0, digestBytes);
  if (!validKey(storedKey))
    return {.status = DerivedAssetCacheStatus::Corrupt, .bytesRead = headerRead};
  fields.remove_prefix(digestBytes);
  const auto payloadSize = readInteger(fields.substr(0, sizeof(uint64_t)));
  fields.remove_prefix(sizeof(uint64_t));
  if (payloadSize != static_cast<uint64_t>(fileSize) - headerBytes ||
      payloadSize > std::string{}.max_size())
    return {.status = DerivedAssetCacheStatus::Corrupt, .bytesRead = headerRead};

  if (storedKey != key) {
    return {.status = keyMismatch == DerivedAssetCacheKeyMismatch::Miss
                        ? DerivedAssetCacheStatus::Miss : DerivedAssetCacheStatus::Corrupt,
            .bytesRead = headerRead};
  }

  std::string payload(static_cast<size_t>(payloadSize), '\0');
  size_t offset = 0;
  while (offset < payload.size()) {
    checkModelImportCancellation(stop);
    const auto count = std::min(payload.size() - offset, ioChunkBytes);
    input.read(payload.data() + offset, static_cast<std::streamsize>(count));
    offset += static_cast<size_t>(input.gcount());
    if (!input)
      return {.status = input.bad() ? DerivedAssetCacheStatus::IoError : DerivedAssetCacheStatus::Corrupt,
              .bytesRead = headerRead + offset};
  }
  if (input.peek() != std::char_traits<char>::eof() || digest(payload, stop) != fields)
    return {.status = DerivedAssetCacheStatus::Corrupt, .bytesRead = headerRead + offset};
  return {DerivedAssetCacheStatus::Hit, std::move(payload), headerRead + offset};
}

uint64_t writeDerivedAssetCache(
    const std::filesystem::path& file, uint32_t formatVersion, std::string_view key,
    std::string_view payload, std::stop_token stop) {
  checkModelImportCancellation(stop);
  if (file.empty() || !validKey(key) ||
      payload.size() > (std::numeric_limits<uint32_t>::max)() - headerBytes)
    return 0;
  try {
    std::string record;
    record.reserve(headerBytes + payload.size());
    record += magic;
    appendInteger(record, formatVersion, sizeof(formatVersion));
    record += key;
    appendInteger(record, payload.size(), sizeof(uint64_t));
    record += digest(payload, stop);
    record.append(payload);
    checkModelImportCancellation(stop);

    const auto path = cacheIoPath(file);
    if (path.empty())
      return 0;
    if (const auto parent = path.parent_path(); !parent.empty())
      std::filesystem::create_directories(parent);
    writeFileAtomically(path, record);
    return record.size();
  } catch (const ModelImportCancelled&) {
    throw;
  } catch (const std::exception&) {
    return 0;
  }
}

} // namespace container::util
