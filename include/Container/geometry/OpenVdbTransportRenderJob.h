#pragma once
#include "Container/geometry/OpenVdbTransportImageScene.h"

namespace container::geometry::vdb {
enum class TransportRenderBackend { Cpu, Gpu };
struct TransportRenderJobOptions {
  std::filesystem::path sceneSpec, output, spirv;
  TransportRenderBackend backend{TransportRenderBackend::Cpu};
  bool validation{false};
  TransportImageOptions image;
  uint64_t maxSpecBytes{8ull * 1024 * 1024}, maxSceneBytes{512ull * 1024 * 1024};
  uint64_t maxDensityValues{16'000'000};
  uint32_t maxWorkPerPath{4096};
};
struct TransportRenderJobResult {
  TransportImageResult image;
  TransportImageExportResult exported;
  std::vector<TransportImageSourceInfo> sources;
  std::filesystem::path output;
};
// Strict bounded JSON schemaVersion1: optional camera{eye,target,up,
// verticalFovDegrees,width,height}; media[{input,gridName,maxResolution,
// importScale,nativeSparse,extinction,albedo,anisotropy,emission}];
// lights[{type:"directional"|"point",vector,value}];
// occluders[[a,b,c]]; environment. Vectors/vertices have3 numeric components.
// Unknown/duplicate keys, nonfinite/nonzero-underflowed Double tokens, depth>12,
// >65536nodes and >1MiB string payloads fail. No comments/trailing JSON values.
// Relative media paths resolve against the ORIGINAL sceneSpec parent.
// JSON only requests scene/camera/dimensions; all sampling/work/path/pixel/
// density/byte caps come from options and cannot be increased by the JSON.
// Limits/caps are validated before I/O. DOM is released before source loading;
// path/spec/source diagnostics and live image/scene/export share memory caps.
// CPU with no VDB media works in OpenVDB-OFF builds. A VDB source reports
// availability before touching its file when the optional feature is OFF.
// GPU creates a dedicated session/device; no live renderer queue is shared.
// Native source requires GPU. Return fields/source snapshots are allocated
// before exclusive EXR publication; errors leave no new partial output.
[[nodiscard]] TransportRenderJobResult RunTransportRenderJob(const TransportRenderJobOptions &);
} // namespace container::geometry::vdb
