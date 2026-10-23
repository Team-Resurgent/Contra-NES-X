#!/usr/bin/env python3
"""The Xbox dashboard's two pictures: the title image (128x128) beside the
game and the save image (64x64) beside its save.

Both are the title screen's CONTRA logo, made to fit the square (averaged
down, not cut) across the middle, on the title's black. Nothing of it is
committed: the port's own core takes the picture (setup.py runs
tools/hosttest to the title, build/icons/shot_*.png), the BMPs land in
Platform/xbox/gen/ (gitignored), and RXDK's bundler turns them into the DXT1
.xbx the dashboard needs (Platform/xbox/titleimage.rdf, saveimage.rdf).

    python tools/xbox/make_icons.py build/icons/shot_00400.png
"""
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "Platform", "xbox", "gen")
# where the logo is looked for on the title, in NES pixels (hosttest's PNGs
# are two PNG pixels to one): below the KONAMI emblem, above PLAY SELECT and
# Bill and Lance
LOGO_AREA = (0, 38, 256, 116)
MARGIN = 1 / 32                 # of the icon, each side


def read_png(path):
    """8-bit RGB or RGBA, non-interlaced. Returns (w, h, bpp, rows)."""
    with open(path, "rb") as f:
        d = f.read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("make_icons: %s is not a PNG" % path)
    p, idat, hdr = 8, [], None
    while p < len(d):
        n, t = struct.unpack(">I4s", d[p:p + 8])
        c = d[p + 8:p + 8 + n]
        p += 12 + n
        if t == b"IHDR":
            hdr = struct.unpack(">IIBBBBB", c)
        elif t == b"IDAT":
            idat.append(c)
        elif t == b"IEND":
            break
    w, h, depth, ctype, _, _, interlace = hdr
    if depth != 8 or ctype not in (2, 6) or interlace != 0:
        raise SystemExit("make_icons: %s is not 8-bit RGB(A) non-interlaced" % path)
    bpp = 4 if ctype == 6 else 3
    raw = zlib.decompress(b"".join(idat))
    st = w * bpp
    rows, prev, i = [], bytes(st), 0
    for _ in range(h):
        flt = raw[i]
        line = bytearray(raw[i + 1:i + 1 + st])
        i += 1 + st
        if flt == 1:
            for x in range(bpp, st):
                line[x] = (line[x] + line[x - bpp]) & 255
        elif flt == 2:
            line = bytearray((a + b) & 255 for a, b in zip(line, prev))
        elif flt == 3:
            for x in range(st):
                a = line[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 255
        elif flt == 4:
            for x in range(st):
                a = line[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        rows.append(bytes(line))
        prev = rows[-1]
    return w, h, bpp, rows


def nes_pixels(rows, bpp):
    """The screenshot at one pixel per NES pixel: [y][x] = (r, g, b)."""
    return [[tuple(r[x * 2 * bpp:x * 2 * bpp + 3]) for x in range(len(r) // (2 * bpp))]
            for r in rows[::2]]


def logo_box(pix):
    """The bounding box of what is not black in LOGO_AREA, or None."""
    x0, y0, x1, y1 = LOGO_AREA
    xs, ys = [], []
    for y in range(y0, min(y1, len(pix))):
        for x in range(x0, min(x1, len(pix[y]))):
            if pix[y][x] != (0, 0, 0):
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return min(xs), min(ys), max(xs) + 1, max(ys) + 1


def fit(pix, box, size):
    """The box, scaled by one factor to fit inside `size` less the margins,
    each icon pixel the average of the NES pixels under it; centred on
    black."""
    bx0, by0, bx1, by1 = box
    bw, bh = bx1 - bx0, by1 - by0
    room = size * (1 - 2 * MARGIN)
    k = min(room / bw, room / bh)
    ow, oh = bw * k, bh * k
    ox, oy = (size - ow) / 2, (size - oh) / 2
    out = []
    for y in range(size):
        line = []
        for x in range(size):
            # the icon pixel's square, in the box's pixels
            sx0, sx1 = (x - ox) / k, (x + 1 - ox) / k
            sy0, sy1 = (y - oy) / k, (y + 1 - oy) / k
            acc, area = [0.0, 0.0, 0.0], 0.0
            for sy in range(max(0, int(sy0)), min(bh, int(sy1) + 1)):
                hy = min(sy1, sy + 1) - max(sy0, sy)
                if hy <= 0:
                    continue
                for sx in range(max(0, int(sx0)), min(bw, int(sx1) + 1)):
                    wx = min(sx1, sx + 1) - max(sx0, sx)
                    if wx <= 0:
                        continue
                    c = pix[by0 + sy][bx0 + sx]
                    for i in range(3):
                        acc[i] += c[i] * wx * hy
            area = (sx1 - sx0) * (sy1 - sy0)
            line.append(tuple(int(round(v / area)) for v in acc))
        out.append(line)
    return out


def write_bmp(path, pix):
    h, w = len(pix), len(pix[0])
    pad = (4 - (w * 3) % 4) % 4
    data = bytearray()
    for row in reversed(pix):
        for r, g, b in row:
            data += bytes((b, g, r))
        data += b"\0" * pad
    hdr = struct.pack("<2sIHHI", b"BM", 54 + len(data), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(data), 2835, 2835, 0, 0)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(hdr + info + data)
    os.replace(tmp, path)


def main():
    if len(sys.argv) < 2 or not os.path.isfile(sys.argv[1]):
        raise SystemExit("make_icons: give the title's screenshot (python setup.py makes it)")
    src = sys.argv[1]
    os.makedirs(OUT, exist_ok=True)
    outs = [(os.path.join(OUT, "titleimage.bmp"), 128), (os.path.join(OUT, "saveimage.bmp"), 64)]
    stamp = os.path.getmtime(src)
    if all(os.path.isfile(p) and os.path.getmtime(p) >= stamp for p, _ in outs):
        print("make_icons: up to date")
        return
    w, h, bpp, rows = read_png(src)
    if w < 512 or h < 2 * LOGO_AREA[3]:
        raise SystemExit("make_icons: %s is not one of hosttest's screenshots" % src)
    pix = nes_pixels(rows, bpp)
    box = logo_box(pix)
    # the logo is most of the width: anything much smaller is not the title.
    # Then the icons are the whole picture a television shows, not nothing
    if box is None or box[2] - box[0] < 120 or box[3] - box[1] < 24:
        print("make_icons: WARNING: no logo in %s (not the title screen?); the icons are the "
              "whole picture" % src)
        box = (0, 8, len(pix[0]), min(len(pix), 232))
    for path, size in outs:
        write_bmp(path, fit(pix, box, size))
    print("make_icons: the title's logo (%dx%d) -> titleimage.bmp (128x128), saveimage.bmp (64x64)"
          % (box[2] - box[0], box[3] - box[1]))


if __name__ == "__main__":
    main()
