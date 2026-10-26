// Save data file system: fsp IFileSystem / IFile / IDirectory backed by a host directory.
//
// The root is $RECOMP_SAVE_DIR, or "save" next to the working directory. Guest paths are
// absolute ("/Progress.sav"); they are confined to the root ("..", drive letters and
// backslashes are rejected).

#include "savefs.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir_host(p) _mkdir(p)
#else
#include <unistd.h>
#define mkdir_host(p) mkdir(p, 0755)
#endif

static const char *root(void)
{
    const char *r = getenv("RECOMP_SAVE_DIR");
    return r && *r ? r : "save";
}

// Builds the host path of guest path `p`; returns 0 when the path is not acceptable.
static int host_path(const char *p, char *out, size_t n)
{
    if (!p || p[0] != '/' || strstr(p, "..") || strchr(p, '\\') || strchr(p, ':'))
        return 0;
    mkdir_host(root());
    snprintf(out, n, "%s%s", root(), p);
    size_t len = strlen(out);
    while (len > strlen(root()) + 1 && out[len - 1] == '/')
        out[--len] = 0; // "/dir/" -> "/dir"
    return 1;
}

int savefs_entry_type(const char *p)
{
    char h[1024];
    struct stat st;
    if (!host_path(p, h, sizeof h) || stat(h, &st))
        return -1;
    return S_ISDIR(st.st_mode) ? 0 : 1;
}

uint32_t savefs_create_file(const char *p, uint64_t size)
{
    char h[1024];
    if (!host_path(p, h, sizeof h)) return FS_RESULT_INVALID_PATH;
    if (savefs_entry_type(p) >= 0) return FS_RESULT_ALREADY_EXISTS;
    FILE *f = fopen(h, "wb");
    if (!f) return FS_RESULT_NOT_FOUND; // parent directory missing
    if (size) { fseek(f, (long)size - 1, SEEK_SET); fputc(0, f); }
    fclose(f);
    return 0;
}

uint32_t savefs_delete_file(const char *p)
{
    char h[1024];
    if (!host_path(p, h, sizeof h)) return FS_RESULT_INVALID_PATH;
    if (savefs_entry_type(p) != 1) return FS_RESULT_NOT_FOUND;
    return remove(h) ? FS_RESULT_NOT_FOUND : 0;
}

uint32_t savefs_create_dir(const char *p)
{
    char h[1024];
    if (!host_path(p, h, sizeof h)) return FS_RESULT_INVALID_PATH;
    if (savefs_entry_type(p) >= 0) return FS_RESULT_ALREADY_EXISTS;
    return mkdir_host(h) ? FS_RESULT_NOT_FOUND : 0;
}

static void remove_tree(const char *h, int keep_self)
{
    DIR *d = opendir(h);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char c[1024];
            snprintf(c, sizeof c, "%s/%s", h, e->d_name);
            struct stat st;
            if (!stat(c, &st) && S_ISDIR(st.st_mode)) remove_tree(c, 0);
            else remove(c);
        }
        closedir(d);
    }
    if (!keep_self) rmdir(h);
}

uint32_t savefs_delete_dir(const char *p, int recursive, int keep_self)
{
    char h[1024];
    if (!host_path(p, h, sizeof h)) return FS_RESULT_INVALID_PATH;
    if (savefs_entry_type(p) != 0) return FS_RESULT_NOT_FOUND;
    if (recursive) { remove_tree(h, keep_self); return 0; }
    return rmdir(h) ? FS_RESULT_NOT_EMPTY : 0;
}

uint32_t savefs_rename(const char *from, const char *to)
{
    char a[1024], b[1024];
    if (!host_path(from, a, sizeof a) || !host_path(to, b, sizeof b)) return FS_RESULT_INVALID_PATH;
    if (savefs_entry_type(from) < 0) return FS_RESULT_NOT_FOUND;
    if (savefs_entry_type(to) >= 0) return FS_RESULT_ALREADY_EXISTS;
    return rename(a, b) ? FS_RESULT_NOT_FOUND : 0;
}

uint32_t savefs_read(const char *p, uint64_t off, void *buf, uint64_t size, uint64_t *done)
{
    char h[1024];
    *done = 0;
    if (!host_path(p, h, sizeof h)) return FS_RESULT_INVALID_PATH;
    FILE *f = fopen(h, "rb");
    if (!f) return FS_RESULT_NOT_FOUND;
    if (!fseek(f, (long)off, SEEK_SET))
        *done = fread(buf, 1, size, f);
    fclose(f);
    return 0;
}

uint32_t savefs_write(const char *p, uint64_t off, const void *buf, uint64_t size)
{
    char h[1024];
    if (!host_path(p, h, sizeof h)) return FS_RESULT_INVALID_PATH;
    FILE *f = fopen(h, "r+b");
    if (!f) return FS_RESULT_NOT_FOUND;
    fseek(f, (long)off, SEEK_SET);
    fwrite(buf, 1, size, f);
    fclose(f);
    return 0;
}

uint64_t savefs_size(const char *p)
{
    char h[1024];
    struct stat st;
    if (!host_path(p, h, sizeof h) || stat(h, &st)) return 0;
    return (uint64_t)st.st_size;
}

uint32_t savefs_set_size(const char *p, uint64_t size)
{
    char h[1024];
    if (!host_path(p, h, sizeof h)) return FS_RESULT_INVALID_PATH;
    uint64_t cur = savefs_size(p);
    if (size == cur) return 0;
    // Portable resize: rewrite the file with the kept prefix and zero padding.
    uint8_t *data = calloc(1, size ? size : 1);
    uint64_t got;
    savefs_read(p, 0, data, size < cur ? size : cur, &got);
    FILE *f = fopen(h, "wb");
    if (!f) { free(data); return FS_RESULT_NOT_FOUND; }
    fwrite(data, 1, size, f);
    fclose(f);
    free(data);
    return 0;
}

int savefs_list(const char *p, savefs_entry_t *out, int max)
{
    char h[1024];
    if (!host_path(p, h, sizeof h)) return 0;
    DIR *d = opendir(h);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[1024];
        struct stat st;
        snprintf(c, sizeof c, "%s/%s", h, e->d_name);
        if (stat(c, &st)) continue;
        snprintf(out[n].name, sizeof out[n].name, "%s", e->d_name);
        out[n].is_file = !S_ISDIR(st.st_mode);
        out[n].size    = out[n].is_file ? (uint64_t)st.st_size : 0;
        ++n;
    }
    closedir(d);
    return n;
}
