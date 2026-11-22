/*
 * IVS Studio - editor + interpreter for interactive video/image scripts
 * Target: TI-Nspire CX with Ndless 4.x  (UNTESTED - written without access to the SDK)
 *
 * Video: VP8 in .ivf files, decoded with the same libvpx that nvid uses, but one frame
 *        per main-loop tick so pause / resume / stop and key handling keep working.
 * Images: PNG / JPEG / BMP through stb_image.h
 */
#include <os.h>
#include <libndls.h>
#include <SDL/SDL.h>
#include <nSDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_FAILURE_STRINGS
#include "stb_image.h"

#define VPX_CODEC_DISABLE_COMPAT 1
#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"

#define W 320
#define H 240
#define MAXTXT 12000
#define COLS 40
#define ROWS 26
#define FRAME_SZ (256 * 1024)
#define RGB(r, g, b) ((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
#define C_BLACK RGB(0, 0, 0)
#define C_WHITE RGB(255, 255, 255)
#define C_NAVY RGB(10, 20, 50)
#define C_BLUE RGB(40, 90, 200)
#define C_DARK RGB(25, 25, 35)

static SDL_Surface *screen;
static nSDL_Font *fw, *fy;      /* white, yellow */
static uint16_t *vframe;        /* last decoded video frame, RGB565, W*H */

/* ------------------------------------------------------------------ drawing */
static void rect(int x, int y, int w, int h, uint16_t c) {
    SDL_Rect r; r.x = x; r.y = y; r.w = w; r.h = h;
    SDL_FillRect(screen, &r, c);
}
static void txt(nSDL_Font *f, int x, int y, const char *s) { nSDL_DrawString(screen, f, x, y, "%s", s); }
static void flip(void) { SDL_Flip(screen); }

/* --------------------------------------------------------------------- keys */
typedef struct { const char *name; t_key k; char lo, hi; } KeyDef;
static KeyDef keys[80];
static int nkeys;
static int K_SHIFT, K_CTRL, K_ESC, K_ENTER, K_CLICK, K_DEL, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_MENU;
static int heldk[80];

static int addk(const char *n, t_key k, char lo, char hi) {
    keys[nkeys].name = n; keys[nkeys].k = k; keys[nkeys].lo = lo; keys[nkeys].hi = hi;
    return nkeys++;
}
#define LET(U, l) addk(#l, KEY_NSPIRE_##U, #l[0], #U[0])
#define DIG(n) addk(#n, KEY_NSPIRE_##n, #n[0], #n[0])

static void init_keys(void) {
    LET(A,a); LET(B,b); LET(C,c); LET(D,d); LET(E,e); LET(F,f); LET(G,g); LET(H,h); LET(I,i);
    LET(J,j); LET(K,k); LET(L,l); LET(M,m); LET(N,n); LET(O,o); LET(P,p); LET(Q,q); LET(R,r);
    LET(S,s); LET(T,t); LET(U,u); LET(V,v); LET(W,w); LET(X,x); LET(Y,y); LET(Z,z);
    DIG(0); DIG(1); DIG(2); DIG(3); DIG(4); DIG(5); DIG(6); DIG(7); DIG(8); DIG(9);
    addk("space", KEY_NSPIRE_SPACE, ' ', ' ');
    addk("period", KEY_NSPIRE_PERIOD, '.', ':');
    addk("comma", KEY_NSPIRE_COMMA, ',', ';');
    addk("minus", KEY_NSPIRE_MINUS, '-', '_');
    addk("plus", KEY_NSPIRE_PLUS, '+', '+');
    addk("divide", KEY_NSPIRE_DIVIDE, '/', '\\');
    addk("lp", KEY_NSPIRE_LP, '(', '[');
    addk("rp", KEY_NSPIRE_RP, ')', ']');
    K_ENTER = addk("enter", KEY_NSPIRE_ENTER, '\n', '\n');
    K_CLICK = addk("click", KEY_NSPIRE_CLICK, 0, 0);
    K_DEL = addk("del", KEY_NSPIRE_DEL, 0, 0);
    K_ESC = addk("esc", KEY_NSPIRE_ESC, 0, 0);
    addk("tab", KEY_NSPIRE_TAB, 0, 0);
    K_MENU = addk("menu", KEY_NSPIRE_MENU, 0, 0);
    K_UP = addk("up", KEY_NSPIRE_UP, 0, 0);
    K_DOWN = addk("down", KEY_NSPIRE_DOWN, 0, 0);
    K_LEFT = addk("left", KEY_NSPIRE_LEFT, 0, 0);
    K_RIGHT = addk("right", KEY_NSPIRE_RIGHT, 0, 0);
    K_SHIFT = addk("shift", KEY_NSPIRE_SHIFT, 0, 0);
    K_CTRL = addk("ctrl", KEY_NSPIRE_CTRL, 0, 0);
}

