#include "Container/utility/OpenVdbAuthoring.h"
#include "Container/geometry/OpenVdbLoader.h"
#include "Container/geometry/OpenVdbInspect.h"
#include "Container/geometry/OpenVdbPoints.h"
#include "Container/geometry/OpenVdbAdvection.h"
#include "Container/geometry/OpenVdbVectorProcessing.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <atomic>
#include <bit>
#include <algorithm>
#include <chrono>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <map>
#include <limits>
#include <set>
#include <stdexcept>
#include <thread>

namespace {
using namespace std::chrono_literals;
using container::ui::OpenVdbAuthoringPanel;
using container::ui::OpenVdbAuthoringRequest;
using container::ui::OpenVdbAuthoringResult;

bool settle(OpenVdbAuthoringPanel& panel) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (panel.busy() && std::chrono::steady_clock::now() < deadline) {
    panel.poll();
    if (panel.busy()) std::this_thread::sleep_for(1ms);
  }
  return !panel.busy();
}

struct AuthoringFiles {
  std::filesystem::path directory = std::filesystem::temp_directory_path() /
      ("container-vdb-authoring-flow-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  AuthoringFiles() {
    if (!std::filesystem::create_directory(directory)) throw std::runtime_error("authoring fixture directory already exists");
  }
  ~AuthoringFiles() {
    std::error_code ignored;
    if (std::filesystem::is_symlink(directory,ignored) || ignored) return;
    const auto target=std::filesystem::weakly_canonical(directory,ignored);
    if (ignored) return;
    const auto parent=std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(),ignored);
    if (!ignored && target.parent_path()==parent && target.filename()==directory.filename() &&
        target.filename().string().starts_with("container-vdb-authoring-flow-"))
      std::filesystem::remove_all(target,ignored);
  }
  std::string path(const char* name) const {
    const auto utf8 = (directory / name).u8string(); return {utf8.begin(), utf8.end()};
  }
};

container::geometry::vdb::BoundaryProjectionInputs boundaryAuthoringFixture(const AuthoringFiles& files,bool pressureDriven=false) {
  namespace vdb=container::geometry::vdb;
  vdb::DenseArrayVolume fluid;fluid.region={{0,0,0},{3,1,1}};fluid.gridName="fluid";
  fluid.values=std::vector<double>{1,1,1};fluid.background=0.;fluid.active={1,1,1};
  (void)vdb::WriteDenseVdb(files.directory/"fluid-raw.vdb",fluid);
  std::ofstream(files.directory/"fluid-meta.json") << R"({"version":1,"inputs":[{"path":"fluid-raw.vdb","edits":[{"selector":"fluid","metadata":[{"name":"artist","operation":"set","type":"string","value":"boundary artist"},{"name":"stable_id","operation":"set","type":"int64","value":9007199254740993}]}]}],"metadata":[{"name":"scene","operation":"set","type":"string","value":"boundary scene"}]})";
  (void)vdb::WriteAuthoredVdbSpec(files.directory/"fluid-meta.json",files.directory/"fluid.vdb");
  vdb::DenseArrayVolume exterior;exterior.gridName="outlets";exterior.background=0.;
  if(pressureDriven) {
    exterior.region={{-1,0,0},{5,1,1}};exterior.values=std::vector<double>{1,0,0,0,1};exterior.active={1,0,0,0,1};
  } else {exterior.region={{0,0,0},{4,1,1}};exterior.values=std::vector<double>{0,0,0,1};exterior.active={0,0,0,1};}
  (void)vdb::WriteDenseVdb(files.directory/"open.vdb",exterior);
  vdb::ProcessingOptions producer;producer.gridName="predictor";
  (void)vdb::WriteConstantVelocity(files.directory/"velocity.vdb",{0,0,0},producer);
  vdb::BoundaryProjectionInputs inputs;inputs.fluid={files.directory/"fluid.vdb","fluid"};
  inputs.velocity={files.directory/"velocity.vdb","predictor"};inputs.open={files.directory/"open.vdb","outlets"};
  if(pressureDriven) {
    vdb::DenseArrayVolume pD;pD.region={{-1,0,0},{5,1,1}};pD.gridName="pD";pD.background=0.;
    pD.values=std::vector<double>{2.5,1.5,.5,-.5,-1.5};pD.active.assign(5,1);
    (void)vdb::WriteDenseVdb(files.directory/"outside-pressure.vdb",pD);
    inputs.pressure={files.directory/"outside-pressure.vdb","pD"};
  }
  return inputs;
}

TEST(OpenVdbAuthoring, BoundaryProjectionSnapshotsGroupedInputsWritesMetadataAndReplaysCanonicalState) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::ProjectBoundaries;request.output=files.path("direct.vdb");
  auto& settings=request.boundaryProjection;settings.projection.timeStep=1;settings.projection.tolerance=1e-10;
  settings.wallVelocityWorld={1,0,0};settings.projection.velocityGridName="flow";settings.projection.pressureGridName="physical_pressure";
  settings.projection.domainGridName="domain";settings.faceKindGridName="kind";settings.faceFractionGridName="theta";
  settings.facePressureGridName="pD";settings.wallVelocityGridName="wall";
  const auto captured=settings;
  std::map<std::filesystem::path,std::string> originals;
  if(vdb::IsAvailable()) {
    request.boundaryInputs=boundaryAuthoringFixture(files);
    for(const auto& binding:{request.boundaryInputs.velocity,request.boundaryInputs.fluid,request.boundaryInputs.open}) {
      std::ifstream file(binding.file,std::ios::binary);originals[binding.file]=std::string(std::istreambuf_iterator<char>(file),{});
    }
  }
  ASSERT_TRUE(panel.submit(request));
  request.boundaryInputs.open.file=files.directory/"missing.vdb";request.boundaryInputs.fluid.grid="changed-after-submit";
  settings.wallVelocityWorld={99,0,0};settings.projection.timeStep=2;settings.maxWorkItems=1;settings.projection.velocityGridName="changed";
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()) {EXPECT_FALSE(panel.result()->success);return;}
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;EXPECT_FALSE(panel.result()->modelOutput);EXPECT_FALSE(panel.result()->restartGrids);
  EXPECT_NE(panel.result()->message.find("Boundary projection direct solve completed; converged: yes"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("Canonical boundary channels: kind, theta, pD, wall"),std::string::npos);
  const auto p=vdb::ReadDenseVdb(files.directory/"direct.vdb",{{0,0,0},{3,1,1}},"physical_pressure");
  ASSERT_TRUE(std::holds_alternative<std::vector<double>>(p.values));
  const auto pressure=std::get<std::vector<double>>(p.values);ASSERT_EQ(pressure.size(),3u);
  for(size_t i=0;i<3;++i)EXPECT_NEAR(pressure[i],2.5-double(i),1e-9);
  const auto u=vdb::ReadDenseVdb(files.directory/"direct.vdb",{{0,0,0},{4,1,1}},"flow");
  const auto velocity=std::get<std::vector<std::array<double,3>>>(u.values);
  for(const auto& value:velocity)EXPECT_NEAR(value[0],1,1e-9);
  EXPECT_EQ(u.gridClass,vdb::DenseGridClass::Staggered);EXPECT_FALSE(u.worldSpace);
  const auto inspected=vdb::InspectFile(files.directory/"direct.vdb");ASSERT_EQ(inspected.grids.size(),7u);
  std::set<std::string> names,memberIds;
  for(const auto& grid:inspected.grids) {
    names.insert(grid.name);
    const auto id=std::find_if(grid.metadata.begin(),grid.metadata.end(),[](const auto& field){return field.name=="container:boundary_projection_state_id";});
    ASSERT_NE(id,grid.metadata.end());memberIds.insert(id->value);
    if(grid.name=="domain" || grid.name=="physical_pressure") {
      const auto artist=std::find_if(grid.metadata.begin(),grid.metadata.end(),[](const auto& field){return field.name=="artist";});
      ASSERT_NE(artist,grid.metadata.end());EXPECT_EQ(artist->value,"boundary artist");
      const auto stable=std::find_if(grid.metadata.begin(),grid.metadata.end(),[](const auto& field){return field.name=="stable_id";});
      ASSERT_NE(stable,grid.metadata.end());EXPECT_EQ(stable->value,"9007199254740993");
    }
  }
  EXPECT_EQ(names,(std::set<std::string>{"flow","physical_pressure","domain","kind","theta","pD","wall"}));EXPECT_EQ(memberIds.size(),1u);
  request.operation=Operation::ReprojectBoundaries;request.output=files.path("replayed.vdb");request.boundaryProjection=captured;
  request.boundaryReplay={files.directory/"direct.vdb","flow","domain","kind","theta","pD","wall"};
  // Replay hides and discards geometric controls retained from a direct job.
  request.boundaryProjection.missing=vdb::MissingFluidBoundary::FreeSurfacePressure;
  request.boundaryProjection.airPressure=vdb::AirPressureLocation::LevelSetInterface;
  request.boundaryProjection.minimumInterfaceFraction=.5;request.boundaryProjection.wallVelocityWorld={99,0,0};request.boundaryProjection.ambientPressure=123;
  ASSERT_TRUE(panel.submit(request));request.boundaryReplay.velocityGrid="changed-after-submit";request.boundaryProjection.maxWorkItems=1;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_FALSE(panel.result()->modelOutput);EXPECT_NE(panel.result()->message.find("Boundary projection replay completed"),std::string::npos);
  const auto repeated=vdb::ReadDenseVdb(files.directory/"replayed.vdb",{{0,0,0},{4,1,1}},"flow");
  for(const auto& value:std::get<std::vector<std::array<double,3>>>(repeated.values))EXPECT_NEAR(value[0],1,1e-9);
  const auto repeatedPressure=vdb::ReadDenseVdb(files.directory/"replayed.vdb",{{0,0,0},{3,1,1}},"physical_pressure");
  for(const auto value:std::get<std::vector<double>>(repeatedPressure.values))EXPECT_NEAR(value,0,1e-9);
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);
  for(const auto& [source,bytes]:originals){std::ifstream file(source,std::ios::binary);EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file),{}),bytes);}
}

TEST(OpenVdbAuthoring, BoundaryPressureReplayAddsFreshImpulseAndCapsProtectOutput) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  if(!vdb::IsAvailable())GTEST_SKIP()<<"OpenVDB is disabled";
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::ProjectBoundaries;request.boundaryInputs=boundaryAuthoringFixture(files,true);
  request.output=files.path("driven.vdb");request.boundaryProjection.projection.timeStep=.25;request.boundaryProjection.projection.tolerance=1e-10;
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  const auto driven=vdb::ReadDenseVdb(files.directory/"driven.vdb",{{0,0,0},{4,1,1}},"velocity");
  for(const auto& value:std::get<std::vector<std::array<double,3>>>(driven.values))EXPECT_NEAR(value[0],.25,1e-9);
  const auto p=vdb::ReadDenseVdb(files.directory/"driven.vdb",{{0,0,0},{3,1,1}},"pressure");
  const auto values=std::get<std::vector<double>>(p.values);for(size_t i=0;i<values.size();++i)EXPECT_NEAR(values[i],1.5-double(i),1e-9);
  std::ifstream source(files.directory/"driven.vdb",std::ios::binary);const std::string original(std::istreambuf_iterator<char>(source),{});source.close();
  request.operation=Operation::ReprojectBoundaries;request.boundaryReplay.state=files.directory/"driven.vdb";request.output=files.path("second-impulse.vdb");
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_NE(panel.result()->message.find("fresh nonuniform pressure-driven impulse on every call"),std::string::npos);
  const auto twice=vdb::ReadDenseVdb(files.directory/"second-impulse.vdb",{{0,0,0},{4,1,1}},"velocity");
  for(const auto& value:std::get<std::vector<std::array<double,3>>>(twice.values))EXPECT_NEAR(value[0],.5,1e-9);
  request.output=files.path("capped.vdb");request.boundaryProjection.maxWorkItems=1;
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);EXPECT_FALSE(std::filesystem::exists(files.directory/"capped.vdb"));
  request.boundaryProjection.maxWorkItems=2'000'000'000;request.output=files.path("driven.vdb");
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);
  std::ifstream after(files.directory/"driven.vdb",std::ios::binary);EXPECT_EQ(std::string(std::istreambuf_iterator<char>(after),{}),original);
}

TEST(OpenVdbAuthoring, DefaultExecutorRunsPointScatterAdvectionAndAttributeWorkflow) {
  namespace vdb = container::geometry::vdb;
  using Operation = container::ui::OpenVdbAuthoringOperation;
  if (!vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB is disabled";
  AuthoringFiles files;
  OpenVdbAuthoringPanel panel;
  OpenVdbAuthoringRequest request;
  request.processing.voxelSize = .25;
  const auto run = [&] {
    if (!panel.submit(request) || !settle(panel) || !panel.result()) return false;
    EXPECT_TRUE(panel.result()->success) << panel.result()->message;
    return panel.result()->success;
  };
  request.output = files.path("sphere.vdb"); ASSERT_TRUE(run());
  request.operation = Operation::Fog; request.input = request.output;
  request.output = files.path("fog.vdb"); ASSERT_TRUE(run());
  request.operation = Operation::Scatter; request.input = request.output;
  request.output = files.path("points.vdb"); request.scatter.pointCount = 8;
  request.scatter.seed = 79; request.scatter.radius = .125; request.maxPoints = 8;
  ASSERT_TRUE(run()); EXPECT_TRUE(panel.result()->modelOutput);
  const auto before = vdb::ReadPoints(files.directory / "points.vdb"); ASSERT_EQ(before.size(), 8u);
  request.operation = Operation::Velocity; request.velocity = {1,0,0};
  request.output = files.path("wind.vdb"); ASSERT_TRUE(run());
  request.operation = Operation::AdvectPoints; request.input = files.path("points.vdb");
  request.secondInput = files.path("wind.vdb"); request.output = files.path("moved.vdb");
  request.pointAdvection.timeStep = .125; request.pointAdvection.integration = vdb::PointIntegration::RK4;
  request.pointAdvection.updateVelocity = false; ASSERT_TRUE(run());
  request.operation = Operation::SamplePointVelocity; request.input = request.output;
  request.output = files.path("sampled.vdb"); ASSERT_TRUE(run());
  EXPECT_TRUE(panel.result()->modelOutput);
  const auto after = vdb::ReadPoints(files.directory / "sampled.vdb"); ASSERT_EQ(after.size(), before.size());
  for (size_t index = 0; index < after.size(); ++index) {
    EXPECT_NEAR(after[index].position[0], before[index].position[0] + .125, 1e-6);
    EXPECT_NEAR(after[index].position[1], before[index].position[1], 1e-6);
    EXPECT_NEAR(after[index].position[2], before[index].position[2], 1e-6);
    EXPECT_EQ(after[index].velocity, (std::array<double,3>{1,0,0}));
    EXPECT_EQ(after[index].radius, .125);
  }
}

TEST(OpenVdbAuthoring, DefaultExecutorResamplesOrdinaryDoubleFieldWithImmutableFullOptions) {
  namespace vdb=container::geometry::vdb;
  using Operation=container::ui::OpenVdbAuthoringOperation;
  static_assert(static_cast<int>(Operation::Count)==101);
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::ResampleField;request.input=files.path("source.vdb");request.output=files.path("sampled.vdb");
  request.inputGrid="polynomial";request.processing.gridName="sampled";
  request.fieldResampling.interpolation=vdb::FieldResamplingInterpolation::Quadratic;
  request.fieldResampling.activity=vdb::FieldResamplingActivity::AnyContributingStencil;
  request.fieldResampling.vectorValues=vdb::FieldVectorTransform::Semantic;
  request.fieldResampling.targetIndexToWorld=std::array<double,16>{.5,.25,0,.25,0,.5,0,.25,0,0,-.5,.25,0,0,0,1};
  std::string original;
  if(vdb::IsAvailable()) {
    vdb::DenseArrayVolume volume;volume.region={{-2,-2,-2},{5,5,5}};
    volume.gridName="polynomial";volume.background=0.;
    std::vector<double> values;
    for(int z=-2;z<=2;++z)for(int y=-2;y<=2;++y)for(int x=-2;x<=2;++x)
      values.push_back(x*x+2*y*y+3*z*z+x*y);
    volume.values=std::move(values);volume.active.assign(125,1);
    (void)vdb::WriteDenseVdb(files.directory/"source.vdb",volume);
    std::ifstream source(files.directory/"source.vdb",std::ios::binary);original.assign(std::istreambuf_iterator<char>(source),{});
  }
  ASSERT_TRUE(panel.submit(request));
  request.fieldResampling.interpolation=vdb::FieldResamplingInterpolation::Nearest;
  request.fieldResampling.targetIndexToWorld->at(3)=.75;
  request.fieldResampling.maxWorkItems=1;
  request.fieldResampling.maxMetadataBytes=1;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()) {EXPECT_FALSE(panel.result()->success);return;}
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_FALSE(panel.result()->modelOutput);EXPECT_EQ(panel.result()->output,files.path("sampled.vdb"));
  EXPECT_NE(panel.result()->message.find("Single-pass center sampling"),std::string::npos);
  const auto sampled=vdb::ReadDenseVdb(files.directory/"sampled.vdb",{{0,0,0},{1,1,1}},"sampled");
  EXPECT_EQ(sampled.translation,(std::array<double,3>{.25,.25,.25}));
  EXPECT_EQ(sampled.linear,(std::array<double,9>{.5,.25,0,0,.5,0,0,0,-.5}));
  ASSERT_TRUE(std::holds_alternative<std::vector<double>>(sampled.values));
  EXPECT_DOUBLE_EQ(std::get<std::vector<double>>(sampled.values).at(0),.4375);
  EXPECT_EQ(sampled.active,std::vector<uint8_t>{1});
  std::ifstream source(files.directory/"source.vdb",std::ios::binary);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(source),{}),original);
  request.output=files.path("capped.vdb");
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  EXPECT_FALSE(panel.result()->success);EXPECT_FALSE(std::filesystem::exists(files.directory/"capped.vdb"));
}

TEST(OpenVdbAuthoring, DefaultExecutorResamplingSeparatesTargetMapFromVectorDeformationAndRawPolicy) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  if(!vdb::IsAvailable())GTEST_SKIP()<<"OpenVDB is disabled";
  AuthoringFiles files;
  vdb::DenseArrayVolume volume;volume.gridName="positions";
  volume.values=std::vector<std::array<double,3>>{{2,3,4}};volume.background=std::array<double,3>{2,3,4};volume.active={1};
  volume.vectorType=vdb::DenseVectorType::ContravariantAbsolute;
  (void)vdb::WriteDenseVdb(files.directory/"source.vdb",volume);
  OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::ResampleField;request.input=files.path("source.vdb");request.inputGrid="positions";
  request.processing.gridName="sampled";request.output=files.path("semantic.vdb");
  const std::array<double,16> deformation{2,0,0,5,0,-1,0,6,0,0,.5,7,0,0,0,1};
  request.fieldResampling.targetIndexToWorld=deformation;request.fieldResampling.worldFromWorld=deformation;
  ASSERT_TRUE(panel.submit(request));request.fieldResampling.vectorValues=vdb::FieldVectorTransform::PreserveWorldComponents;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_FALSE(panel.result()->modelOutput);
  const auto semantic=vdb::ReadDenseVdb(files.directory/"semantic.vdb",{{0,0,0},{1,1,1}},"sampled");
  EXPECT_EQ((std::get<std::vector<std::array<double,3>>>(semantic.values).at(0)),(std::array<double,3>{9,3,9}));
  EXPECT_EQ((std::get<std::array<double,3>>(semantic.background)),(std::array<double,3>{9,3,9}));
  EXPECT_EQ(semantic.vectorType,vdb::DenseVectorType::ContravariantAbsolute);
  request.output=files.path("raw.vdb");
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  const auto raw=vdb::ReadDenseVdb(files.directory/"raw.vdb",{{0,0,0},{1,1,1}},"sampled");
  EXPECT_EQ((std::get<std::vector<std::array<double,3>>>(raw.values).at(0)),(std::array<double,3>{2,3,4}));
  request.fieldResampling.vectorValues=vdb::FieldVectorTransform::Semantic;
  request.fieldResampling.worldFromWorld={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};request.output=files.path("target-only.vdb");
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  const auto targetOnly=vdb::ReadDenseVdb(files.directory/"target-only.vdb",{{0,0,0},{1,1,1}},"sampled");
  EXPECT_EQ((std::get<std::vector<std::array<double,3>>>(targetOnly.values).at(0)),(std::array<double,3>{2,3,4}));
}

TEST(OpenVdbAuthoring, DiagnosticReportsAndMasksSeparateCompletedJobsFromFailedChecks) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::Diagnose;request.input=files.path("source.vdb");request.inputGrid="field";
  request.output=files.path("stale-output.vdb");request.diagnostics.componentRange=std::array<double,2>{0,1};
  request.diagnostics.maxExamples=1;
  // Scalar profiles do not expose or apply point-only caps or predicates.
  request.maxPoints=0;request.pointAttributes.maxAttributes=0;request.pointAttributes.maxGroups=0;
  request.pointAttributes.maxEdits=0;request.pointAttributes.maxStringBytes=0;request.pointSelection.anyOf={"hidden-stale-group"};
  std::string original;
  if(vdb::IsAvailable()) {
    vdb::DenseArrayVolume volume;volume.region={{0,0,0},{2,1,1}};volume.gridName="field";
    volume.values=std::vector<double>{2,0};volume.background=0.;volume.active={1,1};
    (void)vdb::WriteDenseVdb(files.directory/"source.vdb",volume);
    std::ifstream source(files.directory/"source.vdb",std::ios::binary);original.assign(std::istreambuf_iterator<char>(source),{});
  }
  ASSERT_TRUE(panel.submit(request));request.diagnostics.componentRange=std::array<double,2>{-1,3};
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()) {EXPECT_FALSE(panel.result()->success);return;}
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_FALSE(panel.result()->modelOutput);EXPECT_TRUE(panel.result()->output.empty());
  EXPECT_FALSE(std::filesystem::exists(files.directory/"stale-output.vdb"));
  EXPECT_NE(panel.result()->message.find("all requested checks passed: NO; coverage complete"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("range: Fail; coverage complete"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("gradient: NotApplicable"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("Unique finite failure locations: 1"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("Example index[0,0,0]"),std::string::npos);
  request.operation=Operation::DiagnosticMask;request.output=files.path("mask.vdb");
  request.diagnostics.componentRange=std::array<double,2>{0,1};request.diagnostics.maskGridName="outliers";request.diagnostics.maxExamples=0;
  ASSERT_TRUE(panel.submit(request));request.diagnostics.maskGridName="changed-after-submit";request.diagnostics.maxWork=1;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_FALSE(panel.result()->modelOutput);EXPECT_EQ(panel.result()->output,files.path("mask.vdb"));
  EXPECT_NE(panel.result()->message.find("Wrote Bool union failure mask outliers: 1"),std::string::npos);
  EXPECT_EQ(panel.result()->message.find("Example"),std::string::npos);
  const auto mask=vdb::InspectFile(files.directory/"mask.vdb");ASSERT_EQ(mask.grids.size(),1u);
  EXPECT_EQ(mask.grids[0].name,"outliers");EXPECT_EQ(mask.grids[0].valueType,"bool");EXPECT_EQ(mask.grids[0].activeVoxelCount,1u);
  std::ifstream source(files.directory/"source.vdb",std::ios::binary);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(source),{}),original);
  request.output=files.path("capped.vdb");
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);
  EXPECT_FALSE(std::filesystem::exists(files.directory/"capped.vdb"));
  request.diagnostics={};request.operation=Operation::Diagnose;request.input=files.path("fog.vdb");
  request.diagnostics.profile=vdb::DiagnosticProfile::NormalizedFog;
  vdb::DenseArrayVolume fog;fog.gridName="field";fog.values=std::vector<double>{1.25};fog.background=0.;fog.active={1};fog.gridClass=vdb::DenseGridClass::FogVolume;
  (void)vdb::WriteDenseVdb(files.directory/"fog.vdb",fog);
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_TRUE(panel.result()->output.empty());EXPECT_NE(panel.result()->message.find("fog-active-range: Fail"),std::string::npos);
}

