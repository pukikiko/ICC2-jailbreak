#include "qnx.h"
#include "fb.h"
#include "console.h"
#include "input.h"
#include "hmictl.h"

#define SCREEN_W      800
#define SCREEN_H      480
#define ROWS_VISIBLE  24
#define ROW_H         16
#define LIST_X        4
#define LIST_W        500
#define LIST_Y        44
#define GRID_X        512
#define GRID_Y        44
#define CELL_W        29
#define CELL_H        17
#define CELL_DX       31
#define CELL_DY       19
#define GRID_COLS     9
#define MAX_LINES     64
#define LOG_LINES     7
#define MAX_BUTTONS   40
#define BITS          53

#define RGB(r, g, b)  ((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) & 0xf8) >> 3)

#define C_BG      RGB(10, 12, 18)
#define C_PANEL   RGB(24, 30, 42)
#define C_SEL     RGB(34, 62, 104)
#define C_TEXT    RGB(228, 234, 240)
#define C_DIM     RGB(128, 136, 150)
#define C_ACC     RGB(255, 196, 64)
#define C_GOOD    RGB(110, 228, 130)
#define C_PRESET  RGB(96, 184, 255)
#define C_BAD     RGB(232, 84, 84)
#define C_SEEN    RGB(56, 92, 120)
#define C_BOUND   RGB(34, 104, 58)

struct bitev {
    int press, tap, held, release, fallback, ticks;
};

static const struct bitev bit_info[BITS] = {
    [0]  = {314, 315, 312, 313, 761, 30},
    [4]  = {326, 327, 324, 325, 764, 5},
    [6]  = {920, 0, 0, 0, 0, 0},
    [7]  = {924, 0, 0, 0, 0, 0},
    [8]  = {912, 0, 0, 0, 0, 0},
    [9]  = {914, 0, 0, 0, 0, 0},
    [11] = {921, 0, 0, 0, 0, 0},
    [12] = {922, 0, 0, 0, 0, 0},
    [13] = {917, 0, 0, 0, 0, 0},
    [14] = {918, 0, 0, 0, 0, 0},
    [16] = {913, 0, 0, 0, 0, 0},
    [17] = {919, 0, 0, 0, 0, 0},
    [18] = {915, 0, 0, 0, 0, 0},
    [19] = {916, 0, 0, 0, 0, 0},
    [20] = {923, 0, 0, 0, 0, 0},
    [22] = {822, 824, 820, 821, 823, 0},
    [23] = {344, 345, 342, 343, 767, 5},
    [24] = {332, 333, 330, 331, 765, 5},
    [25] = {320, 321, 318, 319, 762, 15},
    [26] = {630, 631, 628, 629, 763, 5},
    [27] = {392, 393, 390, 391, 775, 5},
    [28] = {350, 351, 348, 349, 768, 5},
    [29] = {400, 401, 398, 399, 776, 5},
    [30] = {406, 407, 404, 405, 777, 5},
    [36] = {374, 375, 372, 373, 772, 5},
    [37] = {380, 381, 378, 379, 773, 5},
    [38] = {386, 387, 384, 385, 774, 5},
    [41] = {368, 369, 366, 367, 771, 5},
    [42] = {362, 363, 360, 361, 770, 5},
    [43] = {356, 357, 354, 355, 769, 5},
    [46] = {418, 419, 416, 417, 779, 15},
    [47] = {412, 413, 410, 411, 778, 5},
    [48] = {424, 425, 422, 423, 780, 5},
    [49] = {436, 437, 434, 435, 782, 5},
    [50] = {430, 431, 428, 429, 781, 5},
    [52] = {826, 0, 0, 0, 827, 5},
};

struct button {
    const char *group;
    const char *name;
    const char *can;
    int bit;
    int found;
};

