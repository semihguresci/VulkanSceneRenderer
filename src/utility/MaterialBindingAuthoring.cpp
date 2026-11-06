#include <Container/utility/MaterialBindingAuthoring.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace container::scene {
namespace {
[[noreturn]] void invalid(const char* message) {
  throw std::invalid_argument(message);
}

struct BindingWitness {
  uint32_t primitive{}, owner{};
  int32_t sourceNode{}, sourceMesh{}, sourcePrimitive{}, sourceInstance{};
  bool operator==(const BindingWitness&) const = default;
};

BindingWitness witness(const SceneNode& node) noexcept {
  return {node.primitiveIndex,node.meshOwnerGraphNode,node.sourceNodeIndex,
      node.sourceMeshIndex,node.sourcePrimitiveIndex,node.sourceInstanceIndex};
}

bool eligible(const SceneGraph& graph,const SceneNode* node,uint32_t primitiveCount) noexcept {
  if(!node || node->retired || !node->renderable || node->editorSceneRoot ||
      node->interactionRootBoundary || node->primitiveIndex>=primitiveCount ||
      node->sourceNodeIndex< -1 || node->sourceMeshIndex< -1 ||
      node->sourcePrimitiveIndex< -1 || node->sourceInstanceIndex< -1 ||
      ((node->sourceMeshIndex<0)!=(node->sourcePrimitiveIndex<0))) return false;
  if(node->meshOwnerGraphNode!=SceneGraph::kInvalidNode) {
    const auto* owner=graph.getNode(node->meshOwnerGraphNode);
    if(!owner || owner->retired) return false;
  }
  return true;
}

struct Budget {
  uint64_t limit{},bytes{};
  void add(uint64_t count,uint64_t width=1) {
    if(width && count>(limit-bytes)/width) invalid("Material binding storage budget exceeded");
    bytes+=count*width;
  }
};

void validLimits(const MaterialBindingLimits& limits) {
  for(const auto value:{limits.maxBindings,limits.maxSourceMaterials,limits.maxRuntimeMaterials,
      limits.maxOutputBytes,limits.maxScratchBytes})
    if(!value || value==UINT64_MAX) invalid("Material bindings require finite nonzero limits");
}

void contextShape(MaterialBindingContext context,const MaterialBindingLimits& limits) {
  if(!context.assetIdentity || context.sourceMaterialToRuntime.size()>limits.maxSourceMaterials ||
      context.sourceMaterialToRuntime.size()>size_t(INT32_MAX) ||
      context.runtimeMaterialCount>limits.maxRuntimeMaterials || context.runtimeMaterialCount>uint32_t(INT32_MAX) ||
      context.runtimePrimitiveCount==UINT32_MAX)
    invalid("Material binding source/runtime context is invalid or exceeds its limits");
}

bool sameContext(MaterialBindingContext a,MaterialBindingContext b) noexcept {
  return a.assetIdentity==b.assetIdentity && a.runtimeMaterialCount==b.runtimeMaterialCount &&
      a.runtimePrimitiveCount==b.runtimePrimitiveCount && a.sourceMaterialToRuntime.size()==b.sourceMaterialToRuntime.size() &&
      std::equal(a.sourceMaterialToRuntime.begin(),a.sourceMaterialToRuntime.end(),b.sourceMaterialToRuntime.begin());
}

MaterialBindingSide opposite(MaterialBindingSide side) noexcept {
  return side==MaterialBindingSide::Before?MaterialBindingSide::After:MaterialBindingSide::Before;
}
} // namespace

struct MaterialBindingCommand::State {
  uint64_t graphIdentity{},assetIdentity{},ownedBytes{};
  uint32_t beforeRuntime{},afterRuntime{},primitiveCount{};
  std::vector<uint32_t> beforeMapping,afterMapping;
  std::vector<MaterialBindingAssignment> assignments;
  std::vector<BindingWitness> witnesses;
  std::optional<MaterialBindingAppendTarget> append;
  bool changed{};
};

struct PreparedMaterialBindingChange::State {
  const SceneGraph* owner{};
  uint64_t identity{},revision{};
  MaterialBindingCommand command;
  MaterialBindingSide target{MaterialBindingSide::After};
  std::vector<MaterialBindingAssignment> assignments;
  bool changed{},consumed{};
};