TEST(OpenVdbAuthoring, DiagnosticsKeepUnlocatedFailuresAndMissingGradientCoverageExplicit) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  if(!vdb::IsAvailable())GTEST_SKIP()<<"OpenVDB is disabled";
  AuthoringFiles files;vdb::ProcessingOptions processing;processing.gridName="constant";processing.voxelSize=.25;
  (void)vdb::WriteConstantVelocity(files.directory/"background.vdb",{2,3,4},processing);
  OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;request.operation=Operation::DiagnosticMask;
  request.input=files.path("background.vdb");request.inputGrid="constant";request.output=files.path("empty-mask.vdb");
  request.diagnostics.componentRange=std::array<double,2>{0,1};request.diagnostics.rangeScope=vdb::DiagnosticScope::Stored;
  request.diagnostics.maskGridName="global_failures";
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_FALSE(panel.result()->modelOutput);EXPECT_NE(panel.result()->message.find("unlocated failures: YES"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("Unique finite failure locations: 0"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("Example (unlocated/global): background outside requested range"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("an empty mask does NOT mean the report passed"),std::string::npos);
  const auto empty=vdb::InspectFile(files.directory/"empty-mask.vdb");ASSERT_EQ(empty.grids.size(),1u);EXPECT_EQ(empty.grids[0].activeVoxelCount,0u);
  processing.gridName="surface";(void)vdb::WriteSphere(files.directory/"sphere.vdb",{},processing);
  request.operation=Operation::Diagnose;request.input=files.path("sphere.vdb");request.inputGrid="surface";request.diagnostics={};
  request.diagnostics.profile=vdb::DiagnosticProfile::SymmetricLevelSet;request.diagnostics.gradient=vdb::DiagnosticGradient::FirstOrder;
  request.diagnostics.gradientNormRange={0,100};
  ASSERT_TRUE(panel.submit(request));request.diagnostics.footprint=vdb::DiagnosticFootprint::NativeStoredContinuation;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_TRUE(panel.result()->output.empty());EXPECT_NE(panel.result()->message.find("gradient: Unavailable; coverage INCOMPLETE"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("coverage INCOMPLETE"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("Footprint candidates/evaluated/missing/nonfinite/stored-continuations"),std::string::npos);
  request.diagnostics.gradient=vdb::DiagnosticGradient::WenoInterface;
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_NE(panel.result()->message.find("gradient: Pass; coverage complete"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("no distance, closure or validity certificate is implied"),std::string::npos);
}

TEST(OpenVdbAuthoring, PointDiagnosticsSnapshotGroupsColumnsAndExactInt64Range) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  if(!vdb::IsAvailable())GTEST_SKIP()<<"OpenVDB is disabled";
  AuthoringFiles files;vdb::ProcessingOptions processing;processing.gridName="points";
  const std::array<vdb::PointRecord,2> points{{{{0,0,0},{},1},{{1,0,0},{},1}}};
  (void)vdb::WritePoints(points,files.directory/"base.vdb",processing);
  vdb::PointAttributeEdit ids;ids.name="id";ids.values=std::vector<int64_t>{INT64_C(9007199254740992),INT64_C(9007199254740993)};
  vdb::PointGroupEdit group;group.name="chosen";group.membership={0,1};
  (void)vdb::WritePointAttributeEdits(files.directory/"base.vdb",files.directory/"source.vdb",std::span(&ids,1),std::span(&group,1),"points",processing);
  std::ifstream originalFile(files.directory/"source.vdb",std::ios::binary);
  const std::string original(std::istreambuf_iterator<char>(originalFile),{});originalFile.close();
  OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;request.operation=Operation::Diagnose;
  request.input=files.path("source.vdb");request.inputGrid="points";request.output=files.path("unused.vdb");
  request.diagnostics.profile=vdb::DiagnosticProfile::PointData;request.diagnostics.pointAttributes={"id"};
  request.diagnostics.componentRange=std::array<double,2>{9007199254740992.,9007199254740992.};
  request.pointSelection.anyOf={"chosen"};request.maxPoints=2;
  request.pointAttributes.maxEdits=0; // A stale hidden editing cap is irrelevant.
  ASSERT_TRUE(panel.submit(request));request.diagnostics.pointAttributes={"missing"};request.pointSelection.anyOf={"absent"};request.maxPoints=1;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_TRUE(panel.result()->output.empty());EXPECT_FALSE(panel.result()->modelOutput);EXPECT_FALSE(std::filesystem::exists(files.directory/"unused.vdb"));
  EXPECT_NE(panel.result()->message.find("Point counts valid: yes; total 2; selected 1"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("point-attribute:id: Fail; coverage complete"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("point ordinal 1"),std::string::npos);
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);
  request.maxPoints=2;request.pointSelection.anyOf={"chosen"};
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_NE(panel.result()->message.find("point-attribute:missing: Unavailable"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("coverage INCOMPLETE"),std::string::npos);
  std::ifstream source(files.directory/"source.vdb",std::ios::binary);EXPECT_EQ(std::string(std::istreambuf_iterator<char>(source),{}),original);
}

TEST(OpenVdbAuthoring, DefaultExecutorMeasuresWorldSurfaceWithoutOutput) {
  namespace vdb = container::geometry::vdb;
  if (!vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB is disabled";
  AuthoringFiles files; vdb::ProcessingOptions options; options.voxelSize=.15;
  (void)vdb::WriteSphere(files.directory / "sphere.vdb", {}, options);
  OpenVdbAuthoringPanel panel; OpenVdbAuthoringRequest request;
  request.operation = container::ui::OpenVdbAuthoringOperation::Measure;
  request.input = files.path("sphere.vdb"); request.inputGrid = "surface";
  request.output = files.path("stale-output.vdb");
  ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_TRUE(panel.result()->output.empty()); EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_NE(panel.result()->message.find("Area:"), std::string::npos);
  EXPECT_NE(panel.result()->message.find("world units cubed"), std::string::npos);
  EXPECT_NE(panel.result()->message.find("resolution-dependent"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(files.directory / "stale-output.vdb"));
}

TEST(OpenVdbAuthoring, DefaultExecutorRasterizesNativePointRadiusVelocityAndMass) {
  namespace vdb=container::geometry::vdb;
  using Operation=container::ui::OpenVdbAuthoringOperation;
  if (!vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB is disabled";
  AuthoringFiles files; vdb::ProcessingOptions options; options.voxelSize=.1;
  const std::vector<vdb::PointRecord> records{{{0,0,0},{2,3,4},.3}};
  (void)vdb::WritePoints(records,files.directory/"points.vdb",options);
  OpenVdbAuthoringPanel panel; OpenVdbAuthoringRequest request;
  request.input=files.path("points.vdb"); request.inputGrid="surface";
  request.processing=options; request.maxPoints=1; request.splat.massPerPoint=2;
  for (const auto& [operation,name]:std::array{
       std::pair{Operation::PointsSdf,"sdf.vdb"}, std::pair{Operation::SplatDensity,"density.vdb"},
       std::pair{Operation::SplatVelocity,"velocity.vdb"}, std::pair{Operation::SplatRadius,"radius.vdb"}}) {
    request.operation=operation; request.output=files.path(name);
    request.processing.gridName="field";
    ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
    ASSERT_TRUE(panel.result()->success) << panel.result()->message;
    EXPECT_EQ(panel.result()->modelOutput,operation!=Operation::SplatVelocity);
    if (operation==Operation::SplatVelocity)
      EXPECT_EQ(vdb::SampleVector(files.directory/name,{0,0,0},"field"),(std::array<double,3>{2,3,4}));
    else {
      const auto value=vdb::SampleScalar(files.directory/name,{0,0,0},"field");
      if (operation==Operation::PointsSdf) EXPECT_LT(value,0);
      else EXPECT_NEAR(value,operation==Operation::SplatDensity ? 2000 : .3,1e-9);
    }
  }
}

TEST(OpenVdbAuthoring, DefaultExecutorTracksMovingAndMorphingSdfWithSnapshotSettings) {
  namespace vdb=container::geometry::vdb;
  using Operation=container::ui::OpenVdbAuthoringOperation;
  if (!vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB is disabled";
  AuthoringFiles files; vdb::ProcessingOptions options; options.voxelSize=.1;
  (void)vdb::WriteSphere(files.directory/"source.vdb",{},options);
  (void)vdb::WriteSphere(files.directory/"target.vdb",{.radius=1.2},options);
  (void)vdb::WriteConstantVelocity(files.directory/"wind.vdb",{1,0,0},options);
  OpenVdbAuthoringPanel panel; OpenVdbAuthoringRequest request;
  request.input=files.path("source.vdb"); request.inputGrid="surface";
  request.operation=Operation::AdvectSdf; request.secondInput=files.path("wind.vdb");
  request.secondGrid="surface"; request.output=files.path("moved.vdb"); request.levelSetTransport.timeStep=.2;
  ASSERT_TRUE(panel.submit(request)); request.levelSetTransport.timeStep=-20;
  ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_TRUE(panel.result()->modelOutput); EXPECT_NE(panel.result()->message.find("CFL"),std::string::npos);
  EXPECT_LT(vdb::SampleScalar(files.directory/"moved.vdb",{1.1,0,0}),0);
  EXPECT_GT(vdb::SampleScalar(files.directory/"moved.vdb",{-.9,0,0}),0);
  request.operation=Operation::MorphSdf; request.secondInput=files.path("target.vdb");
  request.output=files.path("morphed.vdb"); request.levelSetTransport.timeStep=1;
  ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_TRUE(panel.result()->modelOutput);
  EXPECT_LT(vdb::SampleScalar(files.directory/"morphed.vdb",{1.1,0,0}),0);
}

TEST(OpenVdbAuthoring, DefaultExecutorEditsAndExportsNativeTypedPointData) {
  namespace vdb=container::geometry::vdb;
  using Operation=container::ui::OpenVdbAuthoringOperation;
  if (!vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB is disabled";
  AuthoringFiles files;
  (void)vdb::WritePoints(std::vector<vdb::PointRecord>{{{0,0,0},{0,0,0},1}},files.directory/"points.vdb");
  std::ofstream(files.directory/"edits.json") << R"({"version":1,"attributes":[{"op":"add","name":"id","type":"int64","values":[9007199254740993]}],"groups":[{"op":"add","name":"selected","values":[true]}]})";
  OpenVdbAuthoringPanel panel; OpenVdbAuthoringRequest request;
  request.operation=Operation::EditPointAttributes; request.input=files.path("points.vdb");
  request.inputGrid="surface"; request.secondInput=files.path("edits.json");
  request.output=files.path("edited.vdb"); request.processing.gridName="native";
  const auto run=[&] {
    if (!panel.submit(request) || !settle(panel) || !panel.result()) return false;
    EXPECT_TRUE(panel.result()->success) << panel.result()->message;
    EXPECT_FALSE(panel.result()->modelOutput);
    return panel.result()->success;
  };
  ASSERT_TRUE(run());
  request.input=request.output; request.inputGrid="native";
  request.operation=Operation::PointAttribute; request.pointField="id"; request.output=files.path("id.json");
  ASSERT_TRUE(run());
  const auto text=[&](const char* name) { std::ifstream stream(files.directory/name); return std::string(std::istreambuf_iterator<char>(stream),{}); };
  EXPECT_NE(text("id.json").find("9007199254740993"),std::string::npos);
  request.operation=Operation::PointGroup; request.pointField="selected"; request.output=files.path("group.json");
  ASSERT_TRUE(run()); EXPECT_NE(text("group.json").find("[true]"),std::string::npos);
  request.operation=Operation::PointSchema; request.output=files.path("schema.json");
  ASSERT_TRUE(run()); EXPECT_NE(text("schema.json").find("leaf_origin_xyz_then_local_index"),std::string::npos);
  EXPECT_NE(text("schema.json").find("selected"),std::string::npos);
  request.operation=Operation::CountPoints; request.pointSelection.anyOf={"selected"};
  ASSERT_TRUE(run()); EXPECT_TRUE(panel.result()->output.empty());
  EXPECT_NE(panel.result()->message.find("1 selected of 1"),std::string::npos);
  request.operation=Operation::PointStatistics; request.pointField="id";
  ASSERT_TRUE(run()); EXPECT_TRUE(panel.result()->output.empty());
  EXPECT_NE(panel.result()->message.find("9007199254740993"),std::string::npos);
  request.operation=Operation::SelectPoints; request.output=files.path("selected.vdb"); ASSERT_TRUE(run());
  EXPECT_EQ(vdb::CountSelectedPoints(files.directory/"selected.vdb").total,1u);
  request.operation=Operation::DeletePoints; request.output=files.path("empty.vdb"); ASSERT_TRUE(run());
  const auto empty=vdb::CountSelectedPoints(files.directory/"empty.vdb");
  EXPECT_EQ(empty.total,0u); EXPECT_TRUE(empty.archivedSchema);
  request.operation=Operation::PointVoxelMask; request.output=files.path("mask.vdb");
  request.pointMask.writeCounts=true; request.pointMask.countGridName="counts";
  request.processing.gridName="occupancy"; ASSERT_TRUE(run());
  EXPECT_NE(panel.result()->message.find("1 selected of 1"),std::string::npos);
  const auto mask=vdb::InspectFile(files.directory/"mask.vdb"); ASSERT_EQ(mask.grids.size(),2u);
  std::set<std::string> types; for (const auto& grid:mask.grids) types.insert(grid.valueType);
  // MaskGrid's public value type is bool; native point-mask tests verify its
  // distinct MaskTree storage rather than inferring it from valueType().
  EXPECT_EQ(types,(std::set<std::string>{"bool","int64"}));
  request.output=files.path("limited-mask.vdb"); request.processing.maxVoxelCount=512;
  ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  EXPECT_FALSE(panel.result()->success); EXPECT_FALSE(std::filesystem::exists(files.directory/"limited-mask.vdb"));
}

TEST(OpenVdbAuthoring, DefaultExecutorAuthorsNativeFilesFromBoundedSpec) {
  namespace vdb=container::geometry::vdb;
  using Operation=container::ui::OpenVdbAuthoringOperation;
  if (!vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB is disabled";
  AuthoringFiles files;
  vdb::ProcessingOptions processing; processing.gridName="velocity";
  (void)vdb::WriteConstantVelocity(files.directory/"wind.vdb",{1,2,3},processing);
  std::filesystem::create_directory(files.directory/"specs");
  std::ofstream(files.directory/"specs/author.json") << R"({"version":1,"inputs":[{"path":"../wind.vdb","edits":[{"selector":"velocity","newName":"flow","metadata":[{"name":"exact_id","operation":"set","type":"int64","value":9007199254740993}]}]}]})";
  OpenVdbAuthoringPanel panel; OpenVdbAuthoringRequest request;
  request.operation=Operation::AuthorFile; request.input=files.path("specs/author.json");
  request.output=files.path("authored.vdb");
  ASSERT_TRUE(panel.submit(request)); request.fileAuthoring.maxSpecBytes=1;
  ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_NE(panel.result()->message.find("renamed 1"),std::string::npos);
  const auto authored=vdb::InspectFile(files.directory/"authored.vdb");
  ASSERT_EQ(authored.grids.size(),1u); EXPECT_EQ(authored.grids[0].name,"flow");
  EXPECT_EQ(vdb::SampleVector(files.directory/"authored.vdb",{4,2,3},"flow"),(std::array<double,3>{1,2,3}));
  request.output=files.path("limited.vdb");
  ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  EXPECT_FALSE(panel.result()->success); EXPECT_FALSE(std::filesystem::exists(files.directory/"limited.vdb"));
  request.fileAuthoring={}; request.output=files.path("authored.vdb");
  ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  EXPECT_FALSE(panel.result()->success); EXPECT_EQ(vdb::InspectFile(files.directory/"authored.vdb").grids[0].name,"flow");
}

TEST(OpenVdbAuthoring, DefaultExecutorCompositesWholeFieldsAndTransformsChosenScope) {
  namespace vdb=container::geometry::vdb;
  using Operation=container::ui::OpenVdbAuthoringOperation;
  if (!vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB is disabled";
  AuthoringFiles files; vdb::DenseArrayVolume a;
  a.values=std::vector<std::array<double,3>>{{1,2,3}}; a.background=std::array<double,3>{1,1,1}; a.active={1};
  auto b=a; b.values=std::vector<std::array<double,3>>{{4,5,6}}; b.background=std::array<double,3>{2,2,2}; b.active={0};
  (void)vdb::WriteDenseVdb(files.directory/"a.vdb",a); (void)vdb::WriteDenseVdb(files.directory/"b.vdb",b);
  OpenVdbAuthoringPanel panel; OpenVdbAuthoringRequest request;
  request.operation=Operation::CompositeFields; request.input=files.path("a.vdb"); request.secondInput=files.path("b.vdb");
  request.output=files.path("sum.vdb"); ASSERT_TRUE(panel.submit(request)); request.fieldComposite.maxWorkItems=1;
  ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result()); ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_EQ(vdb::SampleVector(files.directory/"sum.vdb",{0,0,0}),(std::array<double,3>{5,7,9}));
  EXPECT_EQ(vdb::SampleVector(files.directory/"sum.vdb",{100,100,100}),(std::array<double,3>{3,3,3}));
  request.operation=Operation::TransformField; request.output=files.path("active.vdb");
  request.fieldTransform.operation=vdb::FieldValueOperation::Affine; request.fieldTransform.scale={2,3,4}; request.fieldTransform.offset={1,-1,0};
  ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_EQ(vdb::SampleVector(files.directory/"active.vdb",{0,0,0}),(std::array<double,3>{3,5,12}));
  EXPECT_EQ(vdb::SampleVector(files.directory/"active.vdb",{100,100,100}),(std::array<double,3>{1,1,1}));
  request.output=files.path("all.vdb"); request.fieldTransform.scope=vdb::FieldValueScope::AllValues;
  ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_EQ(vdb::SampleVector(files.directory/"all.vdb",{100,100,100}),(std::array<double,3>{3,2,4}));
  request.output=files.path("limited.vdb"); request.fieldTransform.maxWorkItems=1;
  ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  EXPECT_FALSE(panel.result()->success); EXPECT_FALSE(std::filesystem::exists(files.directory/"limited.vdb"));
}

TEST(OpenVdbAuthoring, DefaultExecutorRunsTopologyMasksAndNativePointSamplingSnapshots) {
  namespace vdb=container::geometry::vdb;
  using Operation=container::ui::OpenVdbAuthoringOperation;
  if (!vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB is disabled";
  AuthoringFiles files; vdb::DenseArrayVolume volume;
  volume.values=std::vector<float>{0}; volume.background=0.0f; volume.active={1};
  (void)vdb::WriteDenseVdb(files.directory/"field.vdb",volume);
  OpenVdbAuthoringPanel panel; OpenVdbAuthoringRequest request;
  const auto run=[&] {
    if (!panel.submit(request) || !settle(panel) || !panel.result()) return false;
    EXPECT_TRUE(panel.result()->success) << panel.result()->message;
    return panel.result()->success;
  };
  request.input=files.path("field.vdb"); request.operation=Operation::TopologyMask;
  request.output=files.path("dilated.vdb"); request.topologyOperation=vdb::TopologyOperation::Dilate;
  ASSERT_TRUE(panel.submit(request)); request.topology.maxWorkItems=1;
  ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result()); ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_EQ(vdb::InspectFile(files.directory/"dilated.vdb").grids[0].activeVoxelCount,7u);
  request.topology={}; request.operation=Operation::TopologyBoolean;
  request.secondInput=files.path("dilated.vdb"); request.output=files.path("difference.vdb");
  request.topologyBoolean=vdb::TopologyBoolean::Difference; ASSERT_TRUE(run());
  EXPECT_EQ(vdb::InspectFile(files.directory/"difference.vdb").grids[0].activeVoxelCount,0u);
  std::vector<vdb::PointRecord> points(2); points[0].position={.25,0,0}; points[1].position={.75,0,0};
  (void)vdb::WritePoints(points,files.directory/"points.vdb");
  vdb::ProcessingOptions processing; processing.gridName="velocity";
  (void)vdb::WriteConstantVelocity(files.directory/"velocity.vdb",{1,-2,3},processing);
  request.operation=Operation::SampleNativePointAttribute; request.input=files.path("points.vdb");
  request.secondInput=files.path("velocity.vdb"); request.output=files.path("sampled.vdb"); request.processing.gridName="points";
  request.pointFieldSampling.attributeName="field_value";
  ASSERT_TRUE(panel.submit(request)); request.pointFieldSampling.maxWork=1;
  ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result()); ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_FALSE(panel.result()->modelOutput);
  const auto sampled=vdb::ReadPointAttribute(files.directory/"sampled.vdb","field_value");
  EXPECT_EQ((std::get<std::vector<std::array<double,3>>>(sampled.values)),
            (std::vector<std::array<double,3>>{{1,-2,3},{1,-2,3}}));
  request.output=files.path("limited.vdb"); ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result()); EXPECT_FALSE(panel.result()->success);
  EXPECT_FALSE(std::filesystem::exists(files.directory/"limited.vdb"));
}

TEST(OpenVdbAuthoring, DefaultExecutorEditsNativeTopologyWithImmutableOptions) {
  namespace vdb=container::geometry::vdb;
  using Operation=container::ui::OpenVdbAuthoringOperation;
  if(!vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB is disabled";
  AuthoringFiles files;
  vdb::DenseArrayVolume volume;
  volume.region.dimensions={2,1,1}; volume.values=std::vector<double>{-0.,1.125}; volume.background=0.; volume.active={0,1};
  (void)vdb::WriteDenseVdb(files.directory/"source.vdb",volume);
  OpenVdbAuthoringPanel panel; OpenVdbAuthoringRequest request;
  request.operation=Operation::EditValueTopology; request.input=files.path("source.vdb");
  request.output=files.path("active.vdb"); request.processing.gridName="field";
  request.valueTopologyOperation=vdb::ValueTopologyOperation::ActivateValue;
  request.valueTopology.value={"-0"};
  ASSERT_TRUE(panel.submit(request)); request.valueTopology.value={"1"}; request.valueTopology.maxWorkItems=1;
  ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result()); ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_EQ(vdb::InspectFile(files.directory/"active.vdb").grids[0].activeVoxelCount,2u);
  request.input=files.path("active.vdb"); request.output=files.path("clipped.vdb");
  request.valueTopology={}; request.valueTopology.minimum={0,0,0}; request.valueTopology.maximum={0,0,0};
  request.valueTopologyOperation=vdb::ValueTopologyOperation::ClipBox;
  ASSERT_TRUE(panel.submit(request)); request.valueTopology.maximum={100000,100000,100000};
  ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result()); ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_EQ(vdb::InspectFile(files.directory/"clipped.vdb").grids[0].activeVoxelCount,1u);
  request.output=files.path("limited.vdb"); request.valueTopology.maxWorkItems=1;
  ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  EXPECT_FALSE(panel.result()->success); EXPECT_FALSE(std::filesystem::exists(files.directory/"limited.vdb"));
}

TEST(OpenVdbAuthoring, DefaultExecutorSnapshotsInspectionHistogramSettingsAndReportsMoments) {
  namespace vdb=container::geometry::vdb;
  using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;
  OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::Inspect;request.input=files.path("field.vdb");
  request.statistics=true;request.inspectionHistogramBins=4;
  request.inspectionHistogramRange=std::array{0.,4.};
  if(vdb::IsAvailable()) {
    vdb::DenseArrayVolume volume;volume.region.dimensions={2,1,1};
    volume.values=std::vector<double>{1,3};volume.background=0.;volume.active={1,1};
    (void)vdb::WriteDenseVdb(files.directory/"field.vdb",volume);
  }
  ASSERT_TRUE(panel.submit(request));
  request.inspectionHistogramBins=0;request.inspectionHistogramRange=std::array{99.,100.};
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()) {
    EXPECT_FALSE(panel.result()->success);
    EXPECT_NE(panel.result()->message.find("unavailable"),std::string::npos) << panel.result()->message;
    return;
  }
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_FALSE(panel.result()->modelOutput);EXPECT_TRUE(panel.result()->output.empty());
  EXPECT_NE(panel.result()->message.find("Mean: 2, population deviation: 1"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("Histogram [0,4]"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("Bins: 0,1,0,1"),std::string::npos);
  request.inspectionHistogramBins=4097;
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  EXPECT_FALSE(panel.result()->success);
}

TEST(OpenVdbAuthoring, DefaultExecutorRunsAllNativeMaskEditsAndSnapshotsSelection) {
  namespace vdb=container::geometry::vdb;
  using Operation=container::ui::OpenVdbAuthoringOperation;
  using Mask=vdb::MaskedValuesOperation;
  if(!vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB is disabled";
  AuthoringFiles files;
  vdb::DenseArrayVolume source; source.region.dimensions={2,1,1}; source.values=std::vector<double>{-0.,1.125}; source.background=0.; source.active={0,1};
  (void)vdb::WriteDenseVdb(files.directory/"source.vdb",source);
  source.values=std::vector<double>{0.,9.}; source.active={1,0};
  (void)vdb::WriteDenseVdb(files.directory/"mask.vdb",source);
  OpenVdbAuthoringPanel panel; OpenVdbAuthoringRequest request;
  request.operation=Operation::MaskedValues; request.input=files.path("source.vdb"); request.secondInput=files.path("mask.vdb");
  request.processing.gridName="field";
  for(const auto pair:{std::pair{Mask::Activate,2u},std::pair{Mask::Deactivate,1u},std::pair{Mask::IntersectActivity,0u},
                      std::pair{Mask::ClipInside,0u},std::pair{Mask::ClipOutside,1u}}) {
    const auto name=std::to_string(static_cast<int>(pair.first))+".vdb";
    request.output=files.path(name.c_str()); request.maskedValuesOperation=pair.first; request.maskedValuesMaxWork=200'000'000;
    ASSERT_TRUE(panel.submit(request)); request.maskedValuesMaxWork=1; request.maskedValuesOperation=Mask::Deactivate;
    ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result()); ASSERT_TRUE(panel.result()->success) << panel.result()->message;
    EXPECT_FALSE(panel.result()->modelOutput);
    EXPECT_EQ(vdb::InspectFile(files.directory/name).grids[0].activeVoxelCount,pair.second);
    EXPECT_EQ(std::signbit(vdb::SampleScalar(files.directory/name,{0,0,0})),pair.first!=Mask::ClipOutside);
  }
  request.output=files.path("limited.vdb");
  ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  EXPECT_FALSE(panel.result()->success); EXPECT_FALSE(std::filesystem::exists(files.directory/"limited.vdb"));
}

TEST(OpenVdbAuthoring, DefaultExecutorSnapshotsNativeSdfFiltersAndReportsGradientQuality) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::SdfFilter;request.input=files.path("source.vdb");request.output=files.path("filtered.vdb");
  request.sdfFilterOperation=vdb::SdfFilterOperation::MeanCurvature;
  if(vdb::IsAvailable()) {
    vdb::ProcessingOptions p;p.voxelSize=.25;(void)vdb::WriteSphere(files.directory/"source.vdb",{},p);
  }
  ASSERT_TRUE(panel.submit(request));request.sdfFilter.maxWorkItems=1;request.sdfFilter.width=99;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()) {
    EXPECT_FALSE(panel.result()->success);EXPECT_NE(panel.result()->message.find("unavailable"),std::string::npos);return;
  }
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;EXPECT_TRUE(panel.result()->modelOutput);
  EXPECT_NE(panel.result()->message.find("Near-interface mean gradient error:"),std::string::npos);
  EXPECT_GT(vdb::SampleScalar(files.directory/"filtered.vdb",{1,0,0}),0);
  request.output=files.path("limited.vdb");request.sdfFilter.width=1;
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  EXPECT_FALSE(panel.result()->success);EXPECT_FALSE(std::filesystem::exists(files.directory/"limited.vdb"));
}

TEST(OpenVdbAuthoring, DefaultExecutorRunsSdfTrackerBandAndAlphaWorkflows) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  if(!vdb::IsAvailable())GTEST_SKIP()<<"OpenVDB is disabled";
  AuthoringFiles files;vdb::ProcessingOptions p;p.voxelSize=.25;
  (void)vdb::WriteSphere(files.directory/"source.vdb",{},p);
  OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::SdfFilter;request.input=files.path("source.vdb");
  for(const auto mode:{vdb::SdfFilterOperation::Normalize,vdb::SdfFilterOperation::Track,
      vdb::SdfFilterOperation::Prune,vdb::SdfFilterOperation::ResizeBand,
      vdb::SdfFilterOperation::DilateBand,vdb::SdfFilterOperation::ErodeBand}) {
    request.sdfFilterOperation=mode;request.sdfFilter={};
    if(mode==vdb::SdfFilterOperation::ResizeBand)request.sdfFilter.bandWidth=4;
    request.output=files.path(("mode-"+std::to_string(int(mode))+".vdb").c_str());
    ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
    ASSERT_TRUE(panel.result()->success)<<panel.result()->message;EXPECT_TRUE(panel.result()->modelOutput);
  }
  vdb::DenseArrayVolume mask;mask.linear={.25,0,0,0,.25,0,0,0,.25};
  mask.values=std::vector<double>{.5};mask.background=.5;mask.active={0};
  (void)vdb::WriteDenseVdb(files.directory/"mask.vdb",mask);
  request.sdfFilter={};request.sdfFilterOperation=vdb::SdfFilterOperation::Laplacian;
  request.sdfFilter.maskInput=files.directory/"mask.vdb";request.sdfFilter.temporal=vdb::SdfTemporalScheme::Rk3;
  request.output=files.path("masked.vdb");
  ASSERT_TRUE(panel.submit(request));request.sdfFilter.maskInput=files.directory/"missing.vdb";
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_TRUE(panel.result()->modelOutput);
}

TEST(OpenVdbAuthoring, DefaultExecutorRebuildsAsymmetricNativeBandsWithImmutableOptions) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::RebuildLevelSet;request.input=files.path("source.vdb");request.output=files.path("rebuilt.vdb");
  request.processing.gridName="repaired";request.levelSetRebuild.exteriorWidth=4;request.levelSetRebuild.interiorWidth=2;
  if(vdb::IsAvailable()) {
    vdb::ProcessingOptions p;p.voxelSize=.25;(void)vdb::WriteSphere(files.directory/"source.vdb",{},p);
  }
  ASSERT_TRUE(panel.submit(request));request.levelSetRebuild.maxWork=1;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()) {EXPECT_FALSE(panel.result()->success);return;}
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;EXPECT_TRUE(panel.result()->modelOutput);
  const auto volume=vdb::ReadDenseVdb(files.directory/"rebuilt.vdb",{},"repaired");
  EXPECT_EQ(volume.gridClass,vdb::DenseGridClass::LevelSet);
  EXPECT_FLOAT_EQ(std::get<float>(volume.background),1);
  EXPECT_FLOAT_EQ(std::get<std::vector<float>>(volume.values).at(0),-.5f);
  EXPECT_NE(panel.result()->message.find("4 / 2 target voxels"),std::string::npos);
  request.output=files.path("bad.vdb");ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);EXPECT_FALSE(std::filesystem::exists(files.directory/"bad.vdb"));
}

TEST(OpenVdbAuthoring, ActiveRegionQueriesRetainCompleteCountsAndExactNativeBitsWithoutOutputs) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::QueryActiveRegions;request.input=files.path("source.vdb");request.inputGrid="field";
  request.secondInput=std::string("\xff",1);request.output=std::string("\xff",1);
  request.processing.maxInputBytes=0;request.processing.maxVoxelCount=0;
  request.activeRegions={{{-1,0,0},{1,0,0}},{{0,0,0},{0,0,0}}};
  std::string original;
  if(vdb::IsAvailable()) {
    vdb::DenseArrayVolume source;source.region={{-1,0,0},{3,1,1}};source.values=std::vector<double>{1.5,-0.,7.};source.background=0.;source.active={1,1,0};source.gridName="field";
    (void)vdb::WriteDenseVdb(files.directory/"source.vdb",source);
    std::ifstream input(files.directory/"source.vdb",std::ios::binary);original={std::istreambuf_iterator<char>(input),{}};
  }
  ASSERT_TRUE(panel.submit(request));request.activeRegions.clear();request.activeRegionOptions.maxWork=1;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()) {EXPECT_FALSE(panel.result()->success);EXPECT_NE(panel.result()->message.find("ENABLE_OPENVDB=ON"),std::string::npos);return;}
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_TRUE(panel.result()->output.empty());EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_NE(panel.result()->message.find("Native type: Double"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("Complete source census: 2 active voxels"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("Complete counts for 2 inclusive INDEX queries"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("bits 0x8000000000000000"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("bits 0x3ff8000000000000"),std::string::npos);
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);
  request.activeRegionOptions={};request.activeRegionOptions.maxRecordsPerQuery=0;request.activeRegionOptions.maxTotalRecords=0;
  request.activeRegions={{{-1,0,0},{1,0,0}}};
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_NE(panel.result()->message.find("matched voxels/records 2/2; omitted by native quota 2/2"),std::string::npos);
  EXPECT_EQ(panel.result()->message.find("bits 0x"),std::string::npos);
  request.activeRegions.clear();ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;EXPECT_NE(panel.result()->message.find("Complete counts for 0 inclusive INDEX queries"),std::string::npos);
  request.activeRegionOptions.maxInputBytes=131072;ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);
  std::ifstream input(files.directory/"source.vdb",std::ios::binary);EXPECT_EQ((std::string{std::istreambuf_iterator<char>(input),{}}),original);
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(files.directory),std::filesystem::directory_iterator{}),1);
}

TEST(OpenVdbAuthoring, ActiveRegionSubmissionOwnsAllNestedQueriesAndLimits) {
  using Operation=container::ui::OpenVdbAuthoringOperation;
  std::promise<OpenVdbAuthoringRequest> capture;auto captured=capture.get_future();
  OpenVdbAuthoringPanel panel([&](const auto& value){capture.set_value(value);return OpenVdbAuthoringResult{true,"queried",{},false};});
  OpenVdbAuthoringRequest request;request.operation=Operation::QueryActiveRegions;request.input="source.vdb";request.inputGrid="chosen";
  request.activeRegions={{{INT32_MIN,-2,3},{INT32_MAX,4,5}},{{6,7,8},{9,10,11}}};
  auto& limits=request.activeRegionOptions;limits.maxInputBytes=123456789;limits.maxStoredLeafVoxels=2222;limits.maxNodes=3333;limits.maxSourceActiveVoxels=UINT64_MAX-1;limits.maxWork=987654321;
  limits.maxGrids=17;limits.maxQueries=18;limits.maxMetadataBytes=1919;limits.maxRecordsPerQuery=20;limits.maxTotalRecords=21;
  ASSERT_TRUE(panel.submit(request));request.input="changed";request.inputGrid.clear();request.activeRegions.clear();request.activeRegionOptions={};
  ASSERT_EQ(captured.wait_for(5s),std::future_status::ready);const auto saved=captured.get();
  EXPECT_EQ(saved.operation,Operation::QueryActiveRegions);EXPECT_EQ(saved.input,"source.vdb");EXPECT_EQ(saved.inputGrid,"chosen");
  ASSERT_EQ(saved.activeRegions.size(),2u);EXPECT_EQ(saved.activeRegions[0].minimum,(std::array<int32_t,3>{INT32_MIN,-2,3}));EXPECT_EQ(saved.activeRegions[1].maximum,(std::array<int32_t,3>{9,10,11}));
  const auto& o=saved.activeRegionOptions;EXPECT_EQ(o.maxInputBytes,123456789);EXPECT_EQ(o.maxStoredLeafVoxels,2222);EXPECT_EQ(o.maxNodes,3333);EXPECT_EQ(o.maxSourceActiveVoxels,UINT64_MAX-1);EXPECT_EQ(o.maxWork,987654321);EXPECT_EQ(o.maxGrids,17);EXPECT_EQ(o.maxQueries,18);EXPECT_EQ(o.maxMetadataBytes,1919);EXPECT_EQ(o.maxRecordsPerQuery,20);EXPECT_EQ(o.maxTotalRecords,21);
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_TRUE(panel.result()->output.empty());EXPECT_FALSE(panel.result()->modelOutput);
}

TEST(OpenVdbAuthoring, DefaultExecutorDerivesTypedSurfaceFieldsWithValidityAndImmutableOptions) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::SurfaceFields;request.input=files.path("source.vdb");request.output=files.path("normal.vdb");
  request.inputGrid="surface";request.processing.gridName="derived";
  request.surfaceFieldOperation=vdb::SurfaceFieldOperation::Normal;
  request.surfaceFields.promoteToDouble=true;request.surfaceFields.validityGridName="valid";
  if(vdb::IsAvailable()) {
    vdb::ProcessingOptions p;p.voxelSize=.25;(void)vdb::WriteSphere(files.directory/"source.vdb",{},p);
  }
  ASSERT_TRUE(panel.submit(request));request.surfaceFields.maxWorkItems=1;
  request.surfaceFields.validityGridName="derived";request.surfaceFieldOperation=vdb::SurfaceFieldOperation::GaussianCurvature;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()) {EXPECT_FALSE(panel.result()->success);return;}
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;EXPECT_FALSE(panel.result()->modelOutput);
  const auto normal=vdb::ReadDenseVdb(files.directory/"normal.vdb",{{3,0,0},{1,1,1}},"derived");
  const auto value=std::get<std::vector<std::array<double,3>>>(normal.values).at(0);
  EXPECT_NEAR(value[0],1,1e-12);EXPECT_NEAR(value[1],0,1e-12);EXPECT_NEAR(value[2],0,1e-12);
  EXPECT_EQ(normal.vectorType,vdb::DenseVectorType::CovariantNormalize);EXPECT_TRUE(normal.worldSpace);
  const auto inspected=vdb::InspectFile(files.directory/"normal.vdb");ASSERT_EQ(inspected.grids.size(),2u);
  for(const auto& grid:inspected.grids) {
    ASSERT_TRUE(grid.activeVoxelCount);EXPECT_GT(*grid.activeVoxelCount,0u);
    if(grid.name=="valid")EXPECT_EQ(grid.valueType,"bool");
  }
  EXPECT_NE(panel.result()->message.find("Bool validity grid valid"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("Missing footprints"),std::string::npos);
  request.output=files.path("bad.vdb");ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);EXPECT_FALSE(std::filesystem::exists(files.directory/"bad.vdb"));
  request.surfaceFields={};request.surfaceFields.promoteToDouble=true;request.surfaceFields.validityGridName="valid";
  for(const auto mode:{vdb::SurfaceFieldOperation::MeanCurvature,vdb::SurfaceFieldOperation::GaussianCurvature,vdb::SurfaceFieldOperation::ClosestPoint}) {
    request.surfaceFieldOperation=mode;request.output=files.path(("field-"+std::to_string(int(mode))+".vdb").c_str());
    ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
    ASSERT_TRUE(panel.result()->success)<<panel.result()->message;EXPECT_FALSE(panel.result()->modelOutput);
    const auto field=vdb::ReadDenseVdb(std::filesystem::path(std::u8string(request.output.begin(),request.output.end())),{{3,0,0},{1,1,1}},"derived");
    if(mode==vdb::SurfaceFieldOperation::ClosestPoint) {
      const auto position=std::get<std::vector<std::array<double,3>>>(field.values).at(0);
      EXPECT_NEAR(position[0],1,1e-12);EXPECT_NEAR(position[1],0,1e-12);EXPECT_NEAR(position[2],0,1e-12);
      EXPECT_EQ(field.vectorType,vdb::DenseVectorType::ContravariantAbsolute);
    } else {
      const auto curvature=std::get<std::vector<double>>(field.values).at(0);
      EXPECT_GT(curvature,0);EXPECT_NEAR(curvature,mode==vdb::SurfaceFieldOperation::MeanCurvature?1/.75:1/(.75*.75),.3);
    }
  }
}

TEST(OpenVdbAuthoring, DefaultExecutorAdvectsGenericNativePointsWithImmutableGroupsMapsPayloadAndLimits) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::AdvectNativePoints;request.input=files.path("source.vdb");request.inputGrid="native";
  request.secondInput=files.path("flow.vdb");request.secondGrid="flow";request.output=files.path("advected.vdb");request.processing.gridName="advected";
  request.pointSelection.anyOf={"A"};request.maxPoints=4;request.pointAttributes.maxEdits=0;
  auto& settings=request.nativePointAdvection;settings.timeStep=2;settings.steps=4;settings.integration=vdb::NativePointIntegration::RK4;
  settings.velocityLayout=vdb::NativePointVelocityLayout::World;settings.groups.anyOf={"hidden-unused-group"};
  const std::array<double,16> target{.5,0,0,1, 0,-.5,0,2, 0,0,.5,3, 0,0,0,1};settings.targetIndexToWorld=target;
  if(vdb::IsAvailable()) {
    const std::array<vdb::PointRecord,4> records{{{{-2,0,0},{3,4,5},.5},{{.25,0,0},{3,4,5},.5},{{2,0,0},{3,4,5},.5},{{4,0,0},{3,4,5},.5}}};
    vdb::ProcessingOptions p;p.voxelSize=1;p.gridName="native";(void)vdb::WritePoints(records,files.directory/"raw.vdb",p);
    vdb::PointAttributeEdit id;id.name="id";id.values=std::vector<int64_t>{INT64_MAX,INT64_MAX-1,INT64_MAX-2,INT64_MAX-3};
    vdb::PointAttributeEdit signal;signal.name="signal";signal.values=std::vector<double>{-0.,1e-200,3,4};
    vdb::PointAttributeEdit label;label.name="label";label.values=std::vector<std::string>{"a",std::string("b\0raw",5),"c","d"};
    vdb::PointGroupEdit group;group.name="A";group.membership={1,0,1,0};
    const std::array edits{id,signal,label};const std::array groups{group};
    (void)vdb::WritePointAttributeEdits(files.directory/"raw.vdb",files.directory/"edited.vdb",edits,groups,"native",p);
    std::ofstream(files.directory/"meta.json")<<R"({"version":1,"inputs":[{"path":"edited.vdb","edits":[{"selector":"native","metadata":[{"name":"artist","operation":"set","type":"string","value":"advection artist"}]}]}]})";
    (void)vdb::WriteAuthoredVdbSpec(files.directory/"meta.json",files.directory/"source.vdb");
    p.gridName="flow";(void)vdb::WriteConstantVelocity(files.directory/"flow.vdb",{.5,-.25,0},p);
  }
  const auto bytes=[](const std::filesystem::path& source){std::ifstream input(source,std::ios::binary);return std::string(std::istreambuf_iterator<char>(input),{});};
  const auto original=vdb::IsAvailable()?bytes(files.directory/"source.vdb"):std::string{};
  const auto velocityBytes=vdb::IsAvailable()?bytes(files.directory/"flow.vdb"):std::string{};
  const auto submitted=request;ASSERT_TRUE(panel.submit(request));
  request.input="missing";request.secondInput="missing";request.inputGrid="missing";request.secondGrid="missing";
  request.nativePointAdvection.timeStep=-999;request.nativePointAdvection.steps=64;
  request.nativePointAdvection.integration=vdb::NativePointIntegration::RK2;
  request.nativePointAdvection.velocityLayout=vdb::NativePointVelocityLayout::NativeMacIndex;
  (*request.nativePointAdvection.targetIndexToWorld)[3]=999;request.nativePointAdvection.maxWork=1;
  request.pointSelection.anyOf={"missing"};request.maxPoints=1;request.pointAttributes.maxAttributes=1;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()){EXPECT_FALSE(panel.result()->success);EXPECT_NE(panel.result()->message.find("ENABLE_OPENVDB=ON"),std::string::npos);return;}
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_NE(panel.result()->message.find("Advected 2 selected native points; retained 4 of 4 records"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("Integration executed; native motion no-op: no"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("Integrator: RK4"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("VELOCITY-index voxels"),std::string::npos);
  const auto ids=std::get<std::vector<int64_t>>(vdb::ReadPointAttribute(files.directory/"advected.vdb","id","advected").values);
  const auto velocities=std::get<std::vector<std::array<double,3>>>(vdb::ReadPointAttribute(files.directory/"advected.vdb","v","advected").values);
  const auto radii=std::get<std::vector<double>>(vdb::ReadPointAttribute(files.directory/"advected.vdb","pscale","advected").values);
  const auto signals=std::get<std::vector<double>>(vdb::ReadPointAttribute(files.directory/"advected.vdb","signal","advected").values);
  const auto labels=std::get<std::vector<std::string>>(vdb::ReadPointAttribute(files.directory/"advected.vdb","label","advected").values);
  const auto memberships=vdb::ReadPointGroup(files.directory/"advected.vdb","A","advected");
  vdb::PointNeighborOptions neighbor;neighbor.indexVoxelSize=8;const std::array<vdb::PointNeighborQuery,1> queries{{{{0,0,0},64}}};
  const auto found=vdb::QueryPointNeighbors(files.directory/"advected.vdb",queries,neighbor,"advected");
  const std::map<int64_t,double> expectedX{{INT64_MAX,-1},{INT64_MAX-1,.25},{INT64_MAX-2,3},{INT64_MAX-3,4}};
  const std::array<double,4> originalSignals{-0.,1e-200,3,4};const std::array<std::string,4> originalLabels{"a",std::string("b\0raw",5),"c","d"};
  ASSERT_EQ(found.queries.at(0).size(),4u);
  for(const auto& hit:found.queries.at(0)) {
    const auto i=hit.reference.ordinal;ASSERT_LT(i,ids.size());const size_t originalIndex=size_t(INT64_MAX-ids[i]);ASSERT_LT(originalIndex,4u);
    EXPECT_NEAR(hit.positionWorld[0],expectedX.at(ids[i]),1e-6);EXPECT_EQ(hit.positionWorld[1],originalIndex%2==0?-.5:0);EXPECT_EQ(hit.positionWorld[2],0);
    EXPECT_EQ(velocities[i],(std::array<double,3>{3,4,5}));EXPECT_EQ(radii[i],.5);EXPECT_EQ(labels[i],originalLabels[originalIndex]);
    EXPECT_EQ(signals[i],originalSignals[originalIndex]);EXPECT_EQ(std::signbit(signals[i]),std::signbit(originalSignals[originalIndex]));
    EXPECT_EQ(bool(memberships[i]),originalIndex%2==0);
  }
  EXPECT_TRUE(std::any_of(found.gridMetadata.begin(),found.gridMetadata.end(),[](const auto& item){return item.name=="artist"&&item.value=="advection artist";}));
  const auto inspected=vdb::InspectFile(files.directory/"advected.vdb");ASSERT_EQ(inspected.grids.size(),1u);ASSERT_TRUE(inspected.grids[0].indexToWorld);EXPECT_EQ(*inspected.grids[0].indexToWorld,target);
  EXPECT_EQ(bytes(files.directory/"source.vdb"),original);EXPECT_EQ(bytes(files.directory/"flow.vdb"),velocityBytes);
  request=submitted;request.output=files.path("capped.vdb");request.nativePointAdvection.maxWork=1;
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);EXPECT_FALSE(std::filesystem::exists(files.directory/"capped.vdb"));
}

TEST(OpenVdbAuthoring, NativePointAdvectionReportsSkippedIntegrationSeparatelyFromTargetMovementAndUiCaps) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  if(!vdb::IsAvailable())GTEST_SKIP()<<"OpenVDB is disabled";
  AuthoringFiles files;vdb::ProcessingOptions p;p.voxelSize=1;p.gridName="native";
  const std::array<vdb::PointRecord,1> points{{{{.25,0,0},{3,4,5},.5}}};(void)vdb::WritePoints(points,files.directory/"points.vdb",p);
  p.gridName="flow";(void)vdb::WriteConstantVelocity(files.directory/"flow.vdb",{.5,0,0},p);
  OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;request.operation=Operation::AdvectNativePoints;
  request.input=files.path("points.vdb");request.inputGrid="native";request.secondInput=files.path("flow.vdb");request.secondGrid="flow";
  request.output=files.path("mapped.vdb");request.processing.gridName="mapped";request.nativePointAdvection.timeStep=0;
  const std::array<double,16> target{-.5,0,0,2,0,.5,0,0,0,0,.5,0,0,0,0,1};request.nativePointAdvection.targetIndexToWorld=target;
  request.pointMotion.worldFromWorld[15]=0;request.pointAdvection.timeStep=NAN;request.pointAttributes.maxEdits=0;
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_FALSE(panel.result()->modelOutput);EXPECT_NE(panel.result()->message.find("Integration skipped; native motion no-op: no"),std::string::npos);
  const auto records=vdb::ReadPoints(files.directory/"mapped.vdb","mapped");ASSERT_EQ(records.size(),1u);EXPECT_EQ(records[0].position,(std::array<double,3>{.25,0,0}));
  const auto grid=vdb::InspectFile(files.directory/"mapped.vdb");ASSERT_TRUE(grid.grids.at(0).indexToWorld);EXPECT_EQ(*grid.grids.at(0).indexToWorld,target);
  request.output=files.path("report-limited.vdb");request.processing.maxInputBytes=32768;const auto retained=request.processing.maxInputBytes;
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);
  EXPECT_NE(panel.result()->message.find("UI report"),std::string::npos);EXPECT_EQ(request.processing.maxInputBytes,retained);EXPECT_FALSE(std::filesystem::exists(files.directory/"report-limited.vdb"));
}

