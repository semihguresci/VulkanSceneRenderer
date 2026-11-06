#include <Container/geometry/GltfLinkedAssets.h>

#include <Container/geometry/GltfMerge.h>
#include <Container/geometry/GltfSceneCatalog.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace container::geometry::gltf {
namespace {

using Json = nlohmann::json;
using Path = std::filesystem::path;
using Aliases = std::map<Path, Path>;
constexpr size_t kMaxTableEntries = 4096;
constexpr size_t kMaxSources = 64;
constexpr size_t kMaxDepth = 16;
constexpr size_t kMaxPlacements = 4096;
constexpr size_t kMaxNodes = 262144;
constexpr char kOriginKey[] = "containerLinkedSource";

std::string utf8(const Path& path) {
  const auto value = path.generic_u8string();
  return {value.begin(), value.end()};
}

[[noreturn]] void fail(std::string_view context, std::string_view message) {
  throw std::runtime_error("glTF linked assets: " + std::string(context) + ": " + std::string(message));
}

int hex(char digit) {
  if (digit >= '0' && digit <= '9') return digit - '0';
  if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
  if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
  return -1;
}

Path relativeUri(std::string_view uri, std::string_view context) {
  if (uri.starts_with("data:")) fail(context, "embedded/data-URI linked files are unsupported");
  if (uri.empty() || uri.size() > 4096) fail(context, "expected a nonempty bounded relative URI");
  std::u8string decoded;
  decoded.reserve(uri.size());
  for (size_t i = 0; i < uri.size(); ++i) {
    unsigned char value = static_cast<unsigned char>(uri[i]);
    if (value == '%') {
      if (uri.size() - i < 3 || hex(uri[i + 1]) < 0 || hex(uri[i + 2]) < 0)
        fail(context, "malformed percent escape in URI");
      value = static_cast<unsigned char>(16 * hex(uri[i + 1]) + hex(uri[i + 2]));
      if (value == '/') fail(context, "encoded path separators are unsupported");
      i += 2;
    }
    if (value < 32 || value == 127 || value == '\\' || value == ':' || value == '?' || value == '#')
      fail(context, "expected a local relative file URI without a query or fragment");
    decoded.push_back(static_cast<char8_t>(value));
  }
  const Path path(decoded);
  if (path.has_root_path() || !path.has_filename()) fail(context, "expected a relative file URI");
  const auto normalized = path.lexically_normal();
  if (!normalized.has_filename() || normalized.filename() == "." || normalized.filename() == "..")
    fail(context, "URI does not name a file");
  return normalized;
}

bool sameFile(const Path& left, const Path& right) {
  if (left == right) return true;
  std::error_code error;
  return std::filesystem::equivalent(left, right, error);
}

Path containedFile(const Path& root, const Path& owner, const Path& relative) {
  const auto candidate = owner.parent_path() / relative;
  std::error_code error;
  auto path = std::filesystem::canonical(candidate, error);
  if (error || !std::filesystem::is_regular_file(path, error) || error)
    fail(utf8(owner), "missing linked file '" + utf8(relative) + "'");
  for (auto ancestor = path.parent_path(); !ancestor.empty();) {
    if (sameFile(ancestor, root)) return path;
    const auto parent = ancestor.parent_path();
    if (ancestor == parent) break;
    ancestor = parent;
  }
  fail(utf8(owner), "linked file '" + utf8(relative) + "' escapes the primary asset directory");
}

std::string_view stringField(const Json& object, const char* name, std::string_view context) {
  const auto field = object.find(name);
  if (field == object.end() || !field->is_string() || field->get_ref<const std::string&>().empty())
    fail(context, std::string("missing or invalid ") + name);
  return field->get_ref<const std::string&>();
}

size_t index(const Json& value, size_t count, std::string_view context) {
  if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0) ||
      value.get<uint64_t>() >= count)
    fail(context, "index is outside its source table");
  return value.get<size_t>();
}

const Json& table(const Json& manifest, const char* key) {
  const auto found = manifest.find(key);
  if (found == manifest.end() || !found->is_array() || found->size() > kMaxTableEntries)
    fail(key, "expected a bounded draft table");
  return *found;
}

std::vector<int> roots(const tinygltf::Model& model) {
  const auto catalog = container::scene::BuildGltfSceneCatalog(model);
  return catalog.entries[catalog.defaultScene].sourceRoots;
}