static struct button buttons[] = {
    {"icc audio panel 764 0x2fc 100ms", "power",           "x1=01 x5=03",      0, 0},
    {"icc audio panel 764 0x2fc 100ms", "eject",           "x2=80",            4, 0},
    {"icc audio panel 764 0x2fc 100ms", "load",            "x2=40",           26, 0},
    {"icc audio panel 764 0x2fc 100ms", "seek up",         "x1=04",           30, 0},
    {"icc audio panel 764 0x2fc 100ms", "seek down",       "x1=08",           29, 0},
    {"icc audio panel 764 0x2fc 100ms", "fm/am",           "x1=20",           24, 0},
    {"icc audio panel 764 0x2fc 100ms", "scn/as",          "x1=40",           22, 0},
    {"icc audio panel 764 0x2fc 100ms", "cd/aux",          "x1=80",           23, 0},
    {"icc audio panel 764 0x2fc 100ms", "menu",            "x1=10",           28, 0},
    {"icc audio panel 764 0x2fc 100ms", "ok",              "x3=21",           27, 0},
    {"icc audio panel 764 0x2fc 100ms", "volume up",       "x4=41",           -1, 0},
    {"icc audio panel 764 0x2fc 100ms", "volume down",     "x4=81",           -1, 0},

    {"icc climate panel 775 0x307 500ms", "climate off",         "x2=10",       6, 0},
    {"icc climate panel 775 0x307 500ms", "recirculate",         "x1=40",       7, 0},
    {"icc climate panel 775 0x307 500ms", "a/c",                 "x1=80",       8, 0},
    {"icc climate panel 775 0x307 500ms", "auto",                "x2=20",       9, 0},
    {"icc climate panel 775 0x307 500ms", "passenger temp down", "bit 11 docs", 11, 0},
    {"icc climate panel 775 0x307 500ms", "passenger temp up",   "bit 12 docs", 12, 0},
    {"icc climate panel 775 0x307 500ms", "fan down",            "x2=08",       13, 0},
    {"icc climate panel 775 0x307 500ms", "fan up",              "x2=04",       14, 0},
    {"icc climate panel 775 0x307 500ms", "air distribution",    "x2=80",       16, 0},
    {"icc climate panel 775 0x307 500ms", "front demist",        "x1=02",       17, 0},
    {"icc climate panel 775 0x307 500ms", "driver temp down",    "x3=80/40",    18, 0},
    {"icc climate panel 775 0x307 500ms", "driver temp up",      "x3=40/80",    19, 0},
    {"icc climate panel 775 0x307 500ms", "rear demist",         "x1=20",       20, 0},

    {"icc button controls 775 0x307 500ms", "hazard lights", "x3=01",  3, 0},
    {"icc button controls 775 0x307 500ms", "cabin lights",  "x4=a0",  2, 0},
    {"icc button controls 775 0x307 500ms", "dsc hold",      "x4=90", 21, 0},
    {"icc button controls 775 0x307 500ms", "lock",          "x4=c0",  1, 0},
    {"icc button controls 775 0x307 500ms", "unlock",        "x4=84",  5, 0},

    {"steering wheel 754 0x2f2 200ms", "swc seek",        "x8=08/09/0c",  48, 0},
    {"steering wheel 754 0x2f2 200ms", "swc volume up",   "x8=10/11/14",  49, 0},
    {"steering wheel 754 0x2f2 200ms", "swc volume down", "x8=18/19/1c",  50, 0},
    {"steering wheel 754 0x2f2 200ms", "swc phone",       "x7=61/65/68",  46, 0},
    {"steering wheel 754 0x2f2 200ms", "swc mode",        "x8=01 x7=48",  47, 0},
};

#define N_BUTTONS ((int)(sizeof buttons / sizeof buttons[0]))

struct line {
    const char *header;
    int button;
};

static struct fb fbm;
static struct line lines[MAX_LINES];
static int nlines;
static int first, sel = -1, armed;
static int stop_hmi;
static void *saved_screen;
static unsigned char seen[BITS / 8 + 1], held[BITS / 8 + 1];
static unsigned long hits[BITS], press_at[BITS], hold_ms[BITS];
static int last_bit = -1, last_down;
static long knob_total;
static char log_text[LOG_LINES][44];
static int log_count, log_next;
static char status[80];
static unsigned long reset_at;
static int leave;

static int bget(const unsigned char *map, int bit)
{
    return map[bit >> 3] >> (bit & 7) & 1;
}

static void bset(unsigned char *map, int bit, int on)
{
    if (on) {
        map[bit >> 3] |= 1 << (bit & 7);
    } else {
        map[bit >> 3] &= ~(1 << (bit & 7));
    }
}

