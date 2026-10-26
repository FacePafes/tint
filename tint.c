/* tint.c - immediate-mode UI. See tint.h. LGPL-3.0-or-later. */
#define _GNU_SOURCE
#include "tint.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

TintTheme tint_theme = { .bg = 0x1e1f22, .panel = 0x2d2f34, .field = 0x17181b, .border = 0x474a52,
                       .text = 0xe4e6ea, .dim = 0x8b8f98, .accent = 0x3d7eff, .accent_text = 0xffffff,
                       .hot = 0x3a3d44, .active = 0x4a4e57 };
TintIn tint_in;
int tint_timeout = -1, tint_zoom = 1;
int tint_raw_labels, tint_settle, tint_list_activate, tint_block, tint_slider_int, tint_ellipsis;
int tint_fill_rest, tint_square, tint_focus_next, tint_h;
const char *tint_hint;

#define BIG 3600000
static int again, closing, claimed, tab, want_cursor, grab;
static uint32_t active, focus, focus0, first_focus, salt[8]; static int nsalt;
static unsigned frame_no;
static int over_clip, over_lay, over_salt, over_scroll, sn;   /* sn: open scroll regions */

static int sc(void)  { return x_scale * tint_zoom; }
static int pad(void) { return 4 * sc(); }                 /* gap between widgets, inner padding */
static int bw(void)  { return sc(); }                     /* border width */
static const TintFont *F;                                 /* current font */
void tint_set_font(const TintFont *f) { F = f; }
static int fw(void)  { return F->w * sc(); }              /* one character cell, px */
static int fh(void)  { return F->h * sc(); }              /* line height, px */
static int rowh(void){ return fh() + 2 * pad(); }         /* height of a standard widget */
int  tint_px(int n)   { return n * sc(); }
int  tint_lpx(int n)  { return n / sc(); }
void tint_redraw(void){ again = 1; }

long tint_now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000L; }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int iclamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ids and perwidget state */
static uint32_t top_salt(void) { return nsalt ? salt[nsalt - 1] : 0; }
static uint32_t hstr(const char *s, size_t n) {
    uint32_t h = 2166136261u ^ top_salt();
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h ? h : 1;
}
static uint32_t hptr(const void *p) { uint32_t h = (uint32_t)(((uintptr_t)p >> 2) * 2654435761u) ^ top_salt(); return h ? h : 1; }
uint32_t tint_id(const char *s) { return hstr(s, strlen(s)); }
void tint_push_id(int i) {
    if (nsalt < 8) { salt[nsalt] = (top_salt() * 16777619u) ^ ((uint32_t)i + 0x9e3779b9u); nsalt++; }
    else over_salt++;
}
void tint_pop_id(void) { if (over_salt) over_salt--; else if (nsalt) nsalt--; }

static struct { uint32_t id; unsigned used; int v[3]; } stt[64];   /* LRU table of int[3] */
int *tint_state(uint32_t id) {
    int f = 0;
    for (int i = 0; i < 64; i++) {
        if (stt[i].id == id) { stt[i].used = frame_no; return stt[i].v; }
        if (stt[i].used < stt[f].used) f = i;
    }
    memset(&stt[f], 0, sizeof stt[f]); stt[f].id = id; stt[f].used = frame_no;
    return stt[f].v;
}

