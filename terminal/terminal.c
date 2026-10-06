/*
 * terminal - a shell on the panel: a framebuffer terminal with an on-screen keyboard,
 * the unit's answer to fbterm + fbkeyboard.
 *
 * the unit has no keyboard and no pty driver, so this does not talk to a tty. it spawns
 * /bin/sh -i with pipes (posix_spawn, because fork is not implemented in a process with
 * the input threads), renders what the shell and its children write, and feeds back the
 * lines typed on the touch keyboard. with no line discipline there is no echo, no erase
 * key and no editing, so the typing, backspace, history, cursor keys and the control keys
 * are all handled here and only completed lines go to the shell. ctrl-c is a SIGINT to
 * our own process group (the shell and whatever it is running), not a byte: with stdin a
 * pipe nothing would translate it.
 *
 * the emulator covers the vt100/ansi subset the unit's own tools use: cursor movement,
 * erase, insert/delete, scroll regions, sgr colours, save/restore, the alternate screen
 * (?1049) and the dec line-drawing set. the font is the sdk's 8x16 console font.
 *
 * the keyboard sits in the bottom 208 px (5 rows), so the terminal is 100x17 with it up
 * and 100x30 with it hidden; "Kbd" hides it and the small button bottom-right brings it
 * back. touch comes from devi; the knob scrolls the history back and forward.
 *
 * run it with apps/terminal.sh, which stops the hmi when it is not stopped already, or
 * from the homebrew menu, which has stopped the hmi and owns the car guard.
 */
#include "qnx.h"
#include "fb.h"
#include "console.h"
#include "input.h"

/* not in the hand written qnx.h: the pipe/spawn pieces the shell needs. the file action
 * and attr structs have unknown layouts, so callers hand the libc's own init a generously
 * sized aligned buffer, the same way hmictl.c does for the spawn attr. */
int pipe(int fds[2]);
int posix_spawn_file_actions_init(void *fa);
int posix_spawn_file_actions_adddup2(void *fa, int fd, int newfd);
int posix_spawn_file_actions_addclose(void *fa, int fd);
int posix_spawn_file_actions_destroy(void *fa);
typedef void (*sighandler_t)(int);
sighandler_t signal(int signum, sighandler_t handler);
#define SIG_IGN  ((sighandler_t)1)
#define SIGINT   2
#define SIGPIPE  13

extern char **environ;

#define SCREEN_W 800
#define SCREEN_H 480
#define COLS     (SCREEN_W / FONT_W)        /* 100 */
#define MAX_ROWS (SCREEN_H / FONT_H)        /* 30 */

/* the keyboard: five rows of keys over the bottom of the screen, the terminal gets the
 * rest. 208 px keeps the terminal on a whole number of 16 px rows (17). */
#define KB_H     208
#define KB_ROWS  ((SCREEN_H - KB_H) / FONT_H)
#define KR0      (SCREEN_H - KB_H)
#define KH0      40
#define KR1      (KR0 + KH0)
#define KHN      42
#define KR2      (KR1 + KHN)
#define KR3      (KR2 + KHN)
#define KR4      (KR3 + KHN)

/* the line buffer: the visible screen is the last `rows` lines of it, everything above is
 * scrollback. a cell is ch, 16-colour fg/bg and the attribute bits. */
#define SCROLLBACK 500
#define LINE_N     (SCROLLBACK + MAX_ROWS)
#define ATTR_BOLD  1
#define ATTR_UNDER 2
#define ATTR_REV   4
#define ATTR_CURSOR 0x80

#define HIST 32
/* the touch driver hands a fresh reader the samples from before it opened (the tap that
 * launched us) as one burst with the next touch; that burst arrives within a millisecond
 * or two, so a short hold is the replay. the threshold stays low because a quick mouse
 * click in the emulator can be well under the 50ms rawplay uses for its exit gesture. */
#define MIN_TAP_MS 10

struct cell {
    unsigned char ch, fg, bg, attr;
};

static const struct cell blank_cell = { ' ', 7, 0, 0 };

struct term {
    struct cell *lines;         /* LINE_N * COLS cells, a ring */
    struct cell *alt;           /* the alternate screen's buffer, allocated on demand */
    int start, count;           /* logical line i is lines[((start + i) % LINE_N)] */
    int rows, cols;
    int cl, cc;                 /* cursor: logical line and column */
    unsigned char fg, bg, attr; /* current sgr state */
    int cursor_on;
    int rtop, rbot;             /* scroll region rows within the window, -1 when none */
    int view;                   /* scrollback offset, 0 is live */
    int g0_graphics;            /* dec special graphics selected in g0 */
    int lnm;                    /* newline mode: a lone \n also returns the carriage */
    /* saved cursor (esc 7 / esc 8, and ?1048) */
    int scl, scc, saved;
    unsigned char sfg, sbg, sattr;
    /* alternate screen bookkeeping */
    int alt_active;
    int m_start, m_count, m_cl, m_cc;
    unsigned char m_fg, m_bg, m_attr;
    /* parser */
    int state;
    long params[8];
    int nparam, param, private_csi;
    /* local line editing: the typed text lives in the grid at [edit_line][edit_c0..] */
    int editing, edit_line, edit_c0, edit_c1, edit_len, edit_pos;
    char edit[COLS + 1];
};

enum {
    ST_GROUND, ST_ESC, ST_CSI, ST_OSC, ST_OSC_ESC, ST_CHARSET0, ST_CHARSET1
};

static struct fb fb;
static struct term term;
static struct cell shadow[MAX_ROWS][COLS];
static int dirty_kb, kb_visible = 1, sym_layer, quit;
static pixel palette[16];

static int in_fd = -1, out_fd = -1;
static pid_t shell_pid;
static unsigned touch_ready, last_out_ms;
static volatile int out_head, out_tail, out_eof;
static unsigned char out_ring[32768];
static int touch_down, touch_key = -1;
static unsigned touch_ms;
/* the menu closes the touch device right before spawning us; if the driver has not let the
 * old connection go yet our open can come up deaf. until the first sample arrives, cycle
 * the device so a bad handoff recovers instead of leaving the keyboard dead. */
static int touch_seen;
static unsigned start_ms, regrab_ms;

/* the shell has no tty, so it prints no prompt and complains about it on stderr at
 * startup. the terminal owns the prompt (printed after the output goes quiet) and drops
 * the startup lines it recognises. */
#define PROMPT_QUIET_MS 250
#define BOOT_MAX 8192
static int at_prompt = 1;
static char boot_buf[BOOT_MAX];
static int boot_len, boot_done;

static unsigned now_ms(void);
static void show_prompt(void);
static void boot_flush(void);

/* ---------------------------------------------------------------- framebuffer --- */

