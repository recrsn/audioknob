# Renders the UI's static backgrounds into firmware/s3/src/ui/assets/ui_bg_data.{c,h}: the clock
# wallpaper and, per source tint, a now-playing background (a radial wash with
# the rounded, gradient art tile baked in) plus ARGB8888 corner overlays that
# round the cover art drawn over that tile. Everything is composited in float
# and 4x4 Bayer-dithered to RGB565, since LVGL's own gradients quantize
# straight to RGB565 and band. Pixels are native-endian RGB565; ui.c swaps
# them for the panel at load.
# Usage: python3 tools/gen_backgrounds.py
import math
import os

W = H = 360
BG = 0x0B0B10

# Mirror ui.c (ART_TILE_SIZE, WIRELESS_TILE_Y, USB_TILE_Y); ui.c asserts them.
TILE_SIZE = 132
TILE_RADIUS = 28
WIRELESS_TILE_Y = -41
USB_TILE_Y = -33

NOW_PLAYING = [
    ("airplay", 0x4EA1FF, WIRELESS_TILE_Y),
    ("bluetooth", 0x7C6CFF, WIRELESS_TILE_Y),
    ("usb", 0x2DD4BF, USB_TILE_Y),
]

# Stand-in for the mock's blurred album-art wash: the tint, strongest behind
# the tile, easing out towards the rim.
WASH_PEAK = 0.24
WASH_FLOOR = 0.05
WASH_RADIUS = 250.0

# The mock's .idle-art blobs under its 50% black scrim.
WALLPAPER_BLOBS = [
    (60.0, 60.0, 160.0, 0x4EA1FF, 0.70),
    (W - 70.0, H - 70.0, 150.0, 0x7C6CFF, 0.60),
    (W - 60.0, 60.0, 130.0, 0xD946A8, 0.60),
    (60.0, H - 60.0, 130.0, 0x2DD4A0, 0.50),
]
SCRIM_KEEP = 0.5

BAYER4 = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def rgb(hex_):
    return ((hex_ >> 16) & 0xFF, (hex_ >> 8) & 0xFF, hex_ & 0xFF)


def mix(a, b, t):
    return tuple(x + (y - x) * t for x, y in zip(a, b))


# Eased rather than linear: a linear ramp ends in a slope break that reads as
# a ring even when dithered.
def falloff(dist, radius):
    t = dist / radius
    return 0.0 if t >= 1.0 else 1.0 - t * t * (3.0 - 2.0 * t)


def quantize(v, levels, threshold):
    q = int(v * (levels - 1) / 255.0 + threshold)
    return min(max(q, 0), levels - 1)


def dither565(c, x, y):
    th = (BAYER4[y & 3][x & 3] + 0.5) / 16.0
    return quantize(c[0], 32, th) << 11 | quantize(c[1], 64, th) << 5 | quantize(c[2], 32, th)


def expand565(v):
    r, g, b = v >> 11, (v >> 5) & 0x3F, v & 0x1F
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def wallpaper_pixel(x, y):
    c = rgb(BG)
    for cx, cy, radius, col, peak in WALLPAPER_BLOBS:
        a = peak * falloff(math.hypot(x - cx, y - cy), radius)
        if a > 0.0:
            c = mix(c, rgb(col), a)
    return tuple(v * SCRIM_KEEP for v in c)


