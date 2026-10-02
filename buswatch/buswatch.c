/*
 * buswatch: a live scope for what the car tells the unit, for working out what makes the
 * icc go to sleep (a door opening with the ignition off is the one that started this) and
 * for watching any of the other vehicle data later.
 *
 * two things are shown side by side, because either could be the trigger:
 *
 * - every frame that crosses uart3, both ways, off ipc's monitor channel (/dev/ipc/0). the
 *   known channels are decoded with the same tables the car emulator speaks (bussignals.h,
 *   baked from ../qemu/v850_messages.py by mksignals.py), the rest is tracked byte and bit
 *   by byte: every change is timestamped, so the door event and whatever moved with it are
 *   in the same list. the v850 terminates can itself, so a door bit would arrive as an ipc
 *   signal, named or not.
 *
 * - gpio3 bit 26, the standby pin pmm's isPowerPinRequestingStandby (pmm 0x10f180) polls.
 *   it is not on the uart: the v850 raises it and pmm takes the unit to standby if it stays
 *   up. bit 28 (brownout) and bit 27 (confirm) sit beside it and are shown as well.
 *
 * sleeping kills the app, so the point is the log: frames, decoded signals, pin edges and
 * marks all go to a file on the stick as they happen (buswatch.log, /tmp when there is no
 * stick). a pin edge also pushes the file out with sync(). pull the stick afterwards and
 * read what happened in the seconds before the unit went down.
 *
 * the ui: power (the pin and the live states), live (the frame feed), changes (every bit
 * that moved, newest first), signals (every decoded value), raw (hex by channel with the
 * changed bytes lit). hold freezes the view; mark drops a labelled line in the log; rec
 * toggles the file. knob and the arrow buttons scroll.
 *
 * run it from the homebrew menu (it owns the screen, stops and resumes the hmi like the
 * other apps), by hand over a running hmi, or `buswatch [seconds] [tab]` for the test bench
 * (tab 0..4 is the screen it opens on).
 */
#include "qnx.h"
#include "fb.h"
#include "console.h"
#include "input.h"
#include "hmictl.h"
#include "menu.h"
#include "menufont.h"
#include "bussignals.h"

int pthread_create(unsigned *thread, const void *attr, void *(*fn)(void *), void *arg);

#define SCREEN_W   800
#define SCREEN_H   480
#define TOP_H      48
#define NAV_W      158
#define BOT_H      46
#define BODY_X     NAV_W
#define BODY_Y     TOP_H
#define BODY_W     (SCREEN_W - NAV_W)
#define BODY_H     (SCREEN_H - TOP_H - BOT_H)
#define ROW_H      20

#define TAB_POWER   0
#define TAB_LIVE    1
#define TAB_CHANGES 2
#define TAB_SIGNALS 3
#define TAB_RAW     4
#define TAB_COUNT   5

#define FRAME_RING  2048
#define PAYLOAD_MAX 160
#define CHANGE_RING 1024
#define RAW_IDS     192
#define RAW_KEY     3
#define RAW_SHOW    6
#define WATCH_MAX   48
#define LOG_ROLL    (4u << 20)

/* gpio3, the block pmm maps at 0x53fa4000 (imx31_lpm.c) and its power pins */
#define GPIO3_PHYS  0x53FA4000ull
#define PIN_POWER   26
#define PIN_CONFIRM 27
#define PIN_BROWN   28

enum { CK_SIG, CK_RAW, CK_PIN, CK_MARK };

struct frame {
    unsigned t, seq;
    unsigned short len, olen;
    unsigned char ch, dir;
    unsigned char nchg;
    unsigned char chg[12];
    unsigned char payload[PAYLOAD_MAX];
};

struct change {
    unsigned t;
    short idx;                  /* signal index for CK_SIG, -1 otherwise */
    unsigned char kind, ch, dir;
    int oldv, newv;
    char tag[28];
};

struct rawid {
    unsigned char used, ch, dir, key[RAW_KEY];
    unsigned char have, lastlen;
    unsigned char last[24];
    unsigned t;
    unsigned changes;
};

struct sigrt {
    const struct busmsg *msg;
    const struct bussig *def;
    int value;
    unsigned char have;
    unsigned last_change;
    unsigned changes;
};

struct chanstats {
    unsigned rx, tx, rx_prev, tx_prev, rate_rx, rate_tx, last_t;
};

struct pin {
    const char *name, *shortname;
    unsigned char bit, value, have;
    unsigned last_change;
};

static pixel C_BG, C_PANEL, C_PANEL2, C_LINE, C_TEXT, C_DIM, C_ACCENT, C_AMBER, C_RED,
             C_GREEN, C_BLUE;

static struct fb fb;
static int after_hmi, own_buttons, run_seconds;
static unsigned t0, mark_count;
static volatile unsigned *gpio3;
static int gpio_ok;
static struct pin pins[3] = {
    { "gpio3.26 power", "POWER 3.26", PIN_POWER, 0, 0, 0 },
    { "gpio3.28 brown", "BROWN 3.28", PIN_BROWN, 0, 0, 0 },
    { "gpio3.27 confirm", "CONFIRM 3.27", PIN_CONFIRM, 0, 0, 0 },
};

/* the frame ring, filled by the reader thread and drained by the ui */
static struct frame frames[FRAME_RING];
static volatile unsigned ring_head, ring_tail, frame_seq;
static unsigned drain_cursor, drain_seq;
static volatile int ring_lock;

static struct change changes[CHANGE_RING];
static unsigned ch_head;

static struct rawid rawids[RAW_IDS];
static struct chanstats chstat[64];
static struct sigrt sigs[BUS_SIGNAL_COUNT];
static int msg_base[BUS_MESSAGE_COUNT];

static int tab, hold, dirty = 1;
static int scroll[TAB_COUNT];
static int watch[WATCH_MAX], nwatch;
static int sel_raw = -1;            /* -1 is every channel */
static int signals_y0 = BODY_Y + 26;
static unsigned last_draw, last_rate;
static unsigned char button_bits[8];
static int button_have;
static int sig_ignition = -1, sig_crank = -1, sig_gear = -1, sig_acm_mode = -1;
static int sig_illum = -1, sig_night = -1, sig_camera = -1, sig_khz = -1;

/* the recording file */
static int rec_fd = -1, rec_slot, rec_stick = 1;        /* slot 1 only after a roll */
static char rec_path[96];
static unsigned rec_bytes;
static volatile int rec_lock;
static int rec_want = 1;

static void lk(volatile int *l) { while (__sync_lock_test_and_set(l, 1)) { } }
static void ul(volatile int *l) { *l = 0; }

static unsigned now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)ts.tv_sec * 1000u + (unsigned)(ts.tv_nsec / 1000000u);
}

/* milliseconds since the scope started, the timestamps the ui and the log show */
static unsigned rel_ms(unsigned t)
{
    return t > t0 ? t - t0 : 0;
}

static const char *fmt_rel(char *buf, int n, unsigned t)
{
    unsigned r = rel_ms(t);

    snprintf(buf, n, "%u.%03u", r / 1000, r % 1000);
    return buf;
}

/* ------------------------------------------------------------------ recording */

static void rec_header(void);

static void rec_close(void)
{
    lk(&rec_lock);
    if (rec_fd >= 0) {
        close(rec_fd);
        rec_fd = -1;
    }
    ul(&rec_lock);
}