static unsigned long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000UL + ts.tv_nsec / 1000000UL;
}

static void log_push(const char *text)
{
    snprintf(log_text[log_next], sizeof log_text[0], "%s", text);
    log_next = (log_next + 1) % LOG_LINES;
    if (log_count < LOG_LINES) {
        log_count++;
    }
}

static void box(int x, int y, int w, int h, pixel c)
{
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > fbm.width) {
        w = fbm.width - x;
    }
    if (y + h > fbm.height) {
        h = fbm.height - y;
    }
    if (w > 0 && h > 0) {
        fb_fill(&fbm, x, y, w, h, c);
    }
}

static void border(int x, int y, int w, int h, pixel c)
{
    box(x, y, w, 1, c);
    box(x, y + h - 1, w, 1, c);
    box(x, y, 1, h, c);
    box(x + w - 1, y, 1, h, c);
}

static void txt(int x, int y, pixel c, const char *s)
{
    fb_text(&fbm, x, y, c, s);
}

static void fmt_hmi(int bit, char *buf, int len)
{
    const struct bitev *b = &bit_info[bit];

    if (bit < 0) {
        snprintf(buf, len, "no event");
    } else if (b->press == 0) {
        snprintf(buf, len, "unused bit");
    } else     if (b->ticks > 0 && b->tap == 0 && b->held == 0 && b->release == 0) {
        snprintf(buf, len, "hmi p%d", b->press);
    } else if (b->ticks > 0) {
        snprintf(buf, len, "hmi p%d tap%d hold%d@%d.%ds rel%d",
                 b->press, b->tap, b->held, b->ticks / 10, b->ticks % 10, b->release);
    } else if (b->fallback) {
        snprintf(buf, len, "hmi p%d rel%d", b->press, b->fallback);
    } else {
        snprintf(buf, len, "hmi p%d", b->press);
    }
}

static void build_lines(void)
{
    nlines = 0;
    for (int i = 0; i < N_BUTTONS && nlines + 1 < MAX_LINES; i++) {
        if (i == 0 || strcmp(buttons[i].group, buttons[i - 1].group) != 0) {
            lines[nlines].header = buttons[i].group;
            lines[nlines].button = -1;
            nlines++;
        }
        lines[nlines].header = 0;
        lines[nlines].button = i;
        nlines++;
    }
    sel = 1;
}

static int button_bound(int index)
{
    return buttons[index].bit >= 0;
}

static void find_row_for_bit(int bit)
{
    for (int i = 0; i < N_BUTTONS; i++) {
        if (buttons[i].bit == bit) {
            for (int l = 0; l < nlines; l++) {
                if (lines[l].button == i) {
                    sel = l;
                    armed = 1;
                    if (sel < first) {
                        first = sel;
                    }
                    if (sel >= first + ROWS_VISIBLE) {
                        first = sel - ROWS_VISIBLE + 1;
                    }
                    return;
                }
            }
        }
    }
}

static void bind_bit(int row, int bit)
{
    char line[80];

    for (int i = 0; i < N_BUTTONS; i++) {
        if (i != row && buttons[i].bit == bit) {
            buttons[i].bit = -1;
            buttons[i].found = 0;
        }
    }
    buttons[row].bit = bit;
    buttons[row].found = 1;
    snprintf(line, sizeof line, "bound %s = bit %d", buttons[row].name, bit);
    log_push(line);
    snprintf(status, sizeof status, "%s", line);
    printf("iccbuttons: %s (%s) -> bit %d\n", buttons[row].name, buttons[row].can, bit);
    armed = 0;
}

