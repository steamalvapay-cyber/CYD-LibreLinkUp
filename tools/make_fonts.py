#!/usr/bin/env python3
"""
Genera src/fonts.h con fuentes suavizadas (anti-aliasing) en formato VLW
para TFT_eSPI, a partir de ficheros TTF.

Uso:
    python3 tools/make_fonts.py [--fonts-dir DIR]

Fuente usada: Poppins (SIL Open Font License 1.1, https://github.com/itfoundry/poppins).
Si quieres otro estilo, cambia los TTF y los tamaños en FONT_SPECS y vuelve a ejecutar.

Formato VLW que lee TFT_eSPI::loadFont(const uint8_t array[]):
    cabecera : 6 x int32 big-endian  (nº glifos, versión=11, tamaño, 0, ascent, descent)
    glifos   : nº glifos x 7 x int32 (unicode, alto, ancho, avance_x, dY, dX, 0)
    bitmaps  : alto*ancho bytes (alfa 8 bits) por glifo, en el mismo orden
"""
import argparse
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

DIGITS = "0123456789"
# ASCII imprimible (sin espacio: TFT_eSPI lo gestiona solo) + acentos del español
TEXT = "".join(chr(c) for c in range(33, 127)) + "áéíóúüñÁÉÍÓÚÜÑ¿¡·…°"

# nombre del array, fichero ttf, tamaño en px, caracteres
FONT_SPECS = [
    ("FontBig",   "Poppins-Bold.ttf",    112, DIGITS),
    ("FontMid",   "Poppins-Medium.ttf",   30, TEXT),
    ("FontSmall", "Poppins-Regular.ttf",  15, TEXT),
]


def build_vlw(ttf_path, px, chars):
    font = ImageFont.truetype(ttf_path, px)
    glyphs = []
    for ch in chars:
        l, t, r, b = font.getbbox(ch, anchor="ls")  # relativo a baseline/origen
        w, h = r - l, b - t
        if w <= 0 or h <= 0:
            continue
        img = Image.new("L", (w, h), 0)
        ImageDraw.Draw(img).text((-l, -t), ch, font=font, fill=255, anchor="ls")
        adv = int(round(font.getlength(ch)))
        dY, dX = -t, l  # dY: baseline->borde superior ; dX: bearing izquierdo
        assert 0 < w < 256 and 0 < h < 256 and 0 < adv < 256, (ch, w, h, adv)
        assert -128 <= dX <= 127, (ch, dX)
        glyphs.append((ord(ch), h, w, adv, dY, dX, img.tobytes()))

    ascent = max(g[4] for g in glyphs)
    descent = max(g[1] - g[4] for g in glyphs)
    out = struct.pack(">6i", len(glyphs), 11, px, 0, ascent, descent)
    for uni, h, w, adv, dY, dX, _ in glyphs:
        out += struct.pack(">7i", uni, h, w, adv, dY, dX, 0)
    for g in glyphs:
        out += g[6]
    return out, len(glyphs), ascent, descent


def to_c_array(name, data):
    lines = [f"// {len(data)} bytes", f"const uint8_t {name}[] PROGMEM = {{"]
    for i in range(0, len(data), 24):
        chunk = ", ".join(f"0x{b:02X}" for b in data[i:i + 24])
        lines.append("  " + chunk + ",")
    lines.append("};\n")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fonts-dir", default="/usr/share/fonts/truetype/google-fonts")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "src", "fonts.h"))
    args = ap.parse_args()

    parts = [
        "// Archivo GENERADO por tools/make_fonts.py — no editar a mano.",
        "// Fuente: Poppins (SIL Open Font License 1.1).",
        "#pragma once",
        "#include <Arduino.h>\n",
    ]
    total = 0
    for name, ttf, px, chars in FONT_SPECS:
        path = os.path.join(args.fonts_dir, ttf)
        if not os.path.exists(path):
            sys.exit(f"No encuentro {path}")
        data, n, asc, desc = build_vlw(path, px, chars)
        total += len(data)
        print(f"{name:10s} {ttf:20s} {px:3d}px  glifos={n:3d} ascent={asc:3d} descent={desc:2d}  {len(data)} bytes")
        parts.append(to_c_array(name, data))
    with open(args.out, "w") as f:
        f.write("\n".join(parts))
    print(f"Total {total} bytes -> {os.path.abspath(args.out)}")


if __name__ == "__main__":
    main()
