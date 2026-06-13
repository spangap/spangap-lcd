#!/usr/bin/env python3
"""Generate the LVGL v9 2-bpp C font `lv_font_spleen_5x8` (fixed 5x8 cell).

The platform's terminal font (Log / CLI / Nomad page view at the larger
size). Coverage is merged from three sources, in priority order:

  1. Spleen 5x8 (scripts/spleen-5x8.bdf) — the text glyphs. Spleen
     (https://github.com/fcambus/spleen) is a monospaced bitmap font,
     BSD-2-Clause.
  2. misc-fixed 5x8 (scripts/fixed-5x8.bdf) — the X11 5x8 font from Markus
     Kuhn's ucs-fonts ("Public domain font. Share and enjoy."). Fills every
     codepoint Spleen lacks; crucially the COMPLETE Box Drawing block
     U+2500-257F (heavy lines + half-strokes + doubles included) — the glyph
     set NomadNet/micron pages draw their "graphics" with. Its light-line
     geometry matches Spleen's exactly (horizontal at row 3, vertical at
     col 2), so the merged block is seamless.
     https://www.cl.cam.ac.uk/~mgk25/ucs-fonts.html
  3. Synthesized: ALL Block Elements U+2580-259F, overriding the donor.
     This is the reason the font is 2 bpp: the cell is 5 px wide, so a
     half-cell block (halves U+258C/2590, the quadrants U+2596-259F) covers
     2.5 px — two full columns plus the CENTER column at half coverage,
     rendered as a grey pixel (4 grey levels at 2 bpp). Heights divide
     exactly (8 rows -> eighth-blocks are 1 row each), so only horizontal
     seams need grey. Each glyph is built from per-pixel rectangle coverage
     (level = round(3 * coverage)); the shades U+2591/2592/2593 map to grey
     directly (flat 1/3, flat 2/3, checkered 2/3+3/3).

Each glyph is stored as a full 5(w) x 8(h) cell (true monospace), 2 bpp,
pixel values packed continuously row-major MSB-first. Metrics: adv_w = 5 px,
line_height = 8, base_line = 1 (both BDFs' FONT_DESCENT). The cmap set is
built generically: the sorted merged codepoints are partitioned into
contiguous runs (FORMAT0_TINY) and the sparse leftovers between them
(SPARSE_TINY), preserving codepoint order so glyph ids stay consecutive per
cmap. Absent codepoints resolve to "no glyph" and get filtered out by
callers (the LCD renderers' printable()).

Usage:  scripts/gen-spleen-font.py [spleen-5x8.bdf] [fixed-5x8.bdf] [out.c]
Defaults: scripts/{spleen-5x8,fixed-5x8}.bdf -> src/lcd_ui/lv_font_spleen_5x8.c

This is a release-time regeneration step, NOT part of the build — the
generated .c is checked in (like every LVGL font). Re-run only when bumping
either source font.
"""
import sys, os

CELL_W, CELL_H = 5, 8
BPP = 2
MAXV = (1 << BPP) - 1   # full-coverage pixel value
MIN_RUN = 16            # contiguous runs at least this long get a FORMAT0 cmap


