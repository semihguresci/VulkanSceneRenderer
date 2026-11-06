// Include after the tool's value/number/count/path helpers and the public
// OpenVdbLevelSetRebuild.h include. Dispatch runLevelSetRebuildCommand before
// the legacy parser. Tool availability is runtime, never a private VDB macro.
std::optional<int> runLevelSetRebuildCommand(int argc, char** argv) {
  if (argc < 2 || std::string_view(argv[1]) != "--rebuild-level-set") return {};
  if (argc < 4)
    throw std::invalid_argument("--rebuild-level-set requires INPUT OUTPUT before options");
  for (int i=2;i<4;++i)
    if (!argv[i][0] || std::string_view(argv[i]).starts_with("--") || std::string_view(argv[i])=="-h")
      throw std::invalid_argument("--rebuild-level-set requires nonempty INPUT OUTPUT before options; use ./-h for a literal filename");
  vdb::LevelSetRebuildOptions rebuild;
  vdb::ProcessingOptions processing;
  std::string selector;
  std::set<std::string> seen;
  const auto argument=[&](int& index,const std::string& option) {
    const auto text=value(argc,argv,index,option);
    if (text.empty() || std::string_view(text).starts_with("--") || text=="-h")
      throw std::invalid_argument("nonempty value required for "+option);
    return text;
  };
  for (int i=4;i<argc;++i) {
    const std::string arg=argv[i],key=arg=="--input-grid"?"--grid":arg;
    if (!seen.insert(key).second)
      throw std::invalid_argument("duplicate level-set rebuild option: "+arg);
    if (arg=="--rebuild-isovalue") rebuild.isovalue=number(argument(i,arg),arg);
    else if (arg=="--rebuild-exterior-width" || arg=="--rebuild-interior-width") {
      const auto width=number(argument(i,arg),arg);
      if (width<1 || width>64)
        throw std::invalid_argument("rebuild band widths require [1,64] TARGET voxels");
      if (arg=="--rebuild-exterior-width") rebuild.exteriorWidth=width;
      else rebuild.interiorWidth=width;
    } else if (arg=="--rebuild-target-transform") {
      std::array<double,16> matrix;
      for (auto& x:matrix) x=number(argument(i,arg),arg);
      if (matrix[12]!=0 || matrix[13]!=0 || matrix[14]!=0 || matrix[15]!=1)
        throw std::invalid_argument("rebuild target requires16 row-major COLUMN-vector affine entries; translation at3,7,11 and last row0,0,0,1");
      rebuild.targetIndexToWorld=matrix;
    } else if (arg=="--max-rebuild-work") rebuild.maxWork=count(argument(i,arg),arg);
    else if (arg=="--max-rebuild-grids") {
      const auto n=count(argument(i,arg),arg);
      if (n>4096) throw std::invalid_argument("rebuild grid catalog cap must be1..4096");
      rebuild.maxGrids=size_t(n);
    } else if (arg=="--max-rebuild-metadata") rebuild.maxMetadataBytes=count(argument(i,arg),arg);
    else if (key=="--grid") selector=argument(i,arg);
    else if (arg=="--grid-name") processing.gridName=argument(i,arg);
    else if (arg=="--max-voxels") processing.maxVoxelCount=count(argument(i,arg),arg);
    else if (arg=="--max-input-bytes") processing.maxInputBytes=count(argument(i,arg),arg);
    else if (arg=="--max-triangles") processing.maxTriangles=size_t(count(argument(i,arg),arg));
    else throw std::invalid_argument("unknown/incompatible level-set rebuild option: "+arg);
  }
  const auto result=vdb::WriteRebuiltLevelSet(path(argv[2]),path(argv[3]),rebuild,selector,processing);
  std::cout<<nlohmann::json{{"schemaVersion",1},{"operation","rebuild-level-set"},
      {"output",argv[3]},{"gridName",result.grid.gridName},{"gridClass",result.grid.gridClass},
      {"sourceValueType",result.sourceValueType},{"valueType",result.sourceValueType},
      {"voxelSize",result.grid.voxelSize},{"activeVoxelCount",result.grid.activeVoxelCount},
      {"inputActiveVoxelCount",result.inputActiveVoxelCount},{"isovalue",result.isovalue},
      {"effectiveExteriorWidth",result.effectiveExteriorWidth},{"effectiveInteriorWidth",result.effectiveInteriorWidth},
      {"sourceIndexOrigin",result.sourceIndexOrigin},{"targetIndexOrigin",result.targetIndexOrigin},
      {"meshTriangleUpperBound",result.meshTriangleUpperBound},{"workUpperBound",result.workUpperBound},
      {"targetIndexToWorld",rebuild.targetIndexToWorld?nlohmann::json(*rebuild.targetIndexToWorld):nlohmann::json(nullptr)},
      {"diagnostics",result.diagnostics}}.dump(2)<<'\n';
  return EXIT_SUCCESS;
}
