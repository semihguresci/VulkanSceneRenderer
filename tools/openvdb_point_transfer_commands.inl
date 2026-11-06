// Requires OpenVdbPointTransfer.h, the runtime IsAvailable declaration,
// <charconv> at translation-unit scope and common value/number/count/path helpers.
// Include with the other operation adapters; dispatch runPointTransferCommand
// before the legacy parser. Full help is in point-transfer/usage-draft.md.
namespace point_transfer_cli {
bool utf8(std::string_view text) noexcept {
  for(size_t i=0;i<text.size();) {
    const auto a=static_cast<unsigned char>(text[i++]);if(a<0x80)continue;
    size_t remaining;unsigned char low=0x80,high=0xbf;
    if(a>=0xc2 && a<=0xdf)remaining=1;
    else if(a>=0xe0 && a<=0xef){remaining=2;if(a==0xe0)low=0xa0;if(a==0xed)high=0x9f;}
    else if(a>=0xf0 && a<=0xf4){remaining=3;if(a==0xf0)low=0x90;if(a==0xf4)high=0x8f;}
    else return false;
    if(remaining>text.size()-i)return false;
    const auto first=static_cast<unsigned char>(text[i++]);if(first<low || first>high)return false;
    while(--remaining){const auto c=static_cast<unsigned char>(text[i++]);if(c<0x80 || c>0xbf)return false;}
  }
  return true;
}
// All caller text is encoded before native publication. The final report uses
// only this reserved buffer, stack numeric conversion and bounded diagnostics;
// it does not construct a JSON DOM or dump string after creating the VDB.
struct Report {
  std::string output,attribute,field,companion,matrix,text;
  uintmax_t reservation=0;
  Report(const char* input,const char* destination,const char* channel,
         const std::string& selector,const vdb::PointTransferOptions& o,
         const vdb::PointAttributeOptions&,const vdb::ProcessingOptions& p) {
    long double bytes=0;
    const auto charge=[&](std::string_view s){
      if(!utf8(s))throw std::invalid_argument("point-transfer CLI requires valid UTF-8 text before output publication");
      bytes+=s.size();
    };
    for(const auto s:{std::string_view(input),std::string_view(destination),std::string_view(channel),
        std::string_view(selector),std::string_view(p.gridName),std::string_view(o.companionGridName),std::string_view(o.weightAttribute)})charge(s);
    for(const auto* groups:{&o.groups.anyOf,&o.groups.allOf,&o.groups.noneOf})for(const auto& s:*groups)charge(s);
    const long double reserve=262144.0L+32*bytes;
    if(reserve>=p.maxInputBytes || reserve>std::numeric_limits<size_t>::max())
      throw std::invalid_argument("point-transfer CLI report/options reserve exceeds --max-input-bytes");
    reservation=uintmax_t(reserve);
    const auto quote=[](std::string_view s){return nlohmann::json(s).dump();};
    output=quote(destination);attribute=quote(channel);field=quote(p.gridName);
    companion=quote(o.companionGridName.empty()?p.gridName+
      (o.companion==vdb::PointTransferCompanion::WeightSum?"_weight":"_density"):o.companionGridName);
    matrix=nlohmann::json(o.targetIndexToWorld).dump();
    text.reserve(size_t(65536.0L+8*bytes));
  }
  template<class T>void numeric(T value) {
    std::array<char,128> buffer;
    if constexpr(std::is_floating_point_v<T>) {
      const auto result=std::to_chars(buffer.data(),buffer.data()+buffer.size(),value,
        std::chars_format::general,std::numeric_limits<T>::max_digits10);
      text.append(buffer.data(),size_t(result.ptr-buffer.data()));
    }else{
      const auto result=std::to_chars(buffer.data(),buffer.data()+buffer.size(),value);
      text.append(buffer.data(),size_t(result.ptr-buffer.data()));
    }
  }
  void boolean(bool value){text.append(value?"true":"false");}
  void string(std::string_view value) {
    constexpr char digits[]="0123456789abcdef";text.push_back('"');
    for(const unsigned char c:value) {
      if(c=='"' || c=='\\'){text.push_back('\\');text.push_back(char(c));}
      else if(c<0x20){text.append("\\u00");text.push_back(digits[c>>4]);text.push_back(digits[c&15]);}
      else text.push_back(char(c));
    }
    text.push_back('"');
  }
  void write(const vdb::PointTransferResult& r,const vdb::PointTransferOptions& o) {
    text.append("{\"schemaVersion\":1,\"operation\":\"transfer-point-attribute\",\"output\":");text.append(output);
    text.append(",\"attribute\":");text.append(attribute);
    text.append(",\"mode\":\"");text.append(o.mode==vdb::PointTransferMode::WeightedMean?"mean":"sum");
    text.append("\",\"storage\":\"");text.append(o.storage==vdb::PointTransferStorage::Double?"double":"float");
    text.append("\",\"companionMode\":\"");text.append(o.companion==vdb::PointTransferCompanion::WeightSum?"weight":"density");
    text.append("\",\"integerPolicy\":\"");text.append(o.integers==vdb::PointTransferIntegers::RejectInexact?"exact":"allow-rounded");
    text.append("\",\"targetIndexToWorld\":");text.append(matrix);
    const auto grid=[&](const char* key,const std::string& name,const vdb::ProcessingResult& result,bool weight) {
      text.append(key);text.append("{\"gridName\":");text.append(name);
      text.append(",\"gridClass\":\"unknown\",\"valueType\":\"");
      text.append(weight?"double":r.componentCount==1?(o.storage==vdb::PointTransferStorage::Double?"double":"float"):
        (o.storage==vdb::PointTransferStorage::Double?"vec3d":"vec3s"));
      text.append("\",\"activeVoxelCount\":");numeric(result.activeVoxelCount);
      text.append(",\"voxelSize\":");numeric(result.voxelSize);text.push_back('}');
    };
    grid(",\"field\":",field,r.field,false);grid(",\"companion\":",companion,r.companion,true);
    text.append(",\"inputCount\":");numeric(r.inputCount);
    text.append(",\"selectedCount\":");numeric(r.selectedCount);
    text.append(",\"selectedInactiveCount\":");numeric(r.selectedInactiveCount);
    text.append(",\"contributionCount\":");numeric(r.contributionCount);
    text.append(",\"roundedIntegerComponents\":");numeric(r.roundedIntegerComponents);
    text.append(",\"roundedFloatComponents\":");numeric(r.roundedFloatComponents);
    text.append(",\"workUpperBound\":");numeric(r.workUpperBound);
    text.append(",\"componentCount\":");numeric(r.componentCount);
    text.append(",\"voxelVolume\":");numeric(r.voxelVolume);
    text.append(",\"totalMass\":");numeric(r.totalMass);
    text.append(",\"depositedMass\":");numeric(r.depositedMass);
    text.append(",\"massErrorUpperBound\":");numeric(r.massErrorUpperBound);
    text.append(",\"maxFloatAbsoluteError\":");numeric(r.maxFloatAbsoluteError);
    text.append(",\"minimum\":[");
    for(size_t c=0;c<3;++c){if(c)text.push_back(',');numeric(r.minimum[c]);}
    text.append("],\"maximum\":[");
    for(size_t c=0;c<3;++c){if(c)text.push_back(',');numeric(r.maximum[c]);}
    text.append("],\"hasValues\":");boolean(r.hasValues);
    text.append(",\"archivedSchema\":");boolean(r.archivedSchema);
    text.append(",\"payloadSemantics\":\"Stored scalar/vector components receive no implicit frame or unit transformation\"");
    text.append(",\"nativeDiagnosticCount\":");numeric(r.diagnostics.size());
    text.append(",\"diagnostics\":[");bool truncated=r.diagnostics.size()>8;
    for(size_t i=0;i<std::min<size_t>(8,r.diagnostics.size());++i) {
      if(i)text.push_back(',');const auto& source=r.diagnostics[i];
      size_t length=std::min<size_t>(1024,source.size());
      if(length<source.size())while(length && (static_cast<unsigned char>(source[length])&0xc0)==0x80)--length;
      const std::string_view part(source.data(),length);truncated|=length!=source.size();
      if(utf8(part))string(part);
      else{string("Native diagnostic contains invalid UTF-8; inspect through the native API for exact bytes");truncated=true;}
    }
    text.append("],\"diagnosticsTruncated\":");boolean(truncated);text.push_back('}');
    std::cout.write(text.data(),std::streamsize(text.size()));std::cout.put('\n');
  }
};
} // namespace point_transfer_cli