/* opens the current log: the stick if it is there, /tmp otherwise. every session appends
 * to the same file until it rolls over at LOG_ROLL, then the other slot takes it. */
static void rec_open(void)
{
    char path[96];

    if (!rec_want) {
        return;
    }
    rec_stick = 1;      /* a stick that showed up since the last open gets a try */
    for (int attempt = 0; attempt < 2; attempt++) {
        int stick = rec_stick && attempt == 0;

        snprintf(path, sizeof path, stick ? "/fs/usb0/homebrew/buswatch%s.log"
                                         : "/tmp/buswatch%s.log", rec_slot ? ".1" : "");
        rec_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (rec_fd >= 0) {
            snprintf(rec_path, sizeof rec_path, "%s", path);
            rec_bytes = 0;
            rec_header();
            return;
        }
        rec_stick = 0;
    }
    printf("buswatch: no writable log file\n");
}

static void rec_header(void)
{
    char head[192];
    int n;

    n = snprintf(head, sizeof head,
                 "\n# buswatch session, uptime %u ms, gpio3 %s, pin 26 high = standby request\n",
                 now_ms(), gpio_ok ? "readable" : "unavailable");
    lk(&rec_lock);
    if (rec_fd >= 0) {
        write(rec_fd, head, n);
        rec_bytes += n;
    }
    ul(&rec_lock);
}

static void rec_line(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    int n;

    if (rec_fd < 0) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if (n > (int)sizeof line) {
        n = sizeof line;
    }
    lk(&rec_lock);
    if (rec_fd >= 0) {
        write(rec_fd, line, n);
        rec_bytes += n;
        if (rec_bytes > LOG_ROLL) {
            close(rec_fd);
            rec_fd = -1;
            ul(&rec_lock);
            printf("buswatch: log rolled\n");
            rec_slot ^= 1;
            rec_open();
            return;
        }
    }
    ul(&rec_lock);
}

static void rec_frame(const struct frame *f, const unsigned char *payload, int len)
{
    char line[256], stamp[24];
    int n;

    if (rec_fd < 0) {
        return;
    }
    fmt_rel(stamp, sizeof stamp, f->t);
    n = snprintf(line, sizeof line, "%s %s ch%02x", stamp, f->dir ? "tx" : "rx", f->ch);
    for (int i = 0; i < len && n < (int)sizeof line - 5; i++) {
        n += snprintf(line + n, sizeof line - n, " %02x", payload[i]);
    }
    line[n++] = '\n';
    lk(&rec_lock);
    if (rec_fd >= 0) {
        write(rec_fd, line, n);
        rec_bytes += n;
    }
    ul(&rec_lock);
}

static void add_change(int kind, int ch, int dir, int idx, const char *tag, int oldv, int newv)
{
    struct change *c;
    char stamp[24];

    lk(&ring_lock);
    c = &changes[ch_head];
    c->t = now_ms();
    c->kind = kind;
    c->idx = idx;
    c->ch = ch;
    c->dir = dir;
    c->oldv = oldv;
    c->newv = newv;
    snprintf(c->tag, sizeof c->tag, "%s", tag);
    ch_head = (ch_head + 1) % CHANGE_RING;
    ul(&ring_lock);

    fmt_rel(stamp, sizeof stamp, c->t);
    if (kind == CK_SIG && idx >= 0) {
        rec_line("%s sig %s.%s %d -> %d\n", stamp, sigs[idx].msg->name, sigs[idx].def->name,
                 oldv, newv);
    } else if (kind == CK_RAW) {
        rec_line("%s raw %s %02x -> %02x\n", stamp, tag, oldv & 0xff, newv & 0xff);
    } else if (kind == CK_PIN) {
        rec_line("%s pin %s %d -> %d\n", stamp, tag, oldv, newv);
    } else {
        rec_line("%s mark %s\n", stamp, tag);
    }
    dirty = 1;
}

/* ------------------------------------------------------------------ decode */

static const struct busmsg *find_msg(int ch, int id)
{
    for (int i = 0; i < BUS_MESSAGE_COUNT; i++) {
        if (bus_messages[i].ch == ch && bus_messages[i].id == id) {
            return &bus_messages[i];
        }
    }
    return 0;
}

static int msg_index(const struct busmsg *m)
{
    return (int)(m - bus_messages);
}

