#include <Container/geometry/GltfMaterialVariants.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace container::geometry::gltf {
namespace {
constexpr const char* kExtension="KHR_materials_variants";
[[noreturn]] void fail(const std::string& context,const std::string& reason) {
  throw std::runtime_error("glTF material variants: "+context+": "+reason);
}
const tinygltf::Value::Object& object(const tinygltf::Value& value,const std::string& context) {
  if (!value.IsObject()) fail(context,"expected an object");
  return value.Get<tinygltf::Value::Object>();
}
const tinygltf::Value& field(const tinygltf::Value& value,const char* name,const std::string& context) {
  const auto& values=object(value,context);
  const auto found=values.find(name);
  if (found==values.end()) fail(context,std::string("missing ")+name);
  return found->second;
}
const tinygltf::Value::Array& array(const tinygltf::Value& value,const std::string& context) {
  if (!value.IsArray()) fail(context,"expected an array");
  return value.Get<tinygltf::Value::Array>();
}
void property(const tinygltf::Value& value,const std::string& context,bool named=false) {
  const auto& fields=object(value,context);
  if (const auto extensions=fields.find("extensions");extensions!=fields.end() && !extensions->second.IsObject())
    fail(context,"extensions must be an object");
  if (named) if (const auto name=fields.find("name");name!=fields.end() && !name->second.IsString())
    fail(context,"name must be a string");
}
size_t index(const tinygltf::Value& value,size_t bound,const std::string& context) {
  if (!value.IsInt() || value.Get<int>()<0 || size_t(value.Get<int>())>=bound)
    fail(context,"index must be a nonnegative integer within its source table");
  return size_t(value.Get<int>());
}
std::string context(size_t mesh,size_t primitive) {
  return "mesh "+std::to_string(mesh)+" primitive "+std::to_string(primitive);
}
int32_t material(int32_t source,size_t count,std::span<const int32_t> mapping,const std::string& label) {
  if (source<0 || size_t(source)>=count) fail(label,"source material index is out of bounds");
  const auto result=mapping.empty()?source:mapping[size_t(source)];
  if (result<0) fail(label,"material translation is missing");
  return result;
}
void table(size_t count,std::span<const int32_t> mapping) {
  if (!mapping.empty() && mapping.size()!=count) fail("material translation","table must cover all imported materials");
  for (const auto entry:mapping)
    if (entry<0) fail("material translation","complete imported material table contains a missing runtime ID");
}

struct GrowthBudget {
  const MaterialVariantGrowthLimits& limits;
  uint64_t bytes{},values{},steps{};
  explicit GrowthBudget(const MaterialVariantGrowthLimits& value):limits(value) {
    for(const auto bound:{limits.maxMaterials,limits.maxMeshes,limits.maxPrimitives,
        limits.maxVariants,limits.maxMappings,limits.maxOwnedBytes,
        limits.maxMetadataValues,limits.maxTraversalSteps})
      if(!bound || bound==UINT64_MAX) fail("material growth","limits must be finite and positive");
    if(!limits.maxMetadataDepth || limits.maxMetadataDepth>128)
      fail("material growth","metadata depth limit must be in 1..128");
  }
  void work(uint64_t count=1) {
    if(count>limits.maxTraversalSteps-steps) fail("material growth","traversal budget exceeded");
    steps+=count;
  }
  void add(uint64_t count,uint64_t width=1) {
    if(width && count>(limits.maxOwnedBytes-bytes)/width)
      fail("material growth","copied storage budget exceeded");
    bytes+=count*width;
  }
  void metadata(const tinygltf::Value& root) {
    // Fixed-depth DFS bounds validation scratch independently of metadata width.
    struct Frame {
      const tinygltf::Value* value{};
      size_t next{};
      tinygltf::Value::Object::const_iterator member;
      bool entered{};
    };
    std::array<Frame,129> frames;
    size_t depth{};frames[0].value=&root;
    for(;;) {
      auto& frame=frames[depth];const auto& value=*frame.value;
      if(!frame.entered) {
        work();
        if(values==limits.maxMetadataValues) fail("material growth","metadata value budget exceeded");
        ++values;add(1,sizeof(tinygltf::Value));frame.entered=true;
        // TinyGLTF's public Get<T>() also exposes inactive containers. A
        // malformed Value must not hide uncharged copy payload behind its tag.
        if((!value.IsString() && !value.Get<std::string>().empty()) ||
            (!value.IsBinary() && !value.Get<std::vector<unsigned char>>().empty()) ||
            (!value.IsArray() && !value.Get<tinygltf::Value::Array>().empty()) ||
            (!value.IsObject() && !value.Get<tinygltf::Value::Object>().empty()) ||
            !(value.IsString() || value.IsBinary() || value.IsArray() || value.IsObject() ||
              value.IsBool() || value.IsNumber() || value.Type()==tinygltf::NULL_TYPE))
          fail("material growth","malformed retained metadata storage/type");
        if(value.IsString()) add(value.Get<std::string>().size()+uint64_t{1});
        else if(value.IsBinary()) add(value.Get<std::vector<unsigned char>>().size());
        else if(value.IsNumber() && !std::isfinite(value.GetNumberAsDouble()))
          fail("material growth","nonfinite retained metadata");
        if(value.IsObject()) frame.member=value.Get<tinygltf::Value::Object>().begin();
      }
      const tinygltf::Value* child{};
      if(value.IsArray()) {
        const auto& children=value.Get<tinygltf::Value::Array>();
        if(frame.next<children.size()) child=&children[frame.next++];
      } else if(value.IsObject()) {
        const auto& children=value.Get<tinygltf::Value::Object>();
        if(frame.member!=children.end()) {
          add(128);add(frame.member->first.size()+uint64_t{1});
          child=&frame.member->second;++frame.member;
        }
      }
      if(child) {
        if(depth==limits.maxMetadataDepth) fail("material growth","metadata depth budget exceeded");
        frames[++depth]=Frame{};frames[depth].value=child;
      } else {
        if(!depth) break;
        --depth;
      }
    }
  }
};
} // namespace