static void palette_init(void)
{
    static const unsigned char rgb[16][3] = {
        {   0,   0,   0 }, { 205,   0,   0 }, {   0, 205,   0 }, { 205, 205,   0 },
        {   0,   0, 238 }, { 205,   0, 205 }, {   0, 205, 205 }, { 229, 229, 229 },
        { 127, 127, 127 }, { 255,   0,   0 }, {   0, 255,   0 }, { 255, 255,   0 },
        {  92,  92, 255 }, { 255,   0, 255 }, {   0, 255, 255 }, { 255, 255, 255 },
    };

    for (int i = 0; i < 16; i++) {
        palette[i] = fb_rgb(rgb[i][0], rgb[i][1], rgb[i][2]);
    }
}

static void draw_cell(int x, int y, struct cell c, int cursor)
{
    const unsigned short *glyph =
        font_glyphs[(c.ch >= FONT_FIRST && c.ch <= FONT_LAST ? c.ch : '?') - FONT_FIRST];
    pixel fg = palette[c.fg & 15], bg = palette[c.bg & 15];

    if ((c.attr & ATTR_BOLD) && !(c.fg & 8)) {
        fg = palette[(c.fg & 7) + 8];
    }
    if (((c.attr & ATTR_REV) != 0) ^ (cursor != 0)) {
        pixel t = fg;
        fg = bg;
        bg = t;
    }
    for (int gy = 0; gy < FONT_H; gy++) {
        pixel *row = (pixel *)((char *)fb.pixels + (y + gy) * fb.stride) + x;
        for (int gx = 0; gx < FONT_W; gx++) {
            row[gx] = (glyph[gy] & (0x8000 >> gx)) ? fg : bg;
        }
    }
    if (c.attr & ATTR_UNDER) {
        pixel *row = (pixel *)((char *)fb.pixels + (y + FONT_H - 1) * fb.stride) + x;
        for (int gx = 0; gx < FONT_W; gx++) {
            row[gx] = fg;
        }
    }
}

/* ---------------------------------------------------------------- line buffer --- */

static int window_top(struct term *t)
{
    return t->count > t->rows ? t->count - t->rows : 0;
}

static struct cell *line_at(struct term *t, int i)
{
    return t->lines + (unsigned)((t->start + i) % LINE_N) * COLS;
}

static void blank_line(struct term *t, int i)
{
    struct cell *line = line_at(t, i);

    for (int x = 0; x < COLS; x++) {
        line[x] = blank_cell;
    }
}

static void append_line(struct term *t)
{
    if (t->count == LINE_N) {
        t->start = (t->start + 1) % LINE_N;
        t->count--;
    }
    blank_line(t, t->count);
    t->count++;
}

static void insert_line(struct term *t, int p)
{
    if (p > t->count) {
        p = t->count;
    }
    if (t->count == LINE_N) {
        t->start = (t->start + 1) % LINE_N;
        t->count--;
        if (p > 0) {
            p--;
        }
    }
    for (int i = t->count; i > p; i--) {
        *line_at(t, i) = *line_at(t, i - 1);
    }
    blank_line(t, p);
    t->count++;
}

static void delete_line(struct term *t, int p)
{
    if (p < 0 || p >= t->count) {
        return;
    }
    for (int i = p; i < t->count - 1; i++) {
        *line_at(t, i) = *line_at(t, i + 1);
    }
    t->count--;
}

static void erase_cells(struct term *t, int i, int from, int to)
{
    struct cell *line;

    if (i < 0 || i >= t->count) {
        return;
    }
    line = line_at(t, i);
    for (int x = from; x < to; x++) {
        line[x] = (struct cell){ ' ', t->fg, t->bg, 0 };
    }
}

static void ensure_line(struct term *t, int i)
{
    while (t->count <= i) {
        append_line(t);
    }
}

/* ---------------------------------------------------------------- emulator --- */

static char dec_graphics(char c)
{
    switch (c) {
    case '`': case 'j': case 'k': case 'l': case 'm': case 'n':
    case 't': case 'u': case 'v': case 'w': case 'r': case 'g':
        return '+';
    case 'q': case 'p': case '0':
        return '-';
    case 'x': case '|':
        return '|';
    case 'a': case 'h': case 'i':
        return '#';
    case 'f': return 'o';
    case '~': return '.';
    case 'o': return '~';
    case 's': return '_';
    case ',': case 'y': return '<';
    case '+': case 'z': case '{': case '}': case 'e': return '>';
    case '.': return 'v';
    case '-': return '^';
    default:  return c;
    }
}

static void scroll_region(struct term *t, int *top, int *bot)
{
    int T = window_top(t);

    *top = t->rtop >= 0 ? T + t->rtop : T;
    *bot = t->rbot >= 0 ? T + t->rbot : T + t->rows - 1;
}

static void linefeed(struct term *t)
{
    int top, bot;

    scroll_region(t, &top, &bot);
    if (t->cl >= bot) {
        if (top <= window_top(t) && bot >= window_top(t) + t->rows - 1) {
            append_line(t);
            t->cl = window_top(t) + t->rows - 1;
        } else {
            delete_line(t, top);
            insert_line(t, bot);
            t->cl = bot;
        }
    } else {
        t->cl++;
        ensure_line(t, t->cl);
    }
}

static void reverse_index(struct term *t)
{
    int top, bot;

    scroll_region(t, &top, &bot);
    if (t->cl <= top) {
        insert_line(t, top);
        t->cl = top + 1;
    } else {
        t->cl--;
    }
    (void)bot;
}

static void put_char(struct term *t, unsigned char ch)
{
    if (t->g0_graphics) {
        ch = (unsigned char)dec_graphics((char)ch);
    }
    ensure_line(t, t->cl);
    line_at(t, t->cl)[t->cc] = (struct cell){ ch, t->fg, t->bg, t->attr };
    if (++t->cc >= COLS) {
        t->cc = 0;
        linefeed(t);
    }
    t->view = 0;
}

static int param(struct term *t, int i, int def)
{
    if (i >= t->nparam || t->params[i] == 0) {
        return def;
    }
    return (int)t->params[i];
}

static void set_cursor(struct term *t, int line, int col)
{
    int T = window_top(t);

    if (line < T) {
        line = T;
    }
    if (line > T + t->rows - 1) {
        line = T + t->rows - 1;
    }
    ensure_line(t, line);
    t->cl = line;
    t->cc = col < 0 ? 0 : (col >= COLS ? COLS - 1 : col);
}

static void sgr(struct term *t, int code)
{
    if (code == 0) {
        t->fg = 7;
        t->bg = 0;
        t->attr = 0;
    } else if (code == 1) {
        t->attr |= ATTR_BOLD;
    } else if (code == 4) {
        t->attr |= ATTR_UNDER;
    } else if (code == 7) {
        t->attr |= ATTR_REV;
    } else if (code == 22) {
        t->attr &= ~ATTR_BOLD;
    } else if (code == 24) {
        t->attr &= ~ATTR_UNDER;
    } else if (code == 27) {
        t->attr &= ~ATTR_REV;
    } else if (code >= 30 && code <= 37) {
        t->fg = code - 30;
    } else if (code == 39) {
        t->fg = 7;
    } else if (code >= 40 && code <= 47) {
        t->bg = code - 40;
    } else if (code == 49) {
        t->bg = 0;
    } else if (code >= 90 && code <= 97) {
        t->fg = code - 90 + 8;
    } else if (code >= 100 && code <= 107) {
        t->bg = code - 100 + 8;
    }
}

