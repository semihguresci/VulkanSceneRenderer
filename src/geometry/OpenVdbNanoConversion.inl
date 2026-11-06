// Private implementation included by OpenVdbNanoInterchange.cpp only when both
// portable NanoVDB and OpenVDB are enabled. Native-only builds do not parse any
// OpenVDB declarations or link an OpenVDB runtime.
namespace {
void initializeVdbNano() {
  static std::once_flag flag;
  std::call_once(flag, [] { openvdb::initialize(); });
}

template <class F> decltype(auto) visitVdb(const openvdb::GridBase &g, F &&f) {
  if (g.isType<openvdb::FloatGrid>())
    return f(static_cast<const openvdb::FloatGrid &>(g));
  if (g.isType<openvdb::DoubleGrid>())
    return f(static_cast<const openvdb::DoubleGrid &>(g));
  if (g.isType<openvdb::Int32Grid>())
    return f(static_cast<const openvdb::Int32Grid &>(g));
  if (g.isType<openvdb::Int64Grid>())
    return f(static_cast<const openvdb::Int64Grid &>(g));
  if (g.isType<openvdb::BoolGrid>())
    return f(static_cast<const openvdb::BoolGrid &>(g));
  if (g.isType<openvdb::MaskGrid>())
    return f(static_cast<const openvdb::MaskGrid &>(g));
  if (g.isType<openvdb::Vec3SGrid>())
    return f(static_cast<const openvdb::Vec3SGrid &>(g));
  if (g.isType<openvdb::Vec3DGrid>())
    return f(static_cast<const openvdb::Vec3DGrid &>(g));
  throw std::invalid_argument(
      "NanoVDB exact conversion supports Float/Double/Int32/Int64/Bool/Mask/"
      "Vec3S/Vec3D VDB grids; choose an explicit supported grid");
}

template <class T> void append(std::vector<uint8_t> &b, T v) {
  const auto old = b.size();
  b.resize(old + sizeof(v));
  std::memcpy(b.data() + old, &v, sizeof(v));
}

std::vector<uint8_t> preserveMetadata(const openvdb::MetaMap &grid,
                                      const openvdb::MetaMap &file,
                                      const NanoVdbLimits &l) {
  uint64_t total = 20, count = 0;
  const auto measure = [&](const openvdb::MetaMap &map) {
    for (auto it = map.beginMeta(); it != map.endMeta(); ++it) {
      const auto &name = it->first;
      const auto type = it->second->typeName();
      require(!name.empty() && name.size() <= l.maxNameBytes &&
                  !type.empty() && type.size() <= l.maxNameBytes &&
                  name.find('\0') == std::string::npos &&
                  type.find('\0') == std::string::npos,
              "VDB metadata key/type exceeds the NanoVDB extension limits");
      require(++count <= 4096, "VDB metadata exceeds 4096 entries");
      total = add(total, add(16, add(add(name.size(), type.size()),
                                     it->second->size())));
      require(total <= l.maxMetadataBytes,
              "VDB metadata exceeds maxMetadataBytes");
    }
  };
  measure(grid);
  measure(file);
  std::vector<uint8_t> bytes;
  bytes.reserve(size_t(total));
  append(bytes, ExtensionMagic);
  append(bytes, uint32_t(1));
  append(bytes, uint32_t(0x01020304));
  append(bytes, uint32_t(count));
  const auto serialize = [&](const openvdb::MetaMap &map, bool fileScope) {
    for (auto it = map.beginMeta(); it != map.endMeta(); ++it) {
      const auto type = it->second->typeName();
      std::ostringstream stream(std::ios::binary | std::ios::out);
      stream.exceptions(std::ios::badbit | std::ios::failbit);
      it->second->write(stream);
      const auto data = stream.str();
      require(data.size() == uint64_t(it->second->size()) + 4,
              "VDB metadata serializer disagrees with its recorded size");
      uint32_t recorded;
      std::memcpy(&recorded, data.data(), 4);
      require(recorded == it->second->size(),
              "VDB metadata serializer emitted an inconsistent size");
      append(bytes, uint32_t(fileScope));
      append(bytes, uint32_t(it->first.size()));
      append(bytes, uint32_t(type.size()));
      append(bytes, recorded);
      bytes.insert(bytes.end(), it->first.begin(), it->first.end());
      bytes.insert(bytes.end(), type.begin(), type.end());
      bytes.insert(bytes.end(), data.begin() + 4, data.end());
    }
  };
  serialize(grid, false);
  serialize(file, true);
  require(bytes.size() == total, "VDB metadata extension size changed");
  return bytes;
}

openvdb::Metadata::Ptr restoreMetadata(const RawMetadata &m) {
  openvdb::Metadata::Ptr result;
  // Never invoke arbitrary registered deserializers on native-file bytes.
  // Builtins have fixed payload sizes (except bounded String); every other
  // type is restored losslessly as UnknownMetadata with its original type tag.
  const auto typed = [&]<class T>() {
    if (m.type != T::staticTypeName())
      return;
    result = std::make_shared<T>();
    if constexpr (!std::is_same_v<T, openvdb::StringMetadata>)
      require(m.value.size() == result->size(),
              "NanoVDB builtin metadata payload has the wrong byte size");
  };
  typed.template operator()<openvdb::BoolMetadata>();
  typed.template operator()<openvdb::Int32Metadata>();
  typed.template operator()<openvdb::Int64Metadata>();
  typed.template operator()<openvdb::FloatMetadata>();
  typed.template operator()<openvdb::DoubleMetadata>();
  typed.template operator()<openvdb::StringMetadata>();
  typed.template operator()<openvdb::Vec2IMetadata>();
  typed.template operator()<openvdb::Vec2SMetadata>();
  typed.template operator()<openvdb::Vec2DMetadata>();
  typed.template operator()<openvdb::Vec3IMetadata>();
  typed.template operator()<openvdb::Vec3SMetadata>();
  typed.template operator()<openvdb::Vec3DMetadata>();
  typed.template operator()<openvdb::Vec4IMetadata>();
  typed.template operator()<openvdb::Vec4SMetadata>();
  typed.template operator()<openvdb::Vec4DMetadata>();
  typed.template operator()<openvdb::Mat4SMetadata>();
  typed.template operator()<openvdb::Mat4DMetadata>();
  if (!result) {
    auto opaque = std::make_shared<openvdb::UnknownMetadata>(m.type);
    opaque->setValue(m.value);
    return opaque;
  }
  if (m.type == openvdb::BoolMetadata::staticTypeName())
    require(m.value[0] <= 1, "NanoVDB Bool metadata representation is invalid");
  std::string bytes(4 + m.value.size(), '\0');
  const uint32_t size = uint32_t(m.value.size());
  std::memcpy(bytes.data(), &size, 4);
  if (size)
    std::memcpy(bytes.data() + 4, m.value.data(), size);
  std::istringstream stream(bytes, std::ios::binary | std::ios::in);
  stream.exceptions(std::ios::badbit | std::ios::failbit);
  result->read(stream);
  require(stream.tellg() == std::streampos(bytes.size()),
          "NanoVDB builtin metadata reader did not consume its exact payload");
  return result;
}

void checkVdbTags(const openvdb::GridBase &g) {
  require(g.transform().isLinear(),
          "NanoVDB exact conversion requires an affine VDB transform");
  const auto checkString = [&](const char *key,
                               std::initializer_list<std::string> values) {
    const auto value = g[key];
    if (!value)
      return;
    const auto text =
        std::dynamic_pointer_cast<const openvdb::StringMetadata>(value);
    require(bool(text) && std::find(values.begin(), values.end(), text->value()) !=
                              values.end(),
            "VDB class/vector tags have an unsupported native representation");
  };
  checkString(openvdb::GridBase::META_GRID_CLASS,
              {"unknown", "level set", "fog volume", "staggered"});
  checkString(openvdb::GridBase::META_VECTOR_TYPE,
              {"invariant", "covariant", "covariant normalize",
               "contravariant relative", "contravariant absolute"});
  for (const auto *key : {openvdb::GridBase::META_IS_LOCAL_SPACE,
                          openvdb::GridBase::META_SAVE_HALF_FLOAT})
    require(!g[key] || bool(g.getMetadata<openvdb::BoolMetadata>(key)),
            "VDB local-space/half-storage tags must have Bool metadata type");
  const bool real = g.isType<openvdb::FloatGrid>() ||
                    g.isType<openvdb::DoubleGrid>();
  require(g.getGridClass() != openvdb::GRID_LEVEL_SET || real,
          "NanoVDB level-set conversion requires real scalar values");
  require(g.getName().find('\0') == std::string::npos,
          "VDB grid name contains NUL");
  nv::Map map;
  const auto matrix = g.transform().baseMap()->getAffineMap()->getMat4();
  map.set(matrix, matrix.inverse());
  mapValid(map);
}

template <class Grid>
uint64_t nativeSize(const Grid &g, uint64_t metadata) {
  using T = typename nv::tools::MapToNano<typename Grid::BuildType>::type;
  using RootData = typename nv::NanoRoot<T>::DataType;
  const auto n = g.tree().nodeCount();
  uint64_t bytes = sizeof(nv::GridData) + sizeof(nv::TreeData) + sizeof(RootData);
  bytes = add(bytes, mul(g.tree().root().getTableSize(), sizeof(typename RootData::Tile)));
  bytes = add(bytes, mul(n[2], sizeof(nv::NanoUpper<T>)));
  bytes = add(bytes, mul(n[1], sizeof(nv::NanoLower<T>)));
  bytes = add(bytes, mul(n[0], sizeof(nv::NanoLeaf<T>)));
  bytes = add(bytes, sizeof(nv::GridBlindMetaData));
  bytes = add(bytes, add(metadata, 31) & ~uint64_t(31));
  if (g.getName().size() >= nv::GridData::MaxNameSize) {
    bytes = add(bytes, sizeof(nv::GridBlindMetaData));
    bytes = add(bytes, add(g.getName().size() + 1, 31) & ~uint64_t(31));
  }
  return bytes;
}

template <class T> void restoreInactiveOrigins(nv::GridData &h) {
  auto &root = reinterpret_cast<nv::NanoGrid<T> &>(h).tree().root();
  const auto walk = [&](auto &&self, auto &node, const nv::Coord &expected) -> void {
    using Node = std::remove_reference_t<decltype(node)>;
    using Child = typename Node::ChildNodeType;
    auto *data = node.data();
    if (data->mBBox.empty()) {
      // Preserve an empty active bbox while encoding the ownership origin.
      // +1 is safe even at the highest aligned internal-node origin.
      data->mBBox[0] = expected + nv::Coord(1);
      data->mBBox[1] = expected;
    }
    if constexpr (Child::LEVEL > 0) {
      for (auto it = data->mChildMask.beginOn(); it; ++it) {
        const auto local = Node::OffsetToLocalCoord(*it);
        const nv::Coord childOrigin(
            int32_t(int64_t(expected[0]) + int64_t(local[0]) * Child::DIM),
            int32_t(int64_t(expected[1]) + int64_t(local[1]) * Child::DIM),
            int32_t(int64_t(expected[2]) + int64_t(local[2]) * Child::DIM));
        self(self, *data->getChild(*it), childOrigin);
      }
    }
  };
  auto *data = root.data();
  for (uint32_t i = 0; i < data->mTableSize; ++i) {
    auto *tile = data->tile(i);
    if (tile->isChild())
      walk(walk, *data->getChild(tile), tile->origin());
  }
}

template <class Grid>
nv::HostBuffer convertVdb(const Grid &g, const std::vector<uint8_t> &metadata,
                          const NanoVdbWriteOptions &options,
                          std::vector<std::string> &diagnostics,
                          const NanoVdbLimits &l, uint64_t &amount) {
  finiteValue(g.background());
  for (auto it = g.tree().cbeginValueAll(); it; ++it) {
    work(amount, 1, l);
    finiteValue(*it);
  }
  nv::tools::CreateNanoGrid<Grid> converter(g);
  converter.setStats(nv::tools::StatsMode::BBox);
  converter.setChecksum(nv::CheckMode::Disable);
  const auto index = converter.addBlindData(
      ExtensionName, nv::GridBlindDataSemantic::Unknown,
      nv::GridBlindDataClass::Unknown, nv::GridType::Unknown, metadata.size(), 1);
  auto handle = converter.getHandle();
  using T = typename nv::tools::MapToNano<typename Grid::BuildType>::type;
  restoreInactiveOrigins<T>(*static_cast<nv::GridData *>(handle.buffer().data()));
  auto &header = *handle.gridData();
  const auto *base = reinterpret_cast<const uint8_t *>(&header);
  auto &blind = *reinterpret_cast<nv::GridBlindMetaData *>(
      const_cast<uint8_t *>(base) + header.mBlindMetadataOffset +
      index * sizeof(nv::GridBlindMetaData));
  std::memcpy(reinterpret_cast<uint8_t *>(&blind) + blind.mDataOffset,
              metadata.data(), metadata.size());
  bool rounded = false;
  for (int k = 0; k < 9; ++k)
    rounded |= double(header.mMap.mMatF[k]) != header.mMap.mMatD[k] ||
               double(header.mMap.mInvMatF[k]) != header.mMap.mInvMatD[k];
  for (int k = 0; k < 3; ++k)
    rounded |= double(header.mMap.mVecF[k]) != header.mMap.mVecD[k];
  require(!rounded || options.allowFloatMapRounding,
          "NanoVDB derived Float map cache rounds the exact Double transform; "
          "explicit allowFloatMapRounding is required");
  if (rounded)
    diagnostics.emplace_back("Derived Float map cache rounded with explicit "
                             "permission; Double affine map retained: " +
                             g.getName());
  return std::move(handle.buffer());
}

Entry nativeEntry(const nv::HostBuffer &buffer, const NanoVdbLimits &l) {
  const auto &h = *static_cast<const nv::GridData *>(buffer.data());
  const auto &t = *reinterpret_cast<const nv::TreeData *>(
      reinterpret_cast<const uint8_t *>(&h) + sizeof(h));
  Entry e;
  e.type = h.mGridType;
  e.gridClass = h.mGridClass;
  e.info.name = h.gridName(); // Only freshly constructed converter output.
  e.info.nativeBytes = e.info.encodedBytes = buffer.size();
  e.info.activeVoxels = t.mVoxelCount;
  e.info.nodeCounts = {t.mNodeCount[0], t.mNodeCount[1], t.mNodeCount[2]};
  counts(e.info.nodeCounts, e.info.activeVoxels, l);
  char text[64];
  e.info.valueType = nv::toStr(text, e.type);
  e.info.gridClass = nv::toStr(text, e.gridClass);
  e.info.supported = supported(e.type, e.gridClass);
  return e;
}

template <class T>
openvdb::GridBase::Ptr reverseTypedNative(const nv::GridData &original) {
  auto buffer = nv::HostBuffer::create(original.mGridSize);
  std::memcpy(buffer.data(), &original, size_t(original.mGridSize));
  auto &h = *static_cast<nv::GridData *>(buffer.data());
  // Work on a private copy. Reading/rewriting native files retains every source
  // byte; only OpenVDB reconstruction needs usable inactive node origins.
  restoreInactiveOrigins<T>(h);
  auto grid = nv::tools::nanoToOpenVDB(*reinterpret_cast<const nv::NanoGrid<T> *>(&h));
  // Native Transform factories/copy() simplify maps; the exact public-I/O
  // factory retains even sub-tolerance shear and distant-origin scale bits.
  auto matrix = openvdb::math::Mat4d::identity();
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col)
      matrix[row][col] = h.mMap.mMatD[3 * col + row];
    matrix[3][row] = h.mMap.mVecD[row];
  }
  grid->setTransform(detail::MakeAffineTransformExact(matrix));
  return grid;
}

