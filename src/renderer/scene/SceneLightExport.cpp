#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <Container/renderer/scene/SceneLightExport.h>
#include <Container/renderer/lighting/LightingManager.h>
#include <Container/renderer/lighting/ImportedSceneLightEdits.h>
#include <Container/geometry/GltfAnimation.h>
#include <Container/utility/SceneManager.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>

namespace container::renderer {
namespace {
using Object = tinygltf::Value::Object;
using Value = tinygltf::Value;
constexpr std::array areaExtensions{"KHR_lights_area", "EXT_lights_area",
                                    "WEBGI_lights_area", "WEBGI_area_light"};
bool areaShape(std::string type) {
  for(auto& c:type) if(c>='A'&&c<='Z') c=char(c+('a'-'A'));
  return type=="rect"||type=="rectangle"||type=="rectangular"||type=="quad"||
         type=="disk"||type=="disc"||type=="circle";
}
bool areaShape(const Value& metadata) {
  if(!metadata.IsObject()) return false;
  for(auto key:{"shape","type","kind"})
    if(metadata.Has(key)&&metadata.Get(key).IsString()) return areaShape(metadata.Get(key).Get<std::string>());
  return false;
}
bool looksArea(const Value& metadata) {
  if(areaShape(metadata)) return true;
  if(!metadata.IsObject()) return false;
  for(auto key:{"width","height","radius","diameter","size","sizeX","sizeY","size_x","size_y"})
    if(metadata.Has(key)&&metadata.Get(key).IsNumber()) return true;
  return false;
}
bool area(const tinygltf::Light& light) {
  if(areaShape(light.type)) return true;
  for(auto name:areaExtensions)
    if(const auto found=light.extensions.find(name);found!=light.extensions.end()&&found->second.IsObject())
      return areaShape(found->second);
  for(const auto& [_,value]:light.extensions) if(looksArea(value)) return areaShape(value);
  if(light.extras.IsObject()&&light.extras.Has("areaLight")&&light.extras.Get("areaLight").IsObject())
    return areaShape(light.extras.Get("areaLight"));
  return areaShape(light.extras);
}
glm::mat4 local(const tinygltf::Node& node) {
  glm::mat4 result(1);
  if (node.matrix.size() == 16) {
    for (int c=0;c<4;++c) for(int r=0;r<4;++r) result[c][r] = float(node.matrix[c*4+r]);
    return result;
  }
  if (node.translation.size()==3) result = glm::translate(result,glm::vec3(node.translation[0],node.translation[1],node.translation[2]));
  if (node.rotation.size()==4) result *= glm::mat4_cast(glm::quat(float(node.rotation[3]),float(node.rotation[0]),float(node.rotation[1]),float(node.rotation[2])));
  if (node.scale.size()==3) result = glm::scale(result,glm::vec3(node.scale[0],node.scale[1],node.scale[2]));
  return result;
}
void require(bool condition) {
  if (!condition) throw std::runtime_error("Scene save has an invalid editable light");
}
bool finite(glm::vec3 value) {return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z);}
bool finite(const glm::mat4& matrix) {
  for (int c=0;c<4;++c) for(int r=0;r<4;++r)
    if (!std::isfinite(matrix[c][r])) return false;
  return true;
}
bool finiteAffine(const glm::mat4& matrix) {
  return finite(matrix) && matrix[0][3]==0 && matrix[1][3]==0 &&
      matrix[2][3]==0 && matrix[3][3]==1;
}

glm::mat4 relativeAffine(const glm::mat4& parent, const glm::mat4& world) {
  require(finiteAffine(parent) && finiteAffine(world));
  const glm::dmat3 parentBasis(parent);
  const double determinant=glm::determinant(parentBasis);
  require(std::isfinite(determinant) && determinant!=0);
  const glm::dmat3 inverse=glm::inverse(parentBasis);
  for (int c=0;c<3;++c) for(int r=0;r<3;++r) require(std::isfinite(inverse[c][r]));
  const glm::dmat3 basis=inverse*glm::dmat3(world);
  const glm::dvec3 translation=inverse*(glm::dvec3(world[3])-glm::dvec3(parent[3]));
  // Solve only the affine basis and origin. A general float mat4 inverse can
  // round the homogeneous 1 to its adjacent float, making a saved node invalid.
  glm::mat4 result(1);
  const auto checkedFloat=[](double value) {
    require(std::isfinite(value) && std::abs(value)<=std::numeric_limits<float>::max());
    return float(value);
  };
  for (int c=0;c<3;++c) for(int r=0;r<3;++r) result[c][r]=checkedFloat(basis[c][r]);
  for (int r=0;r<3;++r) result[3][r]=checkedFloat(translation[r]);
  // The loader composes float matrices too: representable local coefficients
  // alone do not prove that cancellation/overflow preserves the intended world.
  const glm::mat4 recomposed=parent*result;
  require(finiteAffine(recomposed));
  for (int c=0;c<4;++c) for(int r=0;r<4;++r) {
    const double expected=world[c][r];
    const double absoluteTolerance=c==3 && r<3 ? 1e-4 : 1e-5;
    require(std::abs(double(recomposed[c][r])-expected)<=
            absoluteTolerance+1e-5*std::abs(expected));
  }
  return result;
}

glm::vec3 normalizeOr(const glm::vec3& value, const glm::vec3& fallback) {
  const float lengthSquared = glm::dot(value, value);
  if (!std::isfinite(lengthSquared) || lengthSquared <= 1e-12f) return fallback;
  return value * (1.0f/std::sqrt(lengthSquared));
}
struct Candidate {
  size_t node;
  std::string extension;
  tinygltf::Light definition;
  int sourceLight{-1};
};

// Preserve the active sun's traversal priority without detaching it from its
// animated ancestors. Reordering siblings changes no world transforms.
void prioritizeNode(tinygltf::Model& model, size_t target) {
  require(target < model.nodes.size());
  std::vector<int> parents(model.nodes.size(),-2);
  struct Work { int node, parent; };
  std::vector<Work> work;
  auto& roots = model.scenes[model.defaultScene].nodes;
  for (auto root = roots.rbegin(); root != roots.rend(); ++root) work.push_back({*root,-1});
  while (!work.empty()) {
    const auto [index,parent] = work.back(); work.pop_back();
    require(index >= 0 && size_t(index) < model.nodes.size() && parents[size_t(index)] == -2);
    parents[size_t(index)] = parent;
    const auto& children = model.nodes[size_t(index)].children;
    for (auto child = children.rbegin(); child != children.rend(); ++child) work.push_back({*child,index});
  }
  require(parents[target] != -2);
  std::vector<int> path;
  for (int index = static_cast<int>(target); index >= 0; index = parents[size_t(index)]) {
    require(path.size() < model.nodes.size());
    path.push_back(index);
  }
  std::reverse(path.begin(),path.end());
  const auto root = std::find(roots.begin(),roots.end(),path.front());
  require(root != roots.end());
  std::rotate(roots.begin(),root,std::next(root));
  for (size_t depth = 0; depth + 1 < path.size(); ++depth) {
    const auto ancestor = static_cast<size_t>(path[depth]);
    auto& children = model.nodes[ancestor].children;
    const auto active = std::find(children.begin(),children.end(),path[depth+1]);
    std::rotate(children.begin(),active,std::next(active));
    const int light = model.nodes[ancestor].light;
    if (light < 0 || size_t(light) >= model.lights.size() ||
        model.lights[light].type != "directional") continue;
    // A parent is visited before every child. Retain its untouched light on an
    // identity child after the active branch, preserving its animated ancestry.
    require(model.nodes.size() < size_t(std::numeric_limits<int>::max()));
    tinygltf::Node preserved;
    preserved.name = model.lights[light].name;
    preserved.light = light;
    const int child = static_cast<int>(model.nodes.size());
    model.nodes[ancestor].light = -1;
    model.nodes[ancestor].extensions.erase("KHR_lights_punctual");
    model.nodes.push_back(std::move(preserved));
    model.nodes[ancestor].children.push_back(child);
  }
}

struct LightAnimationEdits {
  std::vector<std::vector<tinygltf::AnimationChannel>> channels;
  std::vector<std::vector<tinygltf::AnimationSampler>> samplers;
  std::vector<tinygltf::Accessor> accessors;
  std::vector<tinygltf::BufferView> views;
  std::vector<unsigned char> bytes;
  std::optional<container::geometry::gltf::AnimationLibrary> library;

