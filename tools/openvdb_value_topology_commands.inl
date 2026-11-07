// Dedicated strict parser keeps native literals away from Double conversion.
std::optional<int> runValueTopologyCommand(int argc,char **argv) {
  if(argc<2 || std::string_view(argv[1])!="--edit-topology") return {};
  if(argc<4 || std::string_view(argv[2]).starts_with("--") || std::string_view(argv[3]).starts_with("--"))
    throw std::invalid_argument("--edit-topology requires INPUT OUTPUT before options");
  vdb::ValueTopologyOptions edit;
  vdb::ProcessingOptions limits; limits.gridName="field";
  auto op=vdb::ValueTopologyOperation::PruneExact;
  bool haveOperation=false,haveValue=false,haveBox=false,haveFill=false;
  std::string selection; std::set<std::string> seen;
  for(int i=4;i<argc;++i) {
    const std::string arg=argv[i];
    if(!seen.insert(arg).second) throw std::invalid_argument("duplicate value topology option: "+arg);
    if(arg=="--edit-operation") {
      const auto text=value(argc,argv,i,arg); haveOperation=true;
      if(text=="activate-value") op=vdb::ValueTopologyOperation::ActivateValue;
      else if(text=="deactivate-value") op=vdb::ValueTopologyOperation::DeactivateValue;
      else if(text=="prune-exact") op=vdb::ValueTopologyOperation::PruneExact;
      else if(text=="fill-box") op=vdb::ValueTopologyOperation::FillBox;
      else if(text=="clip-box") op=vdb::ValueTopologyOperation::ClipBox;
      else if(text=="replace-background") op=vdb::ValueTopologyOperation::ReplaceBackground;
      else throw std::invalid_argument("invalid --edit-operation");
    } else if(arg=="--native-value") {
      edit.value={value(argc,argv,i,arg)}; haveValue=true;
    } else if(arg=="--native-vector") {
      edit.value.clear(); for(int n=0;n<3;++n) edit.value.push_back(value(argc,argv,i,arg)); haveValue=true;
    } else if(arg=="--index-box") {
      for(auto &v:edit.minimum) v=indexCoordinate(value(argc,argv,i,arg),arg);
      for(auto &v:edit.maximum) v=indexCoordinate(value(argc,argv,i,arg),arg);
      haveBox=true;
    } else if(arg=="--fill-inactive") { edit.fillActive=false; haveFill=true;
    } else if(arg=="--grid") selection=value(argc,argv,i,arg);
    else if(arg=="--grid-name") limits.gridName=value(argc,argv,i,arg);
    else if(arg=="--max-voxels") limits.maxVoxelCount=count(value(argc,argv,i,arg),arg);
    else if(arg=="--max-input-bytes") limits.maxInputBytes=count(value(argc,argv,i,arg),arg);
    else if(arg=="--max-topology-work") edit.maxWorkItems=count(value(argc,argv,i,arg),arg);
    else throw std::invalid_argument("unknown/incompatible value topology option: "+arg);
  }
  if(seen.contains("--native-value") && seen.contains("--native-vector"))
    throw std::invalid_argument("choose --native-value or --native-vector");
  if(!haveOperation) throw std::invalid_argument("--edit-operation is required");
  const bool box=op==vdb::ValueTopologyOperation::FillBox || op==vdb::ValueTopologyOperation::ClipBox;
  const bool needsValue=op!=vdb::ValueTopologyOperation::PruneExact && op!=vdb::ValueTopologyOperation::ClipBox;
  if(haveBox!=box || haveValue!=needsValue || (haveFill && op!=vdb::ValueTopologyOperation::FillBox))
    throw std::invalid_argument("native value/index box/fill activity does not match the selected edit operation");
  const auto r=vdb::WriteValueTopology(path(argv[2]),path(argv[3]),op,edit,selection,limits);
  std::cout << nlohmann::json{{"schemaVersion",1},{"name",r.grid.gridName},{"gridClass",r.grid.gridClass},
      {"activeVoxelCount",r.grid.activeVoxelCount},{"activeTileCount",r.activeTileCount},
      {"storedLeafCount",r.storedLeafCount},{"workItems",r.workItems},{"diagnostics",r.diagnostics}}.dump(2) << '\n';
  return EXIT_SUCCESS;
}
