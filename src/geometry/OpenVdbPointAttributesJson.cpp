#include "Container/geometry/OpenVdbPointAttributesJson.h"

#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <streambuf>
#include <string_view>
#include <type_traits>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {throw std::runtime_error("OpenVDB point attribute JSON is unavailable; configure with -DENABLE_OPENVDB=ON.");}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
constexpr long double Fixed=128.0L*1024;
constexpr std::string_view Traversal="leaf_origin_xyz_then_local_index";
constexpr std::array<std::string_view,8> Types{"bool","int32","int64","float","double","vec3f","vec3d","string"};
void budget(long double bytes,const ProcessingOptions& p) {
  if (!std::isfinite(bytes) || bytes>p.maxInputBytes)
    throw std::invalid_argument("point attribute JSON/parser/native workspace exceeds the shared input-memory budget");
}
void validate(const ProcessingOptions& p,const PointAttributeOptions& o) {
  if (!p.maxInputBytes || !p.maxVoxelCount || !o.maxPoints || uint64_t(o.maxPoints)>uint64_t(INT64_MAX) ||
      !o.maxAttributes || o.maxAttributes>1024 || !o.maxGroups || o.maxGroups>4096 ||
      !o.maxEdits || o.maxEdits>1024 || !o.maxStringBytes)
    throw std::invalid_argument("invalid point attribute JSON count/string/memory limits");
  budget(Fixed,p);
}
void validateOutput(const std::filesystem::path& output) {
  if (output.empty() || std::filesystem::exists(output) ||
      (output.has_parent_path() && !std::filesystem::is_directory(output.parent_path())))
    throw std::invalid_argument("point attribute JSON output must be a new path in an existing directory");
}
ProcessingOptions nativeBudget(const ProcessingOptions& p,long double reserve) {
  budget(reserve+1,p); auto result=p;
  result.maxInputBytes-=static_cast<uintmax_t>(std::ceil(reserve)); return result;
}
void validName(const std::string& value) {
  if (value.empty() || value.size()>256 || value.find('\0')!=std::string::npos)
    throw std::invalid_argument("point attribute JSON names require1..256 bytes without NUL");
}
struct Snapshot {
  std::filesystem::path directory,file; uintmax_t encoded=0;
  Snapshot(const std::filesystem::path& input,const ProcessingOptions& p) {
    if (!std::filesystem::is_regular_file(input)) throw std::invalid_argument("point attribute edit JSON is not a regular file");
    encoded=std::filesystem::file_size(input); budget(3.0L*encoded+Fixed,p);
    static std::atomic_uint64_t serial{0};
    for (unsigned attempt=0;attempt<64;++attempt) {
      directory=std::filesystem::temp_directory_path()/("container-point-json-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));
      if (std::filesystem::create_directory(directory)) {file=directory/"edits.json";break;}
    }
    if (file.empty()) throw std::runtime_error("cannot create point attribute edit snapshot directory");
    try {
      std::ifstream source(input,std::ios::binary); std::ofstream target(file,std::ios::binary|std::ios::out|std::ios::noreplace);
      if (!source || !target) throw std::runtime_error("cannot open point attribute edit snapshot");
      std::array<char,64*1024> buffer{}; auto remaining=encoded;
      while (remaining) {
        const auto count=static_cast<std::streamsize>(std::min<uintmax_t>(remaining,buffer.size()));
        source.read(buffer.data(),count); if (source.gcount()!=count) throw std::runtime_error("point attribute edit JSON changed size");
        target.write(buffer.data(),count); remaining-=uintmax_t(count);
      }
      target.close(); if (!target || source.bad() || source.peek()!=std::char_traits<char>::eof())
        throw std::runtime_error("point attribute edit snapshot failed or source changed size");
    } catch (...) {cleanup();throw;}
  }
  void cleanup(){std::error_code ignored;std::filesystem::remove(file,ignored);std::filesystem::remove(directory,ignored);}
  ~Snapshot(){cleanup();}
};
// Bound the lexical number buffer before nlohmann builds an arbitrarily long
// token. Strings are bounded by the encoded input reserve and decoded limits.
class TokenInput final:public std::streambuf {
 public:
  explicit TokenInput(std::ifstream& source):source_(source){}
  bool exceeded=false;
 protected:
  int_type underflow() override {
    if (gptr()<egptr()) return traits_type::to_int_type(*gptr());
    source_.read(buffer_,sizeof(buffer_)); const auto count=source_.gcount();
    if (!count) return traits_type::eof();
    for (std::streamsize i=0;i<count;++i) {
      const char c=buffer_[i];
      if (inString_) {if (!escape_ && c=='"') inString_=false; if (!escape_ && c=='\\') escape_=true;else escape_=false;}
      else if (c=='"') {inString_=true;escape_=false;numberBytes_=0;}
      else if (c==' ' || c=='\t' || c=='\r' || c=='\n' || c=='[' || c==']' || c=='{' || c=='}' || c==':' || c==',') numberBytes_=0;
      else if (++numberBytes_>128) {exceeded=true;return traits_type::eof();}
    }
    setg(buffer_,buffer_,buffer_+count);return traits_type::to_int_type(*gptr());
  }
 private:
  std::ifstream& source_;char buffer_[4096];size_t numberBytes_=0;bool inString_=false,escape_=false;
};
using Number=std::variant<int64_t,uint64_t,double>;
using Atom=std::variant<bool,int64_t,uint64_t,double,std::string,std::array<Number,3>>;
template<class T>T numberAs(const Number& value) {
  if constexpr (std::is_integral_v<T>) {
    if (const auto* v=std::get_if<int64_t>(&value)) {
      if (*v<std::numeric_limits<T>::min() || *v>std::numeric_limits<T>::max()) throw std::invalid_argument("integer point attribute JSON value is outside its exact type range");
      return static_cast<T>(*v);
    }
    if (const auto* v=std::get_if<uint64_t>(&value)) {
      if (*v>uint64_t(std::numeric_limits<T>::max())) throw std::invalid_argument("integer point attribute JSON value is outside its exact type range");
      return static_cast<T>(*v);
    }
    throw std::invalid_argument("integer point attributes require integer JSON tokens, without decimal/exponent conversion");
  } else {
    // Convert integer alternatives straight to the destination. Passing an
    // integer through Double first can double-round a halfway Float value.
    return std::visit([](auto source) {
      const auto result=static_cast<T>(source);
      if (!std::isfinite(result) || (source!=0 && result==0)) throw std::invalid_argument("point attribute JSON number overflows/underflows its native type");
      return result;
    },value);
  }
}
Number numeric(const Atom& atom) {
  if (const auto* p=std::get_if<int64_t>(&atom)) return *p;
  if (const auto* p=std::get_if<uint64_t>(&atom)) return *p;
  if (const auto* p=std::get_if<double>(&atom)) return *p;
  throw std::invalid_argument("numeric point attribute JSON values required");
}
template<class T>std::vector<T> convert(const std::vector<Atom>& atoms) {
  std::vector<T> result;result.reserve(atoms.size());
  for (const auto& atom:atoms) {
    if constexpr(std::is_same_v<T,uint8_t>) {
      const auto* value=std::get_if<bool>(&atom); if (!value) throw std::invalid_argument("bool/group point JSON values must be booleans");result.push_back(uint8_t(*value));
    } else if constexpr(std::is_same_v<T,std::string>) {
      const auto* value=std::get_if<std::string>(&atom);if (!value) throw std::invalid_argument("string point JSON values must be strings");result.push_back(*value);
    } else if constexpr(std::is_same_v<T,std::array<float,3>> || std::is_same_v<T,std::array<double,3>>) {
      const auto* value=std::get_if<std::array<Number,3>>(&atom);if (!value) throw std::invalid_argument("Vec3 point JSON values require three-number arrays");
      using Scalar=typename T::value_type;result.push_back({numberAs<Scalar>((*value)[0]),numberAs<Scalar>((*value)[1]),numberAs<Scalar>((*value)[2])});
    } else result.push_back(numberAs<T>(numeric(atom)));
  }
  return result;
}
PointAttributeValues convertType(int type,const std::vector<Atom>& atoms) {
  switch(type) {
    case 0:return convert<uint8_t>(atoms);case 1:return convert<int32_t>(atoms);case 2:return convert<int64_t>(atoms);
    case 3:return convert<float>(atoms);case 4:return convert<double>(atoms);case 5:return convert<std::array<float,3>>(atoms);
    case 6:return convert<std::array<double,3>>(atoms);case 7:return convert<std::string>(atoms);
    default:throw std::invalid_argument("unknown point attribute JSON type");
  }
}
struct Parser: nlohmann::json_sax<nlohmann::json> {
  enum class State{Start,Root,Attributes,Groups,Attribute,Group,Values,Vector,Done} state=State::Start;
  enum class Field{None,Version,Attributes,Groups,Op,Name,NewName,Type,Values} field=Field::None;
  const ProcessingOptions& p;const PointAttributeOptions& o;
  long double reserve=0,retained=0;uintmax_t stringBytes=0,currentStringBytes=0;
  unsigned rootSeen=0,seen=0;bool group=false;int type=-1;
  std::string op,name,newName,error;std::vector<Atom> atoms;std::array<Number,3> vector;size_t vectorSize=0;
  std::vector<PointAttributeEdit> attributes;std::vector<PointGroupEdit> groups;
  Parser(const ProcessingOptions& processing,const PointAttributeOptions& options,uintmax_t encoded):p(processing),o(options),reserve(3.0L*encoded+Fixed){}
  bool fail(const char* why){error=why;return false;}
  void check(size_t extra=0)const {
    budget(reserve+retained+(atoms.size()+static_cast<long double>(extra))*(3*sizeof(Atom)+96)+4.0L*currentStringBytes+4096,p);
  }
  bool atom(Atom value) {
    if (state!=State::Values) return fail("unexpected point JSON value outside a values array");
    if (atoms.size()>=o.maxPoints) return fail("point attribute JSON values exceed maxPoints");
    check(1);atoms.push_back(std::move(value));return true;
  }
  bool null()override{return fail("null point attribute JSON values are unsupported");}
  bool boolean(bool value)override{return atom(value);}
  bool binary(binary_t&)override{return fail("binary point attribute JSON is unsupported");}
  bool number_integer(number_integer_t value)override{return number(Number(int64_t(value)));}
  bool number_unsigned(number_unsigned_t value)override{return number(Number(uint64_t(value)));}
  bool number_float(number_float_t value,const string_t& raw)override {
    if (!std::isfinite(value)) return fail("point JSON numbers must be finite");
    if (value==0) for (const auto c:raw) {if (c=='e'||c=='E') break;if (c>='1'&&c<='9') return fail("point JSON floating token underflows Double range");}
    return number(Number(double(value)));
  }
  bool number(Number value) {
    if (state==State::Root && field==Field::Version) {
      bool one=false;std::visit([&](auto v){using T=decltype(v);if constexpr(std::is_integral_v<T>) one=v==1;},value);
      if (!one) return fail("point attribute JSON version must be integer1");field=Field::None;return true;
    }
    if (state==State::Vector) {if (vectorSize>=3) return fail("point JSON vectors require exactly three numbers");vector[vectorSize++]=std::move(value);return true;}
    return std::visit([&](auto v){return atom(Atom(v));},value);
  }
  bool string(string_t& value)override {
    if ((state==State::Attribute || state==State::Group) && field!=Field::None) {
      switch(field) {
        case Field::Op:op=value;break;
        case Field::Name:validName(value);name=value;break;
        case Field::NewName:validName(value);newName=value;break;
        case Field::Type: {
          const auto found=std::find(Types.begin(),Types.end(),value);if(found==Types.end()) return fail("unknown point attribute JSON type");type=int(found-Types.begin());break;
        }
        default:return fail("unexpected string point attribute JSON field");
      }
      if (value.size()>256) return fail("point attribute JSON field is too long");field=Field::None;return true;
    }
    if (state!=State::Values) return fail("unexpected string in point attribute JSON");
    if (value.size()>o.maxStringBytes-stringBytes) return fail("point attribute JSON strings exceed maxStringBytes");
    stringBytes+=value.size();currentStringBytes+=value.size();check(1);return atom(value);
  }
  bool key(string_t& value)override {
    if (field!=Field::None) return fail("missing point attribute JSON field value");
    unsigned bit=0;
    if (state==State::Root) {
      if(value=="version"){field=Field::Version;bit=1;}else if(value=="attributes"){field=Field::Attributes;bit=2;}
      else if(value=="groups"){field=Field::Groups;bit=4;}else return fail("unknown point edit JSON root key");
      if(rootSeen&bit)return fail("duplicate point edit JSON root key");rootSeen|=bit;
    } else if (state==State::Attribute || state==State::Group) {
      if(value=="op"){field=Field::Op;bit=1;}else if(value=="name"){field=Field::Name;bit=2;}
      else if(value=="new_name"){field=Field::NewName;bit=4;}else if(value=="type" && !group){field=Field::Type;bit=8;}
      else if(value=="values"){field=Field::Values;bit=16;}else return fail("unknown point attribute/group edit JSON key");
      if(seen&bit)return fail("duplicate point attribute/group edit JSON key");seen|=bit;
    } else return fail("unexpected key in point attribute JSON");
    return true;
  }
  bool start_object(std::size_t)override {
    if(state==State::Start){state=State::Root;return true;}
    if(state!=State::Attributes && state!=State::Groups)return fail("point edit JSON requires a versioned object with edit arrays");
    if(attributes.size()+groups.size()>=o.maxEdits)return fail("point edit JSON exceeds maxEdits");
    group=state==State::Groups;state=group?State::Group:State::Attribute;seen=0;field=Field::None;type=-1;
    op.clear();name.clear();newName.clear();atoms.clear();currentStringBytes=0;budget(reserve+retained+4096,p);return true;
  }
  bool end_object()override {
    if(state==State::Root){if(field!=Field::None || !(rootSeen&1))return fail("point edit JSON requires version1");state=State::Done;return true;}
    if((state!=State::Attribute && state!=State::Group)||field!=Field::None||(seen&3)!=3)return fail("each point edit requires op and name");
    PointEditOperation operation;
    if(op=="add")operation=PointEditOperation::Add;else if(op=="set")operation=PointEditOperation::Set;
    else if(op=="rename")operation=PointEditOperation::Rename;else if(op=="drop")operation=PointEditOperation::Drop;else return fail("unknown point edit operation");
    const bool payload=operation==PointEditOperation::Add || operation==PointEditOperation::Set;
    const unsigned required=payload?(group?19:27):(operation==PointEditOperation::Rename?7:3);
    if(seen!=required)return fail("point edit fields do not match its operation (add/set need type+values; rename needs new_name)");
    check();
    if(group) {
      PointGroupEdit edit{operation,name,newName,{}};if(payload)edit.membership=convert<uint8_t>(atoms);
      retained+=512+2.0L*(name.size()+newName.size())+3.0L*edit.membership.size();groups.push_back(std::move(edit));
    } else {
      PointAttributeEdit edit{operation,name,newName,{}};if(payload)edit.values=convertType(type,atoms);
      retained+=512+2.0L*(name.size()+newName.size());
      std::visit([&](const auto& values){retained+=3.0L*values.size()*sizeof(typename std::decay_t<decltype(values)>::value_type);},edit.values);
      retained+=4.0L*currentStringBytes;attributes.push_back(std::move(edit));
    }
    std::vector<Atom>().swap(atoms);currentStringBytes=0;budget(reserve+retained,p);state=group?State::Groups:State::Attributes;return true;
  }
  bool start_array(std::size_t)override {
    if(state==State::Root && (field==Field::Attributes || field==Field::Groups)) {state=field==Field::Attributes?State::Attributes:State::Groups;field=Field::None;return true;}
    if((state==State::Attribute || state==State::Group)&&field==Field::Values){state=State::Values;field=Field::None;return true;}
    if(state==State::Values){state=State::Vector;vectorSize=0;return true;}
    return fail("unexpected nested point edit JSON array");
  }
  bool end_array()override {
    if(state==State::Vector){if(vectorSize!=3)return fail("point JSON vectors require exactly three numbers");state=State::Values;return atom(vector);}
    if(state==State::Values){state=group?State::Group:State::Attribute;return true;}
    if(state==State::Attributes || state==State::Groups){state=State::Root;return true;}
    return fail("unexpected point edit JSON array end");
  }
  bool parse_error(std::size_t at,const std::string&,const nlohmann::detail::exception&)override {error="invalid point attribute JSON/UTF-8 at byte "+std::to_string(at);return false;}
};

bool validUtf8(std::string_view text) {
  for(size_t i=0;i<text.size();) {
    const auto a=uint8_t(text[i++]);if(a<0x80)continue;
    size_t remaining;uint8_t low=0x80,high=0xbf;
    if(a>=0xc2 && a<=0xdf)remaining=1;
    else if(a>=0xe0 && a<=0xef){remaining=2;if(a==0xe0)low=0xa0;if(a==0xed)high=0x9f;}
    else if(a>=0xf0 && a<=0xf4){remaining=3;if(a==0xf0)low=0x90;if(a==0xf4)high=0x8f;}
    else return false;
    if(remaining>text.size()-i)return false;
    const auto first=uint8_t(text[i++]);if(first<low || first>high)return false;
    while(--remaining){const auto c=uint8_t(text[i++]);if(c<0x80 || c>0xbf)return false;}
  }
  return true;
}
class Output {
 public:
  Output(const std::filesystem::path& path,uintmax_t maximum):path_(path),maximum_(maximum) {
    stream_.open(path,std::ios::binary|std::ios::out|std::ios::noreplace);
    if(!stream_)throw std::runtime_error("cannot exclusively create point attribute JSON output");owned_=true;
  }
  ~Output(){if(owned_){stream_.close();std::error_code ignored;std::filesystem::remove(path_,ignored);}}
  void write(std::string_view value) {
    if(value.size()>maximum_-count_)throw std::invalid_argument("point attribute JSON output exceeds the encoded byte budget");
    stream_.write(value.data(),static_cast<std::streamsize>(value.size()));count_+=value.size();
    if(!stream_)throw std::runtime_error("failed writing point attribute JSON output");
  }
  void string(std::string_view value) {
    if(!validUtf8(value))throw std::invalid_argument("native point attribute/name contains invalid UTF-8; JSON export cannot preserve these bytes");
    write("\"");size_t start=0;
    for(size_t i=0;i<value.size();++i) {
      const auto c=uint8_t(value[i]);if(c>=32 && c!='"' && c!='\\')continue;
      write(value.substr(start,i-start));
      if(c=='"')write("\\\"");else if(c=='\\')write("\\\\");else {constexpr char hex[]="0123456789abcdef";char escaped[]{'\\','u','0','0',hex[c>>4],hex[c&15]};write(std::string_view(escaped,6));}
      start=i+1;
    }
    write(value.substr(start));write("\"");
  }
  template<class T>void number(T value) {
    std::array<char,128> bytes{};std::to_chars_result result;
    if constexpr(std::is_floating_point_v<T>) {
      if(value==0 && std::signbit(value)){write("-0.0");return;}
      result=std::to_chars(bytes.data(),bytes.data()+bytes.size(),value,std::chars_format::general,std::numeric_limits<T>::max_digits10);
    } else result=std::to_chars(bytes.data(),bytes.data()+bytes.size(),value);
    if(result.ec!=std::errc{})throw std::runtime_error("cannot encode point attribute JSON number");write(std::string_view(bytes.data(),size_t(result.ptr-bytes.data())));
  }
  void finish(){write("\n");stream_.close();if(!stream_)throw std::runtime_error("failed closing point attribute JSON output");owned_=false;}
 private:
  std::filesystem::path path_;std::ofstream stream_;uintmax_t maximum_,count_=0;bool owned_=false;
};
void prefix(Output& output) {output.write("{\"version\":1,\"traversal\":");output.string(Traversal);}
void description(Output& output,const PointAttributeDescription& d) {
  output.write("{\"name\":");output.string(d.name);output.write(",\"value_type\":");output.string(d.valueType);
  output.write(",\"codec\":");output.string(d.codec);output.write(",\"stride\":");output.number(d.stride);
  output.write(",\"group_storage\":");output.write(d.groupStorage?"true":"false");output.write(",\"any_hidden\":");output.write(d.anyHidden?"true":"false");
  output.write(",\"readable\":");output.write(d.readable?"true":"false");output.write("}");
}
template<class T>void scalar(Output& output,const T& value) {
  if constexpr(std::is_same_v<T,uint8_t>)output.write(value?"true":"false");
  else if constexpr(std::is_same_v<T,std::string>)output.string(value);
  else if constexpr(std::is_same_v<T,std::array<float,3>> || std::is_same_v<T,std::array<double,3>>) {
    output.write("[");for(size_t i=0;i<3;++i){if(i)output.write(",");output.number(value[i]);}output.write("]");
  } else output.number(value);
}
template<class T>void values(Output& output,const std::vector<T>& items) {
  output.write("[");for(size_t i=0;i<items.size();++i){if(i)output.write(",");scalar(output,items[i]);}output.write("]");
}
#endif
} // namespace

