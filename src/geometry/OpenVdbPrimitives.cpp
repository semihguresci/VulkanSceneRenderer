#include "Container/geometry/OpenVdbPrimitives.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <openvdb/openvdb.h>
#include <openvdb/tools/LevelSetPlatonic.h>
#include <tbb/task_arena.h>
#include <mutex>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB Platonic primitives are unavailable; configure with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
constexpr long double Fixed = 8.0L * 1024 * 1024;
struct Preflight { int64_t extent; float scale; float band; long double leaves; };
void initialize() {
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });
}
long double nodes(int64_t extent, int64_t dimension) {
  const long double width = std::floor(static_cast<long double>(extent)/dimension)-
                            std::floor(-static_cast<long double>(extent)/dimension)+1;
  return width*width*width;
}
template<class Tree> long double treeBytes(int64_t extent) {
  using Leaf = typename Tree::LeafNodeType;
  using Upper = typename Tree::RootNodeType::ChildNodeType;
  using Lower = typename Upper::ChildNodeType;
  return nodes(extent,Leaf::DIM)*(sizeof(Leaf)+Leaf::NUM_VALUES*sizeof(typename Tree::ValueType)+256.0L)+
    nodes(extent,Lower::DIM)*(sizeof(Lower)+256.0L)+nodes(extent,Upper::DIM)*(sizeof(Upper)+512.0L);
}
Preflight preflight(const PlatonicOptions& primitive, const ProcessingOptions& p) {
  uint32_t triangles;
  switch (primitive.faceCount) {
  case 4: triangles=4; break; case 6: triangles=12; break; case 8: triangles=8; break;
  case 12: triangles=36; break; case 20: triangles=20; break;
  default: throw std::invalid_argument("Platonic face count must be 4, 6, 8, 12 or 20");
  }
  if (!std::isfinite(primitive.scale) || primitive.scale<1e-6 || primitive.scale>1e6)
    throw std::invalid_argument("Platonic scale must be in [1e-6,1e6] world units");
  if (!std::isfinite(p.voxelSize) || p.voxelSize<1.5e-5 || p.voxelSize>1e6)
    throw std::invalid_argument("Platonic voxel size must be in [1.5e-5,1e6]; OpenVDB affine maps reject smaller isotropic determinants");
  if (!std::isfinite(p.halfWidth) || p.halfWidth<3 || p.halfWidth>64)
    throw std::invalid_argument("Platonic narrow-band half width must be in [3,64] voxels");
  if (!p.maxVoxelCount || !p.maxInputBytes || !p.maxTriangles)
    throw std::invalid_argument("Platonic processing budgets must be positive");
  if (p.maxTriangles<triangles) throw std::invalid_argument("Platonic input surface exceeds the triangle budget");
  if (p.gridName.empty() || p.gridName.size()>256)
    throw std::invalid_argument("Platonic output grid name must contain 1..256 bytes");
  const double ratio=primitive.scale/p.voxelSize;
  if (!std::isfinite(ratio) || ratio<5*(1-8*std::numeric_limits<double>::epsilon()))
    throw std::invalid_argument("Platonic scale must span at least five source voxels for a resolved interior");
  const float scale=static_cast<float>(ratio), band=static_cast<float>(p.halfWidth);
  const double radius=(primitive.faceCount==6?.5:1.)*double(scale);
  const double extent=std::ceil(radius+band+8.0);
  if (!std::isfinite(extent) || extent>double(std::numeric_limits<int32_t>::max())-64)
    throw std::invalid_argument("Platonic bounds exceed safe OpenVDB integer coordinates");
  const float floatRadius=static_cast<float>(radius);
  if (double(std::nextafter(floatRadius,std::numeric_limits<float>::infinity())-floatRadius)>1e-3)
    throw std::invalid_argument("Platonic canonical vertices have insufficient Float32 voxel precision; increase voxel size");
  const double worldExtent=extent*p.voxelSize;
  for (double center:primitive.center) {
    if (!std::isfinite(center)) throw std::invalid_argument("Platonic center must be finite");
    for (double point:{center-worldExtent,center,center+worldExtent}) {
      const double gap=std::max(std::abs(std::nextafter(point,std::numeric_limits<double>::infinity())-point),
                                std::abs(std::nextafter(point,-std::numeric_limits<double>::infinity())-point));
      if (!std::isfinite(point) || !std::isfinite(gap) || gap>p.voxelSize*1e-4)
        throw std::invalid_argument("Platonic center/world bounds have insufficient Double precision; recenter the primitive");
    }
  }
  const auto box=static_cast<int64_t>(extent);
  const long double voxels=nodes(box,1), leaves=nodes(box,openvdb::FloatTree::LeafNodeType::DIM);
  const uint64_t maxLeaves=p.maxVoxelCount/512+(p.maxVoxelCount%512!=0);
  if (voxels>p.maxVoxelCount || leaves>maxLeaves)
    throw std::invalid_argument("Platonic bounded/stored topology exceeds the voxel budget; increase voxel size or maxVoxelCount");
  // MeshToVolume keeps a distance tree, polygon-index tree, masks, local
  // fragment trees and exterior/sign/reinitialization work. Bound the entire
  // padded box for each allocation, not only the final active narrow band.
  const long double workspace=8*treeBytes<openvdb::FloatTree>(box)+4*treeBytes<openvdb::Int32Tree>(box)+
    4*treeBytes<openvdb::BoolTree>(box)+64*voxels+Fixed+16*p.gridName.size();
  if (!std::isfinite(workspace) || workspace>p.maxInputBytes)
    throw std::invalid_argument("Platonic meshToVolume tree/mask workspace exceeds the input-memory budget");
  return {box,scale,band,leaves};
}
void outputPath(const std::filesystem::path& output) {
  if (output.empty() || std::filesystem::exists(output)) throw std::invalid_argument("Platonic output must be a new nonempty path");
  if (output.has_parent_path() && !std::filesystem::is_directory(output.parent_path()))
    throw std::invalid_argument("Platonic output directory does not exist");
}
void validateGrid(const openvdb::FloatGrid& grid, const Preflight& f, const ProcessingOptions& p) {
  if (grid.getGridClass()!=openvdb::GRID_LEVEL_SET || grid.activeVoxelCount()==0 ||
      grid.activeVoxelCount()>p.maxVoxelCount || grid.tree().leafCount()>f.leaves ||
      grid.memUsage()+Fixed+16*p.gridName.size()>p.maxInputBytes)
    throw std::invalid_argument("generated Platonic level set exceeds topology/memory limits or has no surface");
  if (!std::isfinite(grid.background()) || grid.background()<=0 || grid.tree().getValue(openvdb::Coord(0))>=0)
    throw std::invalid_argument("generated Platonic level set has no valid signed interior");
  const double band=grid.background();
  for (auto value=grid.cbeginValueAll();value;++value)
    if (!std::isfinite(*value) || std::abs(double(*value))>band*(1+8*std::numeric_limits<float>::epsilon()))
      throw std::invalid_argument("generated Platonic distances are nonfinite or outside the narrow band");
  const auto bounds=grid.evalActiveVoxelBoundingBox();
  for (size_t axis=0;axis<3;++axis)
    if (bounds.min()[axis]<-f.extent || bounds.max()[axis]>f.extent)
      throw std::invalid_argument("generated Platonic active bounds exceed their preflight estimate");
}
#endif
} // namespace