/* clipping and primitives */
static TintRect cs[16]; static int cn;
#define CLIP cs[cn]
static TintRect isect(TintRect a, TintRect b) {
    int x0 = imax(a.x, b.x), y0 = imax(a.y, b.y), x1 = imin(a.x + a.w, b.x + b.w), y1 = imin(a.y + a.h, b.y + b.h);
    TintRect r = { x0, y0, imax(x1 - x0, 0), imax(y1 - y0, 0) };
    return r;
}
static TintRect visible(TintRect r) { TintRect fb = { 0, 0, x_w, x_h }; return isect(isect(r, CLIP), fb); }
void tint_push_clip(TintRect r) { if (cn < 15) { cs[cn + 1] = isect(cs[cn], r); cn++; } else over_clip++; }
void tint_pop_clip(void) { if (over_clip) over_clip--; else if (cn > 0) cn--; }
static int inside(TintRect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
static int hovered(TintRect r) { return !tint_block && inside(r, tint_in.mx, tint_in.my) && inside(CLIP, tint_in.mx, tint_in.my); }

void tint_fill(TintRect r, uint32_t c) {
    r = visible(r);
    for (int y = r.y; y < r.y + r.h; y++) { uint32_t *p = x_fb + (size_t)y * x_w + r.x; for (int i = 0; i < r.w; i++) p[i] = c; }
}
void tint_outline(TintRect r, uint32_t c) {
    int b = bw();
    tint_fill((TintRect){ r.x, r.y, r.w, b }, c);               tint_fill((TintRect){ r.x, r.y + r.h - b, r.w, b }, c);
    tint_fill((TintRect){ r.x, r.y + b, b, r.h - 2 * b }, c);    tint_fill((TintRect){ r.x + r.w - b, r.y + b, b, r.h - 2 * b }, c);
}
void tint_line(int x0, int y0, int x1, int y1, uint32_t c) {   /* Bresenham each point is a tint_px(1) square */
    long dx = labs((long)x1 - x0), dy = -labs((long)y1 - y0), err = dx + dy;   /* long: no int overflow on extreme coords */
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, t = bw();
    for (;;) {
        tint_fill((TintRect){ x0, y0, t, t }, c);
        if (x0 == x1 && y0 == y1) break;
        long e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
void tint_circle(int cx, int cy, int r, uint32_t c) {
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0; while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;
        tint_fill((TintRect){ cx - dx, cy + dy, 2 * dx + 1, 1 }, c);
    }
}
void tint_blit(int x, int y, int w, int h, const uint32_t *px) {
    TintRect v = visible((TintRect){ x, y, w, h });
    for (int j = v.y; j < v.y + v.h; j++)
        memcpy(x_fb + (size_t)j * x_w + v.x, px + (size_t)(j - y) * w + (v.x - x), (size_t)v.w * 4);
}

/* UTF-8 */
static int u8_next(const char *s, int i) { if (!s[i]) return i; i++; while ((s[i] & 0xC0) == 0x80) i++; return i; }
static int u8_prev(const char *s, int i) { if (i <= 0) return 0; i--; while (i > 0 && (s[i] & 0xC0) == 0x80) i--; return i; }
static uint32_t u8_get(const char **p, const char *e, int *bad) {
    const unsigned char *s = (const unsigned char *)*p; uint32_t c = s[0], min; int n, i;
    if (bad) *bad = 0;
    if (c < 0x80) { *p += 1; return c; }
    if (c >= 0xC2 && c <= 0xDF) { n = 2; c &= 0x1F; min = 0x80; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 3; c &= 0x0F; min = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 4; c &= 0x07; min = 0x10000; }
    else goto invalid;
    if (e - *p < n) goto invalid;
    for (i = 1; i < n; i++) { if ((s[i] & 0xC0) != 0x80) goto invalid; c = c << 6 | (s[i] & 63); }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) goto invalid;
    *p += n; return c;
invalid:
    *p += 1; if (bad) *bad = 1; return 0xFFFD;
}

/* font plumbing (see TintFont in tint.h this file contains no glyph data) */
static const uint8_t *lookup(uint32_t cp, int *cells) {
    *cells = 1; const uint8_t *g = F->glyph(cp, cells);
    if (*cells != 2) *cells = 1;
    return g;
}
static int adv(uint32_t cp) { int n; lookup(cp, &n); return n * fw(); }          /* advance of one character, px */
static int tw(const char *s, int nbytes) {                                       /* pixel width of the first nbytes bytes */
    int w = 0; const char *p = s, *e = s + imax(nbytes, 0);
    while (p < e && *p) w += adv(u8_get(&p, e, NULL));
    return w;
}
static int idx_at(const char *b, int px) {                                       /* byte offset of the character boundary nearest px */
    int i = 0, x = 0; const char *e = b + strlen(b);
    while (b[i]) { const char *p = b + i; int w = adv(u8_get(&p, e, NULL)); if (px < x + w / 2) break; x += w; i = (int)(p - b); }
    return i;
}
static void glyph(int x, int y, uint32_t cp, uint32_t c) {
    int s = sc(), n; const uint8_t *g = lookup(cp, &n); int W = n * F->w;
    if (!g) {                                                                    /* missing glyph: outlined box */
        int bw_ = (W - 2) * s, bh = (F->h - 2) * s;
        if (bw_ > 2 * s && bh > 2 * s) {
            tint_fill((TintRect){ x + s, y + s, bw_, s }, c);              tint_fill((TintRect){ x + s, y + s + bh - s, bw_, s }, c);
            tint_fill((TintRect){ x + s, y + 2 * s, s, bh - 2 * s }, c);   tint_fill((TintRect){ x + s + bw_ - s, y + 2 * s, s, bh - 2 * s }, c);
        }
        return;
    }
    int rb = (W + 7) / 8;
    for (int r = 0; r < F->h; r++) {
        const uint8_t *row = g + r * rb;
        for (int b = 0; b < W; b++) {
            if (!(row[b >> 3] >> (b & 7) & 1)) continue;
            int e = b; while (e < W && (row[e >> 3] >> (e & 7) & 1)) e++;        /* merge a horizontal run */
            tint_fill((TintRect){ x + b * s, y + r * s, (e - b) * s, s }, c); b = e;
        }
    }
}
int tint_text_n(int x, int y, const char *t, int n, uint32_t c) {
    int x0 = x, h = fh(); const char *e = t + imax(n, 0);
    while (t < e && *t) {
        uint32_t cp = u8_get(&t, e, NULL); int a = adv(cp);
        if (x + a > CLIP.x && x < CLIP.x + CLIP.w && y + h > CLIP.y && y < CLIP.y + CLIP.h) glyph(x, y, cp, c);
        x += a;
    }
    return x - x0;
}
int tint_text(int x, int y, const char *t, uint32_t c) { return tint_text_n(x, y, t, (int)strlen(t), c); }
int tint_text_w(const char *t) { return tw(t, (int)strlen(t)); }
static int llen(const char *s) { const char *h = tint_raw_labels ? NULL : strstr(s, "##"); return h ? (int)(h - s) : (int)strlen(s); }   /* visible part of a label */
static void text_c(TintRect r, const char *s, int n, uint32_t c) {                                                 /* centered, clipped to r */
    tint_push_clip(r);
    tint_text_n(r.x + (r.w - tw(s, n)) / 2, r.y + (r.h - fh()) / 2, s, n, c);
    tint_pop_clip();
}
int tint_text_c(TintRect r, const char *s, uint32_t c) { int n = (int)strlen(s); text_c(r, s, n, c); return tw(s, n); }
static void text_fit(int x, int y, int maxw, const char *s, int n, uint32_t c) {   /* left aligned: "..." if tint_ellipsis and too wide */
    if (!tint_ellipsis || tw(s, n) <= maxw) { tint_text_n(x, y, s, n, c); return; }
    int room = maxw - tw("...", 3), w = 0; const char *p = s, *e = s + n;
    while (p < e && *p) { const char *q = p; int a = adv(u8_get(&q, e, NULL)); if (w + a > room) break; w += a; p = q; }
    tint_text_n(x, y, s, (int)(p - s), c); tint_text_n(x + w, y, "...", 3, c);
}

/* layout */
typedef struct { int x, w, cy, n, i, ry, rh, cx[16], cw[16]; } Lay;
static Lay L[8]; static int ln;
#define LY L[ln]

static void setrow(Lay *l, int n, const int *w) {
    int s = sc(), g = pad(), fixed = 0, flex = 0;
    n = iclamp(n, 1, 16);
    for (int i = 0; i < n; i++) { if (w && w[i] > 0) fixed += w[i] * s; else flex++; }
    int avail = imax(l->w - g * (n - 1) - fixed, 0), x = l->x;
    for (int i = 0; i < n; i++) {
        int cw;
        if (w && w[i] > 0) cw = w[i] * s; else { cw = avail / flex; avail -= cw; flex--; }
        l->cx[i] = x; l->cw[i] = cw; x += cw + g;
    }
    l->n = n; l->i = 0;
}
static void endrow(Lay *l) { if (l->i > 0) { l->cy = l->ry + l->rh + pad(); l->i = 0; } l->n = 0; }
static TintRect next_px(int h) {
    Lay *l = &LY;
    if (tint_h > 0) h = tint_h * sc();
    tint_h = 0;
    if (l->n == 0) setrow(l, 1, NULL);
    if (l->i == 0) { l->ry = l->cy; l->rh = 0; }
    TintRect r = { l->cx[l->i], l->ry, l->cw[l->i], h };
    if (tint_square) { r.w = h; tint_square = 0; }
    l->rh = imax(l->rh, h);
    if (++l->i >= l->n) { l->cy = l->ry + l->rh + pad(); l->i = 0; l->n = 0; }
    return r;
}
static int take_fill(void) {                        /* tint_fill_rest: height from the next cell's top to the clip bottom, else -1 */
    if (!tint_fill_rest) return -1;
    tint_fill_rest = 0; tint_h = 0;
    Lay *l = &LY; int top = l->i ? l->ry : l->cy;
    return imax(CLIP.y + CLIP.h - top, 0);
}
void tint_row(int n, const int *w) { endrow(&LY); setrow(&LY, n, w); }
void tint_space(int px) { endrow(&LY); LY.cy += px * sc(); }
void tint_indent(int px) { endrow(&LY); LY.x += px * sc(); LY.w -= px * sc(); }
TintRect tint_cell(int h) { return next_px(h > 0 ? h * sc() : rowh()); }
int tint_text_h(void) { return fh(); }
void tint_cursor(int glyph) { want_cursor = glyph; }
TintRect tint_avail(void) {                         /* what is left below the cursor in the current layout/clip */
    Lay *l = &LY; endrow(l);
    return (TintRect){ l->x, l->cy, l->w, imax(CLIP.y + CLIP.h - l->cy, 0) };
}
void tint_region_begin(TintRect r) {                /* an independent flow layout inside any rectangle */
    tint_push_clip(r);
    if (ln >= 7) { over_lay++; return; }            /* too deep: widgets flow in the parent layout; never clobber it */
    ln++;
    memset(&LY, 0, sizeof LY); LY.x = r.x; LY.w = r.w; LY.cy = r.y;
}
void tint_region_end(void) {
    if (over_lay) over_lay--; else if (ln > 0) { endrow(&LY); ln--; }
    tint_pop_clip();
}

/* interaction */
unsigned tint_interact(uint32_t id, TintRect r) {
    if (tint_block) return 0;
    int in = hovered(r); unsigned f = 0;
    if (in && (!active || active == id)) f |= TINT_HOVER;
    if (in && tint_in.pressed && !active) { active = id; f |= TINT_PRESS; }
    if (active == id && tint_in.down) f |= TINT_HELD;
    if (active == id && tint_in.released) { if (in) f |= TINT_CLICK; active = 0; }
    return f;
}
/* Tab cycles keyboard focus through focusable widgets (inputs, lists). */
static int nkeys(void) { return tint_block ? 0 : tint_in.nkeys; }
static void focusable(uint32_t id) {
    if (tint_block) return;
    if (tint_focus_next) { focus = id; claimed = 1; tint_focus_next = 0; }
    if (!first_focus) first_focus = id;
    if (tab == 2 || (tab == 1 && !focus)) { focus = id; tab = 0; }
    else if (tab == 1 && focus == id) tab = 2;
}

/* frame */
int tint_open(int w, int h, const char *title) {
    if (!F) { fprintf(stderr, "tint: no font set; call tint_set_font() before tint_open()\n"); return 1; }
    if (tint_zoom < 1) tint_zoom = 1;
    if (x_open(w * tint_zoom, h * tint_zoom, title, tint_theme.bg)) return 1;
    again = 1; tint_in.focused = 1; return 0;
}
void tint_close(void) { free(x_fb); x_fb = NULL; x_close(); }

int tint_begin(void) {
    tint_in.pressed = tint_in.released = tint_in.rclick = tint_in.wheel = tint_in.nkeys = 0;
    int wait = again ? 0 : tint_timeout < 0 ? BIG : tint_timeout, first = 1; XEv e;
    again = 0;
    for (;;) {
        if (!x_wait(&e, first ? wait : 0)) { if (first && wait == BIG) continue; break; }
        first = 0;
        switch (e.type) {
        case X_CLOSE: closing = 1; break;
        case X_MOTION: tint_in.mx = e.x; tint_in.my = e.y; break;
        case X_LEAVE: if (!tint_in.down) tint_in.mx = tint_in.my = -100000; break;
        case X_WHEEL: tint_in.mx = e.x; tint_in.my = e.y; tint_in.wheel += e.dy; break;
        case X_BTN_DOWN:
            tint_in.mx = e.x; tint_in.my = e.y;
            if (e.button == 3) tint_in.rclick = 1;
            if (e.button == 1) {
                static long t0; static int x0, y0; long t = tint_now_ms();
                tint_in.clicks = (t - t0 < 400 && abs(e.x - x0) < tint_px(4) && abs(e.y - y0) < tint_px(4)) ? tint_in.clicks + 1 : 1;
                t0 = t; x0 = e.x; y0 = e.y; tint_in.down = tint_in.pressed = 1;
            }
            break;
        case X_BTN_UP:
            tint_in.mx = e.x; tint_in.my = e.y;
            if (e.button == 1) { tint_in.down = 0; tint_in.released = 1; }
            break;
        case X_FOCUS_IN: tint_in.focused = 1; break;
        case X_FOCUS_OUT: tint_in.focused = 0; break;
        case X_KEY:
            if (e.key == K_TAB && !(e.mod & (M_CTRL | M_ALT))) tab = 1;
            else if (tint_in.nkeys < 32) tint_in.keys[tint_in.nkeys++] = e;
            break;
        default: break;     /* X_RESIZE / X_EXPOSE just mean "draw a frame" */
        }
    }
    if (closing) return 0;
    cn = 0; cs[0] = (TintRect){ 0, 0, x_w, x_h };
    tint_fill(cs[0], tint_theme.bg);
    ln = 0; memset(&L[0], 0, sizeof L[0]); L[0].x = pad(); L[0].w = imax(x_w - 2 * pad(), 0); L[0].cy = pad();
    nsalt = 0; over_clip = over_lay = over_salt = over_scroll = 0; sn = 0; claimed = 0; focus0 = focus; first_focus = 0; want_cursor = X_CUR_ARROW; frame_no++;
    return 1;
}
void tint_end(void) {
    if (tint_settle && (tint_in.pressed || tint_in.released || tint_in.rclick || tint_in.nkeys || tab)) again = 1;
    if (tint_in.pressed && !claimed) focus = 0;      /* click on nothing focusable: drop focus */
    if (tab == 2) focus = first_focus;              /* Tab past the last widget wraps around */
    tab = 0;
    if (!tint_in.down) active = 0;
    if (focus != focus0) again = 1;                 /* earlier widgets were drawn with the old focus */
    x_cursor(want_cursor);
    x_present();
}

/* scrollbar: draws the track + thumb, handles dragging returns 1 if *off changed */
static int vscroll(uint32_t id, TintRect t, int content, int *off) {
    int view = t.h, mx = content - view, th = view * view / imax(content, 1);
    tint_fill(t, tint_theme.panel);
    if (mx <= 0) { *off = 0; return 0; }
    th = imax(th, 12 * sc());
    if (view - th <= 0) return 0;
    int ty = t.y + (int)((long)*off * (view - th) / mx), ch = 0;
    unsigned f = tint_interact(id, t);
    if (f & TINT_PRESS) grab = (tint_in.my >= ty && tint_in.my < ty + th) ? tint_in.my - ty : th / 2;
    if (f & TINT_HELD) {
        int n = iclamp((int)((long)(tint_in.my - grab - t.y) * mx / (view - th)), 0, mx);
        if (n != *off) { *off = n; ch = 1; }
        ty = t.y + (int)((long)*off * (view - th) / mx);
    }
    tint_fill((TintRect){ t.x + bw(), ty, t.w - 2 * bw(), th }, (f & (TINT_HELD | TINT_HOVER)) ? tint_theme.dim : tint_theme.border);
    return ch;
}

/* scroll region */
static struct { TintRect r; uint32_t id; int off0; } SC[4];
void tint_scroll_begin(const char *label, int h) {
    int b = bw(), p = pad(), sb = 8 * sc();
    int fr = take_fill();
    TintRect r = next_px(fr >= 0 ? fr : h * sc());
    if (sn >= 4 || ln >= 7) { over_scroll++; return; }   /* too deep: contents flow unscrolled in the parent */
    uint32_t id = tint_id(label); int *v = tint_state(id);
    v[0] = iclamp(v[0], 0, imax(v[1] - (r.h - 2 * b), 0));
    tint_fill(r, tint_theme.field);
    SC[sn].r = r; SC[sn].id = id; SC[sn].off0 = v[0]; sn++;
    tint_push_clip((TintRect){ r.x + b, r.y + b, r.w - 2 * b, r.h - 2 * b });
    ln++;
    memset(&LY, 0, sizeof LY); LY.x = r.x + b + p; LY.w = imax(r.w - 2 * b - 2 * p - sb, 0); LY.cy = r.y + b + p - v[0];
}
void tint_scroll_end(void) {
    if (over_scroll) { over_scroll--; return; }
    if (!sn || !ln) return;
    int b = bw(); sn--; TintRect r = SC[sn].r; int *v = tint_state(SC[sn].id);
    endrow(&LY);
    int content = LY.cy - (r.y + b - SC[sn].off0), view = r.h - 2 * b, off = SC[sn].off0;
    v[1] = content;
    ln--; tint_pop_clip();
    if (hovered(r) && tint_in.wheel) { off -= tint_in.wheel * 36 * sc(); tint_in.wheel = 0; }
    off = iclamp(off, 0, imax(content - view, 0));
    vscroll(SC[sn].id ^ 0xA5A5A5A5u, (TintRect){ r.x + r.w - b - 8 * sc(), r.y + b, 8 * sc(), view }, content, &off);
    off = iclamp(off, 0, imax(content - view, 0));
    v[0] = off;
    if (off != SC[sn].off0) again = 1;              /* content was laid out with the old offset */
    tint_outline(r, tint_theme.border);
}

/* simple widgets */
void tint_label(const char *s) {
    TintRect r = next_px(rowh());
    tint_push_clip(r); text_fit(r.x, r.y + (r.h - fh()) / 2, r.w, s, llen(s), tint_theme.text); tint_pop_clip();
}
void tint_labelf(const char *fmt, ...) {
    char b[512]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a); tint_label(b);
}
void tint_sep(void) { TintRect r = next_px(pad()); tint_fill((TintRect){ r.x, r.y + r.h / 2, r.w, bw() }, tint_theme.border); }

