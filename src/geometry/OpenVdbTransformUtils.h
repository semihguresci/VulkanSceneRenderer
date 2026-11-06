#pragma once
#include <openvdb/math/Transform.h>
#include <openvdb/openvdb.h>
#include <openvdb/io/io.h>
#include <openvdb/util/Name.h>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace container::geometry::vdb::detail {
// Transform::copy() invokes the MapBase constructor, which simplifies affine
// maps. Scale+translation simplification subtracts translated unit positions;
// a large origin can therefore change a scale, and tiny shear can be dropped.
// The Transform copy constructor retains the original immutable map instead.
// Standard map edits replace their map pointer, so subsequent Transform edits
// do not change the source. Our callers never mutate a shared MapBase directly.
inline openvdb::math::Transform::Ptr CopyTransformExact(const openvdb::math::Transform &source) {
  return std::make_shared<openvdb::math::Transform>(source);
}
template<class Grid> auto CopyGridWithNewTreeExact(const Grid &source) {
  auto result=Grid::create(source.background());
  result->insertMeta(source);
  result->setTransform(CopyTransformExact(source.transform()));
  return result;
}
template<class Grid> auto DeepCopyGridExact(const Grid &source) {
  // Calling Grid::deepCopy() first can itself throw when simplification rounds
  // a small scale against a huge origin to zero. Copy tree/metadata separately.
  auto result=CopyGridWithNewTreeExact(source);
  result->setTree(std::make_shared<typename Grid::TreeType>(source.tree()));
  return result;
}
// Construct a native AffineMap through its public serialization API. Both
// Transform(MapBase) and createLinearTransform(Mat4) simplify, so neither is an
// exact matrix factory. This bounded record avoids that simplification without
// accessing private members or registering an application-specific map type.
inline openvdb::math::Transform::Ptr MakeAffineTransformExact(const openvdb::math::Mat4d &matrix) {
  // Dense protocol validation can reach this before its first VDB file call.
  // Native initialize() is already guarded by the SDK's process-wide mutex.
  openvdb::initialize();
  if(matrix[0][3]!=0 || matrix[1][3]!=0 || matrix[2][3]!=0 || matrix[3][3]!=1)
    throw std::invalid_argument("exact OpenVDB map factory requires an affine matrix");
  bool diagonal=true;
  for(size_t r=0;r<3;++r) for(size_t c=0;c<3;++c) if(r!=c && matrix[r][c]!=0) diagonal=false;
  openvdb::math::MapBase::Ptr map;
  if(diagonal) {
    const openvdb::Vec3d scale(matrix[0][0],matrix[1][1],matrix[2][2]);
    const openvdb::Vec3d translation(matrix[3][0],matrix[3][1],matrix[3][2]);
    // ScaleTranslateMap computes voxel sizes/inverses directly from scales;
    // general AffineMap derives voxel sizes by translated-point subtraction.
    if(scale[0]==scale[1] && scale[0]==scale[2])
      map=std::make_shared<openvdb::math::UniformScaleTranslateMap>(scale[0],translation);
    else map=std::make_shared<openvdb::math::ScaleTranslateMap>(scale,translation);
  } else map=std::make_shared<openvdb::math::AffineMap>(matrix);
  std::stringstream record(std::ios::in|std::ios::out|std::ios::binary);
  record.exceptions(std::ios::badbit|std::ios::failbit);
  openvdb::writeString(record,map->type());
  map->write(record);
  record.seekg(0); openvdb::io::setCurrentVersion(record);
  auto result=std::make_shared<openvdb::math::Transform>(); result->read(record);
  return result;
}
} // namespace container::geometry::vdb::detail
