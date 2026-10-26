#ifndef RECOMP_HLE_H
#define RECOMP_HLE_H

#include "host.h"

// Guest address range for host-only functions (procedures returned to the guest).
#define HLE_THUNK_BASE 0x7E00000000ull
#define HLE_THUNK_SIZE 0x0000100000ull

typedef void (*hle_fn_t)(cpu_t *c); // arguments in c->x[0..7] / c->q[0..7], result in x0 / q0

typedef struct
{
    const char *name; // mangled symbol name
    hle_fn_t    fn;
} hle_def_t;

void        hle_init(void);                    // after the modules are loaded and relocated
bool        hle_try(cpu_t *c, uint64_t target); // runs the replacement if `target` has one
bool        hle_is_hooked(uint64_t addr);
int         hle_addresses(uint64_t *out, int max); // replaced module addresses (interpreter hooks)
uint64_t    hle_thunk(const char *name, hle_fn_t fn, void *user);
void       *hle_user(cpu_t *c);
uint64_t    hle_symbol_address(const char *name);
const char *hle_symbol_name(uint64_t addr);

// Guest memory for objects created by host code (bump allocator, zeroed).
#define HLE_HEAP_BASE 0x7E10000000ull
#define HLE_HEAP_SIZE 0x0010000000ull
uint64_t hle_alloc(uint64_t size, uint64_t align);

// Guest string helper.
static inline const char *g_str(uint64_t addr) { return (const char *)mem_host(addr); }

#endif // RECOMP_HLE_H
