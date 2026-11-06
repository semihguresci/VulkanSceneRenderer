#pragma once

// NanoVDB ABI32.7's CreateNanoGrid passes a mutable Range1D lambda to its
// reduction wrapper. Current oneTBB correctly supplies a const range. Adapt a
// private range copy without changing vendor sources or disabling TBB checks.
// Include before CreateNanoGrid in every translation unit using that converter.
#if defined(NANOVDB_USE_TBB)
#include <nanovdb/util/Reduce.h>
namespace nanovdb::util {
template <class T, class Func, class Join>
inline T reduce(Range1D range, const T &identity, const Func &func,
                const Join &join) {
  if (range.empty())
    return identity;
  return tbb::parallel_reduce(
      range, identity,
      [&](const Range1D &input, const T &value) {
        auto mutableRange = input;
        return func(mutableRange, value);
      },
      join);
}
} // namespace nanovdb::util
#endif
