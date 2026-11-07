// Include after OpenVdbNativePointAdvection.h, runtime IsAvailable(), and the
// common value/number/count/path helpers. Requires <charconv>/<type_traits>.
// Dispatch runNativePointAdvectionCommand before the legacy parser.
// Help: --advect-native-points POINTS.vdb VELOCITY.vdb OUTPUT.vdb
//   --point-integration rk2|rk4 --time-step DT --steps N
//   --velocity-layout auto|world|native-mac --velocity-grid NAME
//   --grid/--input-grid/--point-grid NAME --grid-name NAME
//   --point-target-transform M00 ... M33   COLUMN affine target,16 entries
//   --point-position-error X --allow-point-activity-merge
//   --max-point-step-travel X --max-point-travel X (VELOCITY-index L2 voxels)
//   --any-group/--all-group/--exclude-group NAME (repeatable distinct names)
//   --max-point-advection-work/grids/metadata N
//   --max-points/attributes/groups/string-bytes --max-voxels/input-bytes N
// Only P changes. Signed total time is divided into equal steps. v/pscale and
// every non-P payload remain unchanged; native order may change.
namespace native_point_advection_cli {
bool utf8(std::string_view text) noexcept {
  for (size_t i=0;i<text.size();) {
    const auto c=static_cast<unsigned char>(text[i++]);if(c<0x80)continue;
    size_t n;unsigned char low=0x80,high=0xbf;
    if(c>=0xc2&&c<=0xdf)n=1;
    else if(c>=0xe0&&c<=0xef){n=2;if(c==0xe0)low=0xa0;if(c==0xed)high=0x9f;}
    else if(c>=0xf0&&c<=0xf4){n=3;if(c==0xf0)low=0x90;if(c==0xf4)high=0x8f;}
    else return false;
    if(n>text.size()-i)return false;
    const auto first=static_cast<unsigned char>(text[i++]);if(first<low||first>high)return false;
    while(--n){const auto next=static_cast<unsigned char>(text[i++]);if(next<0x80||next>0xbf)return false;}
  }
  return true;
}

struct Report {
  std::string text;
  uintmax_t reservation;
  uint64_t formattingWork;
  template<class T> void numeric(T value) {
    std::array<char,128> bytes;std::to_chars_result converted;
    if constexpr(std::is_floating_point_v<T>)
      converted=std::to_chars(bytes.data(),bytes.data()+bytes.size(),value,std::chars_format::general,std::numeric_limits<T>::max_digits10);
    else converted=std::to_chars(bytes.data(),bytes.data()+bytes.size(),value);
    text.append(bytes.data(),size_t(converted.ptr-bytes.data()));
  }
  void boolean(bool value){text.append(value?"true":"false");}
  void string(std::string_view value) {
    constexpr char hex[]="0123456789abcdef";text.push_back('"');
    for(unsigned char c:value) {
      if(c=='"'||c=='\\'){text.push_back('\\');text.push_back(char(c));}
      else if(c<0x20){text.append("\\u00");text.push_back(hex[c>>4]);text.push_back(hex[c&15]);}
      else text.push_back(char(c));
    }
    text.push_back('"');
  }
  template<class T> void field(const char* key,T value) {
    text.append(",\"");text.append(key);text.append("\":");
    if constexpr(std::is_same_v<T,bool>)boolean(value);else numeric(value);
  }
  void stringField(const char* key,std::string_view value) {
    text.append(",\"");text.append(key);text.append("\":");string(value);
  }
  void array(const std::vector<std::string>& values) {
    text.push_back('[');bool first=true;for(const auto& value:values){if(!first)text.push_back(',');first=false;string(value);}text.push_back(']');
  }
  Report(int argc,char** argv,const vdb::NativePointAdvectionOptions& options,
         const vdb::ProcessingOptions& processing,const std::string& pointGrid,
         const std::string& velocityGrid,long double callerBytes) {
    const long double bytes=262144+32*callerBytes;
    if(bytes>=processing.maxInputBytes||bytes>std::numeric_limits<size_t>::max())
      throw std::invalid_argument("native point advection report/options reservation exceeds --max-input-bytes");
    text.reserve(size_t(bytes));
    // Charge actual capacity (including the terminator), plus owned parser,
    // option/group/path copies and conservative display-formatting workspace.
    const long double retained=text.capacity()+1.0L+65536+32*callerBytes;
    const long double work=4*retained;
    if(retained>=processing.maxInputBytes||work>=options.maxWork)
      throw std::invalid_argument("native point advection actual report/options/formatting reserve exceeds shared byte/work caps");
    reservation=uintmax_t(retained);formattingWork=uint64_t(work);
    for(int i=1;i<argc;++i)if(!utf8(argv[i]))
      throw std::invalid_argument("native point advection caller text must be valid UTF-8 before publication");
    text.append("{\"schemaVersion\":1,\"operation\":\"advect-native-points\",\"points\":");string(argv[2]);
    stringField("velocity",argv[3]);stringField("output",argv[4]);stringField("inputGrid",pointGrid);
    stringField("velocityGrid",velocityGrid);stringField("gridName",processing.gridName);
    text.append(",\"controls\":{\"timeStep\":");numeric(options.timeStep);field("steps",options.steps);
    stringField("integration",options.integration==vdb::NativePointIntegration::RK2?"rk2":"rk4");
    stringField("velocityLayout",options.velocityLayout==vdb::NativePointVelocityLayout::Auto?"auto":
        options.velocityLayout==vdb::NativePointVelocityLayout::World?"world":"native-mac");
    field("maxStepTravelVelocityVoxels",options.maxStepTravelVelocityVoxels);
    field("maxTravelVelocityVoxels",options.maxTravelVelocityVoxels);
    field("maxPositionErrorVoxels",options.maxPositionErrorVoxels);
    field("rejectActivityMerges",options.rejectActivityMerges);
    text.append(",\"targetIndexToWorld\":");
    if(options.targetIndexToWorld){text.push_back('[');for(size_t i=0;i<16;++i){if(i)text.push_back(',');numeric((*options.targetIndexToWorld)[i]);}text.push_back(']');}
    else text.append("null");
    text.append(",\"groups\":{\"anyOf\":");array(options.groups.anyOf);
    text.append(",\"allOf\":");array(options.groups.allOf);text.append(",\"noneOf\":");array(options.groups.noneOf);text.append("}}");
  }
  void nativeName(std::string_view value) {
    const bool valid=utf8(value);const bool truncated=value.size()>4096;
    text.append(",\"velocityGridName\":");if(valid&&!truncated)string(value);else text.append("null");
    field("velocityGridNameValidUtf8",valid);field("velocityGridNameByteCount",value.size());field("velocityGridNameTruncated",truncated);
    text.append(",\"velocityGridNameBytesHex\":");
    if(valid&&!truncated)text.append("null");
    else {constexpr char hex[]="0123456789abcdef";text.push_back('"');for(size_t i=0;i<std::min<size_t>(4096,value.size());++i){const auto c=static_cast<unsigned char>(value[i]);text.push_back(hex[c>>4]);text.push_back(hex[c&15]);}text.push_back('"');}
  }
  void diagnostics(const char* key,const std::vector<std::string>& values) {
    text.append(",\"");text.append(key);text.append("\":[");bool truncated=values.size()>8;
    for(size_t i=0;i<std::min<size_t>(8,values.size());++i) {
      if(i)text.push_back(',');const auto& value=values[i];size_t n=std::min<size_t>(1024,value.size());
      if(n<value.size())while(n&&(static_cast<unsigned char>(value[n])&0xc0)==0x80)--n;
      const std::string_view prefix(value.data(),n);truncated|=n!=value.size();
      if(utf8(prefix))string(prefix);else {string("Invalid UTF-8 native diagnostic; inspect native API bytes");truncated=true;}
    }
    text.push_back(']');text.append(",\"");text.append(key);text.append("Truncated\":");boolean(truncated);
  }
  void write(const vdb::NativePointAdvectionResult& result) {
    const auto& motion=result.motion;
    field("inputCount",motion.inputCount);field("selectedCount",motion.selectedCount);field("outputCount",motion.outputCount);
    field("inputActiveCount",motion.inputActiveCount);field("outputActiveCount",motion.outputActiveCount);
    field("inactiveRecordsActivated",motion.inactiveRecordsActivated);field("outputVoxelCount",motion.outputVoxelCount);
    field("nativeWorkUpperBound",motion.workUpperBound);field("formattingWorkUpperBound",formattingWork);
    field("workUpperBound",motion.workUpperBound+formattingWork);field("maxPositionErrorVoxels",motion.maxPositionErrorVoxels);
    field("maxPositionErrorWorld",motion.maxPositionErrorWorld);field("maxUnselectedPositionErrorVoxels",motion.maxUnselectedPositionErrorVoxels);
    field("noOp",motion.noOp);field("archivedSchema",motion.archivedSchema);field("integrationSkipped",result.integrationSkipped);
    field("executedSteps",result.executedSteps);field("velocitySampleCount",result.velocitySampleCount);
    field("maximumWorldSpeedBound",result.maximumWorldSpeedBound);
    field("maximumStepTravelVelocityVoxels",result.maximumStepTravelVelocityVoxels);
    field("maximumTravelVelocityVoxels",result.maximumTravelVelocityVoxels);field("maximumWorldTravel",result.maximumWorldTravel);
    field("maximumEndpointArithmeticEnvelopeWorld",result.maximumEndpointArithmeticEnvelopeWorld);
    nativeName(result.velocityGridName);stringField("velocityValueType",result.velocityValueType);
    stringField("velocitySemantics",result.velocitySemantics);
    text.append(",\"schema\":{\"pointCount\":");numeric(motion.schema.pointCount);
    field("leafCount",motion.schema.leafCount);field("attributeCount",motion.schema.attributes.size());field("groupCount",motion.schema.groups.size());text.push_back('}');
    stringField("payloadSemantics","Only P changes; v, pscale and all non-P storage are preserved. Native record order may change.");
    stringField("errorSemantics","Codec errors compare to computed RK endpoints; no ODE accuracy, mass conservation or incompressibility certificate.");
    diagnostics("motionDiagnostics",motion.diagnostics);diagnostics("diagnostics",result.diagnostics);
    text.push_back('}');std::cout.write(text.data(),std::streamsize(text.size()));std::cout.put('\n');
  }
};
} // namespace native_point_advection_cli