TEST(OpenVdbAuthoring, NativePointAdvectionSubmissionOwnsEveryNestedSetting) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  std::promise<OpenVdbAuthoringRequest> captured;auto future=captured.get_future();
  OpenVdbAuthoringPanel panel([&](const auto& request){captured.set_value(request);return OpenVdbAuthoringResult{true,"captured",request.output,false};});
  OpenVdbAuthoringRequest request;request.operation=Operation::AdvectNativePoints;request.input="points.vdb";request.secondInput="flow.vdb";request.inputGrid="P";request.secondGrid="V";request.output="new.vdb";
  auto& o=request.nativePointAdvection;o.timeStep=-2;o.steps=7;o.integration=vdb::NativePointIntegration::RK4;o.velocityLayout=vdb::NativePointVelocityLayout::NativeMacIndex;
  o.groups.anyOf={"ignored-hidden"};o.targetIndexToWorld=std::array<double,16>{.5,0,0,1,0,-.5,0,2,0,0,.5,3,0,0,0,1};o.maxPositionErrorVoxels=.01;o.rejectActivityMerges=false;
  o.maxStepTravelVelocityVoxels=3;o.maxTravelVelocityVoxels=12;o.maxWork=123456789;o.maxGrids=17;o.maxMetadataBytes=234567;
  request.pointSelection.anyOf={"A"};request.pointSelection.allOf={"B"};request.pointSelection.noneOf={"C"};request.maxPoints=123;
  request.pointAttributes.maxAttributes=77;request.pointAttributes.maxGroups=88;request.pointAttributes.maxStringBytes=999;
  const auto original=request;ASSERT_TRUE(panel.submit(request));request.nativePointAdvection={};request.pointSelection={};request.input="mutated";request.secondInput="mutated";request.maxPoints=1;request.pointAttributes={};
  ASSERT_TRUE(settle(panel));ASSERT_EQ(future.wait_for(0s),std::future_status::ready);const auto actual=future.get();
  EXPECT_EQ(actual.input,original.input);EXPECT_EQ(actual.secondInput,original.secondInput);EXPECT_EQ(actual.inputGrid,original.inputGrid);EXPECT_EQ(actual.secondGrid,original.secondGrid);
  const auto& a=actual.nativePointAdvection;const auto& b=original.nativePointAdvection;
  EXPECT_EQ(a.timeStep,b.timeStep);EXPECT_EQ(a.steps,b.steps);EXPECT_EQ(a.integration,b.integration);EXPECT_EQ(a.velocityLayout,b.velocityLayout);EXPECT_EQ(a.groups.anyOf,b.groups.anyOf);EXPECT_EQ(a.targetIndexToWorld,b.targetIndexToWorld);
  EXPECT_EQ(a.maxPositionErrorVoxels,b.maxPositionErrorVoxels);EXPECT_EQ(a.rejectActivityMerges,b.rejectActivityMerges);EXPECT_EQ(a.maxStepTravelVelocityVoxels,b.maxStepTravelVelocityVoxels);EXPECT_EQ(a.maxTravelVelocityVoxels,b.maxTravelVelocityVoxels);
  EXPECT_EQ(a.maxWork,b.maxWork);EXPECT_EQ(a.maxGrids,b.maxGrids);EXPECT_EQ(a.maxMetadataBytes,b.maxMetadataBytes);
  EXPECT_EQ(actual.pointSelection.anyOf,original.pointSelection.anyOf);EXPECT_EQ(actual.pointSelection.allOf,original.pointSelection.allOf);EXPECT_EQ(actual.pointSelection.noneOf,original.pointSelection.noneOf);
  EXPECT_EQ(actual.maxPoints,original.maxPoints);EXPECT_EQ(actual.pointAttributes.maxAttributes,original.pointAttributes.maxAttributes);EXPECT_EQ(actual.pointAttributes.maxGroups,original.pointAttributes.maxGroups);EXPECT_EQ(actual.pointAttributes.maxStringBytes,original.pointAttributes.maxStringBytes);
}

TEST(OpenVdbAuthoring, DefaultExecutorMovesSelectedNativePointsAndSnapshotsMapsGroupsAndLimits) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::MoveNativePoints;request.input=files.path("source.vdb");request.output=files.path("moved.vdb");
  request.inputGrid="native";request.processing.gridName="moved";request.pointSelection.anyOf={"A"};
  request.pointMotion.worldFromWorld[3]=10;
  const std::array<double,16> target{.5,0,0,1,0,-.5,0,2,0,0,.5,3,0,0,0,1};
  request.pointMotion.targetIndexToWorld=target;
  if(vdb::IsAvailable()) {
    const std::array<vdb::PointRecord,4> records{{{{-2,0,0},{3,4,5},.5},{{.25,0,0},{3,4,5},.5},{{2,0,0},{3,4,5},.5},{{4,0,0},{3,4,5},.5}}};
    vdb::ProcessingOptions p;p.voxelSize=1;p.gridName="native";
    (void)vdb::WritePoints(records,files.directory/"raw.vdb",p);
    vdb::PointAttributeEdit id;id.name="id";id.values=std::vector<int64_t>{INT64_MAX,INT64_MAX-1,INT64_MAX-2,INT64_MAX-3};
    vdb::PointGroupEdit group;group.name="A";group.membership={1,0,1,0};
    const std::array edits{id};const std::array groups{group};
    (void)vdb::WritePointAttributeEdits(files.directory/"raw.vdb",files.directory/"source.vdb",edits,groups,"native",p);
  }
  const auto bytes=[](const std::filesystem::path& source) {std::ifstream stream(source,std::ios::binary);return std::string(std::istreambuf_iterator<char>(stream),{});};
  const auto sourceBytes=vdb::IsAvailable()?bytes(files.directory/"source.vdb"):std::string{};
  ASSERT_TRUE(panel.submit(request));request.pointMotion.worldFromWorld[3]=999;
  (*request.pointMotion.targetIndexToWorld)[3]=999;request.pointMotion.maxWork=1;
  request.pointSelection.anyOf={"missing"};request.maxPoints=1;request.pointAttributes.maxAttributes=1;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()) {EXPECT_FALSE(panel.result()->success);return;}
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_NE(panel.result()->message.find("Moved 2 selected native points; retained 4 of 4 records"),std::string::npos);
  const auto ids=std::get<std::vector<int64_t>>(vdb::ReadPointAttribute(files.directory/"moved.vdb","id","moved").values);
  const auto velocities=std::get<std::vector<std::array<double,3>>>(vdb::ReadPointAttribute(files.directory/"moved.vdb","v","moved").values);
  const auto radii=std::get<std::vector<double>>(vdb::ReadPointAttribute(files.directory/"moved.vdb","pscale","moved").values);
  const auto memberships=vdb::ReadPointGroup(files.directory/"moved.vdb","A","moved");
  vdb::PointNeighborOptions neighbors;neighbors.indexVoxelSize=8;
  const std::array<vdb::PointNeighborQuery,1> queries{{{{0,0,0},64}}};
  const auto found=vdb::QueryPointNeighbors(files.directory/"moved.vdb",queries,neighbors,"moved");
  ASSERT_EQ(found.queries.at(0).size(),4u);
  const std::map<int64_t,double> expected{{INT64_MAX,8},{INT64_MAX-1,.25},{INT64_MAX-2,12},{INT64_MAX-3,4}};
  for(const auto& hit:found.queries.at(0)) {
    const auto ordinal=hit.reference.ordinal;ASSERT_LT(ordinal,ids.size());
    EXPECT_NEAR(hit.positionWorld[0],expected.at(ids[ordinal]),1e-6);
    EXPECT_EQ(hit.positionWorld[1],0);EXPECT_EQ(hit.positionWorld[2],0);
    EXPECT_EQ(velocities[ordinal],(std::array<double,3>{3,4,5}));EXPECT_EQ(radii[ordinal],.5);
    EXPECT_EQ(bool(memberships[ordinal]),ids[ordinal]==INT64_MAX || ids[ordinal]==INT64_MAX-2);
  }
  const auto inspected=vdb::InspectFile(files.directory/"moved.vdb");ASSERT_EQ(inspected.grids.size(),1u);
  ASSERT_TRUE(inspected.grids[0].indexToWorld);EXPECT_EQ(*inspected.grids[0].indexToWorld,target);
  EXPECT_EQ(bytes(files.directory/"source.vdb"),sourceBytes);
  request.output=files.path("bad.vdb");ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);EXPECT_FALSE(std::filesystem::exists(files.directory/"bad.vdb"));
}

TEST(OpenVdbAuthoring, DefaultExecutorTransfersNativeColumnsWithDensityCancellationAndImmutableOptions) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::TransferPointAttribute;request.input=files.path("source.vdb");request.output=files.path("mean.vdb");
  request.inputGrid="native";request.processing.gridName="transferred";request.pointField="signal";request.pointSelection.anyOf={"A"};
  request.pointTransfer.mode=vdb::PointTransferMode::WeightedMean;
  request.pointTransfer.weightAttribute="mass";request.pointTransfer.companion=vdb::PointTransferCompanion::WorldDensity;
  request.pointTransfer.companionGridName="density";
  const std::array<double,16> target{.5,0,0,1,0,.5,0,2,0,0,-.5,3,0,0,0,1};
  request.pointTransfer.targetIndexToWorld=target;
  if(vdb::IsAvailable()) {
    const std::array<vdb::PointRecord,3> records{{{{0,0,0},{},.5},{{0,0,0},{},.5},{{8,0,0},{},.5}}};
    vdb::ProcessingOptions p;p.voxelSize=1;p.gridName="native";(void)vdb::WritePoints(records,files.directory/"raw.vdb",p);
    vdb::PointAttributeEdit signal;signal.name="signal";signal.values=std::vector<double>{2,4,8};
    vdb::PointAttributeEdit mass;mass.name="mass";mass.values=std::vector<double>{1,3,2};
    vdb::PointGroupEdit group;group.name="A";group.membership={1,1,0};
    const std::array attributes{signal,mass};const std::array groups{group};
    (void)vdb::WritePointAttributeEdits(files.directory/"raw.vdb",files.directory/"source.vdb",attributes,groups,"native",p);
  }
  const auto bytes=[](const std::filesystem::path& source) {std::ifstream stream(source,std::ios::binary);return std::string(std::istreambuf_iterator<char>(stream),{});};
  const auto sourceBytes=vdb::IsAvailable()?bytes(files.directory/"source.vdb"):std::string{};
  const auto submitted=request;
  ASSERT_TRUE(panel.submit(request));request.pointField="P";request.pointSelection.anyOf={"missing"};
  request.pointTransfer.mode=vdb::PointTransferMode::WeightedSum;request.pointTransfer.storage=vdb::PointTransferStorage::Float;
  request.pointTransfer.weightAttribute="missing";request.pointTransfer.targetIndexToWorld[3]=999;
  request.pointTransfer.maxWork=1;request.maxPoints=1;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()) {EXPECT_FALSE(panel.result()->success);return;}
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_NE(panel.result()->message.find("Transferred 2 of 3 native points"),std::string::npos);
  const vdb::DenseRegion cell{{-2,-4,6},{1,1,1}};
  const auto field=vdb::ReadDenseVdb(files.directory/"mean.vdb",cell,"transferred");
  const auto density=vdb::ReadDenseVdb(files.directory/"mean.vdb",cell,"density");
  EXPECT_DOUBLE_EQ(std::get<std::vector<double>>(field.values).at(0),3.5);
  EXPECT_DOUBLE_EQ(std::get<std::vector<double>>(density.values).at(0),32);
  EXPECT_EQ(field.active,(std::vector<uint8_t>{1}));EXPECT_EQ(density.active,field.active);
  for(const auto& grid:vdb::InspectFile(files.directory/"mean.vdb").grids) {
    ASSERT_TRUE(grid.indexToWorld);EXPECT_EQ(*grid.indexToWorld,target);
  }
  request=submitted;request.output=files.path("bad.vdb");request.pointTransfer.maxWork=1;
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  EXPECT_FALSE(panel.result()->success);EXPECT_FALSE(std::filesystem::exists(files.directory/"bad.vdb"));
  vdb::PointAttributeEdit signal;signal.operation=vdb::PointEditOperation::Set;signal.name="signal";signal.values=std::vector<double>{1e308,-1e308,8};
  vdb::PointAttributeEdit mass;mass.operation=vdb::PointEditOperation::Set;mass.name="mass";mass.values=std::vector<double>{1e154,1e154,2};
  const std::array edits{signal,mass};vdb::ProcessingOptions p;p.gridName="native";
  (void)vdb::WritePointAttributeEdits(files.directory/"source.vdb",files.directory/"cancel.vdb",edits,{},"native",p);
  const auto cancelBytes=bytes(files.directory/"cancel.vdb");
  request=submitted;request.input=files.path("cancel.vdb");request.output=files.path("sum.vdb");
  request.pointTransfer.mode=vdb::PointTransferMode::WeightedSum;
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;EXPECT_FALSE(panel.result()->modelOutput);
  const auto zero=vdb::ReadDenseVdb(files.directory/"sum.vdb",cell,"transferred");
  EXPECT_DOUBLE_EQ(std::get<std::vector<double>>(zero.values).at(0),0);EXPECT_EQ(zero.active,(std::vector<uint8_t>{1}));
  const auto support=vdb::ReadDenseVdb(files.directory/"sum.vdb",cell,"density");
  EXPECT_NEAR(std::get<std::vector<double>>(support.values).at(0),1.6e155,1e140);
  EXPECT_EQ(support.active,zero.active);EXPECT_EQ(bytes(files.directory/"source.vdb"),sourceBytes);
  EXPECT_EQ(bytes(files.directory/"cancel.vdb"),cancelBytes);
}

TEST(OpenVdbAuthoring, DefaultExecutorAdvectsTypedFieldsWithImmutableOptions) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::FieldAdvection;request.input=files.path("source.vdb");
  request.secondInput=files.path("velocity.vdb");request.output=files.path("advected.vdb");
  request.fieldAdvection.timeStep=1;request.fieldAdvection.scheme=vdb::FieldAdvectionScheme::Bfecc;
  request.processing.gridName="transported";
  if(vdb::IsAvailable()) {
    vdb::DenseArrayVolume volume;volume.values=std::vector<double>{10};volume.background=3.0;volume.active={1};
    (void)vdb::WriteDenseVdb(files.directory/"source.vdb",volume);
    (void)vdb::WriteConstantVelocity(files.directory/"velocity.vdb",{1,0,0});
  }
  ASSERT_TRUE(panel.submit(request));request.fieldAdvection.maxWorkItems=1;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()) {EXPECT_FALSE(panel.result()->success);return;}
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;EXPECT_FALSE(panel.result()->modelOutput);
  const auto volume=vdb::ReadDenseVdb(files.directory/"advected.vdb",{{0,0,0},{2,1,1}},"transported");
  const auto& values=std::get<std::vector<double>>(volume.values);
  ASSERT_EQ(values.size(),2u);EXPECT_DOUBLE_EQ(values[0],3);EXPECT_DOUBLE_EQ(values[1],10);
  EXPECT_EQ(volume.active,(std::vector<uint8_t>{0,1}));
  EXPECT_NE(panel.result()->message.find("bounded work items"),std::string::npos);
  request.output=files.path("bad.vdb");ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);EXPECT_FALSE(std::filesystem::exists(files.directory/"bad.vdb"));
}

TEST(OpenVdbAuthoring, DefaultExecutorWritesPotentialFlowWithStoredResidualAndImmutableLimits) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::PotentialFlow;request.input=files.path("source.vdb");request.output=files.path("flow.vdb");
  request.potentialFlow.farFieldVelocity={0,0,0};request.potentialFlow.colliderVelocity={0,0,0};
  if(vdb::IsAvailable()) {
    vdb::ProcessingOptions p;p.voxelSize=.25;(void)vdb::WriteSphere(files.directory/"source.vdb",{},p);
  }
  ASSERT_TRUE(panel.submit(request));request.potentialFlow.maxWorkItems=1;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()) {EXPECT_FALSE(panel.result()->success);return;}
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;EXPECT_FALSE(panel.result()->modelOutput);
  const auto inspected=vdb::InspectFile(files.directory/"flow.vdb");ASSERT_EQ(inspected.grids.size(),4u);
  std::set<std::string> names;for(const auto& grid:inspected.grids)names.insert(grid.name);
  EXPECT_EQ(names,(std::set<std::string>{"flow","potential","flow_domain","neumann"}));
  EXPECT_NE(panel.result()->message.find("stored-potential residual 0"),std::string::npos);
  request.output=files.path("bad.vdb");ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);EXPECT_FALSE(std::filesystem::exists(files.directory/"bad.vdb"));
}

TEST(OpenVdbAuthoring, DefaultExecutorQueriesInclusiveNativePointReferencesWithoutOutput) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::PointNeighbors;request.input=files.path("source.vdb");request.distance=0;
  request.pointNeighbors.indexVoxelSize=1e-6;
  if(vdb::IsAvailable()) {
    const std::array<vdb::PointRecord,3> records{{{{0,0,0},{},.1},{{0,0,0},{},.2},{{1,0,0},{},.3}}};
    (void)vdb::WritePoints(records,files.directory/"source.vdb");
  }
  ASSERT_TRUE(panel.submit(request));request.pointNeighbors.maxWork=1;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()) {EXPECT_FALSE(panel.result()->success);return;}
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_TRUE(panel.result()->output.empty());EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_NE(panel.result()->message.find("2 neighbors from 3 selected native points"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("local index"),std::string::npos);
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);
}

TEST(OpenVdbAuthoring, DefaultExecutorRebucketsNativePointsWithTinyTranslatedMaskAndCounts) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::IndexedPointMask;request.input=files.path("source.vdb");request.output=files.path("mask.vdb");
  request.processing.gridName="occupied";request.indexedPointMask.voxelSize=1e-6;
  request.indexedPointMask.originWorld={1e-6,-2e-6,3e-6};request.indexedPointMask.includeCounts=true;
  if(vdb::IsAvailable()) {
    const std::array<vdb::PointRecord,3> records{{{{0,0,0},{},.1},{{0,0,0},{},.2},{{1,0,0},{},.3}}};
    (void)vdb::WritePoints(records,files.directory/"source.vdb");
  }
  ASSERT_TRUE(panel.submit(request));request.indexedPointMask.maxWork=1;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()) {EXPECT_FALSE(panel.result()->success);return;}
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;EXPECT_FALSE(panel.result()->modelOutput);
  const auto inspected=vdb::InspectFile(files.directory/"mask.vdb");ASSERT_EQ(inspected.grids.size(),2u);
  for(const auto& grid:inspected.grids)EXPECT_EQ(grid.activeVoxelCount,2u);
  EXPECT_NE(panel.result()->message.find("3 of 3 native points"),std::string::npos);
  request.output=files.path("bad.vdb");ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);EXPECT_FALSE(std::filesystem::exists(files.directory/"bad.vdb"));
}

TEST(OpenVdbAuthoring, DefaultExecutorWritesRegionMasksAndPreservesRequestSettings) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::Regions;request.regionsOperation=vdb::RegionsOperation::SignedInterior;
  request.input=files.path("source.vdb");request.output=files.path("interior.vdb");request.processing.gridName="inside";
  if(vdb::IsAvailable()) {
    vdb::ProcessingOptions p;p.voxelSize=.25;(void)vdb::WriteSphere(files.directory/"source.vdb",{},p);
  }
  ASSERT_TRUE(panel.submit(request));request.regions.maxWorkItems=1;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()) {EXPECT_FALSE(panel.result()->success);return;}
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_NE(panel.result()->message.find("251 active voxels"),std::string::npos);
  EXPECT_EQ(vdb::InspectFile(files.directory/"interior.vdb").grids[0].activeVoxelCount,251u);
  request.output=files.path("bad.vdb");ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);
  EXPECT_FALSE(std::filesystem::exists(files.directory/"bad.vdb"));
}

TEST(OpenVdbAuthoring, DefaultExecutorReportsApproximateSurfaceGeometryWithoutAnOutputFile) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::SurfaceQuery;request.input=files.path("source.vdb");request.queryPosition={1.2,0,0};
  if(vdb::IsAvailable())(void)vdb::WriteSphere(files.directory/"source.vdb");
  ASSERT_TRUE(panel.submit(request));request.surfaceQuery.maxWork=1;request.queryPosition={0,0,0};
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()) {EXPECT_FALSE(panel.result()->success);return;}
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_TRUE(panel.result()->output.empty());EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_NE(panel.result()->message.find("Approximate surface point cloud"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("Local normal:"),std::string::npos);
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  EXPECT_FALSE(panel.result()->success);
}

TEST(OpenVdbAuthoring, DefaultExecutorFracturesMatchingFieldsWithImmutableBudgets) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::Fracture;request.input=files.path("source.vdb");request.secondInput=files.path("cutter.vdb");
  request.output=files.path("pieces.vdb");request.fracture.maxPieces=8;
  if(vdb::IsAvailable()) {
    vdb::ProcessingOptions p;p.voxelSize=.25;(void)vdb::WriteSphere(files.directory/"source.vdb",{},p);
    vdb::SphereOptions cutter;cutter.radius=.6;cutter.center={.5,0,0};(void)vdb::WriteSphere(files.directory/"cutter.vdb",cutter,p);
  }
  ASSERT_TRUE(panel.submit(request));request.fracture.maxWorkItems=1;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()) {EXPECT_FALSE(panel.result()->success);return;}
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;EXPECT_TRUE(panel.result()->modelOutput);
  EXPECT_NE(panel.result()->message.find("negative sample partition"),std::string::npos);
  EXPECT_GE(vdb::InspectFile(files.directory/"pieces.vdb").grids.size(),2u);
  request.output=files.path("bad.vdb");ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);
  EXPECT_FALSE(std::filesystem::exists(files.directory/"bad.vdb"));
}

TEST(OpenVdbAuthoring, DefaultExecutorExportsBoundedSphereJsonWithImmutableSettings) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::VolumeSpheres;request.input=files.path("source.vdb");request.output=files.path("spheres.json");
  request.volumeSpheres.candidateCount=128;request.volumeSpheres.maxCount=8;
  request.volumeSpheres.maxRadiusWorld=.2;
  if(vdb::IsAvailable()) {
    vdb::ProcessingOptions p;p.voxelSize=.25;(void)vdb::WriteSphere(files.directory/"source.vdb",{},p);
  }
  ASSERT_TRUE(panel.submit(request));request.volumeSpheres.maxCount=0;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()) {EXPECT_FALSE(panel.result()->success);return;}
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_NE(panel.result()->message.find("fitted spheres"),std::string::npos);
  EXPECT_GT(std::filesystem::file_size(files.directory/"spheres.json"),0u);
  request.output=files.path("bad.json");ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);
  EXPECT_FALSE(std::filesystem::exists(files.directory/"bad.json"));
}

TEST(OpenVdbAuthoring, DefaultExecutorRunsSweepingAndTwoGridFieldExtension) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::FastSweep;request.input=files.path("plane.vdb");request.output=files.path("distance.vdb");
  if(vdb::IsAvailable()) {
    vdb::DenseArrayVolume plane;plane.region={{-4,-1,-1},{9,3,3}};
    plane.linear={.25,0,0,0,.25,0,0,0,.25};plane.gridClass=vdb::DenseGridClass::LevelSet;
    std::vector<float> values;for(int z=-1;z<=1;++z)for(int y=-1;y<=1;++y)for(int x=-4;x<=4;++x)values.push_back(float(.35*(x+.5)));
    plane.values=values;plane.background=3.f;plane.active.assign(81,1);(void)vdb::WriteDenseVdb(files.directory/"plane.vdb",plane);
    vdb::DenseArrayVolume field;field.linear=plane.linear;field.values=std::vector<float>{7.f};field.background=7.f;
    (void)vdb::WriteDenseVdb(files.directory/"field.vdb",field);
  }
  ASSERT_TRUE(panel.submit(request));request.fastSweeping.maxWorkItems=1;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()) {EXPECT_FALSE(panel.result()->success);return;}
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_NEAR(vdb::SampleScalar(files.directory/"distance.vdb",{.75,0,0}),.875,.002);
  request.operation=Operation::FastExtend;request.secondInput=files.path("field.vdb");
  request.output=files.path("extended.vdb");request.fastSweeping={};request.fastSweeping.side=vdb::FastSweepingSide::GreaterThanIsovalue;
  ASSERT_TRUE(panel.submit(request));request.fastSweeping.side=vdb::FastSweepingSide::LessThanIsovalue;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_NE(panel.result()->message.find("Extended field: surface_extension"),std::string::npos);
  EXPECT_EQ(vdb::InspectFile(files.directory/"extended.vdb").grids.size(),2u);
  EXPECT_EQ(vdb::SampleScalar(files.directory/"extended.vdb",{.75,0,0},"surface_extension"),7.);
  request.output=files.path("bad.vdb");request.fastSweeping.maxWorkItems=1;
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  EXPECT_FALSE(panel.result()->success);EXPECT_FALSE(std::filesystem::exists(files.directory/"bad.vdb"));
}

TEST(OpenVdbAuthoring, DefaultExecutorSnapshotsNormalizationActivityMaskAndReportsSelection) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::SdfFilter;request.input=files.path("source.vdb");request.output=files.path("normalized.vdb");
  request.sdfFilterOperation=vdb::SdfFilterOperation::Normalize;
  request.sdfFilter.normalizationMaskInput=files.directory/"activity.vdb";
  request.sdfFilter.normalizationMaskGrid="field";
  if(vdb::IsAvailable()) {
    vdb::ProcessingOptions p;p.voxelSize=.25;(void)vdb::WriteSphere(files.directory/"source.vdb",{},p);
    vdb::DenseArrayVolume mask;mask.region={{4,0,0},{3,1,1}};
    mask.linear={.25,0,0,0,.25,0,0,0,.25};mask.values=std::vector<double>{0,9,0};
    mask.background=7.;mask.active={1,0,1};(void)vdb::WriteDenseVdb(files.directory/"activity.vdb",mask);
  }
  ASSERT_TRUE(panel.submit(request));request.sdfFilter.normalizationMaskInput=files.directory/"missing.vdb";
  request.sdfFilter.normalizationMaskGrid="missing";
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()) {
    EXPECT_FALSE(panel.result()->success);EXPECT_NE(panel.result()->message.find("unavailable"),std::string::npos);return;
  }
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_NE(panel.result()->message.find("Normalization activity selection: 2 existing active voxels."),std::string::npos);
  EXPECT_TRUE(panel.result()->modelOutput);
  const auto source=vdb::ReadDenseVdb(files.directory/"source.vdb",{{0,0,0},{9,1,1}});
  const auto output=vdb::ReadDenseVdb(files.directory/"normalized.vdb",{{0,0,0},{9,1,1}});
  EXPECT_EQ(source.active,output.active);
  const auto& original=std::get<std::vector<float>>(source.values);
  const auto& actual=std::get<std::vector<float>>(output.values);
  for(size_t i=0;i<9;++i)if(i!=4 && i!=6)EXPECT_EQ(original[i],actual[i]);
  request.output=files.path("bad.vdb");ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);
  EXPECT_FALSE(std::filesystem::exists(files.directory/"bad.vdb"));
}

TEST(OpenVdbAuthoring, DefaultExecutorSetsSdfBackgroundsAndPropagatesSignsWithImmutableWidths) {
  namespace vdb=container::geometry::vdb;
  using Operation=container::ui::OpenVdbAuthoringOperation;
  if(!vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB is disabled";
  AuthoringFiles files;
  vdb::ProcessingOptions processing; processing.voxelSize=.25;
  const auto original=vdb::WriteSphere(files.directory/"source.vdb",{},processing);
  OpenVdbAuthoringPanel panel; OpenVdbAuthoringRequest request;
  request.operation=Operation::SdfBackground; request.input=files.path("source.vdb"); request.output=files.path("background.vdb");
  request.sdfBackground.outsideWidth=1.; request.sdfBackground.insideWidth=1.25;
  ASSERT_TRUE(panel.submit(request)); request.sdfBackground.insideWidth=.01;
  ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result()); ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_TRUE(panel.result()->modelOutput);
  EXPECT_EQ(vdb::InspectFile(files.directory/"background.vdb").grids[0].activeVoxelCount,original.activeVoxelCount);
  EXPECT_EQ(vdb::SampleScalar(files.directory/"background.vdb",{0,0,0}),-1.25);
  EXPECT_EQ(vdb::SampleScalar(files.directory/"background.vdb",{10,0,0}),1.);
  request.input=request.output; request.output=files.path("flood.vdb"); request.sdfBackground.insideWidth=1.25;
  request.sdfBackgroundOperation=vdb::SdfBackgroundOperation::FloodFill;
  ASSERT_TRUE(panel.submit(request)); request.sdfBackground.maxWorkItems=1;
  ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result()); ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_TRUE(panel.result()->modelOutput);
  EXPECT_EQ(vdb::SampleScalar(files.directory/"flood.vdb",{0,0,0}),-1.25);
  request.output=files.path("limited.vdb");
  ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  EXPECT_FALSE(panel.result()->success); EXPECT_FALSE(std::filesystem::exists(files.directory/"limited.vdb"));
}

TEST(OpenVdbAuthoring, DefaultExecutorFiltersOrdinaryFieldWithExplicitUnits) {
  namespace vdb=container::geometry::vdb;
  using Operation=container::ui::OpenVdbAuthoringOperation;
  if (!vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB is disabled";
  AuthoringFiles files; vdb::DenseArrayVolume volume;
  volume.values=std::vector<std::array<double,3>>{{1,2,3}};
  volume.background=std::array<double,3>{}; volume.active={1};
  (void)vdb::WriteDenseVdb(files.directory/"field.vdb",volume);
  OpenVdbAuthoringPanel panel; OpenVdbAuthoringRequest request;
  request.operation=Operation::FilterField; request.input=files.path("field.vdb"); request.output=files.path("offset.vdb");
  request.fieldFilterOperation=vdb::FieldFilterOperation::Offset;
  request.fieldFilter.offset={1,-2,.5}; request.fieldFilter.iterations=2;
  ASSERT_TRUE(panel.submit(request)); request.fieldFilter.maxWorkItems=1;
  ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_EQ(vdb::SampleVector(files.directory/"offset.vdb",{0,0,0}),(std::array<double,3>{3,-2,4}));
  request.output=files.path("limited.vdb");
  ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  EXPECT_FALSE(panel.result()->success); EXPECT_FALSE(std::filesystem::exists(files.directory/"limited.vdb"));
}

TEST(OpenVdbAuthoring, NativePrimitiveAuthoringUsesIndependentIoAndSnapshotsGeometryFormatAndCaps) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::NanoPrimitive;request.input=std::string("\xff",1);request.secondInput=std::string("\xc0\xaf",2);request.inputGrid="unused";
  request.processing.maxInputBytes=0;request.processing.maxVoxelCount=0;request.output=files.path("native.nvdb");
  request.nanoPrimitiveShape=vdb::NanoVdbPrimitiveShape::Box;auto& settings=request.nanoPrimitive;
  settings.gridClass=vdb::NanoVdbPrimitiveClass::Fog;settings.storage=vdb::NanoVdbPrimitiveStorage::Double;
  settings.voxelSize=.125;settings.halfWidth=2.5;settings.center={.25,.25,.375};settings.origin={.125,.25,.375};
  settings.boxDimensions={1,1.5,2};settings.gridName="submitted-fog";settings.statistics=vdb::NanoVdbStatisticsMode::MinMax;
  settings.radius=9;settings.majorRadius=8;settings.minorRadius=7; // Hidden shape parameters do not affect this UI operation.
  request.nanoWrite.raw=true;request.nanoWrite.checksum=vdb::NanoVdbChecksum::Header;
  const auto submitted=request;
  ASSERT_TRUE(panel.submit(request));request.nanoPrimitiveShape=vdb::NanoVdbPrimitiveShape::Torus;
  request.nanoPrimitive={};request.nanoPrimitive.gridName="mutated";request.nanoPrimitive.voxelSize=0;
  request.nanoWrite={};request.nanoLimits.maxWork=1;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::NanoVdbInterchangeCapabilities().nativeIo){EXPECT_FALSE(panel.result()->success);EXPECT_NE(panel.result()->message.find("ENABLE_NANOVDB_IO"),std::string::npos);EXPECT_FALSE(std::filesystem::exists(files.directory/"native.nvdb"));return;}
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;EXPECT_FALSE(panel.result()->modelOutput);EXPECT_EQ(panel.result()->output,submitted.output);
  EXPECT_NE(panel.result()->message.find("axis-aligned box fog (Double)"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("maximum absolute value error"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("coefficient discrepancy in voxels"),std::string::npos);
  const auto checked=vdb::ValidateNanoVdbFile(files.directory/"native.nvdb");ASSERT_EQ(checked.grids.size(),1u);
  EXPECT_TRUE(checked.raw);EXPECT_EQ(checked.grids[0].name,"submitted-fog");EXPECT_EQ(checked.grids[0].valueType,"double");EXPECT_EQ(checked.grids[0].gridClass,"FOG");EXPECT_EQ(checked.grids[0].checksum,vdb::NanoVdbChecksum::Header);
  if(vdb::NanoVdbInterchangeCapabilities().openVdbConversion){(void)vdb::WriteVdbFromNanoVdb(files.directory/"native.nvdb",files.directory/"converted.vdb",{},true);const auto sampled=vdb::ReadDenseVdb(files.directory/"converted.vdb",{{4,0,0},{1,1,1}},"submitted-fog");EXPECT_EQ(sampled.translation,submitted.nanoPrimitive.origin);EXPECT_EQ(sampled.linear,(std::array<double,9>{.125,0,0,0,.125,0,0,0,.125}));ASSERT_TRUE(std::holds_alternative<std::vector<double>>(sampled.values));EXPECT_NEAR(std::get<std::vector<double>>(sampled.values).at(0),.4,1e-14);}
  request=submitted;request.output=files.path("limited.nvdb");request.nanoLimits.maxWork=1;ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);EXPECT_FALSE(std::filesystem::exists(files.directory/"limited.nvdb"));
  request=submitted;request.output=files.path("report-limited.nvdb");request.nanoLimits.maxWorkspaceBytes=32768;ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);EXPECT_NE(panel.result()->message.find("UI report"),std::string::npos);EXPECT_FALSE(std::filesystem::exists(files.directory/"report-limited.nvdb"));EXPECT_EQ(request.nanoLimits.maxWorkspaceBytes,32768u);
  std::ifstream before(files.directory/"native.nvdb",std::ios::binary);const std::string bytes(std::istreambuf_iterator<char>(before),{});request=submitted;ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);std::ifstream after(files.directory/"native.nvdb",std::ios::binary);EXPECT_EQ(std::string(std::istreambuf_iterator<char>(after),{}),bytes);
}

