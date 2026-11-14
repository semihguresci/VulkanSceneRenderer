#include <Container/geometry/GltfCameraMotion.h>

#include <Container/geometry/GltfAccessor.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <climits>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace container::geometry::gltf {
namespace {

constexpr char kMarker[] = "ContainerCameraMotion";
constexpr char kKeyBufferName[] = "Container camera motion keys";
constexpr size_t kMaxKeys = 65'536;

[[noreturn]] void invalid(const char* reason) {
  throw std::invalid_argument(std::string("Camera motion: ") + reason);
}

template<class T> const T& atIndex(const std::vector<T>& table, int index) {
  if (index < 0 || size_t(index) >= table.size()) invalid("invalid key storage index");
  return table[size_t(index)];
}

float keyTime(double time) {
  if (!std::isfinite(time) || time < 0 || time > std::numeric_limits<float>::max())
    invalid("key time must be finite and nonnegative");
  return static_cast<float>(time);
}

double rotationLengthSquared(const GltfCameraMotionPose& pose) {
  if (!std::ranges::all_of(pose.translation, [](float value) { return std::isfinite(value); }))
    invalid("key translation must be finite");
  double lengthSquared = 0;
  for (const float value : pose.rotation) lengthSquared += double(value) * value;
  if (!std::isfinite(lengthSquared) || lengthSquared <= 0)
    invalid("key rotation must be a finite nonzero quaternion");
  return lengthSquared;
}

void validateKeys(const GltfCameraMotionClip& clip) {
  if (clip.keys.size() > kMaxKeys) invalid("clip exceeds the supported key count");
  float prior = -1;
  for (const auto& key : clip.keys) {
    if (keyTime(key.time) <= prior) invalid("key times must be strictly increasing");
    if (std::abs(rotationLengthSquared(key.pose) - 1) > 1e-5)
      invalid("stored key rotation must be a unit quaternion");
    prior = key.time;
  }
}

void validateCamera(const tinygltf::Model& model, uint32_t sourceNode,
                    const tinygltf::Node* cameraRest = nullptr) {
  if (sourceNode >= model.nodes.size() || sourceNode > INT32_MAX) invalid("camera node is missing");
  const auto& node = model.nodes[sourceNode];
  (void)atIndex(model.cameras, node.camera);
  if (cameraRest && cameraRest->camera != node.camera) invalid("camera rest does not match the target");
  if (!(cameraRest ? cameraRest->matrix : node.matrix).empty())
    invalid("camera node must use TRS for animation");
}

const tinygltf::Value* motionMarker(const tinygltf::Animation& animation) {
  return animation.extras.IsObject() && animation.extras.Has(kMarker)
      ? &animation.extras.Get(kMarker) : nullptr;
}

struct MotionShape {
  uint32_t sourceNode{};
  bool draft{};
  std::array<int, 3> accessors{};
};

MotionShape motionShape(const tinygltf::Model& model, const tinygltf::Animation& animation) {
  const auto& marker = *motionMarker(animation);
  if (!marker.IsObject() || !marker.Get("version").IsInt() || marker.Get("version").Get<int>() != 1 ||
      !marker.Get("draft").IsBool())
    invalid("unsupported camera clip metadata");
  MotionShape shape{};
  shape.draft = marker.Get("draft").Get<bool>();
  if (shape.draft) {
    const auto& node = marker.Get("node");
    if (!node.IsInt() || node.Get<int>() < 0) invalid("camera draft target is invalid");
    shape.sourceNode = uint32_t(node.Get<int>());
  } else {
    if (animation.channels.size() < 2 || animation.samplers.size() < 2)
      invalid("camera clip is missing translation or rotation");
    const auto& translation = animation.channels[0];
    const auto& rotation = animation.channels[1];
    if (translation.sampler != 0 || translation.target_path != "translation" ||
        rotation.sampler != 1 || rotation.target_path != "rotation" ||
        translation.target_node < 0 || translation.target_node != rotation.target_node ||
        !translation.target_extensions.empty() || !rotation.target_extensions.empty())
      invalid("camera clip must target one node's translation and rotation");
    const auto& t = animation.samplers[0];
    const auto& r = animation.samplers[1];
    if (t.interpolation != "LINEAR" || r.interpolation != "LINEAR" || t.input != r.input ||
        t.input == t.output || t.input == r.output || t.output == r.output)
      invalid("camera clip requires independent LINEAR channels with shared times");
    for (size_t index = 2; index < animation.channels.size(); ++index) {
      if (animation.channels[index].sampler < 2)
        invalid("another channel references the camera key samplers");
    }
    // Export can remap node indices. The standard channels are authoritative.
    shape.sourceNode = uint32_t(translation.target_node);
    shape.accessors = {t.input, t.output, r.output};
  }
  const auto& node = atIndex(model.nodes, int(shape.sourceNode));
  (void)atIndex(model.cameras, node.camera);
  return shape;
}

constexpr std::array<int, 3> kAccessorTypes{TINYGLTF_TYPE_SCALAR, TINYGLTF_TYPE_VEC3, TINYGLTF_TYPE_VEC4};
constexpr std::array<size_t, 3> kDimensions{1, 3, 4};

void appendFloat(tinygltf::Buffer& buffer, float value) {
  static_assert(sizeof(float) == sizeof(uint32_t) && std::numeric_limits<float>::is_iec559);
  const uint32_t bits = std::bit_cast<uint32_t>(value);
  for (unsigned shift = 0; shift < 32; shift += 8)
    buffer.data.push_back(static_cast<unsigned char>(bits >> shift));
}

struct KeyStorage {
  std::array<int, 3> accessors{};
  std::array<int, 3> views{};
  int buffer{-1};
};

bool containsIndex(std::span<const int> indices, int requested) {
  return std::ranges::find(indices, requested) != indices.end();
}

bool extensionReference(const tinygltf::Value& value, const KeyStorage& storage, unsigned depth = 0) {
  if (depth >= 64) return true; // Avoid rewriting storage with an unbounded unknown reference.
  if (value.IsArray()) {
    return std::ranges::any_of(value.Get<tinygltf::Value::Array>(), [&](const auto& entry) {
      return extensionReference(entry, storage, depth + 1);
    });
  }
  if (!value.IsObject()) return false;
  for (const auto& [key, entry] : value.Get<tinygltf::Value::Object>()) {
    if (entry.IsInt()) {
      const auto index = entry.Get<int>();
      if ((key == "buffer" && index == storage.buffer) ||
          (key == "bufferView" && containsIndex(storage.views, index)) ||
          (key == "accessor" && containsIndex(storage.accessors, index))) return true;
    }
    if (extensionReference(entry, storage, depth + 1)) return true;
  }
  return false;
}

template<class T> bool extensionReference(const T& element, const KeyStorage& storage) {
  return std::ranges::any_of(element.extensions, [&](const auto& entry) {
    return extensionReference(entry.second, storage);
  });
}

// Reuse the writer's isolated buffer only while its three views/accessors are
// exclusive. Export may pack it with other data or share buffers on reimport.
std::optional<KeyStorage> ownedStorage(const tinygltf::Model& model, size_t clip,
                                      const std::array<int, 3>& accessors) {
  KeyStorage result{.accessors = accessors};
  for (size_t index = 0; index < accessors.size(); ++index) {
    const auto& accessor = atIndex(model.accessors, accessors[index]);
    const auto& view = atIndex(model.bufferViews, accessor.bufferView);
    const auto& buffer = atIndex(model.buffers, view.buffer);
    if (buffer.name != kKeyBufferName || !buffer.uri.empty() || accessor.byteOffset || accessor.normalized ||
        accessor.sparse.isSparse || accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT ||
        accessor.type != kAccessorTypes[index] || view.byteStride || view.target ||
        !accessor.extensions.empty() || !view.extensions.empty() || !buffer.extensions.empty() ||
        (index && result.buffer != view.buffer)) return {};
    result.views[index] = accessor.bufferView;
    result.buffer = view.buffer;
  }
  for (size_t index = 0; index < model.bufferViews.size(); ++index)
    if (model.bufferViews[index].buffer == result.buffer && !containsIndex(result.views, int(index))) return {};
  for (size_t index = 0; index < model.accessors.size(); ++index) {
    const auto& accessor = model.accessors[index];
    if (!containsIndex(result.accessors, int(index)) && (containsIndex(result.views, accessor.bufferView) ||
        (accessor.sparse.isSparse && (containsIndex(result.views, accessor.sparse.indices.bufferView) ||
                                    containsIndex(result.views, accessor.sparse.values.bufferView))))) return {};
  }
  for (size_t index = 0; index < model.animations.size(); ++index) {
    const auto& animation = model.animations[index];
    const size_t first = index == clip ? 2 : 0;
    for (size_t sampler = first; sampler < animation.samplers.size(); ++sampler) {
      const auto& source = animation.samplers[sampler];
      if (containsIndex(accessors, source.input) || containsIndex(accessors, source.output)) return {};
    }
    if (extensionReference(animation, result)) return {};
  }
  const auto attributesUseStorage = [&](const auto& attributes) {
    return std::ranges::any_of(attributes, [&](const auto& attribute) {
      return containsIndex(result.accessors, attribute.second);
    });
  };
  for (const auto& mesh : model.meshes) {
    if (extensionReference(mesh, result)) return {};
    for (const auto& primitive : mesh.primitives)
      if (containsIndex(accessors, primitive.indices) || attributesUseStorage(primitive.attributes) ||
          std::ranges::any_of(primitive.targets, attributesUseStorage) || extensionReference(primitive, result)) return {};
  }
  for (const auto& node : model.nodes) {
    if (extensionReference(node, result)) return {};
    if (const auto instance = node.extensions.find("EXT_mesh_gpu_instancing"); instance != node.extensions.end()) {
      if (!instance->second.IsObject()) return {};
      const auto& attributes = instance->second.Get("attributes");
      if (attributes.IsObject() && std::ranges::any_of(attributes.Get<tinygltf::Value::Object>(), [&](const auto& attribute) {
        return attribute.second.IsInt() && containsIndex(accessors, attribute.second.template Get<int>());
      })) return {};
    }
  }
  for (const auto& skin : model.skins)
    if (containsIndex(accessors, skin.inverseBindMatrices) || extensionReference(skin, result)) return {};
  for (const auto& image : model.images)
    if (containsIndex(result.views, image.bufferView) || extensionReference(image, result)) return {};
  for (const auto& audio : model.audioSources)
    if (containsIndex(result.views, audio.bufferView) || extensionReference(audio, result)) return {};
  for (const auto& view : model.bufferViews)
    if (extensionReference(view, result)) return {};
  if (extensionReference(model, result)) return {};
  return result;
}

std::optional<std::array<int, 3>> dormantAccessors(const tinygltf::Animation& animation) {
  if (animation.samplers.size() < 2) return {};
  const auto& t = animation.samplers[0];
  const auto& r = animation.samplers[1];
  if (t.input == r.input && t.input != t.output && t.input != r.output && t.output != r.output &&
      t.interpolation == "LINEAR" && r.interpolation == "LINEAR" &&
      std::ranges::none_of(animation.channels, [](const auto& channel) { return channel.sampler < 2; }))
    return std::array{t.input, t.output, r.output};
  return {};
}

void markMotion(tinygltf::Animation& animation, uint32_t sourceNode, bool draft) {
  if (!animation.extras.IsObject()) animation.extras = tinygltf::Value(tinygltf::Value::Object{});
  animation.extras.Get<tinygltf::Value::Object>()[kMarker] = tinygltf::Value(tinygltf::Value::Object{
      {"version", tinygltf::Value(1)}, {"draft", tinygltf::Value(draft)}, {"node", tinygltf::Value(int(sourceNode))}});
  animation.extras_json_string.clear();
}

} // namespace

