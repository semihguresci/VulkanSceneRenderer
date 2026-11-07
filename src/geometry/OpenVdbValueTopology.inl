// Included in OpenVdbTopology.cpp's private namespace. Shares its input/budget
// preflight; never calls numeric pruning or lossy active-mask serialization.
void validateValueTopology(ValueTopologyOperation op, const ValueTopologyOptions &o,
                           const ProcessingOptions &p) {
  validate(p, o.maxWorkItems);
  if (op < ValueTopologyOperation::ActivateValue || op > ValueTopologyOperation::ReplaceBackground)
    throw std::invalid_argument("invalid value topology operation");
  const bool box = op == ValueTopologyOperation::FillBox || op == ValueTopologyOperation::ClipBox;
  if (!box && (o.minimum != std::array<int32_t,3>{} || o.maximum != std::array<int32_t,3>{}))
    throw std::invalid_argument("index bounds apply only to fill-box/clip-box");
  if (op != ValueTopologyOperation::FillBox && !o.fillActive)
    throw std::invalid_argument("fill activity applies only to fill-box");
  if ((op == ValueTopologyOperation::PruneExact || op == ValueTopologyOperation::ClipBox) &&
      o.value != std::vector<std::string>{"0"})
    throw std::invalid_argument("prune-exact/clip-box do not accept a value");
  if (o.value.empty() || o.value.size() > 3)
    throw std::invalid_argument("native edit requires one scalar or three vector components");
  for (const auto &v : o.value)
    if (v.empty() || v.size() > 128 || v.find('\0') != std::string::npos)
      throw std::invalid_argument("native value component length must be [1,128]");
  if (box) for (size_t axis=0; axis<3; ++axis)
    if (o.minimum[axis] > o.maximum[axis] || o.maximum[axis] > INT32_MAX-4096)
      throw std::invalid_argument("inclusive index box is reversed or reaches the unsafe final Int32 root block");
}
template<class T> bool exactValue(const T &a, const T &b) {
  if constexpr (std::is_floating_point_v<T>)
    return std::bit_cast<std::array<std::byte,sizeof(T)>>(a) ==
           std::bit_cast<std::array<std::byte,sizeof(T)>>(b);
  else if constexpr (std::is_arithmetic_v<T>) return a == b;
  else return exactValue(a[0],b[0]) && exactValue(a[1],b[1]) && exactValue(a[2],b[2]);
}
template<class T> bool finiteValue(const T &v) {
  if constexpr (std::is_floating_point_v<T>) return std::isfinite(v);
  else if constexpr (std::is_arithmetic_v<T>) return true;
  else return finiteValue(v[0]) && finiteValue(v[1]) && finiteValue(v[2]);
}
template<class T> T component(const std::string &s) {
  if constexpr (std::is_same_v<T,bool>) {
    if (s=="true") return true;
    if (s=="false") return false;
    throw std::invalid_argument("Bool edit values require true or false");
  } else {
    T value{};
    const auto result=std::from_chars(s.data(),s.data()+s.size(),value);
    if (result.ec!=std::errc{} || result.ptr!=s.data()+s.size() || !finiteValue(value))
      throw std::invalid_argument("invalid/out-of-range native decimal value: "+s);
    return value;
  }
}
template<class T> T nativeValue(const std::vector<std::string> &v) {
  if constexpr (std::is_arithmetic_v<T>) {
    if (v.size()!=1) throw std::invalid_argument("scalar edit requires exactly one native component");
    return component<T>(v[0]);
  } else {
    if (v.size()!=3) throw std::invalid_argument("vector edit requires exactly three native components");
    using Scalar=typename T::ValueType;
    return T(component<Scalar>(v[0]),component<Scalar>(v[1]),component<Scalar>(v[2]));
  }
}
// No background-tile erasure: upstream equality would erase inactive -0 tiles
// against a +0 background. Bottom-up traversal invalidates no live node cache.
template<class Node> bool constantBits(const Node &node, typename Node::ValueType &value,
                                      bool &active, Budget &budget) {
  if constexpr (Node::LEVEL>0) if (node.cbeginChildOn()) return false;
  auto it=node.cbeginValueAll();
  if (!it) return false;
  value=*it; active=it.isValueOn();
  for (;it;++it) {
    budget.items(1);
    if (it.isValueOn()!=active || !exactValue(typename Node::ValueType(*it),value)) return false;
  }
  return true;
}
template<class Node> void pruneBits(Node &node, Budget &budget) {
  if constexpr (Node::LEVEL>0) {
    for (auto it=node.beginChildOn();it;++it) {
      pruneBits(*it,budget);
      typename Node::ValueType value{}; bool active=false;
      if (constantBits(*it,value,active,budget)) {
        if constexpr (Node::LEVEL==3) node.addTile(it.getCoord(),value,active);
        else node.addTile(it.pos(),value,active);
      }
    }
  }
}
uintmax_t boxVoxels(const openvdb::CoordBBox &box, Budget &budget) {
  uintmax_t cells=1;
  for (size_t n=0;n<3;++n)
    cells=multiply(cells,uint64_t(int64_t(box.max()[n])-box.min()[n]+1),budget.p.maxVoxelCount,
                   "index box volume exceeds maxVoxelCount");
  return cells;
}
template<class Grid> uintmax_t boxStorage(const ValueTopologyOptions &o, Budget &budget) {
  using Tree=typename Grid::TreeType;
  using Leaf=typename Tree::LeafNodeType;
  using Upper=typename Tree::RootNodeType::ChildNodeType;
  using Lower=typename Upper::ChildNodeType;
  uintmax_t cells=1;
  for (size_t n=0;n<3;++n)
    cells=multiply(cells,uint64_t(int64_t(o.maximum[n])-o.minimum[n]+1),budget.p.maxVoxelCount,
                   "index box volume exceeds maxVoxelCount");
  const auto nodes=[&](int64_t side) {
    uintmax_t count=1;
    for (size_t n=0;n<3;++n)
      count=multiply(count,uint64_t(block(o.maximum[n],side)-block(o.minimum[n],side)+1),
                     budget.p.maxVoxelCount,"index box node estimate exceeds voxel budget");
    return count;
  };
  const auto leaves=nodes(8), lowers=nodes(128), uppers=nodes(4096);
  add(budget.slots,multiply(leaves,512,budget.p.maxVoxelCount,"index box leaf slots exceed maxVoxelCount"),
      budget.p.maxVoxelCount,"combined source/box leaf slots exceed maxVoxelCount");
  uintmax_t bytes=sizeof(Tree);
  for (auto pair : {std::pair{leaves,uintmax_t(sizeof(Leaf))},std::pair{lowers,uintmax_t(sizeof(Lower))},
                    std::pair{uppers,uintmax_t(sizeof(Upper)+128)}})
    add(bytes,multiply(pair.first,pair.second,budget.p.maxInputBytes,"index box allocation exceeds byte cap"),
        budget.p.maxInputBytes,"index box allocation exceeds byte cap");
  budget.check(bytes);
  const auto work=multiply(lowers+uppers,36'864,budget.maxWork,"index box sparse work exceeds maxWorkItems");
  budget.items(multiply(work+leaves*512,8,budget.maxWork,"index box sparse work exceeds maxWorkItems"));
  return bytes;
}
void publishEdited(const std::filesystem::path &source, const std::filesystem::path &output, uintmax_t cap) {
  const auto size=std::filesystem::file_size(source);
  if (size>cap) throw std::invalid_argument("encoded edited VDB exceeds maxInputBytes");
  std::ifstream input(source,std::ios::binary);
  std::ofstream destination; bool created=false;
  try {
    if (!input) throw std::runtime_error("cannot read private edited VDB");
    destination.open(output,std::ios::binary|std::ios::out|std::ios::noreplace);
    created=destination.is_open();
    if (!created) throw std::runtime_error("cannot exclusively publish edited VDB");
    destination.exceptions(std::ios::badbit|std::ios::failbit);
    std::array<char,64*1024> buffer{};
    while (input.read(buffer.data(),buffer.size()) || input.gcount()) destination.write(buffer.data(),input.gcount());
    if (!input.eof()) throw std::runtime_error("cannot read complete private edited VDB");
    destination.flush(); destination.close();
  } catch (...) {
    destination.exceptions(std::ios::goodbit);
    if (destination.is_open()) destination.close();
    if (created) { std::error_code e; std::filesystem::remove(output,e); }
    throw;
  }
}
template<class Grid> TopologyResult editValues(
    const Grid &source, const openvdb::MetaMap &fileMetadata, const std::filesystem::path &output,
    ValueTopologyOperation op, const ValueTopologyOptions &o, Budget &budget, double h,
    const std::filesystem::path &privateOutput) {
  using Value=typename Grid::ValueType;
  const bool prune=op==ValueTopologyOperation::PruneExact, clip=op==ValueTopologyOperation::ClipBox;
  if (!prune && source.getGridClass()!=openvdb::GRID_UNKNOWN && source.getGridClass()!=openvdb::GRID_FOG_VOLUME)
    throw std::invalid_argument("value topology edits require ordinary/fog fields; only prune-exact retains SDF/staggered class");
  Value value{};
  if (!prune && !clip) value=nativeValue<Value>(o.value);
  if (!finiteValue(source.background())) throw std::invalid_argument("nonfinite native background");
  const openvdb::CoordBBox bounds(openvdb::Coord(o.minimum[0],o.minimum[1],o.minimum[2]),
                                  openvdb::Coord(o.maximum[0],o.maximum[1],o.maximum[2]));
  uintmax_t prospective=source.activeVoxelCount();
  for (auto it=source.cbeginValueAll();it;++it) {
    budget.items(1);
    if (!finiteValue(Value(*it))) throw std::invalid_argument("nonfinite stored native value");
    if (op==ValueTopologyOperation::ActivateValue && !it.isValueOn() && exactValue(Value(*it),value))
      add(prospective,it.getVoxelCount(),budget.p.maxVoxelCount,"activation of stored tiles exceeds maxVoxelCount");
    if (op==ValueTopologyOperation::FillBox && it.isValueOn()) {
      openvdb::CoordBBox region; it.getBoundingBox(region); region.intersect(bounds);
      if (region) prospective-=boxVoxels(region,budget);
    }
  }
  if (op==ValueTopologyOperation::FillBox && o.fillActive)
    add(prospective,boxVoxels(bounds,budget),budget.p.maxVoxelCount,"filled/source active voxel sum exceeds maxVoxelCount");
  const auto reserve=clip || op==ValueTopologyOperation::FillBox ? boxStorage<Grid>(o,budget) : 0;
  // Covers clone construction, value/cache passes and native writer traversal.
  budget.items(multiply(budget.work,4,budget.maxWork,"value topology shared work exceeds maxWorkItems"));
  budget.check(reserve);
  auto result=detail::DeepCopyGridExact(source);
  if (clip) {
    result->setTree(std::make_shared<typename Grid::TreeType>(source.background()));
    for (auto it=source.cbeginValueAll();it;++it) {
      budget.items(1);
      openvdb::CoordBBox region; it.getBoundingBox(region); region.intersect(bounds);
      if (region) result->tree().sparseFill(region,Value(*it),it.isValueOn());
    }
  } else if (op==ValueTopologyOperation::FillBox) {
    result->tree().sparseFill(openvdb::CoordBBox(
      openvdb::Coord(o.minimum[0],o.minimum[1],o.minimum[2]),
      openvdb::Coord(o.maximum[0],o.maximum[1],o.maximum[2])),value,o.fillActive);
  } else if (prune) {
    result->tree().clearAllAccessors(); pruneBits(result->tree().root(),budget);
  } else if (op==ValueTopologyOperation::ReplaceBackground) {
    const Value old=source.background();
    for (auto it=result->beginValueOff();it;++it) {
      budget.items(1); if (exactValue(Value(*it),old)) it.setValue(value);
    }
    // setBackground has a numeric-equality early return; stage through another
    // finite value to preserve an explicit +0 <-> -0 change at the root.
    const Value alternate=Value(value==Value(0) ? 1 : 0);
    result->tree().root().setBackground(alternate,false);
    result->tree().root().setBackground(value,false);
  } else {
    for (auto it=result->beginValueAll();it;++it) {
      budget.items(1);
      if (exactValue(Value(*it),value)) it.setActiveState(op==ValueTopologyOperation::ActivateValue);
    }
  }
  if (result->activeVoxelCount()>budget.p.maxVoxelCount || result->tree().leafCount()>budget.p.maxVoxelCount/512)
    throw std::invalid_argument("edited output exceeds active/stored voxel cap");
  budget.check(reserve);
  for (const auto key:{openvdb::GridBase::META_FILE_BBOX_MIN,openvdb::GridBase::META_FILE_BBOX_MAX,
       openvdb::GridBase::META_FILE_MEM_BYTES,openvdb::GridBase::META_FILE_VOXEL_COUNT}) result->removeMeta(key);
  result->setName(budget.p.gridName); result->setSaveFloatAsHalf(false);
  detail::RemoveBoundaryProjectionMetadata(*result);
  if (!prune) result->setGridClass(openvdb::GRID_UNKNOWN);
  const char *name=op==ValueTopologyOperation::ActivateValue ? "activate-value" :
      op==ValueTopologyOperation::DeactivateValue ? "deactivate-value" : prune ? "prune-exact" :
      clip ? "clip-box" : op==ValueTopologyOperation::FillBox ? "fill-box" : "replace-background";
  result->insertMeta("container:value_topology_operation",openvdb::StringMetadata(name));
  result->insertMeta("container:value_topology_source",openvdb::StringMetadata(source.getName()));
  TopologyResult report;
  report.grid={result->getName(),prune ? source.gridClassToString(source.getGridClass()) : "unknown",result->activeVoxelCount(),h};
  report.inputActiveVoxelCountA=source.activeVoxelCount();
  report.storedLeafCount=result->tree().leafCount();
  for (auto it=result->cbeginValueOn();it;++it) if (!it.isVoxelValue()) ++report.activeTileCount;
  report.workItems=budget.work;
  report.diagnostics.emplace_back("native value/activity bits and file/grid metadata retained outside the declared edit; half saving and active-mask compression disabled");
  if (!prune) report.diagnostics.emplace_back("output class reset to UNKNOWN; no SDF validity or simulation-state repair is inferred");
  openvdb::io::File writer(privateOutput.string());
  writer.setCompression(openvdb::io::COMPRESS_ZIP);
  writer.write(openvdb::GridPtrVec{result},fileMetadata); writer.close();
  publishEdited(privateOutput,output,budget.p.maxInputBytes);
  return report;
}