TEST(OpenVdbAuthoring, NativePrimitiveSubmissionOwnsEveryNestedControlWithoutNativeAvailability) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  std::promise<OpenVdbAuthoringRequest> captured;auto future=captured.get_future();
  OpenVdbAuthoringPanel panel([&](const auto& request){captured.set_value(request);return OpenVdbAuthoringResult{true,"captured",request.output,false};});
  OpenVdbAuthoringRequest request;request.operation=Operation::NanoPrimitive;request.output="native.nvdb";
  request.nanoPrimitiveShape=vdb::NanoVdbPrimitiveShape::Torus;auto& o=request.nanoPrimitive;
  o.gridClass=vdb::NanoVdbPrimitiveClass::Fog;o.storage=vdb::NanoVdbPrimitiveStorage::Double;o.voxelSize=.25;o.halfWidth=4;o.center={1,2,3};o.origin={-.5,.25,-.75};o.radius=4;o.boxDimensions={5,6,7};o.majorRadius=2;o.minorRadius=.5;o.gridName="owned";o.statistics=vdb::NanoVdbStatisticsMode::Bounds;
  request.nanoWrite.codec=vdb::NanoVdbCodec::Zip;request.nanoWrite.checksum=vdb::NanoVdbChecksum::Header;request.nanoWrite.raw=true;request.nanoWrite.allowFloatMapRounding=true;
  auto& l=request.nanoLimits;l.maxInputBytes=12345;l.maxNativeBytes=23456;l.maxWorkspaceBytes=34567;l.maxActiveVoxels=45678;l.maxStoredLeafVoxels=56789;l.maxNodes=67890;l.maxWork=78901;l.maxGrids=2;l.maxNameBytes=37;l.maxMetadataBytes=89;
  const auto original=request;ASSERT_TRUE(panel.submit(request));request.nanoPrimitiveShape=vdb::NanoVdbPrimitiveShape::Sphere;request.nanoPrimitive={};request.nanoWrite={};request.nanoLimits={};request.output="mutated.vdb";
  ASSERT_TRUE(settle(panel));ASSERT_EQ(future.wait_for(0s),std::future_status::ready);const auto actual=future.get();
  EXPECT_EQ(actual.nanoPrimitiveShape,original.nanoPrimitiveShape);EXPECT_EQ(actual.output,original.output);const auto& a=actual.nanoPrimitive;const auto& b=original.nanoPrimitive;
  EXPECT_EQ(a.gridClass,b.gridClass);EXPECT_EQ(a.storage,b.storage);EXPECT_EQ(a.voxelSize,b.voxelSize);EXPECT_EQ(a.halfWidth,b.halfWidth);EXPECT_EQ(a.center,b.center);EXPECT_EQ(a.origin,b.origin);EXPECT_EQ(a.radius,b.radius);EXPECT_EQ(a.boxDimensions,b.boxDimensions);EXPECT_EQ(a.majorRadius,b.majorRadius);EXPECT_EQ(a.minorRadius,b.minorRadius);EXPECT_EQ(a.gridName,b.gridName);EXPECT_EQ(a.statistics,b.statistics);
  EXPECT_EQ(actual.nanoWrite.codec,original.nanoWrite.codec);EXPECT_EQ(actual.nanoWrite.checksum,original.nanoWrite.checksum);EXPECT_EQ(actual.nanoWrite.raw,original.nanoWrite.raw);EXPECT_EQ(actual.nanoWrite.allowFloatMapRounding,original.nanoWrite.allowFloatMapRounding);
  const auto& x=actual.nanoLimits;const auto& y=original.nanoLimits;EXPECT_EQ(x.maxInputBytes,y.maxInputBytes);EXPECT_EQ(x.maxNativeBytes,y.maxNativeBytes);EXPECT_EQ(x.maxWorkspaceBytes,y.maxWorkspaceBytes);EXPECT_EQ(x.maxActiveVoxels,y.maxActiveVoxels);EXPECT_EQ(x.maxStoredLeafVoxels,y.maxStoredLeafVoxels);EXPECT_EQ(x.maxNodes,y.maxNodes);EXPECT_EQ(x.maxWork,y.maxWork);EXPECT_EQ(x.maxGrids,y.maxGrids);EXPECT_EQ(x.maxNameBytes,y.maxNameBytes);EXPECT_EQ(x.maxMetadataBytes,y.maxMetadataBytes);
}

TEST(OpenVdbAuthoring, NativeFileJobsSnapshotLimitsAndExposeAvailabilityIndependently) {
  namespace vdb=container::geometry::vdb;
  using Operation=container::ui::OpenVdbAuthoringOperation;
  const auto caps=vdb::NanoVdbInterchangeCapabilities();
  AuthoringFiles files; OpenVdbAuthoringPanel panel; OpenVdbAuthoringRequest request;
  const auto run=[&] {
    if (!panel.submit(request) || !settle(panel) || !panel.result()) return false;
    EXPECT_TRUE(panel.result()->success) << panel.result()->message;
    return panel.result()->success;
  };
  request.operation=Operation::NanoInspect; request.input=files.path("missing.nvdb");
  if (!caps.nativeIo) {
    ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
    EXPECT_FALSE(panel.result()->success);
    EXPECT_NE(panel.result()->message.find("ENABLE_NANOVDB_IO"),std::string::npos);
    return;
  }
  if (!caps.openVdbConversion) {
    request.operation=Operation::VdbToNano; request.output=files.path("bad.nvdb");
    ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
    EXPECT_FALSE(panel.result()->success);
    EXPECT_NE(panel.result()->message.find("ENABLE_OPENVDB"),std::string::npos);
    return;
  }
  request.operation=Operation::Sphere; request.processing.voxelSize=.25;
  request.output=files.path("sphere.vdb"); ASSERT_TRUE(run());
  request.operation=Operation::VdbToNano; request.input=request.output;
  request.output=files.path("sphere.nvdb");
  ASSERT_TRUE(panel.submit(request)); request.nanoLimits.maxWork=1;
  ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_FALSE(panel.result()->modelOutput);
  request.output=files.path("limited.nvdb");
  ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  EXPECT_FALSE(panel.result()->success);
  EXPECT_FALSE(std::filesystem::exists(files.directory/"limited.nvdb"));
  request.nanoLimits={}; request.input=files.path("sphere.nvdb");
  request.operation=Operation::NanoInspect; ASSERT_TRUE(run());
  EXPECT_TRUE(panel.result()->output.empty());
  EXPECT_NE(panel.result()->message.find("structure not validated"),std::string::npos);
  request.operation=Operation::NanoValidate; request.nanoRead.requireFullChecksum=true;
  ASSERT_TRUE(run());
  EXPECT_NE(panel.result()->message.find("CRC verified"),std::string::npos);
  request.operation=Operation::NanoCopy; request.output=files.path("raw.nvdb");
  request.nanoWrite.raw=true; ASSERT_TRUE(run());
  EXPECT_FALSE(panel.result()->modelOutput);
  request.operation=Operation::NanoChecksum; request.input=request.output;
  request.output=files.path("header.nvdb"); request.nanoWrite.checksum=vdb::NanoVdbChecksum::Header;
  ASSERT_TRUE(run());
  EXPECT_EQ(vdb::ValidateNanoVdbFile(files.directory/"header.nvdb").grids[0].checksum,
            vdb::NanoVdbChecksum::Header);
  request.operation=Operation::NanoToVdb; request.input=files.path("raw.nvdb");
  request.output=files.path("restored.vdb"); ASSERT_TRUE(run());
  EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_EQ(vdb::InspectFile(files.directory/"restored.vdb").grids[0].activeVoxelCount,
            vdb::InspectFile(files.directory/"sphere.vdb").grids[0].activeVoxelCount);
  request.operation=Operation::NanoStatistics; request.input=files.path("raw.nvdb");
  request.output=files.path("statistics.nvdb"); request.nanoWrite={};
  request.nanoStatisticsMode=vdb::NanoVdbStatisticsMode::Bounds;
  ASSERT_TRUE(panel.submit(request)); request.nanoStatisticsMode=vdb::NanoVdbStatisticsMode::All;
  ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_NE(panel.result()->message.find("Recomputed native bounds:"),std::string::npos);
  EXPECT_EQ(panel.result()->message.find("native cache average"),std::string::npos);
}

