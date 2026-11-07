#include <Container/geometry/GltfLinkedAssets.h>

#include <Container/geometry/GltfMerge.h>
#include <Container/geometry/GltfInteractivity.h>
#include <Container/geometry/GltfXmpMetadata.h>
#include <Container/geometry/GltfSceneCatalog.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
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
constexpr uint64_t kMaxWitnessBytes = 64ull * 1024 * 1024;
constexpr char kOriginKey[] = "containerLinkedSource";
constexpr char kAnimationPointer[] = "KHR_animation_pointer";
constexpr std::string_view kNodePointerPrefix = "/nodes/";
constexpr GltfMergeLimits kMergeLimits{};

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

Path containedFile(const Path& root, const Path& owner, const Path& relative,
    GltfLinkedCompositionInfo* composition, std::set<Path>* witnesses = nullptr) {
  const auto candidate = owner.parent_path() / relative;
  std::error_code error;
  auto path = std::filesystem::canonical(candidate, error);
  if (error || !std::filesystem::is_regular_file(path, error) || error)
    fail(utf8(owner), "missing linked file '" + utf8(relative) + "'");
  for (auto ancestor = path.parent_path(); !ancestor.empty();) {
    if (sameFile(ancestor, root)) {
      if (composition) {
        const auto [prior, added] = composition->resolutions.emplace(candidate, path);
        if (!added && prior->second != path)
          fail(utf8(owner), "linked URI resolution changed during import");
        if (witnesses) composition->recordWitness(*witnesses, prior->first);
      }
      return path;
    }
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
  // Hard links in different directories can resolve the same relative
  // resource URI to different bytes. The resolved document path is part of
  // the read context even when the document itself is physically shared.
  return left.source == right.source && left.aliases == right.aliases;
}

GltfLinkedIndexRange importedRange(std::span<const int32_t> indices) {
  // The merge planner appends a contiguous range for each resource table.
  return {indices.empty() ? 0 : indices.front(), indices.size()};
}

size_t xmpPacketCount(const tinygltf::Model& model) {
  const auto package = ReadGltfXmpPackage(model);
  if (!package) return 0;
  const auto packets = package->find("packets");
  return packets != package->end() && packets->is_array() ? packets->size() : 0;
}

const std::string& animationPointer(const tinygltf::AnimationChannel& channel) {
  return channel.target_extensions.at(kAnimationPointer).Get("pointer").Get<std::string>();
}

std::optional<int> animationNode(const tinygltf::AnimationChannel& channel) {
  if (channel.target_path != "pointer") return channel.target_node;
  const auto& pointer = animationPointer(channel);
  if (!pointer.starts_with(kNodePointerPrefix)) return std::nullopt;
  const auto slash = pointer.find('/', kNodePointerPrefix.size());
  int node = -1;
  if (slash == std::string::npos) fail("animation pointer", "missing node property");
  const auto end = pointer.data() + slash;
  const auto parsed = std::from_chars(pointer.data() + kNodePointerPrefix.size(), end, node);
  if (parsed.ec != std::errc{} || parsed.ptr != end || node < 0)
    fail("animation pointer", "invalid node index");
  return node;
}

void setAnimationNode(tinygltf::AnimationChannel& channel, int node) {
  if (channel.target_path != "pointer") {
    channel.target_node = node;
    return;
  }
  const auto& pointer = animationPointer(channel);
  const auto slash = pointer.find('/', kNodePointerPrefix.size());
  channel.target_extensions.at(kAnimationPointer).Get<tinygltf::Value::Object>()["pointer"] =
      tinygltf::Value(std::string(kNodePointerPrefix) + std::to_string(node) + pointer.substr(slash));
  channel.target_extensions_json_string.clear();
}

struct AnimationCost {
  uint64_t channels{}, decodedBytes{};

  void add(AnimationCost cost, std::string_view context) {
    if (cost.channels > kMergeLimits.maxAnimationChannels - channels ||
        cost.decodedBytes > kMergeLimits.maxAnimationDecodedBytes - decodedBytes)
      fail(context, "linked animation expansion exceeds its channel or decoded byte budget");
    channels += cost.channels;
    decodedBytes += cost.decodedBytes;
  }

  void add(const tinygltf::Model& model, const tinygltf::Animation& animation,
           const tinygltf::AnimationChannel& channel, std::string_view context) {
    if (channel.sampler < 0 || size_t(channel.sampler) >= animation.samplers.size())
      fail(context, "linked animation channel has an invalid sampler");
    const auto& sampler = animation.samplers[size_t(channel.sampler)];
    add({1, 0}, context);
    for (const int id : {sampler.input, sampler.output}) {
      if (id < 0 || size_t(id) >= model.accessors.size())
        fail(context, "linked animation sampler has an invalid accessor");
      const auto& accessor = model.accessors[size_t(id)];
      const int components = tinygltf::GetNumComponentsInType(uint32_t(accessor.type));
      if (components <= 0 || accessor.count > kMergeLimits.maxAnimationDecodedBytes /
              (size_t(components) * sizeof(double)))
        fail(context, "linked animation accessor exceeds its decoded byte budget");
      add({0, uint64_t(accessor.count) * size_t(components) * sizeof(double)}, context);
    }
  }
};

class Composition {
 public:
  Composition(const GltfLinkedDocumentReader& reader, GltfLinkedReadContext primary,
      GltfLinkedDocument document)
      : reader_(reader), composition_(primary.composition), output_(std::move(document.model)) {
    if (document.manifest.nodeFiles.size() != output_.nodes.size())
      fail(utf8(primary.source), "parsed node table does not match the linked manifest");
    if (output_.nodes.size() > kMaxNodes) fail(utf8(primary.source), "primary node budget exceeded");
    if (composition_) nodePlacements_.resize(output_.nodes.size(), -1);
    const auto label = utf8(primary.source);
    for (const auto& animation : output_.animations)
      for (const auto& channel : animation.channels)
        animationCost_.add(output_, animation, channel, label);
    if (output_.scenes.empty()) {
      tinygltf::Scene scene;
      scene.nodes = roots(output_);
      output_.scenes.push_back(std::move(scene));
      output_.defaultScene = 0;
    }
    ancestry_.push_back(primary.source);
    for (size_t node = 0; node < document.manifest.nodeFiles.size(); ++node) {
      const int file = document.manifest.nodeFiles[node];
      if (file >= 0) place(int(node), primary, document.manifest, size_t(file), -1);
    }
  }

  tinygltf::Model take() && {
    if (composition_) {
      std::vector<GltfLinkedSourceOwnership> sourceContexts;
      sourceContexts.reserve(sources_.size());
      for (auto& source : sources_) {
        source->ownership.source = std::move(source->context.source);
        source->ownership.aliases = std::move(source->context.aliases);
        sourceContexts.push_back(std::move(source->ownership));
      }
      composition_->sourceContexts = std::move(sourceContexts);
      composition_->placementRecords = std::move(placementRecords_);
      composition_->nodePlacements = std::move(nodePlacements_);
      composition_->bindingResolutions = std::move(bindingResolutions_);
    }
    return std::move(output_);
  }

 private:
  struct NodePrototype {
    tinygltf::Node node;
    int original, nestedFile;
  };

  struct AnimationPlacement {
    size_t clip;
    std::vector<std::pair<int, tinygltf::AnimationChannel>> channels;
  };

  struct Source {
    GltfLinkedReadContext context;
    size_t id{};
    GltfLinkedSourceOwnership ownership;
    GltfLinkedManifest manifest;
    std::vector<NodePrototype> nodes;
    std::vector<int> roots;
    std::vector<tinygltf::Skin> skins;
    std::vector<AnimationPlacement> animations;
    AnimationCost animationCost;
    uint64_t placementReferences{}, placementMetadataBytes{};
    std::optional<tinygltf::Value> assetMetadata, sceneMetadata;
  };

  Source& acquire(GltfLinkedReadContext context) {
    for (const auto& source : sources_)
      if (sameContext(source->context, context)) return *source;
    if (sources_.size() >= kMaxSources) fail(utf8(context.source), "linked source count exceeds 64");

    GltfLinkedReadWitnesses witnesses;
    if (composition_) context.witnesses = &witnesses;
    auto child = reader_(context);
    context.witnesses = nullptr;
    if (!child.manifest.nodeFiles.empty() && child.manifest.nodeFiles.size() != child.model.nodes.size())
      fail(utf8(context.source), "parsed node table does not match the linked manifest");
    if (child.model.extensions.contains(kGltfInteractivityExtension))
      fail(utf8(context.source), "linked behavior graphs require independent runtime instances");
    if (child.model.nodes.size() > kMaxNodes - retainedNodes_)
      fail(utf8(context.source), "linked source node budget exceeded");
    retainedNodes_ += child.model.nodes.size();

    auto source = std::make_unique<Source>();
    source->context = std::move(context);
    source->id = sources_.size();
    source->ownership.reads = std::move(witnesses);
    const auto label = utf8(source->context.source);
    source->manifest = std::move(child.manifest);
    // The linked document's root/asset annotations describe that document,
    // while the flattened scene retains the primary document's annotations.
    child.model.extras = {};
    child.model.asset.extras = {};
    child.model.asset.copyright.clear();
    const size_t firstNode = output_.nodes.size();
    const size_t firstSkin = output_.skins.size();
    const size_t firstAnimation = output_.animations.size();
    const size_t sceneCount = output_.scenes.size();
    const auto firstXmpPacket = composition_ ? xmpPacketCount(output_) : 0;
    GltfMergePlan merged;
    try {
      merged = PlanGltfMerge(output_, child.model);
    } catch (const std::runtime_error& error) {
      fail(label, error.what());
    }
    if (std::ranges::find(merged.placement.contextOnly, true) != merged.placement.contextOnly.end())
      fail(label, "skin joints and skeleton must belong to the selected linked scene");
    if (const auto packet = merged.incomingAsset.extensions.find(kGltfXmpExtension);
        packet != merged.incomingAsset.extensions.end())
      source->assetMetadata = std::move(packet->second);
    auto& scene = merged.model.scenes[size_t(merged.incomingScene)];
    if (const auto packet = scene.extensions.find(kGltfXmpExtension); packet != scene.extensions.end())
      source->sceneMetadata = std::move(packet->second);
    // Reuse the selected scene from merge. Linked import places its nodes and
    // independent skin bindings under each authored link.
    const int firstPrototype = merged.parentNode + 1;
    const int firstSkinPrototype = int(firstSkin + child.model.skins.size());
    source->roots = std::move(merged.model.nodes[size_t(merged.parentNode)].children);
    for (auto& root : source->roots) root -= firstPrototype;
    source->nodes.reserve(size_t(merged.summary.placementNodes));
    for (size_t i = 0; i < child.model.nodes.size(); ++i) {
      const int placed = merged.placement.nodes[i];
      if (placed < 0) continue;
      auto node = std::move(merged.model.nodes[size_t(placed)]);
      for (auto& descendant : node.children) descendant -= firstPrototype;
      if (node.skin >= 0) node.skin -= firstSkinPrototype;
      const int nestedFile = source->manifest.nodeFiles.empty() ? -1 : source->manifest.nodeFiles[i];
      source->nodes.push_back({std::move(node), int(i), nestedFile});
    }
    for (size_t i = size_t(firstSkinPrototype); i < merged.model.skins.size(); ++i) {
      auto skin = std::move(merged.model.skins[i]);
      for (auto& joint : skin.joints) joint -= firstPrototype;
      if (skin.skeleton >= 0) skin.skeleton -= firstPrototype;
      source->skins.push_back(std::move(skin));
    }
    if (!source->skins.empty()) {
      std::vector<int> rootByNode(source->nodes.size(), -1);
      std::vector<int> pending;
      for (const int root : source->roots) {
        pending.push_back(root);
        while (!pending.empty()) {
          const int node = pending.back();
          pending.pop_back();
          rootByNode[size_t(node)] = root;
          const auto& children = source->nodes[size_t(node)].node.children;
          pending.insert(pending.end(), children.begin(), children.end());
        }
      }
      for (const auto& skin : source->skins) {
        const int root = rootByNode[size_t(skin.joints.front())];
        if (std::ranges::any_of(skin.joints, [&](int joint) { return rootByNode[size_t(joint)] != root; }) ||
            (skin.skeleton >= 0 && rootByNode[size_t(skin.skeleton)] != root))
          fail(label, "skin joints and skeleton must have a common root in the selected linked scene");
      }
    }
    source->placementReferences = merged.summary.placementReferences;
    source->placementMetadataBytes = merged.summary.placementMetadataBytes;

    std::vector<tinygltf::Animation> clips;
    for (size_t i = 0; i < child.model.animations.size(); ++i) {
      auto clip = std::move(merged.model.animations[firstAnimation + i]);
      const size_t originalCount = child.model.animations[i].channels.size();
      AnimationPlacement placement{firstAnimation + clips.size(), {}};
      std::vector<tinygltf::AnimationChannel> shared;
      for (size_t j = 0; j < clip.channels.size(); ++j) {
        auto& channel = clip.channels[j];
        const auto node = animationNode(channel);
        if (!node) {
          animationCost_.add(merged.model, clip, channel, label);
          shared.push_back(std::move(channel));
        } else if (j >= originalCount) {
          source->animationCost.add(merged.model, clip, channel, label);
          placement.channels.emplace_back(*node - firstPrototype, std::move(channel));
        }
      }
      if (shared.empty() && placement.channels.empty()) continue;
      clip.channels = std::move(shared);
      source->animations.push_back(std::move(placement));
      clips.push_back(std::move(clip));
    }

    if (composition_) {
      const auto& imported = merged.imported;
      auto& ownership = source->ownership;
      ownership.buffers = importedRange(imported.buffers);
      ownership.bufferViews = importedRange(imported.bufferViews);
      ownership.accessors = importedRange(imported.accessors);
      ownership.images = importedRange(imported.images);
      ownership.samplers = importedRange(imported.samplers);
      ownership.textures = importedRange(imported.textures);
      ownership.materials = importedRange(imported.materials);
      ownership.meshes = importedRange(imported.meshes);
      ownership.cameras = importedRange(imported.cameras);
      ownership.lights = importedRange(imported.lights);
      ownership.animations = {int32_t(firstAnimation), clips.size()};
      ownership.variants = importedRange(imported.variants);
      const auto packetCount = xmpPacketCount(merged.model);
      if (packetCount < firstXmpPacket || packetCount > size_t(INT32_MAX))
        fail(label, "invalid linked XMP packet range");
      ownership.xmpPackets = {int32_t(firstXmpPacket), packetCount - firstXmpPacket};
      for (const auto& [name, ids] : imported.extensionLights)
        ownership.extensionLights.emplace(name, importedRange(ids));
    }

    merged.model.nodes.resize(firstNode);
    merged.model.skins.resize(firstSkin);
    merged.model.animations.resize(firstAnimation);
    for (auto& clip : clips) merged.model.animations.push_back(std::move(clip));
    merged.model.scenes.resize(sceneCount);
    std::erase(merged.model.scenes[size_t(merged.destinationScene)].nodes, merged.parentNode);
    merged.model.defaultScene = output_.defaultScene;
    output_ = std::move(merged.model);
    return *sources_.emplace_back(std::move(source));
  }

  void place(int target, const GltfLinkedReadContext& owner, const GltfLinkedManifest& manifest,
             size_t id, int32_t parentPlacement) {
    if (ancestry_.size() > kMaxDepth) fail(utf8(owner.source), "linked nesting exceeds 16 levels");
    if (++placements_ > kMaxPlacements) fail(utf8(owner.source), "linked placement count exceeds 4096");
    const auto& file = manifest.files[id];
    size_t bindingWitness = 0;
    std::set<Path>* binding = nullptr;
    if (composition_) {
      const size_t ownerContext = parentPlacement < 0 ? 0
          : placementRecords_[size_t(parentPlacement)].sourceContext + 1;
      const auto [entry, added] = bindings_.try_emplace(std::pair{ownerContext, id}, bindingResolutions_.size());
      bindingWitness = entry->second;
      if (added) binding = &bindingResolutions_.emplace_back();
    }
    auto path = containedFile(owner.root, owner.source, file.uri, owner.composition, binding);
    if (std::ranges::any_of(ancestry_, [&](const auto& parent) { return sameFile(parent, path); }))
      fail(utf8(owner.source), "reference cycle through '" + utf8(file.uri) + "'");
    auto extension = utf8(path.extension());
    std::ranges::transform(extension, extension.begin(), [](char value) {
      return value >= 'A' && value <= 'Z' ? char(value - 'A' + 'a') : value;
    });
    if ((file.mimeType == "model/gltf+json" && extension != ".gltf") ||
        (file.mimeType == "model/gltf-binary" && extension != ".glb"))
      fail(utf8(owner.source), "linked file MIME type and suffix disagree for '" + utf8(file.uri) + "'");
    GltfLinkedReadContext context{owner.root, std::move(path), {}, owner.composition, true};
    for (const auto& [name, destination] : file.aliases)
      context.aliases.emplace(name, containedFile(owner.root, owner.source,
          manifest.files[destination].uri, owner.composition, binding));
    const auto& source = acquire(std::move(context));
    ancestry_.push_back(source.context.source);

    const auto label = utf8(source.context.source);
    animationCost_.add(source.animationCost, label);
    if (source.placementReferences > kMergeLimits.maxReferences - placementReferences_ ||
        source.placementMetadataBytes > kMergeLimits.maxMetadataBytes - placementMetadataBytes_ ||
        source.skins.size() > kMergeLimits.maxTableEntries - output_.skins.size())
      fail(label, "linked placement expansion exceeds its resource budget");
    placementReferences_ += source.placementReferences;
    placementMetadataBytes_ += source.placementMetadataBytes;
    const int firstSkin = int(output_.skins.size());
    const auto placement = int32_t(placements_ - 1);
    if (composition_)
      placementRecords_.push_back({target, parentPlacement, source.id, id,
          {firstSkin, source.skins.size()}, bindingWitness});
    std::vector<int> placed(source.nodes.size(), -1);
    std::vector<std::pair<int, int>> pending;
    std::vector<std::pair<int, size_t>> nested;
    for (auto it = source.roots.rbegin(); it != source.roots.rend(); ++it) pending.emplace_back(target, *it);
    const auto origin = utf8(source.context.source.lexically_relative(owner.root));
    while (!pending.empty()) {
      const auto [parent, prototype] = pending.back();
      pending.pop_back();
      if (output_.nodes.size() >= kMaxNodes) fail(utf8(owner.source), "linked output node budget exceeded");
      const auto& definition = source.nodes[size_t(prototype)];
      auto node = definition.node;
      node.children.clear();
      if (node.skin >= 0) node.skin += firstSkin;
      using Value = tinygltf::Value;
      Value::Object extras;
      if (node.extras.IsObject()) extras = std::move(node.extras.Get<Value::Object>());
      else if (node.extras.Type() != tinygltf::NULL_TYPE) extras.emplace("sourceExtras", std::move(node.extras));
      if (!extras.emplace(kOriginKey, Value(Value::Object{{"uri", Value(origin)}, {"node", Value(definition.original)}})).second)
        fail(utf8(source.context.source), "node extras already use the reserved linked-source key");
      node.extras = Value(std::move(extras));
      node.extras_json_string.clear();
      const int destination = int(output_.nodes.size());
      placed[size_t(prototype)] = destination;
      output_.nodes[size_t(parent)].children.push_back(destination);
      output_.nodes.push_back(std::move(node));
      if (definition.nestedFile >= 0) nested.emplace_back(destination, size_t(definition.nestedFile));
      for (auto it = definition.node.children.rbegin(); it != definition.node.children.rend(); ++it)
        pending.emplace_back(destination, *it);
    }
    for (auto skin : source.skins) {
      for (auto& joint : skin.joints) joint = placed.at(size_t(joint));
      if (skin.skeleton >= 0) skin.skeleton = placed.at(size_t(skin.skeleton));
      output_.skins.push_back(std::move(skin));
    }
    for (const auto& animation : source.animations) {
      auto& clip = output_.animations[animation.clip];
      for (const auto& [node, prototype] : animation.channels) {
        auto channel = prototype;
        setAnimationNode(channel, placed.at(size_t(node)));
        clip.channels.push_back(std::move(channel));
      }
    }
    // These source contexts have no asset or scene object in the flattened
    // document. Keep their packets separately from the authored placement.
    const auto appendMetadata = [&](const std::optional<tinygltf::Value>& packet, const char* scope) {
      if (!packet) return;
      if (output_.nodes.size() >= kMaxNodes) fail(utf8(owner.source), "linked output node budget exceeded");
      tinygltf::Node metadata;
      metadata.name = utf8(source.context.source.filename()) + " " + scope + " metadata";
      metadata.extensions.emplace(kGltfXmpExtension, *packet);
      using Value = tinygltf::Value;
      metadata.extras = Value(Value::Object{{kOriginKey, Value(Value::Object{
          {"uri", Value(origin)}, {"node", Value(-1)}, {"scope", Value(std::string(scope))}})}});
      output_.nodes[size_t(target)].children.push_back(int(output_.nodes.size()));
      output_.nodes.push_back(std::move(metadata));
    };
    appendMetadata(source.assetMetadata, "asset");
    appendMetadata(source.sceneMetadata, "scene");
    if (composition_) nodePlacements_.resize(output_.nodes.size(), placement);
    for (const auto& [node, fileIndex] : nested)
      place(node, source.context, source.manifest, fileIndex, placement);
    ancestry_.pop_back();
  }

  const GltfLinkedDocumentReader& reader_;
  GltfLinkedCompositionInfo* composition_;
  tinygltf::Model output_;
  std::vector<std::unique_ptr<Source>> sources_;
  std::vector<Path> ancestry_;
  std::vector<GltfLinkedPlacementOwnership> placementRecords_;
  std::vector<int32_t> nodePlacements_;
  std::map<std::pair<size_t, size_t>, size_t> bindings_;
  std::vector<std::set<Path>> bindingResolutions_;
  size_t placements_{}, retainedNodes_{};
  AnimationCost animationCost_;
  uint64_t placementReferences_{}, placementMetadataBytes_{};
};

} // namespace

