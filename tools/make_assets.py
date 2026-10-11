#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Andres Leroi
"""Converts the PNG artwork in DIGIWAVE_Firmware_Grafiken into C arrays for LVGL.

Run from the project root:  python3 tools/make_assets.py
Writes src/assets/assets.c and src/assets/assets.h. Needs only the standard library.
"""
import os
import struct
import sys
import zlib

SRC = "DIGIWAVE_Firmware_Grafiken"
OUT = "src/assets"

TILE = 86  # edge length of the menu tiles on the display

# name in the firmware -> (file, format[, size to scale to])
#   rgb565    opaque picture
#   rgb565a8  colour picture with transparency
#   a8        white icon, coloured at runtime through its alpha channel
#   raw       the file itself, served by the web interface
IMAGES = [
    ("splash", "splash_480x222.png", "rgb565"),
    ("wordmark", "wordmark_160x28.png", "rgb565a8"),
    ("tile_ft8", "tile_ft8_256x256.png", "rgb565a8", TILE),
    ("tile_ft4", "tile_ft4_256x256.png", "rgb565a8", TILE),
    ("tile_cw", "tile_cw_256x256.png", "rgb565a8", TILE),
    ("tile_rtty", "tile_rtty_256x256.png", "rgb565a8", TILE),
    ("tile_navtex", "tile_navtex_256x256.png", "rgb565a8", TILE),
    ("tile_wefax", "tile_wefax_256x256.png", "rgb565a8", TILE),
    ("tile_sstv", "tile_sstv_256x256.png", "rgb565a8", TILE),
    ("tile_logbook", "tile_logbook_256x256.png", "rgb565a8", TILE),
    ("tile_setup", "tile_setup_256x256.png", "rgb565a8", TILE),
    ("tile_wifi", "tile_wifi_256x256.png", "rgb565a8", TILE),
    ("status_gps_fix", "status_gps_fix_16x16.png", "a8"),
    ("status_gps_no_fix", "status_gps_no_fix_16x16.png", "a8"),
    ("status_wifi_on", "status_wifi_on_16x16.png", "a8"),
    ("status_sd_present", "status_sd_present_16x16.png", "a8"),
    ("status_sd_missing", "status_sd_missing_16x16.png", "a8"),
    ("status_tx", "status_tx_16x16.png", "a8"),
    ("status_rx", "status_rx_16x16.png", "a8"),
    ("status_battery_0", "status_battery_0_16x16.png", "a8"),
    ("status_battery_1", "status_battery_1_16x16.png", "a8"),
    ("status_battery_2", "status_battery_2_16x16.png", "a8"),
    ("status_battery_3", "status_battery_3_16x16.png", "a8"),
    ("web_icon_png", "web_icon_180x180.png", "raw"),
]


