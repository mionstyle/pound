// Guest address space: a single 2^39-byte host reservation (PROT_NONE until
// mapped), plus the region map Horizon programs inspect with svcQueryMemory.

#define _GNU_SOURCE
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

// ---- host virtual memory -------------------------------------------------------
#ifdef _WIN32
static void *os_reserve(uint64_t size) { return VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS); }
static int   os_commit(void *p, uint64_t size) { return VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE) ? 0 : -1; }
static void  os_decommit(void *p, uint64_t size) { VirtualFree(p, size, MEM_DECOMMIT); }
#else
static void *os_reserve(uint64_t size)
{
    void *p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
static int  os_commit(void *p, uint64_t size) { return mprotect(p, size, PROT_READ | PROT_WRITE); }
static void os_decommit(void *p, uint64_t size)
{
    madvise(p, size, MADV_DONTNEED);
    mprotect(p, size, PROT_NONE);
}
#endif

uint8_t *recomp_mem_base;

typedef struct
{
    uint64_t addr, size;
    uint32_t state, perm, attr;
} region_t;

#define MAX_REGIONS 4096
static region_t regions[MAX_REGIONS];
static int      region_count;

int mem_init(void)
{
    void *p = os_reserve(1ull << GUEST_ADDR_BITS);
    if (!p)
        return -1;
    recomp_mem_base = p;
    return 0;
}

void *mem_host(uint64_t addr) { return recomp_mem_base + addr; }

void g_read(uint64_t a, void *dst, size_t n) { memcpy(dst, recomp_mem_base + a, n); }
void g_write(uint64_t a, const void *src, size_t n) { memcpy(recomp_mem_base + a, src, n); }

static int region_index(uint64_t addr)
{
    for (int i = 0; i < region_count; ++i)
        if (addr >= regions[i].addr && addr < regions[i].addr + regions[i].size)
            return i;
    return -1;
}

static void sort_regions(void)
{
    for (int i = 1; i < region_count; ++i) {
        region_t r = regions[i];
        int      j = i - 1;
        while (j >= 0 && regions[j].addr > r.addr) {
            regions[j + 1] = regions[j];
            --j;
        }
        regions[j + 1] = r;
    }
}

bool mem_is_free(uint64_t addr, uint64_t size)
{
    for (int i = 0; i < region_count; ++i)
        if (addr < regions[i].addr + regions[i].size && regions[i].addr < addr + size)
            return false;
    return true;
}

uint64_t mem_find_free(uint64_t base, uint64_t limit, uint64_t size)
{
    for (uint64_t a = base; a + size <= limit; a += PAGE_SIZE_) {
        if (mem_is_free(a, size))
            return a;
        // skip past the overlapping region quickly
        int i = region_index(a);
        if (i >= 0)
            a = ((regions[i].addr + regions[i].size + PAGE_SIZE_ - 1) & ~(PAGE_SIZE_ - 1)) - PAGE_SIZE_;
    }
    return 0;
}

// Splits regions so that [addr, addr+size) boundaries fall on region edges.
static void split_at(uint64_t addr)
{
    int i = region_index(addr);
    if (i < 0 || regions[i].addr == addr)
        return;
    if (region_count >= MAX_REGIONS)
        host_fatal("too many memory regions");
    region_t tail = regions[i];
    tail.size     = regions[i].addr + regions[i].size - addr;
    tail.addr     = addr;
    regions[i].size = addr - regions[i].addr;
    regions[region_count++] = tail;
    sort_regions();
}

int mem_map(uint64_t addr, uint64_t size, uint32_t state, uint32_t perm)
{
    if ((addr | size) & (PAGE_SIZE_ - 1) || size == 0 || addr + size > (1ull << GUEST_ADDR_BITS))
        return -1;
    if (!mem_is_free(addr, size))
        return -2;
    if (os_commit(recomp_mem_base + addr, size) != 0)
        return -3;
    if (region_count >= MAX_REGIONS)
        host_fatal("too many memory regions");
    regions[region_count++] = (region_t){ addr, size, state, perm, 0 };
    sort_regions();
    exec_map_region(addr, size);
    if (host_verbose > 1)
        host_log("map   0x%010llx-0x%010llx state %02x perm %x", (unsigned long long)addr,
                 (unsigned long long)(addr + size), state, perm);
    return 0;
}

int mem_unmap(uint64_t addr, uint64_t size)
{
    split_at(addr);
    split_at(addr + size);
    for (int i = 0; i < region_count;) {
        if (regions[i].addr >= addr && regions[i].addr + regions[i].size <= addr + size) {
            exec_unmap_region(regions[i].addr, regions[i].size);
            regions[i] = regions[--region_count];
            continue;
        }
        ++i;
    }
    sort_regions();
    os_decommit(recomp_mem_base + addr, size);
    return 0;
}

int mem_protect(uint64_t addr, uint64_t size, uint32_t perm)
{
    split_at(addr);
    split_at(addr + size);
    for (int i = 0; i < region_count; ++i)
        if (regions[i].addr >= addr && regions[i].addr + regions[i].size <= addr + size)
            regions[i].perm = perm;
    return 0;
}

void mem_query(uint64_t addr, memory_info_t *out)
{
    memset(out, 0, sizeof *out);
    int i = region_index(addr);
    if (i >= 0) {
        // Merge with neighbours that have identical attributes, like the kernel does.
        uint64_t start = regions[i].addr, end = regions[i].addr + regions[i].size;
        for (int j = i - 1; j >= 0 && regions[j].addr + regions[j].size == start && regions[j].state == regions[i].state &&
                            regions[j].perm == regions[i].perm && regions[j].attr == regions[i].attr; --j)
            start = regions[j].addr;
        for (int j = i + 1; j < region_count && regions[j].addr == end && regions[j].state == regions[i].state &&
                            regions[j].perm == regions[i].perm && regions[j].attr == regions[i].attr; ++j)
            end = regions[j].addr + regions[j].size;
        out->addr  = start;
        out->size  = end - start;
        out->state = regions[i].state;
        out->perm  = regions[i].perm;
        out->attr  = regions[i].attr;
        return;
    }
    // Free gap around addr.
    uint64_t lo = 0, hi = 1ull << GUEST_ADDR_BITS;
    for (int j = 0; j < region_count; ++j) {
        uint64_t e = regions[j].addr + regions[j].size;
        if (e <= addr && e > lo) lo = e;
        if (regions[j].addr > addr && regions[j].addr < hi) hi = regions[j].addr;
    }
    if (addr >= (1ull << GUEST_ADDR_BITS)) {
        lo = 1ull << GUEST_ADDR_BITS;
        hi = ~0ull;
        out->state = MS_INACCESSIBLE;
    }
    out->addr = lo;
    out->size = hi - lo;
}

void mem_dump_map(void)
{
    for (int i = 0; i < region_count; ++i)
        host_log("  0x%010llx-0x%010llx state %02x perm %x  %s", (unsigned long long)regions[i].addr,
                 (unsigned long long)(regions[i].addr + regions[i].size), regions[i].state, regions[i].perm,
                 addr_describe(regions[i].addr));
}