TEST(OpenVdbAuthoring, DefaultExecutorExportsPhysicalImageAndEnforcesSnapshotCaps) {
  using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;
  const auto spec=files.directory/std::filesystem::path(u8"sc\u00e8ne.json");
  const auto output=files.directory/std::filesystem::path(u8"r\u00e9sultat.exr");
  std::ofstream(spec) << R"({"schemaVersion":1,"camera":{"width":2,"height":2},"environment":[1,0.5,0.25]})";
  const auto utf8=[](const auto& value) { const auto text=value.u8string(); return std::string(text.begin(),text.end()); };
  OpenVdbAuthoringPanel panel; OpenVdbAuthoringRequest request;
  request.operation=Operation::RenderTransportImage; request.input=utf8(spec); request.output=utf8(output);
  request.transportRender.image.transport.samplesPerRay=2;
  request.transportRender.image.transport.maxScatteringEvents=0;
  ASSERT_TRUE(panel.submit(request)); request.transportRender.image.maxPixels=1;
  ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_TRUE(std::filesystem::exists(output));
  EXPECT_NE(panel.result()->message.find("OpenEXR 2x2"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("SE.R/G/B"),std::string::npos);
  request.output=files.path("limited.exr");
  ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  EXPECT_FALSE(panel.result()->success); EXPECT_FALSE(std::filesystem::exists(files.directory/"limited.exr"));
  request.transportRender.image.maxPixels=4; request.output=utf8(output);
  ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  EXPECT_FALSE(panel.result()->success); EXPECT_TRUE(std::filesystem::exists(output));
}

TEST(OpenVdbAuthoring, DefaultExecutorDenseInterchangeAndFractionalMultiresolution) {
  namespace vdb=container::geometry::vdb;
  using Operation=container::ui::OpenVdbAuthoringOperation;
  if (!vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB is disabled";
  AuthoringFiles files; vdb::ProcessingOptions processing; processing.voxelSize=.25;
  (void)vdb::WriteConstantVelocity(files.directory/"wind.vdb",{1,2,3},processing);
  OpenVdbAuthoringPanel panel; OpenVdbAuthoringRequest request;
  const auto run=[&] {
    if (!panel.submit(request) || !settle(panel) || !panel.result()) return false;
    EXPECT_TRUE(panel.result()->success) << panel.result()->message;
    EXPECT_FALSE(panel.result()->modelOutput);
    return panel.result()->success;
  };
  request.operation=Operation::DenseExport; request.input=files.path("wind.vdb");
  request.output=files.path("wind.cvarr"); request.denseRegion.origin={-1,-1,-1}; request.denseRegion.dimensions={2,2,2};
  ASSERT_TRUE(run()); EXPECT_NE(panel.result()->message.find("explicit activity mask"),std::string::npos);
  request.operation=Operation::DenseImport; request.input=request.output; request.output=files.path("restored.vdb");
  ASSERT_TRUE(run()); EXPECT_EQ(vdb::SampleVector(files.directory/"restored.vdb",{3,2,1}),(std::array<double,3>{1,2,3}));
  request.operation=Operation::MultiResolution; request.input=files.path("wind.vdb"); request.output=files.path("levels.vdb");
  request.multiresolution.levels=3; request.multiresolutionLevel=1; ASSERT_TRUE(run());
  const auto level=vdb::InspectFile(files.directory/"levels.vdb"); ASSERT_EQ(level.grids.size(),1u);
  EXPECT_EQ(level.grids[0].voxelSize,(std::array<double,3>{.5,.5,.5}));
  request.operation=Operation::MultiResolutionInfo; ASSERT_TRUE(run()); EXPECT_TRUE(panel.result()->output.empty());
  EXPECT_NE(panel.result()->message.find("LoD 2"),std::string::npos);
  request.operation=Operation::MultiResolutionSample; request.queryPosition={9,-7,4}; request.sampleLevel=1.5;
  ASSERT_TRUE(run()); EXPECT_TRUE(panel.result()->output.empty());
  EXPECT_NE(panel.result()->message.find("1, 2, 3"),std::string::npos);
}

TEST(OpenVdbAuthoring, DefaultExecutorPublishesSmokeSequenceAndLastFrameRestartSelectors) {
  namespace vdb = container::geometry::vdb;
  if (!vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB is disabled";
  AuthoringFiles files; vdb::ProcessingOptions processing; processing.voxelSize=.25;
  (void)vdb::WriteSphere(files.directory/"sphere.vdb",{},processing);
  processing.gridName="density";
  (void)vdb::WriteFog(files.directory/"sphere.vdb",files.directory/"fog.vdb",{},processing);
  processing.gridName="velocity";
  (void)vdb::WriteConstantVelocity(files.directory/"wind.vdb",{0,0,0},processing);
  (void)vdb::WriteProjectedVelocity(files.directory/"wind.vdb",files.directory/"fog.vdb",files.directory/"initial.vdb",{},"velocity","density");
  OpenVdbAuthoringPanel panel; OpenVdbAuthoringRequest request;
  request.operation=container::ui::OpenVdbAuthoringOperation::SimulateSequence;
  request.input=files.path("fog.vdb"); request.inputGrid="density";
  request.secondInput=request.domainInput=files.path("initial.vdb");
  request.secondGrid="velocity"; request.domainGrid="fluid";
  request.output=files.path("sequence"); request.sequence.frameCount=2;
  request.simulation.densityGridName="rho"; request.simulation.velocityGridName="flow";
  request.simulation.domainGridName="cells"; request.simulation.temperatureGridName="kelvin";
  ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_EQ(std::filesystem::path(panel.result()->output).filename(),"frame_0002.vdb");
  ASSERT_TRUE(panel.result()->restartGrids);
  EXPECT_EQ((*panel.result()->restartGrids),(std::array<std::string,4>{"rho","flow","cells","kelvin"}));
  EXPECT_TRUE(std::filesystem::exists(files.directory/"sequence"/"sequence.json"));
  // AuthoringFiles owns a flat directory; remove only these known generated files.
  for (const auto* name:{"frame_0001.vdb","frame_0002.vdb","sequence.json"})
    std::filesystem::remove(files.directory/"sequence"/name);
  std::filesystem::remove(files.directory/"sequence");
}

TEST(OpenVdbAuthoring, SequenceProgressCancellationRollsBackAndNextSubmissionResetsControl) {
  namespace vdb=container::geometry::vdb;
  if (!vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB is disabled";
  AuthoringFiles files; vdb::ProcessingOptions processing; processing.voxelSize=.25;
  (void)vdb::WriteSphere(files.directory/"sphere.vdb",{},processing);
  processing.gridName="density";
  (void)vdb::WriteFog(files.directory/"sphere.vdb",files.directory/"fog.vdb",{},processing);
  (void)vdb::WriteConstantVelocity(files.directory/"wind.vdb",{0,0,0},processing);
  (void)vdb::WriteProjectedVelocity(files.directory/"wind.vdb",files.directory/"fog.vdb",files.directory/"initial.vdb");
  std::promise<void> completed,release;
  auto released=release.get_future().share();
  std::atomic_uint originalStopCalls{0};
  OpenVdbAuthoringPanel panel; OpenVdbAuthoringRequest request;
  EXPECT_FALSE(panel.requestCancel()); EXPECT_FALSE(panel.progress());
  request.operation=container::ui::OpenVdbAuthoringOperation::SimulateSequence;
  request.input=files.path("fog.vdb"); request.inputGrid="density";
  request.secondInput=request.domainInput=files.path("initial.vdb");
  request.secondGrid="velocity"; request.domainGrid="fluid";
  request.output=files.path("cancelled"); request.sequence.frameCount=3;
  request.sequence.stopRequested=[&]{++originalStopCalls; return false;};
  request.sequence.progress=[&](uint32_t frame,uint32_t total) {
    if (frame!=1 || total!=3) throw std::runtime_error("unexpected progress");
    completed.set_value(); released.wait();
  };
  ASSERT_TRUE(panel.submit(request));
  const auto reached=completed.get_future().wait_for(5s);
  if (reached!=std::future_status::ready) { release.set_value(); FAIL() << "first frame did not complete"; }
  const auto progress=panel.progress();
  EXPECT_TRUE(progress);
  if (progress) { EXPECT_EQ(progress->completedFrames,1u); EXPECT_EQ(progress->totalFrames,3u); }
  EXPECT_GT(originalStopCalls.load(),0u);
  EXPECT_TRUE(panel.requestCancel());
  EXPECT_TRUE(panel.progress()->cancellationRequested);
  EXPECT_FALSE(panel.submit(request)); // Rejecting a concurrent job must retain cancellation.
  release.set_value();
  ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result()); EXPECT_FALSE(panel.result()->success);
  EXPECT_FALSE(std::filesystem::exists(files.directory/"cancelled"));
  EXPECT_FALSE(panel.requestCancel());
  request.sequence.progress={}; request.sequence.stopRequested={}; request.sequence.frameCount=1;
  request.output=files.path("completed");
  ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  ASSERT_TRUE(panel.progress()); EXPECT_EQ(panel.progress()->completedFrames,1u);
  EXPECT_FALSE(panel.progress()->cancellationRequested);
  for (const auto* name:{"frame_0001.vdb","sequence.json"})
    std::filesystem::remove(files.directory/"completed"/name);
  std::filesystem::remove(files.directory/"completed");
}

TEST(OpenVdbAuthoring, DefaultExecutorProjectsClosedVelocityAndUsesSelectedOutputNames) {
  namespace vdb = container::geometry::vdb;
  if (!vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB is disabled";
  AuthoringFiles files;
  vdb::ProcessingOptions options; options.voxelSize = .25;
  (void)vdb::WriteSphere(files.directory / "sphere.vdb", {}, options);
  options.gridName = "density";
  (void)vdb::WriteFog(files.directory / "sphere.vdb", files.directory / "fog.vdb", {}, options);
  options.gridName = "wind";
  (void)vdb::WriteConstantVelocity(files.directory / "wind.vdb", {1,.5,0}, options);
  OpenVdbAuthoringPanel panel;
  OpenVdbAuthoringRequest request;
  request.operation = container::ui::OpenVdbAuthoringOperation::Project;
  request.input = files.path("wind.vdb"); request.inputGrid = "wind";
  request.secondInput = files.path("fog.vdb"); request.secondGrid = "density";
  request.output = files.path("projected.vdb"); request.projection.velocityGridName = "flow";
  request.projection.pressureGridName = "p"; request.projection.writeDomain = false;
  ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_NE(panel.result()->message.find("divergence"), std::string::npos);
  const auto inspection = vdb::InspectFile(files.directory / "projected.vdb");
  ASSERT_EQ(inspection.grids.size(), 2u);
  EXPECT_EQ(inspection.grids[0].name, "flow"); EXPECT_EQ(inspection.grids[1].name, "p");
}

TEST(OpenVdbAuthoring, DefaultExecutorWritesCompleteSmokeRestartState) {
  namespace vdb = container::geometry::vdb;
  if (!vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB is disabled";
  AuthoringFiles files; vdb::ProcessingOptions options; options.voxelSize=.25;
  (void)vdb::WriteSphere(files.directory/"sphere.vdb",{},options);
  options.gridName="density";
  (void)vdb::WriteFog(files.directory/"sphere.vdb",files.directory/"fog.vdb",{},options);
  options.gridName="wind";
  (void)vdb::WriteConstantVelocity(files.directory/"wind.vdb",{0,0,0},options);
  (void)vdb::WriteProjectedVelocity(files.directory/"wind.vdb",files.directory/"fog.vdb",
      files.directory/"prepared.vdb",{},"wind","density");
  OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=container::ui::OpenVdbAuthoringOperation::Simulate;
  request.input=files.path("fog.vdb");request.inputGrid="density";
  request.secondInput=request.domainInput=files.path("prepared.vdb");
  request.secondGrid="velocity";request.domainGrid="fluid";request.output=files.path("state.vdb");
  request.simulation.timeStep=.025;request.simulation.substeps=2;
  request.simulation.densityGridName="smoke";request.simulation.velocityGridName="flow";
  request.simulation.pressureGridName="p";request.simulation.domainGridName="domain";
  request.simulation.temperatureGridName="kelvin";
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_FALSE(panel.result()->modelOutput);ASSERT_TRUE(panel.result()->restartGrids);
  EXPECT_EQ(*panel.result()->restartGrids,(std::array<std::string,4>{"smoke","flow","domain","kelvin"}));
  const auto report=vdb::InspectFile(files.directory/"state.vdb");
  std::set<std::string> names;for(const auto& grid:report.grids)names.insert(grid.name);
  EXPECT_EQ(names,(std::set<std::string>{"smoke","flow","domain","kelvin","p"}));
  EXPECT_NEAR(vdb::SampleScalar(files.directory/"state.vdb",{0,0,0},"kelvin"),293.15,1e-10);
  request.operation=container::ui::OpenVdbAuthoringOperation::Viscosity;
  request.input=files.path("state.vdb"); request.output=files.path("viscous.vdb");
  request.viscosity.densityGridName="smoke"; request.viscosity.velocityGridName="flow";
  request.viscosity.pressureGridName="p"; request.viscosity.domainGridName="domain";
  request.viscosity.temperatureGridName="kelvin"; request.viscosity.kinematicViscosity=.1;
  ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_FALSE(panel.result()->modelOutput); ASSERT_TRUE(panel.result()->restartGrids);
  EXPECT_EQ(*panel.result()->restartGrids,(std::array<std::string,4>{"smoke","flow","domain","kelvin"}));
  EXPECT_NE(panel.result()->message.find("MAC energy"),std::string::npos);
  EXPECT_EQ(vdb::SampleScalar(files.directory/"viscous.vdb",{0,0,0},"smoke"),
            vdb::SampleScalar(files.directory/"state.vdb",{0,0,0},"smoke"));
  EXPECT_EQ(vdb::SampleScalar(files.directory/"viscous.vdb",{0,0,0},"kelvin"),
            vdb::SampleScalar(files.directory/"state.vdb",{0,0,0},"kelvin"));
  request.operation=container::ui::OpenVdbAuthoringOperation::FluidSources;
  request.input=files.path("viscous.vdb"); request.output=files.path("emitted.vdb");
  request.sources.densityGridName="smoke"; request.sources.velocityGridName="flow";
  request.sources.pressureGridName="p"; request.sources.domainGridName="domain";
  request.sources.temperatureGridName="kelvin"; request.sources.timeStep=.5;
  request.sources.sources.push_back({.radius=2,.densityRate=2,.temperatureRate=10});
  ASSERT_TRUE(panel.submit(request)); request.sources.sources[0].densityRate=200;
  ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_FALSE(panel.result()->modelOutput); ASSERT_TRUE(panel.result()->restartGrids);
  EXPECT_EQ(*panel.result()->restartGrids,(std::array<std::string,4>{"smoke","flow","domain","kelvin"}));
  EXPECT_EQ(vdb::SampleScalar(files.directory/"emitted.vdb",{0,0,0},"smoke"),
            vdb::SampleScalar(files.directory/"viscous.vdb",{0,0,0},"smoke")+1);
  EXPECT_EQ(vdb::SampleScalar(files.directory/"emitted.vdb",{0,0,0},"kelvin"),
            vdb::SampleScalar(files.directory/"viscous.vdb",{0,0,0},"kelvin")+5);
  request.operation=container::ui::OpenVdbAuthoringOperation::Simulate;
  request.input=request.secondInput=request.domainInput=request.temperatureInput=files.path("state.vdb");
  request.inputGrid="smoke"; request.secondGrid="flow"; request.domainGrid="domain"; request.temperatureGrid="kelvin";
  request.output=files.path("coupled.vdb"); request.simulation.kinematicViscosity=.1;
  request.simulation.sources.push_back({.radius=2,.densityRate=1,.temperatureRate=2});
  ASSERT_TRUE(panel.submit(request)); ASSERT_TRUE(settle(panel)); ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_NE(panel.result()->message.find("Viscosity:"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("Sources:"),std::string::npos);
  EXPECT_NEAR(vdb::SampleScalar(files.directory/"coupled.vdb",{0,0,0},"smoke"),
              vdb::SampleScalar(files.directory/"state.vdb",{0,0,0},"smoke")+.025,1e-12);
}

TEST(OpenVdbAuthoring, SubmissionOwnsSnapshotAndRejectsConcurrentWorkWithoutWaiting) {
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  OpenVdbAuthoringPanel panel([&](const OpenVdbAuthoringRequest& r) {
    entered.set_value();
    released.wait();
    return OpenVdbAuthoringResult{true, r.input, r.output, true};
  });
  OpenVdbAuthoringRequest request;
  request.input = "original source";
  request.output = "original output";
  EXPECT_TRUE(panel.submit(request));
  EXPECT_EQ(entered.get_future().wait_for(5s), std::future_status::ready);
  request.input = "modified source";
  request.output = "modified output";
  const bool acceptedSecond = panel.submit(request);
  panel.poll();
  const bool running = panel.busy();
  release.set_value();
  EXPECT_FALSE(acceptedSecond);
  EXPECT_TRUE(running);
  ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());
  EXPECT_EQ(panel.result()->message, "original source");
  EXPECT_EQ(panel.result()->output, "original output");
}

TEST(OpenVdbAuthoring, ErrorsAreDeliveredAndNextSubmissionCanSucceed) {
  OpenVdbAuthoringPanel panel([](const OpenVdbAuthoringRequest& r) {
    if (r.input.empty()) throw std::runtime_error("missing source");
    return OpenVdbAuthoringResult{true, "completed", r.output, false};
  });
  EXPECT_TRUE(panel.submit({}));
  ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());
  EXPECT_FALSE(panel.result()->success);
  EXPECT_EQ(panel.result()->message, "missing source");
  OpenVdbAuthoringRequest request;
  request.input = "valid";
  EXPECT_TRUE(panel.submit(request));
  ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());
  EXPECT_TRUE(panel.result()->success);
  EXPECT_EQ(panel.result()->message, "completed");
}

TEST(OpenVdbAuthoring, DestructionWaitsForOwnedWork) {
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  auto panel = std::make_unique<OpenVdbAuthoringPanel>([&](const auto&) {
    entered.set_value(); released.wait();
    return OpenVdbAuthoringResult{true, "done", {}, false};
  });
  EXPECT_TRUE(panel->submit({}));
  EXPECT_EQ(entered.get_future().wait_for(5s), std::future_status::ready);
  auto destroyed = std::async(std::launch::async, [panel = std::move(panel)]() mutable { panel.reset(); });
  const auto beforeRelease = destroyed.wait_for(20ms);
  release.set_value();
  EXPECT_EQ(beforeRelease, std::future_status::timeout);
  EXPECT_EQ(destroyed.wait_for(5s), std::future_status::ready);
}

TEST(OpenVdbAuthoring, DefaultExecutorWritesInspectableSdfOrReportsDisabledBuild) {
  const auto directory = std::filesystem::temp_directory_path() / ("container-vdb-authoring-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(directory);
  const auto output = directory / "sphere.vdb";
  OpenVdbAuthoringPanel panel;
  OpenVdbAuthoringRequest request;
  const auto utf8 = output.u8string();
  request.output.assign(utf8.begin(), utf8.end());
  EXPECT_TRUE(panel.submit(request));
  const bool completed = settle(panel);
  if (completed && panel.result()) {
    if (container::geometry::vdb::IsAvailable()) {
      EXPECT_TRUE(panel.result()->success) << panel.result()->message;
      EXPECT_TRUE(panel.result()->modelOutput);
      if (std::filesystem::exists(output)) {
        const auto report = container::geometry::vdb::InspectFile(output, {.loadStatistics = true});
        EXPECT_EQ(report.grids.size(), 1u);
        EXPECT_GT(report.grids[0].statistics->count, 0u);
      } else ADD_FAILURE() << "sphere was not written";
    } else {
      EXPECT_FALSE(panel.result()->success);
      EXPECT_NE(panel.result()->message.find("unavailable"), std::string::npos);
      EXPECT_FALSE(std::filesystem::exists(output));
    }
  }
  EXPECT_TRUE(completed);
  std::error_code ignored;
  std::filesystem::remove(output, ignored);
  std::filesystem::remove(directory, ignored);
}

TEST(OpenVdbAuthoring, PanelDrawKeepsDraftAndBalancesImGuiState) {
  const auto previous = ImGui::GetCurrentContext();
  auto* context = ImGui::CreateContext();
  auto& io = ImGui::GetIO();
  io.IniFilename = nullptr; io.DisplaySize = {900, 1000}; io.DeltaTime = 1.0f / 60;
  unsigned char* pixels; int width, height;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  {
    OpenVdbAuthoringPanel panel;
    panel.draft.input = "retained.vdb";
    panel.draft.output = "new.vdb";
    for (int operation = 0; operation < int(container::ui::OpenVdbAuthoringOperation::Count); ++operation) {
      panel.draft.operation = static_cast<container::ui::OpenVdbAuthoringOperation>(operation);
      ImGui::NewFrame();
      ImGui::SetNextWindowSize({850, 950});
      ImGui::Begin("OpenVDB authoring test");
      ImGui::SetNextItemOpen(true, ImGuiCond_Always);
      EXPECT_FALSE(panel.draw());
      ImGui::End(); ImGui::Render();
      EXPECT_EQ(panel.draft.input, "retained.vdb");
      EXPECT_EQ(panel.draft.output, "new.vdb");
      EXPECT_FALSE(panel.busy());
    }
  }
  ImGui::DestroyContext(context);
  ImGui::SetCurrentContext(previous);
}

TEST(OpenVdbAuthoring, BoundaryResultHidesGenericInputReuseForGroupedBindings) {
  using Operation = container::ui::OpenVdbAuthoringOperation;
  const auto previous = ImGui::GetCurrentContext();
  auto* context = ImGui::CreateContext();
  auto& io = ImGui::GetIO();
  io.IniFilename = nullptr; io.DisplaySize = {1200, 4000}; io.DeltaTime = 1.0f / 60;
  unsigned char* pixels; int width, height;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  {
    OpenVdbAuthoringPanel panel([](const auto& request) {
      return OpenVdbAuthoringResult{true, "created", request.output, false};
    });
    panel.draft.input = "legacy.vdb";
    panel.draft.inputGrid = "legacy-grid";
    panel.draft.output = "new-state.vdb";
    panel.draft.boundaryInputs.velocity.file = "predictor.vdb";
    panel.draft.boundaryReplay.state = "canonical-state.vdb";
    ImGuiID reuseId = 0;
    const auto frame = [&] {
      ImGui::NewFrame();
      ImGui::SetNextWindowSize({1150, 3900});
      ImGui::Begin("Boundary result actions");
      ImGui::PushID(&panel);
      reuseId = ImGui::GetID("Use output as input");
      ImGui::PopID();
      ImGui::SetNextItemOpen(true, ImGuiCond_Always);
      const auto selected = panel.draw();
      ImGui::End(); ImGui::Render();
      return selected;
    };
    for (const auto operation : {Operation::ProjectBoundaries, Operation::ReprojectBoundaries, Operation::NanoPrimitive, Operation::QueryActiveRegions}) {
      panel.draft.operation = operation;
      ASSERT_TRUE(panel.submit(panel.draft));
      ASSERT_TRUE(settle(panel));
      ASSERT_TRUE(panel.result());
      ASSERT_TRUE(panel.result()->success);
      EXPECT_FALSE(frame()); EXPECT_FALSE(frame());
      ImGui::ActivateItemByID(reuseId);
      EXPECT_FALSE(frame());
      EXPECT_EQ(panel.draft.input, "legacy.vdb");
      EXPECT_EQ(panel.draft.inputGrid, "legacy-grid");
      EXPECT_EQ(panel.draft.boundaryInputs.velocity.file, std::filesystem::path("predictor.vdb"));
      EXPECT_EQ(panel.draft.boundaryReplay.state, std::filesystem::path("canonical-state.vdb"));
    }
    // A fresh producer result still exposes its ordinary output action.
    panel.draft.operation = Operation::Sphere;
    ASSERT_TRUE(panel.submit(panel.draft));
    ASSERT_TRUE(settle(panel));
    EXPECT_FALSE(frame());
    ImGui::ActivateItemByID(reuseId);
    EXPECT_FALSE(frame());
    EXPECT_EQ(panel.draft.input, "new-state.vdb");
    EXPECT_TRUE(panel.draft.inputGrid.empty());
  }
  ImGui::DestroyContext(context);
  ImGui::SetCurrentContext(previous);
}

TEST(OpenVdbAuthoring, ActiveRegionResultsHideAllCustomExecutorOutputActions) {
  using Operation=container::ui::OpenVdbAuthoringOperation;
  const auto previous=ImGui::GetCurrentContext();auto* context=ImGui::CreateContext();
  auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize={1200,4000};io.DeltaTime=1.f/60;
  unsigned char* pixels;int width,height;io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
  {
    OpenVdbAuthoringPanel panel([](const auto& request){OpenVdbAuthoringResult result{true,"queried",request.output,true};result.restartGrids=std::array<std::string,4>{"density","velocity","fluid","temperature"};return result;});
    panel.draft.operation=Operation::QueryActiveRegions;panel.draft.input="source.vdb";panel.draft.inputGrid="source";panel.draft.secondInput="retained-second.vdb";panel.draft.output="foreign-output.vdb";panel.draft.domainInput="retained-domain.vdb";panel.draft.temperatureInput="retained-temperature.vdb";
    ASSERT_TRUE(panel.submit(panel.draft));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success);
    std::array<ImGuiID,3> actions{};
    const auto frame=[&]{ImGui::NewFrame();ImGui::SetNextWindowSize({1150,3900});ImGui::Begin("Readonly query result actions");ImGui::PushID(&panel);actions={ImGui::GetID("Use output as input"),ImGui::GetID("Use state as restart"),ImGui::GetID("Load preview")};ImGui::PopID();ImGui::SetNextItemOpen(true,ImGuiCond_Always);const auto selected=panel.draw();ImGui::End();ImGui::Render();return selected;};
    EXPECT_FALSE(frame());EXPECT_FALSE(frame());
    for(size_t i=0;i<actions.size();++i){ImGui::ActivateItemByID(actions[i]);EXPECT_FALSE(frame());EXPECT_FALSE(panel.previewOptions());EXPECT_EQ(panel.draft.operation,Operation::QueryActiveRegions);EXPECT_EQ(panel.draft.input,"source.vdb");EXPECT_EQ(panel.draft.inputGrid,"source");EXPECT_EQ(panel.draft.secondInput,"retained-second.vdb");EXPECT_EQ(panel.draft.domainInput,"retained-domain.vdb");EXPECT_EQ(panel.draft.temperatureInput,"retained-temperature.vdb");}
  }
  ImGui::DestroyContext(context);ImGui::SetCurrentContext(previous);
}

TEST(OpenVdbAuthoring, RunButtonSubmitsAndLoadRequiresASeparateExplicitClick) {
  if (!container::geometry::vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB is disabled";
  const auto previous = ImGui::GetCurrentContext();
  auto* context = ImGui::CreateContext();
  auto& io = ImGui::GetIO();
  io.IniFilename = nullptr; io.DisplaySize = {900, 1000}; io.DeltaTime = 1.0f / 60;
  unsigned char* pixels; int width, height;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  {
    std::atomic_uint calls{0};
    OpenVdbAuthoringPanel panel([&](const auto& request) {
      ++calls;
      OpenVdbAuthoringResult result{true, "created", request.output, true};
      result.restartGrids=std::array<std::string,4>{"smoke","flow","domain","kelvin"};
      return result;
    });
    panel.draft.output = "chosen-surface.vdb";
    panel.draft.inputGrid = "old-density";
    ImGuiID runId = 0, loadId = 0, reuseId = 0, restartId = 0;
    const auto frame = [&] {
      ImGui::NewFrame();
      ImGui::SetNextWindowSize({850, 950});
      ImGui::Begin("Authoring actions");
      ImGui::PushID(&panel);
      runId = ImGui::GetID("Run OpenVDB operation");
      loadId = ImGui::GetID("Load preview");
      reuseId = ImGui::GetID("Use output as input");
      restartId = ImGui::GetID("Use state as restart");
      ImGui::PopID();
      ImGui::SetNextItemOpen(true, ImGuiCond_Always);
      const auto selected = panel.draw();
      ImGui::End(); ImGui::Render();
      return selected;
    };
    EXPECT_FALSE(frame()); EXPECT_FALSE(frame());
    EXPECT_EQ(calls, 0u);
    ImGui::ActivateItemByID(runId);
    EXPECT_FALSE(frame());
    EXPECT_TRUE(settle(panel));
    EXPECT_EQ(calls, 1u);
    EXPECT_FALSE(frame());
    ImGui::ActivateItemByID(loadId);
    EXPECT_EQ(frame(), std::optional<std::string>("chosen-surface.vdb"));
    ASSERT_TRUE(panel.previewOptions()); EXPECT_FALSE(panel.previewOptions()->vectorPreview);
    ImGui::ActivateItemByID(reuseId);
    EXPECT_FALSE(frame());
    EXPECT_EQ(panel.draft.input, "chosen-surface.vdb");
    EXPECT_TRUE(panel.draft.inputGrid.empty());
    ImGui::ActivateItemByID(restartId);
    EXPECT_FALSE(frame());
    EXPECT_EQ(panel.draft.operation,container::ui::OpenVdbAuthoringOperation::Simulate);
    EXPECT_EQ(panel.draft.secondInput,"chosen-surface.vdb");
    EXPECT_EQ(panel.draft.domainInput,"chosen-surface.vdb");
    EXPECT_EQ(panel.draft.temperatureInput,"chosen-surface.vdb");
    EXPECT_EQ(panel.draft.inputGrid,"smoke");EXPECT_EQ(panel.draft.secondGrid,"flow");
    EXPECT_EQ(panel.draft.domainGrid,"domain");EXPECT_EQ(panel.draft.temperatureGrid,"kelvin");
    EXPECT_EQ(calls, 1u);
  }
  ImGui::DestroyContext(context);
  ImGui::SetCurrentContext(previous);
}

TEST(OpenVdbAuthoring, VectorPreviewButtonSnapshotsDisplaySettingsWithoutAnAuthoringJob) {
  namespace vdb=container::geometry::vdb;
  if (!vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB is disabled";
  const auto previous=ImGui::GetCurrentContext(); auto* context=ImGui::CreateContext();
  auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.DisplaySize={1000,1600}; io.DeltaTime=1.f/60;
  unsigned char* pixels; int width,height; io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
  {
    std::atomic_uint calls{0}; OpenVdbAuthoringPanel panel([&](const auto&) {++calls; return OpenVdbAuthoringResult{};});
    panel.previewInput="constant.vdb"; panel.previewGrid="velocity"; panel.previewKind=2;
    panel.vectorPreview.hasBounds=true; panel.vectorPreview.boundsMin={-2,-1,-1};
    panel.vectorPreview.boundsMax={2,1,1}; panel.vectorPreview.seedCounts={2,1,1};
    ImGuiID loadId=0;
    const auto frame=[&] {
      ImGui::NewFrame(); ImGui::SetNextWindowSize({950,1500}); ImGui::Begin("Vector preview actions");
      ImGui::PushID(&panel);
      const auto treeId=ImGui::GetID("Field preview controls");
      ImGui::GetStateStorage()->SetBool(treeId,true);
      ImGui::PushOverrideID(treeId); loadId=ImGui::GetID("Load field preview"); ImGui::PopID(); ImGui::PopID();
      ImGui::SetNextItemOpen(true,ImGuiCond_Always);
      const auto selected=panel.draw(); ImGui::End(); ImGui::Render(); return selected;
    };
    EXPECT_FALSE(frame()); EXPECT_FALSE(frame()); EXPECT_FALSE(panel.previewOptions());
    ImGui::ActivateItemByID(loadId); EXPECT_EQ(frame(),std::optional<std::string>("constant.vdb"));
    ASSERT_TRUE(panel.previewOptions()); ASSERT_TRUE(panel.previewOptions()->vectorPreview);
    const auto saved=panel.previewOptions(); EXPECT_EQ(saved->gridName,"velocity");
    EXPECT_EQ(saved->vectorPreview->mode,vdb::VectorPreviewMode::Streamlines);
    panel.vectorPreview.boundsMin[0]=-100; panel.previewGrid="changed";
    EXPECT_EQ(saved->vectorPreview->boundsMin[0],-2); EXPECT_EQ(saved->gridName,"velocity");
    EXPECT_EQ(calls,0u); EXPECT_FALSE(panel.busy());
  }
  ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous);
}
TEST(OpenVdbAuthoring, SliceAndNativePointPreviewButtonsSnapshotIndependentSettings) {
  namespace vdb=container::geometry::vdb;
  const auto previous=ImGui::GetCurrentContext(); auto* context=ImGui::CreateContext();
  auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.DisplaySize={1000,1800}; io.DeltaTime=1.f/60;
  unsigned char* pixels; int width,height; io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
  for (const int kind:{3,4}) {
    unsigned calls=0; OpenVdbAuthoringPanel panel([&](const auto&) {++calls; return OpenVdbAuthoringResult{};});
    panel.previewInput="native.vdb"; panel.previewGrid="selected"; panel.previewKind=kind;
    panel.slicePreview.origin={1,2,3}; panel.slicePreview.opacity=.25f;
    panel.slicePreview.integerRange=std::array<int64_t,2>{INT64_MIN,INT64_MAX};
    panel.nativePointPreview.groups.allOf={"selected"}; panel.nativePointPreview.colorAttribute="id";
    panel.nativePointPreview.colorMode=vdb::NativePointColor::ScalarRamp;
    panel.nativePointLimits.maxPoints=42;
    ImGuiID loadId=0;
    const auto frame=[&] {
      ImGui::NewFrame(); ImGui::SetNextWindowSize({950,1750}); ImGui::Begin("Additional field preview actions");
      ImGui::PushID(&panel); const auto tree=ImGui::GetID("Field preview controls");
      ImGui::GetStateStorage()->SetBool(tree,true); ImGui::PushOverrideID(tree);
      loadId=ImGui::GetID("Load field preview"); ImGui::PopID(); ImGui::PopID();
      ImGui::SetNextItemOpen(true,ImGuiCond_Always); const auto selected=panel.draw();
      ImGui::End(); ImGui::Render(); return selected;
    };
    EXPECT_FALSE(frame()); EXPECT_FALSE(frame());
    ImGui::ActivateItemByID(loadId); EXPECT_EQ(frame(),std::optional<std::string>("native.vdb"));
    ASSERT_TRUE(panel.previewOptions()); const auto saved=panel.previewOptions();
    EXPECT_FALSE(saved->vectorPreview); EXPECT_EQ(saved->gridName,"selected");
    EXPECT_EQ(saved->slicePreview.has_value(),kind==3); EXPECT_EQ(saved->nativePointPreview.has_value(),kind==4);
    panel.slicePreview.origin[0]=-99; panel.nativePointPreview.groups.allOf.clear(); panel.nativePointLimits.maxPoints=1;
    panel.slicePreview.integerRange.reset();
    if (kind==3) {
      EXPECT_EQ(saved->slicePreview->origin[0],1); EXPECT_EQ(saved->slicePreview->opacity,.25f);
      EXPECT_EQ(saved->slicePreview->integerRange,(std::optional<std::array<int64_t,2>>(std::array<int64_t,2>{INT64_MIN,INT64_MAX})));
    }
    else { EXPECT_EQ(saved->nativePointPreview->groups.allOf,(std::vector<std::string>{"selected"})); EXPECT_EQ(saved->nativePointLimits.maxPoints,42); }
    EXPECT_EQ(calls,0u); EXPECT_FALSE(panel.busy());
  }
  ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous);
}
std::string rayAuthoringBytes(const std::filesystem::path &file) {
  std::ifstream stream(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), {}};
}

double rayAuthoringNumber(const std::string &message, const std::string &marker,
                          const std::string &key) {
  const auto row = message.find(marker);
  if (row == std::string::npos) {
    ADD_FAILURE() << "missing row " << marker << " in " << message;
    return std::numeric_limits<double>::quiet_NaN();
  }
  const auto end = message.find('\n', row);
  const auto field = message.find(key, row);
  if (field == std::string::npos ||
      (end != std::string::npos && field >= end)) {
    ADD_FAILURE() << "missing field " << key << " in row " << marker;
    return std::numeric_limits<double>::quiet_NaN();
  }
  try {
    return std::stod(message.substr(field + key.size()));
  } catch (...) {
    ADD_FAILURE() << "invalid numeric field " << key << " in " << message;
    return std::numeric_limits<double>::quiet_NaN();
  }
}

container::geometry::vdb::DenseArrayVolume
rayAuthoringPlane(bool missing = false, bool empty = false) {
  namespace vdb = container::geometry::vdb;
  vdb::DenseArrayVolume volume;
  volume.region = {{-3, -3, -3}, {7, 7, 7}};
  volume.gridName = "descriptor:plane";
  volume.gridClass = vdb::DenseGridClass::LevelSet;
  volume.background = 4.;
  std::vector<double> values;
  volume.active.clear();
  for (int z = -3; z <= 3; ++z)
    for (int y = -3; y <= 3; ++y)
      for (int x = -3; x <= 3; ++x) {
        const bool on = !empty && !(missing && x == 1 && y == 1 && z == 1);
        values.push_back(on ? x - .25 : 4);
        volume.active.push_back(uint8_t(on));
      }
  volume.values = std::move(values);
  return volume;
}

TEST(OpenVdbAuthoring,
     RayRequestsSnapshotEveryRayOptionAndCommonCapEvenWithoutSdk) {
  namespace vdb = container::geometry::vdb;
  using Operation = container::ui::OpenVdbAuthoringOperation;
  for (const auto operation :
       {Operation::RayIntersections, Operation::RayVolumeSpans,
        Operation::QueryActiveRegions}) {
    std::promise<OpenVdbAuthoringRequest> captured;
    auto future = captured.get_future();
    OpenVdbAuthoringPanel panel([&](const OpenVdbAuthoringRequest &submitted) {
      captured.set_value(submitted);
      OpenVdbAuthoringResult fake{true, "captured", "fake-output.vdb", true};
      fake.restartGrids = std::array<std::string, 4>{"D", "U", "M", "T"};
      return fake;
    });
    OpenVdbAuthoringRequest request;
    request.operation = operation;
    request.input = "snapshot-source.vdb";
    request.inputGrid = "descriptor:literal";
    request.secondInput = "unused-second.vdb";
    request.output = "unused-output.vdb";
    request.rayQueries = {{{2.25, .25, .25}, {-7, 0, 0}, .125, 3},
                          {{-2, .25, .25}, {3, 0, 0}, 0, 48}};
    request.levelSetRays.isovalue = .5;
    request.levelSetRays.refinementSteps = 0;
    request.levelSetRays.computeNormals = false;
    request.levelSetRays.limits = {23, 17, 4'000'000'000ull, 65536};
    request.volumeRays.limits = {29, 19, 3'000'000'000ull, 32768};
    request.volumeRays.maxSpans = 79;
    request.processing.maxInputBytes = 234'567'891;
    request.processing.maxVoxelCount = 123'456'789;
    const auto original = request;
    ASSERT_TRUE(panel.submit(request));
    request.rayQueries.front().origin[0] = 999;
    request.rayQueries.clear();
    request.levelSetRays = {};
    request.volumeRays = {};
    request.processing.maxInputBytes = 0;
    request.processing.maxVoxelCount = 0;
    request.input = request.inputGrid = "changed";
    // Result policy must follow the submitted operation, not this later draft.
    panel.draft.operation = Operation::Sphere;
    ASSERT_TRUE(settle(panel));
    ASSERT_EQ(future.wait_for(0s), std::future_status::ready);
    const auto actual = future.get();
    EXPECT_EQ(actual.operation, original.operation);
    EXPECT_EQ(actual.input, original.input);
    EXPECT_EQ(actual.inputGrid, original.inputGrid);
    ASSERT_EQ(actual.rayQueries.size(), original.rayQueries.size());
    for (size_t i = 0; i < original.rayQueries.size(); ++i) {
      EXPECT_EQ(actual.rayQueries[i].origin, original.rayQueries[i].origin);
      EXPECT_EQ(actual.rayQueries[i].direction,
                original.rayQueries[i].direction);
      EXPECT_EQ(actual.rayQueries[i].minimumDistance,
                original.rayQueries[i].minimumDistance);
      EXPECT_EQ(actual.rayQueries[i].maximumDistance,
                original.rayQueries[i].maximumDistance);
    }
    EXPECT_EQ(actual.levelSetRays.isovalue, original.levelSetRays.isovalue);
    EXPECT_EQ(actual.levelSetRays.refinementSteps,
              original.levelSetRays.refinementSteps);
    EXPECT_EQ(actual.levelSetRays.computeNormals,
              original.levelSetRays.computeNormals);
    EXPECT_EQ(actual.volumeRays.maxSpans, original.volumeRays.maxSpans);
    const auto compare = [](const auto &a, const auto &b) {
      EXPECT_EQ(a.maxRays, b.maxRays);
      EXPECT_EQ(a.maxGrids, b.maxGrids);
      EXPECT_EQ(a.maxWorkItems, b.maxWorkItems);
      EXPECT_EQ(a.maxMetadataBytes, b.maxMetadataBytes);
    };
    compare(actual.levelSetRays.limits, original.levelSetRays.limits);
    compare(actual.volumeRays.limits, original.volumeRays.limits);
    EXPECT_EQ(actual.processing.maxInputBytes,
              original.processing.maxInputBytes);
    EXPECT_EQ(actual.processing.maxVoxelCount,
              original.processing.maxVoxelCount);
    ASSERT_TRUE(panel.result());
    EXPECT_TRUE(panel.result()->success);
    EXPECT_TRUE(panel.result()->output.empty());
    EXPECT_FALSE(panel.result()->modelOutput);
    EXPECT_FALSE(panel.result()->restartGrids);
    EXPECT_FALSE(panel.previewOptions());
  }
}

TEST(OpenVdbAuthoring,
     RayIntersectionsUseWorldGeometryMetadataAndReadOnlyEarlyPath) {
  namespace vdb = container::geometry::vdb;
  using Operation = container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;
  OpenVdbAuthoringPanel panel;
  OpenVdbAuthoringRequest request;
  request.operation = Operation::RayIntersections;
  request.input = files.path("plane.vdb");
  request.inputGrid = "descriptor:descriptor:plane";
  request.secondInput = std::string("\xc0\xaf", 2);
  request.output = std::string("\xff", 1);
  // Unrelated hidden producer settings cannot become query prerequisites.
  request.processing.voxelSize = 0;
  request.processing.halfWidth = 0;
  request.processing.maxTriangles = 0;
  request.processing.gridName = std::string("\xff", 1);
  request.nanoLimits.maxInputBytes = 0;
  request.surfaceFields.maxWorkItems = 0;
  request.rayQueries = {{{2.25, .25, .25}, {-7, 0, 0}, 0, 3},
                        {{2.25, .25, .25}, {-1, 0, 0}, 0, 1.5}};
  if (vdb::IsAvailable()) {
    (void)vdb::WriteDenseVdb(files.directory / "plane-raw.vdb",
                             rayAuthoringPlane());
    std::ofstream(files.directory / "metadata.json")
        << R"({"version":1,"inputs":[{"path":"plane-raw.vdb","edits":[{"selector":"descriptor:descriptor:plane","metadata":[{"name":"artist","operation":"set","type":"string","value":"ray artist"},{"name":"stable_id","operation":"set","type":"int64","value":9007199254740993}]}]}],"metadata":[{"name":"scene","operation":"set","type":"string","value":"ray scene"}]})";
    (void)vdb::WriteAuthoredVdbSpec(files.directory / "metadata.json",
                                    files.directory / "plane.vdb");
  }
  const auto original = vdb::IsAvailable()
                            ? rayAuthoringBytes(files.directory / "plane.vdb")
                            : "";
  const auto submitted = request;
  ASSERT_TRUE(panel.submit(request));
  request.rayQueries[0].origin = {100, 100, 100};
  request.levelSetRays.isovalue = 3;
  request.levelSetRays.limits.maxWorkItems = 1;
  ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());
  EXPECT_TRUE(panel.result()->output.empty());
  EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_FALSE(panel.result()->restartGrids);
  if (!vdb::IsAvailable()) {
    EXPECT_FALSE(panel.result()->success);
    EXPECT_NE(panel.result()->message.find("ENABLE_OPENVDB=ON"),
              std::string::npos);
    return;
  }
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_NE(panel.result()->message.find("available"), std::string::npos);
  EXPECT_NE(panel.result()->message.find("miss"), std::string::npos);
  EXPECT_NEAR(
      rayAuthoringNumber(panel.result()->message, "Ray 0:", "distance="), 2,
      1e-10);
  EXPECT_NE(panel.result()->message.find("descriptor:plane"),
            std::string::npos);
  EXPECT_NE(panel.result()->message.find("ray artist"), std::string::npos);
  EXPECT_NE(panel.result()->message.find("ray scene"), std::string::npos);
  EXPECT_EQ(rayAuthoringBytes(files.directory / "plane.vdb"), original);
  EXPECT_FALSE(panel.previewOptions());

  request = submitted;
  request.levelSetRays.isovalue = .5;
  request.levelSetRays.computeNormals = false;
  request.rayQueries.resize(1);
  ASSERT_TRUE(panel.submit(request));
  ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_NEAR(
      rayAuthoringNumber(panel.result()->message, "Ray 0:", "distance="), 1.5,
      1e-10);
  EXPECT_NE(panel.result()->message.find("not-requested"), std::string::npos);

  (void)vdb::WriteDenseVdb(files.directory / "missing.vdb",
                           rayAuthoringPlane(true));
  request = submitted;
  request.input = files.path("missing.vdb");
  request.rayQueries = {{{2.25, 0, 0}, {-1, 0, 0}, 0, 3}};
  ASSERT_TRUE(panel.submit(request));
  ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_NE(panel.result()->message.find("missing-active-footprint"),
            std::string::npos);
  EXPECT_NEAR(
      rayAuthoringNumber(panel.result()->message, "Ray 0:", "distance="), 2,
      1e-10);
  request = submitted;
  request.rayQueries.clear();
  ASSERT_TRUE(panel.submit(request));
  ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());
  EXPECT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_TRUE(panel.result()->output.empty());
  EXPECT_EQ(rayAuthoringBytes(files.directory / "plane.vdb"), original);
  EXPECT_EQ(request.processing.maxInputBytes,
            submitted.processing.maxInputBytes);
  EXPECT_EQ(request.levelSetRays.limits.maxWorkItems,
            submitted.levelSetRays.limits.maxWorkItems);
}

TEST(OpenVdbAuthoring,
     RayVolumeSpansAreCoarseWorldBlocksWithCompleteCapsAndNoOutput) {
  namespace vdb = container::geometry::vdb;
  using Operation = container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;
  OpenVdbAuthoringPanel panel;
  OpenVdbAuthoringRequest request;
  request.operation = Operation::RayVolumeSpans;
  request.input = files.path("blocks.vdb");
  request.inputGrid = "descriptor:occupancy";
  request.secondInput = std::string("\xff", 1);
  request.output = files.path("protected-output.vdb");
  request.levelSetRays.refinementSteps =
      99; // Hidden SDF options are irrelevant.
  request.levelSetRays.limits.maxWorkItems = 0;
  request.volumeRays.maxSpans = 64;
  request.rayQueries = {{{-2, .25, .25}, {3, 0, 0}, 0, 80}};
  std::ofstream(files.directory / "protected-output.vdb")
      << "protected arbitrary bytes";
  const auto protectedBytes =
      rayAuthoringBytes(files.directory / "protected-output.vdb");
  if (vdb::IsAvailable()) {
    vdb::DenseArrayVolume volume;
    volume.region = {{0, 0, 0}, {65, 1, 1}};
    volume.gridName = "occupancy";
    volume.background = 0.;
    std::vector<double> values(65, -9);
    volume.active.assign(65, 0);
    for (size_t x : {size_t(0), size_t(8), size_t(24)}) {
      values[x] = 0;
      volume.active[x] = 1;
    }
    volume.values = std::move(values);
    (void)vdb::WriteDenseVdb(files.directory / "blocks.vdb", volume);
  }
  const auto source = vdb::IsAvailable()
                          ? rayAuthoringBytes(files.directory / "blocks.vdb")
                          : "";
  const auto submitted = request;
  ASSERT_TRUE(panel.submit(request));
  request.volumeRays.maxSpans = 1;
  request.rayQueries.clear();
  ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());
  EXPECT_TRUE(panel.result()->output.empty());
  EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_FALSE(panel.result()->restartGrids);
  EXPECT_EQ(rayAuthoringBytes(files.directory / "protected-output.vdb"),
            protectedBytes);
  if (!vdb::IsAvailable()) {
    EXPECT_FALSE(panel.result()->success);
    EXPECT_NE(panel.result()->message.find("ENABLE_OPENVDB=ON"),
              std::string::npos);
    return;
  }
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_NE(panel.result()->message.find("COARSE active-volume"),
            std::string::npos);
  EXPECT_NEAR(rayAuthoringNumber(panel.result()->message, "Span 0:", "entry="),
              1.5, 1e-10);
  EXPECT_NEAR(rayAuthoringNumber(panel.result()->message, "Span 0:", "exit="),
              17.5, 1e-10);
  EXPECT_NEAR(rayAuthoringNumber(panel.result()->message, "Span 1:", "entry="),
              25.5, 1e-10);
  EXPECT_NEAR(rayAuthoringNumber(panel.result()->message, "Span 1:", "exit="),
              33.5, 1e-10);
  EXPECT_EQ(rayAuthoringBytes(files.directory / "blocks.vdb"), source);
  for (const int cap : {0, 1, 2}) {
    request = submitted;
    if (cap == 0)
      request.volumeRays.maxSpans = 1;
    if (cap == 1)
      request.volumeRays.limits.maxWorkItems = 1;
    if (cap == 2)
      request.processing.maxInputBytes = 128 * 1024;
    ASSERT_TRUE(panel.submit(request));
    ASSERT_TRUE(settle(panel));
    ASSERT_TRUE(panel.result());
    EXPECT_FALSE(panel.result()->success);
    EXPECT_TRUE(panel.result()->output.empty());
    EXPECT_EQ(rayAuthoringBytes(files.directory / "blocks.vdb"), source);
    EXPECT_EQ(rayAuthoringBytes(files.directory / "protected-output.vdb"),
              protectedBytes);
  }
}

TEST(OpenVdbAuthoring,
     RayResultCannotExposeReuseRestartOrLoadEvenFromCustomExecutor) {
  using Operation = container::ui::OpenVdbAuthoringOperation;
  struct Context {
    ImGuiContext *previous = ImGui::GetCurrentContext();
    ImGuiContext *owned = ImGui::CreateContext();
    Context() {
      auto &io = ImGui::GetIO();
      io.IniFilename = nullptr;
      io.DisplaySize = {1200, 6000};
      io.DeltaTime = 1.f / 60;
      unsigned char *pixels;
      int width, height;
      io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~Context() {
      ImGui::DestroyContext(owned);
      ImGui::SetCurrentContext(previous);
    }
  } context;
  std::atomic_uint calls{0};
  OpenVdbAuthoringPanel panel([&](const OpenVdbAuthoringRequest &request) {
    ++calls;
    OpenVdbAuthoringResult result{true, "fake success", request.output, true};
    result.restartGrids = std::array<std::string, 4>{"D", "U", "M", "T"};
    return result;
  });
  panel.draft.input = "original-source.vdb";
  panel.draft.inputGrid = "original-grid";
  panel.draft.secondInput = "original-second.vdb";
  panel.draft.domainInput = "original-domain.vdb";
  panel.draft.temperatureInput = "original-temperature.vdb";
  panel.draft.output = "fake-output.vdb";
  panel.previewInput = "independent-preview.vdb";
  panel.previewGrid = "independent-grid";
  ImGuiID reuse = 0, restart = 0, load = 0;
  const auto frame = [&] {
    ImGui::NewFrame();
    ImGui::SetNextWindowSize({1150, 5900});
    ImGui::Begin("Read-only ray result actions");
    ImGui::PushID(&panel);
    reuse = ImGui::GetID("Use output as input");
    restart = ImGui::GetID("Use state as restart");
    load = ImGui::GetID("Load preview");
    ImGui::PopID();
    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    const auto selected = panel.draw();
    ImGui::End();
    ImGui::Render();
    return selected;
  };
  for (const auto operation :
       {Operation::RayIntersections, Operation::RayVolumeSpans,
        Operation::QueryActiveRegions}) {
    panel.draft.operation = operation;
    ASSERT_TRUE(panel.submit(panel.draft));
    ASSERT_TRUE(settle(panel));
    ASSERT_TRUE(panel.result());
    EXPECT_TRUE(panel.result()->success);
    EXPECT_TRUE(panel.result()->output.empty());
    EXPECT_FALSE(panel.result()->modelOutput);
    EXPECT_FALSE(panel.result()->restartGrids);
    EXPECT_FALSE(frame());
    EXPECT_FALSE(frame());
    for (auto id : {reuse, restart, load}) {
      ImGui::ActivateItemByID(id);
      EXPECT_FALSE(frame());
      EXPECT_EQ(panel.draft.operation, operation);
      EXPECT_EQ(panel.draft.input, "original-source.vdb");
      EXPECT_EQ(panel.draft.inputGrid, "original-grid");
      EXPECT_EQ(panel.draft.secondInput, "original-second.vdb");
      EXPECT_EQ(panel.draft.domainInput, "original-domain.vdb");
      EXPECT_EQ(panel.draft.temperatureInput, "original-temperature.vdb");
      EXPECT_EQ(panel.previewInput, "independent-preview.vdb");
      EXPECT_EQ(panel.previewGrid, "independent-grid");
      EXPECT_FALSE(panel.previewOptions());
    }
    // Merely browsing another operation cannot re-enable old read-only output.
    panel.draft.operation = Operation::Sphere;
    EXPECT_FALSE(frame());
    ImGui::ActivateItemByID(load);
    EXPECT_FALSE(frame());
    EXPECT_FALSE(panel.previewOptions());
  }
  EXPECT_EQ(calls, 3u);
  // A new ordinary successful job remains an explicit positive control.
  panel.draft.operation = Operation::Sphere;
  panel.draft.output = "ordinary-surface.vdb";
  ASSERT_TRUE(panel.submit(panel.draft));
  ASSERT_TRUE(settle(panel));
  EXPECT_FALSE(frame());
  EXPECT_FALSE(frame());
  ImGui::ActivateItemByID(load);
  EXPECT_EQ(frame(), std::optional<std::string>("ordinary-surface.vdb"));
  ASSERT_TRUE(panel.previewOptions());
  EXPECT_EQ(calls, 4u);
}


TEST(OpenVdbAuthoring, ValueComponentsKeepFullTypedFieldsAndUseImmutableIndependentCaps) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  const auto directory=files.directory/std::filesystem::path(u8"值组件_é");ASSERT_TRUE(std::filesystem::create_directory(directory));
  const auto sourceFile=directory/"source.vdb",outputFile=directory/"components.vdb";
  const auto text=[](const auto& file){const auto value=file.u8string();return std::string(value.begin(),value.end());};
  request.operation=Operation::ValueComponents;request.input=text(sourceFile);request.inputGrid="field";
  request.output=text(outputFile);request.valueComponents.gridNamePrefix="piece";
  request.secondInput=std::string("\xff",1);request.secondGrid="ignored";
  request.processing.maxInputBytes=0;request.processing.maxVoxelCount=0;request.processing.gridName="ignored-legacy";
  std::string original;
  vdb::DenseArrayVolume source;source.region={{-2,0,0},{5,1,1}};source.values=std::vector<double>{-0.,1.5,7.,-3.,4.25};source.background=-0.;source.active={1,1,0,1,1};source.gridName="field";
  source.linear={-.5,.25,0, 0,.5,0, 0,0,.5};source.translation={2,-1,.25};
  if(vdb::IsAvailable()) {
    (void)vdb::WriteDenseVdb(sourceFile,source);
    std::ifstream input(sourceFile,std::ios::binary);original={std::istreambuf_iterator<char>(input),{}};
  }
  const auto submitted=request.valueComponents;
  ASSERT_TRUE(panel.submit(request));request.valueComponents.gridNamePrefix="changed-after-submit";request.valueComponents.maxComponents=1;request.valueComponents.maxWork=1;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()){EXPECT_FALSE(panel.result()->success);EXPECT_NE(panel.result()->message.find("ENABLE_OPENVDB=ON"),std::string::npos);return;}
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;EXPECT_FALSE(panel.result()->modelOutput);EXPECT_FALSE(panel.result()->restartGrids);
  EXPECT_EQ(panel.result()->output,text(outputFile));
  EXPECT_NE(panel.result()->message.find("SOURCE FIELD with component ACTIVITY"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("six-connected INDEX components: 2"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("not isolated solids/SDFs"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("explicitly choose an exact output grid selector"),std::string::npos);
  for(size_t i=0;i<2;++i){
    const auto name=i?"piece_000002":"piece_000001";
    const auto field=vdb::ReadDenseVdb(outputFile,source.region,name);
    ASSERT_TRUE(std::holds_alternative<std::vector<double>>(field.values));const auto values=std::get<std::vector<double>>(field.values);
    const auto expected=std::get<std::vector<double>>(source.values);ASSERT_EQ(values.size(),expected.size());
    for(size_t n=0;n<values.size();++n)EXPECT_EQ(std::bit_cast<uint64_t>(values[n]),std::bit_cast<uint64_t>(expected[n]));
    EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(field.background)),std::bit_cast<uint64_t>(std::get<double>(source.background)));
    EXPECT_EQ(field.active,i?(std::vector<uint8_t>{0,0,0,1,1}):(std::vector<uint8_t>{1,1,0,0,0}));EXPECT_EQ(field.linear,source.linear);EXPECT_EQ(field.translation,source.translation);
  }
  request.valueComponents=submitted;request.output=files.path("too-many.vdb");request.valueComponents.maxComponents=1;
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);EXPECT_FALSE(std::filesystem::exists(files.directory/"too-many.vdb"));
  for(const auto reportCap:{65536ull,131073ull}) {
    request.valueComponents=submitted;request.output=files.path("report-cap.vdb");request.valueComponents.maxInputBytes=reportCap;
    ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);EXPECT_NE(panel.result()->message.find("report exceeds joint"),std::string::npos);EXPECT_FALSE(std::filesystem::exists(files.directory/"report-cap.vdb"));
  }
  request.valueComponents=submitted;request.output=files.path("work-cap.vdb");request.valueComponents.maxWork=1;
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);EXPECT_FALSE(std::filesystem::exists(files.directory/"work-cap.vdb"));
  for(const auto& prefix:{std::string{},std::string(129,'x'),std::string("a\0b",3)}) {
    request.valueComponents=submitted;request.valueComponents.gridNamePrefix=prefix;request.output=files.path("invalid-prefix.vdb");
    ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);EXPECT_NE(panel.result()->message.find("UI name prefix requires1..128"),std::string::npos);EXPECT_FALSE(std::filesystem::exists(files.directory/"invalid-prefix.vdb"));
  }
  request.valueComponents=submitted;request.output=text(outputFile);
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);
  std::ifstream input(sourceFile,std::ios::binary);EXPECT_EQ((std::string{std::istreambuf_iterator<char>(input),{}}),original);
  EXPECT_EQ(request.valueComponents.maxInputBytes,submitted.maxInputBytes);EXPECT_EQ(request.valueComponents.maxWork,submitted.maxWork);
}

TEST(OpenVdbAuthoring, ValueComponentsEmptyActivityRetainsInactiveDataAndReportsAnExplicitCopy) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::ValueComponents;request.input=files.path("empty.vdb");request.inputGrid="field";request.output=files.path("copy.vdb");
  vdb::DenseArrayVolume source;source.region={{-1,0,0},{3,1,1}};source.gridName="field";source.values=std::vector<double>{-0.,7.,-3.};source.background=-0.;source.active={0,0,0};
  if(vdb::IsAvailable())(void)vdb::WriteDenseVdb(files.directory/"empty.vdb",source);
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()){EXPECT_FALSE(panel.result()->success);return;}
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_NE(panel.result()->message.find("six-connected INDEX components: 0; output grids: 1"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("one inactive source-field copy (_000000)"),std::string::npos);
  const auto copied=vdb::ReadDenseVdb(files.directory/"copy.vdb",source.region,"component_000000");
  const auto values=std::get<std::vector<double>>(copied.values);const auto expected=std::get<std::vector<double>>(source.values);
  ASSERT_EQ(values.size(),expected.size());for(size_t n=0;n<values.size();++n)EXPECT_EQ(std::bit_cast<uint64_t>(values[n]),std::bit_cast<uint64_t>(expected[n]));
  EXPECT_EQ(copied.active,source.active);
}

