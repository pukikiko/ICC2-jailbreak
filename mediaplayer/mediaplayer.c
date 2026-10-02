/*
 * mediaplayer - a file browser and video player for the unit's panel.
 *
 * the playback half is the ffdec/ffplay picture path without the sdl shim: libavformat
 * demuxes, libavcodec decodes, and a table driven integer yuv->rgb565 converter (the same
 * shape as the serial players') scales each frame straight into the uncached panel, so no
 * swscale and no upload copy sit between the decoder and the screen. the ui half borrows the
 * homebrew menu's look: the hmi's own arial (menu.c/menufont.c), rounded cards, a dark
 * gradient field, drawing only what changed.
 *
 * the browser starts at /fs/usb0 (or a directory given on the command line), lists
 * directories first and plays a tapped file, and carries an EXIT button so the caller
 * (the homebrew menu, the launcher, or mediaplayer.sh) gets the panel back.
 *
 * the player has a touch control bar: back to the browser, -10s, play/pause, +10s and a
 * progress track that seeks where it is tapped. it hides itself after a few seconds and
 * comes back on any touch. the knob moves the browser selection and seeks in the player.
 *
 * no audio: this build has no audio out anywhere (ffplay's SDL_OpenAudioDevice always
 * fails), so only the video stream is played.
 *
 * usage: mediaplayer [path] [--lowres N] [--loop] [--stats]
 *   path       a directory to browse, or a file to play once then browse (default /fs/usb0)
 *   --lowres N decode at 1/2^N size and scale back up, for a cpu that cannot keep up
 *   --loop     restart the file at the end instead of returning to the browser
 *   --stats    print decode/draw timings to the console once a second
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#include "qnx.h"
#include "fb.h"
#include "menu.h"
#include "menufont.h"
#include "input.h"

/* qnx 6.4's struct dirent as documented (sys/dirent.h): two 32 bit ino halves, two 32 bit
 * offset halves (the 32 bit _FILE_OFFSET_BITS default), then reclen/namelen and the name at
 * offset 20. readdir hands back a pointer into the DIR's buffer, so only the name is read. */
typedef struct IccDir DIR;
struct dirent {
    unsigned int d_ino, d_ino_hi;
    unsigned int d_offset, d_offset_hi;
    short d_reclen, d_namelen;
    char d_name[1];
};
DIR *opendir(const char *path);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);

#define SCREEN_W    800
#define SCREEN_H    480
#define PATH_MAX    512
#define NAME_MAX    256
#define MAX_ENTRIES 256

/* ---- shared ui palette ------------------------------------------------------ */

static struct fb fb;

static pixel ui_bg_top, ui_bg_bot, ui_text_c, ui_dim_c;
static pixel ui_accent, ui_accent_dk, ui_red, ui_red_dk, ui_track_c, ui_sel_c, ui_row_c;

/* the ui primitives draw here: the panel itself, or the player's control bar buffer */
static struct fb *ui = &fb;

static void ui_init(void)
{
    ui_bg_top = fb_rgb(15, 23, 40);
    ui_bg_bot = fb_rgb(4, 6, 12);
    ui_text_c = fb_rgb(228, 236, 247);
    ui_dim_c = fb_rgb(124, 140, 162);
    ui_accent = fb_rgb(84, 194, 255);
    ui_accent_dk = fb_rgb(36, 92, 132);
    ui_red = fb_rgb(255, 96, 96);
    ui_red_dk = fb_rgb(58, 18, 24);
    ui_track_c = fb_rgb(28, 38, 56);
    ui_sel_c = fb_rgb(26, 38, 62);
    ui_row_c = fb_rgb(13, 19, 31);
}

static void ui_fill(int x, int y, int w, int h, pixel c)
{
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > ui->width) {
        w = ui->width - x;
    }
    if (y + h > ui->height) {
        h = ui->height - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int j = 0; j < h; j++) {
        pixel *row = (pixel *)((char *)ui->pixels + (y + j) * ui->stride) + x;

        for (int i = 0; i < w; i++) {
            row[i] = c;
        }
    }
}

static pixel ui_mix(pixel a, pixel b, int t, int n)
{
    int ar = a >> 11 & 31, ag = a >> 5 & 63, ab = a & 31;
    int br = b >> 11 & 31, bg = b >> 5 & 63, bb = b & 31;

    return (pixel)(((ar * (n - t) + br * t) / n) << 11 |
                   ((ag * (n - t) + bg * t) / n) << 5 |
                   (ab * (n - t) + bb * t) / n);
}

/* a rounded rectangle, corners resolved with the squared distance (no sqrt) */
static void ui_rrect(int x, int y, int w, int h, int r, pixel c)
{
    if (r > w / 2) {
        r = w / 2;
    }
    if (r > h / 2) {
        r = h / 2;
    }
    for (int j = 0; j < h; j++) {
        int inset = 0;

        if (j < r) {
            int dy = r - 1 - j;

            while (inset < r && (r - 1 - inset) * (r - 1 - inset) + dy * dy > (r - 1) * (r - 1)) {
                inset++;
            }
        } else if (j >= h - r) {
            int dy = j - (h - r);

            while (inset < r && (r - 1 - inset) * (r - 1 - inset) + dy * dy > (r - 1) * (r - 1)) {
                inset++;
            }
        }
        ui_fill(x + inset, y + j, w - 2 * inset, 1, c);
    }
}

static void ui_frame_rrect(int x, int y, int w, int h, int r, int t, pixel edge, pixel fill)
{
    ui_rrect(x, y, w, h, r, edge);
    ui_rrect(x + t, y + t, w - 2 * t, h - 2 * t, r - t > 0 ? r - t : 1, fill);
}

static void ui_circle(int cx, int cy, int r, pixel c)
{
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y <= r * r) {
                ui_fill(cx + x, cy + y, 1, 1, c);
            }
        }
    }
}

/* the x on the exit button and the error badge */
static void ui_x(int cx, int cy, int r, int t, pixel c)
{
    for (int i = -r; i <= r; i++) {
        ui_fill(cx + i, cy + i, t, t, c);
        ui_fill(cx + i, cy - i, t, t, c);
    }
}

static void ui_tri(int cx, int cy, int w, int h, int dir, pixel c)
{
    for (int i = 0; i < h; i++) {
        int half = w * (i + 1) / (2 * h);
        int y = dir < 0 ? cy - h / 2 + i : cy + h / 2 - i;

        ui_fill(cx - half, y, half * 2, 1, c);
    }
}

/* a right pointing triangle, for the play mark in the header tile */
static void ui_tri_right(int x, int cy, int w, int h, pixel c)
{
    for (int i = 0; i < w; i++) {
        int hh = h - h * i / w;

        ui_fill(x + i, cy - hh / 2, 1, hh, c);
    }
}

static void ui_text(int x, int baseline, pixel c, const char *s)
{
    menu_text(ui, x, baseline, c, s);
}

static void ui_text_center(int cx, int baseline, pixel c, const char *s)
{
    menu_text(ui, cx - menu_text_width(s) / 2, baseline, c, s);
}

static void ui_text_small(int x, int baseline, pixel c, const char *s)
{
    menu_text_face(ui, menu_font_small_glyphs, menu_font_small_bits, MENU_FONT_SMALL_ASCENT,
                   1, x, baseline, c, s);
}

static void ui_text_small_center(int cx, int baseline, pixel c, const char *s)
{
    ui_text_small(cx - menu_text_face_width(menu_font_small_glyphs, 1, s) / 2,
                  baseline, c, s);
}

/* the wordmark: the big cut with the letters spaced out */
static void ui_text_title(int cx, int baseline, pixel c, int gap, const char *s)
{
    int x = cx - (menu_text_face_width(menu_font_big_glyphs, 1, s) + ((int)strlen(s) - 1) * gap) / 2;

    for (; *s; s++) {
        char ch[2] = { *s, 0 };

        menu_text_face(ui, menu_font_big_glyphs, menu_font_big_bits, MENU_FONT_BIG_ASCENT,
                       1, x, baseline, c, ch);
        x += menu_text_face_width(menu_font_big_glyphs, 1, ch) + gap;
    }
}