  explicit LightAnimationEdits(size_t count) : channels(count), samplers(count) {}

  int scaledOutput(const tinygltf::Model& model,
                   const container::geometry::gltf::AnimationTrack& track,
                   double factor) {
    const size_t base = model.buffers.empty() ? 0 : model.buffers[0].data.size();
    while ((base+bytes.size())%4) bytes.push_back(0);
    tinygltf::BufferView view;
    view.buffer = 0;
    view.byteOffset = base+bytes.size();
    for (double value : track.values) {
      const double scaled = value*factor;
      require(std::isfinite(scaled) && std::abs(scaled) <= std::numeric_limits<float>::max());
      const auto bits = std::bit_cast<uint32_t>(static_cast<float>(scaled));
      for (unsigned int byte = 0; byte < 4; ++byte)
        bytes.push_back(static_cast<unsigned char>(bits >> (byte*8)));
    }
    view.byteLength = base+bytes.size()-view.byteOffset;
    const size_t components = track.target.element ? 1 : track.target.components;
    tinygltf::Accessor accessor;
    require(model.bufferViews.size()+views.size() < size_t(std::numeric_limits<int>::max()) &&
            model.accessors.size()+accessors.size() < size_t(std::numeric_limits<int>::max()));
    accessor.bufferView = static_cast<int>(model.bufferViews.size()+views.size());
    accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    accessor.type = components == 3 ? TINYGLTF_TYPE_VEC3 : TINYGLTF_TYPE_SCALAR;
    accessor.count = track.values.size()/components;
    const int output = static_cast<int>(model.accessors.size()+accessors.size());
    views.push_back(std::move(view));
    accessors.push_back(std::move(accessor));
    return output;
  }