TEST(OpenVdbAuthoring, ValueComponentSubmissionOwnsEveryNativeOptionAndSuppressesModelRestartClaims) {
  using Operation=container::ui::OpenVdbAuthoringOperation;
  std::promise<OpenVdbAuthoringRequest> capture;auto captured=capture.get_future();
  OpenVdbAuthoringPanel panel([&](const auto& value){capture.set_value(value);OpenVdbAuthoringResult result{true,"SOURCE FIELD; choose an explicit output grid selector",value.output,true};result.restartGrids=std::array<std::string,4>{"a","b","c","d"};return result;});
  OpenVdbAuthoringRequest request;request.operation=Operation::ValueComponents;request.input="source.vdb";request.inputGrid="chosen";request.output="components.vdb";
  auto& o=request.valueComponents;o.gridNamePrefix="chosen-prefix";o.maxComponents=17;o.maxGrids=18;o.maxActiveVoxels=UINT64_MAX-1;o.maxStoredLeafVoxels=21;o.maxAggregateOutputLeafVoxels=22;o.maxNodes=23;o.maxWork=987654321;o.maxInputBytes=123456789;o.maxOutputBytes=223456789;o.maxMetadataBytes=1919;
  const auto original=request;
  ASSERT_TRUE(panel.submit(request));request.input="changed";request.inputGrid.clear();request.output="changed.vdb";request.valueComponents={};
  ASSERT_EQ(captured.wait_for(5s),std::future_status::ready);const auto saved=captured.get();
  EXPECT_EQ(saved.operation,Operation::ValueComponents);EXPECT_EQ(saved.input,original.input);EXPECT_EQ(saved.inputGrid,original.inputGrid);EXPECT_EQ(saved.output,original.output);
  const auto& actual=saved.valueComponents;const auto& expected=original.valueComponents;
  EXPECT_EQ(actual.gridNamePrefix,expected.gridNamePrefix);EXPECT_EQ(actual.maxComponents,expected.maxComponents);EXPECT_EQ(actual.maxGrids,expected.maxGrids);EXPECT_EQ(actual.maxActiveVoxels,expected.maxActiveVoxels);EXPECT_EQ(actual.maxStoredLeafVoxels,expected.maxStoredLeafVoxels);EXPECT_EQ(actual.maxAggregateOutputLeafVoxels,expected.maxAggregateOutputLeafVoxels);EXPECT_EQ(actual.maxNodes,expected.maxNodes);EXPECT_EQ(actual.maxWork,expected.maxWork);EXPECT_EQ(actual.maxInputBytes,expected.maxInputBytes);EXPECT_EQ(actual.maxOutputBytes,expected.maxOutputBytes);EXPECT_EQ(actual.maxMetadataBytes,expected.maxMetadataBytes);
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_TRUE(panel.result()->success);EXPECT_EQ(panel.result()->output,original.output);EXPECT_FALSE(panel.result()->modelOutput);EXPECT_FALSE(panel.result()->restartGrids);
}

std::string surfaceUiBytes(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), {}};
}

std::string surfaceUiUtf8(const std::filesystem::path &path) {
  const auto value = path.u8string();
  return {value.begin(), value.end()};
}

struct SurfaceUiExr {
  uint32_t width{}, height{};
  std::map<std::string, std::vector<float>> channels;
  std::string metadata;
  explicit SurfaceUiExr(const std::filesystem::path &path) {
    const auto data = surfaceUiBytes(path);
    const auto u32 = [&](size_t offset) {
      if (offset > data.size() || data.size() - offset < 4)
        throw std::runtime_error("truncated EXR integer");
      uint32_t value = 0;
      for (unsigned i = 0; i < 4; ++i)
        value |= uint32_t(static_cast<unsigned char>(data[offset + i]))
                 << (8 * i);
      return value;
    };
    const auto u64 = [&](size_t offset) {
      return uint64_t(u32(offset)) | (uint64_t(u32(offset + 4)) << 32);
    };
    const auto string = [&](size_t &offset) {
      const auto end = data.find('\0', offset);
      if (end == std::string::npos)
        throw std::runtime_error("unterminated EXR string");
      const auto result = data.substr(offset, end - offset);
      offset = end + 1;
      return result;
    };
    if (u32(0) != 20000630 || u32(4) != 2)
      throw std::runtime_error("expected single-part scanline EXR v2");
    size_t offset = 8;
    std::map<std::string, std::pair<std::string, std::string>> attributes;
    while (offset < data.size() && data[offset] != '\0') {
      const auto name = string(offset), type = string(offset);
      const auto size = u32(offset);
      offset += 4;
      if (size > data.size() - offset || attributes.contains(name))
        throw std::runtime_error("invalid EXR attribute");
      attributes.emplace(name, std::pair{type, data.substr(offset, size)});
      offset += size;
    }
    if (offset >= data.size())
      throw std::runtime_error("missing EXR header terminator");
    ++offset;
    const auto &compression = attributes.at("compression");
    if (compression.first != "compression" ||
        compression.second != std::string(1, '\0'))
      throw std::runtime_error("expected uncompressed EXR");
    const auto &window = attributes.at("dataWindow");
    if (window.first != "box2i" || window.second.size() != 16)
      throw std::runtime_error("invalid EXR window");
    const auto raw32 = [](const std::string &bytes, size_t position) {
      uint32_t value = 0;
      for (unsigned i = 0; i < 4; ++i)
        value |= uint32_t(static_cast<unsigned char>(bytes.at(position + i)))
                 << (8 * i);
      return value;
    };
    if (raw32(window.second, 0) || raw32(window.second, 4))
      throw std::runtime_error("expected zero-origin EXR");
    width = raw32(window.second, 8) + 1;
    height = raw32(window.second, 12) + 1;
    if (!width || !height || width > 2048 || height > 2048)
      throw std::runtime_error("invalid EXR dimensions");
    const auto &list = attributes.at("channels");
    if (list.first != "chlist")
      throw std::runtime_error("missing EXR channels");
    std::vector<std::string> names;
    for (size_t p = 0; p < list.second.size() && list.second[p];) {
      const auto end = list.second.find('\0', p);
      if (end == std::string::npos || list.second.size() - end < 17)
        throw std::runtime_error("invalid EXR channel record");
      names.push_back(list.second.substr(p, end - p));
      p = end + 1;
      if (raw32(list.second, p) != 2 || raw32(list.second, p + 8) != 1 ||
          raw32(list.second, p + 12) != 1)
        throw std::runtime_error("expected full-resolution FLOAT EXR");
      p += 16;
    }
    const std::vector<std::string> expected{
        "A",   "B",   "G",   "HIT.KIND", "N.STATUS", "N.VALID",
        "N.X", "N.Y", "N.Z", "R",        "Z"};
    if (names != expected)
      throw std::runtime_error("expected eleven lexical AOV channels");
    for (const auto &name : names)
      channels[name].resize(size_t(width) * height);
    const size_t tableEnd = offset + size_t(height) * 8;
    std::set<uint32_t> rows;
    for (uint32_t row = 0; row < height; ++row) {
      const auto chunk = u64(offset + row * 8);
      if (chunk < tableEnd || chunk > data.size())
        throw std::runtime_error("invalid EXR scanline offset");
      const auto y = u32(size_t(chunk)), size = u32(size_t(chunk) + 4);
      if (y >= height || !rows.insert(y).second || size != width * 11 * 4 ||
          size > data.size() - size_t(chunk) - 8)
        throw std::runtime_error("invalid EXR scanline");
      for (size_t c = 0; c < names.size(); ++c)
        for (uint32_t x = 0; x < width; ++x) {
          const auto value = std::bit_cast<float>(
              u32(size_t(chunk) + 8 + (c * width + x) * 4));
          if (!std::isfinite(value))
            throw std::runtime_error("nonfinite reference EXR value");
          channels[names[c]][size_t(y) * width + x] = value;
        }
    }
    const auto &custom = attributes.at("container.surface_reference");
    if (custom.first != "string")
      throw std::runtime_error("surface reference metadata is not a string");
    metadata = custom.second;
  }
};

container::geometry::vdb::DenseArrayVolume
surfaceUiPlane(bool missing = false) {
  namespace vdb = container::geometry::vdb;
  vdb::DenseArrayVolume volume;
  volume.region = {{-3, -3, -3}, {7, 7, 7}};
  volume.gridName = "descriptor:plane";
  volume.gridClass = vdb::DenseGridClass::LevelSet;
  volume.background = 4.;
  std::vector<double> values;
  volume.active.clear();
  for (int z = -3; z <= 3; ++z)
    for (int y = -3; y <= 3; ++y)
      for (int x = -3; x <= 3; ++x) {
        const bool on = !(missing && x == 1 && y == 1 && z == 1);
        values.push_back(on ? z - .25 : 4);
        volume.active.push_back(uint8_t(on));
      }
  volume.values = std::move(values);
  return volume;
}

TEST(OpenVdbAuthoring,
     SurfaceReferenceSubmissionOwnsAllCameraShaderLimitsAndExportStateEvenOff) {
  namespace vdb = container::geometry::vdb;
  using Operation = container::ui::OpenVdbAuthoringOperation;
  std::promise<OpenVdbAuthoringRequest> captured;
  auto future = captured.get_future();
  OpenVdbAuthoringPanel panel([&](const auto &request) {
    captured.set_value(request);
    OpenVdbAuthoringResult result{true, "captured", "malicious.exr", true};
    result.restartGrids = std::array<std::string, 4>{"D", "U", "M", "T"};
    return result;
  });
  OpenVdbAuthoringRequest request;
  request.operation = Operation::RenderSurfaceReference;
  request.input = "source.vdb";
  request.inputGrid = "descriptor:literal";
  request.output = "chosen.exr";
  request.surfaceCamera.projection = vdb::SurfaceProjection::Orthographic;
  request.surfaceCamera.eye = {1, 2, 3};
  request.surfaceCamera.target = {.125, .25, .5};
  request.surfaceCamera.up = {0, 1, .125};
  request.surfaceCamera.verticalFovDegrees = 37;
  request.surfaceCamera.orthographicHeight = 7.25;
  request.surfaceCamera.minimumDistance = .125;
  request.surfaceCamera.maximumDistance = 31;
  auto &o = request.surfaceReference;
  o.width = 13;
  o.height = 7;
  o.maxPixels = 99;
  o.shading = vdb::SurfaceShading::WorldNormal;
  o.color = {.125, .25, .5};
  o.background = {.1, .2, .3};
  o.invalidNormalColor = {.9, .8, .7};
  o.rays.isovalue = .25;
  o.rays.refinementSteps = 0;
  o.rays.computeNormals = false;
  o.rays.limits = {111, 13, 3'000'000'000ull, 65536};
  request.surfaceExportBytes = 123'456'789;
  request.processing.maxInputBytes = 345'678'901;
  request.processing.maxVoxelCount = UINT64_MAX - 3;
  const auto original = request;
  ASSERT_TRUE(panel.submit(request));
  request.input = request.inputGrid = request.output = "changed";
  request.surfaceCamera = {};
  request.surfaceReference = {};
  request.surfaceExportBytes = 1;
  request.processing.maxInputBytes = request.processing.maxVoxelCount = 0;
  panel.draft.operation = Operation::Sphere;
  ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
  const auto saved = future.get();
  EXPECT_EQ(saved.operation, original.operation);
  EXPECT_EQ(saved.input, original.input);
  EXPECT_EQ(saved.inputGrid, original.inputGrid);
  EXPECT_EQ(saved.output, original.output);
  const auto &a = saved.surfaceCamera, &b = original.surfaceCamera;
  EXPECT_EQ(a.projection, b.projection);
  EXPECT_EQ(a.eye, b.eye);
  EXPECT_EQ(a.target, b.target);
  EXPECT_EQ(a.up, b.up);
  EXPECT_EQ(a.verticalFovDegrees, b.verticalFovDegrees);
  EXPECT_EQ(a.orthographicHeight, b.orthographicHeight);
  EXPECT_EQ(a.minimumDistance, b.minimumDistance);
  EXPECT_EQ(a.maximumDistance, b.maximumDistance);
  const auto &actual = saved.surfaceReference,
             &expected = original.surfaceReference;
  EXPECT_EQ(actual.width, expected.width);
  EXPECT_EQ(actual.height, expected.height);
  EXPECT_EQ(actual.maxPixels, expected.maxPixels);
  EXPECT_EQ(actual.shading, expected.shading);
  EXPECT_EQ(actual.color, expected.color);
  EXPECT_EQ(actual.background, expected.background);
  EXPECT_EQ(actual.invalidNormalColor, expected.invalidNormalColor);
  EXPECT_EQ(actual.rays.isovalue, expected.rays.isovalue);
  EXPECT_EQ(actual.rays.refinementSteps, expected.rays.refinementSteps);
  EXPECT_EQ(actual.rays.computeNormals, expected.rays.computeNormals);
  EXPECT_EQ(actual.rays.limits.maxRays, expected.rays.limits.maxRays);
  EXPECT_EQ(actual.rays.limits.maxGrids, expected.rays.limits.maxGrids);
  EXPECT_EQ(actual.rays.limits.maxWorkItems, expected.rays.limits.maxWorkItems);
  EXPECT_EQ(actual.rays.limits.maxMetadataBytes,
            expected.rays.limits.maxMetadataBytes);
  EXPECT_EQ(saved.surfaceExportBytes, original.surfaceExportBytes);
  EXPECT_EQ(saved.processing.maxInputBytes, original.processing.maxInputBytes);
  EXPECT_EQ(saved.processing.maxVoxelCount, original.processing.maxVoxelCount);
  ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());
  EXPECT_TRUE(panel.result()->success);
  EXPECT_TRUE(panel.result()->output.empty());
  EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_FALSE(panel.result()->restartGrids);
  EXPECT_FALSE(panel.previewOptions());
}

TEST(
    OpenVdbAuthoring,
    SurfaceReferenceDefaultExecutorWritesBothCamerasNativeAovsAndUnicodeArtifact) {
  namespace vdb = container::geometry::vdb;
  using Operation = container::ui::OpenVdbAuthoringOperation;
  if (!vdb::IsAvailable())
    GTEST_SKIP() << "Actual EXR image requires OpenVDB";
  AuthoringFiles files;
  (void)vdb::WriteDenseVdb(files.directory / "plane-raw.vdb", surfaceUiPlane());
  std::ofstream(files.directory / "metadata.json")
      << R"({"version":1,"inputs":[{"path":"plane-raw.vdb","edits":[{"selector":"descriptor:descriptor:plane","metadata":[{"name":"artist","operation":"set","type":"string","value":"surface artist"}]}]}],"metadata":[{"name":"scene","operation":"set","type":"string","value":"surface scene"}]})";
  (void)vdb::WriteAuthoredVdbSpec(files.directory / "metadata.json",
                                  files.directory / "plane.vdb");
  const auto source = files.directory / std::filesystem::path(u"source_球.vdb");
  std::filesystem::copy_file(files.directory / "plane.vdb", source);
  const auto before = surfaceUiBytes(source);
  const auto unicode =
      files.directory / std::filesystem::path(u"surface_球_é.exr");
  OpenVdbAuthoringRequest request;
  request.operation = Operation::RenderSurfaceReference;
  request.input = surfaceUiUtf8(source);
  request.inputGrid = "descriptor:descriptor:plane";
  request.output = surfaceUiUtf8(unicode);
  request.surfaceReference.width = request.surfaceReference.height = 1;
  request.surfaceReference.color = {.25, .5, 1};
  request.surfaceReference.rays.limits.maxWorkItems = 2'000'000'000;
  request.surfaceCamera.orthographicHeight =
      std::numeric_limits<double>::quiet_NaN();
  request.secondInput = std::string("\xff", 1);
  request.processing.voxelSize = request.processing.halfWidth = 0;
  request.processing.maxTriangles = 0;
  request.processing.gridName = std::string("\xff", 1);
  request.maxPoints = 0;
  request.pointAttributes = {0, 0, 0, 0, 0};
  request.pointSelection.anyOf = {"stale unavailable group"};
  request.surfaceFields.maxWorkItems =
      request.levelSetRays.limits.maxWorkItems = 0;
  OpenVdbAuthoringPanel panel;
  const auto original = request;
  ASSERT_TRUE(panel.submit(request));
  request.surfaceCamera.eye = {100, 100, 100};
  request.surfaceReference.width = 999;
  request.surfaceReference.color = {0, 0, 0};
  request.surfaceExportBytes = 1;
  ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_TRUE(panel.result()->output.empty());
  EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_FALSE(panel.result()->restartGrids);
  EXPECT_FALSE(panel.previewOptions());
  const auto message = panel.result()->message;
  for (const auto *term :
       {"EXR output ", "11 channels", "Pixel 0:", "depth=2.75",
        "normalStatus=available", "surface artist", "surface scene"})
    EXPECT_NE(message.find(term), std::string::npos) << message;
  SurfaceUiExr first(unicode);
  EXPECT_EQ(first.width, 1u);
  EXPECT_EQ(first.height, 1u);
  EXPECT_EQ(first.channels.at("Z"), std::vector<float>{2.75f});
  EXPECT_EQ(first.channels.at("A"), std::vector<float>{1});
  EXPECT_EQ(first.channels.at("N.STATUS"), std::vector<float>{2});
  EXPECT_EQ(first.channels.at("N.VALID"), std::vector<float>{1});
  EXPECT_EQ(first.channels.at("N.Z"), std::vector<float>{1});
  EXPECT_EQ(first.channels.at("R"), std::vector<float>{.25f});
  EXPECT_EQ(first.channels.at("G"), std::vector<float>{.5f});
  EXPECT_EQ(first.channels.at("B"), std::vector<float>{1});
  EXPECT_NE(first.metadata.find("distances=Euclidean WORLD"),
            std::string::npos);
  EXPECT_NE(first.metadata.find("N.STATUS="), std::string::npos);
  EXPECT_NE(first.metadata.find("HIT.KIND="), std::string::npos);
  EXPECT_EQ(surfaceUiBytes(source), before);

  request = original;
  request.output = files.path("orthographic.exr");
  request.surfaceCamera.projection = vdb::SurfaceProjection::Orthographic;
  request.surfaceCamera.verticalFovDegrees =
      std::numeric_limits<double>::quiet_NaN();
  request.surfaceCamera.orthographicHeight = 2;
  request.surfaceReference.width = 2;
  request.surfaceReference.shading = vdb::SurfaceShading::WorldNormal;
  ASSERT_TRUE(panel.submit(request));
  ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  SurfaceUiExr second(files.directory / "orthographic.exr");
  EXPECT_EQ(second.width, 2u);
  EXPECT_EQ(second.channels.at("Z"), (std::vector<float>{2.75f, 2.75f}));
  EXPECT_EQ(second.channels.at("R"), (std::vector<float>{.125f, .125f}));
  EXPECT_EQ(second.channels.at("G"), (std::vector<float>{.25f, .25f}));
  EXPECT_EQ(second.channels.at("B"), (std::vector<float>{1, 1}));
  EXPECT_NE(second.metadata.find("projection=orthographic"), std::string::npos);

  (void)vdb::WriteDenseVdb(files.directory / "missing.vdb",
                           surfaceUiPlane(true));
  request = original;
  request.input = files.path("missing.vdb");
  request.output = files.path("missing.exr");
  request.surfaceReference.shading = vdb::SurfaceShading::WorldNormal;
  request.surfaceReference.invalidNormalColor = {.25, .5, .75};
  ASSERT_TRUE(panel.submit(request));
  ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  EXPECT_NE(
      panel.result()->message.find("normalStatus=missing-active-footprint"),
      std::string::npos);
  SurfaceUiExr missing(files.directory / "missing.exr");
  EXPECT_EQ(missing.channels.at("A"), std::vector<float>{1});
  EXPECT_EQ(missing.channels.at("N.STATUS"), std::vector<float>{3});
  EXPECT_EQ(missing.channels.at("N.VALID"), std::vector<float>{0});
  EXPECT_EQ(missing.channels.at("N.Z"), std::vector<float>{0});
  EXPECT_EQ(missing.channels.at("B"), std::vector<float>{.75f});
  request = original;
  request.output = files.path("matte.exr");
  request.surfaceReference.shading = vdb::SurfaceShading::Matte;
  request.surfaceReference.rays.computeNormals = false;
  request.surfaceReference.invalidNormalColor = {
      std::numeric_limits<double>::quiet_NaN(), -1, 2};
  ASSERT_TRUE(panel.submit(request));
  ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success) << panel.result()->message;
  SurfaceUiExr matte(files.directory / "matte.exr");
  EXPECT_EQ(matte.channels.at("A"), std::vector<float>{1});
  EXPECT_EQ(matte.channels.at("N.STATUS"), std::vector<float>{0});
  EXPECT_EQ(matte.channels.at("R"), std::vector<float>{.25f});
  EXPECT_EQ(surfaceUiBytes(source), before);
}

TEST(
    OpenVdbAuthoring,
    SurfaceReferenceFailureCapsProtectSourceOutputAndOffAvailabilityComesFirst) {
  namespace vdb = container::geometry::vdb;
  using Operation = container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;
  OpenVdbAuthoringPanel panel;
  OpenVdbAuthoringRequest request;
  request.operation = Operation::RenderSurfaceReference;
  request.input = files.path("plane.vdb");
  request.output = files.path("image.exr");
  request.surfaceReference.width = request.surfaceReference.height = 1;
  if (!vdb::IsAvailable()) {
    request.input = request.output = std::string("\xff", 1);
    request.secondInput = std::string("\xc0\xaf", 2);
    request.surfaceCamera.target = request.surfaceCamera.eye;
    request.surfaceReference.width = 0;
    request.surfaceReference.rays.limits.maxWorkItems = 0;
    request.processing.maxInputBytes = request.processing.maxVoxelCount = 0;
    request.surfaceExportBytes = 0;
    ASSERT_TRUE(panel.submit(request));
    ASSERT_TRUE(settle(panel));
    ASSERT_TRUE(panel.result());
    EXPECT_FALSE(panel.result()->success);
    EXPECT_NE(panel.result()->message.find("ENABLE_OPENVDB=ON"),
              std::string::npos);
    EXPECT_TRUE(panel.result()->output.empty());
    EXPECT_FALSE(panel.result()->modelOutput);
    EXPECT_FALSE(panel.result()->restartGrids);
    EXPECT_TRUE(std::filesystem::is_empty(files.directory));
    return;
  }
  (void)vdb::WriteDenseVdb(files.directory / "plane.vdb", surfaceUiPlane());
  const auto source = surfaceUiBytes(files.directory / "plane.vdb");
  const auto good = request;
  for (int failure = 0; failure < 8; ++failure) {
    request = good;
    const auto name = "failed-" + std::to_string(failure) + ".exr";
    request.output = surfaceUiUtf8(files.directory / name);
    if (failure == 0)
      request.processing.maxInputBytes = 131072;
    if (failure == 1)
      request.surfaceExportBytes = 1;
    if (failure == 2)
      request.surfaceReference.rays.limits.maxWorkItems = 1;
    if (failure == 3)
      request.processing.maxVoxelCount = 1;
    if (failure == 4)
      request.surfaceReference.rays.limits.maxMetadataBytes = 1;
    if (failure == 5) {
      request.surfaceReference.width = 2;
      request.surfaceReference.maxPixels = 1;
    }
    if (failure == 6)
      request.inputGrid = "descriptor:missing";
    if (failure == 7)
      request.surfaceCamera.target = request.surfaceCamera.eye;
    ASSERT_TRUE(panel.submit(request));
    ASSERT_TRUE(settle(panel));
    ASSERT_TRUE(panel.result());
    EXPECT_FALSE(panel.result()->success) << failure;
    EXPECT_FALSE(std::filesystem::exists(files.directory / name));
    EXPECT_EQ(surfaceUiBytes(files.directory / "plane.vdb"), source);
  }
  std::ofstream(files.directory / "protected.exr", std::ios::binary)
      << "protected original bytes";
  const auto protectedBytes = surfaceUiBytes(files.directory / "protected.exr");
  request = good;
  request.output = files.path("protected.exr");
  ASSERT_TRUE(panel.submit(request));
  ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());
  EXPECT_FALSE(panel.result()->success);
  EXPECT_EQ(surfaceUiBytes(files.directory / "protected.exr"), protectedBytes);
  request.output = request.input;
  ASSERT_TRUE(panel.submit(request));
  ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());
  EXPECT_FALSE(panel.result()->success);
  EXPECT_EQ(surfaceUiBytes(files.directory / "plane.vdb"), source);
}

TEST(OpenVdbAuthoring,
     SurfaceReferenceArtifactsNeverExposeVdbActionsEvenAfterChangingDraft) {
  using Operation = container::ui::OpenVdbAuthoringOperation;
  struct Context {
    ImGuiContext *previous = ImGui::GetCurrentContext();
    ImGuiContext *owned = ImGui::CreateContext();
    Context() {
      auto &io = ImGui::GetIO();
      io.IniFilename = nullptr;
      io.DisplaySize = {1200, 7000};
      io.DeltaTime = 1.f / 60;
      unsigned char *pixels;
      int width, height;
      io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~Context() {
      ImGui::DestroyContext(owned);
      ImGui::SetCurrentContext(previous);
    }
  } context;
  std::atomic_uint calls{0};
  OpenVdbAuthoringPanel panel([&](const auto &request) {
    ++calls;
    OpenVdbAuthoringResult result{true, "custom success", request.output, true};
    result.restartGrids = std::array<std::string, 4>{"D", "U", "M", "T"};
    return result;
  });
  panel.draft.operation = Operation::RenderSurfaceReference;
  panel.draft.input = "original-source.vdb";
  panel.draft.inputGrid = "original-grid";
  panel.draft.secondInput = "original-second.vdb";
  panel.draft.domainInput = "original-domain.vdb";
  panel.draft.temperatureInput = "original-temperature.vdb";
  panel.draft.output = "malicious.exr";
  panel.previewInput = "independent-preview.vdb";
  panel.previewGrid = "independent-grid";
  ImGuiID reuse = 0, restart = 0, load = 0;
  const auto frame = [&] {
    ImGui::NewFrame();
    ImGui::SetNextWindowSize({1150, 6900});
    ImGui::Begin("Surface reference result actions");
    ImGui::PushID(&panel);
    reuse = ImGui::GetID("Use output as input");
    restart = ImGui::GetID("Use state as restart");
    load = ImGui::GetID("Load preview");
    ImGui::PopID();
    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    const auto selected = panel.draw();
    ImGui::End();
    ImGui::Render();
    return selected;
  };
  ASSERT_TRUE(panel.submit(panel.draft));
  ASSERT_TRUE(settle(panel));
  ASSERT_TRUE(panel.result());
  EXPECT_TRUE(panel.result()->success);
  EXPECT_TRUE(panel.result()->output.empty());
  EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_FALSE(panel.result()->restartGrids);
  for (auto draft : {Operation::RenderSurfaceReference, Operation::Sphere}) {
    panel.draft.operation = draft;
    EXPECT_FALSE(frame());
    EXPECT_FALSE(frame());
    for (const auto id : {reuse, restart, load}) {
      ImGui::ActivateItemByID(id);
      EXPECT_FALSE(frame());
      EXPECT_EQ(panel.draft.operation, draft);
      EXPECT_EQ(panel.draft.input, "original-source.vdb");
      EXPECT_EQ(panel.draft.inputGrid, "original-grid");
      EXPECT_EQ(panel.draft.secondInput, "original-second.vdb");
      EXPECT_EQ(panel.draft.domainInput, "original-domain.vdb");
      EXPECT_EQ(panel.draft.temperatureInput, "original-temperature.vdb");
      EXPECT_EQ(panel.previewInput, "independent-preview.vdb");
      EXPECT_EQ(panel.previewGrid, "independent-grid");
      EXPECT_FALSE(panel.previewOptions());
    }
  }
  EXPECT_EQ(calls, 1u);
  panel.draft.operation = Operation::Sphere;
  panel.draft.output = "new-surface.vdb";
  ASSERT_TRUE(panel.submit(panel.draft));
  ASSERT_TRUE(settle(panel));
  EXPECT_FALSE(frame());
  EXPECT_FALSE(frame());
  ImGui::ActivateItemByID(load);
  EXPECT_EQ(frame(), std::optional<std::string>("new-surface.vdb"));
  ASSERT_TRUE(panel.previewOptions());
  EXPECT_EQ(calls, 2u);
}

TEST(OpenVdbAuthoring, ParticleGeometrySnapshotsEveryControlAndSanitizesSubmittedArtifacts) {
  namespace vdb=container::geometry::vdb;
  using Operation=container::ui::OpenVdbAuthoringOperation;
  for(const auto mode:{vdb::ParticleGeometryOutput::LevelSet,vdb::ParticleGeometryOutput::Mask,vdb::ParticleGeometryOutput::Both}) {
    std::promise<OpenVdbAuthoringRequest> capture;auto captured=capture.get_future();
    OpenVdbAuthoringPanel panel([&](const auto& request) {
      capture.set_value(request);
      OpenVdbAuthoringResult result{true,"custom result","foreign.vdb",true};
      result.restartGrids=std::array<std::string,4>{"D","U","M","T"};
      result.particleGeometryArtifact=container::ui::OpenVdbParticleGeometryArtifact{"foreign.vdb","descriptor:foreign-sdf","descriptor:foreign-mask"};
      return result;
    });
    OpenVdbAuthoringRequest request;request.operation=Operation::ParticleGeometry;
    request.input="submitted.json";request.output="submitted.vdb";request.inputGrid="ignored-hidden-grid";
    auto& o=request.particleGeometry;o.output=mode;o.timeStep=-.75;o.spacing=.5;
    o.minRadiusVoxels=2;o.maxRadiusVoxels=17;o.originWorld=std::array<double,3>{1.5,-2.5,3.5};
    o.maskGridName="submitted-mask";o.maxParticles=19;o.maxTrailInstances=27;
    o.maxTrailLengthVoxels=4096;o.maxNodes=122;o.maxWork=987654321;
    o.maxMetadataBytes=1919;o.maxOutputBytes=223456789;
    request.processing.gridName="submitted-sdf";request.processing.voxelSize=.25;
    request.processing.halfWidth=5;request.processing.maxVoxelCount=UINT64_MAX-7;
    request.processing.maxInputBytes=123456789;request.processing.maxTriangles=77;
    const auto submitted=request;
    ASSERT_TRUE(panel.submit(request));request={};panel.draft={};panel.draft.operation=Operation::Sphere;
    ASSERT_EQ(captured.wait_for(5s),std::future_status::ready);const auto saved=captured.get();
    EXPECT_EQ(saved.operation,submitted.operation);EXPECT_EQ(saved.input,submitted.input);EXPECT_EQ(saved.output,submitted.output);
    EXPECT_EQ(saved.inputGrid,submitted.inputGrid);
    const auto& a=saved.particleGeometry;const auto& b=submitted.particleGeometry;
    EXPECT_EQ(a.output,b.output);EXPECT_EQ(a.timeStep,b.timeStep);EXPECT_EQ(a.spacing,b.spacing);
    EXPECT_EQ(a.minRadiusVoxels,b.minRadiusVoxels);EXPECT_EQ(a.maxRadiusVoxels,b.maxRadiusVoxels);
    EXPECT_EQ(a.originWorld,b.originWorld);EXPECT_EQ(a.maskGridName,b.maskGridName);
    EXPECT_EQ(a.maxParticles,b.maxParticles);EXPECT_EQ(a.maxTrailInstances,b.maxTrailInstances);
    EXPECT_EQ(a.maxTrailLengthVoxels,b.maxTrailLengthVoxels);EXPECT_EQ(a.maxNodes,b.maxNodes);
    EXPECT_EQ(a.maxWork,b.maxWork);EXPECT_EQ(a.maxMetadataBytes,b.maxMetadataBytes);EXPECT_EQ(a.maxOutputBytes,b.maxOutputBytes);
    EXPECT_EQ(saved.processing.gridName,submitted.processing.gridName);EXPECT_EQ(saved.processing.voxelSize,submitted.processing.voxelSize);
    EXPECT_EQ(saved.processing.halfWidth,submitted.processing.halfWidth);EXPECT_EQ(saved.processing.maxVoxelCount,submitted.processing.maxVoxelCount);
    EXPECT_EQ(saved.processing.maxInputBytes,submitted.processing.maxInputBytes);EXPECT_EQ(saved.processing.maxTriangles,submitted.processing.maxTriangles);
    ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success);
    EXPECT_TRUE(panel.result()->output.empty());EXPECT_FALSE(panel.result()->restartGrids);
    EXPECT_EQ(panel.result()->modelOutput,mode!=vdb::ParticleGeometryOutput::Mask);
    ASSERT_TRUE(panel.result()->particleGeometryArtifact);const auto& artifact=*panel.result()->particleGeometryArtifact;
    EXPECT_EQ(artifact.output,submitted.output);
    EXPECT_EQ(artifact.sdfGrid,mode==vdb::ParticleGeometryOutput::Mask?std::nullopt:std::optional<std::string>("descriptor:submitted-sdf"));
    EXPECT_EQ(artifact.maskGrid,mode==vdb::ParticleGeometryOutput::LevelSet?std::nullopt:std::optional<std::string>("descriptor:submitted-mask"));
  }
}

