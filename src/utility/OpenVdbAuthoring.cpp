#include "Container/utility/OpenVdbAuthoring.h"

#include "Container/geometry/OpenVdbAdvection.h"
#include "Container/geometry/OpenVdbInspect.h"
#include "Container/geometry/OpenVdbLoader.h"
#include "Container/geometry/OpenVdbMeasure.h"
#include "Container/geometry/OpenVdbMeshExport.h"
#include "Container/geometry/OpenVdbParticles.h"
#include "Container/geometry/OpenVdbPoints.h"
#include "Container/geometry/OpenVdbPoisson.h"
#include "Container/geometry/OpenVdbSegmentation.h"
#include "Container/geometry/OpenVdbVectorProcessing.h"
#include "Container/geometry/OpenVdbPointAttributesJson.h"
#include "Container/utility/Platform.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <chrono>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <limits>
#include <stdexcept>
#include <utility>
#include <type_traits>
#include <charconv>
#include <set>

namespace container::ui {
namespace {
namespace vdb = geometry::vdb;
using Operation = OpenVdbAuthoringOperation;
bool nativeFileOperation(Operation operation) {
  return operation==Operation::NanoInspect || operation==Operation::NanoValidate ||
      operation==Operation::NanoCopy || operation==Operation::VdbToNano ||
      operation==Operation::NanoToVdb || operation==Operation::NanoChecksum || operation==Operation::NanoStatistics || operation==Operation::NanoPrimitive;
}

const char* defaultGridName(Operation operation) {
  switch (operation) {
  case Operation::Points: case Operation::Scatter: case Operation::AdvectPoints:
  case Operation::SamplePointVelocity: case Operation::SamplePointRadius: return "points";
  case Operation::EditPointAttributes: return "points";
  case Operation::SelectPoints: case Operation::DeletePoints: return "points";
  case Operation::MoveNativePoints: case Operation::AdvectNativePoints: return "points";
  case Operation::SurfaceFields: return "surface_field";
  case Operation::TransferPointAttribute: return "point_field";
  case Operation::ResampleField: return "field";
  case Operation::PointVoxelMask: return "point_mask";
  case Operation::IndexedPointMask: return "point_mask";
  case Operation::FieldAdvection: return "advected";
  case Operation::Regions: return "regions";
  case Operation::Fracture: return "fracture";
  case Operation::CompositeFields: case Operation::TransformField: return "field";
  case Operation::EditValueTopology: case Operation::MaskedValues: return "field";
  case Operation::TopologyMask: case Operation::TopologyBoolean: return "mask";
  case Operation::SampleNativePointAttribute: return "points";
  case Operation::Velocity: return "velocity";
  case Operation::Poisson: return "solution";
  case Operation::MeshDistance: return "distance";
  case Operation::SplatDensity: return "point_density";
  case Operation::SplatVelocity: return "point_velocity";
  case Operation::SplatRadius: return "point_radius";
  default: return "surface";
  }
}

std::filesystem::path path(const std::string& value) {
  return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

OpenVdbAuthoringResult executeNativePrimitive(const OpenVdbAuthoringRequest& r) {
    if(!vdb::NanoVdbInterchangeCapabilities().nativeIo) {
      (void)vdb::WriteNanoVdbPrimitive({},r.nanoPrimitiveShape,r.nanoPrimitive,r.nanoWrite,r.nanoLimits);
      throw std::runtime_error("Native NanoVDB primitive authoring is unavailable; enable ENABLE_NANOVDB_IO.");
    }
    auto limits=r.nanoLimits;
    constexpr uint64_t minimumReport=32768+8192;
    if(limits.maxWorkspaceBytes<=minimumReport || limits.maxWorkspaceBytes>(16ull<<30) || limits.maxWork<=4*minimumReport || limits.maxWork>4'000'000'000ull)
      throw std::invalid_argument("Native primitive UI report needs joint workspace and formatting-work reserves.");
    if(r.output.size()>limits.maxWorkspaceBytes/8)
      throw std::invalid_argument("Native primitive UI output path exceeds the joint report/workspace budget.");
    const auto output=path(r.output);
    OpenVdbAuthoringResult result{true,{},r.output,false};
    auto settings=r.nanoPrimitive;
    if(r.nanoPrimitiveShape!=vdb::NanoVdbPrimitiveShape::Sphere)settings.radius=1;
    if(r.nanoPrimitiveShape!=vdb::NanoVdbPrimitiveShape::Box)settings.boxDimensions={2,2,2};
    if(r.nanoPrimitiveShape!=vdb::NanoVdbPrimitiveShape::Torus){settings.majorRadius=1;settings.minorRadius=.25;}
    result.message.reserve(32768);
    const uint64_t retained=result.message.capacity()+8192+uint64_t(r.output.size())*8;
    const uint64_t formattingWork=retained*4;
    if(retained>=limits.maxWorkspaceBytes || formattingWork>=limits.maxWork)
      throw std::invalid_argument("Native primitive UI retained report exceeds joint workspace/work caps.");
    limits.maxWorkspaceBytes-=retained;limits.maxWork-=formattingWork;
    const auto authored=vdb::WriteNanoVdbPrimitive(output,r.nanoPrimitiveShape,settings,r.nanoWrite,limits);
    bool truncated=false;
    const auto append=[&](std::string_view value){if(value.size()>result.message.capacity()-result.message.size()-128){truncated=true;return;}result.message.append(value);};
    const auto number=[&](auto value){char buffer[128];const auto converted=std::to_chars(buffer,buffer+sizeof(buffer),value);if(converted.ec==std::errc{})append({buffer,size_t(converted.ptr-buffer)});else append("unavailable");};
    append("Authored native CPU ");append(r.nanoPrimitiveShape==vdb::NanoVdbPrimitiveShape::Sphere?"sphere":r.nanoPrimitiveShape==vdb::NanoVdbPrimitiveShape::Box?"axis-aligned box":"WORLD-Y torus");
    append(settings.gridClass==vdb::NanoVdbPrimitiveClass::LevelSet?" level set":" fog");append(settings.storage==vdb::NanoVdbPrimitiveStorage::Float?" (Float)":" (Double)");
    append(".\nKernel candidate cells: ");number(authored.candidateVoxels);append("; full leaf slots: ");number(authored.storedLeafVoxelUpperBound);append("; bounded nodes: ");number(authored.nodeUpperBound);
    append(".\nNative/encoded bytes: ");number(authored.file.nativeBytes);append("/");number(authored.file.encodedBytes);append("; joint native/UI workspace/work upper bounds: ");number(authored.workspaceUpperBound+retained);append("/");number(authored.workUpperBound+formattingWork);
    append(".\nRounded published Float values: ");number(authored.roundedFloatValues);append("; maximum absolute value error: ");number(authored.maximumFloatAbsoluteError);
    append(". Float map caches rounded: ");append(authored.floatMapCachesRounded?"yes":"no");append("; maximum stored-coefficient discrepancy in voxels: ");number(authored.maximumFloatMapCacheErrorVoxels);
    for(const auto& item:authored.file.grids){append(".\n");append(item.name);append(": ");number(item.activeVoxels);append(" active voxels; CRC ");append(item.checksumVerified?"verified":"disabled");append("; full data coverage ");append(item.checksumCoversAllData?"yes":"no");}
    for(const auto& diagnostic:authored.diagnostics){append("\n");append(diagnostic);}
    for(const auto& diagnostic:authored.file.diagnostics){append("\n");append(diagnostic);}
    if(truncated)result.message.append("\nAdditional report text omitted at the display limit.");
    return result;
}

OpenVdbAuthoringResult executeNativePointAdvection(const OpenVdbAuthoringRequest& r) {
  if(!vdb::IsAvailable())
    throw std::runtime_error("OpenVDB native point advection is unavailable; configure with -DENABLE_OPENVDB=ON.");
  long double callerBytes=r.input.size()*1.0L+r.secondInput.size()+r.output.size()+
      r.inputGrid.size()+r.secondGrid.size()+r.processing.gridName.size();
  for(const auto* names:{&r.pointSelection.anyOf,&r.pointSelection.allOf,&r.pointSelection.noneOf})
    for(const auto& name:*names)callerBytes+=name.size()+64.0L;
  if(callerBytes>256*1024 || r.processing.maxInputBytes<=65536 || !r.nativePointAdvection.maxWork ||
      r.nativePointAdvection.maxWork>10'000'000'000ull)
    throw std::invalid_argument("Native point advection UI report/options need bounded shared byte/work caps.");
  OpenVdbAuthoringResult result{true,{},r.output,false};result.message.reserve(32768);
  const long double retained=result.message.capacity()+1.0L+65536+32*callerBytes;
  const long double formatWork=4*retained;
  if(retained>=r.processing.maxInputBytes || formatWork>=r.nativePointAdvection.maxWork)
    throw std::invalid_argument("Native point advection UI retained report exceeds shared byte/work caps.");
  const auto reportBytes=uintmax_t(retained);const auto formattingWork=uint64_t(formatWork);
  auto processing=r.processing;processing.maxInputBytes-=reportBytes;
  const auto& source=r.nativePointAdvection;
  vdb::NativePointAdvectionOptions settings;
  settings.timeStep=source.timeStep;settings.steps=source.steps;settings.integration=source.integration;
  settings.velocityLayout=source.velocityLayout;settings.targetIndexToWorld=source.targetIndexToWorld;
  settings.maxPositionErrorVoxels=source.maxPositionErrorVoxels;settings.rejectActivityMerges=source.rejectActivityMerges;
  settings.maxStepTravelVelocityVoxels=source.maxStepTravelVelocityVoxels;settings.maxTravelVelocityVoxels=source.maxTravelVelocityVoxels;
  settings.maxWork=source.maxWork-formattingWork;settings.maxGrids=source.maxGrids;settings.maxMetadataBytes=source.maxMetadataBytes;
  settings.groups=r.pointSelection;
  auto limits=r.pointAttributes;limits.maxPoints=r.maxPoints;
  limits.maxEdits=vdb::PointAttributeOptions{}.maxEdits; // Reading/moving has no edit-list cap.
  const auto input=path(r.input),velocity=path(r.secondInput),output=path(r.output);
  const auto authored=vdb::WriteAdvectedNativePoints(input,velocity,output,settings,r.inputGrid,r.secondGrid,processing,limits);
  bool truncated=false;
  const auto append=[&](std::string_view value) {
    if(value.size()>result.message.capacity()-result.message.size()-128){truncated=true;return;}
    result.message.append(value);
  };
  const auto number=[&](auto value) {
    char buffer[128];const auto converted=std::to_chars(buffer,buffer+sizeof(buffer),value);
    if(converted.ec==std::errc{})append({buffer,size_t(converted.ptr-buffer)});else append("unavailable");
  };
  const auto& motion=authored.motion;
  append("Advected ");number(motion.selectedCount);append(" selected native points; retained ");number(motion.outputCount);append(" of ");number(motion.inputCount);append(" records.");
  append(authored.integrationSkipped?"\nIntegration skipped; native motion no-op: ":"\nIntegration executed; native motion no-op: ");append(motion.noOp?"yes":"no");
  append(". Integrator: ");append(settings.integration==vdb::NativePointIntegration::RK2?"RK2":"RK4");append("; executed equal steps/samples: ");number(authored.executedSteps);append("/");number(authored.velocitySampleCount);
  append(".\nInput/output active records: ");number(motion.inputActiveCount);append("/");number(motion.outputActiveCount);append("; inactive records activated by explicit OR: ");number(motion.inactiveRecordsActivated);
  append("; output occupied voxels: ");number(motion.outputVoxelCount);
  append(".\nMaximum step/total L2 travel in VELOCITY-index voxels: ");number(authored.maximumStepTravelVelocityVoxels);append("/");number(authored.maximumTravelVelocityVoxels);
  append("; maximum WORLD path length: ");number(authored.maximumWorldTravel);append("; WORLD speed bound per second: ");number(authored.maximumWorldSpeedBound);
  append(".\nFinal P codec/index error in target voxels/WORLD: ");number(motion.maxPositionErrorVoxels);append("/");number(motion.maxPositionErrorWorld);
  append("; unselected target-voxel error: ");number(motion.maxUnselectedPositionErrorVoxels);append("; Double endpoint arithmetic envelope in WORLD units: ");number(authored.maximumEndpointArithmeticEnvelopeWorld);
  append(".\nJoint native/UI work upper bound: ");number(motion.workUpperBound+formattingWork);append("; retained UI bytes: ");number(reportBytes);
  append(".\nOnly P changes; v/pscale, IDs, strings/groups, registered fixed codecs and opaque non-P bits are preserved. Native record order may change. Codec error is relative to computed RK endpoints, not an ODE accuracy, mass-conservation or incompressibility certificate.");
  for(const auto& diagnostic:motion.diagnostics){append("\n");append(diagnostic);}
  for(const auto& diagnostic:authored.diagnostics){append("\n");append(diagnostic);}
  if(truncated)result.message.append("\nAdditional report text omitted at the display limit.");
  static_assert(std::is_nothrow_move_constructible_v<OpenVdbAuthoringResult>);
  return result;
}

struct RayUiText {
  std::string& text;
  bool truncated{false};
  void append(std::string_view value) {
    if(value.size()>text.capacity()-text.size()-128){truncated=true;return;}
    text.append(value);
  }
  template<class T> void number(T value) {
    char buffer[128];std::to_chars_result parsed;
    if constexpr(std::is_floating_point_v<T>)
      parsed=std::to_chars(buffer,buffer+sizeof(buffer),value,std::chars_format::general,std::numeric_limits<T>::max_digits10);
    else parsed=std::to_chars(buffer,buffer+sizeof(buffer),value);
    if(parsed.ec==std::errc{})append({buffer,size_t(parsed.ptr-buffer)});else append("unavailable");
  }
  template<size_t N>void vector(const std::array<double,N>& value) {
    append("(");for(size_t i=0;i<N;++i){if(i)append(",");number(value[i]);}append(")");
  }
  void bytes(std::string_view value) {
    // ASCII escaped BYTE prefix, not a claim that arbitrary native names or
    // metadata are UTF8. Controls/NUL/nonASCII bytes cannot truncate ImGui text.
    constexpr char hex[]="0123456789abcdef";append("\"");
    for(const unsigned char c:value.substr(0,256)) {
      if(c>=32 && c<127 && c!='"' && c!='\\')append({reinterpret_cast<const char*>(&c),1});
      else {char escaped[]={'\\','x',hex[c>>4],hex[c&15]};append({escaped,4});}
    }
    append("\" [");number(value.size());append(" bytes");
    if(value.size()>256)append("; prefix only");append("]");
  }
  void source(const vdb::RayQuerySource& source) {
    append("Source descriptor ");bytes(source.descriptorName);append("; name ");bytes(source.gridName);append("; native type ");bytes(source.valueType);
    append(".\nChecked active cells/tiles/stored leaves: ");number(source.inputActiveVoxelCount);append("/");number(source.inputActiveTileCount);append("/");number(source.inputStoredLeafCount);
    append("; empty activity: ");append(source.empty?"yes":"no");append(".\nExact INDEX to WORLD homogeneous rows:\n");vector(source.indexToWorld);
    const auto summaries=[&](std::string_view title,const std::vector<vdb::RayQueryMetadata>& values) {
      append("\n");append(title);append(": ");number(values.size());append(" records; showing at most8 ASCII escaped byte prefixes.");
      for(size_t i=0;i<std::min<size_t>(8,values.size());++i) {
        append("\n  ");bytes(values[i].name);append(" type=");bytes(values[i].type);append(" bytes=");number(values[i].nativeBytes);
        if(values[i].opaque)append("; opaque native payload.");else{append(" value=");bytes(values[i].value);}
      }
      if(values.size()>8){append("\n  Additional metadata records omitted from display: ");number(values.size()-8);}
    };
    summaries("Grid metadata",source.metadata);summaries("File metadata",source.fileMetadata);
  }
};
const char* rayUiNormal(vdb::RayNormalStatus value) {
  switch(value) {
  case vdb::RayNormalStatus::NotRequested:return "not-requested";
  case vdb::RayNormalStatus::NoHit:return "no-hit";
  case vdb::RayNormalStatus::Available:return "available";
  case vdb::RayNormalStatus::MissingActiveFootprint:return "missing-active-footprint";
  case vdb::RayNormalStatus::DegenerateGradient:return "degenerate-gradient";
  }
  return "unknown";
}
OpenVdbAuthoringResult executeRayQueries(const OpenVdbAuthoringRequest& r,bool sdf) {
  if(!vdb::IsAvailable()) {
    if(sdf)(void)vdb::QueryLevelSetRays({}, {},r.levelSetRays,{},r.processing);
    else(void)vdb::QueryActiveVolumeRays({}, {},r.volumeRays,{},r.processing);
    throw std::runtime_error("OpenVDB rays unavailable; configure ENABLE_OPENVDB=ON.");
  }
  auto level=r.levelSetRays;auto volume=r.volumeRays;
  auto& limits=sdf?level.limits:volume.limits;
  const long double caller=r.input.size()*1.0L+r.inputGrid.size();
  if(caller>1024*1024 || r.rayQueries.size()>1'000'000 || !limits.maxRays ||
     limits.maxRays>1'000'000 || r.rayQueries.size()>limits.maxRays ||
     !limits.maxWorkItems || limits.maxWorkItems>10'000'000'000ull)
    throw std::invalid_argument("Ray UI caller/query/work limits are invalid or unbounded.");
  OpenVdbAuthoringResult result{true,{}, {},false};
  constexpr uint64_t requestedMessage=128*1024;
  const long double expected=requestedMessage+65536+16*caller+
      2.0L*r.rayQueries.capacity()*sizeof(vdb::WorldRay)+sizeof(r);
  if(expected>=r.processing.maxInputBytes || 4*expected>=limits.maxWorkItems)
    throw std::invalid_argument("Ray UI report/ray/path reservation exceeds shared byte/work caps.");
  result.message.reserve(requestedMessage);
  const long double actual=result.message.capacity()+65536.0L+16*caller+
      2.0L*r.rayQueries.capacity()*sizeof(vdb::WorldRay)+sizeof(r);
  if(actual>=r.processing.maxInputBytes || 4*actual>=limits.maxWorkItems)
    throw std::invalid_argument("Ray UI actual retained report capacity exceeds shared byte/work caps.");
  const auto retained=uintmax_t(std::ceil(actual));const uint64_t formatWork=4*uint64_t(retained);
  auto processing=r.processing;processing.maxInputBytes-=retained;limits.maxWorkItems-=formatWork;
  // No access/conversion of stale hidden output or second-input paths.
  const auto input=path(r.input);
  RayUiText text{result.message};
  uint64_t used=0;
  if(sdf) {
    const auto found=vdb::QueryLevelSetRays(input,r.rayQueries,level,r.inputGrid,processing);
    used=found.source.workItems;
    text.append("Approximate native SDF rays: ");text.number(found.rays.size());text.append(" queries; displaying first64. WORLD distances and normalized WORLD directions.\nNative HDDA/one-dimensional chord search is not an exact trilinear cubic solver. Tangencies/multiple roots can be missed; sampled-zero includes signed zero and zero plateaus, not an isolated-surface certificate.\n");
    text.source(found.source);
    for(size_t i=0;i<std::min<size_t>(64,found.rays.size());++i) {
      const auto& ray=found.rays[i];text.append("\nRay ");text.number(i);text.append(": ");
      text.append(ray.kind==vdb::SdfRayHitKind::Miss?"miss":ray.kind==vdb::SdfRayHitKind::SampledZero?"sampled-zero":"bracketed-crossing");
      text.append("; normalizedDirection=");text.vector(ray.normalizedDirection);
      if(ray.kind!=vdb::SdfRayHitKind::Miss) {
        text.append("; distance=");text.number(ray.distance);text.append("; position=");text.vector(ray.position);
        text.append("; sampledResidual=");text.number(ray.sampledResidual);
      } else text.append("; distance/position/residual unavailable");
      text.append("; normalStatus=");text.append(rayUiNormal(ray.normalStatus));
      if(ray.normalStatus==vdb::RayNormalStatus::Available){text.append("; normal=");text.vector(ray.normal);}
      else text.append("; normal unavailable");
      text.append("; startSampleValid=");text.append(ray.startSampleValid?"true":"false");
      text.append("; startedInside=");text.append(!ray.startSampleValid?"unavailable":ray.startedInside?"true":"false");
    }
    if(found.rays.size()>64){text.append("\nAdditional completed ray results omitted from display: ");text.number(found.rays.size()-64);}
    for(const auto& diagnostic:found.diagnostics){text.append("\n");text.bytes(diagnostic);}
  } else {
    const auto found=vdb::QueryActiveVolumeRays(input,r.rayQueries,volume,r.inputGrid,processing);
    used=found.source.workItems;
    text.append("COARSE active-volume rays: ");text.number(found.rays.size());text.append(" queries; native spans=");text.number(found.totalSpanCount);
    text.append(". Whole8^3 active-leaf blocks plus active tiles with CENTERED cell footprints; not exact occupied cells, density integrals or signed solids. Inactive-only leaves excluded. Native intervals<=1e-9 index units omitted; no dilation. Displaying first64 rays/128 total spans.\n");
    text.source(found.source);size_t displayed=0;
    for(size_t i=0;i<std::min<size_t>(64,found.rays.size());++i) {
      const auto& ray=found.rays[i];text.append("\nRay ");text.number(i);text.append(": spans=");text.number(ray.spans.size());
      text.append("; normalizedDirection=");text.vector(ray.normalizedDirection);
      for(size_t j=0;j<ray.spans.size() && displayed<128;++j,++displayed) {
        const auto& span=ray.spans[j];text.append("\n  Span ");text.number(j);text.append(": entry=");text.number(span.entryDistance);text.append("; exit=");text.number(span.exitDistance);
        text.append("; entryPosition=");text.vector(span.entryPosition);text.append("; exitPosition=");text.vector(span.exitPosition);
      }
    }
    if(found.rays.size()>64){text.append("\nAdditional completed rays omitted from display: ");text.number(found.rays.size()-64);}
    if(found.totalSpanCount>displayed){text.append("\nAdditional completed spans omitted from display: ");text.number(found.totalSpanCount-displayed);}
    for(const auto& diagnostic:found.diagnostics){text.append("\n");text.bytes(diagnostic);}
  }
  text.append("\nRead-only report produced; source unchanged, no output/model. Retained UI bytes=");text.number(retained);
  text.append("; native work=");text.number(used);text.append("; joint native/UI work upper bound=");text.number(used+formatWork);
  if(text.truncated)result.message.append("\nAdditional report text omitted at the display limit; native computation was not truncated.");
  return result;
}


OpenVdbAuthoringResult executeValueComponents(const OpenVdbAuthoringRequest& r) {
  if(!vdb::IsAvailable())
    throw std::runtime_error("OpenVDB value components are unavailable; configure with -DENABLE_OPENVDB=ON.");
  const auto& submitted=r.valueComponents;
  if(submitted.gridNamePrefix.empty() || submitted.gridNamePrefix.size()>128 || submitted.gridNamePrefix.find('\0')!=std::string::npos)
    throw std::invalid_argument("Value-component UI name prefix requires1..128 native bytes without NUL.");
  const long double callerBytes=r.input.size()*1.0L+r.output.size()+r.inputGrid.size()+submitted.gridNamePrefix.size();
  if(callerBytes>256*1024 || submitted.maxInputBytes<=131072 || submitted.maxInputBytes>(16ull<<30) || !submitted.maxWork || submitted.maxWork>10'000'000'000ull)
    throw std::invalid_argument("Value-component UI report exceeds joint native/UI byte/work caps or caller bounds.");
  const long double expectedRetained=65536+1.0L+65536+32*callerBytes;
  if(expectedRetained>=submitted.maxInputBytes || 4*expectedRetained>=submitted.maxWork)
    throw std::invalid_argument("Value-component UI report exceeds joint native/UI byte/work caps before allocation.");
  auto settings=submitted;
  OpenVdbAuthoringResult result{true,{},r.output,false};result.message.reserve(65536);
  const long double retained=result.message.capacity()+1.0L+65536+32*callerBytes;
  const long double formattingWork=4*retained;
  if(retained>=settings.maxInputBytes || formattingWork>=settings.maxWork)
    throw std::invalid_argument("Value-component UI retained report exceeds joint native/UI byte/work caps.");
  settings.maxInputBytes-=uintmax_t(retained);settings.maxWork-=uint64_t(formattingWork);
  const auto found=vdb::WriteActiveValueComponents(path(r.input),path(r.output),settings,r.inputGrid);
  bool truncated=false;
  const auto append=[&](std::string_view text){if(text.size()>result.message.capacity()-result.message.size()-128){truncated=true;return;}result.message.append(text);};
  const auto number=[&](auto value){std::array<char,128> buffer;const auto converted=std::to_chars(buffer.data(),buffer.data()+buffer.size(),value);if(converted.ec==std::errc{})append({buffer.data(),size_t(converted.ptr-buffer.data())});else append("unavailable");};
  const auto vector=[&](const auto& values){append("[");for(size_t i=0;i<values.size();++i){if(i)append(", ");number(values[i]);}append("]");};
  append("Exported SOURCE FIELD with component ACTIVITY. Native type: ");append(found.valueType);
  append("; six-connected INDEX components: ");number(found.activeComponentCount);append("; output grids: ");number(found.grids.size());
  if(found.emptySourceCopy)append(". Empty activity writes one inactive source-field copy (_000000).");
  append("\nComplete source active voxels/tiles: ");number(found.sourceActiveVoxels);append("/");number(found.sourceActiveTiles);
  append("; source/expanded/aggregate output leaf slots: ");number(found.sourceStoredLeafVoxels);append("/");number(found.expandedStoredLeafVoxels);append("/");number(found.aggregateOutputLeafVoxels);
  append("; joint node upper bound: ");number(found.nodeUpperBound);
  append(".\nJoint native/UI workspace/work upper bounds: ");number(found.workspaceUpperBound+uintmax_t(retained));append("/");number(found.workUpperBound+uint64_t(formattingWork));
  append(". Grids ordered by component minimum lexicographic XYZ:");
  for(const auto& grid:found.grids){append("\n  ");append(grid.gridName);append(": ");number(grid.activeVoxelCount);append(" active voxels; ");number(grid.storedLeafVoxels);append(" stored leaf slots; INDEX bounds ");if(grid.activeBounds){vector(grid.activeBounds->minimum);append(" to ");vector(grid.activeBounds->maximum);}else append("empty");}
  append("\nAll non-Mask values/background/inactive continuations are unchanged, including values in other components. Mask has activity only. Active false/zero/nonfinite values connect; inactive values do not. Native maps, class/vector/world tags and custom binary grid/file metadata survive; only names/statistics and the exact five canonical boundary-projection provenance keys change.");
  append("\nThese are not isolated solids/SDFs, cavity certificates or physical density/mass partitions. Full source trees are duplicated and charged jointly. No automatic model preview. After Use output as input, explicitly choose an exact output grid selector from the names above; an empty selector is ambiguous for multiple grids. For inspection use a field slice with that explicit selector.");
  for(const auto& diagnostic:found.diagnostics){append("\n");append(diagnostic);}
  if(truncated)result.message.append("\nAdditional report text omitted at the bounded display limit.");
  static_assert(std::is_nothrow_move_constructible_v<OpenVdbAuthoringResult>);
  return result;
}

bool particleGeometrySdf(vdb::ParticleGeometryOutput mode) {
  return mode==vdb::ParticleGeometryOutput::LevelSet || mode==vdb::ParticleGeometryOutput::Both;
}
bool particleGeometryMask(vdb::ParticleGeometryOutput mode) {
  return mode==vdb::ParticleGeometryOutput::Mask || mode==vdb::ParticleGeometryOutput::Both;
}
std::string authoringDescriptorSelector(std::string_view raw) {
  std::string result("descriptor:");const auto separator=raw.find('\x1e');
  if(separator==std::string_view::npos)result.append(raw);
  else {result.append(raw.substr(0,separator));result.push_back('[');result.append(raw.substr(separator+1));result.push_back(']');}
  return result;
}

std::optional<OpenVdbParticleGeometryArtifact> particleGeometryArtifact(
    const OpenVdbAuthoringRequest& request) {
  const auto mode=request.particleGeometry.output;
  const auto validName=[](const std::string& name) {
    return !name.empty() && name.size()<=256 && name.find('\0')==std::string::npos;
  };
  // A malformed custom request gets no actionable artifact. Native validation
  // and OFF availability still execute in their original order.
  if(request.output.empty() || request.output.size()>256*1024 || request.output.find('\0')!=std::string::npos ||
     (!particleGeometrySdf(mode) && !particleGeometryMask(mode)) ||
     (particleGeometrySdf(mode) && !validName(request.processing.gridName)) ||
     (particleGeometryMask(mode) && !validName(request.particleGeometry.maskGridName)) ||
     (mode==vdb::ParticleGeometryOutput::Both && request.processing.gridName==request.particleGeometry.maskGridName))return {};
  OpenVdbParticleGeometryArtifact result;
  result.output=request.output;
  if(particleGeometrySdf(mode))result.sdfGrid=authoringDescriptorSelector(request.processing.gridName);
  if(particleGeometryMask(mode))result.maskGrid=authoringDescriptorSelector(request.particleGeometry.maskGridName);
  return result;
}

std::optional<OpenVdbBoundaryFluidArtifact> boundaryFluidArtifact(const OpenVdbAuthoringRequest& r) {
  if(r.output.empty() || r.output.size()>256*1024 || r.output.find('\0')!=std::string::npos)return {};
  if(r.boundaryFluid.initialVelocity!=vdb::BoundaryFluidInitialVelocity::TransportStored &&
     r.boundaryFluid.initialVelocity!=vdb::BoundaryFluidInitialVelocity::RequireSolenoidal)return {};
  const auto& n=r.boundaryFluid.names;
  const std::array<const std::string*,9> names{&n.velocity,&n.fluid,&n.faceKind,&n.faceFraction,
      &n.facePressure,&n.wallVelocity,&n.density,&n.temperature,&n.pressure};
  std::set<std::string_view> unique;
  for(const auto* name:names)
    if(name->empty() || name->size()>256 || name->find('\0')!=std::string::npos || !unique.insert(*name).second)return {};
  OpenVdbBoundaryFluidArtifact artifact;artifact.output=r.output;artifact.initialVelocity=r.boundaryFluid.initialVelocity;
  for(size_t i=0;i<artifact.grids.size();++i)artifact.grids[i]=authoringDescriptorSelector(*names[i]);
  return artifact;
}

OpenVdbAuthoringResult executeFogRayIntegrals(const OpenVdbAuthoringRequest& r) {
  if(!vdb::IsAvailable()) {
    (void)vdb::IntegrateFogRays({}, {},r.fogRays,{},r.processing);
    throw std::runtime_error("Fog ray integrals unavailable; configure ENABLE_OPENVDB=ON.");
  }
  const auto& options=r.fogRays;
  const long double caller=r.input.size()*1.0L+r.inputGrid.size();
  if(caller>1024*1024 || r.rayQueries.size()>1'000'000 || !options.limits.maxRays ||
     options.limits.maxRays>1'000'000 || r.rayQueries.size()>options.limits.maxRays ||
     !options.limits.maxWorkItems || options.limits.maxWorkItems>10'000'000'000ull)
    throw std::invalid_argument("Fog UI caller/ray/work limits are invalid or unbounded.");
  constexpr uint64_t messageCapacity=128*1024;
  const long double expected=messageCapacity+65536.0L+16*caller+
      2.0L*r.rayQueries.capacity()*sizeof(vdb::WorldRay)+sizeof(r);
  if(expected>=r.processing.maxInputBytes || 4*expected>=options.limits.maxWorkItems)
    throw std::invalid_argument("Fog UI report/rays/path reserve exceeds joint byte/work caps before allocation.");
  OpenVdbAuthoringResult result{true,{}, {},false};result.message.reserve(messageCapacity);
  const long double actual=result.message.capacity()+65536.0L+16*caller+
      2.0L*r.rayQueries.capacity()*sizeof(vdb::WorldRay)+sizeof(r);
  if(actual>=r.processing.maxInputBytes || 4*actual>=options.limits.maxWorkItems)
    throw std::invalid_argument("Fog UI actual retained report exceeds joint byte/work caps.");
  const auto retained=uintmax_t(std::ceil(actual));const uint64_t formatWork=4*uint64_t(retained);
  auto settings=options;settings.limits.maxWorkItems-=formatWork;
  // Copy only the generation-independent fields admitted by this read-only API.
  vdb::ProcessingOptions processing;processing.maxVoxelCount=r.processing.maxVoxelCount;
  processing.maxInputBytes=r.processing.maxInputBytes-retained;
  const auto input=path(r.input);
  RayUiText text{result.message};text.append("Fog ray density integrals from ");text.bytes(r.input);
  text.append("; selector ");text.bytes(r.inputGrid);text.append("; extinction=");text.number(options.extinction);
  text.append("; transmission policy=");text.append(options.transmissionPolicy==vdb::FogTransmissionPolicy::RejectUnderflow?"reject-underflow":"allow-opaque-underflow");
  const auto found=vdb::IntegrateFogRays(input,r.rayQueries,settings,r.inputGrid,processing);
  text.append("\nCompleted fog rays: ");text.number(found.rays.size());
  text.append("; actual/prospective cell intervals=");text.number(found.cellSegments);text.append("/");text.number(found.cellSegmentUpperBound);
  text.append("; background density=");text.number(found.backgroundDensity);
  text.append("; numerically constant=");text.append(found.numericallyConstant?"yes":"no");
  text.append("\nNodal trilinear values include inactive/background data: empty ACTIVITY is not vacuum. Stored non-background knot/tile support expands one index unit, with direct positive constant-background outer intervals. Native tiles are not expanded. Directions normalize and distances are Euclidean WORLD lengths. Exact rational cell events and Simpson products retain tiny independent complements; the represented native Double inverse and normalized direction remain approximate, without an ideal-inverse or integration-error certificate. Only uniform orthogonal affine maps are admitted.\n");
  text.source(found.source);
  text.append("\nCompleted native results; displaying first64 ray summaries:");
  for(size_t i=0;i<std::min<size_t>(64,found.rays.size());++i) {
    const auto& ray=found.rays[i];text.append("\nRay ");text.number(i);text.append(": I=");text.number(ray.densityIntegral);
    text.append("; tau=");text.number(ray.opticalDepth);text.append("; T=");text.number(ray.transmittance);
    text.append("; absorbed=");text.number(ray.absorbedFraction);text.append("; status=");
    text.append(ray.transmissionStatus==vdb::FogTransmissionStatus::Represented?"represented":"opaque-underflow");
    text.append("; cells=");text.number(ray.cellSegments);text.append("; normalized WORLD direction=");text.vector(ray.normalizedDirection);
  }
  if(found.rays.size()>64){text.append("\nAdditional COMPLETED ray results omitted from display: ");text.number(found.rays.size()-64);}
  text.append("\nJoint native/UI work upper bound=");text.number(found.workItems+formatWork);
  text.append("; UI retained bytes/format work=");text.number(retained);text.append("/");text.number(formatWork);
  text.append("\nI integrates density times WORLD length; tau=extinction*I. Thin absorption uses -expm1(-tau) even when T rounds to1. Nonzero final I/tau outside Double range rejects. Opaque-underflow explicitly marks finite tau whose exp rounds to0; it is not an exact-zero certificate. No scattering, emission, exact occupied-voxel spans or dense-preview parity. Native computation is complete or fails; bounded display prefixes do not truncate queries. Source bytes remain unchanged; no VDB output/model/restart is produced. Logical limits are not an RSS sandbox.");
  for(size_t i=0;i<std::min<size_t>(8,found.diagnostics.size());++i){text.append("\n");text.bytes(found.diagnostics[i]);}
  if(found.diagnostics.size()>8){text.append("\nAdditional native diagnostics omitted from display: ");text.number(found.diagnostics.size()-8);}
  if(text.truncated)result.message.append("\nAdditional report text omitted at the bounded display limit.");
  return result;
}

OpenVdbAuthoringResult executeBoundaryFluidStep(const OpenVdbAuthoringRequest& r) {
  if(!vdb::IsAvailable()) {
    (void)vdb::WriteBoundaryFluidStep({}, {},r.boundaryFluid);
    throw std::runtime_error("Boundary fluid unavailable; configure ENABLE_OPENVDB=ON.");
  }
  const auto& input=r.boundaryFluidInputs;const auto& o=r.boundaryFluid;const auto& names=o.names;
  const auto& b=input.boundary;
  long double caller=r.output.size()*1.0L+b.state.native().size()*4.0L+input.density.file.native().size()*4.0L+
      b.velocityGrid.size()+b.fluidGrid.size()+b.faceKindGrid.size()+b.faceFractionGrid.size()+
      b.facePressureGrid.size()+b.wallVelocityGrid.size()+input.density.grid.size();
  if(input.temperature)caller+=input.temperature->file.native().size()*4.0L+input.temperature->grid.size();
  for(const auto* name:{&names.velocity,&names.pressure,&names.fluid,&names.faceKind,&names.faceFraction,
                       &names.facePressure,&names.wallVelocity,&names.density,&names.temperature})caller+=name->size();
  constexpr uint64_t reportCapacity=128*1024;
  const long double expected=reportCapacity+65536.0L+32*caller+sizeof(r);
  if(caller>256*1024 || !o.maxInputBytes || o.maxInputBytes>(16ull<<30) ||
     !o.maxWork || o.maxWork>10'000'000'000ull || expected>=o.maxInputBytes || 4*expected>=o.maxWork)
    throw std::invalid_argument("Boundary fluid UI report/options exceed joint byte/work caps before allocation.");
  OpenVdbAuthoringResult result{true,{}, {},false};result.message.reserve(reportCapacity);
  const long double actual=result.message.capacity()+65536.0L+32*caller+sizeof(r);
  if(actual>=o.maxInputBytes || 4*actual>=o.maxWork)
    throw std::invalid_argument("Boundary fluid UI actual report/options reserve exceeds joint byte/work caps.");
  auto settings=o;settings.maxInputBytes-=uintmax_t(actual);settings.maxWork-=uint64_t(4*actual);
  const auto bindings=input;const auto output=path(r.output);
  RayUiText text{result.message};text.append("Fixed-boundary fluid output9: ");text.bytes(r.output);
  text.append("\nNames: ");
  for(const auto* name:{&names.velocity,&names.pressure,&names.fluid,&names.faceKind,&names.faceFraction,
                       &names.facePressure,&names.wallVelocity,&names.density,&names.temperature}){text.bytes(*name);text.append(" ");}
  text.append("\nRequested total seconds/substeps/CFL: ");text.number(o.timeStep);text.append("/");text.number(o.substeps);text.append("/");text.number(o.maxCfl);
  text.append("; initial velocity policy: ");text.append(o.initialVelocity==vdb::BoundaryFluidInitialVelocity::RequireSolenoidal?"require-solenoidal":"stored");
  text.append("; WORLD gravity/buoyancy direction: ");text.vector(o.gravity);text.append("/");text.vector(o.buoyancyDirection);
  const auto found=vdb::WriteBoundaryFluidStep(bindings,output,settings);
  const auto field=[&](const char* label,auto value){text.append("; ");text.append(label);text.append("=");if constexpr(std::is_same_v<decltype(value),bool>)text.append(value?"yes":"no");else text.number(value);};
  text.append("\nCompleted boundary fluid step");
#define F(x) field(#x,found.x)
  F(fluidCells);F(incidentFaces);F(pressureFaces);F(wallFaces);F(requestedSubsteps);F(actualSubsteps);
  F(attempts);F(rejectedAttempts);F(simulatedTime);F(timePartitionError);F(cellVolume);F(maxTransportCfl);
  F(maxEndpointCfl);F(maxConstantResponseDrift);F(maxDivergenceBefore);F(maxDivergenceAfter);
  F(divergenceRoundoffBound);F(pressureResidual);F(maxWorldSpeedBefore);F(maxWorldSpeedAfter);
  F(densityMassBefore);F(densityMassAfter);F(densityOutwardTransfer);F(temperatureVolumeBefore);
  F(temperatureVolumeAfter);F(temperatureOutwardTransfer);F(densityBalanceError);F(temperatureBalanceError);
  F(densityBalanceRoundoffBound);F(temperatureBalanceRoundoffBound);F(reservoirFaceApplications);
  F(outflowFaceApplications);F(roundedScalarRows);F(roundedMomentumRows);F(totalProjectionIterations);
  F(nodeUpperBound);F(roundoffLimited);F(diagnosticUnderflow);F(temperatureInitialized);F(initialDivergenceWithinTolerance);F(converged);
#undef F
  text.append("\nJoint native/UI workspace/work upper bounds: ");text.number(found.workspaceUpperBound+uintmax_t(actual));text.append("/");text.number(found.workUpperBound+uint64_t(4*actual));
  text.append("; UI retained bytes/format work: ");text.number(uintmax_t(actual));text.append("/");text.number(uint64_t(4*actual));
  text.append("\nAccepted actual dt sequence (all accepted steps):");
  for(const auto& step:found.steps) {
    text.append("\n dt=");text.number(step.timeStep);text.append("; transport/endpoint CFL=");text.number(step.transportCfl);text.append("/");text.number(step.endpointCfl);
    text.append("; constant drift/divergence/residual/iterations=");text.number(step.constantResponseDrift);text.append("/");text.number(step.maxDivergence);text.append("/");text.number(step.pressureResidual);text.append("/");text.number(step.projectionIterations);
  }
  text.append("\nFixed geometry and pressure ghost distances; full transport cells, stationary normal walls. First-order conservative scalar donor flux uses reservoir inflow and adjacent-cell outflow; stored divergence can change constants, without normalization or clamp. Default stored mode measures/transports the actual finite initial MAC, without cleanup projection or value erasure; require-solenoidal enforces the same stored-only entry AND successful output criterion. MAC advection is not conservative momentum/energy or tangential no-slip. Speed samples opposing-face cell-center averages, not a certified whole-field maximum. A fresh nonuniform prescribed-pressure impulse applies per accepted actual dt; discarded retry solves remain charged. Restart requires canonical six AND both scalar selectors from output9; old pressure is recomputed. No moving geometry, cut cells, viscosity, sources or evolving liquid phi. Source/existing destination bytes are preserved; logical caps are not an RSS sandbox.");
  for(size_t i=0;i<std::min<size_t>(8,found.diagnostics.size());++i){text.append("\n");text.bytes(found.diagnostics[i]);}
  if(text.truncated)result.message.append("\nAdditional report text omitted at the bounded display limit.");
  static_assert(std::is_nothrow_move_constructible_v<OpenVdbAuthoringResult>);
  return result;
}

OpenVdbAuthoringResult executeParticleGeometry(const OpenVdbAuthoringRequest& r) {
  if(!vdb::IsAvailable()) {
    (void)vdb::WriteParticleGeometryFile({}, {}, r.particleGeometry, r.processing);
    throw std::runtime_error("Particle geometry unavailable; configure ENABLE_OPENVDB=ON.");
  }
  const auto& requested=r.particleGeometry;
  const bool sdf=particleGeometrySdf(requested.output),mask=particleGeometryMask(requested.output);
  if(!sdf && !mask)throw std::invalid_argument("Unknown particle geometry UI output mode.");
  const long double caller=r.input.size()*1.0L+r.output.size()+
      (sdf?r.processing.gridName.size():0)+(mask?requested.maskGridName.size():0);
  constexpr uint64_t reportBytes=65536;
  if(caller>256*1024 || !r.processing.maxInputBytes || r.processing.maxInputBytes>(16ull<<30) ||
     !requested.maxWork || requested.maxWork>10'000'000'000ull)
    throw std::invalid_argument("Particle geometry UI caller/byte/work caps are invalid.");
  const long double expected=reportBytes+65536.0L+32*caller+sizeof(r);
  if(expected>=r.processing.maxInputBytes || 4*expected>=requested.maxWork)
    throw std::invalid_argument("Particle geometry UI report exceeds joint byte/work caps before allocation.");
  OpenVdbAuthoringResult result{true,{}, {},false};
  result.message.reserve(reportBytes);
  const long double actual=result.message.capacity()+65536.0L+32*caller+sizeof(r);
  if(actual>=r.processing.maxInputBytes || 4*actual>=requested.maxWork)
    throw std::invalid_argument("Particle geometry UI actual report capacity exceeds joint byte/work caps.");
  const auto retained=uintmax_t(std::ceil(actual));
  const auto formattingWork=uint64_t(std::ceil(4*actual));
  vdb::ProcessingOptions processing;
  processing.voxelSize=r.processing.voxelSize;
  processing.maxVoxelCount=r.processing.maxVoxelCount;
  processing.maxInputBytes=r.processing.maxInputBytes-retained;
  if(sdf){processing.gridName=r.processing.gridName;processing.halfWidth=r.processing.halfWidth;}
  // Do not copy an ignored, arbitrarily large hidden name.
  vdb::ParticleGeometryOptions settings;
  settings.output=requested.output;settings.timeStep=requested.timeStep;settings.spacing=requested.spacing;
  settings.minRadiusVoxels=requested.minRadiusVoxels;settings.maxRadiusVoxels=requested.maxRadiusVoxels;
  settings.originWorld=requested.originWorld;
  if(mask)settings.maskGridName=requested.maskGridName;
  settings.maxParticles=requested.maxParticles;settings.maxTrailInstances=requested.maxTrailInstances;
  settings.maxTrailLengthVoxels=requested.maxTrailLengthVoxels;
  settings.maxNodes=requested.maxNodes;settings.maxWork=requested.maxWork-formattingWork;
  settings.maxMetadataBytes=requested.maxMetadataBytes;settings.maxOutputBytes=requested.maxOutputBytes;
  const auto input=path(r.input),output=path(r.output);
  RayUiText text{result.message};
  text.append("Particle geometry VDB output ");text.bytes(r.output);
  text.append("; strict version1 JSON source ");text.bytes(r.input);
  text.append(". Sampled spheres taper OPPOSITE velocity*timeStep. The last stamp need not reach the requested endpoint; this is not a continuous capsule, conserved mass raster or ODE trajectory. Mask activity tests voxel CENTERS strictly inside stamps, not cell intersections or SDF-band activity. No source IDs/attributes are generated.\n");
  const auto found=vdb::WriteParticleGeometryFile(input,output,settings,processing);
  text.append("Particles/sphere-heads/trail-heads: ");text.number(found.particleCount);text.append("/");text.number(found.sphereParticleCount);text.append("/");text.number(found.trailParticleCount);
  text.append("; actual stamps/instance upper bound: ");text.number(found.sphereInstances);text.append("/");text.number(found.instanceUpperBound);
  text.append(".\nWORLD lattice origin: ");text.vector(found.originWorld);
  text.append("; maximum trail length WORLD: ");text.number(found.maximumTrailLengthWorld);
  text.append("; maximum conditioning error in voxels: ");text.number(found.maximumPositionErrorVoxels);
  if(found.levelSet){text.append("\nFloat WORLD-distance SDF ");text.bytes(found.levelSet->gridName);text.append(": ");text.number(found.levelSet->activeVoxelCount);text.append(" active narrow-band voxels; exterior band WORLD: ");text.number(found.effectiveExteriorBand);}
  if(found.mask){text.append("\nNative MaskGrid (GRID_UNKNOWN) ");text.bytes(found.mask->gridName);text.append(": ");text.number(found.mask->activeVoxelCount);text.append(" occupied voxel centers including interior; not SDF activity.");}
  text.append("\nCandidate cells/stored leaf slots/nodes upper bounds: ");text.number(found.candidateVoxelUpperBound);text.append("/");text.number(found.storedLeafVoxelUpperBound);text.append("/");text.number(found.nodeUpperBound);
  text.append(". Joint native/UI workspace/work upper bounds: ");text.number(found.workspaceUpperBound+retained);text.append("/");text.number(found.workUpperBound+formattingWork);
  text.append("; UI report/path/options bytes/work reserved: ");text.number(retained);text.append("/");text.number(formattingWork);
  text.append(".\nNative singular-center candidates/pair bound/repaired values: ");text.number(found.centerCandidateCount);text.append("/");text.number(found.centerRepairPairUpperBound);text.append("/");text.number(found.repairedNativeCenterValues);
  text.append("; rounded Float distances: ");text.number(found.roundedDistanceValues);text.append("; maximum absolute WORLD-distance error: ");text.number(found.maximumDistanceAbsoluteError);
  text.append(". All source and existing destination bytes are preserved. Inspect each selected grid explicitly; only an SDF can use surface preview. This output is not a fluid restart.\n");
  for(const auto& diagnostic:found.diagnostics){text.append("\n");text.bytes(diagnostic);}
  if(text.truncated)result.message.append("\nAdditional report text omitted at the display limit.");
  result.modelOutput=found.levelSet && found.levelSet->activeVoxelCount!=0;
  static_assert(std::is_nothrow_move_constructible_v<OpenVdbAuthoringResult>);
  return result;
}

OpenVdbAuthoringResult executeSurfaceReference(const OpenVdbAuthoringRequest& r) {
  if(!vdb::IsAvailable()) {
    (void)vdb::RenderSurfaceReference({},r.surfaceCamera,r.surfaceReference,{},r.processing);
    throw std::runtime_error("Surface reference images unavailable; configure ENABLE_OPENVDB=ON.");
  }
  const auto& requested=r.surfaceReference;
  const uint64_t pixels=uint64_t(requested.width)*requested.height;
  const long double caller=r.input.size()*1.0L+r.output.size()+r.inputGrid.size();
  const uint64_t effective=std::min<uint64_t>(r.processing.maxInputBytes,r.surfaceExportBytes);
  if(caller>1024*1024 || !requested.width || !requested.height || requested.width>2048 || requested.height>2048 ||
     pixels>262144 || pixels>requested.maxPixels || !requested.rays.limits.maxRays || pixels>requested.rays.limits.maxRays ||
     !requested.rays.limits.maxWorkItems || requested.rays.limits.maxWorkItems>10'000'000'000ull ||
     !r.processing.maxInputBytes || r.processing.maxInputBytes>(16ull<<30) || !r.surfaceExportBytes || r.surfaceExportBytes>(16ull<<30))
    throw std::invalid_argument("Surface reference UI options/caller/pixel/shared caps are invalid.");
  constexpr uint64_t messageBytes=128*1024;
  const long double expected=messageBytes+65536+32*caller+sizeof(r);
  const long double expectedWork=4*expected+32.0L*pixels;
  if(expected>=effective || expectedWork>=requested.rays.limits.maxWorkItems)
    throw std::invalid_argument("Surface reference UI report exceeds joint byte/export/work caps.");
  // This artifact is an EXR, never a VDB model/restart binding. Its byte-escaped
  // path is included in the report; keeping generic output empty also protects
  // the result if the user later browses a different operation draft.
  OpenVdbAuthoringResult result{true,{}, {},false};result.message.reserve(messageBytes);
  const long double retained=result.message.capacity()+65536.0L+32*caller+sizeof(r);
  const long double actualWork=4*retained+32.0L*pixels;
  if(retained>=effective || actualWork>=requested.rays.limits.maxWorkItems)
    throw std::invalid_argument("Surface reference UI actual report capacity exceeds joint limits.");
  const auto bytes=uint64_t(std::ceil(retained)),formatWork=uint64_t(std::ceil(actualWork));
  vdb::ProcessingOptions processing;processing.maxVoxelCount=r.processing.maxVoxelCount;
  processing.maxInputBytes=r.processing.maxInputBytes-bytes;
  auto settings=requested;settings.rays.limits.maxWorkItems-=formatWork;
  if(settings.shading==vdb::SurfaceShading::Matte)
    settings.invalidNormalColor=vdb::SurfaceReferenceOptions{}.invalidNormalColor;
  auto camera=r.surfaceCamera;
  if(camera.projection==vdb::SurfaceProjection::Perspective)camera.orthographicHeight=vdb::SurfaceReferenceCamera{}.orthographicHeight;
  else camera.verticalFovDegrees=vdb::SurfaceReferenceCamera{}.verticalFovDegrees;
  const auto input=path(r.input),output=path(r.output);
  const auto image=vdb::RenderSurfaceReference(input,camera,settings,r.inputGrid,processing);
  RayUiText text{result.message};
  text.append("EXR output ");text.bytes(r.output);text.append(". Native SDF surface reference: ");text.number(image.width);text.append("x");text.number(image.height);
  text.append(" deterministic center pixels; 11 channels, uncompressed linear Float. RGB is native Float shader response, not physical radiance. A is hit coverage; Z is radial WORLD depth; N.X/Y/Z are WORLD normals, N.VALID and N.STATUS distinguish unrequested/miss/available/missing/degenerate; HIT.KIND retains sampled-zero/bracketed status. Zero AOV placeholders require status/masks. No anti-aliasing, shadows, fog transport or uncertainty estimate.\n");
  text.append("Projection: ");text.append(camera.projection==vdb::SurfaceProjection::Perspective?"perspective":"orthographic");text.append("; shader: ");text.append(settings.shading==vdb::SurfaceShading::Matte?"matte":settings.shading==vdb::SurfaceShading::WorldNormal?"WORLD normal":"two-sided headlight abs(N dot rayD)");
  text.append(". Native HDDA/chord is approximate: tangencies/multiple roots may be missed; sampled-zero includes signed zero and plateaus, not an isolated-surface certificate.\n");
  text.source(image.geometry.source);
  text.append("\nHits/misses/unavailable normals: ");text.number(image.hitCount);text.append("/");text.number(pixels-image.hitCount);text.append("/");text.number(image.unavailableNormalCount);
  std::array<uint64_t,5> statuses{};
  for(const auto& hit:image.geometry.rays) {
    const auto status=unsigned(hit.normalStatus);if(status>=statuses.size())throw std::runtime_error("Unknown native surface normal status.");++statuses[status];
  }
  text.append(". Normal status counts [not-requested,no-hit,available,missing-active-footprint,degenerate-gradient]: ");
  for(size_t i=0;i<statuses.size();++i){if(i)text.append(",");text.number(statuses[i]);}
  for(size_t i=0;i<std::min<size_t>(16,image.geometry.rays.size());++i) {
    const auto& hit=image.geometry.rays[i];text.append("\nPixel ");text.number(i);text.append(": ");
    text.append(hit.kind==vdb::SdfRayHitKind::Miss?"miss":hit.kind==vdb::SdfRayHitKind::SampledZero?"sampled-zero":"bracketed-crossing");
    if(hit.kind!=vdb::SdfRayHitKind::Miss){text.append("; depth=");text.number(hit.distance);text.append("; worldPosition=");text.vector(hit.position);text.append("; sampledResidual=");text.number(hit.sampledResidual);}else text.append("; depth/position/residual unavailable");
    text.append("; normalStatus=");text.append(rayUiNormal(hit.normalStatus));
    if(hit.normalStatus==vdb::RayNormalStatus::Available){text.append("; normal=");text.vector(hit.normal);}else text.append("; normal unavailable");
    text.append("; RGBA=");text.vector(image.rgba[i]);
  }
  if(pixels>16){text.append("\nAdditional computed pixels omitted from display: ");text.number(pixels-16);}
  text.append("\nNative/image work: ");text.number(image.work);text.append("; UI formatting/status-scan work: ");text.number(formatWork);text.append("; combined: ");text.number(image.work+formatWork);text.append("; retained UI bytes: ");text.number(bytes);
  text.append(". Rounded shader values: ");text.number(image.roundedColorValues);text.append("; maximum absolute shader error: ");text.number(image.maximumColorAbsoluteError);
  if(result.message.capacity()-result.message.size()<4096)
    throw std::runtime_error("Surface UI report capacity invariant failed before EXR publication.");
  const auto saved=vdb::WriteSurfaceReferenceExr(output,image,effective-bytes);
  // Fixed-size tail after publication: stack formatting only, no DOM/path copy.
  text.append("\nEncoded EXR bytes: ");text.number(saved.encodedBytes);text.append("; rounded export values: ");text.number(saved.roundedValues);text.append("; maximum absolute/relative export error: ");text.number(saved.maximumAbsoluteError);text.append("/");text.number(saved.maximumRelativeError);text.append(". Source unchanged; EXR is not a VDB preview/restart input.");
  if(text.truncated)result.message.append("\nAdditional report text omitted; native image was computed completely.");
  return result;
}

OpenVdbAuthoringResult execute(const OpenVdbAuthoringRequest& r) {
  if(r.operation==Operation::IntegrateFogRays)return executeFogRayIntegrals(r);
  if(r.operation==Operation::BoundaryFluidStep)return executeBoundaryFluidStep(r);
  if(r.operation==Operation::ParticleGeometry)return executeParticleGeometry(r);
  if(r.operation==Operation::RenderSurfaceReference)return executeSurfaceReference(r);
  if(r.operation==Operation::ValueComponents)return executeValueComponents(r);
  if(r.operation==Operation::RayIntersections || r.operation==Operation::RayVolumeSpans)
    return executeRayQueries(r,r.operation==Operation::RayIntersections);
  if(r.operation==Operation::QueryActiveRegions) {
    if(!vdb::IsAvailable())throw std::runtime_error("OpenVDB active region queries are unavailable; configure with -DENABLE_OPENVDB=ON.");
    auto settings=r.activeRegionOptions;
    const long double callerBytes=r.input.size()*1.0L+r.inputGrid.size()+r.activeRegions.size()*sizeof(vdb::IndexRegion);
    if(callerBytes>256*1024 || r.activeRegions.size()>4096 || settings.maxInputBytes>(16ull<<30) || !settings.maxWork || settings.maxWork>10'000'000'000ull)
      throw std::invalid_argument("Active region UI caller/query/report work limits exceed supported bounds.");
    OpenVdbAuthoringResult result{true,{}, {},false};result.message.reserve(131072);
    const long double retained=result.message.capacity()+1.0L+65536+32*callerBytes;
    const long double formattingWork=4*retained;
    if(retained>=settings.maxInputBytes || formattingWork>=settings.maxWork)
      throw std::invalid_argument("Active region UI retained report exceeds joint native/UI byte/work caps.");
    settings.maxInputBytes-=uintmax_t(retained);settings.maxWork-=uint64_t(formattingWork);
    const auto found=vdb::QueryActiveRegions(path(r.input),r.activeRegions,settings,r.inputGrid);
    bool truncated=false;
    const auto append=[&](std::string_view text){if(text.size()>result.message.capacity()-result.message.size()-128){truncated=true;return;}result.message.append(text);};
    const auto number=[&](auto value){std::array<char,128> buffer;const auto converted=std::to_chars(buffer.data(),buffer.data()+buffer.size(),value);if(converted.ec==std::errc{})append({buffer.data(),size_t(converted.ptr-buffer.data())});else append("unavailable");};
    const auto nativeBits=[&](auto value){constexpr char hex[]="0123456789abcdef";std::array<char,sizeof(value)*2> digits;for(size_t i=0;i<digits.size();++i)digits[i]=hex[(value>>((digits.size()-i-1)*4))&15];append("bits 0x");append({digits.data(),digits.size()});};
    const auto components=[&](const auto& values){append("[");for(size_t i=0;i<values.size();++i){if(i)append(", ");number(values[i]);}append("]");};
    const auto value=[&](const vdb::RegionNativeValue& native){std::visit([&](const auto& item){using T=std::decay_t<decltype(item)>;
      if constexpr(std::is_same_v<T,vdb::RegionFloat32Bits>||std::is_same_v<T,vdb::RegionFloat64Bits>)nativeBits(item.bits);
      else if constexpr(std::is_same_v<T,std::array<vdb::RegionFloat32Bits,3>>||std::is_same_v<T,std::array<vdb::RegionFloat64Bits,3>>){append("[");for(size_t i=0;i<3;++i){if(i)append(", ");nativeBits(item[i].bits);}append("]");}
      else if constexpr(std::is_same_v<T,std::array<int32_t,3>>)components(item);
      else if constexpr(std::is_same_v<T,bool>)append(item?"true":"false");else number(item);},native);};
    constexpr const char* nativeTypes[]={"Bool","Mask","Int32","Int64","Float","Double","Vec3I","Vec3S","Vec3D"};
    append("Native type: ");append(nativeTypes[size_t(found.nativeType)]);append(". Complete source census: ");number(found.sourceActiveVoxels);append(" active voxels in ");number(found.sourceActiveRecords);append(" native records; ");number(found.sourceActiveTiles);append(" compact active tiles representing ");number(found.sourceActiveTileVoxels);append(" voxels.");
    append("\nComplete counts for ");number(found.queries.size());append(" inclusive INDEX queries. Stored leaf slots/root entries: ");number(found.storedLeafVoxels);append("/");number(found.sourceRootEntries);
    append(". Joint native/UI workspace/work upper bounds: ");number(found.workspaceUpperBound+uintmax_t(retained));append("/");number(found.workUpperBound+uint64_t(formattingWork));
    size_t displayedRecords=0;
    for(size_t i=0;i<std::min<size_t>(32,found.queries.size());++i){const auto& q=found.queries[i];append("\nQuery ");number(i+1);append(": matched voxels/records ");number(q.matchedVoxelCount);append("/");number(q.matchedRecordCount);append("; omitted by native quota ");number(q.omittedVoxelCount);append("/");number(q.omittedRecordCount);append(q.recordsTruncated?"; native records truncated.":"; native record coverage complete.");
      for(const auto& record:q.records){if(displayedRecords>=32){truncated=true;break;}++displayedRecords;append("\n  ");components(record.clipped.minimum);append(" to ");components(record.clipped.maximum);append("; multiplicity ");number(record.voxelCount);append(record.tile?"; tile level ":"; voxel level ");number(record.sourceLevel);append("; value ");value(record.value);}
    }
    truncated|=found.queries.size()>32;
    append("\nActivity is independent of values: active false/zero/nonfinite values count; inactive continuations do not. Native integers remain exact; Float values use raw bit hex. Vector components are raw with no semantic transform. No world-region conversion or geometry certificate. Full metadata/map bytes are available through the native API/CLI.");
    if(truncated)result.message.append("\nDisplay limited to 32 queries/32 records or bounded text; native matched counts remain complete.");
    return result;
  }
  if(r.operation==Operation::AdvectNativePoints)return executeNativePointAdvection(r);
  if(r.operation==Operation::NanoPrimitive)return executeNativePrimitive(r);
  const auto input = path(r.input), second = path(r.secondInput), output = path(r.output);
  if (!vdb::IsAvailable() && r.operation!=Operation::RenderTransportImage && !nativeFileOperation(r.operation))
    throw std::runtime_error("OpenVDB tools are unavailable in this build.");
  OpenVdbAuthoringResult result{true, {}, r.output, true};
  vdb::ProcessingResult grid;
  switch (r.operation) {
  case Operation::NanoStatistics: {
    auto read=r.nanoRead; read.inputGrid=r.inputGrid;
    const auto stats=vdb::WriteNanoVdbStatistics(input,output,r.nanoStatisticsMode,read,r.nanoWrite,r.nanoLimits);
    std::ostringstream text;
    text << std::setprecision(17) << "Recomputed native "
         << (r.nanoStatisticsMode==vdb::NanoVdbStatisticsMode::Bounds ? "bounds" :
             r.nanoStatisticsMode==vdb::NanoVdbStatisticsMode::MinMax ? "bounds and extrema" : "bounds, extrema and moments")
         << ": " << stats.summaries.size() << " grids; " << stats.work << " work items.";
    const auto value=[&](const vdb::NanoVdbStatisticValue& v) {
      std::visit([&](const auto& item) {
        using T=std::decay_t<decltype(item)>;
        if constexpr (std::is_arithmetic_v<T>) text << item;
        else text << '[' << item[0] << ',' << item[1] << ',' << item[2] << ']';
      },v);
    };
    for (const auto& summary:stats.summaries) {
      text << '\n' << summary.name << ": " << summary.activeVoxels << " active voxels";
      if (summary.minimum) { text << "; minimum "; value(*summary.minimum); }
      if (summary.maximum) { text << "; maximum "; value(*summary.maximum); }
      if (summary.average) text << "; native cache average " << *summary.average;
      if (summary.standardDeviation) text << "; native cache deviation " << *summary.standardDeviation;
    }
    for (const auto& diagnostic:stats.diagnostics) text << '\n' << diagnostic;
    result.message=text.str(); result.modelOutput=false; return result;
  }
  case Operation::NanoInspect: case Operation::NanoValidate: case Operation::NanoCopy:
  case Operation::VdbToNano: case Operation::NanoToVdb: case Operation::NanoChecksum: {
    auto read=r.nanoRead; read.inputGrid=r.inputGrid;
    auto limits=r.nanoLimits;
    vdb::NanoVdbFileInfo report;
    if (r.operation==Operation::NanoInspect) report=vdb::InspectNanoVdbFile(input,limits);
    else if (r.operation==Operation::NanoValidate) report=vdb::ValidateNanoVdbFile(input,read,limits);
    else if (r.operation==Operation::VdbToNano) report=vdb::WriteNanoVdbFromVdb(input,output,r.inputGrid,r.nanoWrite,limits);
    else if (r.operation==Operation::NanoToVdb) report=vdb::WriteVdbFromNanoVdb(input,output,read,r.nanoAllowMetadataLoss,limits);
    else if (r.operation==Operation::NanoChecksum) report=vdb::RewriteNanoVdbChecksums(input,output,r.nanoWrite.checksum,limits);
    else {
      const auto archive=vdb::ReadNanoVdbFile(input,read,limits);
      const auto previous=archive.info().work;
      if (previous>=limits.maxWork) throw std::invalid_argument("NanoVDB copy exhausted aggregate read/write work budget");
      limits.maxWork-=previous;
      report=vdb::WriteNanoVdbFile(archive,output,r.nanoWrite,limits);
      report.work+=previous;
    }
    std::ostringstream text;
    text << report.grids.size() << " native grids; " << report.nativeBytes << " native bytes; "
         << report.encodedBytes << " encoded bytes; " << report.work << " work items.";
    for (const auto& item:report.grids) {
      text << '\n' << item.index << ": " << item.name << " (" << item.valueType << ", "
           << item.activeVoxels << " active voxels); structure "
           << (item.structureValidated ? "validated" : "not validated") << "; CRC "
           << (item.checksumVerified ? "verified" : "not verified") << "; full data coverage "
           << (item.checksumCoversAllData ? "yes" : "no") << '.';
    }
    for (const auto& diagnostic:report.diagnostics) text << '\n' << diagnostic;
    if (r.operation==Operation::NanoInspect || r.operation==Operation::NanoValidate) result.output.clear();
    result.modelOutput=false; result.message=text.str(); return result;
  }
  case Operation::ProjectBoundaries: case Operation::ReprojectBoundaries: {
    auto settings=r.boundaryProjection;
    const bool replay=r.operation==Operation::ReprojectBoundaries;
    if(replay) {
      settings.missing=vdb::MissingFluidBoundary::ClosedWall;
      settings.airPressure=vdb::AirPressureLocation::NeighborCenter;settings.minimumInterfaceFraction=.01;
      settings.wallVelocityWorld={0,0,0};settings.ambientPressure=0;
    }
    const auto projected=replay?vdb::WriteReprojectedBoundaryState(r.boundaryReplay,output,settings,r.processing)
        :vdb::WriteBoundaryProjectedVelocity(r.boundaryInputs,output,settings,r.processing);
    const auto& solved=projected.projection;
    std::ostringstream text;text << std::setprecision(17);
    text << "Boundary projection " << (replay?"replay":"direct solve") << " completed; converged: " << (solved.converged?"yes":"NO")
         << ".\n" << solved.fluidCellCount << " fluid cells; " << solved.faceCount << " fluid-fluid faces; " << solved.componentCount
         << " components (Dirichlet " << projected.dirichletComponents << ", Neumann " << projected.neumannComponents << ").\n"
         << "Maximum/total iterations " << solved.iterations << '/' << projected.totalIterations
         << "; full normalized stored-pressure residual " << solved.residual << "; divergence before/after (1/sec) " << solved.maxDivergenceBefore << '/' << solved.maxDivergenceAfter
         << "; local divergence roundoff bound (1/sec) " << solved.divergenceRoundoffBound << "; " << projected.roundoffLimitedCells << " roundoff-limited cells.\n"
         << "Faces FF/wall/open-face/air-center/free-surface " << projected.fluidFluidFaces << '/' << projected.wallFaces << '/' << projected.openFacePressureFaces
         << '/' << projected.airCellPressureFaces << '/' << projected.freeSurfacePressureFaces << "; effective minimum fraction " << projected.minimumEffectiveFraction
         << "; " << projected.clampedInterfaceFaces << " clamped interfaces, maximum clamp " << projected.maximumFractionClamp << ".\n"
         << "Closed normal flux (voxel volumes/sec)/wall normal error (voxels/sec) " << projected.maxClosedNormalFlux << '/' << projected.maxWallNormalError << "; " << projected.workUpperBound << " bounded work items.\n"
         << "Double local MAC velocity " << solved.velocityGridName << "; Double physical pressure " << solved.pressureGridName;
    if(settings.projection.writeDomain)text << "; Bool fluid " << solved.domainGridName;
    if(projected.boundaryStateWritten)text << ". Canonical boundary channels: " << settings.faceKindGridName << ", " << settings.faceFractionGridName << ", " << settings.facePressureGridName << ", " << settings.wallVelocityGridName;
    else text << ". Boundary channels omitted: this output is not a self-contained replay bundle";
    text << ".\nWORLD tags supply raw physical XYZ velocity components; native MAC is local negative-face velocity in voxels/second. Pressure is unit-density world-distance squared/second squared.\n"
         << "Replay recomputes old pressure and applies a fresh nonuniform pressure-driven impulse on every call; it is not generally idempotent. This fixed-domain projection is not an open/free-surface simulation timestep or a five-grid M05 restart.";
    for(const auto& diagnostic:projected.diagnostics)text << '\n' << diagnostic;
    result.message=text.str();result.modelOutput=false;return result;
  }
  case Operation::Diagnose: case Operation::DiagnosticMask: {
    auto options=r.diagnostics;
    vdb::PointAttributeOptions limits;
    if(options.profile==vdb::DiagnosticProfile::PointData) {
      options.pointGroups=r.pointSelection;limits=r.pointAttributes;limits.maxPoints=r.maxPoints;
      limits.maxEdits=vdb::PointAttributeOptions{}.maxEdits; // No editing occurs here.
    }
    const auto report=r.operation==Operation::DiagnosticMask
        ?vdb::WriteVdbDiagnosticMask(input,output,options,r.inputGrid,r.processing,limits)
        :vdb::DiagnoseVdb(input,options,r.inputGrid,r.processing,limits);
    const auto status=[](vdb::DiagnosticStatus value) {
      switch(value) {
      case vdb::DiagnosticStatus::Pass:return "Pass";
      case vdb::DiagnosticStatus::Fail:return "Fail";
      case vdb::DiagnosticStatus::Unavailable:return "Unavailable";
      case vdb::DiagnosticStatus::NotApplicable:return "NotApplicable";
      }
      return "Unknown";
    };
    std::ostringstream text;text << std::setprecision(17);
    text << "Diagnostics job completed; all requested checks passed: " << (report.allRequestedPassed?"yes":"NO")
         << "; coverage " << (report.complete?"complete":"INCOMPLETE")
         << "; unlocated failures: " << (report.unlocatedFailures?"YES":"no") << ".\n"
         << report.gridName << " (" << report.valueType << ", " << report.gridClass << ", " << report.vectorType
         << "); " << report.activeVoxelCount << " active voxels; " << report.leafCount << " stored leaves / "
         << report.storedLeafSlots << " leaf slots; " << report.activeTileCount << " active and "
         << report.inactiveTileCount << " inactive tiles.\nDecoded bytes: " << report.decodedBytes
         << "; bounded work: " << report.workUpperBound << ". Unique finite failure locations: " << report.maskVoxelCount << '.';
    if(report.points) {
      const auto& points=*report.points;
      text << "\nPoint counts valid: " << (points.countsValid?"yes":"NO; safely validated leaves only")
           << "; total " << points.total << "; selected " << points.selected << " (active " << points.selectedActive
           << ", inactive " << points.selectedInactive << "); archived schema: " << (points.archivedSchema?"yes":"no") << '.';
      text << "\nChecked numeric point attributes:";
      for(const auto& name:points.checkedAttributes)text << ' ' << name;
    }
    for(const auto& check:report.checks) {
      text << "\n" << check.id << ": " << status(check.status) << "; coverage " << (check.coverageComplete?"complete":"INCOMPLETE")
           << "; " << check.description << ".\n  Examined leaf voxels/tiles/points " << check.examinedLeafVoxels << '/' << check.examinedTiles << '/' << check.examinedPoints
           << "; failed leaf voxels/tiles/tile-volume/points " << check.failedLeafVoxels << '/' << check.failedTiles << '/' << check.failedTileVoxelVolume << '/' << check.failedPoints
           << "; background inspected/failed " << (check.backgroundInspected?"yes":"no") << '/' << (check.backgroundFailed?"YES":"no")
           << ".\n  Footprint candidates/evaluated/missing/nonfinite/stored-continuations " << check.candidateCount << '/' << check.evaluatedCount << '/' << check.missingFootprintCount
           << '/' << check.nonfiniteFootprintCount << '/' << check.inactiveContinuationCount << '.';
      for(const auto& example:check.examples) {
        text << "\n  Example";
        if(example.index)text << " index[" << (*example.index)[0] << ',' << (*example.index)[1] << ',' << (*example.index)[2] << ']';
        if(example.pointOrdinal)text << " point ordinal " << *example.pointOrdinal;
        if(!example.index && !example.pointOrdinal)text << " (unlocated/global)";
        text << ": " << example.detail;
      }
    }
    if(report.mask)text << "\nWrote Bool union failure mask " << report.mask->gridName << ": " << report.mask->activeVoxelCount << " marked source locations. Background/schema/global failures have no finite location; an empty mask does NOT mean the report passed.";
    else text << "\nRead-only report; no output or repair. Finite failure counts are reported without writing a mask.";
    text << "\nPass means only the requested checks passed at their reported coverage; no distance, closure or validity certificate is implied.";
    for(const auto& diagnostic:report.diagnostics)text << '\n' << diagnostic;
    result.message=text.str();result.modelOutput=false;
    if(r.operation==Operation::Diagnose)result.output.clear();
    return result;
  }
  case Operation::ResampleField: {
    const auto sampled=vdb::WriteResampledField(input,output,r.fieldResampling,r.inputGrid,r.processing);
    std::ostringstream text;text << std::setprecision(17);
    text << "Resampled ordinary " << sampled.valueType << " field " << sampled.grid.gridName
         << ": " << sampled.grid.activeVoxelCount << " active voxels; " << sampled.sampledVoxelCount
         << " sampled cells (" << sampled.inactiveSampleCount << " inactive); "
         << sampled.inputStoredLeafCount << " source and " << sampled.outputStoredLeafCount << " output leaves.\n"
         << (sampled.exactCopy?"Exact identity topology/value/activity copy.":"Single-pass center sampling over bounded finite support.")
         << "\n" << sampled.roundedFloatComponents << " Float components rounded; maximum absolute error "
         << sampled.maximumFloatAbsoluteError << "; " << sampled.workUpperBound << " bounded work items.";
    for(const auto& diagnostic:sampled.diagnostics)text << '\n' << diagnostic;
    result.message=text.str();result.modelOutput=false;return result;
  }
  case Operation::TransferPointAttribute: {
    auto options=r.pointTransfer;options.groups=r.pointSelection;
    auto limits=r.pointAttributes;limits.maxPoints=r.maxPoints;
    const auto transferred=vdb::WriteTransferredPointAttribute(input,output,r.pointField,options,r.inputGrid,r.processing,limits);
    std::ostringstream text;text << std::setprecision(17);
    text << "Transferred " << transferred.selectedCount << " of " << transferred.inputCount << " native points ("
         << transferred.selectedInactiveCount << " selected inactive records) into " << transferred.field.gridName
         << " and " << transferred.companion.gridName << ".\n" << transferred.componentCount << " components; "
         << transferred.contributionCount << " kernel contributions; world voxel volume " << transferred.voxelVolume
         << ".\nSource/deposited mass: " << transferred.totalMass << " / " << transferred.depositedMass
         << "; companion rounding mass-error bound " << transferred.massErrorUpperBound << ".\n"
         << transferred.roundedIntegerComponents << " integer conversions rounded; " << transferred.roundedFloatComponents
         << " Float components rounded, maximum absolute error " << transferred.maxFloatAbsoluteError << ".\n"
         << transferred.workUpperBound << " bounded work items; " << (transferred.hasValues?"positive-weight field support":"empty field support") << '.';
    for(const auto& diagnostic:transferred.diagnostics)text << '\n' << diagnostic;
    result.message=text.str();result.modelOutput=false;return result;
  }
  case Operation::SurfaceFields: {
    const auto fields=vdb::WriteSurfaceFields(input,output,r.surfaceFieldOperation,r.surfaceFields,r.inputGrid,r.processing);
    std::ostringstream text;text << std::setprecision(17);
    text << "Derived " << fields.field.gridName << " (" << fields.valueType << ") with "
         << fields.validVoxelCount << " valid cells; Bool validity grid " << fields.validity.gridName
         << ".\nMissing footprints: " << fields.missingFootprintVoxelCount << "; degenerate gradients: "
         << fields.degenerateGradientVoxelCount << "; " << fields.workItems << " bounded work items.";
    if(fields.gradientSampleCount)text << "\nMaximum local unit-gradient error: " << fields.maximumUnitGradientError
         << " across " << fields.gradientSampleCount << " diagnostic cells.";
    else text << "\nLocal gradient quality is unavailable: no diagnostic cells.";
    for(const auto& diagnostic:fields.diagnostics)text << '\n' << diagnostic;
    result.message=text.str();result.modelOutput=false;return result;
  }
  case Operation::MoveNativePoints: {
    auto options=r.pointMotion;options.groups=r.pointSelection;
    auto limits=r.pointAttributes;limits.maxPoints=r.maxPoints;
    const auto moved=vdb::WriteMovedPoints(input,output,options,r.inputGrid,r.processing,limits);
    std::ostringstream text;text << std::setprecision(17);
    text << "Moved " << moved.selectedCount << " selected native points; retained " << moved.outputCount
         << " of " << moved.inputCount << " records.\nActive records: " << moved.inputActiveCount << " -> "
         << moved.outputActiveCount << "; " << moved.inactiveRecordsActivated << " inactive records activated by explicit OR."
         << "\nMaximum position error: " << moved.maxPositionErrorVoxels << " target voxels / "
         << moved.maxPositionErrorWorld << " world units; unselected error " << moved.maxUnselectedPositionErrorVoxels
         << " target voxels.\n" << moved.workUpperBound << " bounded work items; "
         << (moved.noOp?"exact no-op":"native records repartitioned") << '.';
    for(const auto& diagnostic:moved.diagnostics)text << '\n' << diagnostic;
    result.message=text.str();result.modelOutput=false;return result;
  }
  case Operation::RebuildLevelSet: {
    const auto rebuilt=vdb::WriteRebuiltLevelSet(input,output,r.levelSetRebuild,r.inputGrid,r.processing);
    std::ostringstream text;text << std::setprecision(17);
    text << "Rebuilt " << rebuilt.sourceValueType << " contour at " << rebuilt.isovalue << " into "
         << rebuilt.grid.gridName << ": " << rebuilt.grid.activeVoxelCount << " active voxels.\n"
         << "Effective exterior/interior bands: " << rebuilt.effectiveExteriorWidth << " / "
         << rebuilt.effectiveInteriorWidth << " target voxels; " << rebuilt.workUpperBound << " bounded work items.";
    for(const auto& diagnostic:rebuilt.diagnostics)text << '\n' << diagnostic;
    result.message=text.str();result.modelOutput=rebuilt.grid.activeVoxelCount!=0;return result;
  }
  case Operation::FieldAdvection: {
    const auto advected=vdb::WriteAdvectedField(input,second,output,r.fieldAdvection,r.inputGrid,r.secondGrid,r.processing);
    std::ostringstream text;text << std::setprecision(17);
    text << "Advected " << advected.grid.gridName << ": " << advected.updatedVoxelCount << " updated voxels; "
         << advected.executedSteps << " steps; " << advected.workItems << " bounded work items.\n"
         << "Maximum step displacement: " << advected.maximumStepDisplacement << " source voxels; "
         << advected.overshootVoxelCount << " overshoots, " << advected.clampedVoxelCount << " clamps, "
         << advected.revertedVoxelCount << " reversions.\nRounded trace components: "
         << advected.roundedTraceComponentCount << "; maximum rounding " << advected.maximumTraceRounding << '.';
    for(const auto& diagnostic:advected.diagnostics)text << '\n' << diagnostic;
    result.message=text.str();result.modelOutput=false;return result;
  }
  case Operation::PotentialFlow: {
    const auto flow=vdb::WritePotentialFlow(input,output,r.potentialFlow,r.inputGrid,r.processing);
    std::ostringstream text;text << std::setprecision(17);
    text << "Potential flow: " << flow.domainCellCount << " domain cells; " << flow.iterations
         << " iterations; stored-potential residual " << flow.residual << "; boundary normal error "
         << flow.maxBoundaryNormalError << '.';
    if(flow.interiorDiagnosticCellCount)text << "\nInterior divergence " << flow.maxInteriorDivergence
         << "; curl " << flow.maxInteriorCurl << " from " << flow.interiorDiagnosticCellCount << " complete stencils.";
    else text << "\nInterior divergence/curl unavailable: no complete diagnostic stencils.";
    for(const auto& grid:flow.grids)text << '\n' << grid.gridName << ": " << grid.activeVoxelCount << " active voxels.";
    for(const auto& diagnostic:flow.diagnostics)text << '\n' << diagnostic;
    result.message=text.str();result.modelOutput=false;return result;
  }
  case Operation::PointNeighbors: {
    auto options=r.pointNeighbors;options.groups=r.pointSelection;
    auto limits=r.pointAttributes;limits.maxPoints=r.maxPoints;
    const std::array<vdb::PointNeighborQuery,1> queries{{{r.queryPosition,r.distance}}};
    const auto found=vdb::QueryPointNeighbors(input,queries,options,r.inputGrid,r.processing,limits);
    std::ostringstream text;text << std::setprecision(17);
    text << found.queries.at(0).size() << " neighbors from " << found.selectedCount << " selected native points; "
         << found.levels.size() << " native index levels.";
    const auto& hits=found.queries.at(0);
    for(size_t i=0;i<std::min<size_t>(hits.size(),256);++i) {
      const auto& hit=hits[i];text << "\nOrdinal " << hit.reference.ordinal << "; leaf ["
        << hit.reference.leafOrigin[0] << ',' << hit.reference.leafOrigin[1] << ',' << hit.reference.leafOrigin[2]
        << "]; local index " << hit.reference.localIndex << "; world distance " << hit.distanceWorld
        << "; particle radius " << hit.radiusWorld << '.';
    }
    if(hits.size()>256)text << "\nShowing the first 256 neighbors. Use the command-line query for the complete report.";
    text << '\n' << found.diagnostic;result.message=text.str();result.output.clear();result.modelOutput=false;return result;
  }
  case Operation::IndexedPointMask: {
    auto options=r.indexedPointMask;options.groups=r.pointSelection;
    auto limits=r.pointAttributes;limits.maxPoints=r.maxPoints;
    const auto mask=vdb::WriteIndexedPointMask(input,output,options,r.inputGrid,r.processing,limits);
    std::ostringstream text;
    text << "Indexed " << mask.selectedCount << " of " << mask.inputCount << " native points into "
         << mask.mask.gridName << ": " << mask.mask.activeVoxelCount << " occupied cells.";
    if(options.includeCounts)text << "\nInt64 counts: " << mask.counts.gridName << '.';
    result.message=text.str();result.modelOutput=false;return result;
  }
  case Operation::Regions: {
    const auto regions=vdb::WriteRegions(input,output,r.regionsOperation,r.regions,r.inputGrid,r.processing);
    std::ostringstream text;
    text << "Wrote " << regions.grids.size() << " native region masks: " << regions.totalActiveVoxelCount
         << " active voxels; " << regions.workItems << " bounded work items.";
    for(const auto& g:regions.grids)text << '\n' << g.gridName << ": " << g.activeVoxelCount << " active voxels.";
    for(const auto& diagnostic:regions.diagnostics)text << '\n' << diagnostic;
    result.message=text.str();result.modelOutput=false;return result;
  }
  case Operation::SurfaceQuery: {
    const std::array<std::array<double,3>,1> positions{r.queryPosition};
    const auto queried=vdb::QueryLevelSetSurface(input,positions,r.surfaceQuery,r.inputGrid,r.processing);
    const auto& q=queried.queries.at(0);std::ostringstream text;text << std::setprecision(17);
    text << "Approximate surface point cloud: [" << q.closestPosition[0] << ", " << q.closestPosition[1]
         << ", " << q.closestPosition[2] << "]; world distance " << q.distance << '.';
    if(q.querySdfValid)text << "\nQuery SDF: " << q.querySdf << '.';
    if(q.geometryValid)text << "\nLocal normal: [" << q.normal[0] << ", " << q.normal[1] << ", " << q.normal[2]
        << "]; mean curvature " << q.meanCurvature << "; Gaussian curvature " << q.gaussianCurvature << '.';
    else text << "\nLocal geometry unavailable at the returned point.";
    text << '\n' << queried.diagnostic;result.message=text.str();result.output.clear();result.modelOutput=false;return result;
  }
  case Operation::Fracture: {
    const auto fractured=vdb::WriteFracturedLevelSet(input,second,output,r.fracture,r.inputGrid,r.secondGrid,r.processing);
    std::ostringstream text;
    text << "Wrote " << fractured.pieces.size() << " fracture pieces; negative sample partition: "
         << fractured.inputNegativeVoxelCount << " = " << fractured.outputNegativeVoxelCount << " + "
         << fractured.cutBoundaryVoxelCount << " cutter boundary samples; " << fractured.workItems << " bounded work items.";
    for(const auto& piece:fractured.pieces)text << '\n' << piece.grid.gridName << ": " << piece.negativeVoxelCount << " negative samples.";
    for(const auto& diagnostic:fractured.diagnostics)text << '\n' << diagnostic;
    result.message=text.str();result.modelOutput=!fractured.pieces.empty();return result;
  }
  case Operation::FastSweep: case Operation::FastExtend: {
    const auto swept=r.operation==Operation::FastExtend?
        vdb::WriteFastExtendedField(input,second,output,r.fastSweepingFog,r.fastSweeping,r.inputGrid,r.secondGrid,r.processing):
        vdb::WriteFastSwept(input,output,r.fastSweepingOperation,r.fastSweeping,r.inputGrid,r.processing);
    std::ostringstream text;
    text << "Wrote swept distance " << swept.grid.gridName << ": " << swept.grid.activeVoxelCount
         << " active voxels; " << swept.boundaryVoxelCount << " boundary seeds; "
         << swept.sweepingVoxelCount << " solved voxels; " << swept.workItems << " bounded work items.";
    if(!swept.extensionGrid.gridName.empty())text << "\nExtended field: " << swept.extensionGrid.gridName << '.';
    for(const auto& diagnostic:swept.diagnostics)text << '\n' << diagnostic;
    result.message=text.str();result.modelOutput=swept.grid.activeVoxelCount!=0;return result;
  }
  case Operation::VolumeSpheres: {
    const auto fitted=vdb::ExportVolumeSpheres(input,output,r.volumeSpheres,r.inputGrid,r.processing);
    std::ostringstream text;
    text << "Exported " << fitted.spheres.size() << " fitted spheres from " << fitted.gridName
         << " (" << fitted.valueType << ").\n" << fitted.diagnostic;
    result.message=text.str();result.modelOutput=false;return result;
  }
  case Operation::SdfFilter: {
    const auto filtered=vdb::WriteSdfFiltered(input,output,r.sdfFilterOperation,r.sdfFilter,r.inputGrid,r.processing);
    std::ostringstream text;
    text << "Wrote native SDF " << filtered.grid.gridName << ": " << filtered.grid.activeVoxelCount
         << " active voxels; half width " << filtered.halfWidthVoxels << " source voxels; "
         << filtered.workItems << " bounded work items.";
    if(filtered.meanGradientError)text << "\nNear-interface mean gradient error: "
        << *filtered.meanGradientError << " from " << filtered.gradientSampleCount << " stencils.";
    if(filtered.normalizationSelectedVoxels)text << "\nNormalization activity selection: "
        << *filtered.normalizationSelectedVoxels << " existing active voxels.";
    for(const auto& diagnostic:filtered.diagnostics)text << '\n' << diagnostic;
    result.message=text.str();result.modelOutput=filtered.grid.activeVoxelCount!=0;return result;
  }
  case Operation::SdfBackground: {
    const auto edited=vdb::WriteSdfBackground(input,output,r.sdfBackgroundOperation,r.sdfBackground,r.inputGrid,r.processing);
    std::ostringstream text;
    text << "Wrote SDF " << edited.grid.gridName << ": " << edited.grid.activeVoxelCount << " active voxels; "
         << edited.storedLeafCount << " leaves; " << edited.workItems << " work items.";
    for(const auto &diagnostic:edited.diagnostics) text << '\n' << diagnostic;
    result.message=text.str(); return result;
  }
  case Operation::EditValueTopology: case Operation::MaskedValues: {
    const auto edited=r.operation==Operation::EditValueTopology
        ? vdb::WriteValueTopology(input,output,r.valueTopologyOperation,r.valueTopology,r.inputGrid,r.processing)
        : vdb::WriteMaskedValues(input,second,output,r.maskedValuesOperation,r.inputGrid,r.secondGrid,r.processing,r.maskedValuesMaxWork);
    std::ostringstream text;
    text << "Wrote native grid " << edited.grid.gridName << ": " << edited.grid.activeVoxelCount
         << " active voxels; " << edited.storedLeafCount << " leaves; " << edited.workItems << " work items.";
    for(const auto &diagnostic:edited.diagnostics) text << '\n' << diagnostic;
    result.message=text.str(); result.modelOutput=false; return result;
  }
  case Operation::TopologyMask: case Operation::TopologyBoolean: {
    const auto topology=r.operation==Operation::TopologyMask
        ? vdb::WriteTopologyMask(input,output,r.topologyOperation,r.topology,r.inputGrid,r.processing)
        : vdb::WriteTopologyBoolean(input,second,output,r.topologyBoolean,r.inputGrid,r.secondGrid,r.processing,r.topology.maxWorkItems);
    std::ostringstream text;
    text << "Wrote native mask " << topology.grid.gridName << ": " << topology.grid.activeVoxelCount
         << " occupied voxels; " << topology.activeTileCount << " compact active tiles; "
         << topology.storedLeafCount << " leaves; " << topology.workItems << " work items.";
    for (const auto& diagnostic:topology.diagnostics) text << '\n' << diagnostic;
    result.message=text.str(); result.modelOutput=false; return result;
  }
  case Operation::SampleNativePointAttribute: {
    auto options=r.pointFieldSampling; options.groups=r.pointSelection;
    auto limits=r.pointAttributes; limits.maxPoints=r.maxPoints;
    const auto sampled=vdb::WriteSampledPointAttribute(input,second,output,options,r.inputGrid,r.secondGrid,r.processing,limits);
    std::ostringstream text;
    text << "Sampled " << sampled.writtenCount << " of " << sampled.inputCount << " native points into "
         << options.attributeName << " from " << sampled.fieldGridName << " (" << sampled.fieldValueType << ").";
    if (!sampled.vectorSemantics.empty()) text << '\n' << sampled.vectorSemantics;
    result.message=text.str(); result.modelOutput=false; return result;
  }
  case Operation::CompositeFields: case Operation::TransformField: {
    const auto composite=r.operation==Operation::CompositeFields
        ? vdb::WriteCompositeField(input,second,output,r.fieldOperation,r.fieldComposite,r.inputGrid,r.secondGrid,r.processing)
        : vdb::WriteTransformedField(input,output,r.fieldTransform,r.inputGrid,r.processing);
    std::ostringstream text;
    text << "Wrote " << composite.grid.gridName << ": " << composite.grid.activeVoxelCount << " active voxels; "
         << composite.storedLeafCount << " stored leaves; " << composite.workItems << " work items.";
    for (const auto& diagnostic:composite.diagnostics) text << '\n' << diagnostic;
    result.message=text.str(); result.modelOutput=false; return result;
  }
  case Operation::PointVoxelMask: {
    auto limits=r.pointAttributes; limits.maxPoints=r.maxPoints;
    const auto mask=vdb::WritePointVoxelMask(input,output,r.pointSelection,r.pointMask,r.inputGrid,r.processing,limits);
    std::ostringstream text;
    text << "Marked " << mask.occupiedVoxelCount << " original point voxels in " << mask.leafCount
         << " leaves; " << mask.selectedCount << " selected of " << mask.inputCount << " points ("
         << mask.selectedActive << " active, " << mask.selectedInactive << " inactive). Native mask: " << mask.maskGridName << '.';
    if (!mask.countGridName.empty()) text << " Exact Int64 counts: " << mask.countGridName << '.';
    if (mask.sourceArchivedSchema) text << " Source has an archived empty schema; outputs are empty native grids.";
    result.message=text.str(); result.modelOutput=false; return result;
  }
  case Operation::FilterField: {
    const auto filtered=vdb::WriteFilteredField(input,output,r.fieldFilterOperation,r.fieldFilter,r.inputGrid,r.processing);
    std::ostringstream text;
    text << "Filtered " << filtered.grid.activeVoxelCount << " active voxels; " << filtered.kernelPasses
         << " kernel passes; " << filtered.voxelizedTileCount << " tiles voxelized; alpha mask "
         << (filtered.maskUsed ? "enabled." : "disabled.");
    for (const auto& diagnostic:filtered.diagnostics) text << '\n' << diagnostic;
    result.message=text.str(); result.modelOutput=false; return result;
  }
  case Operation::RenderTransportImage: {
    auto options=r.transportRender;
    options.sceneSpec=input; options.output=output;
    if (options.backend==vdb::TransportRenderBackend::Gpu)
      options.spirv=r.transportShader.empty() ? container::util::executableDirectory()/"spv_shaders/openvdb_transport_reference.comp.spv" : path(r.transportShader);
    const auto rendered=vdb::RunTransportRenderJob(options);
    std::ostringstream text;
    text << "Wrote scene-linear FLOAT OpenEXR " << rendered.image.width << 'x' << rendered.image.height
         << "; " << rendered.image.samplesPerPixel << " samples per pixel; " << rendered.image.diagnostics.paths
         << " paths; " << rendered.image.diagnostics.work << " tracing work; " << rendered.image.diagnostics.preparationWork
         << " preparation work. Order cap " << rendered.image.maxScatteringEvents << "; truncated paths "
         << rendered.image.diagnostics.truncatedPaths << ". Finite-order bias remains; per-channel standard error "
         << (rendered.image.samplesPerPixel>=2 ? "stored in SE.R/G/B." : "unavailable at one sample.")
         << " Float conversion rounded " << rendered.exported.roundedValues << " values; maximum absolute error "
         << std::setprecision(17) << rendered.exported.maxAbsoluteError << '.';
    result.message=text.str(); result.modelOutput=false; return result;
  }
  case Operation::AuthorFile: {
    const auto authored=vdb::WriteAuthoredVdbSpec(input,output,r.fileAuthoring,r.processing);
    std::ostringstream text;
    text << "Wrote " << authored.outputGridCount << " grids from " << authored.inputGridCount
         << "; renamed " << authored.renamedGridCount << "; removed " << authored.removedGridCount
         << "; metadata edits " << authored.metadataEditCount << "; native points " << authored.pointCount << '.';
    for (const auto& diagnostic:authored.diagnostics) text << '\n' << diagnostic;
    result.message=text.str(); result.modelOutput=false; return result;
  }
  case Operation::SelectPoints: case Operation::DeletePoints: case Operation::CountPoints: case Operation::PointStatistics: {
    auto limits=r.pointAttributes; limits.maxPoints=r.maxPoints;
    std::ostringstream text;
    if (r.operation==Operation::SelectPoints || r.operation==Operation::DeletePoints) {
      const auto selected=vdb::WriteSelectedPoints(input,output,r.pointSelection,
          r.operation==Operation::SelectPoints ? vdb::PointSelectionMode::KeepSelected : vdb::PointSelectionMode::DeleteSelected,
          r.inputGrid,r.processing,limits);
      text << selected.selectedCount << " selected of " << selected.inputCount << "; wrote " << selected.outputCount << " native points.";
      if (selected.archivedSchema) text << " Empty native tree: schema/descriptor/leaf flags retained in a versioned metadata archive. Repopulation is separate.";
    } else if (r.operation==Operation::CountPoints) {
      const auto count=vdb::CountSelectedPoints(input,r.pointSelection,r.inputGrid,r.processing,limits);
      text << count.selected << " selected of " << count.total << "; active-voxel points " << count.selectedActive
           << "; inactive-voxel points " << count.selectedInactive << '.';
      if (count.archivedSchema) text << " Schema read from the bounded zero-point archive.";
      result.output.clear();
    } else {
      const auto statistics=vdb::MeasurePointAttribute(input,r.pointField,r.pointSelection,r.inputGrid,r.processing,limits);
      text << std::setprecision(17) << statistics.selectedCount << " selected points (" << statistics.attribute.valueType << ").";
      if (statistics.hasValues) {
        const auto values=[&](const auto& variant) {
          std::visit([&](const auto& items) {
            if (items.empty()) return;
            if constexpr (requires { items[0][0]; }) text << items[0][0] << ", " << items[0][1] << ", " << items[0][2];
            else if constexpr (std::is_same_v<typename std::decay_t<decltype(items)>::value_type,uint8_t>) text << bool(items[0]);
            else text << items[0];
          },variant);
        };
        text << "\nMinimum: "; values(statistics.minimum);
        text << "\nMaximum: "; values(statistics.maximum);
        for (uint32_t component=0;component<statistics.componentCount;++component)
          text << "\nComponent " << component << " mean " << statistics.mean[component] << "; population standard deviation " << statistics.populationStdDev[component];
      } else text << " Empty selection; no extrema or moments.";
      result.output.clear();
    }
    result.message=text.str(); result.modelOutput=false; return result;
  }
  case Operation::DenseImport: case Operation::DenseExport: {
    const auto volume=r.operation==Operation::DenseImport ? vdb::ReadDenseArrayFile(input,r.processing) :
        vdb::ReadDenseVdb(input,r.denseRegion,r.inputGrid,r.processing);
    if (r.operation==Operation::DenseImport) (void)vdb::WriteDenseVdb(output,volume,r.processing);
    else vdb::WriteDenseArrayFile(output,volume,r.processing);
    std::ostringstream text;
    text << (r.operation==Operation::DenseImport ? "Imported " : "Exported ") << volume.gridName << ": "
         << volume.region.dimensions[0] << 'x' << volume.region.dimensions[1] << 'x' << volume.region.dimensions[2]
         << " native cells and explicit activity mask. Active voxels outside region: "
         << volume.diagnostics.activeVoxelsOutside << "; stored nonbackground values outside: "
         << (volume.diagnostics.storedNonBackgroundOutside ? "yes" : "no") << "; custom metadata keys omitted: "
         << volume.diagnostics.omittedMetadata.size() << ". Array diagnostics retain the omitted key names.";
    result.message=text.str(); result.modelOutput=false; return result;
  }
  case Operation::MultiResolution: case Operation::MultiResolutionInfo: case Operation::MultiResolutionSample: {
    const auto volume=vdb::BuildMultiResolution(input,r.multiresolution,r.inputGrid,r.processing);
    std::ostringstream text;
    if (r.operation==Operation::MultiResolutionSample) {
      text << std::setprecision(17) << "World sample at LoD " << r.sampleLevel << ": ";
      std::visit([&](const auto& value) {
        if constexpr (requires { value[0]; }) text << value[0] << ", " << value[1] << ", " << value[2];
        else text << value;
      },volume->sampleWorld(r.queryPosition,r.sampleLevel));
    } else {
      if (r.operation==Operation::MultiResolution) {
        if (r.multiresolutionLevel) (void)volume->writeLevel(output,*r.multiresolutionLevel,r.processing);
        else volume->writeAll(output,r.processing);
      }
      for (const auto& level:volume->levels()) text << "LoD " << level.level << ": " << level.gridName << " (" << level.valueType
          << "), " << level.activeVoxelCount << " active voxels, " << level.leafCount << " leaves; voxel axes "
          << level.voxelSize[0] << ", " << level.voxelSize[1] << ", " << level.voxelSize[2] << '\n';
    }
    result.message=text.str(); result.modelOutput=false;
    if (r.operation!=Operation::MultiResolution) result.output.clear();
    return result;
  }
  case Operation::Sphere: grid = vdb::WriteSphere(output, r.sphere, r.processing); break;
  case Operation::Platonic: grid = vdb::WritePlatonicLevelSet(output, r.platonic, r.processing); break;
  case Operation::Mesh: grid = vdb::VoxelizeModel(input, output, 1.0f, r.processing); break;
  case Operation::MeshDistance: {
    const auto converted=vdb::VoxelizeModelDistance(input,output,1.0f,r.processing,r.meshConversion);
    result.message="Wrote unsigned distance for "+std::to_string(converted.triangleCount)+
        " triangles; closest-ID grid: "+(converted.triangleGridName.empty()?"omitted":converted.triangleGridName)+
        ". IDs follow source triangle order; this field is not a signed level set.";
    result.modelOutput=false;
    return result;
  }
  case Operation::Particles:
    grid = vdb::WriteParticlesLevelSetFile(input, output, r.processing, {.maxParticles = r.maxParticles}); break;
  case Operation::PointsSdf:
    grid = vdb::WritePointsLevelSet(input,output,r.inputGrid,r.processing,{.maxPoints=r.maxPoints}); break;
  case Operation::SplatDensity: case Operation::SplatVelocity: case Operation::SplatRadius: {
    const auto field=r.operation == Operation::SplatDensity ? vdb::PointSplatField::Density :
        r.operation == Operation::SplatVelocity ? vdb::PointSplatField::Velocity : vdb::PointSplatField::Radius;
    const auto splatted=vdb::WritePointsSplat(input,output,field,r.inputGrid,r.processing,r.splat,{.maxPoints=r.maxPoints});
    std::ostringstream text;
    text << std::setprecision(12) << "Deposited " << splatted.pointCount << " points; mass "
         << splatted.totalMass << " -> " << splatted.depositedMass << ".";
    if (!splatted.densityGridName.empty()) text << " Companion density: " << splatted.densityGridName << '.';
    result.message=text.str(); result.modelOutput=r.operation != Operation::SplatVelocity;
    return result;
  }
  case Operation::Points: {
    const auto points = vdb::WritePointsFile(input, output, r.processing, {.maxPoints = r.maxPoints});
    result.message = "Wrote " + std::to_string(points.pointCount) + " points.";
    return result;
  }
  case Operation::EditPointAttributes: case Operation::PointSchema:
  case Operation::PointAttribute: case Operation::PointGroup: {
    auto limits=r.pointAttributes; limits.maxPoints=r.maxPoints;
    if (r.operation==Operation::EditPointAttributes) {
      const auto schema=vdb::ApplyPointAttributeEditsFile(input,second,output,r.inputGrid,r.processing,limits);
      result.message="Wrote "+std::to_string(schema.pointCount)+" native points, "+
          std::to_string(schema.attributes.size())+" attributes and "+std::to_string(schema.groups.size())+" groups.";
    } else if (r.operation==Operation::PointSchema) {
      const auto schema=vdb::ExportPointAttributeSchema(input,output,r.inputGrid,r.processing,limits);
      result.message="Exported schema for "+std::to_string(schema.pointCount)+" native points.";
    } else {
      if (r.operation==Operation::PointAttribute) vdb::ExportPointAttribute(input,r.pointField,output,r.inputGrid,r.processing,limits);
      else vdb::ExportPointGroup(input,r.pointField,output,r.inputGrid,r.processing,limits);
      result.message="Exported native "+r.pointField+" in leaf-origin/local-index order.";
    }
    result.modelOutput=false; return result;
  }
  case Operation::ExportPoints: {
    const auto points = vdb::ExportPoints(input, output, r.inputGrid, r.processing, {.maxPoints = r.maxPoints});
    result.message = "Exported " + std::to_string(points.pointCount) + " points.";
    result.modelOutput = false;
    return result;
  }
  case Operation::Scatter: case Operation::AdvectPoints:
  case Operation::SamplePointVelocity: case Operation::SamplePointRadius: {
    const vdb::PointOptions points{.maxPoints = r.maxPoints};
    const auto pointResult = r.operation == Operation::Scatter
        ? vdb::ScatterPoints(input, output, r.inputGrid, r.scatter, r.processing, points)
        : r.operation == Operation::AdvectPoints
        ? vdb::WriteAdvectedPoints(input, second, output, r.inputGrid, r.secondGrid,
                                  r.pointAdvection, r.processing, points)
        : vdb::WriteSampledPoints(input, second, output, r.operation == Operation::SamplePointRadius
            ? vdb::PointAttribute::Radius : vdb::PointAttribute::Velocity,
            r.inputGrid, r.secondGrid, r.processing, points);
    result.message = "Wrote " + std::to_string(pointResult.pointCount) + " points.";
    return result;
  }
  case Operation::Project: {
    const auto projected = vdb::WriteProjectedVelocity(input, second, output,
        r.projection, r.inputGrid, r.secondGrid, r.processing);
    std::ostringstream text;
    text << projected.fluidCellCount << " fluid cells, " << projected.componentCount
         << " components; divergence " << std::scientific << std::setprecision(6)
         << projected.maxDivergenceBefore << " -> " << projected.maxDivergenceAfter
         << "; residual " << projected.residual << "; rounding bound "
         << projected.divergenceRoundoffBound << '.';
    if (projected.roundoffLimited) text << " Velocity corrections reached stored-value precision.";
    result.message = text.str(); result.modelOutput = false;
    return result;
  }
  case Operation::FluidSources: {
    const auto state=vdb::WriteFluidSources(input,output,r.sources,r.processing);
    std::ostringstream text;
    text << std::setprecision(12) << state.affectedCellCount << " affected cells; " << state.changedCellCount
         << " changed; density mass " << state.densityMassBefore << " -> " << state.densityMassAfter
         << "; Kelvin-volume integral " << state.temperatureIntegralBefore << " -> " << state.temperatureIntegralAfter << '.';
    result.message=text.str(); result.modelOutput=false;
    result.restartGrids=std::array<std::string,4>{r.sources.densityGridName,r.sources.velocityGridName,
        r.sources.domainGridName,r.sources.temperatureGridName};
    return result;
  }
  case Operation::Viscosity: {
    const auto state=vdb::WriteViscousFluidState(input,output,r.viscosity,r.processing);
    std::ostringstream text;
    text << std::setprecision(12) << "MAC energy " << state.kineticEnergyBefore << " -> "
         << state.kineticEnergyAfterDiffusion << " -> " << state.kineticEnergyAfterProjection
         << "; diffusion residual " << state.diffusionResidual << "; pressure residual "
         << state.pressureResidual << "; divergence " << state.maxDivergenceAfter << '.';
    if (state.skipped) text << " Zero viscosity: validated state copied.";
    if (state.roundoffLimited) text << " Velocity corrections reached stored-value precision.";
    result.message=text.str(); result.modelOutput=false;
    result.restartGrids=std::array<std::string,4>{r.viscosity.densityGridName,r.viscosity.velocityGridName,
        r.viscosity.domainGridName,r.viscosity.temperatureGridName};
    return result;
  }
  case Operation::Simulate: case Operation::SimulateSequence: {
    const vdb::FluidSimulationInputs inputs{.density = input, .velocity = second,
        .domain = path(r.domainInput), .temperature = path(r.temperatureInput),
        .densityGrid = r.inputGrid, .velocityGrid = r.secondGrid,
        .domainGrid = r.domainGrid, .temperatureGrid = r.temperatureGrid};
    if (r.operation == Operation::SimulateSequence) {
      const auto sequence = vdb::WriteFluidSequence(inputs,output,r.sequence,r.simulation,r.processing);
      result.message = "Wrote " + std::to_string(sequence.frames.size()) + " smoke frames and sequence.json; last frame ready for restart.";
      const auto utf8=sequence.frames.back().path.u8string(); result.output={utf8.begin(),utf8.end()};
      result.modelOutput=false;
      result.restartGrids=std::array<std::string,4>{r.simulation.densityGridName,
          r.simulation.velocityGridName,r.simulation.domainGridName,r.simulation.temperatureGridName};
      return result;
    }
    const auto state = vdb::WriteFluidStep(inputs, output, r.simulation, r.processing);
    std::ostringstream text;
    text << state.actualSubsteps << " substeps over " << state.simulatedTime
         << " seconds; density mass " << std::setprecision(12) << state.densityMassBefore
         << " -> " << state.densityMassAfter << "; Kelvin-volume integral "
         << state.temperatureIntegralBefore << " -> " << state.temperatureIntegralAfter
         << " (not thermal energy); divergence " << std::scientific
         << state.maxDivergenceAfter << "; rounding bound " << state.divergenceRoundoffBound << '.';
    if (state.roundoffLimited) text << " Velocity corrections reached stored-value precision.";
    if (state.viscositySubsteps) text << " Viscosity: " << state.viscositySubsteps << " substeps; diffusion residual "
        << state.maxDiffusionResidual << "; dissipated MAC energy " << state.viscosityEnergyDissipated << '.';
    if (state.sourceSubsteps) text << " Sources: " << state.sourceSubsteps << " substeps; density mass delta "
        << state.sourceDensityMassChange << "; Kelvin-volume delta " << state.sourceTemperatureIntegralChange << '.';
    result.message = text.str(); result.modelOutput = false;
    result.restartGrids = std::array<std::string,4>{r.simulation.densityGridName,
        r.simulation.velocityGridName, r.simulation.domainGridName, r.simulation.temperatureGridName};
    return result;
  }
  case Operation::Union: case Operation::Intersection: case Operation::Difference: {
    auto operation = vdb::CsgOperation::Union;
    if (r.operation == Operation::Intersection) operation = vdb::CsgOperation::Intersection;
    if (r.operation == Operation::Difference) operation = vdb::CsgOperation::Difference;
    grid = vdb::WriteCsg(input, second, output, operation, r.inputGrid, r.secondGrid, r.processing);
    break;
  }
  case Operation::Fog: grid = vdb::WriteFog(input, output, r.inputGrid, r.processing); break;
  case Operation::Resample: grid = vdb::WriteResampled(input, output, r.inputGrid, r.processing); break;
  case Operation::Offset: grid = vdb::WriteOffset(input, output, r.distance, r.inputGrid, r.processing); break;
  case Operation::Smooth:
    grid = vdb::WriteSmoothed(input, output, r.filter, r.smoothing, r.inputGrid, r.processing); break;
  case Operation::Velocity:
    grid = vdb::WriteConstantVelocity(output, r.velocity, r.processing);
    result.modelOutput = false;
    break;
  case Operation::Advect:
    grid = vdb::WriteAdvectedFog(input, second, output, {.timeStep = r.time, .steps = r.steps},
                               r.inputGrid, r.secondGrid, r.processing); break;
  case Operation::AdvectSdf: case Operation::MorphSdf: {
    const auto transported=r.operation == Operation::AdvectSdf
        ? vdb::WriteAdvectedLevelSet(input,second,output,r.levelSetTransport,r.inputGrid,r.secondGrid,r.processing)
        : vdb::WriteMorphedLevelSet(input,second,output,r.levelSetTransport,r.inputGrid,r.secondGrid,r.processing);
    result.message="Wrote "+transported.levelSet.gridName+": "+std::to_string(transported.cflSteps)+
        " CFL steps; "+std::to_string(transported.work)+" leaf-slot/sample work units.";
    return result;
  }
  case Operation::Segment: {
    const auto components = vdb::WriteSegmentedLevelSet(input, output, r.inputGrid, r.processing,
                                                       {.maxComponents = r.maxComponents});
    result.message = "Wrote " + std::to_string(components.size()) + " surface components.";
    return result;
  }
  case Operation::ExportMesh: {
    vdb::ImportOptions options;
    options.gridName = r.inputGrid;
    options.isoValue = r.isoValue;
    options.adaptivity = r.adaptivity;
    options.maxActiveVoxels = r.processing.maxVoxelCount;
    options.maxInputBytes = r.processing.maxInputBytes;
    options.maxTriangles = r.processing.maxTriangles;
    const auto mesh = vdb::WriteObjSurface(input, output, options);
    result.message = "Exported " + std::to_string(mesh.triangleCount) + " OBJ triangles.";
    for (const auto& diagnostic : mesh.diagnostics) result.message += "\n" + diagnostic;
    result.modelOutput = false;
    return result;
  }
  case Operation::Inspect: {
    vdb::InspectionOptions options;
    options.gridName = r.inputGrid;
    options.loadStatistics = r.statistics;
    options.histogramBins = r.statistics ? r.inspectionHistogramBins : 0;
    if (options.histogramBins) options.histogramRange = r.inspectionHistogramRange;
    options.maxInputBytes = r.processing.maxInputBytes;
    options.maxActiveVoxels = r.processing.maxVoxelCount;
    const auto inspection = vdb::InspectFile(input, options);
    std::ostringstream text;
    text << "File: " << inspection.fileBytes << " bytes\n";
    for (const auto& information : inspection.grids) {
      text << information.descriptorName << " (" << information.valueType << ", "
           << information.gridClass << ")\nVoxel size: " << information.voxelSize[0]
           << ", " << information.voxelSize[1] << ", " << information.voxelSize[2] << '\n';
      if (information.statistics) {
        const auto& s = *information.statistics;
        text << "Active: " << s.count << ", sum: " << s.sum;
        if (s.minimum) text << ", minimum: " << *s.minimum << ", maximum: " << *s.maximum;
        text << '\n';
        if (s.mean) text << "Mean: " << *s.mean << ", population deviation: "
                         << *s.populationStdDev << ", RMS: " << *s.rootMeanSquare << '\n';
        if (s.histogram) {
          const auto& h = *s.histogram;
          text << "Histogram";
          if (h.minimum) text << " [" << *h.minimum << "," << *h.maximum << "]";
          text << ", below: " << h.belowRange << ", above: " << h.aboveRange << "\nBins: ";
          for (size_t bin=0;bin<h.bins.size();++bin) {
            if (bin) text << ',';
            text << h.bins[bin];
          }
          text << '\n';
        }
      }
      if (!information.statisticsUnavailableReason.empty()) text << information.statisticsUnavailableReason << '\n';
      if (!information.transformDiagnostic.empty()) text << information.transformDiagnostic << '\n';
    }
    result.message = text.str();
    result.output.clear();
    result.modelOutput = false;
    return result;
  }
  case Operation::Measure: {
    const auto measured = vdb::MeasureLevelSet(input, r.inputGrid, r.processing);
    std::ostringstream text;
    text << std::setprecision(12) << "Area: " << measured.area << " (world units squared)\n"
         << "Enclosed volume: " << measured.volume << " (world units cubed)\n"
         << "Average mean curvature: " << measured.averageMeanCurvature << " (inverse world units)\n"
         << "Gaussian curvature integral: " << measured.totalGaussianCurvature << '\n'
         << "Euler estimate: " << measured.eulerEstimate << "; nearest integer: "
         << measured.estimatedEulerCharacteristic << "; rounding error: " << measured.eulerRoundingError
         << '\n' << measured.topologyDiagnostic;
    result.message = text.str(); result.output.clear(); result.modelOutput = false;
    return result;
  }
  case Operation::Differential:
    if (r.differential < 0 || r.differential > 5) throw std::invalid_argument("Unknown field operation.");
    grid = vdb::WriteDifferential(input, output, static_cast<vdb::DifferentialOperation>(r.differential),
                                  r.inputGrid, r.processing);
    result.modelOutput = r.differential == 1 || r.differential == 3 || r.differential == 4;
    break;
  case Operation::Poisson: {
    const auto solution = vdb::WritePoissonSolution(input, output, r.inputGrid,
        {.maxIterations = r.maxIterations, .tolerance = r.tolerance}, r.processing);
    std::ostringstream text;
    text << "Solved in " << solution.iterations << " iterations; residual "
         << std::scientific << std::setprecision(6) << solution.residual << '.';
    result.message = text.str();
    return result;
  }
  case Operation::SampleScalar: case Operation::SampleVector: {
    std::ostringstream text;
    text << std::setprecision(17);
    if (r.operation == Operation::SampleScalar)
      text << "Scalar: " << vdb::SampleScalar(input, r.queryPosition, r.inputGrid, r.processing);
    else {
      const auto vector = vdb::SampleVector(input, r.queryPosition, r.inputGrid, r.processing);
      text << "Vector: " << vector[0] << ", " << vector[1] << ", " << vector[2];
    }
    result.message = text.str(); result.output.clear(); result.modelOutput = false;
    return result;
  }
  default: throw std::invalid_argument("Unknown OpenVDB operation.");
  }
  result.message = "Wrote " + grid.gridName + ": " + std::to_string(grid.activeVoxelCount) + " active voxels.";
  return result;
}

void editDouble(const char* label, double& value) {
  ImGui::InputDouble(label, &value, 0, 0, "%.17g");
}
void editCount(const char* label, uint32_t& value, uint32_t maximum) {
  int draft = static_cast<int>(value);
  if (ImGui::InputInt(label, &draft)) value = static_cast<uint32_t>(std::clamp(draft, 1, int(maximum)));
}
template <class T>
void editBudget(const char* label, T& value) {
  uint64_t entered = value;
  if (ImGui::InputScalar(label, ImGuiDataType_U64, &entered))
    value = static_cast<T>(std::min<uint64_t>(entered, std::numeric_limits<T>::max()));
}
void editSourceSpheres(std::vector<vdb::FluidSourceSphere>& sources) {
  std::optional<size_t> remove;
  for (size_t index=0;index<sources.size();++index) {
    ImGui::PushID(static_cast<int>(index));
    auto& source=sources[index];
    const auto title="Source sphere "+std::to_string(index+1);
    if (ImGui::TreeNode(title.c_str())) {
      ImGui::InputScalarN("World center",ImGuiDataType_Double,source.center.data(),3,nullptr,nullptr,"%.17g");
      editDouble("World radius",source.radius);
      editDouble("Density per second",source.densityRate);
      editDouble("Kelvin per second",source.temperatureRate);
      editDouble("Density decay per second",source.densityDecayRate);
      editDouble("Kelvin relaxation per second",source.temperatureRelaxationRate);
      ImGui::Checkbox("Clear density first",&source.clearDensity);
      ImGui::Checkbox("Reset Kelvin to ambient first",&source.resetTemperature);
      if (ImGui::Button("Remove source")) remove=index;
      ImGui::TreePop();
    }
    ImGui::PopID();
  }
  if (remove) sources.erase(sources.begin()+*remove);
  ImGui::BeginDisabled(sources.size()>=64);
  if (ImGui::Button("Add source sphere")) sources.emplace_back();
  ImGui::EndDisabled();
}
} // namespace

struct OpenVdbAuthoringPanel::JobControl {
  explicit JobControl(uint32_t total):totalFrames(total) {}
  const uint32_t totalFrames;
  std::atomic_uint32_t completedFrames{0};
  std::atomic_bool cancel{false};
};

OpenVdbAuthoringPanel::OpenVdbAuthoringPanel(Executor executor)
    : executor_(executor ? std::move(executor) : Executor(execute)) {}
OpenVdbAuthoringPanel::~OpenVdbAuthoringPanel() { (void)requestCancel(); }

bool OpenVdbAuthoringPanel::submit(const OpenVdbAuthoringRequest& request) {
  poll();
  if (job_.valid()) return false;
  result_.reset();
  try {
    auto snapshot=request;
    jobControl_.reset();
    if (snapshot.operation==Operation::SimulateSequence) {
      auto state=std::make_shared<JobControl>(snapshot.sequence.frameCount);
      jobControl_=state;
      snapshot.sequence.stopRequested=[state,previous=snapshot.sequence.stopRequested] {
        return state->cancel.load(std::memory_order_acquire) || (previous && previous());
      };
      snapshot.sequence.progress=[state,previous=snapshot.sequence.progress](uint32_t completed,uint32_t total) {
        state->completedFrames.store(completed,std::memory_order_release);
        if (previous) previous(completed,total);
      };
    }
    job_ = std::async(std::launch::async, [request=std::move(snapshot), executor = executor_] {
      try {
        // Allocate the submitted artifact before a publishing executor runs.
        auto particleArtifact=request.operation==Operation::ParticleGeometry
            ? particleGeometryArtifact(request) : std::nullopt;
        auto fluidArtifact=request.operation==Operation::BoundaryFluidStep
            ? boundaryFluidArtifact(request) : std::nullopt;
        auto outcome=executor(request);
        outcome.boundaryFluidArtifact.reset();
        if(request.operation==Operation::BoundaryFluidStep) {
          outcome.output.clear();outcome.modelOutput=false;outcome.restartGrids.reset();
          if(outcome.success)outcome.boundaryFluidArtifact=std::move(fluidArtifact);
        }
        outcome.particleGeometryArtifact.reset();
        if(request.operation==Operation::ParticleGeometry) {
          outcome.output.clear();outcome.restartGrids.reset();
          outcome.modelOutput=outcome.success && outcome.modelOutput && particleArtifact && particleArtifact->sdfGrid;
          if(outcome.success)outcome.particleGeometryArtifact=std::move(particleArtifact);
        }
        if(request.operation==Operation::RayIntersections || request.operation==Operation::RayVolumeSpans || request.operation==Operation::QueryActiveRegions || request.operation==Operation::RenderSurfaceReference || request.operation==Operation::IntegrateFogRays) {
          outcome.output.clear();outcome.modelOutput=false;outcome.restartGrids.reset();
        }
        if(request.operation==Operation::ValueComponents) {
          outcome.modelOutput=false;outcome.restartGrids.reset();
        }
        return outcome;
      }
      catch (const std::exception& error) { return OpenVdbAuthoringResult{false, error.what(), {}, false}; }
      catch (...) { return OpenVdbAuthoringResult{false, "OpenVDB operation failed.", {}, false}; }
    });
  } catch (const std::exception& error) {
    jobControl_.reset();
    result_ = OpenVdbAuthoringResult{false, error.what(), {}, false};
    return false;
  }
  return true;
}

void OpenVdbAuthoringPanel::poll() {
  if (job_.valid() && job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    result_ = job_.get();
}

bool OpenVdbAuthoringPanel::requestCancel() {
  if (!busy() || !jobControl_) return false;
  jobControl_->cancel.store(true,std::memory_order_release);
  return true;
}

std::optional<OpenVdbAuthoringProgress> OpenVdbAuthoringPanel::progress() const {
  if (!jobControl_) return {};
  return OpenVdbAuthoringProgress{jobControl_->completedFrames.load(std::memory_order_acquire),
      jobControl_->totalFrames,jobControl_->cancel.load(std::memory_order_acquire)};
}

std::optional<std::string> OpenVdbAuthoringPanel::draw() {
  poll();
  struct IdScope {
    explicit IdScope(const void* id) { ImGui::PushID(id); }
    ~IdScope() { ImGui::PopID(); }
  } idScope(this);
  if (!ImGui::CollapsingHeader("OpenVDB tools")) return {};
  if (!vdb::IsAvailable()) {
    ImGui::TextDisabled("OpenVDB tools unavailable. Native NanoVDB tools and CPU images have independent availability.");
  }
  constexpr const char* names[] = {"Sphere SDF", "Mesh to SDF", "Particle spheres to SDF", "Import points",
      "Export points", "SDF union", "SDF intersection", "SDF difference", "SDF to fog", "Resample",
      "SDF offset", "SDF smoothing", "Constant velocity", "Advect fog", "Separate surfaces", "Export OBJ",
      "Inspect grids", "Field calculus", "Poisson solve", "Sample scalar (nearest)", "Sample vector (linear)",
      "Scatter points", "Advect points", "Sample point velocity", "Sample point radius", "Project velocity", "Platonic SDF", "Smoke timestep", "Measure SDF", "Smoke sequence", "Unsigned mesh distance",
      "Native points to SDF", "Splat point density", "Splat point velocity", "Splat point radius", "Fluid viscosity",
      "Advect SDF", "Morph SDF", "Fluid sources and sinks",
      "Edit native point attributes", "Export native point schema", "Export native point attribute", "Export native point group",
      "Import native dense array", "Export native dense region", "Build multiresolution levels", "Inspect multiresolution levels", "Sample fractional level",
      "Keep selected points", "Delete selected points", "Count selected points", "Point attribute statistics", "Author VDB file from spec", "Render physical volume image", "Filter ordinary field", "Point voxel occupancy mask", "Composite ordinary fields", "Transform field values", "Topology mask and morphology", "Topology Boolean", "Sample native point attribute",
      "Inspect NanoVDB catalog", "Validate NanoVDB", "Copy/re-encode NanoVDB", "VDB to NanoVDB", "NanoVDB to VDB", "Rewrite NanoVDB checksums", "Recompute NanoVDB statistics", "Edit native values and activity", "Masked native value edits and clipping", "SDF backgrounds and signed flood fill", "Native SDF filters and tracking", "Export fitted volume spheres", "Fast sweeping distances", "Fast sweeping field extension", "Region and cavity masks", "Approximate surface queries", "Fracture signed surfaces", "Advect scalar or vector field", "Exterior potential flow", "Native point neighbors", "Indexed point occupancy mask", "Rebuild closed contour to SDF", "Derive native surface fields", "Move native points", "Transfer native point attribute", "Resample ordinary field", "Diagnose native grid", "Write diagnostic failure mask", "Project velocity with boundaries", "Replay canonical boundary projection", "Author native NanoVDB primitive", "Advect native points", "Query active native regions", "Approximate SDF ray intersections", "Coarse active-volume ray spans", "Export value-preserving active components", "Render native SDF surface reference", "Particle trails and occupancy masks", "Fixed-boundary fluid timestep", "Integrate nodal fog WORLD rays"};
  static_assert(std::size(names) == static_cast<size_t>(Operation::Count));
  int selected = static_cast<int>(draft.operation);
  ImGui::Combo("Operation", &selected, names, static_cast<int>(std::size(names)));
  if (selected != static_cast<int>(draft.operation) &&
      draft.processing.gridName == defaultGridName(draft.operation))
    draft.processing.gridName = defaultGridName(static_cast<Operation>(selected));
  draft.operation = static_cast<Operation>(selected);
  const auto operation = draft.operation;
  const bool nativeFile=nativeFileOperation(operation);
  const bool nanoPrimitive=operation==Operation::NanoPrimitive;
  const bool nativeQuery=operation==Operation::NanoInspect || operation==Operation::NanoValidate;
  const bool fileAuthoring=operation==Operation::AuthorFile;
  const bool physicalImage=operation==Operation::RenderTransportImage;
  const bool surfaceImage=operation==Operation::RenderSurfaceReference;
  const bool particleGeometry=operation==Operation::ParticleGeometry;
  const bool filterField=operation==Operation::FilterField;
  const bool compositeFields=operation==Operation::CompositeFields || operation==Operation::TransformField;
  const bool topology=operation==Operation::TopologyMask || operation==Operation::TopologyBoolean;
  const bool valueTopology=operation==Operation::EditValueTopology;
  const bool maskedValues=operation==Operation::MaskedValues;
  const bool sdfBackground=operation==Operation::SdfBackground;
  const bool sdfFilter=operation==Operation::SdfFilter;
  const bool volumeSpheres=operation==Operation::VolumeSpheres;
  const bool fastSweeping=operation==Operation::FastSweep || operation==Operation::FastExtend;
  const bool fastExtension=operation==Operation::FastExtend;
  const bool regions=operation==Operation::Regions;
  const bool activeRegionQuery=operation==Operation::QueryActiveRegions;
  const bool valueComponents=operation==Operation::ValueComponents;
  const bool fogRays=operation==Operation::IntegrateFogRays;
  const bool rayQuery=operation==Operation::RayIntersections || operation==Operation::RayVolumeSpans || fogRays;
  const bool surfaceQuery=operation==Operation::SurfaceQuery;
  const bool fracture=operation==Operation::Fracture;
  const bool fieldAdvection=operation==Operation::FieldAdvection;
  const bool potentialFlow=operation==Operation::PotentialFlow;
  const bool pointNeighbors=operation==Operation::PointNeighbors;
  const bool indexedPointMask=operation==Operation::IndexedPointMask;
  const bool rebuildLevelSet=operation==Operation::RebuildLevelSet;
  const bool surfaceFields=operation==Operation::SurfaceFields;
  const bool moveNativePoints=operation==Operation::MoveNativePoints;
  const bool advectNativePoints=operation==Operation::AdvectNativePoints;
  const bool pointTransfer=operation==Operation::TransferPointAttribute;
  const bool fieldResampling=operation==Operation::ResampleField;
  const bool diagnostics=operation==Operation::Diagnose || operation==Operation::DiagnosticMask;
  const bool diagnosticPoints=diagnostics && draft.diagnostics.profile==vdb::DiagnosticProfile::PointData;
  const bool boundaryProjection=operation==Operation::ProjectBoundaries || operation==Operation::ReprojectBoundaries;
  const bool boundaryReplay=operation==Operation::ReprojectBoundaries;
  const bool boundaryFluidStep=operation==Operation::BoundaryFluidStep;
  const bool sampleNativePoints=operation==Operation::SampleNativePointAttribute;
  const bool smoke = operation == Operation::Simulate || operation == Operation::SimulateSequence;
  const bool splat = operation == Operation::SplatDensity || operation == Operation::SplatVelocity || operation == Operation::SplatRadius;
  const bool rasterizePoints = splat || operation == Operation::PointsSdf;
  const bool viscosity = operation == Operation::Viscosity;
  const bool sources = operation == Operation::FluidSources;
  const bool nativePointAttributes=operation==Operation::EditPointAttributes || operation==Operation::PointSchema ||
      operation==Operation::PointAttribute || operation==Operation::PointGroup;
  const bool pointJsonOutput=nativePointAttributes && operation!=Operation::EditPointAttributes;
  const bool dense=operation==Operation::DenseImport || operation==Operation::DenseExport;
  const bool multiresolution=operation==Operation::MultiResolution || operation==Operation::MultiResolutionInfo || operation==Operation::MultiResolutionSample;
  const bool pointSelection=operation==Operation::SelectPoints || operation==Operation::DeletePoints || operation==Operation::CountPoints || operation==Operation::PointStatistics || operation==Operation::PointVoxelMask || sampleNativePoints || pointNeighbors || indexedPointMask || moveNativePoints || advectNativePoints || pointTransfer || diagnosticPoints;
  const bool pointQuery=operation==Operation::CountPoints || operation==Operation::PointStatistics;
  const bool sdfTransport = operation == Operation::AdvectSdf || operation == Operation::MorphSdf;
  const bool processPoints = operation == Operation::Scatter || operation == Operation::AdvectPoints ||
      operation == Operation::SamplePointVelocity || operation == Operation::SamplePointRadius;
  const bool source = operation != Operation::Sphere && operation != Operation::Velocity && operation != Operation::Platonic && !boundaryProjection && !boundaryFluidStep && !nanoPrimitive;
  const bool pair = operation == Operation::Union || operation == Operation::Intersection ||
      operation == Operation::Difference || operation == Operation::Advect || sdfTransport ||
      operation == Operation::AdvectPoints || operation == Operation::SamplePointVelocity ||
      operation == Operation::SamplePointRadius || operation == Operation::Project || operation==Operation::CompositeFields || operation==Operation::TopologyBoolean || maskedValues || sampleNativePoints || smoke || fastExtension || fracture || fieldAdvection || advectNativePoints;
  if (source) {
    ImGui::InputText(particleGeometry ? "Version 1 particle geometry JSON path" : fileAuthoring ? "Version 1 authoring JSON path" : physicalImage ? "Version 1 image scene JSON path" : advectNativePoints ? "Native points path" : "Input path", &draft.input);
    if (operation != Operation::Particles && operation != Operation::Points &&
        operation != Operation::Mesh && operation != Operation::MeshDistance && operation!=Operation::DenseImport && !viscosity && !sources && !fileAuthoring && !physicalImage && !particleGeometry && operation!=Operation::NanoInspect && operation!=Operation::NanoChecksum)
      ImGui::InputText("Input grid (optional)", &draft.inputGrid);
  }
  if (pair) {
    const char* label = operation == Operation::Project ? "Fluid domain path" :
        operation == Operation::Advect || operation == Operation::AdvectPoints || operation == Operation::AdvectSdf || smoke || fieldAdvection || advectNativePoints ? "Velocity path" :
        operation == Operation::MorphSdf ? "Target SDF path" :
        operation == Operation::SamplePointVelocity || operation == Operation::SamplePointRadius
            ? "Sample field path" : "Second input path";
    ImGui::InputText(label, &draft.secondInput);
    ImGui::InputText(advectNativePoints ? "Velocity grid (optional)" : "Second grid (optional)", &draft.secondGrid);
  }
  if (operation==Operation::EditPointAttributes) ImGui::InputText("Version 1 edits JSON path",&draft.secondInput);
  if (operation==Operation::PointAttribute || operation==Operation::PointGroup)
    ImGui::InputText(operation==Operation::PointAttribute ? "Attribute name" : "Group name",&draft.pointField);
  if (operation==Operation::PointStatistics) ImGui::InputText("Numeric attribute name",&draft.pointField);
  if(boundaryFluidStep) {
    auto& inputs=draft.boundaryFluidInputs;auto& b=inputs.boundary;auto& o=draft.boundaryFluid;
    auto state=util::pathToUtf8(b.state);
    if(ImGui::InputText("Boundary fluid canonical state path",&state))b.state=path(state);
    ImGui::InputText("Boundary fluid MAC selector",&b.velocityGrid);
    ImGui::InputText("Boundary fluid Bool-domain selector",&b.fluidGrid);
    ImGui::InputText("Boundary fluid face-kind selector",&b.faceKindGrid);
    ImGui::InputText("Boundary fluid fraction selector",&b.faceFractionGrid);
    ImGui::InputText("Boundary fluid prescribed-pressure selector",&b.facePressureGrid);
    ImGui::InputText("Boundary fluid wall-velocity selector",&b.wallVelocityGrid);
    const auto editField=[&](const char* fileLabel,const char* gridLabel,vdb::BoundaryProjectionField& field) {
      auto file=util::pathToUtf8(field.file);if(ImGui::InputText(fileLabel,&file))field.file=path(file);
      ImGui::InputText(gridLabel,&field.grid);
    };
    editField("Boundary fluid required density path","Boundary fluid density selector",inputs.density);
    bool temperature=inputs.temperature.has_value();
    if(ImGui::Checkbox("Use boundary fluid temperature input",&temperature))inputs.temperature=temperature?std::optional(vdb::BoundaryProjectionField{}):std::nullopt;
    if(inputs.temperature)editField("Boundary fluid temperature path","Boundary fluid temperature selector",*inputs.temperature);
    int initialVelocity=static_cast<int>(o.initialVelocity);
    if(ImGui::Combo("Boundary fluid initial MAC policy",&initialVelocity,"Transport actual stored MAC\0Require solenoidal entry and output\0"))o.initialVelocity=static_cast<vdb::BoundaryFluidInitialVelocity>(initialVelocity);
    editDouble("Boundary fluid total seconds [1e-6,3600]",o.timeStep);
    editCount("Boundary fluid requested intervals",o.substeps,64);
    editCount("Boundary fluid maximum accepted steps",o.maxSubsteps,64);
    editCount("Boundary fluid maximum trial attempts",o.maxStepAttempts,128);
    editDouble("Boundary fluid CFL (0,.5]",o.maxCfl);
    ImGui::InputScalarN("Boundary fluid WORLD gravity",ImGuiDataType_Double,o.gravity.data(),3,nullptr,nullptr,"%.17g");
    editDouble("Boundary fluid density buoyancy",o.buoyancyDensity);
    editDouble("Boundary fluid temperature buoyancy",o.buoyancyTemperature);
    ImGui::InputScalarN("Boundary fluid unit WORLD buoyancy direction",ImGuiDataType_Double,o.buoyancyDirection.data(),3,nullptr,nullptr,"%.17g");
    editDouble("Boundary fluid ambient Kelvin",o.ambientTemperature);
    editDouble("Boundary fluid reservoir density",o.reservoirDensity);
    bool reservoir=o.reservoirTemperature.has_value();
    if(ImGui::Checkbox("Override boundary fluid reservoir Kelvin",&reservoir))o.reservoirTemperature=reservoir?std::optional(o.ambientTemperature):std::nullopt;
    if(o.reservoirTemperature)editDouble("Boundary fluid reservoir Kelvin",*o.reservoirTemperature);
    editDouble("Boundary fluid residual/divergence tolerance",o.tolerance);
    editCount("Boundary fluid projection iterations per component",o.maxIterations,10000);
    ImGui::InputText("Boundary fluid output density name",&o.names.density);
    ImGui::InputText("Boundary fluid output temperature name",&o.names.temperature);
    ImGui::InputText("Boundary fluid output MAC name",&o.names.velocity);
    ImGui::InputText("Boundary fluid output pressure name",&o.names.pressure);
    ImGui::InputText("Boundary fluid output Bool-domain name",&o.names.fluid);
    ImGui::InputText("Boundary fluid output face-kind name",&o.names.faceKind);
    ImGui::InputText("Boundary fluid output fraction name",&o.names.faceFraction);
    ImGui::InputText("Boundary fluid output prescribed-pressure name",&o.names.facePressure);
    ImGui::InputText("Boundary fluid output wall-velocity name",&o.names.wallVelocity);
    ImGui::TextWrapped("Required canonical six fields retain exact maps and member IDs. Density is Float/Double FOG; optional ordinary temperature uses the same map and initializes to ambient Kelvin when omitted. Geometry and pressure ghost distances are fixed; walls have zero normal velocity. Pressure boundaries accept reservoir inflow and adjacent-cell outflow. First-order shared scalar flux is conservative up to stored rounding; MAC transport is advective, without momentum/energy/no-slip guarantees. Native endpoint retries discard whole trials. Every accepted actual dt adds a fresh prescribed-pressure impulse. Default mode measures and transports actual finite stored MAC divergence with no cleanup projection; strict require-solenoidal checks stored-only entry and successful output. Output9 restarts require all canonical six AND both scalars plus the submitted initial-velocity policy, with matching actual time partitions. No moving geometry, cut cells, evolving liquid phi, viscosity, sources or vorticity controls apply.");
  }

  if(boundaryProjection) {
    auto& settings=draft.boundaryProjection;auto& projection=settings.projection;
    const auto editBinding=[&](const char* fileLabel,const char* gridLabel,vdb::BoundaryProjectionField& binding) {
      auto text=util::pathToUtf8(binding.file);
      if(ImGui::InputText(fileLabel,&text))binding.file=path(text);
      if(!binding.file.empty())ImGui::InputText(gridLabel,&binding.grid);else binding.grid.clear();
    };
    if(boundaryReplay) {
      settings.missing=vdb::MissingFluidBoundary::ClosedWall;settings.airPressure=vdb::AirPressureLocation::NeighborCenter;
      settings.minimumInterfaceFraction=.01;settings.wallVelocityWorld={0,0,0};settings.ambientPressure=0;
      auto state=util::pathToUtf8(draft.boundaryReplay.state);
      if(ImGui::InputText("Canonical boundary state path",&state))draft.boundaryReplay.state=path(state);
      ImGui::InputText("Replay MAC velocity selector",&draft.boundaryReplay.velocityGrid);
      ImGui::InputText("Replay Bool fluid selector",&draft.boundaryReplay.fluidGrid);
      ImGui::InputText("Replay packed face-kind selector",&draft.boundaryReplay.faceKindGrid);
      ImGui::InputText("Replay packed fraction selector",&draft.boundaryReplay.faceFractionGrid);
      ImGui::InputText("Replay packed pressure selector",&draft.boundaryReplay.facePressureGrid);
      ImGui::InputText("Replay local wall-velocity selector",&draft.boundaryReplay.wallVelocityGrid);
      ImGui::TextWrapped("Replay requires six canonical fields with matching member IDs, exact maps and validated packed faces; old pressure is ignored/recomputed. Geometric boundary policies, phi, ambient pressure and wall constants are reset and taken from the saved table. dt, world force, solve limits and new output settings still apply. Nonuniform prescribed pressure gives a NEW pressure-driven impulse on every call; replay is not generally idempotent.");
    } else {
      auto& inputs=draft.boundaryInputs;
      editBinding("Required predictor velocity path","Predictor velocity selector",inputs.velocity);
      editBinding("Required fluid mask path","Fluid mask selector",inputs.fluid);
      editBinding("Solid mask path (optional)","Solid mask selector",inputs.solid);
      editBinding("Open-face mask path (optional)","Open-face mask selector",inputs.open);
      editBinding("Air-cell mask path (optional)","Air-cell mask selector",inputs.air);
      int missing=static_cast<int>(settings.missing);
      if(ImGui::Combo("Missing fluid neighbor",&missing,"Closed wall\0Open FACE pressure (theta .5)\0Air CENTER pressure (theta 1)\0Free-surface pressure from phi\0"))settings.missing=static_cast<vdb::MissingFluidBoundary>(missing);
      if(!inputs.air.file.empty()) {
        int air=static_cast<int>(settings.airPressure);
        if(ImGui::Combo("Explicit air pressure location",&air,"Neighbor center\0Level-set interface\0"))settings.airPressure=static_cast<vdb::AirPressureLocation>(air);
      } else settings.airPressure=vdb::AirPressureLocation::NeighborCenter;
      const bool surface=settings.missing==vdb::MissingFluidBoundary::FreeSurfacePressure || (!inputs.air.file.empty() && settings.airPressure==vdb::AirPressureLocation::LevelSetInterface);
      if(surface) {
        editBinding("Required liquid phi path","Liquid phi selector",inputs.liquidPhi);
        editDouble("Minimum effective interface fraction [1e-6,1]",settings.minimumInterfaceFraction);
      } else {inputs.liquidPhi={};settings.minimumInterfaceFraction=.01;}
      editBinding("World or MAC wall-velocity path (optional)","Wall-velocity selector",inputs.wallVelocity);
      if(inputs.wallVelocity.file.empty())ImGui::InputScalarN("Constant WORLD wall velocity",ImGuiDataType_Double,settings.wallVelocityWorld.data(),3,nullptr,nullptr,"%.17g");
      else settings.wallVelocityWorld={0,0,0};
      editBinding("Prescribed scalar pressure path (optional)","Prescribed pressure selector",inputs.pressure);
      if(inputs.pressure.file.empty())editDouble("Constant ambient physical pressure",settings.ambientPressure);else settings.ambientPressure=0;
      ImGui::TextWrapped("Fluid/solid/open/air masks must be disjoint active true or positive regions on the exact uniform orthogonal fluid map. Explicit regions override missing-neighbor policy. Open pressure is imposed at the physical face, while air pressure uses its declared center or phi interface. Phi requires negative fluid and nonnegative air values. Fraction clamping moves both the effective interface and pressure sampling location and is reported; cells retain full volumes/areas. Wall velocity imposes only the normal component of a stair-step wall, not tangential no-slip or solid motion.");
    }
    editDouble("Boundary projection time step (positive seconds)",projection.timeStep);
    ImGui::InputScalarN("Boundary WORLD acceleration",ImGuiDataType_Double,projection.acceleration.data(),3,nullptr,nullptr,"%.17g");
    editDouble("Full residual/divergence relative tolerance",projection.tolerance);
    editCount("Iterations per connected component",projection.maxIterations,10000);
    editBudget("Maximum fluid cells",projection.maxFluidCells);
    if(ImGui::Checkbox("Write canonical boundary table",&settings.writeBoundaryState) && settings.writeBoundaryState)projection.writeDomain=true;
    if(ImGui::Checkbox("Write Bool fluid domain",&projection.writeDomain) && !projection.writeDomain)settings.writeBoundaryState=false;
    ImGui::InputText("Boundary output MAC velocity name",&projection.velocityGridName);
    ImGui::InputText("Boundary output physical pressure name",&projection.pressureGridName);
    ImGui::InputText("Boundary output Bool fluid name",&projection.domainGridName);
    ImGui::InputText("Boundary output packed face-kind name",&settings.faceKindGridName);
    ImGui::InputText("Boundary output packed fraction name",&settings.faceFractionGridName);
    ImGui::InputText("Boundary output packed pressure name",&settings.facePressureGridName);
    ImGui::InputText("Boundary output MAC wall-velocity name",&settings.wallVelocityGridName);
    ImGui::TextWrapped("All seven native names must remain distinct even when reduced output is selected. WORLD fields supply raw XYZ physical velocity for every recognized vector tag; no tag-driven position translation or normalization occurs. Native MAC uses local negative faces in voxels/second, with world velocity Jacobian times local velocity. Pressure is unit-density world-distance squared/second squared. Independent velocity/pressure field maps must resolve the fluid faces. Closed components require compatible wall flux and a zero gauge without hidden mean subtraction. Stored physical pressure and stored-face divergence are checked. This fixed-domain inviscid operation is separate from an open/free-surface timestep, M05 restart, tangential viscosity, cut cells and moving geometry.");
  }
  if(diagnostics) {
    auto& settings=draft.diagnostics;
    int profile=static_cast<int>(settings.profile);
    if(ImGui::Combo("Diagnostic profile",&profile,"Stored values\0Symmetric level set\0Normalized fog [0,1]\0Native PointData\0")) {
      settings.profile=static_cast<vdb::DiagnosticProfile>(profile);
      if(settings.profile!=vdb::DiagnosticProfile::SymmetricLevelSet) {
        settings.gradient=vdb::DiagnosticGradient::None;settings.gradientNormRange={.5,1.5};settings.footprint=vdb::DiagnosticFootprint::RequireActive;
      }
      if(settings.profile!=vdb::DiagnosticProfile::SymmetricLevelSet && settings.profile!=vdb::DiagnosticProfile::NormalizedFog)settings.magnitudeTolerance=0;
      if(settings.profile!=vdb::DiagnosticProfile::PointData) {settings.pointAttributes.clear();settings.pointGroups={};}
      else settings.rangeScope=vdb::DiagnosticScope::Active;
    }
    bool range=settings.componentRange.has_value();
    if(ImGui::Checkbox("Check inclusive component interval",&range)) {
      settings.componentRange=range?std::optional<std::array<double,2>>(std::array<double,2>{0,1}):std::nullopt;
      if(!range)settings.rangeScope=vdb::DiagnosticScope::Active;
    }
    if(settings.componentRange) {
      ImGui::InputScalarN("Inclusive raw component minimum/maximum",ImGuiDataType_Double,settings.componentRange->data(),2,nullptr,nullptr,"%.17g");
      if(settings.profile!=vdb::DiagnosticProfile::PointData) {
        int scope=static_cast<int>(settings.rangeScope);
        if(ImGui::Combo("Component interval scope",&scope,"Active values\0All stored values and background\0"))settings.rangeScope=static_cast<vdb::DiagnosticScope>(scope);
      } else ImGui::TextWrapped("For PointData the interval applies componentwise to selected numeric columns and group-selected records, including inactive-voxel records. P has separate layout/offset checks; field active/stored range scope is inapplicable.");
    }
    if(settings.profile==vdb::DiagnosticProfile::SymmetricLevelSet) {
      int gradient=static_cast<int>(settings.gradient);
      if(ImGui::Combo("Distance gradient check",&gradient,"None\0First-order Godunov (7 axial samples)\0WENO interface Godunov (19 axial samples)\0")) {
        settings.gradient=static_cast<vdb::DiagnosticGradient>(gradient);
        if(settings.gradient==vdb::DiagnosticGradient::None) {settings.gradientNormRange={.5,1.5};settings.footprint=vdb::DiagnosticFootprint::RequireActive;}
      }
      if(settings.gradient!=vdb::DiagnosticGradient::None) {
        ImGui::InputScalarN("Gradient norm bounds (UNSQUARED)",ImGuiDataType_Double,settings.gradientNormRange.data(),2,nullptr,nullptr,"%.17g");
        int footprint=static_cast<int>(settings.footprint);
        if(ImGui::Combo("Gradient footprint coverage",&footprint,"Require active samples\0Allow native stored continuation\0"))settings.footprint=static_cast<vdb::DiagnosticFootprint>(footprint);
      }
      ImGui::TextWrapped("Symmetric level-set checks require Float/Double world-invariant tags, a uniform orthogonal affine map, positive at-least-three-voxel background, active band ranges, no active tiles and signed inactive-band magnitudes. Gradient bounds are world-unit norm bounds, not squared thresholds. Missing/nonfinite footprints are Unavailable rather than Pass. WENO classifies native six-face zero crossings; explicit finite stored continuation is counted and does not claim active coverage. These are diagnostics, not a distance or closed-surface certificate.");
    }
    if(settings.profile==vdb::DiagnosticProfile::SymmetricLevelSet || settings.profile==vdb::DiagnosticProfile::NormalizedFog)
      editDouble("Absolute inactive/background magnitude tolerance",settings.magnitudeTolerance);
    if(settings.profile==vdb::DiagnosticProfile::NormalizedFog)
      ImGui::TextWrapped("Normalized fog means native density in [0,1] with zero inactive/background values within the absolute tolerance. Physical density above one may be valid for another workflow; this explicit profile does not certify all physical fog.");
    if(settings.profile==vdb::DiagnosticProfile::PointData) {
      std::optional<size_t> remove;
      for(size_t index=0;index<settings.pointAttributes.size();++index) {
        ImGui::PushID(static_cast<int>(index));
        ImGui::InputText("Diagnostic numeric point column",&settings.pointAttributes[index]);ImGui::SameLine();
        if(ImGui::Button("Remove column"))remove=index;
        ImGui::PopID();
      }
      if(remove)settings.pointAttributes.erase(settings.pointAttributes.begin()+static_cast<std::ptrdiff_t>(*remove));
      ImGui::BeginDisabled(settings.pointAttributes.size()>=std::min<size_t>(1024,draft.pointAttributes.maxAttributes));
      if(ImGui::Button("Add diagnostic point column"))settings.pointAttributes.emplace_back();
      ImGui::EndDisabled();
      ImGui::TextWrapped("P/layout are always checked before counts or payload handles; P is a local Vec3f offset, not a world-position certificate. Optional unique numeric stride-one columns use their registered runtime codecs, raw component values and exact native integer comparisons. Missing/unsupported chosen columns are Unavailable. Unrelated opaque columns are not declared corrupt merely because they are present; native string dictionary indices are checked before dereferencing. Unsafe layout disables dependent decoding. An empty numeric list checks P/layout only.");
    }
    if(operation==Operation::DiagnosticMask)ImGui::InputText("Bool failure mask grid name",&settings.maskGridName);
    ImGui::TextWrapped("The job succeeds when a report is produced, independently of Pass/Fail/Unavailable and coverage. Counts distinguish leaf voxels, compact tiles/tile volume, selected points, background and derivative footprints. The optional Bool mask unions unique finite failure locations on the exact source map; points use ORIGINAL stored voxels. Background/schema/global failures have no finite location, and missing derivative footprints are not marked failed. An empty mask does NOT imply all checks passed. No repair occurs; source files stay unchanged.");
  }
  if(fieldResampling) {
    auto& settings=draft.fieldResampling;
    int interpolation=static_cast<int>(settings.interpolation),activity=static_cast<int>(settings.activity),vectors=static_cast<int>(settings.vectorValues);
    if(ImGui::Combo("Field interpolation",&interpolation,"Nearest (all nine native types)\0Linear (real scalar/vector)\0Quadratic (real scalar/vector)\0"))
      settings.interpolation=static_cast<vdb::FieldResamplingInterpolation>(interpolation);
    if(ImGui::Combo("Resampled activity",&activity,"Nearest source cell\0Any nonzero stencil contributor\0"))
      settings.activity=static_cast<vdb::FieldResamplingActivity>(activity);
    if(ImGui::Combo("World vector values",&vectors,"Apply native semantic deformation\0Preserve raw world components\0"))
      settings.vectorValues=static_cast<vdb::FieldVectorTransform>(vectors);
    constexpr const char* worldRows[]={"Field world row X (including translation)","Field world row Y (including translation)","Field world row Z (including translation)"};
    for(size_t row=0;row<3;++row)
      ImGui::InputScalarN(worldRows[row],ImGuiDataType_Double,settings.worldFromWorld.data()+4*row,4,nullptr,nullptr,"%.17g");
    bool target=settings.targetIndexToWorld.has_value();
    if(ImGui::Checkbox("Set explicit field target map",&target))
      settings.targetIndexToWorld=target?std::optional<std::array<double,16>>(std::array<double,16>{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}):std::nullopt;
    if(settings.targetIndexToWorld) {
      constexpr const char* targetRows[]={"Field target row X (including translation)","Field target row Y (including translation)","Field target row Z (including translation)"};
      for(size_t row=0;row<3;++row)
        ImGui::InputScalarN(targetRows[row],ImGuiDataType_Double,settings.targetIndexToWorld->data()+4*row,4,nullptr,nullptr,"%.17g");
    }
    ImGui::TextWrapped("Matrices multiply COLUMN vectors, with translation in the final column and a fixed affine final row. Target omission retains the exact source map; changing only the target preserves vector components. Nonidentity world deformation transforms invariant/relative/absolute/covariant/normalized vectors according to native semantics, including infinite background; raw policy is explicit. Vec3I semantic changes require exact signed-axis power-of-two maps and integral results; normalized Vec3I must remain zero or axis aligned. Nearest preserves exact integer/Boolean values and independent activity, with half ties toward the greater integer. Floating linear/quadratic include inactive/background samples; quadratic lobes can be negative. SDF, MAC and local vectors reject. Dense conservative support/leaf caps, precision/range errors and negative fog overshoot reject. This is point sampling without antialiasing, mass conservation or distance repair.");
  }
  if(pointTransfer) {
    auto& settings=draft.pointTransfer;
    ImGui::InputText("Numeric source point attribute",&draft.pointField);
    int mode=static_cast<int>(settings.mode),storage=static_cast<int>(settings.storage),companion=static_cast<int>(settings.companion),integers=static_cast<int>(settings.integers);
    if(ImGui::Combo("Point transfer reduction",&mode,"Weighted sum\0Weighted mean\0"))settings.mode=static_cast<vdb::PointTransferMode>(mode);
    if(ImGui::Combo("Transferred field storage",&storage,"Double\0Float (explicit rounding)\0"))settings.storage=static_cast<vdb::PointTransferStorage>(storage);
    if(ImGui::Combo("Transfer companion",&companion,"Weight sum\0Density per world volume\0"))settings.companion=static_cast<vdb::PointTransferCompanion>(companion);
    if(ImGui::Combo("Native Int64 conversion",&integers,"Reject inexact conversion\0Allow and count rounded conversion\0"))settings.integers=static_cast<vdb::PointTransferIntegers>(integers);
    ImGui::InputText("Transfer companion grid name (optional)",&settings.companionGridName);
    bool attributeWeight=!settings.weightAttribute.empty();
    if(ImGui::Checkbox("Use a numeric point weight attribute",&attributeWeight)) {
      settings.constantMass=1;settings.weightAttribute=attributeWeight?"pscale":"";
    }
    if(attributeWeight) {
      ImGui::InputText("Positive scalar weight attribute",&settings.weightAttribute);
      settings.constantMass=1;
    } else editDouble("Constant mass per selected point",settings.constantMass);
    constexpr const char* rows[]={"Transfer target row X (including translation)","Transfer target row Y (including translation)","Transfer target row Z (including translation)"};
    for(size_t row=0;row<3;++row)
      ImGui::InputScalarN(rows[row],ImGuiDataType_Double,settings.targetIndexToWorld.data()+4*row,4,nullptr,nullptr,"%.17g");
    ImGui::TextWrapped("Eight-center CIC transfer with an explicit uniform orthogonal target map: COLUMN vectors, translation in the final column, fixed affine final row. Target coordinates set deposition, not point deformation. Numeric scalar/Vec3 columns are raw stored components: no velocity, normal, position or radius transformation occurs. Weighted mean divides by positive weights; weighted sum retains cancellation. The always-Double companion stores weights or mass per actual world voxel volume. Zero-valued cancelled cells retain positive-weight activity. Float field storage and lossy Int64 conversion are explicit choices with diagnostics. P, group backing, strings and chosen dynamic/strided columns reject.");
  }
  if(surfaceFields) {
    int mode=static_cast<int>(draft.surfaceFieldOperation);
    if(ImGui::Combo("Surface field",&mode,"Mean curvature\0Gaussian curvature\0Outward normal\0Approximate closest point\0"))
      draft.surfaceFieldOperation=static_cast<vdb::SurfaceFieldOperation>(mode);
    ImGui::Checkbox("Promote surface output to Double",&draft.surfaceFields.promoteToDouble);
    ImGui::InputText("Surface validity grid name",&draft.surfaceFields.validityGridName);
    ImGui::TextWrapped("Writes one field plus a Bool validity grid on the exact source map. Full active derivative footprints are required; invalid cells remain inactive zero. Mean/Gaussian curvature use inverse world length/area, normals are outward WORLD vectors, and closest-point values are approximate absolute WORLD positions evaluated from the source cell, not exact surface projections. Double promotion helps remote positions; local gradient quality does not certify closure or distance accuracy.");
  }
  if(advectNativePoints) {
    auto& settings=draft.nativePointAdvection;
    editDouble("Total signed native point time (seconds)",settings.timeStep);
    editCount("Equal native point substeps",settings.steps,64);
    int integration=static_cast<int>(settings.integration),layout=static_cast<int>(settings.velocityLayout);
    if(ImGui::Combo("Native point integrator",&integration,"Midpoint RK2\0Classical RK4\0"))settings.integration=static_cast<vdb::NativePointIntegration>(integration);
    if(ImGui::Combo("Native point velocity layout",&layout,"Auto from native class\0Raw WORLD XYZ physical velocity\0Tagged local negative-face MAC\0"))settings.velocityLayout=static_cast<vdb::NativePointVelocityLayout>(layout);
    editDouble("Maximum step L2 travel in VELOCITY index voxels",settings.maxStepTravelVelocityVoxels);
    editDouble("Maximum total L2 travel in VELOCITY index voxels",settings.maxTravelVelocityVoxels);
    bool target=settings.targetIndexToWorld.has_value();
    if(ImGui::Checkbox("Repartition advected points into explicit target map",&target))settings.targetIndexToWorld=target?std::optional<std::array<double,16>>(std::array<double,16>{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}):std::nullopt;
    if(settings.targetIndexToWorld) {
      constexpr const char* rows[]={"Advected target row X (including translation)","Advected target row Y (including translation)","Advected target row Z (including translation)"};
      for(size_t row=0;row<3;++row)ImGui::InputScalarN(rows[row],ImGuiDataType_Double,settings.targetIndexToWorld->data()+4*row,4,nullptr,nullptr,"%.17g");
    }
    editDouble("Maximum advected P codec error in target voxels",settings.maxPositionErrorVoxels);
    bool merge=!settings.rejectActivityMerges;
    if(ImGui::Checkbox("Allow counted OR of advected point activity",&merge))settings.rejectActivityMerges=!merge;
    ImGui::TextWrapped("Static Double RK trajectories precede one native repartition. Signed total time is divided into equal substeps. All five already-WORLD vector tags use raw XYZ distance/second without translating absolute vectors or normalizing normals. Tagged local negative-face MAC reconstructs each component at +.5 and applies J to get WORLD velocity. Limits measure L2 travel in the VELOCITY grid index frame, not point-grid voxels. Target matrices multiply COLUMN positions, with translation in the final column and uniform orthogonal axes. Groups select motion; all inactive records and arbitrary fixed native attributes/codecs, IDs, strings and groups survive. Only P changes: v/pscale, radius and orientation are not updated. Zero time can skip integration while a target change still repartitions. Final codec error compares to computed RK endpoints; no exact ODE, mass-conservation or incompressibility certificate is implied. Dynamic-stride movement and time-varying flow remain separate.");
  }
  if(moveNativePoints) {
    auto& settings=draft.pointMotion;
    constexpr const char* worldRows[]={"Point world row X (including translation)","Point world row Y (including translation)","Point world row Z (including translation)"};
    for(size_t row=0;row<3;++row)
      ImGui::InputScalarN(worldRows[row],ImGuiDataType_Double,settings.worldFromWorld.data()+4*row,4,nullptr,nullptr,"%.17g");
    bool target=settings.targetIndexToWorld.has_value();
    if(ImGui::Checkbox("Repartition into explicit point target map",&target))
      settings.targetIndexToWorld=target?std::optional<std::array<double,16>>(std::array<double,16>{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}):std::nullopt;
    if(settings.targetIndexToWorld) {
      constexpr const char* targetRows[]={"Point target row X (including translation)","Point target row Y (including translation)","Point target row Z (including translation)"};
      for(size_t row=0;row<3;++row)
        ImGui::InputScalarN(targetRows[row],ImGuiDataType_Double,settings.targetIndexToWorld->data()+4*row,4,nullptr,nullptr,"%.17g");
    }
    editDouble("Maximum point error in target voxels",settings.maxPositionErrorVoxels);
    bool merge=!settings.rejectActivityMerges;
    if(ImGui::Checkbox("Allow counted OR of mixed point activity",&merge))settings.rejectActivityMerges=!merge;
    ImGui::TextWrapped("Matrices multiply COLUMN vectors with translation in the final column; the affine final row stays fixed. World deformation may be singular, while an explicit target map must be uniform orthogonal. Groups select P deformation; all records, including inactive records, are retained and repartitioned. Native order may change. v, pscale, arbitrary attributes/codecs, group bits and metadata are preserved without velocity/radius/orientation transformation. Fixed P codecs must meet the explicit target-voxel tolerance; exact no-ops retain their original encoding. Default behavior rejects merged active/inactive records.");
  }
  if(rebuildLevelSet) {
    auto& settings=draft.levelSetRebuild;
    editDouble("Rebuild source isovalue",settings.isovalue);
    editDouble("Rebuild exterior width in target voxels",settings.exteriorWidth);
    editDouble("Rebuild interior width in target voxels",settings.interiorWidth);
    bool target=settings.targetIndexToWorld.has_value();
    if(ImGui::Checkbox("Set explicit rebuild target transform",&target))
      settings.targetIndexToWorld=target?std::optional<std::array<double,16>>(std::array<double,16>{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}):std::nullopt;
    if(settings.targetIndexToWorld) {
      constexpr const char* rows[]={"Target matrix row X (including translation)","Target matrix row Y (including translation)","Target matrix row Z (including translation)"};
      for(size_t row=0;row<3;++row)ImGui::InputScalarN(rows[row],ImGuiDataType_Double,settings.targetIndexToWorld->data()+4*row,4,nullptr,nullptr,"%.17g");
    }
    ImGui::TextWrapped("Rebuilds a bounded closed Float/Double contour into world signed distances with asymmetric target-voxel bands. The optional matrix uses column vectors and translation in its final column; target axes must be uniform and orthogonal. Omission retains the exact source map. Native mesh precision is checked; rebuilding can merge overlaps or close bubbles and does not preserve topology.");
  }
  if(fieldAdvection) {
    auto& settings=draft.fieldAdvection;
    editDouble("Total signed advection time (seconds)",settings.timeStep);
    editCount("Field advection steps",settings.steps,64);
    int scheme=static_cast<int>(settings.scheme),limiter=static_cast<int>(settings.limiter),layout=static_cast<int>(settings.velocityLayout);
    if(ImGui::Combo("Field integration scheme",&scheme,"Semi-Lagrangian Euler\0Midpoint\0RK3\0RK4\0MacCormack\0BFECC\0"))settings.scheme=static_cast<vdb::FieldAdvectionScheme>(scheme);
    if(settings.scheme==vdb::FieldAdvectionScheme::MacCormack || settings.scheme==vdb::FieldAdvectionScheme::Bfecc) {
      if(ImGui::Combo("Correction limiter",&limiter,"None (may overshoot)\0Clamp each component\0Revert each component\0"))settings.limiter=static_cast<vdb::FieldAdvectionLimiter>(limiter);
    }
    if(ImGui::Combo("Velocity layout",&layout,"Auto from grid class\0Collocated world velocity\0Tagged native index MAC\0"))settings.velocityLayout=static_cast<vdb::FieldVelocityLayout>(layout);
    editDouble("Maximum source voxels traveled per step",settings.maxCfl);
    const auto maskUtf8=settings.mask.u8string();std::string mask(maskUtf8.begin(),maskUtf8.end());
    if(ImGui::InputText("Final activity mask (optional)",&mask)) {
      settings.mask=path(mask);if(settings.mask.empty())settings.maskGrid.clear();
    }
    if(!settings.mask.empty())ImGui::InputText("Advection mask grid (optional)",&settings.maskGrid);
    ImGui::Checkbox("Permit active source tile voxelization",&settings.voxelizeActiveTiles);
    ImGui::TextWrapped("Transports passive Float/Double or world Vec3 fields. The optional mask selects final writes; intermediate traces retain their full support. Unselected original values and activity survive. Correction can overshoot; diagnostics do not establish mass conservation. Total source travel is bounded to 256 voxels.");
  }
  if(potentialFlow) {
    auto& settings=draft.potentialFlow;
    ImGui::InputScalarN("Far-field world velocity",ImGuiDataType_Double,settings.farFieldVelocity.data(),3,nullptr,nullptr,"%.17g");
    ImGui::InputScalarN("Collider world velocity",ImGuiDataType_Double,settings.colliderVelocity.data(),3,nullptr,nullptr,"%.17g");
    editCount("Exterior shell steps",settings.exteriorWidth,128);
    editBudget("Maximum flow solver cells",settings.maxSolverVoxels);
    editCount("Potential solver iterations",settings.maxIterations,10000);
    editDouble("Stored-potential residual tolerance",settings.tolerance);
    ImGui::InputText("Flow velocity grid name",&settings.velocityGridName);
    ImGui::InputText("Flow potential grid name",&settings.potentialGridName);
    ImGui::InputText("Flow domain grid name",&settings.domainGridName);
    ImGui::InputText("Flow Neumann grid name",&settings.neumannGridName);
    ImGui::TextWrapped("Solves an irrotational exterior flow around a signed collider with uniform world velocities. The 18-neighbor shell must exceed ceil(source band width)+1. Every component needs an outer Dirichlet face; closed pure-Neumann cavities reject. Boundary velocity uses a projected-normal voxel approximation. Interior divergence/curl are diagnostics where complete stencils exist.");
  }
  if(pointNeighbors) {
    auto& settings=draft.pointNeighbors;int mode=static_cast<int>(settings.mode);
    if(ImGui::Combo("Neighbor criterion",&mode,"Center within query radius\0Particle intersects query sphere\0")) {
      settings.mode=static_cast<vdb::PointNeighborMode>(mode);
      if(settings.mode==vdb::PointNeighborMode::CentersWithinRadius)settings.radiusAttribute.clear();
    }
    editDouble("Query world radius",draft.distance);
    editDouble("Index voxel size in world units",settings.indexVoxelSize);
    if(settings.mode==vdb::PointNeighborMode::ParticleIntersections) {
      ImGui::InputText("Particle radius attribute (empty uses constant)",&settings.radiusAttribute);
      if(settings.radiusAttribute.empty())editDouble("Constant world particle radius",settings.constantRadius);
      editCount("Maximum particle atlas levels",settings.maxAtlasLevels,32);
    }
    ImGui::TextWrapped("Inclusive world-distance queries include selected inactive-voxel points and retain original native references. Radius zero selects coincident centers or particles containing the query. Results sort by distance and original ordinal; group filters below retain native identities.");
  }
  if(indexedPointMask) {
    auto& settings=draft.indexedPointMask;
    editDouble("Indexed mask world voxel size",settings.voxelSize);
    ImGui::InputScalarN("Indexed mask world origin",ImGuiDataType_Double,settings.originWorld.data(),3,nullptr,nullptr,"%.17g");
    if(ImGui::Checkbox("Include indexed Int64 counts",&settings.includeCounts) && !settings.includeCounts)settings.countGridName.clear();
    if(settings.includeCounts)ImGui::InputText("Indexed count grid name (optional)",&settings.countGridName);
    ImGui::TextWrapped("Re-buckets selected native point centers onto this explicit world map. Inactive-voxel records participate. Optional Int64 counts share the output file. Point radii do not expand occupancy.");
  }
  if(regions) {
    using Op=vdb::RegionsOperation;int selectedRegion=static_cast<int>(draft.regionsOperation);
    if(ImGui::Combo("Region operation",&selectedRegion,"Active components\0Strict signed interior\0Enclosed solid and cavities\0Positive cavities\0")) {
      draft.regionsOperation=static_cast<Op>(selectedRegion);
      if(draft.regionsOperation==Op::ActiveComponents)draft.regions.isovalue=0;
      else draft.regions.maxComponents=64;
    }
    if(draft.regionsOperation==Op::ActiveComponents)editCount("Maximum region components",draft.regions.maxComponents,1024);
    else editDouble("Region signed isovalue",draft.regions.isovalue);
    ImGui::TextWrapped("Writes native Bool occupancy masks. Components use face-connected activity and deterministic names. Interior uses the represented signed field; cavities are positive cells disconnected from the finite exterior. These voxel topology results do not certify a source mesh was watertight.");
  }
  if(surfaceQuery)ImGui::TextWrapped("Queries the nearest extracted surface point cloud. Closest position and distance are approximate. Signed samples, normals and curvatures are reported only where their active neighborhoods support them; curvature is measured at the returned point.");
  if(fracture) {
    auto& settings=draft.fracture;
    ImGui::Checkbox("Later cutters also cut earlier fragments",&settings.cutterOverlap);
    ImGui::Checkbox("Split zero-surface components",&settings.segment);
    editCount("Maximum fracture pieces",settings.maxPieces,256);
    std::optional<size_t> removed;
    for(size_t index=0;index<settings.instances.size();++index) {
      ImGui::PushID(int(index));auto& instance=settings.instances[index];
      ImGui::Text("Cutter instance %zu",index);
      ImGui::InputScalarN("World translation",ImGuiDataType_Double,instance.translation.data(),3,nullptr,nullptr,"%.17g");
      ImGui::InputScalarN("World XYZ rotation in radians",ImGuiDataType_Double,instance.rotation.data(),3,nullptr,nullptr,"%.17g");
      bool pivot=instance.pivot.has_value();
      if(ImGui::Checkbox("Set world rotation pivot",&pivot))instance.pivot=pivot?std::optional<std::array<double,3>>(std::array<double,3>{0,0,0}):std::nullopt;
      if(instance.pivot)ImGui::InputScalarN("World pivot",ImGuiDataType_Double,instance.pivot->data(),3,nullptr,nullptr,"%.17g");
      if(ImGui::Button("Remove cutter instance"))removed=index;
      ImGui::PopID();
    }
    if(removed)settings.instances.erase(settings.instances.begin()+*removed);
    ImGui::BeginDisabled(settings.instances.size()>=32);
    if(ImGui::Button("Add rigid cutter instance"))settings.instances.emplace_back();
    ImGui::EndDisabled();
    ImGui::TextWrapped("No instances uses one identity cutter. Translation is in world units; rotations apply X, then Y, then Z. Pivot defaults to the source map origin. Fracture retains represented small pieces and verifies negative voxel accounting. Signed distances and continuous volume remain approximate. Optional surface segmentation rejects negative-exterior cavity surfaces.");
  }
  if(fastSweeping) {
    using Op=vdb::FastSweepingOperation;auto& settings=draft.fastSweeping;
    if(!fastExtension) {
      int mode=static_cast<int>(draft.fastSweepingOperation);
      if(ImGui::Combo("Fast sweeping operation",&mode,"Fog to distance\0Reinitialize distances\0Dilate distance band\0Extend distance into mask\0Topology to distance\0")) {
        draft.fastSweepingOperation=static_cast<Op>(mode);
        settings.isoValue=mode==int(Op::FogToSdf)?.5:0;settings.side=vdb::FastSweepingSide::All;
        settings.dilation=1;settings.neighbors=vdb::FastSweepingNeighbors::Face;
        settings.topologyHalfWidth=3;settings.topologyClosing=0;settings.topologyDilation=0;settings.topologySmoothing=0;
        if(mode==int(Op::DilateSdf) || mode==int(Op::TopologyToSdf)) {settings.mask.clear();settings.maskGrid.clear();}
      }
    } else if(ImGui::Checkbox("Extension source is fog",&draft.fastSweepingFog))settings.isoValue=draft.fastSweepingFog?.5:0;
    const auto mode=draft.fastSweepingOperation;
    const bool topology=!fastExtension && mode==Op::TopologyToSdf;
    if(topology) {
      editCount("Topology half width",settings.topologyHalfWidth,16);settings.topologyHalfWidth=std::max(3u,settings.topologyHalfWidth);
      ImGui::InputScalar("Topology closing steps",ImGuiDataType_U32,&settings.topologyClosing);settings.topologyClosing=std::min(8u,settings.topologyClosing);
      ImGui::InputScalar("Topology dilation steps",ImGuiDataType_U32,&settings.topologyDilation);settings.topologyDilation=std::min(8u,settings.topologyDilation);
      ImGui::InputScalar("Topology smoothing steps",ImGuiDataType_U32,&settings.topologySmoothing);settings.topologySmoothing=std::min(8u,settings.topologySmoothing);
    } else {
      editCount("Eight-direction sweep iterations",settings.iterations,32);
      if(fastExtension || mode==Op::FogToSdf || mode==Op::ReinitializeSdf)editDouble("Sweeping source isovalue",settings.isoValue);
    }
    if(fastExtension || mode==Op::DilateSdf) {
      int side=static_cast<int>(settings.side);
      if(ImGui::Combo("Sweeping side",&side,"Both sides\0Greater than source isovalue\0Less than source isovalue\0"))settings.side=static_cast<vdb::FastSweepingSide>(side);
    }
    if(!fastExtension && mode==Op::DilateSdf) {
      editCount("Distance band dilation",settings.dilation,64);int neighbors=static_cast<int>(settings.neighbors);
      if(ImGui::Combo("Dilation neighbors",&neighbors,"Faces\0Faces and edges\0Faces, edges and vertices\0"))settings.neighbors=static_cast<vdb::FastSweepingNeighbors>(neighbors);
    }
    ImGui::Checkbox("Voxelize active fog/topology tiles",&settings.voxelizeActiveTiles);
    if(fastExtension || (mode!=Op::DilateSdf && mode!=Op::TopologyToSdf)) {
      const auto utf8=settings.mask.u8string();std::string maskText(utf8.begin(),utf8.end());
      if(ImGui::InputText("Sweeping activity mask path",&maskText)) {
        settings.mask=path(maskText);if(maskText.empty())settings.maskGrid.clear();
      }
      if(!settings.mask.empty())ImGui::InputText("Sweeping mask grid (optional)",&settings.maskGrid);
    }
    ImGui::TextWrapped("Computes approximate upwind distances over a bounded domain with fixed sweep iterations. Missing interface crossings and unreachable components fail. Mask extension unions activity; other mask uses restrict activity. Field extension writes distance and extension grids in one VDB. Matching maps and supported native precision are required; sweep count does not certify convergence.");
  }
  if(volumeSpheres) {
    auto& settings=draft.volumeSpheres;
    ImGui::InputScalar("Minimum fitted sphere count",ImGuiDataType_U32,&settings.minCount);
    editCount("Maximum fitted sphere count",settings.maxCount,4096);
    settings.minCount=std::min(settings.minCount,settings.maxCount);
    editCount("Sphere scatter candidates",settings.candidateCount,100000);
    ImGui::Checkbox("Allow overlapping fitted spheres",&settings.overlapping);
    editDouble("Minimum fitted world radius",settings.minRadiusWorld);
    editDouble("Maximum fitted world radius",settings.maxRadiusWorld);
    bool explicitIso=settings.isovalue.has_value();
    if(ImGui::Checkbox("Set sphere fitting isovalue",&explicitIso))
      settings.isovalue=explicitIso?std::optional<double>(0):std::nullopt;
    if(settings.isovalue)editDouble("Sphere fitting isovalue",*settings.isovalue);
    ImGui::TextWrapped("Exports a JSON sphere list in world units. Default isovalue is zero for signed distances and 0.5 for fog. Radius limits shrink or discard fitted spheres afterward. The result is approximate; packing optimality, coverage and exact containment are not guaranteed.");
  }
  if (sdfFilter) {
    using Op=vdb::SdfFilterOperation;
    int selectedFilter=static_cast<int>(draft.sdfFilterOperation);
    if(ImGui::Combo("SDF filter or tracker",&selectedFilter,
        "Gaussian\0Mean\0Median\0Mean curvature flow\0Laplacian flow\0Fillet concave edges\0World distance offset\0Normalize distances\0Track interface\0Prune signed field\0Resize narrow band\0Dilate narrow band\0Erode narrow band\0")) {
      draft.sdfFilterOperation=static_cast<Op>(selectedFilter);
      const auto mode=draft.sdfFilterOperation;
      if(mode!=Op::Gaussian && mode!=Op::Mean && mode!=Op::Median)draft.sdfFilter.width=1;
      if(mode!=Op::Offset)draft.sdfFilter.worldDistance=0;
      if(mode!=Op::ResizeBand)draft.sdfFilter.bandWidth=3;
      if(mode!=Op::DilateBand && mode!=Op::ErodeBand)draft.sdfFilter.bandLayers=1;
      if(mode!=Op::Normalize) {
        draft.sdfFilter.normalizationMaskInput.clear();draft.sdfFilter.normalizationMaskGrid.clear();
      }
      if(mode>Op::Offset) {
        draft.sdfFilter.maskInput.clear();draft.sdfFilter.maskGrid.clear();
        draft.sdfFilter.alphaMinimum=0;draft.sdfFilter.alphaMaximum=1;draft.sdfFilter.invertAlpha=false;
      }
    }
    const auto mode=draft.sdfFilterOperation;
    const bool band=mode==Op::ResizeBand || mode==Op::DilateBand || mode==Op::ErodeBand;
    auto& settings=draft.sdfFilter;
    editCount("SDF iterations",settings.iterations,32);
    if(mode==Op::Gaussian || mode==Op::Mean || mode==Op::Median)editCount("SDF kernel radius",settings.width,4);
    if(mode==Op::Offset)editDouble("Outward world distance per iteration",settings.worldDistance);
    if(mode==Op::ResizeBand)editCount("Target half width in source voxels",settings.bandWidth,64);
    if(mode==Op::DilateBand || mode==Op::ErodeBand)editCount("Band layers per iteration",settings.bandLayers,64);
    if(mode!=Op::Prune && mode!=Op::ErodeBand) {
      int spatial=static_cast<int>(settings.spatial),temporal=static_cast<int>(settings.temporal);
      if(ImGui::Combo("SDF spatial scheme",&spatial,"First order\0Second order\0Third order\0WENO5\0HJWENO5\0"))
        settings.spatial=static_cast<vdb::SdfSpatialScheme>(spatial);
      if(ImGui::Combo("SDF temporal scheme",&temporal,"TVD RK1\0TVD RK2\0TVD RK3\0"))
        settings.temporal=static_cast<vdb::SdfTemporalScheme>(temporal);
      ImGui::InputScalar("Normalizations per tracking pass",ImGuiDataType_U32,&settings.normalizationCount);
      settings.normalizationCount=std::min(64u,settings.normalizationCount);
    }
    if(mode!=Op::Normalize && !band) {
      int trim=static_cast<int>(settings.trim);
      if(ImGui::Combo("Out-of-band trimming",&trim,"Keep both sides\0Trim interior\0Trim exterior\0Trim both sides\0"))
        settings.trim=static_cast<vdb::SdfTrimMode>(trim);
    }
    if(mode<=Op::Offset) {
      const auto utf8=settings.maskInput.u8string();std::string maskText(utf8.begin(),utf8.end());
      if(ImGui::InputText("SDF alpha mask path (optional)",&maskText)) {
        settings.maskInput=path(maskText);
        if(maskText.empty()) {
          settings.maskGrid.clear();settings.alphaMinimum=0;settings.alphaMaximum=1;settings.invertAlpha=false;
        }
      }
      if(!settings.maskInput.empty()) {
        ImGui::InputText("SDF alpha grid (optional)",&settings.maskGrid);
        editDouble("SDF alpha minimum",settings.alphaMinimum);editDouble("SDF alpha maximum",settings.alphaMaximum);
        ImGui::Checkbox("Invert SDF alpha",&settings.invertAlpha);
      }
    }
    if(mode==Op::Normalize) {
      const auto utf8=settings.normalizationMaskInput.u8string();std::string maskText(utf8.begin(),utf8.end());
      if(ImGui::InputText("Normalization activity mask path (optional)",&maskText)) {
        settings.normalizationMaskInput=path(maskText);
        if(maskText.empty())settings.normalizationMaskGrid.clear();
      }
      if(!settings.normalizationMaskInput.empty())
        ImGui::InputText("Normalization activity grid (optional)",&settings.normalizationMaskGrid);
      ImGui::TextWrapped("The activity mask selects existing active distance voxels. Active zero/false values select; inactive values do not. Every unselected value and activity bit is retained. The mask must have the same affine map.");
    }
    ImGui::TextWrapped("Native Float/Double signed fields with uniform orthogonal voxels. Alpha modulates filtering; subsequent tracker normalization is global. Positive offsets expand outward. Band controls change distance-band width. Partial trimming can retain out-of-band values. Reported gradient error is an indicator and does not certify surface closure or distance validity.");
  }
  if (sdfBackground) {
    int op=static_cast<int>(draft.sdfBackgroundOperation);
    if(ImGui::Combo("Signed background mode",&op,"Replace backgrounds by existing signs\0Propagate narrow-band signs, then replace\0"))
      draft.sdfBackgroundOperation=static_cast<vdb::SdfBackgroundOperation>(op);
    bool outside=draft.sdfBackground.outsideWidth.has_value(),inside=draft.sdfBackground.insideWidth.has_value();
    if(ImGui::Checkbox("Set exterior world-distance width",&outside))
      draft.sdfBackground.outsideWidth=outside ? std::optional<double>(.3) : std::nullopt;
    if(draft.sdfBackground.outsideWidth) editDouble("Exterior width",*draft.sdfBackground.outsideWidth);
    if(ImGui::Checkbox("Use asymmetric interior width",&inside))
      draft.sdfBackground.insideWidth=inside ? std::optional<double>(.3) : std::nullopt;
    if(draft.sdfBackground.insideWidth) editDouble("Interior width magnitude",*draft.sdfBackground.insideWidth);
    ImGui::TextWrapped("Native Float/Double SDFs with uniform voxels. Exterior defaults to the source background; interior defaults to the requested exterior. Existing active distances remain exact and must fit the widths. Changing backgrounds does not expand the active band. Flood fill assumes a closed signed field and retains inactive-only branch sign seeds; it does not certify closure or reconstruct an arbitrary corrupted interior.");
  }
  if (maskedValues) {
    int op=static_cast<int>(draft.maskedValuesOperation);
    if(ImGui::Combo("Masked native edit",&op,"Activate\0Deactivate\0Intersect activity\0Clip inside\0Clip outside\0"))
      draft.maskedValuesOperation=static_cast<vdb::MaskedValuesOperation>(op);
    ImGui::TextWrapped("Second input supplies mask activity only. Active zero/false/NaN is selected. Activation/deactivation/intersection retain every native source value, including inactive values. Clipping resets the unselected side to inactive source background. Affine maps must match exactly. Source file/grid metadata and vector frames survive; output class is UNKNOWN. SDF repair is separate.");
  }
  if (valueTopology) {
    using Edit=vdb::ValueTopologyOperation;
    int op=static_cast<int>(draft.valueTopologyOperation);
    if(ImGui::Combo("Native edit",&op,"Activate exact value\0Deactivate exact value\0Prune bit-identical values\0Fill index box\0Clip to index box\0Replace exact inactive background\0")) {
      const auto work=draft.valueTopology.maxWorkItems;
      draft.valueTopology={}; draft.valueTopology.maxWorkItems=work;
      draft.valueTopologyOperation=static_cast<Edit>(op);
    }
    const auto selectedEdit=draft.valueTopologyOperation;
    if(selectedEdit!=Edit::PruneExact && selectedEdit!=Edit::ClipBox) {
      bool vector=draft.valueTopology.value.size()==3;
      if(ImGui::Checkbox("Three vector components",&vector)) draft.valueTopology.value.assign(vector ? 3 : 1,"0");
      for(size_t i=0;i<draft.valueTopology.value.size();++i) {
        ImGui::PushID(static_cast<int>(i)); ImGui::InputText("Native decimal / bool",&draft.valueTopology.value[i]); ImGui::PopID();
      }
    }
    if(selectedEdit==Edit::FillBox || selectedEdit==Edit::ClipBox) {
      ImGui::InputInt3("Inclusive index minimum",draft.valueTopology.minimum.data());
      ImGui::InputInt3("Inclusive index maximum",draft.valueTopology.maximum.data());
    }
    if(selectedEdit==Edit::FillBox) ImGui::Checkbox("Fill active",&draft.valueTopology.fillActive);
    ImGui::TextWrapped("Native values retain exact integer and signed-zero bits. Activation matches stored values only; implicit background stays inactive. Clip resets values outside the index box. Background replacement changes only exact inactive matches. Box growth is bounded before allocation. Ordinary/fog edits output class UNKNOWN; only exact pruning accepts SDF/staggered grids.");
  }
  if (operation==Operation::TopologyMask) {
    int op=static_cast<int>(draft.topologyOperation);
    if (ImGui::Combo("Mask operation",&op,"Active states\0Dilate\0Erode\0Open\0Close\0")) {
      draft.topologyOperation=static_cast<vdb::TopologyOperation>(op);
      if (draft.topologyOperation==vdb::TopologyOperation::ActiveMask) {
        const auto work=draft.topology.maxWorkItems; draft.topology={}; draft.topology.maxWorkItems=work;
      }
    }
    if (draft.topologyOperation!=vdb::TopologyOperation::ActiveMask) {
      int iterations=static_cast<int>(draft.topology.iterations);
      if (ImGui::InputInt("Lattice steps per phase",&iterations)) draft.topology.iterations=static_cast<uint32_t>(std::clamp(iterations,1,32));
      int neighbors=static_cast<int>(draft.topology.connectivity),tiles=static_cast<int>(draft.topology.tilePolicy);
      if (ImGui::Combo("Neighbors",&neighbors,"6 faces\0Faces and edges (18)\0Faces, edges and corners (26)\0"))
        draft.topology.connectivity=static_cast<vdb::TopologyConnectivity>(neighbors);
      if (ImGui::Combo("Active tiles",&tiles,"Preserve compact interiors\0Expand to leaf voxels\0Ignore tiles during edits\0"))
        draft.topology.tilePolicy=static_cast<vdb::TopologyTilePolicy>(tiles);
    }
  }
  if (operation==Operation::TopologyBoolean) {
    int op=static_cast<int>(draft.topologyBoolean);
    if (ImGui::Combo("Occupancy Boolean",&op,"Union\0Intersection\0Difference (A minus B)\0"))
      draft.topologyBoolean=static_cast<vdb::TopologyBoolean>(op);
  }
  if (topology) ImGui::TextWrapped("Creates a native MaskGrid from active states. Stored values, SDF interior signs and point attributes do not determine occupancy. Morphology uses index-lattice neighbors and preserves the exact affine map; Boolean operands require identical affine coefficients. Ignore-tiles morphology edits only leaf voxels, so tiled inputs need not obey whole-set open/close identities.");
  if (sampleNativePoints) {
    ImGui::InputText("Sampled attribute name",&draft.pointFieldSampling.attributeName);
    int edit=draft.pointFieldSampling.operation==vdb::PointEditOperation::Set ? 1 : 0;
    if (ImGui::Combo("Attribute write",&edit,"Add missing attribute\0Set exact existing type\0"))
      draft.pointFieldSampling.operation=edit ? vdb::PointEditOperation::Set : vdb::PointEditOperation::Add;
    int interpolation=static_cast<int>(draft.pointFieldSampling.interpolation),precision=static_cast<int>(draft.pointFieldSampling.precision);
    if (ImGui::Combo("Field interpolation",&interpolation,"Nearest\0Linear\0Quadratic\0"))
      draft.pointFieldSampling.interpolation=static_cast<vdb::PointFieldInterpolation>(interpolation);
    if (ImGui::Combo("Sampled precision",&precision,"Promote floating values to Double\0Preserve source floating type\0"))
      draft.pointFieldSampling.precision=static_cast<vdb::PointFieldPrecision>(precision);
    ImGui::TextWrapped("Samples selected points, including inactive-voxel records, at their world positions. Integer/Boolean fields require nearest sampling; native MAC supports nearest/linear world-vector reconstruction. Add initializes unselected records to zero; Set preserves their existing bits. P, groups, offsets and other attributes remain intact. Quadratic sampling can overshoot and reports unsupported dynamic ranges.");
  }
  if (operation==Operation::PointVoxelMask) {
    ImGui::Checkbox("Write native Int64 counts",&draft.pointMask.writeCounts);
    if (draft.pointMask.writeCounts) ImGui::InputText("Point count grid name",&draft.pointMask.countGridName);
    ImGui::TextWrapped("Native MaskGrid occupancy in the original stored point voxels, including inactive-voxel records selected by the groups below. The exact source transform is retained, including nonlinear maps. P, radius and attribute codecs are not reinterpreted. Optional Int64 counts share the output file and aggregate voxel/leaf-slot budgets. This is voxel occupancy, not sphere coverage.");
  }
  if (smoke) {
    ImGui::InputText("Fluid domain path", &draft.domainInput);
    ImGui::InputText("Domain grid (optional)", &draft.domainGrid);
    ImGui::InputText("Kelvin path (optional)", &draft.temperatureInput);
    if (!draft.temperatureInput.empty()) ImGui::InputText("Kelvin grid (optional)", &draft.temperatureGrid);
  }
  if(particleGeometry) {
    auto& settings=draft.particleGeometry;
    int output=static_cast<int>(settings.output);
    if(ImGui::Combo("Particle geometry output",&output,"Float SDF\0Native occupancy MaskGrid\0Both grids\0"))
      settings.output=static_cast<vdb::ParticleGeometryOutput>(output);
    editDouble("Signed trail time (seconds)",settings.timeStep);
    editDouble("Native trail spacing [0.25,4]",settings.spacing);
    editDouble("Minimum radius cutoff in target voxels",settings.minRadiusVoxels);
    editDouble("Maximum radius cutoff in target voxels",settings.maxRadiusVoxels);
    bool origin=settings.originWorld.has_value();
    if(ImGui::Checkbox("Set explicit particle lattice origin",&origin))settings.originWorld=origin?std::optional(std::array<double,3>{}):std::nullopt;
    if(settings.originWorld)ImGui::InputScalarN("Particle WORLD lattice origin",ImGuiDataType_Double,settings.originWorld->data(),3,nullptr,nullptr,"%.17g");
    editDouble("Particle target WORLD voxel size [1e-6,1e6]",draft.processing.voxelSize);
    if(particleGeometrySdf(settings.output)) {
      editDouble("Particle SDF half width in target voxels [3,64]",draft.processing.halfWidth);
      ImGui::InputText("Particle SDF output grid",&draft.processing.gridName);
    }
    if(particleGeometryMask(settings.output))ImGui::InputText("Particle MaskGrid output grid",&settings.maskGridName);
    ImGui::TextWrapped("Strict JSON version1 object: version=1, particles=[{position:[x,y,z],radius:r,velocity:[x,y,z]}]. Velocity is optional. Positions/radii/velocity use WORLD units; cutoff radii use target voxels. All head radii must meet the explicit cutoffs. Trails taper OPPOSITE velocity*time; the last sampled sphere need not reach the endpoint. MaskGrid marks centers strictly inside stamps including their interiors; it differs from SDF narrow-band activity. This is sampled sphere geometry, not a continuous capsule, mass-conserving raster or ODE trajectory. No source IDs or attributes. An omitted origin uses the reported head/tail midpoint, so changed geometry can change lattice phase. Inspect SDF/mask separately; mask-only output has no surface preview or fluid restart.");
  }
  if(valueComponents) {
    ImGui::InputText("Component grid prefix (1..128 native bytes)",&draft.valueComponents.gridNamePrefix);
    editCount("Maximum active components (1..64)",draft.valueComponents.maxComponents,64);
    ImGui::TextWrapped("Exports the entire SOURCE FIELD with one six-connected INDEX component active per grid. All nine native value types are supported. Active false/zero/NaN/Inf connect; inactive values do not. Non-Mask values/background/inactive continuations remain unchanged everywhere, including other components; Mask has activity only. Source class/vector/world tags are retained labels, not isolated SDF/solid or density-partition certificates. Native maps and custom binary metadata survive. Components sort by minimum lexicographic XYZ and use prefix_000001,...; empty activity writes one inactive source copy prefix_000000. All source-tree copies share the limits below. Reuse requires an explicit output grid selector; preview a selected field slice separately.");
  }
  if(activeRegionQuery) {
    auto& settings=draft.activeRegionOptions;
    std::optional<size_t> remove;
    for(size_t i=0;i<draft.activeRegions.size();++i) {
      ImGui::PushID(int(i));const auto title="Inclusive index box "+std::to_string(i+1);
      if(ImGui::TreeNode(title.c_str())) {
        ImGui::InputScalarN("Minimum index",ImGuiDataType_S32,draft.activeRegions[i].minimum.data(),3);
        ImGui::InputScalarN("Maximum index",ImGuiDataType_S32,draft.activeRegions[i].maximum.data(),3);
        if(ImGui::Button("Remove index box"))remove=i;
        ImGui::TreePop();
      }ImGui::PopID();
    }
    if(remove)draft.activeRegions.erase(draft.activeRegions.begin()+*remove);
    ImGui::BeginDisabled(draft.activeRegions.size()>=std::min<size_t>(4096,settings.maxQueries));
    if(ImGui::Button("Add index box"))draft.activeRegions.emplace_back();
    ImGui::EndDisabled();ImGui::SameLine();
    if(ImGui::Button("Clear boxes; census only"))draft.activeRegions.clear();
    bool countsOnly=settings.maxRecordsPerQuery==0 && settings.maxTotalRecords==0;
    if(ImGui::Checkbox("Counts only; no native records",&countsOnly)) {
      const vdb::ActiveRegionOptions defaults;
      settings.maxRecordsPerQuery=countsOnly?0:defaults.maxRecordsPerQuery;
      settings.maxTotalRecords=countsOnly?0:defaults.maxTotalRecords;
    }
    ImGui::TextWrapped("Read-only inclusive Int32 INDEX boxes. An empty batch still returns the complete source census. Active false/zero/nonfinite values count; inactive values do not. Compact tiles retain exact multiplicities. Counts stay complete when record quotas truncate; earlier boxes consume the aggregate quota first. The panel displays at most 32 query summaries and 32 raw native records. Float bits and native integers remain exact; vector components are not transformed. No world-region conversion or geometry certificate.");
  }
  if(rayQuery) {
    auto& limits=fogRays?draft.fogRays.limits:operation==Operation::RayIntersections?draft.levelSetRays.limits:draft.volumeRays.limits;
    ImGui::TextWrapped("Read-only WORLD rays: directions normalize, distances are Euclidean WORLD lengths. Zero rays validates the source. Source maps require uniform orthogonal affine voxels; exact descriptor selectors resolve duplicate names. Every native result is computed or the operation fails; the display shows bounded prefixes only.");
    const float rowHeight=4*ImGui::GetFrameHeightWithSpacing()+ImGui::GetStyle().ItemSpacing.y+2;
    if(ImGui::BeginChild("WORLD ray batch",ImVec2(0,250),true)) {
      ImGuiListClipper clipper;clipper.Begin(static_cast<int>(std::min<size_t>(1'000'000,draft.rayQueries.size())),rowHeight);
      while(clipper.Step())for(int index=clipper.DisplayStart;index<clipper.DisplayEnd;++index) {
        ImGui::PushID(index);auto& ray=draft.rayQueries[size_t(index)];
        ImGui::InputScalarN("WORLD origin",ImGuiDataType_Double,ray.origin.data(),3,nullptr,nullptr,"%.17g");
        ImGui::InputScalarN("WORLD direction",ImGuiDataType_Double,ray.direction.data(),3,nullptr,nullptr,"%.17g");
        editDouble("Minimum WORLD distance",ray.minimumDistance);editDouble("Maximum WORLD distance",ray.maximumDistance);
        ImGui::Separator();ImGui::PopID();
      }
    }
    ImGui::EndChild();
    const long double nextBytes=2.0L*(draft.rayQueries.size()+1)*sizeof(vdb::WorldRay);
    ImGui::BeginDisabled(draft.rayQueries.size()>=std::min<uint32_t>(1'000'000,limits.maxRays) || nextBytes>=draft.processing.maxInputBytes);
    if(ImGui::Button("Add WORLD ray"))draft.rayQueries.emplace_back();ImGui::EndDisabled();
    ImGui::SameLine();ImGui::BeginDisabled(draft.rayQueries.empty());
    if(ImGui::Button("Remove last ray"))draft.rayQueries.pop_back();ImGui::SameLine();
    if(ImGui::Button("Clear rays; source census only"))draft.rayQueries.clear();ImGui::EndDisabled();
    ImGui::Text("Ray records: %zu",draft.rayQueries.size());
    if(fogRays) {
      editDouble("Fog extinction (nonnegative inverse WORLD length/density)",draft.fogRays.extinction);
      int policy=int(draft.fogRays.transmissionPolicy);
      if(ImGui::Combo("Fog transmission underflow policy",&policy,"Reject underflow\0Allow explicit opaque-underflow status\0"))
        draft.fogRays.transmissionPolicy=static_cast<vdb::FogTransmissionPolicy>(policy);
      ImGui::TextWrapped("Nodal Float/Double density includes inactive values and background. A positive constant field with empty activity is not vacuum. Uniform orthogonal affine maps, normalized WORLD directions and Euclidean WORLD distance intervals are required. Exact rational cell/Simpson arithmetic retains tiny complements, but native inverse/normalization are approximate; there is no ideal-inverse/error certificate. Finite final I/tau range errors fail. Explicit opaque underflow marks exp rounding, not exact zero. No scattering, emission, exact occupancy or dense-preview parity. No output/model/restart action. The first64 completed results and bounded metadata byte prefixes are displayed; native queries are never silently truncated.");
    } else if(operation==Operation::RayIntersections) {
      editDouble("SDF WORLD isovalue",draft.levelSetRays.isovalue);
      int refinements=int(std::min<uint32_t>(draft.levelSetRays.refinementSteps,8));
      if(ImGui::InputInt("Chord refinements (0..8)",&refinements))draft.levelSetRays.refinementSteps=uint32_t(std::clamp(refinements,0,8));
      ImGui::Checkbox("Compute validity-gated WORLD normals",&draft.levelSetRays.computeNormals);
      ImGui::TextWrapped("Approximate native HDDA/chord intersections, not exact trilinear cubic roots: tangencies or multiple roots between samples can be missed. Sampled-zero includes signed zero and zero plateaus. Normals require a complete active8-corner footprint and nondegenerate gradient; unavailable is not a zero normal. Isovalue must stay two voxels inside the signed band.");
    } else ImGui::TextWrapped("COARSE whole8-cubed active-leaf blocks plus active tiles, using centered cell footprints: one active cell in leaf0 can cover[-.5,7.5]. Inactive-only leaves are excluded. These are not exact occupied-voxel, density-integral or signed-solid intervals. No dilation; grazing spans<=1e-9 index-distance units are omitted.");
  }
  if(surfaceImage) {
    auto& camera=draft.surfaceCamera;auto& image=draft.surfaceReference;
    int projection=int(camera.projection);
    if(ImGui::Combo("Surface camera projection",&projection,"Perspective\0Orthographic\0"))camera.projection=static_cast<vdb::SurfaceProjection>(projection);
    ImGui::InputScalarN("Surface WORLD eye",ImGuiDataType_Double,camera.eye.data(),3,nullptr,nullptr,"%.17g");
    ImGui::InputScalarN("Surface WORLD target",ImGuiDataType_Double,camera.target.data(),3,nullptr,nullptr,"%.17g");
    ImGui::InputScalarN("Surface WORLD up",ImGuiDataType_Double,camera.up.data(),3,nullptr,nullptr,"%.17g");
    if(camera.projection==vdb::SurfaceProjection::Perspective) {
      editDouble("Surface vertical FOV degrees [0.1,150]",camera.verticalFovDegrees);
      camera.orthographicHeight=vdb::SurfaceReferenceCamera{}.orthographicHeight;
    } else {
      editDouble("Surface orthographic WORLD height",camera.orthographicHeight);
      camera.verticalFovDegrees=vdb::SurfaceReferenceCamera{}.verticalFovDegrees;
    }
    editDouble("Surface minimum radial WORLD distance",camera.minimumDistance);
    editDouble("Surface maximum radial WORLD distance",camera.maximumDistance);
    editCount("Surface image width",image.width,2048);editCount("Surface image height",image.height,2048);
    int shader=int(image.shading);
    if(ImGui::Combo("Surface reference shader",&shader,"Matte\0WORLD normal\0Two-sided headlight\0"))image.shading=static_cast<vdb::SurfaceShading>(shader);
    ImGui::InputScalarN("Surface response color [0,1]",ImGuiDataType_Double,image.color.data(),3,nullptr,nullptr,"%.17g");
    ImGui::InputScalarN("Surface miss background [0,1]",ImGuiDataType_Double,image.background.data(),3,nullptr,nullptr,"%.17g");
    if(image.shading!=vdb::SurfaceShading::Matte)
      ImGui::InputScalarN("Unavailable-normal diagnostic color [0,1]",ImGuiDataType_Double,image.invalidNormalColor.data(),3,nullptr,nullptr,"%.17g");
    else image.invalidNormalColor=vdb::SurfaceReferenceOptions{}.invalidNormalColor;
    editDouble("Surface SDF WORLD isovalue",image.rays.isovalue);
    int refinements=int(std::min<uint32_t>(image.rays.refinementSteps,8));
    if(ImGui::InputInt("Surface chord refinements (0..8)",&refinements))image.rays.refinementSteps=uint32_t(std::clamp(refinements,0,8));
    ImGui::Checkbox("Surface compute validity-gated WORLD normals",&image.rays.computeNormals);
    ImGui::TextWrapped("Writes a deterministic center-pixel CPU EXR, with 11 linear FLOAT channels: RGB response, hit coverage A, radial WORLD depth Z, WORLD normal components, N.VALID, N.STATUS and HIT.KIND. This is native Float matte/normal/two-sided-headlight reference response, not physical radiance. Distances are Euclidean, not native camera depth-plane clipping. No AA, shadows, fog transport or uncertainty. Native HDDA/chord search is approximate: tangencies and multiple roots can be missed; sampled-zero includes signed zero and plateaus. Unavailable normals retain status and use a diagnostic color in normal-dependent modes. Only Float/Double uniform-orthogonal positive-exterior SDFs are supported; source bytes are preserved. EXR results cannot be reused as VDB models or restart states. The report shows first16 pixels; native computation is complete or fails.");
  }
  const bool query = operation == Operation::SampleScalar || operation == Operation::SampleVector || operation==Operation::MultiResolutionSample || surfaceQuery || pointNeighbors;
  if (operation != Operation::Inspect && operation != Operation::Measure && operation!=Operation::MultiResolutionInfo && operation!=Operation::Diagnose && !query && !pointQuery && !nativeQuery && !activeRegionQuery && !rayQuery) {
    ImGui::InputText(operation == Operation::SimulateSequence ? "New output directory" : surfaceImage ? "New surface EXR output path" : nanoPrimitive ? "New native output path (.nvdb)" : "New output path", &draft.output);
    if (operation != Operation::ExportMesh && operation != Operation::ExportPoints &&
        operation != Operation::Project && !smoke && !viscosity && !sources && !pointJsonOutput && !dense && !multiresolution && !fileAuthoring && !physicalImage && !surfaceImage && !nativeFile && !volumeSpheres && !potentialFlow && !diagnostics && !boundaryProjection && !boundaryFluidStep && !valueComponents && !particleGeometry)
      ImGui::InputText("Output grid", &draft.processing.gridName);
  }
  if (query) ImGui::InputScalarN("World position", ImGuiDataType_Double,
      draft.queryPosition.data(), 3, nullptr, nullptr, "%.17g");
  if (nativeFile) {
    const auto caps=vdb::NanoVdbInterchangeCapabilities();
    ImGui::Text("Native I/O: %s; VDB conversion: %s; ZIP: %s; BLOSC: %s",
        caps.nativeIo ? "available" : "unavailable", caps.openVdbConversion ? "available" : "unavailable",
        caps.zip ? "available" : "unavailable", caps.blosc ? "available" : "unavailable");
    if(nanoPrimitive) {
      auto& settings=draft.nanoPrimitive;
      int shape=static_cast<int>(draft.nanoPrimitiveShape);
      if(ImGui::Combo("Native primitive shape",&shape,"Sphere\0Axis-aligned box\0WORLD-Y torus\0"))draft.nanoPrimitiveShape=static_cast<vdb::NanoVdbPrimitiveShape>(shape);
      int cls=static_cast<int>(settings.gridClass),storage=static_cast<int>(settings.storage),stats=static_cast<int>(settings.statistics);
      if(ImGui::Combo("Native primitive class",&cls,"Signed narrow band\0Interior linear fog\0"))settings.gridClass=static_cast<vdb::NanoVdbPrimitiveClass>(cls);
      if(ImGui::Combo("Native primitive precision",&storage,"Float\0Double\0"))settings.storage=static_cast<vdb::NanoVdbPrimitiveStorage>(storage);
      if(ImGui::Combo("Native primitive caches",&stats,"Bounds\0Bounds and extrema\0Bounds, extrema and moments\0"))settings.statistics=static_cast<vdb::NanoVdbStatisticsMode>(stats);
      ImGui::InputText("Native primitive grid name",&settings.gridName);
      editDouble("Native uniform voxel size [1e-6,1e6]",settings.voxelSize);
      editDouble("Native half width in voxels [1,64]",settings.halfWidth);
      ImGui::InputScalarN("Native WORLD center",ImGuiDataType_Double,settings.center.data(),3,nullptr,nullptr,"%.17g");
      ImGui::InputScalarN("Native WORLD map origin",ImGuiDataType_Double,settings.origin.data(),3,nullptr,nullptr,"%.17g");
      if(draft.nanoPrimitiveShape==vdb::NanoVdbPrimitiveShape::Sphere)editDouble("Native WORLD sphere radius",settings.radius);else settings.radius=1;
      if(draft.nanoPrimitiveShape==vdb::NanoVdbPrimitiveShape::Box)ImGui::InputScalarN("Native WORLD box full dimensions",ImGuiDataType_Double,settings.boxDimensions.data(),3,nullptr,nullptr,"%.17g");else settings.boxDimensions={2,2,2};
      if(draft.nanoPrimitiveShape==vdb::NanoVdbPrimitiveShape::Torus){editDouble("Native WORLD torus major radius",settings.majorRadius);editDouble("Native WORLD torus minor radius",settings.minorRadius);}else{settings.majorRadius=1;settings.minorRadius=.25;}
      ImGui::TextWrapped("Creates a new CPU NanoVDB file without an input grid. The map is positive uniform scale plus WORLD origin; no rotation/shear. Geometry is evaluated in Double before checked Float/Double publication. Fog uses an interior linear ramp and zero exterior. Shapes must resolve at least 1.5 voxels. Float map cache rounding requires an explicit bounded opt-in; default h=.125/origin0 is exact. Logical limits cover the native pipeline, not process RSS. Arbitrary metadata, CUDA and quantized backends are outside this operation. Convert to VDB separately for preview.");
    }
    if (operation==Operation::NanoCopy || operation==Operation::VdbToNano || operation==Operation::NanoStatistics || nanoPrimitive) {
      int codec=static_cast<int>(draft.nanoWrite.codec);
      if (ImGui::Combo("Native codec",&codec,"None\0ZIP\0BLOSC\0"))
        draft.nanoWrite.codec=static_cast<vdb::NanoVdbCodec>(codec);
      if (ImGui::Checkbox("Raw uncompressed buffers",&draft.nanoWrite.raw) && draft.nanoWrite.raw)
        draft.nanoWrite.codec=vdb::NanoVdbCodec::None;
      if(nanoPrimitive && draft.nanoWrite.raw)draft.nanoWrite.codec=vdb::NanoVdbCodec::None;
      if (operation==Operation::VdbToNano || nanoPrimitive)
        ImGui::Checkbox(nanoPrimitive ? "Allow bounded Float map cache rounding" : "Allow derived Float map rounding",&draft.nanoWrite.allowFloatMapRounding);
      else draft.nanoWrite.allowFloatMapRounding=false;
    }
    if (operation==Operation::NanoCopy || operation==Operation::VdbToNano || operation==Operation::NanoChecksum || operation==Operation::NanoStatistics || nanoPrimitive) {
      int check=static_cast<int>(draft.nanoWrite.checksum);
      if (ImGui::Combo("Native checksum",&check,"Disabled\0Header\0Full\0"))
        draft.nanoWrite.checksum=static_cast<vdb::NanoVdbChecksum>(check);
    }
    if (operation==Operation::NanoValidate || operation==Operation::NanoCopy || operation==Operation::NanoToVdb || operation==Operation::NanoStatistics) {
      ImGui::Checkbox("Verify native CRC",&draft.nanoRead.verifyChecksums);
      ImGui::Checkbox("Require full checksum",&draft.nanoRead.requireFullChecksum);
    }
    if (operation==Operation::NanoToVdb)
      ImGui::Checkbox("Allow named metadata omissions/defaults",&draft.nanoAllowMetadataLoss);
    if (operation==Operation::NanoStatistics) {
      int mode=static_cast<int>(draft.nanoStatisticsMode);
      if (ImGui::Combo("Recompute caches",&mode,"Bounds\0Bounds and extrema\0Bounds, extrema and moments\0"))
        draft.nanoStatisticsMode=static_cast<vdb::NanoVdbStatisticsMode>(mode);
      ImGui::TextWrapped("Only active values contribute. Integer extrema retain their exact type. Vector extrema compare squared magnitudes; vector average/deviation describe squared magnitudes. Moments use native cache precision. Bool/Mask expose occupancy bounds. Unsafe numeric ranges and the final Int32 coordinate require rescaling/recentering. Values, activity, maps and blind payloads remain intact.");
    }
    if(!nanoPrimitive) ImGui::TextWrapped("Exact scalar, Boolean, mask and Vec3 trees. Empty selection includes every grid. Native selectors accept name:literal or index:ordinal; VDB selectors accept descriptor:exact name. Catalog inspection does not validate payloads. Checksum repair checks structure and retains codecs; CRC does not establish authenticity. Double maps and inactive values are retained. Native outputs can be converted back before previewing.");
  }
  if (fileAuthoring)
    ImGui::TextWrapped("The version 1 JSON spec selects, copies, removes, renames and merges native grids and edits typed file/grid metadata. Relative inputs resolve from the spec directory. Default ZIP output preserves native bits and registered point attributes/codecs. Active-mask compression and half precision are explicit loss opt-ins in the spec. Outputs must be new files; external limits below cannot be raised by JSON.");
  if (physicalImage) {
    bool gpu=draft.transportRender.backend==vdb::TransportRenderBackend::Gpu;
    if (ImGui::Checkbox("Use dedicated GPU compute device",&gpu))
      draft.transportRender.backend=gpu ? vdb::TransportRenderBackend::Gpu : vdb::TransportRenderBackend::Cpu;
    if (gpu) {
      ImGui::InputText("Compute SPIR-V path (empty: packaged shader)",&draft.transportShader);
      ImGui::Checkbox("Require Vulkan validation",&draft.transportRender.validation);
    }
    editCount("Samples per pixel",draft.transportRender.image.transport.samplesPerRay,8192);
    ImGui::InputScalar("Scattering order cap",ImGuiDataType_U32,&draft.transportRender.image.transport.maxScatteringEvents);
    ImGui::InputScalar("Image seed",ImGuiDataType_U64,&draft.transportRender.image.transport.seed);
    ImGui::TextWrapped("Offline pinhole image from the scene JSON camera and dimensions, with gray extinction, RGB albedo/emission, HG scattering, point/directional lights, constant environment and black triangle occluders. CPU uses Double dense fields; GPU supports trusted native sparse sources on a separate device. Center-pixel sampling has no antialiasing. The linear FLOAT EXR includes standard-error channels and finite-order diagnostics. This job finishes before shutdown and does not change the interactive volume renderer.");
  }
  if (filterField) {
    int filter=static_cast<int>(draft.fieldFilterOperation);
    if (ImGui::Combo("Value filter",&filter,"Mean\0Gaussian (four boxes)\0Median\0Offset\0")) {
      draft.fieldFilterOperation=static_cast<vdb::FieldFilterOperation>(filter);
      if (draft.fieldFilterOperation!=vdb::FieldFilterOperation::Offset) draft.fieldFilter.offset={};
    }
    if (draft.fieldFilterOperation==vdb::FieldFilterOperation::Offset)
      ImGui::InputScalarN("Stored component offset",ImGuiDataType_Double,draft.fieldFilter.offset.data(),3,nullptr,nullptr,"%.17g");
    else editCount("Kernel radius in index voxels",draft.fieldFilter.width,4);
    editCount("Filter iterations",draft.fieldFilter.iterations,32);
    ImGui::Checkbox("Voxelize active tiles before filtering",&draft.fieldFilter.voxelizeActiveTiles);
    const auto maskUtf8=draft.fieldFilter.mask.u8string();
    std::string maskText(maskUtf8.begin(),maskUtf8.end());
    if (ImGui::InputText("Alpha mask path (optional)",&maskText)) {
      draft.fieldFilter.mask=path(maskText);
      if (maskText.empty()) {
        draft.fieldFilter.maskGridName.clear(); draft.fieldFilter.maskMinimum=0;
        draft.fieldFilter.maskMaximum=1; draft.fieldFilter.invertMask=false;
      }
    }
    if (!draft.fieldFilter.mask.empty()) {
      ImGui::InputText("Mask grid (optional)",&draft.fieldFilter.maskGridName);
      editDouble("Mask minimum",draft.fieldFilter.maskMinimum); editDouble("Mask maximum",draft.fieldFilter.maskMaximum);
      ImGui::Checkbox("Invert alpha mask",&draft.fieldFilter.invertMask);
    }
    ImGui::TextWrapped("Ordinary Float/Double fields and supported collocated world vectors. Kernels use index voxel units and sample stored inactive/background values; only active cells change. Active tiles stay unchanged unless voxelized. Masks require the exact same affine map and use SmoothStep Float alpha at each axis pass. Gaussian is four box iterations; vector median uses native lexicographic XYZ order. Native Float weights can round Double results. Scalar offsets require Y and Z zero; fog must remain nonnegative. Tracked SDF and MAC filters are separate operations.");
  }
  if (operation==Operation::CompositeFields) {
    int field=static_cast<int>(draft.fieldOperation);
    if (ImGui::Combo("Pointwise operation",&field,"Sum\0Product\0Divide\0Minimum\0Maximum\0Blend\0Merge active values\0")) {
      const auto activity=draft.fieldComposite.activity; const auto work=draft.fieldComposite.maxWorkItems;
      draft.fieldOperation=static_cast<vdb::FieldCompositeOperation>(field); draft.fieldComposite={};
      draft.fieldComposite.maxWorkItems=work;
      draft.fieldComposite.activity=draft.fieldOperation==vdb::FieldCompositeOperation::MergeActive ? vdb::FieldActivity::Union : activity;
    }
    if (draft.fieldOperation==vdb::FieldCompositeOperation::MergeActive) {
      int preference=static_cast<int>(draft.fieldComposite.mergePreference);
      if (ImGui::Combo("Active value preference",&preference,"Left\0Right\0"))
        draft.fieldComposite.mergePreference=static_cast<vdb::FieldMergePreference>(preference);
    } else {
      int activity=static_cast<int>(draft.fieldComposite.activity);
      if (ImGui::Combo("Output activity",&activity,"Union\0Intersection\0Left\0Right\0"))
        draft.fieldComposite.activity=static_cast<vdb::FieldActivity>(activity);
    }
    if (draft.fieldOperation==vdb::FieldCompositeOperation::Blend) editDouble("B blend weight",draft.fieldComposite.blendWeight);
    if (draft.fieldOperation==vdb::FieldCompositeOperation::Minimum || draft.fieldOperation==vdb::FieldCompositeOperation::Maximum) {
      int extrema=static_cast<int>(draft.fieldComposite.vectorExtrema);
      if (ImGui::Combo("Vector extrema",&extrema,"Componentwise\0Whole vector by scaled magnitude\0"))
        draft.fieldComposite.vectorExtrema=static_cast<vdb::FieldVectorExtrema>(extrema);
    }
    ImGui::TextWrapped("Exact native type, affine map, class and vector semantics must match. Arithmetic includes stored inactive values and the infinite background; activity is a separate policy. Divide requires every denominator component, including background, to be nonzero. Active merge always uses union activity and keeps A's inactive/background values. Source A metadata is retained; topology is unioned without densifying or pruning. Numerical range loss fails before output.");
  }
  if (operation==Operation::TransformField) {
    int transform=static_cast<int>(draft.fieldTransform.operation),scope=static_cast<int>(draft.fieldTransform.scope);
    if (ImGui::Combo("Value operation",&transform,"Copy\0Affine scale and offset\0Absolute\0Clamp\0")) {
      const auto work=draft.fieldTransform.maxWorkItems; const auto oldScope=draft.fieldTransform.scope;
      draft.fieldTransform={}; draft.fieldTransform.operation=static_cast<vdb::FieldValueOperation>(transform);
      draft.fieldTransform.scope=oldScope; draft.fieldTransform.maxWorkItems=work;
    }
    if (ImGui::Combo("Value scope",&scope,"Active values only\0All values and background\0"))
      draft.fieldTransform.scope=static_cast<vdb::FieldValueScope>(scope);
    if (draft.fieldTransform.operation==vdb::FieldValueOperation::Affine) {
      ImGui::InputScalarN("Component scale",ImGuiDataType_Double,draft.fieldTransform.scale.data(),3,nullptr,nullptr,"%.17g");
      ImGui::InputScalarN("Component offset",ImGuiDataType_Double,draft.fieldTransform.offset.data(),3,nullptr,nullptr,"%.17g");
    }
    if (draft.fieldTransform.operation==vdb::FieldValueOperation::Clamp) {
      ImGui::InputScalarN("Component minimum",ImGuiDataType_Double,draft.fieldTransform.minimum.data(),3,nullptr,nullptr,"%.17g");
      ImGui::InputScalarN("Component maximum",ImGuiDataType_Double,draft.fieldTransform.maximum.data(),3,nullptr,nullptr,"%.17g");
    }
    ImGui::TextWrapped("Ordinary native scalar/world-vector values and compact tiles. Active-only preserves all inactive/background bits; all-values also transforms those values. Scalars use X parameters; leave Y/Z at their defaults. Copy retains native bits. Affine/absolute/clamp reject numerical range loss. Clamp bounds round inward to representable native values. SDF tracking and MAC field operators are separate workflows.");
  }
  if (operation==Operation::DenseExport) {
    ImGui::InputScalarN("Absolute index origin",ImGuiDataType_S32,draft.denseRegion.origin.data(),3);
    ImGui::InputScalarN("Region dimensions",ImGuiDataType_U32,draft.denseRegion.dimensions.data(),3);
  }
  if (dense)
    ImGui::TextWrapped("Versioned CVDBARR binary arrays retain native Float/Double/Vec3f/Vec3d bits, x-fastest order, affine transform, background and explicit active mask. Export requires a finite region. Outside values and custom metadata are omitted with diagnostics; metadata values are not part of the array format. Import retains the stored grid name and semantics.");
  if (multiresolution) {
    editCount("Retained levels",draft.multiresolution.levels,8);
    editBudget("Maximum multiresolution work",draft.multiresolution.maxWork);
    if (operation==Operation::MultiResolutionSample) editDouble("Fractional level",draft.sampleLevel);
    if (operation==Operation::MultiResolution) {
      bool selectedLevel=draft.multiresolutionLevel.has_value();
      if (ImGui::Checkbox("Write one level",&selectedLevel)) draft.multiresolutionLevel=selectedLevel ? std::optional<uint32_t>(0) : std::nullopt;
      if (draft.multiresolutionLevel) {
        int level=static_cast<int>(*draft.multiresolutionLevel);
        if (ImGui::InputInt("Output level (zero finest)",&level))
          draft.multiresolutionLevel=static_cast<uint32_t>(std::clamp(level,0,int(std::clamp<uint32_t>(draft.multiresolution.levels,1,8))-1));
      }
    }
    ImGui::TextWrapped("Full-weighting low-pass levels for scalar fog/ordinary fields and collocated world vectors. Source metadata is retained. Level L doubles source index spacing L times; world vectors are not rescaled. SDFs, native MAC and position/normal vector semantics require other operations. All retained levels and scratch share processing caps.");
  }
  const bool generate = operation == Operation::Sphere || operation == Operation::Mesh || operation == Operation::MeshDistance ||
      operation == Operation::Particles || operation == Operation::Points || operation == Operation::Velocity ||
      operation == Operation::Resample || operation == Operation::Platonic || processPoints || rasterizePoints;
  if (generate) editDouble("Voxel size", draft.processing.voxelSize);
  if (generate && operation != Operation::Points && operation != Operation::Velocity && !processPoints && !splat)
    editDouble("Band half width", draft.processing.halfWidth);
  if (operation == Operation::Sphere) {
    editDouble("Radius", draft.sphere.radius);
    ImGui::InputScalarN("Center", ImGuiDataType_Double, draft.sphere.center.data(), 3, nullptr, nullptr, "%.17g");
  }
  if (operation == Operation::Platonic) {
    constexpr std::array<uint32_t,5> faces{4,6,8,12,20};
    int selectedFace = 1;
    for (size_t index = 0; index < faces.size(); ++index)
      if (draft.platonic.faceCount == faces[index]) selectedFace = static_cast<int>(index);
    if (ImGui::Combo("Solid", &selectedFace, "Tetrahedron\0Cube\0Octahedron\0Dodecahedron\0Icosahedron\0"))
      draft.platonic.faceCount = faces[selectedFace];
    editDouble("Canonical world scale", draft.platonic.scale);
    ImGui::InputScalarN("Center", ImGuiDataType_Double, draft.platonic.center.data(), 3, nullptr, nullptr, "%.17g");
  }
  if (operation == Operation::Velocity)
    ImGui::InputScalarN("Velocity", ImGuiDataType_Double, draft.velocity.data(), 3, nullptr, nullptr, "%.17g");
  if (operation == Operation::MeshDistance) {
    ImGui::Checkbox("Save closest triangle IDs",&draft.meshConversion.writeTriangleIds);
    if (draft.meshConversion.writeTriangleIds) ImGui::InputText("Triangle ID grid",&draft.meshConversion.triangleGridName);
    ImGui::TextWrapped("Nonnegative unsigned distances support open surfaces. Native Int32 IDs retain source triangle order and bounded instance provenance. This output is not a signed level set.");
  }
  if (operation == Operation::Offset) editDouble("World distance", draft.distance);
  if (operation == Operation::Smooth) {
    int filter = static_cast<int>(draft.filter);
    ImGui::Combo("Filter", &filter, "Gaussian\0Mean\0Median\0");
    draft.filter = static_cast<vdb::SmoothingOperation>(filter);
    editCount("Kernel radius", draft.smoothing.width, 4);
    editCount("Iterations", draft.smoothing.iterations, 32);
  }
  if (operation == Operation::Advect) {
    editDouble("Time (seconds)", draft.time);
    editCount("Substeps", draft.steps, 64);
  }
  if (sdfTransport) {
    editDouble(operation==Operation::MorphSdf ? "Relaxation pseudo-time" : "Signed time (seconds)",draft.levelSetTransport.timeStep);
    editCount("Tracker normalization iterations",draft.levelSetTransport.normalizationIterations,8);
    editCount("Maximum CFL steps",draft.levelSetTransport.maxCflSteps,4096);
    editBudget("Maximum transport work",draft.levelSetTransport.maxWork);
    ImGui::TextWrapped("Float narrow-band SDF; uniform orthogonal affine voxels. Uses HJWENO5 and TVD RK2 with a tracked band. Nonzero movement below Float32 band resolution fails. Travel is limited to 256 voxels; the source map/band are preserved.");
    if (operation==Operation::MorphSdf)
      ImGui::TextWrapped("Target transform and band must match. Nonnegative pseudo-time relaxes the existing front toward the target; it is not a linear shape interpolation and cannot seed disconnected target regions without a source front.");
    else ImGui::TextWrapped("Velocity must be collocated world-space Vec3S/Vec3D. It may use a different affine map. Native MAC velocity requires a separate conversion.");
  }
  if (operation == Operation::Scatter) {
    editBudget("Point count", draft.scatter.pointCount);
    editBudget("Random seed", draft.scatter.seed);
    editDouble("Point radius", draft.scatter.radius);
  }
  if (operation == Operation::AdvectPoints) {
    editDouble("Time (seconds)", draft.pointAdvection.timeStep);
    editCount("Substeps", draft.pointAdvection.steps, 64);
    int integration = draft.pointAdvection.integration == vdb::PointIntegration::RK4 ? 1 : 0;
    ImGui::Combo("Integrator", &integration, "RK2\0RK4\0");
    draft.pointAdvection.integration = integration ? vdb::PointIntegration::RK4 : vdb::PointIntegration::RK2;
    ImGui::Checkbox("Update final point velocity", &draft.pointAdvection.updateVelocity);
  }
  if (processPoints)
    ImGui::TextWrapped("Writes standard P/v/pscale points into the selected voxel size. Custom attributes and groups require separate workflows.");
  if (nativePointAttributes) {
    ImGui::TextWrapped("Native leaf-origin/local-index order includes inactive point voxels. Typed edits support bool, int32/int64, float/double, Vec3f/Vec3d and UTF-8 strings; group values are booleans. Integer exports retain exact IDs. Unrelated registered attributes/codecs and descriptor metadata are preserved. Position P cannot be edited without repartitioning.");
    if (operation==Operation::EditPointAttributes)
      ImGui::TextWrapped("JSON requires version:1. Optional attributes/groups arrays contain op:add/set/rename/drop and name. Add/set attributes require type and values; groups require boolean values. Rename uses new_name. Unknown/duplicate keys fail. Exported column documents are readback documents, not edit batches.");
  }
  if (pointSelection) {
    const auto editGroups=[](const char* label,std::vector<std::string>& groups) {
      if (!ImGui::TreeNode(label)) return;
      std::optional<size_t> remove;
      for (size_t index=0;index<groups.size();++index) {
        ImGui::PushID(static_cast<int>(index)); ImGui::InputText("Group",&groups[index]); ImGui::SameLine();
        if (ImGui::Button("Remove")) remove=index;
        ImGui::PopID();
      }
      if (remove) groups.erase(groups.begin()+*remove);
      ImGui::BeginDisabled(groups.size()>=4096);
      if (ImGui::Button("Add group")) groups.emplace_back();
      ImGui::EndDisabled(); ImGui::TreePop();
    };
    editGroups("Require any group",draft.pointSelection.anyOf);
    editGroups("Require all groups",draft.pointSelection.allOf);
    editGroups("Exclude any group",draft.pointSelection.noneOf);
    if(moveNativePoints || advectNativePoints)ImGui::TextWrapped("Empty filters move all points. ANY groups combine by union, ALL by intersection, and excluded groups remove points from deformation only. Every named group must exist; no record is deleted. Unselected world positions survive within the reported target-codec tolerance.");
    else if(pointTransfer)ImGui::TextWrapped("Empty filters deposit every point, including inactive-voxel records. ANY groups form a union, ALL an intersection, and excluded groups remove records from deposition only. Every named group must exist. The source and its unrelated attributes remain unchanged; empty selections create empty field/companion grids.");
    else if(diagnosticPoints)ImGui::TextWrapped("Empty group filters inspect every safely decoded point, including inactive-voxel records. ANY groups form a union, ALL an intersection and excluded groups remove records from optional numeric/P checks only. Schema/layout checks still cover the full source. Missing groups and unsafe layouts are reported independently; no points or attributes are changed or deleted.");
    else ImGui::TextWrapped("Empty filters select all points, including inactive-voxel records. Every named group must exist. Native compaction preserves registered fixed-stride attributes/codecs, IDs, masks and metadata. Deleting all/keeping none retains a valid zero-point tree plus an explicit descriptor archive; repopulation is separate. Statistics use numeric stride-one columns; P offsets are not world positions and cannot be measured here.");
  }
  if (splat) {
    editDouble("Mass per point",draft.splat.massPerPoint);
    if (operation != Operation::SplatDensity) ImGui::InputText("Companion density grid",&draft.splat.densityGridName);
    ImGui::TextWrapped("Fixed trilinear eight-cell deposition independent of point radius. Density is mass per world volume; velocity/radius are normalized means. Standard P/v/pscale layout is required.");
  }
  if (operation == Operation::PointsSdf)
    ImGui::TextWrapped("Point radii become sphere-union signed distances. Each radius must span 1.5 to 100 target voxels. Standard P/v/pscale layout is required.");
  if (viscosity) {
    auto& parameters=draft.viscosity;
    editDouble("Time step (seconds)",parameters.timeStep);
    editDouble("Kinematic viscosity",parameters.kinematicViscosity);
    editCount("Maximum iterations",parameters.maxIterations,10000);
    editDouble("Relative solver tolerance",parameters.tolerance);
    editBudget("Maximum fluid cells",parameters.maxFluidCells);
    ImGui::InputText("Density state grid",&parameters.densityGridName);
    ImGui::InputText("Kelvin state grid",&parameters.temperatureGridName);
    ImGui::InputText("Velocity state grid",&parameters.velocityGridName);
    ImGui::InputText("Pressure state grid",&parameters.pressureGridName);
    ImGui::InputText("Domain state grid",&parameters.domainGridName);
    ImGui::TextWrapped("Canonical five-grid restart state with native MAC velocity. Implicit viscosity uses a zero exterior face-lattice stencil approximating stair-step no-slip walls, then projects pressure. Density and Kelvin fields are preserved. Zero viscosity validates and copies the state.");
  }
  if (sources) {
    auto& parameters=draft.sources;
    editDouble("Time step (seconds)",parameters.timeStep);
    editDouble("Ambient Kelvin",parameters.ambientTemperature);
    editBudget("Maximum fluid cells",parameters.maxFluidCells);
    ImGui::InputText("Density state grid",&parameters.densityGridName);
    ImGui::InputText("Kelvin state grid",&parameters.temperatureGridName);
    ImGui::InputText("Velocity state grid",&parameters.velocityGridName);
    ImGui::InputText("Pressure state grid",&parameters.pressureGridName);
    ImGui::InputText("Domain state grid",&parameters.domainGridName);
    editSourceSpheres(parameters.sources);
    ImGui::TextWrapped("Only true fluid-cell centers inside each sphere are affected. Overlapping rates are combined independent of source order. Exact local density/heat ODEs have no hidden mass clamp. Velocity, pressure and domain are preserved. No source spheres validates and copies the selected state.");
  }
  if (operation == Operation::Project) {
    editDouble("Time step (seconds)", draft.projection.timeStep);
    ImGui::InputScalarN("World acceleration", ImGuiDataType_Double,
        draft.projection.acceleration.data(), 3, nullptr, nullptr, "%.17g");
    editCount("Maximum iterations", draft.projection.maxIterations, 10000);
    editDouble("Relative divergence tolerance", draft.projection.tolerance);
    editBudget("Maximum fluid cells", draft.projection.maxFluidCells);
    ImGui::InputText("Velocity output grid", &draft.projection.velocityGridName);
    ImGui::InputText("Pressure output grid", &draft.projection.pressureGridName);
    ImGui::Checkbox("Save fluid domain", &draft.projection.writeDomain);
    if (draft.projection.writeDomain) ImGui::InputText("Domain output grid", &draft.projection.domainGridName);
    ImGui::TextWrapped("Closed walls; active positive scalar or true Bool cells define fluid. Output velocity uses local staggered faces in voxels/second. Pressure assumes unit density. This operation adds force and projects velocity.");
  }
  if (smoke) {
    auto& simulation = draft.simulation;
    editDouble("Time step (seconds)", simulation.timeStep);
    editCount("Requested substeps", simulation.substeps, 64);
    editCount("Maximum adaptive substeps", simulation.maxSubsteps, 64);
    editDouble("Maximum CFL travel", simulation.maxCfl);
    ImGui::InputScalarN("World gravity", ImGuiDataType_Double, simulation.gravity.data(), 3, nullptr, nullptr, "%.17g");
    editDouble("Density buoyancy", simulation.buoyancyDensity);
    editDouble("Kelvin buoyancy", simulation.buoyancyTemperature);
    editDouble("Kinematic viscosity", simulation.kinematicViscosity);
    editDouble("Ambient Kelvin", simulation.ambientTemperature);
    ImGui::InputScalarN("Unit buoyancy direction", ImGuiDataType_Double, simulation.buoyancyDirection.data(), 3, nullptr, nullptr, "%.17g");
    editCount("Maximum iterations", simulation.maxIterations, 10000);
    editDouble("Relative divergence tolerance", simulation.tolerance);
    editBudget("Maximum fluid cells", simulation.maxFluidCells);
    ImGui::InputText("Density output grid", &simulation.densityGridName);
    ImGui::InputText("Kelvin output grid", &simulation.temperatureGridName);
    ImGui::InputText("Velocity output grid", &simulation.velocityGridName);
    ImGui::InputText("Pressure output grid", &simulation.pressureGridName);
    ImGui::InputText("Domain output grid", &simulation.domainGridName);
    ImGui::TextWrapped("Closed-wall smoke step. Use matching native MAC velocity from Project velocity. Density/Kelvin transport can diffuse; mass and integral drift are reported. The complete output can restart this workflow.");
    if (ImGui::TreeNode("Per-substep sources and sinks")) {
      editSourceSpheres(simulation.sources);
      ImGui::TextWrapped("Sources run after transport, forces, viscosity and projection at each actual substep. Added density and heat drive buoyancy on the next actual substep. Ambient Kelvin, timestep, names and processing caps come from this smoke operation.");
      ImGui::TreePop();
    }
    if (operation == Operation::SimulateSequence) {
      editCount("Saved frames",draft.sequence.frameCount,256);
      editBudget("Maximum encoded sequence bytes",draft.sequence.maxOutputBytes);
      ImGui::TextWrapped("Each saved frame advances one time step. The output directory must be new. Frames use frame_0001.vdb and sequence.json; the last state can be reused as a restart.");
    }
  }
  if (operation == Operation::Segment) editCount("Maximum components", draft.maxComponents, 1024);
  if (operation == Operation::ExportMesh) {
    bool overrideIso = draft.isoValue.has_value();
    if (ImGui::Checkbox("Override isovalue", &overrideIso)) draft.isoValue = overrideIso ? std::optional<double>(0) : std::nullopt;
    if (draft.isoValue) editDouble("Isovalue", *draft.isoValue);
    editDouble("Adaptivity", draft.adaptivity);
  }
  if (operation == Operation::Inspect) {
    ImGui::Checkbox("Load scalar statistics", &draft.statistics);
    if (draft.statistics) {
      bool histogram = draft.inspectionHistogramBins != 0;
      if (ImGui::Checkbox("Active-value histogram", &histogram))
        draft.inspectionHistogramBins = histogram ? 64 : 0;
      if (histogram) {
        editCount("Histogram bins", draft.inspectionHistogramBins, 4096);
        bool customRange = draft.inspectionHistogramRange.has_value();
        if (ImGui::Checkbox("Custom histogram range", &customRange))
          draft.inspectionHistogramRange = customRange ? std::optional(std::array{0.,1.}) : std::nullopt;
        if (draft.inspectionHistogramRange) {
          editDouble("Histogram minimum", (*draft.inspectionHistogramRange)[0]);
          editDouble("Histogram maximum", (*draft.inspectionHistogramRange)[1]);
        }
        ImGui::TextWrapped("Active tiles contribute their voxel counts. Bins are half-open; the maximum is included in the last bin. Outside values are counted separately.");
      }
    }
  }
  if (operation == Operation::Measure)
    ImGui::TextWrapped("Closed signed-distance surface integrals in world units. Euler is a resolution-dependent curvature estimate; refine before interpreting topology. No genus is inferred for disconnected surfaces.");
  if (operation == Operation::Differential)
    ImGui::Combo("Field operation", &draft.differential, "Gradient\0Divergence\0Curl\0Laplacian\0Magnitude\0Normalize\0");
  if (operation == Operation::Poisson) {
    editCount("Maximum iterations", draft.maxIterations, 10000);
    editDouble("Relative tolerance", draft.tolerance);
  }
  if (ImGui::TreeNode("Processing limits")) {
    if (physicalImage) {
      editBudget("Maximum scene spec bytes",draft.transportRender.maxSpecBytes);
      editBudget("Maximum scene bytes",draft.transportRender.maxSceneBytes);
      editBudget("Maximum image/workspace bytes",draft.transportRender.image.maxImageBytes);
      editBudget("Maximum estimator bytes",draft.transportRender.image.transport.maxInputBytes);
      editBudget("Maximum density values",draft.transportRender.maxDensityValues);
      editBudget("Maximum pixels",draft.transportRender.image.maxPixels);
      editBudget("Maximum paths",draft.transportRender.image.maxPaths);
      editBudget("Maximum tracing/preparation work",draft.transportRender.image.transport.maxWork);
      editCount("Maximum GPU work per path",draft.transportRender.maxWorkPerPath,65536);
    } else if (nativeFile) {
      editBudget("Maximum encoded file bytes",draft.nanoLimits.maxInputBytes);
      editBudget("Maximum native payload bytes",draft.nanoLimits.maxNativeBytes);
      editBudget("Maximum native workspace bytes",draft.nanoLimits.maxWorkspaceBytes);
      editBudget("Maximum active native voxels",draft.nanoLimits.maxActiveVoxels);
      editBudget("Maximum stored native leaf slots",draft.nanoLimits.maxStoredLeafVoxels);
      editBudget("Maximum native nodes",draft.nanoLimits.maxNodes);
      editBudget("Maximum native work",draft.nanoLimits.maxWork);
      editBudget("Maximum native grids",draft.nanoLimits.maxGrids);
      editBudget("Maximum native name bytes",draft.nanoLimits.maxNameBytes);
      editBudget("Maximum preserved metadata bytes",draft.nanoLimits.maxMetadataBytes);
    } else if(boundaryFluidStep) {
      auto& o=draft.boundaryFluid;
      editBudget("Maximum boundary-fluid cells",o.maxFluidCells);
      editBudget("Maximum aggregate boundary-fluid stored leaf slots",o.maxStoredLeafVoxels);
      editBudget("Maximum aggregate boundary-fluid nodes",o.maxNodes);
      editBudget("Maximum joint boundary-fluid work",o.maxWork);
      editCount("Maximum boundary-fluid catalog grids",o.maxGrids,4096);
      editBudget("Maximum boundary-fluid metadata bytes",o.maxMetadataBytes);
      editBudget("Maximum joint boundary-fluid input/native/report bytes",o.maxInputBytes);
      editBudget("Maximum encoded boundary-fluid output bytes",o.maxOutputBytes);
    } else if(particleGeometry) {
      auto& settings=draft.particleGeometry;
      editBudget("Maximum particle joint input/native/report bytes",draft.processing.maxInputBytes);
      editBudget("Maximum aggregate particle leaf slots/candidate voxels",draft.processing.maxVoxelCount);
      editBudget("Maximum encoded particle output bytes",settings.maxOutputBytes);
      editBudget("Maximum geometry particles",settings.maxParticles);
      editBudget("Maximum sampled trail instances",settings.maxTrailInstances);
      editDouble("Maximum particle trail length in target voxels",settings.maxTrailLengthVoxels);
      editBudget("Maximum aggregate particle nodes",settings.maxNodes);
      editBudget("Maximum joint particle/report work",settings.maxWork);
      editBudget("Maximum particle metadata bytes",settings.maxMetadataBytes);
    } else if(surfaceImage) {
      auto& image=draft.surfaceReference;auto& limits=image.rays.limits;
      editBudget("Maximum joint surface source/image/report bytes",draft.processing.maxInputBytes);
      editBudget("Maximum joint surface EXR/export bytes",draft.surfaceExportBytes);
      editBudget("Maximum surface source active cells/stored leaf slots",draft.processing.maxVoxelCount);
      editBudget("Maximum surface image pixels (1..262144)",image.maxPixels);
      editBudget("Maximum surface ray batch (1..1M)",limits.maxRays);
      editBudget("Maximum scanned surface grids (1..4096)",limits.maxGrids);
      editBudget("Maximum joint surface/report work (1..1e10)",limits.maxWorkItems);
      editBudget("Maximum aggregate surface source metadata bytes",limits.maxMetadataBytes);
    } else if(rayQuery) {
      auto& limits=fogRays?draft.fogRays.limits:operation==Operation::RayIntersections?draft.levelSetRays.limits:draft.volumeRays.limits;
      editBudget("Maximum joint ray/source/report bytes",draft.processing.maxInputBytes);
      editBudget("Maximum source active cells/stored leaf slots",draft.processing.maxVoxelCount);
      editBudget("Maximum ray batch (1..1M)",limits.maxRays);
      editBudget("Maximum scanned ray grids (1..4096)",limits.maxGrids);
      editBudget("Maximum joint ray/report work (1..1e10)",limits.maxWorkItems);
      editBudget("Maximum aggregate ray metadata bytes",limits.maxMetadataBytes);
      if(fogRays) {
        editBudget("Maximum prospective fog cells per ray (1..1M)",draft.fogRays.maxCellSegmentsPerRay);
        editBudget("Maximum prospective fog cells across batch (1..1M)",draft.fogRays.maxCellSegments);
      } else if(operation==Operation::RayVolumeSpans)editBudget("Maximum complete coarse spans (1..1M)",draft.volumeRays.maxSpans);
    } else if(valueComponents) {
      auto& settings=draft.valueComponents;
      editBudget("Maximum joint component/native/report bytes",settings.maxInputBytes);
      editBudget("Maximum aggregate encoded component bytes",settings.maxOutputBytes);
      editBudget("Maximum complete component-active census",settings.maxActiveVoxels);
      editBudget("Maximum each source/expanded leaf slots",settings.maxStoredLeafVoxels);
      editBudget("Maximum aggregate output leaf slots",settings.maxAggregateOutputLeafVoxels);
      editBudget("Maximum joint component nodes",settings.maxNodes);
      editBudget("Maximum joint component/report work",settings.maxWork);
      editBudget("Maximum scanned component grids",settings.maxGrids);
      editBudget("Maximum aggregate component metadata bytes",settings.maxMetadataBytes);
    } else if(activeRegionQuery) {
      auto& settings=draft.activeRegionOptions;
      editBudget("Maximum joint native/report bytes",settings.maxInputBytes);
      editBudget("Maximum stored source leaf slots",settings.maxStoredLeafVoxels);
      editBudget("Maximum source nodes and root entries",settings.maxNodes);
      editBudget("Maximum exact source-active census",settings.maxSourceActiveVoxels);
      editBudget("Maximum joint query/report work",settings.maxWork);
      editBudget("Maximum scanned query grids",settings.maxGrids);
      editBudget("Maximum query batch",settings.maxQueries);
      editBudget("Maximum query metadata bytes",settings.maxMetadataBytes);
      editBudget("Maximum native records per query",settings.maxRecordsPerQuery);
      editBudget("Maximum total native records",settings.maxTotalRecords);
    } else {
    editBudget("Maximum voxels", draft.processing.maxVoxelCount);
    editBudget("Maximum input/workspace bytes", draft.processing.maxInputBytes);
    if (operation != Operation::Measure && operation!=Operation::MultiResolutionInfo && !smoke && !viscosity && !sources && !rasterizePoints && !sdfTransport && !nativePointAttributes && !dense && !multiresolution && !pointSelection && !fileAuthoring && !filterField && !compositeFields && !topology && !valueTopology && !maskedValues && !sdfBackground && !sdfFilter &&
        operation != Operation::Project && operation != Operation::Inspect && !query && !volumeSpheres && !fastSweeping && !regions && !fracture && !fieldAdvection && !potentialFlow && !surfaceFields && !fieldResampling && !diagnostics && !boundaryProjection)
      editBudget("Maximum triangles", draft.processing.maxTriangles);
    if (operation == Operation::Particles)
      editBudget("Maximum particles", draft.maxParticles);
    if (operation == Operation::Points || operation == Operation::ExportPoints || processPoints || rasterizePoints || nativePointAttributes || pointSelection)
      editBudget("Maximum points", draft.maxPoints);
    if (nativePointAttributes || pointSelection) {
      editBudget("Maximum native attributes",draft.pointAttributes.maxAttributes);
      editBudget("Maximum native groups",draft.pointAttributes.maxGroups);
      if (operation==Operation::EditPointAttributes) editBudget("Maximum native edits",draft.pointAttributes.maxEdits);
      editBudget("Maximum point string bytes",draft.pointAttributes.maxStringBytes);
    }
    if (fileAuthoring) {
      editBudget("Maximum spec bytes",draft.fileAuthoring.maxSpecBytes);
      editBudget("Maximum spec depth",draft.fileAuthoring.maxDepth);
      editBudget("Maximum scanned grids",draft.fileAuthoring.maxGrids);
      editBudget("Maximum metadata bytes",draft.fileAuthoring.maxMetadataBytes);
      editBudget("Maximum authoring edits",draft.fileAuthoring.maxEdits);
      editBudget("Maximum native points",draft.fileAuthoring.maxPoints);
      editBudget("Maximum point attributes",draft.fileAuthoring.maxPointAttributes);
      editBudget("Maximum point groups",draft.fileAuthoring.maxPointGroups);
    }
    if (filterField) editBudget("Maximum filter kernel work",draft.fieldFilter.maxWorkItems);
    if (operation==Operation::CompositeFields) editBudget("Maximum composite work",draft.fieldComposite.maxWorkItems);
    if (operation==Operation::TransformField) editBudget("Maximum transform work",draft.fieldTransform.maxWorkItems);
    if (topology) editBudget("Maximum topology work",draft.topology.maxWorkItems);
    if (valueTopology) editBudget("Maximum native edit work",draft.valueTopology.maxWorkItems);
    if (maskedValues) editBudget("Maximum native masked work",draft.maskedValuesMaxWork);
    if (sdfBackground) editBudget("Maximum SDF background work",draft.sdfBackground.maxWorkItems);
    if (sdfFilter) editBudget("Maximum SDF filter/tracker work",draft.sdfFilter.maxWorkItems);
    if(fastSweeping)editBudget("Maximum fast sweeping work",draft.fastSweeping.maxWorkItems);
    if(regions) {
      editBudget("Maximum region work",draft.regions.maxWorkItems);
      editBudget("Maximum region catalog grids",draft.regions.maxGrids);
      editBudget("Maximum region metadata bytes",draft.regions.maxMetadataBytes);
    }
    if(surfaceQuery) {
      editBudget("Maximum surface query work",draft.surfaceQuery.maxWork);
      editBudget("Maximum surface query catalog grids",draft.surfaceQuery.maxGrids);
      editBudget("Maximum surface query metadata bytes",draft.surfaceQuery.maxMetadataBytes);
    }
    if(fracture)editBudget("Maximum fracture work",draft.fracture.maxWorkItems);
    if(rebuildLevelSet) {
      editBudget("Maximum level-set rebuild work",draft.levelSetRebuild.maxWork);
      editBudget("Maximum rebuild catalog grids",draft.levelSetRebuild.maxGrids);
      editBudget("Maximum rebuild metadata bytes",draft.levelSetRebuild.maxMetadataBytes);
    }
    if(surfaceFields) {
      editBudget("Maximum surface field work",draft.surfaceFields.maxWorkItems);
      editCount("Maximum surface field catalog grids",draft.surfaceFields.maxGrids,4096);
      editBudget("Maximum surface field metadata bytes",draft.surfaceFields.maxMetadataBytes);
    }
    if(advectNativePoints) {
      editBudget("Maximum native point advection work",draft.nativePointAdvection.maxWork);
      editBudget("Maximum native point advection catalog grids",draft.nativePointAdvection.maxGrids);
      editBudget("Maximum native point advection metadata bytes",draft.nativePointAdvection.maxMetadataBytes);
    }
    if(moveNativePoints) {
      editBudget("Maximum point motion work",draft.pointMotion.maxWork);
      editBudget("Maximum point motion catalog grids",draft.pointMotion.maxGrids);
      editBudget("Maximum point motion metadata bytes",draft.pointMotion.maxMetadataBytes);
    }
    if(pointTransfer) {
      editBudget("Maximum point transfer work",draft.pointTransfer.maxWork);
      editBudget("Maximum point transfer catalog grids",draft.pointTransfer.maxGrids);
      editBudget("Maximum point transfer metadata bytes",draft.pointTransfer.maxMetadataBytes);
    }
    if(fieldResampling) {
      editBudget("Maximum field resampling work",draft.fieldResampling.maxWorkItems);
      editBudget("Maximum field resampling catalog grids",draft.fieldResampling.maxGrids);
      editBudget("Maximum field resampling metadata bytes",draft.fieldResampling.maxMetadataBytes);
    }
    if(diagnostics) {
      editBudget("Maximum diagnostic work",draft.diagnostics.maxWork);
      editBudget("Maximum diagnostic catalog grids",draft.diagnostics.maxGrids);
      editBudget("Maximum diagnostic metadata bytes",draft.diagnostics.maxMetadataBytes);
      editBudget("Examples per check (zero disables examples)",draft.diagnostics.maxExamples);
      draft.diagnostics.maxExamples=std::min<size_t>(128,draft.diagnostics.maxExamples);
    }
    if(boundaryProjection) {
      editBudget("Maximum boundary projection work",draft.boundaryProjection.maxWorkItems);
      editBudget("Maximum boundary catalog grids",draft.boundaryProjection.maxGrids);
      editBudget("Maximum boundary metadata bytes",draft.boundaryProjection.maxMetadataBytes);
    }
    if(fieldAdvection)editBudget("Maximum field advection work",draft.fieldAdvection.maxWorkItems);
    if(potentialFlow) {
      editBudget("Maximum potential flow work",draft.potentialFlow.maxWorkItems);
      editCount("Maximum potential flow catalog grids",draft.potentialFlow.maxGrids,4096);
      editBudget("Maximum potential flow metadata bytes",draft.potentialFlow.maxMetadataBytes);
    }
    if(pointNeighbors) {
      editBudget("Maximum returned neighbors",draft.pointNeighbors.maxNeighbors);
      editBudget("Maximum point neighbor work",draft.pointNeighbors.maxWork);
      editBudget("Maximum neighbor catalog grids",draft.pointNeighbors.maxGrids);
      editBudget("Maximum neighbor metadata bytes",draft.pointNeighbors.maxMetadataBytes);
    }
    if(indexedPointMask) {
      editBudget("Maximum indexed mask work",draft.indexedPointMask.maxWork);
      editBudget("Maximum indexed mask catalog grids",draft.indexedPointMask.maxGrids);
      editBudget("Maximum indexed mask metadata bytes",draft.indexedPointMask.maxMetadataBytes);
    }
    if(volumeSpheres) {
      editBudget("Maximum sphere fitting work",draft.volumeSpheres.maxWork);
      editBudget("Maximum sphere fitting catalog grids",draft.volumeSpheres.maxGrids);
      editBudget("Maximum sphere fitting metadata bytes",draft.volumeSpheres.maxMetadataBytes);
    }
    if (sampleNativePoints) {
      editBudget("Maximum sampling work",draft.pointFieldSampling.maxWork);
      editBudget("Maximum sampling catalog grids",draft.pointFieldSampling.maxGrids);
      editBudget("Maximum sampling metadata bytes",draft.pointFieldSampling.maxMetadataBytes);
    }
    }
    ImGui::TreePop();
  }
  ImGui::BeginDisabled(busy());
  if (ImGui::Button("Run OpenVDB operation")) (void)submit(draft);
  ImGui::EndDisabled();
  if (busy()) {
    if (const auto state=progress()) {
      const auto label=std::to_string(state->completedFrames)+" / "+std::to_string(state->totalFrames)+" frames";
      ImGui::ProgressBar(state->totalFrames ? float(state->completedFrames)/float(state->totalFrames) : 0,
          ImVec2(-1,0),label.c_str());
      ImGui::BeginDisabled(state->cancellationRequested);
      if (ImGui::Button("Cancel sequence")) (void)requestCancel();
      ImGui::EndDisabled();
      ImGui::TextWrapped(state->cancellationRequested ? "Cancellation requested; waiting for the current timestep." :
          "Cancellation takes effect between timesteps. Frames are published only after the sequence succeeds.");
    } else ImGui::TextUnformatted("Processing...");
  }
  if (result_) {
    ImGui::TextWrapped("%s", result_->message.c_str());
    // Artifact actions follow the submitted job, independently of the draft.
    if(result_->success && result_->boundaryFluidArtifact) {
      const auto& artifact=*result_->boundaryFluidArtifact;
      ImGui::TextWrapped("Boundary fluid output9: %s",artifact.output.c_str());
      if(ImGui::Button("Use boundary fluid state as restart")) {
        draft.operation=Operation::BoundaryFluidStep;auto& inputs=draft.boundaryFluidInputs;
        inputs.boundary.state=path(artifact.output);
        inputs.boundary.velocityGrid=artifact.grids[0];inputs.boundary.fluidGrid=artifact.grids[1];
        inputs.boundary.faceKindGrid=artifact.grids[2];inputs.boundary.faceFractionGrid=artifact.grids[3];
        inputs.boundary.facePressureGrid=artifact.grids[4];inputs.boundary.wallVelocityGrid=artifact.grids[5];
        inputs.density={path(artifact.output),artifact.grids[6]};
        inputs.temperature=vdb::BoundaryProjectionField{path(artifact.output),artifact.grids[7]};
        draft.boundaryFluid.initialVelocity=artifact.initialVelocity;
        draft.output.clear();
      }
      const auto inspect=[&](const char* label,size_t slot) {
        if(ImGui::Button(label)) {
          draft.operation=Operation::Inspect;draft.input=artifact.output;draft.inputGrid=artifact.grids[slot];
          previewInput=artifact.output;previewGrid=artifact.grids[slot];previewKind=3;
        }
      };
      inspect("Inspect boundary fluid density",6);inspect("Inspect boundary fluid velocity",0);
    }

    if(result_->success && result_->particleGeometryArtifact) {
      const auto& artifact=*result_->particleGeometryArtifact;
      ImGui::TextWrapped("Generated particle VDB: %s",artifact.output.c_str());
      const auto inspect=[&](const char* label,const std::optional<std::string>& selector) {
        if(selector && ImGui::Button(label)) {
          draft.operation=Operation::Inspect;draft.input=artifact.output;draft.inputGrid=*selector;
          previewInput=artifact.output;previewGrid=*selector;previewKind=3;
        }
      };
      inspect("Inspect generated SDF",artifact.sdfGrid);
      inspect("Inspect generated mask",artifact.maskGrid);
      if(result_->modelOutput && artifact.sdfGrid && ImGui::Button("Load generated SDF preview")) {
        vdb::ImportOptions options;options.gridName=*artifact.sdfGrid;options.includePoints=false;
        options.maxActiveVoxels=draft.processing.maxVoxelCount;options.maxInputBytes=draft.processing.maxInputBytes;
        options.maxTriangles=draft.processing.maxTriangles;
        previewOptions_=std::make_shared<const vdb::ImportOptions>(std::move(options));
        return artifact.output;
      }
    }
    if (result_->success && !result_->output.empty() && !activeRegionQuery && !rayQuery && !surfaceImage) {
      ImGui::TextWrapped("Output: %s", result_->output.c_str());
      if(valueComponents)ImGui::TextWrapped("Multiple SOURCE FIELD grids: after reuse, choose an explicit component selector from the report before another operation or field-slice preview.");
      if (!boundaryProjection && !nanoPrimitive && !activeRegionQuery && ImGui::Button("Use output as input")) {
        draft.input = result_->output;
        draft.inputGrid.clear();
      }
      if (result_->restartGrids) {
        ImGui::SameLine();
        if (ImGui::Button("Use state as restart")) {
          draft.operation = Operation::Simulate;
          draft.input = draft.secondInput = draft.domainInput = draft.temperatureInput = result_->output;
          draft.inputGrid = (*result_->restartGrids)[0];
          draft.secondGrid = (*result_->restartGrids)[1];
          draft.domainGrid = (*result_->restartGrids)[2];
          draft.temperatureGrid = (*result_->restartGrids)[3];
        }
      }
      if (result_->modelOutput) {
        ImGui::SameLine();
        if (ImGui::Button("Load preview")) {
          vdb::ImportOptions options; options.pointPreview=pointPreview;
          options.maxActiveVoxels=draft.processing.maxVoxelCount;
          options.maxInputBytes=draft.processing.maxInputBytes; options.maxTriangles=draft.processing.maxTriangles;
          previewOptions_=std::make_shared<const vdb::ImportOptions>(std::move(options));
          return result_->output;
        }
      }
    }
  }
  if (ImGui::TreeNode("Field preview controls")) {
    ImGui::InputText("Preview path",&previewInput);
    ImGui::InputText("Preview grid (optional)",&previewGrid);
    ImGui::Combo("Preview type",&previewKind,"Surfaces and points\0Vector glyphs\0Vector streamlines\0Field slice\0Native point attributes\0");
    const bool vectorMode=previewKind==1 || previewKind==2;
    if (previewKind<=2) {
    auto& colorMode=vectorMode ? vectorPreview.colorMode : pointPreview.colorMode;
    int color=static_cast<int>(colorMode);
    ImGui::Combo("Attribute color",&color,"Uniform\0Signed velocity direction\0Velocity magnitude\0");
    colorMode=static_cast<vdb::PointPreviewColor>(color);
    if (colorMode==vdb::PointPreviewColor::Uniform) {
      auto& tint=vectorMode ? vectorPreview.uniformColor : pointPreview.uniformColor;
      ImGui::ColorEdit3("Uniform color",tint.data());
    }
    if (colorMode==vdb::PointPreviewColor::VelocityMagnitude)
      editDouble("World velocity color scale",vectorMode ? vectorPreview.velocityScale : pointPreview.velocityScale);
    }
    if (vectorMode) {
      ImGui::Checkbox("Explicit world bounds",&vectorPreview.hasBounds);
      if (vectorPreview.hasBounds) {
        ImGui::InputScalarN("Bounds minimum",ImGuiDataType_Double,vectorPreview.boundsMin.data(),3,nullptr,nullptr,"%.17g");
        ImGui::InputScalarN("Bounds maximum",ImGuiDataType_Double,vectorPreview.boundsMax.data(),3,nullptr,nullptr,"%.17g");
      }
      ImGui::InputScalarN("Seed counts",ImGuiDataType_U32,vectorPreview.seedCounts.data(),3);
      editDouble("World glyph/tube radius",vectorPreview.worldRadius);
      if (previewKind==1) {
        editDouble("Glyph display seconds",vectorPreview.displaySeconds);
        editDouble("Maximum glyph length",vectorPreview.maxGlyphLength);
      } else {
        editDouble("World streamline step",vectorPreview.worldStep);
        editCount("Maximum streamline steps",vectorPreview.maxSteps,4096);
        ImGui::Checkbox("Trace both directions",&vectorPreview.bothDirections);
      }
      ImGui::InputText("Same-file fluid mask (optional)",&vectorPreview.domainGrid);
      editBudget("Maximum seeds",vectorPreview.maxSeeds);
      editBudget("Maximum preview work",vectorPreview.maxWork);
      ImGui::TextWrapped("World vectors or explicitly tagged native MAC. Constant background fields require explicit bounds. Streamlines show the current field, not particle trajectories. Length caps, zero vectors and trace termination are reported in object metadata.");
    } else if (previewKind==0) {
      editBudget("Maximum preview points",pointPreview.maxPoints);
      ImGui::TextWrapped("Point radii use physical world units and mesh markers. Signed direction colors use neutral gray for zero velocity. Standard P/v/pscale attributes are required.");
    } else if (previewKind==3) {
      ImGui::InputScalarN("Plane center",ImGuiDataType_Double,slicePreview.origin.data(),3,nullptr,nullptr,"%.17g");
      ImGui::InputScalarN("Plane U direction",ImGuiDataType_Double,slicePreview.axisU.data(),3,nullptr,nullptr,"%.17g");
      ImGui::InputScalarN("Plane V direction",ImGuiDataType_Double,slicePreview.axisV.data(),3,nullptr,nullptr,"%.17g");
      ImGui::InputScalarN("World side lengths",ImGuiDataType_Double,slicePreview.worldExtent.data(),2,nullptr,nullptr,"%.17g");
      ImGui::InputScalarN("Edge-inclusive sample counts",ImGuiDataType_U32,slicePreview.sampleCounts.data(),2);
      int value=static_cast<int>(slicePreview.valueMode),map=static_cast<int>(slicePreview.colorMap);
      if (ImGui::Combo("Slice value",&value,"Scalar\0World vector X\0World vector Y\0World vector Z\0World vector magnitude\0")) {
        slicePreview.valueMode=static_cast<vdb::SliceValue>(value);
        if(slicePreview.valueMode!=vdb::SliceValue::Scalar)slicePreview.integerRange.reset();
      }
      if (ImGui::Combo("Color map",&map,"Gray\0Blue to red\0Blue white red\0")) slicePreview.colorMap=static_cast<vdb::SliceColorMap>(map);
      bool exactIntegerRange=slicePreview.integerRange.has_value();
      if(ImGui::Checkbox("Use exact integer scalar color range",&exactIntegerRange)) {
        slicePreview.integerRange=exactIntegerRange?std::optional<std::array<int64_t,2>>(std::array<int64_t,2>{0,1}):std::nullopt;
        if(exactIntegerRange) {slicePreview.valueMode=vdb::SliceValue::Scalar;slicePreview.colorMinimum=0;slicePreview.colorMaximum=1;}
      }
      if(slicePreview.integerRange)ImGui::InputScalarN("Exact signed 64-bit range",ImGuiDataType_S64,slicePreview.integerRange->data(),2);
      else {
        editDouble("Floating/vector range minimum",slicePreview.colorMinimum);editDouble("Floating/vector range maximum",slicePreview.colorMaximum);
      }
      ImGui::TextWrapped("Integer scalars use nearest centers and an automatic exact sampled range; Bool/Mask use 0 to 1. Leave floating limits at 0 and 1 for automatic integer colors, or choose an exact integer range. Floating fields use linear interpolation and the floating limits. Half-index ties select the greater integer center.");
      ImGui::SliderFloat("Slice opacity",&slicePreview.opacity,0,1);
      editBudget("Maximum slice samples",slicePreview.maxSamples); editBudget("Maximum slice work",slicePreview.maxWork);
      editBudget("Maximum slice metadata bytes",slicePreview.maxMetadataBytes);
      ImGui::TextWrapped("Explicit source-world plane with independently normalized U/V directions. Oblique axes form a parallelogram. Scalar and world-vector values include inactive/background interpolation; tagged native MAC is reconstructed in world units. The shared indexed display mesh uses the chosen fixed color range. Opacity uses the existing transparent material path. Object properties report raw values and clipping.");
    } else if (previewKind==4) {
      ImGui::InputText("Radius attribute (empty: constant)",&nativePointPreview.radiusAttribute);
      if (nativePointPreview.radiusAttribute.empty()) editDouble("Constant world radius",nativePointPreview.constantRadius);
      editDouble("Radius multiplier",nativePointPreview.radiusScale);
      int color=static_cast<int>(nativePointPreview.colorMode);
      if (ImGui::Combo("Native attribute color",&color,"Uniform\0Scalar range\0Signed component direction\0String palette\0")) {
        nativePointPreview.colorMode=static_cast<vdb::NativePointColor>(color);
        if (nativePointPreview.colorMode==vdb::NativePointColor::Uniform) nativePointPreview.colorAttribute.clear();
      }
      if (nativePointPreview.colorMode==vdb::NativePointColor::Uniform) ImGui::ColorEdit3("Native uniform color",nativePointPreview.uniformColor.data());
      else ImGui::InputText("Color attribute",&nativePointPreview.colorAttribute);
      ImGui::SliderFloat("Native point opacity",&nativePointPreview.opacity,0,1);
      const auto groups=[](const char* label,std::vector<std::string>& names) {
        if (!ImGui::TreeNode(label)) return;
        std::optional<size_t> remove;
        for (size_t i=0;i<names.size();++i) {
          ImGui::PushID(static_cast<int>(i)); ImGui::InputText("Group",&names[i]); ImGui::SameLine();
          if (ImGui::Button("Remove")) remove=i;
          ImGui::PopID();
        }
        if (remove) names.erase(names.begin()+*remove);
        ImGui::BeginDisabled(names.size()>=4096);
        if (ImGui::Button("Add group")) names.emplace_back();
        ImGui::EndDisabled(); ImGui::TreePop();
      };
      groups("Native any groups",nativePointPreview.groups.anyOf); groups("Native all groups",nativePointPreview.groups.allOf);
      groups("Native excluded groups",nativePointPreview.groups.noneOf);
      editBudget("Maximum native preview points",nativePointLimits.maxPoints);
      editBudget("Maximum native preview attributes",nativePointLimits.maxAttributes);
      editBudget("Maximum native preview groups",nativePointLimits.maxGroups);
      editBudget("Maximum native string bytes",nativePointLimits.maxStringBytes);
      editBudget("Maximum scanned point grids",nativePointPreview.maxGrids);
      editBudget("Maximum native metadata bytes",nativePointPreview.maxMetadataBytes);
      ImGui::TextWrapped("Native fixed-codec point positions and selected typed attributes, including inactive points. Unchosen custom attributes remain intact. Scalar colors use the selected range with exact Int64 differences; strings use a stable palette that can have category collisions. Direction colors show native attribute components. Radius has no implicit attribute fallback. Source schema/codecs/groups remain visible in model properties.");
    }
    if (ImGui::Button("Load field preview")) {
      vdb::ImportOptions options; options.gridName=previewGrid; options.pointPreview=pointPreview;
      options.maxActiveVoxels=draft.processing.maxVoxelCount; options.maxInputBytes=draft.processing.maxInputBytes;
      options.maxTriangles=draft.processing.maxTriangles;
      if (vectorMode) {
        options.vectorPreview=vectorPreview;
        options.vectorPreview->mode=previewKind==1 ? vdb::VectorPreviewMode::Glyphs : vdb::VectorPreviewMode::Streamlines;
      }
      if (previewKind==3) options.slicePreview=slicePreview;
      if (previewKind==4) { options.nativePointPreview=nativePointPreview; options.nativePointLimits=nativePointLimits; }
      previewOptions_=std::make_shared<const vdb::ImportOptions>(std::move(options));
      ImGui::TreePop();
      return previewInput;
    }
    ImGui::TreePop();
  }
  return {};
}
} // namespace container::ui