struct MaterialBindingAuthoringAccess {
  using CommandState=MaterialBindingCommand::State;
  using PreparedState=PreparedMaterialBindingChange::State;
  static const CommandState* state(const MaterialBindingCommand& command) noexcept {return command.state_.get();}
  static const PreparedState* state(const PreparedMaterialBindingChange& prepared) noexcept {return prepared.state_.get();}
  static PreparedState* state(PreparedMaterialBindingChange& prepared) noexcept {return prepared.state_.get();}
  static MaterialBindingCommand command(std::shared_ptr<const CommandState> state) noexcept {
    MaterialBindingCommand result;result.state_=std::move(state);return result;
  }
  static PreparedMaterialBindingChange prepared(std::unique_ptr<PreparedState> state) noexcept {
    PreparedMaterialBindingChange result;result.state_=std::move(state);return result;
  }
};

// The only private graph access: no topology/world/active-membership payload is
// touched, and every eligible ID/value is checked before the first write.
struct MaterialBindingGraphAccess {
  static void publish(SceneGraph& graph,std::span<const MaterialBindingAssignment> assignments,bool changed) noexcept {
    for(const auto& row:assignments) graph.nodes_[row.graphNode].materialIndex=row.afterRuntimeMaterial;
    if(changed) ++graph.revision_;
  }
};

MaterialBindingCommand::operator bool() const noexcept {return bool(state_);}
bool MaterialBindingCommand::changes() const noexcept {return state_ && state_->changed;}
uint64_t MaterialBindingCommand::graphIdentity() const noexcept {return state_?state_->graphIdentity:0;}
std::span<const MaterialBindingAssignment> MaterialBindingCommand::assignments() const noexcept {
  return state_?std::span<const MaterialBindingAssignment>(state_->assignments):std::span<const MaterialBindingAssignment>{};
}
MaterialBindingContext MaterialBindingCommand::context(MaterialBindingSide side) const noexcept {
  if(!state_) return {};
  const bool before=side==MaterialBindingSide::Before;
  return {state_->assetIdentity,before?std::span<const uint32_t>(state_->beforeMapping):std::span<const uint32_t>(state_->afterMapping),
      before?state_->beforeRuntime:state_->afterRuntime,state_->primitiveCount};
}
std::optional<MaterialBindingAppendTarget> MaterialBindingCommand::appendedMaterial() const noexcept {return state_?state_->append:std::nullopt;}
uint64_t MaterialBindingCommand::ownedBytes() const noexcept {return state_?state_->ownedBytes:0;}

PreparedMaterialBindingChange::PreparedMaterialBindingChange() noexcept=default;
PreparedMaterialBindingChange::~PreparedMaterialBindingChange()=default;
PreparedMaterialBindingChange::PreparedMaterialBindingChange(PreparedMaterialBindingChange&&) noexcept=default;
PreparedMaterialBindingChange& PreparedMaterialBindingChange::operator=(PreparedMaterialBindingChange&&) noexcept=default;
PreparedMaterialBindingChange::operator bool() const noexcept {return state_ && !state_->consumed;}
bool PreparedMaterialBindingChange::changes() const noexcept {return bool(*this) && state_->changed;}
uint64_t PreparedMaterialBindingChange::graphIdentity() const noexcept {return state_?state_->identity:0;}
uint64_t PreparedMaterialBindingChange::expectedGraphRevision() const noexcept {return state_?state_->revision:0;}
uint64_t PreparedMaterialBindingChange::candidateGraphRevision() const noexcept {
  return state_?state_->revision+uint64_t(state_->changed):0;
}
MaterialBindingContext PreparedMaterialBindingChange::expectedContext() const noexcept {
  return state_?state_->command.context(opposite(state_->target)):MaterialBindingContext{};
}
MaterialBindingContext PreparedMaterialBindingChange::targetContext() const noexcept {
  return state_?state_->command.context(state_->target):MaterialBindingContext{};
}
MaterialBindingCommand PreparedMaterialBindingChange::command() const noexcept {
  return state_?state_->command:MaterialBindingCommand{};
}
MaterialBindingSide PreparedMaterialBindingChange::targetSide() const noexcept {
  return state_?state_->target:MaterialBindingSide::After;
}
std::span<const MaterialBindingAssignment> PreparedMaterialBindingChange::assignments() const noexcept {
  return state_?std::span<const MaterialBindingAssignment>(state_->assignments):std::span<const MaterialBindingAssignment>{};
}

