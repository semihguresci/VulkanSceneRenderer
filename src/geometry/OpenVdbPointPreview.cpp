#include "Container/geometry/OpenVdbPointPreview.h"
#include "Container/geometry/OpenVdbPoints.h"
#include "Container/geometry/OpenVdbGridSelection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <openvdb/openvdb.h>
#include <openvdb/points/PointDataGrid.h>
#include <tbb/task_arena.h>
#include <iomanip>
#include <locale>
#include <mutex>
#include <set>
#include <sstream>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB point previews are unavailable; configure with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using Grid = openvdb::points::PointDataGrid;
constexpr long double Fixed = 4.0L * 1024 * 1024;
void initialize() {
  static std::once_flag once;
  std::call_once(once, [] {
    openvdb::initialize();
    using V = openvdb::points::TypedAttributeArray<openvdb::Vec3d, openvdb::points::NullCodec>;
    using R = openvdb::points::TypedAttributeArray<double, openvdb::points::NullCodec>;
    if (!V::isRegistered()) V::registerType();
    if (!R::isRegistered()) R::registerType();
  });
}
void budget(long double bytes, const PointPreviewOptions& o, const char* reason) {
  if (!std::isfinite(bytes) || bytes > o.maxInputBytes) throw std::invalid_argument(reason);
}
void validate(float scale, const PointPreviewOptions& o) {
  if (!std::isfinite(scale) || scale <= 0) throw std::invalid_argument("point preview import scale must be finite and positive");
  if (!o.maxPoints || !o.maxActiveVoxels || !o.maxTriangles || !o.maxInputBytes || !o.maxMetadataBytes ||
      o.maxGrids < 1 || o.maxGrids > 4096)
    throw std::invalid_argument("point preview budgets must be positive; maxGrids must be in [1,4096]");
  if (o.colorMode != PointPreviewColor::Uniform && o.colorMode != PointPreviewColor::VelocityDirection &&
      o.colorMode != PointPreviewColor::VelocityMagnitude)
    throw std::invalid_argument("unknown point preview color mode");
  for (float value : o.uniformColor)
    if (!std::isfinite(value) || value < 0 || value > 1) throw std::invalid_argument("point preview uniform color must be finite in [0,1]");
  if (!std::isfinite(o.velocityScale) || o.velocityScale <= 0)
    throw std::invalid_argument("point preview velocityScale must be finite and positive in source world units/second");
}
long double metadataReserve(const openvdb::MetaMap& map) {
  long double bytes = sizeof(openvdb::MetaMap) + 4096;
  for (auto it = map.beginMeta(); it != map.endMeta(); ++it)
    bytes += 4.0L * (it->first.size() + it->second->typeName().size()) +
             16.0L * it->second->size() + 512;
  return bytes;
}
long double modelBytes(const dotbim::Model& model) {
  return model.vertices.capacity() * static_cast<long double>(sizeof(Vertex)) +
         model.indices.capacity() * static_cast<long double>(sizeof(uint32_t)) +
         model.elements.capacity() * static_cast<long double>(sizeof(dotbim::Element) + 1024) +
         model.meshRanges.capacity() * static_cast<long double>(sizeof(dotbim::MeshRange));
}
std::string number(double value) {
  std::ostringstream text; text.imbue(std::locale::classic()); text << std::setprecision(17) << value; return text.str();
}
glm::vec3 color(const PointRecord& point, const PointPreviewOptions& o) {
  if (o.colorMode == PointPreviewColor::Uniform)
    return {o.uniformColor[0], o.uniformColor[1], o.uniformColor[2]};
  const double maximum = std::max({std::abs(point.velocity[0]), std::abs(point.velocity[1]), std::abs(point.velocity[2])});
  if (maximum == 0) return o.colorMode == PointPreviewColor::VelocityDirection ? glm::vec3(.5f) : glm::vec3(.1f,.25f,1);
  const std::array<double,3> relative{point.velocity[0]/maximum, point.velocity[1]/maximum, point.velocity[2]/maximum};
  const double length = std::hypot(relative[0],relative[1],relative[2]);
  if (o.colorMode == PointPreviewColor::VelocityDirection)
    return {float(.5+.5*relative[0]/length),float(.5+.5*relative[1]/length),float(.5+.5*relative[2]/length)};
  // Comparing before multiplying avoids overflow for finite Double payloads
  // whose true Euclidean norm exceeds DBL_MAX.
  const double t = maximum >= o.velocityScale/length ? 1.0 : std::clamp((maximum/o.velocityScale)*length,0.0,1.0);
  return {float(std::lerp(.1,1.,t)),float(std::lerp(.25,.2,t)),float(std::lerp(1.,.05,t))};
}
constexpr std::array<std::array<int,3>,6> Axes{{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}}};
constexpr std::array<std::array<uint32_t,3>,8> Faces{{{0,2,4},{2,1,4},{1,3,4},{3,0,4},{2,0,5},{1,2,5},{3,1,5},{0,3,5}}};
std::array<glm::vec3,6> marker(const PointRecord& point, double scale) {
  const double radius = point.radius * scale;
  if (!std::isfinite(radius) || radius < std::numeric_limits<float>::min() || radius > std::numeric_limits<float>::max())
    throw std::invalid_argument("point preview radius is outside usable Float32 display precision; rescale the point data");
  std::array<glm::vec3,6> result;
  for (size_t vertex = 0; vertex < Axes.size(); ++vertex) for (size_t axis = 0; axis < 3; ++axis) {
    const double center = point.position[axis] * scale;
    const double value = center + radius * Axes[vertex][axis];
    const double represented = static_cast<float>(value);
    if (!std::isfinite(center) || !std::isfinite(value) || !std::isfinite(represented) ||
        std::abs(represented-value) > radius * .001 ||
        (Axes[vertex][axis] != 0 && value == center))
      throw std::invalid_argument("point preview coordinates cannot preserve radius at Float32 display precision; recenter or rescale the point data");
    result[vertex][axis] = static_cast<float>(represented);
  }
  return result;
}
void empty(dotbim::Model& model, uint32_t ordinal, const std::string& name) {
  ++model.importReport.skippedProductCount;
  ++model.importReport.representationWarnings["OpenVDB.EmptyPoints"];
  model.importReport.diagnostics.push_back({"OpenVDB.EmptyPoints",ordinal,ordinal,
    "vdb-points:"+std::to_string(ordinal)+":"+name,"Point grid '"+name+"' contains no stored points"});
}
void preflightMarkers(const dotbim::Model& model, size_t count, const PointPreviewOptions& o,
                      long double resident) {
  if (count > (o.maxTriangles - std::min(o.maxTriangles,model.indices.size()/3))/8)
    throw std::invalid_argument("point marker triangles exceed the shared maxTriangles budget");
  const auto indexLimit = std::numeric_limits<uint32_t>::max();
  if (count > (indexLimit-model.vertices.size())/6 || count > (indexLimit-model.indices.size())/24)
    throw std::invalid_argument("point markers exceed the renderer's 32-bit vertex/index range");
  const auto vertices=model.vertices.size()+6*count, indices=model.indices.size()+24*count;
  budget(resident + count*(3.0L*sizeof(PointRecord)) +
      3.0L*(vertices*static_cast<long double>(sizeof(Vertex))+indices*static_cast<long double>(sizeof(uint32_t))) +
      modelBytes(model)+Fixed,o,"point marker output/decoded workspace exceeds the shared memory budget");
}
void append(dotbim::Model& model, const std::vector<PointRecord>& records, const openvdb::GridBase& metadata,
            const std::string& descriptor, uint32_t ordinal, float scale, const PointPreviewOptions& o,
            long double resident, long double& propertyBytes) {
  if (records.empty()) { empty(model,ordinal,descriptor); return; }
  const auto count = records.size();
  preflightMarkers(model,count,o,resident);
  const auto vertices = model.vertices.size()+6*count, indices = model.indices.size()+24*count;
  budget(resident + records.capacity()*static_cast<long double>(sizeof(PointRecord)) +
      3.0L * (vertices*static_cast<long double>(sizeof(Vertex))+indices*static_cast<long double>(sizeof(uint32_t))) +
      modelBytes(model) + Fixed, o, "point marker output/decoded workspace exceeds the shared memory budget");
  glm::dvec3 minimum(std::numeric_limits<double>::infinity()), maximum(-std::numeric_limits<double>::infinity());
  double radiusMin = std::numeric_limits<double>::infinity(), radiusMax = 0;
  std::array<double,3> velocityMin{INFINITY,INFINITY,INFINITY}, velocityMax{-INFINITY,-INFINITY,-INFINITY};
  // Validate every prospective marker before allocating output geometry.
  for (const auto& point : records) {
    const auto positions = marker(point,scale);
    for (const auto& position : positions) {
      minimum = glm::min(minimum,glm::dvec3(position)); maximum = glm::max(maximum,glm::dvec3(position));
    }
    radiusMin=std::min(radiusMin,point.radius); radiusMax=std::max(radiusMax,point.radius);
    for (size_t axis=0;axis<3;++axis) { velocityMin[axis]=std::min(velocityMin[axis],point.velocity[axis]); velocityMax[axis]=std::max(velocityMax[axis],point.velocity[axis]); }
  }
  const glm::vec3 center((minimum+maximum)*.5);
  const auto extent=glm::max(glm::abs(maximum-glm::dvec3(center)),glm::abs(minimum-glm::dvec3(center)));
  const double bounds=std::hypot(extent.x,extent.y,extent.z);
  float bound=static_cast<float>(bounds);
  if (bound<bounds) bound=std::nextafter(bound,std::numeric_limits<float>::infinity());
  if (!std::isfinite(bound)) throw std::invalid_argument("point marker bounds exceed the renderer's Float32 range");
  dotbim::Element element;
  element.meshId=ordinal; element.geometryKind=dotbim::GeometryKind::Mesh;
  element.guid="vdb-points:"+std::to_string(ordinal)+":"+descriptor;
  element.sourceId=descriptor; element.displayName=metadata.getName().empty()?descriptor:metadata.getName();
  element.type="OpenVDB Points"; element.materialName="OpenVDB Point Markers";
  element.materialIndex=0; element.color=glm::vec4(1); element.doubleSided=false;
  const char* mode=o.colorMode==PointPreviewColor::Uniform?"Uniform":o.colorMode==PointPreviewColor::VelocityDirection?"Signed velocity direction (zero neutral gray)":"Velocity magnitude (blue-to-red, saturated)";
  element.properties={{"OpenVDB","Descriptor",descriptor,""},{"OpenVDB","Point count",std::to_string(count),""},
    {"OpenVDB","Attributes","P:Vec3f, v:Vec3d, pscale:double; NullCodec, stride one",""},
    {"OpenVDB","Position/radius units","Source world units; scaled by importScale",""},
    {"OpenVDB","Velocity units","Source world units/second; color scale is independent of importScale",""},
    {"OpenVDB","Preview","Triangle mesh radius markers; point-style controls do not apply; all stored records including inactive-voxel points",""},
    {"OpenVDB","Color",mode,""},{"OpenVDB","Velocity color scale",number(o.velocityScale),""},
    {"OpenVDB","Radius minimum",number(radiusMin),""},{"OpenVDB","Radius maximum",number(radiusMax),""},
    {"OpenVDB","Import scale",number(scale),""}};
  for (size_t axis=0;axis<3;++axis) {
    element.properties.push_back({"OpenVDB","Velocity minimum "+std::to_string(axis),number(velocityMin[axis]),""});
    element.properties.push_back({"OpenVDB","Velocity maximum "+std::to_string(axis),number(velocityMax[axis]),""});
  }
  std::ostringstream transform; transform.imbue(std::locale::classic()); metadata.transform().print(transform);
  element.properties.push_back({"OpenVDB","Source transform",transform.str(),""});
  for (auto it=metadata.beginMeta();it!=metadata.endMeta();++it) {
    auto text=it->second->str();
    propertyBytes+=it->first.size()+it->second->typeName().size()+text.size()+128.0L;
    if (propertyBytes>o.maxMetadataBytes) throw std::invalid_argument("point grid metadata text exceeds the shared metadata budget");
    element.properties.push_back({"OpenVDB metadata",it->first,std::move(text),it->second->typeName()});
  }
  model.vertices.reserve(vertices); model.indices.reserve(indices);
  dotbim::MeshRange range{.meshId=ordinal,.firstIndex=static_cast<uint32_t>(model.indices.size()),
    .indexCount=static_cast<uint32_t>(24*count),.boundsCenter=center,.boundsRadius=bound};
  for (const auto& point:records) {
    const auto positions=marker(point,scale); const auto tint=color(point,o);
    const auto first=static_cast<uint32_t>(model.vertices.size());
    for (size_t vertex=0;vertex<Axes.size();++vertex) {
      Vertex value; value.position=positions[vertex]; value.color=tint;
      value.normal=glm::vec3(Axes[vertex][0],Axes[vertex][1],Axes[vertex][2]);
      const glm::vec3 reference=std::abs(value.normal.z)<.9f?glm::vec3(0,0,1):glm::vec3(0,1,0);
      value.tangent=glm::vec4(glm::normalize(glm::cross(reference,value.normal)),1);
      model.vertices.push_back(value);
    }
    for (const auto& face:Faces) for (uint32_t vertex:face) model.indices.push_back(first+vertex);
  }
  model.meshRanges.push_back(range); model.elements.push_back(std::move(element));
  ++model.importReport.importedProductCount;
}
#endif
} // namespace