bool sameContext(const GltfLinkedReadContext& left, const GltfLinkedReadContext& right) {
  if (!sameFile(left.source, right.source) || left.aliases.size() != right.aliases.size()) return false;
  return std::ranges::equal(left.aliases, right.aliases, [](const auto& a, const auto& b) {
    return a.first == b.first && sameFile(a.second, b.second);
  });
}

class Composition {
 public:
  Composition(const GltfLinkedDocumentReader& reader, GltfLinkedReadContext primary,
      GltfLinkedDocument document)
      : reader_(reader), output_(std::move(document.model)) {
    if (document.manifest.nodeFiles.size() != output_.nodes.size())
      fail(utf8(primary.source), "parsed node table does not match the linked manifest");
    if (output_.nodes.size() > kMaxNodes) fail(utf8(primary.source), "primary node budget exceeded");
    if (output_.scenes.empty()) {
      tinygltf::Scene scene;
      scene.nodes = roots(output_);
      output_.scenes.push_back(std::move(scene));
      output_.defaultScene = 0;
    }
    ancestry_.push_back(primary.source);
    for (size_t node = 0; node < document.manifest.nodeFiles.size(); ++node) {
      const int file = document.manifest.nodeFiles[node];
      if (file >= 0) place(int(node), primary, document.manifest, size_t(file));
    }
  }

  tinygltf::Model take() && { return std::move(output_); }

 private:
  struct Source {
    GltfLinkedReadContext context;
    GltfLinkedManifest manifest;
    std::vector<tinygltf::Node> nodes;
    std::vector<int> roots;
  };

  Source& acquire(GltfLinkedReadContext context) {
    for (const auto& source : sources_)
      if (sameContext(source->context, context)) return *source;
    if (sources_.size() >= kMaxSources) fail(utf8(context.source), "linked source count exceeds 64");

    auto child = reader_(context);
    if (!child.manifest.nodeFiles.empty() && child.manifest.nodeFiles.size() != child.model.nodes.size())
      fail(utf8(context.source), "parsed node table does not match the linked manifest");
    if (!child.model.animations.empty() || !child.model.skins.empty())
      fail(utf8(context.source), "linked skinning and animation are unsupported in this draft profile");
    if (child.model.nodes.size() > kMaxNodes - retainedNodes_)
      fail(utf8(context.source), "linked source node budget exceeded");
    retainedNodes_ += child.model.nodes.size();

    auto source = std::make_unique<Source>();
    source->context = std::move(context);
    source->manifest = std::move(child.manifest);
    source->roots = roots(child.model);
    // The linked document's root/asset annotations describe that document,
    // while the flattened scene retains the primary document's annotations.
    child.model.extras = {};
    child.model.asset.extras = {};
    child.model.asset.copyright.clear();
    const size_t firstNode = output_.nodes.size();
    const size_t sceneCount = output_.scenes.size();
    GltfMergePlan merged;
    try {
      merged = PlanGltfMerge(output_, child.model);
    } catch (const std::runtime_error& error) {
      fail(utf8(source->context.source), error.what());
    }
    // Keep the merge's shared resource tables and canonical remapped node
    // prototypes. The normal merge's placement and imported scenes are not
    // part of linked import, where the manifest already names each target.
    source->nodes.reserve(child.model.nodes.size());
    for (size_t i = 0; i < child.model.nodes.size(); ++i) {
      auto node = std::move(merged.model.nodes[firstNode + i]);
      for (auto& descendant : node.children) descendant -= int(firstNode);
      source->nodes.push_back(std::move(node));
    }
    merged.model.nodes.resize(firstNode);
    merged.model.scenes.resize(sceneCount);
    std::erase(merged.model.scenes[size_t(merged.destinationScene)].nodes, merged.parentNode);
    merged.model.defaultScene = output_.defaultScene;
    output_ = std::move(merged.model);
    return *sources_.emplace_back(std::move(source));
  }

