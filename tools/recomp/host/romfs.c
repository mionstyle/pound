// Virtual RomFS image built from extracted directories.
//
// Dumps often contain the RomFS already extracted to a directory tree, while the
// game reads a RomFS *image* through fsp IStorage. This file builds the image's
// header, directory/file tables and hash tables in memory (same format and hash
// as Nintendo's), and serves file contents straight from the host files on read.
// Several roots can be layered: later roots (e.g. an update) override earlier ones.

#include "host.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ROMFS_EMPTY       0xFFFFFFFFu
#define ROMFS_DATA_OFFSET 0x200ull

typedef struct node
{
    char         *name;
    int           is_dir;
    uint64_t      size;     // files
    char         *host;     // files: host path
    struct node **kids;
    int           nkids, cap;
    struct node  *parent;
    uint32_t      off;      // entry offset in its table
    uint64_t      data_off; // files: offset relative to the data region
} node_t;

typedef struct { uint64_t off, size; const char *host; } span_t;

static node_t  *g_root;
static uint8_t *g_meta;       // dir hash + dir meta + file hash + file meta
static uint64_t g_meta_off, g_meta_size;
static uint8_t  g_header[0x50];
static span_t  *g_spans;      // files sorted by absolute image offset
static int      g_nspans;
static uint64_t g_image_size;
static int      g_nfiles, g_ndirs;

static node_t *child(node_t *d, const char *name, int is_dir)
{
    for (int i = 0; i < d->nkids; ++i)
        if (!strcmp(d->kids[i]->name, name))
            return d->kids[i];
    node_t *n = calloc(1, sizeof *n);
    n->name   = strdup(name);
    n->is_dir = is_dir;
    n->parent = d;
    if (d->nkids == d->cap) {
        d->cap  = d->cap ? d->cap * 2 : 8;
        d->kids = realloc(d->kids, sizeof *d->kids * (size_t)d->cap);
    }
    d->kids[d->nkids++] = n;
    return n;
}

static void scan(node_t *d, const char *path)
{
    DIR *dir = opendir(path);
    if (!dir)
        return;
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, ".listing"))
            continue;
        size_t      n = strlen(path) + strlen(e->d_name) + 2;
        char       *p = malloc(n);
        struct stat st;
        snprintf(p, n, "%s/%s", path, e->d_name);
        if (stat(p, &st) != 0) {
            free(p);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            scan(child(d, e->d_name, 1), p);
            free(p);
        } else {
            node_t *f = child(d, e->d_name, 0);
            free(f->host);
            f->host = p; // a later root replaces the file
            f->size = (uint64_t)st.st_size;
        }
    }
    closedir(dir);
}

static int by_name(const void *a, const void *b)
{
    return strcmp((*(node_t *const *)a)->name, (*(node_t *const *)b)->name);
}

static void sort_tree(node_t *d)
{
    qsort(d->kids, (size_t)d->nkids, sizeof *d->kids, by_name);
    for (int i = 0; i < d->nkids; ++i)
        if (d->kids[i]->is_dir)
            sort_tree(d->kids[i]);
}

static uint32_t hash_table_count(uint32_t n)
{
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    uint32_t c = n;
    while (c % 2 == 0 || c % 3 == 0 || c % 5 == 0 || c % 7 == 0 || c % 11 == 0 || c % 13 == 0 || c % 17 == 0)
        ++c;
    return c;
}

static uint32_t path_hash(uint32_t parent, const char *name)
{
    uint32_t h = parent ^ 123456789u;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        h = (h >> 5) | (h << 27);
        h ^= *p;
    }
    return h;
}

static uint32_t align4(uint32_t v) { return (v + 3) & ~3u; }

