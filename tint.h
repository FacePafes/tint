/* tint.h - tint is an "opinionated" (used for my projects) minimal above all else immediate-mode UI on top of x11.h.
 * Software rendered, libc only. Small on purpose if you can build it from the calls below, it is not in the library.
 *
 *     tint_set_font(&tint_font_8x8);        // from fonts.h; or your own TintFont
 *     tint_open(360, 240, "hello");
 *     while (tint_begin()) {                 // sleeps until there is input; 0 = window closed
 *         tint_label("Hello");
 *         if (tint_button("Quit")) break;
 *         tint_end();                        // presents the frame
 *     }
 *     tint_close();
 *
 * Immediate mode: widgets are function calls made every frame there are no objects to
 * create or free. State lives in your variables (a checkbox is an int*, a field is a char[]).
 *
 * UNITS. Everything you pass to a *widget or layout* call (heights, widths, spacing) is in
 * logical pixels and is multiplied by tint_px(1) = x_scale * tint_zoom. Drawing primitives,
 * TintRect and mouse coordinates are physical framebuffer pixels use tint_px(n) to convert.
 *
 * IDS. A widget's identity is a hash of its label (inputs/lists: of their data pointer).
 * Two widgets with the same label need different ids: give them a hidden suffix, "Ok##a",
 * "Ok##b" (only the text before "##" is shown), or wrap loops in tint_push_id(i)/tint_pop_id().
 *
 * LGPL-3.0-or-later. */
#ifndef TINT_H
#define TINT_H
#include <stddef.h>
#include <stdint.h>
#include "x11.h"

typedef struct { int x, y, w, h; } TintRect;

/* Fonts are a cell size plus a lookup callback, so scripts can be supplied.
 * glyph(cp, &cells) returns the bitmap for Unicode codepoint cp, or NULL for "missing" (tint draws an outlined box).
 * The bitmap is h rows of ((cells*w + 7) / 8) bytes each, bit 0 = leftmost pixel. *cells is preset to 1;
 * set it to 2 for a double width glyph (e.g. CJK). Glyphs are scaled by tint_px(1). See font8x8.c for a model. */
typedef struct { int w, h; const uint8_t *(*glyph)(uint32_t cp, int *cells); } TintFont;
void tint_set_font(const TintFont *f);            /* required, before tint_open() */

typedef struct {                      /* 0x00RRGGBB; edit before tint_open() or at any time */
    uint32_t bg, panel, field, border, text, dim, accent, accent_text, hot, active;
} TintTheme;
extern TintTheme tint_theme;

typedef struct {                      /* this frame's input; read only for you */
    int mx, my;                       /* pointer, physical px (far off-screen when outside the window) */
    int down, pressed, released;      /* left button: held / went down this frame / went up this frame */
    int rclick;                       /* right button went down this frame */
    int clicks;                       /* 1, 2, 3... consecutive quick clicks of the last press */
    int wheel;                        /* +up / -down notches not yet consumed by a widget */
    XEv keys[32]; int nkeys;          /* X_KEY events this frame (Tab is consumed by the library) */
} TintIn;
extern TintIn tint_in;

extern int tint_timeout;               /* ms: -1 (default) sleeps until input; >=0 also produces a frame after that long (animation) */
extern int tint_zoom;                  /* integer UI zoom on top of x_scale; set before tint_open() */

/* frame */
int  tint_open(int w, int h, const char *title);   /* logical px; 0 on success */
int  tint_begin(void);                             /* wait for input, clear, reset layout; 0 once the window is closed */
void tint_end(void);                               /* present the frame */
void tint_close(void);
void tint_redraw(void);                            /* make the next tint_begin() return without waiting */
int  tint_px(int logical);                         /* logical -> physical */

/* layout: widgets flow top to bottom, full width */
void tint_row(int n, const int *widths);           /* next n widgets share one row; widths[] in logical px, 0 = stretch,
                                                     NULL = equal. One-shot: the layout reverts to one column after. */
void tint_space(int px);                           /* vertical gap */
void tint_indent(int px);                          /* shift left edge (negative to undo) */
void tint_push_id(int i);  void tint_pop_id(void);  /* id salt for repeated widgets */
void tint_scroll_begin(const char *id, int height);/* a scrollable, clipped sub-layout; widgets inside scroll with it */
void tint_scroll_end(void);                        /* (max 4 deep) */

/* widgets (return 1 / non-zero on the event described) */
void tint_label(const char *s);
void tint_labelf(const char *fmt, ...);
int  tint_button(const char *label);               /* clicked */
int  tint_check(const char *label, int *v);        /* toggled */
int  tint_radio(const char *label, int *v, int val);/* selected (sets *v = val) */
int  tint_slider(const char *label, float *v, float lo, float hi); /* changed; shows "label: value" */
void tint_progress(float frac);                    /* 0..1 */
void tint_sep(void);
int  tint_tabs(const char **names, int n, int *sel);/* changed; uses a whole row */
int  tint_list(const char **items, int n, int *sel, int rows); /* selection changed; *sel = -1 for none */
enum { TINT_CHANGED = 1, TINT_SUBMIT = 2 };
int  tint_input(char *buf, size_t cap);            /* TINT_CHANGED | TINT_SUBMIT (Enter). Editing: arrows, Home/End, Ctrl+arrows,
                                                     Backspace/Delete, Ctrl+A/C/X/V, mouse select, double-click word. UTF-8 safe. */

/* custom widgets */
enum { TINT_HOVER = 1, TINT_HELD = 2, TINT_CLICK = 4, TINT_PRESS = 8 };
TintRect  tint_cell(int height);                    /* reserve the next layout cell (logical height; 0 = a standard widget row) */
TintRect  tint_avail(void);                         /* the free area below the layout cursor (ends the current row) */
void     tint_region_begin(TintRect r);            /* widgets inside flow in their own layout, clipped to r (physical px). */
void     tint_region_end(void);                    /* Split panes, sidebars, cards, columns, overlays: regions nest (6 deep). */
void     tint_cursor(int x_cur_glyph);             /* pointer shape for this frame: X_CUR_HAND, X_CUR_HSIZE, ... */
int     *tint_state(uint32_t id);                  /* 3 ints of state that persist between frames, zeroed on first use */
unsigned tint_interact(uint32_t id, TintRect r);    /* hover/press/click logic; id from tint_id() */
uint32_t tint_id(const char *s);                   /* hash of a string, respecting tint_push_id */

/* drawing (physical px, clipped) */
void tint_fill(TintRect r, uint32_t c);
void tint_outline(TintRect r, uint32_t c);          /* border, tint_px(1) thick */
void tint_line(int x0, int y0, int x1, int y1, uint32_t c);
void tint_circle(int cx, int cy, int radius, uint32_t c);   /* filled */
void tint_blit(int x, int y, int w, int h, const uint32_t *rgb);
int  tint_text(int x, int y, const char *s, uint32_t c);    /* current font * tint_px(1); returns width */
int  tint_text_n(int x, int y, const char *s, int nbytes, uint32_t c);
int  tint_text_h(void);                           /* line height of the current font, physical px */
int  tint_text_w(const char *s);                   /* width in physical px */
void tint_push_clip(TintRect r);  void tint_pop_clip(void);   /* nested; intersects with the current clip */
#endif
