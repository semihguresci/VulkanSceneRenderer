#include <Container/geometry/GltfSceneExport.h>
#include <Container/geometry/GltfMaterialVariants.h>
#include <Container/geometry/GltfMaterialExport.h>
#include <Container/geometry/GltfAnimation.h>
#include <Container/geometry/GltfInteractivity.h>
#include <Container/geometry/GltfDeformation.h>
#include <Container/geometry/GltfAccessor.h>
#include <Container/geometry/GltfInstancing.h>
#include <Container/geometry/GltfLinkedAssets.h>
#include <Container/geometry/GltfNodeExportCompaction.h>
#include <Container/geometry/GltfXmpMetadata.h>
#include <Container/utility/SourcePrimitiveMapping.h>

#include "GltfLinkedExportResources.h"
#include "GltfSkinExportAffine.h"
#include "GltfSkinExportAnimation.h"

#include <stb_image_write.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace container::geometry::gltf {
namespace {

using Bytes = std::vector<unsigned char>;

[[noreturn]] void fail(std::string_view message) {
  throw std::runtime_error("Scene export: " + std::string(message));
}

std::string lower(std::string value) {
  for (auto& c : value)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  return value;
}

std::string utf8Path(const std::filesystem::path& path) {
  const auto utf8 = path.u8string();
  return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

Bytes readBytes(const std::filesystem::path& path) {
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

Bytes uriBytes(const std::string& uri,
               const std::filesystem::path& sourcePath,
               std::string& mime) {
  Bytes bytes;
  if (uri.starts_with("data:")) {
    if (!tinygltf::DecodeDataURI(&bytes, mime, uri, 0, false))
      fail("cannot decode source data URI");
  } else {
    if (sourcePath.empty()) fail("external resource needs an original source path");
    std::string decoded;
    if (!tinygltf::URIDecode(uri, &decoded, nullptr))
      fail("cannot decode resource URI");
    if (decoded.find("://") != std::string::npos)
      fail("remote resource URIs must be imported locally before export");
    bytes = readBytes(sourcePath.parent_path() / std::filesystem::u8path(decoded));
  }
  return bytes;
}

void alignBuffer(Bytes& bytes, size_t alignment = 4) {
  while (bytes.size() % alignment != 0) bytes.push_back(0);
}

int appendView(tinygltf::Model& model, const Bytes& bytes, int target = 0) {
  auto& buffer = model.buffers.front().data;
  alignBuffer(buffer);
  tinygltf::BufferView view;
  view.buffer = 0;
  view.byteOffset = buffer.size();
  view.byteLength = bytes.size();
  view.target = target;
  buffer.insert(buffer.end(), bytes.begin(), bytes.end());
  model.bufferViews.push_back(std::move(view));
  return static_cast<int>(model.bufferViews.size() - 1);
}

template <typename T>
void appendScalar(Bytes& bytes, T value) {
  // glTF binary data is little endian, independent of host byte order.
  std::array<unsigned char, sizeof(T)> raw;
  std::memcpy(raw.data(), &value, sizeof(T));
  if constexpr (std::endian::native == std::endian::big)
    std::ranges::reverse(raw);
  bytes.insert(bytes.end(), raw.begin(), raw.end());
}

int appendAccessor(tinygltf::Model& model, Bytes bytes, size_t count,
                   int type, int componentType, int target) {
  tinygltf::Accessor accessor;
  accessor.bufferView = appendView(model, bytes, target);
  accessor.count = count;
  accessor.type = type;
  accessor.componentType = componentType;
  model.accessors.push_back(std::move(accessor));
  return static_cast<int>(model.accessors.size() - 1);
}

void validateView(const tinygltf::Model& model, int index) {
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

std::string imageMime(const Bytes& bytes, const std::string& declared) {
  if (!declared.empty()) return declared;
  if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' &&
      bytes[2] == 'N' && bytes[3] == 'G')
    return "image/png";
  if (bytes.size() >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff)
    return "image/jpeg";
  if (bytes.size() >= 6 && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F')
    return "image/gif";
  if (bytes.size() >= 2 && bytes[0] == 'B' && bytes[1] == 'M')
    return "image/bmp";
  if (bytes.size() >= 4 && bytes[0] == 'D' && bytes[1] == 'D' && bytes[2] == 'S' && bytes[3] == ' ')
    return "image/vnd-ms.dds";
  constexpr unsigned char ktx1[]{0xab,0x4b,0x54,0x58,0x20,0x31,0x31,0xbb,0x0d,0x0a,0x1a,0x0a};
  if (bytes.size() >= sizeof(ktx1) && std::equal(std::begin(ktx1),std::end(ktx1),bytes.begin()))
    return "image/ktx";
  for (const std::string_view signature : {std::string_view("#?RADIANCE"),std::string_view("#?RGBE")})
    if (bytes.size()>=signature.size() && std::equal(signature.begin(),signature.end(),bytes.begin()))
      return "image/vnd.radiance";
  if (bytes.size() >= 12 && bytes[0] == 'R' && bytes[1] == 'I' &&
      bytes[2] == 'F' && bytes[3] == 'F' && bytes[8] == 'W' &&
      bytes[9] == 'E' && bytes[10] == 'B' && bytes[11] == 'P')
    return "image/webp";
  constexpr std::array<unsigned char, 12> ktx2{
      0xab, 'K', 'T', 'X', ' ', '2', '0', 0xbb, 0x0d, 0x0a, 0x1a, 0x0a};
  if (bytes.size() >= ktx2.size() && std::equal(ktx2.begin(), ktx2.end(), bytes.begin()))
    return "image/ktx2";
  if (bytes.size() >= 12 && bytes[4] == 'f' && bytes[5] == 't' &&
      bytes[6] == 'y' && bytes[7] == 'p' && bytes[8] == 'a' &&
      bytes[9] == 'v' && bytes[10] == 'i' && (bytes[11] == 'f' || bytes[11] == 's'))
    return "image/avif";
  fail("cannot determine source image MIME type");
}

Bytes encodeWidePng(const tinygltf::Image& image) {
  // PNG stores 16-bit channels in network byte order. A zlib stream of stored
  // DEFLATE blocks avoids introducing an optional codec dependency at export.
  constexpr size_t maxImageBytes = 256u * 1024u * 1024u;
  const size_t rowBytes = size_t(image.width) * size_t(image.component) * 2;
  if (rowBytes >= maxImageBytes || size_t(image.height) > maxImageBytes / (rowBytes + 1))
    fail("16-bit PNG export exceeds the encoded image byte limit");
  const size_t rawSize = (rowBytes + 1) * size_t(image.height);
  const size_t blocks = (rawSize + 65534) / 65535;
  if (rawSize > maxImageBytes || blocks * 5 + rawSize + 63 > maxImageBytes)
    fail("16-bit PNG export exceeds the encoded image byte limit");
  Bytes raw(rawSize);
  for (int y = 0; y < image.height; ++y) {
    const size_t row = size_t(y) * (rowBytes + 1);
    raw[row] = 0; // PNG filter None.
    for (size_t x = 0; x < rowBytes; x += 2) {
      uint16_t word;
      std::memcpy(&word, image.image.data() + size_t(y) * rowBytes + x, 2);
      raw[row + 1 + x] = static_cast<unsigned char>(word >> 8);
      raw[row + 2 + x] = static_cast<unsigned char>(word);
    }
  }
  Bytes zlib{0x78, 0x01};
  zlib.reserve(rawSize + blocks * 5 + 6);
  uint32_t s1 = 1, s2 = 0;
  for (size_t at = 0; at < raw.size();) {
    const auto count = static_cast<uint16_t>(std::min(size_t{65535}, raw.size() - at));
    zlib.push_back(at + count == raw.size() ? 1 : 0);
    zlib.push_back(static_cast<unsigned char>(count)); zlib.push_back(static_cast<unsigned char>(count >> 8));
    zlib.push_back(static_cast<unsigned char>(~count)); zlib.push_back(static_cast<unsigned char>((~count) >> 8));
    zlib.insert(zlib.end(), raw.begin() + at, raw.begin() + at + count);
    for (size_t i = at; i < at + count; ++i) { s1 = (s1 + raw[i]) % 65521; s2 = (s2 + s1) % 65521; }
    at += count;
  }
  const auto be32 = [](Bytes& output, uint32_t word) {
    for (int shift = 24; shift >= 0; shift -= 8) output.push_back(static_cast<unsigned char>(word >> shift));
  };
  be32(zlib, (s2 << 16) | s1);
  Bytes output{137,80,78,71,13,10,26,10};
  const auto chunk = [&](std::string_view type, const Bytes& payload) {
    be32(output, static_cast<uint32_t>(payload.size()));
    const auto start = output.size();
    output.insert(output.end(), type.begin(), type.end());
    output.insert(output.end(), payload.begin(), payload.end());
    uint32_t crc = UINT32_MAX;
    for (size_t i = start; i < output.size(); ++i) {
      crc ^= output[i];
      for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    be32(output, ~crc);
  };
  Bytes header; be32(header, uint32_t(image.width)); be32(header, uint32_t(image.height));
  header.insert(header.end(), {16, static_cast<unsigned char>(image.component == 1 ? 0 : image.component == 2 ? 4 : image.component == 3 ? 2 : 6), 0, 0, 0});
  chunk("IHDR", header); chunk("IDAT", zlib); chunk("IEND", {});
  return output;
}

Bytes encodeImage(const tinygltf::Image& image) {
  if (image.as_is) return image.image;
  if (image.width <= 0 || image.height <= 0 || image.component < 1 ||
      image.component > 4 || (image.bits != 8 && image.bits != 16) ||
      static_cast<size_t>(image.width) > std::numeric_limits<size_t>::max() /
                                            static_cast<size_t>(image.height) /
                                            static_cast<size_t>(image.component) / size_t(image.bits / 8))
    fail("image has no usable encoded source or 8/16-bit CPU pixels");
  const auto required = static_cast<size_t>(image.width) *
                        static_cast<size_t>(image.height) *
                        static_cast<size_t>(image.component) * size_t(image.bits / 8);
  if (image.image.size() < required) fail("source image pixel data is incomplete");
  if (image.bits == 16) {
    if (image.pixel_type != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
      fail("16-bit PNG export requires unsigned normalized CPU pixels");
    return encodeWidePng(image);
  }
  Bytes bytes;
  auto append = [](void* context, void* data, int size) {
    auto& output = *static_cast<Bytes*>(context);
    const auto* begin = static_cast<const unsigned char*>(data);
    output.insert(output.end(), begin, begin + size);
  };
  if (!stbi_write_png_to_func(append, &bytes, image.width, image.height,
                              image.component, image.image.data(), 0))
    fail("cannot encode source image pixels");
  return bytes;
}

void packBuffers(tinygltf::Model& model,
                    const std::filesystem::path& sourcePath) {
  Bytes packed;
  std::vector<size_t> baseOffsets;
  for (auto& buffer : model.buffers) {
    if (buffer.data.empty() && !buffer.uri.empty()) {
      std::string mime;
      buffer.data = uriBytes(buffer.uri, sourcePath, mime);
    }
    // Retained source accessors can include experimental binary64 streams.
    // Preserve their alignment when concatenating independently aligned buffers.
    alignBuffer(packed, 8);
    baseOffsets.push_back(packed.size());
    packed.insert(packed.end(), buffer.data.begin(), buffer.data.end());
  }
  for (size_t i = 0; i < model.bufferViews.size(); ++i) {
    validateView(model, static_cast<int>(i));
    auto& view = model.bufferViews[i];
    view.byteOffset += baseOffsets[static_cast<size_t>(view.buffer)];
    view.buffer = 0;
  }
  tinygltf::Buffer buffer;
  if (!model.buffers.empty()) {
    buffer.name = model.buffers.front().name;
    buffer.extras = model.buffers.front().extras;
    buffer.extensions = model.buffers.front().extensions;
  }
  buffer.data = std::move(packed);
  model.buffers = {std::move(buffer)};
}
void packImagesAndAudio(tinygltf::Model& model,const std::filesystem::path& sourcePath) {
  for (auto& image : model.images) {
    if (image.bufferView >= 0 && image.uri.empty()) {
      validateView(model, image.bufferView);
      const auto& view = model.bufferViews[static_cast<size_t>(image.bufferView)];
      const auto& data = model.buffers.front().data;
      if (image.mimeType.empty()) {
        const Bytes bytes(data.begin() + view.byteOffset,
                          data.begin() + view.byteOffset + view.byteLength);
        image.mimeType = imageMime(bytes, {});
      }
    } else {
      std::string mime = image.mimeType;
      Bytes bytes;
      if (image.as_is && !image.image.empty())
        bytes = image.image;
      else if (!image.uri.empty())
        bytes = uriBytes(image.uri, sourcePath, mime);
      else {
        bytes = encodeImage(image);
        if (!image.as_is) mime = "image/png";
      }
      if (bytes.empty()) fail("source image resource is empty");
      image.mimeType = imageMime(bytes, mime);
      image.bufferView = appendView(model, bytes);
    }
    image.uri.clear();
    image.image.clear();
    image.as_is = true;
  }
  // Audio is optional and currently unsupported by the renderer, but a source
  // definition can still be carried portably without changing its indices.
  for (auto& audio : model.audioSources) {
    if (audio.uri.empty()) {
      validateView(model, audio.bufferView);
    } else {
      std::string mime = audio.mimeType;
      auto bytes = uriBytes(audio.uri, sourcePath, mime);
      if (mime.empty()) fail("external audio needs an explicit MIME type");
      audio.bufferView = appendView(model, bytes);
      audio.mimeType = std::move(mime);
      audio.uri.clear();
    }
  }
}
void packResources(tinygltf::Model& model,const std::filesystem::path& sourcePath) {
  packBuffers(model,sourcePath);packImagesAndAudio(model,sourcePath);
}

void validateGraph(const scene::SceneGraph& graph) {
  if (graph.nodeCount() > static_cast<size_t>(std::numeric_limits<int>::max()))
    fail("scene has too many nodes");
  std::vector<unsigned char> incoming(graph.nodeCount(), 0);
  for (const auto root : graph.rootNodes()) {
    if (root >= graph.nodeCount() || incoming[root]++ != 0 ||
        graph.getNode(root)->parent != scene::SceneGraph::kInvalidNode)
      fail("scene root topology is invalid");
  }
  for (uint32_t i = 0; i < graph.nodeCount(); ++i) {
    const auto& node = *graph.getNode(i);
    for (const auto child : node.children)
      if (child >= graph.nodeCount() || incoming[child]++ != 0 ||
          graph.getNode(child)->parent != i)
        fail("scene child topology is invalid");
    for (int col = 0; col < 4; ++col)
      for (int row = 0; row < 4; ++row)
        if (!std::isfinite(node.localTransform[col][row]))
          fail("node transform contains non-finite values");
  }
  if (std::ranges::any_of(incoming, [](auto count) { return count != 1; }))
    fail("scene has an unattached node");
  // Counting incoming references alone cannot reject a disconnected cycle.
  std::vector<unsigned char> seen(graph.nodeCount(), 0);
  auto stack = graph.rootNodes();
  size_t visited = 0;
  while (!stack.empty()) {
    const auto i = stack.back();
    stack.pop_back();
    if (seen[i]++) fail("scene has a cycle");
    ++visited;
    const auto& children = graph.getNode(i)->children;
    stack.insert(stack.end(), children.begin(), children.end());
  }
  if (visited != graph.nodeCount()) fail("scene has a disconnected cycle");
}

bool matrixFinite(const glm::dmat4& matrix) {
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      if (!std::isfinite(matrix[c][r]) ||
          std::abs(matrix[c][r]) > std::numeric_limits<float>::max()) return false;
  return true;
}

glm::dmat4 inverseAffine(const glm::dmat4& matrix) {
  const auto inverseLinear = glm::inverse(glm::dmat3(matrix));
  glm::dmat4 inverse(inverseLinear);
  inverse[3] = glm::dvec4(-inverseLinear * glm::dvec3(matrix[3]), 1);
  return inverse;
}

glm::dmat4 sourceTransform(const tinygltf::Node& node) {
  glm::dmat4 matrix(1);
  if (!node.matrix.empty()) {
    if (node.matrix.size() != 16) fail("source node matrix has an invalid size");
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r) matrix[c][r] = node.matrix[size_t(c * 4 + r)];
  } else {
    glm::dvec3 translation(0), scale(1);
    glm::dquat rotation(1, 0, 0, 0);
    if (!node.translation.empty()) {
      if (node.translation.size() != 3) fail("source translation has an invalid size");
      translation = {node.translation[0], node.translation[1], node.translation[2]};
    }
    if (!node.rotation.empty()) {
      if (node.rotation.size() != 4) fail("source rotation has an invalid size");
      rotation = glm::dquat(node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2]);
      const auto length = glm::length(rotation);
      if (!std::isfinite(length) || length < 1e-15) fail("source rotation is invalid");
      rotation /= length;
    }
    if (!node.scale.empty()) {
      if (node.scale.size() != 3) fail("source scale has an invalid size");
      scale = {node.scale[0], node.scale[1], node.scale[2]};
    }
    matrix = glm::translate(glm::dmat4(1), translation) * glm::mat4_cast(rotation) *
             glm::scale(glm::dmat4(1), scale);
  }
  if (!matrixFinite(matrix)) fail("source node transform is outside the finite runtime range");
  return matrix;
}

bool matrixNear(const glm::dmat4& a, const glm::dmat4& b) {
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      if (std::abs(a[c][r] - b[c][r]) > 2e-6 *
          std::max({1.0, std::abs(a[c][r]), std::abs(b[c][r])})) return false;
  return true;
}

struct ScenePlacement {
  glm::dmat4 prefix{1};
  bool visible{true};
  bool selectable{true};
  bool hoverable{true};
  int entry{-1};
};

bool needsScenePlacement(const ScenePlacement& placement) {
  if (!placement.visible || !placement.selectable || !placement.hoverable) return true;
  for (int c=0;c<4;++c) for (int r=0;r<4;++r)
    if (placement.prefix[c][r] != (c == r ? 1.0 : 0.0)) return true;
  return false;
}

std::map<uint32_t, ScenePlacement> validateSceneRoots(const SceneExportSnapshot& snapshot) {
  std::map<uint32_t, ScenePlacement> roots;
  if (snapshot.runtimeSceneRoots.empty()) return roots;
  constexpr auto limit = size_t(std::numeric_limits<int>::max());
  if (snapshot.runtimeSceneRoots.size() > limit || snapshot.defaultSceneIndex < 0 ||
      size_t(snapshot.defaultSceneIndex) >= snapshot.runtimeSceneRoots.size())
    fail("export scene default index is invalid");
  if (snapshot.source && !snapshot.source->scenes.empty() &&
      snapshot.runtimeSceneRoots.size() != snapshot.source->scenes.size())
    fail("runtime scene roots must cover every source scene");
  size_t references{};
  for (const auto& sceneRoots : snapshot.runtimeSceneRoots) {
    if (sceneRoots.size() > limit - references) fail("too many exported scene root references");
    references += sceneRoots.size();
    std::set<uint32_t> unique;
    for (const auto root : sceneRoots) {
      if (root >= snapshot.graph.nodeCount()) fail("export scene root index is invalid");
      if (!unique.insert(root).second) fail("export scene repeats a root node");
      roots.try_emplace(root);
    }
  }
  // Cache shared synthetic ancestry so scene/reference count cannot multiply
  // the cost of a long common import-prefix chain.
  std::map<uint32_t, ScenePlacement> prefixes;
  size_t wrappers{};
  for (auto& [root, placement] : roots) {
    auto ancestor = snapshot.graph.getNode(root)->parent;
    std::vector<uint32_t> chain;
    while (ancestor != scene::SceneGraph::kInvalidNode && !prefixes.contains(ancestor)) {
      const auto& node = *snapshot.graph.getNode(ancestor);
      const int source = snapshot.sourceNodeIndices.empty() ? node.sourceNodeIndex :
          snapshot.sourceNodeIndices[ancestor];
      if (source >= 0 || node.renderable || roots.contains(ancestor))
        fail("export scene root must be below only synthetic editor/import ancestors");
      chain.push_back(ancestor);
      ancestor = node.parent;
    }
    ScenePlacement prefix;
    if (ancestor != scene::SceneGraph::kInvalidNode) prefix = prefixes.at(ancestor);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      const auto& node = *snapshot.graph.getNode(*it);
      prefix.prefix *= glm::dmat4(node.localTransform);
      prefix.visible &= node.visible;
      prefix.selectable &= node.selectable;
      prefix.hoverable &= node.hoverable;
      if (!matrixFinite(prefix.prefix)) fail("export scene prefix transform is outside the finite runtime range");
      prefixes.emplace(*it, prefix);
    }
    placement = prefix;
    if (needsScenePlacement(prefix)) ++wrappers;
  }
  if (wrappers > limit - snapshot.graph.nodeCount()) fail("too many scene placement nodes");
  return roots;
}

void setLocalNodeFlag(tinygltf::Node& node, const char* extension,
                      const char* property, bool value) {
  auto found = node.extensions.find(extension);
  if (found == node.extensions.end()) {
    if (value) return; // Unauthored true is the schema default.
    found = node.extensions.emplace(extension, tinygltf::Value(tinygltf::Value::Object{})).first;
  }
  if (!found->second.IsObject()) fail(std::string(extension) + " must be an object");
  auto& object = found->second.Get<tinygltf::Value::Object>();
  const auto authored = object.find(property);
  if (authored != object.end() && !authored->second.IsBool())
    fail(std::string(extension) + "." + property + " must be a boolean");
  // Retain an absent defaulted property, explicit true, extras and unknown
  // metadata. Editing a local value changes only its known boolean property.
  if (!value || authored != object.end()) object[property] = tinygltf::Value(value);
}

void setMatrix(tinygltf::Node& node, const glm::dmat4& matrix) {
  if (!matrixFinite(matrix)) fail("exported node matrix is outside the finite runtime range");
  node.translation.clear(); node.rotation.clear(); node.scale.clear();
  node.matrix.clear();
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) node.matrix.push_back(matrix[c][r]);
}

std::vector<std::string> pointerTokens(const std::string& pointer) {
  if (pointer.empty() || pointer.front() != '/') fail("animation pointer must be absolute");
  std::vector<std::string> tokens;
  size_t begin = 1;
  while (begin <= pointer.size()) {
    const auto end = pointer.find('/', begin);
    const auto encoded = pointer.substr(begin, end == std::string::npos ? end : end - begin);
    std::string decoded;
    for (size_t i = 0; i < encoded.size(); ++i) {
      if (encoded[i] != '~') decoded += encoded[i];
      else {
        if (++i == encoded.size() || (encoded[i] != '0' && encoded[i] != '1'))
          fail("animation pointer has an invalid RFC 6901 escape");
        decoded += encoded[i] == '0' ? '~' : '/';
      }
    }
    tokens.push_back(std::move(decoded));
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return tokens;
}

int pointerIndex(const std::string& token) {
  int result = -1;
  const auto parsed = std::from_chars(token.data(), token.data() + token.size(), result);
  if (token.empty() || (token.size() > 1 && token.front() == '0') || result < 0 ||
      parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size())
    fail("animation pointer has an invalid object index");
  return result;
}

enum class TargetTable { Node, Material, Camera, Light };
struct ChannelTarget {
  TargetTable table{TargetTable::Node};
  int index{-1};
  std::string property;
  std::vector<std::string> tokens;
  size_t indexToken{1};
};

ChannelTarget channelTarget(const tinygltf::AnimationChannel& channel) {
  ChannelTarget target;
  if (channel.target_path != "pointer") {
    if (channel.target_path != "translation" && channel.target_path != "rotation" &&
        channel.target_path != "scale" && channel.target_path != "weights")
      fail("animation has an unsupported standard target path");
    if (channel.target_node < 0) fail("animation has no target node");
    target.index = channel.target_node;
    target.property = channel.target_path;
    return target;
  }
  const auto extension = channel.target_extensions.find("KHR_animation_pointer");
  if (extension == channel.target_extensions.end() || !extension->second.IsObject() ||
      !extension->second.Get("pointer").IsString()) fail("animation pointer is missing");
  target.tokens = pointerTokens(extension->second.Get("pointer").Get<std::string>());
  if (target.tokens.size() >= 3 && target.tokens[0] == "nodes") target.table = TargetTable::Node;
  else if (target.tokens.size() >= 3 && target.tokens[0] == "materials") target.table = TargetTable::Material;
  else if (target.tokens.size() >= 3 && target.tokens[0] == "cameras") target.table = TargetTable::Camera;
  else if (target.tokens.size() >= 5 && target.tokens[0] == "extensions" &&
           target.tokens[1] == "KHR_lights_punctual" && target.tokens[2] == "lights") {
    target.table = TargetTable::Light;
    target.indexToken = 3;
  } else fail("animation pointer root cannot be remapped safely");
  target.index = pointerIndex(target.tokens[target.indexToken]);
  target.property = target.tokens[target.indexToken + 1];
  return target;
}

void retargetChannel(tinygltf::AnimationChannel& channel, ChannelTarget target, int index) {
  if (target.tokens.empty()) { channel.target_node = index; return; }
  target.tokens[target.indexToken] = std::to_string(index);
  std::string pointer;
  for (const auto& token : target.tokens) {
    pointer += '/';
    for (const auto c : token) {
      if (c == '~') pointer += "~0";
      else if (c == '/') pointer += "~1";
      else pointer += c;
    }
  }
  auto& extension = channel.target_extensions["KHR_animation_pointer"].Get<tinygltf::Value::Object>();
  extension["pointer"] = tinygltf::Value(std::move(pointer));
  channel.target_extensions_json_string.clear();
}

bool isTransformTarget(const ChannelTarget& target) {
  return target.table == TargetTable::Node &&
      (target.property == "translation" || target.property == "rotation" || target.property == "scale");
}

struct PostSkinExport {
  int primitive;
  int owner;
};

constexpr auto kPostSkinLimits = GltfResourceExportCompactionLimits{};
constexpr size_t kMaxPostSkinEntries = kPostSkinLimits.maxTableEntries;

int appendSkinNode(tinygltf::Model& model, int parent, tinygltf::Node node) {
  if (model.nodes.size() >= kMaxPostSkinEntries)
    fail("edited skin hierarchy exceeds the export node capacity");
  const int index = static_cast<int>(model.nodes.size());
  model.nodes.push_back(std::move(node));
  if (parent >= 0) model.nodes[size_t(parent)].children.push_back(index);
  return index;
}

std::array<glm::dmat4, 2> skinTransformFactors(const glm::dmat4& transform) {
  const auto factors = detail::factorSkinExportAffine(transform);
  if (!factors) fail("edited skin transform cannot be expressed as finite glTF TRS nodes");
  return *factors;
}

glm::dmat4 skinAffineTransform(const tinygltf::Node& node) {
  auto transform = sourceTransform(node);
  // The editor uses a binary32 inverse to preserve world placement during
  // reparenting. Its homogeneous 1 can acquire roundoff even on affine input.
  if (transform[0].w != 0 || transform[1].w != 0 || transform[2].w != 0 ||
      std::abs(transform[3].w - 1) > 8 * std::numeric_limits<float>::epsilon())
    fail("edited skin transform must be affine");
  transform[3].w = 1;
  return transform;
}

int appendSkinTransform(tinygltf::Model& model, int parent, tinygltf::Node node,
                        const glm::dmat4& transform) {
  const auto [first, second] = skinTransformFactors(transform);
  if (second == glm::dmat4(1)) {
    if (first != glm::dmat4(1)) setMatrix(node, first);
  } else {
    tinygltf::Node wrapper;
    wrapper.name = node.name + " basis";
    setMatrix(wrapper, first);
    parent = appendSkinNode(model, parent, std::move(wrapper));
    setMatrix(node, second);
  }
  return appendSkinNode(model, parent, std::move(node));
}

int appendInverseSkinSampler(tinygltf::Model& model, tinygltf::Animation& animation,
                             int samplerIndex, const ChannelTarget& target, size_t& remainingBytes) {
  auto sampler = animation.samplers.at(size_t(samplerIndex));
  const CheckedAccessor reader(model, sampler.output, "edited skin animation",
      {AccessorUsage::Generic, HasRequiredExtension(model, "KHR_accessor_float64")});
  const auto count = reader.count();
  const auto components = reader.components();
  if (count > kMaxPostSkinEntries)
    fail("edited skin animation exceeds the export sample capacity");
  const auto type = reader.accessor().type;
  const bool isDouble = reader.accessor().componentType == TINYGLTF_COMPONENT_TYPE_DOUBLE;
  const size_t byteCount = count * components * (isDouble ? sizeof(double) : sizeof(float));
  if (byteCount > remainingBytes) fail("edited skin animations exceed the export payload capacity");
  remainingBytes -= byteCount;
  std::vector<double> values(count * components);
  for (size_t i = 0; i < count; ++i)
    reader.readDoubles(i, std::span(values).subspan(i * components, components));
  const auto component = target.tokens.size() > target.indexToken + 2
      ? std::optional<size_t>(pointerIndex(target.tokens[target.indexToken + 2])) : std::nullopt;
  const auto inverse = detail::inverseSkinExportAnimationSamples(
      std::move(values), target.property,
      sampler.interpolation.empty() ? "LINEAR" : sampler.interpolation, component);
  if (!inverse)
    fail("edited skin owner has a singular or continuously varying scale that cannot be inverted in glTF");
  if (model.accessors.size() >= kMaxPostSkinEntries || model.bufferViews.size() >= kMaxPostSkinEntries ||
      animation.samplers.size() >= kMaxPostSkinEntries)
    fail("edited skin animation exceeds the export sampler capacity");
  Bytes bytes;
  bytes.reserve(byteCount);
  for (const auto value : *inverse) {
    if (isDouble) appendScalar(bytes, value);
    else {
      if (std::abs(value) > std::numeric_limits<float>::max() || (value != 0 && float(value) == 0))
        fail("edited skin inverse animation is outside the glTF FLOAT range");
      appendScalar(bytes, float(value));
    }
  }
  if (isDouble) alignBuffer(model.buffers.front().data, alignof(double));
  sampler.output = appendAccessor(model, std::move(bytes), count, type,
      isDouble ? TINYGLTF_COMPONENT_TYPE_DOUBLE : TINYGLTF_COMPONENT_TYPE_FLOAT, 0);
  const int result = static_cast<int>(animation.samplers.size());
  animation.samplers.push_back(std::move(sampler));
  return result;
}

// A skinned glTF mesh is placed by the joints. Give an edited primitive its own
// skeleton with joint worlds P * inverse(O) * J, where P is the primitive world
// transform and O is its original owner world transform.
void reconcileDeformationTransforms(tinygltf::Model& model, std::span<const PostSkinExport> edits,
                                    const std::set<int>& interactiveTransforms,
                                    std::map<int, std::vector<int>>& morphNodes,
                                    const SceneExportSnapshot& snapshot) {
  std::set<int> animated;
  std::set<int> weights;
  for (const auto& animation : model.animations)
    for (const auto& channel : animation.channels) {
      const auto target = channelTarget(channel);
      if (isTransformTarget(target)) animated.insert(target.index);
      if (target.table == TargetTable::Node && target.property == "weights" &&
          !snapshot.graph.isNodeRetired(uint32_t(target.index)) &&
          (snapshot.sourceResourceMasks.nodes.empty() || snapshot.sourceResourceMasks.nodes[size_t(target.index)] != 0))
        weights.insert(target.index);
    }
  if (edits.empty() && weights.empty()) return;
  if (model.nodes.size() >= kMaxPostSkinEntries)
    fail("deformation hierarchy exceeds the export node capacity");
  std::vector<int> parents(model.nodes.size(), -1);
  for (size_t i = 0; i < parents.size(); ++i)
    for (const int child : model.nodes[i].children) parents.at(size_t(child)) = static_cast<int>(i);

  std::map<int, std::vector<int>> forwardTargets;
  std::map<int, std::vector<std::array<int, 3>>> inverseTargets;
  std::map<int, int> meshTargets;
  std::set<int> matrixNodes;
  size_t remainingSteps = kPostSkinLimits.maxTraversalSteps;
  const auto parentOf = [&](int node) {
    if (remainingSteps == 0) fail("edited skin hierarchy exceeds the export traversal capacity");
    --remainingSteps;
    return parents[size_t(node)];
  };
  const auto markPath = [&](int node) {
    for (; node >= 0 && matrixNodes.insert(node).second; node = parentOf(node)) {}
  };
  const auto checkClone = [&](int source) {
    if (interactiveTransforms.contains(source))
      fail("edited skin cannot duplicate a transform written by KHR_interactivity");
  };
  const auto transformNode = [&](int source, std::string_view suffix) {
    tinygltf::Node node;
    node.name = model.nodes[size_t(source)].name + std::string(suffix);
    return node;
  };

  for (const auto& [primitive, owner] : edits) {
    auto& carrier = model.nodes[size_t(primitive)];
    auto skin = model.skins.at(size_t(carrier.skin));
    glm::dmat4 world(1);
    for (int source = primitive; source >= 0; source = parentOf(source))
      world = skinAffineTransform(model.nodes[size_t(source)]) * world;
    if (!matrixFinite(world) || !matrixFinite(inverseAffine(glm::dmat4(glm::mat4(world)))))
      fail("edited skinned primitive has a singular or unrepresentable world transform");
    tinygltf::Node mesh;
    mesh.name = carrier.name + " skinned mesh";
    mesh.mesh = std::exchange(carrier.mesh, -1);
    carrier.skin = -1;
    mesh.weights = std::exchange(carrier.weights, {});
    if (auto instance = carrier.extensions.extract("EXT_mesh_gpu_instancing"); !instance.empty()) {
      auto& attributes = instance.mapped().Get<tinygltf::Value::Object>()
          .at("attributes").Get<tinygltf::Value::Object>();
      for (const auto* semantic : {"TRANSLATION", "ROTATION", "SCALE"}) attributes.erase(semantic);
      if (!attributes.empty()) mesh.extensions.insert(std::move(instance));
    }
    if (model.skins.size() >= kMaxPostSkinEntries)
      fail("edited skins exceed the export skin capacity");
    mesh.skin = static_cast<int>(model.skins.size());
    meshTargets.emplace(primitive, appendSkinNode(model, primitive, std::move(mesh)));
    const int root = appendSkinNode(model, primitive, transformNode(primitive, " skin rig"));
    markPath(primitive);
    markPath(owner);

    // Cancel the common world prefix before generating inverses. In particular,
    // animation on shared rig ancestors requires no reciprocal scale channels.
    std::map<int, size_t> ownerPath;
    for (int node = owner; node >= 0; node = parentOf(node))
      ownerPath.emplace(node, ownerPath.size());
    int common = owner;
    for (const int joint : skin.joints) {
      markPath(joint);
      int ancestor = joint;
      while (ancestor >= 0 && !ownerPath.contains(ancestor)) ancestor = parentOf(ancestor);
      if (ancestor < 0 || (common >= 0 && ownerPath.at(ancestor) > ownerPath.at(common))) common = ancestor;
    }

    int parent = root;
    for (int source = owner; source != common; source = parentOf(source)) {
      checkClone(source);
      const auto& original = model.nodes[size_t(source)];
      if (!animated.contains(source)) {
        const auto local = skinAffineTransform(original);
        parent = appendSkinTransform(model, parent, transformNode(source, " inverse"), inverseAffine(local));
        continue;
      }

      // Inverse(T * R * S) = inverse(S) * inverse(R) * inverse(T). Keeping
      // each property on its own node preserves its source interpolation.
      auto scale = transformNode(source, " inverse scale");
      auto rotation = transformNode(source, " inverse rotation");
      auto translation = transformNode(source, " inverse translation");
      scale.scale = original.scale.empty() ? std::vector<double>{1, 1, 1} : original.scale;
      rotation.rotation = original.rotation.empty() ? std::vector<double>{0, 0, 0, 1} : original.rotation;
      translation.translation = original.translation.empty() ? std::vector<double>{0, 0, 0} : original.translation;
      for (auto& value : scale.scale) {
        value = 1.0 / value;
        if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
          fail("edited skin owner has a singular or unrepresentable scale");
      }
      for (size_t i = 0; i < 3; ++i) {
        rotation.rotation[i] = -rotation.rotation[i];
        translation.translation[i] = -translation.translation[i];
      }
      const int s = appendSkinNode(model, parent, std::move(scale));
      const int r = appendSkinNode(model, s, std::move(rotation));
      const int t = appendSkinNode(model, r, std::move(translation));
      inverseTargets[source].push_back({t, r, s});
      parent = t;
    }

    std::map<int, int> clones{{common, parent}};
    for (auto& joint : skin.joints) {
      std::vector<int> path;
      for (int source = joint; !clones.contains(source); source = parentOf(source))
        path.push_back(source);
      for (const int source : path | std::views::reverse) {
        checkClone(source);
        const int cloneParent = clones.at(parentOf(source));
        const auto& original = model.nodes[size_t(source)];
        auto node = transformNode(source, " skin joint");
        int clone;
        if (animated.contains(source)) {
          node.translation = original.translation;
          node.rotation = original.rotation;
          node.scale = original.scale;
          clone = appendSkinNode(model, cloneParent, std::move(node));
          forwardTargets[source].push_back(clone);
        } else {
          const auto transform = skinAffineTransform(original);
          clone = appendSkinTransform(model, cloneParent, std::move(node), transform);
        }
        clones.emplace(source, clone);
      }
      joint = clones.at(joint);
    }
    const auto skeleton = clones.find(skin.skeleton);
    skin.skeleton = skin.skeleton >= 0 && skeleton != clones.end() ? skeleton->second : root;
    model.skins.push_back(std::move(skin));
  }

  // The same placement hierarchy must be portable even when an editor affine
  // has shear or the source node is itself a weights-animation target.
  matrixNodes.insert(weights.begin(), weights.end());
  for (const int source : matrixNodes) {
    auto& node = model.nodes[size_t(source)];
    if (node.matrix.empty()) continue;
    const auto [first, second] = skinTransformFactors(skinAffineTransform(node));
    const bool weightTarget = weights.contains(source) && !meshTargets.contains(source);
    if (second == glm::dmat4(1) && !weightTarget) {
      node.matrix.back() = 1;
      continue;
    }
    tinygltf::Node wrapper = transformNode(source, " transform basis");
    wrapper.children = {source};
    if (weightTarget) node.matrix.clear();
    else setMatrix(node, second);
    if (first == glm::dmat4(1) && second == glm::dmat4(1)) continue;
    setMatrix(wrapper, first);
    const int entry = appendSkinNode(model, -1, std::move(wrapper));
    const int parent = parentOf(source);
    if (parent >= 0) std::ranges::replace(model.nodes[size_t(parent)].children, source, entry);
    else for (auto& scene : model.scenes) std::ranges::replace(scene.nodes, source, entry);
    if (weightTarget && second != glm::dmat4(1)) {
      tinygltf::Node remainder = transformNode(source, " transform rotation");
      remainder.children = {source};
      setMatrix(remainder, second);
      model.nodes[size_t(entry)].children.front() = appendSkinNode(model, -1, std::move(remainder));
    }
  }

  if (edits.empty()) return;
  for (auto& [source, destinations] : morphNodes)
    for (auto& destination : destinations)
      if (const auto mesh = meshTargets.find(destination); mesh != meshTargets.end()) destination = mesh->second;

  size_t remainingBytes = kPostSkinLimits.maxPayloadBytes;
  for (auto& animation : model.animations) {
    const size_t originalCount = animation.channels.size();
    std::map<std::pair<int, std::string>, int> inverseSamplers;
    for (size_t i = 0; i < originalCount; ++i) {
      const auto channel = animation.channels[i];
      const auto target = channelTarget(channel);
      if (target.table != TargetTable::Node) continue;
      const auto appendChannel = [&](int node, int sampler) {
        if (animation.channels.size() >= kMaxPostSkinEntries)
          fail("edited skin animation exceeds the export channel capacity");
        auto copy = channel;
        copy.sampler = sampler;
        retargetChannel(copy, target, node);
        animation.channels.push_back(std::move(copy));
      };
      if (target.property == "weights") {
        if (const auto mesh = meshTargets.find(target.index); mesh != meshTargets.end())
          retargetChannel(animation.channels[i], target, mesh->second);
      } else if (isTransformTarget(target)) {
        if (const auto forward = forwardTargets.find(target.index); forward != forwardTargets.end())
          for (const int node : forward->second) appendChannel(node, channel.sampler);
        if (const auto inverse = inverseTargets.find(target.index); inverse != inverseTargets.end()) {
          auto key = target.property;
          if (target.tokens.size() > target.indexToken + 2) key += "/" + target.tokens[target.indexToken + 2];
          auto [sampler, inserted] = inverseSamplers.try_emplace({channel.sampler, std::move(key)}, -1);
          if (inserted)
            sampler->second = appendInverseSkinSampler(model, animation, channel.sampler, target, remainingBytes);
          const size_t slot = target.property == "translation" ? 0 : target.property == "rotation" ? 1 : 2;
          for (const auto& nodes : inverse->second) appendChannel(nodes[slot], sampler->second);
        }
      }
    }
  }
}

bool resourceHoles(const SceneExportSnapshot& snapshot,const tinygltf::Model& source) {
  const auto check=[](std::span<const uint8_t> mask,size_t count) {
    if((!mask.empty() && mask.size()!=count) || !std::ranges::all_of(mask,[](uint8_t x){return x<=1;}))
      fail("resource export phase mask must cover its exact table");
    return std::ranges::find(mask,uint8_t(0))!=mask.end();
  };
  const auto& masks=snapshot.sourceResourceMasks;
  bool holes=check(masks.images,source.images.size());holes=check(masks.textures,source.textures.size()) || holes;
  holes=check(masks.samplers,source.samplers.size()) || holes;
  const auto materials=snapshot.materials.empty()?source.materials.size():snapshot.materials.size();
  (void)check(masks.materials,materials);(void)check(masks.nodes,snapshot.graph.nodeCount());return holes;
}
void omitInactiveSourceChannels(tinygltf::Model& model,const SceneExportSnapshot& snapshot) {
  const auto& masks=snapshot.sourceResourceMasks;
  constexpr size_t maximum=1'000'000;
  if(model.nodes.size()>maximum || model.animations.size()>maximum || snapshot.graph.nodeCount()>maximum)
    fail("resource export owner activity exceeds its finite capacity");
  std::vector<int8_t> sourceNodes(model.nodes.size(),-1);
  for(size_t node=0;node<snapshot.sourceNodeIndices.size();++node) {
    const auto source=snapshot.sourceNodeIndices[node];if(source<0) continue;
    if(size_t(source)>=sourceNodes.size()) fail("source node activity map is invalid");
    const bool retained=!snapshot.graph.isNodeRetired(uint32_t(node)) && (masks.nodes.empty() || masks.nodes[node]!=0);
    sourceNodes[size_t(source)]=std::max<int8_t>(sourceNodes[size_t(source)],retained?1:0);
  }
  size_t channels=0;
  for(const auto& animation:model.animations) {
    if(animation.channels.size()>maximum-channels) fail("resource export animation activity exceeds its finite capacity");
    channels+=animation.channels.size();
  }
  for(auto& animation:model.animations) std::erase_if(animation.channels,[&](const auto& channel) {
    const auto target=channelTarget(channel);
    if(target.table==TargetTable::Material && !masks.materials.empty()) {
      if(target.index<0 || size_t(target.index)>=model.materials.size()) fail("source material animation target is invalid");
      int runtime=target.index;
      if(!snapshot.sourceMaterialToRuntimeIndices.empty()) {
        if(snapshot.sourceMaterialToRuntimeIndices.size()!=model.materials.size()) fail("source material animation map is incomplete");
        runtime=snapshot.sourceMaterialToRuntimeIndices[size_t(target.index)];
      }
      if(runtime<0 || size_t(runtime)>=masks.materials.size()) fail("source material animation map is invalid");
      return masks.materials[size_t(runtime)]==0;
    }
    if(target.table==TargetTable::Node && !snapshot.sourceNodeIndices.empty()) {
      if(target.index<0 || size_t(target.index)>=model.nodes.size()) fail("source node animation target is invalid");
      return sourceNodes[size_t(target.index)]==0;
    }
    return false;
  });
}

int mappedNode(const std::map<int, int>& mapping, int index) {
  const auto mapped = mapping.find(index);
  if (mapped == mapping.end()) fail("animation or skin references a node outside the exported scene");
  return mapped->second;
}

void ensureAnimatedTextureTransform(tinygltf::Material& material,
                                    const std::vector<std::string>& tokens) {
  const auto transform = std::ranges::find(tokens, "KHR_texture_transform");
  if (transform == tokens.end()) return;
  const auto explicitDefaults = [](tinygltf::Value::Object& object) {
    // TinyGLTF preserves a top-level empty extension object, but serializes an
    // empty object nested inside a material extension as null. Explicit schema
    // defaults keep every animated transform parent an actual JSON object.
    if (!object.contains("offset")) object["offset"] = tinygltf::Value(
        tinygltf::Value::Array{tinygltf::Value(0.0), tinygltf::Value(0.0)});
    if (!object.contains("scale")) object["scale"] = tinygltf::Value(
        tinygltf::Value::Array{tinygltf::Value(1.0), tinygltf::Value(1.0)});
    if (!object.contains("rotation")) object["rotation"] = tinygltf::Value(0.0);
  };
  tinygltf::ExtensionMap* core = nullptr;
  if (tokens.size() > 4 && tokens[2] == "pbrMetallicRoughness") {
    if (tokens[3] != "baseColorTexture" && tokens[3] != "metallicRoughnessTexture")
      fail("animated texture transform has an unsupported PBR texture path");
    auto& info = tokens[3] == "baseColorTexture"
        ? material.pbrMetallicRoughness.baseColorTexture
        : material.pbrMetallicRoughness.metallicRoughnessTexture;
    if (info.index < 0) fail("animated texture binding was removed from the runtime material");
    core = &info.extensions;
  } else if (tokens[2] == "normalTexture") {
    if (material.normalTexture.index < 0) fail("animated normal texture binding was removed");
    core = &material.normalTexture.extensions;
  } else if (tokens[2] == "occlusionTexture") {
    if (material.occlusionTexture.index < 0) fail("animated occlusion texture binding was removed");
    core = &material.occlusionTexture.extensions;
  } else if (tokens[2] == "emissiveTexture") {
    if (material.emissiveTexture.index < 0) fail("animated emissive texture binding was removed");
    core = &material.emissiveTexture.extensions;
  }
  if (core) {
    if (!core->contains("KHR_texture_transform"))
      core->emplace("KHR_texture_transform", tinygltf::Value(tinygltf::Value::Object{}));
    auto& value = core->at("KHR_texture_transform");
    if (!value.IsObject()) fail("animated texture transform parent is invalid");
    explicitDefaults(value.Get<tinygltf::Value::Object>());
    return;
  }
  if (tokens[2] != "extensions" || tokens.size() < 8)
    fail("animated texture transform has an unsupported material path");
  const auto extension = material.extensions.find(tokens[3]);
  if (extension == material.extensions.end() || !extension->second.IsObject())
    fail("animated material extension was removed from the runtime material");
  auto* object = &extension->second.Get<tinygltf::Value::Object>();
  const size_t transformIndex = static_cast<size_t>(transform - tokens.begin());
  for (size_t i = 4; i <= transformIndex; ++i) {
    auto found = object->find(tokens[i]);
    if (found == object->end() && (tokens[i] == "extensions" || tokens[i] == "KHR_texture_transform"))
      found = object->emplace(tokens[i], tinygltf::Value(tinygltf::Value::Object{})).first;
    if (found == object->end() || !found->second.IsObject())
      fail("animated extension texture binding was removed from the runtime material");
    if (tokens[i].ends_with("Texture") &&
        (!found->second.Has("index") || !found->second.Get("index").IsInt() ||
         found->second.Get("index").Get<int>() < 0))
      fail("animated extension texture has an invalid binding");
    object = &found->second.Get<tinygltf::Value::Object>();
  }
  explicitDefaults(*object);
}

void validateAnimations(const tinygltf::Model& model) {
  try {
    (void)ParseAnimations(model);
  } catch (const std::exception& error) {
    fail("invalid animation: " + std::string(error.what()));
  }
}

const tinygltf::Value& restMaterialParent(const tinygltf::Material& material,
                                          const std::vector<std::string>& tokens) {
  const tinygltf::Value* parent = nullptr;
  size_t begin = 2;
  if (tokens[0] == "extensions") {
    const auto found = material.extensions.find(tokens[1]);
    if (found != material.extensions.end()) parent = &found->second;
  } else {
    const auto extension = std::ranges::find(tokens, "extensions");
    const size_t position = size_t(extension - tokens.begin());
    std::string texture;
    for (size_t i = 0; i < position; ++i) texture += (i ? "/" : "") + tokens[i];
    const tinygltf::ExtensionMap* extensions = nullptr;
    int index = -1;
    if (texture == "pbrMetallicRoughness/baseColorTexture") {
      extensions = &material.pbrMetallicRoughness.baseColorTexture.extensions;
      index = material.pbrMetallicRoughness.baseColorTexture.index;
    } else if (texture == "pbrMetallicRoughness/metallicRoughnessTexture") {
      extensions = &material.pbrMetallicRoughness.metallicRoughnessTexture.extensions;
      index = material.pbrMetallicRoughness.metallicRoughnessTexture.index;
    } else if (texture == "normalTexture") {
      extensions = &material.normalTexture.extensions; index = material.normalTexture.index;
    } else if (texture == "occlusionTexture") {
      extensions = &material.occlusionTexture.extensions; index = material.occlusionTexture.index;
    } else if (texture == "emissiveTexture") {
      extensions = &material.emissiveTexture.extensions; index = material.emissiveTexture.index;
    }
    if (!extensions || index < 0) fail("authored animation rest texture is missing");
    const auto found = extensions->find("KHR_texture_transform");
    if (found != extensions->end()) parent = &found->second;
    begin = position + 2;
  }
  if (!parent || !parent->IsObject()) fail("authored animation rest material parent is missing");
  for (size_t i = begin; i + 1 < tokens.size(); ++i) {
    if (!parent->Has(tokens[i]) || !parent->Get(tokens[i]).IsObject())
      fail("authored animation rest material parent is missing");
    parent = &parent->Get(tokens[i]);
  }
  return *parent;
}

std::vector<double> animationRestValue(const tinygltf::Model& rest,
                                      const AnimationTarget& target) {
  const auto vectorOr = [&](const std::vector<double>& value, double fallback) {
    if (value.empty()) return std::vector<double>(target.components, fallback);
    if (value.size() != target.components) fail("authored animation rest vector has an invalid size");
    return value;
  };
  if (target.kind == AnimationTargetKind::Material) {
    if (target.index >= rest.materials.size()) fail("authored animation rest material is missing");
    const auto& material = rest.materials[target.index];
    const auto& property = target.property;
    if (property == "pbrMetallicRoughness/baseColorFactor")
      return vectorOr(material.pbrMetallicRoughness.baseColorFactor, 1);
    if (property == "pbrMetallicRoughness/metallicFactor") return {material.pbrMetallicRoughness.metallicFactor};
    if (property == "pbrMetallicRoughness/roughnessFactor") return {material.pbrMetallicRoughness.roughnessFactor};
    if (property == "emissiveFactor") return vectorOr(material.emissiveFactor, 0);
    if (property == "alphaCutoff") return {material.alphaCutoff};
    if (property == "normalTexture/scale") return {material.normalTexture.scale};
    if (property == "occlusionTexture/strength") return {material.occlusionTexture.strength};
    const auto tokens = pointerTokens('/' + property);
    const auto& parent = restMaterialParent(material, tokens);
    if (parent.Has(tokens.back())) {
      const auto& value = parent.Get(tokens.back());
      if (target.components == 1) {
        if (!value.IsNumber()) fail("authored animation rest material scalar is invalid");
        return {value.GetNumberAsDouble()};
      }
      if (!value.IsArray() || value.ArrayLen() != target.components)
        fail("authored animation rest material vector is invalid");
      std::vector<double> values;
      for (size_t i = 0; i < target.components; ++i) {
        if (!value.Get(i).IsNumber()) fail("authored animation rest material component is invalid");
        values.push_back(value.Get(i).GetNumberAsDouble());
      }
      return values;
    }
    // Existing parent objects define numeric schema defaults even when their
    // animated leaves were omitted in the original JSON.
    if (target.components > 1)
      return std::vector<double>(target.components,
          tokens.back() == "sheenColorFactor" || tokens.back() == "offset" ? 0 : 1);
    if (property.find("/extensions/KHR_texture_transform/") != std::string::npos)
      return {tokens.back() == "scale" ? 1.0 : 0.0};
    static const std::map<std::string, double> defaults{
      {"emissiveStrength", 1}, {"ior", 1.5}, {"dispersion", 0},
      {"transmissionFactor", 0}, {"thicknessFactor", 0},
      {"attenuationDistance", std::numeric_limits<double>::infinity()},
      {"clearcoatFactor", 0}, {"clearcoatRoughnessFactor", 0}, {"scale", 1},
      {"sheenRoughnessFactor", 0}, {"specularFactor", 1},
      {"iridescenceFactor", 0}, {"iridescenceIor", 1.3},
      {"iridescenceThicknessMinimum", 100}, {"iridescenceThicknessMaximum", 400},
      {"anisotropyStrength", 0}, {"anisotropyRotation", 0}, {"diffuseTransmissionFactor", 0},
      {"glossinessFactor", 1}};
    const auto found = defaults.find(tokens.back());
    if (found == defaults.end()) fail("authored animation rest property has no supported schema default");
    return {found->second};
  }
  if (target.kind == AnimationTargetKind::Light) {
    if (target.index >= rest.lights.size()) fail("authored animation rest light is missing");
    const auto& light = rest.lights[target.index];
    if (target.property == "color") return vectorOr(light.color, 1);
    if (target.property == "intensity") return {light.intensity};
    if (target.property == "range") return {light.range};
    if (target.property == "spot/innerConeAngle") return {light.spot.innerConeAngle};
    if (target.property == "spot/outerConeAngle") return {light.spot.outerConeAngle};
  } else if (target.kind == AnimationTargetKind::Camera) {
    if (target.index >= rest.cameras.size()) fail("authored animation rest camera is missing");
    const auto& camera = rest.cameras[target.index];
    if (target.property == "perspective/yfov") return {camera.perspective.yfov};
    if (target.property == "perspective/znear") return {camera.perspective.znear};
    if (target.property == "perspective/zfar") return {camera.perspective.zfar};
    if (target.property == "perspective/aspectRatio") return {camera.perspective.aspectRatio};
    if (target.property == "orthographic/xmag") return {camera.orthographic.xmag};
    if (target.property == "orthographic/ymag") return {camera.orthographic.ymag};
    if (target.property == "orthographic/znear") return {camera.orthographic.znear};
    if (target.property == "orthographic/zfar") return {camera.orthographic.zfar};
  } else {
    if (target.index >= rest.nodes.size()) fail("authored animation rest node is missing");
    const auto& node = rest.nodes[target.index];
    if (target.kind == AnimationTargetKind::NodeTranslation) return vectorOr(node.translation, 0);
    if (target.kind == AnimationTargetKind::NodeScale) return vectorOr(node.scale, 1);
    if (target.kind == AnimationTargetKind::NodeRotation)
      return node.rotation.empty() ? std::vector<double>{0,0,0,1} : vectorOr(node.rotation, 0);
    if (target.kind == AnimationTargetKind::NodeFlag) return ReadAnimationTarget(rest, target);
    if (target.kind == AnimationTargetKind::NodeWeights) {
      if (!node.weights.empty()) return vectorOr(node.weights, 0);
      if (node.mesh < 0 || size_t(node.mesh) >= rest.meshes.size())
        fail("authored animation rest morph mesh is missing");
      return vectorOr(rest.meshes[size_t(node.mesh)].weights, 0);
    }
  }
  fail("authored animation rest property is unsupported");
}

void appendAnimationRestores(tinygltf::Model& model, const tinygltf::Model& rest,
    std::span<const size_t> graphClips) {
  // File defaults describe the saved pose. Each clip must nevertheless restore
  // authored defaults for properties animated by a different clip. Keep all
  // original samplers/accessors intact and append one-key STEP channels only.
  const auto library = ParseAnimations(model);
  using Key = std::tuple<AnimationTargetKind, size_t, std::string>;
  const auto keyOf = [](const AnimationTarget& target) {
    return Key{target.kind, target.index, target.property};
  };
  struct Property { AnimationTarget target; std::vector<bool> components; };
  std::map<Key, Property> properties;
  for (const auto& clip : library.clips)
    for (const auto& track : clip.tracks) {
      const auto& target = track.target;
      auto [found, inserted] = properties.try_emplace(keyOf(target),
          Property{target, std::vector<bool>(target.components, false)});
      found->second.target.element.reset();
      auto& components = found->second.components;
      if (components.size() != target.components) fail("animation target shape changes between clips");
      if (target.element) components[*target.element] = true;
      else std::fill(components.begin(), components.end(), true);
    }
  int timeAccessor = -1;
  bool addedPointers = false;
  for (size_t i = 0; i < library.clips.size(); ++i) {
    // Graph-controlled clips leave unrelated properties to the behavior.
    if (std::ranges::binary_search(graphClips, i)) continue;
    auto& animation = model.animations[i];
    std::map<Key, std::vector<bool>> coverage;
    for (const auto& track : library.clips[i].tracks) {
      const auto& target = track.target;
      auto& components = coverage.try_emplace(keyOf(target), target.components, false).first->second;
      if (target.element) components[*target.element] = true;
      else std::fill(components.begin(), components.end(), true);
    }
    for (const auto& [key, property] : properties) {
      auto missing = property.components;
      if (const auto found = coverage.find(key); found != coverage.end())
        for (size_t c = 0; c < missing.size(); ++c) missing[c] = missing[c] && !found->second[c];
      if (std::ranges::none_of(missing, [](bool value) { return value; })) continue;
      const auto values = animationRestValue(rest, property.target);
      if (values.size() != missing.size()) fail("authored animation rest shape differs from the target");
      if (timeAccessor < 0) {
        Bytes bytes; appendScalar(bytes, 0.0f);
        timeAccessor = appendAccessor(model, std::move(bytes), 1,
            TINYGLTF_TYPE_SCALAR, TINYGLTF_COMPONENT_TYPE_FLOAT, 0);
        model.accessors[size_t(timeAccessor)].minValues = {0};
        model.accessors[size_t(timeAccessor)].maxValues = {0};
      }
      const auto append = [&](std::optional<size_t> element) {
        const auto& target = property.target;
        const bool nodeFlag = target.kind == AnimationTargetKind::NodeFlag;
        Bytes bytes;
        for (size_t c = 0; c < values.size(); ++c) {
          if (element && c != *element) continue;
          if (!std::isfinite(values[c]) || std::abs(values[c]) > std::numeric_limits<float>::max())
            fail("authored animation rest value cannot be represented by a finite FLOAT restore channel");
          if (nodeFlag) appendScalar(bytes, static_cast<uint8_t>(values[c] != 0));
          else appendScalar(bytes, static_cast<float>(values[c]));
        }
        const size_t width = element ? 1 : target.components;
        int type = TINYGLTF_TYPE_SCALAR;
        if (target.kind != AnimationTargetKind::NodeWeights) {
          if (width == 2) type = TINYGLTF_TYPE_VEC2;
          else if (width == 3) type = TINYGLTF_TYPE_VEC3;
          else if (width == 4) type = TINYGLTF_TYPE_VEC4;
          else if (width != 1) fail("animation restore vector shape is unsupported");
        }
        tinygltf::AnimationSampler sampler;
        sampler.input = timeAccessor;
        sampler.output = appendAccessor(model, std::move(bytes),
            target.kind == AnimationTargetKind::NodeWeights ? width : 1,
            type, nodeFlag ? TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE : TINYGLTF_COMPONENT_TYPE_FLOAT, 0);
        sampler.interpolation = "STEP";
        tinygltf::AnimationChannel channel;
        channel.sampler = static_cast<int>(animation.samplers.size());
        const bool node = target.kind == AnimationTargetKind::NodeTranslation ||
            target.kind == AnimationTargetKind::NodeRotation || target.kind == AnimationTargetKind::NodeScale ||
            target.kind == AnimationTargetKind::NodeWeights || nodeFlag;
        if (node && !element && !nodeFlag) {
          channel.target_node = static_cast<int>(target.index);
          channel.target_path = target.property;
        } else {
          addedPointers = true;
          std::string root = node ? "/nodes/" : target.kind == AnimationTargetKind::Material ? "/materials/" :
              target.kind == AnimationTargetKind::Camera ? "/cameras/" : "/extensions/KHR_lights_punctual/lights/";
          auto pointer = root + std::to_string(target.index) + '/' + target.property;
          if (element) pointer += '/' + std::to_string(*element);
          channel.target_node = -1;
          channel.target_path = "pointer";
          channel.target_extensions["KHR_animation_pointer"] = tinygltf::Value(
              tinygltf::Value::Object{{"pointer", tinygltf::Value(std::move(pointer))}});
        }
        animation.samplers.push_back(std::move(sampler));
        animation.channels.push_back(std::move(channel));
      };
      if (std::ranges::all_of(missing, [](bool value) { return value; })) append({});
      else for (size_t c = 0; c < missing.size(); ++c) if (missing[c]) append(c);
    }
  }
  if (addedPointers && std::ranges::find(model.extensionsUsed, "KHR_animation_pointer") == model.extensionsUsed.end())
    model.extensionsUsed.push_back("KHR_animation_pointer");
}

tinygltf::Primitive serializePrimitive(tinygltf::Model& output,
                                       const geometry::Mesh& mesh,
                                       const tinygltf::Primitive* source) {
  if (mesh.empty() || mesh.indices().size() % 3 != 0)
    fail("runtime primitive is empty or not a triangle list");
  for (auto index : mesh.indices())
    if (index >= mesh.vertices().size()) fail("runtime mesh index is out of range");
  tinygltf::Primitive primitive;
  if (source) primitive = *source;
  // Bind vertex order is unchanged by runtime winding/tangent repairs. Keep
  // joint/weight sets, morph targets and other auxiliary source attributes on
  // their original accessors while replacing the runtime's authored streams.
  if (source) {
    for (const auto& [_, accessorIndex] : source->attributes)
      if (accessorIndex < 0 || size_t(accessorIndex) >= output.accessors.size() ||
          output.accessors[size_t(accessorIndex)].count != mesh.vertices().size())
        fail("source auxiliary attribute no longer matches the runtime vertex order");
    for (const auto& target : source->targets)
      for (const auto& [_, accessorIndex] : target)
        if (accessorIndex < 0 || size_t(accessorIndex) >= output.accessors.size() ||
            output.accessors[size_t(accessorIndex)].count != mesh.vertices().size())
          fail("source morph target no longer matches the runtime vertex order");
  }
  primitive.mode = TINYGLTF_MODE_TRIANGLES;
  // These index-bearing/compressed source representations describe old vertex
  // buffers; the current CPU geometry is the authoritative representation.
  primitive.extensions.erase("KHR_draco_mesh_compression");
  primitive.extensions.erase("KHR_materials_variants");
  primitive.extensions_json_string.clear();

  Bytes position, normal, tangent, color, uv0, uv1, indices;
  std::array<double, 3> minimum{std::numeric_limits<double>::max(),
                              std::numeric_limits<double>::max(),
                              std::numeric_limits<double>::max()};
  std::array<double, 3> maximum{std::numeric_limits<double>::lowest(),
                              std::numeric_limits<double>::lowest(),
                              std::numeric_limits<double>::lowest()};
  for (const auto& vertex : mesh.vertices()) {
    auto append = [](Bytes& bytes, float value) {
      if (!std::isfinite(value)) fail("runtime geometry contains non-finite values");
      appendScalar(bytes, value);
    };
    for (int axis = 0; axis < 3; ++axis) {
      append(position, vertex.position[axis]);
      minimum[axis] = std::min(minimum[axis], double(vertex.position[axis]));
      maximum[axis] = std::max(maximum[axis], double(vertex.position[axis]));
      append(normal, vertex.normal[axis]);
      append(color, vertex.color[axis]);
    }
    append(color, vertex.colorAlpha);
    for (int axis = 0; axis < 4; ++axis) append(tangent, vertex.tangent[axis]);
    for (int axis = 0; axis < 2; ++axis) {
      append(uv0, vertex.texCoord[axis]);
      append(uv1, vertex.texCoord1[axis]);
    }
  }
  for (const auto index : mesh.indices()) appendScalar(indices, index);
  const auto count = mesh.vertices().size();
  const auto attribute = [&](const char* name, Bytes bytes, int type) {
    const auto index = appendAccessor(output, std::move(bytes), count, type,
                                       TINYGLTF_COMPONENT_TYPE_FLOAT,
                                       TINYGLTF_TARGET_ARRAY_BUFFER);
    primitive.attributes[name] = index;
    return index;
  };
  const auto positions = attribute("POSITION", std::move(position), TINYGLTF_TYPE_VEC3);
  output.accessors[positions].minValues.assign(minimum.begin(), minimum.end());
  output.accessors[positions].maxValues.assign(maximum.begin(), maximum.end());
  const bool authoredNormals = !source || source->attributes.contains("NORMAL");
  const bool authoredTangents = authoredNormals && (!source || source->attributes.contains("TANGENT"));
  // Preserve absence as authored provenance: missing-base deformation frames
  // must continue to regenerate after morphing/skinning when this file reloads.
  if (authoredNormals) attribute("NORMAL", std::move(normal), TINYGLTF_TYPE_VEC3);
  else primitive.attributes.erase("NORMAL");
  if (authoredTangents) attribute("TANGENT", std::move(tangent), TINYGLTF_TYPE_VEC4);
  else primitive.attributes.erase("TANGENT");
  attribute("COLOR_0", std::move(color), TINYGLTF_TYPE_VEC4);
  attribute("TEXCOORD_0", std::move(uv0), TINYGLTF_TYPE_VEC2);
  attribute("TEXCOORD_1", std::move(uv1), TINYGLTF_TYPE_VEC2);
  primitive.indices = appendAccessor(output, std::move(indices), mesh.indices().size(),
                                      TINYGLTF_TYPE_SCALAR,
                                      TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,
                                      TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
  return primitive;
}

std::string resourceName(const Bytes& bytes) {
  uint64_t hash = 14695981039346656037ull;
  for (const auto byte : bytes) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  std::ostringstream name;
  name << "container-" << std::hex << std::setfill('0') << std::setw(16) << hash
       << '-' << bytes.size() << ".bin";
  return name.str();
}

class StagingDirectory {
 public:
  explicit StagingDirectory(const std::filesystem::path& parent) {
    static std::atomic<uint64_t> sequence{0};
    for (int attempt = 0; attempt < 100; ++attempt) {
      const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
      path = parent / (".container-export-" + std::to_string(stamp) + '-' +
                       std::to_string(sequence.fetch_add(1)));
      std::error_code error;
      if (std::filesystem::create_directory(path, error)) return;
      if (error && error != std::errc::file_exists)
        fail("cannot create export staging directory: " + error.message());
    }
    fail("cannot allocate an export staging directory");
  }
  ~StagingDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::filesystem::path path;
};

void replaceFile(const std::filesystem::path& source,
                  const std::filesystem::path& destination) {
#ifdef _WIN32
  if (!MoveFileExW(source.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    fail("cannot replace destination: " +
          std::system_category().message(static_cast<int>(GetLastError())));
#else
  std::error_code error;
  std::filesystem::rename(source, destination, error);
  if (error) fail("cannot replace destination: " + error.message());
#endif
}

void installResource(const std::filesystem::path& staged,
                     const std::filesystem::path& destination,
                     const Bytes& expected) {
  if (std::filesystem::exists(destination)) {
    if (!std::filesystem::is_regular_file(destination) || readBytes(destination) != expected)
      fail("existing export dependency differs from its content-addressed name");
    return;
  }
  // Both paths are on the same filesystem. A hard link publishes complete
  // bytes atomically and cannot overwrite an earlier export's dependency.
  std::error_code error;
  std::filesystem::create_hard_link(staged, destination, error);
  if (!error) return;
  if (std::filesystem::exists(destination)) {
    if (std::filesystem::is_regular_file(destination) && readBytes(destination) == expected)
      return;
    fail("export dependency already exists with different content");
  }
  // FAT/network destinations may not support hard links. copy_file's exclusive
  // creation still protects existing dependencies; final JSON is unpublished.
  if (!std::filesystem::copy_file(staged, destination,
                                  std::filesystem::copy_options::none, error))
    fail("cannot install export dependency: " + error.message());
}

}  // namespace

tinygltf::Model buildExportModel(const SceneExportSnapshot& snapshot) {
  const bool hasBehavior = snapshot.source && snapshot.source->extensions.contains(kGltfInteractivityExtension);
  validateGraph(snapshot.graph);
  if (!snapshot.sourceNodeIndices.empty() &&
      snapshot.sourceNodeIndices.size() != snapshot.graph.nodeCount())
    fail("source node mapping must cover every runtime node");
  if (!snapshot.omitNodeMeshes.empty() &&
      (snapshot.omitNodeMeshes.size() != snapshot.graph.nodeCount() ||
       std::ranges::any_of(snapshot.omitNodeMeshes, [](uint8_t value) { return value > 1; })))
    fail("linked payload mask must cover every runtime node");
  if (snapshot.resourceIndexMaps) *snapshot.resourceIndexMaps = {};
  auto scenePlacements = validateSceneRoots(snapshot);
  tinygltf::Model output;
  if (snapshot.source) output = *snapshot.source;
  output.asset.version = "2.0";
  if (output.asset.generator.empty()) output.asset.generator = "Container scene editor";
  const bool compactResources=resourceHoles(snapshot,output);
  if(compactResources) {omitInactiveSourceChannels(output,snapshot);packBuffers(output,snapshot.sourcePath);}
  else packResources(output, snapshot.sourcePath);
  // Validate source tracks while their original material/node index spaces
  // still exist. Binary resources are already resident and remapped here.
  if (!output.animations.empty()) validateAnimations(output);
  if (snapshot.animationRestSource && !output.animations.empty()) {
    try {
      const auto graphClips = hasBehavior ? GltfInteractivityAnimationClips(*snapshot.source) :
          std::optional<std::vector<size_t>>{std::in_place};
      if (graphClips) appendAnimationRestores(output, *snapshot.animationRestSource, *graphClips);
    } catch (const std::invalid_argument& error) {
      fail("invalid authored animation rest state: " + std::string(error.what()));
    }
  }
  if (!snapshot.materials.empty())
    output.materials.assign(snapshot.materials.begin(), snapshot.materials.end());
  if (output.materials.empty()) output.materials.emplace_back();
  if (!snapshot.sourceMaterialToRuntimeIndices.empty() &&
      (!snapshot.source || snapshot.sourceMaterialToRuntimeIndices.size() != snapshot.source->materials.size()))
    fail("material animation mapping must cover every source material");
  appendMaterialExtensionDeclarations(output);

  std::vector<const tinygltf::Primitive*> sourcePrimitives;
  std::vector<const tinygltf::Mesh*> sourceMeshes;
  std::vector<size_t> sourceMeshIndices;
  std::vector<size_t> sourcePrimitiveIndices;
  if (snapshot.source)
    for (size_t meshIndex = 0; meshIndex < snapshot.source->meshes.size(); ++meshIndex) {
      const auto& mesh = snapshot.source->meshes[meshIndex];
      for (size_t primitiveIndex=0;primitiveIndex<mesh.primitives.size();++primitiveIndex) {
        const auto& primitive=mesh.primitives[primitiveIndex];
        sourcePrimitives.push_back(&primitive);
        sourceMeshes.push_back(&mesh);
        sourceMeshIndices.push_back(meshIndex);
        sourcePrimitiveIndices.push_back(primitiveIndex);
      }
    }
  const auto primitiveMapping = [&] {
    try {
      return scene::MakeRuntimePrimitiveMapping(sourcePrimitives.size(), snapshot.geometry.meshes().size(),
                                                snapshot.runtimePrimitiveToSourceIndices);
    } catch (const std::invalid_argument& error) { fail(error.what()); }
  }();
  bool hasDeformation = !output.skins.empty();
  for (const auto* primitive : sourcePrimitives)
    hasDeformation |= !primitive->targets.empty() || primitive->attributes.contains("JOINTS_0");
  if (hasDeformation) {
    // Validation needs the source mesh/node tables, with resource views packed
    // but source accessors and bind vertex order unchanged.
    if (snapshot.runtimePrimitiveToSourceIndices.empty()) {
      (void)LoadDeformationData(output,
          std::span<const geometry::Mesh>(snapshot.geometry.meshes().data(), sourcePrimitives.size()));
    } else {
      std::vector<geometry::Mesh> sourceGeometry;
      sourceGeometry.reserve(sourcePrimitives.size());
      for (const auto runtime : primitiveMapping.sourceToRuntime)
        sourceGeometry.push_back(snapshot.geometry.meshes()[runtime]);
      (void)LoadDeformationData(output, sourceGeometry);
    }
  }
  output.meshes.clear();
  output.nodes.clear();
  output.nodes.resize(snapshot.graph.nodeCount());
  std::map<uint32_t, tinygltf::Primitive> primitives;
  std::map<std::pair<uint32_t, uint32_t>, int> meshes;
  std::vector<MaterialVariantExportPrimitive> variantExports;
  std::map<int, int> sourceToRuntime;
  for (size_t i = 0; i < snapshot.sourceNodeIndices.size(); ++i) {
    const auto sourceIndex = snapshot.sourceNodeIndices[i];
    if (sourceIndex >= 0 &&
        !sourceToRuntime.emplace(sourceIndex, static_cast<int>(i)).second)
      fail("source node mapping must not contain duplicate source nodes");
  }
  std::set<int> animatedTransforms;
  std::set<int> interactiveTransforms;
  if (hasBehavior) {
    try {
      for (const auto node : GltfInteractivityTransformNodes(*snapshot.source)) {
        animatedTransforms.insert(static_cast<int>(node));
        interactiveTransforms.insert(mappedNode(sourceToRuntime, static_cast<int>(node)));
      }
    } catch (const std::invalid_argument& error) {
      fail("invalid KHR_interactivity export graph: " + std::string(error.what()));
    }
  }
  for (const auto& animation : output.animations)
    for (const auto& channel : animation.channels) {
      const auto target = channelTarget(channel);
      if (isTransformTarget(target)) animatedTransforms.insert(target.index);
    }
  for (auto& skin : output.skins) {
    for (auto& joint : skin.joints) joint = mappedNode(sourceToRuntime, joint);
    if (skin.skeleton >= 0) skin.skeleton = mappedNode(sourceToRuntime, skin.skeleton);
  }
  std::map<int, std::vector<int>> morphNodes;
  const auto sourceInstancing=snapshot.source ? ParseMeshGpuInstancing(*snapshot.source) : GltfInstancingData{};
  std::map<std::pair<int,uint32_t>,int> singletonAttributes;
  std::map<int, glm::dmat4> corrections;
  std::vector<PostSkinExport> postSkinEdits;
  for (uint32_t i = 0; i < snapshot.graph.nodeCount(); ++i) {
    const auto& current = *snapshot.graph.getNode(i);
    auto& node = output.nodes[i];
    if (!snapshot.sourceNodeIndices.empty() && snapshot.sourceNodeIndices[i] >= 0) {
      const auto sourceIndex = static_cast<size_t>(snapshot.sourceNodeIndices[i]);
      if (!snapshot.source || sourceIndex >= snapshot.source->nodes.size())
        fail("source node mapping has an invalid source index");
      node = snapshot.source->nodes[sourceIndex];
      for (auto& lod : node.lods) {
        const auto mapped = sourceToRuntime.find(lod);
        if (mapped == sourceToRuntime.end())
          fail("source level-of-detail reference is outside the exported scene");
        lod = mapped->second;
      }
    }
    node.name = current.name;
    node.children.assign(current.children.begin(), current.children.end());
    const int sourceIndex = snapshot.sourceNodeIndices.empty() ? -1 : snapshot.sourceNodeIndices[i];
    if (animatedTransforms.contains(sourceIndex)) {
      if (!node.matrix.empty()) fail("animated TRS target cannot retain a source matrix");
      const auto pose = sourceTransform(node);
      const glm::dmat4 edited(current.localTransform);
      if (!snapshot.graph.isNodeRetired(i) && !matrixNear(edited, pose)) {
        const auto inverse = inverseAffine(pose);
        if (!matrixFinite(inverse)) fail("edited animated transform cannot be corrected at a singular pose");
        const auto correction = edited * inverse;
        if (!matrixFinite(correction)) fail("animated editor correction is outside the finite runtime range");
        corrections.emplace(static_cast<int>(i), correction);
      }
      // Keep a TRS target at the stable original runtime index. A separate
      // constant parent carries editor changes, including arbitrary shear.
      node.matrix.clear();
    } else {
      // GLM/glTF share the same column-major, right-handed +Y-up convention.
      setMatrix(node, glm::dmat4(current.localTransform));
    }
    node.mesh = -1;
    // Source groups no longer own the mesh. Instance extensions belong only
    // to the emitted mesh nodes, never to a meshless hierarchy group.
    node.extensions.erase("EXT_mesh_gpu_instancing");
    node.skin = -1;
    node.weights.clear();
    node.extensions_json_string.clear();
    setLocalNodeFlag(node, "KHR_node_visibility", "visible", current.visible);
    setLocalNodeFlag(node, "KHR_node_selectability", "selectable", current.selectable);
    setLocalNodeFlag(node, "KHR_node_hoverability", "hoverable", current.hoverable);
    if (current.renderable && (snapshot.omitNodeMeshes.empty() || !snapshot.omitNodeMeshes[i])) {
      if (current.primitiveIndex >= snapshot.geometry.meshes().size())
        fail("renderable node references an invalid runtime primitive");
      if (current.materialIndex >= output.materials.size())
        fail("renderable node references an invalid runtime material");
      const int32_t sourcePrimitive = primitiveMapping.runtimeToSource[current.primitiveIndex];
      if (sourcePrimitive >= 0) {
        uint32_t owner = current.meshOwnerGraphNode;
        if (owner == scene::SceneGraph::kInvalidNode) {
          // Source-bearing renderables and legacy callers can resolve the
          // original mesh owner through the current ancestor chain.
          owner = i;
          while (owner != scene::SceneGraph::kInvalidNode) {
            const int candidate = snapshot.sourceNodeIndices.empty() ? -1 : snapshot.sourceNodeIndices[owner];
            if (candidate >= 0 && size_t(candidate) < snapshot.source->nodes.size() &&
                snapshot.source->nodes[size_t(candidate)].mesh >= 0 &&
                size_t(snapshot.source->nodes[size_t(candidate)].mesh) == sourceMeshIndices[size_t(sourcePrimitive)]) break;
            owner = snapshot.graph.getNode(owner)->parent;
          }
        }
        if (owner != scene::SceneGraph::kInvalidNode) {
          if (owner >= snapshot.graph.nodeCount()) fail("deformed mesh owner is missing");
          const int ownerSource = snapshot.sourceNodeIndices.empty() ? -1 : snapshot.sourceNodeIndices[owner];
          if (ownerSource < 0 || size_t(ownerSource) >= snapshot.source->nodes.size())
            fail("deformed mesh owner has no source identity");
          const auto& instance = snapshot.source->nodes[size_t(ownerSource)];
          const auto* instances=FindNodeInstancing(sourceInstancing,uint32_t(ownerSource));
          const glm::mat4* instanceLocal=nullptr;
          if (current.sourceInstanceIndex>=0 && !snapshot.graph.isNodeRetired(i)) {
            if(!instances || size_t(current.sourceInstanceIndex)>=instances->localMatrices.size())
              fail("renderable source instance identity is invalid");
            instanceLocal=&instances->localMatrices[size_t(current.sourceInstanceIndex)];
            tinygltf::Value::Object attributes;
            for(const auto& attribute:instances->attributes) {
              const auto key=std::pair{attribute.sourceAccessor,uint32_t(current.sourceInstanceIndex)};
              auto found=singletonAttributes.find(key);
              if(found==singletonAttributes.end()) {
                const CheckedAccessor reader(*snapshot.source,attribute.sourceAccessor,"instance export",
                    {AccessorUsage::Generic,HasRequiredExtension(*snapshot.source,"KHR_accessor_float64")});
                Bytes bytes(reader.encodedElementSize());
                reader.readEncodedElement(size_t(current.sourceInstanceIndex),bytes);
                auto accessor=reader.accessor();
                accessor.bufferView=appendView(output,bytes); accessor.byteOffset=0; accessor.count=1;
                accessor.sparse={}; accessor.minValues.clear(); accessor.maxValues.clear();
                const int id=int(output.accessors.size()); output.accessors.push_back(std::move(accessor));
                found=singletonAttributes.emplace(key,id).first;
              }
              attributes[attribute.semantic]=tinygltf::Value(found->second);
            }
            auto extension=instance.extensions.at("EXT_mesh_gpu_instancing");
            extension.Get<tinygltf::Value::Object>()["attributes"]=tinygltf::Value(std::move(attributes));
            node.extensions["EXT_mesh_gpu_instancing"]=std::move(extension);
            const auto inverse=glm::inverse(glm::dmat4(*instanceLocal));
            if(!matrixFinite(inverse)) fail("singular source instance export requires geometry baking");
            setMatrix(node,matrixNear(glm::dmat4(current.localTransform),glm::dmat4(*instanceLocal))
                ? glm::dmat4(1) : glm::dmat4(current.localTransform)*inverse);
          }
          if (instance.mesh < 0 || size_t(instance.mesh) != sourceMeshIndices[size_t(sourcePrimitive)])
            fail("mesh owner differs from the primitive source mesh");
          if (instance.skin >= 0) {
            uint32_t ancestor = i;
            while (!snapshot.graph.isNodeRetired(i) && ancestor != owner) {
              if (ancestor == scene::SceneGraph::kInvalidNode ||
                  snapshot.graph.getNode(ancestor)->localTransform !=
                      (ancestor == i && instanceLocal ? *instanceLocal : glm::mat4(1))) break;
              const int identity = snapshot.sourceNodeIndices.empty() ? -1 : snapshot.sourceNodeIndices[ancestor];
              if (animatedTransforms.contains(identity)) break;
              ancestor = snapshot.graph.getNode(ancestor)->parent;
            }
            if (!snapshot.graph.isNodeRetired(i) && ancestor != owner) {
              if (instanceLocal) setMatrix(node, glm::dmat4(current.localTransform));
              postSkinEdits.push_back({static_cast<int>(i), static_cast<int>(owner)});
            }
            node.skin = instance.skin;
          }
          if (!sourcePrimitives[size_t(sourcePrimitive)]->targets.empty()) {
            node.weights = instance.weights;
            morphNodes[ownerSource].push_back(static_cast<int>(i));
          }
        } else if (!sourcePrimitives[size_t(sourcePrimitive)]->targets.empty() ||
                   sourcePrimitives[size_t(sourcePrimitive)]->attributes.contains("JOINTS_0")) {
          fail("deformed primitive has no original mesh owner");
        }
      }
      const auto key = std::pair{current.primitiveIndex, current.materialIndex};
      if (const auto found = meshes.find(key); found != meshes.end()) {
        node.mesh = found->second;
        continue;
      }
      auto primitive = primitives.find(current.primitiveIndex);
      if (primitive == primitives.end()) {
        const auto index = current.primitiveIndex;
        auto encoded = serializePrimitive(output, snapshot.geometry.meshes()[index],
            sourcePrimitive >= 0 ? sourcePrimitives[size_t(sourcePrimitive)] : nullptr);
        primitive = primitives.emplace(index, std::move(encoded)).first;
      }
      tinygltf::Mesh mesh;
      if (sourcePrimitive >= 0) {
        mesh = *sourceMeshes[size_t(sourcePrimitive)];
        mesh.primitives.clear();
      }
      mesh.name = current.name;
      mesh.primitives.push_back(primitive->second);
      mesh.primitives.front().material = static_cast<int>(current.materialIndex);
      node.mesh = static_cast<int>(output.meshes.size());
      meshes.emplace(key, node.mesh);
      if (sourcePrimitive >= 0)
        variantExports.push_back({sourceMeshIndices[size_t(sourcePrimitive)],sourcePrimitiveIndices[size_t(sourcePrimitive)],output.meshes.size(),0});
      output.meshes.push_back(std::move(mesh));
    }
  }
  if (snapshot.source)
    CopyMaterialVariantMetadataForExport(output,ParseMaterialVariants(*snapshot.source),snapshot.sourceMaterialToRuntimeIndices,variantExports);
  std::map<int, int> entries;
  for (const auto& [nodeIndex, correction] : corrections) {
    tinygltf::Node wrapper;
    wrapper.name = output.nodes[size_t(nodeIndex)].name + " editor transform";
    setMatrix(wrapper, correction);
    wrapper.children = {nodeIndex};
    if (output.nodes.size() >= size_t(std::numeric_limits<int>::max())) fail("too many correction nodes");
    entries[nodeIndex] = static_cast<int>(output.nodes.size());
    output.nodes.push_back(std::move(wrapper));
  }
  for (size_t i = 0; i < snapshot.graph.nodeCount(); ++i)
    for (auto& child : output.nodes[i].children)
      if (const auto wrapper = entries.find(child); wrapper != entries.end()) child = wrapper->second;
  if (snapshot.runtimeSceneRoots.empty()) {
    tinygltf::Scene scene;
    if (snapshot.source && snapshot.source->defaultScene >= 0 &&
        static_cast<size_t>(snapshot.source->defaultScene) < snapshot.source->scenes.size())
      scene = snapshot.source->scenes[static_cast<size_t>(snapshot.source->defaultScene)];
    scene.nodes.assign(snapshot.graph.rootNodes().begin(), snapshot.graph.rootNodes().end());
    for (auto& root : scene.nodes)
      if (const auto wrapper = entries.find(root); wrapper != entries.end()) root = wrapper->second;
    output.scenes = {std::move(scene)};
    output.defaultScene = 0;
  } else {
    // A glTF node has one global parent even when several scenes share it.
    // Detach the root's correction entry from its old synthetic ancestor and
    // reuse one prefix placement node across every scene that selects it.
    for (auto& [root, placement] : scenePlacements) {
      const auto correction = entries.find(static_cast<int>(root));
      placement.entry = correction == entries.end() ? static_cast<int>(root) : correction->second;
      const auto parent = snapshot.graph.getNode(root)->parent;
      if (parent != scene::SceneGraph::kInvalidNode)
        std::erase(output.nodes[parent].children, placement.entry);
      if (!snapshot.graph.isNodeRetired(root) && needsScenePlacement(placement)) {
        tinygltf::Node wrapper;
        wrapper.name = output.nodes[root].name + " scene placement";
        setMatrix(wrapper, placement.prefix);
        wrapper.children = {placement.entry};
        setLocalNodeFlag(wrapper, "KHR_node_visibility", "visible", placement.visible);
        setLocalNodeFlag(wrapper, "KHR_node_selectability", "selectable", placement.selectable);
        setLocalNodeFlag(wrapper, "KHR_node_hoverability", "hoverable", placement.hoverable);
        if (output.nodes.size() >= size_t(std::numeric_limits<int>::max())) fail("too many scene placement nodes");
        placement.entry = static_cast<int>(output.nodes.size());
        output.nodes.push_back(std::move(wrapper));
      }
    }
    output.scenes.clear();
    output.scenes.reserve(snapshot.runtimeSceneRoots.size());
    for (size_t i = 0; i < snapshot.runtimeSceneRoots.size(); ++i) {
      tinygltf::Scene scene;
      if (snapshot.source && i < snapshot.source->scenes.size()) scene = snapshot.source->scenes[i];
      scene.nodes.clear();
      for (const auto root : snapshot.runtimeSceneRoots[i]) scene.nodes.push_back(scenePlacements.at(root).entry);
      output.scenes.push_back(std::move(scene));
    }
    output.defaultScene = snapshot.defaultSceneIndex;
  }
  for (auto& animation : output.animations) {
    std::vector<tinygltf::AnimationChannel> channels;
    for (const auto& original : animation.channels) {
      const auto target = channelTarget(original);
      std::vector<int> destinations;
      if (target.table == TargetTable::Node) {
        if (target.property == "weights") {
          const auto found = morphNodes.find(target.index);
          if (found == morphNodes.end() || found->second.empty())
            fail("morph animation target has no exported mesh instances");
          destinations = found->second;
        } else destinations.push_back(mappedNode(sourceToRuntime, target.index));
      } else if (target.table == TargetTable::Material) {
        if (!snapshot.source || target.index < 0 || size_t(target.index) >= snapshot.source->materials.size())
          fail("material animation source index is invalid");
        const int index = snapshot.sourceMaterialToRuntimeIndices.empty() ? target.index :
            snapshot.sourceMaterialToRuntimeIndices[size_t(target.index)];
        if (index < 0 || size_t(index) >= output.materials.size()) fail("material animation mapping is invalid");
        ensureAnimatedTextureTransform(output.materials[size_t(index)], target.tokens);
        destinations.push_back(index);
      } else {
        const auto count = target.table == TargetTable::Light ? output.lights.size() : output.cameras.size();
        if (target.index < 0 || size_t(target.index) >= count) fail("camera/light animation index is invalid");
        destinations.push_back(target.index);
      }
      for (const int destination : destinations) {
        auto channel = original;
        retargetChannel(channel, target, destination);
        channels.push_back(std::move(channel));
      }
    }
    animation.channels = std::move(channels);
  }
  reconcileDeformationTransforms(output, postSkinEdits, interactiveTransforms, morphNodes, snapshot);
  if (std::ranges::any_of(output.nodes, [](const auto& node) {
        return node.extensions.contains("EXT_mesh_gpu_instancing");
      })) {
    for (auto* declarations : {&output.extensionsUsed, &output.extensionsRequired})
      if (std::ranges::find(*declarations, "EXT_mesh_gpu_instancing") == declarations->end())
        declarations->push_back("EXT_mesh_gpu_instancing");
  } else {
    std::erase(output.extensionsUsed, "EXT_mesh_gpu_instancing");
    std::erase(output.extensionsRequired, "EXT_mesh_gpu_instancing");
  }
  for (const auto* extension : {"KHR_node_visibility", "KHR_node_selectability", "KHR_node_hoverability"})
    if (std::ranges::any_of(output.nodes, [&](const auto& node) { return node.extensions.contains(extension); }) &&
        std::ranges::find(output.extensionsUsed, extension) == output.extensionsUsed.end())
      output.extensionsUsed.emplace_back(extension);
  if (hasBehavior) {
    std::vector<int32_t> nodeMap(snapshot.source->nodes.size(), -1);
    std::vector<int32_t> weightsMap(nodeMap.size(), -1);
    for (const auto& [source, destination] : sourceToRuntime) nodeMap.at(size_t(source)) = destination;
    for (const auto& [source, destinations] : morphNodes)
      if (destinations.size() == 1) weightsMap.at(size_t(source)) = destinations.front();
    const GltfInteractivityMappings mappings{{
        {nodeMap, output.nodes.size()},
        {weightsMap, output.nodes.size()},
        {snapshot.sourceMaterialToRuntimeIndices, output.materials.size()},
        // The light overlay subsequently projects these stable source rows to
        // the definitions actually bound by the exported nodes.
        {{}, output.lights.size()},
        {{}, output.cameras.size()},
        {{}, output.animations.size()},
    }};
    try {
      output.extensions[kGltfInteractivityExtension] = RemapGltfInteractivity(*snapshot.source, mappings, &output);
      if (std::ranges::find(output.extensionsUsed, kGltfInteractivityExtension) == output.extensionsUsed.end())
        output.extensionsUsed.emplace_back(kGltfInteractivityExtension);
      if (!output.extensions.contains(kGltfXmpExtension)) output.extensions_json_string.clear();
      ValidateGltfInteractivity(output);
    } catch (const std::invalid_argument& error) {
      fail(error.what());
    }
  }
  if (!output.animations.empty()) validateAnimations(output);
  appendMaterialExtensionDeclarations(output);
  if(compactResources) {
    GltfResourceExportCompactionLimits limits;
    if(output.nodes.size()>limits.maxTableEntries) fail("resource export node activity exceeds its finite capacity");
    std::vector<uint8_t> nodes(output.nodes.size(),1);
    for(size_t node=0;node<snapshot.graph.nodeCount();++node)
      nodes[node]=!snapshot.graph.isNodeRetired(uint32_t(node)) &&
          (snapshot.sourceResourceMasks.nodes.empty() || snapshot.sourceResourceMasks.nodes[node]!=0);
    auto masks=snapshot.sourceResourceMasks;masks.nodes=nodes;
    const auto resources=PlanGltfResourceExportCompaction(output,masks,limits);
    if (auto* maps = snapshot.resourceIndexMaps) {
      const auto images = resources.oldToNewImages();
      const auto textures = resources.oldToNewTextures();
      const auto samplers = resources.oldToNewSamplers();
      maps->images.assign(images.begin(), images.end());
      maps->textures.assign(textures.begin(), textures.end());
      maps->samplers.assign(samplers.begin(), samplers.end());
    }
    ApplyGltfResourceExportCompaction(output,resources);
    packImagesAndAudio(output,snapshot.sourcePath);
  }
  if (output.buffers.front().data.empty()) output.buffers.clear();
  return output;
}

void writeSceneExport(const SceneExportSnapshot& snapshot,
                      const std::filesystem::path& destination) {
  writeExportModel(buildExportModel(snapshot), destination);
}

void writeLinkedSceneManifest(const GltfLinkedCompositionInfo& composition,
    std::span<const GltfLinkedPlacementEdit> edits, size_t selectedScene,
    const std::filesystem::path& destination) {
  const auto absolute = std::filesystem::absolute(destination);
  if (std::filesystem::exists(absolute) && !std::filesystem::is_regular_file(absolute))
    fail("destination is not a regular file");
  const auto manifest = BuildGltfLinkedManifestForSave(composition, edits, selectedScene, absolute);
  StagingDirectory staging(absolute.parent_path());
  const auto staged = staging.path / absolute.filename();
  std::ofstream file(staged, std::ios::binary | std::ios::trunc);
  file << manifest << '\n';
  file.close();
  if (!file) fail("serializer could not write the staged linked manifest");
  replaceFile(staged, absolute);
}

namespace {

void writePackedScene(const tinygltf::Model& packedModel,
                      const std::filesystem::path& destination,
                      const GltfLegacyMaterialSerializationLimits& legacyLimits,
                      const nlohmann::json& linkedPatch = {}) {
  const auto extension = lower(destination.extension().string());
  if (extension != ".gltf" && extension != ".glb")
    fail("destination must have a .gltf or .glb extension");
  const auto absolute = std::filesystem::absolute(destination);
  if (!std::filesystem::is_directory(absolute.parent_path()))
    fail("destination parent directory does not exist");
  if (std::filesystem::exists(absolute) && !std::filesystem::is_regular_file(absolute))
    fail("destination is not a regular file");
  (void)ValidateSceneSerializationCompatibilityInput(packedModel,legacyLimits);
  const auto xmpPackage = [&]() -> std::string {
    if (!packedModel.extensions.contains(kGltfXmpExtension)) return {};
    if (packedModel.extensions_json_string.size() > legacyLimits.maxJsonBytes)
      fail("XMP package exceeds the serialized JSON byte budget");
    const auto retained = ReadGltfXmpPackage(packedModel);
    return retained ? retained->dump() : std::string{};
  }();
  auto model = packedModel;
  if (!xmpPackage.empty()) model.extensions.erase(kGltfXmpExtension);
  if (model.buffers.size() > 1) fail("writeExportModel requires a packed export model");
  for (const auto& image : model.images)
    if (!image.uri.empty() || image.bufferView < 0)
      fail("writeExportModel requires images packed in buffer views");
  for (const auto& audio : model.audioSources)
    if (!audio.uri.empty() || audio.bufferView < 0)
      fail("writeExportModel requires audio packed in buffer views");
  for (size_t i = 0; i < model.bufferViews.size(); ++i)
    validateView(model, static_cast<int>(i));
  if (!model.buffers.empty()) model.buffers.front().uri.clear();
  StagingDirectory staging(absolute.parent_path());
  const auto stagedFile = staging.path / absolute.filename();
  const bool binary = extension == ".glb";
  if (binary && !linkedPatch.empty()) fail("linked companions must use .gltf");
  std::string dependency;
  if (!binary && !model.buffers.empty()) {
    dependency = resourceName(model.buffers.front().data);
    model.buffers.front().uri = dependency;
  }
  tinygltf::TinyGLTF writer;
  if (!writer.WriteGltfSceneToFile(&model, utf8Path(stagedFile), false, false, true, binary))
    fail("serializer could not write the staged scene");
  if(const auto compatible=StageLegacyMaterialSerialization(model,stagedFile,binary,legacyLimits,xmpPackage))
    replaceFile(*compatible,stagedFile);
  // Verify the serializer closed a complete output before publishing resources.
  if (!std::filesystem::is_regular_file(stagedFile) ||
      std::filesystem::file_size(stagedFile) == 0)
    fail("serializer produced no scene file");
  if (!linkedPatch.empty()) {
    if (std::filesystem::file_size(stagedFile) > legacyLimits.maxJsonBytes)
      fail("staged linked scene exceeds the JSON byte budget");
    std::ifstream input(stagedFile, std::ios::binary);
    auto document = nlohmann::json::parse(input);
    input.close();
    ApplyGltfLinkedHybridManifest(document, linkedPatch);
    const auto serialized = document.dump();
    if (serialized.size() >= legacyLimits.maxJsonBytes)
      fail("linked scene exceeds the JSON byte budget");
    std::ofstream output(stagedFile, std::ios::binary | std::ios::trunc);
    output << serialized << '\n';
    output.close();
    if (!output) fail("serializer could not write the staged linked scene");
  }
  if (!dependency.empty())
    installResource(staging.path / dependency, absolute.parent_path() / dependency,
                     model.buffers.front().data);
  replaceFile(stagedFile, absolute);
}

std::vector<int32_t> linkedNodeCut(const tinygltf::Model& model, std::span<const uint8_t> graphMask) {
  if (graphMask.size() > model.nodes.size() ||
      model.nodes.size() > size_t(std::numeric_limits<int32_t>::max()))
    fail("linked cut has an invalid node table");
  std::vector<uint8_t> cut(model.nodes.size());
  std::vector<int32_t> retired;
  for (size_t i = 0; i < graphMask.size(); ++i) {
    if (graphMask[i] > 1) fail("linked cut has an invalid graph mask");
    if (!graphMask[i]) continue;
    cut[i] = 1;
    retired.push_back(static_cast<int32_t>(i));
  }
  for (size_t i = 0; i < retired.size(); ++i) {
    for (const int child : model.nodes[size_t(retired[i])].children) {
      if (child < 0 || size_t(child) >= cut.size()) fail("linked cut has an invalid child");
      if (cut[size_t(child)]) continue;
      if (size_t(child) < graphMask.size()) fail("linked cut crosses editable hierarchy");
      cut[size_t(child)] = 1;
      retired.push_back(child);
    }
  }
  return retired;
}

void projectLinkedRange(GltfLinkedIndexRange& range, std::span<const int32_t> map) {
  if (!range.count || map.empty()) return;
  if (range.first < 0 || size_t(range.first) >= map.size() ||
      range.count > map.size() - size_t(range.first))
    fail("linked resource range is outside the export map");
  const auto indices = map.subspan(size_t(range.first), range.count);
  if (indices.front() < 0) fail("linked resource was retired in the portable projection");
  for (size_t i = 0; i < indices.size(); ++i)
    if (indices[i] < 0 || size_t(indices[i]) != size_t(indices.front()) + i)
      fail("linked resources lost their contiguous export range");
  range.first = indices.front();
}

}  // namespace

void writeExportModel(const tinygltf::Model& packedModel,
                      const std::filesystem::path& destination,
                      const GltfLegacyMaterialSerializationLimits& legacyLimits) {
  writePackedScene(packedModel, destination, legacyLimits);
}

void writeLinkedHybridScene(tinygltf::Model model, const GltfLinkedHybridExport& options,
                            const std::filesystem::path& destination) {
  std::vector<GltfLinkedHybridCarrier> carriers(options.carriers.begin(), options.carriers.end());
  const auto retired = linkedNodeCut(model, options.prunedGraphNodes);
  if (!retired.empty() || !options.linkedContexts.empty()) {
    auto compacted = CompactGltfExportNodes(model, retired);
    for (auto& carrier : carriers) {
      if (carrier.outputNode < 0 || size_t(carrier.outputNode) >= compacted.oldToNewNodes.size())
        fail("linked carrier is outside the export node table");
      carrier.outputNode = compacted.oldToNewNodes[size_t(carrier.outputNode)];
      if (carrier.outputNode < 0) fail("linked carrier was cut from the export");
    }
    model = std::move(compacted.model);
    std::erase_if(model.animations, [](const auto& animation) { return animation.channels.empty(); });
  }
  const auto patch = BuildGltfLinkedHybridManifest(options.composition, carriers, destination);
  std::vector<GltfLinkedSourceOwnership> contexts;
  contexts.reserve(options.linkedContexts.size());
  for (const auto id : options.linkedContexts) {
    if (id >= options.composition.sourceContexts.size()) fail("linked resource context is outside the export");
    const auto& source = options.composition.sourceContexts[id];
    auto& context = contexts.emplace_back(GltfLinkedSourceOwnership{
        .images = source.images, .samplers = source.samplers, .textures = source.textures,
        .materials = source.materials, .cameras = source.cameras, .lights = source.lights,
        .variants = source.variants, .xmpPackets = source.xmpPackets, .extensionLights = source.extensionLights});
    projectLinkedRange(context.images, options.resourceIndexMaps.images);
    projectLinkedRange(context.textures, options.resourceIndexMaps.textures);
    projectLinkedRange(context.samplers, options.resourceIndexMaps.samplers);
  }
  detail::compactLinkedExportResources(model, contexts,
      options.sourceMaterialToExport, options.retiredMaterials, options.animatedLinkedMaterials);
  writePackedScene(model, destination, {}, patch);
}

}  // namespace container::geometry::gltf
