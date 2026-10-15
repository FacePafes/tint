/* x11.h - a tiny X11 client that speaks the wire protocol directly.
 * Needs only libc: no Xlib, no xcb, no dev headers. LGPL-3.0-or-later. */
#ifndef X11_H
#define X11_H
#include <stdint.h>

enum { X_NONE, X_KEY, X_BTN_DOWN, X_BTN_UP, X_MOTION, X_WHEEL, X_RESIZE, X_EXPOSE, X_CLOSE, X_LEAVE };
enum { M_CTRL = 1, M_SHIFT = 2, M_ALT = 4 };

/* X keysyms for the keys we care about printable keys use their ASCII value. */
enum { K_BACKSPACE = 0xff08, K_TAB = 0xff09, K_RETURN = 0xff0d, K_ESCAPE = 0xff1b,
       K_HOME = 0xff50, K_LEFT = 0xff51, K_UP = 0xff52, K_RIGHT = 0xff53, K_DOWN = 0xff54,
       K_PGUP = 0xff55, K_PGDN = 0xff56, K_END = 0xff57, K_DELETE = 0xffff };

/* glyph numbers in the standard X "cursor" font, for x_cursor() */
enum { X_CUR_CROSS = 34, X_CUR_HAND = 60, X_CUR_ARROW = 68, X_CUR_HSIZE = 108,
       X_CUR_VSIZE = 116, X_CUR_WAIT = 150, X_CUR_IBEAM = 152 };

typedef struct {
    int type;
    int key;          /* X_KEY: keysym (letters lowercased when Ctrl is held) */
    unsigned mod;     /* M_* bits */
    char text[8];     /* X_KEY: UTF-8 to insert, "" if the key is not text */
    int x, y, button; /* mouse events */
    int dy;           /* X_WHEEL: +1 up, -1 down */
    int w, h;         /* X_RESIZE */
} XEv;

extern uint32_t *x_fb;   /* 0x00RRGGBB framebuffer, x_w * x_h */
extern int x_w, x_h;
extern int x_scale;      /* 1, 2 or 3: integer UI magnification suggested by the screen's pixel density */

int  x_open(int w, int h, const char *title, uint32_t bg); /* 0 on success, error on stderr */
int  x_wait(XEv *ev, int timeout_ms);                      /* 1 if an event was returned */
void x_present(void);                                      /* upload the framebuffer */
void x_clip_set(const char *utf8);
char *x_clip_get(void);                                    /* malloc'd, or NULL */
void x_set_icon(int w, int h, const uint32_t *argb);       /* ARGB, after x_open */
void x_set_title(const char *utf8);
void x_cursor(int glyph);                                  /* X_CUR_*; default is X_CUR_ARROW */
void x_close(void);
#endif