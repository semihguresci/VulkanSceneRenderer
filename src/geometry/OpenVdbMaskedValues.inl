// Included after the checked native value helpers in OpenVdbTopology.cpp.
template<class Grid> TopologyResult maskedValues(
    const Grid &source,const openvdb::GridBase &maskSource,const openvdb::MetaMap &fileMeta,
    const std::filesystem::path &output,MaskedValuesOperation op,Budget &budget,double h,
    const std::filesystem::path &privateOutput) {
  using Tree=typename Grid::TreeType;
  using Value=typename Grid::ValueType;
  using Upper=typename Tree::RootNodeType::ChildNodeType;
  using Lower=typename Upper::ChildNodeType;
  using Leaf=typename Tree::LeafNodeType;
  if(source.getGridClass()!=openvdb::GRID_UNKNOWN && source.getGridClass()!=openvdb::GRID_FOG_VOLUME)
    throw std::invalid_argument("native masked value edits require ordinary/fog source A");
  if(!finiteValue(source.background())) throw std::invalid_argument("nonfinite masked source background");
  for(auto it=source.cbeginValueAll();it;++it) {
    budget.items(1);
    if(!finiteValue(Value(*it))) throw std::invalid_argument("nonfinite masked source stored value");
  }
  // A MaskTree node is much smaller than a Vec3D output node. Charge projected
  // native A node sizes for EVERY mask node before materializing/combining it.
  const auto &base=maskSource.baseTree();
  uintmax_t projected=multiply(base.nonLeafCount(),uintmax_t(sizeof(Upper)+sizeof(Lower)+128),
      budget.p.maxInputBytes,"mask projected native internal nodes exceed byte budget");
  add(projected,multiply(base.leafCount(),sizeof(Leaf),budget.p.maxInputBytes,
      "mask projected native leaves exceed byte budget"),budget.p.maxInputBytes,
      "mask projected native topology exceeds byte budget");
  budget.check(projected);
  budget.items(multiply(budget.work,8,budget.maxWork,"masked value shared traversal exceeds maxWorkItems"));
  // combine2's cross-type leaf copy cannot consume ValueMask directly. Use a
  // native BoolTree topology copy, whose independent Boolean payload is never
  // consulted by the kernel (selection still uses activity only).
  auto topology=mask(maskSource);
  auto selection=openvdb::BoolGrid::create(false);
  selection->setTree(std::make_shared<openvdb::BoolTree>(topology->tree(),false,true,openvdb::TopologyCopy()));
  topology.reset();
  auto result=detail::DeepCopyGridExact(source);
  auto tree=std::make_shared<Tree>(source.background());
  const Value background=source.background();
  const auto kernel=[&](openvdb::CombineArgs<Value,bool> &args) {
    const bool selected=args.bIsActive();
    const bool clip=op==MaskedValuesOperation::ClipInside || op==MaskedValuesOperation::ClipOutside;
    const bool keep=op==MaskedValuesOperation::ClipOutside ? !selected : selected;
    args.setResult(clip && !keep ? background : args.a());
    args.setResultIsActive(op==MaskedValuesOperation::Activate ? args.aIsActive() || selected :
      op==MaskedValuesOperation::Deactivate ? args.aIsActive() && !selected :
      args.aIsActive() && (clip ? keep : selected));
  };
  tree->combine2Extended(source.tree(),selection->tree(),kernel,false);
  result->setTree(tree);
  if(result->activeVoxelCount()>budget.p.maxVoxelCount || result->tree().leafCount()>budget.p.maxVoxelCount/512 ||
     result->memUsage()>budget.resident+projected)
    throw std::invalid_argument("masked native output exceeds preflight topology/byte limits");
  budget.check(projected);
  for(const auto key:{openvdb::GridBase::META_FILE_BBOX_MIN,openvdb::GridBase::META_FILE_BBOX_MAX,
      openvdb::GridBase::META_FILE_MEM_BYTES,openvdb::GridBase::META_FILE_VOXEL_COUNT}) result->removeMeta(key);
  result->setName(budget.p.gridName); result->setGridClass(openvdb::GRID_UNKNOWN); result->setSaveFloatAsHalf(false);
  detail::RemoveBoundaryProjectionMetadata(*result);
  const char *name=op==MaskedValuesOperation::Activate ? "activate" : op==MaskedValuesOperation::Deactivate ? "deactivate" :
      op==MaskedValuesOperation::IntersectActivity ? "intersect-activity" : op==MaskedValuesOperation::ClipInside ? "clip-inside" : "clip-outside";
  result->insertMeta("container:masked_values_operation",openvdb::StringMetadata(name));
  result->insertMeta("container:masked_values_source",openvdb::StringMetadata(source.getName()));
  result->insertMeta("container:masked_values_mask",openvdb::StringMetadata(maskSource.getName()));
  TopologyResult report;
  report.grid={result->getName(),"unknown",result->activeVoxelCount(),h};
  report.inputActiveVoxelCountA=source.activeVoxelCount(); report.inputActiveVoxelCountB=maskSource.activeVoxelCount();
  report.storedLeafCount=result->tree().leafCount(); report.workItems=budget.work;
  for(auto it=result->cbeginValueOn();it;++it) if(!it.isVoxelValue()) ++report.activeTileCount;
  report.diagnostics.emplace_back("mask selection uses active states only; mask stored values, signs and background are ignored");
  report.diagnostics.emplace_back("source A native value/activity bits, affine map, vector frame and file/grid metadata retained outside the edit; source B metadata is not merged");
  report.diagnostics.emplace_back("output class UNKNOWN; no SDF/simulation repair inferred; numeric pruning, half saving and active-mask compression disabled");
  openvdb::io::File writer(privateOutput.string()); writer.setCompression(openvdb::io::COMPRESS_ZIP);
  writer.write(openvdb::GridPtrVec{result},fileMeta); writer.close();
  publishEdited(privateOutput,output,budget.p.maxInputBytes);
  return report;
}
