namespace point_neighbors_cli {
bool utf8(std::string_view text) {
  for(size_t i=0;i<text.size();) {
    const auto a=static_cast<unsigned char>(text[i++]);if(a<0x80)continue;
    size_t remaining;unsigned char low=0x80,high=0xbf;
    if(a>=0xc2 && a<=0xdf)remaining=1;
    else if(a>=0xe0 && a<=0xef) {remaining=2;if(a==0xe0)low=0xa0;if(a==0xed)high=0x9f;}
    else if(a>=0xf0 && a<=0xf4) {remaining=3;if(a==0xf0)low=0x90;if(a==0xf4)high=0x8f;}
    else return false;
    if(remaining>text.size()-i)return false;
    const auto first=static_cast<unsigned char>(text[i++]);if(first<low || first>high)return false;
    while(--remaining) {const auto c=static_cast<unsigned char>(text[i++]);if(c<0x80 || c>0xbf)return false;}
  }
  return true;
}
void reportPreflight(const vdb::PointNeighborBatch& batch,size_t queries,uintmax_t maximum) {
  long double reserve=4.0L*1024*1024+queries*1024.0L+batch.levels.size()*1024.0L;
  const auto text=[&](const std::string& value) {
    if(!utf8(value))throw std::invalid_argument("point-neighbor JSON cannot encode invalid native UTF-8 schema/metadata; native source remains unchanged");
    reserve+=16.0L*value.size()+256;
  };
  text(batch.schema.gridName);text(batch.diagnostic);
  for(const auto& a:batch.schema.attributes) {reserve+=2048;text(a.name);text(a.valueType);text(a.codec);}
  for(const auto& name:batch.schema.groups)text(name);
  for(const auto* metadata:{&batch.gridMetadata,&batch.fileMetadata,&batch.descriptorMetadata})
    for(const auto& item:*metadata) {
      reserve+=1024;text(item.name);text(item.type);
      reserve+=16.0L*item.value.size()+256; // Native archive Strings can be binary.
    }
  for(const auto& hits:batch.queries)reserve+=hits.size()*2048.0L;
  for(const auto& bucket:batch.buckets)reserve+=1024+bucket.points.size()*768.0L;
  // Includes the live native result, JSON nodes/keys, escaped text and dump
  // buffer. Native source/index scratch has retired before this conversion.
  if(!std::isfinite(reserve) || reserve>maximum)
    throw std::invalid_argument("point-neighbor JSON report exceeds --max-input-bytes; reduce queries/neighbors/buckets or raise the shared byte cap");
}
nlohmann::json reference(const vdb::NativePointReference& r) {
  return {{"ordinal",r.ordinal},{"leafOrigin",r.leafOrigin},{"localIndex",r.localIndex}};
}
nlohmann::json metadata(const std::vector<vdb::PointNeighborMetadata>& source) {
  auto output=nlohmann::json::array();
  for(const auto& m:source) {
    const bool valid=utf8(m.value);
    nlohmann::json item{{"name",m.name},{"type",m.type},{"value",valid?nlohmann::json(m.value):nlohmann::json(nullptr)},
        {"valueValidUtf8",valid},{"nativeBytes",m.nativeBytes},{"opaque",m.opaque}};
    if(!valid) {
      std::string hex;hex.reserve(m.value.size()*2);constexpr char digits[]="0123456789abcdef";
      for(const unsigned char c:m.value) {hex.push_back(digits[c>>4]);hex.push_back(digits[c&15]);}
      // Exact bytes of the API's metadata text summary. This is not an export
      // of an opaque metadata object's underlying serialized native payload.
      item["valueHex"]=std::move(hex);item["valueHexEncoding"]="metadata-summary-bytes";
    }
    output.push_back(std::move(item));
  }
  return output;
}
} // namespace point_neighbors_cli

