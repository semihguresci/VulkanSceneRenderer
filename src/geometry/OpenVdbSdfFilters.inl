// Shared checked snapshots, descriptor selection and budgets are private to
// OpenVdbTopology.cpp. Unit/local-index working grids keep tracking conditioned.
const char* sdfFilterName(SdfFilterOperation op) {
  switch(op) {
    case SdfFilterOperation::Gaussian:return "gaussian";
    case SdfFilterOperation::Mean:return "mean";
    case SdfFilterOperation::Median:return "median";
    case SdfFilterOperation::MeanCurvature:return "mean-curvature";
    case SdfFilterOperation::Laplacian:return "laplacian";
    case SdfFilterOperation::Fillet:return "fillet";
    case SdfFilterOperation::Offset:return "offset";
    case SdfFilterOperation::Normalize:return "normalize";
    case SdfFilterOperation::Track:return "track";
    case SdfFilterOperation::Prune:return "prune";
    case SdfFilterOperation::ResizeBand:return "resize-band";
    case SdfFilterOperation::DilateBand:return "dilate-band";
    case SdfFilterOperation::ErodeBand:return "erode-band";
  }
  throw std::invalid_argument("invalid SDF filter operation");
}
void validateSdfFilter(SdfFilterOperation op,const SdfFilterOptions& o,const ProcessingOptions& p) {
  validate(p,o.maxWorkItems);(void)sdfFilterName(op);
  if(o.width<1 || o.width>4 || !o.iterations || o.iterations>32 || o.normalizationCount>64 ||
      !o.bandWidth || o.bandWidth>64 || !o.bandLayers || o.bandLayers>64 ||
      o.spatial<SdfSpatialScheme::FirstOrder || o.spatial>SdfSpatialScheme::HjWeno5 ||
      o.temporal<SdfTemporalScheme::Rk1 || o.temporal>SdfTemporalScheme::Rk3 ||
      o.trim<SdfTrimMode::None || o.trim>SdfTrimMode::All)
    throw std::invalid_argument("invalid SDF filter width/iteration/band/tracker options");
  const bool kernel=op==SdfFilterOperation::Gaussian || op==SdfFilterOperation::Mean || op==SdfFilterOperation::Median;
  if(!kernel && o.width!=1) throw std::invalid_argument("kernel width applies only to mean/gaussian/median SDF filters");
  if(!std::isfinite(o.worldDistance) || (op!=SdfFilterOperation::Offset && o.worldDistance!=0))
    throw std::invalid_argument("world distance applies only to SDF offset and must be finite");
  if(op!=SdfFilterOperation::ResizeBand && o.bandWidth!=3)
    throw std::invalid_argument("band width applies only to resize-band");
  if(op!=SdfFilterOperation::DilateBand && op!=SdfFilterOperation::ErodeBand && o.bandLayers!=1)
    throw std::invalid_argument("band layers apply only to dilation/erosion");
  if(!o.maskInput.empty() && op>SdfFilterOperation::Offset)
    throw std::invalid_argument("alpha masks apply to SDF filtering, not tracker-only operations");
  if(!o.normalizationMaskInput.empty() && op!=SdfFilterOperation::Normalize)
    throw std::invalid_argument("normalization activity masks apply only to Normalize");
  if(!o.normalizationMaskInput.empty() && !o.maskInput.empty())
    throw std::invalid_argument("SDF alpha and normalization activity masks are separate modes");
  if(o.normalizationMaskInput.empty() && !o.normalizationMaskGrid.empty())
    throw std::invalid_argument("normalization mask selector requires its input path");
  if(o.maskInput.empty() && (!o.maskGrid.empty() || o.alphaMinimum!=0 || o.alphaMaximum!=1 || o.invertAlpha))
    throw std::invalid_argument("SDF alpha options require a mask input");
  if(!std::isfinite(o.alphaMinimum) || !std::isfinite(o.alphaMaximum) || !(o.alphaMinimum<o.alphaMaximum))
    throw std::invalid_argument("SDF alpha range requires finite minimum < maximum");
}
openvdb::math::BiasedGradientScheme sdfSpatial(SdfSpatialScheme s) {
  switch(s) {
    case SdfSpatialScheme::FirstOrder:return openvdb::math::FIRST_BIAS;
    case SdfSpatialScheme::SecondOrder:return openvdb::math::SECOND_BIAS;
    case SdfSpatialScheme::ThirdOrder:return openvdb::math::THIRD_BIAS;
    case SdfSpatialScheme::Weno5:return openvdb::math::WENO5_BIAS;
    case SdfSpatialScheme::HjWeno5:return openvdb::math::HJWENO5_BIAS;
  }
  throw std::invalid_argument("invalid SDF spatial scheme");
}
openvdb::math::TemporalIntegrationScheme sdfTemporal(SdfTemporalScheme s) {
  switch(s) {
    case SdfTemporalScheme::Rk1:return openvdb::math::TVD_RK1;
    case SdfTemporalScheme::Rk2:return openvdb::math::TVD_RK2;
    case SdfTemporalScheme::Rk3:return openvdb::math::TVD_RK3;
  }
  throw std::invalid_argument("invalid SDF temporal scheme");
}
openvdb::Coord shiftedSdfCoord(const openvdb::Coord& c,const openvdb::Coord& offset,bool restore) {
  openvdb::Coord result;
  for(size_t a=0;a<3;++a) {
    const int64_t value=int64_t(c[a])+(restore?int64_t(offset[a]):-int64_t(offset[a]));
    if(value<INT32_MIN || value>INT32_MAX) throw std::invalid_argument("SDF index translation exceeds Int32");
    result[a]=int32_t(value);
  }
  return result;
}
openvdb::CoordBBox shiftedSdfBox(const openvdb::CoordBBox& box,const openvdb::Coord& offset,bool restore) {
  return {shiftedSdfCoord(box.min(),offset,restore),shiftedSdfCoord(box.max(),offset,restore)};
}
template<class Grid> uintmax_t sdfTreeStorage(const openvdb::CoordBBox& box,Budget& budget,uintmax_t& leafCount) {
  using Tree=typename Grid::TreeType;
  using Leaf=typename Tree::LeafNodeType;
  using Upper=typename Tree::RootNodeType::ChildNodeType;
  using Lower=typename Upper::ChildNodeType;
  const auto nodes=[&](int64_t side) {
    uintmax_t n=1;
    for(size_t a=0;a<3;++a) n=multiply(n,uint64_t(block(box.max()[a],side)-block(box.min()[a],side)+1),
        budget.p.maxVoxelCount,"SDF full-region node estimate exceeds voxel limit");
    return n;
  };
  leafCount=nodes(8);uintmax_t bytes=sizeof(Tree);
  for(const auto [count,size]:{std::pair{leafCount,uintmax_t(sizeof(Leaf))},
      std::pair{nodes(128),uintmax_t(sizeof(Lower))},std::pair{nodes(4096),uintmax_t(sizeof(Upper)+128)}})
    add(bytes,multiply(count,size,budget.p.maxInputBytes,"SDF prospective tree exceeds byte limit"),
        budget.p.maxInputBytes,"SDF prospective tree exceeds byte limit");
  return bytes;
}
struct SdfFilterDomain {
  openvdb::Coord offset;
  openvdb::CoordBBox localBounds;
  uintmax_t projectedBytes{},leaves{};
};
double sdfBandTolerance(double half) {
  // The SDK's public sphere/mesh generators take Float voxel/band inputs even
  // when producing a DoubleGrid. Admit their representable integer bands.
  return 4*std::numeric_limits<float>::epsilon()*std::max(1.,half);
}
template<class Grid> SdfFilterDomain sdfFilterDomain(
    const Grid& source,SdfFilterOperation op,const SdfFilterOptions& o,Budget& budget,double h,bool forceNoOp=false) {
  if(source.getGridClass()!=openvdb::GRID_LEVEL_SET || !std::isfinite(source.background()) || !(source.background()>0) ||
      h<1e-6 || h>1e6)
    throw std::invalid_argument("SDF filtering requires a positive-background native level set and voxel size [1e-6,1e6]");
  std::array<openvdb::Vec3d,3> axes;
  for(size_t a=0;a<3;++a) {
    openvdb::Vec3d e(0);e[a]=1;axes[a]=source.transform().baseMap()->applyJacobian(e)/h;
    if(std::abs(axes[a].length()-1)>1e-12) throw std::invalid_argument("SDF filters require uniform voxels");
    for(size_t b=0;b<a;++b) if(std::abs(axes[a].dot(axes[b]))>1e-12)
      throw std::invalid_argument("SDF filters require orthogonal voxels");
  }
  const double half=double(source.background())/h;
  if(!std::isfinite(half) || half<1-sdfBandTolerance(half) || half>64+sdfBandTolerance(half))
    throw std::invalid_argument("SDF filter band half width must be in [1,64] source voxels");
  if(source.tree().hasActiveTiles()) throw std::invalid_argument("SDF filters require active voxels without active tiles");
  openvdb::CoordBBox bounds;
  for(auto leaf=source.tree().cbeginLeaf();leaf;++leaf) bounds.expand(leaf->getNodeBoundingBox());
  for(auto value=source.cbeginValueAll();value;++value) {
    budget.items(1);
    if(!std::isfinite(*value) || std::abs(double(*value)/h)>1024)
      throw std::invalid_argument("SDF filter distances must be finite and within 1024 source voxels");
    if(value.isValueOn() || *value!=source.background()) bounds.expand(value.getBoundingBox());
  }
  const double travel=std::abs(o.worldDistance)/h;
  if(!std::isfinite(travel) || travel>256 || (travel && travel<.001))
    throw std::invalid_argument("nonzero SDF offsets must span [0.001,256] source voxels per iteration");
  if(op==SdfFilterOperation::ResizeBand || op==SdfFilterOperation::DilateBand || op==SdfFilterOperation::ErodeBand) {
    const auto tolerance=sdfBandTolerance(half);
    if(std::abs(half-std::round(half))>tolerance)
      throw std::invalid_argument("band resizing requires an integral source half width; normalize the inactive background first");
    const double finalWidth=op==SdfFilterOperation::ResizeBand?o.bandWidth:
      std::round(half)+(op==SdfFilterOperation::DilateBand?1:-1)*double(o.bandLayers)*o.iterations;
    if(finalWidth<1 || finalWidth>64) throw std::invalid_argument("resulting SDF band half width must be in [1,64]");
  }
  if(!source.activeVoxelCount())return {};
  if((op==SdfFilterOperation::Offset && !o.worldDistance) ||
      (op==SdfFilterOperation::Normalize && !o.normalizationCount) || forceNoOp) {
    for(size_t a=0;a<3;++a)if(int64_t(bounds.min()[a])<INT32_MIN+4096LL || int64_t(bounds.max()[a])>INT32_MAX-4096LL)
      throw std::invalid_argument("SDF source leaves insufficient coordinate/quality-stencil margin");
    return {};
  }
  uint64_t passes=op==SdfFilterOperation::Gaussian?4:1;
  if(op==SdfFilterOperation::Offset) passes=uint64_t(std::ceil(travel/.5));
  if(op==SdfFilterOperation::DilateBand) passes=o.bandLayers;
  if(op==SdfFilterOperation::ResizeBand) passes=uint64_t(std::max(0.,double(o.bandWidth)-std::floor(half)));
  if(op==SdfFilterOperation::Normalize || op==SdfFilterOperation::Prune || op==SdfFilterOperation::ErodeBand) passes=0;
  const int64_t halo=int64_t(passes*o.iterations),stencil=std::max(3u,o.width);
  SdfFilterDomain result;
  openvdb::Coord low,high;
  uintmax_t cells=1;
  for(size_t a=0;a<3;++a) {
    const int64_t minimum=int64_t(bounds.min()[a])-halo,maximum=int64_t(bounds.max()[a])+halo;
    if(minimum-stencil<INT32_MIN+4096LL || maximum+stencil>INT32_MAX-4096LL)
      throw std::invalid_argument("SDF filter domain leaves insufficient coordinate/stencil/writer margin");
    low[a]=int32_t(minimum);high[a]=int32_t(maximum);
    const int64_t center=minimum+(maximum-minimum)/2;
    result.offset[a]=int32_t(block(center,8)*8);
    cells=multiply(cells,uint64_t(maximum-minimum+1),budget.p.maxVoxelCount,
        "SDF filter full-region expansion exceeds voxel limit");
  }
  const openvdb::CoordBBox worldIndexBounds(low,high);
  result.localBounds=shiftedSdfBox(worldIndexBounds,result.offset,false);
  uintmax_t localLeaves=0;
  result.projectedBytes=sdfTreeStorage<Grid>(worldIndexBounds,budget,result.leaves);
  add(result.projectedBytes,sdfTreeStorage<Grid>(result.localBounds,budget,localLeaves),budget.p.maxInputBytes,
      "SDF translated/output tree reserve exceeds byte limit");
  result.leaves=std::max(result.leaves,localLeaves);
  const auto slots=multiply(result.leaves,512,budget.p.maxVoxelCount,"SDF projected leaf slots exceed voxel limit");
  add(budget.slots,slots,budget.p.maxVoxelCount,"shared source/mask/output slots exceed voxel limit");
  add(result.projectedBytes,multiply(slots,6*sizeof(typename Grid::ValueType),budget.p.maxInputBytes,
      "SDF auxiliary buffers exceed byte limit"),budget.p.maxInputBytes,"SDF workspace exceeds byte limit");
  uint64_t samples=144;
  if(op==SdfFilterOperation::Median) samples+=uint64_t(2*o.width+1)*(2*o.width+1)*(2*o.width+1)*16;
  uintmax_t work=slots;
  for(const auto factor:{std::max<uint64_t>(1,passes),uint64_t(o.iterations),
      uint64_t(std::max(1u,o.normalizationCount)),uint64_t(o.temporal)+1,samples})
    work=multiply(work,factor,budget.maxWork,"SDF filter bounded sample work exceeds maxWorkItems");
  budget.items(work);budget.check(result.projectedBytes);
  return result;
}

