#include <Container/geometry/GltfCameraPoseAuthoring.h>

#include <Container/geometry/GltfAnimationRuntime.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace container::geometry::gltf {
namespace {
using V=tinygltf::Value;
[[noreturn]] void fail(const char* message) {throw std::invalid_argument(std::string("glTF camera pose: ")+message);}
bool exact(double a,double b) noexcept {return std::bit_cast<uint64_t>(a)==std::bit_cast<uint64_t>(b);}
bool exact(float a,float b) noexcept {return std::bit_cast<uint32_t>(a)==std::bit_cast<uint32_t>(b);}
template<class Matrix> bool matrixFinite(const Matrix& m) noexcept {
  for(int c=0;c<4;++c) for(int r=0;r<4;++r) if(!std::isfinite(m[c][r])) return false;
  return true;
}
bool exactMatrix(const glm::mat4& a,const glm::mat4& b) noexcept {
  for(int c=0;c<4;++c) for(int r=0;r<4;++r) if(!exact(a[c][r],b[c][r])) return false;
  return true;
}
bool valueStorage(const V& v) noexcept {
  return (v.IsString() || v.Get<std::string>().empty()) &&
      (v.IsBinary() || v.Get<std::vector<unsigned char>>().empty()) &&
      (v.IsArray() || v.Get<V::Array>().empty()) && (v.IsObject() || v.Get<V::Object>().empty()) &&
      (v.IsString() || v.IsBinary() || v.IsArray() || v.IsObject() || v.IsBool() || v.IsNumber() || v.Type()==tinygltf::NULL_TYPE);
}
bool exactValue(const V& a,const V& b) noexcept {
  if(a.Type()!=b.Type() || !valueStorage(a) || !valueStorage(b)) return false;
  if(a.IsInt()) return a.Get<int>()==b.Get<int>();
  if(a.IsReal()) return exact(a.Get<double>(),b.Get<double>());
  if(a.IsBool()) return a.Get<bool>()==b.Get<bool>();
  if(a.IsString()) return a.Get<std::string>()==b.Get<std::string>();
  if(a.IsBinary()) return a.Get<std::vector<unsigned char>>()==b.Get<std::vector<unsigned char>>();
  if(a.IsArray()) {
    const auto& x=a.Get<V::Array>();const auto& y=b.Get<V::Array>();if(x.size()!=y.size()) return false;
    for(size_t i=0;i<x.size();++i) if(!exactValue(x[i],y[i])) return false;
  } else if(a.IsObject()) {
    const auto& x=a.Get<V::Object>();const auto& y=b.Get<V::Object>();if(x.size()!=y.size()) return false;
    auto next=y.begin();for(const auto& [key,value]:x) {
      if(key!=next->first || !exactValue(value,next->second)) return false;++next;
    }
  }
  return true;
}
bool exactExtensions(const tinygltf::ExtensionMap& a,const tinygltf::ExtensionMap& b) noexcept {
  if(a.size()!=b.size()) return false;auto next=b.begin();
  for(const auto& [key,value]:a) {if(key!=next->first || !exactValue(value,next->second)) return false;++next;}
  return true;
}
template<class T> bool exactMetadata(const T& a,const T& b) noexcept {
  return exactValue(a.extras,b.extras) && exactExtensions(a.extensions,b.extensions) &&
      a.extras_json_string==b.extras_json_string && a.extensions_json_string==b.extensions_json_string;
}
bool exactDoubles(const std::vector<double>& a,const std::vector<double>& b) noexcept {
  if(a.size()!=b.size()) return false;
  for(size_t i=0;i<a.size();++i) if(!exact(a[i],b[i])) return false;return true;
}
bool exactPose(const tinygltf::Node& a,const tinygltf::Node& b) noexcept {
  return exactDoubles(a.matrix,b.matrix) && exactDoubles(a.translation,b.translation) &&
      exactDoubles(a.rotation,b.rotation) && exactDoubles(a.scale,b.scale);
}
bool exactNode(const tinygltf::Node& a,const tinygltf::Node& b,bool weights=true) noexcept {
  return exactPose(a,b) && a.camera==b.camera && a.skin==b.skin && a.mesh==b.mesh && a.light==b.light && a.emitter==b.emitter &&
      a.lods==b.lods && a.children==b.children && a.name==b.name && exactMetadata(a,b) &&
      (!weights || exactDoubles(a.weights,b.weights));
}
bool exactChannel(const tinygltf::AnimationChannel& a,const tinygltf::AnimationChannel& b) noexcept {
  return a.sampler==b.sampler && a.target_node==b.target_node && a.target_path==b.target_path && exactMetadata(a,b) &&
      exactValue(a.target_extras,b.target_extras) && exactExtensions(a.target_extensions,b.target_extensions) &&
      a.target_extras_json_string==b.target_extras_json_string && a.target_extensions_json_string==b.target_extensions_json_string;
}
bool exactGraph(const GltfCameraPoseGraphState& a,const GltfCameraPoseGraphState& b) noexcept {
  return a.identity==b.identity && a.revision==b.revision && a.nodeIncarnation==b.nodeIncarnation &&
      a.parentIncarnation==b.parentIncarnation && a.nodeCount==b.nodeCount && a.node==b.node && a.parent==b.parent &&
      a.siblingIndex==b.siblingIndex && exact(a.viewportAspect,b.viewportAspect) &&
      exactMatrix(a.parentWorld,b.parentWorld) && exactMatrix(a.local,b.local) && exactMatrix(a.world,b.world);
}
bool limitsValid(const GltfCameraPoseAuthoringLimits& l) noexcept {
  return l.maxOwnedBytes && l.maxOwnedBytes!=UINT64_MAX && l.maxMetadataValues && l.maxMetadataValues!=UINT64_MAX &&
      l.maxMetadataDepth<=256 && l.maxNodes && l.maxNodes!=UINT64_MAX && l.maxCameraRecords && l.maxCameraRecords!=UINT64_MAX &&
      l.maxGraphNodes && l.maxGraphNodes<=UINT32_MAX && l.maxAnimationClips!=UINT64_MAX && l.maxAnimationChannels!=UINT64_MAX &&
      l.maxPointerBytes && l.maxPointerBytes!=UINT64_MAX && std::isfinite(l.maxConditionNumber) && l.maxConditionNumber>=1 &&
      std::isfinite(l.maxRelativeError) && l.maxRelativeError>0 && l.maxRelativeError<=1e-2 &&
      std::isfinite(l.maxAngularError) && l.maxAngularError>0 && l.maxAngularError<=1e-2 &&
      std::isfinite(l.maxPositionAbsoluteError) && l.maxPositionAbsoluteError>0;
}
struct Budget {
  const GltfCameraPoseAuthoringLimits& limits;uint64_t bytes{},values{};
  bool add(uint64_t n) noexcept {if(bytes>limits.maxOwnedBytes || n>limits.maxOwnedBytes-bytes) return false;bytes+=n;return true;}
  bool array(uint64_t count,uint64_t width) noexcept {return count<=UINT64_MAX/width && add(count*width);}
  bool value(const V& v,uint32_t depth=0) noexcept {
    if(depth>limits.maxMetadataDepth || values==limits.maxMetadataValues || !valueStorage(v) || !add(sizeof(V))) return false;
    ++values;
    if(v.IsString()) return add(v.Get<std::string>().size());
    if(v.IsBinary()) return add(v.Get<std::vector<unsigned char>>().size());
    if(v.IsNumber()) return std::isfinite(v.GetNumberAsDouble());
    if(v.IsArray()) {for(const auto& child:v.Get<V::Array>()) if(!value(child,depth+1)) return false;}
    else if(v.IsObject()) for(const auto& [key,child]:v.Get<V::Object>())
      if(!add(128) || !add(key.size()) || !value(child,depth+1)) return false;
    return true;
  }
  bool extensions(const tinygltf::ExtensionMap& map) noexcept {
    for(const auto& [key,v]:map) if(!add(128) || !add(key.size()) || !value(v)) return false;return true;
  }
  template<class T> bool metadata(const T& v) noexcept {
    return add(v.extras_json_string.size()) && add(v.extensions_json_string.size()) && value(v.extras) && extensions(v.extensions);
  }
  bool doubles(const std::vector<double>& v) noexcept {
    if(!array(v.size(),sizeof(double))) return false;for(const auto n:v) if(!std::isfinite(n)) return false;return true;
  }
  bool node(const tinygltf::Node& n,bool weights=true) noexcept {
    return add(sizeof(n)) && add(n.name.size()) && array(n.children.size(),sizeof(int)) && array(n.lods.size(),sizeof(int)) &&
        doubles(n.matrix) && doubles(n.translation) && doubles(n.rotation) && doubles(n.scale) &&
        (!weights || doubles(n.weights)) && metadata(n);
  }
  bool camera(const tinygltf::Camera& c) noexcept {
    return add(sizeof(c)) && add(c.name.size()) && add(c.type.size()) && metadata(c) && metadata(c.perspective) && metadata(c.orthographic);
  }
  bool light(const tinygltf::Light& l) noexcept {
    return add(sizeof(l)) && add(l.name.size()) && add(l.type.size()) && doubles(l.color) && metadata(l) && metadata(l.spot) &&
        std::isfinite(l.intensity) && std::isfinite(l.range) && std::isfinite(l.spot.innerConeAngle) && std::isfinite(l.spot.outerConeAngle);
  }
  bool channel(const tinygltf::AnimationChannel& c) noexcept {
    return add(sizeof(c)) && add(c.target_path.size()) && metadata(c) && value(c.target_extras) && extensions(c.target_extensions) &&
        add(c.target_extras_json_string.size()) && add(c.target_extensions_json_string.size());
  }
};
bool nodeShape(const tinygltf::Node& n,size_t count) noexcept {
  if((!n.matrix.empty() && (n.matrix.size()!=16 || !n.translation.empty() || !n.rotation.empty() || !n.scale.empty())) ||
      (!n.translation.empty() && n.translation.size()!=3) || (!n.rotation.empty() && n.rotation.size()!=4) ||
      (!n.scale.empty() && n.scale.size()!=3)) return false;
  for(const auto* v:{&n.matrix,&n.translation,&n.rotation,&n.scale}) for(double d:*v)
    if(!std::isfinite(d) || std::abs(d)>std::numeric_limits<float>::max()) return false;
  if(!n.rotation.empty()) {
    const double norm=std::hypot(std::hypot(n.rotation[0],n.rotation[1]),std::hypot(n.rotation[2],n.rotation[3]));
    if(!std::isfinite(norm) || std::abs(norm-1)>1e-4) return false;
  }
  if(!n.matrix.empty() && (n.matrix[3]!=0 || n.matrix[7]!=0 || n.matrix[11]!=0 || n.matrix[15]!=1)) return false;
  for(const auto child:n.children) if(child<0 || size_t(child)>=count) return false;
  return true;
}
template<class M> bool affine(const M& m,double tolerance) noexcept {
  return matrixFinite(m) && std::abs(double(m[0][3]))<=tolerance && std::abs(double(m[1][3]))<=tolerance &&
      std::abs(double(m[2][3]))<=tolerance && std::abs(double(m[3][3])-1)<=tolerance;
}
double norm(const glm::dmat3& m) noexcept {
  double result{};for(int r=0;r<3;++r) {double row{};for(int c=0;c<3;++c) row+=std::abs(m[c][r]);result=std::max(result,row);}return result;
}
glm::dmat4 checkedInverse(const glm::mat4& matrix,const GltfCameraPoseAuthoringLimits& limits) {
  if(!affine(matrix,limits.maxRelativeError)) fail("nonfinite or nonaffine pose matrix");
  const glm::dmat4 m(matrix);const glm::dmat3 linear(m);const auto det=glm::determinant(linear);
  if(!std::isfinite(det) || det==0) fail("singular pose matrix");
  const auto inverse=glm::inverse(m);
  const double condition=norm(linear)*norm(glm::dmat3(inverse));
  if(!matrixFinite(inverse) || !std::isfinite(condition) || condition>limits.maxConditionNumber)
    fail("pose matrix exceeds its conditioning limit");
  return inverse;
}
bool matrixNear(const glm::mat4& a,const glm::mat4& b,double relative,double absolute) noexcept {
  if(!matrixFinite(a) || !matrixFinite(b)) return false;
  for(int c=0;c<4;++c) for(int r=0;r<4;++r)
    if(std::abs(double(a[c][r])-double(b[c][r]))>absolute+relative*std::max(std::abs(double(a[c][r])),std::abs(double(b[c][r])))) return false;
  return true;
}
bool graphValid(const GltfCameraPoseGraphState& g,const GltfCameraPoseAuthoringLimits& l) {
  if(!g.identity || !g.nodeIncarnation || !g.nodeCount || g.nodeCount>l.maxGraphNodes || g.node>=g.nodeCount ||
      g.siblingIndex>=g.nodeCount || (g.parent!=UINT32_MAX && (g.parent>=g.nodeCount || g.parent==g.node || !g.parentIncarnation)) ||
      (g.parent==UINT32_MAX && g.parentIncarnation) || !std::isfinite(g.viewportAspect) || g.viewportAspect<=0 ||
      !affine(g.parentWorld,0) || !affine(g.local,0) || !affine(g.world,0)) return false;
  if(g.parent==UINT32_MAX && !matrixNear(g.parentWorld,glm::mat4(1),0,0)) return false;
  (void)checkedInverse(g.parentWorld,l);(void)checkedInverse(g.world,l);
  return matrixNear(g.parentWorld*g.local,g.world,l.maxRelativeError,l.maxPositionAbsoluteError);
}
glm::mat4 floatMatrix(const glm::dmat4& m) {
  glm::mat4 result;
  for(int c=0;c<4;++c) for(int r=0;r<4;++r) {
    const double value=m[c][r];
    if(!std::isfinite(value) || std::abs(value)>std::numeric_limits<float>::max() || (value!=0 && float(value)==0))
      fail("pose cannot be represented in binary32");
    result[c][r]=float(value);
  }
  return result;
}
glm::dmat4 rigidViewInverse(const glm::mat4& view,const GltfCameraPoseAuthoringLimits& limits) {
  if(!affine(view,0)) fail("desired camera view is not affine");
  const auto inverse=checkedInverse(view,limits);const glm::dmat3 basis(view);
  const auto gram=glm::transpose(basis)*basis;
  for(int c=0;c<3;++c) for(int r=0;r<3;++r)
    if(std::abs(gram[c][r]-(c==r?1.:0.))>limits.maxAngularError) fail("desired view is not an orthonormal camera view");
  if(std::abs(glm::determinant(basis)-1)>limits.maxAngularError) fail("desired view contains a reflection");
  return inverse;
}
bool viewMatches(const glm::mat4& actual,const glm::mat4& desired,const GltfCameraPoseAuthoringLimits& l) {
  for(int c=0;c<3;++c) for(int r=0;r<3;++r)
    if(std::abs(double(actual[c][r])-desired[c][r])>l.maxAngularError) return false;
  const auto actualWorld=glm::inverse(glm::dmat4(actual)),desiredWorld=glm::inverse(glm::dmat4(desired));
  if(!matrixFinite(actualWorld) || !matrixFinite(desiredWorld)) return false;
  for(int r=0;r<3;++r) if(std::abs(actualWorld[3][r]-desiredWorld[3][r])>
      l.maxPositionAbsoluteError+l.maxRelativeError*std::max(std::abs(actualWorld[3][r]),std::abs(desiredWorld[3][r]))) return false;
  return true;
}
bool decimal(std::string_view text,size_t& result) noexcept {
  if(text.empty() || (text.size()>1 && text.front()=='0')) return false;result=0;
  for(char c:text) {if(c<'0' || c>'9' || result>(SIZE_MAX-size_t(c-'0'))/10) return false;result=result*10+size_t(c-'0');}return true;
}
const V* objectLeaf(const V& object,std::string_view key) noexcept {
  if(!object.IsObject()) return nullptr;
  for(const auto& [name,value]:object.Get<V::Object>()) if(name==key) return &value;
  return nullptr;
}
bool selectedNodePointer(std::string_view path,size_t nodes,int32_t selected,
    std::optional<GltfCameraNodeAnimationBinding>& binding) noexcept {
  if(path.empty() || path.front()!='/') return false;
  if(path=="/nodes") return false;
  if(!path.starts_with("/nodes/")) return true;
  path.remove_prefix(7);const auto slash=path.find('/');const auto token=path.substr(0,slash);size_t node{};
  if(!decimal(token,node) || node>=nodes) return false;
  if(node!=size_t(selected)) return true;
  if(slash==std::string_view::npos) return false;path.remove_prefix(slash+1);
  const auto componentSlash=path.find('/');const auto property=path.substr(0,componentSlash);
  GltfCameraNodeAnimationBinding result;
  if(property=="translation") result.property=GltfCameraNodeAnimationProperty::Translation;
  else if(property=="rotation") result.property=GltfCameraNodeAnimationProperty::Rotation;
  else if(property=="scale") result.property=GltfCameraNodeAnimationProperty::Scale;
  else if(property!="weights") return false;
  if(componentSlash!=std::string_view::npos) {
    size_t component{};
    if(result.property==GltfCameraNodeAnimationProperty::Rotation || !decimal(path.substr(componentSlash+1),component) ||
        component>(result.property==GltfCameraNodeAnimationProperty::Weights?UINT32_MAX:2)) return false;
    result.element=component;
  }
  binding=result;return true;
}
struct ScanInfo {uint64_t bindings{};uint8_t poseProperties{};};
template<class Visit> bool scan(GltfCameraPoseSourceView source,int32_t node,const GltfCameraPoseAuthoringLimits& limits,
    Budget* budget,ScanInfo& info,Visit&& visit) {
  if(node<0 || size_t(node)>=source.nodes.size() || source.nodes.size()>limits.maxNodes ||
      source.cameras.size()>limits.maxCameraRecords || source.animations.size()>limits.maxAnimationClips) return false;
  uint64_t channels{},extensions{},pointerBytes{};
  for(size_t c=0;c<source.animations.size();++c) {
    const auto& animation=source.animations[c];
    if(animation.channels.size()>limits.maxAnimationChannels-channels) return false;channels+=animation.channels.size();
    for(size_t k=0;k<animation.channels.size();++k) {
      const auto& channel=animation.channels[k];std::optional<GltfCameraNodeAnimationBinding> binding;
      if(channel.target_extensions.size()>limits.maxMetadataValues-extensions) return false;extensions+=channel.target_extensions.size();
      const auto ext=std::find_if(channel.target_extensions.begin(),channel.target_extensions.end(),
          [](const auto& item) {return item.first=="KHR_animation_pointer";});
      if(channel.target_path=="pointer") {
        if(channel.target_node!=-1 || ext==channel.target_extensions.end()) return false;
        const auto* pointer=objectLeaf(ext->second,"pointer");if(!pointer || !pointer->IsString()) return false;
        const auto& path=pointer->Get<std::string>();
        if(path.size()>limits.maxPointerBytes || path.size()>limits.maxOwnedBytes-pointerBytes) return false;pointerBytes+=path.size();
        if(!selectedNodePointer(path,source.nodes.size(),node,binding)) return false;
      } else {
        if(channel.target_node< -1 || ext!=channel.target_extensions.end()) return false;
        if(channel.target_node==node) {
          binding.emplace();
          if(channel.target_path=="translation") binding->property=GltfCameraNodeAnimationProperty::Translation;
          else if(channel.target_path=="rotation") binding->property=GltfCameraNodeAnimationProperty::Rotation;
          else if(channel.target_path=="scale") binding->property=GltfCameraNodeAnimationProperty::Scale;
          else if(channel.target_path!="weights") return false;
        }
      }
      if(!binding) continue;
      if(binding->property!=GltfCameraNodeAnimationProperty::Weights) {
        if(!source.nodes[size_t(node)].matrix.empty()) return false;
        info.poseProperties|=uint8_t(1u<<size_t(binding->property));
      }
      if(channel.sampler<0 || size_t(channel.sampler)>=animation.samplers.size()) return false;
      ++info.bindings;
      binding->clip=c;binding->channel=k;
      if((budget && !budget->channel(channel)) || !visit(*binding,channel)) return false;
    }
  }
  return true;
}
struct SourceWitness {size_t nodes{},cameras{},animations{};std::vector<tinygltf::AnimationChannel> channels;};
bool planPublicEqual(const GltfCameraPosePlan& a,const GltfCameraPosePlan& b) noexcept {
  return a.authoredNode==b.authoredNode && a.evaluatedNode==b.evaluatedNode && a.authoredCamera==b.authoredCamera && a.evaluatedCamera==b.evaluatedCamera &&
      exactNode(a.authoredBefore,b.authoredBefore) && exactNode(a.authoredAfter,b.authoredAfter) && exactNode(a.evaluatedBefore,b.evaluatedBefore) &&
      exactNode(a.evaluatedAfter,b.evaluatedAfter) && exactGraph(a.graphBefore,b.graphBefore) && exactMatrix(a.desiredView,b.desiredView) &&
      exactMatrix(a.localAfter,b.localAfter) && exactMatrix(a.worldAfter,b.worldAfter) && exactMatrix(a.viewAfter,b.viewAfter) &&
      a.authoredAnimationBindings==b.authoredAnimationBindings && a.evaluatedAnimationBindings==b.evaluatedAnimationBindings &&
      a.changed==b.changed && a.peakOwnedBytes==b.peakOwnedBytes;
}
void setMatrix(tinygltf::Node& node,const glm::mat4& matrix) {
  node.matrix.resize(16);for(int c=0;c<4;++c) for(int r=0;r<4;++r) node.matrix[size_t(c*4+r)]=matrix[c][r];
  node.translation.clear();node.rotation.clear();node.scale.clear();
}
void copyPose(tinygltf::Node& to,const tinygltf::Node& from) {
  to.matrix=from.matrix;to.translation=from.translation;to.rotation=from.rotation;to.scale=from.scale;
}
uint8_t animatedPoseMask(std::span<const GltfCameraNodeAnimationBinding> bindings) noexcept {
  uint8_t result{};for(const auto& binding:bindings)
    if(binding.property!=GltfCameraNodeAnimationProperty::Weights) result|=uint8_t(1u<<size_t(binding.property));
  return result;
}
std::vector<double>& poseValues(tinygltf::Node& node,GltfCameraNodeAnimationProperty property) noexcept {
  if(property==GltfCameraNodeAnimationProperty::Translation) return node.translation;
  if(property==GltfCameraNodeAnimationProperty::Rotation) return node.rotation;
  return node.scale;
}
const std::vector<double>& poseValues(const tinygltf::Node& node,GltfCameraNodeAnimationProperty property) noexcept {
  if(property==GltfCameraNodeAnimationProperty::Translation) return node.translation;
  if(property==GltfCameraNodeAnimationProperty::Rotation) return node.rotation;
  return node.scale;
}
double poseDefault(GltfCameraNodeAnimationProperty property,size_t component) noexcept {
  return property==GltfCameraNodeAnimationProperty::Scale ||
      (property==GltfCameraNodeAnimationProperty::Rotation && component==3)?1.:0.;
}
void preserveAnimatedPose(tinygltf::Node& to,const tinygltf::Node& from,
    std::span<const GltfCameraNodeAnimationBinding> bindings) {
  for(const auto& binding:bindings) {
    if(binding.property==GltfCameraNodeAnimationProperty::Weights) continue;
    auto& destination=poseValues(to,binding.property);const auto& source=poseValues(from,binding.property);
    if(!binding.element) destination=source;
    else {
      if(destination.empty()) destination.assign(3,poseDefault(binding.property,0));
      destination[*binding.element]=source.empty()?poseDefault(binding.property,*binding.element):source[*binding.element];
    }
  }
}
bool ownsPoseProperty(const GltfCameraPosePlan& command,GltfCameraNodeAnimationProperty property) noexcept {
  return !exactDoubles(poseValues(command.authoredBefore,property),poseValues(command.authoredAfter,property)) ||
      !exactDoubles(poseValues(command.evaluatedBefore,property),poseValues(command.evaluatedAfter,property));
}
bool unanimatedPoseMatches(const tinygltf::Node& current,const tinygltf::Node& expected,const tinygltf::Node& rest,
    const GltfCameraPosePlan& command,std::span<const GltfCameraNodeAnimationBinding> bindings,std::optional<size_t> selectedClip) noexcept {
  if(!current.matrix.empty() || !expected.matrix.empty()) return false;
  for(const auto property:{GltfCameraNodeAnimationProperty::Translation,GltfCameraNodeAnimationProperty::Rotation,
      GltfCameraNodeAnimationProperty::Scale}) {
    if(!ownsPoseProperty(command,property)) continue;
    const auto& a=poseValues(current,property);const auto& b=poseValues(expected,property);const auto& defaults=poseValues(rest,property);
    for(size_t component=0;component<(property==GltfCameraNodeAnimationProperty::Rotation?4u:3u);++component) {
      const auto targets=[&](const auto& binding) {
        return binding.property==property && (!binding.element || *binding.element==component);
      };
      const bool tracked=std::any_of(bindings.begin(),bindings.end(),targets);
      const bool animated=selectedClip && std::any_of(bindings.begin(),bindings.end(),[&](const auto& binding) {
        return binding.clip==*selectedClip && targets(binding);
      });
      const auto& wanted=tracked?defaults:b;
      if(!animated && !exact(a.empty()?poseDefault(property,component):a[component],
          wanted.empty()?poseDefault(property,component):wanted[component])) return false;
    }
  }
  return true;
}
void setTrs(tinygltf::Node& node,const glm::mat4& matrix,const tinygltf::Node& preferred,
    const GltfCameraPoseAuthoringLimits& limits) {
  glm::dmat3 rotation(matrix);glm::dvec3 scale;
  for(int c=0;c<3;++c) {
    scale[c]=glm::length(rotation[c]);
    if(!std::isfinite(scale[c]) || scale[c]==0) fail("animated camera rest has a singular TRS scale");
    rotation[c]/=scale[c];
  }
  for(int c=0;c<3;++c) for(int r=0;r<c;++r)
    if(std::abs(glm::dot(rotation[c],rotation[r]))>limits.maxRelativeError)
      fail("requested animated camera rest requires local shear and cannot be represented as TRS");
  glm::dvec3 signs(1);
  if(preferred.scale.size()==3) for(int c=0;c<3;++c) if(std::signbit(preferred.scale[size_t(c)])) signs[c]=-1;
  if(glm::determinant(rotation)*signs.x*signs.y*signs.z<0) signs.x=-signs.x;
  for(int c=0;c<3;++c) {rotation[c]*=signs[c];scale[c]*=signs[c];}
  auto quaternion=glm::normalize(glm::quat_cast(rotation));
  if(preferred.rotation.size()==4) {
    const glm::dquat before(preferred.rotation[3],preferred.rotation[0],preferred.rotation[1],preferred.rotation[2]);
    if(glm::dot(quaternion,before)<0) quaternion=-quaternion;
  } else if(quaternion.w<0) quaternion=-quaternion;
  node.matrix.clear();node.translation={matrix[3][0],matrix[3][1],matrix[3][2]};
  node.rotation={quaternion.x,quaternion.y,quaternion.z,quaternion.w};node.scale={scale.x,scale.y,scale.z};
  const auto actual=EvaluateAnimatedNodeLocalTransform(node);
  if(!matrixNear(actual,matrix,limits.maxRelativeError,limits.maxPositionAbsoluteError))
    fail("animated camera rest cannot retain its solved pose through binary32 TRS recomposition");
}
} // namespace

