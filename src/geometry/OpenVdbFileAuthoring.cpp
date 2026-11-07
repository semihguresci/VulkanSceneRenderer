#include "Container/geometry/OpenVdbFileAuthoring.h"
#include "Container/geometry/OpenVdbGridSelection.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <openvdb/openvdb.h>
#include <openvdb/points/PointAttribute.h>
#include <openvdb/points/PointDataGrid.h>
#include <openvdb/points/PointGroup.h>
#include <set>
#include <sstream>
#include <tbb/task_arena.h>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB file authoring is unavailable; configure "
                           "with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
namespace points = openvdb::points;
constexpr uintmax_t Fixed = 4ull * 1024 * 1024;

void add(uintmax_t &total, uintmax_t value, uintmax_t limit, const char *why) {
  if (total > limit || value > limit - total)
    throw std::invalid_argument(why);
  total += value;
}
void name(const std::string &value, const char *why) {
  if (value.empty() || value.size() > 256 ||
      value.find('\0') != std::string::npos)
    throw std::invalid_argument(why);
}
void selector(const std::string &value) {
  if (value.empty() || value.size() > 512 ||
      value.find('\0') != std::string::npos)
    throw std::invalid_argument(
        "file grid selectors must be nonempty, bounded names");
}
bool reserved(const std::string &key) {
  return key == openvdb::GridBase::META_GRID_NAME ||
         key == openvdb::GridBase::META_GRID_CLASS ||
         key == openvdb::GridBase::META_SAVE_HALF_FLOAT ||
         key == openvdb::GridBase::META_IS_LOCAL_SPACE ||
         key == openvdb::GridBase::META_VECTOR_TYPE ||
         key == openvdb::GridBase::META_FILE_BBOX_MIN ||
         key == openvdb::GridBase::META_FILE_BBOX_MAX ||
         key == openvdb::GridBase::META_FILE_COMPRESSION ||
         key == openvdb::GridBase::META_FILE_MEM_BYTES ||
         key == openvdb::GridBase::META_FILE_VOXEL_COUNT ||
         key == openvdb::GridBase::META_FILE_DELAYED_LOAD;
}
template <class T> void finite(const T &value) {
  if constexpr (std::is_floating_point_v<T>) {
    if (!std::isfinite(value))
      throw std::invalid_argument("new numeric metadata must be finite");
  } else if constexpr (std::is_same_v<T, std::array<float, 3>> ||
                       std::is_same_v<T, std::array<double, 3>>) {
    for (const auto component : value)
      finite(component);
  }
}
uintmax_t editBytes(const std::vector<FileMetadataEdit> &edits,
                    uintmax_t limit) {
  uintmax_t bytes = 0;
  std::set<std::string> keys;
  for (const auto &edit : edits) {
    name(edit.name, "metadata keys must contain 1..256 bytes without NUL");
    if (reserved(edit.name))
      throw std::invalid_argument(
          "structural grid/file-stat metadata keys cannot be edited");
    if (!keys.insert(edit.name).second)
      throw std::invalid_argument("duplicate metadata key in one edit batch");
    if (edit.operation != FileMetadataOperation::Set &&
        edit.operation != FileMetadataOperation::Remove)
      throw std::invalid_argument("invalid file metadata operation");
    add(bytes, edit.name.size() + 256, limit,
        "metadata edit payload exceeds maxMetadataBytes");
    if (edit.operation == FileMetadataOperation::Set)
      std::visit(
          [&](const auto &value) {
            finite(value);
            using T = std::decay_t<decltype(value)>;
            const uintmax_t size = [&] {
              if constexpr (std::is_same_v<T, std::string>)
                return value.size();
              else
                return sizeof(T);
            }();
            add(bytes, size, limit,
                "metadata edit payload exceeds maxMetadataBytes");
          },
          edit.value);
  }
  return bytes;
}
uintmax_t validate(const FileAuthoringOptions &o, const ProcessingOptions &p) {
  if (o.inputs.empty() || o.inputs.size() > 16 || !o.maxGrids ||
      o.maxGrids > 4096 || !o.maxMetadataBytes || !o.maxEdits ||
      o.maxEdits > 65536 || !o.maxPoints || !o.maxPointAttributes ||
      o.maxPointAttributes > 1024 || !o.maxPointGroups ||
      o.maxPointGroups > 4096 || !p.maxInputBytes || !p.maxVoxelCount)
    throw std::invalid_argument(
        "invalid file authoring count, metadata or memory limits");
  if (o.metadataConflict != FileMetadataConflict::Reject &&
      o.metadataConflict != FileMetadataConflict::PreferFirst &&
      o.metadataConflict != FileMetadataConflict::PreferLast)
    throw std::invalid_argument("invalid file metadata conflict policy");
  if (o.compression != FileCompression::Zip &&
      o.compression != FileCompression::Blosc &&
      o.compression != FileCompression::None)
    throw std::invalid_argument("invalid file compression policy");
#ifndef OPENVDB_USE_BLOSC
  if (o.compression == FileCompression::Blosc)
    throw std::invalid_argument("this OpenVDB build has no Blosc compressor");
#endif
#ifndef OPENVDB_USE_ZLIB
  if (o.compression == FileCompression::Zip)
    throw std::invalid_argument(
        "this OpenVDB build has no ZIP/Zlib compressor");
#endif
  uintmax_t operations = 0, bytes = editBytes(o.metadata, o.maxMetadataBytes);
  add(operations, o.metadata.size(), o.maxEdits,
      "file authoring exceeds maxEdits");
  for (const auto &input : o.inputs) {
    add(operations, input.select.size(), o.maxEdits,
        "file authoring exceeds maxEdits");
    add(operations, input.remove.size(), o.maxEdits,
        "file authoring exceeds maxEdits");
    add(operations, input.edits.size(), o.maxEdits,
        "file authoring exceeds maxEdits");
    for (const auto &selection : input.select)
      selector(selection);
    for (const auto &selection : input.remove)
      selector(selection);
    for (const auto &edit : input.edits) {
      selector(edit.selector);
      if (edit.newName)
        name(*edit.newName,
             "new grid names must contain 1..256 bytes without NUL");
      add(operations, edit.metadata.size(), o.maxEdits,
          "file authoring exceeds maxEdits");
      add(bytes, editBytes(edit.metadata, o.maxMetadataBytes),
          o.maxMetadataBytes,
          "aggregate metadata edit payload exceeds maxMetadataBytes");
    }
  }
  return bytes;
}
void initialize() {
  static std::once_flag once;
  std::call_once(once, [] {
    openvdb::initialize();
    if (!points::PointDataGrid::isRegistered())
      points::PointDataGrid::registerGrid();
    if (!points::StringAttributeArray::isRegistered())
      points::StringAttributeArray::registerType();
    if (!points::GroupAttributeArray::isRegistered())
      points::GroupAttributeArray::registerType();
    const auto registerAttribute = []<class T>() {
      using Array = points::TypedAttributeArray<T, points::NullCodec>;
      if (!Array::isRegistered())
        Array::registerType();
    };
    registerAttribute.template operator()<bool>();
    registerAttribute.template operator()<int32_t>();
    registerAttribute.template operator()<int64_t>();
    registerAttribute.template operator()<float>();
    registerAttribute.template operator()<double>();
    registerAttribute.template operator()<openvdb::Vec3i>();
    registerAttribute.template operator()<openvdb::Vec3f>();
    registerAttribute.template operator()<openvdb::Vec3d>();
  });
}
struct Usage {
  const FileAuthoringOptions &o;
  const ProcessingOptions &p;
  uintmax_t encoded{0}, resident{0}, metadata{0}, catalogue{0};
  uintmax_t active{0}, slots{0}, points{0};
  void check(uintmax_t prospective = 0) const {
    uintmax_t total = 0;
    const auto cap = p.maxInputBytes;
    const char *why =
        "combined file authoring encoded/topology/metadata/writer workspace "
        "exceeds maxInputBytes";
    add(total, Fixed, cap, why);
    add(total, encoded, cap, why);
    add(total, catalogue, cap, why);
    for (unsigned n = 0; n < 4; ++n) {
      add(total, resident, cap, why);
      add(total, prospective, cap, why);
    }
    for (unsigned n = 0; n < 8; ++n)
      add(total, metadata, cap, why);
  }
  void meta(const openvdb::MetaMap &map) {
    uintmax_t bytes = sizeof(openvdb::MetaMap);
    for (auto it = map.beginMeta(); it != map.endMeta(); ++it) {
      if (!it->second)
        throw std::invalid_argument("null metadata is not serializable");
      add(bytes, it->first.size(), o.maxMetadataBytes,
          "metadata exceeds maxMetadataBytes");
      add(bytes, it->second->typeName().size(), o.maxMetadataBytes,
          "metadata exceeds maxMetadataBytes");
      add(bytes, it->second->size(), o.maxMetadataBytes,
          "metadata exceeds maxMetadataBytes");
      add(bytes, 256, o.maxMetadataBytes, "metadata exceeds maxMetadataBytes");
    }
    add(metadata, bytes, o.maxMetadataBytes,
        "aggregate file/grid/point metadata exceeds maxMetadataBytes");
    check();
  }
  void text(uintmax_t bytes) {
    add(metadata, bytes, o.maxMetadataBytes,
        "aggregate point schema strings exceed maxMetadataBytes");
    check();
  }
};
struct Temporary {
  std::filesystem::path directory;
  explicit Temporary(const std::filesystem::path &parent) {
    static std::atomic_uint64_t serial{0};
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
      const auto path =
          parent /
          (".container-author-vdb-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(++serial));
      if (std::filesystem::create_directory(path)) {
        directory = path;
        return;
      }
    }
    throw std::runtime_error(
        "could not reserve file authoring temporary directory");
  }
  ~Temporary() {
    // Only this atomically reserved directory and its known files are owned.
    std::error_code ignored;
    for (auto it = std::filesystem::directory_iterator(directory, ignored);
         !ignored && it != std::filesystem::directory_iterator();
         it.increment(ignored))
      std::filesystem::remove(it->path(), ignored);
    std::filesystem::remove(directory, ignored);
  }
};
void copyBounded(const std::filesystem::path &sourcePath,
                 const std::filesystem::path &targetPath, uintmax_t length) {
  std::ifstream source(sourcePath, std::ios::binary);
  std::ofstream target(targetPath,
                       std::ios::binary | std::ios::out | std::ios::noreplace);
  if (!source || !target)
    throw std::runtime_error("could not open bounded VDB copy");
  std::array<char, 64 * 1024> buffer{};
  auto remaining = length;
  while (remaining) {
    const auto n = static_cast<std::streamsize>(
        std::min<uintmax_t>(remaining, buffer.size()));
    source.read(buffer.data(), n);
    if (source.gcount() != n)
      throw std::runtime_error("VDB source changed size during bounded copy");
    target.write(buffer.data(), n);
    if (!target)
      throw std::runtime_error("failed bounded VDB copy");
    remaining -= uintmax_t(n);
  }
  target.flush();
  if (!target || source.bad() || source.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("VDB source changed size or bounded copy failed");
  target.close();
  if (!target)
    throw std::runtime_error("could not close bounded VDB copy");
}
bool supported(const openvdb::GridBase &g) {
  return g.isType<openvdb::FloatGrid>() || g.isType<openvdb::DoubleGrid>() ||
         g.isType<openvdb::BoolGrid>() || g.isType<openvdb::MaskGrid>() ||
         g.isType<openvdb::Int32Grid>() || g.isType<openvdb::Int64Grid>() ||
         g.isType<openvdb::Vec3IGrid>() || g.isType<openvdb::Vec3SGrid>() ||
         g.isType<openvdb::Vec3DGrid>() || g.isType<points::PointDataGrid>();
}
uintmax_t leafSlots(const openvdb::GridBase &g, uintmax_t cap) {
  const auto leaves = g.baseTree().leafCount();
  if (leaves > cap / 512)
    throw std::invalid_argument(
        "aggregate stored leaf slots exceed maxVoxelCount");
  return leaves * 512;
}
uintmax_t loadedTreeBytes(const openvdb::GridBase &grid) {
  uintmax_t result = 0;
  const auto count = [&]<class Grid>() {
    if (!grid.isType<Grid>())
      return false;
    result = openvdb::tools::memUsageIfLoaded(
        static_cast<const Grid &>(grid).tree(), false);
    return true;
  };
  if (!(count.template operator()<openvdb::FloatGrid>() ||
        count.template operator()<openvdb::DoubleGrid>() ||
        count.template operator()<openvdb::BoolGrid>() ||
        count.template operator()<openvdb::MaskGrid>() ||
        count.template operator()<openvdb::Int32Grid>() ||
        count.template operator()<openvdb::Int64Grid>() ||
        count.template operator()<openvdb::Vec3IGrid>() ||
        count.template operator()<openvdb::Vec3SGrid>() ||
        count.template operator()<openvdb::Vec3DGrid>() ||
        count.template operator()<points::PointDataGrid>()))
    throw std::logic_error(
        "unsupported file tree reached loaded-memory accounting");
  return result;
}
struct CatalogEntry {
  std::string descriptor;
  openvdb::GridBase::Ptr metadata;
};
size_t resolve(const std::string &selection,
               const std::vector<CatalogEntry> &catalogue) {
  std::optional<size_t> found;
  for (size_t n = 0; n < catalogue.size(); ++n) {
    const auto &entry = catalogue[n];
    if (!GridSelectionMatches(selection, entry.descriptor,
                              entry.metadata->getName()))
      continue;
    if (found)
      throw std::invalid_argument("file grid selector is ambiguous; use "
                                  "descriptor:<exact descriptor name>");
    found = n;
  }
  if (!found)
    throw std::invalid_argument("file grid selector matched no grid");
  return *found;
}
openvdb::Metadata::Ptr native(const FileMetadataValue &value) {
  return std::visit(
      [](const auto &item) -> openvdb::Metadata::Ptr {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::array<float, 3>>)
          return std::make_shared<openvdb::Vec3SMetadata>(
              openvdb::Vec3f(item.data()));
        else if constexpr (std::is_same_v<T, std::array<double, 3>>)
          return std::make_shared<openvdb::Vec3DMetadata>(
              openvdb::Vec3d(item.data()));
        else
          return std::make_shared<openvdb::TypedMetadata<T>>(item);
      },
      value);
}
void apply(openvdb::MetaMap &map, const std::vector<FileMetadataEdit> &edits,
           FileAuthoringResult &result) {
  for (const auto &edit : edits) {
    if (edit.operation == FileMetadataOperation::Remove && !map[edit.name])
      throw std::invalid_argument("metadata Remove requires an existing key");
    map.removeMeta(edit.name);
    if (edit.operation == FileMetadataOperation::Set)
      map.insertMeta(edit.name, *native(edit.value));
    ++result.metadataEditCount;
  }
}
bool sameMetadataBits(const openvdb::Metadata &a, const openvdb::Metadata &b) {
  if (a.typeName() != b.typeName() || a.size() != b.size())
    return false;
  std::ostringstream x(std::ios::binary), y(std::ios::binary);
  a.write(x);
  b.write(y);
  return x.str() == y.str();
}
void mergeMetadata(openvdb::MetaMap &target, const openvdb::MetaMap &source,
                   FileMetadataConflict policy) {
  for (auto it = source.beginMeta(); it != source.endMeta(); ++it) {
    const auto existing = target[it->first];
    if (existing && sameMetadataBits(*existing, *it->second))
      continue;
    if (existing) {
      if (policy == FileMetadataConflict::Reject)
        throw std::invalid_argument("conflicting file metadata key: " +
                                    it->first);
      if (policy == FileMetadataConflict::PreferFirst)
        continue;
      target.removeMeta(it->first);
    }
    target.insertMeta(it->first, *it->second);
  }
}
uintmax_t pointPayload(const points::PointDataGrid &grid, Usage &usage) {
  uintmax_t bytes = 0;
  for (auto leaf = grid.tree().cbeginLeaf(); leaf; ++leaf) {
    leaf->validateOffsets();
    const auto count = leaf->pointCount();
    add(usage.points, count, usage.o.maxPoints,
        "aggregate point records exceed maxPoints");
    const auto &set = leaf->attributeSet();
    if (set.size() > usage.o.maxPointAttributes ||
        set.descriptor().groupMap().size() > usage.o.maxPointGroups)
      throw std::invalid_argument(
          "point attribute/group schema exceeds file authoring limits");
    usage.meta(set.descriptor().getMetadata());
    for (const auto &[key, index] : set.descriptor().map()) {
      (void)index;
      usage.text(key.size() + 128);
    }
    for (const auto &[key, offset] : set.descriptor().groupMap()) {
      (void)offset;
      usage.text(key.size() + 128);
    }
    for (size_t n = 0; n < set.size(); ++n) {
      const auto &array = leaf->constAttributeArray(n);
      usage.text(array.type().first.size() + array.type().second.size() + 128);
      if (array.isTransient() || array.isStreaming())
        throw std::invalid_argument("transient/streaming point arrays cannot "
                                    "be preserved by the VDB writer");
      if (array.size() != count)
        throw std::invalid_argument(
            "point attribute array length disagrees with native offsets");
      add(bytes, array.memUsageIfLoaded(), usage.p.maxInputBytes,
          "decoded point attributes exceed maxInputBytes");
    }
  }
  usage.check(bytes);
  return bytes;
}
void forcePointPayload(const points::PointDataGrid &grid) {
  // No typed handles: their streaming/compaction behavior is not a raw copy.
  for (auto leaf = grid.tree().cbeginLeaf(); leaf; ++leaf)
    for (size_t n = 0; n < leaf->attributeSet().size(); ++n)
      leaf->constAttributeArray(n).loadData();
}
uintmax_t write(const std::filesystem::path &temporary,
                const std::filesystem::path &output,
                const openvdb::GridPtrVec &grids,
                const openvdb::MetaMap &metadata, uint32_t compression,
                uintmax_t cap) {
  openvdb::io::File file(temporary.string());
  file.setCompression(compression);
  file.write(grids, metadata);
  file.close();
  const auto bytes = std::filesystem::file_size(temporary);
  if (bytes > cap)
    throw std::invalid_argument(
        "encoded authored VDB output exceeds maxInputBytes");
  // Publication uses noreplace, never a check-then-rename overwrite. If copying
  // fails after creation, remove only the destination created by this call.
  std::ofstream destination(output, std::ios::binary | std::ios::out |
                                        std::ios::noreplace);
  if (!destination)
    throw std::runtime_error(
        "could not exclusively create authored VDB output");
  try {
    std::ifstream source(temporary, std::ios::binary);
    if (!source)
      throw std::runtime_error("could not read authored temporary VDB");
    destination.exceptions(std::ios::badbit | std::ios::failbit);
    std::array<char, 64 * 1024> buffer{};
    while (source.read(buffer.data(), std::streamsize(buffer.size())) ||
           source.gcount())
      destination.write(buffer.data(), source.gcount());
    if (!source.eof())
      throw std::runtime_error("failed authored VDB publication read");
    destination.flush();
    destination.close();
  } catch (...) {
    destination.exceptions(std::ios::goodbit);
    if (destination.is_open())
      destination.close();
    std::error_code ignored;
    std::filesystem::remove(output, ignored);
    throw;
  }
  return bytes;
}
#endif
} // namespace