  void publish(tinygltf::Model& model) {
    if (!bytes.empty()) {
      if (model.buffers.empty()) model.buffers.emplace_back();
      model.buffers[0].data.insert(model.buffers[0].data.end(),bytes.begin(),bytes.end());
    }
    model.bufferViews.insert(model.bufferViews.end(),views.begin(),views.end());
    model.accessors.insert(model.accessors.end(),accessors.begin(),accessors.end());
    for (size_t i = 0; i < channels.size(); ++i) {
      auto& animation = model.animations[i];
      animation.samplers.insert(animation.samplers.end(),samplers[i].begin(),samplers[i].end());
      animation.channels.insert(animation.channels.end(),channels[i].begin(),channels[i].end());
    }
  }
};

// Original definitions can be shared by untouched bindings. Keep their
// channels and duplicate pointer channels for each independently edited clone.
void duplicateLightAnimation(const tinygltf::Model& model, int source, int clone,
                             double rangeScale, double colorScale,
                             LightAnimationEdits& edits) {
  constexpr std::string_view prefix = "/extensions/KHR_lights_punctual/lights/";
  for (size_t clip = 0; clip < model.animations.size(); ++clip) {
    const auto& animation = model.animations[clip];
    for (const auto& channel : animation.channels) {
      const auto found = channel.target_extensions.find("KHR_animation_pointer");
      if (channel.target_path != "pointer" || found == channel.target_extensions.end() ||
          !found->second.IsObject() || !found->second.Has("pointer") ||
          !found->second.Get("pointer").IsString()) continue;
      const auto pointer = found->second.Get("pointer").Get<std::string>();
      if (!pointer.starts_with(prefix)) continue;
      const auto end = pointer.find('/', prefix.size());
      if (end == std::string::npos) continue;
      int index = -1;
      const auto parsed = std::from_chars(pointer.data()+prefix.size(), pointer.data()+end, index);
      if (parsed.ec != std::errc{} || parsed.ptr != pointer.data()+end || index != source) continue;
      auto copy = channel;
      copy.target_extensions["KHR_animation_pointer"].Get<Object>()["pointer"] =
          Value(std::string(prefix) + std::to_string(clone) + pointer.substr(end));
      const std::string property = pointer.substr(end+1);
      const double factor = property == "range" ? rangeScale :
          property == "intensity" ? colorScale :
          property == "color" || property.starts_with("color/") ? 1/colorScale : 1;
      if (factor != 1) {
        if (!edits.library) edits.library = container::geometry::gltf::ParseAnimations(model);
        const auto slash = property.find('/');
        const auto leaf = property.substr(0,slash);
        std::optional<size_t> element;
        if (slash != std::string::npos) {
          size_t index = 0;
          const auto parsed = std::from_chars(property.data()+slash+1,property.data()+property.size(),index);
          require(parsed.ec == std::errc{} && parsed.ptr == property.data()+property.size());
          element = index;
        }
        const auto& tracks = edits.library->clips[clip].tracks;
        const auto track = std::find_if(tracks.begin(),tracks.end(),[&](const auto& track) {
          return track.target.kind == container::geometry::gltf::AnimationTargetKind::Light &&
              track.target.index == size_t(source) && track.target.property == leaf && track.target.element == element;
        });
        require(track != tracks.end());
        auto sampler = animation.samplers[static_cast<size_t>(channel.sampler)];
        sampler.output = edits.scaledOutput(model,*track,factor);
        require(animation.samplers.size()+edits.samplers[clip].size() < size_t(std::numeric_limits<int>::max()));
        copy.sampler = static_cast<int>(animation.samplers.size()+edits.samplers[clip].size());
        edits.samplers[clip].push_back(std::move(sampler));
      }
      edits.channels[clip].push_back(std::move(copy));
    }
  }
}

std::optional<tinygltf::Light> areaDefinition(const tinygltf::Model& source,
                                             const tinygltf::Node& node,
                                             const char* name) {
  const auto association = node.extensions.find(name);
  const auto table = source.extensions.find(name);
  if (association == node.extensions.end() || !association->second.IsObject() ||
      !association->second.Has("light") || !association->second.Get("light").IsNumber() ||
      table == source.extensions.end() || !table->second.IsObject() ||
      !table->second.Has("lights") || !table->second.Get("lights").IsArray()) return std::nullopt;
  const double index = association->second.Get("light").GetNumberAsDouble();
  const auto& lights = table->second.Get("lights");
  if (!std::isfinite(index) || index < 0 || std::floor(index) != index || index >= double(lights.ArrayLen()))
    return std::nullopt;
  const auto& value = lights.Get(static_cast<size_t>(index));
  if (!value.IsObject()) return std::nullopt;
  tinygltf::Light definition;
  definition.type = "rectangle";
  for (const auto* key : {"type", "shape", "kind"})
    if (value.Has(key) && value.Get(key).IsString()) {
      definition.type = value.Get(key).Get<std::string>();
      break;
    }
  definition.extras = value;
  return area(definition)?std::optional{std::move(definition)}:std::nullopt;
}

struct SourceLightCandidates {
  std::vector<Candidate> points, directions, areas;
};

SourceLightCandidates collectSourceLightCandidates(
    const tinygltf::Model& source, size_t scene,
    const std::map<int32_t, size_t>* sourceToExport = nullptr) {
  SourceLightCandidates result;
  if (source.scenes.empty()) return result;
  require(scene < source.scenes.size());
  std::vector<bool> visited(source.nodes.size());
  std::vector<int> work(source.scenes[scene].nodes.rbegin(),source.scenes[scene].nodes.rend());
  while (!work.empty()) {
    const int index = work.back(); work.pop_back();
    require(index >= 0 && size_t(index) < source.nodes.size() && !visited[index]);
    visited[index] = true;
    const auto& node = source.nodes[index];
    size_t exported = size_t(index);
    if (sourceToExport) {
      const auto mapped = sourceToExport->find(index);
      exported = mapped == sourceToExport->end()
          ? std::numeric_limits<size_t>::max() : mapped->second;
    }
    if (node.light >= 0 && size_t(node.light) < source.lights.size()) {
      const auto& definition = source.lights[node.light];
      if (area(definition)) result.areas.push_back({exported, {}, definition, node.light});
      else if (definition.type == "directional") result.directions.push_back({exported, {}, definition, node.light});
      else if (definition.type == "point" || definition.type == "spot") result.points.push_back({exported, {}, definition, node.light});
    }
    for (auto name : areaExtensions)
      if (auto definition = areaDefinition(source, node, name))
        result.areas.push_back({exported, name, std::move(*definition)});
    for (auto child = node.children.rbegin(); child != node.children.rend(); ++child) work.push_back(*child);
  }
  return result;
}

const std::vector<Candidate>& candidatesFor(const SourceLightCandidates& candidates,
                                          EditableLightType type) {
  return type == EditableLightType::Directional ? candidates.directions :
         type == EditableLightType::Area ? candidates.areas : candidates.points;
}

const std::vector<container::scene::AuthoredLightBinding>& bindingsFor(
    const container::scene::AuthoredSceneLights& table, EditableLightType type) {
  return type == EditableLightType::Directional ? table.directionalBindings :
         type == EditableLightType::Area ? table.areaBindings : table.pointBindings;
}

Candidate candidateForBinding(const tinygltf::Model& source,
    const container::scene::AuthoredLightBinding& binding,
    const std::map<int32_t,size_t>& sourceToExport) {
  require(binding.sourceNode >= 0 && size_t(binding.sourceNode) < source.nodes.size());
  const auto mapped = sourceToExport.find(binding.sourceNode);
  require(mapped != sourceToExport.end());
  const auto& node = source.nodes[size_t(binding.sourceNode)];
  if (binding.extension.empty()) {
    require(node.light >= 0 && size_t(node.light) < source.lights.size());
    return {mapped->second,{},source.lights[size_t(node.light)],node.light};
  }
  require(std::ranges::find(areaExtensions,binding.extension) != areaExtensions.end());
  auto definition = areaDefinition(source,node,binding.extension.c_str());
  require(definition.has_value());
  return {mapped->second,binding.extension,std::move(*definition)};
}

bool isActiveDirectional(const container::scene::AuthoredSceneLights* full,
    const container::scene::AuthoredSceneLights* visible, const EditableLightEntity& entity) {
  if (entity.type != EditableLightType::Directional) return false;
  if (!full || entity.source == EditableLightSource::Manual) return true;
  const auto& active = visible ? *visible : *full;
  require(entity.id.index < full->directionalBindings.size());
  return !active.directionalBindings.empty() &&
      full->directionalBindings[entity.id.index] == active.directionalBindings.front();
}

bool sameImportedLightEdit(const EditableLightEntity& a,
                          const EditableLightEntity& b) {
  if (a.type != b.type || a.color != b.color || a.intensity != b.intensity) return false;
  // Directional position is the editor's scene-dependent gizmo anchor rather
  // than an authored lighting property. Ordinal labels and selections also do
  // not belong to the source binding's exported state.
  if (a.type != EditableLightType::Directional &&
      (a.position != b.position || a.range != b.range)) return false;
  if (a.type != EditableLightType::Point) {
    const float aLength = glm::length(a.direction), bLength = glm::length(b.direction);
    if (!std::isfinite(aLength) || !std::isfinite(bLength) ||
        aLength <= 1e-8f || bLength <= 1e-8f ||
        glm::length(a.direction/aLength - b.direction/bLength) > 1e-6f) return false;
  }
  if (a.type == EditableLightType::Spot &&
      (a.innerConeDegrees != b.innerConeDegrees || a.outerConeDegrees != b.outerConeDegrees)) return false;
  return a.type != EditableLightType::Area ||
      (a.tangent == b.tangent && a.bitangent == b.bitangent &&
       a.areaHalfSize == b.areaHalfSize && a.areaShape == b.areaShape);
}
}