def read_png(path):
    """Returns (width, height, list of rows of (r, g, b, a)). 8-bit, not interlaced."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG file")
    pos, idat, palette, trns = 8, b"", None, None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = body
        elif kind == b"tRNS":
            trns = body
        elif kind == b"IDAT":
            idat += body
    if depth != 8 or interlace != 0:
        raise ValueError(f"{path}: only 8-bit, non-interlaced PNGs are supported")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = width * channels
    rows, prev = [], bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        ftype, line = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if ftype == 1:
                line[i] = (line[i] + a) & 255
            elif ftype == 2:
                line[i] = (line[i] + b) & 255
            elif ftype == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        row = []
        for x in range(width):
            px = line[x * channels:(x + 1) * channels]
            if ctype == 6:
                row.append(tuple(px))
            elif ctype == 2:
                row.append((px[0], px[1], px[2], 255))
            elif ctype == 0:
                row.append((px[0], px[0], px[0], 255))
            elif ctype == 4:
                row.append((px[0], px[0], px[0], px[1]))
            else:
                i = px[0]
                alpha = trns[i] if trns and i < len(trns) else 255
                row.append((palette[3 * i], palette[3 * i + 1], palette[3 * i + 2], alpha))
        rows.append(row)
    return width, height, rows


def scale(rows, width, height, size):
    """Shrinks a picture to size x size by averaging the source pixels each target pixel covers."""
    out = []
    fx, fy = width / size, height / size
    for ty in range(size):
        y0, y1 = ty * fy, (ty + 1) * fy
        row = []
        for tx in range(size):
            x0, x1 = tx * fx, (tx + 1) * fx
            acc = [0.0, 0.0, 0.0, 0.0]
            area = 0.0
            for sy in range(int(y0), min(height, int(y1) + 1)):
                wy = min(y1, sy + 1) - max(y0, sy)
                if wy <= 0:
                    continue
                for sx in range(int(x0), min(width, int(x1) + 1)):
                    w = (min(x1, sx + 1) - max(x0, sx)) * wy
                    if w <= 0:
                        continue
                    r, g, b, a = rows[sy][sx]
                    # Colours are weighted by their opacity so that transparent pixels do not darken edges
                    acc[0] += r * a * w
                    acc[1] += g * a * w
                    acc[2] += b * a * w
                    acc[3] += a * w
                    area += w
            if acc[3] > 0:
                row.append((round(acc[0] / acc[3]), round(acc[1] / acc[3]), round(acc[2] / acc[3]), round(acc[3] / area)))
            else:
                row.append((0, 0, 0, 0))
        out.append(row)
    return size, size, out


def rgb565(r, g, b):
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    return bytes((v & 255, v >> 8))


def c_array(name, data):
    lines = [f"static const uint8_t {name}_data[] = {{"]
    for i in range(0, len(data), 20):
        lines.append("    " + ",".join(str(b) for b in data[i:i + 20]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    os.makedirs(OUT, exist_ok=True)
    c = ["// SPDX-License-Identifier: GPL-3.0-or-later", "// Copyright (C) 2026 Andres Leroi", "// Generated by tools/make_assets.py from DIGIWAVE_Firmware_Grafiken - do not edit.", "",
         '#include "assets.h"', ""]
    h = ["// SPDX-License-Identifier: GPL-3.0-or-later", "// Copyright (C) 2026 Andres Leroi", "// Generated by tools/make_assets.py - do not edit.", "#pragma once", "", "#include <lvgl.h>", "",
         "#ifdef __cplusplus", 'extern "C" {', "#endif", ""]
    for name, filename, fmt, *target in IMAGES:
        path = os.path.join(SRC, filename)
        if fmt == "raw":
            data = open(path, "rb").read()
            c += [f"const uint8_t {name}[] = {{"]
            c += ["    " + ",".join(str(b) for b in data[i:i + 20]) + "," for i in range(0, len(data), 20)]
            c += ["};", f"const unsigned int {name}_size = {len(data)};", ""]
            h += [f"extern const uint8_t {name}[];", f"extern const unsigned int {name}_size;"]
            continue
        width, height, rows = read_png(path)
        if target:
            width, height, rows = scale(rows, width, height, target[0])
        pixels = [px for row in rows for px in row]
        if fmt == "rgb565":
            data = b"".join(rgb565(r, g, b) for r, g, b, _ in pixels)
            cf, stride = "LV_COLOR_FORMAT_RGB565", width * 2
        elif fmt == "rgb565a8":
            data = b"".join(rgb565(r, g, b) for r, g, b, _ in pixels) + bytes(a for _, _, _, a in pixels)
            cf, stride = "LV_COLOR_FORMAT_RGB565A8", width * 2
        else:
            data = bytes(a for _, _, _, a in pixels)
            cf, stride = "LV_COLOR_FORMAT_A8", width
        c += [c_array(name, data), "",
              f"const lv_image_dsc_t img_{name} = {{",
              f"    .header = {{.magic = LV_IMAGE_HEADER_MAGIC, .cf = {cf}, .flags = 0, .w = {width}, .h = {height}, .stride = {stride}}},",
              f"    .data_size = sizeof({name}_data),",
              f"    .data = {name}_data,",
              "};", ""]
        h.append(f"extern const lv_image_dsc_t img_{name};  // {width} x {height}")
        print(f"{name}: {width} x {height}, {fmt}, {len(data)} bytes")
    h += ["", "#ifdef __cplusplus", "}", "#endif", ""]
    open(os.path.join(OUT, "assets.c"), "w").write("\n".join(c))
    open(os.path.join(OUT, "assets.h"), "w").write("\n".join(h))


if __name__ == "__main__":
    sys.exit(main())