dotbim::Model LoadPointsPreviewFromFile(const std::filesystem::path& input, const std::string& selection,
                                      float importScale, const PointPreviewOptions& o) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validate(importScale,o); initialize(); tbb::task_arena arena(1);
  return arena.execute([&] {
    if (!std::filesystem::is_regular_file(input)) throw std::invalid_argument("point preview input is not a regular file");
    const auto encoded=std::filesystem::file_size(input);
    budget(static_cast<long double>(encoded)+Fixed,o,"point preview encoded input exceeds the memory budget");
    detail::RequireSeekableFile(input);
    if (!openvdb::io::Archive::isDelayedLoadingEnabled())
      throw std::invalid_argument("bounded point previews require OpenVDB delayed loading; unset OPENVDB_DISABLE_DELAYED_LOAD");
    openvdb::io::File file(input.string());
#ifdef OPENVDB_USE_DELAYED_LOADING
    if (encoded>=std::numeric_limits<openvdb::Index64>::max()) throw std::invalid_argument("point preview input exceeds delayed-copy range");
    file.setCopyMaxBytes(static_cast<openvdb::Index64>(encoded)+1);
#endif
    file.open(true);
    {
      std::set<std::string> descriptors; long double catalog=encoded+Fixed;
      for (auto it=file.beginName();it!=file.endName();++it) {
        if (descriptors.size()>=o.maxGrids) throw std::invalid_argument("point preview descriptor count exceeds maxGrids");
        const auto name=it.gridName(); catalog+=2.0L*name.size()+128;
        budget(catalog,o,"point preview descriptor catalog exceeds the memory budget");
        if (!descriptors.insert(name).second) throw std::invalid_argument("point preview input has colliding grid descriptors; rename colliding grids first");
      }
    }
    std::vector<std::pair<std::string,openvdb::GridBase::Ptr>> selected;
    long double metadataBytes=0;
    for (auto it=file.beginName();it!=file.endName();++it) {
      auto metadata=file.readGridMetadata(it.gridName());
      if (!metadata->isType<Grid>() || !GridSelectionMatches(selection,it.gridName(),metadata->getName())) continue;
      if (!selection.empty() && !selected.empty()) throw std::invalid_argument("point preview grid name is ambiguous; use descriptor:<exact descriptor name>");
      metadataBytes+=metadataReserve(*metadata)+4.0L*it.gridName().size();
      if (metadataBytes>o.maxMetadataBytes) throw std::invalid_argument("point preview raw/text metadata reserve exceeds the shared metadata budget");
      budget(encoded+4*metadataBytes+Fixed,o,"point preview metadata exceeds the shared memory budget");
      selected.emplace_back(it.gridName(),std::move(metadata));
    }
    if (selected.empty()) throw std::invalid_argument("no matching PointDataGrid found for point preview");
    dotbim::Model model;
    model.importReport.sourceFormat="OpenVDB"; model.importReport.sourceItemName="point grids";
    model.importReport.sourceProductCount=selected.size();
    model.unitMetadata.hasSourceUnits=true; model.unitMetadata.sourceUnits="OpenVDB world units";
    model.unitMetadata.hasImportScale=true; model.unitMetadata.importScale=importScale;
    model.unitMetadata.hasEffectiveImportScale=true; model.unitMetadata.effectiveImportScale=importScale;
    dotbim::Material material; material.pbr.baseColor=glm::vec4(1); material.pbr.roughnessFactor=.75f;
    material.pbr.attenuationDistance=std::numeric_limits<float>::max(); model.materials.push_back(std::move(material));
    uint64_t totalActive=0,totalLeaves=0; size_t totalPoints=0; long double propertyBytes=0;
    const auto maxLeaves=o.maxActiveVoxels/512+(o.maxActiveVoxels%512!=0);
    uint32_t ordinal=0;
    for (const auto& [name,metadata]:selected) {
      ++ordinal;
      const auto memory=metadata->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_MEM_BYTES);
      const auto voxels=metadata->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_VOXEL_COUNT);
      if (!memory || memory->value()<0 || !voxels || voxels->value()<0)
        throw std::invalid_argument("point preview requires nonnegative file_mem_bytes/file_voxel_count statistics; rewrite with grid statistics enabled");
      if (uint64_t(voxels->value())>o.maxActiveVoxels-totalActive)
        throw std::invalid_argument("point preview grid metadata exceeds the shared active-voxel budget");
      budget(encoded+4*metadataBytes+modelBytes(model)+memory->value()+Fixed,o,"point preview stored topology exceeds the shared memory budget");
      auto grid=openvdb::gridPtrCast<Grid>(file.readGrid(name));
      const auto active=grid->activeVoxelCount(); const auto leaves=grid->tree().leafCount();
      if (active>o.maxActiveVoxels-totalActive || leaves>maxLeaves-totalLeaves)
        throw std::invalid_argument("point preview stored topology exceeds the shared active-voxel/leaf budget");
      totalActive+=active; totalLeaves+=leaves;
      budget(encoded+4*metadataBytes+modelBytes(model)+grid->memUsage()+Fixed,o,"point preview loaded topology exceeds the shared memory budget");
      size_t pointCount=0;
      for (auto leaf=grid->tree().cbeginLeaf();leaf;++leaf) {
        leaf->validateOffsets();
        if (leaf->pointCount()>o.maxPoints-totalPoints-pointCount)
          throw std::invalid_argument("point preview records exceed the shared maxPoints budget");
        pointCount+=leaf->pointCount();
      }
      grid.reset();
      preflightMarkers(model,pointCount,o,encoded+4*metadataBytes);
      ProcessingOptions processing;
      processing.maxVoxelCount=o.maxActiveVoxels;
      const long double reserve=4*metadataBytes+modelBytes(model)+Fixed;
      budget(reserve+encoded+Fixed,o,"retained point preview data exceeds the shared memory budget");
      processing.maxInputBytes=static_cast<uintmax_t>(o.maxInputBytes-reserve);
      // ReadPoints requires a positive cap; a remaining zero cap permits only
      // an empty next grid, while bounding at most one extra decoded record.
      auto points=ReadPoints(input,"descriptor:"+name,processing,{std::max<size_t>(1,o.maxPoints-totalPoints)});
      if (points.size()>o.maxPoints-totalPoints) throw std::invalid_argument("point preview records exceed the shared maxPoints budget");
      totalPoints+=points.size();
      append(model,points,*metadata,name,ordinal,importScale,o,encoded+4*metadataBytes,propertyBytes);
    }
    file.close();
    model.importReport.completeness=model.importReport.diagnostics.empty()?ImportCompleteness::Complete:ImportCompleteness::Partial;
    return model;
  });
#else
  (void)input; (void)selection; (void)importScale; (void)o; unavailable();
#endif
}
} // namespace container::geometry::vdb
