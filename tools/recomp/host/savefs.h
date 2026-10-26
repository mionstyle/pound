#ifndef RECOMP_SAVEFS_H
#define RECOMP_SAVEFS_H

#include <stdint.h>

// nn::fs results (module 2).
#define FS_RESULT_NOT_FOUND      0x202u  // PathNotFound
#define FS_RESULT_ALREADY_EXISTS 0x402u  // PathAlreadyExists
#define FS_RESULT_NOT_EMPTY      0x1002u   // DirectoryNotEmpty
#define FS_RESULT_INVALID_PATH   0x2EEA02u // InvalidPathFormat

typedef struct
{
    char     name[0x301];
    int      is_file;
    uint64_t size;
} savefs_entry_t;

int      savefs_entry_type(const char *path); // -1 missing, 0 directory, 1 file
uint32_t savefs_create_file(const char *path, uint64_t size);
uint32_t savefs_delete_file(const char *path);
uint32_t savefs_create_dir(const char *path);
uint32_t savefs_delete_dir(const char *path, int recursive, int keep_self);
uint32_t savefs_rename(const char *from, const char *to);
uint32_t savefs_read(const char *path, uint64_t off, void *buf, uint64_t size, uint64_t *done);
uint32_t savefs_write(const char *path, uint64_t off, const void *buf, uint64_t size);
uint64_t savefs_size(const char *path);
uint32_t savefs_set_size(const char *path, uint64_t size);
int      savefs_list(const char *path, savefs_entry_t *out, int max);

#endif
