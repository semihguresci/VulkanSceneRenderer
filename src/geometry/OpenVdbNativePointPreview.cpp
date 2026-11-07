#include "Container/geometry/OpenVdbNativePointPreview.h"
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbPointAttributesInternal.h"
#include <tbb/task_arena.h>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <variant>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable(){throw std::runtime_error("OpenVDB native point previews are unavailable; configure with -DENABLE_OPENVDB=ON.");}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using namespace point_attribute_detail;
constexpr std::array<const char*,3> ArchiveKeys{
  "container:point_empty_manifest_v1","container:point_empty_descriptor_v1","container:point_empty_leaf_flags_v1"};
using Scalar=std::variant<bool,int32_t,int64_t,float,double>;
using Scalars=std::variant<std::monostate,points::AttributeHandle<bool>,points::AttributeHandle<int32_t>,
  points::AttributeHandle<int64_t>,points::AttributeHandle<float>,points::AttributeHandle<double>>;

struct ScalarHandle {
  Scalars handle;
  explicit ScalarHandle(const points::AttributeArray& array) {
    if(!array.hasConstantStride() || array.stride()!=1 || points::isGroup(array))
      throw std::invalid_argument("chosen point scalar requires a numeric fixed-codec stride-one attribute");
    if(array.hasValueType<bool>()) handle.emplace<1>(array,false);
    else if(array.hasValueType<int32_t>()) handle.emplace<2>(array,false);
    else if(array.hasValueType<int64_t>()) handle.emplace<3>(array,false);
    else if(array.hasValueType<float>()) handle.emplace<4>(array,false);
    else if(array.hasValueType<double>()) handle.emplace<5>(array,false);
    else throw std::invalid_argument("chosen point scalar must have bool/int32/int64/Float/Double values");
  }
  Scalar get(openvdb::Index n) const {
    return std::visit([&](const auto& h)->Scalar {
      if constexpr(std::is_same_v<std::decay_t<decltype(h)>,std::monostate>) throw std::logic_error("unbound point scalar");
      else return h.get(n);
    },handle);
  }
};

bool numeric(const points::AttributeArray& array) {
  return array.hasValueType<bool>() || array.hasValueType<int32_t>() || array.hasValueType<int64_t>() ||
    array.hasValueType<float>() || array.hasValueType<double>();
}
double number(const Scalar& value) {
  return std::visit([](auto x){if constexpr(std::is_floating_point_v<decltype(x)>)if(!std::isfinite(x))throw std::invalid_argument("selected point numeric value must be finite");return double(x);},value);
}
void validateChosen(const Leaf& leaf,const NativePointPreviewOptions& o) {
  const auto require=[&](const std::string& field)->const points::AttributeArray& {
    if(!leaf.hasAttribute(field)) throw std::invalid_argument("chosen native point attribute was not found: "+field);
    const auto& array=leaf.constAttributeArray(field);
    if(!array.hasConstantStride() || array.stride()!=1 || points::isGroup(array))
      throw std::invalid_argument("chosen native point attribute must have constant stride one and cannot be group backing: "+field);
    return array;
  };
  if(!o.radiusAttribute.empty()) {
    const auto& array=require(o.radiusAttribute);
    if(o.radiusAttribute=="P" || !numeric(array)) throw std::invalid_argument("point radius requires a positive real numeric scalar (P/vector/string are unsupported)");
  }
  if(o.colorMode==NativePointColor::Uniform) return;
  const auto& array=require(o.colorAttribute);
  if(o.colorMode==NativePointColor::ScalarRamp && !numeric(array)) throw std::invalid_argument("scalar point colors require bool/int32/int64/Float/Double values");
  if(o.colorMode==NativePointColor::VectorDirection && !array.hasValueType<openvdb::Vec3f>() && !array.hasValueType<openvdb::Vec3d>())
    throw std::invalid_argument("vector point colors require native Vec3f/Vec3d components");
  if(o.colorMode==NativePointColor::StringPalette && !points::isString(array))
    throw std::invalid_argument("string point colors require native full-width StringCodec storage");
}