TEST(OpenVdbAuthoring, ParticleGeometryDefaultExecutorWritesIndependentTaperAndMaskOracles) {
  namespace vdb=container::geometry::vdb;
  using Operation=container::ui::OpenVdbAuthoringOperation;
  if(!vdb::IsAvailable())GTEST_SKIP()<<"OpenVDB disabled";
  AuthoringFiles files;
  const auto directory=files.directory/std::filesystem::path(u8"粒子_é");ASSERT_TRUE(std::filesystem::create_directory(directory));
  const auto source=directory/std::filesystem::path(u8"输入.json"),output=directory/std::filesystem::path(u8"双网格.vdb");
  std::ofstream(source)<<R"({"version":1,"particles":[{"position":[0,0,0],"radius":4,"velocity":[4,0,0]}]})";
  const auto original=surfaceUiBytes(source);
  OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::ParticleGeometry;request.input=surfaceUiUtf8(source);request.output=surfaceUiUtf8(output);
  request.particleGeometry.output=vdb::ParticleGeometryOutput::Both;
  request.particleGeometry.minRadiusVoxels=2;request.particleGeometry.maxRadiusVoxels=4;
  request.particleGeometry.originWorld=std::array<double,3>{0,0,0};request.particleGeometry.maskGridName="occupied";
  request.processing.voxelSize=1;request.processing.gridName="taper";request.processing.halfWidth=3;
  request.inputGrid="ignored";request.secondInput=std::string("\xff",1);request.secondGrid="ignored";
  const auto submitted=request;
  ASSERT_TRUE(panel.submit(request));request.particleGeometry.timeStep=0;request.processing.gridName="changed";
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_EQ(surfaceUiBytes(source),original);EXPECT_TRUE(panel.result()->output.empty());EXPECT_FALSE(panel.result()->restartGrids);
  EXPECT_TRUE(panel.result()->modelOutput);ASSERT_TRUE(panel.result()->particleGeometryArtifact);
  EXPECT_EQ(panel.result()->particleGeometryArtifact->output,surfaceUiUtf8(output));
  EXPECT_EQ(panel.result()->particleGeometryArtifact->sdfGrid,std::optional<std::string>("descriptor:taper"));
  EXPECT_EQ(panel.result()->particleGeometryArtifact->maskGrid,std::optional<std::string>("descriptor:occupied"));
  EXPECT_NE(panel.result()->message.find("actual stamps/instance upper bound: 3/"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("GRID_UNKNOWN"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("not a continuous capsule"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("UI report/path/options bytes/work reserved"),std::string::npos);
  const auto field=vdb::ReadDenseVdb(output,{{-6,0,0},{11,1,1}},"descriptor:taper");
  ASSERT_TRUE(std::holds_alternative<std::vector<float>>(field.values));const auto& values=std::get<std::vector<float>>(field.values);
  ASSERT_EQ(values.size(),11u);EXPECT_EQ(field.linear,(std::array<double,9>{1,0,0,0,1,0,0,0,1}));
  EXPECT_EQ(field.translation,(std::array<double,3>{0,0,0}));
  std::vector<vdb::IndexRegion> cells;
  for(int x=-6;x<=4;++x)cells.push_back({{x,0,0},{x,0,0}});
  vdb::ActiveRegionOptions occupancyOptions;occupancyOptions.maxWork=2'000'000'000ull;
  occupancyOptions.maxRecordsPerQuery=0;occupancyOptions.maxTotalRecords=0;
  const auto mask=vdb::QueryActiveRegions(output,cells,occupancyOptions,"descriptor:occupied");
  EXPECT_EQ(mask.nativeType,vdb::RegionValueType::Mask);EXPECT_EQ(mask.valueType,"bool");ASSERT_EQ(mask.queries.size(),11u);
  for(size_t i=0;i<11;++i) {
    const double x=double(i)-6;
    const double distance=std::min({std::abs(x)-4,std::abs(x+2)-3,std::abs(x+3.5)-2.25});
    EXPECT_FLOAT_EQ(values[i],float(std::clamp(distance,-3.,3.)))<<x;
    EXPECT_EQ(mask.queries[i].matchedVoxelCount,distance<0?1u:0u)<<x;
  }
  EXPECT_FLOAT_EQ(values[0],.25f); // No invented endpoint sphere/capsule.
  EXPECT_EQ(mask.queries.front().matchedVoxelCount,0u);
  EXPECT_EQ(mask.queries.back().matchedVoxelCount,0u); // Strict boundary exclusion.
  EXPECT_EQ(submitted.particleGeometry.timeStep,1);
  EXPECT_EQ(submitted.processing.maxInputBytes,vdb::ProcessingOptions{}.maxInputBytes);
  request=submitted;request.output=files.path("sdf-only.vdb");
  request.particleGeometry.output=vdb::ParticleGeometryOutput::LevelSet;
  request.particleGeometry.maskGridName=std::string(1024*1024,'x'); // Hidden and ignored.
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;ASSERT_TRUE(panel.result()->particleGeometryArtifact);
  EXPECT_TRUE(panel.result()->particleGeometryArtifact->sdfGrid);EXPECT_FALSE(panel.result()->particleGeometryArtifact->maskGrid);
  EXPECT_TRUE(panel.result()->modelOutput);const auto sdfOnly=vdb::InspectFile(files.directory/"sdf-only.vdb");
  ASSERT_EQ(sdfOnly.grids.size(),1u);EXPECT_EQ(sdfOnly.grids[0].name,"taper");
  const auto endpoint=vdb::ReadDenseVdb(files.directory/"sdf-only.vdb",{{-6,0,0},{1,1,1}},"descriptor:taper");
  EXPECT_FLOAT_EQ(std::get<std::vector<float>>(endpoint.values).front(),.25f);
  EXPECT_EQ(surfaceUiBytes(source),original);
}

TEST(OpenVdbAuthoring, ParticleGeometryMaskOnlyIgnoresHiddenSdfSettingsAndNeverClaimsModelOrRestart) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;const auto source=files.directory/"mask.json",output=files.directory/"mask.vdb";
  std::ofstream(source)<<R"({"version":1,"particles":[{"position":[0,0,0],"radius":2}]})";
  OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;request.operation=Operation::ParticleGeometry;
  request.input=files.path("mask.json");request.output=files.path("mask.vdb");
  request.particleGeometry.output=vdb::ParticleGeometryOutput::Mask;request.particleGeometry.originWorld=std::array<double,3>{};
  request.particleGeometry.maskGridName="inside";request.processing.voxelSize=1;
  request.processing.gridName=std::string(1024*1024,'x');request.processing.halfWidth=std::numeric_limits<double>::quiet_NaN();
  request.processing.maxTriangles=0;request.inputGrid=std::string(1024*1024,'x');
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  if(!vdb::IsAvailable()){EXPECT_FALSE(panel.result()->success);return;}
  ASSERT_TRUE(panel.result()->success)<<panel.result()->message;EXPECT_FALSE(panel.result()->modelOutput);EXPECT_FALSE(panel.result()->restartGrids);
  ASSERT_TRUE(panel.result()->particleGeometryArtifact);EXPECT_FALSE(panel.result()->particleGeometryArtifact->sdfGrid);
  EXPECT_EQ(panel.result()->particleGeometryArtifact->maskGrid,std::optional<std::string>("descriptor:inside"));
  const auto inspected=vdb::InspectFile(output);ASSERT_EQ(inspected.grids.size(),1u);
  EXPECT_EQ(inspected.grids[0].name,"inside");EXPECT_EQ(inspected.grids[0].valueType,"bool"); // Public inspection normalizes Bool/Mask.
  vdb::ActiveRegionOptions occupancyOptions;occupancyOptions.maxWork=2'000'000'000ull;
  occupancyOptions.maxRecordsPerQuery=0;occupancyOptions.maxTotalRecords=0;
  const std::array<vdb::IndexRegion,2> occupancyCells{{{{0,0,0},{0,0,0}},{{2,0,0},{2,0,0}}}};
  const auto nativeMask=vdb::QueryActiveRegions(output,occupancyCells,occupancyOptions,"descriptor:inside");
  EXPECT_EQ(nativeMask.nativeType,vdb::RegionValueType::Mask);EXPECT_EQ(nativeMask.valueType,"bool");
  ASSERT_EQ(nativeMask.queries.size(),2u);EXPECT_EQ(nativeMask.queries[0].matchedVoxelCount,1u);
  EXPECT_EQ(nativeMask.queries[1].matchedVoxelCount,0u); // Strict sphere boundary is outside.

  const auto original=surfaceUiBytes(source);EXPECT_FALSE(original.empty());
  auto empty=request;empty.input=files.path("empty.json");empty.output=files.path("empty.vdb");
  std::ofstream(files.directory/"empty.json")<<R"({"version":1,"particles":[]})";
  ASSERT_TRUE(panel.submit(empty));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_FALSE(panel.result()->modelOutput);EXPECT_EQ(surfaceUiBytes(source),original);
}

TEST(OpenVdbAuthoring, ParticleGeometryFailuresReserveReportAndProtectSourceAndExistingOutput) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;
  request.operation=Operation::ParticleGeometry;request.input=files.path("particles.json");request.output=files.path("generated.vdb");
  request.processing.voxelSize=1;request.processing.gridName="surface";request.particleGeometry.originWorld=std::array<double,3>{};
  std::ofstream(files.directory/"particles.json")<<R"({"version":1,"particles":[{"position":[0,0,0],"radius":2}]})";
  const auto original=surfaceUiBytes(files.directory/"particles.json");const auto submitted=request;
  if(!vdb::IsAvailable()) {
    request.input="missing.json";request.output="missing-parent/out.vdb";request.processing.maxInputBytes=0;request.particleGeometry.maxWork=0;
    ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);
    EXPECT_NE(panel.result()->message.find("ENABLE_OPENVDB=ON"),std::string::npos);EXPECT_FALSE(panel.result()->particleGeometryArtifact);return;
  }
  for(int cap=0;cap<6;++cap) {
    request=submitted;request.output=files.path(("cap-"+std::to_string(cap)+".vdb").c_str());
    if(cap==0)request.processing.maxInputBytes=65536;
    if(cap==1)request.particleGeometry.maxWork=1;
    if(cap==2)request.processing.maxVoxelCount=1;
    if(cap==3)request.particleGeometry.maxOutputBytes=1;
    if(cap==4)request.particleGeometry.maxMetadataBytes=1;
    if(cap==5)request.particleGeometry.maxNodes=1;
    ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success)<<cap;
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(std::u8string(request.output.begin(),request.output.end()))));
    EXPECT_FALSE(panel.result()->particleGeometryArtifact);EXPECT_FALSE(panel.result()->modelOutput);EXPECT_TRUE(panel.result()->output.empty());
  }
  request=submitted;request.output=files.path("foreign.vdb");std::ofstream(files.directory/"foreign.vdb")<<"foreign output";
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);
  EXPECT_EQ(surfaceUiBytes(files.directory/"foreign.vdb"),"foreign output");
  request=submitted;request.input=files.path("bad.json");std::ofstream(files.directory/"bad.json")<<R"({"version":1,"particles":[],"unknown":true})";
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);
  EXPECT_FALSE(std::filesystem::exists(files.directory/"generated.vdb"));
  EXPECT_EQ(surfaceUiBytes(files.directory/"particles.json"),original);
}

TEST(OpenVdbAuthoring, ParticleGeometryActionsPinSubmittedSelectorsAcrossDraftChangesAndPreserveOrdinaryPreview) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  struct Context {
    ImGuiContext* previous=ImGui::GetCurrentContext();ImGuiContext* owned=ImGui::CreateContext();
    Context(){auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize={1200,9000};io.DeltaTime=1.f/60;unsigned char* p;int w,h;io.Fonts->GetTexDataAsRGBA32(&p,&w,&h);}
    ~Context(){ImGui::DestroyContext(owned);ImGui::SetCurrentContext(previous);}
  } context;
  for(const auto mode:{vdb::ParticleGeometryOutput::LevelSet,vdb::ParticleGeometryOutput::Mask,vdb::ParticleGeometryOutput::Both}) {
    OpenVdbAuthoringPanel panel([](const auto& request){OpenVdbAuthoringResult result{true,"custom",request.operation==Operation::ParticleGeometry?"foreign.vdb":request.output,true};result.restartGrids=std::array<std::string,4>{"D","U","M","T"};result.particleGeometryArtifact=container::ui::OpenVdbParticleGeometryArtifact{"foreign.vdb","foreign","foreign"};return result;});
    panel.draft.operation=Operation::ParticleGeometry;panel.draft.input="original.json";panel.draft.output="submitted.vdb";
    panel.draft.processing.gridName="submitted-sdf";panel.draft.particleGeometry.maskGridName="submitted-mask";panel.draft.particleGeometry.output=mode;
    panel.previewInput="independent.vdb";panel.previewGrid="independent-grid";
    ImGuiID reuse{},restart{},legacyLoad{},sdfInspect{},maskInspect{},sdfLoad{};
    const auto frame=[&] {
      ImGui::NewFrame();ImGui::SetNextWindowSize({1150,8900});ImGui::Begin("Particle generated artifact actions");ImGui::PushID(&panel);
      reuse=ImGui::GetID("Use output as input");restart=ImGui::GetID("Use state as restart");legacyLoad=ImGui::GetID("Load preview");
      sdfInspect=ImGui::GetID("Inspect generated SDF");maskInspect=ImGui::GetID("Inspect generated mask");sdfLoad=ImGui::GetID("Load generated SDF preview");
      ImGui::PopID();ImGui::SetNextItemOpen(true,ImGuiCond_Always);auto selected=panel.draw();ImGui::End();ImGui::Render();return selected;
    };
    ASSERT_TRUE(panel.submit(panel.draft));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
    panel.draft.operation=Operation::Sphere;panel.draft.output="draft-changed.vdb";panel.draft.processing.gridName="draft-changed-grid";
    EXPECT_FALSE(frame());EXPECT_FALSE(frame());
    for(const auto id:{reuse,restart,legacyLoad}) {
      ImGui::ActivateItemByID(id);EXPECT_FALSE(frame());EXPECT_EQ(panel.draft.input,"original.json");
      EXPECT_EQ(panel.previewInput,"independent.vdb");EXPECT_EQ(panel.previewGrid,"independent-grid");EXPECT_FALSE(panel.previewOptions());
    }
    if(mode==vdb::ParticleGeometryOutput::Mask) {
      ImGui::ActivateItemByID(sdfInspect);EXPECT_FALSE(frame());EXPECT_EQ(panel.draft.input,"original.json");
      ImGui::ActivateItemByID(sdfLoad);EXPECT_FALSE(frame());EXPECT_FALSE(panel.previewOptions());
    } else {
      ImGui::ActivateItemByID(sdfLoad);EXPECT_EQ(frame(),std::optional<std::string>("submitted.vdb"));
      ASSERT_TRUE(panel.previewOptions());EXPECT_EQ(panel.previewOptions()->gridName,"descriptor:submitted-sdf");EXPECT_FALSE(panel.previewOptions()->includePoints);
      ImGui::ActivateItemByID(sdfInspect);EXPECT_FALSE(frame());EXPECT_EQ(panel.draft.operation,Operation::Inspect);
      EXPECT_EQ(panel.draft.input,"submitted.vdb");EXPECT_EQ(panel.draft.inputGrid,"descriptor:submitted-sdf");
      EXPECT_EQ(panel.previewInput,"submitted.vdb");EXPECT_EQ(panel.previewGrid,"descriptor:submitted-sdf");EXPECT_EQ(panel.previewKind,3);
    }
    if(mode!=vdb::ParticleGeometryOutput::LevelSet) {
      panel.draft.operation=Operation::RenderSurfaceReference;panel.draft.input="later-draft.vdb";
      EXPECT_FALSE(frame());EXPECT_FALSE(frame());ImGui::ActivateItemByID(maskInspect);EXPECT_FALSE(frame());
      EXPECT_EQ(panel.draft.operation,Operation::Inspect);EXPECT_EQ(panel.draft.input,"submitted.vdb");
      EXPECT_EQ(panel.draft.inputGrid,"descriptor:submitted-mask");EXPECT_EQ(panel.previewGrid,"descriptor:submitted-mask");EXPECT_EQ(panel.previewKind,3);
    }
    panel.draft.operation=Operation::Sphere;panel.draft.output="ordinary.vdb";
    ASSERT_TRUE(panel.submit(panel.draft));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
    EXPECT_FALSE(panel.result()->particleGeometryArtifact);EXPECT_FALSE(frame());EXPECT_FALSE(frame());
    ImGui::ActivateItemByID(legacyLoad);EXPECT_EQ(frame(),std::optional<std::string>("ordinary.vdb"));
    // Ordinary custom executor behavior remains unchanged; no particle binding survives.
  }
}

TEST(OpenVdbAuthoring, ParticleGeometryFailedOrMalformedCustomResultsHaveNoActionableArtifact) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  for(int malformed=0;malformed<4;++malformed) {
    OpenVdbAuthoringPanel panel([&](const auto&){OpenVdbAuthoringResult result{malformed!=0,"custom","foreign.vdb",true};result.particleGeometryArtifact=container::ui::OpenVdbParticleGeometryArtifact{"foreign.vdb","foreign","foreign"};return result;});
    OpenVdbAuthoringRequest request;request.operation=Operation::ParticleGeometry;request.input="source.json";request.output="submitted.vdb";
    if(malformed==1)request.particleGeometry.output=static_cast<vdb::ParticleGeometryOutput>(100);
    if(malformed==2)request.processing.gridName=std::string("bad\0name",8);
    if(malformed==3){request.particleGeometry.output=vdb::ParticleGeometryOutput::Both;request.processing.gridName=request.particleGeometry.maskGridName;}
    ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->particleGeometryArtifact);
    EXPECT_FALSE(panel.result()->modelOutput);EXPECT_FALSE(panel.result()->restartGrids);EXPECT_TRUE(panel.result()->output.empty());
  }
}


container::geometry::vdb::BoundaryFluidInputs boundaryFluidUiChannel(const AuthoringFiles& files) {
  namespace vdb=container::geometry::vdb;
  vdb::DenseArrayVolume f;f.region={{0,0,0},{4,1,1}};f.gridName="F";f.background=0.;
  f.values=std::vector<double>{1,1,1,1};f.active={1,1,1,1};
  (void)vdb::WriteDenseVdb(files.directory/"flow-fluid.vdb",f);
  vdb::DenseArrayVolume outside;outside.region={{-1,0,0},{6,1,1}};outside.gridName="O";outside.background=0.;
  outside.values=std::vector<double>{1,0,0,0,0,1};outside.active={1,0,0,0,0,1};
  (void)vdb::WriteDenseVdb(files.directory/"flow-open.vdb",outside);
  vdb::DenseArrayVolume u;u.region={{0,0,0},{5,1,1}};u.gridName="U";u.background=std::array<double,3>{};
  u.values=std::vector<std::array<double,3>>(5,{.5,0,0});u.active={1,1,1,1,1};
  u.gridClass=vdb::DenseGridClass::Staggered;u.vectorType=vdb::DenseVectorType::ContravariantRelative;u.worldSpace=false;
  (void)vdb::WriteDenseVdb(files.directory/"flow-velocity.vdb",u);
  vdb::BoundaryProjectionInputs raw;raw.fluid={files.directory/"flow-fluid.vdb","F"};
  raw.velocity={files.directory/"flow-velocity.vdb","U"};raw.open={files.directory/"flow-open.vdb","O"};
  vdb::BoundaryProjectionOptions projection;projection.projection.timeStep=.125;projection.projection.tolerance=1e-10;
  (void)vdb::WriteBoundaryProjectedVelocity(raw,files.directory/"flow-state.vdb",projection);
  vdb::DenseArrayVolume d;d.region={{0,0,0},{4,1,1}};d.gridName="D";d.background=0.;
  d.gridClass=vdb::DenseGridClass::FogVolume;d.values=std::vector<double>{1,0,0,0};d.active={1,1,1,1};
  (void)vdb::WriteDenseVdb(files.directory/"flow-density.vdb",d);
  vdb::BoundaryFluidInputs input;input.boundary.state=files.directory/"flow-state.vdb";
  input.density={files.directory/"flow-density.vdb","D"};return input;
}

struct BoundaryFluidUiContext {
  ImGuiContext* previous=ImGui::GetCurrentContext();ImGuiContext* owned=ImGui::CreateContext();
  BoundaryFluidUiContext(){auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize={1200,10000};io.DeltaTime=1.f/60;unsigned char* p;int w,h;io.Fonts->GetTexDataAsRGBA32(&p,&w,&h);}
  ~BoundaryFluidUiContext(){ImGui::DestroyContext(owned);ImGui::SetCurrentContext(previous);}
};

TEST(OpenVdbAuthoring, BoundaryFluidSnapshotsEveryInputControlNameAndResetsCustomArtifacts) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  std::promise<OpenVdbAuthoringRequest> capture;auto future=capture.get_future();
  OpenVdbAuthoringPanel panel([&](const auto& request){capture.set_value(request);OpenVdbAuthoringResult result{true,"custom","foreign.vdb",true};result.restartGrids=std::array<std::string,4>{"D","U","F","T"};result.boundaryFluidArtifact=container::ui::OpenVdbBoundaryFluidArtifact{"foreign.vdb",{}};return result;});
  OpenVdbAuthoringRequest request;request.operation=Operation::BoundaryFluidStep;request.output="submitted.vdb";
  auto& inputs=request.boundaryFluidInputs;inputs.boundary.state="submitted-state.vdb";
  inputs.boundary.velocityGrid="inU";inputs.boundary.fluidGrid="inF";inputs.boundary.faceKindGrid="inK";
  inputs.boundary.faceFractionGrid="inTheta";inputs.boundary.facePressureGrid="inPD";inputs.boundary.wallVelocityGrid="inW";
  inputs.density={"submitted-density.vdb","inD"};inputs.temperature=vdb::BoundaryProjectionField{"submitted-temperature.vdb","inT"};
  auto& o=request.boundaryFluid;o.initialVelocity=vdb::BoundaryFluidInitialVelocity::RequireSolenoidal;o.timeStep=.75;o.substeps=3;o.maxSubsteps=17;o.maxStepAttempts=27;o.maxCfl=.25;
  o.gravity={1,-2,3};o.buoyancyDensity=.125;o.buoyancyTemperature=.25;o.buoyancyDirection={1,0,0};
  o.ambientTemperature=280;o.reservoirDensity=2;o.reservoirTemperature=281;o.tolerance=1e-8;o.maxIterations=177;
  o.maxFluidCells=19;o.maxStoredLeafVoxels=UINT64_MAX-9;o.maxNodes=21;o.maxWork=123456789;
  o.maxGrids=23;o.maxMetadataBytes=1025;o.maxInputBytes=123456788;o.maxOutputBytes=123456787;
  o.names={"descriptor:D","descriptor:T","descriptor:U","descriptor:P","descriptor:F","descriptor:K","descriptor:theta","descriptor:PD","descriptor:W"};
  const auto submitted=request;ASSERT_TRUE(panel.submit(request));request={};panel.draft={};
  ASSERT_EQ(future.wait_for(5s),std::future_status::ready);const auto saved=future.get();
  EXPECT_EQ(saved.operation,Operation::BoundaryFluidStep);EXPECT_EQ(saved.output,submitted.output);
  const auto& a=saved.boundaryFluidInputs;const auto& b=submitted.boundaryFluidInputs;
  EXPECT_EQ(a.boundary.state,b.boundary.state);EXPECT_EQ(a.boundary.velocityGrid,b.boundary.velocityGrid);EXPECT_EQ(a.boundary.fluidGrid,b.boundary.fluidGrid);
  EXPECT_EQ(a.boundary.faceKindGrid,b.boundary.faceKindGrid);EXPECT_EQ(a.boundary.faceFractionGrid,b.boundary.faceFractionGrid);
  EXPECT_EQ(a.boundary.facePressureGrid,b.boundary.facePressureGrid);EXPECT_EQ(a.boundary.wallVelocityGrid,b.boundary.wallVelocityGrid);
  EXPECT_EQ(a.density.file,b.density.file);EXPECT_EQ(a.density.grid,b.density.grid);ASSERT_TRUE(a.temperature);ASSERT_TRUE(b.temperature);
  EXPECT_EQ(a.temperature->file,b.temperature->file);EXPECT_EQ(a.temperature->grid,b.temperature->grid);
  const auto& x=saved.boundaryFluid;const auto& y=submitted.boundaryFluid;
#define E(field) EXPECT_EQ(x.field,y.field)
  E(initialVelocity);E(timeStep);E(substeps);E(maxSubsteps);E(maxStepAttempts);E(maxCfl);E(gravity);E(buoyancyDensity);E(buoyancyTemperature);
  E(buoyancyDirection);E(ambientTemperature);E(reservoirDensity);E(reservoirTemperature);E(tolerance);E(maxIterations);
  E(maxFluidCells);E(maxStoredLeafVoxels);E(maxNodes);E(maxWork);E(maxGrids);E(maxMetadataBytes);E(maxInputBytes);E(maxOutputBytes);
  E(names.density);E(names.temperature);E(names.velocity);E(names.pressure);E(names.fluid);E(names.faceKind);
  E(names.faceFraction);E(names.facePressure);E(names.wallVelocity);
#undef E
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_TRUE(panel.result()->success);EXPECT_FALSE(panel.result()->modelOutput);
  EXPECT_TRUE(panel.result()->output.empty());EXPECT_FALSE(panel.result()->restartGrids);EXPECT_FALSE(panel.result()->particleGeometryArtifact);
  ASSERT_TRUE(panel.result()->boundaryFluidArtifact);const auto& artifact=*panel.result()->boundaryFluidArtifact;
  EXPECT_EQ(artifact.output,"submitted.vdb");EXPECT_EQ(artifact.initialVelocity,vdb::BoundaryFluidInitialVelocity::RequireSolenoidal);
  EXPECT_EQ(artifact.grids,(std::array<std::string,8>{"descriptor:descriptor:U","descriptor:descriptor:F","descriptor:descriptor:K","descriptor:descriptor:theta","descriptor:descriptor:PD","descriptor:descriptor:W","descriptor:descriptor:D","descriptor:descriptor:T"}));
}

TEST(OpenVdbAuthoring, BoundaryFluidRealExecutorAndReportedRestartMatchIndependentChannelPulse) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  if(!vdb::IsAvailable())GTEST_SKIP();AuthoringFiles files;BoundaryFluidUiContext context;OpenVdbAuthoringPanel panel;
  const auto firstPath=files.directory/std::filesystem::path(u8"状態-é.vdb");
  OpenVdbAuthoringRequest request;request.operation=Operation::BoundaryFluidStep;request.output=surfaceUiUtf8(firstPath);
  request.boundaryFluidInputs=boundaryFluidUiChannel(files);request.boundaryFluid.timeStep=.5;
  const auto unicodeState=files.directory/std::filesystem::path(u8"入力-状態.vdb"),unicodeDensity=files.directory/std::filesystem::path(u8"入力-密度.vdb");
  std::filesystem::copy_file(request.boundaryFluidInputs.boundary.state,unicodeState);
  std::filesystem::copy_file(request.boundaryFluidInputs.density.file,unicodeDensity);
  request.boundaryFluidInputs.boundary.state=unicodeState;request.boundaryFluidInputs.density.file=unicodeDensity;
  request.boundaryFluid.names={"descriptor:D","descriptor:T","descriptor:U","descriptor:P","descriptor:F","descriptor:K","descriptor:theta","descriptor:PD","descriptor:W"};
  request.input="ignored-missing.json";request.processing.maxInputBytes=0;request.processing.gridName=std::string(1024*1024,'x');
  std::map<std::filesystem::path,std::string> sources;
  for(const auto& p:{request.boundaryFluidInputs.boundary.state,request.boundaryFluidInputs.density.file}){std::ifstream in(p,std::ios::binary);sources[p]={std::istreambuf_iterator<char>(in),{}};}
  ASSERT_TRUE(panel.submit(request));request.boundaryFluid.timeStep=2;request.boundaryFluidInputs.density.grid="changed";
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  ASSERT_TRUE(panel.result()->boundaryFluidArtifact);EXPECT_FALSE(panel.result()->restartGrids);EXPECT_FALSE(panel.result()->modelOutput);EXPECT_TRUE(panel.result()->output.empty());
  EXPECT_NE(panel.result()->message.find("Completed boundary fluid step"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("Accepted actual dt sequence"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("densityMassAfter=1"),std::string::npos);
  const auto first=vdb::ReadDenseVdb(firstPath,{{0,0,0},{4,1,1}},"descriptor:descriptor:D");
  EXPECT_EQ(std::get<std::vector<double>>(first.values),(std::vector<double>{.75,.25,0,0}));
  const auto catalog=vdb::InspectFile(firstPath);ASSERT_EQ(catalog.grids.size(),9u);
  for(const auto& grid:catalog.grids)EXPECT_EQ(grid.descriptorName,grid.name);
  panel.draft={};panel.draft.operation=Operation::Sphere;panel.draft.output="stale-draft.vdb";
  ImGuiID restart{};const auto frame=[&]{ImGui::NewFrame();ImGui::SetNextWindowSize({1150,9900});ImGui::Begin("Boundary fluid native restart");ImGui::PushID(&panel);restart=ImGui::GetID("Use boundary fluid state as restart");ImGui::PopID();ImGui::SetNextItemOpen(true,ImGuiCond_Always);auto model=panel.draw();ImGui::End();ImGui::Render();return model;};
  EXPECT_FALSE(frame());EXPECT_FALSE(frame());ImGui::ActivateItemByID(restart);EXPECT_FALSE(frame());
  ASSERT_EQ(panel.draft.operation,Operation::BoundaryFluidStep);EXPECT_TRUE(panel.draft.output.empty());
  EXPECT_EQ(panel.draft.boundaryFluidInputs.boundary.state,firstPath);
  EXPECT_EQ(panel.draft.boundaryFluidInputs.density.grid,"descriptor:descriptor:D");ASSERT_TRUE(panel.draft.boundaryFluidInputs.temperature);
  EXPECT_EQ(panel.draft.boundaryFluidInputs.temperature->grid,"descriptor:descriptor:T");
  panel.draft.boundaryFluid.timeStep=.5;panel.draft.output=files.path("second.vdb");
  ASSERT_TRUE(panel.submit(panel.draft));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  const auto second=vdb::ReadDenseVdb(files.directory/"second.vdb",{{0,0,0},{4,1,1}},"density");
  EXPECT_EQ(std::get<std::vector<double>>(second.values),(std::vector<double>{.5625,.375,.0625,0}));
  for(const auto& [p,original]:sources){std::ifstream in(p,std::ios::binary);EXPECT_EQ((std::string(std::istreambuf_iterator<char>(in),{})),original);}
}

TEST(OpenVdbAuthoring, BoundaryFluidRealExecutorHasIndependentClosedRingScalarAndMomentumOracle) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  if(!vdb::IsAvailable())GTEST_SKIP();AuthoringFiles files;
  vdb::DenseArrayVolume f;f.region={{0,0,0},{2,2,1}};f.gridName="F";f.background=0.;f.values=std::vector<double>{1,1,1,1};f.active={1,1,1,1};
  (void)vdb::WriteDenseVdb(files.directory/"ring-fluid.vdb",f);
  vdb::DenseArrayVolume u;u.region={{0,0,0},{3,3,2}};u.gridName="U";u.background=std::array<double,3>{};
  std::vector<std::array<double,3>> velocity(18);velocity[1]={.25,0,0};velocity[4]={-.25,.25,0};velocity[3]={0,-.25,0};
  u.values=velocity;u.active.assign(18,1);u.gridClass=vdb::DenseGridClass::Staggered;u.vectorType=vdb::DenseVectorType::ContravariantRelative;u.worldSpace=false;
  (void)vdb::WriteDenseVdb(files.directory/"ring-u.vdb",u);
  vdb::BoundaryProjectionInputs raw;raw.fluid={files.directory/"ring-fluid.vdb","F"};raw.velocity={files.directory/"ring-u.vdb","U"};
  (void)vdb::WriteBoundaryProjectedVelocity(raw,files.directory/"ring-state.vdb");
  f.gridName="D";f.gridClass=vdb::DenseGridClass::FogVolume;f.values=std::vector<double>{1,0,0,0};
  (void)vdb::WriteDenseVdb(files.directory/"ring-density.vdb",f);
  OpenVdbAuthoringRequest request;request.operation=Operation::BoundaryFluidStep;request.output=files.path("ring.vdb");
  request.boundaryFluidInputs.boundary.state=files.directory/"ring-state.vdb";request.boundaryFluidInputs.density={files.directory/"ring-density.vdb","D"};request.boundaryFluid.timeStep=.5;
  OpenVdbAuthoringPanel panel;ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  const auto d=vdb::ReadDenseVdb(files.directory/"ring.vdb",{{0,0,0},{2,2,1}},"density");
  EXPECT_EQ(std::get<std::vector<double>>(d.values),(std::vector<double>{.875,.125,0,0}));
  const auto stored=vdb::ReadDenseVdb(files.directory/"ring.vdb",{{0,0,0},{3,3,2}},"velocity");
  const auto values=std::get<std::vector<std::array<double,3>>>(stored.values);
  EXPECT_EQ(values[1][0],.21875);EXPECT_EQ(values[4][0],-.21875);EXPECT_EQ(values[4][1],.21875);EXPECT_EQ(values[3][1],-.21875);
  EXPECT_NE(panel.result()->message.find("densityMassAfter=1"),std::string::npos);EXPECT_NE(panel.result()->message.find("densityOutwardTransfer=0"),std::string::npos);
}

TEST(OpenVdbAuthoring, BoundaryFluidFailurePathsReserveJointReportAndProtectEverySourceAndOutput) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringRequest request;request.operation=Operation::BoundaryFluidStep;request.output=files.path("bounded.vdb");
  OpenVdbAuthoringPanel panel;
  if(!vdb::IsAvailable()) {
    request.boundaryFluidInputs.boundary.state="missing-parent/missing.vdb";request.boundaryFluid.maxInputBytes=0;request.boundaryFluid.maxWork=0;
    ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);
    EXPECT_NE(panel.result()->message.find("ENABLE_OPENVDB=ON"),std::string::npos);EXPECT_FALSE(panel.result()->boundaryFluidArtifact);return;
  }
  request.boundaryFluidInputs=boundaryFluidUiChannel(files);
  std::ifstream in(request.boundaryFluidInputs.boundary.state,std::ios::binary);const std::string original(std::istreambuf_iterator<char>(in),{});in.close();
  const auto ordinary=request;
  for(int cap=0;cap<8;++cap) {
    request=ordinary;
    if(cap==0)request.boundaryFluid.maxInputBytes=1024;
    if(cap==1)request.boundaryFluid.maxWork=1;
    if(cap==2)request.boundaryFluid.maxFluidCells=1;
    if(cap==3)request.boundaryFluid.maxOutputBytes=1;
    if(cap==4)request.boundaryFluid.maxMetadataBytes=1;
    if(cap==5)request.boundaryFluid.maxNodes=1;
    if(cap==6)request.boundaryFluid.maxGrids=1;
    if(cap==7)request.boundaryFluid.maxStoredLeafVoxels=1;
    ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success)<<cap;
    EXPECT_FALSE(panel.result()->boundaryFluidArtifact);EXPECT_TRUE(panel.result()->output.empty());EXPECT_FALSE(panel.result()->modelOutput);EXPECT_FALSE(std::filesystem::exists(files.directory/"bounded.vdb"));
  }
  std::ofstream(files.directory/"bounded.vdb",std::ios::binary)<<"foreign destination";
  request=ordinary;ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));EXPECT_FALSE(panel.result()->success);
  std::ifstream foreign(files.directory/"bounded.vdb",std::ios::binary);EXPECT_EQ((std::string(std::istreambuf_iterator<char>(foreign),{})),"foreign destination");
  request.output=files.path("flow-state.vdb");ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));EXPECT_FALSE(panel.result()->success);
  std::ifstream source(ordinary.boundaryFluidInputs.boundary.state,std::ios::binary);EXPECT_EQ((std::string(std::istreambuf_iterator<char>(source),{})),original);
}

TEST(OpenVdbAuthoring, BoundaryFluidSubmittedActionsSurviveDraftSwitchesAndPreserveOrdinaryPreview) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;BoundaryFluidUiContext context;
  OpenVdbAuthoringPanel panel([](const auto& request){OpenVdbAuthoringResult result{true,"custom",request.operation==Operation::BoundaryFluidStep?"foreign.vdb":request.output,true};result.restartGrids=std::array<std::string,4>{"staleD","staleU","staleF","staleT"};result.boundaryFluidArtifact=container::ui::OpenVdbBoundaryFluidArtifact{"foreign.vdb",{}};return result;});
  panel.draft.operation=Operation::BoundaryFluidStep;panel.draft.output="submitted.vdb";
  panel.draft.boundaryFluid.names={"D","T","U","P","F","K","theta","PD","W"};
  panel.draft.boundaryFluid.initialVelocity=vdb::BoundaryFluidInitialVelocity::RequireSolenoidal;
  panel.previewInput="independent.vdb";panel.previewGrid="independent-grid";
  ASSERT_TRUE(panel.submit(panel.draft));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->boundaryFluidArtifact);
  const auto artifact=*panel.result()->boundaryFluidArtifact;panel.draft={};panel.draft.operation=Operation::Sphere;panel.draft.input="unrelated-source.vdb";
  ImGuiID genericReuse{},genericRestart{},genericLoad{},restart{},inspectD{},inspectU{};
  const auto frame=[&]{ImGui::NewFrame();ImGui::SetNextWindowSize({1150,9900});ImGui::Begin("Boundary fluid artifact actions");ImGui::PushID(&panel);
    genericReuse=ImGui::GetID("Use output as input");genericRestart=ImGui::GetID("Use state as restart");genericLoad=ImGui::GetID("Load preview");
    restart=ImGui::GetID("Use boundary fluid state as restart");inspectD=ImGui::GetID("Inspect boundary fluid density");inspectU=ImGui::GetID("Inspect boundary fluid velocity");
    ImGui::PopID();ImGui::SetNextItemOpen(true,ImGuiCond_Always);auto model=panel.draw();ImGui::End();ImGui::Render();return model;};
  EXPECT_FALSE(frame());EXPECT_FALSE(frame());
  for(const auto id:{genericReuse,genericRestart,genericLoad}){ImGui::ActivateItemByID(id);EXPECT_FALSE(frame());EXPECT_EQ(panel.draft.input,"unrelated-source.vdb");EXPECT_EQ(panel.previewInput,"independent.vdb");EXPECT_EQ(panel.previewGrid,"independent-grid");EXPECT_FALSE(panel.previewOptions());}
  ImGui::ActivateItemByID(restart);EXPECT_FALSE(frame());EXPECT_EQ(panel.draft.operation,Operation::BoundaryFluidStep);
  EXPECT_EQ(panel.draft.boundaryFluid.initialVelocity,vdb::BoundaryFluidInitialVelocity::RequireSolenoidal);
  const auto& inputs=panel.draft.boundaryFluidInputs;EXPECT_EQ(inputs.boundary.state,std::filesystem::path("submitted.vdb"));
  EXPECT_EQ(inputs.boundary.velocityGrid,artifact.grids[0]);EXPECT_EQ(inputs.boundary.fluidGrid,artifact.grids[1]);
  EXPECT_EQ(inputs.boundary.faceKindGrid,artifact.grids[2]);EXPECT_EQ(inputs.boundary.faceFractionGrid,artifact.grids[3]);
  EXPECT_EQ(inputs.boundary.facePressureGrid,artifact.grids[4]);EXPECT_EQ(inputs.boundary.wallVelocityGrid,artifact.grids[5]);
  EXPECT_EQ(inputs.density.file,std::filesystem::path("submitted.vdb"));EXPECT_EQ(inputs.density.grid,artifact.grids[6]);ASSERT_TRUE(inputs.temperature);
  EXPECT_EQ(inputs.temperature->file,std::filesystem::path("submitted.vdb"));EXPECT_EQ(inputs.temperature->grid,artifact.grids[7]);EXPECT_TRUE(panel.draft.output.empty());
  for(const auto [id,slot]:{std::pair{inspectD,size_t(6)},std::pair{inspectU,size_t(0)}}) {
    panel.draft.operation=Operation::RenderSurfaceReference;panel.draft.input="changed.vdb";EXPECT_FALSE(frame());EXPECT_FALSE(frame());ImGui::ActivateItemByID(id);EXPECT_FALSE(frame());
    EXPECT_EQ(panel.draft.operation,Operation::Inspect);EXPECT_EQ(panel.draft.input,"submitted.vdb");EXPECT_EQ(panel.draft.inputGrid,artifact.grids[slot]);
    EXPECT_EQ(panel.previewInput,"submitted.vdb");EXPECT_EQ(panel.previewGrid,artifact.grids[slot]);EXPECT_EQ(panel.previewKind,3);EXPECT_FALSE(panel.previewOptions());
  }
  panel.draft.operation=Operation::Sphere;panel.draft.output="ordinary.vdb";ASSERT_TRUE(panel.submit(panel.draft));ASSERT_TRUE(settle(panel));EXPECT_FALSE(panel.result()->boundaryFluidArtifact);
  EXPECT_FALSE(frame());EXPECT_FALSE(frame());ImGui::ActivateItemByID(genericLoad);EXPECT_EQ(frame(),std::optional<std::string>("ordinary.vdb"));
}

