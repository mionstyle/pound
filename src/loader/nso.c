#include "nso.h"
#include "sha256.h"
#include <string.h>

#define NSO_HEADER_SIZE 0x100U
#define NSO_PAGE_SIZE   0x1000U

static uint32_t
read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

error_t
nso_parse_header(const uint8_t *file, const size_t file_size, nso_header_t *out_header)
{
    if (NULL == file || NULL == out_header || file_size < NSO_HEADER_SIZE)
    {
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (0 != memcmp(file, "NSO0", 4))
    {
        return POUND_ERROR_INVALID_FORMAT;
    }

    memset(out_header, 0, sizeof(*out_header));
    const uint32_t flags = read_u32(file + 0x0C);

    static const size_t segment_header_offsets[NSO_SEGMENT_COUNT] = {0x10, 0x20, 0x30};
    uint64_t            image_end                                  = 0;

    for (size_t i = 0; i < NSO_SEGMENT_COUNT; ++i)
    {
        nso_segment_t *seg = &out_header->segments[i];
        const uint8_t *sh  = file + segment_header_offsets[i];

        seg->file_offset   = read_u32(sh + 0x0);
        seg->memory_offset = read_u32(sh + 0x4);
        seg->size          = read_u32(sh + 0x8);
        seg->file_size     = read_u32(file + 0x60 + 4 * i);
        seg->is_compressed = (uint8_t)((flags >> i) & 1U);
        seg->check_hash    = (uint8_t)((flags >> (i + 3)) & 1U);
        memcpy(seg->hash, file + 0xA0 + 32 * i, 32);

        if ((uint64_t)seg->file_offset + seg->file_size > file_size)
        {
            return POUND_ERROR_INVALID_FORMAT;
        }

        if (!seg->is_compressed && seg->file_size != seg->size)
        {
            return POUND_ERROR_INVALID_FORMAT;
        }

        // Segments must be page-aligned and in order: .text < .rodata < .data.
        if (0 != (seg->memory_offset % NSO_PAGE_SIZE) || seg->memory_offset < image_end)
        {
            return POUND_ERROR_INVALID_FORMAT;
        }

        image_end = (uint64_t)seg->memory_offset + seg->size;
    }

    out_header->bss_size = read_u32(file + 0x3C);
    memcpy(out_header->module_id, file + 0x40, 32);

    image_end += out_header->bss_size;
    image_end = (image_end + NSO_PAGE_SIZE - 1) & ~(uint64_t)(NSO_PAGE_SIZE - 1);
    if (image_end > SIZE_MAX)
    {
        return POUND_ERROR_GUEST_ADDRESS_OVERFLOW;
    }

    out_header->image_size = (size_t)image_end;
    return POUND_SUCCESS;
}

error_t
lz4_decompress_block(const uint8_t *src,
                     const size_t   src_size,
                     uint8_t       *dst,
                     const size_t   dst_capacity,
                     size_t        *out_written)
{
    size_t ip = 0;
    size_t op = 0;

    while (ip < src_size)
    {
        const uint8_t token = src[ip++];

        // Literal run.
        size_t literal_length = token >> 4;
        if (15 == literal_length)
        {
            uint8_t extra;
            do
            {
                if (ip >= src_size)
                {
                    return POUND_ERROR_INVALID_FORMAT;
                }
                extra = src[ip++];
                literal_length += extra;
            } while (255 == extra);
        }

        if (literal_length > src_size - ip || literal_length > dst_capacity - op)
        {
            return POUND_ERROR_INVALID_FORMAT;
        }
        memcpy(dst + op, src + ip, literal_length);
        ip += literal_length;
        op += literal_length;

        // The last sequence has literals only.
        if (ip >= src_size)
        {
            break;
        }

        // Match copy.
        if (src_size - ip < 2)
        {
            return POUND_ERROR_INVALID_FORMAT;
        }
        const size_t match_offset = (size_t)src[ip] | ((size_t)src[ip + 1] << 8);
        ip += 2;
        if (0 == match_offset || match_offset > op)
        {
            return POUND_ERROR_INVALID_FORMAT;
        }

        size_t match_length = (token & 0x0FU) + 4;
        if (19 == match_length)
        {
            uint8_t extra;
            do
            {
                if (ip >= src_size)
                {
                    return POUND_ERROR_INVALID_FORMAT;
                }
                extra = src[ip++];
                match_length += extra;
            } while (255 == extra);
        }

        if (match_length > dst_capacity - op)
        {
            return POUND_ERROR_INVALID_FORMAT;
        }

        // Byte-by-byte because source and destination may overlap.
        const uint8_t *match = dst + op - match_offset;
        for (size_t i = 0; i < match_length; ++i)
        {
            dst[op + i] = match[i];
        }
        op += match_length;
    }

    *out_written = op;
    return POUND_SUCCESS;
}

error_t
nso_load_image(const uint8_t      *file,
               const size_t        file_size,
               const nso_header_t *header,
               uint8_t            *image,
               const size_t        image_capacity)
{
    if (NULL == file || NULL == header || NULL == image || image_capacity < header->image_size)
    {
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    memset(image, 0, header->image_size);

    for (size_t i = 0; i < NSO_SEGMENT_COUNT; ++i)
    {
        const nso_segment_t *seg = &header->segments[i];
        const uint8_t       *src = file + seg->file_offset;
        uint8_t             *dst = image + seg->memory_offset;

        if (seg->file_offset + (size_t)seg->file_size > file_size)
        {
            return POUND_ERROR_INVALID_FORMAT;
        }

        if (seg->is_compressed)
        {
            size_t  written = 0;
            error_t err     = lz4_decompress_block(src, seg->file_size, dst, seg->size, &written);
            if (POUND_SUCCESS != err)
            {
                return err;
            }
            if (written != seg->size)
            {
                return POUND_ERROR_INVALID_FORMAT;
            }
        }
        else
        {
            memcpy(dst, src, seg->size);
        }

        if (seg->check_hash)
        {
            uint8_t digest[32];
            sha256(dst, seg->size, digest);
            if (0 != memcmp(digest, seg->hash, 32))
            {
                return POUND_ERROR_HASH_MISMATCH;
            }
        }
    }

    return POUND_SUCCESS;
}
