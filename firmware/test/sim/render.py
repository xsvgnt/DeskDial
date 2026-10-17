"""Turns the simulator's screen dumps (*.raw, 360x360 RGB565 with the bytes
swapped, as LV_COLOR_16_SWAP=1 stores them) into PNG files, with the corners
outside the round panel greyed out. Standard library only.

    python3 render.py build/screens        # every *.raw below that folder
"""
# SPDX-License-Identifier: MIT
import pathlib
import struct
import sys
import zlib

W = H = 360


def to_rgb(raw: bytes) -> bytes:
    out = bytearray()
    r2 = (W / 2) ** 2
    for y in range(H):
        out.append(0)  # PNG filter: none
        for x in range(W):
            if (x - W / 2 + 0.5) ** 2 + (y - H / 2 + 0.5) ** 2 > r2:
                out += b"\x30\x30\x30"  # outside the round glass
                continue
            i = (y * W + x) * 2
            v = raw[i] << 8 | raw[i + 1]  # swapped RGB565 back to normal order
            r, g, b = v >> 11, (v >> 5) & 0x3F, v & 0x1F
            out += bytes((r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2))
    return bytes(out)


def png(rgb: bytes) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    head = struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", head) + chunk(b"IDAT", zlib.compress(rgb, 9)) + chunk(b"IEND", b"")


def main() -> None:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "build/screens")
    files = sorted(root.rglob("*.raw"))
    for f in files:
        f.with_suffix(".png").write_bytes(png(to_rgb(f.read_bytes())))
    print(f"{len(files)} screens rendered under {root}")


if __name__ == "__main__":
    main()