def parse_bdf(path):
    """Return {codepoint: CELL_H x CELL_W list-of-lists of 0/MAXV}."""
    with open(path, "r") as f:
        lines = f.read().splitlines()

    fbb = None
    for ln in lines:
        if ln.startswith("FONTBOUNDINGBOX"):
            _, w, h, x, y = ln.split()
            fbb = (int(w), int(h), int(x), int(y))
            break
    if fbb is None:
        raise SystemExit("%s: no FONTBOUNDINGBOX" % path)
    _, _, fbbx, fbby = fbb

    glyphs = {}
    i = 0
    n = len(lines)
    while i < n:
        if lines[i].startswith("STARTCHAR"):
            enc, bbx, bitmap = None, None, []
            i += 1
            while i < n and not lines[i].startswith("ENDCHAR"):
                ln = lines[i]
                if ln.startswith("ENCODING"):
                    enc = int(ln.split()[1])
                elif ln.startswith("BBX"):
                    _, bw, bh, bx, by = ln.split()
                    bbx = (int(bw), int(bh), int(bx), int(by))
                elif ln.startswith("BITMAP"):
                    i += 1
                    while i < n and not lines[i].startswith("ENDCHAR"):
                        bitmap.append(lines[i].strip())
                        i += 1
                    break
                i += 1
            while i < n and not lines[i].startswith("ENDCHAR"):
                i += 1
            i += 1
            if enc is None or enc < 0x20 or bbx is None:
                continue
            if enc > 0xFFFF:        # cmaps below are 16-bit; nothing real is lost
                continue

            bw, bh, bx, by = bbx
            cell = [[0] * CELL_W for _ in range(CELL_H)]
            top_row = (fbby + CELL_H) - (by + bh)   # row of glyph's top pixel
            left_col = bx - fbbx
            for j in range(bh):
                if j >= len(bitmap):
                    break
                rowbytes = bytes.fromhex(bitmap[j]) if bitmap[j] else b""
                for c in range(bw):
                    byte_i = c // 8
                    if byte_i >= len(rowbytes):
                        continue
                    bit = (rowbytes[byte_i] >> (7 - (c % 8))) & 1
                    if not bit:
                        continue
                    rr, cc = top_row + j, left_col + c
                    if 0 <= rr < CELL_H and 0 <= cc < CELL_W:
                        cell[rr][cc] = MAXV
            glyphs[enc] = cell
        else:
            i += 1
    return glyphs


def block_glyphs():
    """All Block Elements U+2580-259F from per-pixel rectangle coverage.

    Rect coords are floats in pixel units (x: 0..CELL_W, y: 0..CELL_H);
    a pixel's level is round(MAXV * covered_area), maxed over rects — a
    half-covered seam column comes out grey.
    """
    W, H = float(CELL_W), float(CELL_H)

    def fill(cell, x0, y0, x1, y1):
        for r in range(CELL_H):
            ycov = max(0.0, min(r + 1.0, y1) - max(float(r), y0))
            if ycov <= 0.0:
                continue
            for c in range(CELL_W):
                xcov = max(0.0, min(c + 1.0, x1) - max(float(c), x0))
                if xcov <= 0.0:
                    continue
                cell[r][c] = max(cell[r][c], int(MAXV * xcov * ycov + 0.5))

    def make(*rects):
        cell = [[0] * CELL_W for _ in range(CELL_H)]
        for rc in rects:
            fill(cell, *rc)
        return cell

    out = {}
    # Halves and eighths (vertical fractions are exact at 8 rows; horizontal
    # fractions land grey on the seam column).
    out[0x2580] = make((0, 0, W, H / 2))                 # ▀ upper half
    for k in range(1, 8):                                # ▁..▇ lower eighths
        out[0x2580 + k] = make((0, H - H * k / 8, W, H))
    out[0x2588] = make((0, 0, W, H))                     # █ full
    for k in range(7, 0, -1):                            # ▉..▏ left eighths
        out[0x2588 + (8 - k)] = make((0, 0, W * k / 8, H))
    out[0x2590] = make((W / 2, 0, W, H))                 # ▐ right half
    out[0x2594] = make((0, 0, W, H / 8))                 # ▔ upper eighth
    out[0x2595] = make((W - W / 8, 0, W, H))             # ▕ right eighth

    # Shades: 2 bpp greys instead of 1-bpp dithers. ▓ checkers full/grey so
    # it stays distinct from █.
    light = [[1] * CELL_W for _ in range(CELL_H)]                          # ░
    medium = [[2] * CELL_W for _ in range(CELL_H)]                         # ▒
    dark = [[3 if (r + c) % 2 == 0 else 2 for c in range(CELL_W)]          # ▓
            for r in range(CELL_H)]
    out[0x2591], out[0x2592], out[0x2593] = light, medium, dark

    # Quadrants: (upper-left, upper-right, lower-left, lower-right).
    UL, UR = (0, 0, W / 2, H / 2), (W / 2, 0, W, H / 2)
    LL, LR = (0, H / 2, W / 2, H), (W / 2, H / 2, W, H)
    for cp, quads in {
        0x2596: (LL,), 0x2597: (LR,), 0x2598: (UL,), 0x2599: (UL, LL, LR),
        0x259A: (UL, LR), 0x259B: (UL, UR, LL), 0x259C: (UL, UR, LR),
        0x259D: (UR,), 0x259E: (UR, LL), 0x259F: (UR, LL, LR),
    }.items():
        out[cp] = make(*quads)
    return out


