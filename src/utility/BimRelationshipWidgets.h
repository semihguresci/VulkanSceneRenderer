#pragma once

namespace container::renderer {
class BimRelationshipGraph;
}

namespace container::ui {
class BimRelationshipBrowserCache;

namespace detail {
void drawBimGraphSpatialEdges(const renderer::BimRelationshipGraph &graph,
                              BimRelationshipBrowserCache &cache);
}
}
