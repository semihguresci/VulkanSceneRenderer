#include "Container/geometry/OpenVdbPointMask.h"
#include "OpenVdbProvenance.h"
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbPointAttributesInternal.h"
#include <openvdb/io/Compression.h>
#include <tbb/task_arena.h>
#include <array>
#include <cmath>
#include <optional>
#include <string_view>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB point voxel masks are unavailable; configure with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using namespace point_attribute_detail;
constexpr std::array<std::string_view,3> ArchiveKeys{
  "container:point_empty_manifest_v1","container:point_empty_descriptor_v1","container:point_empty_leaf_flags_v1"};

struct Predicate {
  std::array<std::vector<points::GroupHandle>,3> handles;
  Predicate(const Leaf& leaf,const PointGroupSelection& selection) {
    size_t index=0;
    for(const auto* names:{&selection.anyOf,&selection.allOf,&selection.noneOf}) {
      handles[index].reserve(names->size());
      for(const auto& n:*names) handles[index].push_back(leaf.groupHandle(n));
      ++index;
    }
  }
  bool operator()(openvdb::Index point) const {
    bool any=handles[0].empty();for(const auto& h:handles[0]) any|=h.get(point);
    if(!any) return false;
    for(const auto& h:handles[1]) if(!h.get(point)) return false;
    for(const auto& h:handles[2]) if(h.get(point)) return false;
    return true;
  }
};

long double predicateBytes(const PointGroupSelection& selection,const PointAttributeOptions& options,
                           const PointAttributeSchema* schema) {
  long double result=4096;size_t count=0;
  for(const auto* names:{&selection.anyOf,&selection.allOf,&selection.noneOf}) for(const auto& n:*names) {
    if(++count>3*options.maxGroups) throw std::invalid_argument("point voxel selection exceeds group-name limits");
    name(n);
    if(schema && std::find(schema->groups.begin(),schema->groups.end(),n)==schema->groups.end())
      throw std::invalid_argument("point voxel selection group was not found: "+n);
    result+=8.0L*n.size()+1024;
  }
  return result;
}

template<class GridT> long double branchBytes() {
  using Root=typename GridT::TreeType::RootNodeType;
  using Upper=typename Root::ChildNodeType;
  using Lower=typename Upper::ChildNodeType;
  using OutputLeaf=typename Lower::ChildNodeType;
  // A separate upper/lower branch for EVERY leaf overestimates sharing.
  return sizeof(Upper)+sizeof(Lower)+sizeof(OutputLeaf)+
      static_cast<long double>(OutputLeaf::NUM_VALUES)*sizeof(typename OutputLeaf::ValueType)+2048;
}

struct Estimate {
  PointMaskResult result;
  uint64_t occupiedLeaves=0;
};
Estimate scan(const Loaded& source,const PointGroupSelection& selection) {
  Estimate result;result.result.inputCount=source.schema.pointCount;
  for(const auto* leaf:source.leaves) {
    Predicate selected(*leaf,selection);bool occupiedLeaf=false;openvdb::Index previous=0;
    for(openvdb::Index voxel=0;voxel<Leaf::NUM_VALUES;++voxel) {
      const auto end=leaf->getValue(voxel);uint64_t count=0;
      for(auto point=previous;point<end;++point) count+=uint64_t(selected(point));
      previous=end;if(!count) continue;
      occupiedLeaf=true;++result.result.occupiedVoxelCount;result.result.selectedCount+=count;
      if(leaf->isValueOn(voxel)) result.result.selectedActive+=count;
      else result.result.selectedInactive+=count;
    }
    result.occupiedLeaves+=uint64_t(occupiedLeaf);
  }
  result.result.leafCount=result.occupiedLeaves;return result;
}

std::string packedSelection(const PointGroupSelection& selection) {
  std::string result="v1;";
  for(const auto* names:{&selection.anyOf,&selection.allOf,&selection.noneOf}) {
    result+=std::to_string(names->size())+":";
    for(const auto& n:*names) result+=std::to_string(n.size())+":"+n;
    result+=';';
  }
  return result;
}

