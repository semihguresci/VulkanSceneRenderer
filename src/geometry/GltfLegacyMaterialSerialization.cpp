#include <Container/geometry/GltfLegacyMaterialSerialization.h>
#include <Container/geometry/GltfCameraAuthoring.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <climits>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace container::geometry::gltf {
namespace {
constexpr std::array<std::string_view,8> textureFields{
 "roughnessTexture","metalnessTexture","specularTexture","heightTexture",
 "displacementTexture","opacityTexture","refractionTexture","transmissionTexture"};
constexpr std::array<std::string_view,5> factorFields{"opacity","specular","refraction","heightScale","heightOffset"};
[[noreturn]] void fail(std::string_view message) {throw std::runtime_error("Scene legacy material serialization: "+std::string(message));}
uint64_t add(uint64_t a,uint64_t b) {if(b>UINT64_MAX-a) fail("size arithmetic overflow");return a+b;}
uint64_t multiply(uint64_t a,uint64_t b) {if(a && b>UINT64_MAX/a) fail("size arithmetic overflow");return a*b;}
void limitsValid(const GltfLegacyMaterialSerializationLimits& limits) {
 for(const auto n:{limits.maxMaterials,limits.maxParameterValues,limits.maxParameterBytes,limits.maxJsonBytes,limits.maxJsonValues,limits.maxOwnedBytes,limits.maxCameras})
  if(!n || n==UINT64_MAX) fail("finite nonzero serialization capacities are required");
 if(!limits.maxJsonDepth || limits.maxJsonDepth>256 || !limits.maxGlbChunks || limits.maxGlbChunks>65536 || limits.maxJsonBytes>std::numeric_limits<size_t>::max()) fail("JSON depth/byte/chunk capacity is invalid");
}
void utf8(std::string_view value) {
 for(size_t i=0;i<value.size();) {
  const auto c=static_cast<unsigned char>(value[i]);if(c<128) {++i;continue;}
  size_t count{};uint32_t code{};
  if(c>=0xc2 && c<=0xdf) {count=2;code=c&0x1f;}
  else if(c>=0xe0 && c<=0xef) {count=3;code=c&0x0f;}
  else if(c>=0xf0 && c<=0xf4) {count=4;code=c&7;}
  else fail("legacy parameter key is not valid UTF-8");
  if(count>value.size()-i) fail("truncated UTF-8 legacy parameter key");
  for(size_t j=1;j<count;++j) {const auto next=static_cast<unsigned char>(value[i+j]);if((next&0xc0)!=0x80) fail("invalid UTF-8 continuation");code=(code<<6)|(next&0x3f);}
  if((count==3 && code<0x800) || (count==4 && code<0x10000) || code>0x10ffff || (code>=0xd800 && code<=0xdfff)) fail("invalid UTF-8 scalar");i+=count;
 }
}
bool known(std::string_view key) {return std::ranges::find(textureFields,key)!=textureFields.end() || std::ranges::find(factorFields,key)!=factorFields.end();}
struct Prepared {
 bool present{};uint64_t values{},bytes{},encodedGrowth{};
};
Prepared preflight(std::span<const tinygltf::Material> materials,size_t textures,const GltfLegacyMaterialSerializationLimits& limits) {
 limitsValid(limits);if(materials.size()>limits.maxMaterials || textures>INT_MAX) fail("material/resource count exceeds its capacity");
 Prepared result;
 const auto charge=[&](uint64_t bytes) {
  if(++result.values>limits.maxParameterValues) fail("legacy parameter traversal exceeds its capacity");
  result.bytes=add(result.bytes,bytes);if(result.bytes>limits.maxParameterBytes) fail("legacy parameter metadata exceeds its capacity");
 };
 for(const auto& material:materials) for(const auto* map:{&material.values,&material.additionalValues}) {
  for(const auto field:textureFields) {
   const auto i=map->find(std::string(field));if(i==map->end()) continue;result.present=true;const auto& p=i->second;
   if(!p.number_array.empty() || !p.string_value.empty() || p.has_number_value || p.bool_value || p.json_double_value.empty()) fail("known legacy texture must be an unambiguous numeric object");
   charge(add(sizeof(p),field.size()));result.encodedGrowth=add(result.encodedGrowth,add(128,multiply(field.size(),6)));
   const auto index=p.json_double_value.find("index");
   if(index==p.json_double_value.end() || !std::isfinite(index->second) || index->second<0 || index->second>INT_MAX ||
      std::trunc(index->second)!=index->second || size_t(index->second)>=textures) fail("legacy texture source index is out of range");
   for(const auto& [name,value]:p.json_double_value) {
    charge(add(128,name.size()));
    utf8(name);if(!std::isfinite(value)) fail("legacy texture parameter must be finite");
    if((name=="texCoord" || name=="channel") && (value<0 || value>(name=="texCoord"?1:4) || std::trunc(value)!=value)) fail("legacy UV/channel selector is out of range");
    result.encodedGrowth=add(result.encodedGrowth,add(128,multiply(name.size(),6)));
   }
  }
  for(const auto field:factorFields) {
   const auto i=map->find(std::string(field));if(i==map->end()) continue;result.present=true;const auto& p=i->second;
   if(!p.has_number_value || !p.number_array.empty() || !p.json_double_value.empty() || !p.string_value.empty() || p.bool_value || !std::isfinite(p.number_value))
    fail("known legacy factor must be an unambiguous finite scalar");
   if((field=="opacity" || field=="specular" || field=="refraction") && (p.number_value<0 || p.number_value>1)) fail("legacy unit factor is outside [0,1]");
   charge(add(sizeof(p),field.size()));result.encodedGrowth=add(result.encodedGrowth,add(128,multiply(field.size(),6)));
  }
 }
 if(result.present && (add(result.bytes,128*1024)>limits.maxOwnedBytes || result.encodedGrowth>limits.maxJsonBytes))
  fail("legacy parameter output exceeds its finite byte reservation");
 return result;
}
struct CameraPrepared {bool present{};uint64_t bytes{};};
CameraPrepared cameraPreflight(std::span<const tinygltf::Camera> cameras,const GltfLegacyMaterialSerializationLimits& limits) {
 if(cameras.size()>limits.maxCameras) fail("camera count exceeds its serialization capacity");
 CameraPrepared result;
 for(const auto& camera:cameras) if(camera.type=="perspective") {
  if(!std::isfinite(camera.perspective.zfar) || camera.perspective.zfar<0)
   fail("perspective far must be positive or the typed omitted sentinel");
  result.present|=camera.perspective.zfar==0;
 }
 if(result.present) {
  // The camera-only path must reserve the same bounded SAX/output scratch as
  // legacy projection before writeExportModel makes its first model copy.
  if(limits.maxOwnedBytes<128*1024) fail("camera compatibility scratch exceeds its finite byte reservation");
  GltfCameraAuthoringLimits cameraLimits;cameraLimits.maxMetadataValues=limits.maxJsonValues;cameraLimits.maxMetadataDepth=limits.maxJsonDepth;
  for(const auto& camera:cameras) {
   const auto reserved=add(result.bytes,128*1024);if(reserved>=limits.maxOwnedBytes) fail("camera compatibility records exceed their byte reservation");
   cameraLimits.maxOwnedBytes=limits.maxOwnedBytes-reserved;
   try {result.bytes=add(result.bytes,GltfCameraRecordStorageBytes(camera,cameraLimits));}
   catch(const std::invalid_argument& error) {fail(error.what());}
  }
 }
 return result;
}
uint32_t read32(std::istream& stream) {
 std::array<unsigned char,4> data{};if(!stream.read(reinterpret_cast<char*>(data.data()),4)) fail("truncated staged GLB header");
 return uint32_t(data[0])|(uint32_t(data[1])<<8)|(uint32_t(data[2])<<16)|(uint32_t(data[3])<<24);
}
void write32(std::ostream& stream,uint32_t value) {for(unsigned shift=0;shift<32;shift+=8) stream.put(char(value>>shift));}
class Output {
 public:
 Output(std::ostream& stream,uint64_t maximum):stream_(stream),maximum_(maximum) {}
 uint64_t size() const noexcept {return size_;}
 void text(std::string_view value) {if(value.size()>maximum_-size_) fail("rewritten material JSON exceeds its capacity");size_+=value.size();stream_.write(value.data(),std::streamsize(value.size()));if(!stream_) fail("cannot write staged material JSON");}
 void character(char value) {text(std::string_view(&value,1));}
 void string(std::string_view value) {
  text("\"");constexpr char hex[]="0123456789abcdef";
  for(const auto raw:value) {const auto c=static_cast<unsigned char>(raw);
   if(c=='"') text("\\\"");else if(c=='\\') text("\\\\");
   else if(c<32) {const std::array<char,6> escape{'\\','u','0','0',hex[c>>4],hex[c&15]};text({escape.data(),escape.size()});}
   else character(char(c));
  }text("\"");
 }
 template<class Number> void integer(Number value) {std::array<char,32> buffer{};const auto result=std::to_chars(buffer.data(),buffer.data()+buffer.size(),value);if(result.ec!=std::errc{}) fail("cannot encode JSON integer");text({buffer.data(),size_t(result.ptr-buffer.data())});}
 void number(double value) {
  if(!std::isfinite(value)) fail("cannot emit nonfinite legacy value");
  if(value==0 && std::signbit(value)) {text("-0.0");return;}
  std::array<char,64> buffer{};const auto result=std::to_chars(buffer.data(),buffer.data()+buffer.size(),value,std::chars_format::general,std::numeric_limits<double>::max_digits10);
  if(result.ec!=std::errc{}) fail("cannot encode legacy numeric value");text({buffer.data(),size_t(result.ptr-buffer.data())});
 }
 private:std::ostream& stream_;uint64_t maximum_{},size_{};
};
class LegacySax final : public nlohmann::json_sax<nlohmann::json> {
 using Json=nlohmann::json;
 enum class Pending {None,Materials,Pbr,Cameras,Perspective,OmitFar};
 struct Context {bool object{},first{true},root{},materials{},material{},pbr{},hasPbr{},cameras{},camera{},perspective{};
  Pending pending{};size_t index{},materialIndex{},cameraIndex{};};
 std::array<Context,256> stack_{};size_t depth_{};uint64_t values_{};bool sawMaterials_{},sawCameras_{};
 std::span<const tinygltf::Material> materials_;std::span<const tinygltf::Camera> cameras_;
 bool materialRequired_{},cameraRequired_{};Output& output_;const GltfLegacyMaterialSerializationLimits& limits_;
 void visit() {if(++values_>limits_.maxJsonValues) fail("serialized JSON traversal exceeds its capacity");}
 void prefix(bool object=false,bool array=false) {
  visit();if(!depth_) return;auto& parent=stack_[depth_-1];
  if((parent.materials || parent.cameras) && !object) fail("staged source table contains a non-object record");
  if(parent.object) {
   if(((parent.pending==Pending::Pbr || parent.pending==Pending::Perspective) && !object) ||
       ((parent.pending==Pending::Materials || parent.pending==Pending::Cameras) && !array) || parent.pending==Pending::OmitFar)
    fail("staged serializer produced a malformed compatibility field");
   parent.pending=Pending::None;
  } else {if(!parent.first) output_.character(',');parent.first=false;++parent.index;}
 }
 void property(Context& context,std::string_view name) {
  if(!context.first) output_.character(',');context.first=false;output_.string(name);output_.character(':');
 }
 bool hasKnown(const tinygltf::ParameterMap& map) const {
  for(const auto name:textureFields) if(map.contains(std::string(name))) return true;
  for(const auto name:factorFields) if(map.contains(std::string(name))) return true;
  return false;
 }
 void parameters(Context& context,const tinygltf::ParameterMap& map,size_t objectDepth) {
  for(const auto field:textureFields) if(const auto found=map.find(std::string(field));found!=map.end()) {
   if(objectDepth>=limits_.maxJsonDepth) fail("injected legacy texture nesting exceeds its capacity");
   property(context,field);output_.character('{');bool first=true;
   for(const auto& [name,value]:found->second.json_double_value) {
    if(!first) output_.character(',');first=false;output_.string(name);output_.character(':');
    if(name=="index" || name=="texCoord" || name=="channel") output_.integer(int64_t(value));else output_.number(value);
   }output_.character('}');
  }
  for(const auto field:factorFields) if(const auto found=map.find(std::string(field));found!=map.end()) {property(context,field);output_.number(found->second.number_value);}
 }
 bool scalar(std::string_view token) {prefix();output_.text(token);return true;}
 bool omitFar(double value) {
  if(!depth_ || stack_[depth_-1].pending!=Pending::OmitFar) return false;
  if(value!=0) fail("staged perspective far disagrees with its typed omission sentinel");
  visit();stack_[depth_-1].pending=Pending::None;return true;
 }
 public:
 LegacySax(const tinygltf::Model& model,bool materialRequired,bool cameraRequired,Output& output,const GltfLegacyMaterialSerializationLimits& limits):
  materials_(model.materials),cameras_(model.cameras),materialRequired_(materialRequired),cameraRequired_(cameraRequired),output_(output),limits_(limits) {}
 bool null() override {return scalar("null");}
 bool boolean(bool value) override {return scalar(value?"true":"false");}
 bool number_integer(number_integer_t value) override {if(omitFar(double(value))) return true;prefix();output_.integer(value);return true;}
 bool number_unsigned(number_unsigned_t value) override {if(omitFar(double(value))) return true;prefix();output_.integer(value);return true;}
 bool number_float(number_float_t value,const string_t& token) override {if(omitFar(value)) return true;prefix();output_.text(token);return true;}
 bool string(string_t& value) override {prefix();output_.string(value);return true;}
 bool binary(binary_t&) override {fail("binary JSON token is not allowed");}
 bool start_object(std::size_t) override {
  if(depth_>=limits_.maxJsonDepth) fail("serialized JSON nesting exceeds its capacity");
  Context context;context.object=true;context.root=depth_==0;
  if(depth_) {const auto& parent=stack_[depth_-1];
   context.material=parent.materials;context.pbr=parent.material && parent.pending==Pending::Pbr;
   context.materialIndex=parent.materials?parent.index:parent.materialIndex;
   context.camera=parent.cameras;context.perspective=parent.camera && parent.pending==Pending::Perspective;
   context.cameraIndex=parent.cameras?parent.index:parent.cameraIndex;
   if(context.material && context.materialIndex>=materials_.size()) fail("staged material table grew unexpectedly");
   if(context.camera && context.cameraIndex>=cameras_.size()) fail("staged camera table grew unexpectedly");
  }
  prefix(true);output_.character('{');stack_[depth_++]=context;return true;
 }
 bool key(string_t& value) override {
  visit();if(!depth_ || !stack_[depth_-1].object) fail("invalid JSON object key");auto& context=stack_[depth_-1];
  if((context.material && materials_[context.materialIndex].additionalValues.contains(value) && known(value)) ||
      (context.pbr && materials_[context.materialIndex].values.contains(value) && known(value))) fail("TinyGLTF unexpectedly serialized a legacy compatibility field already");
  if(context.perspective && value=="zfar" && cameras_[context.cameraIndex].type=="perspective" &&
      cameras_[context.cameraIndex].perspective.zfar==0) {context.pending=Pending::OmitFar;return true;}
  property(context,value);
  if(context.root && value=="materials") {if(sawMaterials_) fail("duplicate serialized material table");sawMaterials_=true;context.pending=Pending::Materials;}
  else if(context.root && value=="cameras") {if(sawCameras_) fail("duplicate serialized camera table");sawCameras_=true;context.pending=Pending::Cameras;}
  else if(context.material && value=="pbrMetallicRoughness") {context.hasPbr=true;context.pending=Pending::Pbr;}
  else if(context.camera && value=="perspective") context.pending=Pending::Perspective;
  else context.pending=Pending::None;
  return true;
 }
 bool end_object() override {
  if(!depth_ || !stack_[depth_-1].object) fail("unbalanced serialized JSON object");auto& context=stack_[depth_-1];
  if(context.pbr) parameters(context,materials_[context.materialIndex].values,depth_);
  if(context.material) {
   const auto& material=materials_[context.materialIndex];
   if(!context.hasPbr && hasKnown(material.values)) {
    if(depth_>=limits_.maxJsonDepth) fail("injected PBR material nesting exceeds its capacity");
    property(context,"pbrMetallicRoughness");output_.character('{');Context pbr;pbr.object=true;parameters(pbr,material.values,depth_+1);output_.character('}');
   }
   parameters(context,material.additionalValues,depth_);
  }
  output_.character('}');--depth_;return true;
 }
 bool start_array(std::size_t) override {
  if(depth_>=limits_.maxJsonDepth) fail("serialized JSON nesting exceeds its capacity");Context context;
  if(depth_) {const auto& parent=stack_[depth_-1];context.materials=parent.root && parent.pending==Pending::Materials;
   context.cameras=parent.root && parent.pending==Pending::Cameras;context.materialIndex=parent.materialIndex;context.cameraIndex=parent.cameraIndex;}
  prefix(false,true);output_.character('[');stack_[depth_++]=context;return true;
 }
 bool end_array() override {
  if(!depth_ || stack_[depth_-1].object) fail("unbalanced serialized JSON array");const auto& context=stack_[depth_-1];
  if(context.materials && context.index!=materials_.size()) fail("staged material table changed size");
  if(context.cameras && context.index!=cameras_.size()) fail("staged camera table changed size");
  output_.character(']');--depth_;return true;
 }
 bool parse_error(std::size_t,const std::string&,const nlohmann::detail::exception& error) override {fail(std::string("staged JSON is invalid: ")+error.what());}
 void complete() const {if(depth_ || (materialRequired_ && !sawMaterials_) || (cameraRequired_ && !sawCameras_)) fail("staged serializer omitted a required compatibility table");}
};
} // namespace