static void on_button(int bit, int down)
{
    char line[80];

    if (bit < 0 || bit >= BITS) {
        return;
    }
    bset(seen, bit, 1);
    if (down) {
        press_at[bit] = now_ms();
        hold_ms[bit] = 0;
        hits[bit]++;
        bset(held, bit, 1);
        last_bit = bit;
        last_down = 1;
        snprintf(line, sizeof line, "bit %d down  (hmi %d)", bit, bit_info[bit].press);
        log_push(line);
        printf("iccbuttons: bit %d down (hmi %d)\n", bit, bit_info[bit].press);
        if (armed && sel >= 0 && sel < nlines && lines[sel].button >= 0) {
            bind_bit(lines[sel].button, bit);
        }
    } else {
        bset(held, bit, 0);
        hold_ms[bit] = now_ms() - press_at[bit];
        last_bit = bit;
        last_down = 0;
        snprintf(line, sizeof line, "bit %d up after %lu ms", bit, hold_ms[bit]);
        log_push(line);
        printf("iccbuttons: bit %d up after %lu ms\n", bit, hold_ms[bit]);
    }
}

static void draw_list(void)
{
    char text[64];

    for (int i = 0; i < ROWS_VISIBLE; i++) {
        int n = first + i;
        int y = LIST_Y + i * ROW_H;

        if (n >= nlines) {
            break;
        }
        if (lines[n].header) {
            box(LIST_X, y, LIST_W, ROW_H, C_PANEL);
            snprintf(text, sizeof text, "%s", lines[n].header);
            txt(LIST_X + 6, y, C_ACC, text);
            continue;
        }
        struct button *b = &buttons[lines[n].button];
        if (n == sel) {
            box(LIST_X, y, LIST_W, ROW_H, C_SEL);
        }
        if (b->bit >= 0) {
            snprintf(text, sizeof text, "[%2d] %-22s %s", b->bit, b->name, b->can);
        } else {
            snprintf(text, sizeof text, "[ ?] %-22s %s", b->name, b->can);
        }
        txt(LIST_X + 6, y, button_bound(lines[n].button) ?
            (b->found ? C_GOOD : C_PRESET) : C_DIM, text);
    }
}

static void draw_grid(void)
{
    char text[8];

    for (int bit = 0; bit < BITS; bit++) {
        int x = GRID_X + (bit % GRID_COLS) * CELL_DX;
        int y = GRID_Y + (bit / GRID_COLS) * CELL_DY;
        pixel fill = C_PANEL, text_col = C_DIM;

        if (bget(seen, bit)) {
            fill = C_SEEN;
            text_col = C_TEXT;
        }
        for (int i = 0; i < N_BUTTONS; i++) {
            if (buttons[i].bit == bit) {
                fill = C_BOUND;
                text_col = C_GOOD;
            }
        }
        if (bget(held, bit)) {
            fill = C_GOOD;
            text_col = 0;
        }
        box(x, y, CELL_W, CELL_H, fill);
        if (bit == last_bit) {
            border(x, y, CELL_W, CELL_H, C_ACC);
        }
        snprintf(text, sizeof text, "%2d", bit);
        txt(x + 7, y + 1, text_col, text);
    }
}

static const char *row_name_for_bit(int bit)
{
    for (int i = 0; i < N_BUTTONS; i++) {
        if (buttons[i].bit == bit) {
            return buttons[i].name;
        }
    }
    return 0;
}

static void draw_info(void)
{
    char text[80], hmi[64];
    int y = GRID_Y + ((BITS + GRID_COLS - 1) / GRID_COLS) * CELL_DY + 8;

    if (last_bit >= 0) {
        fmt_hmi(last_bit, hmi, sizeof hmi);
        snprintf(text, sizeof text, "last bit %d %s", last_bit, last_down ? "down" : "up");
        txt(GRID_X, y, C_TEXT, text);
        snprintf(text, sizeof text, "held %lu ms", hold_ms[last_bit]);
        txt(GRID_X, y + 16, C_DIM, text);
        txt(GRID_X, y + 32, C_DIM, hmi);
        const char *name = row_name_for_bit(last_bit);
        snprintf(text, sizeof text, "row %s", name ? name : "(unbound)");
        txt(GRID_X, y + 48, name ? C_GOOD : C_DIM, text);
    } else {
        txt(GRID_X, y, C_DIM, "press a panel button");
        txt(GRID_X, y + 16, C_DIM, "to see its bitmap bit");
    }

    int bound = 0;
    for (int i = 0; i < N_BUTTONS; i++) {
        if (button_bound(i)) {
            bound++;
        }
    }
    int seen_count = 0;
    for (int bit = 0; bit < BITS; bit++) {
        if (bget(seen, bit)) {
            seen_count++;
        }
    }
    snprintf(text, sizeof text, "bound %d/%d  bits seen %d/%d", bound, N_BUTTONS, seen_count, BITS);
    txt(GRID_X, y + 68, C_TEXT, text);
    snprintf(text, sizeof text, "knob %+ld  hits %lu", knob_total,
             last_bit >= 0 ? hits[last_bit] : 0UL);
    txt(GRID_X, y + 84, C_DIM, text);

    int log_y = y + 108;
    for (int i = 0; i < log_count; i++) {
        int idx = (log_next - log_count + i + LOG_LINES) % LOG_LINES;

        txt(GRID_X, log_y + i * 16, C_DIM, log_text[idx]);
    }
}