bool structural(std::string_view key) {
  constexpr std::array<std::string_view,8> generated{
    "container:point_voxel_operation","container:point_voxel_source_grid","container:point_voxel_input_count",
    "container:point_voxel_selected_count","container:point_voxel_selected_inactive","container:point_voxel_selection",
    "container:point_voxel_units","container:point_voxel_archived_source"};
  return key==openvdb::GridBase::META_GRID_CLASS || key==openvdb::GridBase::META_GRID_NAME ||
    key==openvdb::GridBase::META_SAVE_HALF_FLOAT || key==openvdb::GridBase::META_IS_LOCAL_SPACE ||
    key==openvdb::GridBase::META_VECTOR_TYPE || key==openvdb::GridBase::META_FILE_BBOX_MIN ||
    key==openvdb::GridBase::META_FILE_BBOX_MAX || key==openvdb::GridBase::META_FILE_COMPRESSION ||
    key==openvdb::GridBase::META_FILE_MEM_BYTES || key==openvdb::GridBase::META_FILE_VOXEL_COUNT ||
    key==openvdb::GridBase::META_FILE_DELAYED_LOAD || key=="container:point_count" ||
    std::find(ArchiveKeys.begin(),ArchiveKeys.end(),key)!=ArchiveKeys.end() ||
    std::find(generated.begin(),generated.end(),key)!=generated.end();
}

void metadata(openvdb::GridBase& output,const Loaded& source,const PointMaskResult& result,
              const std::string& gridName,const std::string& selection,bool counts) {
  output.insertMeta(*source.grid);
  detail::RemoveBoundaryProjectionMetadata(output);
  for(auto it=output.beginMeta();it!=output.endMeta();) {
    const auto key=(it++)->first;if(structural(key)) output.removeMeta(key);
  }
  output.setName(gridName);output.setGridClass(openvdb::GRID_UNKNOWN);
  output.setVectorType(openvdb::VEC_INVARIANT);output.setIsInWorldSpace(true);
  output.setSaveFloatAsHalf(false);
  output.insertMeta("container:point_voxel_operation",openvdb::StringMetadata(counts?"selected-native-point-count":"selected-native-point-occupancy"));
  output.insertMeta("container:point_voxel_source_grid",openvdb::StringMetadata(source.grid->getName()));
  output.insertMeta("container:point_voxel_input_count",openvdb::Int64Metadata(int64_t(result.inputCount)));
  output.insertMeta("container:point_voxel_selected_count",openvdb::Int64Metadata(int64_t(result.selectedCount)));
  output.insertMeta("container:point_voxel_selected_inactive",openvdb::Int64Metadata(int64_t(result.selectedInactive)));
  output.insertMeta("container:point_voxel_selection",openvdb::StringMetadata(selection));
  output.insertMeta("container:point_voxel_units",openvdb::StringMetadata(counts?"selected point records per original voxel":"original source point voxel occupancy"));
  output.insertMeta("container:point_voxel_archived_source",openvdb::BoolMetadata(result.sourceArchivedSchema));
}

template<class GridT> void verify(const GridT& grid,const Estimate& expected,const ProcessingOptions& p) {
  if(grid.tree().hasActiveTiles() || grid.activeVoxelCount()!=expected.result.occupiedVoxelCount ||
     grid.tree().leafCount()!=expected.occupiedLeaves || grid.activeVoxelCount()>p.maxVoxelCount ||
     grid.tree().leafCount()>p.maxVoxelCount/512)
    throw std::runtime_error("point voxel output topology disagrees with preflight or exceeds limits");
}
#endif
} // namespace

