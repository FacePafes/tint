/* x11.c - minimal X11 protocol client. See x11.h. LGPL-3.0-or-later. */
#define _GNU_SOURCE
#include "x11.h"
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

uint32_t *x_fb; int x_w, x_h, x_scale = 1;
int x_allow_synthetic = 0, x_trust_selection = 0;
int x_keyup = 0, x_focus = 0, x_fixed_size = 0;
#define MAXDIM 16384                 
#define MAXPIX ((size_t)1 << 26)     
static int scr_w, scr_h;

#define QN 512
#define NCUR 154
static struct {
    int fd, depth; uint16_t seq; unsigned maxreq;
    uint32_t base, mask, n, root, win, gc;
    int kmin, kspk; uint32_t *ks;
    uint32_t a_proto, a_del, a_netname, a_utf8, a_clip, a_targets, a_prop, a_icon;
    char *clip; int clip_own;
    XEv q[QN]; int qh, qt;
    int sel_done, sel_wait; uint32_t sel_prop;
    uint32_t font, cur[NCUR]; int cur_now;
    XEv rel; int rel_pending, rel_code; uint32_t rel_time;   /* a KeyRelease held back until we know it is not auto-repeat */
} X;

static void p16(uint8_t *b, int o, unsigned v) { b[o] = v & 255; b[o + 1] = (v >> 8) & 255; }
static void p32(uint8_t *b, int o, uint32_t v) { p16(b, o, v & 0xffff); p16(b, o + 2, v >> 16); }
static unsigned g16(const uint8_t *b, int o) { return b[o] | (unsigned)b[o + 1] << 8; }
static uint32_t g32(const uint8_t *b, int o) { return g16(b, o) | (uint32_t)g16(b, o + 2) << 16; }
static int pad4(int n) { return (n + 3) & ~3; }

static long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000L; }