  void place(int target, const GltfLinkedReadContext& owner, const GltfLinkedManifest& manifest, size_t id) {
    if (ancestry_.size() > kMaxDepth) fail(utf8(owner.source), "linked nesting exceeds 16 levels");
    if (++placements_ > kMaxPlacements) fail(utf8(owner.source), "linked placement count exceeds 4096");
    const auto& file = manifest.files[id];
    auto path = containedFile(owner.root, owner.source, file.uri);
    if (std::ranges::any_of(ancestry_, [&](const auto& parent) { return sameFile(parent, path); }))
      fail(utf8(owner.source), "reference cycle through '" + utf8(file.uri) + "'");
    auto extension = utf8(path.extension());
    std::ranges::transform(extension, extension.begin(), [](char value) {
      return value >= 'A' && value <= 'Z' ? char(value - 'A' + 'a') : value;
    });
    if ((file.mimeType == "model/gltf+json" && extension != ".gltf") ||
        (file.mimeType == "model/gltf-binary" && extension != ".glb"))
      fail(utf8(owner.source), "linked file MIME type and suffix disagree for '" + utf8(file.uri) + "'");
    GltfLinkedReadContext context{owner.root, std::move(path), {}};
    for (const auto& [name, destination] : file.aliases)
      context.aliases.emplace(name, containedFile(owner.root, owner.source, manifest.files[destination].uri));
    const auto& source = acquire(std::move(context));
    ancestry_.push_back(source.context.source);

    std::vector<std::pair<int, int>> pending;
    std::vector<std::pair<int, size_t>> nested;
    for (auto it = source.roots.rbegin(); it != source.roots.rend(); ++it) pending.emplace_back(target, *it);
    const auto origin = utf8(source.context.source.lexically_relative(owner.root));
    while (!pending.empty()) {
      const auto [parent, prototype] = pending.back();
      pending.pop_back();
      if (output_.nodes.size() >= kMaxNodes) fail(utf8(owner.source), "linked output node budget exceeded");
      const auto& definition = source.nodes[size_t(prototype)];
      auto node = definition;
      node.children.clear();
      using Value = tinygltf::Value;
      Value::Object extras;
      if (node.extras.IsObject()) extras = std::move(node.extras.Get<Value::Object>());
      else if (node.extras.Type() != tinygltf::NULL_TYPE) extras.emplace("sourceExtras", std::move(node.extras));
      if (!extras.emplace(kOriginKey, Value(Value::Object{{"uri", Value(origin)}, {"node", Value(prototype)}})).second)
        fail(utf8(source.context.source), "node extras already use the reserved linked-source key");
      node.extras = Value(std::move(extras));
      node.extras_json_string.clear();
      const int destination = int(output_.nodes.size());
      output_.nodes[size_t(parent)].children.push_back(destination);
      output_.nodes.push_back(std::move(node));
      if (!source.manifest.nodeFiles.empty() && source.manifest.nodeFiles[size_t(prototype)] >= 0)
        nested.emplace_back(destination, size_t(source.manifest.nodeFiles[size_t(prototype)]));
      for (auto it = definition.children.rbegin(); it != definition.children.rend(); ++it)
        pending.emplace_back(destination, *it);
    }
    for (const auto& [node, fileIndex] : nested) place(node, source.context, source.manifest, fileIndex);
    ancestry_.pop_back();
  }

  const GltfLinkedDocumentReader& reader_;
  tinygltf::Model output_;
  std::vector<std::unique_ptr<Source>> sources_;
  std::vector<Path> ancestry_;
  size_t placements_{}, retainedNodes_{};
};

} // namespace