GltfCameraMotionClip EditGltfCameraMotionKey(const GltfCameraMotionClip& clip, GltfCameraMotionKeyAction action,
    double time, std::optional<double> destinationTime, std::optional<GltfCameraMotionPose> pose) {
  validateKeys(clip);
  const float key = keyTime(time);
  GltfCameraMotionClip result = clip;
  auto& keys = result.keys;
  const auto find = [&](float target) { return std::ranges::lower_bound(keys, target, {}, &GltfCameraMotionKey::time); };
  const auto found = find(key);
  const bool exists = found != keys.end() && found->time == key;
  if (action == GltfCameraMotionKeyAction::Capture) {
    if (!pose || destinationTime) invalid("capture requires a pose");
    const auto length = std::sqrt(rotationLengthSquared(*pose));
    for (auto& component : pose->rotation) component = static_cast<float>(component / length);
    if (exists) found->pose = *pose;
    else {
      if (keys.size() == kMaxKeys) invalid("clip exceeds the supported key count");
      keys.insert(found, {key, *pose});
    }
    return result;
  }
  if (!exists || pose) invalid("selected key is missing");
  if (action == GltfCameraMotionKeyAction::Delete) {
    if (destinationTime) invalid("delete does not take a destination time");
    keys.erase(found);
  } else if (action == GltfCameraMotionKeyAction::Move) {
    if (!destinationTime) invalid("move requires a destination time");
    const float destination = keyTime(*destinationTime);
    if (destination == key) return result;
    const auto existing = find(destination);
    if (existing != keys.end() && existing->time == destination) invalid("destination time already has a key");
    auto moved = *found;
    moved.time = destination;
    keys.erase(found);
    keys.insert(find(destination), moved);
  } else invalid("unknown key action");
  return result;
}