static void wr_all(const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t r = write(X.fd, b, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { fprintf(stderr, "tint: lost connection to X server\n"); exit(1); }
        b += r; n -= r;
    }
}
static int rd_try(void *p, size_t n) {
    uint8_t *b = p;
    while (n) {
        ssize_t r = read(X.fd, b, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return 0;
        b += r; n -= r;
    }
    return 1;
}
static void rd_all(void *p, size_t n) {
    if (!rd_try(p, n)) { fprintf(stderr, "tint: lost connection to X server\n"); exit(1); }
}
static void send_req(const void *a, size_t n) { X.seq++; wr_all(a, n); }
static uint32_t new_id(void) { return X.base | (X.n++ & X.mask); }

/* connection */
static int connect_display(const char *d, char *numbuf) {
    const char *colon = strrchr(d, ':');
    if (!colon) return -1;
    int num = atoi(colon + 1);
    snprintf(numbuf, 16, "%d", num);
    char host[256];
    size_t hl = (size_t)(colon - d);
    if (hl >= sizeof host) return -1;
    memcpy(host, d, hl); host[hl] = 0;
    if (!*host || !strcmp(host, "unix")) {
        for (int abs = 0; abs < 2; abs++) {
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;
            int n = snprintf(a.sun_path + abs, sizeof a.sun_path - abs, "/tmp/.X11-unix/X%d", num);
            if (connect(fd, (struct sockaddr *)&a, offsetof(struct sockaddr_un, sun_path) + abs + n) == 0) return fd;
            close(fd);
        }
        return -1;
    }
    char port[16]; snprintf(port, sizeof port, "%d", 6000 + num);
    struct addrinfo hints, *res, *r;
    memset(&hints, 0, sizeof hints); hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res)) return -1;
    int fd = -1;
    for (r = res; r; r = r->ai_next) {
        fd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, r->ai_addr, r->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* MIT-MAGIC-COOKIE-1 from ~/.Xauthority 💀*/
static int find_cookie(const char *num, int fd, uint8_t *cookie, int *clen) {
    struct sockaddr_storage ss; socklen_t sl = sizeof ss;
    int local = 0; const uint8_t *ip = NULL; unsigned iplen = 0, ipfam = 0;
    if (getpeername(fd, (struct sockaddr *)&ss, &sl)) return 0;
    if (ss.ss_family == AF_UNIX) local = 1;
    else if (ss.ss_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)&ss;
        ip = (const uint8_t *)&a->sin_addr; iplen = 4; ipfam = 0;       /* FamilyInternet */
        if (ip[0] == 127) local = 1;
    } else if (ss.ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)&ss;
        ip = (const uint8_t *)&a->sin6_addr; iplen = 16; ipfam = 6;     /* FamilyInternet6 */
        if (IN6_IS_ADDR_LOOPBACK(&a->sin6_addr)) local = 1;
    } else return 0;

    const char *path = getenv("XAUTHORITY"); char buf[600];
    if (!path) { const char *h = getenv("HOME"); if (!h) return 0; snprintf(buf, sizeof buf, "%s/.Xauthority", h); path = buf; }
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    uint8_t *d = malloc(1 << 20); if (!d) { fclose(f); return 0; }
    size_t n = fread(d, 1, 1 << 20, f); fclose(f);
    char host[256] = ""; gethostname(host, sizeof host - 1);
    size_t p = 0; int best = -1;
    #define BE16(o) ((d[o] << 8) | d[(o) + 1])
    while (p + 2 <= n) {
        unsigned fam = BE16(p); p += 2;
        if (p + 2 > n) break;
        unsigned al = BE16(p); p += 2; size_t ap = p; p += al;
        if (p + 2 > n) break;
        unsigned nl = BE16(p); p += 2; size_t np = p; p += nl;
        if (p + 2 > n) break;
        unsigned ml = BE16(p); p += 2; size_t mp = p; p += ml;
        if (p + 2 > n) break;
        unsigned dl = BE16(p); p += 2; size_t dp = p; p += dl;
        if (p > n) break;
        if (ml != 18 || memcmp(d + mp, "MIT-MAGIC-COOKIE-1", 18)) continue;
        if (nl && !(nl == strlen(num) && !memcmp(d + np, num, nl))) continue;
        int score = 0;
        if (fam == 65535) score = 1;                                                           /* FamilyWild: meant for any host */
        else if (ip && fam == ipfam && al == iplen && !memcmp(d + ap, ip, iplen)) score = 2;   /* the peer's own address */
        else if (local && fam == 256 && al == strlen(host) && !memcmp(d + ap, host, al)) score = 3;  /* FamilyLocal, this host */
        if (!score) continue;                                                                  /* entry is for some other host: never send it */
        if (score > best && dl <= 64) { best = score; memcpy(cookie, d + dp, dl); *clen = (int)dl; }
    }
    free(d);
    return best > 0;
}

/* message reading */
static void process_raw(const uint8_t *e);

static void read_msg(uint8_t *m) {
    rd_all(m, 32);
    if ((m[0] & 0x7f) == 35) {                           /* GenericEvent: skip its payload */
        size_t n = (size_t)g32(m, 4) * 4; uint8_t junk[512];
        while (n) { size_t k = n < sizeof junk ? n : sizeof junk; rd_all(junk, k); n -= k; }
        m[0] = 0x7f;
    }
}
/* Wait for the reply to the request with the sequence number `want` events met on the way are queued. */
static int read_reply(uint16_t want, uint8_t hdr[32], uint8_t **extra) {
    for (;;) {
        uint8_t m[32]; read_msg(m);
        if (m[0] == 1) {
            size_t n = (size_t)g32(m, 4) * 4; *extra = NULL;
            if (n) { *extra = malloc(n); if (!*extra) { fprintf(stderr, "tint: out of memory\n"); exit(1); } rd_all(*extra, n); }
            memcpy(hdr, m, 32); return 1;
        }
        if (m[0] == 0) {
            if (g16(m, 2) == want) return 0;
            continue; /* error from an earlier fire and-forget request (e.g. missing cursor font) */
        }
        process_raw(m);
    }
}
static int poll_readable(int ms) {
    struct pollfd p = { X.fd, POLLIN, 0 };
    int r = poll(&p, 1, ms);
    return r > 0;
}

