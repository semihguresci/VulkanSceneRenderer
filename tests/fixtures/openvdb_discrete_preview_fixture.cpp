#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/File.h>
#include <openvdb/openvdb.h>
#endif

namespace {
void generate(const std::filesystem::path& output) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  if(output.empty() || std::filesystem::exists(output) ||
      !std::filesystem::is_directory(output.has_parent_path()?output.parent_path():std::filesystem::path(".")))
    throw std::invalid_argument("fixture output must be new in an existing directory");
  openvdb::initialize();
  constexpr int64_t first=INT64_C(9007199254740992);
  auto ids=openvdb::Int64Grid::create(first);
  auto reference=openvdb::FloatGrid::create(0);
  auto boolean=openvdb::BoolGrid::create(false);
  auto booleanReference=openvdb::FloatGrid::create(0);
  auto mask=openvdb::MaskGrid::create(false);
  auto maskReference=openvdb::FloatGrid::create(0);
  auto vector=openvdb::Vec3IGrid::create(openvdb::Vec3i(0));
  ids->setName("ids");reference->setName("id_reference");
  boolean->setName("bool_values");booleanReference->setName("bool_reference");
  mask->setName("mask_activity");maskReference->setName("mask_reference");vector->setName("vector_i");
  openvdb::GridPtrVec grids{ids,reference,boolean,booleanReference,mask,maskReference,vector};
  for(auto& grid:grids) {
    grid->setTransform(openvdb::math::Transform::createLinearTransform(.5));
    grid->setGridClass(openvdb::GRID_UNKNOWN);
    grid->setSaveFloatAsHalf(false);
    grid->insertMeta("fixture",openvdb::StringMetadata("native discrete slice GPU oracle"));
  }
  vector->setIsInWorldSpace(true);vector->setVectorType(openvdb::VEC_INVARIANT);
  for(int z=-1;z<=1;++z)for(int y=-2;y<=2;++y)for(int x=-2;x<=2;++x) {
    const openvdb::Coord c(x,y,z);const int index=x+2;
    ids->tree().setValue(c,first+index);
    // Independent small exactly represented values, without converting an
    // Int64 ID to Double before subtracting its large baseline.
    reference->tree().setValue(c,float(index)*.25f);
    const bool odd=(index%2)!=0;
    if(odd)boolean->tree().setValueOff(c,true);else boolean->tree().setValueOn(c,false);
    booleanReference->tree().setValue(c,odd?1.f:0.f);
    if(odd)mask->tree().setValueOff(c);else mask->tree().setValueOn(c);
    maskReference->tree().setValue(c,odd?0.f:1.f);
    vector->tree().setValue(c,openvdb::Vec3i(index,4-index,x));
  }
  struct Temporary {
    std::filesystem::path directory,file;
    ~Temporary(){std::error_code e;std::filesystem::remove(file,e);std::filesystem::remove(directory,e);}
  } temporary;
  static std::atomic_uint64_t sequence{};
  for(unsigned attempt=0;attempt<64;++attempt) {
    auto candidate=std::filesystem::temp_directory_path()/("container-discrete-preview-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++sequence));
    if(std::filesystem::create_directory(candidate)){temporary.directory=std::move(candidate);break;}
  }
  if(temporary.directory.empty())throw std::runtime_error("cannot reserve fixture temporary directory");
  temporary.file=temporary.directory/"fixture.vdb";
  openvdb::MetaMap metadata;
  metadata.insertMeta("fixture_schema",openvdb::Int32Metadata(1));
  metadata.insertMeta("exact_first_id",openvdb::Int64Metadata(first));
  openvdb::io::File writer(temporary.file.string());
  writer.setCompression(openvdb::io::COMPRESS_ZIP);writer.write(grids,metadata);writer.close();
  bool owned=false;
  try {
    std::ifstream source(temporary.file,std::ios::binary);
    std::ofstream target(output,std::ios::binary|std::ios::out|std::ios::noreplace);
    if(!target)throw std::runtime_error("cannot exclusively create fixture output");owned=true;
    if(!source)throw std::runtime_error("cannot open completed fixture snapshot");
    target.exceptions(std::ios::badbit|std::ios::failbit);source.exceptions(std::ios::badbit);
    std::array<char,65536> buffer{};
    while(source) {source.read(buffer.data(),std::streamsize(buffer.size()));if(source.gcount())target.write(buffer.data(),source.gcount());}
    if(!source.eof())throw std::runtime_error("fixture snapshot read failed");
    target.close();
  } catch(...) {
    if(owned){std::error_code error;std::filesystem::remove(output,error);}throw;
  }
#else
  (void)output;
  throw std::runtime_error("OpenVDB discrete preview fixtures require -DENABLE_OPENVDB=ON.");
#endif
}
template<class Character> int run(int argc,Character** argv) {
  try {
    if(argc!=2)throw std::invalid_argument("usage: OpenVdbDiscretePreviewFixture OUTPUT.vdb");
    generate(std::filesystem::path(argv[1]));
    std::cout<<"{\"schemaVersion\":1,\"grids\":7,\"firstId\":\"9007199254740992\",\"lastId\":\"9007199254740996\"}\n";
    return 0;
  } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
} // namespace
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){return run(argc,argv);}
#else
int main(int argc,char** argv){return run(argc,argv);}
#endif