static int find_key(const char *n) {
    int i;
    for (i = 0; i < nkeys; i++) if (!strcmp(keys[i].name, n)) return i;
    return -1;
}
static char key_char(int i) {
    return isKeyPressed(keys[K_SHIFT].k) ? keys[i].hi : keys[i].lo;
}
static void release_all(void) { wait_no_key_pressed(); memset(heldk, 0, sizeof heldk); }

/* newly pressed key (with auto-repeat if asked); -1 if none. Modifiers never generate events. */
static int key_event(int repeat) {
    int i, ev = -1;
    for (i = 0; i < nkeys; i++) {
        if (i == K_SHIFT || i == K_CTRL) continue;
        if (isKeyPressed(keys[i].k)) {
            if (ev >= 0 && heldk[i] == 0) continue;   /* handle on a later call */
            heldk[i]++;
            if (ev < 0 && (heldk[i] == 1 || (repeat && heldk[i] > 12 && heldk[i] % 3 == 0))) ev = i;
        } else heldk[i] = 0;
    }
    return ev;
}

/* ---------------------------------------------------------------- dialogs */
static void message(const char *s) {
    char tmp[120]; int lines = 1, i, n = 0, y;
    for (i = 0; s[i]; i++) if (s[i] == '\n') lines++;
    int bh = 24 + lines * 10, by = (H - bh) / 2;
    rect(28, by - 2, 264, bh + 4, C_WHITE); rect(30, by, 260, bh, C_DARK);
    y = by + 6;
    for (i = 0;; i++) {
        if (s[i] == '\n' || !s[i]) { tmp[n] = 0; txt(fw, 36, y, tmp); y += 10; n = 0; if (!s[i]) break; }
        else if (n < 118) tmp[n++] = s[i];
    }
    txt(fy, 36, by + bh - 12, "press any key");
    flip(); release_all();
    while (key_event(0) < 0) msleep(25);
    release_all();
}
static void toast(const char *s) {
    rect(78, 108, 164, 24, C_WHITE); rect(80, 110, 160, 20, C_DARK);
    txt(fw, 88, 116, s); flip(); msleep(600);
}

static int chooser(const char *title, const char **it, int n) {
    int sel = 0, i, bw = 180, bh = 24 + n * 12, bx = (W - bw) / 2, by = (H - bh) / 2;
    release_all();
    for (;;) {
        rect(bx - 2, by - 2, bw + 4, bh + 4, C_WHITE); rect(bx, by, bw, bh, C_DARK);
        txt(fy, bx + 6, by + 4, title);
        for (i = 0; i < n; i++) {
            if (i == sel) rect(bx + 4, by + 18 + i * 12, bw - 8, 11, C_BLUE);
            txt(fw, bx + 8, by + 20 + i * 12, it[i]);
        }
        flip();
        int e = key_event(1);
        if (e == K_UP && sel > 0) sel--;
        else if (e == K_DOWN && sel < n - 1) sel++;
        else if (e == K_ENTER || e == K_CLICK) { release_all(); return sel; }
        else if (e == K_ESC || e == K_MENU) { release_all(); return -1; }
        msleep(25);
    }
}

