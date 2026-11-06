std::optional<int> runRegionsCommand(int argc,char** argv) {
  if(argc<2 || std::string_view(argv[1])!="--regions")return {};
  if(argc<4 || !argv[2][0] || !argv[3][0] || std::string_view(argv[2]).starts_with("--") || std::string_view(argv[3]).starts_with("--") || std::string_view(argv[2])=="-h" || std::string_view(argv[3])=="-h")
    throw std::invalid_argument("--regions requires INPUT OUTPUT before options");
  using Op=vdb::RegionsOperation;Op op=Op::ActiveComponents;bool haveOperation=false;
  vdb::RegionsOptions o;vdb::ProcessingOptions p;std::string grid;std::set<std::string> seen;
  for(int i=4;i<argc;++i) {
    const std::string arg=argv[i];
    if(!seen.insert(arg).second)throw std::invalid_argument("duplicate regions option: "+arg);
    if(arg=="--region-operation") {
      const auto mode=value(argc,argv,i,arg);haveOperation=true;
      if(mode=="components")op=Op::ActiveComponents;
      else if(mode=="interior")op=Op::SignedInterior;
      else if(mode=="enclosed")op=Op::EnclosedRegion;
      else if(mode=="cavities")op=Op::Cavities;
      else throw std::invalid_argument("--region-operation requires components, interior, enclosed or cavities");
    } else if(arg=="--region-isovalue")o.isovalue=number(value(argc,argv,i,arg),arg);
    else if(arg=="--max-region-components" || arg=="--max-region-grids") {
      const auto n=count(value(argc,argv,i,arg),arg);
      if(n>(arg=="--max-region-components"?1024:4096))throw std::invalid_argument("region component/catalog limit exceeded");
      if(arg=="--max-region-components")o.maxComponents=uint32_t(n);else o.maxGrids=uint32_t(n);
    } else if(arg=="--max-region-work")o.maxWorkItems=count(value(argc,argv,i,arg),arg);
    else if(arg=="--max-region-metadata")o.maxMetadataBytes=count(value(argc,argv,i,arg),arg);
    else if(arg=="--grid")grid=value(argc,argv,i,arg);
    else if(arg=="--grid-name")p.gridName=value(argc,argv,i,arg);
    else if(arg=="--max-voxels")p.maxVoxelCount=count(value(argc,argv,i,arg),arg);
    else if(arg=="--max-input-bytes")p.maxInputBytes=count(value(argc,argv,i,arg),arg);
    else throw std::invalid_argument("unknown/incompatible regions option: "+arg);
  }
  if(!haveOperation || (seen.contains("--region-isovalue") && op==Op::ActiveComponents) ||
      (seen.contains("--max-region-components") && op!=Op::ActiveComponents))
    throw std::invalid_argument("regions operation missing or incompatible component/isovalue option");
  const auto r=vdb::WriteRegions(path(argv[2]),path(argv[3]),op,o,grid,p);
  nlohmann::json grids=nlohmann::json::array();
  for(const auto& g:r.grids)grids.push_back({{"name",g.gridName},{"gridClass",g.gridClass},
      {"activeVoxelCount",g.activeVoxelCount},{"voxelSize",g.voxelSize}});
  std::cout<<nlohmann::json{{"schemaVersion",1},{"grids",std::move(grids)},
      {"sourceActiveVoxelCount",r.sourceActiveVoxelCount},{"totalActiveVoxelCount",r.totalActiveVoxelCount},
      {"workItems",r.workItems},{"diagnostics",r.diagnostics}}.dump(2)<<'\n';
  return EXIT_SUCCESS;
}
