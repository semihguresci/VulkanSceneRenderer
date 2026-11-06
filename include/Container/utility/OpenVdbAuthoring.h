#pragma once

#include "Container/geometry/OpenVdbProcessing.h"
#include "Container/geometry/OpenVdbPointProcessing.h"
#include "Container/geometry/OpenVdbFluidProjection.h"
#include "Container/geometry/OpenVdbPrimitives.h"
#include "Container/geometry/OpenVdbFluidSimulation.h"
#include "Container/geometry/OpenVdbFluidSequence.h"
#include "Container/geometry/OpenVdbMeshConversion.h"
#include "Container/geometry/OpenVdbVectorPreview.h"
#include "Container/geometry/OpenVdbPointRasterization.h"
#include "Container/geometry/OpenVdbFluidViscosity.h"
#include "Container/geometry/OpenVdbLevelSetTransport.h"
#include "Container/geometry/OpenVdbFluidSources.h"
#include "Container/geometry/OpenVdbPointAttributes.h"
#include "Container/geometry/OpenVdbDenseInterchange.h"
#include "Container/geometry/OpenVdbMultiResolution.h"
#include "Container/geometry/OpenVdbPointSelection.h"
#include "Container/geometry/OpenVdbFileAuthoringSpec.h"
#include "Container/geometry/OpenVdbTransportRenderJob.h"
#include "Container/geometry/OpenVdbFieldFilter.h"
#include "Container/geometry/OpenVdbPointMask.h"
#include "Container/geometry/OpenVdbSlicePreview.h"
#include "Container/geometry/OpenVdbNativePointPreview.h"
#include "Container/geometry/OpenVdbFieldComposite.h"
#include "Container/geometry/OpenVdbTopology.h"
#include "Container/geometry/OpenVdbMaskedValues.h"
#include "Container/geometry/OpenVdbSdfBackground.h"
#include "Container/geometry/OpenVdbSdfFilters.h"
#include "Container/geometry/OpenVdbVolumeSpheres.h"
#include "Container/geometry/OpenVdbFastSweeping.h"
#include "Container/geometry/OpenVdbRegions.h"
#include "Container/geometry/OpenVdbSurfaceQueries.h"
#include "Container/geometry/OpenVdbFracture.h"
#include "Container/geometry/OpenVdbFieldAdvection.h"
#include "Container/geometry/OpenVdbPotentialFlow.h"
#include "Container/geometry/OpenVdbPointNeighbors.h"
#include "Container/geometry/OpenVdbLevelSetRebuild.h"
#include "Container/geometry/OpenVdbSurfaceFields.h"
#include "Container/geometry/OpenVdbPointMotion.h"
#include "Container/geometry/OpenVdbNativePointAdvection.h"
#include "Container/geometry/OpenVdbPointTransfer.h"
#include "Container/geometry/OpenVdbFieldResampling.h"
#include "Container/geometry/OpenVdbDiagnostics.h"
#include "Container/geometry/OpenVdbBoundaryProjection.h"
#include "Container/geometry/OpenVdbPointFieldSampling.h"
#include "Container/geometry/OpenVdbNanoInterchange.h"
#include "Container/geometry/OpenVdbNanoStatistics.h"
#include "Container/geometry/OpenVdbNanoPrimitives.h"
#include "Container/geometry/OpenVdbRegionQueries.h"
#include "Container/geometry/OpenVdbRayQueries.h"
#include "Container/geometry/OpenVdbValueComponents.h"
#include "Container/geometry/OpenVdbSurfaceReference.h"
#include "Container/geometry/OpenVdbParticleGeometry.h"
#include "Container/geometry/OpenVdbBoundaryFluidStep.h"
#include "Container/geometry/OpenVdbFogRayIntegrals.h"

#include <array>
#include <functional>
#include <future>
#include <optional>
#include <memory>
#include <string>

namespace container::geometry::vdb { struct ImportOptions; }

