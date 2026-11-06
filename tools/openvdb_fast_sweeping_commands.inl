std::optional<int> runFastSweepingCommand(int argc,char** argv) {
  if(argc<2)return {};
  const std::string_view command=argv[1];
  const bool extend=command=="--fast-extend";
  if(!extend && command!="--fast-sweep")return {};
  const int start=extend?5:4;
  if(argc<start)throw std::invalid_argument("fast sweeping requires INPUT [FIELD] OUTPUT before options");
  for(int i=2;i<start;++i)if(std::string_view(argv[i]).starts_with("--"))
    throw std::invalid_argument("fast sweeping requires INPUT [FIELD] OUTPUT before options");
  using Op=vdb::FastSweepingOperation;
  Op op=Op::ReinitializeSdf;vdb::FastSweepingOptions o;vdb::ProcessingOptions p;
  std::string grid,fieldGrid;bool fog=false,haveOperation=false;std::set<std::string> seen;
  for(int i=start;i<argc;++i) {
    const std::string arg=argv[i];
    if(!seen.insert(arg).second)throw std::invalid_argument("duplicate fast-sweeping option: "+arg);
    if(arg=="--sweep-operation" && !extend) {
      const auto name=value(argc,argv,i,arg);haveOperation=true;
      if(name=="fog")op=Op::FogToSdf;
      else if(name=="reinitialize")op=Op::ReinitializeSdf;
      else if(name=="dilate")op=Op::DilateSdf;
      else if(name=="mask")op=Op::MaskSdf;
      else if(name=="topology")op=Op::TopologyToSdf;
      else throw std::invalid_argument("--sweep-operation requires fog, reinitialize, dilate, mask or topology");
    } else if(arg=="--sweep-fog" && extend)fog=true;
    else if(arg=="--sweep-isovalue")o.isoValue=number(value(argc,argv,i,arg),arg);
    else if(arg=="--sweep-side") {
      const auto side=value(argc,argv,i,arg);
      if(side=="all")o.side=vdb::FastSweepingSide::All;
      else if(side=="greater")o.side=vdb::FastSweepingSide::GreaterThanIsovalue;
      else if(side=="less")o.side=vdb::FastSweepingSide::LessThanIsovalue;
      else throw std::invalid_argument("--sweep-side requires all, greater or less");
    } else if(arg=="--sweep-neighbors") {
      const auto n=value(argc,argv,i,arg);
      if(n=="face")o.neighbors=vdb::FastSweepingNeighbors::Face;
      else if(n=="face-edge")o.neighbors=vdb::FastSweepingNeighbors::FaceEdge;
      else if(n=="all")o.neighbors=vdb::FastSweepingNeighbors::FaceEdgeVertex;
      else throw std::invalid_argument("--sweep-neighbors requires face, face-edge or all");
    } else if(arg=="--sweep-mask") {
      o.mask=path(value(argc,argv,i,arg));
      if(o.mask.empty())throw std::invalid_argument("--sweep-mask requires a nonempty path");
    } else if(arg=="--sweep-mask-grid")o.maskGrid=value(argc,argv,i,arg);
    else if(arg=="--sweep-voxelize-tiles")o.voxelizeActiveTiles=true;
    else if(arg=="--sweep-iterations" || arg=="--sweep-dilation" || arg=="--topology-half-width" ||
            arg=="--topology-closing" || arg=="--topology-dilation" || arg=="--topology-smoothing") {
      const bool zero=arg=="--topology-closing" || arg=="--topology-dilation" || arg=="--topology-smoothing";
      const auto n=unsignedCount(value(argc,argv,i,arg),arg,zero);
      const uint64_t max=arg=="--sweep-iterations"?32:arg=="--sweep-dilation"?64:arg=="--topology-half-width"?16:8;
      if(n>max || (arg=="--topology-half-width" && n<3))throw std::invalid_argument("fast-sweeping count exceeds option range");
      if(arg=="--sweep-iterations")o.iterations=uint32_t(n);
      else if(arg=="--sweep-dilation")o.dilation=uint32_t(n);
      else if(arg=="--topology-half-width")o.topologyHalfWidth=uint32_t(n);
      else if(arg=="--topology-closing")o.topologyClosing=uint32_t(n);
      else if(arg=="--topology-dilation")o.topologyDilation=uint32_t(n);
      else o.topologySmoothing=uint32_t(n);
    } else if(arg=="--grid")grid=value(argc,argv,i,arg);
    else if(arg=="--field-grid" && extend)fieldGrid=value(argc,argv,i,arg);
    else if(arg=="--grid-name")p.gridName=value(argc,argv,i,arg);
    else if(arg=="--max-voxels")p.maxVoxelCount=count(value(argc,argv,i,arg),arg);
    else if(arg=="--max-input-bytes")p.maxInputBytes=count(value(argc,argv,i,arg),arg);
    else if(arg=="--max-sweep-work")o.maxWorkItems=count(value(argc,argv,i,arg),arg);
    else throw std::invalid_argument("unknown/incompatible fast-sweeping option: "+arg);
  }
  const bool topology=!extend && op==Op::TopologyToSdf;
  if((!extend && !haveOperation) || (seen.contains("--sweep-mask-grid") && o.mask.empty()) ||
      (!extend && op==Op::MaskSdf && o.mask.empty()) ||
      (seen.contains("--sweep-mask") && !extend && (op==Op::DilateSdf || topology)) ||
      (seen.contains("--sweep-isovalue") && !extend && (op==Op::DilateSdf || op==Op::MaskSdf)) ||
      ((seen.contains("--sweep-dilation") || seen.contains("--sweep-neighbors")) && (extend || op!=Op::DilateSdf)) ||
      (seen.contains("--sweep-side") && !extend && op!=Op::DilateSdf) ||
      ((seen.contains("--topology-half-width") || seen.contains("--topology-closing") ||
        seen.contains("--topology-dilation") || seen.contains("--topology-smoothing")) && !topology) ||
      (topology && (seen.contains("--sweep-isovalue") || seen.contains("--sweep-iterations"))))
    throw std::invalid_argument("fast-sweeping option does not apply to the selected operation");
  if(!seen.contains("--sweep-isovalue") && (fog || (!extend && op==Op::FogToSdf)))o.isoValue=.5;
  const auto r=extend?vdb::WriteFastExtendedField(path(argv[2]),path(argv[3]),path(argv[4]),fog,o,grid,fieldGrid,p):
      vdb::WriteFastSwept(path(argv[2]),path(argv[3]),op,o,grid,p);
  std::cout<<nlohmann::json{{"schemaVersion",1},{"gridName",r.grid.gridName},
      {"activeVoxelCount",r.grid.activeVoxelCount},{"extensionGrid",r.extensionGrid.gridName},
      {"inputActiveVoxelCount",r.inputActiveVoxelCount},{"boundaryVoxelCount",r.boundaryVoxelCount},
      {"sweepingVoxelCount",r.sweepingVoxelCount},{"workItems",r.workItems},{"diagnostics",r.diagnostics}}.dump(2)<<'\n';
  return EXIT_SUCCESS;
}
