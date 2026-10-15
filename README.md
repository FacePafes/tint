# tint

My UI library.

tint is small on purpose, it does a few things, one way each, and
leaves the rest to you. If you want a toolkit that does everything, this is not it.

## Hello

```c
#include "tint.h"
#include "fonts.h"

int main(void) {
    tint_set_font(&tint_font_8x8);
    if (tint_open(360, 240, "hello")) return 1;
    int clicks = 0;
    while (tint_begin()) {                  /* waits for input 0 = window closed */
        tint_labelf("clicked %d times", clicks);
        if (tint_button("Click me")) clicks++;
        tint_end();                         /* presents the frame */
    }
    tint_close();
}
```

## A small core with an open ended surface

tint ships about a dozen widgets:
label, button, check, radio, slider, progress, tabs, list, text, input, scroll region.
Every built in widget is written with the same public calls you have, a few lines each.
There is no widget class to subclass, no retained tree to register with, no event system to hook into.
A widget is a function that reserves some space, asks about the mouse, and draws.

Here is a complete example toggle switch, which is not built in. It is the actual code from `demo.c`:

```c
static int toggle(const char *label, int *on) {
    TintRect r = tint_cell(0);                              /* reserve a standard height cell */
    unsigned f = tint_interact(tint_id(label), r);          /* hover / press / held / click, for free */
    if (f & TINT_CLICK) *on = !*on;
    if (f & TINT_HOVER) tint_cursor(X_CUR_HAND);            /* pointer shape for this frame */
    int h = r.h, w = h * 2;
    tint_fill((TintRect){ r.x, r.y, w, h }, *on ? tint_theme.accent : tint_theme.border);
    tint_circle(r.x + (*on ? w - h / 2 : h / 2), r.y + h / 2, h / 2 - tint_px(2), tint_theme.text);
    tint_text(r.x + w + tint_px(4), r.y + (h - tint_text_h()) / 2, label, tint_theme.text);
    return !!(f & TINT_CLICK);
}
```

It lays out, scales with the screen, respects the theme and the current font, works inside scroll
regions and rows, and gets the right id and cursor. Call it like any other widget.

What you build with is small and fully public:

| you want to... | use |
|---|---|
| take space in the layout | `tint_cell`, `tint_row`, `tint_space`, `tint_indent`, `tint_avail` |
| get mouse behaviour | `tint_interact` (hover / press / held / click), `tint_in` (raw pointer, buttons, wheel, keys) |
| make your own containers | `tint_region_begin/end`: an independent flow layout in any rectangle: split panes, sidebars, cards, columns, overlays. Regions nest. |
| draw | `tint_fill`, `tint_outline`, `tint_line`, `tint_circle`, `tint_blit` (raw pixels), `tint_text`, nested clipping |
| draw anything at all | `x_fb`, the framebuffer itself: `x_w * x_h` pixels of `0x00RRGGBB`. Plot, render a game view, show an image, paint a terminal grid. |
| keep state | your own variables (immediate mode), or `tint_state(id)` for 3 ints that persist between frames |
| tell widgets apart | `tint_id`, `tint_push_id` / `tint_pop_id` |
| match the look | `tint_theme` colours, `tint_px()` scaling, `tint_text_h()` / `tint_text_w()` metrics |

The Custom tab of `demo.c` builds a draggable splitter, two independent layout panes and a
freehand canvas this way, in about 30 lines, without touching library internals. The same pattern
gives you knobs, plots, colour pickers, tables, trees, timelines and node editors. The library does
not need to know about them, and you never wait for it to grow a widget.

## Build

```sh
cc -O2 -o demo demo.c tint.c x11.c font8x8.c font_eu.c font_zh.c && ./demo
./demo 2          # 2x zoom;   ./demo 2 4 2  starts on the Fonts tab with the Chinese font
```

Requires an X11 or XWayland session (`$DISPLAY`) and a 24/32-bit little endian display.

## Files

| file | purpose |
|---|---|
| `tint.h`, `tint.c` | the UI library (`tint.h` is the full API reference) |
| `x11.h`, `x11.c` | the backend: window, framebuffer, events, clipboard, cursors. Usable on its own. |
| `fonts.h`, `font8x8.c`, `font_eu.c`, `font_zh.c` | fonts, one file each link only what you use (see below) |
| `mkfont.py` | generates a font file for any language from GNU Unifont |
| `demo.c` | a tour of everything |
| `LICENSE.md`, `COPYING.md` | LGPL-3.0 and GPL-3.0 texts (the LGPL builds on the GPL) |
| `OFL.md` | SIL Open Font License 1.1, for the Unifont-derived fonts |