std::optional<int> runPointNeighborsCommand(int argc,char** argv) {
  if(argc<2)return {};
  const std::string_view command=argv[1];const bool writeMask=command=="--indexed-point-mask";
  if(!writeMask && command!="--point-neighbors")return {};
  const int start=writeMask?4:3;
  if(argc<start)throw std::invalid_argument("point indexing requires INPUT [OUTPUT] before options");
  for(int i=2;i<start;++i)if(!argv[i][0] || std::string_view(argv[i]).starts_with("--") || std::string_view(argv[i])=="-h")
    throw std::invalid_argument("point indexing requires INPUT [OUTPUT] before options");
  vdb::PointNeighborOptions o;vdb::IndexedPointMaskOptions mask;
  vdb::PointAttributeOptions attributes;vdb::ProcessingOptions p;
  std::string grid,mode="centers";std::vector<vdb::PointNeighborQuery> queries;
  std::set<std::string> seen;std::array<std::set<std::string>,3> groupNames;
  const auto argument=[&](int& i,const std::string& option) {
    const auto text=value(argc,argv,i,option);
    if(text.empty() || std::string_view(text).starts_with("--") || text=="-h")
      throw std::invalid_argument("missing nonempty value for "+option);
    return text;
  };
  const auto positive=[&](int& i,const std::string& option,uint64_t maximum) {
    const auto n=count(argument(i,option),option);
    if(n>maximum)throw std::invalid_argument("point indexing count exceeds range: "+option);return n;
  };
  const auto voxel=[&](int& i,const std::string& option) {
    const auto n=number(argument(i,option),option);
    if(n<1e-6 || n>1e6)throw std::invalid_argument("point indexing voxel size must be in [1e-6,1e6]");return n;
  };
  for(int i=start;i<argc;++i) {
    const std::string arg=argv[i],key=arg=="--input-grid"?"--grid":arg;
    if(arg=="--neighbor-query" && !writeMask) {
      if(queries.size()>=1'000'000)throw std::invalid_argument("neighbor query list exceeds 1000000");
      vdb::PointNeighborQuery q;for(auto& x:q.center)x=number(argument(i,arg),arg);
      q.radius=number(argument(i,arg),arg);
      if(q.radius<0 || q.radius>1e6)throw std::invalid_argument("neighbor query radius must be in [0,1e6]");
      queries.push_back(q);continue;
    }
    if(arg=="--any-group" || arg=="--all-group" || arg=="--exclude-group") {
      const auto name=argument(i,arg);const size_t index=arg=="--any-group"?0:arg=="--all-group"?1:2;
      if(groupNames[index].size()>=4096 || !groupNames[index].insert(name).second)
        throw std::invalid_argument("duplicate/excessive point index group predicate: "+arg);
      (index==0?o.groups.anyOf:index==1?o.groups.allOf:o.groups.noneOf).push_back(name);continue;
    }
    if(!seen.insert(key).second)throw std::invalid_argument("duplicate point-index option: "+arg);
    if(arg=="--neighbor-mode" && !writeMask) {
      mode=argument(i,arg);
      if(mode=="centers")o.mode=vdb::PointNeighborMode::CentersWithinRadius;
      else if(mode=="particles")o.mode=vdb::PointNeighborMode::ParticleIntersections;
      else throw std::invalid_argument("--neighbor-mode requires centers or particles");
    } else if(arg=="--neighbor-buckets" && !writeMask)o.includeBuckets=true;
    else if(arg=="--neighbor-index-voxel-size" && !writeMask)o.indexVoxelSize=voxel(i,arg);
    else if(arg=="--neighbor-atlas-levels" && !writeMask)o.maxAtlasLevels=uint32_t(positive(i,arg,32));
    else if(arg=="--neighbor-radius-attribute" && !writeMask)o.radiusAttribute=argument(i,arg);
    else if(arg=="--neighbor-radius" && !writeMask) {
      o.constantRadius=number(argument(i,arg),arg);
      if(!(o.constantRadius>0) || o.constantRadius>1e6)throw std::invalid_argument("--neighbor-radius requires (0,1e6]");
    } else if(arg=="--max-neighbor-queries" && !writeMask)o.maxQueries=size_t(positive(i,arg,1'000'000));
    else if(arg=="--max-neighbors" && !writeMask)o.maxNeighbors=positive(i,arg,10'000'000);
    else if(arg=="--indexed-voxel-size" && writeMask)mask.voxelSize=voxel(i,arg);
    else if(arg=="--indexed-origin" && writeMask)for(auto& x:mask.originWorld)x=number(argument(i,arg),arg);
    else if(arg=="--indexed-counts" && writeMask)mask.includeCounts=true;
    else if(arg=="--indexed-count-name" && writeMask)mask.countGridName=argument(i,arg);
    else if(arg=="--max-point-index-work") {
      const auto n=count(argument(i,arg),arg);o.maxWork=n;mask.maxWork=n;
    } else if(arg=="--max-point-index-grids") {
      const auto n=size_t(positive(i,arg,4096));o.maxGrids=n;mask.maxGrids=n;
    } else if(arg=="--max-point-index-metadata") {
      const auto n=count(argument(i,arg),arg);o.maxMetadataBytes=n;mask.maxMetadataBytes=n;
    } else if(arg=="--max-points")attributes.maxPoints=size_t(positive(i,arg,uint64_t(INT64_MAX)));
    else if(arg=="--max-attributes")attributes.maxAttributes=size_t(positive(i,arg,1024));
    else if(arg=="--max-groups")attributes.maxGroups=size_t(positive(i,arg,4096));
    else if(arg=="--max-string-bytes")attributes.maxStringBytes=count(argument(i,arg),arg);
    else if(key=="--grid")grid=argument(i,arg);
    else if(arg=="--grid-name" && writeMask)p.gridName=argument(i,arg);
    else if(arg=="--max-voxels")p.maxVoxelCount=count(argument(i,arg),arg);
    else if(arg=="--max-input-bytes")p.maxInputBytes=count(argument(i,arg),arg);
    else throw std::invalid_argument("unknown/incompatible point-index option: "+arg);
  }
  const bool particles=o.mode==vdb::PointNeighborMode::ParticleIntersections;
  if((!particles && (seen.contains("--neighbor-radius") || seen.contains("--neighbor-radius-attribute") || seen.contains("--neighbor-atlas-levels"))) ||
      (seen.contains("--neighbor-radius") && seen.contains("--neighbor-radius-attribute")) ||
      (writeMask && seen.contains("--indexed-count-name") && !mask.includeCounts) ||
      (!writeMask && queries.empty() && !o.includeBuckets) || queries.size()>o.maxQueries)
    throw std::invalid_argument("point-index options do not apply: query or buckets required; radius/atlas controls require particles; count name requires counts");
  if(writeMask) {
    mask.groups=o.groups;
    const auto r=vdb::WriteIndexedPointMask(path(argv[2]),path(argv[3]),mask,grid,p,attributes);
    const auto gridJson=[](const auto& g) {return nlohmann::json{{"gridName",g.gridName},{"gridClass",g.gridClass},
        {"activeVoxelCount",g.activeVoxelCount},{"voxelSize",g.voxelSize}};};
    std::cout<<nlohmann::json{{"schemaVersion",1},{"operation","indexed-point-mask"},
        {"mask",gridJson(r.mask)},{"counts",mask.includeCounts?gridJson(r.counts):nlohmann::json(nullptr)},
        {"inputCount",r.inputCount},{"selectedCount",r.selectedCount},{"originWorld",mask.originWorld}}.dump(2)<<'\n';
    return EXIT_SUCCESS;
  }
  const auto r=vdb::QueryPointNeighbors(path(argv[2]),queries,o,grid,p,attributes);
  point_neighbors_cli::reportPreflight(r,queries.size(),p.maxInputBytes);
  auto descriptions=nlohmann::json::array(),levels=nlohmann::json::array(),buckets=nlohmann::json::array(),items=nlohmann::json::array();
  for(const auto& a:r.schema.attributes)descriptions.push_back({{"name",a.name},{"valueType",a.valueType},{"codec",a.codec},
      {"stride",a.stride},{"groupStorage",a.groupStorage},{"anyHidden",a.anyHidden},{"readable",a.readable}});
  for(const auto& l:r.levels)levels.push_back({{"voxelSize",l.voxelSize},{"minRadius",l.minRadius},{"maxRadius",l.maxRadius},
      {"pointCount",l.pointCount},{"occupiedVoxelCount",l.occupiedVoxelCount},{"leafCount",l.leafCount}});
  for(const auto& b:r.buckets) {
    auto refs=nlohmann::json::array();for(const auto& ref:b.points)refs.push_back(point_neighbors_cli::reference(ref));
    buckets.push_back({{"level",b.level},{"origin",b.origin},{"points",std::move(refs)}});
  }
  for(size_t i=0;i<r.queries.size();++i) {
    auto hits=nlohmann::json::array();
    for(const auto& h:r.queries[i])hits.push_back({{"reference",point_neighbors_cli::reference(h.reference)},
        {"positionWorld",h.positionWorld},{"distanceWorld",h.distanceWorld},{"radiusWorld",h.radiusWorld}});
    items.push_back({{"centerWorld",queries[i].center},{"radiusWorld",queries[i].radius},{"hits",std::move(hits)}});
  }
  std::cout<<nlohmann::json{{"schemaVersion",1},{"operation","point-neighbors"},{"mode",mode},
      {"traversal","leaf_origin_xyz_then_local_index"},
      {"schema",{{"gridName",r.schema.gridName},{"pointCount",r.schema.pointCount},{"leafCount",r.schema.leafCount},
          {"attributes",std::move(descriptions)},{"groups",r.schema.groups}}},
      {"selectedCount",r.selectedCount},{"archivedSchema",r.archivedSchema},{"indexOriginWorld",r.indexOriginWorld},
      {"levels",std::move(levels)},{"buckets",std::move(buckets)},{"queries",std::move(items)},
      {"gridMetadata",point_neighbors_cli::metadata(r.gridMetadata)},
      {"fileMetadata",point_neighbors_cli::metadata(r.fileMetadata)},
      {"descriptorMetadata",point_neighbors_cli::metadata(r.descriptorMetadata)},{"diagnostic",r.diagnostic}}.dump(2)<<'\n';
  return EXIT_SUCCESS;
}