PointAttributeSchema ApplyPointAttributeEditsFile(const std::filesystem::path& input,const std::filesystem::path& editsJson,
    const std::filesystem::path& output,const std::string& inputGrid,const ProcessingOptions& p,const PointAttributeOptions& o) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validate(p,o);validateOutput(output);Snapshot snapshot(editsJson,p);Parser parser(p,o,snapshot.encoded);
  {
    std::ifstream source(snapshot.file,std::ios::binary);if(!source)throw std::runtime_error("cannot open point edit JSON snapshot");
    TokenInput buffer(source);std::istream stream(&buffer);
    const bool parsed=nlohmann::json::sax_parse(stream,&parser);
    if(buffer.exceeded)throw std::invalid_argument("point attribute JSON numeric token exceeds128 bytes");
    if(!parsed || parser.state!=Parser::State::Done)throw std::invalid_argument(parser.error.empty()?"invalid point attribute edit JSON":parser.error);
    if(source.bad())throw std::runtime_error("failed reading point edit JSON snapshot");
  }
  return WritePointAttributeEdits(input,output,parser.attributes,parser.groups,inputGrid,nativeBudget(p,parser.reserve+parser.retained),o);
#else
  (void)input;(void)editsJson;(void)output;(void)inputGrid;(void)p;(void)o;unavailable();
