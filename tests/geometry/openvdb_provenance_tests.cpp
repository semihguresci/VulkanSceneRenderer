#include "Container/geometry/OpenVdbBoundaryProjection.h"
#include "Container/geometry/OpenVdbDiagnostics.h"
#include "Container/geometry/OpenVdbFieldAdvection.h"
#include "Container/geometry/OpenVdbFieldComposite.h"
#include "Container/geometry/OpenVdbFieldFilter.h"
#include "Container/geometry/OpenVdbFieldResampling.h"
#include "Container/geometry/OpenVdbFileAuthoring.h"
#include "Container/geometry/OpenVdbFluidSimulation.h"
#include "Container/geometry/OpenVdbFluidViscosity.h"
#include "Container/geometry/OpenVdbMaskedValues.h"
#include "Container/geometry/OpenVdbMultiResolution.h"
#include "Container/geometry/OpenVdbPoisson.h"
#include "Container/geometry/OpenVdbRegions.h"
#include "Container/geometry/OpenVdbVectorProcessing.h"
#include "../../src/geometry/OpenVdbProvenance.h"
#include <gtest/gtest.h>
#include <atomic>
#include <bit>
#include <chrono>
#include <fstream>
#include <map>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#endif
namespace {
namespace vdb=container::geometry::vdb;
TEST(OpenVdbProvenanceAvailability, DisabledBoundaryProducerPrecedesIo){
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  try{(void)vdb::WriteBoundaryProjectedVelocity({},"missing/out.vdb");FAIL();}
  catch(const std::runtime_error& e){EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),std::string::npos);}
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using C=openvdb::Coord;using V=openvdb::Vec3d;using Scalar=openvdb::DoubleGrid;using Mac=openvdb::Vec3DGrid;using Mask=openvdb::BoolGrid;
constexpr const char* Keys[]={"container:boundary_projection_schema","container:boundary_face_channels",
  "container:boundary_projection_state_id","container:boundary_clamped_faces","container:boundary_maximum_clamp"};
