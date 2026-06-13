#!/usr/bin/env python3
"""Generate spangap's compact accented UI font: lv_font_montserrat_12_latin.

LVGL's stock Montserrat fonts cover only ASCII + a sparse symbol set — no
accented Latin — so user text (names, messages) loses umlauts/accents to
placeholder boxes. This builds a drop-in *superset* of lv_font_montserrat_12:

    ASCII (0x20-0x7F) + degree + bullet
  + Latin-1 Supplement + Latin Extended-A (U+00A0-U+017F: Western & Central
    European accents)
  + the full LVGL/FontAwesome symbol set (so LV_SYMBOL_* still render)

The output (src/lcd_ui/lv_font_montserrat_12_latin.c) is checked in; the build
never runs this — regenerate only to change the size or codepoint ranges.

Requires lv_font_conv (https://github.com/lvgl/lv_font_conv): found on PATH, or
run through Node's `npx`. The Montserrat + FontAwesome source fonts ship inside
the lvgl component, so point --lvgl-fonts at
  <consumer>/managed_components/lvgl__lvgl/scripts/built_in_font
or just run this from inside a consumer checkout and it will find that dir by
walking up from the cwd.

Usage:  scripts/gen-text-font.py [--lvgl-fonts DIR] [-o OUT.c]
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.normpath(
    os.path.join(HERE, "..", "src", "lcd_ui", "lv_font_montserrat_12_latin.c"))

SIZE = 12
BPP = 4
# ASCII + degree (0xB0) + bullet (0x2022) + Latin-1 Supplement + Latin Extended-A.
TEXT_RANGE = "0x20-0x7F,0xB0,0x2022,0xA0-0x17F"
# The LVGL built-in symbol set (FontAwesome glyph ids), verbatim from lvgl's
# scripts/built_in_font/built_in_font_gen.py — keeps us byte-compatible with the
# stock fonts' LV_SYMBOL_* coverage — plus our additions at the end:
#   61445 (0xF005) star — the Nomad LCD browser's bookmark toggle.
SYMS = ("61441,61448,61451,61452,61452,61453,61457,61459,61461,61465,61468,"
        "61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,"
        "61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,"
        "61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,"
        "61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,"
        "62099,62212,62189,62810,63426,63650,61445")

# Canonical prologue (lv_font_conv emits an absolute-path comment + a
# dual-branch include; rewrite to match the rest of src/lcd_ui).
PROLOGUE = """\
/*******************************************************************************
 * Montserrat 12 px, 4 bpp — ASCII + Latin-1 Supplement + Latin Extended-A
 * (U+00A0–U+017F: Western & Central European accents) + the LVGL symbol set.
 *
 * A drop-in accented superset of LVGL's lv_font_montserrat_12. GENERATED — do
 * not hand-edit; regenerate with scripts/gen-text-font.py (it wraps lv_font_conv
 * over Montserrat-Medium.ttf + FontAwesome, both shipped with the lvgl
 * component). Montserrat: OFL-1.1; Font Awesome Free: OFL-1.1 / CC-BY-4.0.
 ******************************************************************************/

#include "lvgl.h"
"""


def find_lvgl_fonts():
    d = os.getcwd()
    for _ in range(8):
        cand = os.path.join(d, "managed_components", "lvgl__lvgl",
                            "scripts", "built_in_font")
        if os.path.isdir(cand):
            return cand
        d = os.path.dirname(d)
    return None


def conv_cmd():
    if shutil.which("lv_font_conv"):
        return ["lv_font_conv"]
    if shutil.which("npx"):
        return ["npx", "-y", "lv_font_conv"]
    sys.exit("lv_font_conv not found — install it (npm i -g lv_font_conv) or "
             "make Node's npx available.")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lvgl-fonts", metavar="DIR",
                    help="lvgl built_in_font dir (auto-detected if omitted)")
    ap.add_argument("-o", "--output", default=DEFAULT_OUT, metavar="OUT.c")
    args = ap.parse_args()

    fonts = args.lvgl_fonts or find_lvgl_fonts()
    if not fonts:
        sys.exit("couldn't locate the lvgl built_in_font dir; pass --lvgl-fonts "
                 "<.../managed_components/lvgl__lvgl/scripts/built_in_font>")
    ttf = os.path.join(fonts, "Montserrat-Medium.ttf")
    awe = os.path.join(fonts, "FontAwesome5-Solid+Brands+Regular.woff")
    for f in (ttf, awe):
        if not os.path.isfile(f):
            sys.exit("missing source font: " + f)

    cmd = conv_cmd() + [
        "--no-compress", "--no-prefilter", "--bpp", str(BPP), "--size", str(SIZE),
        "--font", ttf, "-r", TEXT_RANGE,
        "--font", awe, "-r", SYMS,
        "--format", "lvgl", "-o", args.output, "--force-fast-kern-format",
    ]
    subprocess.run(cmd, check=True)

    # Replace lv_font_conv's prologue (machine-path comment + lvgl/lvgl.h branch)
    # with our canonical one, so a regen reproduces the committed file verbatim.
    with open(args.output, "r", encoding="utf-8") as fh:
        txt = fh.read()
    guard = txt.index("#ifndef LV_FONT")
    with open(args.output, "w", encoding="utf-8") as fh:
        fh.write(PROLOGUE + "\n" + txt[guard:])
    print("wrote", args.output)


if __name__ == "__main__":
    main()