TEST(OpenVdbAuthoring, BoundaryFluidMalformedCustomSuccessCannotAdvertiseActionableOutput) {
  using Operation=container::ui::OpenVdbAuthoringOperation;
  for(int malformed=0;malformed<6;++malformed) {
    OpenVdbAuthoringPanel panel([&](const auto&){OpenVdbAuthoringResult result{malformed!=0,"custom","foreign.vdb",true};result.boundaryFluidArtifact=container::ui::OpenVdbBoundaryFluidArtifact{"foreign.vdb",{}};return result;});
    OpenVdbAuthoringRequest request;request.operation=Operation::BoundaryFluidStep;request.output="submitted.vdb";
    if(malformed==1)request.boundaryFluid.names.density=request.boundaryFluid.names.temperature;
    if(malformed==2)request.boundaryFluid.names.velocity=std::string("bad\0name",8);
    if(malformed==3)request.boundaryFluid.names.faceKind=std::string(257,'x');
    if(malformed==4)request.output.clear();
    if(malformed==5)request.boundaryFluid.initialVelocity=static_cast<container::geometry::vdb::BoundaryFluidInitialVelocity>(100);
    ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->boundaryFluidArtifact);
    EXPECT_FALSE(panel.result()->particleGeometryArtifact);EXPECT_FALSE(panel.result()->modelOutput);EXPECT_FALSE(panel.result()->restartGrids);EXPECT_TRUE(panel.result()->output.empty());
  }
}

TEST(OpenVdbAuthoring, ParticleGeometryNativeRecordSeparatorNamesUseExactDescriptorsForReadbackAndActions) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  if(!vdb::IsAvailable())GTEST_SKIP();AuthoringFiles files;BoundaryFluidUiContext context;OpenVdbAuthoringPanel panel;
  const auto source=files.directory/"particle-names.json",output=files.directory/"particle-names.vdb";
  const std::string json=R"({"version":1,"particles":[{"position":[0,0,0],"radius":2}]})";
  std::ofstream(source,std::ios::binary)<<json;
  OpenVdbAuthoringRequest request;request.operation=Operation::ParticleGeometry;request.input=surfaceUiUtf8(source);request.output=surfaceUiUtf8(output);
  request.particleGeometry.output=vdb::ParticleGeometryOutput::Both;request.particleGeometry.originWorld=std::array<double,3>{};
  request.processing.voxelSize=1;request.particleGeometry.minRadiusVoxels=2;
  request.processing.gridName="surface\x1e" "9";request.particleGeometry.maskGridName="occupied\x1e" "17";
  const auto rawSdf=request.processing.gridName,rawMask=request.particleGeometry.maskGridName;
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  ASSERT_TRUE(panel.result()->particleGeometryArtifact);const auto artifact=*panel.result()->particleGeometryArtifact;
  ASSERT_TRUE(artifact.sdfGrid);ASSERT_TRUE(artifact.maskGrid);
  EXPECT_EQ(*artifact.sdfGrid,"descriptor:surface[9]");EXPECT_EQ(*artifact.maskGrid,"descriptor:occupied[17]");
  vdb::ActiveRegionOptions options;options.maxWork=2'000'000'000ull;options.maxRecordsPerQuery=options.maxTotalRecords=0;
  const std::array<vdb::IndexRegion,2> cells{{{{0,0,0},{0,0,0}},{{2,0,0},{2,0,0}}}};
  EXPECT_ANY_THROW((void)vdb::QueryActiveRegions(output,cells,options,"descriptor:"+rawSdf));
  EXPECT_ANY_THROW((void)vdb::QueryActiveRegions(output,cells,options,"descriptor:"+rawMask));
  const auto mask=vdb::QueryActiveRegions(output,cells,options,*artifact.maskGrid);
  EXPECT_EQ(mask.nativeType,vdb::RegionValueType::Mask);EXPECT_EQ(mask.descriptorName,"occupied[17]");EXPECT_EQ(mask.gridName,rawMask);
  ASSERT_EQ(mask.queries.size(),2u);EXPECT_EQ(mask.queries[0].matchedVoxelCount,1u);EXPECT_EQ(mask.queries[1].matchedVoxelCount,0u);
  const auto field=vdb::ReadDenseVdb(output,{{0,0,0},{1,1,1}},*artifact.sdfGrid);
  EXPECT_EQ(field.gridName,rawSdf);EXPECT_FLOAT_EQ(std::get<std::vector<float>>(field.values).front(),-2.f);
  panel.draft.operation=Operation::Sphere;panel.draft.input="later-draft.vdb";panel.previewInput="independent.vdb";panel.previewGrid="independent";
  ImGuiID inspectSdf{},inspectMask{},load{};
  const auto frame=[&]{ImGui::NewFrame();ImGui::SetNextWindowSize({1150,9900});ImGui::Begin("Particle exact native descriptor actions");ImGui::PushID(&panel);
    inspectSdf=ImGui::GetID("Inspect generated SDF");inspectMask=ImGui::GetID("Inspect generated mask");load=ImGui::GetID("Load generated SDF preview");
    ImGui::PopID();ImGui::SetNextItemOpen(true,ImGuiCond_Always);auto model=panel.draw();ImGui::End();ImGui::Render();return model;};
  EXPECT_FALSE(frame());EXPECT_FALSE(frame());ImGui::ActivateItemByID(load);EXPECT_EQ(frame(),std::optional<std::string>(surfaceUiUtf8(output)));
  ASSERT_TRUE(panel.previewOptions());EXPECT_EQ(panel.previewOptions()->gridName,*artifact.sdfGrid);
  ImGui::ActivateItemByID(inspectSdf);EXPECT_FALSE(frame());EXPECT_EQ(panel.draft.inputGrid,*artifact.sdfGrid);EXPECT_EQ(panel.previewGrid,*artifact.sdfGrid);
  panel.draft.operation=Operation::RenderSurfaceReference;panel.draft.inputGrid="changed";EXPECT_FALSE(frame());EXPECT_FALSE(frame());
  ImGui::ActivateItemByID(inspectMask);EXPECT_FALSE(frame());EXPECT_EQ(panel.draft.inputGrid,*artifact.maskGrid);EXPECT_EQ(panel.previewGrid,*artifact.maskGrid);EXPECT_EQ(panel.previewKind,3);
  std::ifstream input(source,std::ios::binary);EXPECT_EQ((std::string(std::istreambuf_iterator<char>(input),{})),json);
}


container::geometry::vdb::BoundaryFluidInputs boundaryFluidUiArrestFixture(const AuthoringFiles& files) {
  namespace vdb=container::geometry::vdb;
  const std::array<double,9> linear{.5,0,0,0,.5,0,0,0,.5};
  vdb::DenseArrayVolume f;f.region={{0,0,0},{3,1,1}};f.gridName="F";f.background=0.;f.linear=linear;
  f.values=std::vector<double>{1,1,1};f.active={1,1,1};(void)vdb::WriteDenseVdb(files.directory/"arrest-fluid.vdb",f);
  vdb::DenseArrayVolume exterior;exterior.region={{-1,0,0},{5,1,1}};exterior.gridName="O";exterior.background=0.;exterior.linear=linear;
  exterior.values=std::vector<double>{1,0,0,0,1};exterior.active={1,0,0,0,1};(void)vdb::WriteDenseVdb(files.directory/"arrest-open.vdb",exterior);
  vdb::DenseArrayVolume u;u.region={{0,0,0},{4,1,1}};u.gridName="U";u.background=std::array<double,3>{};u.linear=linear;
  u.values=std::vector<std::array<double,3>>(4,{-.5,0,0});u.active={1,1,1,1};
  u.gridClass=vdb::DenseGridClass::Staggered;u.vectorType=vdb::DenseVectorType::ContravariantRelative;u.worldSpace=false;
  (void)vdb::WriteDenseVdb(files.directory/"arrest-velocity.vdb",u);
  vdb::DenseArrayVolume pressure;pressure.region={{-1,0,0},{5,1,1}};pressure.gridName="PD";pressure.background=0.;pressure.linear=linear;
  pressure.values=std::vector<double>{5.25,3.75,2.25,.75,-.75};pressure.active={1,1,1,1,1};(void)vdb::WriteDenseVdb(files.directory/"arrest-pd.vdb",pressure);
  vdb::BoundaryProjectionInputs raw;raw.fluid={files.directory/"arrest-fluid.vdb","F"};raw.velocity={files.directory/"arrest-velocity.vdb","U"};
  raw.open={files.directory/"arrest-open.vdb","O"};raw.pressure={files.directory/"arrest-pd.vdb","PD"};
  vdb::BoundaryProjectionOptions projection;projection.projection.timeStep=.125;projection.projection.tolerance=1e-10;
  (void)vdb::WriteBoundaryProjectedVelocity(raw,files.directory/"arrest-state.vdb",projection);
  f.gridName="D";f.gridClass=vdb::DenseGridClass::FogVolume;f.values=std::vector<double>{0,0,0};(void)vdb::WriteDenseVdb(files.directory/"arrest-density.vdb",f);
  vdb::BoundaryFluidInputs result;result.boundary.state=files.directory/"arrest-state.vdb";result.density={files.directory/"arrest-density.vdb","D"};return result;
}

TEST(OpenVdbAuthoring, BoundaryFluidStoredPolicyRestartsActualArrestAndStrictEnforcesEntryAndEndpoint) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  if(!vdb::IsAvailable())GTEST_SKIP();AuthoringFiles files;BoundaryFluidUiContext context;OpenVdbAuthoringPanel panel;
  const auto input=boundaryFluidUiArrestFixture(files);OpenVdbAuthoringRequest request;
  request.operation=Operation::BoundaryFluidStep;request.boundaryFluidInputs=input;request.output=files.path("arrest-first.vdb");
  request.boundaryFluid.timeStep=.125;request.boundaryFluid.gravity={-4,0,0};
  ASSERT_EQ(request.boundaryFluid.initialVelocity,vdb::BoundaryFluidInitialVelocity::TransportStored);
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_NE(panel.result()->message.find("initialDivergenceWithinTolerance=yes"),std::string::npos);
  ASSERT_TRUE(panel.result()->boundaryFluidArtifact);const auto artifact=*panel.result()->boundaryFluidArtifact;
  EXPECT_EQ(artifact.initialVelocity,vdb::BoundaryFluidInitialVelocity::TransportStored);
  const auto first=vdb::ReadDenseVdb(files.directory/"arrest-first.vdb",{{0,0,0},{4,1,1}},"velocity");
  for(const auto& value:std::get<std::vector<std::array<double,3>>>(first.values))EXPECT_NEAR(value[0],0,1e-12);
  panel.draft={};panel.draft.operation=Operation::Sphere;panel.draft.boundaryFluid.initialVelocity=vdb::BoundaryFluidInitialVelocity::RequireSolenoidal;
  ImGuiID restart{};const auto frame=[&]{ImGui::NewFrame();ImGui::SetNextWindowSize({1150,9900});ImGui::Begin("Boundary fluid initial policy restart");ImGui::PushID(&panel);restart=ImGui::GetID("Use boundary fluid state as restart");ImGui::PopID();ImGui::SetNextItemOpen(true,ImGuiCond_Always);auto model=panel.draw();ImGui::End();ImGui::Render();return model;};
  EXPECT_FALSE(frame());EXPECT_FALSE(frame());ImGui::ActivateItemByID(restart);EXPECT_FALSE(frame());
  ASSERT_EQ(panel.draft.operation,Operation::BoundaryFluidStep);EXPECT_EQ(panel.draft.boundaryFluid.initialVelocity,vdb::BoundaryFluidInitialVelocity::TransportStored);
  panel.draft.boundaryFluid.timeStep=.125;panel.draft.boundaryFluid.gravity={-4,0,0};panel.draft.output=files.path("arrest-second.vdb");
  const auto restartRequest=panel.draft;ASSERT_TRUE(panel.submit(panel.draft));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_NE(panel.result()->message.find("initialDivergenceWithinTolerance=no"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("actualSubsteps=1"),std::string::npos);EXPECT_NE(panel.result()->message.find("attempts=1"),std::string::npos);
  const auto second=vdb::ReadDenseVdb(files.directory/"arrest-second.vdb",{{0,0,0},{4,1,1}},"velocity");
  for(const auto& value:std::get<std::vector<std::array<double,3>>>(second.values))EXPECT_NEAR(value[0],-.25,2e-6);
  const auto pressure=vdb::ReadDenseVdb(files.directory/"arrest-second.vdb",{{0,0,0},{3,1,1}},"pressure");
  const auto p=std::get<std::vector<double>>(pressure.values);ASSERT_EQ(p.size(),3u);
  for(size_t i=0;i<3;++i)EXPECT_NEAR(p[i],3.75-1.5*double(i),2e-6);
  auto strictEntry=restartRequest;strictEntry.output=files.path("strict-entry.vdb");strictEntry.boundaryFluid.initialVelocity=vdb::BoundaryFluidInitialVelocity::RequireSolenoidal;
  ASSERT_TRUE(panel.submit(strictEntry));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);EXPECT_FALSE(panel.result()->boundaryFluidArtifact);EXPECT_FALSE(std::filesystem::exists(files.directory/"strict-entry.vdb"));
  auto strictEndpoint=request;strictEndpoint.output=files.path("strict-endpoint.vdb");strictEndpoint.boundaryFluid.initialVelocity=vdb::BoundaryFluidInitialVelocity::RequireSolenoidal;
  ASSERT_TRUE(panel.submit(strictEndpoint));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);EXPECT_FALSE(panel.result()->boundaryFluidArtifact);EXPECT_FALSE(std::filesystem::exists(files.directory/"strict-endpoint.vdb"));
}

container::geometry::vdb::DenseArrayVolume fogUiField(
    std::vector<double> values={8},std::vector<uint8_t> active={0},
    std::array<uint32_t,3> dimensions={1,1,1},double background=0) {
  namespace vdb=container::geometry::vdb;vdb::DenseArrayVolume field;
  field.region={{0,0,0},dimensions};field.gridName="density";field.background=background;
  field.values=std::move(values);field.active=std::move(active);field.gridClass=vdb::DenseGridClass::FogVolume;
  return field;
}
double fogUiNumber(const std::string& message,const std::string& label) {
  const auto at=message.find(label);
  if(at==std::string::npos)throw std::runtime_error("Missing fog report field: "+label+" in "+message);
  double value=0;const auto parsed=std::from_chars(message.data()+at+label.size(),message.data()+message.size(),value);
  if(parsed.ec!=std::errc{})throw std::runtime_error("Unrepresentable fog report field: "+label);
  return value;
}

TEST(OpenVdbAuthoring, FogIntegralSnapshotsEveryRayLimitAndSuppressesForeignArtifacts) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  std::promise<OpenVdbAuthoringRequest> capture;auto future=capture.get_future();
  OpenVdbAuthoringPanel panel([&](const auto& request){capture.set_value(request);
    OpenVdbAuthoringResult result{true,"custom","foreign.vdb",true};
    result.restartGrids=std::array<std::string,4>{"D","U","F","T"};
    result.particleGeometryArtifact=container::ui::OpenVdbParticleGeometryArtifact{"foreign.vdb","descriptor:S","descriptor:M"};
    result.boundaryFluidArtifact=container::ui::OpenVdbBoundaryFluidArtifact{"foreign.vdb",{}};return result;});
  OpenVdbAuthoringRequest request;request.operation=Operation::IntegrateFogRays;
  request.input="submitted-fog.vdb";request.inputGrid="descriptor:density";request.output="ignored.vdb";
  request.rayQueries={{{1,2,3},{4,5,6},.125,7},{{-1,-2,-3},{0,-7,0},0,8}};
  request.fogRays.extinction=.375;request.fogRays.transmissionPolicy=vdb::FogTransmissionPolicy::AllowOpaqueUnderflow;
  request.fogRays.maxCellSegmentsPerRay=19;request.fogRays.maxCellSegments=29;
  request.fogRays.limits.maxRays=31;request.fogRays.limits.maxGrids=17;
  request.fogRays.limits.maxMetadataBytes=9007199254740993ull;request.fogRays.limits.maxWorkItems=123456789;
  request.processing.maxInputBytes=UINT64_MAX-3;request.processing.maxVoxelCount=UINT64_MAX-7;
  const auto submitted=request;ASSERT_TRUE(panel.submit(request));request={};panel.draft={};
  ASSERT_EQ(future.wait_for(5s),std::future_status::ready);const auto saved=future.get();
  EXPECT_EQ(saved.operation,Operation::IntegrateFogRays);EXPECT_EQ(saved.input,submitted.input);EXPECT_EQ(saved.inputGrid,submitted.inputGrid);
  ASSERT_EQ(saved.rayQueries.size(),submitted.rayQueries.size());
  for(size_t i=0;i<saved.rayQueries.size();++i) {
    EXPECT_EQ(saved.rayQueries[i].origin,submitted.rayQueries[i].origin);EXPECT_EQ(saved.rayQueries[i].direction,submitted.rayQueries[i].direction);
    EXPECT_EQ(saved.rayQueries[i].minimumDistance,submitted.rayQueries[i].minimumDistance);EXPECT_EQ(saved.rayQueries[i].maximumDistance,submitted.rayQueries[i].maximumDistance);
  }
  EXPECT_EQ(saved.fogRays.extinction,submitted.fogRays.extinction);EXPECT_EQ(saved.fogRays.transmissionPolicy,submitted.fogRays.transmissionPolicy);
  EXPECT_EQ(saved.fogRays.maxCellSegmentsPerRay,submitted.fogRays.maxCellSegmentsPerRay);EXPECT_EQ(saved.fogRays.maxCellSegments,submitted.fogRays.maxCellSegments);
  EXPECT_EQ(saved.fogRays.limits.maxRays,submitted.fogRays.limits.maxRays);EXPECT_EQ(saved.fogRays.limits.maxGrids,submitted.fogRays.limits.maxGrids);
  EXPECT_EQ(saved.fogRays.limits.maxMetadataBytes,submitted.fogRays.limits.maxMetadataBytes);EXPECT_EQ(saved.fogRays.limits.maxWorkItems,submitted.fogRays.limits.maxWorkItems);
  EXPECT_EQ(saved.processing.maxInputBytes,submitted.processing.maxInputBytes);EXPECT_EQ(saved.processing.maxVoxelCount,submitted.processing.maxVoxelCount);
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success);
  EXPECT_TRUE(panel.result()->output.empty());EXPECT_FALSE(panel.result()->modelOutput);EXPECT_FALSE(panel.result()->restartGrids);
  EXPECT_FALSE(panel.result()->particleGeometryArtifact);EXPECT_FALSE(panel.result()->boundaryFluidArtifact);EXPECT_FALSE(panel.previewOptions());
}

TEST(OpenVdbAuthoring, FogIntegralRealExecutorRetainsInactiveTentUnicodeSourceMetadataAndWorldUnits) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  if(!vdb::IsAvailable())GTEST_SKIP();AuthoringFiles files;
  (void)vdb::WriteDenseVdb(files.directory/"fog-raw.vdb",fogUiField());
  std::ofstream(files.directory/"fog-meta.json")<<R"({"version":1,"inputs":[{"path":"fog-raw.vdb","edits":[{"selector":"density","metadata":[{"name":"artist","operation":"set","type":"string","value":"fog artist"}]}]}],"metadata":[{"name":"scene","operation":"set","type":"string","value":"fog scene"}]})";
  (void)vdb::WriteAuthoredVdbSpec(files.directory/"fog-meta.json",files.directory/"fog-meta.vdb");
  const auto source=files.directory/std::filesystem::path(u8"密度-é.vdb");std::filesystem::copy_file(files.directory/"fog-meta.vdb",source);
  const auto before=surfaceUiBytes(source);const auto foreign=files.directory/"untouched.vdb";std::ofstream(foreign)<<"existing output bytes";
  OpenVdbAuthoringRequest request;request.operation=Operation::IntegrateFogRays;request.input=surfaceUiUtf8(source);request.inputGrid="descriptor:density";
  request.output=surfaceUiUtf8(foreign);request.rayQueries={{{-1,0,0},{7,0,0},0,2}};
  request.processing.voxelSize=request.processing.halfWidth=0;request.processing.maxTriangles=0;request.processing.gridName=std::string(1024*1024,'x');
  request.secondInput="unused-missing.vdb";request.levelSetRays.limits.maxWorkItems=0;
  OpenVdbAuthoringPanel panel;ASSERT_TRUE(panel.submit(request));request.rayQueries[0].maximumDistance=99;request.fogRays.extinction=0;
  ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  const auto& message=panel.result()->message;
  EXPECT_DOUBLE_EQ(fogUiNumber(message,"Ray 0: I="),8);EXPECT_DOUBLE_EQ(fogUiNumber(message,"; tau="),8);
  EXPECT_NEAR(fogUiNumber(message,"; T="),std::exp(-8.),1e-16);
  EXPECT_NEAR(fogUiNumber(message,"; absorbed="),-std::expm1(-8.),1e-15);
  for(const auto* term:{"empty activity: yes","fog artist","fog scene","Exact rational","represented","not vacuum","No scattering","first64"}) {
    EXPECT_NE(message.find(term),std::string::npos)<<message;
  }
  EXPECT_TRUE(panel.result()->output.empty());EXPECT_FALSE(panel.result()->modelOutput);EXPECT_FALSE(panel.result()->restartGrids);
  EXPECT_EQ(surfaceUiBytes(source),before);EXPECT_EQ(surfaceUiBytes(foreign),"existing output bytes");
}

TEST(OpenVdbAuthoring, FogIntegralRealCubicAndBackgroundHoleHaveIndependentAnalyticValues) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  if(!vdb::IsAvailable())GTEST_SKIP();AuthoringFiles files;OpenVdbAuthoringPanel panel;
  std::vector<double> corners(8);corners[7]=8;
  (void)vdb::WriteDenseVdb(files.directory/"cubic.vdb",fogUiField(corners,std::vector<uint8_t>(8,1),{2,2,2}));
  OpenVdbAuthoringRequest request;request.operation=Operation::IntegrateFogRays;request.input=files.path("cubic.vdb");
  request.rayQueries={{{0,0,0},{1,1,1},0,std::sqrt(3.)}};
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_NEAR(fogUiNumber(panel.result()->message,"Ray 0: I="),2*std::sqrt(3.),1e-12); // integral8*u^3 ds
  (void)vdb::WriteDenseVdb(files.directory/"hole.vdb",fogUiField({0},{0},{1,1,1},2));
  request.input=files.path("hole.vdb");request.rayQueries={{{-2,0,0},{3,0,0},0,4}};
  const auto before=surfaceUiBytes(files.directory/"hole.vdb");
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_DOUBLE_EQ(fogUiNumber(panel.result()->message,"Ray 0: I="),6); // BG8 minus nodal tent2
  EXPECT_NE(panel.result()->message.find("empty activity: yes"),std::string::npos);
  EXPECT_NE(panel.result()->message.find("numerically constant=no"),std::string::npos);EXPECT_EQ(surfaceUiBytes(files.directory/"hole.vdb"),before);
}

TEST(OpenVdbAuthoring, FogIntegralBeerReportsThinOpaqueVacuumAndZeroExtinctionWithoutInventedFiles) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  if(!vdb::IsAvailable())GTEST_SKIP();AuthoringFiles files;OpenVdbAuthoringPanel panel;
  OpenVdbAuthoringRequest request;request.operation=Operation::IntegrateFogRays;request.rayQueries={{{0,0,0},{1,0,0},0,1}};
  const double tiny=std::ldexp(1.,-1060);
  (void)vdb::WriteDenseVdb(files.directory/"thin.vdb",fogUiField({tiny},{0},{1,1,1},tiny));request.input=files.path("thin.vdb");
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_DOUBLE_EQ(fogUiNumber(panel.result()->message,"; T="),1);EXPECT_EQ(fogUiNumber(panel.result()->message,"; absorbed="),tiny);
  (void)vdb::WriteDenseVdb(files.directory/"opaque.vdb",fogUiField({1000},{0},{1,1,1},1000));request.input=files.path("opaque.vdb");
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));EXPECT_FALSE(panel.result()->success);EXPECT_TRUE(panel.result()->output.empty());
  request.fogRays.transmissionPolicy=vdb::FogTransmissionPolicy::AllowOpaqueUnderflow;
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_EQ(fogUiNumber(panel.result()->message,"; T="),0);EXPECT_EQ(fogUiNumber(panel.result()->message,"; absorbed="),1);
  EXPECT_NE(panel.result()->message.find("status=opaque-underflow"),std::string::npos);
  request.fogRays.extinction=0;ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_EQ(fogUiNumber(panel.result()->message,"Ray 0: I="),1000);EXPECT_EQ(fogUiNumber(panel.result()->message,"; T="),1);
  EXPECT_EQ(fogUiNumber(panel.result()->message,"; absorbed="),0);
  (void)vdb::WriteDenseVdb(files.directory/"vacuum.vdb",fogUiField({0},{0}));request.input=files.path("vacuum.vdb");request.fogRays.extinction=1;
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_EQ(fogUiNumber(panel.result()->message,"Ray 0: I="),0);EXPECT_EQ(fogUiNumber(panel.result()->message,"; T="),1);
}

TEST(OpenVdbAuthoring, FogIntegralEveryCapAndEmptyQuerySourceValidationPreserveInputAndOffPrecedesOptions) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  AuthoringFiles files;OpenVdbAuthoringPanel panel;OpenVdbAuthoringRequest request;request.operation=Operation::IntegrateFogRays;
  request.input=files.path("missing.vdb");request.output=files.path("never-written.vdb");
  if(!vdb::IsAvailable()) {
    request.input=std::string("\xff",1);request.processing.maxInputBytes=request.processing.maxVoxelCount=0;
    request.fogRays.limits.maxRays=request.fogRays.limits.maxGrids=0;request.fogRays.limits.maxWorkItems=0;
    request.fogRays.maxCellSegments=request.fogRays.maxCellSegmentsPerRay=0;request.fogRays.extinction=-1;
    ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success);
    EXPECT_NE(panel.result()->message.find("ENABLE_OPENVDB=ON"),std::string::npos);EXPECT_TRUE(panel.result()->output.empty());
    EXPECT_FALSE(panel.result()->modelOutput);EXPECT_FALSE(panel.result()->restartGrids);EXPECT_FALSE(panel.result()->particleGeometryArtifact);EXPECT_FALSE(panel.result()->boundaryFluidArtifact);
    EXPECT_TRUE(std::filesystem::is_empty(files.directory));return;
  }
  (void)vdb::WriteDenseVdb(files.directory/"density.vdb",fogUiField());
  auto second=fogUiField();second.gridName="other";(void)vdb::WriteDenseVdb(files.directory/"other.vdb",second);
  std::ofstream(files.directory/"bundle.json")<<R"({"version":1,"inputs":[{"path":"density.vdb"},{"path":"other.vdb"}]})";
  (void)vdb::WriteAuthoredVdbSpec(files.directory/"bundle.json",files.directory/"bundle.vdb");
  request.input=files.path("bundle.vdb");request.inputGrid="density";request.rayQueries={{{-1,0,0},{1,0,0},0,2},{{1,0,0},{-1,0,0},0,2}};
  const auto before=surfaceUiBytes(files.directory/"bundle.vdb");const auto good=request;
  for(int failure=0;failure<8;++failure) {
    request=good;
    if(failure==0)request.processing.maxInputBytes=1;
    if(failure==1)request.processing.maxVoxelCount=511;
    if(failure==2)request.fogRays.limits.maxWorkItems=1;
    if(failure==3)request.fogRays.limits.maxRays=1;
    if(failure==4)request.fogRays.limits.maxGrids=1;
    if(failure==5)request.fogRays.limits.maxMetadataBytes=1;
    if(failure==6)request.fogRays.maxCellSegmentsPerRay=1;
    if(failure==7)request.fogRays.maxCellSegments=1;
    ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());EXPECT_FALSE(panel.result()->success)<<failure;
    EXPECT_TRUE(panel.result()->output.empty());EXPECT_FALSE(std::filesystem::exists(files.directory/"never-written.vdb"));EXPECT_EQ(surfaceUiBytes(files.directory/"bundle.vdb"),before);
  }
  (void)vdb::WriteDenseVdb(files.directory/"negative.vdb",fogUiField({-1},{0}));request=good;request.input=files.path("negative.vdb");request.rayQueries.clear();
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));EXPECT_FALSE(panel.result()->success);
  request=good;request.rayQueries.clear();ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  EXPECT_NE(panel.result()->message.find("Completed fog rays: 0"),std::string::npos);EXPECT_EQ(surfaceUiBytes(files.directory/"bundle.vdb"),before);
}

TEST(OpenVdbAuthoring, FogIntegralCompletedBatchDisplayIsExplicitlyBoundedRatherThanTruncatedQueries) {
  namespace vdb=container::geometry::vdb;using Operation=container::ui::OpenVdbAuthoringOperation;
  if(!vdb::IsAvailable())GTEST_SKIP();AuthoringFiles files;(void)vdb::WriteDenseVdb(files.directory/"constant.vdb",fogUiField({2},{0},{1,1,1},2));
  OpenVdbAuthoringRequest request;request.operation=Operation::IntegrateFogRays;request.input=files.path("constant.vdb");
  request.fogRays.limits.maxWorkItems=2'000'000'000;request.fogRays.limits.maxRays=65;
  request.rayQueries.assign(65,{{0,0,0},{1,0,0},0,1});OpenVdbAuthoringPanel panel;
  ASSERT_TRUE(panel.submit(request));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());ASSERT_TRUE(panel.result()->success)<<panel.result()->message;
  const auto& message=panel.result()->message;EXPECT_NE(message.find("Completed fog rays: 65"),std::string::npos);
  EXPECT_NE(message.find("Ray 63: I=2"),std::string::npos);EXPECT_EQ(message.find("Ray 64:"),std::string::npos);
  EXPECT_NE(message.find("Additional COMPLETED ray results omitted from display: 1"),std::string::npos);
  EXPECT_LT(message.size(),128*1024);EXPECT_TRUE(panel.result()->output.empty());
}

TEST(OpenVdbAuthoring, FogIntegralCustomResultsCannotExposeAnyArtifactActionThroughDraftSwitches) {
  using Operation=container::ui::OpenVdbAuthoringOperation;BoundaryFluidUiContext context;
  OpenVdbAuthoringPanel panel([](const auto& request){OpenVdbAuthoringResult result{true,"custom",request.output,true};
    result.restartGrids=std::array<std::string,4>{"D","U","F","T"};
    result.particleGeometryArtifact=container::ui::OpenVdbParticleGeometryArtifact{"foreign.vdb","descriptor:S","descriptor:M"};
    result.boundaryFluidArtifact=container::ui::OpenVdbBoundaryFluidArtifact{"foreign.vdb",{}};return result;});
  panel.draft.operation=Operation::IntegrateFogRays;panel.draft.input="submitted.vdb";panel.draft.output="foreign.vdb";
  ASSERT_TRUE(panel.submit(panel.draft));ASSERT_TRUE(settle(panel));ASSERT_TRUE(panel.result());
  EXPECT_TRUE(panel.result()->output.empty());EXPECT_FALSE(panel.result()->modelOutput);EXPECT_FALSE(panel.result()->restartGrids);
  EXPECT_FALSE(panel.result()->particleGeometryArtifact);EXPECT_FALSE(panel.result()->boundaryFluidArtifact);
  panel.draft={};panel.draft.operation=Operation::Sphere;panel.draft.input="unrelated.vdb";panel.previewInput="independent.vdb";panel.previewGrid="independent-grid";
  std::array<ImGuiID,7> actions{};
  const auto frame=[&]{ImGui::NewFrame();ImGui::SetNextWindowSize({1150,9900});ImGui::Begin("Fog readonly result actions");ImGui::PushID(&panel);
    size_t i=0;for(const auto* label:{"Use output as input","Use state as restart","Load preview","Use boundary fluid state as restart","Inspect boundary fluid density","Inspect generated SDF","Load generated SDF preview"})actions[i++]=ImGui::GetID(label);
    ImGui::PopID();ImGui::SetNextItemOpen(true,ImGuiCond_Always);auto model=panel.draw();ImGui::End();ImGui::Render();return model;};
  EXPECT_FALSE(frame());EXPECT_FALSE(frame());
  for(const auto action:actions){ImGui::ActivateItemByID(action);EXPECT_FALSE(frame());EXPECT_EQ(panel.draft.input,"unrelated.vdb");EXPECT_EQ(panel.previewInput,"independent.vdb");EXPECT_EQ(panel.previewGrid,"independent-grid");EXPECT_FALSE(panel.previewOptions());}
  panel.draft.operation=Operation::Sphere;panel.draft.output="ordinary.vdb";ASSERT_TRUE(panel.submit(panel.draft));ASSERT_TRUE(settle(panel));
  EXPECT_FALSE(frame());EXPECT_FALSE(frame());ImGui::ActivateItemByID(actions[2]);EXPECT_EQ(frame(),std::optional<std::string>("ordinary.vdb"));
}

} // namespace


