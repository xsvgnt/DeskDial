"""Regenerates fixtures/*.bin: MSG_TEXT_IMG payloads (Japanese, Korean and
Arabic titles) like the Windows companion's TextImage makes them. Renders at
the em size with the baseline on a fixed row, crops to the ink width (with an
ellipsis past the maximum width), packs to 4-bit alpha, high nibble first.

    python3 make_textimg.py fixtures

Needs Pillow with libraqm, Noto Sans CJK and DejaVu Sans (Debian/Ubuntu:
fonts-noto-cjk, fonts-dejavu-core).
"""
# SPDX-License-Identifier: MIT
import sys, struct
from PIL import Image, ImageDraw, ImageFont
CJK = "/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc"
AR = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
def render(text, em, height, baseline, maxw, field, out, font_path=CJK, index=0, direction=None):
    font = ImageFont.truetype(font_path, em, index=index, layout_engine=ImageFont.Layout.RAQM)
    def w_of(t): return int(font.getlength(t, direction=direction))
    if w_of(text) > maxw:
        while text and w_of(text + "…") > maxw: text = text[:-1]
        text += "…"
    W = w_of(text) + 4
    img = Image.new("L", (W, height), 0)
    d = ImageDraw.Draw(img)
    d.text((2, baseline), text, fill=255, font=font, anchor="ls", direction=direction)
    bbox = img.getbbox() or (0, 0, 1, height)
    img = img.crop((bbox[0], 0, bbox[2], height))
    w, h = img.size
    w = min(w, maxw)
    stride = (w + 1) // 2
    a4 = bytearray(stride * h)
    px = img.load()
    for y in range(h):
        for x in range(w):
            v = (px[x, y] + 8) // 17
            a4[y * stride + x // 2] |= (v << 4) if x % 2 == 0 else v
    open(out, "wb").write(bytes([field]) + struct.pack("<HH", w, h) + bytes(a4))
    print(out, w, "x", h)
o = sys.argv[1]
render("夜に駆ける — 群青 (Racing Into The Night)", 22, 30, 23, 1024, 0, o + "/ja_title.bin")
render("봄날", 22, 30, 23, 1024, 0, o + "/ko_title.bin", index=1)
render("방탄소년단", 16, 24, 18, 240, 1, o + "/ko_artist.bin", index=1)
render("حبيبي يا نور العين", 22, 30, 23, 1024, 0, o + "/ar_title.bin", AR, direction="rtl")
render("عمرو دياب", 16, 24, 18, 240, 1, o + "/ar_artist.bin", AR, direction="rtl")