static void save_cursor(struct term *t)
{
    t->scl = t->cl;
    t->scc = t->cc;
    t->sfg = t->fg;
    t->sbg = t->bg;
    t->sattr = t->attr;
    t->saved = 1;
}

static void restore_cursor(struct term *t)
{
    if (!t->saved) {
        return;
    }
    set_cursor(t, t->scl, t->scc);
    t->fg = t->sfg;
    t->bg = t->sbg;
    t->attr = t->sattr;
}

static void alt_enter(struct term *t)
{
    struct cell *swap;

    if (t->alt_active) {
        return;
    }
    if (!t->alt) {
        t->alt = malloc(sizeof(struct cell) * LINE_N * COLS);
        if (!t->alt) {
            return;
        }
    }
    t->m_start = t->start;
    t->m_count = t->count;
    t->m_cl = t->cl;
    t->m_cc = t->cc;
    t->m_fg = t->fg;
    t->m_bg = t->bg;
    t->m_attr = t->attr;
    swap = t->lines;
    t->lines = t->alt;
    t->alt = swap;
    t->start = 0;
    t->count = 1;
    blank_line(t, 0);
    t->cl = 0;
    t->cc = 0;
    t->fg = 7;
    t->bg = 0;
    t->attr = 0;
    t->rtop = t->rbot = -1;
    t->view = 0;
    t->alt_active = 1;
}

static void alt_leave(struct term *t)
{
    struct cell *swap;

    if (!t->alt_active) {
        return;
    }
    swap = t->lines;
    t->lines = t->alt;
    t->alt = swap;
    t->start = t->m_start;
    t->count = t->m_count;
    t->cl = t->m_cl;
    t->cc = t->m_cc;
    t->fg = t->m_fg;
    t->bg = t->m_bg;
    t->attr = t->m_attr;
    t->rtop = t->rbot = -1;
    t->view = 0;
    t->alt_active = 0;
}

static void csi_mode(struct term *t, int set)
{
    if (t->private_csi != '?') {
        return;
    }
    for (int i = 0; i < t->nparam; i++) {
        switch ((int)t->params[i]) {
        case 25:
            t->cursor_on = set;
            break;
        case 47:
        case 1047:
        case 1049:
            if (set) {
                alt_enter(t);
            } else {
                alt_leave(t);
            }
            break;
        case 1048:
            if (set) {
                save_cursor(t);
            } else {
                restore_cursor(t);
            }
            break;
        default:
            break;
        }
    }
}

static void csi(struct term *t, int final)
{
    int T = window_top(t), n, top, bot;
    struct cell *line;

    switch (final) {
    case 'A':
        set_cursor(t, t->cl - param(t, 0, 1), t->cc);
        break;
    case 'B':
        set_cursor(t, t->cl + param(t, 0, 1), t->cc);
        break;
    case 'C':
        set_cursor(t, t->cl, t->cc + param(t, 0, 1));
        break;
    case 'D':
        set_cursor(t, t->cl, t->cc - param(t, 0, 1));
        break;
    case 'E':
        set_cursor(t, t->cl + param(t, 0, 1), 0);
        break;
    case 'F':
        set_cursor(t, t->cl - param(t, 0, 1), 0);
        break;
    case 'G':
    case '`':
        set_cursor(t, t->cl, param(t, 0, 1) - 1);
        break;
    case 'd':
        set_cursor(t, T + param(t, 0, 1) - 1, t->cc);
        break;
    case 'H':
    case 'f':
        set_cursor(t, T + param(t, 0, 1) - 1, param(t, 1, 1) - 1);
        break;
    case 'J':
        n = param(t, 0, 0);
        if (n == 2) {
            for (int i = T; i < t->count; i++) {
                blank_line(t, i);
            }
        } else if (n == 1) {
            for (int i = T; i < t->cl; i++) {
                blank_line(t, i);
            }
            erase_cells(t, t->cl, 0, t->cc + 1);
        } else {
            erase_cells(t, t->cl, t->cc, COLS);
            for (int i = t->cl + 1; i < t->count; i++) {
                blank_line(t, i);
            }
        }
        break;
    case 'K':
        n = param(t, 0, 0);
        if (n == 2) {
            erase_cells(t, t->cl, 0, COLS);
        } else if (n == 1) {
            erase_cells(t, t->cl, 0, t->cc + 1);
        } else {
            erase_cells(t, t->cl, t->cc, COLS);
        }
        break;
    case 'L':
        n = param(t, 0, 1);
        while (n-- > 0) {
            insert_line(t, t->cl);
        }
        break;
    case 'M':
        n = param(t, 0, 1);
        while (n-- > 0) {
            delete_line(t, t->cl);
        }
        ensure_line(t, t->cl);
        break;
    case 'P':
        n = param(t, 0, 1);
        ensure_line(t, t->cl);
        line = line_at(t, t->cl);
        if (n > COLS - t->cc) {
            n = COLS - t->cc;
        }
        memmove(&line[t->cc], &line[t->cc + n], (COLS - t->cc - n) * sizeof(struct cell));
        erase_cells(t, t->cl, COLS - n, COLS);
        break;
    case '@':
        n = param(t, 0, 1);
        ensure_line(t, t->cl);
        line = line_at(t, t->cl);
        if (n > COLS - t->cc) {
            n = COLS - t->cc;
        }
        memmove(&line[t->cc + n], &line[t->cc], (COLS - t->cc - n) * sizeof(struct cell));
        erase_cells(t, t->cl, t->cc, t->cc + n);
        break;
    case 'X':
        n = param(t, 0, 1);
        if (t->cc + n > COLS) {
            n = COLS - t->cc;
        }
        erase_cells(t, t->cl, t->cc, t->cc + n);
        break;
    case 'S':
        scroll_region(t, &top, &bot);
        n = param(t, 0, 1);
        while (n-- > 0) {
            delete_line(t, top);
            insert_line(t, bot);
        }
        break;
    case 'T':
        scroll_region(t, &top, &bot);
        n = param(t, 0, 1);
        while (n-- > 0) {
            insert_line(t, top);
            delete_line(t, bot + 1);
        }
        break;
    case 'm':
        for (int i = 0; i < t->nparam; i++) {
            sgr(t, (int)t->params[i]);
        }
        break;
    case 'r':
        top = param(t, 0, 1);
        bot = param(t, 1, t->rows);
        if (top < bot && top >= 1 && bot <= t->rows) {
            t->rtop = top - 1;
            t->rbot = bot - 1;
            set_cursor(t, T, 0);
        } else {
            t->rtop = t->rbot = -1;
        }
        break;
    case 's':
        save_cursor(t);
        break;
    case 'u':
        restore_cursor(t);
        break;
    case 'h':
        if (t->private_csi == '?') {
            csi_mode(t, 1);
        } else if (param(t, 0, 0) == 20) {
            t->lnm = 1;
        }
        break;
    case 'l':
        if (t->private_csi == '?') {
            csi_mode(t, 0);
        } else if (param(t, 0, 0) == 20) {
            t->lnm = 0;
        }
        break;
    default:
        break;
    }
}