openvdb::GridBase::Ptr reverseNative(const nv::GridData &h) {
  // Dispatch only checked fixed-size representations. The upstream generic
  // handle dispatcher also instantiates quantized/point/index code we exclude.
  switch (h.mGridType) {
  case nv::GridType::Float:
    return reverseTypedNative<float>(h);
  case nv::GridType::Double:
    return reverseTypedNative<double>(h);
  case nv::GridType::Int32:
    return reverseTypedNative<int32_t>(h);
  case nv::GridType::Int64:
    return reverseTypedNative<int64_t>(h);
  case nv::GridType::Boolean:
    return reverseTypedNative<bool>(h);
  case nv::GridType::Mask:
    return reverseTypedNative<nv::ValueMask>(h);
  case nv::GridType::Vec3f:
    return reverseTypedNative<nv::Vec3f>(h);
  case nv::GridType::Vec3d:
    return reverseTypedNative<nv::Vec3d>(h);
  default:
    throw std::logic_error("unchecked native type reached reverse conversion");
  }
}
} // namespace

NanoVdbFileInfo WriteNanoVdbFromVdb(const std::filesystem::path &input,
                                    const std::filesystem::path &output,
                                    const std::string &selection,
                                    const NanoVdbWriteOptions &options,
                                    const NanoVdbLimits &l) {
  limits(l);
  (void)mode(options.checksum);
  codecAvailable(options.codec);
  require(!options.raw || options.codec == NanoVdbCodec::None,
          "raw NanoVDB buffers cannot use ZIP/BLOSC compression");
  require(!output.empty() && !std::filesystem::exists(output),
          "NanoVDB conversion output must be a new nonempty path");
  initializeVdbNano();
  Snapshot snapshot(input, l);
  detail::RequireSeekableFile(snapshot.temporary.file);
  openvdb::io::File file(snapshot.temporary.file.string());
  file.setCopyMaxBytes(0);
  file.open(true);
  const auto fileMetadata = file.getMetadata();
  std::vector<std::string> descriptors;
  std::set<std::string> unique;
  uint64_t resident = 0, metadataTotal = 0, catalogBytes = 0;
  for (auto it = file.beginName(); it != file.endName(); ++it) {
    require(unique.insert(it.gridName()).second,
            "VDB input contains colliding grid descriptors");
    require(unique.size() <= l.maxGrids,
            "VDB input descriptor catalog exceeds maxGrids");
    catalogBytes = add(catalogBytes, add(mul(it.gridName().size(), 16), 8192));
    workspace(snapshot.size + static_cast<long double>(catalogBytes) +
                  mul(l.maxMetadataBytes, 16) + mul(l.maxNodes, 80) + Fixed, l);
    auto meta = file.readGridMetadata(it.gridName());
    if (!GridSelectionMatches(selection, it.gridName(), meta->getName()))
      continue;
    (void)visitVdb(*meta, [](const auto &) { return true; });
    checkVdbTags(*meta);
    require(meta->getName().size() <= l.maxNameBytes,
            "VDB grid name exceeds maxNameBytes");
    const auto bytes = meta->getMetadata<openvdb::Int64Metadata>(
        openvdb::GridBase::META_FILE_MEM_BYTES);
    const auto active = meta->getMetadata<openvdb::Int64Metadata>(
        openvdb::GridBase::META_FILE_VOXEL_COUNT);
    require(bytes && active && bytes->value() >= 0 && active->value() >= 0 &&
                uint64_t(active->value()) <= l.maxActiveVoxels,
            "VDB conversion requires valid recorded memory/voxel statistics");
    // Check the complete recorded source sum before reading any tree.
    resident = add(resident, uint64_t(bytes->value()));
    workspace(snapshot.size + static_cast<long double>(catalogBytes) +
                  3.0L * resident + l.maxNativeBytes +
                  mul(l.maxMetadataBytes, 16) + mul(l.maxNodes, 80) + Fixed, l);
    descriptors.push_back(it.gridName());
  }
  require(!descriptors.empty(), "no VDB grid matches NanoVDB conversion selector");
  require(selection.empty() || descriptors.size() == 1,
          "VDB conversion selector is ambiguous; use descriptor:<exact name>");
  auto data = std::make_shared<NanoVdbArchiveData>();
  data->info.encodedBytes = snapshot.size;
  data->buffers.reserve(descriptors.size());
  uint64_t actualResident = 0, nodes = 0, slots = 0, active = 0;
  tbb::task_arena serial(1);
  for (const auto &descriptor : descriptors) {
    auto grid = file.readGrid(descriptor);
    checkVdbTags(*grid);
    const auto n = grid->baseTree().nodeCount();
    require(n.size() == 4 && n[3] == 1,
            "VDB conversion requires ordinary 5/4/3 trees");
    for (auto count : n)
      nodes = add(nodes, count);
    slots = add(slots, mul(n[0], 512));
    active = add(active, grid->activeVoxelCount());
    require(nodes <= l.maxNodes && slots <= l.maxStoredLeafVoxels &&
                active <= l.maxActiveVoxels,
            "aggregate VDB source topology exceeds NanoVDB limits");
    const auto memory = visitVdb(*grid, [](const auto &typed) {
      return uint64_t(openvdb::tools::memUsageIfLoaded(typed.tree(), false));
    });
    actualResident = add(actualResident, memory);
    require(actualResident <= resident,
            "decoded VDB tree exceeds its recorded memory preflight");
    auto metadata = preserveMetadata(*grid, *fileMetadata, l);
    metadataTotal = add(metadataTotal, metadata.size());
    require(metadataTotal <= l.maxMetadataBytes,
            "aggregate VDB extensions exceed maxMetadataBytes");
    const auto expected = visitVdb(*grid, [&](const auto &typed) {
      return nativeSize(typed, metadata.size());
    });
    data->info.nativeBytes = add(data->info.nativeBytes, expected);
    require(data->info.nativeBytes <= l.maxNativeBytes,
            "converted native payloads exceed maxNativeBytes");
    work(data->info.work, add(expected / 2, 1), l);
    // Includes the retained native archive, source tree, upstream node manager,
    // converter, one writer copy, codec scratch and bounded metadata parsing.
    workspace(snapshot.size + static_cast<long double>(catalogBytes) +
                  3.0L * resident + 4.0L * data->info.nativeBytes +
                  mul(l.maxMetadataBytes, 16) + mul(l.maxNodes, 80) + Fixed, l);
    grid->readNonresidentBuffers();
    require(grid->memUsage() <= memory,
            "loaded VDB tree exceeds its decoded memory preflight");
    auto buffer = serial.execute([&] {
      return visitVdb(*grid, [&](const auto &typed) {
        return convertVdb(typed, metadata, options, data->info.diagnostics, l,
                           data->info.work);
      });
    });
    require(buffer.size() == expected,
            "NanoVDB converter allocation differs from exact native preflight");
    const auto entry = nativeEntry(buffer, l);
    auto info = entry.info;
    info.index = uint32_t(data->info.grids.size());
    NanoVdbReadOptions validation;
    validateBuffer(buffer, entry, info, validation, l, data->info.work,
                     data->info.diagnostics);
    data->info.grids.push_back(std::move(info));
    data->buffers.push_back(std::move(buffer));
  }
  file.close();
  if (!selection.empty() && unique.size() != descriptors.size())
    data->info.diagnostics.emplace_back("Explicit VDB selection omitted " +
        std::to_string(unique.size() - descriptors.size()) + " other grids.");
  data->info.diagnostics.emplace_back(
      "VDB grid/file metadata retained as versioned opaque native blind data; "
      "no values, inactive states or vector-frame tags were converted.");
  return writeArchive(*data, output, options, l, false, data->info.work);
}

