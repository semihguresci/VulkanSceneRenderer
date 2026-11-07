#include "Container/geometry/OpenVdbFluidSequence.h"
#include "Container/geometry/OpenVdbLoader.h"

#include <gtest/gtest.h>
#include <bit>
#include <chrono>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#endif

namespace {
namespace vdb=container::geometry::vdb;

TEST(OpenVdbFluidSequenceAvailability, DisabledFailsBeforeInputsAndCallbacks) {
  if (vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB enabled";
  bool called=false; vdb::FluidSequenceOptions sequence;
  sequence.frameCount=0; sequence.stopRequested=[&]{called=true; return true;};
  try { (void)vdb::WriteFluidSequence({}, {}, sequence); FAIL(); }
  catch (const std::runtime_error& error) { EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"),std::string::npos); }
  EXPECT_FALSE(called);
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbFluidSequence : public ::testing::Test {
protected:
  std::filesystem::path directory=std::filesystem::temp_directory_path()/("container-fluid-sequence-test-"+
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  vdb::FluidSimulationInputs inputs;
  vdb::FluidSimulationOptions simulation;
  void SetUp() override {
    openvdb::initialize(); std::filesystem::create_directory(directory);
    auto domain=openvdb::BoolGrid::create(false); domain->setName("cells");
    auto density=openvdb::DoubleGrid::create(0); density->setName("rho"); density->setGridClass(openvdb::GRID_FOG_VOLUME);
    auto velocity=openvdb::Vec3DGrid::create(openvdb::Vec3d(0)); velocity->setName("world");
    velocity->setIsInWorldSpace(true); velocity->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
    for (int x=0;x<4;++x) for (int y=0;y<4;++y) for (int z=0;z<2;++z) {
      const openvdb::Coord c(x,y,z); domain->tree().setValue(c,true);
      density->tree().setValue(c,.2+.05*x);
      velocity->tree().setValue(c,openvdb::Vec3d(-.02*(y-1.5),.02*(x-1.5),0));
    }
    openvdb::io::File source((directory/"source.vdb").string()); source.write({domain,density,velocity}); source.close();
    vdb::FluidProjectionOptions projection; projection.velocityGridName="u"; projection.domainGridName="wall";
    (void)vdb::WriteProjectedVelocity(directory/"source.vdb",directory/"source.vdb",directory/"initial.vdb",
        projection,"world","cells");
    inputs={.density=directory/"source.vdb",.velocity=directory/"initial.vdb",.domain=directory/"initial.vdb",
        .densityGrid="rho",.velocityGrid="u",.domainGrid="wall"};
    simulation.timeStep=.05; simulation.substeps=2;
    simulation.densityGridName="smoke"; simulation.temperatureGridName="kelvin";
    simulation.velocityGridName="wind"; simulation.pressureGridName="p"; simulation.domainGridName="fluid";
  }
  void TearDown() override {
    // Test-owned tree with a checked literal prefix. Production cleanup never
    // performs recursive removal.
    const auto resolved=std::filesystem::absolute(directory).lexically_normal();
    ASSERT_TRUE(std::filesystem::equivalent(resolved.parent_path(),std::filesystem::temp_directory_path()));
    ASSERT_TRUE(resolved.filename().string().starts_with("container-fluid-sequence-test-"));
    std::error_code ignored; std::filesystem::remove_all(resolved,ignored);
  }
  void noStaging() {
    for (const auto& entry:std::filesystem::directory_iterator(directory))
      EXPECT_FALSE(entry.path().filename().string().starts_with(".container-vdb-sequence-"));
  }
  openvdb::GridBase::Ptr read(const std::filesystem::path& path,const std::string& name) {
    openvdb::io::File file(path.string()); file.open(false); auto grid=file.readGrid(name); file.close(); return grid;
  }
};

TEST_F(OpenVdbFluidSequence, PublishesCustomNamedFramesManifestAndEquivalentRestart) {
  std::vector<uint32_t> progress; vdb::FluidSequenceOptions options; options.frameCount=2;
  options.progress=[&](uint32_t frame,uint32_t total){EXPECT_EQ(total,2u); progress.push_back(frame);};
  const auto sequence=vdb::WriteFluidSequence(inputs,directory/"sequence",options,simulation);
  ASSERT_EQ(sequence.frames.size(),2u); EXPECT_EQ(progress,(std::vector<uint32_t>{1,2}));
  EXPECT_DOUBLE_EQ(sequence.frames.back().time,.1); EXPECT_GT(sequence.encodedBytes,0u);
  std::ifstream stream(sequence.manifest); nlohmann::json manifest; stream>>manifest;
  EXPECT_EQ(manifest["grids"]["density"],"smoke"); EXPECT_EQ(manifest["initialFrameIncluded"],false);
  EXPECT_EQ(manifest["frames"][1]["file"],"frame_0002.vdb");
  const auto first=directory/"first.vdb", second=directory/"second.vdb";
  (void)vdb::WriteFluidStep(inputs,first,simulation);
  const vdb::FluidSimulationInputs restart{.density=first,.velocity=first,.domain=first,.temperature=first,
      .densityGrid="smoke",.velocityGrid="wind",.domainGrid="fluid",.temperatureGrid="kelvin"};
  const auto stepped=vdb::WriteFluidStep(restart,second,simulation);
  EXPECT_DOUBLE_EQ(sequence.frames.back().simulation.densityMassAfter,stepped.densityMassAfter);
  for (const auto* name:{"smoke","kelvin","p"}) {
    auto a=openvdb::gridPtrCast<openvdb::DoubleGrid>(read(sequence.frames.back().path,name));
    auto b=openvdb::gridPtrCast<openvdb::DoubleGrid>(read(second,name));
    EXPECT_EQ(a->activeVoxelCount(),b->activeVoxelCount());
    for (auto it=a->cbeginValueAll();it;++it) EXPECT_DOUBLE_EQ(*it,b->tree().getValue(it.getCoord()));
  }
  auto a=openvdb::gridPtrCast<openvdb::Vec3DGrid>(read(sequence.frames.back().path,"wind"));
  auto b=openvdb::gridPtrCast<openvdb::Vec3DGrid>(read(second,"wind"));
  for (auto it=a->cbeginValueAll();it;++it) EXPECT_EQ(*it,b->tree().getValue(it.getCoord()));
  noStaging();
}

TEST_F(OpenVdbFluidSequence, CancellationBeforeAndBetweenFramesCleansOwnedFiles) {
  vdb::FluidSequenceOptions options; options.frameCount=2; bool stop=true;
  options.stopRequested=[&]{return stop;};
  EXPECT_THROW((void)vdb::WriteFluidSequence(inputs,directory/"cancelled",options,simulation),std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(directory/"cancelled")); noStaging();
  stop=false; uint32_t completed=0;
  options.progress=[&](uint32_t frame,uint32_t){completed=frame; stop=true;};
  EXPECT_THROW((void)vdb::WriteFluidSequence(inputs,directory/"cancelled",options,simulation),std::runtime_error);
  EXPECT_EQ(completed,1u); EXPECT_FALSE(std::filesystem::exists(directory/"cancelled")); noStaging();
}

TEST_F(OpenVdbFluidSequence, ByteCapsCallbackErrorsAndLateDestinationPreserveSources) {
  const auto before=std::filesystem::file_size(directory/"source.vdb");
  vdb::FluidSequenceOptions options; options.frameCount=2; options.maxOutputBytes=1;
  EXPECT_THROW((void)vdb::WriteFluidSequence(inputs,directory/"limited",options,simulation),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(directory/"limited")); noStaging();
  options.maxOutputBytes=64*1024*1024;
  options.progress=[](uint32_t,uint32_t){throw std::runtime_error("callback failure");};
  EXPECT_THROW((void)vdb::WriteFluidSequence(inputs,directory/"callback",options,simulation),std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(directory/"callback")); noStaging();
  options.progress=[&](uint32_t frame,uint32_t total){if (frame==total) {
    std::filesystem::create_directory(directory/"appeared");
    std::ofstream(directory/"appeared"/"keep")<<"existing";
  }};
  EXPECT_THROW((void)vdb::WriteFluidSequence(inputs,directory/"appeared",options,simulation),std::exception);
  std::ifstream keep(directory/"appeared"/"keep"); std::string preserved; keep>>preserved;
  EXPECT_EQ(preserved,"existing"); EXPECT_EQ(std::filesystem::file_size(directory/"source.vdb"),before); noStaging();
}

TEST_F(OpenVdbFluidSequence, RejectsInvalidCountsPathsAndExistingDestination) {
  vdb::FluidSequenceOptions options; options.frameCount=0;
  EXPECT_THROW((void)vdb::WriteFluidSequence(inputs,directory/"bad",options,simulation),std::invalid_argument);
  options.frameCount=257;
  EXPECT_THROW((void)vdb::WriteFluidSequence(inputs,directory/"bad",options,simulation),std::invalid_argument);
  options.frameCount=1;
  EXPECT_THROW((void)vdb::WriteFluidSequence(inputs,directory/"missing"/"bad",options,simulation),std::invalid_argument);
  EXPECT_THROW((void)vdb::WriteFluidSequence(inputs,directory/"source.vdb",options,simulation),std::invalid_argument);
  noStaging();
}

TEST_F(OpenVdbFluidSequence, CoupledViscosityAndSourcesPropagateThroughFramesRestartAndManifestMetrics) {
  const auto readBytes=[](const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream),{});
  };
  const auto sourceBefore=readBytes(directory/"source.vdb");
  const auto initialBefore=readBytes(directory/"initial.vdb");
  simulation.kinematicViscosity=.15; simulation.ambientTemperature=305;
  simulation.buoyancyTemperature=.1;
  simulation.densityGridName="descriptor:smoke"; simulation.temperatureGridName="descriptor:kelvin";
  simulation.velocityGridName="descriptor:wind"; simulation.pressureGridName="descriptor:p";
  simulation.domainGridName="descriptor:fluid";
  vdb::FluidSourceSphere source; source.center={1,1,.5}; source.radius=.6;
  source.densityRate=2; source.densityDecayRate=.1;
  source.temperatureRate=3; source.temperatureRelaxationRate=.02;
  simulation.sources={source};
  vdb::FluidSequenceOptions options; options.frameCount=2;
  const auto sequence=vdb::WriteFluidSequence(inputs,directory/"coupled",options,simulation);
  ASSERT_EQ(sequence.frames.size(),2u);
  const auto firstPath=directory/"manual-first.vdb", secondPath=directory/"manual-second.vdb";
  const auto first=vdb::WriteFluidStep(inputs,firstPath,simulation);
  const vdb::FluidSimulationInputs restart{.density=firstPath,.velocity=firstPath,.domain=firstPath,.temperature=firstPath,
      .densityGrid="descriptor:"+simulation.densityGridName,.velocityGrid="descriptor:"+simulation.velocityGridName,
      .domainGrid="descriptor:"+simulation.domainGridName,.temperatureGrid="descriptor:"+simulation.temperatureGridName};
  const auto second=vdb::WriteFluidStep(restart,secondPath,simulation);
  for (size_t frame=0;frame<2;++frame) {
    const auto& measured=sequence.frames[frame].simulation;
    const auto& manual=frame==0 ? first : second;
    EXPECT_EQ(measured.actualSubsteps,2u); EXPECT_EQ(measured.viscositySubsteps,2u);
    EXPECT_EQ(measured.sourceSubsteps,2u); EXPECT_EQ(measured.sourceAffectedCellApplications,4u);
    EXPECT_GT(measured.viscosityEnergyDissipated,0);
    EXPECT_DOUBLE_EQ(measured.sourceDensityMassChange,manual.sourceDensityMassChange);
    EXPECT_DOUBLE_EQ(measured.sourceTemperatureIntegralChange,manual.sourceTemperatureIntegralChange);
    const auto expected=frame==0 ? firstPath : secondPath;
    for (const auto& name:{simulation.densityGridName,simulation.temperatureGridName,simulation.pressureGridName}) {
      const auto a=openvdb::gridPtrCast<openvdb::DoubleGrid>(read(sequence.frames[frame].path,name));
      const auto b=openvdb::gridPtrCast<openvdb::DoubleGrid>(read(expected,name));
      ASSERT_TRUE(a); ASSERT_TRUE(b); EXPECT_TRUE(a->tree().hasSameTopology(b->tree()));
      for (auto value=a->cbeginValueAll();value;++value)
        EXPECT_EQ(std::bit_cast<uint64_t>(*value),std::bit_cast<uint64_t>(b->tree().getValue(value.getCoord())));
    }
    const auto a=openvdb::gridPtrCast<openvdb::Vec3DGrid>(read(sequence.frames[frame].path,simulation.velocityGridName));
    const auto b=openvdb::gridPtrCast<openvdb::Vec3DGrid>(read(expected,simulation.velocityGridName));
    ASSERT_TRUE(a); ASSERT_TRUE(b); EXPECT_TRUE(a->tree().hasSameTopology(b->tree()));
    for (auto value=a->cbeginValueAll();value;++value)
      for (size_t axis=0;axis<3;++axis)
        EXPECT_EQ(std::bit_cast<uint64_t>((*value)[axis]),std::bit_cast<uint64_t>(b->tree().getValue(value.getCoord())[axis]));
    EXPECT_DOUBLE_EQ(a->metaValue<double>("container:projection_dt"),.025);
    const auto mask=openvdb::gridPtrCast<openvdb::BoolGrid>(read(sequence.frames[frame].path,simulation.domainGridName));
    const auto expectedMask=openvdb::gridPtrCast<openvdb::BoolGrid>(read(expected,simulation.domainGridName));
    ASSERT_TRUE(mask); ASSERT_TRUE(expectedMask);
    EXPECT_TRUE(mask->tree().hasSameTopology(expectedMask->tree()));
    for (auto value=mask->cbeginValueAll();value;++value)
      EXPECT_EQ(*value,expectedMask->tree().getValue(value.getCoord()));
  }
  std::ifstream stream(sequence.manifest); nlohmann::json manifest; stream>>manifest;
  EXPECT_EQ(manifest["grids"]["density"],simulation.densityGridName);
  EXPECT_EQ(manifest["grids"]["temperature"],simulation.temperatureGridName);
  EXPECT_EQ(manifest["grids"]["velocity"],simulation.velocityGridName);
  EXPECT_EQ(manifest["grids"]["pressure"],simulation.pressureGridName);
  EXPECT_EQ(manifest["grids"]["domain"],simulation.domainGridName);
  EXPECT_DOUBLE_EQ(manifest["kinematicViscosity"].get<double>(),simulation.kinematicViscosity);
  EXPECT_EQ(manifest["sourceSphereCount"],1);
  EXPECT_EQ(manifest["sourcesAffectBuoyancy"],"next actual substep");
  EXPECT_NE(manifest["operatorSplitting"].get<std::string>().find("scalar sources last"),std::string::npos);
  for (size_t frame=0;frame<2;++frame) {
    const auto& record=manifest["frames"][frame]; const auto& measured=sequence.frames[frame].simulation;
    EXPECT_EQ(record["viscositySubsteps"],measured.viscositySubsteps);
    EXPECT_EQ(record["sourceSubsteps"],measured.sourceSubsteps);
    EXPECT_EQ(record["maxDiffusionIterations"],measured.maxDiffusionIterations);
    EXPECT_EQ(record["sourceAffectedCellApplications"],measured.sourceAffectedCellApplications);
    EXPECT_EQ(record["sourceChangedCellApplications"],measured.sourceChangedCellApplications);
    EXPECT_DOUBLE_EQ(record["maxDiffusionResidual"].get<double>(),measured.maxDiffusionResidual);
    EXPECT_DOUBLE_EQ(record["viscosityEnergyDissipated"].get<double>(),measured.viscosityEnergyDissipated);
    EXPECT_DOUBLE_EQ(record["sourceDensityMassChange"].get<double>(),measured.sourceDensityMassChange);
    EXPECT_DOUBLE_EQ(record["sourceTemperatureIntegralChange"].get<double>(),measured.sourceTemperatureIntegralChange);
  }
  EXPECT_EQ(readBytes(directory/"source.vdb"),sourceBefore);
  EXPECT_EQ(readBytes(directory/"initial.vdb"),initialBefore); noStaging();
}

TEST_F(OpenVdbFluidSequence, CancellingOrFailingAComposedSequenceCleansFramesAndNestedPrimitiveTemporaries) {
  const auto readBytes=[](const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream),{});
  };
  const auto before=readBytes(directory/"source.vdb");
  simulation.kinematicViscosity=.2;
  vdb::FluidSourceSphere source; source.center={1,1,.5}; source.radius=.6;
  source.densityRate=4; source.temperatureRate=5; simulation.sources={source};
  bool stop=false; uint32_t completed=0;
  vdb::FluidSequenceOptions options; options.frameCount=2;
  options.stopRequested=[&]{return stop;};
  options.progress=[&](uint32_t frame,uint32_t) {
    completed=frame;
    for (const auto& entry:std::filesystem::directory_iterator(directory))
      if (entry.path().filename().string().starts_with(".container-vdb-sequence-")) {
        const auto framePath=entry.path()/"frame_0001.vdb";
        const auto density=openvdb::gridPtrCast<openvdb::DoubleGrid>(read(framePath,"smoke"));
        const auto temperature=openvdb::gridPtrCast<openvdb::DoubleGrid>(read(framePath,"kelvin"));
        EXPECT_GT(density->tree().getValue({1,1,0}),.35);
        EXPECT_GT(temperature->tree().getValue({1,1,0}),simulation.ambientTemperature);
      }
    stop=true;
  };
  EXPECT_THROW((void)vdb::WriteFluidSequence(inputs,directory/"cancelled-composition",options,simulation),std::runtime_error);
  EXPECT_EQ(completed,1u); EXPECT_FALSE(std::filesystem::exists(directory/"cancelled-composition")); noStaging();
  options.stopRequested={}; options.progress={};
  simulation.sources[0].densityRate=0; simulation.sources[0].densityDecayRate=1e6;
  EXPECT_THROW((void)vdb::WriteFluidSequence(inputs,directory/"stiff-composition",options,simulation),std::exception);
  EXPECT_FALSE(std::filesystem::exists(directory/"stiff-composition")); noStaging();
  for (const auto& entry:std::filesystem::directory_iterator(directory))
    EXPECT_FALSE(entry.path().filename().string().starts_with(".container-")) << entry.path();
  EXPECT_EQ(readBytes(directory/"source.vdb"),before);
}
#endif
} // namespace