static void feed(struct term *t, unsigned char b)
{
    switch (t->state) {
    case ST_GROUND:
        t->view = 0;            /* output (or a key) returns to the live screen */
        if (b == 0x1b) {
            t->state = ST_ESC;
        } else if (b == '\n' || b == 0x0b || b == 0x0c) {
            /* the shell's output has no tty to turn \n into \r\n for it, so newline mode
             * is on unless the program says otherwise (esc [ 20 l) */
            if (t->lnm) {
                t->cc = 0;
            }
            linefeed(t);
        } else if (b == '\r') {
            t->cc = 0;
        } else if (b == '\b') {
            if (t->cc > 0) {
                t->cc--;
            }
        } else if (b == '\t') {
            t->cc = (t->cc / 8 + 1) * 8;
            if (t->cc >= COLS) {
                t->cc = COLS - 1;
            }
        } else if (b < 0x20 || b == 0x7f) {
            /* bell, vertical tab, and the rest of the c0 set: nothing to do */
        } else if (b >= 0x80 && b < 0xc0) {
            /* utf-8 continuation: the font is ascii, drop it */
        } else {
            put_char(t, b < 0x80 ? b : '?');
        }
        break;
    case ST_ESC:
        if (b == '[') {
            t->state = ST_CSI;
            t->nparam = 0;
            t->param = 0;
            t->private_csi = 0;
        } else if (b == ']') {
            t->state = ST_OSC;
        } else if (b == '(') {
            t->state = ST_CHARSET0;
        } else if (b == ')') {
            t->state = ST_CHARSET1;
        } else if (b == '7') {
            save_cursor(t);
            t->state = ST_GROUND;
        } else if (b == '8') {
            restore_cursor(t);
            t->state = ST_GROUND;
        } else if (b == 'D') {
            linefeed(t);
            t->state = ST_GROUND;
        } else if (b == 'M') {
            reverse_index(t);
            t->state = ST_GROUND;
        } else if (b == 'E') {
            t->cc = 0;
            linefeed(t);
            t->state = ST_GROUND;
        } else if (b == 'c') {
            for (int i = 0; i < t->count; i++) {
                blank_line(t, i);
            }
            set_cursor(t, window_top(t), 0);
            t->fg = 7;
            t->bg = 0;
            t->attr = 0;
            t->rtop = t->rbot = -1;
            t->state = ST_GROUND;
        } else {
            t->state = ST_GROUND;
        }
        break;
    case ST_CSI:
        if (b >= '0' && b <= '9') {
            t->param = t->param * 10 + (b - '0');
        } else if (b == ';') {
            if (t->nparam < 7) {
                t->params[t->nparam++] = t->param;
            }
            t->param = 0;
        } else if (b == '?' || b == '>' || b == '=') {
            t->private_csi = b;
        } else if (b >= 0x40 && b <= 0x7e) {
            if (t->nparam < 8) {
                t->params[t->nparam++] = t->param;
            }
            csi(t, b);
            t->state = ST_GROUND;
        } else {
            t->state = ST_GROUND;
        }
        break;
    case ST_OSC:
        if (b == 0x07) {
            t->state = ST_GROUND;
        } else if (b == 0x1b) {
            t->state = ST_OSC_ESC;
        }
        break;
    case ST_OSC_ESC:
        t->state = b == '\\' ? ST_GROUND : ST_OSC;
        break;
    case ST_CHARSET0:
        t->g0_graphics = b == '0';
        t->state = ST_GROUND;
        break;
    case ST_CHARSET1:
        t->state = ST_GROUND;
        break;
    }
}

static void feed_str(const char *s)
{
    while (*s) {
        feed(&term, (unsigned char)*s++);
    }
}

static void show_prompt(void)
{
    feed_str("$ ");
    at_prompt = 1;
    term.view = 0;
}

static int line_has(const char *p, int n, const char *s)
{
    int m = (int)strlen(s);

    for (int i = 0; i + m <= n; i++) {
        if (memcmp(p + i, s, m) == 0) {
            return 1;
        }
    }
    return 0;
}

/* pdksh's one-time complaints about the missing tty (tty_init/j_change, both the raw and
 * the program-prefixed copy of each line) are noise in a terminal that knows there is no
 * pty; everything else the shell says at startup is passed through. */
static int is_shell_noise(const char *p, int n)
{
    static const char *noise[] = {
        "tty_init", "j_change", "No controlling tty",
        "Can't find tty file descriptor", "won't have full job control",
        "No such device or address",
    };

    for (unsigned i = 0; i < sizeof noise / sizeof noise[0]; i++) {
        if (line_has(p, n, noise[i])) {
            return 1;
        }
    }
    return 0;
}

static void boot_flush(void)
{
    int start = 0;

    if (boot_done) {
        return;
    }
    boot_done = 1;
    for (int i = 0; i <= boot_len; i++) {
        if (i == boot_len || boot_buf[i] == '\n') {
            int n = i - start;

            if (n > 0 && !is_shell_noise(boot_buf + start, n)) {
                for (int j = start; j < i; j++) {
                    feed(&term, (unsigned char)boot_buf[j]);
                }
                if (i < boot_len) {
                    feed(&term, '\n');
                }
            }
            start = i + 1;
        }
    }
    boot_len = 0;
}

/* ---------------------------------------------------------------- rendering --- */

static void term_render(struct term *t)
{
    int top = window_top(t) - t->view;

    if (top < 0) {
        top = 0;
    }
    for (int r = 0; r < t->rows; r++) {
        int li = top + r;

        for (int x = 0; x < COLS; x++) {
            struct cell want = li < t->count ? line_at(t, li)[x] : blank_cell;
            int cursor = t->view == 0 && t->cursor_on && li == t->cl && x == t->cc;

            if (cursor) {
                want.attr |= ATTR_CURSOR;
            }
            if (memcmp(&shadow[r][x], &want, sizeof want) != 0) {
                draw_cell(x * FONT_W, r * FONT_H, want, cursor);
                shadow[r][x] = want;
            }
        }
    }
}

static void term_set_rows(struct term *t, int rows)
{
    int T;

    t->rows = rows;
    T = window_top(t);
    if (t->cl < T) {
        t->cl = T;
    }
    if (t->cl > T + rows - 1) {
        t->cl = T + rows - 1;
    }
    if (t->cc >= COLS) {
        t->cc = COLS - 1;
    }
    t->rtop = t->rbot = -1;
    t->view = 0;
    t->editing = 0;
    memset(shadow, 0xff, sizeof shadow);
}

