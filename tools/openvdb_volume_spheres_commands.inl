std::optional<int> runVolumeSpheresCommand(int argc,char** argv) {
  if(argc<2 || std::string_view(argv[1])!="--volume-spheres")return {};
  if(argc<4 || std::string_view(argv[2]).starts_with("--") || std::string_view(argv[3]).starts_with("--"))
    throw std::invalid_argument("--volume-spheres requires INPUT OUTPUT.json before options");
  vdb::VolumeSpheresOptions o;vdb::ProcessingOptions p;std::string grid;std::set<std::string> seen;
  for(int i=4;i<argc;++i) {
    const std::string arg=argv[i];
    if(!seen.insert(arg).second)throw std::invalid_argument("duplicate sphere-fitting option: "+arg);
    if(arg=="--sphere-min-count" || arg=="--sphere-max-count" || arg=="--sphere-candidates") {
      const auto n=unsignedCount(value(argc,argv,i,arg),arg,arg=="--sphere-min-count");
      const uint64_t maximum=arg=="--sphere-candidates"?100000:4096;
      if(n>maximum)throw std::invalid_argument("sphere-fitting count exceeds option limit");
      if(arg=="--sphere-min-count")o.minCount=uint32_t(n);
      else if(arg=="--sphere-max-count")o.maxCount=uint32_t(n);
      else o.candidateCount=uint32_t(n);
    } else if(arg=="--sphere-min-radius")o.minRadiusWorld=number(value(argc,argv,i,arg),arg);
    else if(arg=="--sphere-max-radius")o.maxRadiusWorld=number(value(argc,argv,i,arg),arg);
    else if(arg=="--sphere-overlap")o.overlapping=true;
    else if(arg=="--iso")o.isovalue=number(value(argc,argv,i,arg),arg);
    else if(arg=="--grid")grid=value(argc,argv,i,arg);
    else if(arg=="--max-voxels")p.maxVoxelCount=count(value(argc,argv,i,arg),arg);
    else if(arg=="--max-input-bytes")p.maxInputBytes=count(value(argc,argv,i,arg),arg);
    else if(arg=="--max-sphere-work")o.maxWork=count(value(argc,argv,i,arg),arg);
    else if(arg=="--max-sphere-grids")o.maxGrids=count(value(argc,argv,i,arg),arg);
    else if(arg=="--max-sphere-metadata")o.maxMetadataBytes=count(value(argc,argv,i,arg),arg);
    else throw std::invalid_argument("unknown/incompatible sphere-fitting option: "+arg);
  }
  if(o.minCount>o.maxCount || o.minRadiusWorld<0 || o.maxRadiusWorld<=0 || o.minRadiusWorld>o.maxRadiusWorld ||
      o.maxGrids>4096)
    throw std::invalid_argument("invalid sphere-fitting count/radius/catalog interval");
  const auto r=vdb::ExportVolumeSpheres(path(argv[2]),path(argv[3]),o,grid,p);
  std::cout<<nlohmann::json{{"schemaVersion",1},{"gridName",r.gridName},{"valueType",r.valueType},
      {"sphereCount",r.spheres.size()},{"voxelSize",r.voxelSize},{"isovalue",r.isovalue},
      {"diagnostic",r.diagnostic}}.dump(2)<<'\n';
  return EXIT_SUCCESS;
}