static pixel ui_bg_at(int y)
{
    return ui_mix(ui_bg_top, ui_bg_bot, y, ui->height - 1);
}

/* a vertical gradient with a faint dot grid, cheap enough to paint every redraw */
static void ui_bg(void)
{
    for (int y = 0; y < ui->height; y++) {
        ui_fill(0, y, ui->width, 1, ui_bg_at(y));
    }
    for (int y = 24; y < ui->height; y += 48) {
        for (int x = 24; x < ui->width; x += 48) {
            ui_fill(x, y, 2, 2, ui_mix(ui_bg_at(y), ui_text_c, 1, 14));
        }
    }
}

/* ---- icons ------------------------------------------------------------------ */

static void icon_folder(int x, int y)
{
    pixel body = fb_rgb(252, 196, 88), lid = fb_rgb(214, 152, 52), slot = fb_rgb(122, 86, 30);

    ui_fill(x + 2, y + 7, 15, 6, lid);
    ui_rrect(x, y + 11, 34, 22, 4, body);
    ui_fill(x + 6, y + 16, 22, 4, slot);
    ui_fill(x + 6, y + 23, 13, 3, slot);
}

static void icon_film(int x, int y)
{
    pixel body = fb_rgb(74, 102, 142), hole = fb_rgb(15, 21, 33), screen = fb_rgb(22, 30, 46);

    ui_rrect(x + 1, y, 32, 34, 4, body);
    ui_fill(x + 12, y + 8, 13, 18, screen);
    for (int i = 0; i < 3; i++) {
        ui_fill(x + 4, y + 3 + i * 11, 5, 7, hole);
        ui_fill(x + 25, y + 3 + i * 11, 5, 7, hole);
    }
}

static void icon_doc(int x, int y)
{
    pixel body = fb_rgb(72, 80, 94), line = fb_rgb(28, 34, 46);

    ui_rrect(x + 4, y, 27, 34, 3, body);
    for (int i = 0; i < 4; i++) {
        ui_fill(x + 10, y + 7 + i * 6, 16, 3, line);
    }
}

static void icon_chevron(int x, int y, pixel c)
{
    for (int i = 0; i < 7; i++) {
        ui_fill(x + i, y + i, 3, 3, c);
        ui_fill(x + i, y + 12 - i, 3, 3, c);
    }
}

static void icon_play(int cx, int cy, pixel c)
{
    for (int i = 0; i < 13; i++) {
        ui_fill(cx - 6 + i, cy - 10 + i, 2, 21 - 2 * i, c);
    }
}

static void icon_pause(int cx, int cy, pixel c)
{
    ui_fill(cx - 7, cy - 9, 6, 19, c);
    ui_fill(cx + 2, cy - 9, 6, 19, c);
}

/* the media/extension badge, returns its width */
static int ext_badge(int x, int y, const char *ext, pixel bg, pixel fg)
{
    int w = menu_text_face_width(menu_font_small_glyphs, 1, ext) + 14;

    ui_rrect(x, y, w, 20, 5, bg);
    menu_text_face(ui, menu_font_small_glyphs, menu_font_small_bits, MENU_FONT_SMALL_ASCENT,
                   1, x + 7, y + (20 + MENU_FONT_SMALL_ASCENT - MENU_FONT_SMALL_DESCENT) / 2,
                   fg, ext);
    return w;
}

/* ---- small text helpers ----------------------------------------------------- */

static void text_elide(char *dst, size_t n, const char *src, int maxw)
{
    snprintf(dst, n, "%s", src);
    if (menu_text_width(dst) <= maxw) {
        return;
    }
    for (int len = (int)strlen(src) - 1; len > 0; len--) {
        snprintf(dst, n, "%.*s...", len, src);
        if (menu_text_width(dst) <= maxw) {
            return;
        }
    }
    snprintf(dst, n, "...");
}

/* the cwd, losing leading components until the tail fits */
static void path_elide(char *dst, size_t n, const char *path, int maxw)
{
    int starts[32], ns = 1, len = (int)strlen(path);

    if (menu_text_width(path) <= maxw) {
        snprintf(dst, n, "%s", path);
        return;
    }
    starts[0] = 0;
    for (int i = 1; i < len && ns < 32; i++) {
        if (path[i - 1] == '/' && path[i]) {
            starts[ns++] = i;
        }
    }
    for (int k = ns - 1; k >= 0; k--) {
        snprintf(dst, n, "...%s", path + starts[k]);
        if (menu_text_width(dst) <= maxw || starts[k] == 0) {
            return;
        }
    }
    text_elide(dst, n, path, maxw);
}

static void fmt_size(char *buf, size_t n, unsigned int size)
{
    if (size >= 1048576u) {
        unsigned int whole = size / 1048576u, tenth = (size % 1048576u) * 10u / 1048576u;

        snprintf(buf, n, "%u.%u MB", whole, tenth);
    } else if (size >= 1024u) {
        snprintf(buf, n, "%u KB", size / 1024u);
    } else {
        snprintf(buf, n, "%u B", size);
    }
}

static void fmt_time(char *buf, size_t n, int64_t us)
{
    int s, h, m;

    if (us < 0) {
        us = 0;
    }
    s = (int)(us / 1000000);
    h = s / 3600;
    m = s / 60 % 60;
    s %= 60;
    if (h) {
        snprintf(buf, n, "%d:%02d:%02d", h, m, s);
    } else {
        snprintf(buf, n, "%d:%02d", m, s);
    }
}

static long long now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ---- the file browser -------------------------------------------------------- */

#define HDR_H    64
#define ROW_X    20
#define ROW_W    (SCREEN_W - 2 * ROW_X)
#define ROW_Y0   78
#define ROW_H    54
#define ROWS     6
#define FOOT_Y   406
#define FOOT_H   74
#define EXIT_X   14
#define EXIT_Y   12
#define EXIT_W   104
#define EXIT_H   40
#define UP_X     24
#define UP_Y     418
#define UP_W     88
#define UP_H     50
#define DOWN_X   (UP_X + UP_W + 14)
#define DOWN_Y   UP_Y

enum { BZ_NONE = -100, BZ_EXIT, BZ_UP, BZ_DOWN };

struct entry {
    char name[NAME_MAX];
    unsigned int size;
    unsigned char is_dir, media, up;
};

static struct entry b_entries[MAX_ENTRIES];
static int b_count, b_first, b_sel;
static int b_press = BZ_NONE, b_press_row = -1;
static char b_dir[PATH_MAX];