static void term_clear_screen(struct term *t)
{
    int T = window_top(t);

    for (int i = T; i < t->count; i++) {
        blank_line(t, i);
    }
    t->cl = T;
    t->cc = 0;
    t->view = 0;
    t->editing = 0;
}

static void term_init(struct term *t, int rows)
{
    t->lines = malloc(sizeof(struct cell) * LINE_N * COLS);
    t->alt = 0;
    t->start = 0;
    t->count = 1;
    t->cols = COLS;
    t->rows = rows;
    t->cl = 0;
    t->cc = 0;
    t->fg = 7;
    t->bg = 0;
    t->attr = 0;
    t->cursor_on = 1;
    t->rtop = t->rbot = -1;
    t->view = 0;
    t->g0_graphics = 0;
    t->lnm = 1;
    t->saved = 0;
    t->alt_active = 0;
    t->state = ST_GROUND;
    t->nparam = 0;
    t->editing = 0;
    t->edit_len = 0;
    t->edit_pos = 0;
    t->edit[0] = 0;
    blank_line(t, 0);
}

/* ---------------------------------------------------------------- line editing --- */

static char hist[HIST][COLS + 1];
static int hist_n, hist_pos = -1;

static void edit_sync(void)
{
    struct cell *line;

    if (!term.editing) {
        return;
    }
    ensure_line(&term, term.edit_line);
    line = line_at(&term, term.edit_line);
    for (int i = 0; i < term.edit_len; i++) {
        line[term.edit_c0 + i] = (struct cell){ (unsigned char)term.edit[i], 7, 0, 0 };
    }
    for (int i = term.edit_len; i < term.edit_c1 - term.edit_c0; i++) {
        line[term.edit_c0 + i] = blank_cell;
    }
    term.edit_c1 = term.edit_c0 + term.edit_len;
    term.edit[term.edit_len] = 0;
    term.cc = term.edit_c0 + term.edit_pos;
}

static void edit_begin(void)
{
    if (term.editing) {
        return;
    }
    if (!at_prompt) {
        show_prompt();          /* typing before the quiet timer fired */
    }
    term.editing = 1;
    term.edit_line = term.cl;
    term.edit_c0 = term.cc;
    term.edit_c1 = term.cc;
    term.edit_len = 0;
    term.edit_pos = 0;
    term.edit[0] = 0;
}

static void edit_cancel(void)
{
    term.editing = 0;
}

static void edit_insert(int ch)
{
    if (!term.editing) {
        edit_begin();
    }
    if (term.edit_len >= COLS - term.edit_c0) {
        return;
    }
    memmove(term.edit + term.edit_pos + 1, term.edit + term.edit_pos,
            term.edit_len - term.edit_pos);
    term.edit[term.edit_pos++] = (char)ch;
    term.edit_len++;
    edit_sync();
}

static void edit_backspace(void)
{
    if (!term.editing || term.edit_pos == 0) {
        return;
    }
    memmove(term.edit + term.edit_pos - 1, term.edit + term.edit_pos,
            term.edit_len - term.edit_pos);
    term.edit_pos--;
    term.edit_len--;
    edit_sync();
}

static void edit_move(int delta)
{
    if (!term.editing) {
        return;
    }
    term.edit_pos += delta;
    if (term.edit_pos < 0) {
        term.edit_pos = 0;
    }
    if (term.edit_pos > term.edit_len) {
        term.edit_pos = term.edit_len;
    }
    term.cc = term.edit_c0 + term.edit_pos;
}

static void edit_home(void)
{
    if (!term.editing) {
        return;
    }
    term.edit_pos = 0;
    term.cc = term.edit_c0;
}

static void edit_end(void)
{
    if (!term.editing) {
        return;
    }
    term.edit_pos = term.edit_len;
    term.cc = term.edit_c0 + term.edit_len;
}

static void edit_kill_line(void)
{
    if (!term.editing) {
        return;
    }
    term.edit_len = 0;
    term.edit_pos = 0;
    edit_sync();
}

static void edit_kill_to_end(void)
{
    if (!term.editing) {
        return;
    }
    term.edit_len = term.edit_pos;
    edit_sync();
}

static void edit_kill_word(void)
{
    if (!term.editing) {
        return;
    }
    while (term.edit_pos > 0 && term.edit[term.edit_pos - 1] == ' ') {
        edit_backspace();
    }
    while (term.edit_pos > 0 && term.edit[term.edit_pos - 1] != ' ') {
        edit_backspace();
    }
}

static void history_push(const char *s)
{
    int n = (int)strlen(s);

    if (n == 0) {
        hist_pos = -1;
        return;
    }
    if (n > COLS) {
        n = COLS;
    }
    if (hist_n > 0 && strcmp(hist[hist_n - 1], s) == 0) {
        hist_pos = -1;
        return;
    }
    if (hist_n == HIST) {
        memmove(hist[0], hist[1], (HIST - 1) * (COLS + 1));
        hist_n--;
    }
    memcpy(hist[hist_n], s, n);
    hist[hist_n][n] = 0;
    hist_n++;
    hist_pos = -1;
}

static void history_move(int dir)
{
    int n;

    if (hist_n == 0) {
        return;
    }
    if (!term.editing) {
        edit_begin();
        hist_pos = hist_n;
    }
    if (dir < 0) {
        if (hist_pos < 0) {
            hist_pos = hist_n;
        }
        if (hist_pos > 0) {
            hist_pos--;
        }
    } else {
        if (hist_pos < 0) {
            return;
        }
        if (++hist_pos >= hist_n) {
            hist_pos = -1;
        }
    }
    if (hist_pos < 0) {
        term.edit_len = 0;
        term.edit_pos = 0;
    } else {
        n = (int)strlen(hist[hist_pos]);
        if (n > COLS) {
            n = COLS;
        }
        memcpy(term.edit, hist[hist_pos], n);
        term.edit_len = n;
        term.edit_pos = n;
    }
    edit_sync();
}

/* ---------------------------------------------------------------- the shell --- */

static void send_to_shell(const char *buf, int n)
{
    int off = 0;

    while (off < n && in_fd >= 0) {
        int w = write(in_fd, buf + off, n - off);

        if (w <= 0) {
            break;
        }
        off += w;
    }
}

static void *out_thread(void *arg)
{
    unsigned char buf[512];

    for (;;) {
        int n = read(out_fd, buf, sizeof buf);

        if (n <= 0) {
            out_eof = 1;
            break;
        }
        for (int i = 0; i < n; i++) {
            int next = (out_head + 1) % (int)sizeof out_ring;

            while (next == out_tail) {
                usleep(1000);
            }
            out_ring[out_head] = buf[i];
            out_head = next;
        }
    }
    return arg;
}

static int out_pop(unsigned char *b)
{
    if (out_tail == out_head) {
        return 0;
    }
    *b = out_ring[out_tail];
    out_tail = (out_tail + 1) % (int)sizeof out_ring;
    return 1;
}