NanoVdbFileInfo WriteVdbFromNanoVdb(const std::filesystem::path &input,
                                    const std::filesystem::path &output,
                                    const NanoVdbReadOptions &options,
                                    bool allowMetadataLoss,
                                    const NanoVdbLimits &l) {
  require(!output.empty() && !std::filesystem::exists(output),
          "VDB conversion output must be a new nonempty path");
  auto archive = ReadNanoVdbFile(input, options, l);
  initializeVdbNano();
  auto report = archive.info();
  // Native fixed layouts are generally larger than VDB fixed layouts. The
  // factor also reserves tree allocations, child staging vectors, ZIP writer
  // scratch and root-map overhead, before constructing any reverse tree.
  workspace(report.encodedBytes + 10.0L * report.nativeBytes +
                mul(l.maxMetadataBytes, 16) + mul(l.maxNodes, 80) + Fixed, l);
  openvdb::GridPtrVec grids;
  openvdb::MetaMap fileMetadata;
  std::vector<uint8_t> originalFileMetadata;
  bool fileMetadataChosen = false;
  tbb::task_arena serial(1);
  for (size_t i = 0; i < archive.data_->buffers.size(); ++i) {
    const auto &buffer = archive.data_->buffers[i];
    const auto &h = *static_cast<const nv::GridData *>(buffer.data());
    work(report.work, add(buffer.size() / 2, 1), l);
    openvdb::MetaMap gridMetadata, thisFileMetadata;
    std::vector<uint8_t> fileIdentity;
    const auto base = reinterpret_cast<const uint8_t *>(&h);
    bool extension = false;
    for (uint32_t k = 0; k < h.mBlindMetadataCount; ++k) {
      const auto &b = *reinterpret_cast<const nv::GridBlindMetaData *>(
          base + h.mBlindMetadataOffset + k * sizeof(nv::GridBlindMetaData));
      if (std::string(b.mName) == ExtensionName) {
        extension = true;
        const auto payload = reinterpret_cast<const uint8_t *>(&b) + b.mDataOffset;
        for (const auto &m : parseMetadata({payload, size_t(b.mValueCount)}, l)) {
          auto value = restoreMetadata(m);
          (m.file ? thisFileMetadata : gridMetadata).insertMeta(m.name, *value);
          if (m.file) {
            append(fileIdentity, uint32_t(m.name.size()));
            append(fileIdentity, uint32_t(m.type.size()));
            append(fileIdentity, uint32_t(m.value.size()));
            fileIdentity.insert(fileIdentity.end(), m.name.begin(), m.name.end());
            fileIdentity.insert(fileIdentity.end(), m.type.begin(), m.type.end());
            fileIdentity.insert(fileIdentity.end(), m.value.begin(), m.value.end());
          }
        }
      } else if (b.mDataClass != nv::GridBlindDataClass::GridName) {
        require(allowMetadataLoss,
                "VDB reverse conversion would discard third-party blind data; "
                "explicit allowMetadataLoss is required");
        report.diagnostics.emplace_back("Omitted third-party blind payload '" +
            std::string(b.mName) + "' from grid: " + report.grids[i].name);
      }
    }
    if (!extension && (h.mGridType == nv::GridType::Vec3f ||
                        h.mGridType == nv::GridType::Vec3d)) {
      require(allowMetadataLoss,
              "native vector grid has no preserved VDB frame metadata; "
              "explicit allowMetadataLoss is required");
      report.diagnostics.emplace_back("Defaulted vector frame to invariant, "
          "local-space because native file has no VDB extension: " + report.grids[i].name);
    }
    if (fileMetadataChosen && fileIdentity != originalFileMetadata) {
      require(allowMetadataLoss,
              "native grids carry conflicting original VDB file metadata; "
              "explicit allowMetadataLoss is required");
      report.diagnostics.emplace_back("Omitted conflicting original file "
          "metadata on grid: " + report.grids[i].name);
    } else if (!fileMetadataChosen) {
      originalFileMetadata = std::move(fileIdentity);
      fileMetadata = thisFileMetadata;
      fileMetadataChosen = true;
    }
    auto grid = serial.execute([&] { return reverseNative(h); });
    if (!extension && (h.mGridType == nv::GridType::Vec3f ||
                        h.mGridType == nv::GridType::Vec3d)) {
      grid->setVectorType(openvdb::VEC_INVARIANT);
      grid->setIsInWorldSpace(false);
    }
    if (extension) {
      grid->clearMetadata();
      grid->insertMeta(gridMetadata);
      require(grid->getName() == report.grids[i].name,
              "preserved VDB name disagrees with native grid header");
      checkVdbTags(*grid);
      // Preserve class identity; stale metadata must not override a modified
      // native class without being noticed.
      const auto actualClass = grid->getGridClass();
      const auto expectedClass = h.mGridClass == nv::GridClass::LevelSet ? openvdb::GRID_LEVEL_SET
          : h.mGridClass == nv::GridClass::FogVolume ? openvdb::GRID_FOG_VOLUME
          : h.mGridClass == nv::GridClass::Staggered ? openvdb::GRID_STAGGERED
                                                  : openvdb::GRID_UNKNOWN;
      require(actualClass == expectedClass,
              "preserved VDB class disagrees with native grid header");
      if (grid->saveFloatAsHalf()) {
        require(allowMetadataLoss,
                "full-precision VDB output must clear preserved half-storage "
                "metadata; explicit allowMetadataLoss is required");
        grid->setSaveFloatAsHalf(false);
        report.diagnostics.emplace_back("Cleared original half-storage flag "
            "to retain exact native values: " + grid->getName());
      }
    }
    // Remove file-loader bookkeeping; the writer recalculates these records.
    for (const auto *key : {openvdb::GridBase::META_FILE_BBOX_MIN,
                            openvdb::GridBase::META_FILE_BBOX_MAX,
                            openvdb::GridBase::META_FILE_COMPRESSION,
                            openvdb::GridBase::META_FILE_MEM_BYTES,
                            openvdb::GridBase::META_FILE_VOXEL_COUNT,
                            openvdb::GridBase::META_FILE_DELAYED_LOAD})
      grid->removeMeta(key);
    grids.push_back(std::move(grid));
  }
  report.diagnostics.emplace_back(
      "Exact native values/states, Double map and retained VDB metadata "
      "written with ZIP and full-precision inactive storage.");
  Temporary temporary{"converted.vdb"};
  {
    openvdb::io::File file(temporary.file.string());
    file.setCompression(openvdb::io::COMPRESS_ZIP);
    file.write(grids, fileMetadata);
    file.close();
  }
  const auto encoded = std::filesystem::file_size(temporary.file);
  require(encoded <= l.maxInputBytes, "encoded VDB output exceeds maxInputBytes");
  // Finish diagnostics/report allocation before exclusive publication.
  report.encodedBytes = encoded;
  copyExclusive(temporary.file, output, encoded);
  return report;
}