bool IsGltfCameraMotionDraft(const tinygltf::Animation& animation) {
  const auto* marker = motionMarker(animation);
  return marker && marker->IsObject() && marker->Get("version").IsInt() && marker->Get("version").Get<int>() == 1 &&
      marker->Get("draft").IsBool() && marker->Get("draft").Get<bool>();
}

std::optional<GltfCameraMotionClip> ReadGltfCameraMotion(const tinygltf::Model& model, size_t clip) try {
  if (clip >= model.animations.size() || !motionMarker(model.animations[clip])) return {};
  const auto& animation = model.animations[clip];
  const auto shape = motionShape(model, animation);
  GltfCameraMotionClip result{shape.sourceNode, animation.name, {}};
  if (shape.draft) return result;
  const CheckedAccessor times(model, shape.accessors[0], "camera motion times");
  const CheckedAccessor translations(model, shape.accessors[1], "camera motion translations");
  const CheckedAccessor rotations(model, shape.accessors[2], "camera motion rotations");
  const std::array readers{&times, &translations, &rotations};
  if (!times.count() || times.count() > kMaxKeys) invalid("invalid camera key count");
  for (size_t index = 0; index < readers.size(); ++index) {
    const auto& reader = *readers[index];
    const auto& accessor = reader.accessor();
    if (accessor.normalized || accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT ||
        accessor.type != kAccessorTypes[index] || reader.count() != times.count())
      invalid("camera keys require matching FLOAT accessors");
  }
  result.keys.resize(times.count());
  for (size_t index = 0; index < times.count(); ++index) {
    auto& key = result.keys[index];
    times.readFloats(index, std::span{&key.time, 1});
    translations.readFloats(index, key.pose.translation);
    rotations.readFloats(index, key.pose.rotation);
  }
  const auto& input = times.accessor();
  if (input.minValues.size() != 1 || input.maxValues.size() != 1 ||
      static_cast<float>(input.minValues.front()) != result.keys.front().time ||
      static_cast<float>(input.maxValues.front()) != result.keys.back().time)
    invalid("camera time accessor bounds do not match its keys");
  validateKeys(result);
  return result;
} catch (const std::runtime_error& error) {
  throw std::invalid_argument(error.what());
}

