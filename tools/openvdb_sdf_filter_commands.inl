std::optional<int> runSdfFilterCommand(int argc,char** argv) {
  if(argc<2 || std::string_view(argv[1])!="--sdf-filter")return {};
  if(argc<4 || std::string_view(argv[2]).starts_with("--") || std::string_view(argv[3]).starts_with("--"))
    throw std::invalid_argument("--sdf-filter requires INPUT OUTPUT before options");
  using Op=vdb::SdfFilterOperation;
  vdb::SdfFilterOptions o;vdb::ProcessingOptions p;std::string grid;
  Op operation=Op::Mean;bool haveOperation=false;std::set<std::string> seen;
  for(int i=4;i<argc;++i) {
    const std::string arg=argv[i];
    if(!seen.insert(arg).second)throw std::invalid_argument("duplicate SDF filter option: "+arg);
    if(arg=="--sdf-filter-operation") {
      const auto name=value(argc,argv,i,arg);haveOperation=true;
      if(name=="gaussian")operation=Op::Gaussian;
      else if(name=="mean")operation=Op::Mean;
      else if(name=="median")operation=Op::Median;
      else if(name=="mean-curvature")operation=Op::MeanCurvature;
      else if(name=="laplacian")operation=Op::Laplacian;
      else if(name=="fillet")operation=Op::Fillet;
      else if(name=="offset")operation=Op::Offset;
      else if(name=="normalize")operation=Op::Normalize;
      else if(name=="track")operation=Op::Track;
      else if(name=="prune")operation=Op::Prune;
      else if(name=="resize-band")operation=Op::ResizeBand;
      else if(name=="dilate-band")operation=Op::DilateBand;
      else if(name=="erode-band")operation=Op::ErodeBand;
      else throw std::invalid_argument("unknown --sdf-filter-operation");
    } else if(arg=="--sdf-spatial") {
      const auto scheme=value(argc,argv,i,arg);
      if(scheme=="first")o.spatial=vdb::SdfSpatialScheme::FirstOrder;
      else if(scheme=="second")o.spatial=vdb::SdfSpatialScheme::SecondOrder;
      else if(scheme=="third")o.spatial=vdb::SdfSpatialScheme::ThirdOrder;
      else if(scheme=="weno5")o.spatial=vdb::SdfSpatialScheme::Weno5;
      else if(scheme=="hjweno5")o.spatial=vdb::SdfSpatialScheme::HjWeno5;
      else throw std::invalid_argument("--sdf-spatial requires first, second, third, weno5 or hjweno5");
    } else if(arg=="--sdf-temporal") {
      const auto scheme=value(argc,argv,i,arg);
      if(scheme=="rk1")o.temporal=vdb::SdfTemporalScheme::Rk1;
      else if(scheme=="rk2")o.temporal=vdb::SdfTemporalScheme::Rk2;
      else if(scheme=="rk3")o.temporal=vdb::SdfTemporalScheme::Rk3;
      else throw std::invalid_argument("--sdf-temporal requires rk1, rk2 or rk3");
    } else if(arg=="--sdf-trim") {
      const auto mode=value(argc,argv,i,arg);
      if(mode=="none")o.trim=vdb::SdfTrimMode::None;
      else if(mode=="interior")o.trim=vdb::SdfTrimMode::Interior;
      else if(mode=="exterior")o.trim=vdb::SdfTrimMode::Exterior;
      else if(mode=="all")o.trim=vdb::SdfTrimMode::All;
      else throw std::invalid_argument("--sdf-trim requires none, interior, exterior or all");
    } else if(arg=="--sdf-width" || arg=="--sdf-iterations" || arg=="--sdf-normalizations" ||
              arg=="--sdf-band-width" || arg=="--sdf-band-layers") {
      const auto amount=unsignedCount(value(argc,argv,i,arg),arg,arg=="--sdf-normalizations");
      const uint64_t maximum=arg=="--sdf-width"?4:arg=="--sdf-iterations"?32:64;
      if(amount>maximum)throw std::invalid_argument("SDF filter count exceeds its option limit");
      if(arg=="--sdf-width")o.width=uint32_t(amount);
      else if(arg=="--sdf-iterations")o.iterations=uint32_t(amount);
      else if(arg=="--sdf-normalizations")o.normalizationCount=uint32_t(amount);
      else if(arg=="--sdf-band-width")o.bandWidth=uint32_t(amount);
      else o.bandLayers=uint32_t(amount);
    } else if(arg=="--sdf-distance")o.worldDistance=number(value(argc,argv,i,arg),arg);
    else if(arg=="--sdf-mask") {
      o.maskInput=path(value(argc,argv,i,arg));
      if(o.maskInput.empty())throw std::invalid_argument("--sdf-mask requires a nonempty path");
    } else if(arg=="--sdf-mask-grid")o.maskGrid=value(argc,argv,i,arg);
    else if(arg=="--sdf-normalization-mask") {
      o.normalizationMaskInput=path(value(argc,argv,i,arg));
      if(o.normalizationMaskInput.empty())throw std::invalid_argument("--sdf-normalization-mask requires a nonempty path");
    } else if(arg=="--sdf-normalization-grid")o.normalizationMaskGrid=value(argc,argv,i,arg);
    else if(arg=="--sdf-alpha-range") {
      o.alphaMinimum=number(value(argc,argv,i,arg),arg);o.alphaMaximum=number(value(argc,argv,i,arg),arg);
      if(!(o.alphaMinimum<o.alphaMaximum))throw std::invalid_argument("SDF alpha minimum must be less than maximum");
    } else if(arg=="--sdf-invert-alpha")o.invertAlpha=true;
    else if(arg=="--grid")grid=value(argc,argv,i,arg);
    else if(arg=="--grid-name")p.gridName=value(argc,argv,i,arg);
    else if(arg=="--max-voxels")p.maxVoxelCount=count(value(argc,argv,i,arg),arg);
    else if(arg=="--max-input-bytes")p.maxInputBytes=count(value(argc,argv,i,arg),arg);
    else if(arg=="--max-sdf-work") {
      o.maxWorkItems=count(value(argc,argv,i,arg),arg);
      if(o.maxWorkItems>10'000'000'000ull)throw std::invalid_argument("--max-sdf-work exceeds 10000000000");
    } else throw std::invalid_argument("unknown/incompatible SDF filter option: "+arg);
  }
  if(!haveOperation)throw std::invalid_argument("--sdf-filter-operation is required");
  const bool kernel=operation==Op::Gaussian || operation==Op::Mean || operation==Op::Median;
  const bool layers=operation==Op::DilateBand || operation==Op::ErodeBand;
  const bool trackerOnly=operation>Op::Offset;
  if((seen.contains("--sdf-width") && !kernel) ||
      (seen.contains("--sdf-distance")!=(operation==Op::Offset)) ||
      (seen.contains("--sdf-band-width") && operation!=Op::ResizeBand) ||
      (seen.contains("--sdf-band-layers") && !layers) ||
      (seen.contains("--sdf-mask") && trackerOnly) ||
      (seen.contains("--sdf-normalization-mask") && operation!=Op::Normalize) ||
      (seen.contains("--sdf-normalization-grid") && o.normalizationMaskInput.empty()) ||
      ((seen.contains("--sdf-mask-grid") || seen.contains("--sdf-alpha-range") || seen.contains("--sdf-invert-alpha")) && o.maskInput.empty()) ||
      (seen.contains("--sdf-trim") && (operation==Op::Normalize || operation==Op::ResizeBand || layers)) ||
      ((seen.contains("--sdf-spatial") || seen.contains("--sdf-temporal") || seen.contains("--sdf-normalizations")) &&
        (operation==Op::Prune || operation==Op::ErodeBand)))
    throw std::invalid_argument("SDF filter option does not apply to this operation, or offset distance is missing");
  const auto r=vdb::WriteSdfFiltered(path(argv[2]),path(argv[3]),operation,o,grid,p);
  std::cout<<nlohmann::json{{"schemaVersion",1},{"name",r.grid.gridName},{"gridClass",r.grid.gridClass},
      {"activeVoxelCount",r.grid.activeVoxelCount},{"storedLeafCount",r.storedLeafCount},{"workItems",r.workItems},
      {"halfWidthVoxels",r.halfWidthVoxels},{"gradientSampleCount",r.gradientSampleCount},
      {"meanGradientError",optionalJson(r.meanGradientError)},
      {"normalizationSelectedVoxels",optionalJson(r.normalizationSelectedVoxels)},
      {"diagnostics",r.diagnostics}}.dump(2)<<'\n';
  return EXIT_SUCCESS;
}