GltfLinkedManifest PrepareGltfLinkedManifest(Json& manifest, bool enabled) {
  const auto nodes = manifest.find("nodes");
  const bool references = nodes != manifest.end() && nodes->is_array() &&
      std::ranges::any_of(*nodes, [](const auto& node) { return node.contains("externalAsset"); });
  const bool hasLinks = references || manifest.contains("externalAssets");
  const auto assetRecord = manifest.find("asset");
  const bool optedDraft = enabled && assetRecord != manifest.end() && assetRecord->is_object() &&
      assetRecord->contains("version") && (*assetRecord)["version"] == "2.1" &&
      (manifest.contains("files") ||
       (assetRecord->contains("minVersion") && (*assetRecord)["minVersion"] == "2.1"));
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
  return mapped == aliases.end()
      ? containedFile(root, source, relative, composition, witnesses ? &witnesses->resolutions : nullptr)
      : mapped->second;
}

void GltfLinkedReadContext::observe(const Path& path, std::span<const unsigned char> bytes) const {
  if (!composition) return;
  composition->observe(path, bytes);
  if (witnesses) composition->recordWitness(witnesses->inputs, path);
}

tinygltf::Model LoadGltfLinkedComposition(const Path& path, const GltfLinkedDocumentReader& reader,
    GltfLinkedCompositionInfo* composition) {
  std::error_code error;
  auto source = std::filesystem::canonical(path, error);
  if (error || !std::filesystem::is_regular_file(source, error) || error)
    fail(utf8(path), "primary glTF file is missing");
  GltfLinkedReadContext primary{source.parent_path(), std::move(source), {}, composition};
  auto document = reader(primary);
  if (document.manifest.nodeFiles.empty()) return std::move(document.model);
  if (composition) {
    composition->source = primary.source;
    composition->primaryNodes = document.manifest.nodeFiles.size();
    composition->primaryAnimated = !document.model.animations.empty();
    for (size_t node = 0; node < document.manifest.nodeFiles.size(); ++node)
      if (document.manifest.nodeFiles[node] >= 0) composition->placements.push_back(int(node));
  }
  return Composition(reader, std::move(primary), std::move(document)).take();
}

