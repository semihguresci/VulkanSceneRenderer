#pragma once

#include <string_view>

namespace container::geometry::vdb {

// Plain names accept display or descriptor names and must be ambiguity-checked
// by the caller. The explicit prefix selects the exact name reported by
// OpenVDB's descriptor iterator, including its human-readable [N] suffix.
[[nodiscard]] inline bool GridSelectionMatches(std::string_view selection,
                                               std::string_view descriptorName,
                                               std::string_view displayName) {
  constexpr std::string_view prefix = "descriptor:";
  if (selection.starts_with(prefix))
    return selection.substr(prefix.size()) == descriptorName;
  return selection.empty() || selection == descriptorName || selection == displayName;
}

} // namespace container::geometry::vdb
