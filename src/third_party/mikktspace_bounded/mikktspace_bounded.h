#pragma once

#include <stddef.h>
#include <stdint.h>
#include <mikktspace.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MikkTSpaceBoundedResult {
    MIKKTS_BOUNDED_SUCCESS = 0,
    MIKKTS_BOUNDED_INVALID_INPUT,
    MIKKTS_BOUNDED_WORK_LIMIT,
    MIKKTS_BOUNDED_MEMORY_LIMIT,
    MIKKTS_BOUNDED_OUT_OF_MEMORY,
} MikkTSpaceBoundedResult;

/* Generate the default MikkTSpace basis, debiting workRemaining so several
 * invocations can share one budget. maxScratchBytes bounds live heap storage,
 * including allocation ledger records. Each call also bounds stack depth.
 * Input callbacks must provide stable finite data and all callbacks must
 * return normally. A failed call never invokes either output callback.
 */
MikkTSpaceBoundedResult genTangSpaceDefaultBounded(const SMikkTSpaceContext *context,
                                                uint64_t *workRemaining,
                                                size_t maxScratchBytes);

#ifdef __cplusplus
}
#endif