/* requests */
static void chg_prop(uint32_t win, uint32_t prop, uint32_t type, int fmt, const void *data, size_t nbytes) {
    size_t maxw = X.maxreq ? X.maxreq : 4096;
    if (maxw > 65535) maxw = 65535;
    size_t chunk = (maxw - 6) * 4;                       /* data bytes per request */
    const uint8_t *src = data; int mode = 0;             /* 0 = Replace, 2 = Append */
    do {
        size_t n = nbytes < chunk ? nbytes : chunk, padn = (4 - (n & 3)) & 3;
        uint8_t hd[24]; memset(hd, 0, sizeof hd);
        hd[0] = 18; hd[1] = (uint8_t)mode; p16(hd, 2, (unsigned)(6 + (n + padn) / 4));
        p32(hd, 4, win); p32(hd, 8, prop); p32(hd, 12, type); hd[16] = (uint8_t)fmt;
        p32(hd, 20, (uint32_t)(n / (size_t)(fmt / 8)));
        X.seq++; wr_all(hd, 24); wr_all(src, n); wr_all("\0\0\0", padn);
        src += n; nbytes -= n; mode = 2;
    } while (nbytes);
}
static void set_title(const char *t) {
    if (!t) t = "";
    chg_prop(X.win, 39, 31, 8, t, strlen(t));
    chg_prop(X.win, X.a_netname, X.a_utf8, 8, t, strlen(t));
}
void x_set_title(const char *t) { set_title(t); }