const MaterialVariantPrimitive& MaterialVariantLibrary::primitive(size_t mesh,size_t local) const {
  if (meshOffsets_.empty() || mesh>=meshOffsets_.size()-1 || local>=meshOffsets_[mesh+1]-meshOffsets_[mesh])
    fail(context(mesh,local),"source primitive index is out of bounds");
  return primitives_[meshOffsets_[mesh]+local];
}

std::optional<MaterialVariantLibrary> MaterialVariantLibrary::withExtendedMaterialCount(
    size_t expectedCurrentMaterialCount,size_t newMaterialCount,
    const MaterialVariantGrowthLimits& limits) const {
  if(newMaterialCount<materialCount_)
    fail("material growth","source material count cannot shrink without tail-removal authority");
  validateMaterialCountChange(expectedCurrentMaterialCount,newMaterialCount,limits);
  if(newMaterialCount==materialCount_) return std::nullopt;
  std::optional<MaterialVariantLibrary> result(std::in_place,*this);
  result->materialCount_=newMaterialCount;
  return result;
}

MaterialVariantLibrary MaterialVariantLibrary::withRemovedLastMaterial(
    size_t expectedCurrentMaterialCount,const MaterialVariantGrowthLimits& limits) const {
  if(!materialCount_) fail("material removal","the source material table is empty");
  validateMaterialCountChange(expectedCurrentMaterialCount,materialCount_-1,limits);
  MaterialVariantLibrary result(*this);
  --result.materialCount_;
  return result;
}