## Fonts and languages

`tint.c` contains no glyph data, A font is a cell size plus a lookup callback:

```c
typedef struct { int w, h; const uint8_t *(*glyph)(uint32_t cp, int *cells); } TintFont;
```

`glyph` returns the bitmap for a Unicode codepoint (`h` rows, bit 0 = leftmost pixel), or `NULL` for
"missing", which tint draws as an outlined box. Set `*cells = 2` for a double-width glyph such as a
Chinese character. Because it is a callback, a font can be a table, a binary search, a compressed
blob, or computed. You select one with `tint_set_font()` (before `tint_open`, and you may switch at
any time), and a program that never links a font file carries none.

Included fonts:

| symbol | file | cell | covers | size |
|---|---|---|---|---|
| `tint_font_8x8` | `font8x8.c` | 8×8 | English / ASCII | 4 KB |
| `tint_font_eu` | `font_eu.c` | 8×16 | Latin-1 and Latin Extended-A (French, Spanish, German, Portuguese, Italian, Polish, Czech, Turkish, ...), Greek, Cyrillic, `€` and general punctuation (774 glyphs) | 72 KB source |
| `tint_font_zh` | `font_zh.c` | 8×16, wide | Simplified Chinese: the 3,755 GB2312 level-1 hanzi, GB2312 punctuation and symbols, and Latin-1 (4,608 glyphs) | 760 KB source, about 160 KB in the binary |

**Own fonts:**

```sh
python3 mkfont.py tw --ranges 20-7e,a0-ff --codec big5:a140-a3bf --codec big5:a440-c67e     # Traditional Chinese
python3 mkfont.py jp --ranges 20-7e,3000-30ff --codec shift_jis:889f-9ffc                    # Japanese: kana + level-1 kanji
python3 mkfont.py kr --ranges 20-7e --codec euc-kr:b0a1-c8fe                                 # Korean: 2,350 Hangul syllables
python3 mkfont.py ui --text all_my_strings.txt                                               # exactly the characters you use
```

then `extern const TintFont tint_font_tw;`. Needs Python with Pillow and the `fonts-unifont` package
(or `--font path/to/unifont.otf`). It reads Unifont's outlines on their native pixel grid, so the
result is exact bitmaps.

## How it works

**Immediate mode.** Widgets are function calls you make every frame. There are no widget objects to
create, store or free your state lives in your own variables. A checkbox is an `int*`, a slider is a
`float*`, a text field is a `char[]`. A widget returns non zero on the event it reports.

**Frames are event-driven.** `tint_begin()` blocks until something happens (input, resize, expose),
clears the framebuffer and resets the layout; `tint_end()` presents. For animation set
`tint_timeout = 16` and a frame is produced at least that often. `-1` (the default) sleeps until
input. `tint_redraw()` asks for another frame immediately.

**Layout.** Widgets stack top to bottom at full width. `tint_row(n, widths)` makes the next `n`
widgets share a row:

```c
int w[] = { 60, 0 };                    /* 60px label, input takes the rest (0 = stretch) */
tint_row(2, w);
tint_label("Name");
tint_input(name, sizeof name);

tint_row(3, NULL);                      /* NULL = three equal columns */
tint_radio("Low", &mode, 0); tint_radio("Mid", &mode, 1); tint_radio("High", &mode, 2);
```

`tint_row` is one shot: after `n` widgets the layout returns to one column. For anything more
elaborate, make a region.

**Units.** Everything passed to a *widget or layout* call is in logical pixels, multiplied by
`tint_px(1)` = `x_scale * tint_zoom`. `x_scale` (1, 2 or 3) is detected from the screen's DPI by the
backend; `tint_zoom` is your own integer multiplier (set it before `tint_open`). Drawing primitives,
`TintRect` and mouse coordinates are *physical* framebuffer pixels; use `tint_px(n)` to convert.
Font size scales the same way, and widget heights follow the font (`tint_cell(0)`).

**IDs.** A widget's identity is a hash of its label (inputs and lists use their data pointer). Two
widgets with the same label need different ids: add a hidden suffix, `"Delete##3"` (only the text
before `##` is shown), or wrap loops in `tint_push_id(i)` / `tint_pop_id()`.

