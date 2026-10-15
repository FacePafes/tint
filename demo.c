/* demo.c - tour of tint.
 *   cc -O2 -o demo demo.c tint.c x11.c font8x8.c font_eu.c font_zh.c && ./demo [zoom [tab [font]]]
 * LGPL-3.0-or-later. */
#include "tint.h"
#include "fonts.h"
#include <stdio.h>
#include <stdlib.h>

/* A custom widget: a toggle switch. Everything here is public API: tint_cell, tint_interact, the primitives. */
static int toggle(const char *label, int *on) {
    TintRect r = tint_cell(0);                                       /* reserve a standard-height cell */
    unsigned f = tint_interact(tint_id(label), r);                   /* hover / press / click for free */
    if (f & TINT_CLICK) *on = !*on;
    if (f & TINT_HOVER) tint_cursor(X_CUR_HAND);
    int h = r.h, w = h * 2;
    tint_fill((TintRect){ r.x, r.y, w, h }, *on ? tint_theme.accent : tint_theme.border);
    tint_circle(r.x + (*on ? w - h / 2 : h / 2), r.y + h / 2, h / 2 - tint_px(2), tint_theme.text);
    tint_text(r.x + w + tint_px(4), r.y + (h - tint_text_h()) / 2, label, tint_theme.text);
    return !!(f & TINT_CLICK);
}

#define NPTS 8192
int main(int argc, char **argv) {
    if (argc > 1) tint_zoom = atoi(argv[1]);
    int start = argc > 2 ? atoi(argv[2]) : 0, font0 = argc > 3 ? atoi(argv[3]) : 0;
    tint_set_font(&tint_font_8x8);
    if (tint_open(460, 400, "tint demo")) return 1;

    const char *tabs[] = { "Widgets", "List", "Scroll", "Custom", "Fonts" };
    const char *fruit[] = { "Apple", "Apricot", "Banana", "Blackberry", "Cherry", "Damson", "Elderberry", "Fig",
                            "Grape", "Guava", "Kiwi", "Lemon", "Lime", "Mango", "Nectarine", "Orange", "Pear", "Plum" };
    const TintFont *fonts[] = { &tint_font_8x8, &tint_font_eu, &tint_font_zh };
    int tab = start, chk = 1, mode = 1, clicks = 0, sel = 2, which = font0, sw = 1, split = 0;
    float vol = 0.4f;
    char name[64] = "edit me", note[64] = "", mixed[96] = "Édith, ça va ? Привет! 你好";
    static struct { int x, y; } pts[NPTS]; int np = 0;               /* canvas strokes; x = -1 starts a new one */

    while (tint_begin()) {
        tint_set_font(fonts[which]);
        tint_tabs(tabs, 5, &tab);
        tint_space(4);

        if (tab == 0) {
            tint_label("Immediate mode widgets");
            tint_sep();
            tint_row(2, NULL);
            if (tint_button("Click me please!")) clicks++;
            if (tint_button("Reset me!")) clicks = 0;
            tint_labelf("clicked it %d times", clicks);
            tint_check("Enable the thingy", &chk);
            tint_row(3, NULL);
            tint_radio("Low", &mode, 0); tint_radio("Mid", &mode, 1); tint_radio("High", &mode, 2);
            tint_slider("volume", &vol, 0, 1);
            tint_progress(vol);
            tint_sep();
            int w[] = { 60, 0 };
            tint_row(2, w); tint_label("Name"); tint_input(name, sizeof name);
            tint_row(2, w); tint_label("Note");
            if (tint_input(note, sizeof note) & TINT_SUBMIT) { snprintf(name, sizeof name, "%s", note); note[0] = 0; }
            tint_labelf("Hello, %s!  (Type in note.)", name);
        } else if (tab == 1) {
            tint_list(fruit, 18, &sel, 12);
            tint_labelf("selected: %s", sel >= 0 ? fruit[sel] : "(none)");
            tint_label("Click it, then you can use the arrow key/PgUp/PgDn/Home/End.");
        } else if (tab == 2) {
            tint_scroll_begin("rows", 220);
            for (int i = 0; i < 30; i++) {
                tint_push_id(i);
                int w[] = { 0, 70 };
                tint_row(2, w); tint_labelf("Row %d", i);
                if (tint_button("Buttons")) clicks++;
                tint_pop_id();
            }
            tint_scroll_end();
            tint_labelf("clicks so far: %d", clicks);
        } else if (tab == 3) {
            toggle("custom toggle switch", &sw);
            tint_label("Split and two independent regions and canvas:");
            TintRect a = tint_avail();                               /* everything below this point */
            int bar = tint_px(6);
            if (!split) split = a.w / 3;
            TintRect grip = { a.x + split, a.y, bar, a.h };
            unsigned f = tint_interact(tint_id("split"), grip);      /* the splitter is just another widget */
            if (f & TINT_HELD) split = tint_in.mx - a.x - bar / 2;
            if (f & (TINT_HOVER | TINT_HELD)) tint_cursor(X_CUR_HSIZE);
            if (split < tint_px(60)) split = tint_px(60);
            if (split > a.w - tint_px(80)) split = a.w - tint_px(80);
            grip.x = a.x + split;
            tint_fill(grip, (f & (TINT_HOVER | TINT_HELD)) ? tint_theme.accent : tint_theme.border);

            TintRect l = { a.x, a.y, split, a.h }, r = { a.x + split + bar, a.y, a.w - split - bar, a.h };
            tint_fill(l, tint_theme.field); tint_region_begin(l);    /* left pane: its own flow layout */
            tint_label("left pane"); if (tint_button("Button")) clicks++; tint_labelf("%d", clicks);
            tint_region_end();

            unsigned c = tint_interact(tint_id("canvas"), r);        /* right pane: freehand canvas */
            if ((c & TINT_PRESS) && np < NPTS) pts[np++].x = -1;
            if ((c & TINT_HELD) && np < NPTS - 1) { pts[np].x = tint_in.mx; pts[np].y = tint_in.my; np++; }
            tint_fill(r, tint_theme.field);
            tint_push_clip(r);
            for (int i = 1; i < np; i++) if (pts[i].x >= 0 && pts[i - 1].x >= 0) tint_line(pts[i - 1].x, pts[i - 1].y, pts[i].x, pts[i].y, tint_theme.accent);
            tint_text(r.x + tint_px(4), r.y + tint_px(4), "you can draw here", tint_theme.dim);
            tint_pop_clip();
            if (tint_in.rclick && tint_in.mx >= r.x) np = 0;         /* right-click clears */
        } else {
            tint_label("Font:");
            tint_row(3, NULL);
            if (tint_radio("8x8 English", &which, 0)) tint_redraw();
            if (tint_radio("Europe", &which, 1)) tint_redraw();
            if (tint_radio("Chinese", &which, 2)) tint_redraw();
            tint_sep();
            tint_label("The quick brown fox jumps over the lazy dog");
            tint_label("français ? => çàéèêôûù");
            tint_label("Привет, мир!   Γειά σου, Κόσμε");
            tint_label("你好，世界！ 确定 取消 设置 文件");
            tint_sep();
            tint_label("Type or paste anything:");
            tint_input(mixed, sizeof mixed);
            tint_label("(missing glyphs are shown as boxes)");
        }
        tint_end();
    }
    tint_close();
    return 0;
}