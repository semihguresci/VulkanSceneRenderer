// Requires OpenVdbRegionQueries.h, runtime IsAvailable(), <charconv>,
// <type_traits>, and common value/path helpers. Dispatch
// runRegionQueriesCommand before the legacy parser. Full usage and schema:
// out/openvdb-review/region-queries/usage-draft.md.
namespace region_queries_cli {
bool utf8(std::string_view s) noexcept {
  for(size_t i=0;i<s.size();){const auto c=static_cast<unsigned char>(s[i++]);if(c<0x80)continue;
    size_t n;unsigned char low=0x80,high=0xbf;
    if(c>=0xc2&&c<=0xdf)n=1;else if(c>=0xe0&&c<=0xef){n=2;if(c==0xe0)low=0xa0;if(c==0xed)high=0x9f;}
    else if(c>=0xf0&&c<=0xf4){n=3;if(c==0xf0)low=0x90;if(c==0xf4)high=0x8f;}else return false;
    if(n>s.size()-i)return false;const auto first=static_cast<unsigned char>(s[i++]);if(first<low||first>high)return false;
    while(--n){const auto d=static_cast<unsigned char>(s[i++]);if(d<0x80||d>0xbf)return false;}
  }return true;
}
uint64_t integer(std::string_view s,std::string_view flag,uint64_t maximum,bool zero=false){
  if(s.empty()||s.find_first_not_of("0123456789")!=std::string_view::npos)throw std::invalid_argument("unsigned decimal required for "+std::string(flag));
  uint64_t n{};const auto r=std::from_chars(s.data(),s.data()+s.size(),n);
  if(r.ec!=std::errc{}||r.ptr!=s.data()+s.size()||(!zero&&!n)||n>maximum)throw std::invalid_argument("region query count exceeds range: "+std::string(flag));return n;
}
int32_t coordinate(std::string_view s){
  const auto digits=s.starts_with('-')?s.substr(1):s;
  if(digits.empty()||digits.find_first_not_of("0123456789")!=std::string_view::npos)throw std::invalid_argument("--region requires six signed Int32 decimal indices");
  int32_t n{};const auto r=std::from_chars(s.data(),s.data()+s.size(),n);if(r.ec!=std::errc{}||r.ptr!=s.data()+s.size())throw std::invalid_argument("--region endpoint exceeds Int32");return n;
}
const char* type(vdb::RegionValueType t){
  switch(t){case vdb::RegionValueType::Boolean:return "bool";case vdb::RegionValueType::Mask:return "mask";case vdb::RegionValueType::Int32:return "int32";case vdb::RegionValueType::Int64:return "int64";case vdb::RegionValueType::Float:return "float32";case vdb::RegionValueType::Double:return "float64";case vdb::RegionValueType::Vec3I:return "vec3i";case vdb::RegionValueType::Vec3S:return "vec3f";case vdb::RegionValueType::Vec3D:return "vec3d";}throw std::invalid_argument("unknown native region type");
}
struct Report {
  std::string text;uintmax_t reservation{};uint64_t formattingWork{};size_t prefixBytes{};
  void need(size_t n){if(n>text.capacity()-text.size())throw std::invalid_argument("region query bounded JSON reservation exhausted; no partial report emitted");}
  void append(std::string_view s){need(s.size());text.append(s);}
  void character(char c){need(1);text.push_back(c);}
  template<class T>void numeric(T n){std::array<char,128> b;std::to_chars_result r;
    if constexpr(std::is_floating_point_v<T>)r=std::to_chars(b.data(),b.data()+b.size(),n,std::chars_format::general,std::numeric_limits<T>::max_digits10);
    else r=std::to_chars(b.data(),b.data()+b.size(),n);if(r.ec!=std::errc{})throw std::invalid_argument("region query numeric formatting failed");append({b.data(),size_t(r.ptr-b.data())});}
  void boolean(bool b){append(b?"true":"false");}
  void string(std::string_view s){constexpr char h[]="0123456789abcdef";character('"');for(unsigned char c:s){if(c=='"'||c=='\\'){character('\\');character(char(c));}else if(c<0x20){append("\\u00");character(h[c>>4]);character(h[c&15]);}else character(char(c));}character('"');}
  template<class T>void field(const char* key,T n){append(",\"");append(key);append("\":");if constexpr(std::is_same_v<T,bool>)boolean(n);else numeric(n);}
  void hex(std::span<const uint8_t> b){constexpr char h[]="0123456789abcdef";character('"');for(const auto c:b){character(h[c>>4]);character(h[c&15]);}character('"');}
  template<class UInt>void bits(UInt b){constexpr char h[]="0123456789abcdef";character('"');for(size_t i=sizeof(UInt)*2;i-->0;)character(h[(b>>(i*4))&15]);character('"');}
  void byteString(std::string_view s){const bool valid=utf8(s),truncated=s.size()>prefixBytes;const auto n=std::min(s.size(),prefixBytes);
    append("{\"text\":");if(valid&&!truncated)string(s);else append("null");field("validUtf8",valid);field("byteCount",s.size());field("prefixBytes",n);field("truncated",truncated);append(",\"bytesHex\":");hex({reinterpret_cast<const uint8_t*>(s.data()),n});character('}');}
  void blob(std::span<const uint8_t> b,const char* encoding){const auto n=std::min(b.size(),prefixBytes);append("{\"encoding\":");string(encoding);field("byteCount",b.size());field("prefixBytes",n);field("truncated",n!=b.size());append(",\"bytesHex\":");hex(b.first(n));character('}');}
  template<class T,size_t N>void array(const std::array<T,N>& a){character('[');for(size_t i=0;i<N;++i){if(i)character(',');numeric(a[i]);}character(']');}
  void box(const vdb::IndexRegion& b){append("{\"minimum\":");array(b.minimum);append(",\"maximum\":");array(b.maximum);character('}');}
  void nativeValue(const vdb::RegionNativeValue& v,vdb::RegionValueType native){append("{\"type\":");string(type(native));
    std::visit([&](const auto& n){using T=std::decay_t<decltype(n)>;
      if constexpr(std::is_same_v<T,vdb::RegionFloat32Bits>||std::is_same_v<T,vdb::RegionFloat64Bits>){append(",\"bits\":");bits(n.bits);}
      else if constexpr(std::is_same_v<T,std::array<vdb::RegionFloat32Bits,3>>||std::is_same_v<T,std::array<vdb::RegionFloat64Bits,3>>){append(",\"bits\":[");for(size_t i=0;i<3;++i){if(i)character(',');bits(n[i].bits);}character(']');}
      else if constexpr(std::is_same_v<T,std::array<int32_t,3>>){append(",\"value\":");array(n);}
      else if constexpr(std::is_same_v<T,bool>){append(",\"value\":");boolean(n);}
      else{append(",\"value\":");numeric(n);}},v);character('}');}
  void metadata(const std::vector<vdb::RegionQueryMetadata>& items){character('[');for(size_t i=0;i<items.size();++i){if(i)character(',');const auto& m=items[i];append("{\"name\":");byteString(m.name);append(",\"type\":");byteString(m.type);append(",\"serializedValue\":");blob(m.serializedValue,"native-metadata-write-with-uint32-size-prefix");character('}');}character(']');}
  Report(size_t queryCount,const vdb::ActiveRegionOptions& o,size_t displayBytes,long double callerBytes):prefixBytes(displayBytes){
    const long double records=std::min<long double>(o.maxTotalRecords,o.maxRecordsPerQuery*static_cast<long double>(queryCount));
    // Record bounds include both boxes, exact typed bits and all counts. The
    // metadata reserve covers key/type envelopes, escaped text, exact byte hex,
    // source transform bytes and fixed native diagnostics without a JSON DOM.
    const long double bound=1048576+640*records+2048.0L*queryCount+16.0L*o.maxMetadataBytes+32*callerBytes;
    if(bound>=o.maxInputBytes||bound>std::numeric_limits<size_t>::max())throw std::invalid_argument("region query report reservation exceeds --max-input-bytes; reduce record/metadata limits");
    text.reserve(size_t(bound));const long double retained=text.capacity()+1.0L+65536+32*callerBytes;
    const long double work=2*retained;if(retained>=o.maxInputBytes||work>=o.maxWork)throw std::invalid_argument("region query actual report/options/formatting reservation exceeds shared byte/work caps");
    reservation=uintmax_t(retained);formattingWork=uint64_t(work);
  }
  void write(const vdb::ActiveRegionBatch& r,std::string_view input,std::string_view selector){
    append("{\"schemaVersion\":1,\"operation\":\"query-active-regions\",\"input\":");string(input);append(",\"inputGrid\":");string(selector);append(",\"nativeType\":");string(type(r.nativeType));
    append(",\"descriptorName\":");byteString(r.descriptorName);append(",\"gridName\":");byteString(r.gridName);append(",\"valueType\":");byteString(r.valueType);append(",\"gridClass\":");byteString(r.gridClass);append(",\"vectorType\":");byteString(r.vectorType);append(",\"transformType\":");byteString(r.transformType);
    field("inWorldSpace",r.inWorldSpace);field("affine",r.affine);append(",\"indexToWorld\":");if(r.indexToWorld)array(*r.indexToWorld);else append("null");append(",\"serializedTransform\":");blob(r.serializedTransform,"native-vdb-transform-record");append(",\"background\":");nativeValue(r.background,r.nativeType);
    field("sourceActiveVoxels",r.sourceActiveVoxels);field("sourceActiveRecords",r.sourceActiveRecords);field("sourceActiveTiles",r.sourceActiveTiles);field("sourceActiveTileVoxels",r.sourceActiveTileVoxels);field("sourceLeafNodes",r.sourceLeafNodes);field("sourceLowerNodes",r.sourceLowerNodes);field("sourceUpperNodes",r.sourceUpperNodes);field("sourceRootEntries",r.sourceRootEntries);field("storedLeafVoxels",r.storedLeafVoxels);field("storedInternalSlots",r.storedInternalSlots);field("nodeAndRootEntryCount",r.nodeAndRootEntryCount);field("nativeBytes",r.nativeBytes);field("nativeWorkspaceUpperBound",r.workspaceUpperBound);field("reportReservation",reservation);field("workspaceUpperBound",r.workspaceUpperBound+reservation);field("nativeWorkUpperBound",r.workUpperBound);field("formattingWorkUpperBound",formattingWork);field("workUpperBound",r.workUpperBound+formattingWork);field("countsComplete",r.countsComplete);append(",\"traversal\":");string(r.traversal);field("reportBytePrefixLimit",prefixBytes);
    append(",\"queries\":[");for(size_t i=0;i<r.queries.size();++i){if(i)character(',');const auto& q=r.queries[i];append("{\"region\":");box(q.region);field("matchedRecordCount",q.matchedRecordCount);field("matchedVoxelCount",q.matchedVoxelCount);field("returnedVoxelCount",q.returnedVoxelCount);field("omittedRecordCount",q.omittedRecordCount);field("omittedVoxelCount",q.omittedVoxelCount);field("countsComplete",q.countsComplete);field("recordsTruncated",q.recordsTruncated);append(",\"records\":[");for(size_t j=0;j<q.records.size();++j){if(j)character(',');const auto& v=q.records[j];append("{\"source\":");box(v.source);append(",\"clipped\":");box(v.clipped);field("sourceLevel",v.sourceLevel);field("tile",v.tile);field("voxelCount",v.voxelCount);append(",\"value\":");nativeValue(v.value,r.nativeType);character('}');}append("]}");}character(']');
    append(",\"gridMetadata\":");metadata(r.gridMetadata);append(",\"fileMetadata\":");metadata(r.fileMetadata);append(",\"diagnostics\":[");for(size_t i=0;i<r.diagnostics.size();++i){if(i)character(',');byteString(r.diagnostics[i]);}append("]}");std::cout.write(text.data(),std::streamsize(text.size()));std::cout.put('\n');
  }
};
} // namespace region_queries_cli

