#!/usr/bin/env python3
"""Generate an LVGL v9 1-bpp C font from Tom Thumb + misc-fixed 4x6 BDFs.

A fixed 4(w) x 6(h) cell font, `lv_font_tomthumb_4x6` — the smallest page
font the LCD offers (80 columns on a 320 px panel). The even cell width is
the point: Block Elements split cells into 2x2 quadrants, which only tile
seamlessly when the cell halves are equal (4 -> 2+2, 6 -> 3+3). Coverage is
merged from three sources, in priority order:

  1. Tom Thumb (scripts/tom-thumb.bdf) — Robey Pointer's 3x5-in-4x6 micro
     font, the most legible text glyphs at this size. ASCII + Latin-1 + a
     few extras. https://robey.lag.net/2010/01/23/tiny-monospace-font.html
     (MIT per the BDF COPYRIGHT; also offered CC0/CC-BY 3.0 upstream.)
  2. misc-fixed 4x6 (scripts/fixed-4x6.bdf) — the X11 "micro" font from
     Markus Kuhn's ucs-fonts ("Public domain font. Share and enjoy.").
     Fills every codepoint Tom Thumb lacks; crucially the COMPLETE Box
     Drawing block U+2500-257F (heavy lines + half-strokes included) and
     Block Elements U+2580-2595 — the glyph set NomadNet/micron pages draw
     their "graphics" with. https://www.cl.cam.ac.uk/~mgk25/ucs-fonts.html
  3. Synthesized: the ten quadrant glyphs U+2596-259F (added to Unicode
     after misc-fixed's coverage push, missing from both BDFs). At 4x6 a
     quadrant is an exact 2x3 px rectangle, so they are generated here.

Both BDFs share metrics (DWIDTH 4, ascent 5, descent 1), so glyphs merge
onto one baseline. Emission mirrors gen-spleen-font.py (full-cell true
monospace, 1 bpp, rows packed continuously MSB-first), except the cmap set
is built generically: the sorted merged codepoints are partitioned into
contiguous runs (FORMAT0_TINY) and the sparse leftovers between them
(SPARSE_TINY), preserving codepoint order so glyph ids stay consecutive
per cmap. Absent codepoints resolve to "no glyph" and get filtered out by
callers (the LCD renderers' printable()).

Usage:  scripts/gen-tomthumb-font.py [tom-thumb.bdf] [fixed-4x6.bdf] [out.c]
Defaults: scripts/{tom-thumb,fixed-4x6}.bdf -> src/lcd_ui/lv_font_tomthumb_4x6.c

This is a release-time regeneration step, NOT part of the build — the
generated .c is checked in (like every LVGL font). Re-run only when bumping
either source font.
"""
import sys, os

CELL_W, CELL_H = 4, 6
MIN_RUN = 16     # contiguous runs at least this long get their own FORMAT0 cmap


def parse_bdf(path):
    """Return {codepoint: CELL_H x CELL_W list-of-lists of 0/1}."""
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
                        cell[rr][cc] = 1
            glyphs[enc] = cell
        else:
            i += 1
    return glyphs


def quadrant_glyphs():
    """U+2596-259F: each quadrant is an exact 2(w) x 3(h) px block."""
    # (upper-left, upper-right, lower-left, lower-right) per codepoint
    QUADS = {
        0x2596: (0, 0, 1, 0),  # ▖
        0x2597: (0, 0, 0, 1),  # ▗
        0x2598: (1, 0, 0, 0),  # ▘
        0x2599: (1, 0, 1, 1),  # ▙
        0x259A: (1, 0, 0, 1),  # ▚
        0x259B: (1, 1, 1, 0),  # ▛
        0x259C: (1, 1, 0, 1),  # ▜
        0x259D: (0, 1, 0, 0),  # ▝
        0x259E: (0, 1, 1, 0),  # ▞
        0x259F: (0, 1, 1, 1),  # ▟
    }
    out = {}
    for cp, (ul, ur, ll, lr) in QUADS.items():
        cell = [[0] * CELL_W for _ in range(CELL_H)]
        for r in range(CELL_H):
            for c in range(CELL_W):
                top, left = r < CELL_H // 2, c < CELL_W // 2
                if (top and left and ul) or (top and not left and ur) \
                   or (not top and left and ll) or (not top and not left and lr):
                    cell[r][c] = 1
        out[cp] = cell
    return out


