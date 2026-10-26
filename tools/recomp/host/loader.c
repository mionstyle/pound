// Loads the exefs NSOs the way Horizon's loader does: one contiguous code
// region starting with rtld, then main, then sdk. Each module is laid out as
// .text (RX, Code), .rodata (R, Code), .data+.bss (RW, CodeData).

#include "host.h"
#include "nso.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

module_t g_modules[MAX_MODULES];
int      g_module_count;

// Recompiled code for each module, linked in when available (weak otherwise).
#define MODULE_SYMBOLS(n)                                                                     \
    extern const recomp_entry_t recomp_table_##n[] __attribute__((weak));                   \
    extern const size_t         recomp_table_##n##_size __attribute__((weak));              \
    uint64_t                    recomp_module_base_##n;
MODULE_SYMBOLS(rtld)
MODULE_SYMBOLS(main)
MODULE_SYMBOLS(sdk)

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

static uint64_t load_one(const char *dir, const char *name, uint64_t base, const recomp_entry_t *table,
                         size_t table_size, uint64_t *base_var)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    size_t   fsize;
    uint8_t *file = read_file(path, &fsize);
    if (!file)
        return 0;
    nso_header_t h;
    if (nso_parse_header(file, fsize, &h) != POUND_SUCCESS)
        host_fatal("%s: bad NSO header", path);

    const nso_segment_t *t = &h.segments[NSO_SEGMENT_TEXT], *r = &h.segments[NSO_SEGMENT_RO],
                        *d = &h.segments[NSO_SEGMENT_DATA];
    uint64_t text_end = r->memory_offset;
    uint64_t ro_end   = d->memory_offset;
    uint64_t end      = h.image_size;
    if (mem_map(base, text_end, MS_CODE, MP_RX) || mem_map(base + text_end, ro_end - text_end, MS_CODE, MP_R) ||
        mem_map(base + ro_end, end - ro_end, MS_CODE_DATA, MP_RW))
        host_fatal("%s: cannot map at 0x%llx", name, (unsigned long long)base);
    if (nso_load_image(file, fsize, &h, mem_host(base), h.image_size) != POUND_SUCCESS)
        host_fatal("%s: decompression / hash check failed", name);
    free(file);
    (void)t;

    module_t *m = &g_modules[g_module_count++];
    snprintf(m->name, sizeof m->name, "%s", name);
    m->base       = base;
    m->size       = end;
    m->table      = table;
    m->table_size = table ? table_size : 0;
    m->base_var   = base_var;
    *base_var     = base;
    host_log("loaded %-5s at 0x%010llx size 0x%08llx (%zu recompiled functions)", name, (unsigned long long)base,
             (unsigned long long)end, m->table_size);
    return base + end;
}

int loader_load_exefs(const char *dir)
{
    uint64_t next = CODE_BASE;
    next = load_one(dir, "rtld", next, recomp_table_rtld, recomp_table_rtld ? recomp_table_rtld_size : 0,
                    &recomp_module_base_rtld);
    if (!next)
        host_fatal("rtld not found in %s", dir);
    next = load_one(dir, "main", next, recomp_table_main, recomp_table_main ? recomp_table_main_size : 0,
                    &recomp_module_base_main);
    if (!next)
        host_fatal("main not found in %s", dir);
    next = load_one(dir, "sdk", next, recomp_table_sdk, recomp_table_sdk ? recomp_table_sdk_size : 0,
                    &recomp_module_base_sdk);
    if (!next)
        host_fatal("sdk not found in %s", dir);
    return 0;
}

const char *addr_describe(uint64_t addr)
{
    static char buf[4][64];
    static int  k;
    char       *b = buf[k++ & 3];
    for (int i = 0; i < g_module_count; ++i)
        if (addr >= g_modules[i].base && addr < g_modules[i].base + g_modules[i].size) {
            snprintf(b, 64, "%s+0x%llx", g_modules[i].name, (unsigned long long)(addr - g_modules[i].base));
            return b;
        }
    snprintf(b, 64, "0x%llx", (unsigned long long)addr);
    return b;
}