int x_open(int w, int h, const char *title, uint32_t bg) {
    if (w < 1 || h < 1 || w > MAXDIM || h > MAXDIM) { fprintf(stderr, "tint: bad window size %dx%d\n", w, h); return 1; }
    signal(SIGPIPE, SIG_IGN);
    const char *disp = getenv("DISPLAY");
    if (!disp || !*disp) { fprintf(stderr, "tint: DISPLAY is not set (needs an X11 or XWayland session)\n"); return 1; }
    char num[16];
    X.fd = connect_display(disp, num);
    if (X.fd < 0) { fprintf(stderr, "tint: cannot connect to X display '%s'\n", disp); return 1; }

    uint8_t cookie[64]; int cl = 0;
    if (!find_cookie(num, X.fd, cookie, &cl)) cl = 0;
    int name_len = cl ? 18 : 0;
    uint8_t req[12 + 20 + 64 + 8]; memset(req, 0, sizeof req);
    req[0] = 'l'; p16(req, 2, 11); p16(req, 4, 0);
    p16(req, 6, (unsigned)name_len); p16(req, 8, (unsigned)cl);
    int off = 12;
    if (cl) { memcpy(req + off, "MIT-MAGIC-COOKIE-1", 18); off += pad4(18); memcpy(req + off, cookie, (size_t)cl); off += pad4(cl); }
    wr_all(req, (size_t)off);

    uint8_t hd[8]; rd_all(hd, 8);
    size_t blen = g16(hd, 6) * 4u;
    uint8_t *s = malloc(blen + 1); rd_all(s, blen);
    if (hd[0] != 1) {
        char msg[256]; int k = 0, rl = hd[1] < blen ? hd[1] : (int)blen;   /* server text: never print raw control bytes */
        for (int i = 0; i < rl && k < (int)sizeof msg - 1; i++) msg[k++] = (s[i] >= 0x20 && s[i] < 0x7f) ? (char)s[i] : '?';
        msg[k] = 0; fprintf(stderr, "tint: X server refused connection: %s\n", msg); return 1;
    }
    if (blen < 32) { fprintf(stderr, "tint: malformed X setup reply\n"); return 1; }

    X.base = g32(s, 4); X.mask = g32(s, 8); X.maxreq = g16(s, 18);
    int nscreens = s[20], nfmt = s[21], kmin = s[26];
    if (s[22] != 0) { fprintf(stderr, "tint: big-endian X servers are not supported\n"); return 1; }
    X.kmin = kmin;
    size_t o = 32 + (size_t)pad4((int)g16(s, 16));
    if (nscreens < 1 || o + (size_t)nfmt * 8 + 40 > blen) { fprintf(stderr, "tint: malformed X setup reply\n"); return 1; }
    int fmt_bpp[256] = {0};
    for (int i = 0; i < nfmt; i++) fmt_bpp[s[o + i * 8]] = s[o + i * 8 + 1];
    o += (size_t)nfmt * 8;
    X.root = g32(s, (int)o); X.depth = s[o + 38];
    scr_w = (int)g16(s, (int)o + 20); scr_h = (int)g16(s, (int)o + 22);
    {   /* pixel density from the screen's reported size; 0 mm (unknown) leaves the scale at 1 */
        unsigned mm = g16(s, (int)o + 24);
        unsigned dpi = mm ? (unsigned)((unsigned long)scr_w * 254UL / ((unsigned long)mm * 10UL)) : 96;
        x_scale = dpi >= 240 ? 3 : dpi >= 144 ? 2 : 1;
    }
    if (X.depth != 24 && X.depth != 32) { fprintf(stderr, "tint: unsupported screen depth %d\n", X.depth); return 1; }
    if (fmt_bpp[X.depth] != 32) { fprintf(stderr, "tint: unsupported pixel format\n"); return 1; }
    free(s);
    if (X.maxreq < 4096) X.maxreq = 4096;

    /* atoms (pipelined) */
    uint32_t *dst[] = { &X.a_proto, &X.a_del, &X.a_netname, &X.a_utf8, &X.a_clip, &X.a_targets, &X.a_prop, &X.a_icon };
    const char *names[] = { "WM_PROTOCOLS", "WM_DELETE_WINDOW", "_NET_WM_NAME", "UTF8_STRING", "CLIPBOARD", "TARGETS", "_TINT_PASTE", "_NET_WM_ICON" };
    uint16_t seqs[8];
    for (int i = 0; i < 8; i++) {
        size_t l = strlen(names[i]); uint8_t b[64]; memset(b, 0, sizeof b);
        b[0] = 16; p16(b, 2, 2 + (unsigned)pad4((int)l) / 4); p16(b, 4, (unsigned)l); memcpy(b + 8, names[i], l);
        send_req(b, 8 + (size_t)pad4((int)l)); seqs[i] = X.seq;
    }
    for (int i = 0; i < 8; i++) {
        uint8_t h[32], *ex;
        if (read_reply(seqs[i], h, &ex)) { *dst[i] = g32(h, 8); free(ex); }
    }

    /* keyboard map */
    {
        uint8_t b[8] = {0}; b[0] = 101; p16(b, 2, 2); b[4] = (uint8_t)kmin; b[5] = (uint8_t)(255 - kmin + 1);
        send_req(b, 8);
        uint8_t h[32], *ex;
        if (read_reply(X.seq, h, &ex)) {
            X.kspk = h[1]; size_t cnt = g32(h, 4);
            if (ex && X.kspk > 0 && cnt >= (size_t)(256 - kmin) * (size_t)X.kspk) {   /* every (code, idx) keysym() can ask for must exist */
                X.ks = malloc(cnt * 4 + 4);
                if (X.ks) for (size_t i = 0; i < cnt; i++) X.ks[i] = g32(ex, (int)(i * 4));
            }
            free(ex);
        }
    }

    /* window + gc */
    w *= x_scale; h *= x_scale;                          /* keep the same number of characters per line */
    if (scr_w > 0 && w > scr_w * 9 / 10) w = scr_w * 9 / 10;
    if (scr_h > 0 && h > scr_h * 9 / 10) h = scr_h * 9 / 10;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > MAXDIM) w = MAXDIM;
    if (h > MAXDIM) h = MAXDIM;
    if ((size_t)w * (size_t)h > MAXPIX) { fprintf(stderr, "tint: window too large\n"); return 1; }
    x_w = w; x_h = h; x_fb = calloc((size_t)w * (size_t)h, 4);
    if (!x_fb) { fprintf(stderr, "tint: out of memory\n"); return 1; }
    X.win = new_id(); X.gc = new_id();
    {
        uint8_t b[40]; memset(b, 0, sizeof b);
        b[0] = 1; b[1] = 0; p16(b, 2, 10);
        p32(b, 4, X.win); p32(b, 8, X.root); p16(b, 12, 0); p16(b, 14, 0);
        p16(b, 16, (unsigned)w); p16(b, 18, (unsigned)h); p16(b, 20, 0); p16(b, 22, 1);
        p32(b, 24, 0);                       /* visual: CopyFromParent */
        p32(b, 28, 0x2 | 0x800);             /* back-pixel | event-mask */
        p32(b, 32, bg);
        /* key, btn press/release, leave, pointer motion, btn motion, expose, structure (+ key release, focus change) */
        p32(b, 36, 1 | 4 | 8 | 32 | 64 | (1 << 13) | (1 << 15) | (1 << 17) | (x_keyup ? 2u : 0u) | (x_focus ? 1u << 21 : 0u));
        send_req(b, 40);
    }
    {
        uint8_t b[16] = {0}; b[0] = 55; p16(b, 2, 4); p32(b, 4, X.gc); p32(b, 8, X.win); p32(b, 12, 0);
        send_req(b, 16);
    }
    set_title(title);
    chg_prop(X.win, 67, 31, 8, "tint\0tint", sizeof "tint\0Tint");           /* WM_CLASS */
    chg_prop(X.win, X.a_proto, 4, 32, &X.a_del, 4);            /* WM_PROTOCOLS: WM_DELETE_WINDOW */
    if (x_fixed_size) {                                        /* WM_NORMAL_HINTS: PMinSize | PMaxSize = the size we opened */
        uint8_t sh[72]; memset(sh, 0, sizeof sh);
        p32(sh, 0, 16 | 32); p32(sh, 20, (uint32_t)w); p32(sh, 24, (uint32_t)h); p32(sh, 28, (uint32_t)w); p32(sh, 32, (uint32_t)h);
        chg_prop(X.win, 40, 41, 32, sh, sizeof sh);            /* atoms 40 = WM_NORMAL_HINTS, 41 = WM_SIZE_HINTS */
    }

    /* cursors come from the standard "cursor" font (errors, if any, are ignored) */
    {
        uint8_t b[24]; memset(b, 0, sizeof b);
        X.font = new_id();
        b[0] = 45; p16(b, 2, 5); p32(b, 4, X.font); p16(b, 8, 6); memcpy(b + 12, "cursor", 6);
        send_req(b, 20);
    }
    x_cursor(X_CUR_ARROW);
    { uint8_t b[8] = {0}; b[0] = 8; p16(b, 2, 2); p32(b, 4, X.win); send_req(b, 8); } /* MapWindow */
    return 0;
}

