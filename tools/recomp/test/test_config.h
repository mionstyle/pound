// Force-included (-include) into every translation unit of the test build.
#pragma once
#include <stdint.h>
#define RECOMP_GUEST_BITS 40
void recomp_fault(uint64_t addr);
#define RECOMP_CHECK_ADDR(a, n) ((((uint64_t)(a) + (n)) >> RECOMP_GUEST_BITS) ? recomp_fault((a)) : (void)0)
