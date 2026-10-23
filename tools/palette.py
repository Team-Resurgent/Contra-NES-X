#!/usr/bin/env python3
"""The NES's 64 colours, worked out from the 2C02's composite signal; the
table in src/core/ppu.c is this script's output.

The PPU makes each colour as a square wave over twelve phases of the colour
subcarrier: hue 1-12 picks which six phases are high, the level (0-3) the
two voltages it swings between; hue 0 is the high voltage throughout (the
greys), hue 13 the low, 14 and 15 black. A television decodes that as YIQ:
Y the average over the cycle, I and Q the wave against the subcarrier at
two phases ninety degrees apart.

    python tools/palette.py          # prints the C table
"""
import math

# the 2C02's output voltages, per level (low and high), and its black and white
LOW = (0.350, 0.518, 0.962, 1.550)
HIGH = (1.094, 1.506, 1.962, 1.962)
BLACK, WHITE = 0.518, 1.962
HUE_SHIFT = 120.0        # degrees: colour 0x16 red, 0x1A green, 0x12 blue
SATURATION = 1.2         # a little over the decode: the colours as NES palettes show them
GAMMA = 1.0              # the voltages taken as they are


def colour(c):
    level, hue = (c >> 4) & 3, c & 15
    if hue >= 14:
        return 0, 0, 0
    y = i = q = 0.0
    for phase in range(12):
        if hue == 0:
            v = HIGH[level]
        elif hue == 13:
            v = LOW[level]
        else:
            v = HIGH[level] if (hue + phase) % 12 < 6 else LOW[level]
        s = (v - BLACK) / (WHITE - BLACK)
        a = math.radians(phase * 30 + HUE_SHIFT)
        y += s
        i += s * math.cos(a)
        q += s * math.sin(a)
    y, i, q = y / 12, i / 12 * SATURATION, q / 12 * SATURATION
    rgb = (y + 0.956 * i + 0.621 * q,
           y - 0.272 * i - 0.647 * q,
           y - 1.106 * i + 1.703 * q)
    out = []
    for v in rgb:
        v = min(max(v, 0.0), 1.0) ** GAMMA
        out.append(int(round(v * 255)))
    return tuple(out)


def main():
    rows = []
    for c in range(64):
        r, g, b = colour(c)
        rows.append("0x%02X%02X%02X" % (r, g, b))
    print("const uint32_t ppu_rgb[64] = {")
    for k in range(0, 64, 8):
        print("    " + ", ".join(rows[k:k + 8]) + ",")
    print("};")


if __name__ == "__main__":
    main()