int romfs_build(const char *const *roots, int nroots)
{
    g_root = calloc(1, sizeof *g_root);
    g_root->name   = strdup("");
    g_root->is_dir = 1;
    for (int i = 0; i < nroots; ++i) {
        struct stat st;
        if (stat(roots[i], &st) != 0 || !S_ISDIR(st.st_mode)) {
            host_log("romfs: '%s' not found, skipped", roots[i]);
            continue;
        }
        scan(g_root, roots[i]);
        host_log("romfs: layered '%s'", roots[i]);
    }
    sort_tree(g_root);

    // Breadth-first list of directories; files in directory order.
    int     dcap = 64, fcap = 1024;
    node_t **dirs = malloc(sizeof *dirs * (size_t)dcap), **files = malloc(sizeof *files * (size_t)fcap);
    int      nd = 0, nf = 0;
    dirs[nd++] = g_root;
    for (int i = 0; i < nd; ++i)
        for (int k = 0; k < dirs[i]->nkids; ++k) {
            node_t *c = dirs[i]->kids[k];
            if (c->is_dir) {
                if (nd == dcap) dirs = realloc(dirs, sizeof *dirs * (size_t)(dcap *= 2));
                dirs[nd++] = c;
            }
        }
    for (int i = 0; i < nd; ++i)
        for (int k = 0; k < dirs[i]->nkids; ++k)
            if (!dirs[i]->kids[k]->is_dir) {
                if (nf == fcap) files = realloc(files, sizeof *files * (size_t)(fcap *= 2));
                files[nf++] = dirs[i]->kids[k];
            }

    // Offsets.
    uint32_t dmeta = 0, fmeta = 0;
    for (int i = 0; i < nd; ++i) { dirs[i]->off = dmeta; dmeta += 0x18 + align4((uint32_t)strlen(dirs[i]->name)); }
    uint64_t data = 0;
    for (int i = 0; i < nf; ++i) {
        files[i]->off      = fmeta;
        fmeta += 0x20 + align4((uint32_t)strlen(files[i]->name));
        data = (data + 0xF) & ~0xFull;
        files[i]->data_off = data;
        data += files[i]->size;
    }
    uint32_t dcount = hash_table_count((uint32_t)nd), fcount = hash_table_count((uint32_t)nf);
    uint32_t dhash_size = dcount * 4, fhash_size = fcount * 4;

    g_meta_size = (uint64_t)dhash_size + dmeta + fhash_size + fmeta;
    g_meta      = calloc(1, g_meta_size);
    uint8_t *dh = g_meta, *dm = dh + dhash_size, *fh = dm + dmeta, *fm = fh + fhash_size;
    memset(dh, 0xFF, dhash_size);
    memset(fh, 0xFF, fhash_size);

#define PUT32(p, v) do { uint32_t v_ = (v); memcpy((p), &v_, 4); } while (0)
#define PUT64(p, v) do { uint64_t v_ = (v); memcpy((p), &v_, 8); } while (0)
    for (int i = 0; i < nd; ++i) {
        node_t  *d = dirs[i];
        uint8_t *e = dm + d->off;
        uint32_t parent = d->parent ? d->parent->off : 0;
        uint32_t sibling = ROMFS_EMPTY, first_dir = ROMFS_EMPTY, first_file = ROMFS_EMPTY;
        if (d->parent) {
            node_t *p = d->parent;
            for (int k = 0; k < p->nkids; ++k)
                if (p->kids[k] == d) {
                    for (int j = k + 1; j < p->nkids; ++j)
                        if (p->kids[j]->is_dir) { sibling = p->kids[j]->off; break; }
                    break;
                }
        }
        for (int k = 0; k < d->nkids; ++k) {
            if (d->kids[k]->is_dir && first_dir == ROMFS_EMPTY) first_dir = d->kids[k]->off;
            if (!d->kids[k]->is_dir && first_file == ROMFS_EMPTY) first_file = d->kids[k]->off;
        }
        uint32_t h = path_hash(parent, d->name) % dcount;
        uint32_t prev;
        memcpy(&prev, dh + 4 * h, 4);
        PUT32(e + 0x00, parent);
        PUT32(e + 0x04, sibling);
        PUT32(e + 0x08, first_dir);
        PUT32(e + 0x0C, first_file);
        PUT32(e + 0x10, prev);
        PUT32(e + 0x14, (uint32_t)strlen(d->name));
        memcpy(e + 0x18, d->name, strlen(d->name));
        PUT32(dh + 4 * h, d->off);
    }
    for (int i = 0; i < nf; ++i) {
        node_t  *f = files[i];
        uint8_t *e = fm + f->off;
        uint32_t sibling = ROMFS_EMPTY;
        node_t  *p = f->parent;
        for (int k = 0; k < p->nkids; ++k)
            if (p->kids[k] == f) {
                for (int j = k + 1; j < p->nkids; ++j)
                    if (!p->kids[j]->is_dir) { sibling = p->kids[j]->off; break; }
                break;
            }
        uint32_t h = path_hash(p->off, f->name) % fcount;
        uint32_t prev;
        memcpy(&prev, fh + 4 * h, 4);
        PUT32(e + 0x00, p->off);
        PUT32(e + 0x04, sibling);
        PUT64(e + 0x08, f->data_off);
        PUT64(e + 0x10, f->size);
        PUT32(e + 0x18, prev);
        PUT32(e + 0x1C, (uint32_t)strlen(f->name));
        memcpy(e + 0x20, f->name, strlen(f->name));
        PUT32(fh + 4 * h, f->off);
    }

    g_meta_off = (ROMFS_DATA_OFFSET + data + 3) & ~3ull;
    uint64_t h[10] = { 0x50,
                       g_meta_off, dhash_size,
                       g_meta_off + dhash_size, dmeta,
                       g_meta_off + dhash_size + dmeta, fhash_size,
                       g_meta_off + dhash_size + dmeta + fhash_size, fmeta,
                       ROMFS_DATA_OFFSET };
    memcpy(g_header, h, sizeof h);
    g_image_size = g_meta_off + g_meta_size;

    g_spans  = malloc(sizeof *g_spans * (size_t)(nf ? nf : 1));
    g_nspans = nf;
    for (int i = 0; i < nf; ++i)
        g_spans[i] = (span_t){ ROMFS_DATA_OFFSET + files[i]->data_off, files[i]->size, files[i]->host };
    g_nfiles = nf;
    g_ndirs  = nd;
    free(dirs);
    free(files);
    host_log("romfs: %d directories, %d files, image 0x%llx bytes", nd, nf, (unsigned long long)g_image_size);
    return 0;
}