int tint_button(const char *label) {
    TintRect r = next_px(rowh()); unsigned f = tint_interact(tint_id(label), r);
    uint32_t bg = (f & TINT_HELD) && (f & TINT_HOVER) ? tint_theme.active : (f & TINT_HOVER) ? tint_theme.hot : tint_theme.panel;
    tint_fill(r, bg); tint_outline(r, tint_theme.border);
    text_c(r, label, llen(label), tint_theme.text);
    return !!(f & TINT_CLICK);
}
int tint_check(const char *label, int *v) {
    int s = sc(), bs = fh() + 2 * s; TintRect r = next_px(rowh()); unsigned f = tint_interact(tint_id(label), r);
    if (f & TINT_CLICK) *v = !*v;
    TintRect bx = { r.x, r.y + (r.h - bs) / 2, bs, bs };
    tint_fill(bx, tint_theme.field); tint_outline(bx, (f & TINT_HOVER) ? tint_theme.accent : tint_theme.border);
    if (*v) tint_fill((TintRect){ bx.x + 3 * s, bx.y + 3 * s, bs - 6 * s, bs - 6 * s }, tint_theme.accent);
    tint_push_clip(r); text_fit(r.x + bs + pad(), r.y + (r.h - fh()) / 2, r.w - bs - pad(), label, llen(label), tint_theme.text); tint_pop_clip();
    return !!(f & TINT_CLICK);
}
int tint_radio(const char *label, int *v, int val) {
    int s = sc(), rad = (fh() + 2 * s) / 2; TintRect r = next_px(rowh()); unsigned f = tint_interact(tint_id(label), r);
    int res = !!(f & TINT_CLICK); if (res) *v = val;
    int cx = r.x + rad, cy = r.y + r.h / 2;
    tint_circle(cx, cy, rad, (f & TINT_HOVER) ? tint_theme.accent : tint_theme.border);
    tint_circle(cx, cy, rad - s, tint_theme.field);
    if (*v == val) tint_circle(cx, cy, rad - 3 * s, tint_theme.accent);
    tint_push_clip(r); text_fit(r.x + 2 * rad + pad(), r.y + (r.h - fh()) / 2, r.w - 2 * rad - pad(), label, llen(label), tint_theme.text); tint_pop_clip();
    return res;
}
int tint_slider(const char *label, float *v, float lo, float hi) {
    int b = bw(), res = 0; TintRect r = next_px(rowh()); unsigned f = tint_interact(tint_id(label), r);
    if ((f & (TINT_HELD | TINT_PRESS | TINT_CLICK)) && hi > lo && r.w > 2 * b) {
        float t = (float)(tint_in.mx - (r.x + b)) / (float)(r.w - 2 * b);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        float nv = lo + t * (hi - lo);
        if (tint_slider_int) { nv = (float)(long)(nv + (nv < 0 ? -0.5f : 0.5f)); nv = nv < lo ? lo : nv > hi ? hi : nv; }
        if (nv != *v) { *v = nv; res = 1; }
    }
    float t = hi > lo ? (*v - lo) / (hi - lo) : 0; t = t < 0 ? 0 : t > 1 ? 1 : t;
    tint_fill(r, tint_theme.field);
    tint_fill((TintRect){ r.x + b, r.y + b, (int)((r.w - 2 * b) * t), r.h - 2 * b }, tint_theme.accent);
    tint_outline(r, (f & TINT_HOVER) ? tint_theme.dim : tint_theme.border);
    char buf[128]; int n = tint_slider_int ? snprintf(buf, sizeof buf, "%.*s: %d", llen(label), label, (int)(*v + (*v < 0 ? -0.5f : 0.5f)))
                                           : snprintf(buf, sizeof buf, "%.*s: %.2f", llen(label), label, *v);
    text_c(r, buf, imin(n, (int)sizeof buf - 1), tint_theme.accent_text);
    return res;
}
void tint_progress(float t) {
    int b = bw(); TintRect r = next_px(rowh()); t = t < 0 ? 0 : t > 1 ? 1 : t;
    tint_fill(r, tint_theme.field);
    tint_fill((TintRect){ r.x + b, r.y + b, (int)((r.w - 2 * b) * t), r.h - 2 * b }, tint_theme.accent);
    tint_outline(r, tint_theme.border);
    char buf[16]; int n = snprintf(buf, sizeof buf, "%d%%", (int)(t * 100 + 0.5f)); text_c(r, buf, n, tint_theme.accent_text);
}
int tint_tabs(const char **names, int n, int *sel) {
    TintRect r = next_px(rowh()); int res = 0, w = r.w / imax(n, 1);
    for (int i = 0; i < n; i++) {
        TintRect t = { r.x + i * w, r.y, i == n - 1 ? r.w - i * w : w, r.h };
        unsigned f = tint_interact(tint_id(names[i]) ^ 0x7ab5c3d1u, t);
        if ((f & TINT_CLICK) && *sel != i) { *sel = i; res = 1; }
        tint_fill(t, i == *sel ? tint_theme.accent : (f & TINT_HOVER) ? tint_theme.hot : tint_theme.panel);
        tint_outline(t, tint_theme.border);
        text_c(t, names[i], llen(names[i]), i == *sel ? tint_theme.accent_text : tint_theme.text);
    }
    return res;
}

