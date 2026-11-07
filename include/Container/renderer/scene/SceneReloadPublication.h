#pragma once

#include "Container/utility/SceneGraph.h"

#include <cstdint>
#include <utility>

namespace container::renderer {

// A failed commit can rebuild the previous model into a fresh graph. Retire
// node-index bindings before presenting that graph, regardless of load success.
// A preparation failure leaves the publication intact and retains its bindings.
template <class RetireBindings>
bool retireBindingsAfterSceneReload(const scene::SceneGraph &graph,
                                   uint64_t previousIdentity,
                                   RetireBindings &&retire) {
  if (graph.identity() == previousIdentity)
    return false;
  std::forward<RetireBindings>(retire)();
  return true;
}

} // namespace container::renderer