static std::vector<EditableLightEntity> evaluateEditableSceneLightsImpl(
    const container::scene::AuthoredSceneLights& authored,
    const SceneLightEdits& edits,
    const glm::mat4& sceneTransform, bool includeDirectional) {
  require(finite(sceneTransform));
  for (const auto count : {authored.points.size(), authored.areas.size(),
                            edits.manualPointLights.size(), edits.manualAreaLights.size()})
    require(count <= size_t(kInvalidEditableLightIndex));
  std::vector<EditableLightEntity> result;
  const bool manualSun = edits.directionalOverride &&
      edits.directionalOverride->source == EditableLightSource::Manual;
  if (includeDirectional && (!authored.directionals.empty() || manualSun)) {
    container::gpu::LightingData lighting{};
    const auto baseDirection = glm::normalize(glm::vec3(-0.45f,-1.0f,-0.3f));
    lighting.directionalDirection = glm::vec4(
        glm::normalize(glm::vec3(sceneTransform*glm::vec4(baseDirection,0))),0);
    if (!authored.directionals.empty()) {
      const auto& sun = authored.directionals.front();
      const auto fallback = normalizeOr(glm::vec3(lighting.directionalDirection),glm::vec3(0,0,1));
      lighting.directionalDirection = glm::vec4(normalizeOr(
          glm::vec3(sceneTransform*glm::vec4(glm::vec3(sun.direction),0)),fallback),0);
      lighting.directionalColorIntensity = sun.colorIntensity;
    }
    if (edits.directionalOverride &&
        (manualSun || edits.directionalOverride->source == EditableLightSource::Imported))
      lighting = directionalLightDataFromEditable(*edits.directionalOverride,lighting);
    // A directional light's displayed position depends on scene bounds, not
    // lighting. Use the retained manual position or outer-root origin for the
    // export-only table; direction/color/intensity match the live pipeline.
    const auto displayPosition = manualSun ? edits.directionalOverride->position
                                          : glm::vec3(sceneTransform[3]);
    result.push_back(editableDirectionalLight(
        manualSun ? EditableLightSource::Manual : EditableLightSource::Imported,
        displayPosition,lighting,false));
  }
  for (size_t i = 0; i < authored.points.size(); ++i) {
    const auto& original = authored.points[i];
    auto light = original;
    light.positionRadius = glm::vec4(
        glm::vec3(sceneTransform*glm::vec4(glm::vec3(original.positionRadius),1)),
        original.positionRadius.w);
    if (original.coneOuterCosType.y >= 0.5f) {
      const auto fallback = normalizeOr(glm::vec3(original.directionInnerCos),glm::vec3(0,0,-1));
      light.directionInnerCos = glm::vec4(normalizeOr(
          glm::vec3(sceneTransform*glm::vec4(glm::vec3(original.directionInnerCos),0)),fallback),
          original.directionInnerCos.w);
    }
    if (i < edits.importedPointOverrides.size() && edits.importedPointOverrides[i])
      light = *edits.importedPointOverrides[i];
    result.push_back(editablePointLight(EditableLightSource::Imported,uint32_t(i),light,false));
  }
  for (size_t i = 0; i < authored.areas.size(); ++i) {
    const auto& original = authored.areas[i];
    auto light = original;
    light.positionRange = glm::vec4(
        glm::vec3(sceneTransform*glm::vec4(glm::vec3(original.positionRange),1)),original.positionRange.w);
    const auto directionFallback = normalizeOr(glm::vec3(original.directionType),glm::vec3(0,0,-1));
    light.directionType = glm::vec4(normalizeOr(
        glm::vec3(sceneTransform*glm::vec4(glm::vec3(original.directionType),0)),directionFallback),
        original.directionType.w);
    const auto tangentFallback = normalizeOr(glm::vec3(original.tangentHalfSize),glm::vec3(1,0,0));
    light.tangentHalfSize = glm::vec4(normalizeOr(
        glm::vec3(sceneTransform*glm::vec4(glm::vec3(original.tangentHalfSize),0)),tangentFallback),
        original.tangentHalfSize.w);
    const auto bitangentFallback = normalizeOr(glm::vec3(original.bitangentHalfSize),glm::vec3(0,1,0));
    light.bitangentHalfSize = glm::vec4(normalizeOr(
        glm::vec3(sceneTransform*glm::vec4(glm::vec3(original.bitangentHalfSize),0)),bitangentFallback),
        original.bitangentHalfSize.w);
    if (i < edits.importedAreaOverrides.size() && edits.importedAreaOverrides[i])
      light = *edits.importedAreaOverrides[i];
    result.push_back(editableAreaLight(EditableLightSource::Imported,uint32_t(i),light,false));
  }
  for (size_t i = 0; i < edits.manualPointLights.size(); ++i)
    result.push_back(editablePointLight(EditableLightSource::Manual,uint32_t(i),edits.manualPointLights[i],false));
  for (size_t i = 0; i < edits.manualAreaLights.size(); ++i)
    result.push_back(editableAreaLight(EditableLightSource::Manual,uint32_t(i),edits.manualAreaLights[i],false));
  return result;
}

