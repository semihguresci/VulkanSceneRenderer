#include "Container/geometry/OpenVdbMeshConversion.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "Container/geometry/DotBimLoader.h"
#include "OpenVdbFileUtils.h"
#include <openvdb/openvdb.h>
#include <openvdb/tools/MeshToVolume.h>
#include <tbb/task_arena.h>
#include <mutex>
#include <unordered_map>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB mesh conversion is unavailable; configure with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
constexpr long double Fixed=8.0L*1024*1024;
using Position=std::array<double,3>;
using Triangle=std::array<uint32_t,3>;
void budget(long double bytes,const ProcessingOptions& p,const char* reason) {
  if (!std::isfinite(bytes) || bytes>p.maxInputBytes) throw std::invalid_argument(reason);
}
void options(const std::filesystem::path& output,const ProcessingOptions& p,const MeshConversionOptions& c) {
  if (!c.unsignedDistance)
    throw std::invalid_argument("this mesh conversion supports unsigned distance only; use WriteMeshLevelSet for closed signed surfaces");
  if (!p.maxVoxelCount || !p.maxInputBytes || !p.maxTriangles)
    throw std::invalid_argument("mesh conversion budgets must be positive");
  if (!std::isfinite(p.voxelSize) || p.voxelSize<1.5e-5 || p.voxelSize>1e6)
    throw std::invalid_argument("mesh distance voxel size must be in [1.5e-5,1e6]; smaller isotropic OpenVDB affine maps are unsupported");
  if (!std::isfinite(p.halfWidth) || p.halfWidth<3 || p.halfWidth>64)
    throw std::invalid_argument("mesh distance band half width must be in [3,64] voxels");
  if (p.gridName.empty() || p.gridName.size()>256 || (c.writeTriangleIds &&
      (c.triangleGridName.empty() || c.triangleGridName.size()>256 || c.triangleGridName==p.gridName)))
    throw std::invalid_argument("mesh output names must contain 1..256 bytes and be distinct");
  if (output.empty() || std::filesystem::exists(output)) throw std::invalid_argument("mesh conversion output must be a new nonempty path");
  if (output.has_parent_path() && !std::filesystem::is_directory(output.parent_path()))
    throw std::invalid_argument("mesh conversion output directory does not exist");
}
int64_t block(int32_t value,int64_t dimension) {
  const int64_t v=value; return v>=0?v/dimension:(v+1)/dimension-1;
}
long double blocks(const openvdb::CoordBBox& box,int64_t dimension) {
  long double count=1;
  for (int axis=0;axis<3;++axis) count*=block(box.max()[axis],dimension)-block(box.min()[axis],dimension)+1;
  return count;
}
template<class Tree> long double treeBytes(const openvdb::CoordBBox& box) {
  using Leaf=typename Tree::LeafNodeType; using Upper=typename Tree::RootNodeType::ChildNodeType; using Lower=typename Upper::ChildNodeType;
  return blocks(box,Leaf::DIM)*(sizeof(Leaf)+Leaf::NUM_VALUES*sizeof(typename Tree::ValueType)+256.0L)+
    blocks(box,Lower::DIM)*(sizeof(Lower)+256.0L)+blocks(box,Upper::DIM)*(sizeof(Upper)+512.0L);
}
double ulp(double value) {
  return std::max(std::abs(std::nextafter(value,std::numeric_limits<double>::infinity())-value),
                  std::abs(std::nextafter(value,-std::numeric_limits<double>::infinity())-value));
}
struct Preflight { openvdb::Vec3d center; openvdb::CoordBBox bounds; long double resident; };
Preflight preflight(std::span<const Position> points,std::span<const Triangle> triangles,
                    const ProcessingOptions& p,const MeshConversionOptions& c,long double ownedBytes,size_t provenanceBytes) {
  if (points.empty() || triangles.empty()) throw std::invalid_argument("mesh conversion needs vertices and triangles");
  if (points.size()>uint64_t(std::numeric_limits<int32_t>::max()) || triangles.size()>uint64_t(std::numeric_limits<int32_t>::max()) || triangles.size()>p.maxTriangles)
    throw std::invalid_argument("mesh conversion exceeds triangle or native Int32 index limits");
  const long double resident=ownedBytes+points.size()*(sizeof(Position)+sizeof(openvdb::Vec3d))+triangles.size()*(2.0L*sizeof(Triangle))+
    8.0L*provenanceBytes+16.0L*(p.gridName.size()+c.triangleGridName.size())+Fixed;
  budget(resident,p,"mesh conversion geometry/provenance exceeds the input-memory budget");
  openvdb::Vec3d minimum(std::numeric_limits<double>::infinity()),maximum(-std::numeric_limits<double>::infinity());
  for (const auto& point:points) for (int axis=0;axis<3;++axis) {
    if (!std::isfinite(point[axis]) || !std::isfinite(ulp(point[axis])) || ulp(point[axis])>p.voxelSize*1e-4)
      throw std::invalid_argument("mesh world vertices require finite Double precision within 1e-4 voxel; recenter the source");
  }
  for (const auto& triangle:triangles) {
    for (uint32_t vertex:triangle) {
      if (vertex>=points.size()) throw std::invalid_argument("mesh triangle references a missing vertex");
      for (int axis=0;axis<3;++axis) {
        minimum[axis]=std::min(minimum[axis],points[vertex][axis]); maximum[axis]=std::max(maximum[axis],points[vertex][axis]);
      }
    }
    const openvdb::Vec3d a(points[triangle[0]].data()),b(points[triangle[1]].data()),d(points[triangle[2]].data());
    const auto ab=b-a,ad=d-a;
    const double longest=std::max({ab.length(),ad.length(),(d-b).length()});
    const double cross=ab.cross(ad).length();
    if (!std::isfinite(longest) || !std::isfinite(cross) || longest<=0 || cross<=longest*longest*1e-12)
      throw std::invalid_argument("mesh contains a degenerate or insufficiently conditioned triangle");
  }
  openvdb::Vec3d center; openvdb::Coord lo,hi;
  for (int axis=0;axis<3;++axis) {
    center[axis]=std::midpoint(minimum[axis],maximum[axis]);
    const double low=std::floor((minimum[axis]-center[axis])/p.voxelSize-p.halfWidth-8);
    const double high=std::ceil((maximum[axis]-center[axis])/p.voxelSize+p.halfWidth+8);
    if (!std::isfinite(low) || !std::isfinite(high) || low<std::numeric_limits<int32_t>::min()+32. || high>std::numeric_limits<int32_t>::max()-32.)
      throw std::invalid_argument("mesh distance bounds exceed safe OpenVDB integer coordinates");
    if (ulp(center[axis]+low*p.voxelSize)>p.voxelSize*1e-4 || ulp(center[axis]+high*p.voxelSize)>p.voxelSize*1e-4)
      throw std::invalid_argument("mesh world band bounds have insufficient Double precision; recenter the source");
    lo[axis]=int32_t(low); hi[axis]=int32_t(high);
  }
  const openvdb::CoordBBox bounds(lo,hi); const int outputs=c.writeTriangleIds?2:1;
  const auto maxLeaves=p.maxVoxelCount/512+(p.maxVoxelCount%512!=0);
  if (outputs*blocks(bounds,1)>p.maxVoxelCount || outputs*blocks(bounds,8)>maxLeaves)
    throw std::invalid_argument("mesh distance padded bounds/output topology exceed the voxel budget");
  const long double workspace=resident+8*treeBytes<openvdb::FloatTree>(bounds)+4*treeBytes<openvdb::Int32Tree>(bounds)+
    4*treeBytes<openvdb::BoolTree>(bounds)+64*blocks(bounds,1);
  budget(workspace,p,"meshToVolume distance/ID/mask workspace exceeds the input-memory budget");
  return {center,bounds,resident};
}
struct Adapter {
  const std::vector<openvdb::Vec3d>& points; std::span<const Triangle> triangles;
  size_t polygonCount() const {return triangles.size();} size_t pointCount() const {return points.size();}
  size_t vertexCount(size_t) const {return 3;}
  void getIndexSpacePoint(size_t n,size_t v,openvdb::Vec3d& point) const {point=points[triangles[n][v]];}
};
MeshConversionResult convert(std::span<const Position> points,std::span<const Triangle> triangles,
    const std::filesystem::path& output,const ProcessingOptions& p,const MeshConversionOptions& c,
    long double ownedBytes=0,const std::string& provenance={}) {
  const auto f=preflight(points,triangles,p,c,ownedBytes,provenance.size());
  std::vector<openvdb::Vec3d> local; local.reserve(points.size());
  for (const auto& point:points) local.emplace_back((openvdb::Vec3d(point.data())-f.center)/p.voxelSize);
  std::vector<Triangle> canonical(triangles.begin(),triangles.end());
  for (auto& triangle:canonical) std::sort(triangle.begin(),triangle.end(),[&](uint32_t a,uint32_t b) {
    for (int axis=0;axis<3;++axis) if (local[a][axis]!=local[b][axis]) return local[a][axis]<local[b][axis];
    return a<b;
  });
  auto nativeIds=openvdb::Int32Grid::create(-1);
  const auto unit=openvdb::math::Transform::createLinearTransform(1.);
  auto distance=openvdb::tools::meshToVolume<openvdb::FloatGrid>(Adapter{local,canonical},*unit,float(p.halfWidth),0.f,
    openvdb::tools::UNSIGNED_DISTANCE_FIELD,nativeIds.get());
  if (distance->empty() || distance->tree().hasActiveTiles()) throw std::invalid_argument("mesh distance conversion produced no voxelized narrow band");
  const float background=float(double(distance->background())*p.voxelSize);
  if (!std::isfinite(background) || background<=0) throw std::invalid_argument("mesh distance background exceeds finite Float32 world units");
  for (auto it=distance->beginValueAll();it;++it) {
    const bool active=it.isValueOn(); const float value=float(double(*it)*p.voxelSize);
    if (!std::isfinite(value) || value<0 || value>background*(1+1e-6)) throw std::invalid_argument("mesh conversion produced invalid unsigned distances");
    it.setValue(value); it.setActiveState(active);
  }
  distance->tree().root().setBackground(background,false);
  auto matrix=openvdb::math::Mat4d::identity();
  for(int axis=0;axis<3;++axis) {matrix[axis][axis]=p.voxelSize;matrix[3][axis]=f.center[axis];}
  auto transform=detail::MakeAffineTransformExact(matrix);
  distance->setTransform(transform); distance->setGridClass(openvdb::GRID_UNKNOWN); distance->setName(p.gridName);
  openvdb::GridPtrVec grids{distance}; openvdb::Int32Grid::Ptr ids;
  if (c.writeTriangleIds) {
    ids=openvdb::Int32Grid::create(-1); ids->setTransform(detail::CopyTransformExact(*transform)); ids->setName(c.triangleGridName);
    auto access=ids->getAccessor(); const auto native=nativeIds->getConstAccessor();
    for (auto it=distance->cbeginValueOn();it;++it) {
      const auto id=native.getValue(it.getCoord());
      if (id<0 || uint64_t(id)>=triangles.size()) throw std::invalid_argument("upstream mesh conversion returned an invalid closest-triangle index");
      access.setValue(it.getCoord(),id);
    }
    grids.push_back(ids);
  }
  nativeIds.reset();
  uint64_t active=0,leaves=0; long double bytes=f.resident;
  for (auto& grid:grids) {
    grid->insertMeta("container:distance_type",openvdb::StringMetadata("unsigned"));
    grid->insertMeta("container:triangle_count",openvdb::Int64Metadata(int64_t(triangles.size())));
    grid->insertMeta("container:triangle_id_order",openvdb::StringMetadata("zero-based input triangle order; exact distance ties use lower ID"));
    grid->insertMeta("container:band_voxels",openvdb::DoubleMetadata(p.halfWidth));
    if (!provenance.empty()) grid->insertMeta("container:triangle_provenance",openvdb::StringMetadata(provenance));
    active+=grid->activeVoxelCount(); bytes+=grid->memUsage();
  }
  leaves=distance->tree().leafCount()+(ids?ids->tree().leafCount():0);
  const auto maxLeaves=p.maxVoxelCount/512+(p.maxVoxelCount%512!=0);
  if (active>p.maxVoxelCount || leaves>maxLeaves || distance->tree().leafCount()>blocks(f.bounds,8) ||
      (ids && ids->tree().leafCount()>blocks(f.bounds,8)))
    throw std::invalid_argument("mesh conversion actual output topology exceeds its voxel/preflight budget");
  for (auto leaf=distance->tree().cbeginLeaf();leaf;++leaf) for (int axis=0;axis<3;++axis)
    if (block(leaf->origin()[axis],8)<block(f.bounds.min()[axis],8) ||
        block(leaf->origin()[axis],8)>block(f.bounds.max()[axis],8))
      throw std::invalid_argument("mesh distance leaf lies outside preflight bounds");
  budget(bytes,p,"mesh conversion loaded output grids exceed the input-memory budget");
  detail::WriteFileExclusive(output,grids,p.maxInputBytes);
  return {{p.gridName,"unsigned distance",distance->activeVoxelCount(),p.voxelSize},c.writeTriangleIds?c.triangleGridName:std::string{},triangles.size()};
}
void initialize() { static std::once_flag once; std::call_once(once,[]{openvdb::initialize();}); }

