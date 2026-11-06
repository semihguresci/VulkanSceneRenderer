#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#define CONTAINER_FAST_SWEEPING_GRID openvdb::DoubleGrid
#define CONTAINER_FAST_SWEEPING_VECTOR_GRID openvdb::Vec3DGrid
#define CONTAINER_FAST_SWEEPING_RUN runDouble
#include "OpenVdbFastSweepingNative.inl"
#endif
