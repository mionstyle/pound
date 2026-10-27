#!/usr/bin/env python3
"""ppm2png.py <in.ppm>... [--max N]: writes <in>.png next to each PPM (no dependencies).

Images wider than N pixels (default 960) are downscaled by an integer factor (nearest), so
frame dumps (RECOMP_DUMP_FRAMES, RECOMP_VK_DUMP_TARGETS) are quick to look at. Also prints
the mean and maximum of each channel (a black image shows max 0).
"""
import struct
import sys
import zlib


def read_ppm(path):
    d = open(path, 'rb').read()
    fields, pos = [], 0
    while len(fields) < 4:
        while d[pos:pos + 1].isspace():
            pos += 1
        if d[pos:pos + 1] == b'#':
            pos = d.index(b'\n', pos)
            continue
        end = pos
        while not d[end:end + 1].isspace():
            end += 1
        fields.append(d[pos:end])
        pos = end
    return int(fields[1]), int(fields[2]), d[pos + 1:]


def write_png(path, w, h, rgb):
    def chunk(t, data):
        return struct.pack('>I', len(data)) + t + data + struct.pack('>I', zlib.crc32(t + data) & 0xFFFFFFFF)
    raw = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def main(argv):
    mx = 960
    if '--max' in argv:
        mx = int(argv[argv.index('--max') + 1])
        del argv[argv.index('--max'):argv.index('--max') + 2]
    for p in argv[1:]:
        w, h, px = read_ppm(p)
        f = max(1, -(-w // mx))
        ow, oh = w // f, h // f
        if f > 1:
            rows = []
            for y in range(oh):
                row = px[(y * f) * w * 3:(y * f + 1) * w * 3]
                rows.append(b''.join(row[x * f * 3:x * f * 3 + 3] for x in range(ow)))
            out = b''.join(rows)
        else:
            out = px[:w * h * 3]
        write_png(p[:-4] + '.png', ow, oh, out)
        stats = []
        for c in range(3):
            ch = out[c::3]
            stats.append('%d/%d' % (sum(ch) // max(1, len(ch)), max(ch) if ch else 0))
        print('%s %dx%d mean/max r %s g %s b %s' % (p, w, h, *stats))


if __name__ == '__main__':
    main(sys.argv)