struct Flattened { std::vector<Position> positions; std::vector<Triangle> triangles; std::string provenance; long double ownedBytes; };
Flattened flatten(const dotbim::Model& model,const ProcessingOptions& p) {
  const auto& report=model.importReport;
  if (report.completeness==ImportCompleteness::Partial || report.completeness==ImportCompleteness::Failed ||
      report.skippedProductCount || report.partialProductCount)
    throw std::invalid_argument("mesh conversion cannot omit incomplete imported geometry");
  const long double sourceBytes=model.vertices.size()*static_cast<long double>(sizeof(Vertex))+
    model.indices.size()*sizeof(uint32_t)+model.elements.size()*static_cast<long double>(sizeof(dotbim::Element))+
    model.meshRanges.size()*sizeof(dotbim::MeshRange);
  budget(sourceBytes+Fixed,p,"mesh source geometry exceeds the input-memory budget");
  std::unordered_map<uint32_t,const dotbim::MeshRange*> ranges;
  budget(sourceBytes+model.meshRanges.size()*128.0L+Fixed,p,"mesh range catalog exceeds the input-memory budget");
  for (const auto& range:model.meshRanges)
    if (!ranges.emplace(range.meshId,&range).second) throw std::invalid_argument("mesh source has duplicate mesh IDs");
  uint64_t count=0;
  for (const auto& element:model.elements) {
    if (element.geometryKind!=dotbim::GeometryKind::Mesh) throw std::invalid_argument("mesh conversion requires triangle surfaces; points/curves cannot be omitted");
    const auto found=ranges.find(element.meshId);
    if (found==ranges.end()) throw std::invalid_argument("mesh element references a missing mesh range");
    const auto& range=*found->second;
    if (range.indexCount%3 || uint64_t(range.firstIndex)+range.indexCount>model.indices.size()) throw std::invalid_argument("mesh source has an invalid triangle range");
    count+=range.indexCount/3;
    if (count>p.maxTriangles || count>uint64_t(std::numeric_limits<int32_t>::max())/3)
      throw std::invalid_argument("flattened mesh instances exceed triangle/native vertex limits");
  }
  // Three vertices and an ID-provenance triple per triangle. Reserve the full
  // decimal JSON upper bound and all later metadata copies before allocating.
  const long double owned=sourceBytes+model.meshRanges.size()*128.0L;
  budget(owned+count*(3*sizeof(Position)+sizeof(Triangle)+8.0L*48)+Fixed,p,
         "flattened mesh/provenance workspace exceeds the input-memory budget");
  Flattened result; result.ownedBytes=owned; result.positions.reserve(size_t(count)*3); result.triangles.reserve(size_t(count));
  result.provenance.reserve(size_t(count)*48+2); result.provenance="[";
  for (size_t n=0;n<model.elements.size();++n) {
    const auto& element=model.elements[n]; const auto& range=*ranges.at(element.meshId);
    const glm::dmat4 matrix(element.transform);
    for (int column=0;column<4;++column) for (int row=0;row<4;++row)
      if (!std::isfinite(matrix[column][row])) throw std::invalid_argument("mesh instance transform must be finite");
    const double determinant=glm::determinant(glm::dmat3(matrix));
    if (matrix[0][3]!=0 || matrix[1][3]!=0 || matrix[2][3]!=0 || matrix[3][3]!=1 || !std::isfinite(determinant) || determinant==0)
      throw std::invalid_argument("mesh instance transform must be nonsingular affine");
    for (uint64_t offset=range.firstIndex;offset<uint64_t(range.firstIndex)+range.indexCount;offset+=3) {
      Triangle triangle;
      for (int corner=0;corner<3;++corner) {
        const auto index=model.indices[size_t(offset)+corner];
        if (index>=model.vertices.size()) throw std::invalid_argument("mesh instance references a missing source vertex");
        const auto world=matrix*glm::dvec4(model.vertices[index].position,1.);
        if (!std::isfinite(world.w) || world.w!=1) throw std::invalid_argument("mesh instance vertex is not affine");
        triangle[corner]=uint32_t(result.positions.size()); result.positions.push_back({world.x,world.y,world.z});
      }
      result.triangles.push_back(triangle);
      if (result.triangles.size()>1) result.provenance+=",";
      result.provenance+="["+std::to_string(n)+","+std::to_string(element.meshId)+","+std::to_string((offset-range.firstIndex)/3)+"]";
    }
  }
  result.provenance+="]"; return result;
}
#endif
} // namespace

MeshConversionResult WriteMeshDistance(std::span<const std::array<double,3>> points,std::span<const std::array<uint32_t,3>> triangles,
    const std::filesystem::path& output,const ProcessingOptions& p,const MeshConversionOptions& c) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  options(output,p,c); initialize(); tbb::task_arena arena(1);
  return arena.execute([&]{return convert(points,triangles,output,p,c);});
#else
  (void)points; (void)triangles; (void)output; (void)p; (void)c; unavailable();
#endif
}
MeshConversionResult WriteMeshDistance(const dotbim::Model& model,const std::filesystem::path& output,
    const ProcessingOptions& p,const MeshConversionOptions& c) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  options(output,p,c); initialize(); tbb::task_arena arena(1);
  return arena.execute([&]{const auto input=flatten(model,p); return convert(input.positions,input.triangles,output,p,c,input.ownedBytes,input.provenance);});
#else
  (void)model; (void)output; (void)p; (void)c; unavailable();
#endif
}
} // namespace container::geometry::vdb