static void draw_footer(void)
{
    txt(4, 436, C_TEXT, "up");
    txt(56, 436, C_TEXT, "down");
    txt(120, 436, C_TEXT, "clear");
    txt(200, 436, C_TEXT, "reset");
    if (status[0]) {
        txt(280, 436, C_ACC, status);
    }
}

static void draw(void)
{
    box(0, 0, SCREEN_W, SCREEN_H, C_BG);
    txt(8, 4, C_TEXT, "ICC button explorer");
    if (armed) {
        txt(8, 24, C_ACC, "press the button on the panel now");
    } else {
        txt(8, 24, C_DIM, "touch a row, then press that button on the panel");
    }
    box(660, 6, 52, 26, C_PANEL);
    border(660, 6, 52, 26, C_GOOD);
    txt(670, 11, C_GOOD, "save");
    box(724, 6, 68, 26, C_PANEL);
    border(724, 6, 68, 26, C_BAD);
    txt(744, 11, C_BAD, "exit");

    draw_list();
    draw_grid();
    draw_info();
    draw_footer();
}

static void save_map(const char *path)
{
    FILE *f = fopen(path, "w");
    char hmi[64], text[160];

    if (!f) {
        const char *alt = "/tmp/iccbuttons.map";

        f = fopen(alt, "w");
        path = alt;
    }
    if (!f) {
        snprintf(status, sizeof status, "could not save map: %s", strerror(errno));
        return;
    }
    fprintf(f, "# icc button explorer map\n");
    fprintf(f, "# bits are the front panel bitmap on ipc channel 6; in homebrew input.c turns a\n");
    fprintf(f, "# press into INPUT_BUTTON with e.code == bit and e.down == 1. can guesses are from\n");
    fprintf(f, "# reference/fg_controller_area_network_latest.xlsx and reference/can0icc.py.\n");
    fprintf(f, "#\n# bit  name                     can guess        hmi events\n");
    printf("iccbuttons: map\n");
    for (int i = 0; i < N_BUTTONS; i++) {
        if (buttons[i].bit >= 0) {
            fmt_hmi(buttons[i].bit, hmi, sizeof hmi);
            snprintf(text, sizeof text, "%4d  %-24s %-16s %s\n", buttons[i].bit,
                     buttons[i].name, buttons[i].can, hmi);
        } else {
            snprintf(text, sizeof text, "   ?  %-24s %-16s not seen\n", buttons[i].name,
                     buttons[i].can);
        }
        fprintf(f, "%s", text);
        printf("iccbuttons: %s", text);
    }
    fprintf(f, "\n# homebrew table, bit + name\n");
    printf("iccbuttons: homebrew table\n");
    for (int i = 0; i < N_BUTTONS; i++) {
        if (buttons[i].bit >= 0) {
            fprintf(f, "{%d, \"%s\"},\n", buttons[i].bit, buttons[i].name);
            printf("iccbuttons: {%d, \"%s\"},\n", buttons[i].bit, buttons[i].name);
        }
    }
    fclose(f);
    snprintf(status, sizeof status, "saved %s", path);
    log_push(status);
}

static int in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void scroll(int delta)
{
    first += delta;
    if (first < 0) {
        first = 0;
    }
    if (first > nlines - ROWS_VISIBLE) {
        first = nlines - ROWS_VISIBLE;
    }
    if (first < 0) {
        first = 0;
    }
}