namespace {
constexpr uint64_t kContentHashSeed = 14695981039346656037ull;
uint64_t hashContent(uint64_t hash, std::span<const unsigned char> bytes) {
  for (const auto byte : bytes) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  return hash;
}
} // namespace

void GltfLinkedCompositionInfo::observe(const Path& path, std::span<const unsigned char> bytes) {
  const GltfLinkedFileContents contents{bytes.size(), hashContent(kContentHashSeed, bytes)};
  const auto [previous, added] = inputs.emplace(path, contents);
  if (!added && previous->second != contents) fail(utf8(path), "linked input changed during import");
}

void GltfLinkedCompositionInfo::recordWitness(std::set<Path>& witnesses, const Path& path) {
  if (witnesses.contains(path)) return;
  const auto bytes = 128 + uint64_t(path.native().size()) * sizeof(Path::value_type);
  if (bytes > kMaxWitnessBytes - dependencyWitnessBytes)
    fail(utf8(path), "linked dependency witnesses exceed the ownership budget");
  witnesses.insert(path);
  dependencyWitnessBytes += bytes;
}

void ValidateGltfLinkedExportDestination(const GltfLinkedCompositionInfo& composition, const Path& destination) {
  const auto target = std::filesystem::absolute(destination);
  const auto lexical = target.lexically_normal();
  std::error_code error;
  const auto canonical = std::filesystem::weakly_canonical(target, error);
  const auto matches = [&](const Path& input) {
    return input.lexically_normal() == lexical || (!error && canonical == input) || sameFile(input, target);
  };
  if (std::ranges::any_of(composition.inputs, [&](const auto& input) { return matches(input.first); }) ||
      std::ranges::any_of(composition.resolutions, [&](const auto& path) {
        return matches(path.first) || matches(path.second);
      }))
    fail(utf8(target), "cannot replace a linked input file");
}