void MaterialVariantLibrary::validateMaterialCountChange(size_t expectedCurrentMaterialCount,
    size_t newMaterialCount,const MaterialVariantGrowthLimits& limits) const {
  GrowthBudget budget(limits);
  if(expectedCurrentMaterialCount!=materialCount_)
    fail("material growth","current source material count does not match the library");
  if(newMaterialCount>size_t(INT32_MAX) || materialCount_>size_t(INT32_MAX) ||
      newMaterialCount>limits.maxMaterials || materialCount_>limits.maxMaterials)
    fail("material growth","source material count exceeds its identity/storage limit");
  // Growth must not legitimize a malformed reference into the new tail;
  // removal must reject every reference to the disappearing last identity.
  const auto referenceCount=std::min(materialCount_,newMaterialCount);
  if(names_.size()>limits.maxVariants || primitives_.size()>limits.maxPrimitives ||
      (meshOffsets_.size() && meshOffsets_.size()-1>limits.maxMeshes))
    fail("material growth","variant/primitive/mesh count limit exceeded");
  if(primitiveExtensions_.size()!=primitives_.size())
    fail("material growth","retained primitive extension table has the wrong shape");
  if(meshOffsets_.empty()) {
    if(!primitives_.empty()) fail("material growth","primitive table has no mesh offsets");
  } else if(meshOffsets_.front()!=0 || meshOffsets_.back()!=primitives_.size())
    fail("material growth","mesh offsets do not cover the primitive table");
  budget.add(1,sizeof(MaterialVariantLibrary));
  budget.add(names_.size(),sizeof(std::string));
  for(const auto& name:names_) {budget.work();budget.add(name.size()+uint64_t{1});}
  budget.add(primitives_.size(),sizeof(MaterialVariantPrimitive));
  budget.add(meshOffsets_.size(),sizeof(size_t));
  budget.add(primitiveExtensions_.size(),sizeof(std::optional<tinygltf::Value>));
  uint64_t mappingCount{};
  for(size_t mesh=0;mesh+1<meshOffsets_.size();++mesh) {
    budget.work();
    if(meshOffsets_[mesh]>meshOffsets_[mesh+1] || meshOffsets_[mesh+1]>primitives_.size())
      fail("material growth","mesh offsets are not monotone bounded ranges");
    for(size_t flat=meshOffsets_[mesh];flat<meshOffsets_[mesh+1];++flat) {
      budget.work();const auto& primitive=primitives_[flat];
      if(primitive.sourceMesh!=mesh || primitive.sourcePrimitive!=flat-meshOffsets_[mesh])
        fail("material growth","primitive identities do not match their mesh offsets");
      if(primitive.defaultMaterial< -1 || (primitive.defaultMaterial>=0 && size_t(primitive.defaultMaterial)>=referenceCount))
        fail("material growth","primitive default references a material outside the retained range");
      if(primitive.mappings.size()>limits.maxMappings-mappingCount)
        fail("material growth","mapping count limit exceeded");
      mappingCount+=primitive.mappings.size();budget.add(primitive.mappings.size(),sizeof(std::pair<size_t,int32_t>));
      std::optional<size_t> previous;
      for(const auto& [variant,mapped]:primitive.mappings) {
        budget.work();
        if(variant>=names_.size() || (previous && variant<=*previous) || mapped<0 || size_t(mapped)>=referenceCount)
          fail("material growth","primitive mapping has invalid/duplicate identities");
        previous=variant;
      }
      if(primitiveExtensions_[flat]) budget.metadata(*primitiveExtensions_[flat]);
      else if(!primitive.mappings.empty()) fail("material growth","parsed mappings have no retained metadata");
    }
  }
  if(rootExtension_) budget.metadata(*rootExtension_);
  else if(required_ || !names_.empty() || mappingCount)
    fail("material growth","variant context has no retained root extension");

  // Check the retained raw extension and derived lookup tables against each
  // other. This also refuses malformed const-cast/table corruption before a
  // new material range could make an old out-of-range reference appear valid.
  if(rootExtension_) {
    property(*rootExtension_,"material growth root");
    const auto& variants=array(field(*rootExtension_,"variants","material growth root"),"material growth variants");
    if(variants.empty() || variants.size()!=names_.size()) fail("material growth","variant names disagree with retained metadata");
    for(size_t i=0;i<variants.size();++i) {
      budget.work();property(variants[i],"material growth variant",true);
      const auto& name=field(variants[i],"name","material growth variant");
      if(!name.IsString() || name.Get<std::string>()!=names_[i])
        fail("material growth","variant name differs from retained metadata");
    }
  }
  budget.add(names_.size(),sizeof(size_t));
  std::vector<size_t> seen(names_.size(),SIZE_MAX);
  for(size_t flat=0;flat<primitives_.size();++flat) if(primitiveExtensions_[flat]) {
    budget.work();const auto& original=*primitiveExtensions_[flat];
    property(original,"material growth primitive");
    const auto& mappings=array(field(original,"mappings","material growth primitive"),"material growth mappings");
    if(mappings.empty()) fail("material growth","retained mapping table is empty");
    size_t pairs{};
    for(const auto& entry:mappings) {
      budget.work();property(entry,"material growth mapping",true);
      const auto mapped=index(field(entry,"material","material growth mapping"),referenceCount,"material growth mapping material");
      const auto& variants=array(field(entry,"variants","material growth mapping"),"material growth mapping variants");
      if(variants.empty()) fail("material growth","retained mapping has no variants");
      for(const auto& item:variants) {
        budget.work();const auto variant=index(item,names_.size(),"material growth mapping variant");
        if(seen[variant]==flat) fail("material growth","retained metadata assigns a variant more than once");
        seen[variant]=flat;
        const auto& parsed=primitives_[flat].mappings;
        const auto found=std::ranges::lower_bound(parsed,variant,{},&std::pair<size_t,int32_t>::first);
        if(found==parsed.end() || found->first!=variant || found->second!=int32_t(mapped))
          fail("material growth","parsed mapping disagrees with retained metadata");
        ++pairs;
      }
    }
    if(pairs!=primitives_[flat].mappings.size()) fail("material growth","retained mapping table is incomplete");
  }
}

