#!/usr/bin/env python3
"""Genera src/logo.h — il wordmark ARCADEVOX come maschera compatta.

Il resto della schermata di avvio (sole, griglia, orizzonte) e' disegnato a
primitive dal firmware: costa zero byte e si adatta ai colori. Le lettere no —
il font 6x8 di Adafruit_GFX non fa un logo — quindi il wordmark viene
rasterizzato qui una volta sola e finisce in flash come maschera.

Formato: 2 bit per pixel, cioe' 4 livelli di copertura (0 = trasparente,
3 = pieno). L'antialiasing a 4 livelli costa il doppio di una maschera 1 bit
ma a 32 px di altezza e' la differenza fra lettere seghettate e lettere pulite.

Uso:  python3 tools/make_logo.py
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont

FONT = os.path.expanduser("~/Library/Fonts/Handel Gothic.ttf")
TEXT = "ARCADEVOX"
WIDTH = 204   # sta dentro il cerchio del display alla quota del wordmark
HEIGHT = 32
OUT = os.path.join(os.path.dirname(__file__), "..", "src", "logo.h")


def render():
    """Rasterizza a risoluzione alta e rimpicciolisce: bordi puliti."""
    font = ImageFont.truetype(FONT, 240)
    im = Image.new("L", (3000, 600), 0)
    ImageDraw.Draw(im).text((60, 60), TEXT, font=font, fill=255)
    return im.crop(im.getbbox()).resize((WIDTH, HEIGHT), Image.LANCZOS)


def pack(im):
    """4 pixel per byte, dal piu' significativo al meno."""
    px = im.load()
    stride = (WIDTH * 2 + 7) // 8
    rows = []
    for y in range(HEIGHT):
        row = bytearray(stride)
        for x in range(WIDTH):
            level = min(3, px[x, y] * 4 // 256)
            row[x >> 2] |= level << (6 - 2 * (x & 3))
        rows.append(row)
    return stride, rows


def main():
    if not os.path.exists(FONT):
        sys.exit("font non trovato: " + FONT)

    stride, rows = pack(render())

    out = [
        "// logo.h — wordmark ARCADEVOX, generato da tools/make_logo.py.",
        "//",
        "// NON modificare a mano: rigenerare con `python3 tools/make_logo.py`.",
        "//",
        '// Font: Handel Gothic. Maschera a 2 bit per pixel (4 livelli di',
        "// copertura), impacchettata 4 pixel per byte dal bit piu' significativo.",
        "// Il colore lo decide chi disegna: qui c'e' solo la forma delle lettere.",
        "#pragma once",
        "",
        "#include <Arduino.h>",
        "",
        "#define LOGO_W %d" % WIDTH,
        "#define LOGO_H %d" % HEIGHT,
        "#define LOGO_STRIDE %d  // byte per riga" % stride,
        "",
        "const uint8_t LOGO_MASK[LOGO_H * LOGO_STRIDE] PROGMEM = {",
    ]
    for y, row in enumerate(rows):
        for i in range(0, stride, 16):
            chunk = ", ".join("0x%02X" % b for b in row[i:i + 16])
            out.append("    " + chunk + ",")
        out.append("    // riga %d" % y)
    out += ["};", ""]

    with open(OUT, "w") as f:
        f.write("\n".join(out))
    print("scritto %s (%d byte di maschera)" % (os.path.normpath(OUT), stride * HEIGHT))


if __name__ == "__main__":
    main()