std::vector<EditableLightEntity> evaluateEditableSceneLights(
    const container::scene::AuthoredSceneLights& authored,
    const SceneLightEdits& edits, const glm::mat4& sceneTransform) {
  return evaluateEditableSceneLightsImpl(authored,edits,sceneTransform,true);
}

std::vector<EditableLightEntity> evaluateEditableSceneLightsForExport(
    const container::scene::AuthoredSceneLights& full,
    const container::scene::AuthoredSceneLights& visible,
    const SceneLightEdits& localEdits,
    const ImportedSceneLightEdits& globalImportedEdits,
    const glm::mat4& sceneTransform) {
  require(full.points.size() == full.pointBindings.size() &&
          full.areas.size() == full.areaBindings.size() &&
          full.directionals.size() == full.directionalBindings.size() &&
          full.directionals.size() <= size_t(kInvalidEditableLightIndex));
  const auto merged = globalImportedEdits.merge(full,localEdits);
  auto nonDirectional = evaluateEditableSceneLightsImpl(full,merged,sceneTransform,false);
  std::vector<EditableLightEntity> result;
  const bool manualSun = localEdits.directionalOverride &&
      localEdits.directionalOverride->source == EditableLightSource::Manual;
  for (size_t i = 0; i < full.directionals.size(); ++i) {
    const auto& binding = full.directionalBindings[i];
    if (manualSun && !visible.directionalBindings.empty() &&
        binding == visible.directionalBindings.front()) continue;
    container::scene::AuthoredSceneLights sun;
    sun.directionals.push_back(full.directionals[i]);
    SceneLightEdits override;
    if (const auto found = globalImportedEdits.directionals.find(binding);
        found != globalImportedEdits.directionals.end()) override.directionalOverride = found->second;
    auto evaluated = evaluateEditableSceneLightsImpl(sun,override,sceneTransform,true);
    require(evaluated.size() == 1);
    evaluated.front().id.index = uint32_t(i);
    result.push_back(std::move(evaluated.front()));
  }
  if (manualSun) {
    auto evaluated = evaluateEditableSceneLightsImpl({},localEdits,sceneTransform,true);
    require(!evaluated.empty() && evaluated.front().type == EditableLightType::Directional);
    result.push_back(std::move(evaluated.front()));
  }
  result.insert(result.end(),std::make_move_iterator(nonDirectional.begin()),
                std::make_move_iterator(nonDirectional.end()));
  return result;
}