/* list */
int tint_list(const char **it, int n, int *sel, int rows) {
    int s = sc(), b = bw(), rh = fh() + pad(), res = 0, p = pad(); rows = imax(rows, 1);
    int fr = take_fill();
    TintRect r = next_px(fr >= 0 ? fr : rows * rh + 2 * b), in = { r.x + b, r.y + b, r.w - 2 * b, r.h - 2 * b };
    rows = imax(in.h / rh, 1);                         /* page size equals the argument unless tint_fill_rest / tint_h changed the height */
    uint32_t id = hptr(sel); int *v = tint_state(id); focusable(id);
    int content = n * rh, view = in.h, mx = imax(content - view, 0);
    if (hovered(r) && tint_in.wheel) { v[0] -= tint_in.wheel * 3 * rh; tint_in.wheel = 0; }
    if (focus == id && n > 0)
        for (int k = 0; k < nkeys(); k++) {
            int key = tint_in.keys[k].key, d = key == K_UP ? -1 : key == K_DOWN ? 1 : key == K_PGUP ? -rows :
                      key == K_PGDN ? rows : key == K_HOME ? -n : key == K_END ? n : 0;
            if (key == K_RETURN && tint_list_activate && *sel >= 0 && *sel < n) res |= TINT_SUBMIT;
            if (!d) continue;
            int ns = iclamp(*sel < 0 && d > 0 ? d - 1 : *sel + d, 0, n - 1);
            if (ns != *sel) { *sel = ns; res |= TINT_CHANGED; }
            if (ns * rh < v[0]) v[0] = ns * rh;
            if ((ns + 1) * rh > v[0] + view) v[0] = (ns + 1) * rh - view;
        }
    v[0] = iclamp(v[0], 0, mx);
    tint_fill(r, tint_theme.field);
    int sb = mx > 0 ? 8 * s : 0;
    if (sb) vscroll(id ^ 0xA5A5A5A5u, (TintRect){ in.x + in.w - sb, in.y, sb, in.h }, content, &v[0]);
    tint_push_clip((TintRect){ in.x, in.y, in.w - sb, in.h });
    for (int i = v[0] / rh; i < n && i * rh - v[0] < view; i++) {
        TintRect ir = { in.x, in.y + i * rh - v[0], in.w - sb, rh };
        unsigned f = tint_interact(id + (uint32_t)(i + 1) * 0x9E3779B1u, ir);
        if (f & TINT_PRESS) {
            focus = id; claimed = 1; if (*sel != i) { *sel = i; res |= TINT_CHANGED; }
            if (tint_list_activate && tint_in.clicks == 2) res |= TINT_SUBMIT;
        }
        if (i == *sel) tint_fill(ir, tint_theme.accent); else if (f & TINT_HOVER) tint_fill(ir, tint_theme.hot);
        text_fit(ir.x + p, ir.y + (rh - fh()) / 2, ir.w - 2 * p, it[i], (int)strlen(it[i]), i == *sel ? tint_theme.accent_text : tint_theme.text);
    }
    tint_pop_clip();
    tint_outline(r, focus == id ? tint_theme.accent : tint_theme.border);
    return res;
}

