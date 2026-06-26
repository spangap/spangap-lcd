/**
 * stylesheet.cpp — sheet selection + calibration. Picks the registered sheet
 * matching the real panel size (else the built-in default), copies it to the
 * active sheet, and resolves percent fields to px. Keeping the active sheet a
 * value copy means the rest of the shell reads plain px off lcdStyle() with no
 * per-read percent math.
 */
#include "stylesheet.h"
#include "log.h"

extern const LcdStyle lcdStyleDefault320x240;

namespace {

/* Registry of known sheets. One today; a per-board sheet is a new entry. */
const LcdStyle* const s_sheets[] = { &lcdStyleDefault320x240 };

LcdStyle s_active = lcdStyleDefault320x240;   /* safe default before begin() */

void calibrate(LcdStyle& s, int w, int h) {
    s.displayW = w;
    s.displayH = h;
    /* Resolve percents to px. Only the recents card is a percent today. */
    s.recents.cardW = (w * s.recents.cardWPct) / 100;
    /* Sanity cross-checks: a packed launcher row/column must fit the viewport. */
    int gridW = s.launcher.padLeft + s.launcher.cols * s.launcher.tileW +
                (s.launcher.cols - 1) * s.launcher.padCol;
    int gridH = s.launcher.padTop + s.launcher.rows * s.launcher.tileH +
                (s.launcher.rows - 1) * s.launcher.padRow;
    if (gridW > w)            warn("stylesheet '%s': launcher row %dpx > %dpx\n", s.name, gridW, w);
    if (gridH > h - s.statusBar.h)
        warn("stylesheet '%s': launcher grid %dpx > %dpx usable\n", s.name, gridH, h - s.statusBar.h);
}

}  // namespace

const LcdStyle& lcdStyle(void) { return s_active; }

void lcdStyleBegin(int w, int h) {
    const LcdStyle* pick = &lcdStyleDefault320x240;
    for (auto* s : s_sheets)
        if (s->displayW == w && s->displayH == h) { pick = s; break; }
    s_active = *pick;
    calibrate(s_active, w, h);
    info("stylesheet '%s' for %dx%d (recents card %dpx)\n",
         s_active.name, w, h, s_active.recents.cardW);
}
