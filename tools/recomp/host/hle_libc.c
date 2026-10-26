// C library HLE: the SDK's memcpy family is written with SIMD instructions the
// translator does not cover yet, so these hot functions run as host code instead of in
// the interpreter. Guest memory is one flat mapping, so host memmove/memset apply directly.

#include "hle.h"
#include <string.h>

static void hle_memmove(cpu_t *c) // memcpy / memmove(dst, src, n) -> dst
{
    if (c->x[2])
        memmove(mem_host(c->x[0]), mem_host(c->x[1]), c->x[2]);
}

static void hle_memset(cpu_t *c) // memset(dst, value, n) -> dst
{
    if (c->x[2])
        memset(mem_host(c->x[0]), (int)(c->x[1] & 0xFF), c->x[2]);
}

static void hle_memcmp(cpu_t *c) // memcmp(a, b, n)
{
    int r = c->x[2] ? memcmp(mem_host(c->x[0]), mem_host(c->x[1]), c->x[2]) : 0;
    c->x[0] = (uint64_t)(int64_t)(r < 0 ? -1 : r > 0);
}

static void hle_strlen(cpu_t *c) { c->x[0] = strlen((const char *)mem_host(c->x[0])); }

const hle_def_t hle_defs_libc[] = {
    { "memcpy", hle_memmove },
    { "__memcpy_aarch64", hle_memmove },
    { "__memcpy_aarch64_simd", hle_memmove },
    { "memmove", hle_memmove },
    { "memset", hle_memset },
    { "memcmp", hle_memcmp },
    { "strlen", hle_strlen },
    { NULL, NULL },
};
