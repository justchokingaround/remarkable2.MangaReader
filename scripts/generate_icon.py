#!/usr/bin/env python3
"""Generate a crisp 256x256 monochrome E-Ink launcher icon for rm2manga."""
import struct
import zlib
from pathlib import Path


def write_png_grayscale(path: Path, width: int, height: int, pixels: bytearray) -> None:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    raw = bytearray()
    for y in range(height):
        raw.append(0)  # Filter type 0 (None)
        raw.extend(pixels[y * width : (y + 1) * width])

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)  # 8-bit grayscale
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def main() -> None:
    w, h = 256, 256
    px = bytearray([255] * (w * h))

    def set_px(x: int, y: int, val: int) -> None:
        if 0 <= x < w and 0 <= y < h:
            px[y * w + x] = val

    def fill_rect(x0: int, y0: int, x1: int, y1: int, val: int) -> None:
        for y in range(max(0, y0), min(h, y1)):
            for x in range(max(0, x0), min(w, x1)):
                px[y * w + x] = val

    def stroke_rect(x0: int, y0: int, x1: int, y1: int, t: int, val: int = 0) -> None:
        fill_rect(x0, y0, x1, y0 + t, val)
        fill_rect(x0, y1 - t, x1, y1, val)
        fill_rect(x0, y0, x0 + t, y1, val)
        fill_rect(x1 - t, y0, x1, y1, val)

    def draw_line(x0: int, y0: int, x1: int, y1: int, thickness: int, val: int = 0) -> None:
        steps = max(abs(x1 - x0), abs(y1 - y0), 1)
        for i in range(steps + 1):
            cx = int(round(x0 + (x1 - x0) * i / steps))
            cy = int(round(y0 + (y1 - y0) * i / steps))
            for dy in range(-thickness // 2, thickness // 2 + 1):
                for dx in range(-thickness // 2, thickness // 2 + 1):
                    set_px(cx + dx, cy + dy, val)

    # Rounded outer card border (manga tankobon page with dynamic panels)
    margin = 18
    # Drop-offset shadow frame for physical tankobon book feel
    fill_rect(margin + 10, margin + 10, w - margin + 6, h - margin + 6, 0)
    fill_rect(margin, margin, w - margin, h - margin, 255)
    stroke_rect(margin, margin, w - margin, h - margin, 8, 0)

    # Classic manga asymmetric panel layout inside the page
    inner_l = margin + 18
    inner_r = w - margin - 18
    inner_t = margin + 18
    inner_b = h - margin - 18

    # Top-right & top-left split panel with diagonal action gutter
    mid_y = 128
    # Fill top-left panel with subtle manga screentone dots
    for y in range(inner_t + 4, mid_y - 5):
        split_x = int(132 + (y - inner_t) * 0.18)
        for x in range(inner_l + 4, split_x - 5):
            if (x + y) % 4 == 0 and (x - y) % 4 == 0:
                set_px(x, y, 80)

    # Top panel borders
    stroke_rect(inner_l, inner_t, inner_r, mid_y - 5, 5, 0)
    # White diagonal gutter + double black panel strokes
    draw_line(130, inner_t, 148, mid_y - 5, 13, 255)
    draw_line(125, inner_t, 143, mid_y - 5, 5, 0)
    draw_line(135, inner_t, 153, mid_y - 5, 5, 0)

    # Speed lines in top-right panel
    for offset in range(-20, 45, 13):
        draw_line(156, inner_t + 16 + max(0, offset), inner_r - 8, inner_t + 8 + max(0, offset // 2), 2, 0)

    # Bottom wide panel (solid black ink header band with crisp white 'M' emblem)
    stroke_rect(inner_l, mid_y + 5, inner_r, inner_b, 5, 0)
    fill_rect(inner_l + 5, mid_y + 10, inner_r - 5, inner_b - 5, 0)

    # Bold geometric 'M' inside bottom dark panel
    mx0, mx1 = 84, 172
    my0, my1 = mid_y + 24, inner_b - 18
    # Left & right pillars
    fill_rect(mx0, my0, mx0 + 18, my1, 255)
    fill_rect(mx1 - 18, my0, mx1, my1, 255)
    # Center V-strokes of M
    draw_line(mx0 + 9, my0 + 4, (mx0 + mx1) // 2, my1 - 14, 15, 255)
    draw_line(mx1 - 9, my0 + 4, (mx0 + mx1) // 2, my1 - 14, 15, 255)

    out_path = Path(__file__).resolve().parent.parent / "icon.png"
    write_png_grayscale(out_path, w, h, px)
    print(f"Wrote {out_path} ({out_path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
