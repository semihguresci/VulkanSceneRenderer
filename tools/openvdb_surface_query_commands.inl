std::optional<int> runSurfaceQueryCommand(int argc,char** argv) {
  if(argc<2 || std::string_view(argv[1])!="--surface-query")return {};
  if(argc<3 || !argv[2][0] || std::string_view(argv[2]).starts_with("--") || std::string_view(argv[2])=="-h")
    throw std::invalid_argument("--surface-query requires INPUT before options");
  vdb::SurfaceQueryOptions o;vdb::ProcessingOptions p;std::string grid;
  std::vector<std::array<double,3>> queries;std::set<std::string> seen;
  for(int i=3;i<argc;++i) {
    const std::string arg=argv[i];
    if(arg=="--position") {
      if(queries.size()>=1'000'000)throw std::invalid_argument("surface query list exceeds 1000000 positions");
      std::array<double,3> q;
      for(auto& component:q)component=number(value(argc,argv,i,arg),arg);
      queries.push_back(q);continue;
    }
    if(!seen.insert(arg).second)throw std::invalid_argument("duplicate surface-query option: "+arg);
    if(arg=="--grid")grid=value(argc,argv,i,arg);
    else if(arg=="--max-surface-queries")o.maxQueries=count(value(argc,argv,i,arg),arg);
    else if(arg=="--max-surface-work")o.maxWork=count(value(argc,argv,i,arg),arg);
    else if(arg=="--max-surface-grids")o.maxGrids=count(value(argc,argv,i,arg),arg);
    else if(arg=="--max-surface-metadata")o.maxMetadataBytes=count(value(argc,argv,i,arg),arg);
    else if(arg=="--max-voxels")p.maxVoxelCount=count(value(argc,argv,i,arg),arg);
    else if(arg=="--max-input-bytes")p.maxInputBytes=count(value(argc,argv,i,arg),arg);
    else throw std::invalid_argument("unknown/incompatible surface-query option: "+arg);
  }
  const auto r=vdb::QueryLevelSetSurface(path(argv[2]),queries,o,grid,p);
  nlohmann::json items=nlohmann::json::array();
  for(const auto& q:r.queries) {
    items.push_back({{"query",q.query},{"closestPosition",q.closestPosition},{"distance",q.distance},
        {"querySdfValid",q.querySdfValid},{"querySdf",q.querySdfValid?nlohmann::json(q.querySdf):nlohmann::json(nullptr)},
        {"geometryValid",q.geometryValid},{"normal",q.geometryValid?nlohmann::json(q.normal):nlohmann::json(nullptr)},
        {"meanCurvature",q.geometryValid?nlohmann::json(q.meanCurvature):nlohmann::json(nullptr)},
        {"gaussianCurvature",q.geometryValid?nlohmann::json(q.gaussianCurvature):nlohmann::json(nullptr)}});
  }
  const auto metadata=[](const auto& source) {
    nlohmann::json result=nlohmann::json::array();
    for(const auto& m:source)result.push_back({{"name",m.name},{"type",m.type},{"value",m.value},
        {"nativeBytes",m.nativeBytes},{"opaque",m.opaque}});
    return result;
  };
  std::cout<<nlohmann::json{{"schemaVersion",1},{"gridName",r.gridName},{"valueType",r.valueType},
      {"voxelSize",r.voxelSize},{"diagnostic",r.diagnostic},{"metadata",metadata(r.metadata)},
      {"fileMetadata",metadata(r.fileMetadata)},{"queries",std::move(items)}}.dump(2)<<'\n';
  return EXIT_SUCCESS;
}