namespace {

Path linkedSaveTarget(const GltfLinkedCompositionInfo& composition, const Path& destination) {
  std::error_code error;
  const auto target = std::filesystem::absolute(destination);
  const auto targetRoot = std::filesystem::canonical(target.parent_path(), error);
  if (error || !sameFile(targetRoot, composition.source.parent_path()))
    fail(utf8(target), "save beside the original linked manifest to retain relative URIs and aliases");
  const auto ascii = [](const Path& path) {
    auto extension = utf8(path.extension());
    std::ranges::transform(extension, extension.begin(), [](char value) {
      return value >= 'A' && value <= 'Z' ? char(value - 'A' + 'a') : value;
    });
    return extension == ".gltf";
  };
  if (!ascii(target) || !ascii(composition.source))
    fail(utf8(target), "reference-preserving save currently requires ASCII .gltf manifests");
  if (composition.placements.empty() || !composition.inputs.contains(composition.source))
    fail(utf8(target), "no retained linked placement source");
  ValidateGltfLinkedExportDestination(composition, target);
  return target;
}

void verifyResolution(const Path& requested, const Path& canonical) {
  std::error_code error;
  if (std::filesystem::canonical(requested, error) != canonical || error)
    fail(utf8(requested), "linked URI resolution changed after import");
}

void verifyContents(const Path& path, GltfLinkedFileContents expected, std::string* contents = nullptr) {
  std::error_code error;
  if (std::filesystem::file_size(path, error) != expected.size || error)
    fail(utf8(path), "linked input changed after import");
  std::ifstream stream(path, std::ios::binary);
  if (contents) contents->reserve(size_t(expected.size));
  std::array<unsigned char, 64 * 1024> chunk;
  uint64_t hash = kContentHashSeed;
  uint64_t remaining = expected.size;
  while (remaining) {
    const auto count = size_t(std::min<uint64_t>(remaining, chunk.size()));
    if (!stream.read(reinterpret_cast<char*>(chunk.data()), std::streamsize(count)))
      fail(utf8(path), "could not read the imported linked input");
    hash = hashContent(hash, std::span(chunk).first(count));
    if (contents) contents->append(reinterpret_cast<const char*>(chunk.data()), count);
    remaining -= count;
  }
  if (!stream || stream.peek() != std::char_traits<char>::eof() || hash != expected.hash)
    fail(utf8(path), "linked input changed after import");
}

void verifyRetainedPlacements(const GltfLinkedCompositionInfo& composition, std::vector<bool> retained) {
  std::vector<bool> sources(composition.sourceContexts.size());
  std::vector<bool> bindings(composition.bindingResolutions.size());
  std::set<const Path*> checkedInputs, checkedResolutions;
  const auto resolution = [&](const Path& path) {
    const auto found = composition.resolutions.find(path);
    if (found == composition.resolutions.end()) fail(utf8(path), "missing retained URI witness");
    if (checkedResolutions.insert(&found->first).second) verifyResolution(found->first, found->second);
  };
  const auto input = [&](const Path& path) {
    const auto found = composition.inputs.find(path);
    if (found == composition.inputs.end()) fail(utf8(path), "missing retained input witness");
    if (checkedInputs.insert(&found->first).second) verifyContents(found->first, found->second);
  };
  for (size_t i = 0; i < retained.size(); ++i) {
    const auto& placement = composition.placementRecords[i];
    if (placement.parentPlacement >= 0) {
      if (size_t(placement.parentPlacement) >= i) fail("save", "invalid nested placement ownership");
      retained[i] = retained[size_t(placement.parentPlacement)];
    }
    if (!retained[i]) continue;
    if (placement.bindingWitness >= bindings.size() || placement.sourceContext >= sources.size())
      fail("save", "invalid linked dependency ownership");
    if (!bindings[placement.bindingWitness]) {
      bindings[placement.bindingWitness] = true;
      for (const auto& path : composition.bindingResolutions[placement.bindingWitness]) resolution(path);
    }
    if (!sources[placement.sourceContext]) {
      sources[placement.sourceContext] = true;
      const auto& source = composition.sourceContexts[placement.sourceContext];
      input(source.source);
      for (const auto& path : source.reads.inputs) input(path);
      for (const auto& path : source.reads.resolutions) resolution(path);
    }
  }
}

} // namespace

