// TU prerequisites: OpenVdbBoundaryProjection.h, runtime IsAvailable(),
// <charconv>, and common value/number/count/path helpers. Dispatch
// runBoundaryProjectionCommand before the legacy parser. Help is retained in
// out/openvdb-review/boundary-projection/usage-draft.md.
namespace boundary_projection_cli {
bool utf8(std::string_view s) noexcept {
  for(size_t i=0;i<s.size();){const auto c=static_cast<unsigned char>(s[i++]);if(c<0x80)continue;
    size_t n;unsigned char low=0x80,high=0xbf;
    if(c>=0xc2&&c<=0xdf)n=1;else if(c>=0xe0&&c<=0xef){n=2;if(c==0xe0)low=0xa0;if(c==0xed)high=0x9f;}
    else if(c>=0xf0&&c<=0xf4){n=3;if(c==0xf0)low=0x90;if(c==0xf4)high=0x8f;}else return false;
    if(n>s.size()-i)return false;const auto first=static_cast<unsigned char>(s[i++]);if(first<low||first>high)return false;
    while(--n){const auto next=static_cast<unsigned char>(s[i++]);if(next<0x80||next>0xbf)return false;}
  }return true;
}
struct Report {
  std::string text,output,names,bindings,controls;uintmax_t reservation=0;bool replay=false;
  Report(std::string_view destination,const std::vector<std::string>& caller,
         const nlohmann::json& sourceBindings,const vdb::BoundaryProjectionOptions& o,
         const vdb::ProcessingOptions& p,bool replayed):replay(replayed){
    long double bytes=0;for(const auto& s:caller){if(!utf8(s))throw std::invalid_argument("boundary projection CLI requires valid UTF-8 before output publication");bytes+=s.size();}
    const long double reserve=262144+32*bytes;if(bytes>256*1024||reserve>=p.maxInputBytes||reserve>std::numeric_limits<size_t>::max())throw std::invalid_argument("boundary projection CLI report/options reservation exceeds --max-input-bytes");reservation=uintmax_t(reserve);
    output=nlohmann::json(destination).dump();bindings=sourceBindings.dump();
    names=nlohmann::json{{"velocity",o.projection.velocityGridName},{"pressure",o.projection.pressureGridName},
      {"fluid",o.projection.writeDomain?nlohmann::json(o.projection.domainGridName):nlohmann::json(nullptr)},
      {"faceKind",o.writeBoundaryState?nlohmann::json(o.faceKindGridName):nlohmann::json(nullptr)},
      {"faceFraction",o.writeBoundaryState?nlohmann::json(o.faceFractionGridName):nlohmann::json(nullptr)},
      {"facePressure",o.writeBoundaryState?nlohmann::json(o.facePressureGridName):nlohmann::json(nullptr)},
      {"wallVelocity",o.writeBoundaryState?nlohmann::json(o.wallVelocityGridName):nlohmann::json(nullptr)}}.dump();
    controls=nlohmann::json{{"timeStep",o.projection.timeStep},{"accelerationWorld",o.projection.acceleration},
      {"tolerance",o.projection.tolerance},{"maxIterationsPerComponent",o.projection.maxIterations},
      {"maxFluidCells",o.projection.maxFluidCells},{"maxWorkItems",o.maxWorkItems}}.dump();
    text.reserve(size_t(65536+16*bytes));
  }
  template<class T>void numeric(T v){std::array<char,128> b;std::to_chars_result result;
    if constexpr(std::is_floating_point_v<T>)result=std::to_chars(b.data(),b.data()+b.size(),v,std::chars_format::general,std::numeric_limits<T>::max_digits10);
    else result=std::to_chars(b.data(),b.data()+b.size(),v);text.append(b.data(),size_t(result.ptr-b.data()));}
  void boolean(bool v){text.append(v?"true":"false");}
  template<class T>void field(const char* key,T value){text.append(",\"");text.append(key);text.append("\":");if constexpr(std::is_same_v<T,bool>)boolean(value);else numeric(value);}
  void string(std::string_view s){constexpr char hex[]="0123456789abcdef";text.push_back('"');for(unsigned char c:s){if(c=='"'||c=='\\'){text.push_back('\\');text.push_back(char(c));}else if(c<0x20){text.append("\\u00");text.push_back(hex[c>>4]);text.push_back(hex[c&15]);}else text.push_back(char(c));}text.push_back('"');}
  void write(const vdb::BoundaryProjectionResult& r){const auto& p=r.projection;
    text.append("{\"schemaVersion\":1,\"operation\":\"");text.append(replay?"reproject-boundaries":"project-boundaries");text.append("\",\"output\":");text.append(output);
    text.append(",\"bindings\":");text.append(bindings);text.append(",\"controls\":");text.append(controls);text.append(",\"gridNames\":");text.append(names);
    text.append(",\"projection\":{\"converged\":");boolean(p.converged);
    field("fluidCellCount",p.fluidCellCount);field("faceCount",p.faceCount);field("componentCount",p.componentCount);field("iterations",p.iterations);
    field("maxDivergenceBefore",p.maxDivergenceBefore);field("maxDivergenceAfter",p.maxDivergenceAfter);field("residual",p.residual);
    field("divergenceRoundoffBound",p.divergenceRoundoffBound);field("roundoffLimited",p.roundoffLimited);text.push_back('}');
    field("fluidFluidFaces",r.fluidFluidFaces);field("wallFaces",r.wallFaces);field("openFacePressureFaces",r.openFacePressureFaces);
    field("airCellPressureFaces",r.airCellPressureFaces);field("freeSurfacePressureFaces",r.freeSurfacePressureFaces);field("clampedInterfaceFaces",r.clampedInterfaceFaces);
    field("dirichletComponents",r.dirichletComponents);field("neumannComponents",r.neumannComponents);field("totalIterations",r.totalIterations);
    field("workUpperBound",r.workUpperBound);field("roundoffLimitedCells",r.roundoffLimitedCells);field("minimumEffectiveFraction",r.minimumEffectiveFraction);
    field("maximumFractionClamp",r.maximumFractionClamp);field("maxClosedNormalFlux",r.maxClosedNormalFlux);field("maxWallNormalError",r.maxWallNormalError);
    field("replayed",r.replayed);field("boundaryStateWritten",r.boundaryStateWritten);
    text.append(",\"velocityPolicy\":\"WORLD tags use raw physical XYZ velocity components; MAC uses local negative faces in voxels/second\"");
    text.append(",\"replayPolicy\":\"Old pressure is recomputed; nonuniform prescribed pressure adds a fresh impulse on every call\"");
    field("nativeDiagnosticCount",r.diagnostics.size());text.append(",\"diagnostics\":[");bool truncated=r.diagnostics.size()>8;
    for(size_t i=0;i<std::min<size_t>(8,r.diagnostics.size());++i){if(i)text.push_back(',');const auto& s=r.diagnostics[i];size_t n=std::min<size_t>(1024,s.size());if(n<s.size())while(n&&(static_cast<unsigned char>(s[n])&0xc0)==0x80)--n;const std::string_view prefix(s.data(),n);truncated|=n!=s.size();if(utf8(prefix))string(prefix);else{string("Native diagnostic contains invalid UTF-8; use the native API for exact bytes");truncated=true;}}
    text.append("],\"diagnosticsTruncated\":");boolean(truncated);text.push_back('}');std::cout.write(text.data(),std::streamsize(text.size()));std::cout.put('\n');
  }
};
} // namespace boundary_projection_cli