def tile_origin(tile_y):
    return ((W - TILE_SIZE) // 2, (H - TILE_SIZE) // 2 + tile_y)


def wash_pixel(tint, x, y, tile_y):
    ox, oy = tile_origin(tile_y)
    d = math.hypot(x - (ox + TILE_SIZE / 2), y - (oy + TILE_SIZE / 2))
    return mix(rgb(BG), rgb(tint), WASH_FLOOR + (WASH_PEAK - WASH_FLOOR) * falloff(d, WASH_RADIUS))


# Fraction of the pixel centred at (x, y) inside the tile, from its signed
# distance to the rounded rect.
def tile_coverage(x, y, tile_y):
    ox, oy = tile_origin(tile_y)
    half = TILE_SIZE / 2
    qx = abs(x - (ox + half)) - (half - TILE_RADIUS)
    qy = abs(y - (oy + half)) - (half - TILE_RADIUS)
    sd = math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - TILE_RADIUS
    return min(max(0.5 - sd, 0.0), 1.0)


# The tile's own vertical gradient, tint at the top to the screen colour.
def tile_pixel(tint, y, tile_y):
    _, oy = tile_origin(tile_y)
    t = min(max((y + 0.5 - oy) / TILE_SIZE, 0.0), 1.0)
    return mix(rgb(tint), rgb(BG), t)


def render(pixel):
    return [dither565(pixel(x + 0.5, y + 0.5), x, y) for y in range(H) for x in range(W)]


def now_playing(tint, tile_y):
    def pixel(x, y):
        cov = tile_coverage(x, y, tile_y)
        wash = wash_pixel(tint, x, y, tile_y)
        return mix(wash, tile_pixel(tint, y, tile_y), cov) if cov else wash

    return render(pixel)


# Each corner restores what the background shows outside the tile's rounding,
# so square art drawn over the tile ends up rounded and antialiased. Fully
# covered pixels repeat the background's dithered value exactly.
def corners(bg, tint, tile_y):
    ox, oy = tile_origin(tile_y)
    out = []
    for cy, cx in ((0, 0), (0, 1), (1, 0), (1, 1)):
        px = []
        for ly in range(TILE_RADIUS):
            for lx in range(TILE_RADIUS):
                x = ox + cx * (TILE_SIZE - TILE_RADIUS) + lx
                y = oy + cy * (TILE_SIZE - TILE_RADIUS) + ly
                alpha = 1.0 - tile_coverage(x + 0.5, y + 0.5, tile_y)
                if alpha >= 1.0:
                    r, g, b = expand565(bg[y * W + x])
                else:
                    r, g, b = (round(v) for v in wash_pixel(tint, x + 0.5, y + 0.5, tile_y))
                px += [b, g, r, round(alpha * 255)]
        out.append(px)
    return out


def c_array(ctype, name, values, per_line, fmt):
    lines = [f"const {ctype} {name}[{len(values)}] = {{"]
    for i in range(0, len(values), per_line):
        lines.append("    " + ",".join(fmt % v for v in values[i : i + per_line]) + ",")
    lines.append("};\n")
    return "\n".join(lines)


def c_corners(name, quads):
    lines = [f"const uint8_t {name}[4][UI_BG_CORNER_BYTES] = {{"]
    for px in quads:
        lines.append("    {")
        for i in range(0, len(px), 32):
            lines.append("        " + ",".join(str(v) for v in px[i : i + 32]) + ",")
        lines.append("    },")
    lines.append("};\n")
    return "\n".join(lines)


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "firmware", "s3", "src", "ui", "assets")
    banner = "// Generated by tools/gen_backgrounds.py; do not edit.\n"
    header = [
        banner,
        "#pragma once\n",
        "#include <stdint.h>\n",
        f"#define UI_BG_W {W}",
        f"#define UI_BG_H {H}",
        f"#define UI_BG_TILE_SIZE {TILE_SIZE}",
        f"#define UI_BG_TILE_RADIUS {TILE_RADIUS}",
        f"#define UI_BG_WIRELESS_TILE_Y ({WIRELESS_TILE_Y})",
        f"#define UI_BG_USB_TILE_Y ({USB_TILE_Y})\n",
        "// RGB565, native-endian, UI_BG_W x UI_BG_H.",
        "extern const uint16_t ui_bg_wallpaper[UI_BG_W * UI_BG_H];",
    ]
    body = [banner, '#include "ui_bg_data.h"\n']
    body.append(c_array("uint16_t", "ui_bg_wallpaper", render(wallpaper_pixel), 16, "0x%04x"))
    for name, tint, tile_y in NOW_PLAYING:
        bg = now_playing(tint, tile_y)
        body.append(c_array("uint16_t", f"ui_bg_{name}", bg, 16, "0x%04x"))
        header.append(f"extern const uint16_t ui_bg_{name}[UI_BG_W * UI_BG_H];")
        body.append(c_corners(f"ui_bg_{name}_corners", corners(bg, tint, tile_y)))
    header.append("\n// ARGB8888 (B, G, R, A bytes), UI_BG_TILE_RADIUS square: top-left, top-right,")
    header.append("// bottom-left, bottom-right.")
    header.append("#define UI_BG_CORNER_BYTES (UI_BG_TILE_RADIUS * UI_BG_TILE_RADIUS * 4)")
    for name, _, _ in NOW_PLAYING:
        header.append(f"extern const uint8_t ui_bg_{name}_corners[4][UI_BG_CORNER_BYTES];")

    with open(os.path.join(root, "ui_bg_data.h"), "w") as f:
        f.write("\n".join(header) + "\n")
    with open(os.path.join(root, "ui_bg_data.c"), "w") as f:
        f.write("\n".join(body))


if __name__ == "__main__":
    main()