ProcessingResult WritePlatonicLevelSet(const std::filesystem::path& output, const PlatonicOptions& primitive,
                                      const ProcessingOptions& p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  outputPath(output); initialize(); tbb::task_arena arena(1);
  return arena.execute([&] {
    const auto f=preflight(primitive,p);
    auto grid=openvdb::tools::createLevelSetPlatonic<openvdb::FloatGrid>(
      static_cast<int>(primitive.faceCount),f.scale,openvdb::Vec3f(0),1.0f,f.band);
    const float background=static_cast<float>(double(grid->background())*p.voxelSize);
    for (auto value=grid->beginValueAll();value;++value) {
      const bool active=value.isValueOn();
      value.setValue(static_cast<float>(double(*value)*p.voxelSize)); value.setActiveState(active);
    }
    grid->tree().root().setBackground(background,false);
    auto matrix=openvdb::math::Mat4d::identity();
    for(size_t axis=0;axis<3;++axis) {matrix[axis][axis]=p.voxelSize;matrix[3][axis]=primitive.center[axis];}
    grid->setTransform(detail::MakeAffineTransformExact(matrix));
    grid->setName(p.gridName);
    grid->insertMeta("container:primitive",openvdb::StringMetadata("platonic"));
    grid->insertMeta("container:platonic_faces",openvdb::Int32Metadata(static_cast<int32_t>(primitive.faceCount)));
    grid->insertMeta("container:platonic_scale",openvdb::DoubleMetadata(primitive.scale));
    grid->insertMeta("container:platonic_center",openvdb::Vec3DMetadata({primitive.center[0],primitive.center[1],primitive.center[2]}));
    validateGrid(*grid,f,p);
    detail::WriteFileExclusive(output,{grid},p.maxInputBytes);
    return ProcessingResult{grid->getName(),"level set",grid->activeVoxelCount(),p.voxelSize};
  });
#else
  (void)output; (void)primitive; (void)p; unavailable();
#endif
}
} // namespace container::geometry::vdb
