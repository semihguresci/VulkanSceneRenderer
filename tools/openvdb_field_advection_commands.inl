// Include after the common path/value/number/count parser helpers.
std::optional<int> runFieldAdvectionCommand(int argc,char** argv) {
  if(argc<2 || std::string_view(argv[1])!="--advect-field")return {};
  if(argc<5)throw std::invalid_argument("--advect-field requires INPUT VELOCITY OUTPUT before options");
  for(int i=2;i<5;++i)if(!argv[i][0] || std::string_view(argv[i]).starts_with("--") || std::string_view(argv[i])=="-h")
    throw std::invalid_argument("--advect-field requires INPUT VELOCITY OUTPUT before options");
  vdb::FieldAdvectionOptions o;vdb::ProcessingOptions p;
  std::string grid,velocityGrid,scheme="midpoint",limiter="clamp",layout="auto";
  std::set<std::string> seen;
  const auto argument=[&](int& i,const std::string& option) {
    const auto text=value(argc,argv,i,option);
    if(text.empty() || std::string_view(text).starts_with("--") || text=="-h")
      throw std::invalid_argument("missing nonempty value for "+option);
    return text;
  };
  for(int i=5;i<argc;++i) {
    const std::string arg=argv[i],key=arg=="--input-grid"?"--grid":arg;
    if(!seen.insert(key).second)throw std::invalid_argument("duplicate field-advection option: "+arg);
    if(arg=="--time") {
      o.timeStep=number(argument(i,arg),arg);
      if(std::abs(o.timeStep)>3600)throw std::invalid_argument("--time must be in [-3600,3600] seconds");
    } else if(arg=="--steps") {
      const auto n=count(argument(i,arg),arg);
      if(n>64)throw std::invalid_argument("--steps must be in [1,64]");o.steps=uint32_t(n);
    } else if(arg=="--advection-scheme") {
      scheme=argument(i,arg);
      if(scheme=="semi")o.scheme=vdb::FieldAdvectionScheme::Semi;
      else if(scheme=="midpoint")o.scheme=vdb::FieldAdvectionScheme::Midpoint;
      else if(scheme=="rk3")o.scheme=vdb::FieldAdvectionScheme::Rk3;
      else if(scheme=="rk4")o.scheme=vdb::FieldAdvectionScheme::Rk4;
      else if(scheme=="maccormack")o.scheme=vdb::FieldAdvectionScheme::MacCormack;
      else if(scheme=="bfecc")o.scheme=vdb::FieldAdvectionScheme::Bfecc;
      else throw std::invalid_argument("--advection-scheme requires semi, midpoint, rk3, rk4, maccormack or bfecc");
    } else if(arg=="--advection-limiter") {
      limiter=argument(i,arg);
      if(limiter=="none")o.limiter=vdb::FieldAdvectionLimiter::None;
      else if(limiter=="clamp")o.limiter=vdb::FieldAdvectionLimiter::Clamp;
      else if(limiter=="revert")o.limiter=vdb::FieldAdvectionLimiter::Revert;
      else throw std::invalid_argument("--advection-limiter requires none, clamp or revert");
    } else if(arg=="--velocity-layout") {
      layout=argument(i,arg);
      if(layout=="auto")o.velocityLayout=vdb::FieldVelocityLayout::Auto;
      else if(layout=="world")o.velocityLayout=vdb::FieldVelocityLayout::CollocatedWorld;
      else if(layout=="mac-index")o.velocityLayout=vdb::FieldVelocityLayout::NativeMacIndex;
      else throw std::invalid_argument("--velocity-layout requires auto, world or mac-index");
    } else if(arg=="--advection-cfl") {
      o.maxCfl=number(argument(i,arg),arg);
      if(!(o.maxCfl>0) || o.maxCfl>16)throw std::invalid_argument("--advection-cfl must be in (0,16]");
    } else if(arg=="--advection-mask")o.mask=path(argument(i,arg));
    else if(arg=="--advection-mask-grid")o.maskGrid=argument(i,arg);
    else if(arg=="--advection-voxelize-tiles")o.voxelizeActiveTiles=true;
    else if(arg=="--max-advection-work") {
      o.maxWorkItems=count(argument(i,arg),arg);
      if(o.maxWorkItems>10'000'000'000ull)throw std::invalid_argument("--max-advection-work must be <=10000000000");
    } else if(key=="--grid")grid=argument(i,arg);
    else if(arg=="--velocity-grid")velocityGrid=argument(i,arg);
    else if(arg=="--grid-name")p.gridName=argument(i,arg);
    else if(arg=="--max-voxels")p.maxVoxelCount=count(argument(i,arg),arg);
    else if(arg=="--max-input-bytes")p.maxInputBytes=count(argument(i,arg),arg);
    else throw std::invalid_argument("unknown/incompatible field-advection option: "+arg);
  }
  const bool correction=o.scheme==vdb::FieldAdvectionScheme::MacCormack || o.scheme==vdb::FieldAdvectionScheme::Bfecc;
  if((seen.contains("--advection-limiter") && !correction) ||
      (seen.contains("--advection-mask-grid") && o.mask.empty()))
    throw std::invalid_argument("field-advection limiter requires maccormack/bfecc; mask grid requires --advection-mask");
  const auto r=vdb::WriteAdvectedField(path(argv[2]),path(argv[3]),path(argv[4]),o,grid,velocityGrid,p);
  std::cout<<nlohmann::json{{"schemaVersion",1},{"operation","field-advection"},
      {"gridName",r.grid.gridName},{"gridClass",r.grid.gridClass},{"voxelSize",r.grid.voxelSize},
      {"activeVoxelCount",r.grid.activeVoxelCount},{"inputActiveVoxelCount",r.inputActiveVoxelCount},
      {"updatedVoxelCount",r.updatedVoxelCount},{"workItems",r.workItems},
      {"scheme",scheme},{"limiter",correction?nlohmann::json(limiter):nlohmann::json(nullptr)},
      {"requestedVelocityLayout",layout},{"timeSeconds",o.timeStep},{"requestedSteps",o.steps},
      {"executedSteps",r.executedSteps},{"maximumWorldSpeed",r.maximumWorldSpeed},
      {"maximumStepDisplacement",r.maximumStepDisplacement},
      {"overshootVoxelCount",r.overshootVoxelCount},{"clampedVoxelCount",r.clampedVoxelCount},
      {"revertedVoxelCount",r.revertedVoxelCount},{"roundedTraceComponentCount",r.roundedTraceComponentCount},
      {"maximumTraceRounding",r.maximumTraceRounding},{"inputMinimum",r.inputMinimum},
      {"inputMaximum",r.inputMaximum},{"outputMinimum",r.outputMinimum},{"outputMaximum",r.outputMaximum},
      {"maximumRoundTripCorrection",r.maximumRoundTripCorrection},{"noOp",r.noOp},
      {"maskUsed",r.maskUsed},{"fogClassCleared",r.fogClassCleared},{"diagnostics",r.diagnostics}}.dump(2)<<'\n';
  return EXIT_SUCCESS;
}