uint64_t romfs_size(void) { return g_image_size; }

// Reads [off, off+size) of the virtual image into dst.
void romfs_read(uint64_t off, uint8_t *dst, uint64_t size)
{
    memset(dst, 0, size);
    uint64_t end = off + size;
    // header
    if (off < sizeof g_header) {
        uint64_t n = (end < sizeof g_header ? end : sizeof g_header) - off;
        memcpy(dst, g_header + off, n);
    }
    // metadata
    if (end > g_meta_off && off < g_meta_off + g_meta_size) {
        uint64_t a = off > g_meta_off ? off : g_meta_off;
        uint64_t b = end < g_meta_off + g_meta_size ? end : g_meta_off + g_meta_size;
        memcpy(dst + (a - off), g_meta + (a - g_meta_off), b - a);
    }
    // file data: binary search for the first span ending after `off`
    int lo = 0, hi = g_nspans;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (g_spans[mid].off + g_spans[mid].size <= off) lo = mid + 1; else hi = mid;
    }
    for (int i = lo; i < g_nspans && g_spans[i].off < end; ++i) {
        const span_t *s = &g_spans[i];
        uint64_t a = off > s->off ? off : s->off;
        uint64_t b = end < s->off + s->size ? end : s->off + s->size;
        if (a >= b)
            continue;
        FILE *f = fopen(s->host, "rb");
        if (!f) {
            host_log("romfs: cannot open %s", s->host);
            continue;
        }
        fseeko(f, (off_t)(a - s->off), SEEK_SET);
        size_t got = fread(dst + (a - off), 1, b - a, f);
        fclose(f);
        if (host_verbose)
            host_log("romfs: read %s +0x%llx (0x%zx bytes)", s->host, (unsigned long long)(a - s->off), got);
    }
}