namespace container::ui {

enum class OpenVdbAuthoringOperation {
  Sphere, Mesh, Particles, Points, ExportPoints, Union, Intersection, Difference,
  Fog, Resample, Offset, Smooth, Velocity, Advect, Segment, ExportMesh, Inspect,
  Differential, Poisson, SampleScalar, SampleVector, Scatter, AdvectPoints,
  SamplePointVelocity, SamplePointRadius, Project, Platonic, Simulate, Measure, SimulateSequence, MeshDistance,
  PointsSdf, SplatDensity, SplatVelocity, SplatRadius, Viscosity, AdvectSdf, MorphSdf, FluidSources,
  EditPointAttributes, PointSchema, PointAttribute, PointGroup,
  DenseImport, DenseExport, MultiResolution, MultiResolutionInfo, MultiResolutionSample,
  SelectPoints, DeletePoints, CountPoints, PointStatistics, AuthorFile, RenderTransportImage, FilterField, PointVoxelMask, CompositeFields, TransformField, TopologyMask, TopologyBoolean, SampleNativePointAttribute,
  NanoInspect, NanoValidate, NanoCopy, VdbToNano, NanoToVdb, NanoChecksum, NanoStatistics, EditValueTopology, MaskedValues, SdfBackground, SdfFilter, VolumeSpheres, FastSweep, FastExtend, Regions, SurfaceQuery, Fracture, FieldAdvection, PotentialFlow, PointNeighbors, IndexedPointMask, RebuildLevelSet, SurfaceFields, MoveNativePoints, TransferPointAttribute, ResampleField, Diagnose, DiagnosticMask, ProjectBoundaries, ReprojectBoundaries, NanoPrimitive, AdvectNativePoints, QueryActiveRegions, RayIntersections, RayVolumeSpans, ValueComponents, RenderSurfaceReference, ParticleGeometry, BoundaryFluidStep, IntegrateFogRays, Count
};

struct OpenVdbAuthoringRequest {
  OpenVdbAuthoringOperation operation{OpenVdbAuthoringOperation::Sphere};
  std::string input, secondInput, output, inputGrid, secondGrid;
  std::string domainInput, domainGrid, temperatureInput, temperatureGrid;
  geometry::vdb::ProcessingOptions processing;
  geometry::vdb::SphereOptions sphere;
  geometry::vdb::PlatonicOptions platonic;
  geometry::vdb::SmoothingOptions smoothing;
  geometry::vdb::ScatterOptions scatter;
  geometry::vdb::PointAdvectionOptions pointAdvection;
  geometry::vdb::FluidProjectionOptions projection;
  geometry::vdb::FluidSimulationOptions simulation;
  geometry::vdb::FluidSequenceOptions sequence;
  geometry::vdb::MeshConversionOptions meshConversion;
  geometry::vdb::PointSplatOptions splat;
  geometry::vdb::FluidViscosityOptions viscosity;
  geometry::vdb::LevelSetTransportOptions levelSetTransport;
  geometry::vdb::FluidSourcesOptions sources;
  geometry::vdb::PointAttributeOptions pointAttributes;
  std::string pointField;
  geometry::vdb::DenseRegion denseRegion;
  geometry::vdb::MultiResolutionOptions multiresolution;
  std::optional<uint32_t> multiresolutionLevel;
  double sampleLevel{0};
  geometry::vdb::PointGroupSelection pointSelection;
  geometry::vdb::FileAuthoringSpecOptions fileAuthoring;
  geometry::vdb::TransportRenderJobOptions transportRender;
  std::string transportShader;
  geometry::vdb::FieldFilterOptions fieldFilter;
  geometry::vdb::FieldFilterOperation fieldFilterOperation{geometry::vdb::FieldFilterOperation::Mean};
  geometry::vdb::PointMaskOptions pointMask;
  geometry::vdb::FieldCompositeOperation fieldOperation{geometry::vdb::FieldCompositeOperation::Sum};
  geometry::vdb::FieldCompositeOptions fieldComposite;
  geometry::vdb::FieldValueTransformOptions fieldTransform;
  geometry::vdb::TopologyOperation topologyOperation{geometry::vdb::TopologyOperation::ActiveMask};
  geometry::vdb::TopologyBoolean topologyBoolean{geometry::vdb::TopologyBoolean::Union};
  geometry::vdb::TopologyOptions topology;
  geometry::vdb::ValueTopologyOperation valueTopologyOperation{geometry::vdb::ValueTopologyOperation::PruneExact};
  geometry::vdb::ValueTopologyOptions valueTopology;
  geometry::vdb::MaskedValuesOperation maskedValuesOperation{geometry::vdb::MaskedValuesOperation::IntersectActivity};
  uint64_t maskedValuesMaxWork{200'000'000};
  geometry::vdb::SdfBackgroundOperation sdfBackgroundOperation{geometry::vdb::SdfBackgroundOperation::BackgroundOnly};
  geometry::vdb::SdfBackgroundOptions sdfBackground;
  geometry::vdb::SdfFilterOperation sdfFilterOperation{geometry::vdb::SdfFilterOperation::Mean};
  geometry::vdb::SdfFilterOptions sdfFilter;
  geometry::vdb::VolumeSpheresOptions volumeSpheres;
  geometry::vdb::FastSweepingOperation fastSweepingOperation{geometry::vdb::FastSweepingOperation::ReinitializeSdf};
  geometry::vdb::FastSweepingOptions fastSweeping;
  bool fastSweepingFog{false};
  geometry::vdb::RegionsOperation regionsOperation{geometry::vdb::RegionsOperation::ActiveComponents};
  geometry::vdb::RegionsOptions regions;
  geometry::vdb::SurfaceQueryOptions surfaceQuery;
  geometry::vdb::FractureOptions fracture;
  geometry::vdb::FieldAdvectionOptions fieldAdvection;
  geometry::vdb::PotentialFlowOptions potentialFlow;
  geometry::vdb::PointNeighborOptions pointNeighbors;
  geometry::vdb::IndexedPointMaskOptions indexedPointMask;
  geometry::vdb::LevelSetRebuildOptions levelSetRebuild;
  geometry::vdb::SurfaceFieldOperation surfaceFieldOperation{geometry::vdb::SurfaceFieldOperation::MeanCurvature};
  geometry::vdb::SurfaceFieldsOptions surfaceFields;
  geometry::vdb::PointMotionOptions pointMotion;
  geometry::vdb::NativePointAdvectionOptions nativePointAdvection;
  geometry::vdb::PointTransferOptions pointTransfer;
  geometry::vdb::FieldResamplingOptions fieldResampling;
  geometry::vdb::DiagnosticsOptions diagnostics;
  geometry::vdb::BoundaryProjectionInputs boundaryInputs;
  geometry::vdb::BoundaryProjectionOptions boundaryProjection;
  geometry::vdb::BoundaryProjectionReplay boundaryReplay;
  geometry::vdb::PointFieldSamplingOptions pointFieldSampling;
  geometry::vdb::NanoVdbReadOptions nanoRead;
  geometry::vdb::NanoVdbWriteOptions nanoWrite;
  geometry::vdb::NanoVdbLimits nanoLimits;
  geometry::vdb::NanoVdbStatisticsMode nanoStatisticsMode{geometry::vdb::NanoVdbStatisticsMode::All};
  geometry::vdb::NanoVdbPrimitiveShape nanoPrimitiveShape{geometry::vdb::NanoVdbPrimitiveShape::Sphere};
  geometry::vdb::NanoVdbPrimitiveOptions nanoPrimitive;
  geometry::vdb::ActiveValueComponentsOptions valueComponents;
  geometry::vdb::ActiveRegionOptions activeRegionOptions;
  std::vector<geometry::vdb::IndexRegion> activeRegions{geometry::vdb::IndexRegion{}};
  geometry::vdb::LevelSetRayOptions levelSetRays;
  geometry::vdb::VolumeRayOptions volumeRays;
  std::vector<geometry::vdb::WorldRay> rayQueries{geometry::vdb::WorldRay{}};
  geometry::vdb::SurfaceReferenceCamera surfaceCamera;
  geometry::vdb::SurfaceReferenceOptions surfaceReference;
  uint64_t surfaceExportBytes{512ull*1024*1024};
  geometry::vdb::ParticleGeometryOptions particleGeometry;
  geometry::vdb::BoundaryFluidInputs boundaryFluidInputs;
  geometry::vdb::BoundaryFluidStepOptions boundaryFluid;
  geometry::vdb::FogRayIntegralOptions fogRays;
  bool nanoAllowMetadataLoss{false};
  geometry::vdb::SmoothingOperation filter{geometry::vdb::SmoothingOperation::Gaussian};
  std::array<double, 3> velocity{1, 0, 0};
  std::array<double, 3> queryPosition{};
  double distance{0.1}, time{1.0 / 24.0}, adaptivity{0.0};
  std::optional<double> isoValue;
  uint32_t steps{1}, maxComponents{64}, maxIterations{200};
  size_t maxPoints{1'000'000}, maxParticles{1'000'000};
  double tolerance{1e-6};
  int differential{0};
  bool statistics{false};
  uint32_t inspectionHistogramBins{0};
  std::optional<std::array<double,2>> inspectionHistogramRange;
};

// Generated VDB bindings belong to the submitted particle job. The panel
// constructs exact descriptor selectors before execution; executor-returned
// bindings and later draft edits cannot replace them.
struct OpenVdbParticleGeometryArtifact {
  std::string output;
  std::optional<std::string> sdfGrid, maskGrid;
};

// Submitted output9 binding; old pressure is recomputed on restart. Slots are
// velocity, fluid, kind, fraction, prescribed pressure, wall velocity, D, T.
struct OpenVdbBoundaryFluidArtifact {
  std::string output;
  std::array<std::string,8> grids;
  geometry::vdb::BoundaryFluidInitialVelocity initialVelocity{geometry::vdb::BoundaryFluidInitialVelocity::TransportStored};
};

struct OpenVdbAuthoringResult {
  bool success{false};
  std::string message, output;
  bool modelOutput{false};
  // Density, velocity, domain and temperature selectors from the submitted job.
  std::optional<std::array<std::string,4>> restartGrids;
  std::optional<OpenVdbParticleGeometryArtifact> particleGeometryArtifact;
  std::optional<OpenVdbBoundaryFluidArtifact> boundaryFluidArtifact;
};

struct OpenVdbAuthoringProgress {
  uint32_t completedFrames{}, totalFrames{};
  bool cancellationRequested{};
};

// One snapshot per explicit submission. File processing runs off the UI thread;
// polling never waits. Destruction joins an active operation so it cannot
// outlive the UI or libraries. Sequence jobs can cancel between timesteps;
// an active library solve must finish first. Other jobs are not cancellable.
class OpenVdbAuthoringPanel {
public:
  using Executor = std::function<OpenVdbAuthoringResult(const OpenVdbAuthoringRequest&)>;
  explicit OpenVdbAuthoringPanel(Executor executor = {});
  ~OpenVdbAuthoringPanel();
  OpenVdbAuthoringPanel(const OpenVdbAuthoringPanel&) = delete;
  OpenVdbAuthoringPanel& operator=(const OpenVdbAuthoringPanel&) = delete;
  bool submit(const OpenVdbAuthoringRequest& request);
  void poll();
  bool requestCancel();
  [[nodiscard]] std::optional<OpenVdbAuthoringProgress> progress() const;
  [[nodiscard]] bool busy() const { return job_.valid(); }
  [[nodiscard]] const std::optional<OpenVdbAuthoringResult>& result() const { return result_; }
  // Returns a model path only after an explicit Load preview click.
  [[nodiscard]] std::optional<std::string> draw();
  [[nodiscard]] const auto& previewOptions() const { return previewOptions_; }
  OpenVdbAuthoringRequest draft;
  // Display settings are snapshotted only when Load preview is clicked.
  std::string previewInput, previewGrid;
  int previewKind{0}; // 0 scalar/points, 1 glyphs, 2 streamlines, 3 slice, 4 native points.
  geometry::vdb::PointPreviewOptions pointPreview;
  geometry::vdb::VectorPreviewOptions vectorPreview;
  geometry::vdb::SlicePreviewOptions slicePreview;
  geometry::vdb::NativePointPreviewOptions nativePointPreview;
  geometry::vdb::PointAttributeOptions nativePointLimits;
private:
  struct JobControl;
  Executor executor_;
  std::future<OpenVdbAuthoringResult> job_;
  std::optional<OpenVdbAuthoringResult> result_;
  std::shared_ptr<JobControl> jobControl_;
  std::shared_ptr<const geometry::vdb::ImportOptions> previewOptions_;
};

} // namespace container::ui