## Built-in widgets

| call | returns | notes |
|---|---|---|
| `tint_label(s)`, `tint_labelf(fmt, ...)` | | |
| `tint_button(label)` | clicked | |
| `tint_check(label, int *v)` | toggled | |
| `tint_radio(label, int *v, int val)` | selected | sets `*v = val` |
| `tint_slider(label, float *v, lo, hi)` | changed | shows `label: value` |
| `tint_progress(frac)` | | 0..1 |
| `tint_sep()` | | horizontal rule |
| `tint_tabs(names, n, int *sel)` | changed | uses a whole row |
| `tint_list(items, n, int *sel, rows)` | selection changed | scrollbar, wheel, arrows / PgUp / PgDn / Home / End when focused |
| `tint_input(char *buf, size_t cap)` | `TINT_CHANGED` / `TINT_SUBMIT` | see below |
| `tint_scroll_begin(id, height)` / `tint_scroll_end()` | | clipped, scrollable sub-layout max 4 deep |

**Text input** supports arrows, Home/End, Ctrl+arrow word jumps, Backspace/Delete (Ctrl for words),
Ctrl+A/C/X/V via the X clipboard, mouse selection, double-click to select a word, triple-click for
all, and Enter (`TINT_SUBMIT`). It is UTF-8 safe, never overflows `cap`, and works with wide glyphs.
Tab cycles focus forward through inputs and lists; Esc drops focus.

```c
if (tint_input(path, sizeof path) & TINT_SUBMIT) open_file(path);
```

## Theming

Colours are `0x00RRGGBB` fields in `tint_theme`: `bg panel field border text dim accent accent_text
hot active`. Change them at any time: `tint_theme.accent = 0xe0803a;`

## Backend (`x11.h`)

tint sits on a small backend you can also use on its own:

```c
int   x_open(w, h, title, bg);     void  x_present(void);
int   x_wait(XEv *ev, int ms);     void  x_cursor(X_CUR_IBEAM);
void  x_clip_set(utf8);            char *x_clip_get(void);
void  x_set_title(utf8);           void  x_set_icon(w, h, argb);
```

Events are `X_KEY` (keysym, modifiers and the UTF-8 text to insert), `X_BTN_DOWN/UP`, `X_MOTION`,
`X_WHEEL`, `X_RESIZE`, `X_EXPOSE`, `X_LEAVE` and `X_CLOSE`.

## Known limits

- **No z-order.** Widgets do not know when something is drawn over them. An overlay is something
  you build: draw it last, in a region, and skip hit testing the widgets beneath while it is open.
- **Fonts:** no text shaping (so Arabic, Devanagari, Thai and similar scripts do not display
  correctly), no right to left layout, no combining marks, Basic Multilingual Plane only, and no
  input-method (IME) composition. Pasting works.
- No blinking caret (so no timer is needed). Two clicks drained into one frame count as one.
- Nested scroll regions give the wheel to the innermost one under the pointer.
- X11 only (XWayland counts). Little-endian servers with 32 bits per pixel only.

## Licence

tint's code (`tint.c`, `tint.h`, `x11.c`, `x11.h`) is **LGPL-3.0-or-later**: you can use it in
programs under any licence, and changes to tint itself STAY FOSS. Because tint is normally compiled
straight into your program (static linking).

The font data keeps its own licences, which are not LGPL:

| file | licence | source |
|---|---|---|
| `font8x8.c` | public domain | `font8x8_basic` by Daniel Hepper |
| `font_eu.c`, `font_zh.c`, and anything made by `mkfont.py` | SIL Open Font License 1.1 (`OFL.md`) | bitmaps extracted from [GNU Unifont](https://unifoundry.com/unifont/) 15.1, copyright 1998-2023 Roman Czyborra, Paul Hardy, Qianqian Fang, Andrew Miller, Johnnie Weaver, David Corbett, Nils Moskopp, Rebecca Bettencourt, Minseo Lee, Ho-Seok Ee, et al. |

Unifont is dual-licensed (OFL 1.1, or GPL-2+ with the Font Embedding Exception) these files use the
OFL, which lets you bundle the font data in a program under any licence. Under the OFL the font data
itself stays under the OFL and cannot be sold on its own, and `OFL.md` must travel with it. Leave out
`font_eu.c` and `font_zh.c` and nothing of Unifont is in your program: `tint.c` does not depend on
any font file.
