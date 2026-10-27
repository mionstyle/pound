// From LAS Recomp (github.com/peiikai/LASRecomp, runtime/nvn/, MIT licence, (c) 2026 Peiikai).
// Tegra X1 block-linear layout. A GOB is 64 bytes x 8 rows; blocks stack GOBs vertically
// (block height) and in depth (block depth). Levels shrink their block height as they shrink.
#include "tegra.h"

#include <string.h>

static uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
static uint64_t align_up(uint64_t a, uint64_t b) { return (a + b - 1) / b * b; }

uint32_t tegra_default_block_height_log2(uint32_t height_in_blocks) {
    uint32_t gobs = div_up(height_in_blocks, 8);
    uint32_t log2 = 0;
    while ((1u << log2) < gobs && log2 < 4) log2++;
    return log2;
}

static uint32_t block_depth_for(uint32_t depth) {
    uint32_t log2 = 0;
    while ((1u << log2) < depth && log2 < 5) log2++;
    return log2;
}

void tegra_layout_init(TexLayout *t) {
    uint32_t hb = div_up(t->height, t->fmt->bh);
    t->block_height_log2 = tegra_default_block_height_log2(hb);
    t->block_depth_log2 = t->depth > 1 ? block_depth_for(t->depth) : 0;
}

// Block height shrinks with the level so a block never exceeds the level height.
static uint32_t level_block_height(uint32_t bh_log2, uint32_t hb) {
    while (bh_log2 > 0 && (8u << (bh_log2 - 1)) >= hb) bh_log2--;
    return bh_log2;
}

static uint32_t level_block_depth(uint32_t bd_log2, uint32_t d) {
    while (bd_log2 > 0 && (1u << (bd_log2 - 1)) >= d) bd_log2--;
    return bd_log2;
}

static uint64_t level_size(const TexLayout *t, uint32_t level, LevelLayout *out) {
    uint32_t w = t->width >> level, h = t->height >> level, d = t->depth >> level;
    if (!w) w = 1;
    if (!h) h = 1;
    if (!d) d = 1;
    uint32_t wb = div_up(w, t->fmt->bw), hb = div_up(h, t->fmt->bh);
    uint32_t bh = level_block_height(t->block_height_log2, hb);
    uint32_t bd = level_block_depth(t->block_depth_log2, d);
    uint32_t gob_rows = 8u << bh, gob_depth = 1u << bd;
    uint64_t width_gobs = div_up(wb * t->fmt->bpb, 64);
    uint64_t size = width_gobs * 512 * (1u << bh) * (1u << bd) * div_up(hb, gob_rows) * div_up(d, gob_depth);
    if (out) {
        out->w = w; out->h = h; out->d = d;
        out->wb = wb; out->hb = hb;
        out->block_height_log2 = bh;
        out->block_depth_log2 = bd;
    }
    return size;
}

LevelLayout tegra_level(const TexLayout *t, uint32_t level) {
    LevelLayout l = {0};
    uint64_t off = 0;
    for (uint32_t i = 0; i < level; i++) off += level_size(t, i, NULL);
    level_size(t, level, &l);
    l.offset = off;
    return l;
}

uint64_t tegra_layer_size(const TexLayout *t) {
    uint64_t size = 0;
    for (uint32_t i = 0; i < t->levels; i++) size += level_size(t, i, NULL);
    if (t->layers > 1) {
        // layers are aligned to the level-0 block size
        uint32_t bh = t->block_height_log2;
        size = align_up(size, 512ull << bh);
    }
    return size;
}

uint64_t tegra_total_size(const TexLayout *t) {
    return tegra_layer_size(t) * (t->layers ? t->layers : 1);
}

