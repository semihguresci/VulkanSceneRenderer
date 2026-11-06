#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#define CONTAINER_FAST_SWEEPING_GRID openvdb::FloatGrid
#define CONTAINER_FAST_SWEEPING_VECTOR_GRID openvdb::Vec3SGrid
#define CONTAINER_FAST_SWEEPING_RUN runFloat
#include "OpenVdbFastSweepingNative.inl"
#endif