static void on_touch(int x, int y)
{
    if (in_rect(x, y, 724, 6, 68, 26)) {
        leave = 1;
        return;
    }
    if (in_rect(x, y, 660, 6, 52, 26)) {
        save_map("/fs/usb0/homebrew/iccbuttons.map");
        return;
    }
    if (in_rect(x, y, 4, 432, 48, 44)) {
        scroll(-1);
        return;
    }
    if (in_rect(x, y, 56, 432, 60, 44)) {
        scroll(1);
        return;
    }
    if (in_rect(x, y, 118, 432, 70, 44)) {
        if (sel >= 0 && sel < nlines && lines[sel].button >= 0) {
            int i = lines[sel].button;

            buttons[i].bit = -1;
            buttons[i].found = 0;
            snprintf(status, sizeof status, "cleared %s", buttons[i].name);
            log_push(status);
        }
        return;
    }
    if (in_rect(x, y, 198, 432, 70, 44)) {
        if (now_ms() - reset_at < 5000) {
            for (int i = 0; i < N_BUTTONS; i++) {
                if (buttons[i].found) {
                    buttons[i].bit = -1;
                    buttons[i].found = 0;
                }
            }
            status[0] = 0;
            reset_at = 0;
            log_push("cleared every discovered binding");
        } else {
            reset_at = now_ms();
            snprintf(status, sizeof status, "touch reset again to clear discovered bits");
            log_push(status);
        }
        return;
    }
    if (x >= GRID_X && y >= GRID_Y && y < GRID_Y + ((BITS + GRID_COLS - 1) / GRID_COLS) * CELL_DY) {
        int col = (x - GRID_X) / CELL_DX, row = (y - GRID_Y) / CELL_DY;
        int bit = row * GRID_COLS + col;

        if (bit >= 0 && bit < BITS) {
            find_row_for_bit(bit);
            if (row_name_for_bit(bit)) {
                armed = 1;
            }
            return;
        }
    }
    if (in_rect(x, y, LIST_X, LIST_Y, LIST_W, ROWS_VISIBLE * ROW_H)) {
        int n = first + (y - LIST_Y) / ROW_H;

        if (n >= 0 && n < nlines && lines[n].button >= 0) {
            sel = n;
            armed = 1;
        }
    }
}

static void on_event(const struct input_event *e)
{
    if (e->type == INPUT_BUTTON) {
        on_button(e->code, e->down);
    } else if (e->type == INPUT_KNOB) {
        knob_total += e->code;
        scroll(e->code);
    } else if (e->type == INPUT_TOUCH && e->down) {
        on_touch(e->x, e->y);
    }
}

int main(int argc, char **argv)
{
    struct input_event e;
    int own_buttons = 0;

    if (fb_open(&fbm) != 0) {
        printf("iccbuttons: no framebuffer (%s)\n", strerror(errno));
        return 1;
    }
    if (access("/hmi_", 0) == 0) {
        own_buttons = hmi_events_pause();
        hmi_signal(HMI_SIGSTOP);
        saved_screen = malloc(fbm.stride * fbm.height);
        if (saved_screen) {
            memcpy(saved_screen, fbm.pixels, fbm.stride * fbm.height);
        }
        stop_hmi = 1;
    }
    if (input_start() != 0) {
        printf("iccbuttons: no controls (%s)\n", strerror(errno));
        if (stop_hmi) {
            hmi_signal(HMI_SIGCONT);
        }
        return 1;
    }
    build_lines();
    log_push("touch a row, then press the button");
    printf("iccbuttons: press panel buttons, touch a name first to bind it\n");
    draw();

    while (!leave) {
        input_wait(&e);
        on_event(&e);
        while (input_poll(&e)) {
            on_event(&e);
        }
        draw();
    }

    save_map(argc > 1 ? argv[1] : "/fs/usb0/homebrew/iccbuttons.map");
    if (stop_hmi) {
        if (saved_screen) {
            memcpy(fbm.pixels, saved_screen, fbm.stride * fbm.height);
        }
        if (own_buttons) {
            hmi_events_resume();
        }
        hmi_signal(HMI_SIGCONT);
    }
    printf("iccbuttons: bye\n");
    return 0;
}