static void exportEditableSceneLightsForScene(tinygltf::Model& model,
                               std::span<const EditableLightEntity> lights,
                               const tinygltf::Model* originalSource,
                               std::span<const int32_t> sourceNodeIndices,
                               double importedRangeScale,
                               std::optional<size_t> sourceSceneOverride = {},
                               const container::scene::AuthoredSceneLights* sceneBindings = nullptr,
                               const container::scene::AuthoredSceneLights* visibleBindings = nullptr) {
  require(std::isfinite(importedRangeScale) && importedRangeScale > 0);
  if (model.scenes.empty() || model.defaultScene < 0 || size_t(model.defaultScene)>=model.scenes.size())
    throw std::runtime_error("Scene light export needs an active glTF scene");
  std::vector<glm::mat4> worlds(model.nodes.size(),glm::mat4(1));
  std::vector<bool> visited(model.nodes.size());
  struct WorldWork { int node; glm::mat4 parent; };
  std::vector<WorldWork> work;
  for (auto root = model.scenes[model.defaultScene].nodes.rbegin();
       root != model.scenes[model.defaultScene].nodes.rend(); ++root) work.push_back({*root,glm::mat4(1)});
  while (!work.empty()) {
    const auto [index,parent] = work.back(); work.pop_back();
    require(index>=0 && size_t(index)<model.nodes.size() && !visited[index]);
    visited[index]=true;
    const auto& node=model.nodes[index];
    worlds[index]=parent*local(node);
    require(finite(worlds[index]));
    for (auto child = node.children.rbegin(); child != node.children.rend(); ++child)
      work.push_back({*child,worlds[index]});
  }
  // Imported IDs refer to original source traversal, which remains stable when
  // editor reparenting or sibling ordering changes exported traversal order.
  require(!originalSource || sourceNodeIndices.size() == model.nodes.size());
  std::map<int32_t, size_t> sourceToExport;
  for (size_t i=0;i<sourceNodeIndices.size();++i)
    if (sourceNodeIndices[i]>=0) require(sourceToExport.emplace(sourceNodeIndices[i],i).second);
  const auto& source = originalSource ? *originalSource : model;
  const auto sourceScene = sourceSceneOverride.value_or(
      source.defaultScene < 0 ? size_t{0} : size_t(source.defaultScene));
  const auto candidates = sceneBindings ? SourceLightCandidates{} : collectSourceLightCandidates(
      source, sourceScene, originalSource ? &sourceToExport : nullptr);
  const auto& points = candidates.points;
  const auto& directions = candidates.directions;
  const auto& areas = candidates.areas;
  bool punctualUsed=false, areaUsed=false;
  struct Plan {
    std::optional<Candidate> original;
    tinygltf::Light light;
    tinygltf::Node node;
    bool activeDirectional{false};
    bool rootDirectional{false};
    int animatedSourceLight{-1};
    double colorScale{1};
  };
  std::vector<Plan> plans;
  std::set<std::tuple<EditableLightSource,int,uint32_t>> identities;
  bool directionalPlanned=false;
  for(const auto& entity:lights) {
    if(entity.source==EditableLightSource::Generated) continue;
    require(entity.source==EditableLightSource::Imported || entity.source==EditableLightSource::Manual);
    require(entity.type==EditableLightType::Point || entity.type==EditableLightType::Spot ||
            entity.type==EditableLightType::Directional || entity.type==EditableLightType::Area);
    require(entity.id.type==entity.type && entity.id.source==entity.source && entity.id.index!=kInvalidEditableLightIndex);
    const int category=entity.type==EditableLightType::Directional?0:entity.type==EditableLightType::Area?2:1;
    require(identities.emplace(entity.source,category,entity.id.index).second);
    if(entity.type==EditableLightType::Directional) {
      // LightingManager has one active directional binding. A manual sun is an
      // override of that binding, rather than another independently lit sun.
      if (!sceneBindings || entity.source == EditableLightSource::Manual) {
        require(entity.id.index==0 && !directionalPlanned);
        directionalPlanned=true;
      }
    }
    require(finite(entity.position)&&finite(entity.direction)&&finite(entity.color)&&
            glm::all(glm::greaterThanEqual(entity.color,glm::vec3(0)))&&
            std::isfinite(entity.intensity)&&entity.intensity>=0 &&
            std::isfinite(entity.range)&&entity.range>=0);
    std::optional<Candidate> original;
    if(entity.source==EditableLightSource::Imported) {
      if (sceneBindings) {
        const auto& bindings = bindingsFor(*sceneBindings,entity.type);
        require(entity.id.index < bindings.size());
        original = candidateForBinding(source,bindings[entity.id.index],sourceToExport);
      } else {
        const auto& candidates=entity.type==EditableLightType::Directional?directions:
                               entity.type==EditableLightType::Area?areas:points;
        require(entity.id.index<candidates.size());
        original=candidates[entity.id.index];
      }
      require(original->node<model.nodes.size() && visited[original->node]);
    } else if(entity.type==EditableLightType::Directional) {
      if (sceneBindings) {
        const auto& visible = visibleBindings ? *visibleBindings : *sceneBindings;
        if (!visible.directionalBindings.empty())
          original = candidateForBinding(source,visible.directionalBindings.front(),sourceToExport);
      } else if (!directions.empty()) original=directions.front();
      if (original) require(original->node<model.nodes.size() && visited[original->node]);
    }
    tinygltf::Light light;
    if(original) light=original->definition;
    if(!original || entity.source==EditableLightSource::Manual) light.name=entity.label;
    const double colorScale=std::max({1.0,double(entity.color.r),double(entity.color.g),double(entity.color.b)});
    light.color={entity.color.r/colorScale,entity.color.g/colorScale,entity.color.b/colorScale};
    light.intensity=double(entity.intensity)*colorScale;
    require(light.intensity<=std::numeric_limits<float>::max());
    light.range=entity.type==EditableLightType::Directional ? 0.0 : double(entity.range);
    light.type=entity.type==EditableLightType::Directional?"directional":entity.type==EditableLightType::Spot?"spot":"point";
    glm::mat4 transform(1);
    transform[3]=glm::vec4(entity.position,1);
    const float length=glm::length(entity.direction);
    require(std::isfinite(length)&&length>1e-8f);
    const glm::vec3 direction=entity.direction/length;
    if(entity.type==EditableLightType::Area) {
      require(finite(entity.tangent)&&finite(entity.bitangent)&&
              std::isfinite(entity.areaHalfSize.x)&&std::isfinite(entity.areaHalfSize.y)&&
              entity.areaHalfSize.x>0&&entity.areaHalfSize.y>0);
      const auto tangentLength=glm::length(entity.tangent);
      const auto bitangentLength=glm::length(entity.bitangent);
      require(std::isfinite(tangentLength)&&std::isfinite(bitangentLength)&&tangentLength>1e-8f&&bitangentLength>1e-8f);
      const auto tangent=entity.tangent/tangentLength;
      const auto bitangent=entity.bitangent/bitangentLength;
      // Preserve the renderer's authored area basis, including roll.
      require(entity.areaShape==container::gpu::kAreaLightTypeDisk || entity.areaShape==container::gpu::kAreaLightTypeRectangle);
      if(entity.areaShape==container::gpu::kAreaLightTypeDisk &&
          std::abs(entity.areaHalfSize.x-entity.areaHalfSize.y)>1e-5f*std::max(entity.areaHalfSize.x,entity.areaHalfSize.y))
        throw std::runtime_error("Scene light export: elliptical disk lights require an extended area-light schema");
      transform[0]=glm::vec4(tangent,0);
      transform[1]=glm::vec4(bitangent,0);
      transform[2]=glm::vec4(-direction,0);
      require(std::abs(glm::determinant(glm::mat3(transform)))>1e-6f);
      Object metadata;
      if(const auto retained=light.extensions.find("KHR_lights_area");retained!=light.extensions.end()&&retained->second.IsObject())
        metadata=retained->second.Get<Object>();
      metadata["shape"]=Value(std::string(entity.areaShape==container::gpu::kAreaLightTypeDisk?"disk":"rectangle"));
      metadata["width"]=Value(double(entity.areaHalfSize.x)*2);
      metadata["height"]=Value(double(entity.areaHalfSize.y)*2);
      metadata["range"]=Value(double(entity.range));
      metadata.erase("distance");
      metadata.erase("maxDistance");
      if(entity.areaShape==container::gpu::kAreaLightTypeDisk) metadata["radius"]=Value(double(entity.areaHalfSize.x));
      else metadata.erase("radius");
      light.extensions["KHR_lights_area"]=Value(std::move(metadata));
      areaUsed=true;
    } else {
      transform=glm::translate(glm::mat4(1),entity.position)*
                glm::mat4_cast(glm::rotation(glm::vec3(0,0,-1),direction));
    }
    if(entity.type==EditableLightType::Spot) {
      require(std::isfinite(entity.innerConeDegrees)&&std::isfinite(entity.outerConeDegrees)&&
              entity.innerConeDegrees>=0 && entity.outerConeDegrees>0 &&
              entity.innerConeDegrees<entity.outerConeDegrees && entity.outerConeDegrees<=90);
      light.spot.innerConeAngle=glm::radians(double(entity.innerConeDegrees));
      light.spot.outerConeAngle=glm::radians(double(entity.outerConeDegrees));
    }
    tinygltf::Node node;
    node.name=entity.label.empty()?"Authored light":entity.label;
    const bool activeDirectional=isActiveDirectional(sceneBindings,visibleBindings,entity);
    const bool rootDirectional=entity.type==EditableLightType::Directional && entity.source==EditableLightSource::Manual;
    if(original && !rootDirectional) {
      transform=relativeAffine(worlds[original->node],transform);
    }
    require(finiteAffine(transform));
    for(int c=0;c<4;++c) for(int r=0;r<4;++r) node.matrix.push_back(double(transform[c][r]));
    const int animatedSourceLight = original && entity.source==EditableLightSource::Imported
                                      ? original->sourceLight : -1;
    plans.push_back({std::move(original),std::move(light),std::move(node),activeDirectional,
                     rootDirectional,animatedSourceLight,colorScale});
    punctualUsed=true;
  }
  require(model.nodes.size()+plans.size()<=size_t(std::numeric_limits<int>::max()) &&
          model.lights.size()+plans.size()<=size_t(std::numeric_limits<int>::max()));
  LightAnimationEdits animationEdits(model.animations.size());
  for (size_t i = 0; i < plans.size(); ++i)
    if (plans[i].animatedSourceLight >= 0)
      duplicateLightAnimation(model,plans[i].animatedSourceLight,
          static_cast<int>(model.lights.size()+i),importedRangeScale,plans[i].colorScale,animationEdits);
  // Validate the entire overlay before changing the detached export snapshot.
  animationEdits.publish(model);
  for(auto& plan:plans) {
    if(plan.original) {
      auto& original=model.nodes[plan.original->node];
      if(plan.original->extension.empty()) {
        original.light=-1;
        original.extensions.erase("KHR_lights_punctual");
      } else original.extensions.erase(plan.original->extension);
      if(!plan.rootDirectional)
        original.children.push_back(static_cast<int>(model.nodes.size()));
    }
    if(plan.rootDirectional) {
      // Reload activates the first directional encountered in scene traversal.
      // Keep untouched source suns bound, but put the currently active sun
      // before them without reordering or moving the existing mesh hierarchy.
      auto& roots=model.scenes[model.defaultScene].nodes;
      roots.insert(roots.begin(),static_cast<int>(model.nodes.size()));
    } else if(!plan.original)
      model.scenes[model.defaultScene].nodes.push_back(static_cast<int>(model.nodes.size()));
    plan.node.light=static_cast<int>(model.lights.size());
    model.lights.push_back(std::move(plan.light));
    model.nodes.push_back(std::move(plan.node));
    if(plan.activeDirectional && !plan.rootDirectional)
      prioritizeNode(model,model.nodes.size()-1);
  }
  auto declare=[&](const char* name) {if(std::ranges::find(model.extensionsUsed,name)==model.extensionsUsed.end())model.extensionsUsed.push_back(name);};
  if(punctualUsed) declare("KHR_lights_punctual");
  if(areaUsed) declare("KHR_lights_area");
}