static int same_str(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int find_sig(const char *msg, const char *sig)
{
    for (int i = 0; i < BUS_SIGNAL_COUNT; i++) {
        if (same_str(sigs[i].msg->name, msg) && same_str(sigs[i].def->name, sig)) {
            return i;
        }
    }
    return -1;
}

static const char *hint_lookup(const char *msg, const char *sig, int value)
{
    for (int i = 0; i < BUS_HINT_COUNT; i++) {
        if (bus_hints[i].value == value && strcmp(bus_hints[i].msg, msg) == 0 &&
            strcmp(bus_hints[i].sig, sig) == 0) {
            return bus_hints[i].text;
        }
    }
    return 0;
}

/* a signal stores its low byte at `byte` and grows into lower indexed bytes, matching
 * v850.py's pack(). returns -1 when the frame is too short. */
static int extract(const unsigned char *d, int len, const struct bussig *s)
{
    int nbytes = (s->bit + s->bits + 7) / 8;
    unsigned v = 0;

    for (int i = 0; i < nbytes; i++) {
        int b = (int)s->byte - i;

        if (b < 0 || b >= len) {
            return -1;
        }
        v |= (unsigned)d[b] << (8 * i);
    }
    if (s->bits < 32) {
        v = (v >> s->bit) & ((1u << s->bits) - 1);
    } else {
        v >>= s->bit;
    }
    return (int)v;
}

static int is_signed_temp(const char *name)
{
    return strcmp(name, "rtc_temperature") == 0 || strcmp(name, "outside_temp") == 0;
}

static void sig_value_str(const struct sigrt *s, char *buf, int n)
{
    const char *name = s->def->name;
    const char *hint = hint_lookup(s->msg->name, name, s->value);

    if (hint) {
        snprintf(buf, n, "%d %s", s->value, hint);
    } else if (strcmp(name, "driver_temp") == 0 || strcmp(name, "passenger_temp") == 0) {
        if (s->value == 0) snprintf(buf, n, "blank");
        else if (s->value == 1) snprintf(buf, n, "lo");
        else if (s->value == 0xfe) snprintf(buf, n, "hi");
        else if (s->value == 0xff) snprintf(buf, n, "error");
        else snprintf(buf, n, "%d.%d C", s->value / 2, (s->value % 2) * 5);
    } else if (is_signed_temp(name)) {
        snprintf(buf, n, "%d C", s->value);
    } else if (strcmp(name, "khz") == 0) {
        if (s->value >= 10000) snprintf(buf, n, "%d.%d", s->value / 1000, (s->value % 1000) / 100);
        else snprintf(buf, n, "%d", s->value);
    } else if (strcmp(name, "seconds") == 0) {
        snprintf(buf, n, "%d:%02d", s->value / 60, s->value % 60);
    } else {
        snprintf(buf, n, "%d", s->value);
    }
}

static int frame_key(const struct frame *f, unsigned char *key)
{
    key[0] = key[1] = key[2] = 0;
    if (f->len < 1) {
        return 0;
    }
    key[0] = f->payload[0];
    switch (f->ch) {
    case 2: case 3: case 4:                 /* codec: msg id, version */
    case 7: case 0x0f:                      /* version, id */
        if (f->len > 1) key[1] = f->payload[1];
        break;
    case 0x0b:                              /* version, tag, service */
        if (f->len > 1) key[1] = f->payload[1];
        if (f->len > 2) key[2] = f->payload[2];
        break;
    case 6:                                 /* version, type, (data) */
    case 5:                                 /* version, msg id, (dtc) */
        if (f->len > 1) key[1] = f->payload[1];
        break;
    }
    return 1;
}

/* text-y frames would fill the change list with every rds/console character */
static int quiet_frame(const struct frame *f)
{
    if (f->ch == 0x0c) return 1;                                        /* CANsole text */
    if (f->ch == 2 && f->len >= 1) {
        unsigned id = f->payload[0];
        if (id == 4 || id == 7 || id == 0x08 || id == 0x12) return 1;   /* rds/mp3/bt strings */
    }
    return 0;
}

static struct rawid *rawid_find(const struct frame *f, int create)
{
    unsigned char key[RAW_KEY];
    int free_slot = -1;

    if (!frame_key(f, key)) {
        return 0;
    }
    for (int i = 0; i < RAW_IDS; i++) {
        struct rawid *r = &rawids[i];

        if (!r->used) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (r->ch == f->ch && r->dir == f->dir && memcmp(r->key, key, RAW_KEY) == 0) {
            return r;
        }
    }
    if (!create || free_slot < 0) {
        return 0;
    }
    rawids[free_slot].used = 1;
    rawids[free_slot].ch = f->ch;
    rawids[free_slot].dir = f->dir;
    memcpy(rawids[free_slot].key, key, RAW_KEY);
    rawids[free_slot].have = 0;
    rawids[free_slot].changes = 0;
    return &rawids[free_slot];
}

static void decode_named(struct frame *f)
{
    const struct busmsg *m = find_msg(f->ch, f->payload[0]);

    if (!m || f->len < 2) {
        return;
    }
    for (int i = 0; i < m->nsigs; i++) {
        const struct bussig *def = &m->sigs[i];
        struct sigrt *s = &sigs[msg_base[msg_index(m)] + i];
        int v = extract(f->payload + 2, f->len - 2, def);   /* byte 0/1 are the msg id/version */

        if (v < 0) {
            continue;
        }
        if (def->bits == 8 && is_signed_temp(def->name) && (v & 0x80)) {
            v -= 256;
        }
        if (!s->have || s->value != v) {
            if (s->have) {
                add_change(CK_SIG, f->ch, f->dir, (int)(s - sigs), "", s->value, v);
                s->changes++;
            }
            s->value = v;
            s->have = 1;
            s->last_change = f->t;
        }
    }
}

static void decode_buttons(struct frame *f)
{
    if (f->len < 3) {
        return;
    }
    if (f->payload[1] == 0 && f->len >= 4) {
        int bytes = f->len - 2;

        if (bytes > 8) bytes = 8;
        if (!button_have) {
            memcpy(button_bits, f->payload + 2, bytes);
            button_have = 1;
            return;
        }
        for (int bit = 0; bit < bytes * 8; bit++) {
            int is = f->payload[2 + bit / 8] >> (bit % 8) & 1;
            int was = button_bits[bit / 8] >> (bit % 8) & 1;

            if (is == was) {
                continue;
            }
            button_bits[bit / 8] ^= 1 << (bit % 8);
            {
                const char *name = "?";
                char tag[28];

                for (int i = 0; i < BUS_BUTTON_COUNT; i++) {
                    if (bus_buttons[i].bit == bit) {
                        name = bus_buttons[i].name;
                        break;
                    }
                }
                snprintf(tag, sizeof tag, "button %s", name);
                add_change(CK_PIN, f->ch, f->dir, -1, tag, was, is);
            }
        }
    } else if (f->payload[1] == 1 && f->len >= 4) {
        char tag[28];
        int d = (signed char)f->payload[2];

        snprintf(tag, sizeof tag, "rotary %+d", d);
        add_change(CK_PIN, f->ch, f->dir, -1, tag, 0, d);
    }
}

static void decode_gps(struct frame *f)
{
    static int last_speed = -1, last_gear = -1;
    char tag[28];
    int speed = -1, gear = -1;

    if (f->len < 4) {
        return;
    }
    if (f->payload[0] == 2) {
        gear = f->payload[1];
        speed = f->payload[2] | (f->payload[3] << 8);
    } else if (f->payload[0] == 1 && f->len >= 6) {
        speed = f->payload[1] | (f->payload[2] << 8);
        gear = f->payload[4];
    }
    if (speed < 0) {
        return;
    }
    if (gear != last_gear) {
        snprintf(tag, sizeof tag, "gps gear %d%s", gear, gear == 0x0b ? " reverse" : "");
        add_change(CK_RAW, f->ch, f->dir, -1, tag, last_gear < 0 ? 0 : last_gear, gear);
        last_gear = gear;
    }
    /* speed arrives around 10 hz, only note the km/h boundaries */
    if (speed / 128 != last_speed / 128) {
        snprintf(tag, sizeof tag, "gps speed %d.%d km/h", speed / 128, (speed % 128) * 100 / 128);
        add_change(CK_RAW, f->ch, f->dir, -1, tag, last_speed < 0 ? 0 : last_speed / 128, speed / 128);
        last_speed = speed;
    }
}

static void decode_time(struct frame *f)
{
    static int last_min = -1;
    char tag[28];

    if (f->len < 8 || f->payload[1] < 2 || f->payload[1] > 5) {
        return;
    }
    if (f->payload[3] == last_min) {
        return;
    }
    last_min = f->payload[3];
    snprintf(tag, sizeof tag, "time %02u:%02u %02u/%02u/20%02u", f->payload[4], f->payload[3],
             f->payload[5], f->payload[6] + 1, f->payload[7]);
    add_change(CK_RAW, f->ch, f->dir, -1, tag, 0, 0);
}

static void decode_raw(struct frame *f, const unsigned char *payload, int len)
{
    struct rawid *r = rawid_find(f, 1);
    int shown = len > 24 ? 24 : len;
    int nchg = 0, quiet = quiet_frame(f);

    if (!r) {
        return;
    }
    if (!r->have) {
        memcpy(r->last, payload, shown);
        r->lastlen = shown;
        r->have = 1;
        r->t = f->t;
        return;
    }
    if (!quiet && shown != r->lastlen) {
        char tag[28];

        snprintf(tag, sizeof tag, "ch%02x %02x %02x len", f->ch, r->key[0], r->key[1]);
        add_change(CK_RAW, f->ch, f->dir, -1, tag, r->lastlen, shown);
    }
    for (int i = 0; i < shown && i < r->lastlen; i++) {
        if (payload[i] == r->last[i]) {
            continue;
        }
        if (f->nchg < sizeof f->chg) {
            f->chg[f->nchg++] = i;
        }
        if (!quiet && nchg < 4 && shown <= 48) {
            char tag[28];

            snprintf(tag, sizeof tag, "ch%02x %02x %02x b%d", f->ch, r->key[0], r->key[1], i);
            add_change(CK_RAW, f->ch, f->dir, -1, tag, r->last[i], payload[i]);
        }
        nchg++;
    }
    if (nchg) {
        r->changes++;
        r->t = f->t;
    }
    memcpy(r->last, payload, shown);
    r->lastlen = shown;
}

static void decode(struct frame *f)
{
    if (f->dir) {
        chstat[f->ch].tx++;
    } else {
        chstat[f->ch].rx++;
    }
    chstat[f->ch].last_t = f->t;
    f->nchg = 0;

    if (!f->dir && (f->ch == 2 || f->ch == 3 || f->ch == 4)) {
        decode_named(f);
    }
    if (!f->dir && f->ch == 6) {
        decode_buttons(f);
    }
    if (!f->dir && f->ch == 0x0e) {
        decode_gps(f);
    }
    if (!f->dir && f->ch == 7) {
        decode_time(f);
    }
    decode_raw(f, f->payload, f->len);
}

/* ------------------------------------------------------------------ reader */

static void *ipc_reader(void *arg)
{
    static unsigned char buf[1002];
    int fd = -1;
    unsigned silent = 0;

    (void)arg;
    for (;;) {
        int n;

        if (fd < 0) {
            fd = open("/dev/ipc/0", O_RDONLY);
            if (fd < 0) {
                usleep(250000);
                continue;
            }
            silent = 0;
        }
        n = read(fd, buf, sizeof buf);
        if (n < 0) {
            close(fd);
            fd = -1;
            usleep(50000);
            continue;
        }
        if (n < 2) {
            if (++silent > 2500) {
                close(fd);
                fd = -1;
                silent = 0;
                continue;
            }
            usleep(2000);
            continue;
        }
        silent = 0;
        {
            struct frame *fr;
            unsigned idx, len = (unsigned)n - 2;
            unsigned copy = len > PAYLOAD_MAX ? PAYLOAD_MAX : len;

            lk(&ring_lock);
            idx = ring_head;
            fr = &frames[idx];
            fr->t = now_ms();
            fr->seq = ++frame_seq;
            fr->ch = buf[0];
            fr->dir = buf[1] ? 1 : 0;
            fr->len = copy;
            fr->olen = len > 0xffff ? 0xffff : len;
            memcpy(fr->payload, buf + 2, copy);
            ring_head = (ring_head + 1) % FRAME_RING;
            if (ring_head == ring_tail) {
                ring_tail = (ring_tail + 1) % FRAME_RING;
            }
            ul(&ring_lock);
            rec_frame(fr, buf + 2, (int)len);
        }
    }
    return arg;
}

/* ------------------------------------------------------------------ gpio */

static void poll_gpio(void)
{
    unsigned dr;

    if (!gpio_ok) {
        return;
    }
    dr = *gpio3;
    for (int i = 0; i < 3; i++) {
        int v = (dr >> pins[i].bit) & 1;

        if (pins[i].have && v == pins[i].value) {
            continue;
        }
        if (!pins[i].have) {
            pins[i].have = 1;
            pins[i].value = v;
            pins[i].last_change = now_ms();
            if (i == 0 && v) {
                sync();     /* started mid-request: the countdown is already running */
            }
            continue;
        }
        add_change(CK_PIN, 0, 0, -1, pins[i].name, pins[i].value, v);
        pins[i].value = v;
        pins[i].last_change = now_ms();
        if (i == 0) {
            /* the standby request: get the evidence onto the stick before the unit goes */
            sync();
        }
    }
}

/* ------------------------------------------------------------------ drawing */

static void init_colours(void)
{
    C_BG = fb_rgb(9, 12, 17);
    C_PANEL = fb_rgb(21, 27, 36);
    C_PANEL2 = fb_rgb(30, 38, 51);
    C_LINE = fb_rgb(52, 62, 78);
    C_TEXT = fb_rgb(233, 239, 246);
    C_DIM = fb_rgb(133, 146, 162);
    C_ACCENT = fb_rgb(0, 198, 160);
    C_AMBER = fb_rgb(255, 176, 32);
    C_RED = fb_rgb(233, 66, 66);
    C_GREEN = fb_rgb(64, 208, 96);
    C_BLUE = fb_rgb(96, 168, 255);
}

static void fillr(int x, int y, int w, int h, pixel c) { fb_fill(&fb, x, y, w, h, c); }

static int text(int x, int y, pixel c, const char *s)
{
    fb_text(&fb, x, y, c, s);
    return fb_text_width(s);
}

static int textf(int x, int y, pixel c, const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return text(x, y, c, buf);
}

/* the framebuffer has no clip: a string drawn past x=800 wraps into the next row, so
 * anything that can run long gets cut here */
static void text_clip(int x, int y, pixel c, const char *s, int maxw)
{
    char buf[128];
    int max = maxw / FONT_W;

    if (max < 1) {
        return;
    }
    if (max > (int)sizeof buf - 1) {
        max = sizeof buf - 1;
    }
    snprintf(buf, max + 1, "%s", s);
    text(x, y, c, buf);
}

static void smalltext(int x, int baseline, pixel c, const char *s)
{
    menu_text_face(&fb, menu_font_small_glyphs, menu_font_small_bits, MENU_FONT_SMALL_ASCENT,
                   1, x, baseline, c, s);
}

static void bigtext(int x, int baseline, pixel c, const char *s)
{
    menu_text_face(&fb, menu_font_big_glyphs, menu_font_big_bits, MENU_FONT_BIG_ASCENT,
                   1, x, baseline, c, s);
}

static void card(int x, int y, int w, int h, pixel bg, pixel edge)
{
    fillr(x, y, w, h, bg);
    fillr(x, y, w, 1, edge);
    fillr(x, y + h - 1, w, 1, edge);
}

static void arrow(int x, int y, int w, int h, int up, pixel c)
{
    for (int i = 0; i < h; i++) {
        int ww = w * (up ? h - i : i + 1) / h;

        if (ww < 1) ww = 1;
        fillr(x + (w - ww) / 2, y + i, ww, 1, c);
    }
}

static const char *tab_name(int t)
{
    static const char *names[TAB_COUNT] = { "POWER", "LIVE", "CHANGES", "SIGNALS", "RAW" };

    return names[t];
}

static void draw_top(void)
{
    char buf[192];
    unsigned rx = 0, tx = 0, age;
    int w;

    fillr(0, 0, SCREEN_W, TOP_H, C_PANEL);
    fillr(0, TOP_H - 1, SCREEN_W, 1, C_LINE);
    menu_text(&fb, 14, 35, C_ACCENT, "Bus Watch");
    smalltext(176, 33, C_DIM, "icc <-> v850 ipc + standby pin");

    for (int i = 0; i < 64; i++) {
        rx += chstat[i].rate_rx;
        tx += chstat[i].rate_tx;
    }
    age = rel_ms(now_ms()) / 1000;
    snprintf(buf, sizeof buf, "rx %u/s tx %u/s  %02u:%02u  ring %u  %s", rx, tx, age / 60,
             age % 60, (ring_head - ring_tail + FRAME_RING) % FRAME_RING,
             rec_fd >= 0 ? "REC" : "rec off");
    w = fb_text_width(buf);
    text(SCREEN_W - 104 - w, 16, rec_fd >= 0 ? C_RED : C_DIM, buf);

    card(SCREEN_W - 92, 8, 80, 32, C_PANEL2, C_LINE);
    text(SCREEN_W - 92 + (80 - fb_text_width("EXIT")) / 2, 16, C_TEXT, "EXIT");
}

static void draw_nav(void)
{
    fillr(0, TOP_H, NAV_W, SCREEN_H - TOP_H, C_PANEL);
    fillr(NAV_W - 1, TOP_H, 1, SCREEN_H - TOP_H, C_LINE);
    for (int i = 0; i < TAB_COUNT; i++) {
        int y = TOP_H + 8 + i * 74;

        if (i == tab) {
            fillr(0, y, NAV_W - 1, 66, C_PANEL2);
            fillr(0, y, 5, 66, C_ACCENT);
        } else {
            fillr(0, y, NAV_W - 1, 66, C_PANEL);
            fillr(0, y + 65, NAV_W - 1, 1, C_BG);
        }
        smalltext(22, y + 30, i == tab ? C_TEXT : C_DIM, tab_name(i));
        text(22, y + 36, i == tab ? C_ACCENT : C_LINE, "................");
    }
}

static void draw_bottom(void)
{
    static const char *labels[6] = { "HOLD", "MARK", "REC", "CLEAR", "UP", "DOWN" };
    int x = BODY_X + 8;
    int y = SCREEN_H - BOT_H + 6;

    fillr(0, SCREEN_H - BOT_H, SCREEN_W, BOT_H, C_PANEL);
    fillr(0, SCREEN_H - BOT_H, SCREEN_W, 1, C_LINE);
    for (int i = 0; i < 6; i++) {
        int w = i < 4 ? 104 : 56;
        pixel edge = C_LINE, fg = C_TEXT;

        if (i == 0 && hold) { edge = C_AMBER; fg = C_AMBER; }
        if (i == 2) { edge = rec_fd >= 0 ? C_RED : C_LINE; fg = rec_fd >= 0 ? C_RED : C_DIM; }
        card(x, y, w, 34, C_PANEL2, edge);
        text(x + (w - fb_text_width(labels[i])) / 2, y + 9, fg, labels[i]);
        x += w + 8;
    }
}

static void draw_power(void)
{
    int y = BODY_Y + 10;
    int cw = (BODY_W - 4 * 12) / 3;

    for (int i = 0; i < 3; i++) {
        int x = BODY_X + 12 + i * (cw + 12);
        struct pin *p = &pins[i];
        int hi = p->value != 0;
        pixel c = !p->have ? C_DIM : (i == 0 ? (hi ? C_RED : C_GREEN) : (hi ? C_AMBER : C_DIM));
        const char *state = !p->have ? "n/a" : (hi ? "HIGH" : "LOW");
        char sub[48] = "";

        card(x, y, cw, 100, C_PANEL, C_LINE);
        smalltext(x + 12, y + 22, C_DIM, p->shortname);
        bigtext(x + 12, y + 72, c, state);
        if (i == 0) {
            if (hi) {
                snprintf(sub, sizeof sub, "standby requested %us", (now_ms() - p->last_change) / 1000);
            } else {
                snprintf(sub, sizeof sub, "unit active");
            }
        } else if (i == 1) {
            snprintf(sub, sizeof sub, "%s", hi ? "brownout" : "ok");
        } else {
            snprintf(sub, sizeof sub, "%s", hi ? "confirm high" : "confirm low");
        }
        smalltext(x + 12, y + 92, C_DIM, sub);
    }
    y += 112;

    {
        static const struct { int *slot; const char *label; const char *msg, *sig; } items[] = {
            { &sig_ignition, "IGNITION", "vehicle.state", "ignition" },
            { &sig_crank, "CRANK", "vehicle.state", "crank" },
            { &sig_gear, "GEAR", "vehicle.state", "gear" },
            { &sig_acm_mode, "AUDIO", "acm.tuner", "mode" },
            { &sig_khz, "TUNER", "acm.tuner", "khz" },
            { &sig_illum, "ILLUM", "vehicle.state", "illumination" },
            { &sig_night, "NIGHT", "vehicle.config", "night" },
            { &sig_camera, "CAMERA", "vehicle.config", "camera_fitted" },
        };
        int gw = (BODY_W - 5 * 12) / 4;

        for (int i = 0; i < 8; i++) {
            int x = BODY_X + 12 + (i % 4) * (gw + 12);
            int gy = y + (i / 4) * 56;
            int idx = *items[i].slot;

            card(x, gy, gw, 50, C_PANEL, C_LINE);
            smalltext(x + 8, gy + 17, C_DIM, items[i].label);
            if (idx >= 0 && sigs[idx].have) {
                char v[32];

                sig_value_str(&sigs[idx], v, sizeof v);
                text(x + 8, gy + 26, C_TEXT, v);
            } else {
                text(x + 8, gy + 26, C_LINE, "-");
            }
        }
        y += 2 * 56 + 4;
    }

    fillr(BODY_X + 12, y - 6, BODY_W - 24, 1, C_LINE);
    smalltext(BODY_X + 14, y + 10, C_ACCENT, "RECENT CHANGES");
    y += 20;
    {
        int rows = (BODY_Y + BODY_H - y - 4) / ROW_H;
        int shown = 0;

        if (rows > 9) rows = 9;
        for (int i = 0; i < CHANGE_RING && shown < rows; i++) {
            const struct change *c = &changes[(ch_head - 1 - i + CHANGE_RING) % CHANGE_RING];
            char stamp[24];

            if (!c->t) {
                break;
            }
            if (c->kind == CK_RAW && !(c->ch == 2 || c->ch == 3 || c->ch == 4)) {
                continue;
            }
            fmt_rel(stamp, sizeof stamp, c->t);
            textf(BODY_X + 14, y + shown * ROW_H, C_DIM, "[%s]", stamp);
            if (c->kind == CK_SIG && c->idx >= 0) {
                char a[24], b[24];
                struct sigrt *s = &sigs[c->idx];
                int save = s->value;

                s->value = c->oldv;
                sig_value_str(s, a, sizeof a);
                s->value = c->newv;
                sig_value_str(s, b, sizeof b);
                s->value = save;
                textf(BODY_X + 105, y + shown * ROW_H, C_TEXT, "%s.%s", s->msg->name, s->def->name);
                textf(BODY_X + 430, y + shown * ROW_H, C_AMBER, "%s -> %s", a, b);
            } else if (c->kind == CK_PIN) {
                textf(BODY_X + 105, y + shown * ROW_H, C_AMBER, "%s  %d -> %d", c->tag, c->oldv,
                      c->newv);
            } else if (c->kind == CK_MARK) {
                textf(BODY_X + 105, y + shown * ROW_H, C_ACCENT, "%s", c->tag);
            } else {
                textf(BODY_X + 105, y + shown * ROW_H, C_TEXT, "%s  %02x -> %02x", c->tag,
                      c->oldv & 0xff, c->newv & 0xff);
            }
            shown++;
        }
        if (!shown) {
            text(BODY_X + 14, y, C_LINE, "nothing yet");
        }
    }
}

static void frame_summary(const struct frame *f, char *buf, int n)
{
    const struct busmsg *m = find_msg(f->ch, f->payload[0]);

    buf[0] = 0;
    if (m && f->len >= 2) {
        int base = msg_base[msg_index(m)];
        int used = snprintf(buf, n, "%s", m->name);

        for (int i = 0; i < m->nsigs && i < 3; i++) {
            struct sigrt *s = &sigs[base + i];

            if (!s->have) {
                continue;
            }
            if (used + (int)strlen(s->def->name) + 12 >= n) {
                break;
            }
            used += snprintf(buf + used, n - used, " %s=%d", s->def->name, s->value);
        }
    } else if (f->ch == 6 && f->len >= 2) {
        if (f->payload[1] == 0) {
            int used = 0;

            for (int i = 0; i < BUS_BUTTON_COUNT && used < n - 12; i++) {
                int bit = bus_buttons[i].bit;

                if (bit < (int)sizeof button_bits * 8 && (button_bits[bit / 8] >> (bit % 8) & 1)) {
                    used += snprintf(buf + used, n - used, "%s ", bus_buttons[i].name);
                }
            }
            if (!used) snprintf(buf, n, "buttons released");
        } else {
            snprintf(buf, n, "rotary");
        }
    } else if (f->ch == 0x0e && f->len >= 4 && f->payload[0] == 2) {
        unsigned sp = f->payload[2] | (f->payload[3] << 8);

        snprintf(buf, n, "speed %u.%u km/h%s", sp / 128, (sp % 128) * 100 / 128,
                 f->payload[1] == 0x0b ? " reverse" : "");
    } else if (f->ch == 7 && f->len >= 8) {
        snprintf(buf, n, "time %02u:%02u:%02u", f->payload[4], f->payload[3], f->payload[2]);
    } else if (f->ch == 5 && f->len >= 4) {
        snprintf(buf, n, "dtc %02x%02x", f->payload[2], f->payload[3]);
    } else if (f->ch == 0x0c && f->len > 2 && f->payload[0] == 4) {
        int k = f->len - 2 > 24 ? 24 : f->len - 2;

        snprintf(buf, n, "%.*s", k, (const char *)(f->payload + 2));
    } else if (f->ch == 0x0b && f->len >= 4) {
        snprintf(buf, n, "diag tag %02x svc %02x type %02x", f->payload[1], f->payload[2],
                 f->payload[3]);
    } else if (f->ch == 0x0f && f->len >= 2) {
        static const char *ids[] = { "usb routine", "version", "serial", "reset", "board ver",
                                     "map part", "tmc part" };

        snprintf(buf, n, "updater %s", f->payload[1] < 7 ? ids[f->payload[1]] : "?");
    }
}

static void draw_live(void)
{
    int y = BODY_Y + 8;
    int rows = (BODY_H - 8 - 22) / ROW_H;
    unsigned n = 0, idx;
    char buf[128];

    snprintf(buf, sizeof buf, "%s    %u frames seen", hold ? "HOLD - press HOLD to follow"
                                                           : "following", frame_seq);
    text(BODY_X + 10, y, hold ? C_AMBER : C_DIM, buf);
    y += 22;

    idx = (ring_head + FRAME_RING - 1) % FRAME_RING;
    while (idx != ring_tail && n < (unsigned)rows) {
        const struct frame *f = &frames[idx];
        char stamp[24], sum[64];
        int x = BODY_X + 10;
        pixel dc = f->dir ? C_BLUE : C_ACCENT;

        if (f->ch == 0) {                       /* link acks, not car data */
            idx = (idx + FRAME_RING - 1) % FRAME_RING;
            continue;
        }
        fmt_rel(stamp, sizeof stamp, f->t);
        x += textf(x, y, C_DIM, "%s", stamp);
        x += text(x, y, dc, f->dir ? " tx " : " rx ");
        x += textf(x, y, C_DIM, "ch%02x ", f->ch);
        for (int i = 0; i < f->len && i < 10; i++) {
            int hot = 0;

            for (int k = 0; k < f->nchg; k++) {
                if (f->chg[k] == i) hot = 1;
            }
            x += textf(x, y, hot ? C_AMBER : C_TEXT, "%02x ", f->payload[i]);
            x += 4;
        }
        frame_summary(f, sum, sizeof sum);
        if (x < SCREEN_W - 80) {
            text_clip(x + 12, y, C_TEXT, sum, SCREEN_W - 8 - (x + 12));
        }
        y += ROW_H;
        n++;
        idx = (idx + FRAME_RING - 1) % FRAME_RING;
    }
    if (!n) {
        text(BODY_X + 10, y, C_LINE, "no frames yet - is the ipc monitor readable?");
    }
}

static void draw_changes(void)
{
    int y = BODY_Y + 8;
    int rows = (BODY_H - 8 - 22) / ROW_H;
    int shown = 0;

    text(BODY_X + 10, y, C_DIM, "every signal, byte and pin edge, newest first");
    y += 22;
    for (int i = 0; i < CHANGE_RING && shown < rows; i++) {
        const struct change *c = &changes[(ch_head - 1 - i + CHANGE_RING) % CHANGE_RING];
        char stamp[24];
        int x = BODY_X + 10;

        if (!c->t) {
            break;
        }
        fmt_rel(stamp, sizeof stamp, c->t);
        x += textf(x, y, C_DIM, "[%s]", stamp);
        x += text(x, y, c->kind == CK_MARK ? C_ACCENT : (c->kind == CK_RAW ? C_DIM : C_AMBER),
                  c->kind == CK_SIG ? " SIG " : c->kind == CK_RAW ? " RAW " :
                  c->kind == CK_PIN ? " PIN " : "MARK ");
        if (c->kind == CK_SIG && c->idx >= 0) {
            char a[24], b[24];
            struct sigrt *s = &sigs[c->idx];
            int save = s->value;

            s->value = c->oldv;
            sig_value_str(s, a, sizeof a);
            s->value = c->newv;
            sig_value_str(s, b, sizeof b);
            s->value = save;
            textf(x, y, C_TEXT, "%s.%s", s->msg->name, s->def->name);
            textf(BODY_X + 470, y, C_TEXT, "%s -> %s", a, b);
        } else {
            text(x, y, C_TEXT, c->tag);
            if (c->kind != CK_MARK) {
                textf(BODY_X + 470, y, C_TEXT, "%02x -> %02x", c->oldv & 0xff, c->newv & 0xff);
            }
        }
        y += ROW_H;
        shown++;
    }
    if (!shown) {
        text(BODY_X + 10, y, C_LINE, "no changes yet");
    }
}

static int watched(int idx)
{
    for (int i = 0; i < nwatch; i++) {
        if (watch[i] == idx) return 1;
    }
    return 0;
}

static void toggle_watch(int idx)
{
    for (int i = 0; i < nwatch; i++) {
        if (watch[i] == idx) {
            memmove(&watch[i], &watch[i + 1], (nwatch - i - 1) * sizeof watch[0]);
            nwatch--;
            dirty = 1;
            return;
        }
    }
    if (nwatch < WATCH_MAX) {
        watch[nwatch++] = idx;
    }
    dirty = 1;
}

static void draw_signals(void)
{
    int y = BODY_Y + 6;
    int first, rows;

    if (nwatch) {
        text(BODY_X + 10, y + 4, C_ACCENT, "WATCHED");
        y += 20;
        for (int i = 0; i < nwatch && i < 3; i++) {
            const struct sigrt *s = &sigs[watch[i]];
            char v[32], stamp[24];

            sig_value_str(s, v, sizeof v);
            fmt_rel(stamp, sizeof stamp, s->last_change);
            textf(BODY_X + 10, y, C_AMBER, "* %s.%s", s->msg->name, s->def->name);
            textf(BODY_X + 380, y, C_TEXT, "%-16s", v);
            textf(BODY_X + 512, y, C_LINE, "%s", stamp);
            y += ROW_H;
        }
        text(BODY_X + 10, y + 6, C_ACCENT, "ALL SIGNALS (tap a row to watch)");
        y += 26;
    } else {
        text(BODY_X + 10, y + 4, C_DIM, "ALL SIGNALS - tap a row to put it on the watch list");
        y += 26;
    }
    signals_y0 = y;
    first = scroll[TAB_SIGNALS];
    rows = (BODY_Y + BODY_H - y - 6) / ROW_H;
    for (int i = 0; i < rows && first + i < BUS_SIGNAL_COUNT; i++) {
        const struct sigrt *s = &sigs[first + i];
        char v[40], stamp[24];

        if (!s->have) {
            continue;
        }
        sig_value_str(s, v, sizeof v);
        fmt_rel(stamp, sizeof stamp, s->last_change);
        textf(BODY_X + 10, y + i * ROW_H, watched(first + i) ? C_AMBER : C_DIM, "%s.%s",
              s->msg->name, s->def->name);
        textf(BODY_X + 380, y + i * ROW_H, C_TEXT, "%-16s", v);
        textf(BODY_X + 512, y + i * ROW_H, C_LINE, "%s", stamp);
        textf(BODY_X + 596, y + i * ROW_H, C_LINE, "%u", s->changes);
    }
    if (first > 0) {
        arrow(BODY_X + BODY_W - 26, y, 12, 10, 1, C_ACCENT);
    }
    if (first + rows < BUS_SIGNAL_COUNT) {
        arrow(BODY_X + BODY_W - 26, BODY_Y + BODY_H - 22, 12, 10, 0, C_ACCENT);
    }
}

static int channel_match(const struct frame *f, int ch)
{
    return ch < 0 || f->ch == ch;
}

static void draw_raw(void)
{
    static const int buttons[] = { -1, 2, 3, 4, 6, 7, 0x0b, 0x0c, 0x0e, 0x0f, 5, 0x0d };
    int y = BODY_Y + 6, bw = 46, x = BODY_X + 8;

    for (int i = 0; i < (int)(sizeof buttons / sizeof buttons[0]); i++) {
        char label[8];
        int sel = sel_raw == buttons[i];

        if (buttons[i] < 0) snprintf(label, sizeof label, "all");
        else snprintf(label, sizeof label, "%02x", buttons[i]);
        card(x, y, bw, 26, sel ? C_ACCENT : C_PANEL2, sel ? C_ACCENT : C_LINE);
        text(x + (bw - fb_text_width(label)) / 2, y + 5, sel ? C_BG : C_TEXT, label);
        x += bw + 4;
    }
    y += 34;
    text(BODY_X + 10, y, C_ACCENT, "LAST FRAMES");
    y += 20;
    {
        unsigned idx = (ring_head + FRAME_RING - 1) % FRAME_RING;
        int shown = 0;

        while (idx != ring_tail && shown < RAW_SHOW) {
            const struct frame *f = &frames[idx];

            if (f->ch == 0 || !channel_match(f, sel_raw)) {
                idx = (idx + FRAME_RING - 1) % FRAME_RING;
                continue;
            }
            {
                char stamp[24];
                int xx = BODY_X + 10;

                fmt_rel(stamp, sizeof stamp, f->t);
                xx += textf(xx, y, C_DIM, "%s %s ch%02x", stamp, f->dir ? "tx" : "rx", f->ch);
                for (int i = 0; i < f->len && i < 16; i++) {
                    int hot = 0;

                    for (int k = 0; k < f->nchg; k++) {
                        if (f->chg[k] == i) hot = 1;
                    }
                    xx += textf(xx, y, hot ? C_AMBER : C_TEXT, " %02x", f->payload[i]);
                }
            }
            y += ROW_H;
            shown++;
            idx = (idx + FRAME_RING - 1) % FRAME_RING;
        }
        if (!shown) {
            text(BODY_X + 10, y, C_LINE, "no frames on this channel");
            y += ROW_H;
        }
    }
    y += 6;
    text(BODY_X + 10, y, C_ACCENT, "BYTE / BIT CHANGES (amber = changed)");
    y += 20;
    {
        int rows = (BODY_Y + BODY_H - y - 4) / ROW_H;
        int shown = 0;

        for (int i = 0; i < CHANGE_RING && shown < rows; i++) {
            const struct change *c = &changes[(ch_head - 1 - i + CHANGE_RING) % CHANGE_RING];
            char stamp[24];

            if (!c->t) break;
            if (c->kind != CK_RAW && c->kind != CK_PIN) continue;
            if (sel_raw >= 0 && c->kind == CK_RAW && c->ch != sel_raw) continue;
            if (sel_raw >= 0 && c->kind == CK_PIN && c->ch != sel_raw) continue;
            fmt_rel(stamp, sizeof stamp, c->t);
            textf(BODY_X + 10, y, C_DIM, "[%s]", stamp);
            textf(BODY_X + 105, y, C_TEXT, "%s", c->tag);
            textf(BODY_X + 470, y, C_AMBER, "%02x -> %02x", c->oldv & 0xff, c->newv & 0xff);
            y += ROW_H;
            shown++;
        }
        if (!shown) {
            text(BODY_X + 10, y, C_LINE, "nothing changed yet");
        }
    }
}

static void draw(void)
{
    fillr(0, 0, SCREEN_W, SCREEN_H, C_BG);
    draw_top();
    draw_nav();
    switch (tab) {
    case TAB_POWER: draw_power(); break;
    case TAB_LIVE: draw_live(); break;
    case TAB_CHANGES: draw_changes(); break;
    case TAB_SIGNALS: draw_signals(); break;
    case TAB_RAW: draw_raw(); break;
    }
    draw_bottom();
    last_draw = now_ms();
    dirty = 0;
}

/* ------------------------------------------------------------------ input */

static void scroll_by(int delta)
{
    int max = 0;

    if (delta == 0) {
        return;
    }
    switch (tab) {
    case TAB_CHANGES: max = CHANGE_RING - 10; break;
    case TAB_SIGNALS: max = BUS_SIGNAL_COUNT - 10; break;
    case TAB_RAW: max = 1024; break;
    default: break;
    }
    scroll[tab] += delta;
    if (scroll[tab] < 0) scroll[tab] = 0;
    if (scroll[tab] > max) scroll[tab] = max;
    dirty = 1;
}

static void do_mark(void)
{
    char tag[28];

    snprintf(tag, sizeof tag, "MARK #%u", ++mark_count);
    add_change(CK_MARK, 0, 0, -1, tag, 0, 0);
    sync();
}

static void do_clear(void)
{
    lk(&ring_lock);
    ch_head = 0;
    memset(changes, 0, sizeof changes);
    ul(&ring_lock);
    memset(chstat, 0, sizeof chstat);
    memset(rawids, 0, sizeof rawids);
    for (int i = 0; i < BUS_SIGNAL_COUNT; i++) {
        sigs[i].changes = 0;
    }
    dirty = 1;
}

static void tap(int x, int y)
{
    if (x >= SCREEN_W - 92 && y >= 8 && y < 40) {
        run_seconds = -1;
        return;
    }
    if (x < NAV_W && y >= TOP_H) {
        int t = (y - TOP_H - 8) / 74;

        if (t >= 0 && t < TAB_COUNT && y >= TOP_H + 8 + t * 74 && y < TOP_H + 8 + t * 74 + 66) {
            tab = t;
            dirty = 1;
        }
        return;
    }
    if (y >= SCREEN_H - BOT_H) {
        static const int widths[6] = { 104, 104, 104, 104, 56, 56 };
        int bx = BODY_X + 8;
        int by = SCREEN_H - BOT_H + 6;

        for (int i = 0; i < 6; i++) {
            if (x >= bx && x < bx + widths[i] && y >= by && y < by + 34) {
                switch (i) {
                case 0: hold = !hold; break;
                case 1: do_mark(); break;
                case 2:
                    rec_want = !rec_want;
                    if (rec_want) rec_open();
                    else rec_close();
                    break;
                case 3: do_clear(); break;
                case 4: scroll_by(-(BODY_H / ROW_H - 2)); break;
                case 5: scroll_by(BODY_H / ROW_H - 2); break;
                }
                dirty = 1;
                return;
            }
            bx += widths[i] + 8;
        }
        return;
    }
    if (tab == TAB_SIGNALS) {
        if (y >= signals_y0) {
            int row = (y - signals_y0) / ROW_H + scroll[TAB_SIGNALS];

            if (row < BUS_SIGNAL_COUNT) {
                toggle_watch(row);
            }
        }
    } else if (tab == TAB_RAW) {
        static const int buttons[] = { -1, 2, 3, 4, 6, 7, 0x0b, 0x0c, 0x0e, 0x0f, 5, 0x0d };
        int bw = 46, bx = BODY_X + 8;

        for (int i = 0; i < (int)(sizeof buttons / sizeof buttons[0]); i++) {
            if (y >= BODY_Y + 6 && y < BODY_Y + 32 && x >= bx && x < bx + bw) {
                sel_raw = buttons[i];
                dirty = 1;
                return;
            }
            bx += bw + 4;
        }
    } else if (tab == TAB_LIVE) {
        hold = !hold;
        dirty = 1;
    }
}

static void input_event(struct input_event *e)
{
    if (e->type == INPUT_TOUCH && e->down) {
        tap(e->x, e->y);
    } else if (e->type == INPUT_KNOB) {
        scroll_by(e->code > 0 ? 3 : -3);
    } else if (e->type == INPUT_BUTTON && e->down) {
        if (e->code == 27) do_mark();
        else if (e->code == 28) hold = !hold;
        else if (e->code == 30) scroll_by(3);
        else if (e->code == 29) scroll_by(-3);
        dirty = 1;
    }
}

/* ------------------------------------------------------------------ setup */

static void init_signals(void)
{
    int base = 0;

    for (int m = 0; m < BUS_MESSAGE_COUNT; m++) {
        msg_base[m] = base;
        for (int i = 0; i < bus_messages[m].nsigs; i++, base++) {
            struct sigrt *s = &sigs[base];

            s->msg = &bus_messages[m];
            s->def = &bus_messages[m].sigs[i];
            s->value = 0;
            s->have = 0;
            s->last_change = now_ms();
            s->changes = 0;
        }
    }
    sig_ignition = find_sig("vehicle.state", "ignition");
    sig_crank = find_sig("vehicle.state", "crank");
    sig_gear = find_sig("vehicle.state", "gear");
    sig_acm_mode = find_sig("acm.tuner", "mode");
    sig_khz = find_sig("acm.tuner", "khz");
    sig_illum = find_sig("vehicle.state", "illumination");
    sig_night = find_sig("vehicle.config", "night");
    sig_camera = find_sig("vehicle.config", "camera_fitted");
}

int main(int argc, char **argv)
{
    struct input_event e;
    unsigned thread;
    void *saved = 0;
    unsigned last_stick_check = 0;

    if (argc > 1) {
        int v = (int)strtol(argv[1], 0, 0);

        if (v > 0) run_seconds = v;
    }
    if (argc > 2) {
        int v = (int)strtol(argv[2], 0, 0);

        if (v >= 0 && v < TAB_COUNT) tab = v;
    }
    if (fb_open(&fb) != 0) {
        printf("buswatch: no framebuffer\n");
        return 1;
    }
    init_colours();
    t0 = now_ms();
    init_signals();
    gpio3 = mmap_device_memory(0, 0x20, PROT_READ | PROT_WRITE | PROT_NOCACHE, 0, GPIO3_PHYS);
    gpio_ok = gpio3 != MAP_FAILED;
    if (!gpio_ok) {
        printf("buswatch: gpio3 not mappable, power pins unavailable\n");
    }
    input_start();
    after_hmi = access("/hmi_", 0) == 0;
    if (after_hmi) {
        own_buttons = hmi_events_pause();
        hmi_signal(HMI_SIGSTOP);
        saved = malloc(fb.stride * fb.height);
        if (saved) {
            memcpy(saved, fb.pixels, fb.stride * fb.height);
        }
    }
    rec_open();
    pthread_create(&thread, 0, ipc_reader, 0);

    for (;;) {
        int did = 0;

        while (input_poll(&e)) {
            input_event(&e);
            did = 1;
        }
        poll_gpio();

        /* ingest whatever the reader has, decode it on this thread. a held view does not
         * decode, so the ring can pass it by; skip to what is still there. */
        if (!hold) {
            if (frame_seq - drain_seq > FRAME_RING) {
                drain_seq = frame_seq - ((ring_head - ring_tail + FRAME_RING) % FRAME_RING);
                drain_cursor = ring_tail;
            }
            while (drain_seq != frame_seq) {
                decode(&frames[drain_cursor]);
                drain_cursor = (drain_cursor + 1) % FRAME_RING;
                drain_seq++;
                did = 1;
            }
        }
        if (now_ms() - last_rate >= 1000) {
            for (int i = 0; i < 64; i++) {
                chstat[i].rate_rx = chstat[i].rx - chstat[i].rx_prev;
                chstat[i].rate_tx = chstat[i].tx - chstat[i].tx_prev;
                chstat[i].rx_prev = chstat[i].rx;
                chstat[i].tx_prev = chstat[i].tx;
            }
            last_rate = now_ms();
            did = 1;
        }
        if (did || now_ms() - last_draw > 500) {
            dirty = 1;
        }
        if (dirty && now_ms() - last_draw >= 60) {
            draw();
        }
        if (run_seconds > 0 && (int)(rel_ms(now_ms()) / 1000) >= run_seconds) {
            break;
        }
        if (now_ms() - last_stick_check > 2000) {
            if (rec_want && rec_fd < 0) {
                rec_open();
            }
            last_stick_check = now_ms();
        }
        usleep(10000);
    }

    rec_close();
    if (after_hmi) {
        if (saved) {
            memcpy(fb.pixels, saved, fb.stride * fb.height);
            free(saved);
        }
        if (own_buttons) {
            hmi_events_resume();
        }
        hmi_signal(HMI_SIGCONT);
    } else {
        fillr(0, 0, SCREEN_W, SCREEN_H, 0);
    }
    printf("buswatch: done, %u marks, log %s\n", mark_count, rec_path);
    return 0;
}