// Offset of byte (x bytes, y rows, z slices) within a block-linear surface.
static inline uint64_t gob_offset(uint32_t xb, uint32_t y, uint32_t z, uint32_t width_gobs, uint32_t bh_log2,
                                  uint32_t bd_log2, uint64_t slice_blocks_rows) {
    uint32_t gob_x = xb >> 6, gob_y = y >> 3;
    uint32_t block_y = gob_y >> bh_log2, gob_in_block_y = gob_y & ((1u << bh_log2) - 1);
    uint32_t block_z = z >> bd_log2, z_in_block = z & ((1u << bd_log2) - 1);
    uint64_t block_size = 512ull << (bh_log2 + bd_log2);
    uint64_t off = ((uint64_t)block_z * slice_blocks_rows + block_y) * width_gobs * block_size;
    off += (uint64_t)gob_x * block_size;
    off += ((uint64_t)z_in_block << bh_log2 | gob_in_block_y) * 512;
    // within the GOB: 64x8 bytes arranged in 16-byte sectors
    uint32_t xi = xb & 63, yi = y & 7;
    off += ((xi >> 5) << 8) | ((yi >> 1) << 6) | (((xi >> 4) & 1) << 5) | ((yi & 1) << 4) | (xi & 0x0F);
    return off;
}

static void copy_surface(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                         uint32_t bh_log2, uint32_t bd_log2, bool to_linear) {
    uint32_t row_bytes = wb * bpb;
    uint32_t width_gobs = (row_bytes + 63) / 64;
    uint32_t gob_rows = 8u << bh_log2;
    uint64_t blocks_rows = (hb + gob_rows - 1) / gob_rows;
    for (uint32_t z = 0; z < d; z++) {
        for (uint32_t y = 0; y < hb; y++) {
            uint8_t *lin = dst + ((uint64_t)z * hb + y) * row_bytes;
            const uint8_t *lin_src = src + ((uint64_t)z * hb + y) * row_bytes;
            // 16-byte runs are contiguous in both layouts
            for (uint32_t x = 0; x < row_bytes; x += 16) {
                uint32_t n = row_bytes - x < 16 ? row_bytes - x : 16;
                uint64_t o = gob_offset(x, y, z, width_gobs, bh_log2, bd_log2, blocks_rows);
                if (to_linear) memcpy(lin + x, src + o, n);
                else memcpy(dst + o, lin_src + x, n);
            }
        }
    }
}

void tegra_deswizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                     uint32_t bh_log2, uint32_t bd_log2) {
    copy_surface(src, dst, wb, hb, d, bpb, bh_log2, bd_log2, true);
}

void tegra_swizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                   uint32_t bh_log2, uint32_t bd_log2) {
    copy_surface(src, dst, wb, hb, d, bpb, bh_log2, bd_log2, false);
}

#define C(bpb) {bpb, 1, 1, FMT_COLOR, 0}
#define B(bpb, w, h) {bpb, w, h, FMT_COMPRESSED, 0}

