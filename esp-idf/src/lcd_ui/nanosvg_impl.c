/* nanosvg_impl.c — the single translation unit that emits nanosvg's function
 * bodies. lcd_icons.cpp includes the same headers for the declarations only
 * (without the IMPLEMENTATION macros), so the (warning-heavy, C99) library code
 * compiles once as C, away from our -Werror C++ flags. */
#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#include "nanosvg.h"

#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