static int prompt(const char *title, char *out, int max) {
    int n = strlen(out);
    release_all();
    for (;;) {
        char shown[48]; const char *s = n > 34 ? out + n - 34 : out;
        snprintf(shown, sizeof shown, "%s_", s);
        rect(18, 88, 284, 64, C_WHITE); rect(20, 90, 280, 60, C_DARK);
        txt(fy, 26, 96, title);
        rect(26, 112, 268, 14, C_NAVY); txt(fw, 30, 115, shown);
        txt(fw, 26, 134, "enter = ok    esc = cancel");
        flip();
        int e = key_event(1);
        if (e >= 0) {
            if (e == K_ENTER || e == K_CLICK) { release_all(); return n > 0; }
            if (e == K_ESC) { release_all(); return 0; }
            if (e == K_DEL) { if (n) out[--n] = 0; }
            else { char c = key_char(e); if (c && c != '\n' && n < max - 1) { out[n++] = c; out[n] = 0; } }
        }
        msleep(25);
    }
}

/* -------------------------------------------------------------- reference */
static const char *ref_lines[] = {
    "IVS STUDIO REFERENCE",
    "",
    "[video] file name of a .ivf video",
    "[image] file name of png/jpg/bmp",
    "[key]   a key name, see KEYS below",
    "[folder] folder with your media",
    "",
    "-- VIDEO --",
    "vidpl [video]  play video from start",
    "vidp           pause video",
    "vidup          unpause video",
    "vidr           stop and remove video",
    "",
    "-- IMAGES --",
    "imgs [image]   show image",
    "imgr           remove image",
    "",
    "-- LOGIC & FLOW --",
    "loc [folder]   folder holding this",
    "  project's videos/images. Must be",
    "  at the beginning of the script.",
    "  e.g.  loc /myprojects/project1",
    "onst (",
    "  commands",
    ")              runs once on start",
    "when [key] (",
    "  commands",
    ")              runs each time that key",
    "  is pressed. Checked constantly,",
    "  no loop needed.",
    "exit           exits the program",
    "",
    "Put the ( on the same line as",
    "onst/when, and ) on its own line.",
    "Comments start with //",
    "",
    "-- KEYS --",
    "a-z  0-9  space  enter  esc  tab",
    "menu  up  down  left  right  del",
    "click  ctrl  shift  period  comma",
    "minus  plus  divide  lp  rp",
    "",
    "-- NOTES --",
    "Esc stops a running program unless",
    "the script defines  when esc.",
    "Ctrl+Esc always stops it.",
    "Files on the calculator end in .tns",
    "(myvideo.ivf.tns). Write just",
    "myvideo.ivf in the script.",
    "Video should be 320x240 or smaller.",
    "Keep images under ~640x480.",
    "",
    "-- TYPING --",
    "shift+letter = capital",
    "shift+minus = _   shift+period = :",
    "shift+comma = ;   divide key = /",
    "Menu > Symbols inserts any of them.",
    0
};
static void reference(void) {
    int n = 0, top = 0, i;
    while (ref_lines[n]) n++;
    release_all();
    for (;;) {
        rect(0, 0, W, H, C_NAVY); rect(0, 0, W, 12, C_BLUE);
        txt(fw, 4, 2, "Reference   up/down scroll   esc back");
        for (i = 0; i < ROWS && top + i < n; i++) txt(fw, 4, 16 + i * 8, ref_lines[top + i]);
        flip();
        int e = key_event(1);
        if (e == K_UP && top > 0) top--;
        else if (e == K_DOWN && top < n - ROWS) top++;
        else if (e == K_ESC || e == K_MENU) { release_all(); return; }
        msleep(25);
    }
}

/* ---------------------------------------------------------------- parser */
enum { C_VIDPL, C_VIDP, C_VIDUP, C_VIDR, C_IMGS, C_IMGR, C_EXIT };
static const struct { const char *n; int id, arg; } cmdtab[] = {
    {"vidpl", C_VIDPL, 1}, {"vidp", C_VIDP, 0}, {"vidup", C_VIDUP, 0}, {"vidr", C_VIDR, 0},
    {"imgs", C_IMGS, 1}, {"imgr", C_IMGR, 0}, {"exit", C_EXIT, 0}
};
#define MAXCMD 256
#define MAXBLK 48
typedef struct { uint8_t id; char arg[48]; } Cmd;
typedef struct { int key, start, count; } Block;     /* key -1 = onst */
static Cmd cmds[MAXCMD]; static int ncmds;
static Block blocks[MAXBLK]; static int nblocks;
static Block *curb;
static char folder[96], perr[80];

