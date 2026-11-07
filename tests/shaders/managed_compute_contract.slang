#ifndef MANAGED_COMPUTE_VALUE
#define MANAGED_COMPUTE_VALUE 17
#endif
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> output;
[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain() { output[0] = MANAGED_COMPUTE_VALUE; }