/* text input */
/* Clipboard text is untrusted: make it valid UTF-8 with no control characters (C0, DEL, C1) before it can reach
 * the caller's buffer. In place; the output is never longer than the input. */
static void sanitize_paste(char *t) {
    const char *p = t, *e = t + strlen(t); char *o = t;
    while (p < e) {
        const char *q = p; int bad; uint32_t cp = u8_get(&p, e, &bad);
        if (bad) *o++ = '?';
        else if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) *o++ = ' ';
        else while (q < p) *o++ = *q++;
    }
    *o = 0;
}
static int is_sp(char c) { return isspace((unsigned char)c); }
static int w_prev(const char *b, int i) { while (i > 0 && is_sp(b[i - 1])) i--; while (i > 0 && !is_sp(b[i - 1])) i--; return i; }
static int w_next(const char *b, int i) { while (b[i] && !is_sp(b[i])) i++; while (b[i] && is_sp(b[i])) i++; return i; }
static int ed_del(char *b, int len, int lo, int hi) { memmove(b + lo, b + hi, (size_t)(len - hi + 1)); return len - (hi - lo); }
static int ed_ins(char *b, int cap, int len, int at, const char *t, int n) {
    if (n <= 0 || len + n + 1 > cap) return len;
    memmove(b + at + n, b + at, (size_t)(len - at + 1)); memcpy(b + at, t, (size_t)n); return len + n;
}

