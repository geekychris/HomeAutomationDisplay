"""Draw the add-on's icon.png (128x128) and logo.png (250x100): a yellow
board with a dark screen of tiles. No dependencies; run from anywhere:
    python3 scripts/make_icons.py
"""
import struct
import zlib
from pathlib import Path

HUB = Path(__file__).resolve().parent.parent / "hub"
YELLOW, DARK, CARD, ON = (0xE3, 0xC1, 0x3B), (0x15, 0x17, 0x1A), (0x3A, 0x40, 0x48), (0xF5, 0xA6, 0x23)


def png(path, w, h, pixel):
    rows = b"".join(b"\0" + b"".join(bytes(pixel(x, y)) for x in range(w)) for y in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")
    path.write_bytes(data)


def inside(x, y, x0, y0, x1, y1, r):
    if not (x0 <= x < x1 and y0 <= y < y1):
        return False
    cx = min(max(x, x0 + r), x1 - 1 - r)
    cy = min(max(y, y0 + r), y1 - 1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def board(ox, oy, s):
    """A display at (ox, oy), s pixels per unit, 32 x 24 units."""
    tiles = [(5, 5, 15, 12, True), (17, 5, 27, 12, False), (5, 14, 15, 21, False), (17, 14, 27, 21, True)]

    def px(x, y):
        u, v = (x - ox) / s, (y - oy) / s
        if not inside(u, v, 0, 0, 32, 24, 2.5):
            return None
        if not inside(u, v, 3, 3, 29, 23, 0.8):
            return YELLOW
        for x0, y0, x1, y1, on in tiles:
            if inside(u, v, x0, y0, x1, y1, 1):
                return ON if on else CARD
        return DARK
    return px


def draw(path, w, h, ox, oy, s):
    px = board(ox, oy, s)
    png(path, w, h, lambda x, y: (*c, 255) if (c := px(x, y)) else (0, 0, 0, 0))


draw(HUB / "icon.png", 128, 128, 0, 16, 4)
draw(HUB / "logo.png", 250, 100, 77, 2, 3)
print("wrote", HUB / "icon.png", HUB / "logo.png")