struct Predicate {
  std::array<std::vector<points::GroupHandle>,3> handles;
  Predicate(const Leaf& leaf,const PointGroupSelection& s) {
    size_t i=0;for(const auto* names:{&s.anyOf,&s.allOf,&s.noneOf}) {
      handles[i].reserve(names->size());for(const auto& n:*names)handles[i].push_back(leaf.groupHandle(n));++i;
    }
  }
  bool operator()(openvdb::Index point)const {
    bool any=handles[0].empty();for(const auto& h:handles[0])any|=h.get(point);if(!any)return false;
    for(const auto& h:handles[1])if(!h.get(point))return false;for(const auto& h:handles[2])if(h.get(point))return false;return true;
  }
};
long double selectionBytes(const PointGroupSelection& s,const PointAttributeOptions& options,const PointAttributeSchema* schema) {
  long double bytes=4096;size_t count=0;
  for(const auto* names:{&s.anyOf,&s.allOf,&s.noneOf})for(const auto& n:*names) {
    if(++count>3*options.maxGroups)throw std::invalid_argument("native point preview group predicate exceeds count limit");name(n);
    if(schema && std::find(schema->groups.begin(),schema->groups.end(),n)==schema->groups.end())throw std::invalid_argument("native point preview group was not found: "+n);
    bytes+=8.0L*n.size()+1024;
  }
  return bytes;
}

struct Affine {
  openvdb::Vec3d origin;std::array<openvdb::Vec3d,3> basis;double minimum;
  explicit Affine(const openvdb::math::Transform& transform) {
    if(!transform.isLinear())throw std::invalid_argument("native point preview requires an affine source transform");
    origin=transform.indexToWorld(openvdb::Vec3d(0));const auto map=transform.baseMap();
    basis={map->applyJacobian({1,0,0}),map->applyJacobian({0,1,0}),map->applyJacobian({0,0,1})};
    auto normalized=basis;minimum=INFINITY;
    for(size_t k=0;k<3;++k) {
      const double length=std::hypot(basis[k].x(),basis[k].y(),basis[k].z());
      if(!std::isfinite(origin[k]) || !std::isfinite(length) || length<1e-6 || length>1e6)
        throw std::invalid_argument("native point affine axes must be finite in [1e-6,1e6] world units");
      minimum=std::min(minimum,length);normalized[k]/=length;
    }
    if(std::abs(normalized[0].dot(normalized[1].cross(normalized[2])))<1e-8)
      throw std::invalid_argument("native point affine is singular or insufficiently conditioned");
  }
  openvdb::Vec3d world(const openvdb::Coord& coordinate,const openvdb::Vec3f& local) const {
    for(size_t k=0;k<3;++k)if(!std::isfinite(local[k]) || std::abs(local[k])>.500001f)
      throw std::invalid_argument("selected point P requires finite voxel-local offsets in [-0.5,0.5]");
    openvdb::Vec3d result;
    for(size_t row=0;row<3;++row) {
      double sum=origin[row],correction=0;
      const auto add=[&](double term){const double next=sum+term;correction+=std::abs(sum)>=std::abs(term)?(sum-next)+term:(term-next)+sum;sum=next;};
      for(size_t col=0;col<3;++col)for(double x:{double(coordinate[col]),double(local[col])}) {
        const double product=basis[col][row]*x;add(product);add(std::fma(basis[col][row],x,-product));
      }
      result[row]=sum+correction;
      const double spacing=std::max(std::abs(std::nextafter(result[row],INFINITY)-result[row]),std::abs(result[row]-std::nextafter(result[row],-INFINITY)));
      if(!std::isfinite(result[row]) || !std::isfinite(spacing) || spacing>minimum*1e-4)
        throw std::invalid_argument("native point world position loses source voxel precision; recenter source data");
    }
    return result;
  }
};

