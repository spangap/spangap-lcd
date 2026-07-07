#!/usr/bin/env python3
"""
lcd-icons.py — stage launcher icon SVG *sources* into the factory image.

Invoked by the spangap_lcd_icons() CMake helper. The device rasterizes icons at
runtime with nanosvg (lcd_icons.cpp), so the build no longer rasterizes anything
— it just merges the .svg sources by basename and copies them:

    <out>/<name>.svg   ->   /fixed/icons/<name>.svg

--src may be given more than once: dirs are merged by basename in the order
listed, so a later (consumer) dir overrides an earlier (platform-default) one
with the same icon name — the build-time analogue of the data/ merge. Only .svg
is shipped (nanosvg has no raster-image path; a PNG source would need
re-authoring as SVG).
"""
import argparse
import os
import shutil
import sys


def warn(msg):
    print(f"lcd-icons: {msg}", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", action="append", default=[],
                    help="icon source dir (*.svg); repeatable, later wins")
    ap.add_argument("--out", required=True, help="output root (.../icons)")
    a = ap.parse_args()

    # Merge source dirs by basename, later dirs overriding earlier ones.
    merged = {}  # filename -> absolute path
    for d in a.src:
        if not os.path.isdir(d):
            warn(f"no source dir {d} — skipping it")
            continue
        for f in sorted(os.listdir(d)):
            if f.lower().endswith(".svg"):
                merged[f] = os.path.join(d, f)
            elif f.lower().endswith(".png"):
                warn(f"{f}: PNG icons are no longer rasterized at build time "
                     "(nanosvg is SVG-only) — provide an .svg source; skipping")

    if not merged:
        warn("no .svg in any --src dir — skipping")
        return 0

    os.makedirs(a.out, exist_ok=True)
    made = 0
    for fn in sorted(merged):
        shutil.copyfile(merged[fn], os.path.join(a.out, fn))
        made += 1

    print(f"lcd-icons: staged {made} icon SVG(s) -> /fixed/icons")
    return 0


if __name__ == "__main__":
    sys.exit(main())