MaterialVariantLibrary ParseMaterialVariants(const tinygltf::Model& source) {
  MaterialVariantLibrary result;
  result.materialCount_=source.materials.size();
  result.required_=std::ranges::find(source.extensionsRequired,std::string(kExtension))!=source.extensionsRequired.end();
  if (result.materialCount_>size_t(std::numeric_limits<int32_t>::max())) fail("materials","too many source materials");
  const auto root=source.extensions.find(kExtension);
  bool anyMapping=false;
  for (const auto& mesh:source.meshes) for (const auto& primitive:mesh.primitives)
    anyMapping=anyMapping || primitive.extensions.contains(kExtension);
  const bool declared=std::ranges::find(source.extensionsUsed,std::string(kExtension))!=source.extensionsUsed.end();
  if ((root!=source.extensions.end() || anyMapping) && !declared) fail("root","extension is missing from extensionsUsed");
  if (anyMapping && root==source.extensions.end()) fail("root","primitive mappings require a root variants table");
  if (root!=source.extensions.end()) {
    property(root->second,"root");
    const auto& names=array(field(root->second,"variants","root"),"root variants");
    if (names.empty()) fail("root variants","at least one variant is required");
    for (size_t i=0;i<names.size();++i) {
      const auto label="variant "+std::to_string(i);
      property(names[i],label,true);
      const auto& name=field(names[i],"name",label);
      if (!name.IsString()) fail(label,"name must be a string");
      result.names_.push_back(name.Get<std::string>());
    }
    result.rootExtension_=std::make_shared<const tinygltf::Value>(root->second);
  } else if (declared || result.required_) {
    fail("root","declared extension requires a root variants table");
  }
  for (size_t m=0;m<source.meshes.size();++m) {
    result.meshOffsets_.push_back(result.primitives_.size());
    for (size_t p=0;p<source.meshes[m].primitives.size();++p) {
      const auto& primitive=source.meshes[m].primitives[p];
      const auto label=context(m,p);
      if (primitive.material < -1 || (primitive.material>=0 && size_t(primitive.material)>=source.materials.size()))
        fail(label,"default material index is out of bounds");
      MaterialVariantPrimitive parsed{m,p,primitive.material,{}};
      std::optional<tinygltf::Value> original;
      if (const auto ext=primitive.extensions.find(kExtension);ext!=primitive.extensions.end()) {
        property(ext->second,label);
        const auto& mappings=array(field(ext->second,"mappings",label),label+" mappings");
        if (mappings.empty()) fail(label,"at least one mapping is required");
        std::set<size_t> seen;
        for (size_t i=0;i<mappings.size();++i) {
          const auto mappingLabel=label+" mapping "+std::to_string(i);
          property(mappings[i],mappingLabel,true);
          const auto mapped=index(field(mappings[i],"material",mappingLabel),source.materials.size(),mappingLabel+" material");
          const auto& variants=array(field(mappings[i],"variants",mappingLabel),mappingLabel+" variants");
          if (variants.empty()) fail(mappingLabel,"at least one variant index is required");
          for (size_t v=0;v<variants.size();++v) {
            const auto variant=index(variants[v],result.names_.size(),mappingLabel+" variant "+std::to_string(v));
            if (!seen.insert(variant).second) fail(mappingLabel,"variant index is assigned more than once in this primitive");
            parsed.mappings.emplace_back(variant,int32_t(mapped));
          }
        }
        std::ranges::sort(parsed.mappings);
        original=ext->second;
      }
      result.primitives_.push_back(std::move(parsed));
      result.primitiveExtensions_.push_back(std::move(original));
    }
  }
  result.meshOffsets_.push_back(result.primitives_.size());
  return result;
}