constexpr std::array<std::array<int,3>,6> Axes{{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}}};
constexpr std::array<std::array<uint32_t,3>,8> Faces{{{0,2,4},{2,1,4},{1,3,4},{3,0,4},{2,0,5},{1,2,5},{3,1,5},{0,3,5}}};
struct Record {openvdb::Vec3d world;double radius;Scalar scalar;glm::vec3 color;};
std::array<glm::vec3,6> marker(const Record& point,double scale) {
  const double radius=point.radius*scale;
  if(!std::isfinite(radius) || radius<std::numeric_limits<float>::min() || radius>std::numeric_limits<float>::max())
    throw std::invalid_argument("native point marker radius is outside usable Float display precision; rescale data");
  std::array<glm::vec3,6> result;
  for(size_t n=0;n<6;++n)for(size_t k=0;k<3;++k) {
    const double center=point.world[k]*scale,value=center+radius*Axes[n][k];const float represented=float(value);
    if(!std::isfinite(center) || !std::isfinite(value) || !std::isfinite(represented) ||
       std::abs(double(represented)-value)>radius*.001 || (Axes[n][k] && value==center))
      throw std::invalid_argument("native point marker cannot preserve radius in Float display coordinates; recenter or rescale data");
    result[n][k]=represented;
  }
  return result;
}
glm::vec3 direction(const openvdb::Vec3d& value) {
  for(size_t k=0;k<3;++k)if(!std::isfinite(value[k]))throw std::invalid_argument("selected vector color components must be finite");
  const double scale=std::max({std::abs(value[0]),std::abs(value[1]),std::abs(value[2])});
  if(!std::isfinite(scale))throw std::invalid_argument("selected vector color components must be finite");
  if(scale==0)return glm::vec3(.5f);
  const auto relative=value/scale;const double length=std::hypot(relative.x(),relative.y(),relative.z());
  return {float(.5+.5*relative.x()/length),float(.5+.5*relative.y()/length),float(.5+.5*relative.z()/length)};
}
bool validUtf8(std::string_view value) {
  for(size_t i=0;i<value.size();) {
    const auto c=uint8_t(value[i++]);if(c<128)continue;unsigned n;uint32_t code;
    if(c>=0xc2 && c<=0xdf){n=1;code=c&31;}else if(c>=0xe0 && c<=0xef){n=2;code=c&15;}
    else if(c>=0xf0 && c<=0xf4){n=3;code=c&7;}else return false;
    if(n>value.size()-i)return false;for(unsigned j=0;j<n;++j){const auto b=uint8_t(value[i++]);if((b&0xc0)!=0x80)return false;code=(code<<6)|(b&63);}
    if((n==1 && code<128)||(n==2 && code<2048)||(n==3 && code<65536)||code>0x10ffff||(code>=0xd800 && code<=0xdfff))return false;
  }return true;
}
glm::vec3 palette(std::string_view value) {
  if(!validUtf8(value))throw std::invalid_argument("selected string palette value contains invalid UTF-8");
  uint64_t hash=14695981039346656037ull;for(const auto byte:value){hash^=uint8_t(byte);hash*=1099511628211ull;}
  constexpr std::array<std::array<float,3>,12> colors{{{.89f,.29f,.2f},{.2f,.56f,.86f},{.25f,.72f,.42f},{.78f,.4f,.83f},
    {.95f,.65f,.2f},{.2f,.75f,.76f},{.86f,.4f,.6f},{.58f,.68f,.2f},{.55f,.45f,.87f},{.75f,.55f,.35f},{.3f,.68f,.6f},{.75f,.75f,.8f}}};
  const auto& color=colors[hash%colors.size()];return {color[0],color[1],color[2]};
}
double fraction(const Scalar& value,const Scalar& minimum,const Scalar& maximum) {
  return std::visit([&](auto x)->double {
    using T=decltype(x);const T lo=std::get<T>(minimum),hi=std::get<T>(maximum);
    if constexpr(std::is_same_v<T,bool>)return x?1:0;
    if(lo==hi)return .5;
    if constexpr(std::is_integral_v<T>)return double(uint64_t(x)-uint64_t(lo))/double(uint64_t(hi)-uint64_t(lo));
    else {
      const double d=double(x),a=double(lo),b=double(hi);
      if(a<0 && b>0){const double scale=std::max(-a,b);return (d/scale-a/scale)/(b/scale-a/scale);}
      return (d-a)/(b-a);
    }
  },value);
}

