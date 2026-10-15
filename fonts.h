/* fonts.h - the bundled fonts. Link only the .c file(s) you use tint.c depends on none of them.
 *   tint_font_8x8   font8x8.c   8x8   English / ASCII                                          (public domain)
 *   tint_font_eu    font_eu.c   8x16  Latin (French, Spanish, German, ...), Greek, Cyrillic    (Unifont, OFL)
 *   tint_font_zh    font_zh.c   8x16  Simplified Chinese (GB2312 level 1) + Latin-1            (Unifont, OFL)
 * Generate more with mkfont.py. */
#ifndef TINT_FONTS_H
#define TINT_FONTS_H
#include "tint.h"
extern const TintFont tint_font_8x8, tint_font_eu, tint_font_zh;
#endif