struct PreparedGltfCameraMotionEdit::State {
  size_t index{};
  bool appendClip{}, removeClip{}, appendKeys{};
  std::optional<KeyStorage> retireKeys;
  KeyStorage storage;
  tinygltf::Animation clip;
  tinygltf::Buffer keyBuffer;
  std::array<tinygltf::BufferView, 3> views;
  std::array<tinygltf::Accessor, 3> accessors;
};

PreparedGltfCameraMotionEdit::PreparedGltfCameraMotionEdit() noexcept = default;
PreparedGltfCameraMotionEdit::~PreparedGltfCameraMotionEdit() = default;
PreparedGltfCameraMotionEdit::PreparedGltfCameraMotionEdit(PreparedGltfCameraMotionEdit&&) noexcept = default;
PreparedGltfCameraMotionEdit& PreparedGltfCameraMotionEdit::operator=(PreparedGltfCameraMotionEdit&&) noexcept = default;

PreparedGltfCameraMotionEdit PrepareGltfCameraMotionEdit(const tinygltf::Model& model, size_t clip,
    const std::optional<GltfCameraMotionClip>& after, const tinygltf::Node* cameraRest) {
  PreparedGltfCameraMotionEdit prepared;
  prepared.state_ = std::make_unique<PreparedGltfCameraMotionEdit::State>();
  auto& state = *prepared.state_;
  state.index = clip;
  state.appendClip = clip == model.animations.size();
  if (clip > model.animations.size() || clip >= INT32_MAX) invalid("invalid clip index");
  std::optional<std::array<int, 3>> oldAccessors;
  if (!state.appendClip) {
    const auto& current = model.animations[clip];
    if (!motionMarker(current)) invalid("only an editor camera clip can be edited");
    const auto shape = motionShape(model, current);
    if (after && shape.sourceNode != after->sourceNode) invalid("camera clip target changed");
    oldAccessors = shape.draft ? dormantAccessors(current) : std::optional{shape.accessors};
    if (!after) {
      if (clip + 1 != model.animations.size()) invalid("only a tail clip can be removed");
      state.removeClip = true;
      if (oldAccessors) {
        const auto own = ownedStorage(model, clip, *oldAccessors);
        if (own && own->buffer == int(model.buffers.size()) - 1 &&
            own->views == std::array{int(model.bufferViews.size()) - 3, int(model.bufferViews.size()) - 2,
                                    int(model.bufferViews.size()) - 1} &&
            own->accessors == std::array{int(model.accessors.size()) - 3, int(model.accessors.size()) - 2,
                                        int(model.accessors.size()) - 1})
          state.retireKeys = own;
      }
      return prepared;
    }
    state.clip = current;
    if (!shape.draft) state.clip.channels.erase(state.clip.channels.begin(), state.clip.channels.begin() + 2);
    else if (!oldAccessors) {
      for (auto& channel : state.clip.channels) channel.sampler += 2;
      state.clip.samplers.insert(state.clip.samplers.begin(), 2, tinygltf::AnimationSampler{});
    }
  }
  if (!after) invalid("there is no camera clip to remove");
  validateCamera(model, after->sourceNode, cameraRest);
  validateKeys(*after);
  state.clip.name = after->name;
  markMotion(state.clip, after->sourceNode, after->keys.empty());
  if (after->keys.empty()) return prepared;

  const auto shared = oldAccessors ? ownedStorage(model, clip, *oldAccessors) : std::nullopt;
  state.appendKeys = !shared;
  if (shared) state.storage = *shared;
  else {
    if (model.buffers.size() >= INT32_MAX || model.bufferViews.size() > INT32_MAX - 3 ||
        model.accessors.size() > INT32_MAX - 3) invalid("key storage tables exceed the supported range");
    state.storage.buffer = static_cast<int>(model.buffers.size());
    for (size_t index = 0; index < state.views.size(); ++index) {
      state.storage.views[index] = static_cast<int>(model.bufferViews.size() + index);
      state.storage.accessors[index] = static_cast<int>(model.accessors.size() + index);
    }
  }

  state.keyBuffer.name = kKeyBufferName;
  state.keyBuffer.data.reserve(after->keys.size() * 8 * sizeof(float));
  for (size_t index = 0; index < state.views.size(); ++index) {
    auto& view = state.views[index];
    view.buffer = state.storage.buffer;
    view.byteOffset = state.keyBuffer.data.size();
    view.byteLength = after->keys.size() * kDimensions[index] * sizeof(float);
    for (const auto& key : after->keys) {
      if (index == 0) appendFloat(state.keyBuffer, key.time);
      else if (index == 1) for (float value : key.pose.translation) appendFloat(state.keyBuffer, value);
      else for (float value : key.pose.rotation) appendFloat(state.keyBuffer, value);
    }
    auto& accessor = state.accessors[index];
    accessor.bufferView = state.storage.views[index];
    accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    accessor.type = kAccessorTypes[index];
    accessor.count = after->keys.size();
  }
  state.accessors[0].minValues = {after->keys.front().time};
  state.accessors[0].maxValues = {after->keys.back().time};
  if (state.clip.samplers.size() < 2) state.clip.samplers.resize(2);
  for (int index = 0; index < 2; ++index) {
    auto& sampler = state.clip.samplers[size_t(index)];
    sampler.input = state.storage.accessors[0];
    sampler.output = state.storage.accessors[size_t(index) + 1];
    sampler.interpolation = "LINEAR";
    tinygltf::AnimationChannel channel;
    channel.sampler = index;
    channel.target_node = static_cast<int>(after->sourceNode);
    channel.target_path = index ? "rotation" : "translation";
    state.clip.channels.insert(state.clip.channels.begin() + index, std::move(channel));
  }
  return prepared;
}

