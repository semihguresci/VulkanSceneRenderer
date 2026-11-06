#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/File.h>
#include <openvdb/openvdb.h>
#endif
namespace {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
openvdb::GridPtrVec regionFixtures(){
  const openvdb::Coord point(-1,2,3),inactive(0,2,3);
  auto boolean=openvdb::BoolGrid::create(true);boolean->setName("bool");boolean->tree().touchLeaf(point)->setValueOn(point,false);boolean->tree().touchLeaf(inactive)->setValueOff(inactive,true);
  auto mask=openvdb::MaskGrid::create(false);mask->setName("mask");mask->tree().setValueOn(point);mask->tree().setValueOff(inactive);
  auto i32=openvdb::Int32Grid::create(0);i32->setName("int32");i32->tree().setValueOn(point,INT32_MIN);
  auto i64=openvdb::Int64Grid::create(INT64_MIN);i64->setName("int64");i64->tree().setValueOn(point,INT64_C(9007199254740993));
  auto f=openvdb::FloatGrid::create(0);f->setName("float");auto* fl=f->tree().touchLeaf(point);fl->setValueOn(point);const uint32_t fb=0x7f812345;std::memcpy(fl->buffer().data()+fl->coordToOffset(point),&fb,sizeof(fb));
  auto d=openvdb::DoubleGrid::create(0);d->setName("double");auto* dl=d->tree().touchLeaf(point);dl->setValueOn(point);const uint64_t db=0xfff0000000001234;std::memcpy(dl->buffer().data()+dl->coordToOffset(point),&db,sizeof(db));
  auto vi=openvdb::Vec3IGrid::create(openvdb::Vec3i(0));vi->setName("vec3i");vi->tree().setValueOn(point,openvdb::Vec3i(INT32_MIN,0,INT32_MAX));
  auto vf=openvdb::Vec3SGrid::create(openvdb::Vec3s(0));vf->setName("vec3f");auto* vfl=vf->tree().touchLeaf(point);vfl->setValueOn(point);const std::array<uint32_t,3> vfb{0x80000000,0x7f812345,0xc0000000};std::memcpy(&vfl->buffer().data()[vfl->coordToOffset(point)][0],vfb.data(),sizeof(vfb));
  auto vd=openvdb::Vec3DGrid::create(openvdb::Vec3d(0));vd->setName("vec3d");auto* vdl=vd->tree().touchLeaf(point);vdl->setValueOn(point);const std::array<uint64_t,3> vdb{0x8000000000000000,0x7ff0000000001234,0xc000000000000000};std::memcpy(&vdl->buffer().data()[vdl->coordToOffset(point)][0],vdb.data(),sizeof(vdb));
  auto tiles=openvdb::Int64Grid::create(0);tiles->setName("tiles");tiles->tree().addTile(3,openvdb::Coord(0),INT64_C(9007199254740993),true);tiles->tree().addTile(3,openvdb::Coord(4096,0,0),INT64_MAX,true);
  openvdb::GridPtrVec grids{boolean,mask,i32,i64,f,d,vi,vf,vd,tiles};
  for(auto& grid:grids){grid->setTransform(openvdb::math::Transform::createLinearTransform(.5));grid->setGridClass(openvdb::GRID_UNKNOWN);grid->setSaveFloatAsHalf(false);grid->insertMeta("binary",openvdb::StringMetadata(std::string("raw\0\xff",5)));grid->insertMeta("fixture",openvdb::StringMetadata("native active region query oracle"));}
  for(auto& grid:{openvdb::GridBase::Ptr(vi),openvdb::GridBase::Ptr(vf),openvdb::GridBase::Ptr(vd)}){grid->setIsInWorldSpace(false);grid->setVectorType(openvdb::VEC_CONTRAVARIANT_ABSOLUTE);}
  return grids;
}
openvdb::GridPtrVec rayWeakFixtures(bool opposite){
  auto grid=openvdb::DoubleGrid::create(4.);grid->setName("phi");grid->setGridClass(openvdb::GRID_LEVEL_SET);grid->setSaveFloatAsHalf(false);
  for(int z=-3;z<=3;++z)for(int y=-3;y<=3;++y){
    for(int x=-3;x<=3;++x){const double value=opposite?(x<=0?-1e-300:3e-300):(x<0?-1e-300:(x+1)*1e-300);grid->tree().setValueOn(openvdb::Coord(x,y,z),value);}
    if(opposite)for(int x=-6;x<=-4;++x)grid->tree().touchLeaf(openvdb::Coord(x,y,z))->setValueOff(openvdb::Coord(x,y,z),-4.);
  }return {grid};
}
openvdb::GridPtrVec rayVolumeFixtures(){
  const openvdb::Coord point(0);
  auto f=openvdb::FloatGrid::create(0);f->setName("float");auto* fl=f->tree().touchLeaf(point);fl->setValueOn(point);const uint32_t fb=0x7fc12345;std::memcpy(fl->buffer().data(),&fb,sizeof(fb));
  auto d=openvdb::DoubleGrid::create(0);d->setName("double");auto* dl=d->tree().touchLeaf(point);dl->setValueOn(point);const uint64_t db=0x7ff8000000001234;std::memcpy(dl->buffer().data(),&db,sizeof(db));
  auto b=openvdb::BoolGrid::create(false);b->setName("bool");b->tree().setValueOn(point,false);
  auto m=openvdb::MaskGrid::create(false);m->setName("mask");m->tree().setValueOn(point);
  auto i=openvdb::Int32Grid::create(0);i->setName("int32");i->tree().setValueOn(point,INT32_MIN);
  auto j=openvdb::Int64Grid::create(0);j->setName("int64");j->tree().setValueOn(point,INT64_C(9007199254740993));
  auto vi=openvdb::Vec3IGrid::create(openvdb::Vec3i(0));vi->setName("vec3i");vi->tree().setValueOn(point,openvdb::Vec3i(INT32_MIN,0,INT32_MAX));
  auto vs=openvdb::Vec3SGrid::create(openvdb::Vec3s(0));vs->setName("vec3s");auto* sl=vs->tree().touchLeaf(point);sl->setValueOn(point);const std::array<uint32_t,3> sb{0x80000000,0x7fc12345,0x7f800000};std::memcpy(&sl->buffer().data()[0][0],sb.data(),sizeof(sb));
  auto vd=openvdb::Vec3DGrid::create(openvdb::Vec3d(0));vd->setName("vec3d");auto* vl=vd->tree().touchLeaf(point);vl->setValueOn(point);const std::array<uint64_t,3> vb{0x8000000000000000,0x7ff8000000001234,0x7ff0000000000000};std::memcpy(&vl->buffer().data()[0][0],vb.data(),sizeof(vb));
  return {f,d,b,m,i,j,vi,vs,vd};
}
openvdb::GridPtrVec raySpanFixtures(){
  auto grid=openvdb::BoolGrid::create(false);grid->setName("spans");for(int x:{0,8,24})grid->tree().setValueOn(openvdb::Coord(x,0,0),false);grid->tree().touchLeaf(openvdb::Coord(64,0,0))->setValueOff(openvdb::Coord(64,0,0),true);return {grid};
}
#endif
void writeFixture(const std::filesystem::path& output
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
    ,const openvdb::GridPtrVec& grids,bool verifyRegion=false
#endif
){
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  if(output.empty()||std::filesystem::exists(output)||!std::filesystem::is_directory(output.has_parent_path()?output.parent_path():std::filesystem::path(".")))throw std::invalid_argument("query fixture output must be new in an existing directory");
  struct Temporary{std::filesystem::path directory,file;~Temporary(){std::error_code e;std::filesystem::remove(file,e);std::filesystem::remove(directory,e);}} temp;
  static std::atomic_uint64_t serial{};for(int i=0;i<64;++i){auto directory=std::filesystem::temp_directory_path()/("container-query-fixture-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));if(std::filesystem::create_directory(directory)){temp.directory=std::move(directory);break;}}if(temp.directory.empty())throw std::runtime_error("cannot reserve native query fixture temporary directory");temp.file=temp.directory/"fixture.vdb";
  openvdb::MetaMap metadata;metadata.insertMeta("fixture_schema",openvdb::Int32Metadata(1));metadata.insertMeta("exact_id",openvdb::Int64Metadata(INT64_C(9007199254740993)));metadata.insertMeta("binary",openvdb::StringMetadata(std::string("raw\0\xff",5)));
  openvdb::io::File file(temp.file.string());file.setCompression(openvdb::io::COMPRESS_ZIP);file.write(grids,metadata);file.close();
  // Independent SDK readback verifies that fixture construction/serialization
  // did not quiet signaling NaNs before the adapter under test sees the file.
  if(verifyRegion){openvdb::io::File read(temp.file.string());read.open(false);const auto f=openvdb::gridPtrCast<openvdb::FloatGrid>(read.readGrid("float"));const auto d=openvdb::gridPtrCast<openvdb::DoubleGrid>(read.readGrid("double"));uint32_t fb{};uint64_t db{};const auto& fv=f->tree().getValue(openvdb::Coord(-1,2,3));const auto& dv=d->tree().getValue(openvdb::Coord(-1,2,3));std::memcpy(&fb,&fv,sizeof(fb));std::memcpy(&db,&dv,sizeof(db));if(fb!=0x7f812345||db!=0xfff0000000001234)throw std::runtime_error("native query fixture signaling NaN bits changed before API input");read.close();}
  bool owned=false;try{std::ifstream source(temp.file,std::ios::binary);std::ofstream target(output,std::ios::binary|std::ios::out|std::ios::noreplace);if(!target)throw std::runtime_error("cannot exclusively create query fixture");owned=true;if(!source)throw std::runtime_error("cannot read completed query fixture");target.exceptions(std::ios::badbit|std::ios::failbit);source.exceptions(std::ios::badbit);std::array<char,65536> buffer{};while(source){source.read(buffer.data(),std::streamsize(buffer.size()));if(source.gcount())target.write(buffer.data(),source.gcount());}if(!source.eof())throw std::runtime_error("native query fixture copy failed");target.close();}catch(...){if(owned){std::error_code e;std::filesystem::remove(output,e);}throw;}
#else
  (void)output;throw std::runtime_error("OpenVDB query fixtures require -DENABLE_OPENVDB=ON.");
#endif
}
void generate(const std::filesystem::path& output,bool rays){
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  openvdb::initialize();
  if(!rays){writeFixture(output,regionFixtures(),true);return;}
  if(!std::filesystem::is_directory(output))throw std::invalid_argument("--ray-fixtures requires an existing directory");
  const std::array<const char*,4> names{"ray-weak-same.vdb","ray-weak-opposite.vdb","ray-volume-nine.vdb","ray-spans.vdb"};
  for(const auto* name:names)if(std::filesystem::exists(output/name))throw std::invalid_argument("ray fixture outputs must all be new");
  std::vector<std::filesystem::path> completed;completed.reserve(names.size());
  try{for(size_t i=0;i<names.size();++i){const auto grids=i<2?rayWeakFixtures(i==1):(i==2?rayVolumeFixtures():raySpanFixtures());for(const auto& grid:grids){grid->insertMeta("binary",openvdb::StringMetadata(std::string("raw\0\xff",5)));grid->insertMeta("exact_id",openvdb::Int64Metadata(INT64_C(9007199254740993)));}auto path=output/names[i];writeFixture(path,grids);completed.push_back(std::move(path));}}
  catch(...){for(const auto& path:completed){std::error_code e;std::filesystem::remove(path,e);}throw;}
#else
  (void)output;(void)rays;throw std::runtime_error("OpenVDB query fixtures require -DENABLE_OPENVDB=ON.");
#endif
}
template<class Character>int run(int argc,Character** argv){try{
  if(argc!=3||(std::filesystem::path(argv[1])!="--region"&&std::filesystem::path(argv[1])!="--ray-fixtures"))throw std::invalid_argument("usage: OpenVdbQueryFixtureGenerator --region OUTPUT.vdb | --ray-fixtures EXISTING_DIR");
  const bool rays=std::filesystem::path(argv[1])=="--ray-fixtures";generate(std::filesystem::path(argv[2]),rays);std::cout<<(rays?"{\"schemaVersion\":1,\"mode\":\"ray\",\"files\":4}\n":"{\"schemaVersion\":1,\"mode\":\"region\",\"grids\":10}\n");return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
} // namespace
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){return run(argc,argv);}
#else
int main(int argc,char** argv){return run(argc,argv);}
#endif