void exportEditableSceneLights(tinygltf::Model& model,
                               std::span<const EditableLightEntity> lights,
                               const tinygltf::Model* originalSource,
                               std::span<const int32_t> sourceNodeIndices,
                               double importedRangeScale) {
  exportEditableSceneLightsForScene(model, lights, originalSource,
                                   sourceNodeIndices, importedRangeScale);
}

void exportEditableSceneLightsForScenes(
    tinygltf::Model& model,
    std::span<const std::vector<EditableLightEntity>> sceneLights,
    const tinygltf::Model* originalSource,
    std::span<const int32_t> sourceNodeIndices,
    double importedRangeScale,
    std::span<const container::scene::AuthoredSceneLights> sceneBindings,
    std::span<const container::scene::AuthoredSceneLights> visibleSceneBindings) {
  require(std::isfinite(importedRangeScale) && importedRangeScale > 0);
  require(!model.scenes.empty() && sceneLights.size() == model.scenes.size() &&
          model.scenes.size() <= size_t(std::numeric_limits<int>::max()) &&
          model.nodes.size() <= size_t(std::numeric_limits<int32_t>::max()) &&
          model.defaultScene >= 0 && size_t(model.defaultScene) < model.scenes.size());
  require(sceneBindings.empty() || sceneBindings.size() == sceneLights.size());
  require(visibleSceneBindings.empty() ||
      (!sceneBindings.empty() && visibleSceneBindings.size() == sceneLights.size()));
  const auto& source = originalSource ? *originalSource : model;
  require(source.scenes.empty() || source.scenes.size() == model.scenes.size());
  std::vector<int32_t> sourceNodes;
  if (originalSource || !sourceNodeIndices.empty()) {
    require(sourceNodeIndices.size() == model.nodes.size());
    sourceNodes.assign(sourceNodeIndices.begin(), sourceNodeIndices.end());
  } else {
    sourceNodes.reserve(model.nodes.size());
    for (size_t i = 0; i < model.nodes.size(); ++i) sourceNodes.push_back(int32_t(i));
  }
  std::map<int32_t, size_t> sourceToExport;
  for (size_t i = 0; i < sourceNodes.size(); ++i)
    if (sourceNodes[i] >= 0) require(sourceToExport.emplace(sourceNodes[i], i).second);

  using Binding = std::pair<size_t, std::string>;
  std::vector<SourceLightCandidates> candidates;
  std::map<Binding, size_t> bindingSceneCounts;
  for (size_t scene = 0; scene < sceneLights.size(); ++scene) {
    candidates.push_back(sceneBindings.empty() ? collectSourceLightCandidates(source, scene, &sourceToExport)
                                               : SourceLightCandidates{});
    if (sceneBindings.empty()) {
      for (const auto* table : {&candidates.back().points, &candidates.back().directions,
                                &candidates.back().areas})
        for (const auto& candidate : *table)
          ++bindingSceneCounts[{candidate.node, candidate.extension}];
    } else {
      for (const auto* table : {&sceneBindings[scene].pointBindings,&sceneBindings[scene].directionalBindings,
                                &sceneBindings[scene].areaBindings})
        for (const auto& binding : *table) {
          const auto original = candidateForBinding(source,binding,sourceToExport);
          ++bindingSceneCounts[{original.node,original.extension}];
        }
    }
  }
  struct Handled {
    EditableLightEntity entity;
    size_t exportedNode{std::numeric_limits<size_t>::max()};
  };
  std::map<Binding, Handled> handled;
  // The full batch is transactional, including later conflicts and failures
  // while adding animation data or preserving a directional ancestor.
  auto output = model;
  const int selectedScene = output.defaultScene;
  for (size_t scene = 0; scene < sceneLights.size(); ++scene) {
    output.defaultScene = static_cast<int>(scene);
    const auto* bindings = sceneBindings.empty() ? nullptr : &sceneBindings[scene];
    const auto* visible = visibleSceneBindings.empty() ? bindings : &visibleSceneBindings[scene];
    std::vector<EditableLightEntity> filtered;
    std::vector<std::pair<Binding, size_t>> addedBindings;
    std::vector<size_t> sharedSuns;
    std::set<std::tuple<EditableLightSource, int, uint32_t>> identities;
    bool directionalPlanned = false;
    for (const auto& entity : sceneLights[scene]) {
      if (entity.source == EditableLightSource::Generated) continue;
      require(entity.source == EditableLightSource::Imported || entity.source == EditableLightSource::Manual);
      require(entity.type == EditableLightType::Point || entity.type == EditableLightType::Spot ||
              entity.type == EditableLightType::Directional || entity.type == EditableLightType::Area);
      require(entity.id.type == entity.type && entity.id.source == entity.source &&
              entity.id.index != kInvalidEditableLightIndex);
      const int category = entity.type == EditableLightType::Directional ? 0 : entity.type == EditableLightType::Area ? 2 : 1;
      require(identities.emplace(entity.source, category, entity.id.index).second);
      if (entity.type == EditableLightType::Directional) {
        if (!bindings || entity.source == EditableLightSource::Manual) {
          require(entity.id.index == 0 && !directionalPlanned);
          directionalPlanned = true;
        }
      }
      require(finite(entity.position) && finite(entity.direction) && finite(entity.color) &&
              glm::all(glm::greaterThanEqual(entity.color, glm::vec3(0))) &&
              std::isfinite(entity.intensity) && entity.intensity >= 0 &&
              std::isfinite(entity.range) && entity.range >= 0 &&
              std::isfinite(glm::length(entity.direction)) && glm::length(entity.direction) > 1e-8f);
      if (entity.source == EditableLightSource::Manual) {
        if (entity.type == EditableLightType::Directional) {
          std::optional<Candidate> original;
          if (visible && !visible->directionalBindings.empty())
            original = candidateForBinding(source,visible->directionalBindings.front(),sourceToExport);
          else if (!bindings && !candidates[scene].directions.empty()) original = candidates[scene].directions.front();
          if (original && bindingSceneCounts.at({original->node, original->extension}) > 1)
            throw std::runtime_error("Scene light export: a manual sun cannot replace a source binding shared by multiple scenes; use separate source light nodes");
        }
        filtered.push_back(entity);
        continue;
      }
      Candidate original;
      if (bindings) {
        const auto& table = bindingsFor(*bindings,entity.type);
        require(entity.id.index < table.size());
        original = candidateForBinding(source,table[entity.id.index],sourceToExport);
      } else {
        const auto& table = candidatesFor(candidates[scene], entity.type);
        require(entity.id.index < table.size());
        original = table[entity.id.index];
      }
      require(original.node < model.nodes.size());
      const Binding binding{original.node, original.extension};
      if (const auto previous = handled.find(binding); previous != handled.end()) {
        if (!sameImportedLightEdit(previous->second.entity, entity))
          throw std::runtime_error("Scene light export: conflicting edits on a source light node shared by multiple scenes; use separate source light nodes");
        if (isActiveDirectional(bindings,visible,entity)) sharedSuns.push_back(previous->second.exportedNode);
        continue;
      }
      addedBindings.push_back({binding, filtered.size()});
      handled.emplace(binding, Handled{entity});
      filtered.push_back(entity);
    }
    const size_t firstLight = output.lights.size();
    const size_t firstNode = output.nodes.size();
    if (!filtered.empty()) {
      sourceNodes.resize(output.nodes.size(), -1);
      exportEditableSceneLightsForScene(output, filtered, &source, sourceNodes,
                                       importedRangeScale, scene, bindings, visible);
      for (const auto& [binding, ordinal] : addedBindings) {
        const auto light = firstLight + ordinal;
        const auto found = std::find_if(output.nodes.begin() + firstNode, output.nodes.end(),
            [&](const auto& node) { return node.light >= 0 && size_t(node.light) == light; });
        require(found != output.nodes.end());
        handled.at(binding).exportedNode = size_t(found - output.nodes.begin());
      }
    }
    for (const auto sun : sharedSuns) prioritizeNode(output, sun);
  }
  output.defaultScene = selectedScene;
  model = std::move(output);
}
} // namespace container::renderer