static char **shell_env(void)
{
    static char term_env[] = "TERM=vt100";
    static char ps1_env[] = "PS1=";     /* the terminal prints the prompt itself */
    static char ps2_env[] = "PS2=";
    static char lines_env[32];
    static char cols_env[32];
    char **env;
    int n = 0, i;

    while (environ[n]) {
        n++;
    }
    env = malloc((n + 6) * sizeof(char *));
    if (!env) {
        return environ;
    }
    for (i = 0; i < n; i++) {
        env[i] = environ[i];
    }
    snprintf(lines_env, sizeof lines_env, "LINES=%d", term.rows);
    snprintf(cols_env, sizeof cols_env, "COLUMNS=%d", COLS);
    env[n++] = term_env;
    env[n++] = ps1_env;
    env[n++] = ps2_env;
    env[n++] = lines_env;
    env[n++] = cols_env;
    env[n] = 0;
    return env;
}

static int spawn_shell(void)
{
    union {
        long long align;
        unsigned char raw[512];
    } fa;
    int to_child[2], from_child[2];
    char *argv[3];
    pid_t pid = 0;
    int err;

    if (pipe(to_child) != 0 || pipe(from_child) != 0) {
        return -1;
    }
    memset(fa.raw, 0, sizeof fa.raw);
    err = posix_spawn_file_actions_init(fa.raw);
    if (!err) err = posix_spawn_file_actions_adddup2(fa.raw, to_child[0], 0);
    if (!err) err = posix_spawn_file_actions_adddup2(fa.raw, from_child[1], 1);
    if (!err) err = posix_spawn_file_actions_adddup2(fa.raw, from_child[1], 2);
    if (!err) err = posix_spawn_file_actions_addclose(fa.raw, to_child[0]);
    if (!err) err = posix_spawn_file_actions_addclose(fa.raw, to_child[1]);
    if (!err) err = posix_spawn_file_actions_addclose(fa.raw, from_child[0]);
    if (!err) err = posix_spawn_file_actions_addclose(fa.raw, from_child[1]);
    argv[0] = "sh";
    argv[1] = "-i";
    argv[2] = 0;
    if (!err) {
        err = posix_spawn(&pid, "/bin/sh", fa.raw, 0, argv, shell_env());
    }
    posix_spawn_file_actions_destroy(fa.raw);
    close(to_child[0]);
    close(from_child[1]);
    if (err) {
        close(to_child[1]);
        close(from_child[0]);
        return -1;
    }
    in_fd = to_child[1];
    out_fd = from_child[0];
    shell_pid = pid;
    return 0;
}

/* ---------------------------------------------------------------- keyboard --- */

enum {
    K_CHAR, K_SPACE, K_ESC, K_TAB, K_CTRL, K_ALT, K_SHIFT, K_LEFT, K_UP, K_DOWN,
    K_RIGHT, K_BKSP, K_ENTER, K_HOME, K_END, K_PGUP, K_PGDN, K_SYM, K_ABC, K_KBD, K_EXIT
};

struct key {
    unsigned char kind;
    char base, shift;
    const char *label;
    short x, y, w, h;
};

#define KC(base, shift, x, y, w, h) { K_CHAR, base, shift, 0, x, y, w, h }
#define KS(kind, label, x, y, w, h) { kind, 0, 0, label, x, y, w, h }

static const struct key keys_alpha[] = {
    /* function row: esc, tab, the one-shot modifiers, arrows, hide keyboard */
    KS(K_ESC,   "Esc",    0, KR0, 80, KH0),
    KS(K_TAB,   "Tab",   80, KR0, 80, KH0),
    KS(K_CTRL,  "Ctrl", 160, KR0, 80, KH0),
    KS(K_ALT,   "Alt",  240, KR0, 80, KH0),
    KS(K_SHIFT, "Shift",320, KR0, 80, KH0),
    KS(K_LEFT,  "Left", 400, KR0, 80, KH0),
    KS(K_UP,    "Up",   480, KR0, 80, KH0),
    KS(K_DOWN,  "Down", 560, KR0, 80, KH0),
    KS(K_RIGHT, "Right",640, KR0, 80, KH0),
    KS(K_KBD,   "Kbd",  720, KR0, 80, KH0),
    /* digits */
    KC('1', '!',   0, KR1, 80, KHN), KC('2', '@',  80, KR1, 80, KHN),
    KC('3', '#', 160, KR1, 80, KHN), KC('4', '$', 240, KR1, 80, KHN),
    KC('5', '%', 320, KR1, 80, KHN), KC('6', '^', 400, KR1, 80, KHN),
    KC('7', '&', 480, KR1, 80, KHN), KC('8', '*', 560, KR1, 80, KHN),
    KC('9', '(', 640, KR1, 80, KHN), KC('0', ')', 720, KR1, 80, KHN),
    /* qwerty */
    KC('q', 'Q',   0, KR2, 80, KHN), KC('w', 'W',  80, KR2, 80, KHN),
    KC('e', 'E', 160, KR2, 80, KHN), KC('r', 'R', 240, KR2, 80, KHN),
    KC('t', 'T', 320, KR2, 80, KHN), KC('y', 'Y', 400, KR2, 80, KHN),
    KC('u', 'U', 480, KR2, 80, KHN), KC('i', 'I', 560, KR2, 80, KHN),
    KC('o', 'O', 640, KR2, 80, KHN), KC('p', 'P', 720, KR2, 80, KHN),
    /* asdf */
    KC('a', 'A',   0, KR3, 80, KHN), KC('s', 'S',  80, KR3, 80, KHN),
    KC('d', 'D', 160, KR3, 80, KHN), KC('f', 'F', 240, KR3, 80, KHN),
    KC('g', 'G', 320, KR3, 80, KHN), KC('h', 'H', 400, KR3, 80, KHN),
    KC('j', 'J', 480, KR3, 80, KHN), KC('k', 'K', 560, KR3, 80, KHN),
    KC('l', 'L', 640, KR3, 80, KHN), KS(K_BKSP, "Bksp", 720, KR3, 80, KHN),
    /* bottom */
    KS(K_SYM, "?123", 0, KR4, 80, KHN),
    KC('z', 'Z',  80, KR4, 72, KHN), KC('x', 'X', 152, KR4, 72, KHN),
    KC('c', 'C', 224, KR4, 72, KHN), KC('v', 'V', 296, KR4, 72, KHN),
    KC('b', 'B', 368, KR4, 72, KHN), KC('n', 'N', 440, KR4, 72, KHN),
    KC('m', 'M', 512, KR4, 72, KHN),
    KC(',', '<', 584, KR4, 36, KHN), KC('.', '>', 620, KR4, 36, KHN),
    KS(K_SPACE, "Space", 656, KR4, 80, KHN), KS(K_ENTER, "Enter", 736, KR4, 64, KHN),
};