class OpenVdbProvenance:public ::testing::Test {
protected:
  void SetUp()override{
    openvdb::initialize();static std::atomic_uint64_t serial{};directory=std::filesystem::temp_directory_path()/("container-vdb-provenance-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));std::filesystem::create_directory(directory);
    auto fluid=Mask::create(false);fluid->setName("fluid");decorate(*fluid);for(int x=0;x<2;++x)for(int y=0;y<2;++y)fluid->tree().setValueOn({x,y,0},true);
    auto velocity=Mac::create(V(0));velocity->setName("wind");decorate(*velocity);write("inputs.vdb",{fluid,velocity});
    vdb::BoundaryProjectionInputs input;input.velocity={file("inputs.vdb"),"wind"};input.fluid={file("inputs.vdb"),"fluid"};vdb::BoundaryProjectionOptions options;options.projection.timeStep=1;
    (void)vdb::WriteBoundaryProjectedVelocity(input,file("canonical.vdb"),options,p);canonicalBundle("canonical.vdb",true);protect("inputs.vdb");protect("canonical.vdb");
    auto flow=Mac::create(V(.25,0,0));flow->setName("flow");write("flow.vdb",{flow});protect("flow.vdb");p.gridName="derived";
  }
  void TearDown()override{for(const auto& [path,bits]:originals)EXPECT_EQ(bytes(path),bits);std::error_code e;for(const auto& item:std::filesystem::directory_iterator(directory))std::filesystem::remove_all(item.path(),e);std::filesystem::remove(directory,e);}
  std::filesystem::path file(const std::string& name)const{return directory/name;}
  static std::string bytes(const std::filesystem::path& path){std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
  void protect(const std::string& name){originals[file(name)]=bytes(file(name));}
  static void decorate(openvdb::GridBase& g){g.insertMeta("artist",openvdb::StringMetadata(std::string("scene\0artist",12)));g.insertMeta("container:boundary_note",openvdb::StringMetadata("caller boundary note"));}
  void write(const std::string& name,const openvdb::GridPtrVec& grids){openvdb::MetaMap metadata;metadata.insertMeta("scene",openvdb::Int64Metadata(INT64_C(9007199254740993)));openvdb::io::File writer(file(name).string());writer.setCompression(openvdb::io::COMPRESS_ZIP);writer.write(grids,metadata);writer.close();}
  auto grids(const std::string& name){openvdb::io::File f(file(name).string());f.open(false);auto result=f.getGrids();f.close();return result;}
  template<class Grid>typename Grid::Ptr read(const std::string& name,const std::string& grid){openvdb::io::File f(file(name).string());f.open(false);auto result=openvdb::gridPtrCast<Grid>(f.readGrid(grid));f.close();return result;}
  void removed(const std::string& name,size_t count=1){const auto all=grids(name);ASSERT_EQ(all->size(),count);for(const auto& g:*all){for(const auto* key:Keys)EXPECT_FALSE((*g)[key])<<g->getName()<<":"<<key;EXPECT_EQ(g->metaValue<std::string>("container:boundary_note"),"caller boundary note");EXPECT_EQ(g->metaValue<std::string>("artist"),std::string("scene\0artist",12));}}
  void fileScene(const std::string& name,bool present=true){openvdb::io::File f(file(name).string());f.open(false);const auto metadata=f.getMetadata();const auto value=metadata->getMetadata<openvdb::Int64Metadata>("scene");if(present){ASSERT_TRUE(value);EXPECT_EQ(value->value(),INT64_C(9007199254740993));}else EXPECT_FALSE((*metadata)["scene"]);f.close();}
  void canonicalBundle(const std::string& name,bool scene){const auto all=grids(name);ASSERT_EQ(all->size(),7);std::string id;
    for(const auto& g:*all){const auto schema=g->getMetadata<openvdb::StringMetadata>(Keys[0]),layout=g->getMetadata<openvdb::StringMetadata>(Keys[1]),member=g->getMetadata<openvdb::StringMetadata>(Keys[2]);ASSERT_TRUE(schema);ASSERT_TRUE(layout);ASSERT_TRUE(member);EXPECT_EQ(schema->value(),"negative_xyz_faces_v1");EXPECT_EQ(layout->value(),"negative_faces_xyz");EXPECT_FALSE(member->value().empty());if(id.empty())id=member->value();EXPECT_EQ(member->value(),id);
      if(g->getName()=="boundary_fraction"){const auto count=g->getMetadata<openvdb::Int64Metadata>(Keys[3]);const auto maximum=g->getMetadata<openvdb::DoubleMetadata>(Keys[4]);ASSERT_TRUE(count);ASSERT_TRUE(maximum);EXPECT_EQ(count->value(),0);EXPECT_EQ(maximum->value(),0);}}
    fileScene(name,scene);
  }
  void preserved(const std::string& name,const std::string& selector={}){const auto all=grids(name);const auto source=grids("canonical.vdb");ASSERT_FALSE(all->empty());
    for(const auto& g:*all){const auto originalName=!selector.empty()?selector:g->getName()=="copied_velocity"?"velocity":g->getName();openvdb::GridBase::Ptr original;for(const auto& candidate:*source)if(candidate->getName()==originalName)original=candidate;ASSERT_TRUE(original)<<originalName;
      for(const auto* key:Keys){const auto actual=(*g)[key],expected=(*original)[key];ASSERT_EQ(bool(actual),bool(expected))<<g->getName()<<":"<<key;if(actual){EXPECT_EQ(actual->typeName(),expected->typeName())<<key;EXPECT_TRUE(*actual==*expected)<<g->getName()<<":"<<key;}}}
  }
  void legacyState(){auto canonical=grids("canonical.vdb");openvdb::GridPtrVec state;Mask::Ptr fluid;
    for(const auto& grid:*canonical)if(grid->getName()=="fluid"||grid->getName()=="velocity"||grid->getName()=="pressure"){state.push_back(grid);if(grid->getName()=="fluid")fluid=openvdb::gridPtrCast<Mask>(grid);}
    ASSERT_TRUE(fluid);for(bool temperature:{false,true}){auto scalar=Scalar::create(0);scalar->insertMeta(*fluid);scalar->setName(temperature?"temperature":"density");scalar->setGridClass(temperature?openvdb::GRID_UNKNOWN:openvdb::GRID_FOG_VOLUME);for(auto c=fluid->cbeginValueOn();c;++c)scalar->tree().setValueOn(c.getCoord(),temperature?300:.25);state.push_back(scalar);}write("legacy.vdb",state);protect("legacy.vdb");
  }
  std::filesystem::path directory;std::map<std::filesystem::path,std::string> originals;vdb::ProcessingOptions p;
};
TEST_F(OpenVdbProvenance, ExactReservedKeysIgnoreWrongTypesAndPreserveAllPayloadMapAndCustomBits){
  auto grid=Scalar::create(0);grid->tree().setValueOn({0,0,0},3);grid->tree().touchLeaf({-1,0,0})->setValueOff({-1,0,0},-0.);grid->transform().postTranslate(V(.25,2,-3));decorate(*grid);
  for(const auto* key:Keys)grid->insertMeta(key,openvdb::Int32Metadata(17));const auto tree=grid->constTreePtr();const auto map=grid->transform().baseMap();const auto active=grid->activeVoxelCount();
  vdb::detail::RemoveBoundaryProjectionMetadata(*grid);vdb::detail::RemoveBoundaryProjectionMetadata(*grid);
  for(const auto* key:Keys)EXPECT_FALSE((*grid)[key]);EXPECT_EQ(grid->constTreePtr(),tree);EXPECT_EQ(grid->transform().baseMap(),map);EXPECT_EQ(grid->activeVoxelCount(),active);EXPECT_EQ(grid->tree().getValue({0,0,0}),3);EXPECT_FALSE(grid->tree().isValueOn({-1,0,0}));EXPECT_EQ(std::bit_cast<uint64_t>(grid->tree().getValue({-1,0,0})),UINT64_C(0x8000000000000000));EXPECT_EQ(grid->metaValue<std::string>("container:boundary_note"),"caller boundary note");
}
TEST_F(OpenVdbProvenance, ResamplingUnaryAndAdvectionPreserveOnlyDocumentedExactCopyBranches){
  auto source=read<Scalar>("canonical.vdb","pressure");
  auto copied=vdb::WriteResampledField(file("canonical.vdb"),file("resample-copy.vdb"),{},"pressure",p);EXPECT_TRUE(copied.exactCopy);preserved("resample-copy.vdb","pressure");fileScene("resample-copy.vdb");
  vdb::FieldResamplingOptions sampling;sampling.targetIndexToWorld=std::array<double,16>{1,0,0,.25,0,1,0,0,0,0,1,0,0,0,0,1};
  (void)vdb::WriteResampledField(file("canonical.vdb"),file("sampled.vdb"),sampling,"pressure",p);removed("sampled.vdb");
  vdb::FieldValueTransformOptions unary;(void)vdb::WriteTransformedField(file("canonical.vdb"),file("copy.vdb"),unary,"pressure",p);preserved("copy.vdb","pressure");
  unary.operation=vdb::FieldValueOperation::Affine;(void)vdb::WriteTransformedField(file("canonical.vdb"),file("identity-arithmetic.vdb"),unary,"pressure",p);removed("identity-arithmetic.vdb");
  vdb::FieldAdvectionOptions advection;advection.timeStep=0;auto noOp=vdb::WriteAdvectedField(file("canonical.vdb"),file("flow.vdb"),file("no-op.vdb"),advection,"pressure","flow",p);EXPECT_TRUE(noOp.noOp);preserved("no-op.vdb","pressure");fileScene("no-op.vdb");
  advection.timeStep=1;auto moved=vdb::WriteAdvectedField(file("canonical.vdb"),file("flow.vdb"),file("advected.vdb"),advection,"pressure","flow",p);EXPECT_FALSE(moved.noOp);removed("advected.vdb");
  for(const auto* name:{"resample-copy.vdb","copy.vdb","no-op.vdb"}){auto result=read<Scalar>(name,"derived");EXPECT_EQ(result->activeVoxelCount(),source->activeVoxelCount());for(auto v=source->cbeginValueAll();v;++v){EXPECT_EQ(result->tree().isValueOn(v.getCoord()),v.isValueOn());EXPECT_EQ(std::bit_cast<uint64_t>(result->tree().getValue(v.getCoord())),std::bit_cast<uint64_t>(double(*v)));}EXPECT_EQ(result->transform().baseMap()->applyMap(V(0)),source->transform().baseMap()->applyMap(V(0)));}
}
TEST_F(OpenVdbProvenance, ComputedIdenticalCompositionFilteringAndDifferentialPoissonOutputsInvalidate){
  (void)vdb::WriteCompositeField(file("canonical.vdb"),file("canonical.vdb"),file("sum.vdb"),vdb::FieldCompositeOperation::Sum,{},"pressure","pressure",p);removed("sum.vdb");
  (void)vdb::WriteFilteredField(file("canonical.vdb"),file("filtered.vdb"),vdb::FieldFilterOperation::Mean,{},"pressure",p);removed("filtered.vdb");
  (void)vdb::WriteDifferential(file("canonical.vdb"),file("gradient.vdb"),vdb::DifferentialOperation::Gradient,"pressure",p);removed("gradient.vdb");
  auto solved=vdb::WritePoissonSolution(file("canonical.vdb"),file("poisson.vdb"),"pressure",{},p);EXPECT_TRUE(solved.converged);removed("poisson.vdb");
}
TEST_F(OpenVdbProvenance, TopologyValuesMaskedEditsAndComponentMasksInvalidateDerivedMembership){
  (void)vdb::WriteTopologyMask(file("canonical.vdb"),file("topology.vdb"),vdb::TopologyOperation::ActiveMask,{},"fluid",p);removed("topology.vdb");
  (void)vdb::WriteValueTopology(file("canonical.vdb"),file("pruned.vdb"),vdb::ValueTopologyOperation::PruneExact,{},"pressure",p);removed("pruned.vdb");
  (void)vdb::WriteMaskedValues(file("canonical.vdb"),file("canonical.vdb"),file("masked.vdb"),vdb::MaskedValuesOperation::IntersectActivity,"pressure","fluid",p);removed("masked.vdb");
  const auto components=vdb::WriteRegions(file("canonical.vdb"),file("regions.vdb"),vdb::RegionsOperation::ActiveComponents,{},"fluid",p);ASSERT_FALSE(components.grids.empty());removed("regions.vdb",components.grids.size());
}
TEST_F(OpenVdbProvenance, EveryMultiresolutionLevelInvalidatesButOneLevelPassthroughPreserves){
  vdb::MultiResolutionOptions options;options.levels=1;auto exact=vdb::BuildMultiResolution(file("canonical.vdb"),options,"pressure",p);exact->writeAll(file("one.vdb"),p);preserved("one.vdb","pressure");
  options.levels=2;auto filtered=vdb::BuildMultiResolution(file("canonical.vdb"),options,"pressure",p);filtered->writeAll(file("two.vdb"),p);removed("two.vdb",2);
}
TEST_F(OpenVdbProvenance, ReadOnlyDiagnosticsPreserveSourcesButEmptyAndNonemptyOwnedFailureMasksInvalidate){
  (void)vdb::DiagnoseVdb(file("canonical.vdb"),{},"pressure",p);preserved("canonical.vdb");
  (void)vdb::WriteVdbDiagnosticMask(file("canonical.vdb"),file("empty-failure.vdb"),{},"pressure",p);removed("empty-failure.vdb");EXPECT_EQ(grids("empty-failure.vdb")->front()->activeVoxelCount(),0);
  vdb::DiagnosticsOptions options;options.componentRange=std::array<double,2>{1,2};(void)vdb::WriteVdbDiagnosticMask(file("canonical.vdb"),file("failures.vdb"),options,"pressure",p);removed("failures.vdb");EXPECT_EQ(grids("failures.vdb")->front()->activeVoxelCount(),4);
}
TEST_F(OpenVdbProvenance, LegacyProjectionSimulationSourcesAndBothViscosityBranchesInvalidateEveryStateMember){
  legacyState();(void)vdb::WriteProjectedVelocity(file("canonical.vdb"),file("canonical.vdb"),file("closed.vdb"),{},"velocity","fluid",p);removed("closed.vdb",3);
  vdb::FluidSourcesOptions sources;(void)vdb::WriteFluidSources(file("legacy.vdb"),file("sources-copy.vdb"),sources,p);removed("sources-copy.vdb",5);
  for(double viscosity:{0.,.01}){vdb::FluidViscosityOptions options;options.kinematicViscosity=viscosity;const auto name=viscosity==0?"zero-viscosity.vdb":"viscosity.vdb";(void)vdb::WriteViscousFluidState(file("legacy.vdb"),file(name),options,p);removed(name,5);}
  vdb::FluidSimulationInputs in;in.density=in.velocity=in.domain=in.temperature=file("legacy.vdb");in.densityGrid="density";in.velocityGrid="velocity";in.domainGrid="fluid";in.temperatureGrid="temperature";
  (void)vdb::WriteFluidStep(in,file("simulation.vdb"),{},p);removed("simulation.vdb",5);
  fileScene("closed.vdb",false);vdb::BoundaryProjectionInputs input;input.fluid={file("closed.vdb"),"fluid"};input.velocity={file("closed.vdb"),"velocity"};(void)vdb::WriteBoundaryProjectedVelocity(input,file("fresh.vdb"),{},p);canonicalBundle("fresh.vdb",false);
  auto oldId=read<Mask>("canonical.vdb","fluid")->metaValue<std::string>(Keys[2]);auto freshId=read<Mask>("fresh.vdb","fluid")->metaValue<std::string>(Keys[2]);EXPECT_NE(oldId,freshId);vdb::BoundaryProjectionReplay replay;replay.state=file("fresh.vdb");EXPECT_NO_THROW((void)vdb::WriteReprojectedBoundaryState(replay,file("fresh-replay.vdb"),{},p));canonicalBundle("fresh-replay.vdb",false);
}
TEST_F(OpenVdbProvenance, PureFileRenameCopyRemainsReplayableAndDerivedFluidCannotAdvertiseOldBundle){
  vdb::FileAuthoringOptions authoring;vdb::FileAuthoringInput input;input.path=file("canonical.vdb");input.edits.push_back({"velocity",std::string("copied_velocity"),{}});authoring.inputs.push_back(input);(void)vdb::WriteAuthoredVdb(file("copied.vdb"),authoring,p);preserved("copied.vdb");fileScene("copied.vdb");
  vdb::BoundaryProjectionReplay replay;replay.state=file("copied.vdb");replay.velocityGrid="copied_velocity";EXPECT_NO_THROW((void)vdb::WriteReprojectedBoundaryState(replay,file("copied-replay.vdb"),{},p));canonicalBundle("copied-replay.vdb",true);
  vdb::FieldResamplingOptions sampling;sampling.interpolation=vdb::FieldResamplingInterpolation::Nearest;sampling.targetIndexToWorld=std::array<double,16>{1,0,0,.25,0,1,0,0,0,0,1,0,0,0,0,1};auto limits=p;limits.gridName="fluid";(void)vdb::WriteResampledField(file("canonical.vdb"),file("derived-fluid.vdb"),sampling,"fluid",limits);removed("derived-fluid.vdb");
  auto bundle=grids("canonical.vdb");for(auto& g:*bundle)if(g->getName()=="fluid")g=read<Mask>("derived-fluid.vdb","fluid");write("invalid-bundle.vdb",*bundle);replay={};replay.state=file("invalid-bundle.vdb");
  try{(void)vdb::WriteReprojectedBoundaryState(replay,file("must-not-exist.vdb"),{},p);FAIL();}catch(const std::invalid_argument& e){EXPECT_NE(std::string(e.what()).find("missing valid schema"),std::string::npos);}EXPECT_FALSE(std::filesystem::exists(file("must-not-exist.vdb")));preserved("canonical.vdb");
}
#endif
} // namespace