static int is_media_name(const char *name)
{
    static const char *exts[] = {
        "mpg", "mpeg", "m1v", "m2v", "ts", "m2t", "vob", "avi", "mkv", "webm",
        "mjpeg", "mjpg", "jpg", "jpeg", "yuv", "raw", 0
    };
    const char *dot = strrchr(name, '.');

    if (!dot || !dot[1]) {
        return 0;
    }
    for (int i = 0; exts[i]; i++) {
        if (strcasecmp(dot + 1, exts[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static void join_path(char *dst, size_t n, const char *dir, const char *name)
{
    char tmp[PATH_MAX];

    if (!strcmp(dir, "/")) {
        snprintf(tmp, sizeof tmp, "/%s", name);
    } else {
        snprintf(tmp, sizeof tmp, "%s/%s", dir, name);
    }
    snprintf(dst, n, "%s", tmp);
}

static void parent_path(char *path)
{
    char *slash = strrchr(path, '/');

    if (slash && slash != path) {
        *slash = 0;
    } else {
        strcpy(path, "/");
    }
}

static int entry_cmp(const void *a, const void *b)
{
    const struct entry *x = a, *y = b;

    if (x->up != y->up) {
        return x->up ? -1 : 1;
    }
    if (x->is_dir != y->is_dir) {
        return x->is_dir ? -1 : 1;
    }
    return strcasecmp(x->name, y->name);
}

static void ensure_visible(void)
{
    if (b_sel < b_first) {
        b_first = b_sel;
    }
    if (b_sel >= b_first + ROWS) {
        b_first = b_sel - ROWS + 1;
    }
    if (b_count <= ROWS) {
        b_first = 0;
    } else if (b_first > b_count - ROWS) {
        b_first = b_count - ROWS;
    }
    if (b_first < 0) {
        b_first = 0;
    }
}

/* select a named entry after a rescan, so coming up from a directory keeps the cursor on it */
static void select_name(const char *name)
{
    b_sel = 0;
    for (int i = 0; i < b_count; i++) {
        if (!strcmp(b_entries[i].name, name)) {
            b_sel = i;
            break;
        }
    }
    ensure_visible();
}

static int scan_dir(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *de;
    char full[PATH_MAX];

    if (!d) {
        return -1;
    }
    b_count = 0;
    if (strcmp(path, "/")) {
        struct entry *e = &b_entries[b_count++];

        memset(e, 0, sizeof *e);
        strcpy(e->name, "..");
        e->is_dir = e->up = 1;
    }
    while (b_count < MAX_ENTRIES && (de = readdir(d)) != 0) {
        struct entry *e;
        int namelen = de->d_namelen;

        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
            continue;
        }
        if (namelen <= 0 || namelen >= NAME_MAX) {
            namelen = (int)strlen(de->d_name);
            if (namelen <= 0 || namelen >= NAME_MAX) {
                continue;
            }
        }
        e = &b_entries[b_count];
        memset(e, 0, sizeof *e);
        memcpy(e->name, de->d_name, namelen);
        e->name[namelen] = 0;
        e->media = is_media_name(e->name);
        join_path(full, sizeof full, path, e->name);
        /* qnx opendir() is the directory test: it returns ENOTDIR for a file. the size
         * probe only runs for media, so browsing / never opens a device node by hand. */
        {
            DIR *sub = opendir(full);

            if (sub) {
                e->is_dir = 1;
                closedir(sub);
            } else if (e->media) {
                int fd = open(full, O_RDONLY);

                if (fd >= 0) {
                    long size = lseek(fd, 0, SEEK_END);

                    e->size = size > 0 ? (unsigned int)size : 0;
                    close(fd);
                }
            }
        }
        b_count++;
    }
    closedir(d);
    qsort(b_entries, b_count, sizeof b_entries[0], entry_cmp);
    ensure_visible();
    return 0;
}

static void browser_header(void)
{
    char path[PATH_MAX];
    pixel fill = b_press == BZ_EXIT ? ui_red_dk : fb_rgb(13, 19, 30);
    pixel edge = b_press == BZ_EXIT ? ui_red : fb_rgb(96, 116, 144);
    pixel fg = b_press == BZ_EXIT ? fb_rgb(255, 226, 226) : ui_text_c;
    int x = 158;

    ui_fill(0, 0, SCREEN_W, HDR_H, fb_rgb(10, 15, 25));
    ui_fill(0, HDR_H - 2, SCREEN_W, 2, ui_accent_dk);

    ui_frame_rrect(EXIT_X, EXIT_Y, EXIT_W, EXIT_H, 8, 2, edge, fill);
    ui_x(EXIT_X + 22, EXIT_Y + EXIT_H / 2, 7, 2, fg);
    ui_text_small(EXIT_X + 42, EXIT_Y + (EXIT_H + MENU_FONT_SMALL_ASCENT - MENU_FONT_SMALL_DESCENT) / 2,
                  fg, "EXIT");

    ui_rrect(x - 24, 22, 20, 20, 5, ui_accent);
    ui_tri_right(x - 19, 32, 9, 12, fb_rgb(8, 14, 24));
    ui_text(x, 43, ui_text_c, "Media Player");

    path_elide(path, sizeof path, b_dir, 290);
    ui_text_small(SCREEN_W - 20 - menu_text_face_width(menu_font_small_glyphs, 1, path),
                  40, ui_dim_c, path);
}

static void browser_rows(void)
{
    char name[NAME_MAX + 8];
    char size[32];

    if (b_count == 0) {
        icon_folder(SCREEN_W / 2 - 17, 168);
        ui_text_center(SCREEN_W / 2, 240, ui_text_c, "No files here");
        ui_text_small_center(SCREEN_W / 2, 268, ui_dim_c, "pick another folder");
        return;
    }
    for (int i = 0; i < ROWS; i++) {
        int idx = b_first + i, y;
        const struct entry *e;
        int x;

        if (idx >= b_count) {
            break;
        }
        e = &b_entries[idx];
        y = ROW_Y0 + i * ROW_H;
        ui_rrect(ROW_X, y + 2, ROW_W, ROW_H - 6, 10,
                 idx == b_sel ? ui_sel_c : ui_row_c);
        if (idx == b_sel) {
            ui_rrect(ROW_X + 7, y + 10, 5, ROW_H - 22, 3, ui_accent);
        }
        if (i == b_press_row) {
            ui_rrect(ROW_X, y + 2, ROW_W, ROW_H - 6, 10, fb_rgb(36, 50, 78));
        }

        if (e->up) {
            icon_folder(ROW_X + 18, y + 12);
        } else if (e->is_dir) {
            icon_folder(ROW_X + 18, y + 12);
        } else if (e->media) {
            icon_film(ROW_X + 19, y + 11);
        } else {
            icon_doc(ROW_X + 19, y + 11);
        }

        x = ROW_X + 70;
        if (!e->is_dir && !e->up) {
            const char *dot = strrchr(e->name, '.');

            if (dot && dot[1]) {
                char ext[8];

                snprintf(ext, sizeof ext, "%s", dot + 1);
                for (char *p = ext; *p; p++) {
                    if (*p >= 'a' && *p <= 'z') {
                        *p -= 32;
                    }
                }
                x += ext_badge(x, y + 16, ext,
                               e->media ? fb_rgb(30, 64, 96) : fb_rgb(38, 44, 56),
                               e->media ? ui_accent : ui_dim_c) + 10;
            }
        }

        text_elide(name, sizeof name, e->name, ROW_X + ROW_W - 96 - x);
        ui_text(x, y + ROW_H / 2 + 10, e->media || e->is_dir ? ui_text_c : ui_dim_c, name);

        if (e->is_dir || e->up) {
            icon_chevron(ROW_X + ROW_W - 26, y + 18, ui_dim_c);
        } else if (e->media) {
            fmt_size(size, sizeof size, e->size);
            ui_text_small(ROW_X + ROW_W - 18 - menu_text_face_width(menu_font_small_glyphs, 1, size),
                          y + ROW_H / 2 + 6, ui_dim_c, size);
        }
    }
}

static void browser_footer(void)
{
    char line[64];
    pixel edge = fb_rgb(70, 88, 116);

    ui_fill(0, FOOT_Y, SCREEN_W, FOOT_H, fb_rgb(10, 15, 25));
    ui_fill(0, FOOT_Y, SCREEN_W, 2, ui_accent_dk);

    ui_frame_rrect(UP_X, UP_Y, UP_W, UP_H, 10, 2,
                   b_press == BZ_UP ? ui_accent : edge,
                   b_press == BZ_UP ? ui_accent_dk : fb_rgb(16, 24, 38));
    ui_tri(UP_X + UP_W / 2, UP_Y + UP_H / 2, 22, 12, -1,
           b_press == BZ_UP ? ui_text_c : ui_dim_c);

    ui_frame_rrect(DOWN_X, DOWN_Y, UP_W, UP_H, 10, 2,
                   b_press == BZ_DOWN ? ui_accent : edge,
                   b_press == BZ_DOWN ? ui_accent_dk : fb_rgb(16, 24, 38));
    ui_tri(DOWN_X + UP_W / 2, DOWN_Y + UP_H / 2, 22, 12, 1,
           b_press == BZ_DOWN ? ui_text_c : ui_dim_c);

    if (b_count > 0) {
        int last = b_first + ROWS > b_count ? b_count : b_first + ROWS;

        snprintf(line, sizeof line, "%d-%d of %d", b_first + 1, last, b_count);
    } else {
        snprintf(line, sizeof line, "empty");
    }
    ui_text_center(SCREEN_W / 2, FOOT_Y + 34, ui_text_c, line);
    ui_text_small_center(SCREEN_W / 2, FOOT_Y + 58, ui_dim_c, "tap a file to play - knob scrolls");
}

static void browser_draw(void)
{
    ui = &fb;
    ui_bg();
    browser_header();
    browser_rows();
    browser_footer();
}

static int browser_hit(int x, int y)
{
    if (x >= EXIT_X && x < EXIT_X + EXIT_W && y >= EXIT_Y && y < EXIT_Y + EXIT_H) {
        return BZ_EXIT;
    }
    if (y >= UP_Y && y < UP_Y + UP_H && x >= UP_X && x < UP_X + UP_W) {
        return BZ_UP;
    }
    if (y >= DOWN_Y && y < DOWN_Y + UP_H && x >= DOWN_X && x < DOWN_X + UP_W) {
        return BZ_DOWN;
    }
    if (y >= ROW_Y0 && y < ROW_Y0 + ROWS * ROW_H && x >= ROW_X && x < ROW_X + ROW_W) {
        int row = (y - ROW_Y0) / ROW_H;

        if (b_first + row < b_count) {
            return row;
        }
    }
    return BZ_NONE;
}

/* returns 1 with a file path in out, 0 when the exit button was used */
static int browse(char *dir, char *out, size_t outn)
{
    static const char *fallbacks[] = { "/fs/usb0", "/tmp", "/" };

    /* drop whatever the caller left in the queue (the tap that started this program, say)
     * so it cannot act on the first screen */
    input_flush();
    snprintf(b_dir, sizeof b_dir, "%s", dir);
    for (unsigned i = 0; i < sizeof fallbacks / sizeof fallbacks[0]; i++) {
        if (scan_dir(b_dir) == 0) {
            break;
        }
        snprintf(b_dir, sizeof b_dir, "%s", fallbacks[i]);
    }
    snprintf(dir, PATH_MAX, "%s", b_dir);

    for (;;) {
        struct input_event e;
        int zone, done = 0;

        browser_draw();
        input_wait(&e);
        if (e.type == INPUT_KNOB && e.code) {
            if (b_count > 0) {
                b_sel += e.code > 0 ? 1 : -1;
                if (b_sel < 0) b_sel = 0;
                if (b_sel >= b_count) b_sel = b_count - 1;
                ensure_visible();
            }
            continue;
        }
        if (e.type != INPUT_TOUCH || !e.down) {
            continue;
        }
        zone = browser_hit(e.x, e.y);
        b_press = zone;
        b_press_row = zone >= 0 ? zone : -1;
        browser_draw();
        /* follow the finger; act only if it comes up on the same zone */
        for (;;) {
            input_wait(&e);
            if (e.type == INPUT_KNOB && e.code) {
                continue;
            }
            if (e.type == INPUT_TOUCH && !e.down) {
                break;
            }
            if (e.type == INPUT_TOUCH && e.down) {
                int nz = browser_hit(e.x, e.y);

                if (nz != zone) {
                    zone = nz;
                    b_press = nz;
                    b_press_row = nz >= 0 ? nz : -1;
                    browser_draw();
                }
            }
        }
        b_press = BZ_NONE;
        b_press_row = -1;
        if (zone == BZ_NONE) {
            continue;
        }
        if (zone == BZ_EXIT) {
            snprintf(dir, PATH_MAX, "%s", b_dir);
            return 0;
        }
        if (zone == BZ_UP) {
            b_first -= ROWS;
            if (b_first < 0) b_first = 0;
            b_sel = b_first;
            continue;
        }
        if (zone == BZ_DOWN) {
            b_first += ROWS;
            if (b_first > b_count - ROWS) b_first = b_count - ROWS;
            if (b_first < 0) b_first = 0;
            b_sel = b_first;
            continue;
        }
        {
            int idx = b_first + zone;
            const struct entry *ent = &b_entries[idx];

            if (ent->up) {
                char came[NAME_MAX];

                snprintf(came, sizeof came, "%s", strrchr(b_dir, '/') ? strrchr(b_dir, '/') + 1 : b_dir);
                parent_path(b_dir);
                scan_dir(b_dir);
                select_name(came);
            } else if (ent->is_dir) {
                join_path(b_dir, sizeof b_dir, b_dir, ent->name);
                if (scan_dir(b_dir) != 0) {
                    parent_path(b_dir);
                    scan_dir(b_dir);
                } else {
                    b_sel = 0;
                    ensure_visible();
                }
            } else {
                join_path(out, outn, b_dir, ent->name);
                done = 1;
            }
        }
        if (done) {
            snprintf(dir, PATH_MAX, "%s", b_dir);
            return 1;
        }
    }
}

/* ---- decoding and drawing ---------------------------------------------------- */

#define COFF_R 205
#define COFF_G 154
#define COFF_B 258

static int csc_ready;
static int ty[256], tyf[256];
static int trv[2][256], tgu[2][256], tgv[2][256], tbu[2][256];
static pixel tab_r[1024], tab_g[1024], tab_b[1024];

static void csc_init(void)
{
    if (csc_ready) {
        return;
    }
    for (int i = 0; i < 256; i++) {
        int c = i < 16 ? 0 : (i > 235 ? 219 : i - 16);
        int d = i - 128;

        ty[i] = (298 * c + 128) >> 8;
        tyf[i] = (298 * i + 128) >> 8;
        trv[0][i] = (409 * d) >> 8;
        tgu[0][i] = (-100 * d) >> 8;
        tgv[0][i] = (-208 * d) >> 8;
        tbu[0][i] = (516 * d) >> 8;
        trv[1][i] = (459 * d) >> 8;
        tgu[1][i] = (-55 * d) >> 8;
        tgv[1][i] = (-136 * d) >> 8;
        tbu[1][i] = (541 * d) >> 8;
    }
    for (int k = 0; k < 1024; k++) {
        int v;

        v = k - COFF_R;
        v = v < 0 ? 0 : (v > 255 ? 255 : v);
        tab_r[k] = (pixel)((v >> 3) << 11);
        v = k - COFF_G;
        v = v < 0 ? 0 : (v > 255 ? 255 : v);
        tab_g[k] = (pixel)((v >> 2) << 5);
        v = k - COFF_B;
        v = v < 0 ? 0 : (v > 255 ? 255 : v);
        tab_b[k] = (pixel)(v >> 3);
    }
    csc_ready = 1;
}

static inline pixel yuv420_pix(const unsigned char *yp, const unsigned char *up,
                               const unsigned char *vp, int x, int full, int m)
{
    int y = (full ? tyf : ty)[yp[x]];
    int u = up[x >> 1], v = vp[x >> 1];

    return tab_r[y + trv[m][v] + COFF_R] |
           tab_g[y + tgu[m][u] + tgv[m][v] + COFF_G] |
           tab_b[y + tbu[m][u] + COFF_B];
}

/* a whole luma row to rgb565, two pixels a store. the u/v contributions are resolved once
 * per chroma pair and shared by both pixels, which is the difference between this and two
 * calls to yuv420_pix: under --icount 2 the panel conversion is the hot loop, so the fewer
 * table loads and stores the better. out may be 2 mod 4 (an odd destination x), handled
 * with one single pixel first. */
static void conv_row_420(pixel *out, const unsigned char *yp, const unsigned char *up,
                         const unsigned char *vp, int w, int full, int m)
{
    const int *yt = full ? tyf : ty;
    const int *cr = trv[m], *cgu = tgu[m], *cgv = tgv[m], *cbu = tbu[m];
    int x = 0;

    if (((uintptr_t)out & 3) && w > 0) {
        int y = yt[yp[0]], u = up[0], v = vp[0];

        *out++ = tab_r[y + cr[v] + COFF_R] | tab_g[y + cgu[u] + cgv[v] + COFF_G] |
                 tab_b[y + cbu[u] + COFF_B];
        x = 1;
    }
    for (; x + 1 < w; x += 2) {
        int u = up[x >> 1], v = vp[x >> 1];
        int r = cr[v] + COFF_R, g = cgu[u] + cgv[v] + COFF_G, b = cbu[u] + COFF_B;
        int ya = yt[yp[x]], yb = yt[yp[x + 1]];

        *(uint32_t *)(void *)out = (uint32_t)(tab_r[ya + r] | tab_g[ya + g] | tab_b[ya + b]) |
                                   (uint32_t)(tab_r[yb + r] | tab_g[yb + g] | tab_b[yb + b]) << 16;
        out += 2;
    }
    if (x < w) {
        int y = yt[yp[x]], u = up[x >> 1], v = vp[x >> 1];

        *out = tab_r[y + cr[v] + COFF_R] | tab_g[y + cgu[u] + cgv[v] + COFF_G] |
               tab_b[y + cbu[u] + COFF_B];
    }
}

/* ---- the player -------------------------------------------------------------- */

#define PBAR_H      100
#define PBAR_Y      (SCREEN_H - PBAR_H)
#define TRK_X0      20
#define TRK_X1      780
#define TRK_Y       34
#define TRK_H       10
#define PBTN_Y      54
#define PBTN_H      38
#define BAR_HOLD_US 4000000
#define LATE_US     80000
#define JUMP_US     1000000
#define SEEK_STEP_US 10000000

enum { PS_PLAY, PS_PAUSE, PS_END };
enum { Z_NONE = 0, Z_BACK, Z_REW, Z_PLAY, Z_FWD, Z_TRACK };

static pixel pbar_px[SCREEN_W * PBAR_H];
static struct fb pbar_fb = { pbar_px, SCREEN_W, PBAR_H, SCREEN_W * 2 };

static struct {
    AVFormatContext *fmt;
    AVCodecContext *dec;
    const AVCodec *codec;
    AVPacket *pkt;
    AVFrame *frm;
    int vs;
    int have_frame;
    int draining;
    int pkt_pending;
    int err;
    int slow;                   /* the pipeline is below real time: show every frame */
    AVRational tb;
    AVRational fps;
    int64_t frame_dur;
    int64_t last_pts, last_dur;
    int64_t duration_us;
    int64_t cur_us;
    /* frame geometry */
    int fw, fh, dw, dh, dx, dy, intscale, iscale;
    int *xmap, *ymap;
    int map_dw, map_dh, map_fw, map_fh;
    pixel *conv;
    int conv_w;
    struct SwsContext *sws;
    int sws_w, sws_h, sws_fmt, sws_dw, sws_dh;
} pl;

static char pl_name[NAME_MAX + 8];
static int pl_state = PS_PLAY;
static int pl_press = Z_NONE;
static int pl_tap_empty;
static long long pl_bar_until;
static int pl_dirty = 1;        /* bar buffer or screen state needs a rebuild */
static int pl_bar_on;           /* the control bar pixels are on the panel */
static int pl_need_frame;
static int64_t pl_clock_us, pl_base_us;
static int opt_lowres, opt_loop, opt_stats;
static long long pl_stat_at;
static unsigned long pl_frames, pl_dropped;
static long long pl_dec_us, pl_draw_us;
/* the speed check: once a second, how many frames actually reached the panel against the
 * file's own rate. two slow seconds in a row mean full size cannot keep up under this cpu,
 * so the decoder drops to half size (the same fallback the other players use) */
static long long pl_health_at;
static unsigned long pl_health_frames;
static int pl_slow_secs;
static long long pl_stat0;

/* sizing: integer upscale when the frame is smaller than the panel (crisp 2x/3x pixels),
 * fractional fit when it is bigger. both cases go through the same sample maps. */
static void player_layout(int fw, int fh)
{
    int dw, dh, s, intscale = 1;

    if (fw == pl.fw && fh == pl.fh && pl.xmap) {
        return;
    }
    s = SCREEN_W / fw;
    if (SCREEN_H / fh < s) {
        s = SCREEN_H / fh;
    }
    if (s >= 1) {
        if (s > 3) {
            s = 3;
        }
        dw = fw * s;
        dh = fh * s;
    } else {
        int p = SCREEN_W * 100 / fw;

        if (SCREEN_H * 100 / fh < p) {
            p = SCREEN_H * 100 / fh;
        }
        dw = fw * p / 100;
        dh = fh * p / 100;
        intscale = 0;
    }
    if (dw < 2) dw = 2;
    if (dh < 2) dh = 2;
    pl.fw = fw;
    pl.fh = fh;
    pl.dw = dw;
    pl.dh = dh;
    pl.intscale = intscale;
    /* the horizontal expansion of an integer scale is a shift, not a map lookup */
    pl.iscale = intscale ? dw / fw : 0;
    pl.dx = (SCREEN_W - dw) / 2;
    pl.dy = (SCREEN_H - dh) / 2;

    if (pl.map_dw != dw || pl.map_fw != fw) {
        free(pl.xmap);
        pl.xmap = malloc(dw * sizeof *pl.xmap);
        pl.map_dw = dw;
        pl.map_fw = fw;
        for (int x = 0; pl.xmap && x < dw; x++) {
            pl.xmap[x] = x * fw / dw;
        }
    }
    if (pl.map_dh != dh || pl.map_fh != fh) {
        free(pl.ymap);
        pl.ymap = malloc(dh * sizeof *pl.ymap);
        pl.map_dh = dh;
        pl.map_fh = fh;
        for (int y = 0; pl.ymap && y < dh; y++) {
            pl.ymap[y] = y * fh / dh;
        }
    }
    if (pl.conv_w < fw) {
        free(pl.conv);
        pl.conv = malloc(fw * sizeof *pl.conv);
        pl.conv_w = pl.conv ? fw : 0;
    }
    /* the letterbox, repainted only when the geometry changes */
    fb_fill(&fb, 0, 0, SCREEN_W, pl.dy, 0);
    fb_fill(&fb, 0, pl.dy + pl.dh, SCREEN_W, SCREEN_H - pl.dy - pl.dh, 0);
    fb_fill(&fb, 0, pl.dy, pl.dx, pl.dh, 0);
    fb_fill(&fb, pl.dx + pl.dw, pl.dy, SCREEN_W - pl.dx - pl.dw, pl.dh, 0);
}

static void draw_video(const AVFrame *f)
{
    int m = f->colorspace == AVCOL_SPC_BT709;

    player_layout(f->width, f->height);
    if (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P) {
        int full = f->format == AV_PIX_FMT_YUVJ420P;
        const unsigned char *yp = f->data[0], *up = f->data[1], *vp = f->data[2];
        int ys = f->linesize[0], us = f->linesize[1], vs = f->linesize[2];
        int cached = -1;

        for (int y = 0; y < pl.dh; y++) {
            int sy = pl.ymap[y];
            pixel *drow = (pixel *)((char *)fb.pixels + (pl.dy + y) * fb.stride) + pl.dx;
            const unsigned char *yrow = yp + sy * ys;
            const unsigned char *urow = up + (sy >> 1) * us;
            const unsigned char *vrow = vp + (sy >> 1) * vs;

            if (pl.dw == pl.fw) {
                conv_row_420(drow, yrow, urow, vrow, pl.fw, full, m);
            } else if (pl.intscale) {
                if (sy != cached) {
                    conv_row_420(pl.conv, yrow, urow, vrow, pl.fw, full, m);
                    cached = sy;
                }
                if (pl.conv) {
                    if (pl.iscale == 2) {
                        for (int x = 0; x < pl.dw; x++) {
                            drow[x] = pl.conv[x >> 1];
                        }
                    } else if (pl.iscale == 1) {
                        memcpy(drow, pl.conv, pl.fw * sizeof *drow);
                    } else if (pl.iscale == 3) {
                        for (int x = 0; x < pl.dw; x++) {
                            drow[x] = pl.conv[x / 3];
                        }
                    } else {
                        for (int x = 0; x < pl.dw; x++) {
                            drow[x] = pl.conv[pl.xmap[x]];
                        }
                    }
                }
            } else {
                for (int x = 0; x < pl.dw; x++) {
                    drow[x] = yuv420_pix(yrow, urow, vrow, pl.xmap[x], full, m);
                }
            }
        }
    } else if (pl.dw > 0) {
        /* anything else (422 mjpeg, rgb, ...) goes through swscale once per frame */
        if (!pl.sws || pl.sws_w != f->width || pl.sws_h != f->height || pl.sws_fmt != f->format ||
            pl.sws_dw != pl.dw || pl.sws_dh != pl.dh) {
            sws_freeContext(pl.sws);
            pl.sws = sws_getContext(f->width, f->height, f->format, pl.dw, pl.dh,
                                    AV_PIX_FMT_RGB565LE, SWS_POINT, NULL, NULL, NULL);
            pl.sws_w = f->width;
            pl.sws_h = f->height;
            pl.sws_fmt = f->format;
            pl.sws_dw = pl.dw;
            pl.sws_dh = pl.dh;
        }
        if (pl.sws) {
            uint8_t *dst[4] = { (uint8_t *)fb.pixels + pl.dy * fb.stride + pl.dx * 2, 0, 0, 0 };
            int dsts[4] = { fb.stride, 0, 0, 0 };

            sws_scale(pl.sws, (const uint8_t * const *)f->data, f->linesize, 0, f->height,
                      dst, dsts);
        }
    }
}

static void pbar_blit(void)
{
    for (int y = 0; y < PBAR_H; y++) {
        memcpy((char *)fb.pixels + (PBAR_Y + y) * fb.stride, pbar_px + y * SCREEN_W, SCREEN_W * 2);
    }
}

static void pbar_erase(void)
{
    fb_fill(&fb, 0, PBAR_Y, SCREEN_W, PBAR_H, 0);
}

static void chip(int x, int w, const char *label, int pressed, int hot)
{
    pixel edge = pressed ? ui_accent : (hot ? ui_accent : fb_rgb(70, 88, 116));
    pixel fill = hot ? ui_accent_dk : fb_rgb(16, 24, 38);
    pixel fg = ui_text_c;

    ui_frame_rrect(x, PBTN_Y, w, PBTN_H, 9, 2, edge, fill);
    ui_text_small_center(x + w / 2,
                         PBTN_Y + (PBTN_H + MENU_FONT_SMALL_ASCENT - MENU_FONT_SMALL_DESCENT) / 2,
                         fg, label);
}

static void pbar_draw(void)
{
    char time[32], name[NAME_MAX + 8];
    int frac = 0;

    ui = &pbar_fb;
    ui_fill(0, 0, SCREEN_W, PBAR_H, fb_rgb(10, 15, 25));
    ui_fill(0, 0, SCREEN_W, 2, ui_accent_dk);

    text_elide(name, sizeof name, pl_name, 520);
    ui_text_small(20, 22, ui_text_c, name);
    if (pl.duration_us > 0) {
        char a[16], b[16];

        fmt_time(a, sizeof a, pl.cur_us);
        fmt_time(b, sizeof b, pl.duration_us);
        snprintf(time, sizeof time, "%s / %s", a, b);
        frac = (int)(pl.cur_us * 1000 / pl.duration_us);
        if (frac < 0) frac = 0;
        if (frac > 1000) frac = 1000;
    } else {
        fmt_time(time, sizeof time, pl.cur_us);
    }
    ui_text_small(SCREEN_W - 20 - menu_text_face_width(menu_font_small_glyphs, 1, time), 22,
                  ui_dim_c, time);

    /* the track */
    ui_rrect(TRK_X0, TRK_Y, TRK_X1 - TRK_X0, TRK_H, TRK_H / 2, ui_track_c);
    if (pl.duration_us > 0) {
        int tw = (TRK_X1 - TRK_X0) * frac / 1000;

        if (tw > 0) {
            ui_rrect(TRK_X0, TRK_Y, tw, TRK_H, TRK_H / 2, ui_accent);
        }
        ui_circle(TRK_X0 + tw, TRK_Y + TRK_H / 2, 9, ui_accent);
        ui_circle(TRK_X0 + tw, TRK_Y + TRK_H / 2, 5, ui_text_c);
    }

    chip(202, 92, "BACK", pl_press == Z_BACK, 0);
    chip(310, 72, "-10", pl_press == Z_REW, 0);
    chip(398, 112, pl_state == PS_PAUSE ? "PLAY" : "PAUSE", pl_press == Z_PLAY, 1);
    chip(526, 72, "+10", pl_press == Z_FWD, 0);

    ui = &fb;
}

static int pbar_hit(int x, int y)
{
    if (y < PBAR_Y) {
        return Z_NONE;
    }
    y -= PBAR_Y;
    if (y >= TRK_Y - 8 && y < TRK_Y + TRK_H + 8 && x >= TRK_X0 && x <= TRK_X1) {
        return Z_TRACK;
    }
    if (y < PBTN_Y || y >= PBTN_Y + PBTN_H) {
        return Z_NONE;
    }
    if (x >= 202 && x < 202 + 92) return Z_BACK;
    if (x >= 310 && x < 310 + 72) return Z_REW;
    if (x >= 398 && x < 398 + 112) return Z_PLAY;
    if (x >= 526 && x < 526 + 72) return Z_FWD;
    return Z_NONE;
}

static int bar_visible(void)
{
    return pl_state == PS_PAUSE || now_us() < pl_bar_until;
}

/* ---- the decode side --------------------------------------------------------- */

static int media_open(const char *path)
{
    const AVCodec *codec;
    int ret;

    memset(&pl, 0, sizeof pl);
    pl.last_pts = AV_NOPTS_VALUE;
    if ((ret = avformat_open_input(&pl.fmt, path, NULL, NULL)) < 0) {
        pl.err = ret;
        return -1;
    }
    if ((ret = avformat_find_stream_info(pl.fmt, NULL)) < 0) {
        pl.err = ret;
        return -1;
    }
    pl.vs = av_find_best_stream(pl.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (pl.vs < 0) {
        pl.err = pl.vs;
        return -1;
    }
    pl.codec = codec;
    pl.dec = avcodec_alloc_context3(codec);
    if (!pl.dec) {
        pl.err = AVERROR(ENOMEM);
        return -1;
    }
    avcodec_parameters_to_context(pl.dec, pl.fmt->streams[pl.vs]->codecpar);
    if (opt_lowres > 0) {
        pl.dec->lowres = opt_lowres;
    }
    if ((ret = avcodec_open2(pl.dec, codec, NULL)) < 0) {
        pl.err = ret;
        return -1;
    }
    pl.pkt = av_packet_alloc();
    pl.frm = av_frame_alloc();
    if (!pl.pkt || !pl.frm) {
        pl.err = AVERROR(ENOMEM);
        return -1;
    }
    pl.tb = pl.fmt->streams[pl.vs]->time_base;
    pl.fps = av_guess_frame_rate(pl.fmt, pl.fmt->streams[pl.vs], NULL);
    if (pl.fps.num <= 0 || pl.fps.den <= 0 || pl.tb.num <= 0) {
        pl.fps = (AVRational){ 25, 1 };
    }
    pl.frame_dur = av_rescale_q(1000000LL * pl.fps.den / pl.fps.num, AV_TIME_BASE_Q, pl.tb);
    if (pl.frame_dur < 1) {
        pl.frame_dur = 1;
    }
    pl.last_dur = pl.frame_dur;
    pl.duration_us = pl.fmt->duration;
    if (pl.duration_us <= 0) {
        AVRational r = pl.fmt->streams[pl.vs]->time_base;
        int64_t d = pl.fmt->streams[pl.vs]->duration;

        pl.duration_us = d > 0 ? av_rescale_q(d, r, AV_TIME_BASE_Q) : 0;
    }
    return 0;
}

static void media_close(void)
{
    if (pl.pkt) {
        av_packet_free(&pl.pkt);
    }
    if (pl.frm) {
        av_frame_free(&pl.frm);
    }
    if (pl.dec) {
        avcodec_free_context(&pl.dec);
    }
    if (pl.fmt) {
        avformat_close_input(&pl.fmt);
    }
    sws_freeContext(pl.sws);
    free(pl.xmap);
    free(pl.ymap);
    free(pl.conv);
    memset(&pl, 0, sizeof pl);
}

/* returns 1 with a frame in pl.frm, 0 at end of stream, -1 on a fatal decode error */
static int media_next(void)
{
    if (pl.have_frame) {
        return 1;
    }
    for (;;) {
        if (pl.pkt_pending) {
            int ret = avcodec_send_packet(pl.dec, pl.pkt);

            if (ret == AVERROR(EAGAIN)) {
                /* decoder full: hand out what it has, retry the packet after */
            } else if (ret < 0) {
                pl.err = ret;
                return -1;
            } else {
                av_packet_unref(pl.pkt);
                pl.pkt_pending = 0;
            }
        } else if (!pl.draining) {
            int ret = av_read_frame(pl.fmt, pl.pkt);

            if (ret < 0) {
                if (ret != AVERROR_EOF) {
                    pl.err = ret;
                    return -1;
                }
                pl.draining = 1;
                avcodec_send_packet(pl.dec, NULL);
            } else if (pl.pkt->stream_index != pl.vs) {
                av_packet_unref(pl.pkt);
                continue;
            } else {
                pl.pkt_pending = 1;
                continue;
            }
        }
        {
            int ret = avcodec_receive_frame(pl.dec, pl.frm);

            if (ret == 0) {
                pl.have_frame = 1;
                return 1;
            }
            if (ret == AVERROR(EAGAIN)) {
                /* a decoder that wants frames out but has none while a packet is still
                 * pending cannot make progress; report end rather than spinning on it */
                if (pl.draining || pl.pkt_pending) {
                    return 0;
                }
                continue;
            }
            if (ret == AVERROR_EOF) {
                return 0;
            }
            pl.err = ret;
            return -1;
        }
    }
}

static int64_t frame_pts_us(void)
{
    int64_t pts = pl.frm->pts;

    if (pts == AV_NOPTS_VALUE) {
        pts = pl.last_pts != AV_NOPTS_VALUE ? pl.last_pts + pl.last_dur : 0;
    }
    pl.last_pts = pts;
    pl.last_dur = pl.frm->duration > 0 ? pl.frm->duration : pl.frame_dur;
    return av_rescale_q(pts, pl.tb, AV_TIME_BASE_Q);
}

static void player_seek_us(int64_t target)
{
    int64_t ts;

    if (target < 0) {
        target = 0;
    }
    if (pl.duration_us > 0 && target > pl.duration_us) {
        target = pl.duration_us;
    }
    /* the whole-file route first (av_time_base, stream -1), the same one ffplay uses: on a
     * container without a real index it lets the demuxer pick the closest point instead of
     * a binary search collapsing to the head of the file. BACKWARD asks for the keyframe at
     * or before the target, which matters most after a decoder reopen: a fresh context has
     * no dimensions until a sequence header arrives. */
    if (avformat_seek_file(pl.fmt, -1, INT64_MIN, target, INT64_MAX, AVSEEK_FLAG_BACKWARD) < 0) {
        ts = av_rescale_q(target, AV_TIME_BASE_Q, pl.tb);
        if (av_seek_frame(pl.fmt, pl.vs, ts, AVSEEK_FLAG_BACKWARD) < 0) {
            return;
        }
    }
    avcodec_flush_buffers(pl.dec);
    av_packet_unref(pl.pkt);
    av_frame_unref(pl.frm);
    pl.have_frame = 0;
    pl.pkt_pending = 0;
    pl.draining = 0;
    pl.last_pts = AV_NOPTS_VALUE;
    pl.last_dur = pl.frame_dur;
    pl.cur_us = target;
    pl_base_us = target;
    pl_clock_us = now_us();
    pl_need_frame = 1;
    pl_dirty = 1;
}

/* reopen the decoder with lowres forced. libavcodec only takes lowres before open, so a
 * player that finds itself too slow for full size has to rebuild the codec context and seek
 * back to where the picture is. the format context and the browser stay untouched. */
static int media_reopen(int lowres)
{
    AVCodecContext *dec = avcodec_alloc_context3(pl.codec);

    if (!dec) {
        return -1;
    }
    avcodec_parameters_to_context(dec, pl.fmt->streams[pl.vs]->codecpar);
    if (lowres > 0) {
        dec->lowres = lowres;
    }
    if (avcodec_open2(dec, pl.codec, NULL) < 0) {
        avcodec_free_context(&dec);
        return -1;
    }
    avcodec_free_context(&pl.dec);
    pl.dec = dec;
    opt_lowres = lowres;
    av_packet_unref(pl.pkt);
    av_frame_unref(pl.frm);
    pl.have_frame = 0;
    pl.pkt_pending = 0;
    pl.draining = 0;
    pl.slow = 0;
    player_seek_us(pl.cur_us);
    return 0;
}

static void show_message(const char *title, const char *l1, const char *l2, long long hold_us)
{
    long long until = now_us() + hold_us;

    input_flush();
    ui = &fb;
    ui_bg();
    ui_frame_rrect(80, 130, SCREEN_W - 160, 220, 16, 2, ui_red, fb_rgb(24, 16, 22));
    ui_fill(98, 132, SCREEN_W - 196, 3, ui_red);
    ui_circle(SCREEN_W / 2, 190, 22, fb_rgb(70, 22, 28));
    ui_x(SCREEN_W / 2, 190, 10, 3, ui_red);
    ui_text_center(SCREEN_W / 2, 250, ui_text_c, title);
    if (l1) {
        ui_text_small_center(SCREEN_W / 2, 282, ui_dim_c, l1);
    }
    if (l2) {
        ui_text_small_center(SCREEN_W / 2, 304, ui_dim_c, l2);
    }
    ui_text_small_center(SCREEN_W / 2, 332, ui_dim_c, "touch to continue");

    for (;;) {
        struct input_event e;

        while (input_poll(&e)) {
            if (e.type == INPUT_TOUCH && e.down) {
                return;
            }
        }
        if (now_us() >= until) {
            return;
        }
        usleep(20000);
    }
}

static void player_stats(void)
{
    long long now = now_us();

    if (now - pl_stat_at < 1000000) {
        return;
    }
    pl_stat_at = now;
    printf("mediaplayer: %lu frames, %lu dropped, decode %lld.%02lld ms, draw %lld.%02lld ms, "
           "pos %lld.%02lld s, elapsed %lld.%01lld s\n",
           pl_frames, pl_dropped, pl_dec_us / 1000, pl_dec_us % 1000 / 10,
           pl_draw_us / 1000, pl_draw_us % 1000 / 10,
           pl.cur_us / 1000000, pl.cur_us % 1000000 / 10000,
           (now - pl_stat0) / 1000000, (now - pl_stat0) % 1000000 / 100000);
    fflush(stdout);
}

/* once a second, check the drawn frame count against the file's own rate. two slow seconds
 * in a row at full size drop the decoder to half size; if half size is still not enough,
 * stop dropping frames and play every one at the speed the unit can manage. */
static void player_health(long long now)
{
    unsigned drawn, want;

    if (now - pl_health_at < 1000000) {
        return;
    }
    drawn = pl_frames - pl_health_frames;
    want = pl.fps.num > 0 && pl.fps.den > 0 ? (unsigned)(pl.fps.num / pl.fps.den) : 25u;
    if (want < 1) {
        want = 1;
    }
    pl_health_frames = pl_frames;
    pl_health_at = now;
    if (drawn * 100 >= want * 85) {
        pl_slow_secs = 0;
        return;
    }
    if (++pl_slow_secs < 2) {
        return;
    }
    pl_slow_secs = 0;
    if (opt_lowres == 0) {
        printf("mediaplayer: too slow at full size, retrying at half size\n");
        fflush(stdout);
        if (media_reopen(1) != 0) {
            printf("mediaplayer: lowres restart failed, playing slowly\n");
            fflush(stdout);
            pl.slow = 1;
        }
    } else {
        pl.slow = 1;
    }
}

static void player_run(const char *path)
{
    int quit = 0;

    printf("mediaplayer: %s\n", path);
    input_flush();
    if (media_open(path) != 0) {
        char detail[96];
        const char *name = strrchr(path, '/');

        av_strerror(pl.err, detail, sizeof detail);
        show_message("Cannot play this file", name ? name + 1 : path, detail, 5000000);
        media_close();
        return;
    }
    snprintf(pl_name, sizeof pl_name, "%s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    pl_state = PS_PLAY;
    pl_press = Z_NONE;
    pl_tap_empty = 0;
    pl_need_frame = 0;
    pl_dirty = 1;
    pl_bar_until = now_us() + BAR_HOLD_US;
    pl_frames = pl_dropped = 0;
    pl_dec_us = pl_draw_us = 0;
    pl_stat0 = pl_stat_at = now_us();
    pl_clock_us = now_us();
    pl_base_us = 0;
    pl.cur_us = 0;
    pl_health_at = pl_clock_us;
    pl_health_frames = 0;
    pl_slow_secs = 0;

    for (;;) {
        struct input_event e;
        long long now = now_us();

        while (input_poll(&e)) {
            if (pl_state == PS_END) {
                if ((e.type == INPUT_TOUCH && e.down) || (e.type == INPUT_KNOB && e.code)) {
                    pl_bar_until = 0;
                }
                continue;
            }
            if (e.type == INPUT_KNOB && e.code) {
                player_seek_us(pl.cur_us + (e.code < 0 ? -SEEK_STEP_US : SEEK_STEP_US));
            } else if (e.type == INPUT_TOUCH) {
                if (e.down) {
                    if (!bar_visible()) {
                        pl_bar_until = now + BAR_HOLD_US;
                        pl_dirty = 1;
                    } else {
                        int z = pbar_hit(e.x, e.y);

                        if (z == Z_TRACK) {
                            if (pl.duration_us > 0) {
                                player_seek_us(pl.duration_us * (e.x - TRK_X0) / (TRK_X1 - TRK_X0));
                            }
                        } else if (z != Z_NONE) {
                            pl_press = z;
                            pl_dirty = 1;
                        } else {
                            pl_tap_empty = 1;
                        }
                    }
                } else if (pl_press != Z_NONE) {
                    int z = pbar_hit(e.x, e.y), act = z == pl_press;

                    pl_press = Z_NONE;
                    pl_dirty = 1;
                    if (act) {
                        if (z == Z_BACK) {
                            quit = 1;
                        } else if (z == Z_REW) {
                            player_seek_us(pl.cur_us - SEEK_STEP_US);
                        } else if (z == Z_FWD) {
                            player_seek_us(pl.cur_us + SEEK_STEP_US);
                        } else if (z == Z_PLAY) {
                            if (pl_state == PS_PLAY) {
                                pl_state = PS_PAUSE;
                            } else {
                                pl_state = PS_PLAY;
                                pl_base_us = pl.cur_us;
                                pl_clock_us = now;
                            }
                            pl_dirty = 1;
                        }
                    }
                } else if (pl_tap_empty) {
                    pl_tap_empty = 0;
                    pl_bar_until = 0;
                    pl_dirty = 1;
                }
            }
        }
        if (quit) {
            break;
        }

        if (pl_state == PS_PLAY) {
            int r;

            if (pl_need_frame) {
                long long t0 = now_us();

                r = media_next();
                pl_dec_us += now_us() - t0;
                pl_need_frame = 0;
                if (r < 0) {
                    char detail[96];

                    av_strerror(pl.err, detail, sizeof detail);
                    show_message("Playback failed", detail, NULL, 5000000);
                    break;
                }
                if (r == 0) {
                    if (opt_loop) {
                        player_seek_us(0);
                        continue;
                    }
                    pl_state = PS_END;
                    pl_bar_until = now + 2000000;
                    pl_dirty = 1;
                } else {
                    long long t1 = now_us();

                    draw_video(pl.frm);
                    pl_draw_us += now_us() - t1;
                    pl.cur_us = frame_pts_us();
                    pl_frames++;
                    av_frame_unref(pl.frm);
                    pl.have_frame = 0;
                    pl_dirty = 1;
                }
            } else {
                long long t0 = now_us();
                int64_t target, pts;

                r = media_next();
                pl_dec_us += now_us() - t0;
                if (r < 0) {
                    char detail[96];

                    av_strerror(pl.err, detail, sizeof detail);
                    show_message("Playback failed", detail, NULL, 5000000);
                    break;
                }
                if (r == 0) {
                    if (opt_loop) {
                        player_seek_us(0);
                        continue;
                    }
                    pl_state = PS_END;
                    pl_bar_until = now + 2000000;
                    pl_dirty = 1;
                } else {
                    pts = frame_pts_us();
                    target = pl_base_us + (now - pl_clock_us);
                    if (pts + LATE_US < target) {
                        if (pl.slow) {
                            /* below real time even at half size: stop dropping, re-anchor
                             * every frame and show the file at the unit's own pace */
                            pl_base_us = pts;
                            pl_clock_us = now;
                            target = pts;
                        } else if (target - pts > JUMP_US) {
                            /* a slow decode or a long stall: dropping frames would only
                             * freeze the picture, because decode, not drawing, is the cost
                             * and it never catches up. re-anchor the clock to what is
                             * actually on screen and carry on. */
                            pl_base_us = pts;
                            pl_clock_us = now;
                            target = pts;
                        } else {
                            pl_dropped++;
                            av_frame_unref(pl.frm);
                            pl.have_frame = 0;
                            continue;
                        }
                    }
                    if (pts > target + 1000) {
                        /* sleep the whole remainder in one call: a loop of tiny sleeps
                         * overshoots badly on qnx's timer granularity, which turns a 25 fps
                         * file into a slideshow even though decode and draw have headroom.
                         * the cap keeps touch and the knob responsive. */
                        int64_t wait = pts - target;

                        if (wait > 200000) {
                            wait = 200000;
                        }
                        usleep((unsigned int)wait);
                        continue;
                    }
                    {
                        long long t1 = now_us();

                        draw_video(pl.frm);
                        pl_draw_us += now_us() - t1;
                    }
                    pl.cur_us = pts;
                    pl_frames++;
                    av_frame_unref(pl.frm);
                    pl.have_frame = 0;
                    pl_dirty = 1;
                }
            }
            player_health(now_us());
            if (opt_stats) {
                player_stats();
            }
        } else if (pl_state == PS_PAUSE) {
            if (pl_need_frame) {
                int r = media_next();

                if (r == 1) {
                    draw_video(pl.frm);
                    pl.cur_us = frame_pts_us();
                    av_frame_unref(pl.frm);
                    pl.have_frame = 0;
                    pl_dirty = 1;
                }
                pl_need_frame = 0;
            }
            usleep(10000);
        } else if (pl_state == PS_END) {
            if (pl_dirty) {
                pbar_draw();
                pl_dirty = 0;
            }
            pbar_blit();
            pl_bar_on = 1;
            if (now >= pl_bar_until) {
                break;
            }
            usleep(10000);
            continue;
        }

        if (bar_visible()) {
            static int last_sec = -1;
            int sec = (int)(pl.cur_us / 1000000);

            if (pl_dirty || sec != last_sec) {
                pbar_draw();
                last_sec = sec;
                pl_dirty = 0;
            }
            pbar_blit();
            pl_bar_on = 1;
        } else if (pl_bar_on) {
            /* the bar was dismissed: the next decoded frame repaints its rows anyway, but a
             * paused/still picture needs the strip cleared now */
            pbar_erase();
            pl_bar_on = 0;
        }
    }
    media_close();
}

/* ---- main -------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    static char start[PATH_MAX] = "/fs/usb0";
    static char first_file[PATH_MAX] = "";
    static char chosen[PATH_MAX];

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--lowres") && i + 1 < argc) {
            opt_lowres = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--loop")) {
            opt_loop = 1;
        } else if (!strcmp(argv[i], "--stats")) {
            opt_stats = 1;
        } else if (argv[i][0] != '-') {
            snprintf(start, sizeof start, "%s", argv[i]);
        }
    }
    /* the demuxer's probing chatter (a bare audio stream with no codec parameters, say) is
     * noise on a console nobody is reading: keep the console for this player's own output */
    av_log_set_level(AV_LOG_ERROR);
    if (fb_open(&fb) != 0) {
        printf("mediaplayer: no framebuffer\n");
        return 1;
    }
    if (input_start() != 0) {
        printf("mediaplayer: no controls\n");
        return 1;
    }
    csc_init();
    ui_init();

    {
        DIR *d = opendir(start);

        if (d) {
            closedir(d);
        } else if (access(start, 0) == 0) {
            snprintf(first_file, sizeof first_file, "%s", start);
            parent_path(start);
        }
    }

    for (;;) {
        if (first_file[0]) {
            player_run(first_file);
            first_file[0] = 0;
        }
        if (!browse(start, chosen, sizeof chosen)) {
            break;
        }
        player_run(chosen);
    }
    return 0;
}