PointMaskResult WritePointVoxelMask(const std::filesystem::path& input,const std::filesystem::path& output,
    const PointGroupSelection& selection,const PointMaskOptions& mask,const std::string& inputGrid,
    const ProcessingOptions& p,const PointAttributeOptions& options) {
#if !defined(CONTAINER_HAS_OPENVDB) || !CONTAINER_HAS_OPENVDB
  (void)input;(void)output;(void)selection;(void)mask;(void)inputGrid;(void)p;(void)options;unavailable();
#else
  validateOptions(p,options,true);name(p.gridName);
  if(mask.writeCounts){name(mask.countGridName);if(mask.countGridName==p.gridName)throw std::invalid_argument("point mask/count grid names must be distinct");}
  if(output.empty() || std::filesystem::exists(output) ||
     (output.has_parent_path() && !std::filesystem::is_directory(output.parent_path())))
    throw std::invalid_argument("point voxel output requires a new path in an existing directory");
  const auto initialPredicate=predicateBytes(selection,options,nullptr);
  const auto nameBytes=p.gridName.size()+(mask.writeCounts?mask.countGridName.size():0);
  budget(Fixed+initialPredicate+8.0L*nameBytes,p,"point voxel options exceed memory budget");
  initialize();tbb::task_arena arena(1);
  return arena.execute([&] {
    auto source=load(input,inputGrid,p,options);
    bool archive=false;
    for(auto it=source.grid->beginMeta();it!=source.grid->endMeta();++it)
      archive|=std::find(ArchiveKeys.begin(),ArchiveKeys.end(),it->first)!=ArchiveKeys.end();
    if(archive) {
      // Reuse the validated archive reader on this already-owned immutable
      // snapshot. Its second empty-tree/metadata load shares the same budget.
      const long double retained=source.snapshot->encoded+3*source.decoded+8*source.metadata+initialPredicate+Fixed;
      budget(retained+1,p,"point voxel archived-schema validation exceeds shared memory budget");
      auto nested=p;nested.maxInputBytes-=uintmax_t(std::ceil(retained));
      const auto counted=CountSelectedPoints(source.snapshot->file,selection,inputGrid,nested,options);
      if(!counted.archivedSchema || counted.total)
        throw std::invalid_argument("point voxel input has conflicting/unsupported empty-schema archive metadata");
    } else (void)predicateBytes(selection,options,&source.schema);
    const long double resident=source.snapshot->encoded+4*source.decoded+12*source.metadata+initialPredicate+Fixed;
    budget(resident,p,"point voxel source/predicate workspace exceeds shared memory budget");
    auto estimate=scan(source,selection);
    const uint64_t outputGrids=mask.writeCounts?2:1;
    if(estimate.result.selectedCount>options.maxPoints || estimate.result.selectedCount>uint64_t(INT64_MAX) ||
       estimate.result.occupiedVoxelCount>p.maxVoxelCount/outputGrids || estimate.occupiedLeaves>p.maxVoxelCount/512/outputGrids)
      throw std::invalid_argument("point voxel count/occupied leaf slots exceed point/topology limits");
    estimate.result.maskGridName=p.gridName;estimate.result.sourceArchivedSchema=archive;
    if(mask.writeCounts) estimate.result.countGridName=mask.countGridName;
    const long double treeBytes=estimate.occupiedLeaves*(branchBytes<openvdb::MaskGrid>()+
      (mask.writeCounts?branchBytes<openvdb::Int64Grid>():0));
    const long double outputMetadata=(mask.writeCounts?2:1)*(4*source.metadata+initialPredicate+
        8.0L*nameBytes+16*1024);
    budget(resident+4*(treeBytes+outputMetadata)+Fixed,p,"point voxel prospective leaf/node/metadata/writer workspace exceeds shared memory budget");
    auto occupancy=openvdb::MaskGrid::create(false);
    auto counts=mask.writeCounts?openvdb::Int64Grid::create(int64_t(0)):openvdb::Int64Grid::Ptr{};
    occupancy->setTransform(detail::CopyTransformExact(source.grid->transform()));
    if(counts) counts->setTransform(detail::CopyTransformExact(source.grid->transform()));
    auto accessor=occupancy->getAccessor();
    std::optional<openvdb::Int64Grid::Accessor> countAccessor;
    if(counts) countAccessor.emplace(counts->getAccessor());
    for(const auto* leaf:source.leaves) {
      Predicate selected(*leaf,selection);openvdb::Index previous=0;
      for(openvdb::Index voxel=0;voxel<Leaf::NUM_VALUES;++voxel) {
        const auto end=leaf->getValue(voxel);uint64_t count=0;
        for(auto point=previous;point<end;++point) count+=uint64_t(selected(point));
        previous=end;if(!count) continue;
        const auto coordinate=leaf->offsetToGlobalCoord(voxel);
        accessor.setValueOn(coordinate,true);
        if(counts) countAccessor->setValueOn(coordinate,int64_t(count));
      }
    }
    const auto packed=packedSelection(selection);
    metadata(*occupancy,source,estimate.result,p.gridName,packed,false);
    if(counts) metadata(*counts,source,estimate.result,mask.countGridName,packed,true);
    verify(*occupancy,estimate,p);if(counts) verify(*counts,estimate,p);
    long double actual=occupancy->memUsage()+4*metadataBytes(*occupancy);
    if(counts) actual+=counts->memUsage()+4*metadataBytes(*counts);
    budget(resident+4*actual+Fixed,p,"point voxel output/writer workspace exceeds shared memory budget");
    openvdb::GridPtrVec grids{occupancy};if(counts)grids.push_back(counts);
    static_assert(std::is_nothrow_move_constructible_v<PointMaskResult>);
    detail::WriteFileExclusive(output,grids,p.maxInputBytes,openvdb::io::COMPRESS_ZIP);
    return std::move(estimate.result); // No allocating string copy after publication.
  });
#endif
}
} // namespace container::geometry::vdb
