#include <Container/geometry/GltfPackedSceneExport.h>
#include <Container/geometry/GltfLinkedAssets.h>
#include <Container/geometry/GltfNodeExportCompaction.h>
#include <Container/geometry/GltfXmpMetadata.h>

#include "GltfExportCommon.h"
#include "GltfLinkedExportResources.h"
#include "GltfSourceJson.h"

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace container::geometry::gltf {
namespace {

using detail::Bytes;
using detail::fail;
using detail::readBytes;
using detail::validateView;

std::string lower(std::string value) {
  for (auto& c : value)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  return value;
}

std::string utf8Path(const std::filesystem::path& path) {
  const auto utf8 = path.u8string();
  return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
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
  const auto alreadyInstalled = [&] {
    const auto status = std::filesystem::symlink_status(destination);
    if (!std::filesystem::exists(status)) return false;
    if (!std::filesystem::is_regular_file(status) ||
        std::filesystem::file_size(destination) != expected.size() || readBytes(destination) != expected)
      fail("existing export dependency differs from its content-addressed name");
    return true;
  };
  if (alreadyInstalled()) return;
  if (std::filesystem::file_size(staged) != expected.size()) fail("serializer produced an incomplete export dependency");
  // The staged resource is complete and on the destination filesystem.
  // Publish it exclusively so concurrent saves can safely reuse the winner.
  std::error_code error;
#ifdef _WIN32
  if (MoveFileExW(staged.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) return;
  error = {static_cast<int>(GetLastError()), std::system_category()};
#else
  std::filesystem::create_hard_link(staged, destination, error);
  if (!error) return;
#if defined(__linux__) && defined(SYS_renameat2)
  if (error != std::errc::file_exists) {
    if (syscall(SYS_renameat2, AT_FDCWD, staged.c_str(), AT_FDCWD, destination.c_str(), RENAME_NOREPLACE) == 0)
      return;
    error = {errno, std::generic_category()};
  }
#endif
#endif
  if (!alreadyInstalled()) fail("cannot install export dependency: " + error.message());
}

}  // namespace

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

void verifyStagedGlb(const std::filesystem::path& staged, uint64_t binBytes, uint64_t size) {
  std::ifstream input(staged, std::ios::binary);
  std::array<unsigned char, detail::kGlbJsonOffset> prefix{};
  if (!input.read(reinterpret_cast<char*>(prefix.data()), prefix.size())) fail("staged GLB is missing its header");
  const auto jsonBytes = detail::GlbJsonChunkLength(prefix, size);
  if (!jsonBytes || !*jsonBytes || *jsonBytes % 4) fail("staged GLB has an invalid header or JSON chunk");
  const auto jsonEnd = detail::kGlbJsonOffset + uint64_t(*jsonBytes);
  const auto paddedBin = (binBytes + 3) & ~uint64_t{3};
  if (!binBytes && size == jsonEnd) return;
  if (size - jsonEnd != paddedBin + 8) fail("staged GLB has an incomplete binary chunk");
  input.seekg(static_cast<std::streamoff>(jsonEnd));
  std::array<unsigned char, 8> bin{};
  if (!input.read(reinterpret_cast<char*>(bin.data()), bin.size())) fail("staged GLB is missing its binary chunk header");
  const auto length = uint32_t(bin[0]) | (uint32_t(bin[1]) << 8) | (uint32_t(bin[2]) << 16) | (uint32_t(bin[3]) << 24);
  if (length != paddedBin || bin[4] != 'B' || bin[5] != 'I' || bin[6] != 'N' || bin[7] != 0)
    fail("staged GLB has an invalid binary chunk header");
}

void writePackedScene(const tinygltf::Model& packedModel,
                      const std::filesystem::path& destination,
                      const GltfLegacyMaterialSerializationLimits& legacyLimits,
                      const nlohmann::json& linkedPatch = {}) {
  const auto extension = lower(destination.extension().string());
  if (extension != ".gltf" && extension != ".glb")
    fail("destination must have a .gltf or .glb extension");
  const bool binary = extension == ".glb";
  if (packedModel.buffers.size() > 1) fail("writeExportModel requires a packed export model");
  const uint64_t binBytes = packedModel.buffers.empty() ? 0 : packedModel.buffers.front().data.size();
  if (binary && binBytes > std::numeric_limits<uint32_t>::max() - 32)
    fail("packed buffer exceeds the GLB container length");
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
  DropEmptyGltfExportAnimations(model);
  if (!xmpPackage.empty()) model.extensions.erase(kGltfXmpExtension);
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
  if (!std::filesystem::is_regular_file(stagedFile))
    fail("serializer produced no scene file");
  const auto stagedSize = std::filesystem::file_size(stagedFile);
  if (!stagedSize) fail("serializer produced an empty scene file");
  if (binary) verifyStagedGlb(stagedFile, binBytes, stagedSize);
  if (!linkedPatch.empty()) {
    if (stagedSize > legacyLimits.maxJsonBytes)
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
    DropEmptyGltfExportAnimations(model);
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