class GltfCameraPoseReceipt {
public:
  GltfCameraPosePlan expected;
  GltfCameraPoseAuthoringLimits limits;
  tinygltf::Camera authoredLens,evaluatedLens;
  SourceWitness authored,evaluated;
  std::shared_ptr<const GltfCameraPoseReceipt> command;
  std::optional<size_t> selectedClip;
};
struct GltfCameraPoseAccess {
  static const auto& receipt(const GltfCameraPosePlan& plan) noexcept {return plan.receipt_;}
  static void publish(GltfCameraPosePlan& plan,std::shared_ptr<const GltfCameraPoseReceipt> receipt) noexcept {plan.receipt_=std::move(receipt);}
};
namespace {
bool receiptMatches(const GltfCameraPosePlan& plan) noexcept {
  const auto& receipt=GltfCameraPoseAccess::receipt(plan);if(!receipt) return false;Budget budget{receipt->limits};
  return budget.node(plan.authoredBefore) && budget.node(plan.authoredAfter) && budget.node(plan.evaluatedBefore) && budget.node(plan.evaluatedAfter) &&
      planPublicEqual(plan,receipt->expected);
}
bool sourceMatches(GltfCameraPoseSourceView source,int32_t node,int32_t camera,const tinygltf::Node& before,
    const tinygltf::Camera& lens,std::span<const GltfCameraNodeAnimationBinding> bindings,const SourceWitness& witness,
    const GltfCameraPoseAuthoringLimits& limits,Budget& budget) noexcept {
  if(source.nodes.size()!=witness.nodes || source.cameras.size()!=witness.cameras || source.animations.size()!=witness.animations ||
      node<0 || camera<0 || size_t(node)>=source.nodes.size() || size_t(camera)>=source.cameras.size() ||
      !budget.node(source.nodes[size_t(node)],false) || !budget.camera(source.cameras[size_t(camera)]) ||
      !exactNode(source.nodes[size_t(node)],before,false) || !GltfCameraRecordsEqual(source.cameras[size_t(camera)],lens)) return false;
  size_t i{};ScanInfo info;
  const bool valid=scan(source,node,limits,&budget,info,[&](const auto& binding,const auto& channel) {
    if(i>=bindings.size() || i>=witness.channels.size() || binding!=bindings[i] || !exactChannel(channel,witness.channels[i])) return false;++i;return true;
  });
  return valid && i==bindings.size() && i==witness.channels.size();
}
void capture(GltfCameraPoseSourceView source,int32_t node,const ScanInfo& info,const GltfCameraPoseAuthoringLimits& limits,
    std::vector<GltfCameraNodeAnimationBinding>& bindings,SourceWitness& witness) {
  witness.nodes=source.nodes.size();witness.cameras=source.cameras.size();witness.animations=source.animations.size();
  bindings.reserve(size_t(info.bindings));witness.channels.reserve(size_t(info.bindings));ScanInfo ignored;
  if(!scan(source,node,limits,nullptr,ignored,[&](const auto& binding,const auto& channel) {
    bindings.push_back(binding);witness.channels.push_back(channel);return true;
  })) fail("selected source animation changed during preparation");
}
void validateSelection(GltfCameraPoseSourceView source,int32_t node,const GltfCameraPoseAuthoringLimits& limits) {
  if(node<0 || size_t(node)>=source.nodes.size() || source.nodes.size()>limits.maxNodes || source.cameras.size()>limits.maxCameraRecords)
    fail("selected source node identity is unavailable or over budget");
  const auto& n=source.nodes[size_t(node)];
  if(n.camera<0 || size_t(n.camera)>=source.cameras.size() || !nodeShape(n,source.nodes.size()) ||
      !GltfCameraProjectionValid(source.cameras[size_t(n.camera)])) fail("selected node/camera record is malformed");
}
GltfCameraPosePlan buildPlan(GltfCameraPoseSourceView authored,GltfCameraPoseSourceView evaluated,int32_t aNode,int32_t eNode,
    const GltfCameraPoseGraphState& graph,const glm::mat4& desired,const glm::mat4& local,const glm::mat4& world,const glm::mat4& view,
    const tinygltf::Node* restoreA,const tinygltf::Node* restoreE,bool noChange,const GltfCameraPoseAuthoringLimits& limits,
    std::shared_ptr<const GltfCameraPoseReceipt> command={},std::optional<size_t> selectedClip={}) {
  validateSelection(authored,aNode,limits);validateSelection(evaluated,eNode,limits);
  const auto& a=authored.nodes[size_t(aNode)];const auto& e=evaluated.nodes[size_t(eNode)];Budget budget{limits};
  if(!budget.add(sizeof(GltfCameraPosePlan)+sizeof(GltfCameraPoseReceipt)+2048) ||
      (command && !budget.add(command->expected.peakOwnedBytes))) fail("selected pose command storage exceeds its budget");
  for(int i=0;i<4;++i) if(!budget.node(a) || !budget.node(e)) fail("selected node metadata exceeds its finite budget");
  if(!budget.camera(authored.cameras[size_t(a.camera)]) || !budget.camera(evaluated.cameras[size_t(e.camera)])) fail("lens witness metadata exceeds its budget");
  ScanInfo ai,ei;
  if(!scan(authored,aNode,limits,&budget,ai,[](const auto&,const auto&) {return true;}) ||
      !scan(evaluated,eNode,limits,&budget,ei,[](const auto&,const auto&) {return true;})) fail("selected node animation context is malformed, unsupported or over budget");
  if(!budget.array(ai.bindings,2*sizeof(GltfCameraNodeAnimationBinding)) || !budget.array(ei.bindings,2*sizeof(GltfCameraNodeAnimationBinding)))
    fail("selected raw node target storage exceeds its budget");
  if(restoreA && (!budget.node(*restoreA) || !budget.node(*restoreE))) fail("replayed selected pose storage exceeds its budget");
  GltfCameraPosePlan plan;plan.authoredNode=aNode;plan.evaluatedNode=eNode;plan.authoredCamera=a.camera;plan.evaluatedCamera=e.camera;
  plan.authoredBefore=a;plan.authoredAfter=a;plan.evaluatedBefore=e;plan.evaluatedAfter=e;
  auto receipt=std::make_shared<GltfCameraPoseReceipt>();receipt->limits=limits;receipt->command=std::move(command);
  receipt->selectedClip=selectedClip;
  receipt->authoredLens=authored.cameras[size_t(a.camera)];receipt->evaluatedLens=evaluated.cameras[size_t(e.camera)];
  capture(authored,aNode,ai,limits,plan.authoredAnimationBindings,receipt->authored);
  capture(evaluated,eNode,ei,limits,plan.evaluatedAnimationBindings,receipt->evaluated);
  const auto animated=uint8_t(ai.poseProperties|ei.poseProperties);
  if(animated && !exactMatrix(EvaluateAnimatedNodeLocalTransform(e),graph.local))
    fail("animated selected node and current graph local pose differ");
  if(restoreA) {copyPose(plan.authoredAfter,*restoreA);copyPose(plan.evaluatedAfter,*restoreE);}
  else if(!noChange || (animated && !exactMatrix(EvaluateAnimatedNodeLocalTransform(a),local))) {
    if(animated) setTrs(plan.authoredAfter,local,e,limits);else setMatrix(plan.authoredAfter,local);
    copyPose(plan.evaluatedAfter,plan.authoredAfter);
  }
  plan.graphBefore=graph;plan.desiredView=desired;plan.localAfter=local;plan.worldAfter=world;plan.viewAfter=view;
  if(animated) {
    if(!plan.authoredAfter.matrix.empty() || !plan.evaluatedAfter.matrix.empty()) fail("animated camera rest must retain TRS representation");
    preserveAnimatedPose(plan.evaluatedAfter,e,plan.evaluatedAnimationBindings);
    plan.localAfter=EvaluateAnimatedNodeLocalTransform(plan.authoredAfter);plan.worldAfter=graph.parentWorld*plan.localAfter;
    (void)checkedInverse(plan.worldAfter,limits);
    const auto candidate=EvaluateAnimatedCamera(evaluated.cameras[size_t(e.camera)],plan.worldAfter,graph.viewportAspect);
    if(!candidate || !viewMatches(candidate->view,desired,limits)) fail("animated camera rest does not retain the requested eye and roll");
    plan.viewAfter=candidate->view;
  }
  plan.changed=!exactPose(a,plan.authoredAfter) || !exactPose(e,plan.evaluatedAfter) || (!animated && !exactMatrix(graph.local,local));
  plan.peakOwnedBytes=budget.bytes;
  receipt->expected=plan;GltfCameraPoseAccess::publish(plan,std::move(receipt));return plan;
}
} // namespace