std::optional<int> runRegionQueriesCommand(int argc,char** argv){
  if(argc<2||std::string_view(argv[1])!="--query-active-regions")return {};
  if(argc<3||!argv[2][0]||std::string_view(argv[2]).starts_with("--")||std::string_view(argv[2])=="-h")throw std::invalid_argument("--query-active-regions requires nonempty INPUT before options; use ./-h for a literal filename");
  vdb::ActiveRegionOptions options;std::vector<vdb::IndexRegion> queries;std::string selector;size_t displayBytes=4096;bool countsOnly=false;std::set<std::string> seen;
  const auto argument=[&](int& i,const std::string& flag){const auto s=value(argc,argv,i,flag);if(s.empty()||std::string_view(s).starts_with("--")||s=="-h")throw std::invalid_argument("missing nonempty value for "+flag);return s;};
  const auto bounded=[&](int& i,const std::string& flag,uint64_t maximum,bool zero=false){return region_queries_cli::integer(argument(i,flag),flag,maximum,zero);};
  for(int i=3;i<argc;++i){const std::string arg=argv[i],key=arg=="--input-grid"?"--grid":arg;
    if(arg=="--region"){if(queries.size()>=4096)throw std::invalid_argument("region query list exceeds4096");vdb::IndexRegion q;for(auto& n:q.minimum)n=region_queries_cli::coordinate(argument(i,arg));for(auto& n:q.maximum)n=region_queries_cli::coordinate(argument(i,arg));for(size_t a=0;a<3;++a)if(q.minimum[a]>q.maximum[a])throw std::invalid_argument("--region requires ordered inclusive minimum/maximum indices");queries.push_back(q);continue;}
    if(!seen.insert(key).second)throw std::invalid_argument("duplicate region query option: "+arg);
    if(key=="--grid")selector=argument(i,arg);
    else if(arg=="--counts-only")countsOnly=true;
    else if(arg=="--max-input-bytes")options.maxInputBytes=bounded(i,arg,16ull<<30);
    else if(arg=="--max-voxels")options.maxStoredLeafVoxels=bounded(i,arg,UINT64_MAX);
    else if(arg=="--max-region-nodes")options.maxNodes=bounded(i,arg,1'000'000);
    else if(arg=="--max-region-active-voxels")options.maxSourceActiveVoxels=bounded(i,arg,UINT64_MAX);
    else if(arg=="--max-region-work")options.maxWork=bounded(i,arg,10'000'000'000ull);
    else if(arg=="--max-region-grids")options.maxGrids=size_t(bounded(i,arg,4096));
    else if(arg=="--max-region-queries")options.maxQueries=size_t(bounded(i,arg,4096));
    else if(arg=="--max-region-metadata")options.maxMetadataBytes=bounded(i,arg,16ull<<20);
    else if(arg=="--max-region-records")options.maxRecordsPerQuery=bounded(i,arg,1'000'000,true);
    else if(arg=="--max-region-total-records")options.maxTotalRecords=bounded(i,arg,1'000'000,true);
    else if(arg=="--region-report-bytes")displayBytes=size_t(bounded(i,arg,16ull<<20,true));
    else throw std::invalid_argument("unknown/incompatible region query option: "+arg);
  }
  if(queries.size()>options.maxQueries)throw std::invalid_argument("region query batch exceeds --max-region-queries");
  if(countsOnly){if(seen.contains("--max-region-records")||seen.contains("--max-region-total-records"))throw std::invalid_argument("--counts-only cannot be combined with explicit record caps");options.maxRecordsPerQuery=options.maxTotalRecords=0;}
  if(!vdb::IsAvailable()){(void)vdb::QueryActiveRegions({}, {},options,selector);return EXIT_FAILURE;}
  long double callerBytes=0;for(int i=1;i<argc;++i){const std::string_view s=argv[i];if(!region_queries_cli::utf8(s))throw std::invalid_argument("region query caller text must be valid UTF-8");callerBytes+=s.size();}if(callerBytes>256*1024)throw std::invalid_argument("region query caller/options text exceeds256KiB");
  region_queries_cli::Report report(queries.size(),options,displayBytes,callerBytes);auto native=options;native.maxInputBytes-=report.reservation;native.maxWork-=report.formattingWork;
  const auto result=vdb::QueryActiveRegions(path(argv[2]),queries,native,selector);report.write(result,argv[2],selector);return EXIT_SUCCESS;
}
