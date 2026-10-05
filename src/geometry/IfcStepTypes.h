#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace container::geometry::ifc::detail {
struct StepValue {
  enum class Kind { Omitted, Number, String, Enum, Ref, Text, List };
  Kind kind{Kind::Omitted};
  double number{0};
  uint32_t ref{0};
  std::string text{};
  std::vector<StepValue> list{};
};
struct Entity {
  uint32_t id{0};
  std::string type{};
  StepValue args{};
};
} // namespace container::geometry::ifc::detail