FileAuthoringResult WriteAuthoredVdb(const std::filesystem::path &output,
                                     const FileAuthoringOptions &o,
                                     const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  const auto editsBytes = validate(o, p);
  if (output.empty() || std::filesystem::exists(output))
    throw std::invalid_argument("file authoring output must be a new path");
  const auto parent = output.has_parent_path() ? output.parent_path()
                                               : std::filesystem::path(".");
  if (!std::filesystem::is_directory(parent))
    throw std::invalid_argument(
        "file authoring output directory does not exist");
  initialize();
  if (!openvdb::io::Archive::isDelayedLoadingEnabled())
    throw std::invalid_argument("bounded file authoring requires delayed "
                                "loading; unset OPENVDB_DISABLE_DELAYED_LOAD");
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument(
      "bounded file authoring requires OpenVDB delayed-loading support");
#else
  tbb::task_arena arena(1);
  return arena.execute([&] {
    Usage usage{o, p};
    usage.metadata = editsBytes;
    for (const auto &input : o.inputs) {
      for (const auto *selections : {&input.select, &input.remove})
        for (const auto &selection : *selections)
          add(usage.catalogue, 2 * selection.size() + 128, p.maxInputBytes,
              "selector request payload exceeds maxInputBytes");
      for (const auto &edit : input.edits)
        add(usage.catalogue, 2 * edit.selector.size() + 256, p.maxInputBytes,
            "grid edit request payload exceeds maxInputBytes");
    }
    std::vector<uintmax_t> lengths;
    for (const auto &input : o.inputs) {
      if (!std::filesystem::is_regular_file(input.path))
        throw std::invalid_argument(
            "file authoring input must be a regular file");
      const auto bytes = std::filesystem::file_size(input.path);
      add(usage.encoded, bytes, p.maxInputBytes,
          "aggregate encoded inputs exceed maxInputBytes");
      lengths.push_back(bytes);
    }
    usage.check();
    Temporary temporary(parent);
    FileAuthoringResult result;
    result.inputBytes = usage.encoded;
    openvdb::MetaMap fileMetadata;
    openvdb::GridPtrVec grids;
    std::set<std::string> finalNames;
    for (size_t inputIndex = 0; inputIndex < o.inputs.size(); ++inputIndex) {
      const auto &input = o.inputs[inputIndex];
      const auto snapshot = temporary.directory /
                            ("input-" + std::to_string(inputIndex) + ".vdb");
      copyBounded(input.path, snapshot, lengths[inputIndex]);
      detail::RequireSeekableFile(snapshot);
      openvdb::io::File file(snapshot.string());
      // The module's private snapshot is immutable and lives through writing.
      file.setCopyMaxBytes(0);
      file.open(true);
      const auto inputMetadata = file.getMetadata();
      usage.meta(*inputMetadata);
      mergeMetadata(fileMetadata, *inputMetadata, o.metadataConflict);
      std::vector<std::string> descriptors;
      std::set<std::string> unique;
      for (auto it = file.beginName(); it != file.endName(); ++it) {
        if (result.inputGridCount == o.maxGrids)
          throw std::invalid_argument(
              "scanned grid descriptors exceed maxGrids");
        ++result.inputGridCount;
        auto descriptor = it.gridName();
        add(usage.catalogue, descriptor.size(), p.maxInputBytes,
            "descriptor catalogue exceeds maxInputBytes");
        add(usage.catalogue, 256, p.maxInputBytes,
            "descriptor catalogue exceeds maxInputBytes");
        usage.check();
        if (!unique.insert(descriptor).second)
          throw std::invalid_argument("input has colliding human-readable grid "
                                      "descriptors; rename grids first");
        descriptors.push_back(std::move(descriptor));
      }
      std::vector<CatalogEntry> catalogue;
      for (const auto &descriptor : descriptors) {
        auto meta = file.readGridMetadata(descriptor);
        usage.meta(*meta);
        catalogue.push_back({descriptor, std::move(meta)});
      }
      std::set<size_t> selected, removed, edited;
      if (input.select.empty()) {
        for (size_t n = 0; n < catalogue.size(); ++n)
          selected.insert(n);
      } else
        for (const auto &selection : input.select) {
          if (!selected.insert(resolve(selection, catalogue)).second)
            throw std::invalid_argument("overlapping file grid selections");
        }
      for (const auto &selection : input.remove) {
        const auto n = resolve(selection, catalogue);
        if (!selected.contains(n) || !removed.insert(n).second)
          throw std::invalid_argument(
              "removals must name distinct selected grids");
      }
      result.removedGridCount += removed.size();
      std::map<size_t, const FileGridEdit *> gridEdits;
      for (const auto &edit : input.edits) {
        const auto n = resolve(edit.selector, catalogue);
        if (!selected.contains(n) || removed.contains(n) ||
            !edited.insert(n).second)
          throw std::invalid_argument(
              "grid edits must name distinct selected, nonremoved grids");
        gridEdits.emplace(n, &edit);
      }
      for (const auto n : selected) {
        if (removed.contains(n))
          continue;
        const auto &entry = catalogue[n];
        if (!supported(*entry.metadata))
          throw std::invalid_argument(
              "file authoring does not support registered grid type: " +
              entry.metadata->type());
        const auto mem = entry.metadata->getMetadata<openvdb::Int64Metadata>(
            openvdb::GridBase::META_FILE_MEM_BYTES);
        const auto voxels = entry.metadata->getMetadata<openvdb::Int64Metadata>(
            openvdb::GridBase::META_FILE_VOXEL_COUNT);
        if (!mem || !voxels || mem->value() < 0 || voxels->value() < 0)
          throw std::invalid_argument(
              "bounded file authoring requires recorded memory/active-voxel "
              "statistics");
        if (uintmax_t(voxels->value()) > p.maxVoxelCount - usage.active)
          throw std::invalid_argument(
              "recorded aggregate active voxels exceed maxVoxelCount");
        usage.check(uintmax_t(mem->value()));
        auto grid = file.readGrid(entry.descriptor);
        add(usage.active, grid->activeVoxelCount(), p.maxVoxelCount,
            "aggregate active voxels exceed maxVoxelCount");
        add(usage.slots, leafSlots(*grid, p.maxVoxelCount), p.maxVoxelCount,
            "aggregate stored leaf slots exceed maxVoxelCount");
        // memUsage() omits nonresident leaf buffers. Retain their FULL decoded
        // size before any materialization, including across subsequent grids.
        const auto fullTree = loadedTreeBytes(*grid);
        add(usage.resident, fullTree, p.maxInputBytes,
            "fully loaded VDB trees exceed maxInputBytes");
        usage.check();
        if (auto pointGrid =
                openvdb::gridPtrCast<points::PointDataGrid>(grid)) {
          if (pointGrid->tree().hasActiveTiles())
            throw std::invalid_argument(
                "PointData active tiles cannot encode point offsets");
          const auto payload = pointPayload(*pointGrid, usage);
          add(usage.resident, payload, p.maxInputBytes,
              "point arrays exceed maxInputBytes");
          usage.check();
          forcePointPayload(*pointGrid);
        }
        grid->readNonresidentBuffers();
        if (grid->memUsage() > fullTree)
          throw std::runtime_error(
              "loaded tree exceeded its preflight memory estimate");
        const auto edit = gridEdits.find(n);
        if (edit != gridEdits.end()) {
          if (edit->second->newName &&
              *edit->second->newName != grid->getName()) {
            grid->setName(*edit->second->newName);
            ++result.renamedGridCount;
          }
          apply(*grid, edit->second->metadata, result);
        }
        name(grid->getName(),
             "final grid names must contain 1..256 bytes without NUL");
        if (!finalNames.insert(grid->getName()).second)
          throw std::invalid_argument("final grid names must be unique; "
                                      "explicitly rename duplicate names");
        grid->setSaveFloatAsHalf(o.saveFloatAsHalf &&
                                 (grid->isType<openvdb::FloatGrid>() ||
                                  grid->isType<openvdb::Vec3SGrid>()));
        result.gridNames.push_back(grid->getName());
        grids.push_back(std::move(grid));
      }
      file.close();
    }
    apply(fileMetadata, o.metadata, result);
    usage.meta(
        fileMetadata); // Retained merged copy and edited metadata are live.
    usage.check();
    uint32_t compression = openvdb::io::COMPRESS_NONE;
    if (o.compression == FileCompression::Zip)
      compression |= openvdb::io::COMPRESS_ZIP;
    if (o.compression == FileCompression::Blosc)
      compression |= openvdb::io::COMPRESS_BLOSC;
    if (o.activeMaskCompression) {
      compression |= openvdb::io::COMPRESS_ACTIVE_MASK;
      result.diagnostics.emplace_back(
          "active-mask compression requested: inactive floating-point "
          "signed-zero bits may change");
    }
    if (o.saveFloatAsHalf)
      result.diagnostics.emplace_back(
          "half precision requested: Float/Vec3S tree values are quantized; "
          "point attributes retain their native codecs");
    result.outputBytes =
        write(temporary.directory / "authored.vdb", output, grids, fileMetadata,
              compression, p.maxInputBytes);
    result.outputGridCount = grids.size();
    result.activeVoxelCount = usage.active;
    result.pointCount = usage.points;
    return result;
  });
#endif
#else
  (void)output;
  (void)o;
  (void)p;
  unavailable();
#endif
}
} // namespace container::geometry::vdb
