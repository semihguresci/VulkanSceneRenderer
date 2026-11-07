#include "Container/geometry/OpenVdbSdfFilters.h"
#include "Container/geometry/OpenVdbInspect.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <bit>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/io/io.h>
#include <openvdb/tools/LevelSetSphere.h>
#include <openvdb/tools/Composite.h>
#endif
namespace {
namespace vdb=container::geometry::vdb;
TEST(SdfFilterAvailability, DisabledPrecedesOptionsAndFilesystem) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  EXPECT_THROW((void)vdb::WriteSdfFiltered("missing","exists",vdb::SdfFilterOperation::Mean,{}, {},{.maxVoxelCount=0}),std::runtime_error);
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class SdfFilters:public ::testing::Test {
protected:
  std::filesystem::path directory;
  void SetUp()override {
    openvdb::initialize();static std::atomic_uint64_t sequence{0};
    directory=std::filesystem::temp_directory_path()/("container-sdf-filter-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++sequence));
    if(!std::filesystem::create_directory(directory))throw std::runtime_error("new test directory required");
  }
  void TearDown()override {
    if(directory.empty())return;
    std::error_code ec;
    if(std::filesystem::is_symlink(directory,ec) || ec)return;
    const auto target=std::filesystem::weakly_canonical(directory,ec);
    if(ec)return;
    const auto parent=std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(),ec);
    if(!ec && target.parent_path()==parent && target.filename()==directory.filename() &&
        target.filename().string().starts_with("container-sdf-filter-"))std::filesystem::remove_all(target,ec);
  }
  auto path(const char* name)const{return directory/name;}
  template<class Grid> static typename Grid::Ptr sphere() {
    auto result=openvdb::tools::createLevelSetSphere<Grid>(1.,openvdb::Vec3f(0),.25,3.);
    result->setName("surface");result->insertMeta("artist",openvdb::StringMetadata("native"));return result;
  }
  static void write(const std::filesystem::path& path,const openvdb::GridBase::Ptr& grid) {
    openvdb::MetaMap metadata;metadata.insertMeta("owner",openvdb::StringMetadata("filter"));
    openvdb::io::File file(path.string());file.write(openvdb::GridPtrVec{grid},metadata);
  }
  template<class Grid> static typename Grid::Ptr read(const std::filesystem::path& path) {
    openvdb::io::File file(path.string());file.open(false);const auto result=openvdb::gridPtrCast<Grid>(file.readGrid(file.beginName().gridName()));file.close();return result;
  }
};
TEST_F(SdfFilters, FloatAndDoubleCurvatureLaplacianAndFilletProduceFiniteNativeFields) {
  for(bool nativeDouble:{false,true}) {
    if(nativeDouble)write(path("source.vdb"),sphere<openvdb::DoubleGrid>());
    else write(path("source.vdb"),sphere<openvdb::FloatGrid>());
    for(const auto op:{vdb::SdfFilterOperation::MeanCurvature,vdb::SdfFilterOperation::Laplacian,vdb::SdfFilterOperation::Fillet}) {
      SCOPED_TRACE(int(op));
      const auto report=vdb::WriteSdfFiltered(path("source.vdb"),path("result.vdb"),op);
      EXPECT_GT(report.grid.activeVoxelCount,0u);EXPECT_EQ(report.grid.voxelSize,.25);
      const auto check=[&](const auto& grid) {
        EXPECT_EQ(grid->getGridClass(),openvdb::GRID_LEVEL_SET);EXPECT_EQ(grid->background(),.75);
        EXPECT_EQ(grid->template metaValue<std::string>("artist"),"native");
        EXPECT_FALSE(grid->saveFloatAsHalf());
        for(auto value=grid->cbeginValueAll();value;++value)EXPECT_TRUE(std::isfinite(*value));
      };
      if(nativeDouble)check(read<openvdb::DoubleGrid>(path("result.vdb")));
      else check(read<openvdb::FloatGrid>(path("result.vdb")));
      std::filesystem::remove(path("result.vdb"));
    }
  }
}
TEST_F(SdfFilters, TrackerBandOperationsHaveExplicitPositiveNativeWidths) {
  write(path("source.vdb"),sphere<openvdb::DoubleGrid>());
  for(const auto op:{vdb::SdfFilterOperation::Normalize,vdb::SdfFilterOperation::Track,
      vdb::SdfFilterOperation::Prune,vdb::SdfFilterOperation::ResizeBand,
      vdb::SdfFilterOperation::DilateBand,vdb::SdfFilterOperation::ErodeBand}) {
    vdb::SdfFilterOptions options;if(op==vdb::SdfFilterOperation::ResizeBand)options.bandWidth=4;
    const auto report=vdb::WriteSdfFiltered(path("source.vdb"),path("result.vdb"),op,options);
    EXPECT_GT(report.grid.activeVoxelCount,0u);
    const auto result=read<openvdb::DoubleGrid>(path("result.vdb"));
    const double expected=op==vdb::SdfFilterOperation::ResizeBand || op==vdb::SdfFilterOperation::DilateBand?1.:
        op==vdb::SdfFilterOperation::ErodeBand?.5:.75;
    EXPECT_EQ(result->background(),expected);
    std::filesystem::remove(path("result.vdb"));
  }
}
TEST_F(SdfFilters, NativeFloatAndDoubleAlphaMasksRunWithMatchingMaps) {
  for(bool sourceDouble:{false,true}) {
  openvdb::GridBase::Ptr grid;
  if(sourceDouble)grid=sphere<openvdb::DoubleGrid>();else grid=sphere<openvdb::FloatGrid>();
  write(path("source.vdb"),grid);
  for(bool nativeDouble:{false,true}) {
    if(nativeDouble) {
      auto mask=openvdb::DoubleGrid::create(.5);mask->setTransform(std::make_shared<openvdb::math::Transform>(grid->transform()));write(path("mask.vdb"),mask);
    } else {
      auto mask=openvdb::FloatGrid::create(.5f);mask->setTransform(std::make_shared<openvdb::math::Transform>(grid->transform()));write(path("mask.vdb"),mask);
    }
    vdb::SdfFilterOptions options;options.maskInput=path("mask.vdb");
    const auto report=vdb::WriteSdfFiltered(path("source.vdb"),path("result.vdb"),vdb::SdfFilterOperation::Mean,options);
    EXPECT_GT(report.grid.activeVoxelCount,0u);std::filesystem::remove(path("result.vdb"));
  }
  }
}
TEST_F(SdfFilters, EverySpatialTemporalSchemeAndTrimModeHasFiniteNativeOutputAndQuality) {
  for(bool nativeDouble:{false,true}) {
  SCOPED_TRACE(nativeDouble);
  if(nativeDouble)write(path("source.vdb"),sphere<openvdb::DoubleGrid>());
  else write(path("source.vdb"),sphere<openvdb::FloatGrid>());
  for(int spatial=0;spatial<5;++spatial)for(int temporal=0;temporal<3;++temporal) {
    vdb::SdfFilterOptions o;o.spatial=vdb::SdfSpatialScheme(spatial);o.temporal=vdb::SdfTemporalScheme(temporal);
    const auto report=vdb::WriteSdfFiltered(path("source.vdb"),path("result.vdb"),vdb::SdfFilterOperation::Track,o);
    EXPECT_EQ(report.halfWidthVoxels,3.);EXPECT_GT(report.gradientSampleCount,0u);
    ASSERT_TRUE(report.meanGradientError);EXPECT_LT(*report.meanGradientError,.2);
    std::filesystem::remove(path("result.vdb"));
  }
  for(int trim=0;trim<4;++trim) {
    vdb::SdfFilterOptions o;o.trim=vdb::SdfTrimMode(trim);
    const auto report=vdb::WriteSdfFiltered(path("source.vdb"),path("result.vdb"),vdb::SdfFilterOperation::MeanCurvature,o);
    EXPECT_GT(report.grid.activeVoxelCount,0u);
    const auto check=[](const auto& result) {
      for(auto value=result->cbeginValueAll();value;++value)EXPECT_TRUE(std::isfinite(*value));
    };
    if(nativeDouble)check(read<openvdb::DoubleGrid>(path("result.vdb")));
    else check(read<openvdb::FloatGrid>(path("result.vdb")));
    std::filesystem::remove(path("result.vdb"));
  }
  }
}
TEST_F(SdfFilters, CurvatureAndLaplacianShrinkSphereWhileFilletFillsConcavity) {
  const auto source=sphere<openvdb::DoubleGrid>();write(path("source.vdb"),source);
  for(const auto op:{vdb::SdfFilterOperation::MeanCurvature,vdb::SdfFilterOperation::Laplacian}) {
    (void)vdb::WriteSdfFiltered(path("source.vdb"),path("result.vdb"),op);
    const auto result=read<openvdb::DoubleGrid>(path("result.vdb"));
    EXPECT_GT(result->tree().getValue({4,0,0}),source->tree().getValue({4,0,0}));
    std::filesystem::remove(path("result.vdb"));
  }
  auto left=openvdb::tools::createLevelSetSphere<openvdb::DoubleGrid>(1.,openvdb::Vec3f(-.75f,0,0),.25,3.);
  auto right=openvdb::tools::createLevelSetSphere<openvdb::DoubleGrid>(1.,openvdb::Vec3f(.75f,0,0),.25,3.);
  openvdb::tools::csgUnion(*left,*right);write(path("concavity.vdb"),left);
  vdb::SdfFilterOptions o;o.iterations=4;
  (void)vdb::WriteSdfFiltered(path("concavity.vdb"),path("tracked.vdb"),vdb::SdfFilterOperation::Track,o);
  (void)vdb::WriteSdfFiltered(path("concavity.vdb"),path("fillet.vdb"),vdb::SdfFilterOperation::Fillet,o);
  EXPECT_LT(read<openvdb::DoubleGrid>(path("fillet.vdb"))->tree().getValue({0,3,0}),
      read<openvdb::DoubleGrid>(path("tracked.vdb"))->tree().getValue({0,3,0})-1e-5);
}
TEST_F(SdfFilters, ZeroAlphaMatchesTrackingAndInverseAlphaMatchesUnmaskedMean) {
  const auto source=sphere<openvdb::DoubleGrid>();write(path("source.vdb"),source);
  auto mask=openvdb::DoubleGrid::create(0.);
  mask->setTransform(std::make_shared<openvdb::math::Transform>(source->transform()));write(path("mask.vdb"),mask);
  vdb::SdfFilterOptions o;o.maskInput=path("mask.vdb");
  (void)vdb::WriteSdfFiltered(path("source.vdb"),path("masked.vdb"),vdb::SdfFilterOperation::Mean,o);
  (void)vdb::WriteSdfFiltered(path("source.vdb"),path("track.vdb"),vdb::SdfFilterOperation::Track);
  const auto a=read<openvdb::DoubleGrid>(path("masked.vdb")),b=read<openvdb::DoubleGrid>(path("track.vdb"));
  EXPECT_EQ(a->activeVoxelCount(),b->activeVoxelCount());
  for(auto value=a->cbeginValueOn();value;++value)EXPECT_EQ(*value,b->tree().getValue(value.getCoord()));
  o.invertAlpha=true;
  (void)vdb::WriteSdfFiltered(path("source.vdb"),path("inverse.vdb"),vdb::SdfFilterOperation::Mean,o);
  (void)vdb::WriteSdfFiltered(path("source.vdb"),path("mean.vdb"),vdb::SdfFilterOperation::Mean);
  const auto c=read<openvdb::DoubleGrid>(path("inverse.vdb")),d=read<openvdb::DoubleGrid>(path("mean.vdb"));
  EXPECT_EQ(c->activeVoxelCount(),d->activeVoxelCount());
  for(auto value=c->cbeginValueOn();value;++value)EXPECT_NEAR(*value,d->tree().getValue(value.getCoord()),1e-12);
}
TEST_F(SdfFilters, KernelOperationsAndSignedWorldOffsetsHaveNativeWorkflows) {
  write(path("source.vdb"),sphere<openvdb::FloatGrid>());
  for(const auto op:{vdb::SdfFilterOperation::Gaussian,vdb::SdfFilterOperation::Mean,vdb::SdfFilterOperation::Median}) {
    const auto report=vdb::WriteSdfFiltered(path("source.vdb"),path("result.vdb"),op);
    EXPECT_GT(report.grid.activeVoxelCount,0u);std::filesystem::remove(path("result.vdb"));
  }
  vdb::SdfFilterOptions o;o.worldDistance=.125;
  (void)vdb::WriteSdfFiltered(path("source.vdb"),path("expanded.vdb"),vdb::SdfFilterOperation::Offset,o);
  o.worldDistance=-.125;
  (void)vdb::WriteSdfFiltered(path("source.vdb"),path("eroded.vdb"),vdb::SdfFilterOperation::Offset,o);
  EXPECT_LT(read<openvdb::FloatGrid>(path("expanded.vdb"))->tree().getValue({4,0,0}),0);
  EXPECT_GT(read<openvdb::FloatGrid>(path("eroded.vdb"))->tree().getValue({4,0,0}),0);
}
TEST_F(SdfFilters, NoOpAndNativePruneRetainTinyValuesAndSignedZeroExactly) {
  auto source=openvdb::DoubleGrid::create(.3);source->setGridClass(openvdb::GRID_LEVEL_SET);
  source->setTransform(openvdb::math::Transform::createLinearTransform(.1));
  source->tree().setValue({0,0,0},-0.);source->tree().setValue({1,0,0},1.125e-310);
  write(path("source.vdb"),source);
  for(const auto op:{vdb::SdfFilterOperation::Offset,vdb::SdfFilterOperation::Normalize,vdb::SdfFilterOperation::Prune}) {
    vdb::SdfFilterOptions o;if(op==vdb::SdfFilterOperation::Normalize)o.normalizationCount=0;
    (void)vdb::WriteSdfFiltered(path("source.vdb"),path("result.vdb"),op,o);
    const auto result=read<openvdb::DoubleGrid>(path("result.vdb"));
    for(int x=0;x<2;++x)EXPECT_EQ(std::bit_cast<uint64_t>(result->tree().getValue({x,0,0})),
        std::bit_cast<uint64_t>(source->tree().getValue({x,0,0})));
    EXPECT_EQ(result->background(),source->background());std::filesystem::remove(path("result.vdb"));
  }
}
TEST_F(SdfFilters, DistantWorldOriginsRetainExactCoefficientsAndSameLatticeDistances) {
  auto source=openvdb::tools::createLevelSetSphere<openvdb::DoubleGrid>(1.,openvdb::Vec3f(0),.1,3.);
  write(path("source.vdb"),source);
  (void)vdb::WriteSdfFiltered(path("source.vdb"),path("reference.vdb"),vdb::SdfFilterOperation::MeanCurvature);
  source->transform().postTranslate(openvdb::Vec3d(1e8,-1e8,1e8));write(path("distant.vdb"),source);
  const auto matrix=source->transform().baseMap()->getAffineMap()->getMat4();
  (void)vdb::WriteSdfFiltered(path("distant.vdb"),path("result.vdb"),vdb::SdfFilterOperation::MeanCurvature);
  const auto result=read<openvdb::DoubleGrid>(path("result.vdb")),reference=read<openvdb::DoubleGrid>(path("reference.vdb"));
  const auto actual=result->transform().baseMap()->getAffineMap()->getMat4();
  for(int r=0;r<4;++r)for(int c=0;c<4;++c)EXPECT_EQ(actual(r,c),matrix(r,c));
  EXPECT_EQ(result->activeVoxelCount(),reference->activeVoxelCount());
  for(auto value=result->cbeginValueOn();value;++value)EXPECT_EQ(*value,reference->tree().getValue(value.getCoord()));
}
TEST_F(SdfFilters, InvalidOptionsAndAllSharedBudgetsRejectWithoutPublishing) {
  write(path("source.vdb"),sphere<openvdb::FloatGrid>());
  for(int invalid=0;invalid<11;++invalid) {
    vdb::SdfFilterOptions o;vdb::ProcessingOptions p;auto op=vdb::SdfFilterOperation::Mean;
    if(invalid==0)o.iterations=0;
    if(invalid==1)o.normalizationCount=65;
    if(invalid==2)o.spatial=vdb::SdfSpatialScheme(99);
    if(invalid==3)o.temporal=vdb::SdfTemporalScheme(99);
    if(invalid==4)o.trim=vdb::SdfTrimMode(99);
    if(invalid==5)o.worldDistance=1;
    if(invalid==6)o.maskGrid="ignored";
    if(invalid==7)o.maxWorkItems=1;
    if(invalid==8)p.maxVoxelCount=512;
    if(invalid==9)p.maxInputBytes=1024;
    if(invalid==10){op=vdb::SdfFilterOperation::ErodeBand;o.bandLayers=3;}
    EXPECT_THROW((void)vdb::WriteSdfFiltered(path("source.vdb"),path("bad.vdb"),op,o,{},p),std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(path("bad.vdb")));
  }
  EXPECT_THROW((void)vdb::WriteSdfFiltered(path("source.vdb"),path("source.vdb"),vdb::SdfFilterOperation::Mean),std::invalid_argument);
}
TEST_F(SdfFilters, SdkDoubleSphereBandsResizeAtNonbinaryVoxelSizeWithoutOffByOneLayers) {
  const auto source=openvdb::tools::createLevelSetSphere<openvdb::DoubleGrid>(1.,openvdb::Vec3f(0),.1,3.);
  write(path("source.vdb"),source);const auto h=source->voxelSize()[0];
  vdb::SdfFilterOptions o;o.bandWidth=4;
  const auto result=vdb::WriteSdfFiltered(path("source.vdb"),path("result.vdb"),vdb::SdfFilterOperation::ResizeBand,o);
  EXPECT_DOUBLE_EQ(result.halfWidthVoxels,4.);
  EXPECT_EQ(read<openvdb::DoubleGrid>(path("result.vdb"))->background(),4*h);
}
TEST_F(SdfFilters, EmptyFieldsRetainNullQualityAndBandOperationsChangeOnlySignedWidths) {
  auto source=openvdb::DoubleGrid::create(.75);source->setGridClass(openvdb::GRID_LEVEL_SET);
  source->setTransform(openvdb::math::Transform::createLinearTransform(.25));write(path("empty.vdb"),source);
  vdb::SdfFilterOptions o;o.bandWidth=4;
  const auto result=vdb::WriteSdfFiltered(path("empty.vdb"),path("result.vdb"),vdb::SdfFilterOperation::ResizeBand,o);
  EXPECT_EQ(result.grid.activeVoxelCount,0u);EXPECT_EQ(result.gradientSampleCount,0u);EXPECT_FALSE(result.meanGradientError);
  EXPECT_EQ(result.halfWidthVoxels,4.);EXPECT_EQ(read<openvdb::DoubleGrid>(path("result.vdb"))->background(),1.);
}
TEST_F(SdfFilters, AlphaSelectionSkipsIntegersAndRejectsNonfiniteMapsAndIntervalsEvenForEmptyFields) {
  const auto source=sphere<openvdb::DoubleGrid>();write(path("source.vdb"),source);
  auto mask=openvdb::FloatGrid::create(.5f);mask->setName("z");mask->setTransform(std::make_shared<openvdb::math::Transform>(source->transform()));
  auto integer=openvdb::Int32Grid::create();integer->setName("a");
  {openvdb::io::File f(path("mixed.vdb").string());f.write(openvdb::GridPtrVec{integer,mask});}
  vdb::SdfFilterOptions o;o.maskInput=path("mixed.vdb");
  EXPECT_NO_THROW((void)vdb::WriteSdfFiltered(path("source.vdb"),path("result.vdb"),vdb::SdfFilterOperation::Mean,o));
  o.maskGrid="a";
  EXPECT_THROW((void)vdb::WriteSdfFiltered(path("source.vdb"),path("bad.vdb"),vdb::SdfFilterOperation::Mean,o),std::invalid_argument);
  for(int invalid=0;invalid<5;++invalid) {
    mask=openvdb::FloatGrid::create(invalid==0?std::numeric_limits<float>::quiet_NaN():.5f);
    mask->setTransform(std::make_shared<openvdb::math::Transform>(source->transform()));
    if(invalid==1)mask->tree().setValueOff({0,0,0},std::numeric_limits<float>::infinity());
    if(invalid==2)mask->transform().postTranslate({.25,0,0});
    write(path("mask.vdb"),mask);o={};o.maskInput=path("mask.vdb");
    if(invalid==3)o.alphaMaximum=std::numeric_limits<double>::denorm_min();
    if(invalid==4)o.alphaMaximum=std::numeric_limits<float>::denorm_min();
    EXPECT_THROW((void)vdb::WriteSdfFiltered(path("source.vdb"),path("bad.vdb"),vdb::SdfFilterOperation::Mean,o),std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(path("bad.vdb")));
  }
  auto empty=openvdb::DoubleGrid::create(.75);empty->setGridClass(openvdb::GRID_LEVEL_SET);
  empty->setTransform(std::make_shared<openvdb::math::Transform>(source->transform()));write(path("empty.vdb"),empty);
  mask=openvdb::FloatGrid::create(std::numeric_limits<float>::quiet_NaN());
  mask->setTransform(std::make_shared<openvdb::math::Transform>(source->transform()));write(path("mask.vdb"),mask);
  o={};o.maskInput=path("mask.vdb");
  EXPECT_THROW((void)vdb::WriteSdfFiltered(path("empty.vdb"),path("bad.vdb"),vdb::SdfFilterOperation::Mean,o),std::invalid_argument);
}
TEST_F(SdfFilters, UnicodeParentsSnapshotsAndFileMetadataSurviveExclusivePublication) {
  write(path("source.vdb"),sphere<openvdb::DoubleGrid>());
  const auto parent=directory/std::filesystem::path(std::u8string(u8"filter-\u6e2c\u8a66-\u00e9"));
  std::filesystem::create_directory(parent);std::filesystem::copy_file(path("source.vdb"),parent/"source.vdb");
  (void)vdb::WriteSdfFiltered(parent/"source.vdb",parent/"result.vdb",vdb::SdfFilterOperation::MeanCurvature);
  std::filesystem::copy_file(parent/"result.vdb",path("readback.vdb"));
  openvdb::io::File f(path("readback.vdb").string());f.open(false);
  EXPECT_EQ(f.getMetadata()->metaValue<std::string>("owner"),"filter");f.close();
  const auto size=std::filesystem::file_size(parent/"result.vdb");
  EXPECT_THROW((void)vdb::WriteSdfFiltered(parent/"source.vdb",parent/"result.vdb",vdb::SdfFilterOperation::MeanCurvature),std::invalid_argument);
  EXPECT_EQ(std::filesystem::file_size(parent/"result.vdb"),size);
}
TEST_F(SdfFilters, LargeSignedIndexTranslationsMatchIndependentLocalLatticeReference) {
  const auto source=sphere<openvdb::DoubleGrid>();write(path("source.vdb"),source);
  (void)vdb::WriteSdfFiltered(path("source.vdb"),path("reference.vdb"),vdb::SdfFilterOperation::MeanCurvature);
  const auto reference=read<openvdb::DoubleGrid>(path("reference.vdb"));
  for(int delta:{-100000000,100000000}) {
    auto translated=openvdb::DoubleGrid::create(source->background());
    translated->setGridClass(openvdb::GRID_LEVEL_SET);
    translated->setTransform(std::make_shared<openvdb::math::Transform>(source->transform()));
    for(auto value=source->cbeginValueAll();value;++value) {
      if(value.isValueOn() || value.isVoxelValue() || *value==source->background())continue;
      auto box=value.getBoundingBox();box.translate({delta,0,0});translated->tree().sparseFill(box,*value,false);
    }
    for(auto value=source->cbeginValueAll();value;++value) {
      if(!value.isVoxelValue() || (!value.isValueOn() && *value==source->background()))continue;
      auto coordinate=value.getCoord();coordinate[0]+=delta;
      if(value.isValueOn())translated->tree().setValue(coordinate,*value);
      else translated->tree().setValueOff(coordinate,*value);
    }
    write(path("translated.vdb"),translated);
    (void)vdb::WriteSdfFiltered(path("translated.vdb"),path("result.vdb"),vdb::SdfFilterOperation::MeanCurvature);
    const auto result=read<openvdb::DoubleGrid>(path("result.vdb"));
    EXPECT_EQ(result->activeVoxelCount(),reference->activeVoxelCount());
    for(auto value=result->cbeginValueOn();value;++value) {
      auto coordinate=value.getCoord();coordinate[0]-=delta;
      EXPECT_EQ(*value,reference->tree().getValue(coordinate));
      EXPECT_EQ(value.isValueOn(),reference->tree().isValueOn(coordinate));
    }
    std::filesystem::remove(path("result.vdb"));
  }
}
TEST_F(SdfFilters, ActivityMaskedNormalizationPreservesEveryUnselectedWorldValueBit) {
  auto source=openvdb::tools::createLevelSetSphere<openvdb::DoubleGrid>(1.,openvdb::Vec3f(0),.1,3.);
  for(auto value=source->beginValueOn();value;++value)value.setValue(*value*1.25);
  source->tree().setValue({-9,0,0},-0.);source->tree().setValue({-8,0,0},1.125e-310);
  write(path("source.vdb"),source);
  auto mask=openvdb::BoolGrid::create(false);
  mask->setTransform(std::make_shared<openvdb::math::Transform>(source->transform()));
  // Active false is selected. Inactive true on the other side is ignored.
  mask->tree().sparseFill(openvdb::CoordBBox({0,-32,-32},{32,32,32}),false,true);
  mask->tree().setValueOff({-9,0,0},true);write(path("activity.vdb"),mask);
  vdb::SdfFilterOptions o;o.normalizationMaskInput=path("activity.vdb");
  const auto report=vdb::WriteSdfFiltered(path("source.vdb"),path("result.vdb"),vdb::SdfFilterOperation::Normalize,o);
  ASSERT_TRUE(report.normalizationSelectedVoxels);EXPECT_GT(*report.normalizationSelectedVoxels,0u);
  const auto result=read<openvdb::DoubleGrid>(path("result.vdb"));
  EXPECT_EQ(result->activeVoxelCount(),source->activeVoxelCount());EXPECT_EQ(result->background(),source->background());
  uint64_t selected=0,changed=0;
  for(auto value=source->cbeginValueAll();value;++value) {
    const bool chosen=value.isValueOn() && mask->tree().isValueOn(value.getCoord());
    const auto actual=result->tree().getValue(value.getCoord());
    if(chosen) {++selected;changed+=std::bit_cast<uint64_t>(actual)!=std::bit_cast<uint64_t>(double(*value));}
    else {
      EXPECT_EQ(std::bit_cast<uint64_t>(actual),std::bit_cast<uint64_t>(double(*value)));
      EXPECT_EQ(result->tree().isValueOn(value.getCoord()),value.isValueOn());
    }
  }
  EXPECT_EQ(*report.normalizationSelectedVoxels,selected);EXPECT_GT(changed,0u);
  o.normalizationCount=0;
  const auto zero=vdb::WriteSdfFiltered(path("source.vdb"),path("zero.vdb"),vdb::SdfFilterOperation::Normalize,o);
  EXPECT_EQ(zero.normalizationSelectedVoxels,selected);
  const auto unchanged=read<openvdb::DoubleGrid>(path("zero.vdb"));
  for(auto value=source->cbeginValueAll();value;++value)
    EXPECT_EQ(std::bit_cast<uint64_t>(unchanged->tree().getValue(value.getCoord())),std::bit_cast<uint64_t>(double(*value)));
}
TEST_F(SdfFilters, NormalizationMasksIgnoreNaNPayloadsAndEmptySelectionsCopyExactValues) {
  const auto source=sphere<openvdb::DoubleGrid>();write(path("source.vdb"),source);
  auto mask=openvdb::Vec3DGrid::create(openvdb::Vec3d(std::numeric_limits<double>::quiet_NaN()));
  mask->setTransform(std::make_shared<openvdb::math::Transform>(source->transform()));
  mask->tree().setValue({4,0,0},openvdb::Vec3d(std::numeric_limits<double>::quiet_NaN()));
  write(path("nan.vdb"),mask);
  vdb::SdfFilterOptions o;o.normalizationMaskInput=path("nan.vdb");
  const auto selected=vdb::WriteSdfFiltered(path("source.vdb"),path("selected.vdb"),vdb::SdfFilterOperation::Normalize,o);
  EXPECT_EQ(selected.normalizationSelectedVoxels,1u);
  mask->tree().clear();write(path("empty-mask.vdb"),mask);o.normalizationMaskInput=path("empty-mask.vdb");
  const auto empty=vdb::WriteSdfFiltered(path("source.vdb"),path("empty.vdb"),vdb::SdfFilterOperation::Normalize,o);
  EXPECT_EQ(empty.normalizationSelectedVoxels,0u);
  const auto result=read<openvdb::DoubleGrid>(path("empty.vdb"));
  for(auto value=source->cbeginValueAll();value;++value)
    EXPECT_EQ(std::bit_cast<uint64_t>(result->tree().getValue(value.getCoord())),std::bit_cast<uint64_t>(double(*value)));
}
TEST_F(SdfFilters, NormalizationMaskModesMapsAndSourceSelectionAreStrict) {
  const auto source=sphere<openvdb::FloatGrid>();write(path("source.vdb"),source);
  auto mask=openvdb::MaskGrid::create(false);mask->setName("activity");
  mask->setTransform(std::make_shared<openvdb::math::Transform>(source->transform()));
  mask->tree().setValueOn({4,0,0});write(path("mask.vdb"),mask);
  vdb::SdfFilterOptions o;o.normalizationMaskInput=path("mask.vdb");o.normalizationMaskGrid="activity";
  const auto result=vdb::WriteSdfFiltered(path("source.vdb"),path("result.vdb"),vdb::SdfFilterOperation::Normalize,o);
  EXPECT_EQ(result.normalizationSelectedVoxels,1u);
  EXPECT_THROW((void)vdb::WriteSdfFiltered(path("source.vdb"),path("bad.vdb"),vdb::SdfFilterOperation::Mean,o),std::invalid_argument);
  o.normalizationMaskGrid="missing";
  EXPECT_THROW((void)vdb::WriteSdfFiltered(path("source.vdb"),path("bad.vdb"),vdb::SdfFilterOperation::Normalize,o),std::invalid_argument);
  o.normalizationMaskGrid="activity";mask->transform().postTranslate({.25,0,0});write(path("wrong-map.vdb"),mask);
  o.normalizationMaskInput=path("wrong-map.vdb");
  EXPECT_THROW((void)vdb::WriteSdfFiltered(path("source.vdb"),path("bad.vdb"),vdb::SdfFilterOperation::Normalize,o),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("bad.vdb")));
}
TEST_F(SdfFilters, AllNineNativeNormalizationMaskTypesSelectActiveZeroOrFalse) {
  const auto source=sphere<openvdb::FloatGrid>();write(path("source.vdb"),source);
  const auto make=[]<class Grid>() -> openvdb::GridBase::Ptr {
    auto result=Grid::create();result->tree().setValueOn({4,0,0});return result;
  };
  const std::vector<openvdb::GridBase::Ptr> masks{
    make.operator()<openvdb::BoolGrid>(),make.operator()<openvdb::MaskGrid>(),
    make.operator()<openvdb::Int32Grid>(),make.operator()<openvdb::Int64Grid>(),
    make.operator()<openvdb::FloatGrid>(),make.operator()<openvdb::DoubleGrid>(),
    make.operator()<openvdb::Vec3IGrid>(),make.operator()<openvdb::Vec3SGrid>(),make.operator()<openvdb::Vec3DGrid>()};
  for(const auto& mask:masks) {
    SCOPED_TRACE(mask->type());
    mask->setTransform(std::make_shared<openvdb::math::Transform>(source->transform()));write(path("mask.vdb"),mask);
    vdb::SdfFilterOptions o;o.normalizationMaskInput=path("mask.vdb");
    const auto result=vdb::WriteSdfFiltered(path("source.vdb"),path("result.vdb"),vdb::SdfFilterOperation::Normalize,o);
    EXPECT_EQ(result.normalizationSelectedVoxels,1u);EXPECT_EQ(result.grid.activeVoxelCount,source->activeVoxelCount());
    std::filesystem::remove(path("result.vdb"));
  }
}
TEST_F(SdfFilters, UnitConditioningSupportsSmallAndLargePhysicalVoxelSizes) {
  const auto original=sphere<openvdb::DoubleGrid>();
  for(double h:{1e-6,1e6}) {
    auto source=openvdb::DoubleGrid::create(3*h);source->setGridClass(openvdb::GRID_LEVEL_SET);
    if(h<1.5e-5) {
      // The scalar factory rejects a small absolute determinant, although its
      // persisted UniformScaleMap has finite, well-conditioned derivatives.
      // Construct that stock map through the public SDK serialization API.
      std::stringstream record(std::ios::in|std::ios::out|std::ios::binary);
      openvdb::writeString(record,openvdb::math::UniformScaleMap::mapType());
      for(const double value:{h,h,1/h,(1/h)*(1/h),.5/h})openvdb::Vec3d(value).write(record);
      record.seekg(0);openvdb::io::setCurrentVersion(record);source->transform().read(record);
    } else source->setTransform(openvdb::math::Transform::createLinearTransform(h));
    for(auto value=original->cbeginValueAll();value;++value) {
      if(value.isVoxelValue() || (!value.isValueOn() && *value==original->background()))continue;
      source->tree().sparseFill(value.getBoundingBox(),double(*value)*4*h,value.isValueOn());
    }
    for(auto value=original->cbeginValueAll();value;++value) {
      if(!value.isVoxelValue() || (!value.isValueOn() && *value==original->background()))continue;
      if(value.isValueOn())source->tree().setValue(value.getCoord(),double(*value)*4*h);
      else source->tree().setValueOff(value.getCoord(),double(*value)*4*h);
    }
    write(path("source.vdb"),source);
    const auto report=vdb::WriteSdfFiltered(path("source.vdb"),path("result.vdb"),vdb::SdfFilterOperation::MeanCurvature);
    EXPECT_EQ(report.grid.voxelSize,h);ASSERT_TRUE(report.meanGradientError);EXPECT_LT(*report.meanGradientError,.2);
    const auto inspection=vdb::InspectFile(path("result.vdb"));
    ASSERT_TRUE(inspection.grids[0].indexToWorld)<<inspection.grids[0].transformDiagnostic;
    EXPECT_EQ((*inspection.grids[0].indexToWorld)[0],h);
    EXPECT_GT(read<openvdb::DoubleGrid>(path("result.vdb"))->tree().getValue({4,0,0}),0);
    std::filesystem::remove(path("result.vdb"));
  }
}
TEST_F(SdfFilters, DoubleIntermediateScalingUnderflowIsRejectedBeforePublication) {
  auto source=openvdb::DoubleGrid::create(3e6);source->setGridClass(openvdb::GRID_LEVEL_SET);
  source->setTransform(openvdb::math::Transform::createLinearTransform(1e6));
  source->tree().setValue({0,0,0},1e-320);write(path("source.vdb"),source);
  vdb::SdfFilterOptions o;o.normalizationCount=0;
  try {
    (void)vdb::WriteSdfFiltered(path("source.vdb"),path("conditioned.vdb"),vdb::SdfFilterOperation::Mean,o);
    FAIL()<<"nonzero Double distance disappeared during unit conditioning";
  } catch(const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("underflows unit conditioning"),std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(path("conditioned.vdb")));

  source=openvdb::DoubleGrid::create(3e-6);source->setGridClass(openvdb::GRID_LEVEL_SET);
  std::stringstream record(std::ios::in|std::ios::out|std::ios::binary);
  openvdb::writeString(record,openvdb::math::UniformScaleMap::mapType());
  for(double value:{1e-6,1e-6,1e6,1e12,5e5})openvdb::Vec3d(value).write(record);
  record.seekg(0);openvdb::io::setCurrentVersion(record);source->transform().read(record);
  source->tree().fill(openvdb::CoordBBox({-4,-4,-4},{4,4,4}),0.,true);
  source->tree().setValue({0,0,0},std::numeric_limits<double>::denorm_min());write(path("tiny.vdb"),source);
  o.trim=vdb::SdfTrimMode::None;
  try {
    (void)vdb::WriteSdfFiltered(path("tiny.vdb"),path("restored.vdb"),vdb::SdfFilterOperation::Mean,o);
    FAIL()<<"nonzero averaged Double distance disappeared during world restoration";
  } catch(const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("underflows world units"),std::string::npos);
  }
  EXPECT_FALSE(std::filesystem::exists(path("restored.vdb")));
}
TEST_F(SdfFilters, ActivityMaskedNormalizationSchemesRetainUnselectedNativeBits) {
  const auto source=sphere<openvdb::FloatGrid>();
  for(auto value=source->beginValueOn();value;++value)value.setValue(*value*1.25f);
  write(path("source.vdb"),source);
  auto mask=openvdb::MaskGrid::create(false);
  mask->setTransform(std::make_shared<openvdb::math::Transform>(source->transform()));
  for(auto value=source->cbeginValueOn();value;++value)if(value.getCoord().x()>=0)mask->tree().setValueOn(value.getCoord());
  write(path("mask.vdb"),mask);
  for(int spatial=0;spatial<5;++spatial)for(int temporal=0;temporal<3;++temporal) {
    vdb::SdfFilterOptions o;o.normalizationMaskInput=path("mask.vdb");
    o.spatial=vdb::SdfSpatialScheme(spatial);o.temporal=vdb::SdfTemporalScheme(temporal);
    const auto report=vdb::WriteSdfFiltered(path("source.vdb"),path("result.vdb"),vdb::SdfFilterOperation::Normalize,o);
    EXPECT_EQ(report.normalizationSelectedVoxels,mask->activeVoxelCount());
    const auto result=read<openvdb::FloatGrid>(path("result.vdb"));
    for(auto value=source->cbeginValueOn();value;++value)if(value.getCoord().x()<0)
      EXPECT_EQ(std::bit_cast<uint32_t>(result->tree().getValue(value.getCoord())),std::bit_cast<uint32_t>(float(*value)));
    std::filesystem::remove(path("result.vdb"));
  }
}
#endif
} // namespace