template<class Value> Value sdfNative(double value,const char* reason) {
  if(!std::isfinite(value) || std::abs(value)>std::numeric_limits<Value>::max())
    throw std::invalid_argument(reason);
  const Value native=Value(value);
  if(value!=0 && native==0) throw std::invalid_argument(reason);
  return native;
}
template<class Grid,class Convert> typename Grid::Ptr sdfTranslated(
    const Grid& source,const openvdb::Coord& offset,bool restore,
    typename Grid::ValueType background,Convert convert,const openvdb::CoordBBox* crop=nullptr) {
  using Value=typename Grid::ValueType;
  auto result=Grid::create(background);
  result->setGridClass(source.getGridClass());
  result->setTransform(openvdb::math::Transform::createLinearTransform(1.));
  openvdb::CoordBBox sourceCrop;
  if(crop)sourceCrop=shiftedSdfBox(*crop,offset,!restore);
  for(auto value=source.cbeginValueAll();value;++value) {
    if(value.isVoxelValue() || (!value.isValueOn() && *value==source.background())) continue;
    auto sourceBox=value.getBoundingBox();
    if(crop)sourceBox.intersect(sourceCrop);
    if(!sourceBox)continue;
    auto box=shiftedSdfBox(sourceBox,offset,restore);
    if(box)result->tree().sparseFill(box,convert(Value(*value)),value.isValueOn());
  }
  auto writer=result->getAccessor();
  for(auto value=source.cbeginValueAll();value;++value) {
    if(!value.isVoxelValue() || (!value.isValueOn() && *value==source.background())) continue;
    if(crop && !sourceCrop.isInside(value.getCoord()))continue;
    const auto coord=shiftedSdfCoord(value.getCoord(),offset,restore);
    if(crop && !crop->isInside(coord))continue;
    const Value native=convert(Value(*value));
    if(value.isValueOn())writer.setValue(coord,native);else writer.setValueOff(coord,native);
  }
  return result;
}
template<class Grid> std::pair<double,uint64_t> sdfGradientQuality(const Grid& grid,double h=1) {
  const auto reader=grid.getConstAccessor();double mean=0;uint64_t count=0;
  for(auto value=grid.cbeginValueOn();value;++value) {
    if(std::abs(double(*value)/h)>1)continue;
    openvdb::Vec3d gradient(0);bool interior=true;
    for(size_t a=0;a<3;++a) {
      auto low=value.getCoord(),high=low;--low[a];++high[a];
      if(!reader.isValueOn(low) || !reader.isValueOn(high)){interior=false;break;}
      gradient[a]=((double(reader.getValue(high))-double(reader.getValue(low)))/h)*.5;
    }
    if(interior) {
      const double error=std::abs(std::hypot(gradient[0],gradient[1],gradient[2])-1);
      if(!std::isfinite(error))throw std::invalid_argument("SDF gradient quality exceeds finite Double range");
      ++count;mean+=(error-mean)/count;
    }
  }
  return {mean,count};
}
template<class Grid,class MaskGrid> void sdfExecuteFilter(
    Grid& grid,const MaskGrid* mask,SdfFilterOperation op,const SdfFilterOptions& o,double h,
    const openvdb::MaskTree* normalizationMask=nullptr) {
  using Value=typename Grid::ValueType;
  using Alpha=typename MaskGrid::ValueType;
  openvdb::tools::LevelSetFilter<Grid,MaskGrid> filter(grid);
  filter.setGrainSize(0);filter.setSpatialScheme(sdfSpatial(o.spatial));
  filter.setTemporalScheme(sdfTemporal(o.temporal));filter.setNormCount(int(o.normalizationCount));
  using Trim=openvdb::tools::lstrack::TrimMode;
  switch(o.trim) {
    case SdfTrimMode::None:filter.setTrimming(Trim::kNone);break;
    case SdfTrimMode::Interior:filter.setTrimming(Trim::kInterior);break;
    case SdfTrimMode::Exterior:filter.setTrimming(Trim::kExterior);break;
    case SdfTrimMode::All:filter.setTrimming(Trim::kAll);break;
  }
  if(mask) {
    const auto low=sdfNative<Alpha>(o.alphaMinimum,"SDF alpha endpoint exceeds native mask range");
    const auto high=sdfNative<Alpha>(o.alphaMaximum,"SDF alpha endpoint exceeds native mask range");
    const Alpha delta=high-low;
    if(!(delta>0) || !std::isfinite(delta) || !std::isfinite(Alpha(1)/delta))
      throw std::invalid_argument("SDF alpha native interval/reciprocal must be finite and positive");
    filter.setMaskRange(low,high);filter.invertMask(o.invertAlpha);
  }
  const Value offset=sdfNative<Value>(-o.worldDistance/h,"SDF offset underflows native source-voxel units");
  for(uint32_t iteration=0;iteration<o.iterations && !grid.empty();++iteration) {
    switch(op) {
      case SdfFilterOperation::Gaussian:filter.gaussian(int(o.width),mask);break;
      case SdfFilterOperation::Mean:filter.mean(int(o.width),mask);break;
      case SdfFilterOperation::Median:filter.median(int(o.width),mask);break;
      case SdfFilterOperation::MeanCurvature:filter.meanCurvature(mask);break;
      case SdfFilterOperation::Laplacian:filter.laplacian(mask);break;
      case SdfFilterOperation::Fillet:filter.fillet(mask);break;
      case SdfFilterOperation::Offset:if(offset!=0)filter.offset(offset,mask);break;
      case SdfFilterOperation::Normalize:
        if(normalizationMask)filter.normalize(normalizationMask);else filter.normalize();
        break;
      case SdfFilterOperation::Track:filter.track();break;
      case SdfFilterOperation::Prune:filter.prune();break;
      case SdfFilterOperation::ResizeBand:filter.resize(o.bandWidth);break;
      case SdfFilterOperation::DilateBand:filter.dilate(int(o.bandLayers));break;
      case SdfFilterOperation::ErodeBand:filter.erode(int(o.bandLayers));break;
    }
  }
}