static const struct key keys_sym[] = {
    KS(K_ESC,   "Esc",    0, KR0, 80, KH0),
    KS(K_TAB,   "Tab",   80, KR0, 80, KH0),
    KS(K_CTRL,  "Ctrl", 160, KR0, 80, KH0),
    KS(K_ALT,   "Alt",  240, KR0, 80, KH0),
    KS(K_SHIFT, "Shift",320, KR0, 80, KH0),
    KS(K_LEFT,  "Left", 400, KR0, 80, KH0),
    KS(K_UP,    "Up",   480, KR0, 80, KH0),
    KS(K_DOWN,  "Down", 560, KR0, 80, KH0),
    KS(K_RIGHT, "Right",640, KR0, 80, KH0),
    KS(K_KBD,   "Kbd",  720, KR0, 80, KH0),
    KC('!', '!',   0, KR1, 80, KHN), KC('@', '@',  80, KR1, 80, KHN),
    KC('#', '#', 160, KR1, 80, KHN), KC('$', '$', 240, KR1, 80, KHN),
    KC('%', '%', 320, KR1, 80, KHN), KC('^', '^', 400, KR1, 80, KHN),
    KC('&', '&', 480, KR1, 80, KHN), KC('*', '*', 560, KR1, 80, KHN),
    KC('(', '(', 640, KR1, 80, KHN), KC(')', ')', 720, KR1, 80, KHN),
    KC('-', '-',   0, KR2, 80, KHN), KC('_', '_',  80, KR2, 80, KHN),
    KC('=', '=', 160, KR2, 80, KHN), KC('+', '+', 240, KR2, 80, KHN),
    KC('[', '[', 320, KR2, 80, KHN), KC(']', ']', 400, KR2, 80, KHN),
    KC('{', '{', 480, KR2, 80, KHN), KC('}', '}', 560, KR2, 80, KHN),
    KC('\\', '\\', 640, KR2, 80, KHN), KC('|', '|', 720, KR2, 80, KHN),
    KC(';', ';',   0, KR3, 80, KHN), KC(':', ':',  80, KR3, 80, KHN),
    KC('\'', '\'', 160, KR3, 80, KHN), KC('"', '"', 240, KR3, 80, KHN),
    KC('<', '<', 320, KR3, 80, KHN), KC('>', '>', 400, KR3, 80, KHN),
    KC('/', '/', 480, KR3, 80, KHN), KC('?', '?', 560, KR3, 80, KHN),
    KS(K_BKSP, "Bksp", 720, KR3, 80, KHN),
    KS(K_ABC, "ABC", 0, KR4, 80, KHN),
    KS(K_HOME, "Home", 80, KR4, 80, KHN),
    KS(K_END, "End", 160, KR4, 80, KHN),
    KS(K_PGUP, "PgUp", 240, KR4, 80, KHN),
    KS(K_PGDN, "PgDn", 320, KR4, 80, KHN),
    KS(K_EXIT, "Exit", 400, KR4, 80, KHN),
    KS(K_SPACE, "Space", 480, KR4, 160, KHN),
    KS(K_ENTER, "Enter", 640, KR4, 160, KHN),
};

static int mod_ctrl, mod_alt, mod_shift;

static const struct key *active_keys(void)
{
    return sym_layer ? keys_sym : keys_alpha;
}

static int active_count(void)
{
    return sym_layer ? (int)(sizeof keys_sym / sizeof keys_sym[0])
                     : (int)(sizeof keys_alpha / sizeof keys_alpha[0]);
}

static int hit_key(int x, int y)
{
    const struct key *keys = active_keys();

    for (int i = 0; i < active_count(); i++) {
        if (x >= keys[i].x && x < keys[i].x + keys[i].w &&
            y >= keys[i].y && y < keys[i].y + keys[i].h) {
            return i;
        }
    }
    return -1;
}

static int hit_kbd_button(int x, int y)
{
    return x >= SCREEN_W - 76 && x < SCREEN_W - 4 && y >= SCREEN_H - 28 && y < SCREEN_H - 4;
}

static void draw_key(const struct key *k, int pressed)
{
    char one[2] = { 0, 0 };
    const char *text = k->label;
    pixel bg, fg = fb_rgb(238, 240, 244);
    int tw;

    if (k->kind == K_CHAR) {
        char c = k->base;

        if (mod_shift) {
            if (k->shift) {
                c = k->shift;
            } else if (c >= 'a' && c <= 'z') {
                c -= 32;
            }
        }
        one[0] = c;
        text = one;
    }
    bg = pressed ? fb_rgb(70, 110, 170) : fb_rgb(52, 56, 66);
    if ((k->kind == K_CTRL && mod_ctrl) || (k->kind == K_ALT && mod_alt) ||
        (k->kind == K_SHIFT && mod_shift)) {
        bg = fb_rgb(40, 130, 100);
    }
    fb_fill(&fb, k->x + 2, k->y + 2, k->w - 4, k->h - 4, bg);
    tw = (int)strlen(text) * FONT_W;
    fb_text(&fb, k->x + (k->w - tw) / 2, k->y + (k->h - FONT_H) / 2, fg, text);
}

static void draw_keyboard(void)
{
    const struct key *keys = active_keys();

    fb_fill(&fb, 0, SCREEN_H - KB_H, SCREEN_W, KB_H, fb_rgb(22, 24, 30));
    for (int i = 0; i < active_count(); i++) {
        draw_key(&keys[i], i == touch_key);
    }
}

static void draw_kbd_button(void)
{
    int x = SCREEN_W - 76, y = SCREEN_H - 28, w = 72, h = 24;

    fb_fill(&fb, x, y, w, h, touch_key == -2 ? fb_rgb(70, 110, 170) : fb_rgb(40, 44, 54));
    fb_text(&fb, x + (w - 3 * FONT_W) / 2, y + 4, fb_rgb(238, 240, 244), "kbd");
}

static void render_all(void)
{
    term_render(&term);
    if (kb_visible) {
        draw_keyboard();
    } else {
        draw_kbd_button();
    }
    dirty_kb = 0;
}

/* ---------------------------------------------------------------- input --- */

static void ctrl_key(char c)
{
    switch (c) {
    case 'c':
        killpg(0, SIGINT);
        break;
    case 'd':
        if (!term.editing) {
            close(in_fd);
            in_fd = -1;
        }
        break;
    case 'l':
        term_clear_screen(&term);
        show_prompt();
        break;
    case 'u':
        edit_kill_line();
        break;
    case 'w':
        edit_kill_word();
        break;
    case 'a':
        edit_home();
        break;
    case 'e':
        edit_end();
        break;
    case 'k':
        edit_kill_to_end();
        break;
    default:
        break;
    }
}

static void edit_commit(void)
{
    if (!boot_done) {
        boot_flush();
    }
    if (term.editing) {
        history_push(term.edit);
        send_to_shell(term.edit, term.edit_len);
        term.editing = 0;
    }
    send_to_shell("\n", 1);
    feed(&term, '\r');
    feed(&term, '\n');
    at_prompt = 0;
    last_out_ms = now_ms();
}