Json BuildGltfLinkedManifestForSave(const GltfLinkedCompositionInfo& composition,
    std::span<const GltfLinkedPlacementEdit> edits, size_t selectedScene, const Path& destination) {
  const auto target = linkedSaveTarget(composition, destination);
  for (const auto& [requested, canonical] : composition.resolutions)
    verifyResolution(requested, canonical);
  std::string manifest;
  for (const auto& [path, expected] : composition.inputs)
    verifyContents(path, expected, path == composition.source ? &manifest : nullptr);
  auto output = Json::parse(manifest);
  for (const auto& edit : edits) {
    if (edit.node < 0 || size_t(edit.node) >= composition.primaryNodes)
      fail(utf8(target), "edit does not name a node in the primary linked manifest");
    auto& node = output.at("nodes").at(size_t(edit.node));
    if (edit.name) node["name"] = *edit.name;
    if (edit.matrix) {
      if (!std::ranges::all_of(*edit.matrix, [](double value) { return std::isfinite(value); }))
        fail(utf8(target), "placement transform contains nonfinite values");
      node.erase("translation");
      node.erase("rotation");
      node.erase("scale");
      node["matrix"] = *edit.matrix;
    }
  }
  if (output.contains("scenes")) {
    if (selectedScene >= output.at("scenes").size()) fail(utf8(target), "selected source scene is unavailable");
    output["scene"] = selectedScene;
  }
  return output;
}

