#pragma once

// Visual Studio applies target-wide forced includes to mixed C/C++ targets.
#ifdef __cplusplus
#include "nonstd/expected.hpp"

namespace nonstd {
using expected_lite::make_unexpected;
}  // namespace nonstd
#endif