void x_cursor(int shape) {
    if (shape < 0 || shape >= NCUR || shape == X.cur_now) return;
    if (!X.cur[shape]) {
        X.cur[shape] = new_id();
        uint8_t c[32]; memset(c, 0, sizeof c);
        c[0] = 94; p16(c, 2, 8); p32(c, 4, X.cur[shape]); p32(c, 8, X.font); p32(c, 12, X.font);
        p16(c, 16, (unsigned)shape); p16(c, 18, (unsigned)shape + 1);
        p16(c, 20, 0); p16(c, 22, 0); p16(c, 24, 0);               /* fore: black */
        p16(c, 26, 0xffff); p16(c, 28, 0xffff); p16(c, 30, 0xffff); /* back: white */
        send_req(c, 32);
    }
    uint8_t a[16]; memset(a, 0, sizeof a);
    a[0] = 2; p16(a, 2, 4); p32(a, 4, X.win); p32(a, 8, 0x4000); p32(a, 12, X.cur[shape]);
    send_req(a, 16);
    X.cur_now = shape;
}

void x_set_icon(int w, int h, const uint32_t *argb) {
    if (w < 1 || h < 1 || w > 4096 || h > 4096 || !argb) return;
    size_t n = (size_t)w * (size_t)h; uint8_t *b = malloc((n + 2) * 4);
    if (!b) return;
    p32(b, 0, (uint32_t)w); p32(b, 4, (uint32_t)h);
    for (size_t i = 0; i < n; i++) p32(b, (int)(8 + i * 4), argb[i]);
    chg_prop(X.win, X.a_icon, 6, 32, b, (n + 2) * 4);
    free(b);
}

