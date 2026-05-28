#!/usr/bin/env python3
"""
lcd-icons.py — rasterize launcher icon sources into LVGL RGB565A8 .bin files.

Invoked by the spangap_lcd_icons() CMake helper. For each *.svg / *.png source
in the --src dirs and each WxH bucket in --sizes, render to that size and convert
to the LVGL binary image format (RGB565A8: 16-bit colour + 8-bit alpha) via the
LVGLImage.py shipped with the lvgl component. Output:

    <out>/<WxH>/<name>.bin   ->   /fixed/lcd/icons/<WxH>/<name>.bin

--src may be given more than once: dirs are merged by basename in the order
listed, so a later (consumer) dir overrides an earlier (platform-default) one
with the same icon name — the build-time analogue of the data/ merge.

Best-effort by design: if an optional dependency is missing (Pillow for raster,
cairosvg for SVG) or LVGLImage.py isn't found, it warns and skips so the
firmware still builds — the launcher just shows label-only tiles until icons
are present. Pre-rendered .bin files dropped directly into
<consumer>/data/lcd/icons/<WxH>/ ship via the normal data merge with no tooling.
"""
import argparse
import os
import subprocess
import sys
import tempfile


def warn(msg):
    print(f"lcd-icons: {msg}", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", action="append", default=[],
                    help="icon source dir (*.svg/*.png); repeatable, later wins")
    ap.add_argument("--out", required=True, help="output root (.../lcd/icons)")
    ap.add_argument("--sizes", required=True, help="comma list, e.g. 40x40,64x64")
    ap.add_argument("--lvgl-image-py", required=True, help="path to LVGLImage.py")
    a = ap.parse_args()

    if not os.path.isfile(a.lvgl_image_py):
        warn(f"LVGLImage.py not found ({a.lvgl_image_py}) — skipping")
        return 0

    # Merge source dirs by basename, later dirs overriding earlier ones.
    merged = {}  # filename -> absolute path
    for d in a.src:
        if not os.path.isdir(d):
            warn(f"no source dir {d} — skipping it")
            continue
        for f in sorted(os.listdir(d)):
            if f.lower().endswith((".svg", ".png")):
                merged[f] = os.path.join(d, f)
    sources = sorted(merged)
    if not sources:
        warn("no .svg/.png in any --src dir — skipping")
        return 0

    try:
        from PIL import Image
    except ImportError:
        warn("Pillow not installed (pip install pillow) — skipping icon conversion")
        return 0

    buckets = []
    for tok in a.sizes.split(","):
        tok = tok.strip().lower()
        if not tok:
            continue
        try:
            w, h = (int(x) for x in tok.split("x"))
        except ValueError:
            warn(f"bad size '{tok}' — expected WxH")
            continue
        buckets.append((tok, w, h))

    made = 0
    for label, w, h in buckets:
        outdir = os.path.join(a.out, label)
        os.makedirs(outdir, exist_ok=True)
        for fn in sources:
            stem = os.path.splitext(fn)[0]
            src = merged[fn]
            with tempfile.TemporaryDirectory() as td:
                png = os.path.join(td, stem + ".png")
                if fn.lower().endswith(".svg"):
                    try:
                        import cairosvg
                        cairosvg.svg2png(url=src, write_to=png,
                                         output_width=w, output_height=h)
                    except ImportError:
                        warn("cairosvg not installed (pip install cairosvg) — "
                             f"cannot rasterize {fn}")
                        continue
                    except Exception as e:  # noqa: BLE001
                        warn(f"svg render failed for {fn}: {e}")
                        continue
                else:
                    try:
                        im = Image.open(src).convert("RGBA").resize(
                            (w, h), Image.LANCZOS)
                        im.save(png)
                    except Exception as e:  # noqa: BLE001
                        warn(f"png resize failed for {fn}: {e}")
                        continue

                cmd = [sys.executable, a.lvgl_image_py,
                       "--ofmt", "BIN", "--cf", "RGB565A8",
                       "--output", outdir, png]
                try:
                    subprocess.run(cmd, check=True,
                                   stdout=subprocess.DEVNULL)
                except subprocess.CalledProcessError as e:
                    warn(f"LVGLImage.py failed for {fn} @ {label}: {e}")
                    continue
                made += 1

    print(f"lcd-icons: wrote {made} icon(s) across {len(buckets)} size(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