static char *trim(char *s) {
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    return s;
}
static void lowerstr(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32; }
static int fail(int ln, const char *m) { snprintf(perr, sizeof perr, "Line %d: %s", ln, m); return 0; }

static int add_cmd(char *s, int ln) {
    char *arg = s; int i;
    while (*arg && *arg != ' ' && *arg != '\t') arg++;
    if (*arg) { *arg++ = 0; while (*arg == ' ' || *arg == '\t') arg++; }
    lowerstr(s);
    for (i = 0; i < 7; i++) {
        if (strcmp(s, cmdtab[i].n)) continue;
        if (cmdtab[i].arg && !*arg) return fail(ln, "needs a file name");
        if (ncmds >= MAXCMD) return fail(ln, "script too long");
        if (strlen(arg) >= sizeof cmds[0].arg) return fail(ln, "file name too long");
        cmds[ncmds].id = cmdtab[i].id;
        strcpy(cmds[ncmds].arg, arg);
        ncmds++; curb->count++;
        return 1;
    }
    return fail(ln, "unknown command");
}
static int body(char *t, int ln) {
    char *close = strchr(t, ')'), *s = t;
    if (close) *close = 0;
    while (s) {
        char *semi = strchr(s, ';');
        if (semi) *semi = 0;
        s = trim(s);
        if (*s && !add_cmd(s, ln)) return 0;
        s = semi ? semi + 1 : NULL;
    }
    if (close) curb = NULL;
    return 1;
}
static int open_block(int key, char *rest, int ln) {
    if (*rest != '(') return fail(ln, "expected ( after header");
    if (nblocks >= MAXBLK) return fail(ln, "too many blocks");
    curb = &blocks[nblocks++];
    curb->key = key; curb->start = ncmds; curb->count = 0;
    return body(rest + 1, ln);
}
static int parse(const char *src) {
    int ln = 0;
    nblocks = ncmds = 0; folder[0] = 0; curb = NULL; perr[0] = 0;
    while (*src) {
        char line[128], word[16], *t, *c; int n = 0, k = 0;
        while (*src && *src != '\n') { if (n < 127 && *src != '\r') line[n++] = *src; src++; }
        if (*src) src++;
        line[n] = 0; ln++;
        if ((c = strstr(line, "//"))) *c = 0;
        t = trim(line);
        if (!*t) continue;
        if (curb) { if (!body(t, ln)) return 0; continue; }
        while (*t && *t != ' ' && *t != '\t' && *t != '(' && k < 15) word[k++] = *t++;
        word[k] = 0; lowerstr(word); t = trim(t);
        if (!strcmp(word, "loc")) {
            int l;
            snprintf(folder, sizeof folder, "%s", t);
            l = strlen(folder);
            while (l > 1 && folder[l - 1] == '/') folder[--l] = 0;
        } else if (!strcmp(word, "onst")) {
            if (!open_block(-1, t, ln)) return 0;
        } else if (!strcmp(word, "when")) {
            char kn[16]; int key;
            k = 0;
            while (*t && *t != ' ' && *t != '\t' && *t != '(' && k < 15) kn[k++] = *t++;
            kn[k] = 0; lowerstr(kn); t = trim(t);
            key = find_key(kn);
            if (key < 0) return fail(ln, "unknown key");
            if (!open_block(key, t, ln)) return 0;
        } else return fail(ln, "unknown statement");
    }
    if (curb) return fail(ln, "missing )");
    return 1;
}

/* ------------------------------------------------------------------ media */
static FILE *open_media(const char *name) {
    char base[160], p[260]; int l; FILE *f;
    if (!strncmp(folder, "/documents", 10)) snprintf(base, sizeof base, "%s", folder);
    else if (folder[0] == '/') snprintf(base, sizeof base, "/documents%s", folder);
    else if (folder[0]) snprintf(base, sizeof base, "/documents/%s", folder);
    else strcpy(base, "/documents");
    l = strlen(base);
    while (l > 1 && base[l - 1] == '/') base[--l] = 0;
    snprintf(p, sizeof p, "%s/%s", base, name);
    f = fopen(p, "rb");
    if (!f) { strcat(p, ".tns"); f = fopen(p, "rb"); }
    return f;
}