std::optional<int> runBoundaryProjectionCommand(int argc,char** argv){
  if(argc<2)return {};const std::string_view command=argv[1];const bool replay=command=="--reproject-boundaries";
  if(command!="--project-boundaries"&&!replay)return {};
  const int begin=replay?4:5;if(argc<begin)throw std::invalid_argument("boundary projection requires VELOCITY FLUID OUTPUT, or replay STATE OUTPUT, before options");
  for(int i=2;i<begin;++i)if(!argv[i][0]||std::string_view(argv[i]).starts_with("--")||std::string_view(argv[i])=="-h")throw std::invalid_argument("boundary projection requires nonempty positional paths; use ./-h for a literal filename");
  vdb::BoundaryProjectionOptions o;vdb::ProcessingOptions p;vdb::BoundaryProjectionReplay state;
  std::string velocityGrid,fluidGrid;std::array<std::string,6> optionalFiles,optionalGrids;
  constexpr const char* roles[]={"solid","open","air","phi","wall","pressure"};std::set<std::string> seen;
  const auto argument=[&](int& i,const std::string& flag){const auto s=value(argc,argv,i,flag);if(s.empty()||std::string_view(s).starts_with("--")||s=="-h")throw std::invalid_argument("missing nonempty value for "+flag);return s;};
  const auto bounded=[&](int& i,const std::string& flag,uint64_t cap){const auto n=count(argument(i,flag),flag);if(n>cap)throw std::invalid_argument("boundary projection count exceeds range: "+flag);return n;};
  const auto real=[&](int& i,const std::string& flag,double low,double high){const double n=number(argument(i,flag),flag);if(n<low||n>high)throw std::invalid_argument("boundary projection numeric value exceeds range: "+flag);return n;};
  for(int i=begin;i<argc;++i){const std::string arg=argv[i],key=(arg=="--grid"||arg=="--input-grid")?"--velocity-grid":arg;
    if(!seen.insert(key).second)throw std::invalid_argument("duplicate boundary projection option: "+arg);
    if(key=="--velocity-grid")velocityGrid=argument(i,arg);
    else if(arg=="--fluid-grid")fluidGrid=argument(i,arg);
    else if(arg=="--time-step")o.projection.timeStep=real(i,arg,1e-6,3600);
    else if(arg=="--acceleration")for(double& n:o.projection.acceleration)n=real(i,arg,-1e12,1e12);
    else if(arg=="--tolerance")o.projection.tolerance=real(i,arg,1e-12,1e-2);
    else if(arg=="--iterations")o.projection.maxIterations=uint32_t(bounded(i,arg,10000));
    else if(arg=="--max-fluid-cells")o.projection.maxFluidCells=bounded(i,arg,1'000'000);
    else if(arg=="--velocity-name")o.projection.velocityGridName=argument(i,arg);
    else if(arg=="--pressure-name")o.projection.pressureGridName=argument(i,arg);
    else if(arg=="--fluid-name")o.projection.domainGridName=argument(i,arg);
    else if(arg=="--face-kind-name")o.faceKindGridName=argument(i,arg);
    else if(arg=="--face-fraction-name")o.faceFractionGridName=argument(i,arg);
    else if(arg=="--face-pressure-name")o.facePressureGridName=argument(i,arg);
    else if(arg=="--wall-velocity-name")o.wallVelocityGridName=argument(i,arg);
    else if(arg=="--no-boundary-state")o.writeBoundaryState=false;
    else if(arg=="--no-domain")o.projection.writeDomain=false;
    else if(arg=="--max-boundary-work")o.maxWorkItems=bounded(i,arg,10'000'000'000ULL);
    else if(arg=="--max-boundary-grids")o.maxGrids=size_t(bounded(i,arg,4096));
    else if(arg=="--max-boundary-metadata")o.maxMetadataBytes=count(argument(i,arg),arg);
    else if(arg=="--max-voxels")p.maxVoxelCount=count(argument(i,arg),arg);
    else if(arg=="--max-input-bytes")p.maxInputBytes=count(argument(i,arg),arg);
    else if(replay){
      if(arg=="--face-kind-grid")state.faceKindGrid=argument(i,arg);
      else if(arg=="--face-fraction-grid")state.faceFractionGrid=argument(i,arg);
      else if(arg=="--face-pressure-grid")state.facePressureGrid=argument(i,arg);
      else if(arg=="--wall-velocity-grid")state.wallVelocityGrid=argument(i,arg);
      else throw std::invalid_argument("unknown/incompatible canonical boundary replay option: "+arg);
    }else if(arg=="--missing-boundary"){
      const auto s=argument(i,arg);if(s=="wall")o.missing=vdb::MissingFluidBoundary::ClosedWall;else if(s=="open")o.missing=vdb::MissingFluidBoundary::OpenFacePressure;else if(s=="air")o.missing=vdb::MissingFluidBoundary::AirCellPressure;else if(s=="surface")o.missing=vdb::MissingFluidBoundary::FreeSurfacePressure;else throw std::invalid_argument("--missing-boundary requires wall|open|air|surface");
    }else if(arg=="--air-pressure"){
      const auto s=argument(i,arg);if(s=="center")o.airPressure=vdb::AirPressureLocation::NeighborCenter;else if(s=="surface")o.airPressure=vdb::AirPressureLocation::LevelSetInterface;else throw std::invalid_argument("--air-pressure requires center|surface");
    }else if(arg=="--minimum-interface-fraction")o.minimumInterfaceFraction=real(i,arg,1e-6,1);
    else if(arg=="--wall-velocity")for(double& n:o.wallVelocityWorld)n=real(i,arg,-1e12,1e12);
    else if(arg=="--ambient-pressure")o.ambientPressure=number(argument(i,arg),arg);
    else{bool found=false;for(size_t n=0;n<6;++n){const std::string role=std::string("--boundary-")+roles[n];if(arg==role){optionalFiles[n]=argument(i,arg);found=true;break;}if(arg==role+"-grid"){optionalGrids[n]=argument(i,arg);found=true;break;}}if(!found)throw std::invalid_argument("unknown/incompatible boundary projection option: "+arg);}
  }
  std::set<std::string> names;for(const auto* n:{&o.projection.velocityGridName,&o.projection.pressureGridName,&o.projection.domainGridName,&o.faceKindGridName,&o.faceFractionGridName,&o.facePressureGridName,&o.wallVelocityGridName})if(n->empty()||n->size()>256||!names.insert(*n).second)throw std::invalid_argument("boundary projection seven output names must be distinct1..256byte names");
  if(o.writeBoundaryState&&!o.projection.writeDomain)throw std::invalid_argument("--no-domain requires --no-boundary-state");
  if(!o.projection.writeDomain&&seen.contains("--fluid-name"))throw std::invalid_argument("--fluid-name is irrelevant with --no-domain");
  if(!o.writeBoundaryState)for(const auto* flag:{"--face-kind-name","--face-fraction-name","--face-pressure-name","--wall-velocity-name"})if(seen.contains(flag))throw std::invalid_argument("boundary output name is irrelevant with --no-boundary-state");
  if(!replay){for(size_t n=0;n<6;++n)if(optionalFiles[n].empty()&&!optionalGrids[n].empty())throw std::invalid_argument("boundary grid selector requires its role file");
    if(!optionalFiles[4].empty()&&seen.contains("--wall-velocity"))throw std::invalid_argument("wall field and constant --wall-velocity are mutually exclusive");
    if(!optionalFiles[5].empty()&&seen.contains("--ambient-pressure"))throw std::invalid_argument("pressure field and --ambient-pressure are mutually exclusive");
    if(optionalFiles[2].empty()&&seen.contains("--air-pressure"))throw std::invalid_argument("--air-pressure requires --boundary-air");
    const bool surface=o.missing==vdb::MissingFluidBoundary::FreeSurfacePressure||(!optionalFiles[2].empty()&&o.airPressure==vdb::AirPressureLocation::LevelSetInterface);
    if(surface!=!optionalFiles[3].empty()||(!surface&&seen.contains("--minimum-interface-fraction")))throw std::invalid_argument("surface policy requires --boundary-phi; phi/fraction flags are irrelevant without a surface policy");
  }
  if(!vdb::IsAvailable()){
    if(replay)(void)vdb::WriteReprojectedBoundaryState({}, {},o,p);else(void)vdb::WriteBoundaryProjectedVelocity({}, {},o,p);return EXIT_FAILURE;
  }
  // Validate/encode every caller string and reserve the complete report before
  // native path conversion, input loading, or exclusive VDB publication.
  long double callerBytes=0;for(int i=1;i<argc;++i){const std::string_view s=argv[i];if(!boundary_projection_cli::utf8(s))throw std::invalid_argument("boundary projection CLI requires valid UTF-8 before output publication");callerBytes+=s.size();}
  if(callerBytes>256*1024||262144+32*callerBytes>=p.maxInputBytes)throw std::invalid_argument("boundary projection CLI report/options reservation exceeds --max-input-bytes");
  std::vector<std::string> caller;caller.reserve(size_t(argc));for(int i=1;i<argc;++i)caller.emplace_back(argv[i]);
  nlohmann::json bindings;if(replay){bindings={{"state",argv[2]},{"velocityGrid",velocityGrid.empty()?state.velocityGrid:velocityGrid},{"fluidGrid",fluidGrid.empty()?state.fluidGrid:fluidGrid},
      {"faceKindGrid",state.faceKindGrid},{"faceFractionGrid",state.faceFractionGrid},{"facePressureGrid",state.facePressureGrid},{"wallVelocityGrid",state.wallVelocityGrid}};
  }else{bindings={{"velocity",{{"file",argv[2]},{"grid",velocityGrid}}},{"fluid",{{"file",argv[3]},{"grid",fluidGrid}}}};for(size_t n=0;n<6;++n)bindings[roles[n]]=optionalFiles[n].empty()?nlohmann::json(nullptr):nlohmann::json{{"file",optionalFiles[n]},{"grid",optionalGrids[n]}};}
  boundary_projection_cli::Report report(argv[begin-1],caller,bindings,o,p,replay);auto native=p;native.maxInputBytes-=report.reservation;
  vdb::BoundaryProjectionResult result;if(replay){state.state=path(argv[2]);if(!velocityGrid.empty())state.velocityGrid=velocityGrid;if(!fluidGrid.empty())state.fluidGrid=fluidGrid;result=vdb::WriteReprojectedBoundaryState(state,path(argv[3]),o,native);}
  else{vdb::BoundaryProjectionInputs in;in.velocity={path(argv[2]),velocityGrid};in.fluid={path(argv[3]),fluidGrid};
    const std::array<vdb::BoundaryProjectionField*,6> rolesOut{&in.solid,&in.open,&in.air,&in.liquidPhi,&in.wallVelocity,&in.pressure};for(size_t n=0;n<6;++n)if(!optionalFiles[n].empty())*rolesOut[n]={path(optionalFiles[n]),optionalGrids[n]};result=vdb::WriteBoundaryProjectedVelocity(in,path(argv[4]),o,native);
  }
  report.write(result);return EXIT_SUCCESS;
}
