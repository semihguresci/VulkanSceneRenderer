#pragma once

#include <tiny_gltf.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace container::geometry::gltf::detail {

using Bytes = std::vector<unsigned char>;

[[noreturn]] inline void fail(std::string_view message) {
  throw std::runtime_error("Scene export: " + std::string(message));
}

inline Bytes readBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) fail("cannot read resource '" + path.string() + "'");
  const auto size = stream.tellg();
  if (size < 0 || static_cast<uintmax_t>(size) >
                      static_cast<uintmax_t>(std::numeric_limits<size_t>::max()))
    fail("resource size is invalid");
  Bytes bytes(static_cast<size_t>(size));
  stream.seekg(0);
  if (!bytes.empty())
    stream.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
  if (!stream) fail("cannot read complete resource '" + path.string() + "'");
  return bytes;
}

inline void validateView(const tinygltf::Model& model, int index) {
  if (index < 0 || static_cast<size_t>(index) >= model.bufferViews.size())
    fail("source image has an invalid buffer view");
  const auto& view = model.bufferViews[static_cast<size_t>(index)];
  if (view.buffer < 0 || static_cast<size_t>(view.buffer) >= model.buffers.size())
    fail("source buffer view has an invalid buffer");
  const auto& bytes = model.buffers[static_cast<size_t>(view.buffer)].data;
  if (view.byteOffset > bytes.size() ||
      view.byteLength > bytes.size() - view.byteOffset)
    fail("source buffer view exceeds its buffer");
}

}  // namespace container::geometry::gltf::detail
