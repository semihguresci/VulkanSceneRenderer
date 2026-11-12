#include <Container/geometry/GltfAccessor.h>
#include <Container/geometry/GltfCompression.h>
#include <Container/geometry/GltfModelLoader.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#if defined(CONTAINER_HAS_MESHOPT)
#include <meshoptimizer.h>
#endif
#if defined(CONTAINER_HAS_DRACO)
#include <draco/compression/encode.h>
#include <draco/mesh/mesh.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>

namespace {
using namespace container::geometry::gltf;
using Json = nlohmann::json;
using Bytes = std::vector<unsigned char>;
constexpr const char* kExt = "EXT_meshopt_compression";
constexpr const char* kKhr = "KHR_meshopt_compression";
constexpr const char* kDraco = "KHR_draco_mesh_compression";

tinygltf::Value value(const Json& item) {
  if (item.is_object()) {
    tinygltf::Value::Object output;
    for (const auto& [key, child] : item.items()) output[key] = value(child);
    return tinygltf::Value(std::move(output));
  }
  if (item.is_array()) {
    tinygltf::Value::Array output;
    for (const auto& child : item) output.push_back(value(child));
    return tinygltf::Value(std::move(output));
  }
  if (item.is_boolean()) return tinygltf::Value(item.get<bool>());
  if (item.is_number_integer()) return tinygltf::Value(item.get<int>());
  if (item.is_number()) return tinygltf::Value(item.get<double>());
  if (item.is_string()) return tinygltf::Value(item.get<std::string>());
  return {};
}
template<class T> Bytes raw(const T& source) {
  const auto* first = reinterpret_cast<const unsigned char*>(&source);
  return {first,first+sizeof(source)};
}
struct Fixture {
  tinygltf::Model model;
  Json document;
};
Fixture meshopt(Bytes encoded,size_t count,size_t stride,std::string mode="ATTRIBUTES",
                std::string filter="NONE",const char* name=kKhr) {
  const Json extension{{"buffer",1},{"byteOffset",0},{"byteLength",encoded.size()},
      {"count",count},{"byteStride",stride},{"mode",mode},{"filter",filter}};
  Fixture result;
  result.document = {{"asset",{{"version","2.0"}}},{"extensionsUsed",{name}},
      {"extensionsRequired",{name}},{"buffers",Json::array({
          {{"byteLength",count*stride},{"uri","fallback.bin"},{"extensions",{{name,{{"fallback",true}}}}}},
          {{"byteLength",encoded.size()},{"uri","encoded.bin"}}})},
      {"bufferViews",Json::array({{{"buffer",0},{"byteOffset",0},{"byteLength",count*stride},
          {"extensions",{{name,extension}}}}})}};
  result.model.asset.version="2.0";
  result.model.extensionsUsed={name}; result.model.extensionsRequired={name};
  tinygltf::Buffer fallback; fallback.uri="fallback.bin"; fallback.data.assign(count*stride,0xcd);
  fallback.extensions[name]=value(Json{{"fallback",true}});
  tinygltf::Buffer compressed; compressed.uri="encoded.bin"; compressed.data=std::move(encoded);
  result.model.buffers={std::move(fallback),std::move(compressed)};
  tinygltf::BufferView view; view.buffer=0; view.byteLength=count*stride; view.extensions[name]=value(extension);
  result.model.bufferViews.push_back(std::move(view));
  return result;
}
void accessor(Fixture& fixture,int component,int type,size_t count,bool normalized=false) {
  tinygltf::Accessor output; output.bufferView=0; output.componentType=component; output.type=type;
  output.count=count; output.normalized=normalized; output.name="preserve accessor metadata";
  output.extras=value(Json{{"original",true}});
  fixture.model.accessors.push_back(std::move(output));
}
class Directory {
public:
  Directory() {
    static std::atomic<uint64_t> sequence{};
    path=std::filesystem::temp_directory_path()/ ("container-compression-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(sequence++));
    std::filesystem::create_directory(path);
  }
  ~Directory() {
    std::error_code ignored;
    for (const auto& file:std::filesystem::directory_iterator(path))
      if (file.is_regular_file()) std::filesystem::remove(file.path(),ignored);
    std::filesystem::remove(path,ignored);
  }
  std::filesystem::path path;
};

TEST(GltfCompression, DefaultsToFiniteDecodedBudgetAndDiagnosesUnavailableRequiredBackends) {
  EXPECT_EQ(CompressionLimits{}.maxDecodedBytes,256ull*1024*1024);
  EXPECT_TRUE(IsGeometryCompressionExtension(kKhr));
  EXPECT_TRUE(IsGeometryCompressionExtension(kExt));
  EXPECT_TRUE(IsGeometryCompressionExtension(kDraco));
  EXPECT_FALSE(IsGeometryCompressionExtension("KHR_mesh_quantization"));
  for (const auto* name:{kExt,kKhr,kDraco}) {
    const Json document{{"extensionsUsed",{name}},{"extensionsRequired",{name}}};
    const bool available=name==kDraco?HasAvailableDracoDecoder():HasMeshoptDecoder();
    if (available) EXPECT_NO_THROW((void)InspectCompressedGltfSource(document,false));
    else EXPECT_THROW((void)InspectCompressedGltfSource(document,false),std::runtime_error);
  }
}

TEST(GltfCompression, OptionalTaggedFallbackUsesOrdinaryBytesOnlyWithDecodersDisabled) {
  for (const auto* name:{kExt,kKhr}) {
    auto fixture=meshopt({0x01},3,12,"ATTRIBUTES","NONE",name);
    fixture.document["extensionsRequired"]=Json::array(); fixture.model.extensionsRequired.clear();
    EXPECT_NO_THROW((void)InspectCompressedGltfSource(fixture.document,false));
    if (HasMeshoptDecoder()) {
      // Available decoders must select the extension and diagnose its bad
      // stream instead of silently accepting fallback that could differ.
      EXPECT_THROW((void)DecodeCompressedGltf(fixture.model),std::runtime_error);
      continue;
    }
    CompressionStatistics stats;
    const auto original=fixture.model.buffers[0].data;
    const auto decoded=DecodeCompressedGltf(fixture.model,{},&stats);
    EXPECT_EQ(decoded.buffers[0].data,original);
    EXPECT_EQ(stats.fallbackViews,1u); EXPECT_EQ(stats.meshoptViews,0u);
    EXPECT_TRUE(decoded.extensionsUsed.empty()); EXPECT_TRUE(decoded.bufferViews[0].extensions.empty());
    fixture.document["buffers"][0].erase("uri");
    EXPECT_THROW((void)InspectCompressedGltfSource(fixture.document,false),std::runtime_error);
  }
}

TEST(GltfCompression, RejectsAmbiguousFallbackReferencesAndEncodedDestinationAliases) {
  auto fixture=meshopt({0x01},3,12);
  fixture.document["buffers"][0].erase("uri");
  if (!HasMeshoptDecoder()) {
    EXPECT_THROW((void)InspectCompressedGltfSource(fixture.document,false),std::runtime_error);
    return;
  }
  const auto plan=InspectCompressedGltfSource(fixture.document,false);
  ASSERT_EQ(plan.placeholders.size(),1u); EXPECT_EQ(plan.placeholders[0].byteLength,36u);
  auto invalid=fixture.document;
  invalid["bufferViews"].push_back({{"buffer",0},{"byteLength",4}});
  EXPECT_THROW((void)InspectCompressedGltfSource(invalid,false),std::runtime_error);
  invalid=fixture.document;
  invalid["bufferViews"][0]["extensions"][kKhr]["buffer"]=0;
  EXPECT_THROW((void)InspectCompressedGltfSource(invalid,false),std::runtime_error);
  invalid=fixture.document;
  invalid["extensionsUsed"].push_back(kExt); invalid["extensionsRequired"]={kExt};
  EXPECT_THROW((void)InspectCompressedGltfSource(invalid,false),std::runtime_error);
  invalid=fixture.document;
  invalid["bufferViews"][0]["extensions"][kExt]=invalid["bufferViews"][0]["extensions"][kKhr];
  EXPECT_THROW((void)InspectCompressedGltfSource(invalid,false),std::runtime_error);
}

#if defined(CONTAINER_HAS_MESHOPT)
Bytes vertices(const Bytes& input,size_t count,size_t stride,int version=1) {
  Bytes encoded(meshopt_encodeVertexBufferBound(count,stride));
  encoded.resize(meshopt_encodeVertexBufferLevel(encoded.data(),encoded.size(),input.data(),count,stride,2,version));
  if (encoded.empty()) throw std::runtime_error("test encoder failed");
  return encoded;
}
TEST(GltfCompression, MeshoptAttributeVersionsPreserveOriginalStorageAndAccessorSemantics) {
  const std::array<float,9> positions{0,0,0,2,0,0,0,3,0};
  for (const auto* name:{kExt,kKhr}) for(int version=0;version<=1;++version) {
    SCOPED_TRACE(std::string(name)+std::to_string(version));
    auto fixture=meshopt(vertices(raw(positions),3,12,version),3,12,"ATTRIBUTES","NONE",name);
    accessor(fixture,5126,TINYGLTF_TYPE_VEC3,3);
    const auto encoded=fixture.model.buffers[1].data;
    if(name==kExt && version==1) {
      EXPECT_THROW((void)DecodeCompressedGltf(fixture.model),std::runtime_error); continue;
    }
    CompressionStatistics stats;
    const auto decoded=DecodeCompressedGltf(fixture.model,{},&stats);
    EXPECT_EQ(decoded.buffers[1].data,encoded); EXPECT_EQ(fixture.model.buffers.size(),2u);
    EXPECT_EQ(decoded.buffers.back().data,raw(positions)); EXPECT_EQ(stats.decodedBytes,36u);
    EXPECT_EQ(decoded.accessors[0].componentType,5126); EXPECT_EQ(decoded.accessors[0].count,3u);
    EXPECT_EQ(decoded.accessors[0].name,"preserve accessor metadata"); EXPECT_TRUE(decoded.accessors[0].extras.Has("original"));
    const CheckedAccessor checked(decoded,0,"decoded position",{AccessorUsage::VertexAttribute});
    std::array<float,3> result{}; checked.readFloats(2,result); EXPECT_EQ(result,(std::array<float,3>{0,3,0}));
    EXPECT_TRUE(decoded.bufferViews[0].extensions.empty()); EXPECT_TRUE(decoded.extensionsRequired.empty());
  }
}

TEST(GltfCompression, UntaggedRealDestinationCanHoldSeparateExtAndKhrViews) {
  const std::array<float,9> positions{0,0,0,2,0,0,0,3,0};
  auto fixture=meshopt(vertices(raw(positions),3,12,0),3,12,"ATTRIBUTES","NONE",kExt);
  fixture.model.buffers[0].extensions.clear(); fixture.model.buffers[0].data.resize(72);
  fixture.document["buffers"][0].erase("extensions"); fixture.document["buffers"][0]["byteLength"]=72;
  auto second=fixture.model.bufferViews[0]; second.byteOffset=36;
  const auto definition=second.extensions.at(kExt); second.extensions.clear(); second.extensions[kKhr]=definition;
  fixture.model.bufferViews.push_back(second);
  auto secondJson=fixture.document["bufferViews"][0]; secondJson["byteOffset"]=36;
  const auto jsonDefinition=secondJson["extensions"][kExt]; secondJson["extensions"]={{kKhr,jsonDefinition}};
  fixture.document["bufferViews"].push_back(secondJson);
  fixture.model.extensionsUsed.push_back(kKhr); fixture.model.extensionsRequired.push_back(kKhr);
  fixture.document["extensionsUsed"].push_back(kKhr); fixture.document["extensionsRequired"].push_back(kKhr);
  EXPECT_NO_THROW((void)InspectCompressedGltfSource(fixture.document,false));
  const auto decoded=DecodeCompressedGltf(fixture.model);
  EXPECT_EQ(decoded.buffers[size_t(decoded.bufferViews[0].buffer)].data,raw(positions));
  EXPECT_EQ(decoded.buffers[size_t(decoded.bufferViews[1].buffer)].data,raw(positions));
  fixture.document["buffers"][0]["extensions"]={{kExt,{{"fallback",true}}}};
  EXPECT_THROW((void)InspectCompressedGltfSource(fixture.document,false),std::runtime_error);
}

TEST(GltfCompression, MeshoptAllIndexModesWidthsAndBitstreamVersions) {
  const std::array<uint32_t,6> indices{0,1,2,2,1,3};
  for(const std::string mode:{"TRIANGLES","INDICES"}) for(size_t stride:{2u,4u}) for(int version:{0,1}) {
    SCOPED_TRACE(mode+std::to_string(stride)+std::to_string(version));
    meshopt_encodeIndexVersion(version);
    Bytes encoded(mode=="TRIANGLES"?meshopt_encodeIndexBufferBound(indices.size(),4):meshopt_encodeIndexSequenceBound(indices.size(),4));
    const auto size=mode=="TRIANGLES"?meshopt_encodeIndexBuffer(encoded.data(),encoded.size(),indices.data(),indices.size()):
        meshopt_encodeIndexSequence(encoded.data(),encoded.size(),indices.data(),indices.size());
    encoded.resize(size);
    auto fixture=meshopt(std::move(encoded),indices.size(),stride,mode);
    accessor(fixture,stride==2?5123:5125,TINYGLTF_TYPE_SCALAR,indices.size());
    const auto decoded=DecodeCompressedGltf(fixture.model);
    const CheckedAccessor checked(decoded,0,"indices",{AccessorUsage::Indices});
    for(size_t i=0;i<indices.size();++i) {
      std::array<uint32_t,1> result{}; checked.readUnsigned(i,result); EXPECT_EQ(result[0],indices[i]);
    }
  }
  meshopt_encodeIndexVersion(1);
}

TEST(GltfCompression, MeshoptFiltersCoverEverySupportedStrideAndColorUsesKhrOnly) {
  const std::array<float,8> data{0,0,1,1,1,0,0,1};
  for(const std::string filter:{"OCTAHEDRAL","QUATERNION","EXPONENTIAL","COLOR"}) {
    const std::vector<size_t> strides=filter=="QUATERNION"?std::vector<size_t>{8}:
        filter=="EXPONENTIAL"?std::vector<size_t>{4,8,12,16}:std::vector<size_t>{4,8};
    for(const auto stride:strides) {
      SCOPED_TRACE(filter+std::to_string(stride));
      const size_t count=filter=="EXPONENTIAL"?1:2;
      Bytes filtered(count*stride);
      if(filter=="OCTAHEDRAL") meshopt_encodeFilterOct(filtered.data(),count,stride,stride==4?8:16,data.data());
      else if(filter=="QUATERNION") meshopt_encodeFilterQuat(filtered.data(),count,stride,16,data.data());
      else if(filter=="EXPONENTIAL") meshopt_encodeFilterExp(filtered.data(),count,stride,24,data.data(),meshopt_EncodeExpSharedVector);
      else meshopt_encodeFilterColor(filtered.data(),count,stride,stride==4?8:16,data.data());
      auto expected=filtered;
      if(filter=="OCTAHEDRAL") meshopt_decodeFilterOct(expected.data(),count,stride);
      else if(filter=="QUATERNION") meshopt_decodeFilterQuat(expected.data(),count,stride);
      else if(filter=="EXPONENTIAL") meshopt_decodeFilterExp(expected.data(),count,stride);
      else meshopt_decodeFilterColor(expected.data(),count,stride);
      auto fixture=meshopt(vertices(filtered,count,stride),count,stride,"ATTRIBUTES",filter);
      EXPECT_EQ(DecodeCompressedGltf(fixture.model).buffers.back().data,expected);
      if(filter=="COLOR") {
        fixture.document["bufferViews"][0]["extensions"][kExt]=fixture.document["bufferViews"][0]["extensions"][kKhr];
        fixture.document["bufferViews"][0]["extensions"].erase(kKhr);
        fixture.document["extensionsUsed"]={kExt}; fixture.document["extensionsRequired"]={kExt};
        EXPECT_THROW((void)InspectCompressedGltfSource(fixture.document,false),std::runtime_error);
      }
    }
  }
}

TEST(GltfCompression, MeshoptRejectsRangesSchemaBudgetsAndMalformedPayloadTransactionally) {
  const std::array<float,9> xyz{0,0,0,1,0,0,0,1,0};
  auto fixture=meshopt(vertices(raw(xyz),3,12),3,12);
  const auto original=fixture.model.buffers[0].data;
  CompressionStatistics stats; stats.decodedBytes=987;
  auto limits=CompressionLimits{}; limits.maxDecodedBytes=35;
  EXPECT_THROW((void)DecodeCompressedGltf(fixture.model,limits,&stats),std::runtime_error);
  EXPECT_EQ(stats.decodedBytes,987u); EXPECT_EQ(fixture.model.buffers[0].data,original);
  for(const auto& replacement:std::vector<Json>{
      {{"byteStride",3}},{{"count",4}},{{"byteLength",std::numeric_limits<uint64_t>::max()}},
      {{"buffer",2}},{{"byteOffset",-1}},{{"mode","UNKNOWN"}},{{"filter","UNKNOWN"}}}) {
    auto invalid=fixture.document;
    invalid["bufferViews"][0]["extensions"][kKhr].update(replacement);
    EXPECT_THROW((void)InspectCompressedGltfSource(invalid,false),std::runtime_error)<<replacement.dump();
  }
  fixture.model.buffers[1].data.assign(fixture.model.buffers[1].data.size(),0xff);
  EXPECT_THROW((void)DecodeCompressedGltf(fixture.model,{},&stats),std::runtime_error);
  EXPECT_EQ(stats.decodedBytes,987u); EXPECT_EQ(fixture.model.buffers[0].data,original);
}

TEST(GltfCompression, MeshoptPlaceholderStandInAndSparseOverrideRemainIndependent) {
  const std::array<float,9> xyz{0,0,0,1,0,0,0,1,0};
  auto fixture=meshopt(vertices(raw(xyz),3,12),3,12);
  fixture.document["buffers"][0].erase("uri");
  const auto plan=InspectCompressedGltfSource(fixture.document,false);
  fixture.model.buffers[0].data={0}; fixture.model.buffers[0].uri="private-synthetic-uri";
  accessor(fixture,5126,TINYGLTF_TYPE_VEC3,3);
  tinygltf::Buffer overrides; overrides.data={1,0,0,0};
  const auto replacement=raw(std::array<float,3>{7,8,9}); overrides.data.insert(overrides.data.end(),replacement.begin(),replacement.end());
  fixture.model.buffers.push_back(std::move(overrides));
  tinygltf::BufferView indices; indices.buffer=2; indices.byteLength=1;
  tinygltf::BufferView values; values.buffer=2; values.byteOffset=4; values.byteLength=12;
  fixture.model.bufferViews.push_back(indices); fixture.model.bufferViews.push_back(values);
  auto& sparse=fixture.model.accessors[0].sparse; sparse.isSparse=true; sparse.count=1;
  sparse.indices.bufferView=1; sparse.indices.byteOffset=0; sparse.indices.componentType=5121;
  sparse.values.bufferView=2; sparse.values.byteOffset=0;
  const auto decoded=DecodeCompressedGltf(fixture.model,{},nullptr,plan.placeholders);
  EXPECT_TRUE(decoded.buffers[0].data.empty()); EXPECT_TRUE(decoded.buffers[0].uri.empty());
  EXPECT_EQ(fixture.model.buffers[0].data,Bytes{0}); EXPECT_EQ(decoded.buffers[2].data,fixture.model.buffers[2].data);
  const CheckedAccessor checked(decoded,0,"sparse decoded position",{AccessorUsage::VertexAttribute});
  std::array<float,3> output{}; checked.readFloats(1,output); EXPECT_EQ(output,(std::array<float,3>{7,8,9}));
  checked.readFloats(2,output); EXPECT_EQ(output,(std::array<float,3>{0,1,0}));
}
#endif

#if defined(CONTAINER_HAS_DRACO) && defined(_WIN32)
#ifndef CONTAINER_DRACO_WORKER_EXECUTABLE
#define CONTAINER_DRACO_WORKER_EXECUTABLE "ContainerDracoWorker.exe"
#endif
Fixture dracoTriangle() {
  draco::Mesh mesh; mesh.set_num_points(3); mesh.SetNumFaces(1);
  mesh.SetFace(draco::FaceIndex(0),{draco::PointIndex(0),draco::PointIndex(1),draco::PointIndex(2)});
  const auto attribute=[&](draco::GeometryAttribute::Type semantic,int components,draco::DataType type,bool normalized,int id,const auto& data) {
    draco::GeometryAttribute definition; definition.Init(semantic,nullptr,components,type,normalized,sizeof(data[0]),0);
    const auto index=mesh.AddAttribute(definition,true,3); mesh.attribute(index)->set_unique_id(id);
    for(uint32_t i=0;i<3;++i) mesh.attribute(index)->SetAttributeValue(draco::AttributeValueIndex(i),data[i].data());
  };
  attribute(draco::GeometryAttribute::POSITION,3,draco::DT_FLOAT32,false,7,
      std::array<std::array<float,3>,3>{{{0,0,0},{2,0,0},{0,3,0}}});
  attribute(draco::GeometryAttribute::COLOR,3,draco::DT_UINT8,true,19,
      std::array<std::array<uint8_t,3>,3>{{{255,0,0},{0,255,0},{0,0,255}}});
  attribute(draco::GeometryAttribute::TEX_COORD,2,draco::DT_UINT8,true,23,
      std::array<std::array<uint8_t,2>,3>{{{0,0},{255,0},{0,255}}});
  draco::Encoder encoder; encoder.SetSpeedOptions(10,10);
  encoder.SetAttributeQuantization(draco::GeometryAttribute::POSITION,14);
  draco::EncoderBuffer encoded; const auto status=encoder.EncodeMeshToBuffer(mesh,&encoded);
  if(!status.ok()) throw std::runtime_error(status.error_msg_string());
  Fixture fixture; fixture.model.asset.version="2.0";
  tinygltf::Buffer buffer; buffer.data.assign(encoded.data(),encoded.data()+encoded.size()); fixture.model.buffers.push_back(buffer);
  tinygltf::BufferView view; view.buffer=0; view.byteLength=buffer.data.size(); fixture.model.bufferViews.push_back(view);
  const auto add=[&](int type,int component,bool normalized) {
    tinygltf::Accessor accessor; accessor.type=type; accessor.componentType=component; accessor.normalized=normalized; accessor.count=3;
    accessor.extras=value(Json{{"preserve",true}}); fixture.model.accessors.push_back(accessor);
  };
  add(TINYGLTF_TYPE_VEC3,5126,false); add(TINYGLTF_TYPE_VEC3,5121,true); add(TINYGLTF_TYPE_VEC2,5121,true); add(TINYGLTF_TYPE_SCALAR,5123,false);
  tinygltf::Primitive primitive; primitive.attributes={{"POSITION",0},{"COLOR_0",1},{"TEXCOORD_0",2}}; primitive.indices=3; primitive.mode=4;
  primitive.extensions[kDraco]=value(Json{{"bufferView",0},{"attributes",{{"POSITION",7},{"COLOR_0",19},{"TEXCOORD_0",23}}}});
  tinygltf::Mesh output; output.primitives.push_back(primitive); fixture.model.meshes.push_back(output);
  tinygltf::Node node; node.mesh=0; fixture.model.nodes.push_back(node);
  tinygltf::Scene scene; scene.nodes={0}; fixture.model.scenes.push_back(scene); fixture.model.defaultScene=0;
  fixture.model.extensionsUsed={kDraco}; fixture.model.extensionsRequired={kDraco};
  return fixture;
}
CompressionLimits dracoLimits() {
  CompressionLimits limits; limits.dracoWorkerExecutable=std::filesystem::absolute(CONTAINER_DRACO_WORKER_EXECUTABLE); return limits;
}
TEST(GltfCompression, DracoPadsNarrowAttributesAndPortableReloadPreservesNormalizedValues) {
  const auto fixture=dracoTriangle();
  ASSERT_TRUE(std::filesystem::is_regular_file(dracoLimits().dracoWorkerExecutable));
  const auto decoded=DecodeCompressedGltf(fixture.model,dracoLimits());
  EXPECT_EQ(decoded.buffers[0].data,fixture.model.buffers[0].data); EXPECT_EQ(fixture.model.accessors.size(),4u);
  const auto& primitive=decoded.meshes[0].primitives[0];
  const CheckedAccessor positions(decoded,primitive.attributes.at("POSITION"),"position",{AccessorUsage::VertexAttribute});
  const CheckedAccessor colors(decoded,primitive.attributes.at("COLOR_0"),"color",{AccessorUsage::VertexAttribute});
  const CheckedAccessor uvs(decoded,primitive.attributes.at("TEXCOORD_0"),"uv",{AccessorUsage::VertexAttribute});
  EXPECT_EQ(decoded.bufferViews[colors.accessor().bufferView].byteStride,4u);
  EXPECT_EQ(decoded.bufferViews[uvs.accessor().bufferView].byteStride,4u);
  EXPECT_TRUE(colors.accessor().normalized); EXPECT_EQ(colors.accessor().componentType,5121);
  EXPECT_TRUE(colors.accessor().extras.Has("preserve")); EXPECT_TRUE(primitive.extensions.empty());
  for(size_t i=0;i<3;++i) {
    std::array<float,3> p{},color{}; std::array<float,2> uv{};
    positions.readFloats(i,p); colors.readFloats(i,color); uvs.readFloats(i,uv);
    const size_t source=p[0]==2?1:p[1]==3?2:0;
    EXPECT_EQ(color[source],1); EXPECT_EQ(uv[0],source==1?1:0); EXPECT_EQ(uv[1],source==2?1:0);
  }
  Directory directory;
  for(bool binary:{false,true}) {
    const auto path=directory.path/(binary?"portable.glb":"portable.gltf");
    tinygltf::TinyGLTF writer; ASSERT_TRUE(writer.WriteGltfSceneToFile(&decoded,path.string(),true,true,true,binary));
    const auto reloaded=LoadModelWithSource(path.string());
    ASSERT_EQ(reloaded.model.meshes().size(),1u); EXPECT_EQ(reloaded.model.meshes()[0].vertices().size(),3u);
  }
}
TEST(GltfCompression, DracoRejectsCountShapeStripAndWorkerFailureWithoutChangingSource) {
  auto fixture=dracoTriangle(); const auto encoded=fixture.model.buffers[0].data;
  CompressionStatistics stats; stats.dracoPrimitives=19;
  fixture.model.accessors[0].count=4;
  EXPECT_THROW((void)DecodeCompressedGltf(fixture.model,dracoLimits(),&stats),std::runtime_error);
  fixture.model.accessors[0].count=3; fixture.model.meshes[0].primitives[0].mode=5;
  EXPECT_THROW((void)DecodeCompressedGltf(fixture.model,dracoLimits(),&stats),std::runtime_error);
  fixture.model.meshes[0].primitives[0].mode=4;
  auto limits=dracoLimits(); limits.dracoWorkerExecutable=std::filesystem::absolute("missing-worker.exe");
  EXPECT_THROW((void)DecodeCompressedGltf(fixture.model,limits,&stats),std::runtime_error);
  EXPECT_EQ(fixture.model.buffers[0].data,encoded); EXPECT_EQ(fixture.model.accessors.size(),4u); EXPECT_EQ(stats.dracoPrimitives,19u);
}

TEST(GltfCompression, DracoAllowsSemanticAliasesButRejectsIncompatibleShapesBeforeWorkerLaunch) {
  auto fixture=dracoTriangle();
  auto& primitive=fixture.model.meshes[0].primitives[0];
  primitive.attributes["_POSITION_COPY"]=0;
  auto definition=primitive.extensions.at(kDraco).Get<tinygltf::Value::Object>();
  auto attributes=definition["attributes"].Get<tinygltf::Value::Object>();
  attributes["_POSITION_COPY"]=tinygltf::Value(7); definition["attributes"]=tinygltf::Value(attributes);
  primitive.extensions[kDraco]=tinygltf::Value(definition);
  const auto decoded=DecodeCompressedGltf(fixture.model,dracoLimits());
  const auto& output=decoded.meshes[0].primitives[0];
  const CheckedAccessor position(decoded,output.attributes.at("POSITION"),"position");
  const CheckedAccessor alias(decoded,output.attributes.at("_POSITION_COPY"),"alias");
  for(size_t point=0;point<3;++point) {
    std::array<float,3> a{},b{}; position.readFloats(point,a); alias.readFloats(point,b); EXPECT_EQ(a,b);
  }
  primitive.attributes["_POSITION_COPY"]=2; // VEC2 cannot alias the same decoded VEC3.
  auto unavailable=dracoLimits(); unavailable.dracoWorkerExecutable=std::filesystem::absolute("must-not-launch.exe");
  try { (void)DecodeCompressedGltf(fixture.model,unavailable); FAIL()<<"expected shape validation failure"; }
  catch(const std::runtime_error& error) { EXPECT_NE(std::string(error.what()).find("incompatible component counts"),std::string::npos); }
}

TEST(GltfCompression, DracoNormalizationKeepsThePreflightFallbackDecision) {
  const auto fixture = dracoTriangle();
  const auto limits = dracoLimits();
  ASSERT_TRUE(HasAvailableDracoDecoder(limits.dracoWorkerExecutable));
  auto source = DecodeCompressedGltf(fixture.model, limits);
  source.extensionsUsed = {kDraco};
  source.meshes[0].primitives[0].extensions[kDraco] =
      fixture.model.meshes[0].primitives[0].extensions.at(kDraco);
  source.buffers[0].data.front() = 0;

  CompressionStatistics statistics;
  const auto fallback = DecodeCompressedGltf(source, limits, &statistics, {}, {}, false);
  EXPECT_EQ(statistics.fallbackPrimitives, 1u);
  EXPECT_EQ(statistics.dracoPrimitives, 0u);
  EXPECT_EQ(statistics.decodedBytes, 0u);
  EXPECT_EQ(fallback.buffers.size(), source.buffers.size());
  EXPECT_EQ(fallback.accessors[0].bufferView, source.accessors[0].bufferView);
  EXPECT_TRUE(fallback.meshes[0].primitives[0].extensions.empty());
  EXPECT_THROW((void)DecodeCompressedGltf(source, limits), std::runtime_error);
}
#endif
} // namespace