#endif
}
PointAttributeSchema ExportPointAttributeSchema(const std::filesystem::path& input,const std::filesystem::path& outputJson,
    const std::string& inputGrid,const ProcessingOptions& p,const PointAttributeOptions& o) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validate(p,o);validateOutput(outputJson);const auto schema=InspectPointAttributes(input,inputGrid,nativeBudget(p,Fixed),o);
  Output output(outputJson,p.maxInputBytes);prefix(output);output.write(",\"grid_name\":");output.string(schema.gridName);
  output.write(",\"point_count\":");output.number(schema.pointCount);output.write(",\"leaf_count\":");output.number(schema.leafCount);
  output.write(",\"attributes\":[");for(size_t i=0;i<schema.attributes.size();++i){if(i)output.write(",");description(output,schema.attributes[i]);}
  output.write("],\"groups\":");values(output,schema.groups);output.write("}");output.finish();return schema;
#else
  (void)input;(void)outputJson;(void)inputGrid;(void)p;(void)o;unavailable();
#endif
}
void ExportPointAttribute(const std::filesystem::path& input,const std::string& attribute,const std::filesystem::path& outputJson,
    const std::string& inputGrid,const ProcessingOptions& p,const PointAttributeOptions& o) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validate(p,o);validateOutput(outputJson);const auto column=ReadPointAttribute(input,attribute,inputGrid,nativeBudget(p,Fixed),o);
  Output output(outputJson,p.maxInputBytes);prefix(output);output.write(",\"attribute\":");description(output,column.attribute);
  output.write(",\"type\":");output.string(Types[column.values.index()]);output.write(",\"values\":");
  std::visit([&](const auto& items){values(output,items);},column.values);output.write("}");output.finish();
#else
  (void)input;(void)attribute;(void)outputJson;(void)inputGrid;(void)p;(void)o;unavailable();
#endif
}
void ExportPointGroup(const std::filesystem::path& input,const std::string& group,const std::filesystem::path& outputJson,
    const std::string& inputGrid,const ProcessingOptions& p,const PointAttributeOptions& o) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validate(p,o);validateOutput(outputJson);const auto membership=ReadPointGroup(input,group,inputGrid,nativeBudget(p,Fixed),o);
  Output output(outputJson,p.maxInputBytes);prefix(output);output.write(",\"group\":");output.string(group);output.write(",\"values\":");
  values(output,membership);output.write("}");output.finish();
#else
  (void)input;(void)group;(void)outputJson;(void)inputGrid;(void)p;(void)o;unavailable();
#endif
}
} // namespace container::geometry::vdb