std::optional<int> runPointTransferCommand(int argc,char** argv) {
  if(argc<2 || std::string_view(argv[1])!="--transfer-point-attribute")return {};
  if(argc<5)throw std::invalid_argument("--transfer-point-attribute requires INPUT ATTRIBUTE OUTPUT before options");
  for(int i=2;i<5;++i)if(!argv[i][0] || std::string_view(argv[i]).starts_with("--") || std::string_view(argv[i])=="-h")
    throw std::invalid_argument("point transfer requires nonempty INPUT ATTRIBUTE OUTPUT; use ./-h for a literal filename");
  vdb::PointTransferOptions o;vdb::PointAttributeOptions attributes;vdb::ProcessingOptions p;p.gridName="field";
  std::string selector;std::set<std::string> seen;std::array<std::set<std::string>,3> groups;
  const auto argument=[&](int& i,const std::string& option) {
    const auto text=value(argc,argv,i,option);
    if(text.empty() || std::string_view(text).starts_with("--") || text=="-h")throw std::invalid_argument("missing nonempty value for "+option);
    return text;
  };
  const auto positive=[&](int& i,const std::string& option,uint64_t maximum) {
    const auto n=count(argument(i,option),option);
    if(n>maximum)throw std::invalid_argument("point transfer count exceeds range: "+option);return n;
  };
  for(int i=5;i<argc;++i) {
    const std::string arg=argv[i],key=arg=="--input-grid"?"--grid":arg;
    if(arg=="--any-group" || arg=="--all-group" || arg=="--exclude-group") {
      const auto name=argument(i,arg);const size_t n=arg=="--any-group"?0:arg=="--all-group"?1:2;
      if(groups[n].size()>=4096 || !groups[n].insert(name).second)throw std::invalid_argument("duplicate/excessive point-transfer group predicate: "+arg);
      (n==0?o.groups.anyOf:n==1?o.groups.allOf:o.groups.noneOf).push_back(name);continue;
    }
    if(!seen.insert(key).second)throw std::invalid_argument("duplicate point-transfer option: "+arg);
    if(arg=="--transfer-mode") {
      const auto text=argument(i,arg);
      if(text=="mean")o.mode=vdb::PointTransferMode::WeightedMean;
      else if(text=="sum")o.mode=vdb::PointTransferMode::WeightedSum;
      else throw std::invalid_argument("--transfer-mode requires mean or sum");
    }else if(arg=="--transfer-storage") {
      const auto text=argument(i,arg);
      if(text=="double")o.storage=vdb::PointTransferStorage::Double;
      else if(text=="float")o.storage=vdb::PointTransferStorage::Float;
      else throw std::invalid_argument("--transfer-storage requires double or float");
    }else if(arg=="--transfer-companion") {
      const auto text=argument(i,arg);
      if(text=="weight")o.companion=vdb::PointTransferCompanion::WeightSum;
      else if(text=="density")o.companion=vdb::PointTransferCompanion::WorldDensity;
      else throw std::invalid_argument("--transfer-companion requires weight or density");
    }else if(arg=="--transfer-integers") {
      const auto text=argument(i,arg);
      if(text=="exact")o.integers=vdb::PointTransferIntegers::RejectInexact;
      else if(text=="allow-rounded")o.integers=vdb::PointTransferIntegers::AllowRounded;
      else throw std::invalid_argument("--transfer-integers requires exact or allow-rounded");
    }else if(arg=="--transfer-target-transform") {
      for(auto& x:o.targetIndexToWorld)x=number(argument(i,arg),arg);
      const auto& m=o.targetIndexToWorld;
      if(m[12]!=0 || m[13]!=0 || m[14]!=0 || m[15]!=1)throw std::invalid_argument("target transform requires16 COLUMN affine entries; translation at3,7,11 and final row0,0,0,1");
    }else if(arg=="--transfer-companion-name")o.companionGridName=argument(i,arg);
    else if(arg=="--transfer-weight-attribute")o.weightAttribute=argument(i,arg);
    else if(arg=="--transfer-mass") {
      o.constantMass=number(argument(i,arg),arg);
      if(!(o.constantMass>0))throw std::invalid_argument("--transfer-mass requires a positive finite mass");
    }else if(arg=="--max-point-transfer-work")o.maxWork=positive(i,arg,10'000'000'000ULL);
    else if(arg=="--max-point-transfer-grids")o.maxGrids=size_t(positive(i,arg,4096));
    else if(arg=="--max-point-transfer-metadata")o.maxMetadataBytes=count(argument(i,arg),arg);
    else if(arg=="--max-points")attributes.maxPoints=size_t(positive(i,arg,uint64_t(INT64_MAX)));
    else if(arg=="--max-attributes")attributes.maxAttributes=size_t(positive(i,arg,1024));
    else if(arg=="--max-groups")attributes.maxGroups=size_t(positive(i,arg,4096));
    else if(arg=="--max-string-bytes")attributes.maxStringBytes=count(argument(i,arg),arg);
    else if(key=="--grid")selector=argument(i,arg);
    else if(arg=="--grid-name")p.gridName=argument(i,arg);
    else if(arg=="--max-voxels")p.maxVoxelCount=count(argument(i,arg),arg);
    else if(arg=="--max-input-bytes")p.maxInputBytes=count(argument(i,arg),arg);
    else throw std::invalid_argument("unknown/incompatible point-transfer option: "+arg);
  }
  if(seen.contains("--transfer-weight-attribute") && seen.contains("--transfer-mass"))
    throw std::invalid_argument("--transfer-weight-attribute and --transfer-mass are mutually exclusive");
  if(!vdb::IsAvailable()) {
    // Keep strict syntax checks above, then let the native OFF stub take
    // precedence over UTF-8/report budgets and filesystem path conversion.
    (void)vdb::WriteTransferredPointAttribute({}, {}, {}, o, {}, p, attributes);
    return EXIT_FAILURE;
  }
  point_transfer_cli::Report report(argv[2],argv[4],argv[3],selector,o,attributes,p);
  auto native=p;native.maxInputBytes-=report.reservation;
  const auto result=vdb::WriteTransferredPointAttribute(path(argv[2]),path(argv[4]),argv[3],o,selector,native,attributes);
  report.write(result,o);return EXIT_SUCCESS;
}