std::string text(double value){std::ostringstream s;s.imbue(std::locale::classic());s<<std::setprecision(17)<<value;return s.str();}
struct Properties {
  dotbim::Element element;long double bytes=0;const NativePointPreviewOptions& options;
  void add(std::string set,std::string name,std::string value,std::string type={}) {
    bytes+=4.0L*(set.size()+name.size()+value.size()+type.size())+512;
    if(bytes>options.maxMetadataBytes)throw std::invalid_argument("native point preview metadata/properties exceed maxMetadataBytes");
    element.properties.push_back({std::move(set),std::move(name),std::move(value),std::move(type)});
  }
};
long double mapReserve(const openvdb::MetaMap& map) {
  long double bytes=4096;for(auto i=map.beginMeta();i!=map.endMeta();++i)bytes+=16.0L*(i->first.size()+i->second->typeName().size()+i->second->size())+1024;
  return bytes;
}
void mapProperties(Properties& properties,const openvdb::MetaMap& map,const std::string& scope) {
  for(auto i=map.beginMeta();i!=map.endMeta();++i) {
    const auto& m=*i->second;std::string value;
    const bool archive=std::find(ArchiveKeys.begin(),ArchiveKeys.end(),i->first)!=ArchiveKeys.end();
    if(!archive && (dynamic_cast<const openvdb::StringMetadata*>(&m) || dynamic_cast<const openvdb::BoolMetadata*>(&m) ||
       dynamic_cast<const openvdb::Int32Metadata*>(&m) || dynamic_cast<const openvdb::Int64Metadata*>(&m) ||
       dynamic_cast<const openvdb::FloatMetadata*>(&m) || dynamic_cast<const openvdb::DoubleMetadata*>(&m) ||
       dynamic_cast<const openvdb::Vec3SMetadata*>(&m) || dynamic_cast<const openvdb::Vec3DMetadata*>(&m))) value=m.str();
    else value="Opaque native metadata; "+std::to_string(m.size())+" bytes; source unchanged";
    properties.add(scope,i->first,std::move(value),m.typeName());
  }
}

std::pair<std::string,long double> catalog(const Snapshot& snapshot,const std::string& selection,const NativePointPreviewOptions& o,const ProcessingOptions& p) {
  detail::RequireSeekableFile(snapshot.file);
  if(!openvdb::io::Archive::isDelayedLoadingEnabled())throw std::invalid_argument("native point preview requires delayed loading; unset OPENVDB_DISABLE_DELAYED_LOAD");
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument("native point preview requires OpenVDB delayed-loading support");
#else
  if(snapshot.encoded>=uintmax_t(std::numeric_limits<openvdb::Index64>::max()))throw std::invalid_argument("native point preview snapshot exceeds delayed-copy range");
  openvdb::io::File file(snapshot.file.string());file.setCopyMaxBytes(openvdb::Index64(snapshot.encoded)+1);file.open(true);
  std::set<std::string> descriptors;long double reserve=Fixed+snapshot.encoded;
  for(auto i=file.beginName();i!=file.endName();++i) {
    if(descriptors.size()>=o.maxGrids)throw std::invalid_argument("native point preview catalog exceeds maxGrids");
    const auto name=i.gridName();reserve+=4.0L*name.size()+256;budget(reserve,p,"native point preview catalog exceeds memory budget");
    if(!descriptors.insert(name).second)throw std::invalid_argument("native point preview input has colliding descriptors; rename grids first");
  }
  std::string chosen;
  for(auto i=file.beginName();i!=file.endName();++i) {
    const auto grid=file.readGridMetadata(i.gridName());
    budget(reserve+4*metadataBytes(*grid),p,"native point preview catalog metadata exceeds memory budget");
    if(!grid->isType<Grid>() || !GridSelectionMatches(selection,i.gridName(),grid->getName()))continue;
    if(!chosen.empty())throw std::invalid_argument("native point preview grid selector is ambiguous; use descriptor:<exact name>");
    chosen=i.gridName();if(selection.empty())break;
  }
  if(chosen.empty())throw std::invalid_argument("no matching PointDataGrid found for native preview");
  file.close();return {chosen,reserve};
#endif
}
#endif
} // namespace