void x_close(void) { if (X.fd > 0) close(X.fd); }

/* drawing */
void x_present(void) {
    if (x_w < 1 || x_h < 1) return;
    size_t stride = (size_t)x_w * 4;
    size_t maxb = (size_t)X.maxreq * 4 - 24;
    size_t rows = maxb / stride;
    if (rows < 1) return;                    
    for (int y = 0; y < x_h; y += (int)rows) {
        int h = (int)rows < x_h - y ? (int)rows : x_h - y;
        uint8_t hd[24]; memset(hd, 0, sizeof hd);
        hd[0] = 72; hd[1] = 2; p16(hd, 2, 6 + (unsigned)h * (unsigned)x_w);
        p32(hd, 4, X.win); p32(hd, 8, X.gc);
        p16(hd, 12, (unsigned)x_w); p16(hd, 14, (unsigned)h); p16(hd, 16, 0); p16(hd, 18, (unsigned)y);
        hd[21] = (uint8_t)X.depth;
        X.seq++; wr_all(hd, 24); wr_all(x_fb + (size_t)y * (size_t)x_w, (size_t)h * stride);
    }
}

/* clipboard */
void x_clip_set(const char *s) {
    free(X.clip); X.clip = strdup(s); X.clip_own = 1;
    uint8_t b[16] = {0}; b[0] = 22; p16(b, 2, 4); p32(b, 4, X.win); p32(b, 8, X.a_clip); p32(b, 12, 0);
    send_req(b, 16);
}
static void sel_request(const uint8_t *e) {
    uint32_t req = g32(e, 12), sel = g32(e, 16), tgt = g32(e, 20), prop = g32(e, 24);
    if (!prop) prop = tgt;
    int ok = 0;
    if (sel == X.a_clip && X.clip && strlen(X.clip) < 200000) {
        if (tgt == X.a_targets) { uint32_t t[3] = { X.a_targets, X.a_utf8, 31 }; uint8_t tb[12]; for (int i = 0; i < 3; i++) p32(tb, i * 4, t[i]); chg_prop(req, prop, 4, 32, tb, 12); ok = 1; }
        else if (tgt == X.a_utf8 || tgt == 31) { chg_prop(req, prop, tgt, 8, X.clip, strlen(X.clip)); ok = 1; }
    }
    uint8_t b[44]; memset(b, 0, sizeof b);
    b[0] = 25; p16(b, 2, 11); p32(b, 4, req); p32(b, 8, 0);
    b[12] = 31; p32(b, 16, g32(e, 4)); p32(b, 20, req); p32(b, 24, sel); p32(b, 28, tgt); p32(b, 32, ok ? prop : 0);
    send_req(b, 44);
}
char *x_clip_get(void) {
    if (X.clip_own && X.clip) return strdup(X.clip);
    uint8_t b[24] = {0}; b[0] = 24; p16(b, 2, 6);
    p32(b, 4, X.win); p32(b, 8, X.a_clip); p32(b, 12, X.a_utf8); p32(b, 16, X.a_prop); p32(b, 20, 0);
    X.sel_done = 0; X.sel_wait = 1; send_req(b, 24);
    long end = now_ms() + 600;
    while (!X.sel_done) {
        long left = end - now_ms(); if (left <= 0) { X.sel_wait = 0; return NULL; }
        if (poll_readable((int)left)) { uint8_t m[32]; read_msg(m); if (m[0] == 1 || m[0] == 0) continue; process_raw(m); }
    }
    X.sel_wait = 0;
    if (!X.sel_prop) return NULL;
    uint8_t g[24] = {0}; g[0] = 20; g[1] = 1; p16(g, 2, 6);
    p32(g, 4, X.win); p32(g, 8, X.sel_prop); p32(g, 12, 0); p32(g, 16, 0); p32(g, 20, 65536);
    send_req(g, 24);
    uint8_t h[32], *ex; char *out = NULL;
    if (read_reply(X.seq, h, &ex)) {
        size_t have = (size_t)g32(h, 4) * 4, n = g32(h, 16);       /* n comes from the server: it must fit in what was actually sent */
        if (h[1] == 8 && g32(h, 8) != X.a_proto && ex && n <= have) { out = malloc(n + 1); if (out) { memcpy(out, ex, n); out[n] = 0; } }
        free(ex);
    }
    return out;
}

