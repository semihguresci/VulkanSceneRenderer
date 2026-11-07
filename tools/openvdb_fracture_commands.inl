std::optional<int> runFractureCommand(int argc,char** argv) {
  if(argc<2 || std::string_view(argv[1])!="--fracture")return {};
  if(argc<5)throw std::invalid_argument("--fracture requires INPUT CUTTER OUTPUT before options");
  for(int i=2;i<5;++i)if(!argv[i][0] || std::string_view(argv[i]).starts_with("--") || std::string_view(argv[i])=="-h")
    throw std::invalid_argument("--fracture requires INPUT CUTTER OUTPUT before options");
  vdb::FractureOptions o;vdb::ProcessingOptions p;std::string grid,cutterGrid;std::set<std::string> seen;
  for(int i=5;i<argc;++i) {
    const std::string arg=argv[i];
    if(arg=="--fracture-instance") {
      if(o.instances.size()>=32)throw std::invalid_argument("fracture supports at most 32 instances");
      vdb::FractureInstance instance;
      for(auto& x:instance.translation)x=number(value(argc,argv,i,arg),arg);
      for(auto& x:instance.rotation)x=number(value(argc,argv,i,arg),arg);
      o.instances.push_back(instance);continue;
    }
    if(arg=="--fracture-pivot") {
      if(o.instances.empty() || o.instances.back().pivot)
        throw std::invalid_argument("fracture pivot requires a preceding instance without a pivot");
      std::array<double,3> pivot;
      for(auto& x:pivot)x=number(value(argc,argv,i,arg),arg);
      o.instances.back().pivot=pivot;continue;
    }
    if(!seen.insert(arg).second)throw std::invalid_argument("duplicate fracture option: "+arg);
    if(arg=="--fracture-disjoint-cutters")o.cutterOverlap=false;
    else if(arg=="--fracture-segment")o.segment=true;
    else if(arg=="--max-fracture-pieces") {
      const auto n=count(value(argc,argv,i,arg),arg);
      if(n>256)throw std::invalid_argument("fracture piece cap exceeds 256");o.maxPieces=uint32_t(n);
    } else if(arg=="--max-fracture-work")o.maxWorkItems=count(value(argc,argv,i,arg),arg);
    else if(arg=="--grid")grid=value(argc,argv,i,arg);
    else if(arg=="--cutter-grid")cutterGrid=value(argc,argv,i,arg);
    else if(arg=="--grid-name")p.gridName=value(argc,argv,i,arg);
    else if(arg=="--max-voxels")p.maxVoxelCount=count(value(argc,argv,i,arg),arg);
    else if(arg=="--max-input-bytes")p.maxInputBytes=count(value(argc,argv,i,arg),arg);
    else throw std::invalid_argument("unknown/incompatible fracture option: "+arg);
  }
  const auto r=vdb::WriteFracturedLevelSet(path(argv[2]),path(argv[3]),path(argv[4]),o,grid,cutterGrid,p);
  nlohmann::json pieces=nlohmann::json::array();
  for(const auto& piece:r.pieces)pieces.push_back({{"name",piece.grid.gridName},
      {"activeVoxelCount",piece.grid.activeVoxelCount},{"residual",piece.residual},
      {"instance",piece.instance},{"negativeVoxelCount",piece.negativeVoxelCount}});
  std::cout<<nlohmann::json{{"schemaVersion",1},{"pieces",std::move(pieces)},
      {"inputNegativeVoxelCount",r.inputNegativeVoxelCount},{"outputNegativeVoxelCount",r.outputNegativeVoxelCount},
      {"cutBoundaryVoxelCount",r.cutBoundaryVoxelCount},{"workItems",r.workItems},
      {"diagnostics",r.diagnostics}}.dump(2)<<'\n';
  return EXIT_SUCCESS;
}
