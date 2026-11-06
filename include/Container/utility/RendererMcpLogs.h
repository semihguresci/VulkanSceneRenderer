#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace container::log {

struct RendererMcpLogEntry {
  std::string_view component;
  std::string_view severity;
  std::string_view event;
  int64_t recordedAtUnixMs{};
};

// These entries contain fixed event descriptions, never a raw log payload.
void setRendererMcpLogCapture(bool enabled);
[[nodiscard]] std::vector<RendererMcpLogEntry> recentRendererMcpLogs(size_t limit);

} // namespace container::log