/* image layer */
static struct { uint8_t *px; int w, h; } img;
static void img_free(void) { if (img.px) { free(img.px); img.px = NULL; } }

static int img_show(const char *name) {
    FILE *f = open_media(name); long n; uint8_t *data, *px; int w, h, c;
    if (!f) return 0;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    data = malloc(n);
    if (!data) { fclose(f); return 0; }
    if (fread(data, 1, n, f) != (size_t)n) { free(data); fclose(f); return 0; }
    fclose(f);
    px = stbi_load_from_memory(data, (int)n, &w, &h, &c, 4);
    free(data);
    if (!px) return 0;
    if (w > W || h > H) {                       /* nearest-neighbour downscale to fit */
        int nw, nh, x, y; uint8_t *np;
        if (w * H > h * W) { nw = W; nh = h * W / w; } else { nh = H; nw = w * H / h; }
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
        np = malloc(nw * nh * 4);
        if (!np) { free(px); return 0; }
        for (y = 0; y < nh; y++) for (x = 0; x < nw; x++)
            memcpy(np + (y * nw + x) * 4, px + ((y * h / nh) * w + (x * w / nw)) * 4, 4);
        free(px); px = np; w = nw; h = nh;
    }
    img_free(); img.px = px; img.w = w; img.h = h;
    return 1;
}

/* video layer */
static struct {
    int running, active, paused;
    FILE *f; vpx_codec_ctx_t codec; uint8_t *frame;
} vid;

static void video_stop(void) {
    if (vid.running) { vpx_codec_destroy(&vid.codec); fclose(vid.f); free(vid.frame); }
    vid.running = 0; vid.paused = 0;
}
static int video_start(const char *name) {
    uint8_t hdr[32];
    vid.f = open_media(name);
    if (!vid.f) return 0;
    if (fread(hdr, 1, 32, vid.f) != 32 || hdr[0] != 'D' || hdr[1] != 'K' || hdr[2] != 'I' || hdr[3] != 'F') {
        fclose(vid.f); return 0;
    }
    vid.frame = malloc(FRAME_SZ);
    if (!vid.frame || vpx_codec_dec_init(&vid.codec, vpx_codec_vp8_dx(), NULL, 0)) {
        free(vid.frame); fclose(vid.f); return 0;
    }
    vid.running = 1; vid.active = 1; vid.paused = 0;
    return 1;
}
static uint8_t clamp8(int x) { return x < 0 ? 0 : (x > 255 ? 255 : x); }

static void convert(vpx_image_t *im) {
    int dw = im->d_w < W ? im->d_w : W, dh = im->d_h < H ? im->d_h : H;
    int ox = (W - dw) / 2, oy = (H - dh) / 2, x, y;
    uint8_t *yb = im->planes[0], *ub = im->planes[1], *vb = im->planes[2];
    for (y = 0; y < dh; y++) {
        uint16_t *row = vframe + (oy + y) * W + ox;
        for (x = 0; x < dw; x++) {
            int c = yb[x] - 16, d = ub[x >> 1] - 128, e = vb[x >> 1] - 128;
            uint8_t r = clamp8((298 * c + 409 * e + 128) >> 8);
            uint8_t g = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
            uint8_t b = clamp8((298 * c + 516 * d + 128) >> 8);
            row[x] = RGB(r, g, b);
        }
        yb += im->stride[0];
        if (y & 1) { ub += im->stride[1]; vb += im->stride[2]; }
    }
}
/* decode ONE frame. Returns 1 if the picture changed. End of file keeps the last frame on screen. */
static int video_step(void) {
    uint8_t fh[12]; unsigned sz; vpx_codec_iter_t it = NULL; vpx_image_t *im; int got = 0;
    if (!vid.running || vid.paused) return 0;
    if (fread(fh, 1, 12, vid.f) != 12) { video_stop(); return 0; }
    sz = fh[0] | (fh[1] << 8) | (fh[2] << 16) | ((unsigned)fh[3] << 24);
    if (sz > FRAME_SZ || fread(vid.frame, 1, sz, vid.f) != sz) { video_stop(); return 0; }
    if (vpx_codec_decode(&vid.codec, vid.frame, sz, NULL, 0)) { video_stop(); return 0; }
    while ((im = vpx_codec_get_frame(&vid.codec, &it))) { convert(im); got = 1; }
    return got;
}