dotbim::Model LoadNativePointsPreviewFromFile(const std::filesystem::path& input,const std::string& inputGrid,
    float importScale,const NativePointPreviewOptions& o,const ProcessingOptions& p,const PointAttributeOptions& limits) {
#if !defined(CONTAINER_HAS_OPENVDB) || !CONTAINER_HAS_OPENVDB
  (void)input;(void)inputGrid;(void)importScale;(void)o;(void)p;(void)limits;unavailable();
#else
  validateOptions(p,limits);if(!p.maxTriangles || !o.maxMetadataBytes || !o.maxGrids || o.maxGrids>4096 ||
    !std::isfinite(importScale) || importScale<=0 || !std::isfinite(o.radiusScale) || o.radiusScale<=0 ||
    (!o.radiusAttribute.empty() && o.radiusAttribute=="P") ||
    (o.radiusAttribute.empty() && (!std::isfinite(o.constantRadius) || o.constantRadius<=0)) ||
    !std::isfinite(o.opacity) || o.opacity<0 || o.opacity>1)
    throw std::invalid_argument("invalid native point preview scale/radius/opacity/catalog/metadata/triangle controls");
  if(o.colorMode!=NativePointColor::Uniform && o.colorMode!=NativePointColor::ScalarRamp && o.colorMode!=NativePointColor::VectorDirection && o.colorMode!=NativePointColor::StringPalette)
    throw std::invalid_argument("unknown native point preview color mode");
  for(const auto color:o.uniformColor)if(!std::isfinite(color) || color<0 || color>1)throw std::invalid_argument("native point uniform color must be finite in [0,1]");
  if(!o.radiusAttribute.empty())name(o.radiusAttribute);if(o.colorMode!=NativePointColor::Uniform)name(o.colorAttribute);
  const auto predicateReserve=selectionBytes(o.groups,limits,nullptr);budget(predicateReserve+Fixed,p,"native point preview options exceed memory budget");
  initialize();tbb::task_arena arena(1);
  return arena.execute([&] {
    Snapshot snapshot(input,p);const auto [descriptor,outerReserve]=catalog(snapshot,inputGrid,o,p);
    budget(outerReserve+predicateReserve+1,p,"native point preview catalog/source reserves exceed shared budget");
    auto native=p;native.maxInputBytes-=uintmax_t(std::ceil(outerReserve+predicateReserve));
    auto source=load(snapshot.file,"descriptor:"+descriptor,native,limits);
    bool archived=false;for(const auto* key:ArchiveKeys)archived|=bool((*source.grid)[key]);
    long double resident=outerReserve+predicateReserve+source.snapshot->encoded+4*source.decoded+8*source.metadata+Fixed;
    budget(resident+1,p,"native point preview retained source exceeds shared budget");
    if(archived) {
      auto nested=p;nested.maxInputBytes-=uintmax_t(std::ceil(resident));
      const auto counted=CountSelectedPoints(source.snapshot->file,o.groups,"descriptor:"+descriptor,nested,limits);
      if(!counted.archivedSchema)throw std::invalid_argument("native point preview empty-schema archive conflicts with native points");
      source.schema=counted.schema;
    } else (void)selectionBytes(o.groups,limits,&source.schema);
    for(const auto* leaf:source.leaves)validateChosen(*leaf,o);
    uint64_t selected=0,inactive=0;
    for(const auto* leaf:source.leaves){Predicate predicate(*leaf,o.groups);for(auto point=leaf->beginIndexAll();point;++point)if(predicate(*point)){++selected;inactive+=!leaf->isValueOn(point.getCoord());}}
    if(selected>limits.maxPoints || selected>p.maxTriangles/8 || selected>UINT32_MAX/24 || selected>UINT32_MAX/6)
      throw std::invalid_argument("native point marker count exceeds point/triangle/32-bit index limits");
    long double propertiesReserve=mapReserve(*source.grid)+predicateReserve+16384;
    if(!source.leaves.empty())propertiesReserve+=mapReserve(source.leaves.front()->attributeSet().descriptor().getMetadata());
    for(const auto& a:source.schema.attributes)propertiesReserve+=8.0L*(a.name.size()+a.valueType.size()+a.codec.size())+2048;
    for(const auto& group:source.schema.groups)propertiesReserve+=8.0L*group.size()+1024;
    if(propertiesReserve>o.maxMetadataBytes)throw std::invalid_argument("native point source/schema metadata reserve exceeds maxMetadataBytes");
    const long double geometry=selected*(6.0L*sizeof(Vertex)+24.0L*sizeof(uint32_t));
    budget(resident+3*(geometry+selected*static_cast<long double>(sizeof(Record)))+4*propertiesReserve+4*source.maxStringLength+Fixed,p,
      "native point prospective model/records/metadata workspace exceeds shared memory budget");
    Properties properties{{},0,o};auto& element=properties.element;
    element.meshId=1;element.geometryKind=dotbim::GeometryKind::Mesh;element.guid="vdb-native-points:"+descriptor;
    element.sourceId=descriptor;element.displayName=source.grid->getName();element.type="OpenVDB Native Points";
    element.materialName="OpenVDB Native Point Markers";element.materialIndex=0;element.color=glm::vec4(1);element.doubleSided=false;
    properties.add("OpenVDB","Descriptor",descriptor);properties.add("OpenVDB","Source point count",std::to_string(source.schema.pointCount));
    properties.add("OpenVDB","Selected point count",std::to_string(selected));properties.add("OpenVDB","Selected inactive points",std::to_string(inactive));
    properties.add("OpenVDB","Traversal","leaf_origin_xyz_then_local_index; includes inactive voxels");
    properties.add("OpenVDB","Preview","Physical radius triangle mesh markers; point-style controls do not apply");
    properties.add("OpenVDB","Radius",o.radiusAttribute.empty()?"Constant source-world radius "+text(o.constantRadius):"Native scalar "+o.radiusAttribute+" interpreted in source-world units");
    properties.add("OpenVDB","Radius scale",text(o.radiusScale));properties.add("OpenVDB","Import scale",text(importScale));
    properties.add("OpenVDB","Opacity",text(o.opacity));properties.add("OpenVDB","Color attribute",o.colorMode==NativePointColor::Uniform?"Uniform":o.colorAttribute);
    const char* modes[]{"Uniform RGB","Selected scalar range; exact integer differences; bool false/true","Signed native vector direction; zero neutral gray","FNV-1a UTF-8 bytes; fixed 12-color palette; collisions possible"};
    properties.add("OpenVDB","Color semantics",modes[size_t(o.colorMode)]);
    for(const auto& a:source.schema.attributes)properties.add("OpenVDB attributes",a.name,"type="+a.valueType+"; codec="+a.codec+"; stride="+std::to_string(a.stride)+"; hidden="+std::to_string(a.anyHidden)+"; native payload unchanged");
    for(const auto& group:source.schema.groups)properties.add("OpenVDB groups",group,"Native group membership retained in source");
    size_t list=0;for(const auto* names:{&o.groups.anyOf,&o.groups.allOf,&o.groups.noneOf}){for(const auto& n:*names)properties.add("OpenVDB selection",std::array<const char*,3>{"Any group","All group","Excluded group"}[list],n);++list;}
    mapProperties(properties,*source.grid,"OpenVDB metadata");
    if(!source.leaves.empty())mapProperties(properties,source.leaves.front()->attributeSet().descriptor().getMetadata(),"OpenVDB descriptor metadata");
    dotbim::Model model;model.importReport.sourceFormat="OpenVDB";model.importReport.sourceItemName="native point grid";model.importReport.sourceProductCount=1;
    model.unitMetadata.hasSourceUnits=true;model.unitMetadata.sourceUnits="OpenVDB world units";
    model.unitMetadata.hasImportScale=true;model.unitMetadata.importScale=importScale;model.unitMetadata.hasEffectiveImportScale=true;model.unitMetadata.effectiveImportScale=importScale;
    if(!selected) {
      ++model.importReport.skippedProductCount;++model.importReport.representationWarnings["OpenVDB.EmptyNativePoints"];
      model.importReport.diagnostics.push_back({"OpenVDB.EmptyNativePoints",1,1,element.guid,"Native point selection contains no records; source schema/metadata remain unchanged"});
      return model;
    }
    const Affine affine(source.grid->transform());std::vector<Record> records;records.reserve(size_t(selected));
    std::optional<Scalar> minimum,maximum;
    for(const auto* leaf:source.leaves) {
      Predicate predicate(*leaf,o.groups);points::AttributeHandle<openvdb::Vec3f> positions(leaf->constAttributeArray("P"),false);
      std::optional<ScalarHandle> radii,scalars;std::optional<points::AttributeHandle<openvdb::Vec3f>> vectorF;
      std::optional<points::AttributeHandle<openvdb::Vec3d>> vectorD;std::optional<points::StringAttributeHandle> strings;
      if(!o.radiusAttribute.empty())radii.emplace(leaf->constAttributeArray(o.radiusAttribute));
      if(o.colorMode==NativePointColor::ScalarRamp)scalars.emplace(leaf->constAttributeArray(o.colorAttribute));
      if(o.colorMode==NativePointColor::VectorDirection){const auto& a=leaf->constAttributeArray(o.colorAttribute);if(a.hasValueType<openvdb::Vec3f>())vectorF.emplace(a,false);else vectorD.emplace(a,false);}
      if(o.colorMode==NativePointColor::StringPalette)strings.emplace(leaf->constAttributeArray(o.colorAttribute),leaf->attributeSet().descriptor().getMetadata(),true);
      for(auto point=leaf->beginIndexAll();point;++point)if(predicate(*point)) {
        Record record;record.world=affine.world(point.getCoord(),positions.get(*point));
        const double radius=radii?number(radii->get(*point)):o.constantRadius;record.radius=radius*o.radiusScale;
        if(!(radius>0) || !(record.radius>0) || !std::isfinite(record.radius))throw std::invalid_argument("selected point radius/scale product must be positive finite without underflow");
        record.color={o.uniformColor[0],o.uniformColor[1],o.uniformColor[2]};
        if(scalars) {
          record.scalar=scalars->get(*point);(void)number(record.scalar);
          if(!minimum){minimum=record.scalar;maximum=record.scalar;}
          else std::visit([&](auto x){using T=decltype(x);if(x<std::get<T>(*minimum))*minimum=x;if(x>std::get<T>(*maximum))*maximum=x;},record.scalar);
        }
        if(vectorF)record.color=direction(openvdb::Vec3d(vectorF->get(*point)));if(vectorD)record.color=direction(vectorD->get(*point));
        if(strings)record.color=palette(strings->get(*point));
        (void)marker(record,importScale);records.push_back(std::move(record));
      }
    }
    if(records.size()!=selected)throw std::runtime_error("native point preview record count changed during decoding");
    if(minimum)for(auto& record:records){const auto t=std::clamp(fraction(record.scalar,*minimum,*maximum),0.,1.);record.color={float(std::lerp(.1,1.,t)),float(std::lerp(.25,.2,t)),float(std::lerp(1.,.05,t))};}
    model.vertices.reserve(size_t(selected)*6);model.indices.reserve(size_t(selected)*24);
    glm::dvec3 low(INFINITY),high(-INFINITY);
    for(const auto& record:records) {
      const auto vertices=marker(record,importScale);const auto first=uint32_t(model.vertices.size());
      for(size_t n=0;n<6;++n) {
        Vertex vertex;vertex.position=vertices[n];vertex.color=record.color;vertex.colorAlpha=1;
        vertex.normal={float(Axes[n][0]),float(Axes[n][1]),float(Axes[n][2])};
        const auto reference=std::abs(vertex.normal.z)<.9f?glm::vec3(0,0,1):glm::vec3(0,1,0);
        vertex.tangent=glm::vec4(glm::normalize(glm::cross(reference,vertex.normal)),1);model.vertices.push_back(vertex);
        low=glm::min(low,glm::dvec3(vertex.position));high=glm::max(high,glm::dvec3(vertex.position));
      }
      for(const auto& face:Faces)for(const auto n:face)model.indices.push_back(first+n);
    }
    const glm::vec3 center((low+high)*.5);const auto extent=glm::max(glm::abs(high-glm::dvec3(center)),glm::abs(low-glm::dvec3(center)));
    const double length=std::hypot(extent.x,extent.y,extent.z);float bound=float(length);if(bound<length)bound=std::nextafter(bound,INFINITY);
    if(!std::isfinite(bound))throw std::invalid_argument("native point preview bounds exceed Float model range");
    model.meshRanges.push_back({1,0,uint32_t(model.indices.size()),center,bound});model.elements.push_back(std::move(element));
    dotbim::Material material;material.pbr.baseColor=glm::vec4(1);material.pbr.opacityFactor=o.opacity;
    material.pbr.alphaMode=o.opacity<1?container::material::AlphaMode::Blend:container::material::AlphaMode::Opaque;
    material.pbr.roughnessFactor=.75f;material.pbr.attenuationDistance=std::numeric_limits<float>::max();model.materials.push_back(std::move(material));
    ++model.importReport.importedProductCount;return model;
  });
#endif
}
} // namespace container::geometry::vdb
