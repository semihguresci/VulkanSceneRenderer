#include <Container/utility/SceneNodeAuthoringPolicy.h>

#include <stdexcept>
#include <string>

namespace container::scene {
namespace {
[[noreturn]] void reject(const char* message) {
  throw std::invalid_argument(std::string("Scene node authoring: ") + message);
}

bool whitespace(uint32_t scalar) {
  return scalar == 0x20 || scalar == 0xA0 || scalar == 0x1680 ||
      (scalar >= 0x2000 && scalar <= 0x200A) || scalar == 0x202F ||
      scalar == 0x205F || scalar == 0x3000;
}
} // namespace

void ValidateSceneNodeName(std::string_view name,
                          const SceneNodeAuthoringLimits& limits) {
  if (!limits.maxNameBytes) reject("name byte budget must be positive");
  if (name.empty()) reject("name must not be empty");
  if (name.size() > limits.maxNameBytes) reject("name byte budget exceeded");
  bool nonblank = false;
  for (size_t position = 0; position < name.size();) {
    const auto first = static_cast<uint8_t>(name[position++]);
    uint32_t scalar{};
    size_t continuation{};
    uint32_t minimum{};
    if (first < 0x80) scalar = first;
    else if (first >= 0xC2 && first <= 0xDF) {
      scalar = first & 0x1F; continuation = 1; minimum = 0x80;
    } else if (first >= 0xE0 && first <= 0xEF) {
      scalar = first & 0x0F; continuation = 2; minimum = 0x800;
    } else if (first >= 0xF0 && first <= 0xF4) {
      scalar = first & 0x07; continuation = 3; minimum = 0x10000;
    } else reject("name contains malformed UTF-8");
    if (continuation > name.size() - position) reject("name contains truncated UTF-8");
    for (size_t i = 0; i < continuation; ++i) {
      const auto next = static_cast<uint8_t>(name[position++]);
      if ((next & 0xC0) != 0x80) reject("name contains malformed UTF-8 continuation");
      scalar = (scalar << 6) | (next & 0x3F);
    }
    if (scalar < minimum || scalar > 0x10FFFF ||
        (scalar >= 0xD800 && scalar <= 0xDFFF))
      reject("name contains an invalid or overlong UTF-8 scalar");
    if (scalar < 0x20 || (scalar >= 0x7F && scalar <= 0x9F) ||
        scalar == 0x2028 || scalar == 0x2029)
      reject("name must not contain control characters or line separators");
    nonblank |= !whitespace(scalar);
  }
  if (!nonblank) reject("name must contain a non-whitespace character");
}

} // namespace container::scene