static void render(void) {
    uint16_t *dst = (uint16_t *)screen->pixels; int pp = screen->pitch / 2, x, y;
    for (y = 0; y < H; y++) memcpy(dst + y * pp, vframe + y * W, W * 2);
    if (img.px) {
        int ox = (W - img.w) / 2, oy = (H - img.h) / 2;
        for (y = 0; y < img.h; y++) {
            uint16_t *d = dst + (oy + y) * pp + ox; uint8_t *s = img.px + y * img.w * 4;
            for (x = 0; x < img.w; x++, s += 4) {
                int a = s[3];
                if (a == 255) d[x] = RGB(s[0], s[1], s[2]);
                else if (a) {
                    uint16_t o = d[x]; int orr = (o >> 11) << 3, og = ((o >> 5) & 63) << 2, ob = (o & 31) << 3;
                    d[x] = RGB((s[0] * a + orr * (255 - a)) / 255, (s[1] * a + og * (255 - a)) / 255,
                               (s[2] * a + ob * (255 - a)) / 255);
                }
            }
        }
    }
    SDL_Flip(screen);
}

/* -------------------------------------------------------------- interpreter */
static int need_render;

static int rt_error(const char *what, const char *name) {
    char m[120]; snprintf(m, sizeof m, "%s:\n%s", what, name); message(m); return -1;
}
/* 0 = ok, 1 = exit requested, -1 = error */
static int exec_block(Block *b) {
    int i;
    for (i = b->start; i < b->start + b->count; i++) {
        Cmd *c = &cmds[i];
        switch (c->id) {
        case C_VIDPL:
            video_stop(); memset(vframe, 0, W * H * 2);
            if (!video_start(c->arg)) return rt_error("Can't play video", c->arg);
            video_step(); need_render = 1; break;
        case C_VIDP: if (vid.running) vid.paused = 1; break;
        case C_VIDUP: vid.paused = 0; break;
        case C_VIDR: video_stop(); vid.active = 0; memset(vframe, 0, W * H * 2); need_render = 1; break;
        case C_IMGS: if (!img_show(c->arg)) return rt_error("Can't load image", c->arg); need_render = 1; break;
        case C_IMGR: img_free(); need_render = 1; break;
        case C_EXIT: return 1;
        }
    }
    return 0;
}

static void run(const char *src) {
    int i, status = 0, has_esc = 0; uint8_t pk[80], cs[80];
    if (!parse(src)) { message(perr); return; }
    memset(vframe, 0, W * H * 2); img_free();
    vid.running = vid.active = vid.paused = 0; need_render = 1;
    for (i = 0; i < nblocks; i++) if (blocks[i].key == K_ESC) has_esc = 1;
    release_all();
    for (i = 0; i < nblocks && !status; i++) if (blocks[i].key < 0) status = exec_block(&blocks[i]);
    for (i = 0; i < nkeys; i++) pk[i] = isKeyPressed(keys[i].k) ? 1 : 0;
    render(); need_render = 0;
    while (status == 0) {
        if (video_step()) need_render = 1;
        for (i = 0; i < nkeys; i++) cs[i] = isKeyPressed(keys[i].k) ? 1 : 0;
        if (cs[K_ESC] && cs[K_CTRL]) break;
        for (i = 0; i < nblocks && status == 0; i++) {
            int k = blocks[i].key;
            if (k >= 0 && cs[k] && !pk[k]) status = exec_block(&blocks[i]);
        }
        if (status == 0 && !has_esc && cs[K_ESC] && !pk[K_ESC]) break;
        memcpy(pk, cs, sizeof pk);
        if (need_render) { render(); need_render = 0; }
        if (!(vid.running && !vid.paused)) msleep(15);
    }
    video_stop(); img_free(); vid.active = 0;
    release_all();
}

/* ------------------------------------------------------------------ editor */
static char *tbuf; static int tlen, tcur, tmod, ttop;
static char tpath[160];

