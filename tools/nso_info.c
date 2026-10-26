// Small command-line tool: parses an NSO, decompresses it, verifies hashes
// and optionally writes the flat memory image to disk.
//
//   nso_info <main> [out.bin]

#include "nso.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static uint8_t *
read_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (NULL == f)
    {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buffer = malloc((size_t)size);
    if (NULL != buffer && fread(buffer, 1, (size_t)size, f) != (size_t)size)
    {
        free(buffer);
        buffer = NULL;
    }
    fclose(f);
    *out_size = (size_t)size;
    return buffer;
}

int
main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <nso> [out.bin]\n", argv[0]);
        return 1;
    }

    size_t   file_size = 0;
    uint8_t *file      = read_file(argv[1], &file_size);
    if (NULL == file)
    {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }

    nso_header_t header;
    error_t      err = nso_parse_header(file, file_size, &header);
    if (POUND_SUCCESS != err)
    {
        fprintf(stderr, "invalid NSO header (error %d)\n", err);
        return 1;
    }

    static const char *names[NSO_SEGMENT_COUNT] = {".text", ".rodata", ".data"};
    printf("module id : ");
    for (int i = 0; i < 20; ++i)
    {
        printf("%02x", header.module_id[i]);
    }
    printf("\n");
    for (int i = 0; i < NSO_SEGMENT_COUNT; ++i)
    {
        const nso_segment_t *s = &header.segments[i];
        printf("%-8s mem +0x%08" PRIx32 "  size 0x%08" PRIx32 "  file 0x%08" PRIx32 "%s%s\n",
               names[i], s->memory_offset, s->size, s->file_size,
               s->is_compressed ? "  lz4" : "", s->check_hash ? "  sha256" : "");
    }
    printf(".bss     size 0x%08" PRIx32 "\n", header.bss_size);
    printf("image    0x%zx bytes\n", header.image_size);

    uint8_t *image = malloc(header.image_size);
    err            = nso_load_image(file, file_size, &header, image, header.image_size);
    if (POUND_SUCCESS != err)
    {
        fprintf(stderr, "load failed (error %d)\n", err);
        return 1;
    }
    printf("load OK, all segment hashes verified\n");

    if (argc >= 3)
    {
        FILE *out = fopen(argv[2], "wb");
        fwrite(image, 1, header.image_size, out);
        fclose(out);
        printf("image written to %s\n", argv[2]);
    }

    free(image);
    free(file);
    return 0;
}
