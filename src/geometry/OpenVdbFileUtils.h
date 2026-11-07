#pragma once

// Private helpers for new processing modules. File::write emits grid offsets;
// io::Stream does not, which prevents bounded metadata-only reads downstream.
#include <openvdb/io/File.h>
#include "OpenVdbTransformUtils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>

namespace container::geometry::vdb::detail {

inline std::filesystem::path CreateAsciiVdbTemporaryDirectory() {
  // The SDK accepts narrow filenames. Final publication uses a native path,
  // but the intermediate SDK path must be checked rather than assuming TEMP
  // (which can itself contain a Unicode user name) is ASCII.
  std::error_code error;
  const auto temporary = std::filesystem::temp_directory_path(error);
  error.clear();
  const auto working = std::filesystem::current_path(error);
  static std::atomic_uint64_t sequence{0};
  for (const auto& base : {temporary, working}) {
    error.clear();
    if (base.empty() || !std::filesystem::is_directory(base, error) || error) continue;
    const auto absolute = std::filesystem::absolute(base, error).lexically_normal();
    if (error) continue;
    const auto text = absolute.u8string();
    if (std::any_of(text.begin(), text.end(), [](char8_t c) { return c > 127; })) continue;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
      const auto candidate = absolute / (".container-vdb-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++sequence));
      error.clear();
      if (std::filesystem::create_directory(candidate, error)) return candidate;
      if (error) break;
    }
  }
  throw std::runtime_error("OpenVDB requires an ASCII writable temporary directory; configure TEMP/TMP or use an ASCII working directory");
}

inline void RequireSeekableFile(const std::filesystem::path& path) {
  class Probe : public openvdb::io::Archive {
  public:
    bool offsets(std::istream& input) { readHeader(input); return inputHasGridOffsets(); }
  };
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::invalid_argument("could not open OpenVDB input");
  input.exceptions(std::ios::badbit | std::ios::failbit);
  Probe probe;
  if (!probe.offsets(input))
    throw std::invalid_argument("bounded processing requires random-access grid offsets; rewrite the streaming VDB as a seekable file");
  if (probe.fileVersion() < openvdb::OPENVDB_FILE_VERSION_GRID_INSTANCING ||
      probe.fileVersion() > openvdb::OPENVDB_FILE_VERSION)
    throw std::invalid_argument("bounded processing requires a supported VDB format with metadata-readable transforms; rewrite with a current OpenVDB version");
}

inline void WriteFileExclusive(const std::filesystem::path& output,
                               const openvdb::GridPtrVec& grids,
                               uintmax_t maxBytes = std::numeric_limits<uintmax_t>::max(),
                               std::optional<uint32_t> compression = {}) {
  if (output.empty()) throw std::invalid_argument("output path must not be empty");
  if (std::filesystem::exists(output)) throw std::invalid_argument("output already exists; choose a new .vdb path");
  const auto parent = output.has_parent_path() ? output.parent_path() : std::filesystem::path(".");
  if (!std::filesystem::is_directory(parent)) throw std::invalid_argument("output directory does not exist");
  const auto temporaryDirectory = CreateAsciiVdbTemporaryDirectory();
  const auto temporaryFile = temporaryDirectory / "data.vdb";
  std::ofstream destination;
  bool created = false;
  const auto cleanup = [&] {
    std::error_code ignored;
    std::filesystem::remove(temporaryFile, ignored);
    std::filesystem::remove(temporaryDirectory, ignored);
  };
  try {
    openvdb::io::File file(temporaryFile.string());
    if (compression) file.setCompression(*compression);
    file.write(grids);
    file.close();
    if (std::filesystem::file_size(temporaryFile) > maxBytes)
      throw std::invalid_argument("encoded VDB output exceeds the file-size budget");
    std::ifstream source(temporaryFile, std::ios::binary);
    if (!source) throw std::runtime_error("could not read temporary VDB output");
    source.exceptions(std::ios::badbit);
    destination.open(output, std::ios::binary | std::ios::out | std::ios::noreplace);
    created = destination.is_open();
    if (!created) throw std::runtime_error("could not exclusively create VDB output");
    destination.exceptions(std::ios::badbit | std::ios::failbit);
    std::array<char, 64 * 1024> buffer{};
    while (source.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || source.gcount())
      destination.write(buffer.data(), source.gcount());
    if (!source.eof()) throw std::runtime_error("failed while reading temporary VDB output");
    destination.flush();
    destination.close();
    source.close();
    cleanup();
  } catch (...) {
    destination.exceptions(std::ios::goodbit);
    if (destination.is_open()) destination.close();
    if (created) { std::error_code ignored; std::filesystem::remove(output, ignored); }
    cleanup();
    throw;
  }
}
} // namespace container::geometry::vdb::detail