static const FormatInfo formats[] = {
    [0x00] = C(0),
    [0x01] = C(1), [0x02] = C(1), [0x03] = C(1), [0x04] = C(1),                 // R8
    [0x05] = C(2), [0x06] = C(2), [0x07] = C(2), [0x08] = C(2), [0x09] = C(2),  // R16
    [0x0A] = C(4), [0x0B] = C(4), [0x0C] = C(4),                                // R32
    [0x0D] = C(2), [0x0E] = C(2), [0x0F] = C(2), [0x10] = C(2),                 // RG8
    [0x11] = C(4), [0x12] = C(4), [0x13] = C(4), [0x14] = C(4), [0x15] = C(4),  // RG16
    [0x16] = C(8), [0x17] = C(8), [0x18] = C(8),                                // RG32
    [0x19] = C(3), [0x1A] = C(3), [0x1B] = C(3), [0x1C] = C(3),                 // RGB8
    [0x1D] = C(6), [0x1E] = C(6), [0x1F] = C(6), [0x20] = C(6), [0x21] = C(6),  // RGB16
    [0x22] = C(12), [0x23] = C(12), [0x24] = C(12),                             // RGB32
    [0x25] = C(4), [0x26] = C(4), [0x27] = C(4), [0x28] = C(4),                 // RGBA8
    [0x29] = C(8), [0x2A] = C(8), [0x2B] = C(8), [0x2C] = C(8), [0x2D] = C(8),  // RGBA16
    [0x2E] = C(16), [0x2F] = C(16), [0x30] = C(16),                             // RGBA32
    [0x31] = {1, 1, 1, FMT_STENCIL, 0},
    [0x32] = {2, 1, 1, FMT_DEPTH, 0},
    [0x33] = {4, 1, 1, FMT_DEPTH, 0},
    [0x34] = {4, 1, 1, FMT_DEPTH, 0},
    [0x35] = {4, 1, 1, FMT_DEPTH_STENCIL, 0},
    [0x36] = {8, 1, 1, FMT_DEPTH_STENCIL, 0},
    [0x37] = C(4), [0x38] = C(4),                                               // RGBX8/RGBA8 sRGB
    [0x39] = C(2), [0x3A] = C(2), [0x3B] = C(2), [0x3C] = C(2),                 // RGBA4, RGB5, RGB5A1, RGB565
    [0x3D] = C(4), [0x3E] = C(4), [0x3F] = C(4), [0x40] = C(4),                 // RGB10A2(UI), R11G11B10F, RGB9E5
    [0x41] = B(8, 4, 4), [0x42] = B(8, 4, 4), [0x43] = B(16, 4, 4), [0x44] = B(16, 4, 4),
    [0x45] = B(8, 4, 4), [0x46] = B(8, 4, 4), [0x47] = B(16, 4, 4), [0x48] = B(16, 4, 4),
    [0x49] = B(8, 4, 4), [0x4A] = B(8, 4, 4), [0x4B] = B(16, 4, 4), [0x4C] = B(16, 4, 4),
    [0x4D] = B(16, 4, 4), [0x4E] = B(16, 4, 4), [0x4F] = B(16, 4, 4), [0x50] = B(16, 4, 4),
    // integer-to-float vertex formats
    [0x51] = C(1), [0x52] = C(1), [0x53] = C(2), [0x54] = C(2), [0x55] = C(4), [0x56] = C(4),
    [0x57] = C(2), [0x58] = C(2), [0x59] = C(4), [0x5A] = C(4), [0x5B] = C(8), [0x5C] = C(8),
    [0x5D] = C(3), [0x5E] = C(3), [0x5F] = C(6), [0x60] = C(6), [0x61] = C(12), [0x62] = C(12),
    [0x63] = C(4), [0x64] = C(4), [0x65] = C(8), [0x66] = C(8), [0x67] = C(16), [0x68] = C(16),
    [0x69] = C(4), [0x6A] = C(4), [0x6B] = C(4), [0x6C] = C(4),
    [0x6D] = C(4), [0x6E] = C(4), [0x6F] = C(4), [0x70] = C(4),                 // RGBX8
    [0x71] = C(8), [0x72] = C(8), [0x73] = C(8), [0x74] = C(8), [0x75] = C(8),  // RGBX16
    [0x76] = C(16), [0x77] = C(16), [0x78] = C(16),                             // RGBX32
    // ASTC (linear then sRGB)
    [0x79] = B(16, 4, 4), [0x7A] = B(16, 5, 4), [0x7B] = B(16, 5, 5), [0x7C] = B(16, 6, 5),
    [0x7D] = B(16, 6, 6), [0x7E] = B(16, 8, 5), [0x7F] = B(16, 8, 6), [0x80] = B(16, 8, 8),
    [0x81] = B(16, 10, 5), [0x82] = B(16, 10, 6), [0x83] = B(16, 10, 8), [0x84] = B(16, 10, 10),
    [0x85] = B(16, 12, 10), [0x86] = B(16, 12, 12),
    [0x87] = B(16, 4, 4), [0x88] = B(16, 5, 4), [0x89] = B(16, 5, 5), [0x8A] = B(16, 6, 5),
    [0x8B] = B(16, 6, 6), [0x8C] = B(16, 8, 5), [0x8D] = B(16, 8, 6), [0x8E] = B(16, 8, 8),
    [0x8F] = B(16, 10, 5), [0x90] = B(16, 10, 6), [0x91] = B(16, 10, 8), [0x92] = B(16, 10, 10),
    [0x93] = B(16, 12, 10), [0x94] = B(16, 12, 12),
    [0x95] = C(2), [0x96] = C(2), [0x97] = C(2),                                 // BGR5, BGR5A1, A1BGR5
    [0x98] = C(4), [0x99] = C(4), [0x9A] = C(4), [0x9B] = C(4),                  // BGRX8, BGRA8 (+sRGB)
};

const FormatInfo *nvn_format_info(uint32_t f) {
    static const FormatInfo unknown = {4, 1, 1, FMT_COLOR, 0};
    if (f >= sizeof formats / sizeof *formats || !formats[f].bpb) return &unknown;
    return &formats[f];
}
