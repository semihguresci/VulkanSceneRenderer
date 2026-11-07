template<class Grid> TopologyResult sdfBackground(
    const Grid &source,const openvdb::MetaMap &fileMeta,const std::filesystem::path &output,
    SdfBackgroundOperation op,const SdfBackgroundOptions &o,Budget &budget,double h,
    const std::filesystem::path &privateOutput) {
  using Value=typename Grid::ValueType;
  if(source.getGridClass()!=openvdb::GRID_LEVEL_SET || !std::isfinite(source.background()) || !(source.background()>0))
    throw std::invalid_argument("SDF background input must be a level set with finite positive background");
  if(h<1e-6 || h>1e6) throw std::invalid_argument("SDF background tools require voxel size [1e-6,1e6]");
  std::array<openvdb::Vec3d,3> axes;
  for(size_t i=0;i<3;++i) {
    openvdb::Vec3d unit(0); unit[i]=1; axes[i]=source.transform().baseMap()->applyJacobian(unit)/h;
    if(std::abs(axes[i].length()-1)>1e-12) throw std::invalid_argument("SDF background tools require uniform voxels");
    for(size_t j=0;j<i;++j) if(std::abs(axes[i].dot(axes[j]))>1e-12)
      throw std::invalid_argument("SDF background tools reject affine shear");
  }
  const double outsideRequest=o.outsideWidth.value_or(double(source.background()));
  const double insideRequest=o.insideWidth.value_or(outsideRequest);
  if(outsideRequest>double(std::numeric_limits<Value>::max()) || insideRequest>double(std::numeric_limits<Value>::max()))
    throw std::invalid_argument("SDF background width exceeds native value range");
  const Value outside=Value(outsideRequest),inside=Value(insideRequest);
  if(!(outside>0) || !(inside>0)) throw std::invalid_argument("SDF background width underflows native representation");
  for(auto it=source.cbeginValueAll();it;++it) {
    budget.items(1);
    const Value value=*it;
    if(!std::isfinite(value)) throw std::invalid_argument("SDF background input contains nonfinite stored values");
    if(it.isValueOn()) {
      if(!it.isVoxelValue()) throw std::invalid_argument("SDF background input must not contain active tiles");
      if(value>outside || value < -inside)
        throw std::invalid_argument("requested SDF backgrounds would truncate unchanged active distances");
    }
  }
  uintmax_t projected=0;
  if(op==SdfBackgroundOperation::FloodFill) {
    uintmax_t count=0;
    std::array<int64_t,3> minimum{},maximum{};
    for(auto it=source.tree().root().cbeginChildOn();it;++it) {
      budget.items(1); const auto c=it.getCoord();
      for(size_t i=0;i<3;++i) {
        if(!count) minimum[i]=maximum[i]=c[i];
        else {minimum[i]=std::min(minimum[i],int64_t(c[i]));maximum[i]=std::max(maximum[i],int64_t(c[i]));}
      }
      ++count;
    }
    for(size_t i=0;i<3;++i) if(maximum[i]-minimum[i]>INT32_MAX)
      throw std::invalid_argument("SDF flood-fill root coordinate span exceeds safe Int32 differences; recenter/partition first");
    // Each adjacent child pair may fill a z scanline. This deliberately bounds
    // all pairs by the full z span, without generating a giant coordinate set.
    const auto candidates=multiply(count,uint64_t((maximum[2]-minimum[2])/4096+1),
        budget.maxWork,"SDF root scanline growth exceeds maxWorkItems");
    budget.items(candidates);
    projected=multiply(candidates,256,budget.p.maxInputBytes,"SDF root scanline allocation exceeds byte budget");
  }
  budget.check(projected);
  budget.items(multiply(budget.work,8,budget.maxWork,"SDF background shared traversal exceeds maxWorkItems"));
  auto result=detail::DeepCopyGridExact(source);
  if(op==SdfBackgroundOperation::FloodFill)
    openvdb::tools::signedFloodFillWithValues(result->tree(),outside,-inside,false);
  openvdb::tools::changeAsymmetricLevelSetBackground(result->tree(),outside,-inside,false);
  if(result->activeVoxelCount()!=source.activeVoxelCount() || result->tree().leafCount()!=source.tree().leafCount() ||
     result->memUsage()>budget.resident+projected)
    throw std::runtime_error("SDF background output exceeded preflight topology limits");
  for(auto it=source.cbeginValueOn();it;++it) {
    budget.items(1);
    if(!result->tree().isValueOn(it.getCoord()) || !exactValue(Value(*it),result->tree().getValue(it.getCoord())))
      throw std::runtime_error("SDF background tool changed an active distance");
  }
  budget.check(projected);
  for(const auto key:{openvdb::GridBase::META_FILE_BBOX_MIN,openvdb::GridBase::META_FILE_BBOX_MAX,
      openvdb::GridBase::META_FILE_MEM_BYTES,openvdb::GridBase::META_FILE_VOXEL_COUNT}) result->removeMeta(key);
  result->setName(budget.p.gridName); result->setSaveFloatAsHalf(false);
  detail::RemoveBoundaryProjectionMetadata(*result);
  result->insertMeta("container:sdf_background_operation",openvdb::StringMetadata(op==SdfBackgroundOperation::FloodFill ? "flood-fill" : "background-only"));
  result->insertMeta("container:sdf_outside_width",openvdb::DoubleMetadata(double(outside)));
  result->insertMeta("container:sdf_inside_width",openvdb::DoubleMetadata(double(inside)));
  TopologyResult report;
  report.grid={result->getName(),"level set",result->activeVoxelCount(),h};
  report.inputActiveVoxelCountA=source.activeVoxelCount(); report.storedLeafCount=result->tree().leafCount(); report.workItems=budget.work;
  std::ostringstream widths; widths << std::setprecision(std::numeric_limits<Value>::max_digits10)
      << "native world-distance backgrounds: outside=" << outside << ", inside=" << -inside;
  report.diagnostics.push_back(widths.str());
  report.diagnostics.emplace_back("active distance/activity bits and affine map retained; background changes do not expand the active distance band");
  if(op==SdfBackgroundOperation::FloodFill)
    report.diagnostics.emplace_back("native flood fill assumes a closed signed field and retains inactive-only branch sign seeds; no closure certification or arbitrary interior/distance repair");
  report.diagnostics.emplace_back("file/grid metadata retained except regenerated statistics/provenance; half saving disabled");
  openvdb::io::File writer(privateOutput.string()); writer.setCompression(openvdb::io::COMPRESS_ZIP);
  writer.write(openvdb::GridPtrVec{result},fileMeta); writer.close();
  publishEdited(privateOutput,output,budget.p.maxInputBytes);
  return report;
}
