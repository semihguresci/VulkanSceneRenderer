#include "OpenVdbPointMotionCore.h"

namespace container::geometry::vdb {
PointMotionResult WriteMovedPoints(const std::filesystem::path& input,const std::filesystem::path& output,const PointMotionOptions& o,const std::string& selector,const ProcessingOptions& p,const PointAttributeOptions& limits) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(p,limits,true);matrix(o.worldFromWorld);if(o.targetIndexToWorld)matrix(*o.targetIndexToWorld);
  if(!o.maxWork || o.maxWork>10'000'000'000ULL || !o.maxGrids || o.maxGrids>4096 || !o.maxMetadataBytes ||
     !std::isfinite(o.maxPositionErrorVoxels) || o.maxPositionErrorVoxels<=0 || o.maxPositionErrorVoxels>.25)
    throw std::invalid_argument("invalid point motion work/catalog/metadata/position-error controls");
  size_t terms=0;for(const auto* names:{&o.groups.anyOf,&o.groups.allOf,&o.groups.noneOf})for(const auto& n:*names) {
    name(n);if(++terms>3*limits.maxGroups)throw std::invalid_argument("point motion group predicate exceeds count cap");
  }
  if(output.empty() || std::filesystem::exists(output) || !std::filesystem::is_directory(output.has_parent_path()?output.parent_path():std::filesystem::path(".")))
    throw std::invalid_argument("point motion output must be new in an existing directory");
  initialize();tbb::task_arena arena(1);return arena.execute([&]{return execute(input,output,o,selector,p,limits);});
#else
  (void)input;(void)output;(void)o;(void)selector;(void)p;(void)limits;unavailable();
#endif
}
} // namespace container::geometry::vdb