GltfCameraPoseSourceView GltfCameraPoseView(const tinygltf::Model& model) noexcept {return GltfCameraSourceView(model);}
GltfCameraPosePlan PlanGltfCameraViewPose(GltfCameraPoseSourceView authored,GltfCameraPoseSourceView evaluated,
    int32_t aNode,int32_t eNode,const GltfCameraPoseGraphState& graph,const glm::mat4& desired,const GltfCameraPoseAuthoringLimits& limits) {
  if(!limitsValid(limits)) fail("pose authoring requires finite positive capacities and tolerances");
  validateSelection(authored,aNode,limits);validateSelection(evaluated,eNode,limits);
  if(!graphValid(graph,limits)) fail("actual graph pose/parent witness is invalid or inconsistent");
  const auto desiredWorld=rigidViewInverse(desired,limits);
  const auto& lens=evaluated.cameras[size_t(evaluated.nodes[size_t(eNode)].camera)];
  const auto old=EvaluateAnimatedCamera(lens,graph.world,graph.viewportAspect);if(!old) fail("selected camera cannot evaluate its current world pose");
  const bool noChange=matrixNear(old->view,desired,0,0);
  glm::mat4 local=graph.local,world=graph.world,view=old->view;
  if(!noChange) {
    const glm::dmat3 oldBasis(glm::inverse(glm::dmat4(old->view)));
    const auto residual=glm::transpose(oldBasis)*glm::dmat3(graph.world);
    glm::dmat4 wanted(1);const auto linear=glm::dmat3(desiredWorld)*residual;
    for(int c=0;c<3;++c) for(int r=0;r<3;++r) wanted[c][r]=linear[c][r];
    wanted[3]=desiredWorld[3];
    auto solved=checkedInverse(graph.parentWorld,limits)*wanted;
    if(!affine(solved,limits.maxRelativeError)) fail("solved local pose is not affine");
    // Inverse roundoff must not create a noncanonical glTF last row that a
    // later replay would reject as an invalid matrix representation.
    for(int c=0;c<3;++c) solved[c][3]=0;solved[3][3]=1;
    local=floatMatrix(solved);world=graph.parentWorld*local;
    if(!affine(world,limits.maxRelativeError)) fail("published float world pose is unrepresentable");
    (void)checkedInverse(world,limits);
    const auto candidate=EvaluateAnimatedCamera(lens,world,graph.viewportAspect);
    if(!candidate || !viewMatches(candidate->view,desired,limits)) fail("published camera eye or roll cannot match the desired view");view=candidate->view;
  }
  return buildPlan(authored,evaluated,aNode,eNode,graph,desired,local,world,view,nullptr,nullptr,noChange,limits);
}
bool GltfCameraPoseEditMatches(const GltfCameraPosePlan& plan,GltfCameraPoseSourceView authored,
    GltfCameraPoseSourceView evaluated,const GltfCameraPoseGraphState& graph) noexcept {
  if(!receiptMatches(plan) || !exactGraph(plan.graphBefore,graph)) return false;
  const auto& receipt=*GltfCameraPoseAccess::receipt(plan);Budget budget{receipt.limits};
  return sourceMatches(authored,plan.authoredNode,plan.authoredCamera,plan.authoredBefore,receipt.authoredLens,
      plan.authoredAnimationBindings,receipt.authored,receipt.limits,budget) &&
      sourceMatches(evaluated,plan.evaluatedNode,plan.evaluatedCamera,plan.evaluatedBefore,receipt.evaluatedLens,
      plan.evaluatedAnimationBindings,receipt.evaluated,receipt.limits,budget);
}
void ValidateGltfCameraPoseEditBefore(const GltfCameraPosePlan& plan,GltfCameraPoseSourceView authored,
    GltfCameraPoseSourceView evaluated,const GltfCameraPoseGraphState& graph) {
  if(!GltfCameraPoseEditMatches(plan,authored,evaluated,graph)) fail("stale or modified selected node pose receipt");
}
GltfCameraPosePlan RebaseGltfCameraViewPose(const GltfCameraPosePlan& original,GltfCameraEditSide side,
    GltfCameraPoseSourceView authored,GltfCameraPoseSourceView evaluated,const GltfCameraPoseGraphState& graph,
    const GltfCameraPoseAuthoringLimits& limits,std::optional<size_t> selectedClip) {
  if(!limitsValid(limits) || !receiptMatches(original) || (side!=GltfCameraEditSide::Before && side!=GltfCameraEditSide::After))
    fail("invalid or modified pose replay command");
  const auto& receipt=GltfCameraPoseAccess::receipt(original);const auto root=receipt->command?receipt->command:receipt;
  const auto& trusted=root->expected;const auto& before=trusted.graphBefore;
  if(graph.identity!=before.identity || graph.node!=before.node || graph.nodeIncarnation!=before.nodeIncarnation ||
      graph.parent!=before.parent || graph.parentIncarnation!=before.parentIncarnation || graph.siblingIndex!=before.siblingIndex ||
      !graphValid(graph,limits)) fail("pose history graph binding or original parent placement changed");
  validateSelection(authored,trusted.authoredNode,limits);validateSelection(evaluated,trusted.evaluatedNode,limits);
  const auto& a=authored.nodes[size_t(trusted.authoredNode)];const auto& e=evaluated.nodes[size_t(trusted.evaluatedNode)];
  const auto& expectedA=side==GltfCameraEditSide::Before?trusted.authoredAfter:trusted.authoredBefore;
  const auto& expectedE=side==GltfCameraEditSide::Before?trusted.evaluatedAfter:trusted.evaluatedBefore;
  const auto& wantedA=side==GltfCameraEditSide::Before?trusted.authoredBefore:trusted.authoredAfter;
  const auto& wantedE=side==GltfCameraEditSide::Before?trusted.evaluatedBefore:trusted.evaluatedAfter;
  const auto& expectedLocal=side==GltfCameraEditSide::Before?trusted.localAfter:trusted.graphBefore.local;
  auto local=side==GltfCameraEditSide::Before?trusted.graphBefore.local:trusted.localAfter;
  const bool animated=animatedPoseMask(trusted.authoredAnimationBindings)||animatedPoseMask(trusted.evaluatedAnimationBindings);
  if(a.camera!=trusted.authoredCamera || e.camera!=trusted.evaluatedCamera || !exactPose(a,expectedA) ||
      (animated?!unanimatedPoseMatches(e,expectedE,expectedA,trusted,trusted.evaluatedAnimationBindings,selectedClip):!exactPose(e,expectedE)) ||
      (animated?!exactMatrix(graph.local,EvaluateAnimatedNodeLocalTransform(e)):!exactMatrix(graph.local,expectedLocal)))
    fail("pose history owned endpoint or camera association changed");
  if(animated) local=EvaluateAnimatedNodeLocalTransform(wantedA);
  const glm::mat4 world=graph.parentWorld*local;
  (void)checkedInverse(world,limits);
  const auto candidate=EvaluateAnimatedCamera(evaluated.cameras[size_t(e.camera)],world,graph.viewportAspect);
  if(!candidate) fail("replayed camera cannot evaluate its current parent-relative endpoint");
  Budget replayBudget{limits};
  if(!replayBudget.add(sizeof(tinygltf::Node)+16*sizeof(double))) fail("replayed evaluated pose scratch exceeds its finite budget");
  tinygltf::Node wantedEvaluated;copyPose(wantedEvaluated,wantedE);
  if(animated) for(const auto property:{GltfCameraNodeAnimationProperty::Translation,GltfCameraNodeAnimationProperty::Rotation,
      GltfCameraNodeAnimationProperty::Scale})
    if(!ownsPoseProperty(trusted,property)) poseValues(wantedEvaluated,property)=poseValues(e,property);
  auto result=buildPlan(authored,evaluated,trusted.authoredNode,trusted.evaluatedNode,graph,candidate->view,local,world,candidate->view,
      &wantedA,&wantedEvaluated,false,limits,root,selectedClip);
  if(result.authoredAnimationBindings!=trusted.authoredAnimationBindings || result.evaluatedAnimationBindings!=trusted.evaluatedAnimationBindings)
    fail("pose history selected animation target schema changed");
  return result;
}
bool GltfCameraPoseReplayClipMatches(const GltfCameraPosePlan& plan,std::optional<size_t> selectedClip) noexcept {
  if(!receiptMatches(plan)) return false;
  const auto& receipt=GltfCameraPoseAccess::receipt(plan);
  return !receipt->command ||
      !(animatedPoseMask(plan.authoredAnimationBindings)||animatedPoseMask(plan.evaluatedAnimationBindings)) ||
      receipt->selectedClip==selectedClip;
}
uint64_t GltfCameraPoseNodeStorageBytes(const tinygltf::Node& node,const GltfCameraPoseAuthoringLimits& limits) {
  if(!limitsValid(limits)) fail("invalid selected node storage limits");Budget budget{limits};
  if(!budget.node(node)) fail("selected node metadata exceeds its storage budget");return budget.bytes;
}
bool GltfCameraNodePoseEqual(const tinygltf::Node& a,const tinygltf::Node& b) noexcept {return exactPose(a,b);}
bool GltfCameraPoseNodeRecordsEqual(const tinygltf::Node& a,const tinygltf::Node& b) noexcept {return exactNode(a,b);}
uint64_t GltfAuthoringValueStorageBytes(const V& value,const GltfCameraPoseAuthoringLimits& limits) {
  if(!limitsValid(limits)) fail("invalid selected value storage limits");Budget budget{limits};
  if(!budget.value(value)) fail("selected raw value exceeds its finite storage budget");return budget.bytes;
}
bool GltfAuthoringValuesEqual(const V& a,const V& b) noexcept {return exactValue(a,b);}
bool GltfCameraPoseValuesMatch(const V& a,const V& b,const GltfCameraPoseAuthoringLimits& limits) noexcept {
  if(!limitsValid(limits)) return false;Budget left{limits},right{limits};
  return left.value(a) && right.value(b) && exactValue(a,b);
}
uint64_t GltfCameraPoseLightStorageBytes(const tinygltf::Light& light,const GltfCameraPoseAuthoringLimits& limits) {
  if(!limitsValid(limits)) fail("invalid selected light storage limits");Budget budget{limits};
  if(!budget.light(light)) fail("selected light exceeds its finite storage budget");return budget.bytes;
}
bool GltfCameraPoseLightRecordsMatch(const tinygltf::Light& a,const tinygltf::Light& b,const GltfCameraPoseAuthoringLimits& limits) noexcept {
  if(!limitsValid(limits)) return false;Budget left{limits},right{limits};
  return left.light(a) && right.light(b) && a.name==b.name && a.type==b.type && exactDoubles(a.color,b.color) &&
      exact(a.intensity,b.intensity) && exact(a.range,b.range) && exact(a.spot.innerConeAngle,b.spot.innerConeAngle) &&
      exact(a.spot.outerConeAngle,b.spot.outerConeAngle) && exactMetadata(a,b) && exactMetadata(a.spot,b.spot);
}
} // namespace container::geometry::gltf