void ReserveGltfCameraMotionEdit(tinygltf::Model& model, const PreparedGltfCameraMotionEdit& prepared) {
  const auto& state = *prepared.state_;
  if (state.appendClip) model.animations.reserve(model.animations.size() + 1);
  if (state.appendKeys) {
    model.buffers.reserve(model.buffers.size() + 1);
    model.bufferViews.reserve(model.bufferViews.size() + 3);
    model.accessors.reserve(model.accessors.size() + 3);
  }
}

void PublishGltfCameraMotionEdit(tinygltf::Model& model, PreparedGltfCameraMotionEdit&& prepared) noexcept {
  static_assert(std::is_nothrow_move_constructible_v<tinygltf::Animation> &&
                std::is_nothrow_move_assignable_v<tinygltf::Animation> &&
                std::is_nothrow_move_constructible_v<tinygltf::Buffer> &&
                std::is_nothrow_move_assignable_v<tinygltf::Buffer> &&
                std::is_nothrow_move_constructible_v<tinygltf::BufferView> &&
                std::is_nothrow_move_assignable_v<tinygltf::BufferView> &&
                std::is_nothrow_move_constructible_v<tinygltf::Accessor> &&
                std::is_nothrow_move_assignable_v<tinygltf::Accessor>);
  auto state = std::move(prepared.state_);
  if (state->removeClip) {
    model.animations.pop_back();
    if (state->retireKeys) {
      model.accessors.resize(model.accessors.size() - 3);
      model.bufferViews.resize(model.bufferViews.size() - 3);
      model.buffers.pop_back();
    }
    return;
  }
  if (state->appendKeys) {
    model.buffers.push_back(std::move(state->keyBuffer));
    for (auto& view : state->views) model.bufferViews.push_back(std::move(view));
    for (auto& accessor : state->accessors) model.accessors.push_back(std::move(accessor));
  } else if (state->storage.buffer >= 0) {
    model.buffers[size_t(state->storage.buffer)] = std::move(state->keyBuffer);
    for (size_t index = 0; index < state->views.size(); ++index) {
      model.bufferViews[size_t(state->storage.views[index])] = std::move(state->views[index]);
      model.accessors[size_t(state->storage.accessors[index])] = std::move(state->accessors[index]);
    }
  }
  if (state->appendClip) model.animations.push_back(std::move(state->clip));
  else model.animations[state->index] = std::move(state->clip);
}

void ApplyGltfCameraMotion(tinygltf::Model& model, size_t clip, const std::optional<GltfCameraMotionClip>& after) {
  auto edit = PrepareGltfCameraMotionEdit(model, clip, after);
  ReserveGltfCameraMotionEdit(model, edit);
  PublishGltfCameraMotionEdit(model, std::move(edit));
}

} // namespace container::geometry::gltf