std::span<const std::string_view> KnownLegacyMaterialTextureFields() noexcept {return textureFields;}
std::span<const std::string_view> KnownLegacyMaterialFactorFields() noexcept {return factorFields;}
bool ValidateLegacyMaterialSerializationInput(std::span<const tinygltf::Material> materials,size_t textures,const GltfLegacyMaterialSerializationLimits& limits) {
 return preflight(materials,textures,limits).present;
}
bool ValidateSceneSerializationCompatibilityInput(const tinygltf::Model& model,const GltfLegacyMaterialSerializationLimits& limits) {
 const auto material=preflight(model.materials,model.textures.size(),limits);
 return cameraPreflight(model.cameras,limits).present || material.present;
}
std::optional<std::filesystem::path> StageLegacyMaterialSerialization(const tinygltf::Model& model,const std::filesystem::path& stagedFile,
    bool binary,const GltfLegacyMaterialSerializationLimits& limits) {
 const auto prepared=preflight(model.materials,model.textures.size(),limits);const auto camera=cameraPreflight(model.cameras,limits);const bool cameraRewrite=camera.present;
 if(!prepared.present && !cameraRewrite) return {};
 const auto size=std::filesystem::file_size(stagedFile);std::ifstream input(stagedFile,std::ios::binary);if(!input) fail("cannot read staged scene");
 uint64_t jsonBytes=size,jsonStart{},tailStart{};
 if(binary) {
  if(size>UINT32_MAX || size<20 || read32(input)!=0x46546c67 || read32(input)!=2 || read32(input)!=size) fail("staged GLB header is malformed");
  jsonBytes=read32(input);if(read32(input)!=0x4e4f534a || !jsonBytes || jsonBytes%4 || jsonBytes>size-20) fail("staged GLB JSON chunk is malformed");
  jsonStart=20;tailStart=add(jsonStart,jsonBytes);
  uint32_t chunks=1;
  for(uint64_t position=tailStart;position<size;) {
   if(++chunks>limits.maxGlbChunks) fail("staged GLB chunk traversal exceeds its capacity");
   if(size-position<8) fail("truncated staged GLB chunk stream");input.seekg(std::streamoff(position));const auto count=read32(input),type=read32(input);
   if(count%4 || count>size-position-8 || type==0x4e4f534a) fail("staged GLB trailing chunk is malformed");position=add(position,add(8,count));
  }
 }
 if(!jsonBytes || jsonBytes>limits.maxJsonBytes || add(jsonBytes,prepared.encodedGrowth)>limits.maxJsonBytes ||
    add(add(add(multiply(jsonBytes,2),prepared.bytes),camera.bytes),128*1024)>limits.maxOwnedBytes) fail("staged material JSON exceeds its finite byte reservation");
 input.seekg(std::streamoff(jsonStart));std::string json(size_t(jsonBytes),'\0');
 if(!input.read(json.data(),std::streamsize(json.size()))) fail("cannot read complete staged JSON");
 auto replacement=stagedFile;replacement+=".legacy-materials";
 if(std::filesystem::exists(replacement)) fail("legacy material staging output already exists");
 std::ofstream output(replacement,std::ios::binary|std::ios::trunc);if(!output) fail("cannot open legacy material staging output");
 if(binary) for(int i=0;i<5;++i) write32(output,0);
 Output emitted(output,limits.maxJsonBytes);LegacySax sax(model,prepared.present,cameraRewrite,emitted,limits);
 if(!nlohmann::json::sax_parse(json,&sax)) fail("cannot parse staged JSON");sax.complete();
 if(binary) {
  const auto padding=(4-emitted.size()%4)%4;for(uint64_t i=0;i<padding;++i) output.put(' ');
  const auto newJson=add(emitted.size(),padding),newTotal=add(add(20,newJson),size-tailStart);
  if(newJson>UINT32_MAX || newTotal>UINT32_MAX) fail("rewritten GLB exceeds its 32-bit container length");
  input.clear();input.seekg(std::streamoff(tailStart));std::array<char,64*1024> buffer{};uint64_t remaining=size-tailStart;
  while(remaining) {const auto count=std::min<uint64_t>(remaining,buffer.size());if(!input.read(buffer.data(),std::streamsize(count))) fail("cannot copy staged GLB payload");output.write(buffer.data(),std::streamsize(count));if(!output) fail("cannot write staged GLB payload");remaining-=count;}
  output.seekp(0);write32(output,0x46546c67);write32(output,2);write32(output,uint32_t(newTotal));write32(output,uint32_t(newJson));write32(output,0x4e4f534a);
 }
 output.flush();if(!output) fail("cannot finish staged legacy material output");output.close();if(!output) fail("cannot close staged legacy material output");input.close();
 return replacement;
}
} // namespace container::geometry::gltf