template<class Mask> void sdfValidateAlpha(const Mask& source,const SdfFilterOptions& o,Budget& budget) {
  using Value=typename Mask::ValueType;
  if(!std::isfinite(source.background()))throw std::invalid_argument("nonfinite SDF alpha background");
  for(auto value=source.cbeginValueAll();value;++value) {
    budget.items(1);
    if(!std::isfinite(*value))throw std::invalid_argument("nonfinite stored SDF alpha value");
  }
  const Value low=sdfNative<Value>(o.alphaMinimum,"SDF alpha endpoint exceeds native mask range");
  const Value high=sdfNative<Value>(o.alphaMaximum,"SDF alpha endpoint exceeds native mask range");
  const Value delta=high-low;
  if(!(delta>0) || !std::isfinite(delta) || !std::isfinite(Value(1)/delta))
    throw std::invalid_argument("SDF alpha native interval/reciprocal must be finite and positive");
}
template<class Mask> typename Mask::Ptr sdfWorkingMask(
    const Mask& source,const SdfFilterDomain& domain,Budget&) {
  using Value=typename Mask::ValueType;
  return sdfTranslated(source,domain.offset,false,source.background(),
      [](Value value){return value;},&domain.localBounds);
}
template<class Grid> uint64_t sdfNormalizationSelection(
    const Grid& source,const openvdb::GridBase& mask,const openvdb::Coord& offset,
    openvdb::MaskTree* destination,Budget& budget) {
  budget.items(source.activeVoxelCount());uint64_t selected=0;
  visit(mask,[&](const auto& nativeMask) {
    const auto reader=nativeMask.getConstAccessor();
    for(auto value=source.cbeginValueOn();value;++value) {
      if(!reader.isValueOn(value.getCoord()))continue;
      ++selected;
      if(destination)destination->setValueOn(shiftedSdfCoord(value.getCoord(),offset,false));
    }
    return true;
  });
  return selected;
}
template<class Grid> SdfFilterResult sdfFiltered(
    const Grid& source,const openvdb::GridBase* maskSource,const openvdb::MetaMap& fileMetadata,
    const std::filesystem::path& output,SdfFilterOperation op,const SdfFilterOptions& o,
    Budget& budget,double h,const std::filesystem::path& privateOutput) {
  using Value=typename Grid::ValueType;
  const bool maskedNormalization=!o.normalizationMaskInput.empty();
  std::optional<uint64_t> normalizationSelected;
  if(maskedNormalization)normalizationSelected=sdfNormalizationSelection(source,*maskSource,{},nullptr,budget);
  const auto domain=sdfFilterDomain(source,op,o,budget,h,normalizationSelected && !*normalizationSelected);
  auto result=detail::CopyGridWithNewTreeExact(source);
  double quality=0;uint64_t qualityCount=0;
  const bool noOp=(op==SdfFilterOperation::Offset && !o.worldDistance) ||
      (op==SdfFilterOperation::Normalize && !o.normalizationCount) || (normalizationSelected && !*normalizationSelected);
  if(source.activeVoxelCount() && !noOp) {
    const bool preserveValues=op==SdfFilterOperation::Prune;
    const double half=double(source.background())/h;
    const double nearest=std::round(half);
    const Value unitBackground=preserveValues?source.background():sdfNative<Value>(std::abs(half-nearest)<=
        sdfBandTolerance(half)?nearest:half,
        "SDF unit background is unrepresentable");
    auto working=sdfTranslated(source,domain.offset,false,unitBackground,[&](Value value) {
      if(preserveValues)return value;
      if(value==source.background())return unitBackground;
      if(value==-source.background())return -unitBackground;
      const double conditioned=double(value)/h;
      if(value!=0 && conditioned==0)
        throw std::invalid_argument("SDF source distance underflows unit conditioning");
      return sdfNative<Value>(conditioned,"SDF source distance underflows unit conditioning");
    });
    if(working->activeVoxelCount()!=source.activeVoxelCount() || working->tree().leafCount()>domain.leaves)
      throw std::runtime_error("SDF translated source exceeded topology preflight");
    std::shared_ptr<openvdb::MaskTree> selection;
    if(maskedNormalization) {
      selection=std::make_shared<openvdb::MaskTree>(false);
      const auto count=sdfNormalizationSelection(source,*maskSource,domain.offset,selection.get(),budget);
      if(count!=*normalizationSelected || selection->memUsage()>domain.projectedBytes)
        throw std::runtime_error("normalization mask projection exceeded preflight selection/storage");
      sdfExecuteFilter(*working,static_cast<const openvdb::FloatGrid*>(nullptr),op,o,h,selection.get());
    } else if(maskSource) {
      if(maskSource->isType<openvdb::FloatGrid>()) {
        const auto mask=sdfWorkingMask(static_cast<const openvdb::FloatGrid&>(*maskSource),domain,budget);
        sdfExecuteFilter(*working,mask.get(),op,o,h);
      } else if(maskSource->isType<openvdb::DoubleGrid>()) {
        const auto mask=sdfWorkingMask(static_cast<const openvdb::DoubleGrid&>(*maskSource),domain,budget);
        sdfExecuteFilter(*working,mask.get(),op,o,h);
      } else throw std::invalid_argument("SDF alpha masks require Float/Double scalar grids");
    } else sdfExecuteFilter(*working,static_cast<const openvdb::FloatGrid*>(nullptr),op,o,h);
    if(working->activeVoxelCount()>domain.leaves*512 || working->tree().leafCount()>domain.leaves ||
        working->memUsage()>domain.projectedBytes || working->tree().hasActiveTiles())
      throw std::runtime_error("SDF filtering exceeded preflight topology/native representation");
    for(auto value=working->cbeginValueAll();value;++value) {
      if(!std::isfinite(*value))throw std::invalid_argument("SDF filter produced nonfinite distances");
      if(value.isValueOn() || *value!=working->background()) {
        const auto box=value.getBoundingBox();
        if(!domain.localBounds.isInside(box.min()) || !domain.localBounds.isInside(box.max()))
          throw std::runtime_error("SDF filter grew beyond its checked local domain");
      }
    }
    const Value worldBackground=op==SdfFilterOperation::ResizeBand?
        sdfNative<Value>(double(o.bandWidth)*h,"SDF target band width is unrepresentable"):
        working->background()==unitBackground?source.background():
        sdfNative<Value>(double(source.background())+(double(working->background())-double(unitBackground))*h,
                         "SDF output background is unrepresentable");
    if(!(worldBackground>0))throw std::invalid_argument("SDF filter produced a nonpositive band width");
    const auto toWorld=[&](Value value) {
      if(preserveValues)return value;
      if(value==working->background())return worldBackground;
      if(value==-working->background())return -worldBackground;
      const double restored=double(value)*h;
      if(value!=0 && restored==0)
        throw std::invalid_argument("SDF result distance underflows world units");
      return sdfNative<Value>(restored,"SDF result distance underflows world units");
    };
    if(maskedNormalization) {
      if(working->activeVoxelCount()!=source.activeVoxelCount() || working->background()!=unitBackground)
        throw std::runtime_error("masked normalization changed source topology/background");
      result->setTree(std::make_shared<typename Grid::TreeType>(source.tree()));
      for(auto value=selection->cbeginValueOn();value;++value) {
        const auto original=shiftedSdfCoord(value.getCoord(),domain.offset,true);
        result->tree().setValue(original,toWorld(working->tree().getValue(value.getCoord())));
      }
    } else {
      auto restored=sdfTranslated(*working,domain.offset,true,worldBackground,toWorld);
      result->setTree(restored->treePtr());
    }
    if(result->memUsage()>domain.projectedBytes)throw std::runtime_error("restored SDF exceeded preflight memory");
  } else {
    result->setTree(std::make_shared<typename Grid::TreeType>(source.tree()));
    if(!source.activeVoxelCount() && (op==SdfFilterOperation::ResizeBand ||
        op==SdfFilterOperation::DilateBand || op==SdfFilterOperation::ErodeBand)) {
      const double width=op==SdfFilterOperation::ResizeBand?double(o.bandWidth)*h:
          double(source.background())+(op==SdfFilterOperation::DilateBand?1:-1)*double(o.bandLayers)*o.iterations*h;
      openvdb::tools::changeLevelSetBackground(result->tree(),sdfNative<Value>(width,"empty SDF band width is unrepresentable"),false);
    }
  }
  if(noOp || op==SdfFilterOperation::Prune)
    budget.items(multiply(result->activeVoxelCount(),8,budget.maxWork,"SDF quality sample work exceeds maxWorkItems"));
  std::tie(quality,qualityCount)=sdfGradientQuality(*result,h);
  budget.check(domain.projectedBytes);
  for(const auto key:{openvdb::GridBase::META_FILE_BBOX_MIN,openvdb::GridBase::META_FILE_BBOX_MAX,
      openvdb::GridBase::META_FILE_MEM_BYTES,openvdb::GridBase::META_FILE_VOXEL_COUNT})result->removeMeta(key);
  result->setName(budget.p.gridName);result->setSaveFloatAsHalf(false);
  detail::RemoveBoundaryProjectionMetadata(*result);
  result->insertMeta("container:sdf_filter_operation",openvdb::StringMetadata(sdfFilterName(op)));
  SdfFilterResult report;
  report.grid={result->getName(),"level set",result->activeVoxelCount(),h};
  report.inputActiveVoxelCountA=source.activeVoxelCount();
  report.inputActiveVoxelCountB=maskSource?maskSource->activeVoxelCount():0;
  report.storedLeafCount=result->tree().leafCount();report.workItems=budget.work;
  report.halfWidthVoxels=double(result->background())/h;
  report.gradientSampleCount=qualityCount;
  report.normalizationSelectedVoxels=normalizationSelected;
  if(qualityCount)report.meanGradientError=quality;
  std::ostringstream diagnostic;
  diagnostic << "near-interface central-gradient mean |norm-1|=" << quality << " from " << qualityCount
      << " fully active stencils; this is not a closure/distance validity certificate";
  report.diagnostics.push_back(diagnostic.str());
  report.diagnostics.emplace_back("native precision, exact affine coefficients and file/grid metadata retained");
  if(maskedNormalization)report.diagnostics.emplace_back("native mask activity selects existing active distance voxels; mask values are ignored and every unselected world value/activity bit is retained");
  else if(maskSource)report.diagnostics.emplace_back("native alpha mask modulates the filter update; subsequent tracker normalization is global");
  if(!maskedNormalization)report.diagnostics.emplace_back("tracking may change active topology and all normalized distances");
  if(o.trim!=SdfTrimMode::All)report.diagnostics.emplace_back("partial/no trimming can retain out-of-band active distances");
  openvdb::io::File writer(privateOutput.string());writer.setCompression(openvdb::io::COMPRESS_ZIP);
  writer.write(openvdb::GridPtrVec{result},fileMetadata);writer.close();
  publishEdited(privateOutput,output,budget.p.maxInputBytes);
  return report;
}