MaterialBindingCommand PlanMaterialBindings(const SceneGraph& graph,MaterialBindingContext context,
    std::span<const MaterialBindingRequest> requests,std::optional<MaterialBindingAppendTarget> append,
    const MaterialBindingLimits& limits) {
  validLimits(limits);contextShape(context,limits);
  if(requests.size()>limits.maxBindings) invalid("Material binding batch exceeds its row limit");
  if(append && (append->sourceMaterial!=context.sourceMaterialToRuntime.size() ||
      append->runtimeMaterial!=context.runtimeMaterialCount ||
      context.runtimeMaterialCount==uint32_t(INT32_MAX) ||
      context.sourceMaterialToRuntime.size()==size_t(INT32_MAX) ||
      context.runtimeMaterialCount+1>limits.maxRuntimeMaterials ||
      context.sourceMaterialToRuntime.size()+1>limits.maxSourceMaterials))
    invalid("Prospective material binding must name the exact source/runtime append tail");
  using Access=MaterialBindingAuthoringAccess;
  Budget output{limits.maxOutputBytes},scratch{limits.maxScratchBytes};
  output.add(sizeof(Access::CommandState)+128);
  output.add(context.sourceMaterialToRuntime.size(),2*sizeof(uint32_t));
  output.add(append.has_value(),sizeof(uint32_t));
  output.add(requests.size(),sizeof(MaterialBindingAssignment)+sizeof(BindingWitness));
  scratch.add(context.sourceMaterialToRuntime.size(),sizeof(std::pair<uint32_t,int32_t>));
  scratch.add(requests.size(),sizeof(MaterialBindingRequest));

  // Validate ranges before reserving/sorting. Identity lookup is source-sized,
  // never runtime-sized or proportional to the authoring graph's payload.
  for(const auto runtime:context.sourceMaterialToRuntime)
    if(runtime>=context.runtimeMaterialCount) invalid("Source material mapping contains an unavailable runtime ID");
  for(const auto& request:requests) {
    const auto* node=graph.getNode(request.graphNode);
    if(!eligible(graph,node,context.runtimePrimitiveCount) || request.expectedRuntimeMaterial>=context.runtimeMaterialCount ||
        node->materialIndex!=request.expectedRuntimeMaterial ||
        (request.replacementRuntimeMaterial>=context.runtimeMaterialCount &&
         (!append || request.replacementRuntimeMaterial!=append->runtimeMaterial)))
      invalid("Material binding request has a stale, missing or invalid renderable/material identity");
  }
  std::vector<std::pair<uint32_t,int32_t>> inverse;
  inverse.reserve(context.sourceMaterialToRuntime.size());
  for(size_t source=0;source<context.sourceMaterialToRuntime.size();++source)
    inverse.emplace_back(context.sourceMaterialToRuntime[source],int32_t(source));
  std::sort(inverse.begin(),inverse.end());
  for(size_t i=1;i<inverse.size();++i)
    if(inverse[i-1].first==inverse[i].first) invalid("Source material definitions alias the same runtime ID");
  auto ordered=std::vector<MaterialBindingRequest>(requests.begin(),requests.end());
  std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.graphNode<b.graphNode;});
  for(size_t i=1;i<ordered.size();++i)
    if(ordered[i-1].graphNode==ordered[i].graphNode) invalid("Material binding batch contains duplicate graph nodes");
  const auto sourceOf=[&](uint32_t runtime) {
    if(append && runtime==append->runtimeMaterial) return int32_t(append->sourceMaterial);
    const auto found=std::lower_bound(inverse.begin(),inverse.end(),runtime,
        [](const auto& row,uint32_t value){return row.first<value;});
    return found!=inverse.end() && found->first==runtime?found->second:int32_t(-1);
  };
  auto state=std::make_shared<Access::CommandState>();
  state->graphIdentity=graph.identity();state->assetIdentity=context.assetIdentity;
  state->beforeRuntime=context.runtimeMaterialCount;state->afterRuntime=context.runtimeMaterialCount+uint32_t(append.has_value());
  state->primitiveCount=context.runtimePrimitiveCount;state->append=append;state->ownedBytes=output.bytes;
  state->beforeMapping.assign(context.sourceMaterialToRuntime.begin(),context.sourceMaterialToRuntime.end());
  state->afterMapping.reserve(context.sourceMaterialToRuntime.size()+size_t(append.has_value()));
  state->afterMapping.assign(context.sourceMaterialToRuntime.begin(),context.sourceMaterialToRuntime.end());
  if(append) state->afterMapping.push_back(append->runtimeMaterial);
  state->assignments.reserve(ordered.size());state->witnesses.reserve(ordered.size());
  for(const auto& request:ordered) {
    state->assignments.push_back({request.graphNode,request.expectedRuntimeMaterial,request.replacementRuntimeMaterial,
        sourceOf(request.expectedRuntimeMaterial),sourceOf(request.replacementRuntimeMaterial)});
    state->witnesses.push_back(witness(*graph.getNode(request.graphNode)));
    state->changed|=request.expectedRuntimeMaterial!=request.replacementRuntimeMaterial;
  }
  if(state->changed && graph.revision()==UINT64_MAX) invalid("Material binding graph revision is exhausted");
  return Access::command(std::move(state));
}