static void ed_ins(char c) {
    if (tlen >= MAXTXT - 1) return;
    memmove(tbuf + tcur + 1, tbuf + tcur, tlen - tcur + 1);
    tbuf[tcur++] = c; tlen++; tmod = 1;
}
static void ed_del(void) {
    if (!tcur) return;
    memmove(tbuf + tcur - 1, tbuf + tcur, tlen - tcur + 1);
    tcur--; tlen--; tmod = 1;
}
static int line_start(int p) { while (p > 0 && tbuf[p - 1] != '\n') p--; return p; }
static int line_end(int p) { while (p < tlen && tbuf[p] != '\n') p++; return p; }

static void ed_up(void) {
    int ls = line_start(tcur), col = tcur - ls, pls, plen;
    if (!ls) return;
    pls = line_start(ls - 1); plen = ls - 1 - pls;
    tcur = pls + (col < plen ? col : plen);
}
static void ed_down(void) {
    int le = line_end(tcur), col = tcur - line_start(tcur), nle, nls;
    if (le >= tlen) return;
    nls = le + 1; nle = line_end(nls);
    tcur = nls + (col < nle - nls ? col : nle - nls);
}
static void ed_draw(void) {
    int line = 0, col = 0, i, r, p = 0, ln = 0, hx; char tmp[COLS + 1], st[48];
    for (i = 0; i < tcur; i++) { if (tbuf[i] == '\n') { line++; col = 0; } else col++; }
    if (line < ttop) ttop = line;
    if (line >= ttop + ROWS) ttop = line - ROWS + 1;
    hx = col > COLS - 2 ? col - (COLS - 2) : 0;
    rect(0, 0, W, H, C_NAVY); rect(0, 0, W, 12, C_BLUE);
    snprintf(st, sizeof st, "%.34s%s", tpath, tmod ? " *" : "");
    txt(fw, 4, 2, st);
    while (ln < ttop && p < tlen) if (tbuf[p++] == '\n') ln++;
    for (r = 0; r < ROWS; r++) {
        int e = p, len, n = 0;
        while (e < tlen && tbuf[e] != '\n') e++;
        len = e - p;
        for (i = hx; i < len && n < COLS; i++) tmp[n++] = tbuf[p + i];
        tmp[n] = 0;
        txt(fw, 0, 15 + r * 8, tmp);
        if (ttop + r == line) rect((col - hx) * 8, 15 + r * 8, 2, 8, RGB(255, 220, 0));
        if (e >= tlen) break;
        p = e + 1;
    }
    rect(0, H - 11, W, 11, C_BLUE);
    snprintf(st, sizeof st, "Ln %d Col %d    menu = Menu", line + 1, col + 1);
    txt(fw, 4, H - 9, st);
}

static int save_file(void) {
    FILE *f = fopen(tpath, "wb");
    if (!f) { message("Could not save file"); return 0; }
    fwrite(tbuf, 1, tlen, f); fclose(f);
    tmod = 0; toast("Saved");
    return 1;
}
static int load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    tlen = fread(tbuf, 1, MAXTXT - 1, f); tbuf[tlen] = 0; fclose(f);
    snprintf(tpath, sizeof tpath, "%s", path);
    tcur = 0; ttop = 0; tmod = 0;
    return 1;
}
/* returns 1 if the editor should close */
static int editor_menu(void) {
    static const char *items[] = {"Save", "Run", "Reference", "Symbols", "Close file"};
    static const char *syms[] = {"(", ")", "/", ".", ",", "_", "-", ":", ";", "\\"};
    static const char *yn[] = {"Save and close", "Close without saving", "Cancel"};
    int c = chooser("Menu", items, 5);
    if (c == 0) save_file();
    else if (c == 1) run(tbuf);
    else if (c == 2) reference();
    else if (c == 3) { int s = chooser("Insert symbol", syms, 10); if (s >= 0) ed_ins(syms[s][0]); }
    else if (c == 4) {
        if (!tmod) return 1;
        c = chooser("Unsaved changes", yn, 3);
        if (c == 0) return save_file();
        if (c == 1) return 1;
    }
    return 0;
}
static void editor(void) {
    int redraw = 1;
    ttop = 0; release_all();
    for (;;) {
        int e;
        if (redraw) { ed_draw(); flip(); redraw = 0; }
        e = key_event(1);
        if (e >= 0) {
            redraw = 1;
            if (e == K_MENU || e == K_ESC) { if (editor_menu()) return; }
            else if (e == K_LEFT) { if (tcur) tcur--; }
            else if (e == K_RIGHT) { if (tcur < tlen) tcur++; }
            else if (e == K_UP) ed_up();
            else if (e == K_DOWN) ed_down();
            else if (e == K_DEL) ed_del();
            else { char c = key_char(e); if (c) ed_ins(c); else redraw = 0; }
        }
        msleep(20);
    }
}

