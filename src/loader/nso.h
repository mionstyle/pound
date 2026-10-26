#ifndef POUND_LOADER_NSO_H
#define POUND_LOADER_NSO_H

#include "errors.h"
#include <stddef.h>
#include <stdint.h>

/// NSO (Nintendo Shared Object) executable, as found in a title's exefs
/// (`main`, `sdk`, `rtld`, `subsdk*`). Same format on Switch 1 and Switch 2.

#define NSO_SEGMENT_TEXT  0
#define NSO_SEGMENT_RO    1
#define NSO_SEGMENT_DATA  2
#define NSO_SEGMENT_COUNT 3

typedef struct
{
    /// Offset of the (possibly compressed) segment inside the file.
    uint32_t file_offset;

    /// Offset of the segment relative to the module's load address.
    uint32_t memory_offset;

    /// Decompressed size in bytes.
    uint32_t size;

    /// Size of the segment as stored in the file.
    uint32_t file_size;

    /// SHA-256 of the decompressed segment.
    uint8_t hash[32];

    uint8_t is_compressed;
    uint8_t check_hash;
} nso_segment_t;

typedef struct
{
    nso_segment_t segments[NSO_SEGMENT_COUNT];
    uint32_t      bss_size;
    uint8_t       module_id[32];

    /// Total bytes the module occupies once loaded, rounded up to a page.
    size_t image_size;
} nso_header_t;

/// Parses and validates the 0x100-byte NSO header.
error_t nso_parse_header(const uint8_t *file, size_t file_size, nso_header_t *out_header);

/// Decompresses every segment of `file` into `image`, which must be at least
/// `header->image_size` bytes. The .bss region is zeroed. When the header asks for it,
/// each segment's SHA-256 is verified.
error_t nso_load_image(const uint8_t      *file,
                       size_t              file_size,
                       const nso_header_t *header,
                       uint8_t            *image,
                       size_t              image_capacity);

/// Decompresses one LZ4 block. Writes the number of produced bytes into `out_written`.
error_t lz4_decompress_block(const uint8_t *src,
                             size_t         src_size,
                             uint8_t       *dst,
                             size_t         dst_capacity,
                             size_t        *out_written);

#endif // POUND_LOADER_NSO_H
