std::optional<int> runSdfBackgroundCommand(int argc,char **argv) {
  if(argc<2 || std::string_view(argv[1])!="--sdf-background") return {};
  if(argc<4 || std::string_view(argv[2]).starts_with("--") || std::string_view(argv[3]).starts_with("--"))
    throw std::invalid_argument("--sdf-background requires INPUT OUTPUT before options");
  vdb::ProcessingOptions limits; vdb::SdfBackgroundOptions options;
  auto operation=vdb::SdfBackgroundOperation::BackgroundOnly;
  std::string grid; bool haveMode=false; std::set<std::string> seen;
  for(int i=4;i<argc;++i) {
    const std::string arg=argv[i];
    if(!seen.insert(arg).second) throw std::invalid_argument("duplicate SDF background option: "+arg);
    if(arg=="--sdf-mode") {
      haveMode=true; const auto mode=value(argc,argv,i,arg);
      if(mode=="background") operation=vdb::SdfBackgroundOperation::BackgroundOnly;
      else if(mode=="flood-fill") operation=vdb::SdfBackgroundOperation::FloodFill;
      else throw std::invalid_argument("--sdf-mode requires background or flood-fill");
    } else if(arg=="--outside-width") options.outsideWidth=number(value(argc,argv,i,arg),arg);
    else if(arg=="--inside-width") options.insideWidth=number(value(argc,argv,i,arg),arg);
    else if(arg=="--grid") grid=value(argc,argv,i,arg);
    else if(arg=="--grid-name") limits.gridName=value(argc,argv,i,arg);
    else if(arg=="--max-voxels") limits.maxVoxelCount=count(value(argc,argv,i,arg),arg);
    else if(arg=="--max-input-bytes") limits.maxInputBytes=count(value(argc,argv,i,arg),arg);
    else if(arg=="--max-topology-work") options.maxWorkItems=count(value(argc,argv,i,arg),arg);
    else throw std::invalid_argument("unknown/incompatible SDF background option: "+arg);
  }
  if(!haveMode) throw std::invalid_argument("--sdf-mode is required");
  const auto r=vdb::WriteSdfBackground(path(argv[2]),path(argv[3]),operation,options,grid,limits);
  std::cout << nlohmann::json{{"schemaVersion",1},{"name",r.grid.gridName},{"gridClass",r.grid.gridClass},
      {"activeVoxelCount",r.grid.activeVoxelCount},{"storedLeafCount",r.storedLeafCount},
      {"workItems",r.workItems},{"diagnostics",r.diagnostics}}.dump(2) << '\n';
  return EXIT_SUCCESS;
}