GltfLinkedManifest PrepareGltfLinkedManifest(Json& manifest, bool enabled) {
  const auto nodes = manifest.find("nodes");
  const bool references = nodes != manifest.end() && nodes->is_array() &&
      std::ranges::any_of(*nodes, [](const auto& node) { return node.contains("externalAsset"); });
  const bool hasLinks = references || manifest.contains("externalAssets");
  const auto assetRecord = manifest.find("asset");
  const bool optedDraft = enabled && assetRecord != manifest.end() && assetRecord->is_object() &&
      assetRecord->contains("version") && (*assetRecord)["version"] == "2.1";
  if (!hasLinks && !optedDraft) return {};
  if (!enabled) fail("manifest", "enable experimental glTF linked assets to load this file");
  auto& asset = manifest.at("asset");
  if (stringField(asset, "version", "asset") != "2.1")
    fail("asset.version", "linked assets require the pinned glTF 2.1 draft profile");
  if (asset.contains("minVersion")) {
    const auto minimum = stringField(asset, "minVersion", "asset");
    if (minimum != "2.0" && minimum != "2.1") fail("asset.minVersion", "unsupported linked asset minimum version");
    asset["minVersion"] = "2.0";
  }
  GltfLinkedManifest result;
  result.normalized = true;
  if (!hasLinks) {
    if (manifest.contains("files")) fail("files", "the draft file table is supported only for external assets and aliases");
    asset["version"] = "2.0";
    return result;
  }
  const auto& files = table(manifest, "files");
  const auto& external = table(manifest, "externalAssets");
  result.files.reserve(files.size());
  size_t aliasCount = 0;
  for (size_t i = 0; i < files.size(); ++i) {
    const auto& file = files[i];
    const auto label = "files[" + std::to_string(i) + "]";
    if (file.contains("bufferView") || file.contains("data")) fail(label, "embedded linked files are unsupported");
    GltfLinkedFile entry{relativeUri(stringField(file, "uri", label), label),
        std::string(stringField(file, "mimeType", label)), {}};
    if (auto aliases = file.find("aliases"); aliases != file.end()) {
      if (!aliases->is_array() || aliases->size() > kMaxTableEntries - aliasCount)
        fail(label, "alias table exceeds its budget");
      aliasCount += aliases->size();
      for (const auto& alias : *aliases) {
        const auto name = relativeUri(stringField(alias, "alias", label), label);
        const auto target = alias.find("file");
        if (target == alias.end()) fail(label, "alias is missing its file index");
        if (!entry.aliases.emplace(name, index(*target, files.size(), label)).second)
          fail(label, "duplicate normalized alias '" + utf8(name) + "'");
      }
    }
    result.files.push_back(std::move(entry));
  }
  std::vector<int> externalFiles;
  externalFiles.reserve(external.size());
  for (size_t i = 0; i < external.size(); ++i) {
    const auto& entry = external[i];
    const auto label = "externalAssets[" + std::to_string(i) + "]";
    const auto file = entry.find("file");
    if (file == entry.end()) fail(label, "missing file index");
    const auto id = index(*file, result.files.size(), label);
    const auto& mime = result.files[id].mimeType;
    if (mime != "model/gltf+json" && mime != "model/gltf-binary") fail(label, "unsupported linked model MIME type");
    externalFiles.push_back(int(id));
  }
  if (references) {
    if (nodes->size() > kMaxNodes) fail("nodes", "linked manifest node budget exceeded");
    result.nodeFiles.assign(nodes->size(), -1);
    for (size_t i = 0; i < nodes->size(); ++i) {
      auto& node = (*nodes)[i];
      const auto reference = node.find("externalAsset");
      if (reference == node.end()) continue;
      const auto label = "nodes[" + std::to_string(i) + "].externalAsset";
      result.nodeFiles[i] = externalFiles[index(*reference, externalFiles.size(), label)];
      const auto extensions = node.find("extensions");
      if (node.contains("skin") || (extensions != node.end() && extensions->contains("EXT_mesh_gpu_instancing")))
        fail(label, "an externalAsset node cannot also bind a skin or GPU instances");
      node.erase(reference);
      // The placing node remains editable. Its visual payload comes from the
      // external document, while its transform and own children are retained.
      node.erase("mesh");
      node.erase("camera");
      node.erase("weights");
    }
  }
  asset["version"] = "2.0";
  manifest.erase("externalAssets");
  manifest.erase("files");
  return result;
}

Path GltfLinkedReadContext::resource(std::string_view uri) const {
  const auto relative = relativeUri(uri, utf8(source));
  const auto mapped = aliases.find(relative);
  return mapped == aliases.end() ? containedFile(root, source, relative) : mapped->second;
}

tinygltf::Model LoadGltfLinkedComposition(const Path& path, const GltfLinkedDocumentReader& reader) {
  std::error_code error;
  auto source = std::filesystem::canonical(path, error);
  if (error || !std::filesystem::is_regular_file(source, error) || error)
    fail(utf8(path), "primary glTF file is missing");
  GltfLinkedReadContext primary{source.parent_path(), std::move(source), {}};
  auto document = reader(primary);
  if (document.manifest.nodeFiles.empty()) return std::move(document.model);
  return Composition(reader, std::move(primary), std::move(document)).take();
}

} // namespace container::geometry::gltf