/* ------------------------------------------------------------- start screen */
static int ends_with(const char *s, const char *suf) {
    int a = strlen(s), b = strlen(suf);
    return a >= b && !strcmp(s + a - b, suf);
}
static void name_to_path(const char *name, char *out, int max) {
    char tmp[120];
    if (name[0] == '/') snprintf(tmp, sizeof tmp, "%s", name);
    else snprintf(tmp, sizeof tmp, "/documents/%s", name);
    if (ends_with(tmp, ".ivs.tns")) snprintf(out, max, "%s", tmp);
    else if (ends_with(tmp, ".ivs")) snprintf(out, max, "%s.tns", tmp);
    else snprintf(out, max, "%s.ivs.tns", tmp);
}
static int start_screen(void) {
    static const char *lbl[] = {"Open file", "Create file"};
    int sel = 0, i;
    release_all();
    for (;;) {
        rect(0, 0, W, H, C_NAVY);
        txt(fy, 110, 40, "IVS STUDIO");
        txt(fw, 52, 56, "interactive video & image scripts");
        for (i = 0; i < 2; i++) {
            rect(80, 100 + i * 48, 160, 34, i == sel ? C_WHITE : C_BLUE);
            rect(82, 102 + i * 48, 156, 30, i == sel ? C_BLUE : C_DARK);
            txt(fw, 160 - (int)strlen(lbl[i]) * 4, 114 + i * 48, lbl[i]);
        }
        txt(fw, 60, 215, "up/down + enter      esc = quit");
        flip();
        int e = key_event(0);
        if (e == K_UP || e == K_DOWN) sel = 1 - sel;
        else if (e == K_ENTER || e == K_CLICK) return sel;
        else if (e == K_ESC) return -1;
        msleep(25);
    }
}

int main(int argc, char **argv) {
    static char lastname[48];
    if (!has_colors) { show_msgbox("IVS Studio", "This app needs a TI-Nspire CX."); return 0; }
    tbuf = calloc(MAXTXT, 1); vframe = calloc(W * H, 2);
    if (!tbuf || !vframe) { show_msgbox("IVS Studio", "Out of memory."); return 1; }
    init_keys();
    SDL_Init(SDL_INIT_VIDEO);
    screen = SDL_SetVideoMode(W, H, 16, SDL_SWSURFACE);
    fw = nSDL_LoadFont(NSDL_FONT_VGA, 255, 255, 255);
    fy = nSDL_LoadFont(NSDL_FONT_VGA, 255, 220, 0);

    if (argc >= 2) {                         /* opened from the file browser */
        if (load_file(argv[1])) editor(); else message("Could not open file");
    } else {
        cfg_register_fileext("ivs", "ivstudio");
        for (;;) {
            int c = start_screen();
            if (c < 0) break;
            if (c == 0) {                    /* open */
                if (prompt("Open file (name):", lastname, sizeof lastname)) {
                    char p[160]; name_to_path(lastname, p, sizeof p);
                    if (load_file(p)) editor();
                    else { char m[200]; snprintf(m, sizeof m, "File not found:\n%.40s", p); message(m); }
                }
            } else {                         /* create */
                char name[48] = "";
                if (prompt("New file name:", name, sizeof name)) {
                    char p[160]; FILE *f; name_to_path(name, p, sizeof p);
                    if ((f = fopen(p, "rb"))) { fclose(f); toast("Exists - opening it"); load_file(p); }
                    else { tlen = tcur = ttop = 0; tbuf[0] = 0; tmod = 1; snprintf(tpath, sizeof tpath, "%s", p); }
                    snprintf(lastname, sizeof lastname, "%s", name);
                    editor();
                }
            }
        }
    }
    nSDL_FreeFont(fw); nSDL_FreeFont(fy);
    SDL_Quit();
    wait_no_key_pressed();
    return 0;
}