def pack_cell(cell):
    """Pack a CELL_H x CELL_W cell of 0..MAXV into continuous MSB-first
    BPP-bits-per-pixel bytes."""
    bits = []
    for r in range(CELL_H):
        for c in range(CELL_W):
            for k in range(BPP - 1, -1, -1):
                bits.append((cell[r][c] >> k) & 1)
    out = bytearray()
    for k in range(0, len(bits), 8):
        b = 0
        for j in range(8):
            if k + j < len(bits) and bits[k + j]:
                b |= 1 << (7 - j)
        out.append(b)
    return bytes(out)


def build_segments(cps):
    """Partition sorted codepoints into cmap segments, preserving order.

    Returns [(kind, [cps...])] with kind 'range' (FORMAT0_TINY, consecutive)
    or 'sparse' (SPARSE_TINY). Order preservation keeps glyph ids consecutive
    within each segment when glyphs are emitted in codepoint order.
    """
    runs = []
    start = prev = cps[0]
    for cp in cps[1:]:
        if cp == prev + 1:
            prev = cp
            continue
        runs.append(list(range(start, prev + 1)))
        start = prev = cp
    runs.append(list(range(start, prev + 1)))

    segments = []
    sparse = []
    for run in runs:
        if len(run) >= MIN_RUN:
            if sparse:
                segments.append(("sparse", sparse))
                sparse = []
            segments.append(("range", run))
        else:
            sparse.extend(run)
    if sparse:
        segments.append(("sparse", sparse))
    return segments


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    spleen = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "spleen-5x8.bdf")
    fixed = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "fixed-5x8.bdf")
    out = sys.argv[3] if len(sys.argv) > 3 else os.path.join(
        here, "..", "src", "lcd_ui", "lv_font_spleen_5x8.c")

    glyphs = parse_bdf(fixed)        # broad coverage first ...
    for cp, cell in parse_bdf(spleen).items():
        glyphs[cp] = cell            # ... Spleen overrides where present
    for cp, cell in block_glyphs().items():
        glyphs[cp] = cell            # synthesized blocks override both

    cps = sorted(glyphs)
    segments = build_segments(cps)

    # Glyph ids run 1.. in codepoint order (id 0 stays reserved).
    entries = []   # (codepoint, packed-bytes, bitmap_index)
    idx = 0
    for cp in cps:
        data = pack_cell(glyphs[cp])
        entries.append((cp, data, idx)); idx += len(data)

    L = []
    L.append("/" + "*" * 78)
    L.append(" * Spleen 5x8 + misc-fixed 5x8 -> LVGL v9 2-bpp font, fixed 5x8 cell.")
    L.append(" * %d glyphs: Spleen text, misc-fixed everything else (incl. the complete" % len(cps))
    L.append(" * Box Drawing block U+2500-257F), synthesized Block Elements U+2580-259F.")
    L.append(" * 2 bpp so half-cell blocks land grey on the odd 5 px width's center")
    L.append(" * column (a 5 px cell halves at 2.5 px); vertical fractions are exact.")
    L.append(" * Generated by scripts/gen-spleen-font.py; do not edit.")
    L.append(" *")
    L.append(" * Spleen (c) 2018-2026 Frederic Cambus, BSD-2-Clause.")
    L.append(" *   https://github.com/fcambus/spleen")
    L.append(" * misc-fixed 5x8: \"Public domain font. Share and enjoy.\"")
    L.append(" *   https://www.cl.cam.ac.uk/~mgk25/ucs-fonts.html")
    L.append(" " + "*" * 78 + "/")
    L.append("")
    L.append("#ifdef LV_LVGL_H_INCLUDE_SIMPLE")
    L.append('    #include "lvgl.h"')
    L.append("#else")
    L.append('    #include "lvgl.h"')
    L.append("#endif")
    L.append("")
    L.append("#ifndef LV_FONT_SPLEEN_5X8")
    L.append("    #define LV_FONT_SPLEEN_5X8 1")
    L.append("#endif")
    L.append("")
    L.append("#if LV_FONT_SPLEEN_5X8")
    L.append("")
    L.append("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {")
    for cp, data, _ in entries:
        ch = chr(cp) if 0x20 <= cp <= 0x7E and cp != 0x5C else ""
        L.append("    /* U+%04X%s */ %s," % (cp, " '%s'" % ch if ch else "",
                                             ", ".join("0x%02x" % b for b in data)))
    L.append("};")
    L.append("")
    L.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    L.append("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,")
    for cp, _, bi in entries:
        L.append("    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = 0, .ofs_y = -1},"
                 % (bi, CELL_W * 16, CELL_W, CELL_H))
    L.append("};")
    L.append("")
    sparse_i = 0
    for kind, seg in segments:
        if kind != "sparse":
            continue
        # SPARSE_TINY unicode_list: offsets from the segment's first cp,
        # ascending (bsearch'd by LVGL).
        L.append("static const uint16_t unicode_list_%d[] = {" % sparse_i)
        for k in range(0, len(seg), 12):
            L.append("    " + ", ".join("0x%04x" % (cp - seg[0]) for cp in seg[k:k+12]) + ",")
        L.append("};")
        L.append("")
        sparse_i += 1
    L.append("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
    gid = 1
    sparse_i = 0
    for si, (kind, seg) in enumerate(segments):
        comma = "," if si + 1 < len(segments) else ""
        if kind == "range":
            L.append("    {")
            L.append("        .range_start = %d, .range_length = %d, .glyph_id_start = %d,"
                     % (seg[0], len(seg), gid))
            L.append("        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY")
            L.append("    }%s" % comma)
        else:
            # range_length is the fast-reject bound: a codepoint resolves only
            # if (cp - range_start) < range_length, so it must cover the last
            # offset in the list.
            span = (seg[-1] - seg[0]) + 1
            L.append("    {")
            L.append("        .range_start = %d, .range_length = %d, .glyph_id_start = %d,"
                     % (seg[0], span, gid))
            L.append("        .unicode_list = unicode_list_%d, .glyph_id_ofs_list = NULL, .list_length = %d, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY"
                     % (sparse_i, len(seg)))
            L.append("    }%s" % comma)
            sparse_i += 1
        gid += len(seg)
    L.append("};")
    L.append("")
    L.append("#if LVGL_VERSION_MAJOR >= 8")
    L.append("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    L.append("#else")
    L.append("static lv_font_fmt_txt_dsc_t font_dsc = {")
    L.append("#endif")
    L.append("    .glyph_bitmap = glyph_bitmap,")
    L.append("    .glyph_dsc = glyph_dsc,")
    L.append("    .cmaps = cmaps,")
    L.append("    .kern_dsc = NULL,")
    L.append("    .kern_scale = 0,")
    L.append("    .cmap_num = %d," % len(segments))
    L.append("    .bpp = %d," % BPP)
    L.append("    .kern_classes = 0,")
    L.append("    .bitmap_format = 0,")
    L.append("};")
    L.append("")
    L.append("#if LVGL_VERSION_MAJOR >= 8")
    L.append("const lv_font_t lv_font_spleen_5x8 = {")
    L.append("#else")
    L.append("lv_font_t lv_font_spleen_5x8 = {")
    L.append("#endif")
    L.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    L.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    L.append("    .line_height = %d," % CELL_H)
    L.append("    .base_line = 1,")
    L.append("#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)")
    L.append("    .subpx = LV_FONT_SUBPX_NONE,")
    L.append("#endif")
    L.append("#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8")
    L.append("    .underline_position = 0,")
    L.append("    .underline_thickness = 0,")
    L.append("#endif")
    L.append("    .dsc = &font_dsc")
    L.append("};")
    L.append("")
    L.append("#endif /* LV_FONT_SPLEEN_5X8 */")
    L.append("")

    out = os.path.normpath(out)
    with open(out, "w") as f:
        f.write("\n".join(L))
    n_range = sum(1 for k, _ in segments if k == "range")
    print("wrote %s (%d glyphs, %d bitmap bytes, %d bpp, %d cmaps: %d range + %d sparse)"
          % (out, len(cps), idx, BPP, len(segments), n_range, len(segments) - n_range))


if __name__ == "__main__":
    main()