std::optional<int> runNativePointAdvectionCommand(int argc,char** argv) {
  if(argc<2||std::string_view(argv[1])!="--advect-native-points")return {};
  if(!vdb::IsAvailable())throw std::runtime_error("OpenVDB native point advection is unavailable; configure with -DENABLE_OPENVDB=ON.");
  if(argc<5)throw std::invalid_argument("--advect-native-points requires POINTS VELOCITY OUTPUT before options");
  for(int i=2;i<5;++i)if(!argv[i][0]||std::string_view(argv[i]).starts_with("--")||std::string_view(argv[i])=="-h")
    throw std::invalid_argument("--advect-native-points requires nonempty paths before options; use ./-h for a literal filename");
  long double callerBytes=0;for(int i=1;i<argc;++i)callerBytes+=std::string_view(argv[i]).size();
  if(callerBytes>256*1024)throw std::invalid_argument("native point advection caller text exceeds256KiB");
  vdb::NativePointAdvectionOptions options;vdb::ProcessingOptions processing;processing.gridName="points";
  vdb::PointAttributeOptions attributes;std::string pointGrid,velocityGrid;
  std::set<std::string> seen;std::array<std::set<std::string>,3> groupNames;
  const auto argument=[&](int& i,const std::string& option) {
    const auto result=value(argc,argv,i,option);
    if(result.empty()||std::string_view(result).starts_with("--")||result=="-h")throw std::invalid_argument("missing nonempty value for "+option);
    return result;
  };
  const auto bounded=[&](int& i,const std::string& option,uint64_t maximum) {
    const auto result=count(argument(i,option),option);if(result>maximum)throw std::invalid_argument("native point advection count exceeds range: "+option);return result;
  };
  const auto positive=[&](int& i,const std::string& option,double maximum) {
    const double result=number(argument(i,option),option);if(!(result>0)||result>maximum)throw std::invalid_argument("native point advection positive value exceeds range: "+option);return result;
  };
  for(int i=5;i<argc;++i) {
    const std::string option=argv[i],key=(option=="--grid"||option=="--input-grid")?"--point-grid":option;
    if(option=="--any-group"||option=="--all-group"||option=="--exclude-group") {
      const auto name=argument(i,option);const size_t group=option=="--any-group"?0:option=="--all-group"?1:2;
      if(groupNames[group].size()>=4096||!groupNames[group].insert(name).second)throw std::invalid_argument("duplicate/excessive native point group predicate: "+option);
      (group==0?options.groups.anyOf:group==1?options.groups.allOf:options.groups.noneOf).push_back(name);continue;
    }
    if(!seen.insert(key).second)throw std::invalid_argument("duplicate native point advection option: "+option);
    if(key=="--point-grid")pointGrid=argument(i,option);
    else if(option=="--velocity-grid")velocityGrid=argument(i,option);
    else if(option=="--grid-name")processing.gridName=argument(i,option);
    else if(option=="--point-integration") {
      const auto mode=argument(i,option);if(mode=="rk2")options.integration=vdb::NativePointIntegration::RK2;
      else if(mode=="rk4")options.integration=vdb::NativePointIntegration::RK4;
      else throw std::invalid_argument("--point-integration requires rk2|rk4");
    } else if(option=="--velocity-layout") {
      const auto mode=argument(i,option);if(mode=="auto")options.velocityLayout=vdb::NativePointVelocityLayout::Auto;
      else if(mode=="world")options.velocityLayout=vdb::NativePointVelocityLayout::World;
      else if(mode=="native-mac")options.velocityLayout=vdb::NativePointVelocityLayout::NativeMacIndex;
      else throw std::invalid_argument("--velocity-layout requires auto|world|native-mac");
    } else if(option=="--time-step") {
      options.timeStep=number(argument(i,option),option);if(std::abs(options.timeStep)>3600)throw std::invalid_argument("native point time-step must lie in[-3600,3600]");
    } else if(option=="--steps")options.steps=uint32_t(bounded(i,option,64));
    else if(option=="--point-position-error")options.maxPositionErrorVoxels=positive(i,option,.25);
    else if(option=="--allow-point-activity-merge")options.rejectActivityMerges=false;
    else if(option=="--max-point-step-travel")options.maxStepTravelVelocityVoxels=positive(i,option,16);
    else if(option=="--max-point-travel")options.maxTravelVelocityVoxels=positive(i,option,256);
    else if(option=="--max-point-advection-work")options.maxWork=bounded(i,option,10'000'000'000ULL);
    else if(option=="--max-point-advection-grids")options.maxGrids=size_t(bounded(i,option,4096));
    else if(option=="--max-point-advection-metadata")options.maxMetadataBytes=count(argument(i,option),option);
    else if(option=="--max-points")attributes.maxPoints=size_t(bounded(i,option,uint64_t(INT64_MAX)));
    else if(option=="--max-attributes")attributes.maxAttributes=size_t(bounded(i,option,1024));
    else if(option=="--max-groups")attributes.maxGroups=size_t(bounded(i,option,4096));
    else if(option=="--max-string-bytes")attributes.maxStringBytes=count(argument(i,option),option);
    else if(option=="--max-voxels")processing.maxVoxelCount=count(argument(i,option),option);
    else if(option=="--max-input-bytes")processing.maxInputBytes=count(argument(i,option),option);
    else if(option=="--point-target-transform") {
      std::array<double,16> matrix;for(auto& entry:matrix)entry=number(argument(i,option),option);
      if(matrix[12]!=0||matrix[13]!=0||matrix[14]!=0||matrix[15]!=1)throw std::invalid_argument("point target requires16 COLUMN affine entries, translation3/7/11 and last row0,0,0,1");
      options.targetIndexToWorld=matrix;
    } else throw std::invalid_argument("unknown/incompatible native point advection option: "+option);
  }
  native_point_advection_cli::Report report(argc,argv,options,processing,pointGrid,velocityGrid,callerBytes);
  auto nativeProcessing=processing;nativeProcessing.maxInputBytes-=report.reservation;
  auto nativeOptions=options;nativeOptions.maxWork-=report.formattingWork;
  const auto result=vdb::WriteAdvectedNativePoints(path(argv[2]),path(argv[3]),path(argv[4]),nativeOptions,pointGrid,velocityGrid,nativeProcessing,attributes);
  report.write(result);return EXIT_SUCCESS;
}