MaterialVariantSelectionPlan planMaterialVariantSelection(const MaterialVariantLibrary& library,
    std::optional<size_t> selectedVariant,std::span<const int32_t> mapping,int32_t implicitDefaultRuntime) {
  if (selectedVariant && *selectedVariant>=library.names().size()) fail("selection","variant index is out of bounds");
  if (implicitDefaultRuntime<0) fail("selection","implicit default material must have a runtime ID");
  table(library.materialCount(),mapping);
  MaterialVariantSelectionPlan result; result.selectedVariant=selectedVariant;
  result.assignments.reserve(library.primitives().size());
  for (const auto& primitive:library.primitives()) {
    auto source=primitive.defaultMaterial;
    if (selectedVariant) {
      const auto found=std::ranges::lower_bound(primitive.mappings,*selectedVariant,{},&std::pair<size_t,int32_t>::first);
      if (found!=primitive.mappings.end() && found->first==*selectedVariant) source=found->second;
    }
    const auto runtime=source<0?implicitDefaultRuntime:material(source,library.materialCount(),mapping,context(primitive.sourceMesh,primitive.sourcePrimitive));
    result.assignments.push_back({primitive.sourceMesh,primitive.sourcePrimitive,source,runtime});
  }
  return result;
}

void CopyMaterialVariantMetadataForExport(tinygltf::Model& destination,const MaterialVariantLibrary& library,
    std::span<const int32_t> mapping,std::span<const MaterialVariantExportPrimitive> primitives) {
  if (!library.hasVariants()) return;
  table(library.materialCount(),mapping);
  struct Replacement { size_t mesh{},primitive{}; std::optional<tinygltf::Value> extension; };
  std::vector<Replacement> replacements;
  std::set<std::pair<size_t,size_t>> seen;
  for (const auto& primitive:primitives) {
    (void)library.primitive(primitive.sourceMesh,primitive.sourcePrimitive);
    if (primitive.destinationMesh>=destination.meshes.size() ||
        primitive.destinationPrimitive>=destination.meshes[primitive.destinationMesh].primitives.size())
      fail("export","destination primitive index is out of bounds");
    if (!seen.emplace(primitive.destinationMesh,primitive.destinationPrimitive).second)
      fail("export","destination primitive is specified more than once");
    auto original=library.primitiveExtensions_[library.meshOffsets_[primitive.sourceMesh]+primitive.sourcePrimitive];
    if (original) {
      auto fields=original->Get<tinygltf::Value::Object>();
      auto mappings=fields.at("mappings").Get<tinygltf::Value::Array>();
      for (auto& item:mappings) {
        auto fields=item.Get<tinygltf::Value::Object>();
        const auto translated=material(fields.at("material").Get<int>(),library.materialCount(),mapping,"export mapping");
        if (size_t(translated)>=destination.materials.size()) fail("export mapping","translated material exceeds destination table");
        fields["material"]=tinygltf::Value(translated); item=tinygltf::Value(std::move(fields));
      }
      fields["mappings"]=tinygltf::Value(std::move(mappings)); original=tinygltf::Value(std::move(fields));
    }
    replacements.push_back({primitive.destinationMesh,primitive.destinationPrimitive,std::move(original)});
  }
  // Prepare all potentially allocating metadata before editing destination.
  auto root=destination.extensions;
  root[kExtension]=*library.rootExtension_;
  auto used=destination.extensionsUsed;
  if (std::ranges::find(used,std::string(kExtension))==used.end()) used.emplace_back(kExtension);
  auto required=destination.extensionsRequired;
  if (library.required_ && std::ranges::find(required,std::string(kExtension))==required.end()) required.emplace_back(kExtension);
  std::vector<tinygltf::ExtensionMap> maps;
  maps.reserve(replacements.size());
  for (const auto& replacement:replacements) {
    auto map=destination.meshes[replacement.mesh].primitives[replacement.primitive].extensions;
    if (replacement.extension) map[kExtension]=*replacement.extension;
    else map.erase(kExtension);
    maps.push_back(std::move(map));
  }
  destination.extensions.swap(root); destination.extensionsUsed.swap(used); destination.extensionsRequired.swap(required);
  destination.extensions_json_string.clear();
  for (size_t i=0;i<replacements.size();++i) {
    auto& primitive=destination.meshes[replacements[i].mesh].primitives[replacements[i].primitive];
    primitive.extensions.swap(maps[i]); primitive.extensions_json_string.clear();
  }
}
} // namespace container::geometry::gltf
