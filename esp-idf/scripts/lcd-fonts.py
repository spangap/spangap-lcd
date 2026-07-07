#!/usr/bin/env python3
"""
lcd-fonts.py — subset the shipped vector fonts into data_merged/fonts/*.ttf.

Invoked by the spangap_lcd_fonts() CMake helper. Each source face in --src is
subset with fonttools' pyftsubset to the codepoint ranges the on-device UI /
terminal need, written RAW (uncompressed, no flavor) so it mmaps in place for
FreeType / tiny_ttf:

    <src>/<face>.ttf   ->   <out>/<face>.ttf   ->   /fixed/fonts/<face>.ttf

The four faces:
  ui.ttf, ui-semibold.ttf  proportional UI — Latin + punctuation + euro
  mono.ttf                 monospace terminal — Latin + box-drawing + blocks
  symbols.ttf              FontAwesome-5 subset, exactly the LV_SYMBOL_*
                           codepoints; already trimmed at the source, so it is
                           copied verbatim (no re-subsetting). lcd_fonts.cpp
                           chains it as every UI/MONO font's fallback.

Best-effort by design (mirrors lcd-icons.py): if fontTools isn't importable the
script copies the source TTF through verbatim so the firmware still builds — the
face just isn't shrunk. A raw copy is functionally identical, only larger in
/fixed.
"""
import argparse
import os
import shutil
import sys

# fontTools may live outside the read-only IDF venv (installed with
# `pip install --target=...`); make a couple of well-known spots importable
# before giving up and falling back to a raw copy.
for _cand in (os.environ.get("SPANGAP_PYLIBS"),
              os.path.expanduser("~/.spangap-pylibs")):
    if _cand and os.path.isdir(_cand) and _cand not in sys.path:
        sys.path.insert(0, _cand)


def warn(msg):
    print(f"lcd-fonts: {msg}", file=sys.stderr)


# Per-face subset ranges. Keys are the output basenames.
UI_UNICODES = (
    "U+0020-00FF,"   # Basic Latin + Latin-1 Supplement (Western European)
    "U+0100-017F,"   # Latin Extended-A (Central European)
    "U+2013-2014,"   # en/em dash
    "U+2018-2019,"   # curly single quotes
    "U+201C-201D,"   # curly double quotes
    "U+2022,"        # bullet
    "U+2026,"        # ellipsis
    "U+20AC"         # euro
)
MONO_UNICODES = (
    "U+0020-00FF,"   # Basic Latin + Latin-1 Supplement
    "U+2190-2193,"   # arrows (TUI)
    "U+2500-257F,"   # Box Drawing (complete)
    "U+2580-259F,"   # Block Elements (halves/quadrants/shades)
    "U+25A0-25CF"    # Geometric Shapes common in TUIs
)
# symbols.ttf is a FontAwesome-5 subset already trimmed to exactly the
# LV_SYMBOL_* codepoints (see the vendored source) — copy it verbatim (None
# range), no re-subsetting needed.
FACE_RANGES = {
    "ui": UI_UNICODES,
    "ui-semibold": UI_UNICODES,
    "mono": MONO_UNICODES,
    "symbols": None,
}


def subset_one(src, dst, unicodes):
    """Subset src->dst with pyftsubset. Returns True on success."""
    try:
        from fontTools import subset  # noqa: F401
    except ImportError:
        return False
    args = [
        src,
        f"--output-file={dst}",
        f"--unicodes={unicodes}",
        "--layout-features=",     # drop GSUB/GPOS: no shaping on device
        "--hinting",              # keep TrueType hints (legibility at 12px)
        "--desubroutinize",
        "--drop-tables+=DSIG",
        "--recalc-bounds",
        # No --flavor: emit RAW TrueType (uncompressed) so it mmaps in place.
    ]
    # pyftsubset reads argv; call its main so we don't shell out.
    try:
        from fontTools.subset import main as ft_subset_main
        ft_subset_main(args)
    except SystemExit as e:                       # pyftsubset calls sys.exit
        if e.code not in (0, None):
            warn(f"pyftsubset exit {e.code} for {src}")
            return False
    except Exception as e:                         # noqa: BLE001
        warn(f"subset failed for {src}: {e}")
        return False
    return os.path.isfile(dst)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="source font dir (*.ttf)")
    ap.add_argument("--out", required=True, help="output dir (.../fonts)")
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    made = 0
    for face, unicodes in FACE_RANGES.items():
        src = os.path.join(a.src, face + ".ttf")
        dst = os.path.join(a.out, face + ".ttf")
        if not os.path.isfile(src):
            warn(f"missing source {src} — skipping {face}")
            continue
        if unicodes is None:
            shutil.copyfile(src, dst)   # already-subset source (symbols)
            made += 1
        elif subset_one(src, dst, unicodes):
            made += 1
        else:
            # Fall back to a raw copy so the build still succeeds.
            warn(f"fontTools unavailable — copying {face}.ttf verbatim (not subset)")
            shutil.copyfile(src, dst)
            made += 1
    print(f"lcd-fonts: wrote {made} font(s) -> /fixed/fonts")
    return 0


if __name__ == "__main__":
    sys.exit(main())
