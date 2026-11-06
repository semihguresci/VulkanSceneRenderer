#pragma once

#include <openvdb/io/Archive.h>
#include <openvdb/io/GridDescriptor.h>
#include <openvdb/openvdb.h>
#include <fstream>
#include <functional>
#include <stdexcept>

namespace container::geometry::vdb::detail {

// The existing Inspector/PointMotion raw-Archive policy, factored for native
// point readers. File::readGridMetadata() copies the transform through the
// simplifying MapBase factory; a persisted tiny scale can then throw or change.
// This reads the same metadata/map record without copying or normalizing it.
class ExactMetadataArchive : public openvdb::io::Archive {
public:
  void begin(std::istream& stream, openvdb::io::StreamMetadata::Ptr& metadata) {
    readHeader(stream);
    if (!inputHasGridOffsets() ||
        fileVersion() < openvdb::OPENVDB_FILE_VERSION_GRID_INSTANCING ||
        fileVersion() > openvdb::OPENVDB_FILE_VERSION)
      throw std::invalid_argument("point metadata requires supported seekable metadata-readable VDB");
    openvdb::io::setStreamMetadataPtr(stream, metadata, false);
    setFormatVersion(stream);
    setLibraryVersion(stream);
    setDataCompression(stream);
  }
  int32_t count(std::istream& stream) { return readGridCount(stream); }
  openvdb::GridBase::Ptr next(std::istream& stream,
                            openvdb::io::GridDescriptor& descriptor,
                            uintmax_t fileBytes) {
    auto grid = descriptor.read(stream);
    const auto afterDescriptor = int64_t(stream.tellg());
    const auto start = descriptor.getGridPos(), end = descriptor.getEndPos();
    const auto blocks = descriptor.getBlockPos();
    if (!grid || start < afterDescriptor || end <= start || uintmax_t(end) > fileBytes ||
        (!descriptor.isInstance() && (blocks < start || blocks > end)))
      throw std::invalid_argument("point metadata descriptor offsets exceed encoded file");
    descriptor.seekToGrid(stream);
    readGridCompression(stream);
    grid->readMeta(stream);
    grid->readTransform(stream);
    if (fileVersion() < openvdb::OPENVDB_FILE_VERSION_NO_GRIDMAP && grid->getName().empty())
      grid->setName(descriptor.gridName());
    if (int64_t(stream.tellg()) > (descriptor.isInstance() ? end : blocks))
      throw std::invalid_argument("point metadata/transform crosses payload boundary");
    grid->removeMeta(openvdb::GridBase::META_FILE_DELAYED_LOAD);
    descriptor.seekToEnd(stream);
    return grid;
  }
};
} // namespace container::geometry::vdb::detail