def pack_cell(cell):
    """Pack a CELL_H x CELL_W cell into continuous MSB-first 1-bpp bytes."""
    bits = []
    for r in range(CELL_H):
        for c in range(CELL_W):
            bits.append(cell[r][c])
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
    tom = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "tom-thumb.bdf")
    fix = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "fixed-4x6.bdf")
    out = sys.argv[3] if len(sys.argv) > 3 else os.path.join(
        here, "..", "src", "lcd_ui", "lv_font_tomthumb_4x6.c")

    glyphs = parse_bdf(fix)          # broad coverage first ...
    for cp, cell in parse_bdf(tom).items():
        glyphs[cp] = cell            # ... Tom Thumb overrides where present
    for cp, cell in quadrant_glyphs().items():
        glyphs.setdefault(cp, cell)

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
    L.append(" * Tom Thumb + misc-fixed 4x6 -> LVGL v9 1-bpp font, fixed 4x6 cell.")
    L.append(" * %d glyphs: Tom Thumb text (ASCII/Latin-1), misc-fixed everything else" % len(cps))
    L.append(" * (incl. complete Box Drawing U+2500-257F + Block Elements U+2580-2595),")
    L.append(" * synthesized quadrants U+2596-259F. The even cell width makes 2x2 block")
    L.append(" * glyphs tile seamlessly across cells (micron page graphics).")
    L.append(" * Generated by scripts/gen-tomthumb-font.py; do not edit.")
    L.append(" *")
    L.append(" * Tom Thumb (c) 1999 Brian J. Swetland / Vassilii Khachaturov /")
    L.append(" *   Robey Pointer, MIT. https://robey.lag.net/2010/01/23/tiny-monospace-font.html")
    L.append(" * misc-fixed 4x6: \"Public domain font. Share and enjoy.\"")
    L.append(" *   https://www.cl.cam.ac.uk/~mgk25/ucs-fonts.html")
    L.append(" " + "*" * 78 + "/")
    L.append("")
    L.append("#ifdef LV_LVGL_H_INCLUDE_SIMPLE")
    L.append('    #include "lvgl.h"')
    L.append("#else")
    L.append('    #include "lvgl.h"')
    L.append("#endif")
    L.append("")
    L.append("#ifndef LV_FONT_TOMTHUMB_4X6")
    L.append("    #define LV_FONT_TOMTHUMB_4X6 1")
    L.append("#endif")
    L.append("")
    L.append("#if LV_FONT_TOMTHUMB_4X6")
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
    L.append("    .bpp = 1,")
    L.append("    .kern_classes = 0,")
    L.append("    .bitmap_format = 0,")
    L.append("};")
    L.append("")
    L.append("#if LVGL_VERSION_MAJOR >= 8")
    L.append("const lv_font_t lv_font_tomthumb_4x6 = {")
    L.append("#else")
    L.append("lv_font_t lv_font_tomthumb_4x6 = {")
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
    L.append("#endif /* LV_FONT_TOMTHUMB_4X6 */")
    L.append("")

    out = os.path.normpath(out)
    with open(out, "w") as f:
        f.write("\n".join(L))
    n_range = sum(1 for k, _ in segments if k == "range")
    print("wrote %s (%d glyphs, %d bitmap bytes, %d cmaps: %d range + %d sparse)"
          % (out, len(cps), idx, len(segments), n_range, len(segments) - n_range))


if __name__ == "__main__":
    main()
