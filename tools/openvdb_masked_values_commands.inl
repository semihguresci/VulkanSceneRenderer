std::optional<int> runMaskedValuesCommand(int argc,char **argv) {
  if(argc<2 || std::string_view(argv[1])!="--edit-topology-mask") return {};
  if(argc<5) throw std::invalid_argument("--edit-topology-mask requires INPUT MASK OUTPUT before options");
  for(int i=2;i<5;++i) if(std::string_view(argv[i]).starts_with("--"))
    throw std::invalid_argument("native masked value paths must precede options");
  vdb::ProcessingOptions limits; limits.gridName="field";
  auto op=vdb::MaskedValuesOperation::IntersectActivity;
  uint64_t work=200'000'000; bool haveOperation=false;
  std::string inputGrid,maskGrid; std::set<std::string> seen;
  for(int i=5;i<argc;++i) {
    const std::string arg=argv[i];
    if(!seen.insert(arg).second) throw std::invalid_argument("duplicate native masked option: "+arg);
    if(arg=="--mask-operation") {
      const auto text=value(argc,argv,i,arg); haveOperation=true;
      if(text=="activate") op=vdb::MaskedValuesOperation::Activate;
      else if(text=="deactivate") op=vdb::MaskedValuesOperation::Deactivate;
      else if(text=="intersect-activity") op=vdb::MaskedValuesOperation::IntersectActivity;
      else if(text=="clip-inside") op=vdb::MaskedValuesOperation::ClipInside;
      else if(text=="clip-outside") op=vdb::MaskedValuesOperation::ClipOutside;
      else throw std::invalid_argument("invalid native --mask-operation");
    } else if(arg=="--grid") inputGrid=value(argc,argv,i,arg);
    else if(arg=="--mask-grid") maskGrid=value(argc,argv,i,arg);
    else if(arg=="--grid-name") limits.gridName=value(argc,argv,i,arg);
    else if(arg=="--max-voxels") limits.maxVoxelCount=count(value(argc,argv,i,arg),arg);
    else if(arg=="--max-input-bytes") limits.maxInputBytes=count(value(argc,argv,i,arg),arg);
    else if(arg=="--max-topology-work") work=count(value(argc,argv,i,arg),arg);
    else throw std::invalid_argument("unknown/incompatible native masked option: "+arg);
  }
  if(!haveOperation) throw std::invalid_argument("--mask-operation is required");
  const auto r=vdb::WriteMaskedValues(path(argv[2]),path(argv[3]),path(argv[4]),op,inputGrid,maskGrid,limits,work);
  std::cout << nlohmann::json{{"schemaVersion",1},{"name",r.grid.gridName},{"gridClass",r.grid.gridClass},
      {"activeVoxelCount",r.grid.activeVoxelCount},{"inputActiveVoxelCount",r.inputActiveVoxelCountA},
      {"maskActiveVoxelCount",r.inputActiveVoxelCountB},{"activeTileCount",r.activeTileCount},
      {"storedLeafCount",r.storedLeafCount},{"workItems",r.workItems},{"diagnostics",r.diagnostics}}.dump(2) << '\n';
  return EXIT_SUCCESS;
}