static void scroll_view(int delta)
{
    int T = window_top(&term);

    term.view += delta;
    if (term.view < 0) {
        term.view = 0;
    }
    if (term.view > T) {
        term.view = T;
    }
}

static void key_press(const struct key *k)
{
    if (k->kind != K_PGUP && k->kind != K_PGDN) {
        term.view = 0;          /* anything but scrolling leaves the history view */
    }
    switch (k->kind) {
    case K_CHAR: {
        char c = k->base;

        if (mod_shift) {
            if (k->shift) {
                c = k->shift;
            } else if (c >= 'a' && c <= 'z') {
                c -= 32;
            }
        }
        if (mod_ctrl) {
            ctrl_key(c);
        } else if (mod_alt) {
            send_to_shell("\033", 1);
            send_to_shell(&c, 1);
        } else {
            edit_insert((unsigned char)c);
        }
        mod_ctrl = mod_alt = mod_shift = 0;
        break;
    }
    case K_SPACE:
        edit_insert(' ');
        break;
    case K_ESC:
        send_to_shell("\033", 1);
        break;
    case K_TAB:
        send_to_shell("\t", 1);
        break;
    case K_CTRL:
        mod_ctrl = !mod_ctrl;
        break;
    case K_ALT:
        mod_alt = !mod_alt;
        break;
    case K_SHIFT:
        mod_shift = !mod_shift;
        break;
    case K_LEFT:
        edit_move(-1);
        break;
    case K_RIGHT:
        edit_move(1);
        break;
    case K_UP:
        history_move(-1);
        break;
    case K_DOWN:
        history_move(1);
        break;
    case K_HOME:
        edit_home();
        break;
    case K_END:
        edit_end();
        break;
    case K_BKSP:
        edit_backspace();
        break;
    case K_ENTER:
        edit_commit();
        break;
    case K_PGUP:
        scroll_view(term.rows - 1);
        break;
    case K_PGDN:
        scroll_view(-(term.rows - 1));
        break;
    case K_SYM:
        sym_layer = 1;
        touch_key = -1;
        break;
    case K_ABC:
        sym_layer = 0;
        touch_key = -1;
        break;
    case K_KBD:
        kb_visible = 0;
        term_set_rows(&term, MAX_ROWS);
        touch_key = -1;
        break;
    case K_EXIT:
        quit = 1;
        break;
    }
    dirty_kb = 1;
}

static void handle_input(struct input_event *e)
{
    if (e->type == INPUT_TOUCH) {
        touch_seen = 1;
        if (e->down) {
            if (!touch_down) {
                touch_down = 1;
                touch_ms = now_ms();
            }
            if (now_ms() < touch_ready) {
                return;
            }
            if (kb_visible) {
                int k = hit_key(e->x, e->y);

                if (k >= 0 && k != touch_key) {
                    touch_key = k;
                    dirty_kb = 1;
                }
            } else if (hit_kbd_button(e->x, e->y)) {
                if (touch_key != -2) {
                    touch_key = -2;
                    dirty_kb = 1;
                }
            }
        } else {
            unsigned held;

            if (!touch_down) {
                return;
            }
            touch_down = 0;
            held = now_ms() - touch_ms;
            if (now_ms() >= touch_ready && held >= MIN_TAP_MS) {
                if (kb_visible && touch_key >= 0) {
                    key_press(&active_keys()[touch_key]);
                } else if (!kb_visible && touch_key == -2) {
                    kb_visible = 1;
                    term_set_rows(&term, KB_ROWS);
                }
            }
            touch_key = -1;
            dirty_kb = 1;
        }
    } else if (e->type == INPUT_KNOB) {
        scroll_view(-e->code);
    }
}

/* ---------------------------------------------------------------- main --- */

static void *saved_screen;

static void save_screen(void)
{
    saved_screen = malloc(fb.stride * fb.height);
    if (saved_screen) {
        memcpy(saved_screen, fb.pixels, fb.stride * fb.height);
    }
}

static void restore_screen(void)
{
    if (saved_screen) {
        memcpy(fb.pixels, saved_screen, fb.stride * fb.height);
    }
}

static unsigned now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)ts.tv_sec * 1000u + (unsigned)(ts.tv_nsec / 1000000u);
}

int main(void)
{
    struct input_event e;
    unsigned thread;

    if (fb_open(&fb) != 0) {
        printf("terminal: no screen\n");
        return 1;
    }
    if (input_start() != 0) {
        printf("terminal: no controls\n");
        return 1;
    }
    palette_init();
    term_init(&term, KB_ROWS);
    memset(shadow, 0xff, sizeof shadow);
    save_screen();
    input_flush();

    if (spawn_shell() != 0) {
        printf("terminal: could not start /bin/sh\n");
        restore_screen();
        return 1;
    }
    /* after the spawn, so the shell and its children start with the default SIGINT and
     * ctrl-c can reach them; SIGPIPE is ours to ignore when the shell has gone away */
    signal(SIGINT, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    if (pthread_create(&thread, 0, out_thread, 0) != 0) {
        printf("terminal: no reader thread\n");
        kill(shell_pid, SIGKILL);
        restore_screen();
        return 1;
    }
    pthread_detach(thread);
    /* the touch driver replays the tap that launched us with the next touch; ignore the
     * first moments so it cannot type a phantom key */
    touch_ready = now_ms() + 300;
    start_ms = now_ms();

    feed_str("ICC2 terminal - /bin/sh on pipes, no pty: prompt and line editing are local.\n");
    show_prompt();
    last_out_ms = now_ms();
    render_all();
    for (;;) {
        int did = 0;
        unsigned char b;

        if (term.editing && out_tail != out_head) {
            edit_cancel();
        }
        while (out_pop(&b)) {
            last_out_ms = now_ms();
            if (!boot_done) {
                if (boot_len < BOOT_MAX) {
                    boot_buf[boot_len++] = (char)b;
                }
                if (boot_len == BOOT_MAX) {
                    boot_flush();
                }
            } else {
                feed(&term, b);
            }
            did = 1;
        }
        if (!boot_done && boot_len && now_ms() - last_out_ms > 150) {
            boot_flush();
            did = 1;
        }
        while (input_poll(&e)) {
            handle_input(&e);
            did = 1;
        }
        if (!at_prompt && !term.editing && out_tail == out_head &&
            now_ms() - last_out_ms >= PROMPT_QUIET_MS) {
            show_prompt();
            did = 1;
        }
        if (did || dirty_kb) {
            render_all();
        }
        if (!touch_seen && now_ms() - start_ms > 2000 && now_ms() - regrab_ms > 2000) {
            input_release_touch();
            input_grab_touch();
            regrab_ms = now_ms();
        }
        /* the shell is gone when its output pipe is: no waitpid here, qnx's waitpid
         * ignores WNOHANG and would block the loop (measured on the guest) */
        if (quit || out_eof) {
            break;
        }
        input_wait_event(15000);
    }

    if (shell_pid > 0) {
        kill(shell_pid, SIGKILL);
    }
    restore_screen();
    _exit(0);
}
