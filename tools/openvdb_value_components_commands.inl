// Requires OpenVdbValueComponents.h, runtime IsAvailable(), <charconv>,
// <type_traits>, common value/path helpers. Dispatch
// runValueComponentsCommand before the legacy parser. No private SDK macro.
// Help: --value-components INPUT.vdb OUTPUT.vdb
//   --grid/--input-grid NAME --component-prefix/--grid-name PREFIX
//   --max-components N (1..64), --max-component-active-voxels N
//   --max-component-output-voxels N (AGGREGATE output leaf slots)
//   --max-component-nodes/work/grids/metadata N
//   --max-voxels N (each source/expanded tree's stored leaf slots)
//   --max-input-bytes/--max-output-bytes N (joint workspace/encoded cap)
// Each output retains the entire SOURCE FIELD with one component's ACTIVITY.
// Other component values become inactive, never zeroed. Mask has activity only.
// These are not isolated solids/SDFs or physical density partitions.
namespace value_components_cli {
bool utf8(std::string_view s)noexcept{for(size_t i=0;i<s.size();){const auto c=static_cast<unsigned char>(s[i++]);if(c<0x80)continue;size_t n;unsigned char low=0x80,high=0xbf;if(c>=0xc2&&c<=0xdf)n=1;else if(c>=0xe0&&c<=0xef){n=2;if(c==0xe0)low=0xa0;if(c==0xed)high=0x9f;}else if(c>=0xf0&&c<=0xf4){n=3;if(c==0xf0)low=0x90;if(c==0xf4)high=0x8f;}else return false;if(n>s.size()-i)return false;const auto first=static_cast<unsigned char>(s[i++]);if(first<low||first>high)return false;while(--n){const auto d=static_cast<unsigned char>(s[i++]);if(d<0x80||d>0xbf)return false;}}return true;}
uint64_t count(std::string_view s,const std::string& flag,uint64_t maximum){if(s.empty()||s.find_first_not_of("0123456789")!=std::string_view::npos)throw std::invalid_argument("unsigned decimal required for "+flag);uint64_t n{};const auto r=std::from_chars(s.data(),s.data()+s.size(),n);if(r.ec!=std::errc{}||r.ptr!=s.data()+s.size()||!n||n>maximum)throw std::invalid_argument("value-component count exceeds range: "+flag);return n;}
struct Report {
  std::string text;uintmax_t reservation{};uint64_t formattingWork{};
  void need(size_t n){if(n>text.capacity()-text.size())throw std::invalid_argument("value-component bounded JSON reservation exhausted; no partial report emitted");}
  void append(std::string_view s){need(s.size());text.append(s);}
  void character(char c){need(1);text.push_back(c);}
  template<class T>void numeric(T n){std::array<char,128> b;const auto r=std::to_chars(b.data(),b.data()+b.size(),n);if(r.ec!=std::errc{})throw std::invalid_argument("value-component numeric formatting failed");append({b.data(),size_t(r.ptr-b.data())});}
  void boolean(bool b){append(b?"true":"false");}
  void string(std::string_view s){constexpr char h[]="0123456789abcdef";character('"');for(unsigned char c:s){if(c=='"'||c=='\\'){character('\\');character(char(c));}else if(c<0x20){append("\\u00");character(h[c>>4]);character(h[c&15]);}else character(char(c));}character('"');}
  template<class T>void field(const char* key,T n){append(",\"");append(key);append("\":");if constexpr(std::is_same_v<T,bool>)boolean(n);else numeric(n);}
  void stringField(const char* key,std::string_view s){append(",\"");append(key);append("\":");string(s);}
  void box(const std::optional<vdb::IndexRegion>& b){if(!b){append("null");return;}append("{\"minimum\":[");for(size_t i=0;i<3;++i){if(i)character(',');numeric(b->minimum[i]);}append("],\"maximum\":[");for(size_t i=0;i<3;++i){if(i)character(',');numeric(b->maximum[i]);}append("]}");}
  Report(const vdb::ActiveValueComponentsOptions& o,long double callerBytes){const long double bound=262144+32*callerBytes;if(bound>=o.maxInputBytes||bound>std::numeric_limits<size_t>::max())throw std::invalid_argument("value-component report/options reserve exceeds --max-input-bytes");text.reserve(size_t(bound));const long double retained=text.capacity()+1.0L+65536+32*callerBytes,work=4*retained;if(retained>=o.maxInputBytes||work>=o.maxWork)throw std::invalid_argument("value-component actual report/options/formatting reserve exceeds shared byte/work caps");reservation=uintmax_t(retained);formattingWork=uint64_t(work);}
  void write(const vdb::ActiveValueComponentsResult& r,std::string_view input,std::string_view output,std::string_view selector){
    append("{\"schemaVersion\":1,\"operation\":\"value-components\",\"input\":");string(input);stringField("output",output);stringField("inputGrid",selector);stringField("valueType",r.valueType);field("activeComponentCount",r.activeComponentCount);field("emptySourceCopy",r.emptySourceCopy);field("sourceActiveVoxels",r.sourceActiveVoxels);field("sourceActiveTiles",r.sourceActiveTiles);field("sourceStoredLeafVoxels",r.sourceStoredLeafVoxels);field("expandedStoredLeafVoxels",r.expandedStoredLeafVoxels);field("aggregateOutputLeafVoxels",r.aggregateOutputLeafVoxels);field("nodeUpperBound",r.nodeUpperBound);field("nativeSourceBytes",r.nativeSourceBytes);field("nativeWorkspaceUpperBound",r.workspaceUpperBound);field("reportReservation",reservation);field("workspaceUpperBound",r.workspaceUpperBound+reservation);field("nativeWorkUpperBound",r.workUpperBound);field("formattingWorkUpperBound",formattingWork);field("workUpperBound",r.workUpperBound+formattingWork);
    stringField("valuePolicy","Entire SOURCE FIELD values/background/inactive continuations are retained; only component ACTIVITY changes. Mask has no independent value payload.");stringField("geometryPolicy","INDEX six-neighbor activity; source class/vector/world tags retained as labels. Outputs are not isolated solids/SDFs, cavity certificates or physical density/mass partitions.");stringField("ordering","component_minimum_lexicographic_xyz");stringField("metadataPolicy","Custom native grid/file metadata and exact map retained; names/statistics regenerated, exact five canonical boundary-projection provenance keys removed.");
    append(",\"grids\":[");for(size_t i=0;i<r.grids.size();++i){if(i)character(',');const auto& g=r.grids[i];append("{\"gridName\":");string(g.gridName);field("activeVoxelCount",g.activeVoxelCount);field("storedLeafVoxels",g.storedLeafVoxels);append(",\"activeBounds\":");box(g.activeBounds);character('}');}character(']');
    field("nativeDiagnosticCount",r.diagnostics.size());append(",\"diagnostics\":[");bool truncated=r.diagnostics.size()>8;for(size_t i=0;i<std::min<size_t>(8,r.diagnostics.size());++i){if(i)character(',');const auto& d=r.diagnostics[i];size_t n=std::min<size_t>(1024,d.size());if(n<d.size())while(n&&(static_cast<unsigned char>(d[n])&0xc0)==0x80)--n;const std::string_view prefix(d.data(),n);truncated|=n!=d.size();if(utf8(prefix))string(prefix);else{string("Native diagnostic contains invalid UTF-8; use native API for exact bytes");truncated=true;}}character(']');field("diagnosticsTruncated",truncated);character('}');std::cout.write(text.data(),std::streamsize(text.size()));std::cout.put('\n');
  }
};
} // namespace value_components_cli
std::optional<int> runValueComponentsCommand(int argc,char** argv){
  if(argc<2||std::string_view(argv[1])!="--value-components")return {};
  if(argc<4)throw std::invalid_argument("--value-components requires INPUT OUTPUT before options");
  for(int i=2;i<4;++i)if(!argv[i][0]||std::string_view(argv[i]).starts_with("--")||std::string_view(argv[i])=="-h")throw std::invalid_argument("--value-components requires nonempty INPUT OUTPUT; use ./-h for a literal filename");
  vdb::ActiveValueComponentsOptions options;std::string selector;std::set<std::string> seen;
  const auto argument=[&](int& i,const std::string& flag){const auto s=value(argc,argv,i,flag);if(s.empty()||std::string_view(s).starts_with("--")||s=="-h")throw std::invalid_argument("missing nonempty value for "+flag);return s;};
  const auto bounded=[&](int& i,const std::string& flag,uint64_t maximum){return value_components_cli::count(argument(i,flag),flag,maximum);};
  for(int i=4;i<argc;++i){const std::string arg=argv[i],key=arg=="--input-grid"?"--grid":arg=="--grid-name"?"--component-prefix":arg;if(!seen.insert(key).second)throw std::invalid_argument("duplicate value-component option: "+arg);
    if(key=="--grid")selector=argument(i,arg);else if(key=="--component-prefix")options.gridNamePrefix=argument(i,arg);
    else if(arg=="--max-components")options.maxComponents=uint32_t(bounded(i,arg,64));
    else if(arg=="--max-component-active-voxels")options.maxActiveVoxels=bounded(i,arg,UINT64_MAX);
    else if(arg=="--max-voxels")options.maxStoredLeafVoxels=bounded(i,arg,UINT64_MAX);
    else if(arg=="--max-component-output-voxels")options.maxAggregateOutputLeafVoxels=bounded(i,arg,UINT64_MAX);
    else if(arg=="--max-component-nodes")options.maxNodes=bounded(i,arg,1'000'000);
    else if(arg=="--max-component-work")options.maxWork=bounded(i,arg,10'000'000'000ull);
    else if(arg=="--max-component-grids")options.maxGrids=uint32_t(bounded(i,arg,4096));
    else if(arg=="--max-component-metadata")options.maxMetadataBytes=bounded(i,arg,16ull<<20);
    else if(arg=="--max-input-bytes")options.maxInputBytes=bounded(i,arg,16ull<<30);
    else if(arg=="--max-output-bytes")options.maxOutputBytes=bounded(i,arg,16ull<<30);
    else throw std::invalid_argument("unknown/incompatible value-component option: "+arg);
  }
  if(!vdb::IsAvailable()){(void)vdb::WriteActiveValueComponents({}, {},options,selector);return EXIT_FAILURE;}
  long double callerBytes=0;for(int i=1;i<argc;++i){const std::string_view s=argv[i];if(!value_components_cli::utf8(s))throw std::invalid_argument("value-component caller text must be valid UTF-8 before publication");callerBytes+=s.size();}if(callerBytes>256*1024)throw std::invalid_argument("value-component caller/options text exceeds256KiB");
  value_components_cli::Report report(options,callerBytes);auto native=options;native.maxInputBytes-=report.reservation;native.maxWork-=report.formattingWork;
  const auto result=vdb::WriteActiveValueComponents(path(argv[2]),path(argv[3]),native,selector);report.write(result,argv[2],argv[3],selector);return EXIT_SUCCESS;
}