PreparedMaterialBindingChange RebaseMaterialBindings(const SceneGraph& graph,MaterialBindingContext context,
    const MaterialBindingCommand& command,MaterialBindingSide target,const MaterialBindingLimits& limits) {
  validLimits(limits);contextShape(context,limits);
  if(target!=MaterialBindingSide::Before && target!=MaterialBindingSide::After)
    invalid("Unknown material binding history side");
  using Access=MaterialBindingAuthoringAccess;
  const auto* saved=Access::state(command);
  if(!saved || saved->graphIdentity!=graph.identity() ||
      !sameContext(context,command.context(opposite(target))))
    invalid("Material binding history belongs to a different graph, asset or material identity context");
  if(saved->assignments.size()>limits.maxBindings ||
      saved->afterMapping.size()>limits.maxSourceMaterials || saved->afterRuntime>limits.maxRuntimeMaterials)
    invalid("Material binding history exceeds its row/context limits");
  Budget output{limits.maxOutputBytes};
  output.add(command.ownedBytes());output.add(sizeof(Access::PreparedState));
  output.add(saved->assignments.size(),sizeof(MaterialBindingAssignment));
  if(saved->changed && graph.revision()==UINT64_MAX) invalid("Material binding graph revision is exhausted");
  for(size_t i=0;i<saved->assignments.size();++i) {
    const auto& row=saved->assignments[i];const auto* node=graph.getNode(row.graphNode);
    const auto before=target==MaterialBindingSide::After?row.beforeRuntimeMaterial:row.afterRuntimeMaterial;
    if(!eligible(graph,node,saved->primitiveCount) || node->materialIndex!=before || witness(*node)!=saved->witnesses[i])
      invalid("Material binding history has a stale or retired primitive/material binding");
  }
  auto state=std::make_unique<Access::PreparedState>();
  state->owner=&graph;state->identity=graph.identity();state->revision=graph.revision();
  state->command=command;state->target=target;state->changed=saved->changed;
  state->assignments.reserve(saved->assignments.size());
  for(auto row:saved->assignments) {
    if(target==MaterialBindingSide::Before) {
      std::swap(row.beforeRuntimeMaterial,row.afterRuntimeMaterial);
      std::swap(row.beforeSourceMaterial,row.afterSourceMaterial);
    }
    state->assignments.push_back(row);
  }
  return Access::prepared(std::move(state));
}

PreparedMaterialBindingChange PrepareMaterialBindings(const SceneGraph& graph,MaterialBindingContext context,
    const MaterialBindingCommand& command,const MaterialBindingLimits& limits) {
  return RebaseMaterialBindings(graph,context,command,MaterialBindingSide::After,limits);
}

bool CanPublishMaterialBindings(const SceneGraph& graph,const PreparedMaterialBindingChange& prepared,
    MaterialBindingContext context) noexcept {
  using Access=MaterialBindingAuthoringAccess;
  const auto* state=Access::state(prepared);
  if(!state || state->consumed || state->owner!=&graph || state->identity!=graph.identity() ||
      state->revision!=graph.revision() || (state->changed && graph.revision()==UINT64_MAX) ||
      !sameContext(context,prepared.expectedContext())) return false;
  return MatchesMaterialBindings(graph,state->command,context,opposite(state->target));
}

bool MatchesMaterialBindings(const SceneGraph& graph,const MaterialBindingCommand& command,
    MaterialBindingContext context,MaterialBindingSide side) noexcept {
  if(side!=MaterialBindingSide::Before && side!=MaterialBindingSide::After) return false;
  const auto* saved=MaterialBindingAuthoringAccess::state(command);
  if(!saved || saved->graphIdentity!=graph.identity() ||
      !sameContext(context,command.context(side)) || saved->witnesses.size()!=saved->assignments.size()) return false;
  for(size_t i=0;i<saved->assignments.size();++i) {
    const auto& row=saved->assignments[i];const auto* node=graph.getNode(row.graphNode);
    const auto material=side==MaterialBindingSide::Before?row.beforeRuntimeMaterial:row.afterRuntimeMaterial;
    if(!eligible(graph,node,saved->primitiveCount) || node->materialIndex!=material ||
        witness(*node)!=saved->witnesses[i]) return false;
  }
  return true;
}

bool PublishMaterialBindings(SceneGraph& graph,PreparedMaterialBindingChange& prepared,
    MaterialBindingContext context) noexcept {
  if(!CanPublishMaterialBindings(graph,prepared,context)) return false;
  auto& state=*MaterialBindingAuthoringAccess::state(prepared);
  MaterialBindingGraphAccess::publish(graph,state.assignments,state.changed);
  state.consumed=true;return true;
}

} // namespace container::scene
