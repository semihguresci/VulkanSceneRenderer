#pragma once
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/Grid.h>

namespace container::geometry::vdb::detail {
// Derived values/topology cannot certify membership of the old canonical MAC
// boundary table. Exact copy workflows deliberately choose whether to call.
// Remove exact reserved keys regardless of their registered metadata type;
// unrelated custom boundary metadata belongs to the caller and survives.
inline void RemoveBoundaryProjectionMetadata(openvdb::GridBase& grid) {
  for(const char* key:{"container:boundary_projection_schema",
                      "container:boundary_face_channels",
                      "container:boundary_projection_state_id",
                      "container:boundary_clamped_faces",
                      "container:boundary_maximum_clamp"})
    grid.removeMeta(key);
}
} // namespace container::geometry::vdb::detail
#endif