int tint_input(char *buf, size_t capz) {
    int s = sc(), b = bw(), p = pad(), cw = fw(), cap = (int)capz, res = 0;
    const char *hint = tint_hint; tint_hint = NULL;
    TintRect r = next_px(rowh());
    if (cap < 1) return 0;
    uint32_t id = hptr(buf); int *v = tint_state(id); focusable(id);
    int len = (int)strlen(buf), cur = iclamp(v[0], 0, len), anc = iclamp(v[1], 0, len);
    unsigned f = tint_interact(id, r);
    if (f & TINT_HOVER) want_cursor = X_CUR_IBEAM;
    int iw = r.w - 2 * p;

    if (f & (TINT_PRESS | TINT_HELD)) {                                    /* mouse: place caret / drag select */
        int at = idx_at(buf, imax(tint_in.mx - (r.x + p) + v[2], 0));
        if (f & TINT_PRESS) {
            focus = id; claimed = 1;
            if (tint_in.clicks == 2) { cur = anc = at; while (anc > 0 && !is_sp(buf[anc - 1])) anc--; while (buf[cur] && !is_sp(buf[cur])) cur++; }
            else if (tint_in.clicks >= 3) { anc = 0; cur = len; }
            else cur = anc = at;
        } else if (tint_in.clicks == 1) cur = at;
    }
    if (focus == id)
        for (int k = 0; k < nkeys(); k++) {
            XEv *e = &tint_in.keys[k]; int ctrl = e->mod & M_CTRL, sh = e->mod & M_SHIFT;
            int lo = imin(cur, anc), hi = imax(cur, anc);
            switch (e->key) {
            case K_LEFT:  cur = (lo != hi && !sh && !ctrl) ? lo : ctrl ? w_prev(buf, cur) : u8_prev(buf, cur); if (!sh) anc = cur; break;
            case K_RIGHT: cur = (lo != hi && !sh && !ctrl) ? hi : ctrl ? w_next(buf, cur) : u8_next(buf, cur); if (!sh) anc = cur; break;
            case K_HOME: cur = 0;   if (!sh) anc = cur; break;
            case K_END:  cur = len; if (!sh) anc = cur; break;
            case K_BACKSPACE: case K_DELETE:
                if (lo == hi) {
                    if (e->key == K_BACKSPACE) lo = ctrl ? w_prev(buf, cur) : u8_prev(buf, cur);
                    else hi = ctrl ? w_next(buf, cur) : u8_next(buf, cur);
                }
                if (hi > lo) { len = ed_del(buf, len, lo, hi); cur = anc = lo; res |= TINT_CHANGED; }
                break;
            case K_RETURN: res |= TINT_SUBMIT; break;
            case K_ESCAPE: focus = 0; break;
            default:
                if (ctrl && e->key == 'a') { anc = 0; cur = len; }
                else if (ctrl && (e->key == 'c' || e->key == 'x')) {
                    if (hi > lo) {
                        char *t = malloc((size_t)(hi - lo) + 1);
                        if (t) {
                            memcpy(t, buf + lo, (size_t)(hi - lo)); t[hi - lo] = 0;
                            x_clip_set(t); free(t);
                            if (e->key == 'x') { len = ed_del(buf, len, lo, hi); cur = anc = lo; res |= TINT_CHANGED; }
                        }
                    }
                } else if (ctrl && e->key == 'v') {
                    char *t = x_clip_get();
                    if (t) {
                        sanitize_paste(t);
                        if (hi > lo) { len = ed_del(buf, len, lo, hi); cur = anc = lo; res |= TINT_CHANGED; }
                        int n = (int)strlen(t), room = cap - 1 - len;
                        if (n > room) { n = imax(room, 0); while (n > 0 && (t[n] & 0xC0) == 0x80) n--; }   /* don't cut a character */
                        int nl = ed_ins(buf, cap, len, cur, t, n);
                        if (nl != len) { len = nl; cur += n; anc = cur; res |= TINT_CHANGED; }
                        free(t);
                    }
                } else if (e->text[0] && !ctrl) {
                    if (hi > lo) { len = ed_del(buf, len, lo, hi); cur = anc = lo; res |= TINT_CHANGED; }
                    int n = (int)strlen(e->text), nl = ed_ins(buf, cap, len, cur, e->text, n);
                    if (nl != len) { len = nl; cur += n; anc = cur; res |= TINT_CHANGED; }
                }
            }
        }
    v[0] = cur; v[1] = anc;
    int cx = tw(buf, cur);                                   /* keep the caret visible */
    if (cx < v[2]) v[2] = cx;
    if (cx + cw > v[2] + iw) v[2] = cx + cw - iw;
    if (tw(buf, len) + cw <= iw) v[2] = 0;
    v[2] = imax(v[2], 0);

    TintRect tx = { r.x + p - v[2], r.y + (r.h - fh()) / 2, 0, fh() };
    tint_fill(r, tint_theme.field);
    tint_push_clip((TintRect){ r.x + b, r.y + b, r.w - 2 * b, r.h - 2 * b });
    if (cur != anc) {
        int a = tw(buf, imin(cur, anc)), z = tw(buf, imax(cur, anc));
        tint_fill((TintRect){ tx.x + a, tx.y - s, z - a, fh() + 2 * s }, tint_theme.accent);
    }
    tint_text(tx.x, tx.y, buf, tint_theme.text);
    if (hint && !len && focus != id) text_fit(r.x + p, tx.y, iw, hint, (int)strlen(hint), tint_theme.dim);
    if (focus == id) tint_fill((TintRect){ tx.x + cx, tx.y - s, s, fh() + 2 * s }, tint_theme.text);
    tint_pop_clip();
    tint_outline(r, focus == id ? tint_theme.accent : (f & TINT_HOVER) ? tint_theme.dim : tint_theme.border);
    return res;
}