/* events */
static void push(const XEv *e) { int n = (X.qt + 1) % QN; if (n == X.qh) return; X.q[X.qt] = *e; X.qt = n; }

static unsigned mods_of(unsigned st) { return ((st & 4) ? M_CTRL : 0) | ((st & 1) ? M_SHIFT : 0) | ((st & 8) ? M_ALT : 0); }

static uint32_t keysym(int code, int idx) {
    if (!X.ks || code < X.kmin || idx >= X.kspk) return 0;
    return X.ks[(code - X.kmin) * X.kspk + idx];
}
static int utf8_enc(uint32_t c, char *o) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | ((c >> 12) & 63)); o[2] = (char)(0x80 | ((c >> 6) & 63)); o[3] = (char)(0x80 | (c & 63)); return 4;
}
static int key_ev(const uint8_t *e, XEv *out) {      /* decode a Key event; 0 if it is to be ignored */
    int code = e[1]; unsigned st = g16(e, 28);
    int shift = st & 1, lock = st & 2, numlock = st & 16;
    int base = (st & 0x80) ? 2 : 0;
    uint32_t ks = keysym(code, base + (shift ? 1 : 0));
    if (!ks && shift) ks = keysym(code, base);
    if (!ks) ks = keysym(code, 0);
    if (ks >= 0xff80 && ks <= 0xffbd && numlock) { uint32_t k2 = keysym(code, 1); if (k2) ks = k2; }
    if (lock) {
        if (ks >= 'a' && ks <= 'z' && !shift) ks -= 32;
        else if (ks >= 'A' && ks <= 'Z' && shift) ks += 32;
    }
    /* keypad and odd aliases -> canonical keysyms */
    if (ks == 0xff8d) ks = K_RETURN;
    else if (ks == 0xfe20) ks = K_TAB;
    else if (ks >= 0xffb0 && ks <= 0xffb9) ks = '0' + (ks - 0xffb0);
    else if (ks == 0xffae) ks = '.'; else if (ks == 0xffab) ks = '+'; else if (ks == 0xffad) ks = '-';
    else if (ks == 0xffaa) ks = '*'; else if (ks == 0xffaf) ks = '/';
    else if (ks >= 0xff95 && ks <= 0xff9f) {
        static const uint32_t m[] = { K_HOME, K_LEFT, K_UP, K_RIGHT, K_DOWN, K_PGUP, K_PGDN, K_END, 0, 0, 0, K_DELETE };
        uint32_t r = m[ks - 0xff95]; if (r) ks = r;
    }
    if (ks >= 0xffe1 && ks <= 0xffee) return 0; /* bare modifier */
    if (!ks) return 0;
    XEv ev; memset(&ev, 0, sizeof ev);
    ev.type = X_KEY; ev.code = code; ev.mod = mods_of(st); ev.synthetic = (e[0] & 0x80) != 0;
    if ((ev.mod & M_CTRL) && ks >= 'A' && ks <= 'Z') ks += 32;
    ev.key = (int)ks;
    if (!(ev.mod & (M_CTRL | M_ALT))) {
        uint32_t cp = 0;
        if ((ks >= 0x20 && ks <= 0x7e) || (ks >= 0xa0 && ks <= 0xff)) cp = ks;
        else if ((ks & 0xff000000u) == 0x01000000u) cp = ks & 0xffffff;
        else if (ks == 0x20ac) cp = 0x20ac;
        if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0;  
        if (cp) ev.text[utf8_enc(cp, ev.text)] = 0;
    }
    *out = ev; return 1;
}
static void flush_release(void) { if (X.rel_pending) { push(&X.rel); X.rel_pending = 0; } }
static void process_raw(const uint8_t *e) {
    int code = e[0] & 0x7f; XEv ev; memset(&ev, 0, sizeof ev);
    ev.synthetic = (e[0] & 0x80) != 0;               
    /* X reports auto-repeat as KeyRelease + KeyPress with the same keycode and timestamp: drop the release, keep the press */
    if (X.rel_pending) { if (code == 2 && e[1] == X.rel_code && g32(e, 4) == X.rel_time) X.rel_pending = 0; else flush_release(); }
    if (ev.synthetic && !x_allow_synthetic && (code == 2 || code == 3 || code == 4 || code == 5 || code == 6)) return;
    switch (code) {
    case 2: { XEv k; if (key_ev(e, &k)) push(&k); break; }
    case 3:
        if (x_keyup) { XEv k; if (key_ev(e, &k)) { k.type = X_KEY_UP; k.text[0] = 0; X.rel = k; X.rel_pending = 1; X.rel_code = e[1]; X.rel_time = g32(e, 4); } }
        break;
    case 9: case 10:
        if (x_focus && e[8] != 1 && e[8] != 2 && e[1] < 5) { ev.type = code == 9 ? X_FOCUS_IN : X_FOCUS_OUT; push(&ev); }
        break;
    case 4: case 5: {
        int b = e[1]; ev.x = (int16_t)g16(e, 24); ev.y = (int16_t)g16(e, 26); ev.mod = mods_of(g16(e, 28));
        if (b == 4 || b == 5) { if (code == 4) { ev.type = X_WHEEL; ev.dy = b == 4 ? 1 : -1; push(&ev); } break; }
        ev.type = code == 4 ? X_BTN_DOWN : X_BTN_UP; ev.button = b; push(&ev); break; }
    case 6: ev.type = X_MOTION; ev.x = (int16_t)g16(e, 24); ev.y = (int16_t)g16(e, 26); ev.mod = mods_of(g16(e, 28)); push(&ev); break;
    case 8: ev.type = X_LEAVE; push(&ev); break;
    case 12: if (g16(e, 16) == 0) { ev.type = X_EXPOSE; push(&ev); } break;
    case 22: {
        int w = (int)g16(e, 20), h = (int)g16(e, 22);
        if (w > 0 && h > 0 && w <= MAXDIM && h <= MAXDIM && (size_t)w * (size_t)h <= MAXPIX && (w != x_w || h != x_h)) {
            uint32_t *nf = calloc((size_t)w * (size_t)h, 4);
            if (!nf) break;                                  
            free(x_fb); x_fb = nf; x_w = w; x_h = h;
            ev.type = X_RESIZE; ev.w = w; ev.h = h; push(&ev);
        }
        break; }
    case 33:
        if (g32(e, 8) == X.a_proto && g32(e, 12) == X.a_del) { ev.type = X_CLOSE; push(&ev); }
        break;
    case 30: sel_request(e); break;
    case 29: X.clip_own = 0; break;
    case 31:       
        if (x_trust_selection || (X.sel_wait && g32(e, 8) == X.win && g32(e, 12) == X.a_clip && (g32(e, 20) == X.a_prop || g32(e, 20) == 0))) {
            X.sel_done = 1; X.sel_prop = g32(e, 20);
        }
        break;
    default: break;
    }
}
int x_wait(XEv *out, int timeout_ms) {
    long end = now_ms() + timeout_ms;
    for (;;) {
        if (X.qh != X.qt) { *out = X.q[X.qh]; X.qh = (X.qh + 1) % QN; return 1; }
        if (X.rel_pending && !poll_readable(2)) { flush_release(); continue; }   /* no auto-repeat press followed: a real release */
        long left = end - now_ms(); if (left < 0) left = 0;
        if (!poll_readable((int)left)) return 0;
        uint8_t m[32]; read_msg(m);
        if (m[0] >= 2) process_raw(m);
    }
}