Json BuildGltfLinkedHybridManifest(const GltfLinkedCompositionInfo& composition,
    std::span<const GltfLinkedHybridCarrier> carriers, const Path& destination) {
  const auto target = linkedSaveTarget(composition, destination);
  if (carriers.empty()) return Json::object();
  if (carriers.size() > composition.placements.size())
    fail(utf8(target), "retained carriers exceed the primary linked placements");

  std::string text;
  verifyContents(composition.source, composition.inputs.at(composition.source), &text);
  const auto original = Json::parse(text);
  const auto& nodes = original.at("nodes");
  const auto& files = table(original, "files");
  const auto& assets = table(original, "externalAssets");
  std::vector<int32_t> rootPlacements(composition.primaryNodes, -1);
  for (size_t i = 0; i < composition.placementRecords.size(); ++i) {
    const auto& placement = composition.placementRecords[i];
    if (placement.parentPlacement >= 0) continue;
    if (placement.carrierNode < 0 || size_t(placement.carrierNode) >= rootPlacements.size())
      fail(utf8(target), "invalid primary linked placement ownership");
    rootPlacements[size_t(placement.carrierNode)] = int32_t(i);
  }

  std::vector<bool> keptPlacements(composition.placementRecords.size());
  std::vector<bool> keptAssets(assets.size());
  std::set<int32_t> outputNodes;
  std::vector<size_t> carrierAssets;
  carrierAssets.reserve(carriers.size());
  for (const auto& [source, output] : carriers) {
    if (source < 0 || size_t(source) >= rootPlacements.size() || rootPlacements[size_t(source)] < 0 ||
        output < 0 || size_t(output) >= kMergeLimits.maxNodes || !outputNodes.insert(output).second)
      fail(utf8(target), "invalid or duplicate retained linked carrier");
    const auto placement = size_t(rootPlacements[size_t(source)]);
    if (keptPlacements[placement]) fail(utf8(target), "duplicate retained primary placement");
    keptPlacements[placement] = true;
    const auto asset = index(nodes.at(size_t(source)).at("externalAsset"), assets.size(), "externalAsset");
    if (index(assets[asset].at("file"), files.size(), "externalAssets.file") !=
        composition.placementRecords[placement].manifestFile)
      fail(utf8(target), "retained placement file does not match the primary manifest");
    keptAssets[asset] = true;
    carrierAssets.push_back(asset);
  }
  verifyRetainedPlacements(composition, std::move(keptPlacements));

  // Keep referenced files and the transitive alias targets, even when a
  // retained alias row itself has aliases. Preserve the original table order.
  std::vector<bool> keptFiles(files.size());
  std::vector<size_t> pending;
  const auto keepFile = [&](const Json& value) {
    const auto file = index(value, files.size(), "files alias");
    if (!keptFiles[file]) {
      keptFiles[file] = true;
      pending.push_back(file);
    }
  };
  for (size_t i = 0; i < assets.size(); ++i)
    if (keptAssets[i]) keepFile(assets[i].at("file"));
  for (size_t i = 0; i < pending.size(); ++i) {
    const auto& file = files[pending[i]];
    if (const auto aliases = file.find("aliases"); aliases != file.end())
      for (const auto& alias : *aliases) keepFile(alias.at("file"));
  }
  std::vector<size_t> fileMap(files.size()), assetMap(assets.size());
  Json newFiles = Json::array(), newAssets = Json::array(), nodeRefs = Json::array();
  for (size_t i = 0; i < files.size(); ++i) {
    if (!keptFiles[i]) continue;
    fileMap[i] = newFiles.size();
    newFiles.push_back(files[i]);
  }
  for (auto& file : newFiles)
    if (auto aliases = file.find("aliases"); aliases != file.end())
      for (auto& alias : *aliases) alias["file"] = fileMap[index(alias.at("file"), files.size(), "files alias")];
  for (size_t i = 0; i < assets.size(); ++i) {
    if (!keptAssets[i]) continue;
    assetMap[i] = newAssets.size();
    auto asset = assets[i];
    asset["file"] = fileMap[index(asset.at("file"), files.size(), "externalAssets.file")];
    newAssets.push_back(std::move(asset));
  }
  for (size_t i = 0; i < carriers.size(); ++i)
    nodeRefs.push_back({{"node", carriers[i].outputNode}, {"externalAsset", assetMap[carrierAssets[i]]}});
  Json version{{"version", "2.1"}};
  const auto& originalVersion = original.at("asset");
  if (const auto minimum = originalVersion.find("minVersion"); minimum != originalVersion.end())
    version["minVersion"] = *minimum;
  return {{"asset", std::move(version)}, {"files", std::move(newFiles)},
          {"externalAssets", std::move(newAssets)}, {"nodeExternalAssets", std::move(nodeRefs)}};
}

void ApplyGltfLinkedHybridManifest(Json& document, const Json& patch) {
  if (patch.empty()) return;
  auto& nodes = document.at("nodes");
  const auto& assets = table(patch, "externalAssets");
  for (const auto& ref : table(patch, "nodeExternalAssets")) {
    auto& node = nodes.at(index(ref.at("node"), nodes.size(), "output linked carrier"));
    const auto extensions = node.find("extensions");
    if (node.contains("mesh") || node.contains("camera") || node.contains("skin") || node.contains("weights") ||
        node.contains("externalAsset") ||
        (extensions != node.end() && extensions->contains("EXT_mesh_gpu_instancing")))
      fail("output linked carrier", "a retained link cannot overwrite an inline mesh, camera or skin");
    node["externalAsset"] = index(ref.at("externalAsset"), assets.size(), "output externalAsset");
  }
  auto& asset = document.at("asset");
  const auto& version = patch.at("asset");
  asset["version"] = version.at("version");
  if (const auto minimum = version.find("minVersion"); minimum != version.end()) asset["minVersion"] = *minimum;
  else asset.erase("minVersion");
  document["files"] = patch.at("files");
  document["externalAssets"] = assets;
}

} // namespace container::geometry::gltf
