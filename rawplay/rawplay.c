/*
 * rawplay - the unit end of the raw livi link. it receives uncompressed rgb565 frames on
 * the composite gadget's vendor interface and puts them straight into the ipu's sdc
 * background plane (the panel), in the panel's own format, with no decode, no colour
 * conversion and no scaling. touch/knob/button go back on the same wire. the host end is
 * rawplay/rawlink.c, the protocol is in rawplay/README.md section 2.2.
 *
 * the bulk-in urbs point straight at the framebuffer mapping, so the usb controller's dma
 * writes each frame where the panel scans it out (zero copy). if the stack will not take
 * the buffer, the transport falls back to a dma staging buffer and one copy, which is
 * printed at startup and shown in --stats; it is never silent.
 *
 * usage: rawplay --usb [seconds] [--stats] [--stage] [--direct]
 */
#include "qnx.h"
#include "fb.h"
#include "console.h"
#include "input.h"
#include "raw-usb.h"
#include "hmictl.h"
#include "jlog.h"

#define LR_MAGIC0    'L'
#define LR_MAGIC1    'R'
#define LR_HEADER    16
#define LR_MSG       512    /* every LR message is padded to one max packet (see below) */
#define LR_MODE      1
#define LR_FRAME     2
#define FMT_RGB565   1
#define MODE_PAYLOAD 16

#define LI_TOUCH     1
#define LI_KNOB      2
#define LI_BUTTON    3
#define LI_READY     5
#define LI_RAW       8

/* the exit region: reserved while streaming (a visible button would mean cpu writes into
 * the buffer the usb dma and the ipu share), drawn while no frames are flowing */
#define UI_BTN_X     8
#define UI_BTN_Y     8
#define UI_BTN_W     96
#define UI_BTN_H     36

/* the stream watchdog, the same shape the serial players used: a live link that goes
 * quiet exits, a link that never delivered anything waits longer for the host to start. */
#define STARTUP_DEAD_US 15000000
#define LINK_DEAD_US     5000000
#define READY_INTERVAL_US 1000000

/* the touch driver keeps the samples from before a reader opened and hands them over in
 * one burst when the next touch arrives: a run always starts with the tap that launched
 * it, and a second run also gets the exit gesture that ended the first one. the replayed
 * gesture is delivered in a few milliseconds, so a press that short is history, not a
 * person: an exit needs the press to have actually been held, or the first touch of the
 * next run quits it before the user has done anything. */
#define EXIT_PRESS_US    50000

/* the climate panel keys (their bits on ipc channel 6, see ICC2 sdk input.c). the hvac state
 * changes on the v850's own side when one is pressed, but the only place that state is
 * shown is the hmi's climate bar, and the hmi sits stopped behind the video. so a press
 * opens a window with --hmi: streaming pauses, the hmi runs long enough to draw the bar,
 * and a fresh press keeps the window open. a held key holds it open too. */
#define HMI_WINDOW_MS    2000
#define HMI_WINDOW_STR_(x) #x
#define HMI_WINDOW_STR(x)  HMI_WINDOW_STR_(x)
static const unsigned char climate_keys[] = { 6, 7, 8, 9, 11, 12, 13, 14, 16, 17, 18, 19, 20 };

int pthread_create(unsigned *thread, const void *attr, void *(*fn)(void *), void *arg);

static struct fb fb;
static struct console con;
static int opt_stats, opt_stage = 1, opt_seconds, opt_usb;
static int opt_poll_us = 100000;
static int expected_len;

static volatile int running = 1;
static unsigned long frames, drawn, superseded;
static unsigned long long t_start, rx_us, rx_bytes, last_rx_us;
static unsigned long long src_dt_us;
static unsigned src_frames, last_ts;
static int have_ts;
static volatile int ui_quit, ui_btn_down;

/* the climate window: the input thread raises hmi_until_ms on a climate key and only the
 * main loop signals the hmi, so no frame is read between the continue and the stop */
static int opt_hmi;
static volatile unsigned long hmi_until_ms;
static volatile unsigned hmi_gen;
static volatile int climate_held;
static int hmi_active;
static int hmi_buttons;     /* buttons was restarted for the window and has to be stopped again */

/* nothing writes the bulk-out pipe from the thread that produces a message: the input
 * thread and the frame loop queue, and one writer thread drains input first, then the
 * newest ack, then the ready heartbeat. so a touch never waits behind an in-flight ack,
 * and an ack never stalls the loop that has to keep reading frames on a slow guest. a
 * full queue drops the message (touch) or the credit (ack), never blocks. */
#define OUT_MAX 32

static unsigned char qin[OUT_MAX][8], qlenin[OUT_MAX], qack[OUT_MAX][8];
static unsigned qhead_in, qtail_in, qhead_ack, qtail_ack;
static volatile int qlock, ready_pending;
static unsigned long dropped_in, dropped_ack;

/* the writer sleeps on this instead of polling: an ack queued the moment a frame lands
 * must not wait out a fixed tick before the host can see it (the host's latency sample
 * is send-to-ack, and a touch on a slow link must go out now, not at the next millisecond) */
typedef union {
    long long _align;
    unsigned char _pad[64];
} out_sem_t;

int sem_init(out_sem_t *sem, int pshared, unsigned value);
int sem_post(out_sem_t *sem);
int sem_timedwait(out_sem_t *sem, const struct timespec *abs);
static out_sem_t out_sem;

static void q_lock(void)
{
    while (__sync_lock_test_and_set(&qlock, 1)) {
    }
}

static void q_unlock(void)
{
    qlock = 0;
}

static unsigned char rxbuf[1024];
static int rxpos, rxlen;
static unsigned char *discard_buf;
static const char *exit_reason;

static long long now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static unsigned long now_ms(void)
{
    return (unsigned long)(now_us() / 1000);
}

static int is_climate_key(int bit)
{
    for (unsigned i = 0; i < sizeof climate_keys; i++) {
        if (climate_keys[i] == bit) {
            return 1;
        }
    }
    return 0;
}

/* every panel event the input thread sees passes through here. a climate key extends the
 * window; the main loop notices after the frame it is on and brings the hmi back. */
static void note_climate(const struct input_event *ev)
{
    if (!opt_hmi || ev->type != INPUT_BUTTON || !is_climate_key(ev->code)) {
        return;
    }
    if (ev->down) {
        climate_held++;
    } else if (climate_held > 0) {
        climate_held--;
    }
    __sync_synchronize();
    hmi_gen++;
    hmi_until_ms = now_ms() + HMI_WINDOW_MS;
}

static void push_input(const unsigned char *m, int n)
{
    unsigned t = qtail_in, next = (t + 1) % OUT_MAX;

    q_lock();
    if (next == qhead_in) {
        /* a full queue drops the oldest sample, not the newest: during a drag the last
         * position matters and the release must never be the event that is lost */
        qhead_in = (qhead_in + 1) % OUT_MAX;
        dropped_in++;
    }
    memcpy(qin[t], m, n);
    qlenin[t] = n;
    qtail_in = next;
    q_unlock();
    sem_post(&out_sem);
}

static int pop_input(unsigned char *m)
{
    unsigned h = qhead_in;
    int n;

    q_lock();
    if (h == qtail_in) {
        q_unlock();
        return 0;
    }
    n = qlenin[h];
    memcpy(m, qin[h], n);
    qhead_in = (h + 1) % OUT_MAX;
    q_unlock();
    return n;
}

static void push_ack(unsigned seq, int status)
{
    unsigned t = qtail_ack, next = (t + 1) % OUT_MAX;
    unsigned char *m;

    q_lock();
    if (next == qhead_ack) {
        /* the host's window only needs a newer position, so an old credit is worthless */
        qhead_ack = (qhead_ack + 1) % OUT_MAX;
        dropped_ack++;
    }
    m = qack[t];
    m[0] = 'L';
    m[1] = 'I';
    m[2] = LI_RAW;
    m[3] = seq;
    m[4] = seq >> 8;
    m[5] = seq >> 16;
    m[6] = seq >> 24;
    m[7] = (unsigned char)status;
    qtail_ack = next;
    q_unlock();
    sem_post(&out_sem);
}

static int pop_ack(unsigned char *m)
{
    unsigned h = qhead_ack;

    q_lock();
    if (h == qtail_ack) {
        q_unlock();
        return 0;
    }
    memcpy(m, qack[h], 8);
    qhead_ack = (h + 1) % OUT_MAX;
    q_unlock();
    return 8;
}

static void send_ready(void)
{
    ready_pending = 1;
    sem_post(&out_sem);
}

static void send_ack(unsigned seq, int status)
{
    push_ack(seq, status);
}

static void send_input(const struct input_event *ev)
{
    unsigned char m[8];
    int n = 0;

    if (ev->type == INPUT_TOUCH) {
        m[0] = 'L';
        m[1] = 'I';
        m[2] = LI_TOUCH;
        m[3] = ev->x;
        m[4] = ev->x >> 8;
        m[5] = ev->y;
        m[6] = ev->y >> 8;
        m[7] = ev->down;
        n = 8;
    } else if (ev->type == INPUT_KNOB && ev->code) {
        m[0] = 'L';
        m[1] = 'I';
        m[2] = LI_KNOB;
        m[3] = ev->code;
        n = 4;
    } else if (ev->type == INPUT_BUTTON) {
        m[0] = 'L';
        m[1] = 'I';
        m[2] = LI_BUTTON;
        m[3] = ev->code;
        m[4] = ev->down;
        n = 5;
    }
    if (n) {
        push_input(m, n);
    }
}

static void *writer_thread(void *arg)
{
    unsigned char m[8];

    while (running) {
        int n = pop_input(m);

        if (!n) {
            n = pop_ack(m);
        }
        if (!n && ready_pending) {
            ready_pending = 0;
            m[0] = 'L';
            m[1] = 'I';
            m[2] = LI_READY;
            n = 3;
        }
        if (n) {
            raw_usb_write(m, n);
            continue;
        }
        {
            struct timespec ts;

            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100000000;
            while (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            sem_timedwait(&out_sem, &ts);
        }
    }
    return arg;
}

static int ui_btn_hit(int x, int y)
{
    return x >= UI_BTN_X && x < UI_BTN_X + UI_BTN_W &&
           y >= UI_BTN_Y && y < UI_BTN_Y + UI_BTN_H;
}

/* the panel owns the video, so the input thread only forwards; the exit gesture is a
 * held press and release inside the top-left region. the hold is what keeps the replayed
 * exit gesture of the previous run from ending this one (see EXIT_PRESS_US).
 *
 * the driver reports a sample every millisecond while a finger is down, and a fresh
 * reader gets the samples from before it opened as a burst with the next touch. only the
 * newest sample is useful to the host, so one pass forwards at most one touch: press and
 * move samples coalesce to the newest position, and a release is forwarded only for a
 * press the host was actually told about. that is what keeps a replayed gesture, the tap
 * that launched the player included, from reaching the host as a flood of moves and
 * phantom clicks. the exit gesture still sees every sample, coalesced or not. */
static void *input_thread(void *arg)
{
    int owner = 0, host_down = 0;
    long long owner_us = 0;

    while (running) {
        struct input_event ev, pending;
        int have_pending = 0;

        while (input_poll(&ev)) {
            note_climate(&ev);
            if (ev.type != INPUT_TOUCH) {
                if (have_pending) {
                    send_input(&pending);
                    have_pending = 0;
                    host_down = 1;
                }
                send_input(&ev);
                continue;
            }
            if (ev.down) {
                if (!owner && ui_btn_hit(ev.x, ev.y)) {
                    owner = 1;
                    owner_us = now_us();
                    ui_btn_down = 1;
                    have_pending = 0;
                    if (host_down) {
                        /* the region takes the gesture over: release the host rather
                         * than leave it with a press nothing will end */
                        ev.down = 0;
                        send_input(&ev);
                        host_down = 0;
                    }
                    continue;
                }
                if (owner) {
                    have_pending = 0;
                    continue;
                }
                pending = ev;
                have_pending = 1;
                continue;
            }
            if (owner) {
                if (ui_btn_hit(ev.x, ev.y) &&
                    now_us() - owner_us >= EXIT_PRESS_US) {
                    ui_quit = 1;
                    /* the read loop may be sitting in a wait that only the link
                     * can end; this is what makes the exit gesture work on a
                     * link that never delivers anything */
                    raw_usb_stop();
                }
                ui_btn_down = 0;
                owner = 0;
                have_pending = 0;
                if (host_down) {
                    send_input(&ev);
                    host_down = 0;
                }
                continue;
            }
            if (!host_down) {
                /* a replayed release or the tail of a replayed gesture: the host never
                 * saw the press, so it must not see the release either */
                have_pending = 0;
                continue;
            }
            if (have_pending) {
                send_input(&pending);
                have_pending = 0;
            }
            send_input(&ev);
            host_down = 0;
        }
        if (have_pending) {
            send_input(&pending);
            host_down = 1;
        }
        /* event driven: the touch/panel readers post the queue, so this wakes on the next
         * sample instead of after a poll interval. the timeout is only a lost-wakeup net. */
        input_wait_event(opt_poll_us);
    }
    return arg;
}

/* ---- the byte stream -----------------------------------------------------------
 *
 * every bulk-in request is a whole number of the endpoint's max packets. the device
 * sends maxpacket-sized packets and does not know how much the host asked for, so a
 * request that is not a multiple of the maxpacket makes the host drop the tail of the
 * last packet; the ehci side then errors the transfer (measured on a real car as
 * USBD_STATUS_CMP_ERR/IO on every read, which is what "no video from the host" was).
 * a message tail shorter than a packet is taken through rxbuf, which keeps whatever
 * came with it for the next read instead of losing it. */

static int sget(unsigned char *b)
{
    if (rxpos >= rxlen) {
        /* the first bytes get the longer startup wait: rawplay is usually started just
         * before the host sender. once anything has arrived a 5 s silence is a dead
         * link, and the transport fails the read instead of blocking here forever. */
        int r = raw_usb_read(rxbuf, raw_usb_mps(),
                             rx_bytes ? LINK_DEAD_US / 1000 : STARTUP_DEAD_US / 1000);

        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            return 0;
        }
        rxpos = 0;
        rxlen = r;
    }
    *b = rxbuf[rxpos++];
    return 1;
}

/* read n bytes into dst. the leftovers already buffered are used first; what remains is
 * read in maxpacket multiples straight into dst, except a tail smaller than a packet,
 * which goes through rxbuf and leaves the overshoot buffered. */
static int read_exact(unsigned char *dst, int n)
{
    int done = 0, quiet = 0;

    if (rxlen - rxpos > 0) {
        int take = rxlen - rxpos > n ? n : rxlen - rxpos;

        memcpy(dst, rxbuf + rxpos, take);
        rxpos += take;
        done += take;
    }
    while (done < n) {
        int mps = raw_usb_mps();
        int rem = n - done;
        int r;

        if (rem >= mps) {
            int chunk = rem - rem % mps;

            r = raw_usb_read(dst + done, chunk, LINK_DEAD_US / 1000);
            if (r < 0) {
                return -1;      /* dead, quiet or stopped; without this the loop spins */
            }
            if (r > 0) {
                quiet = 0;
                done += r;
            }
        } else {
            int take;

            r = raw_usb_read(rxbuf, mps, LINK_DEAD_US / 1000);
            if (r < 0) {
                return -1;
            }
            if (r > 0) {
                quiet = 0;
                take = r > rem ? rem : r;
                memcpy(dst + done, rxbuf, take);
                rxpos = take;
                rxlen = r;
                done += take;
                continue;
            }
        }
        if (r == 0 && ++quiet >= 10) {
            return -1;              /* 5 s with nothing is a dead link, not a slow one */
        }
    }
    return 0;
}

static int discard(int n)
{
    int quiet = 0;
    int maxw = raw_usb_scratch_size();

    while (n > 0) {
        int mps = raw_usb_mps();
        int r;

        if (rxlen - rxpos > 0) {
            int take = rxlen - rxpos > n ? n : rxlen - rxpos;

            rxpos += take;
            n -= take;
            continue;
        }
        if (n >= mps) {
            int chunk = n - n % mps;

            if (chunk > maxw) {
                chunk = maxw;       /* the scratch buffer is one urb's worth */
            }
            r = raw_usb_read(discard_buf, chunk, LINK_DEAD_US / 1000);
        } else {
            /* a sub-packet tail: through rxbuf so the rest of the packet is not lost */
            r = raw_usb_read(rxbuf, mps, LINK_DEAD_US / 1000);
            if (r > 0) {
                int take = r > n ? n : r;

                rxpos = take;
                rxlen = r;
                n -= take;
                continue;
            }
        }
        if (r < 0) {
            return -1;
        }
        if (++quiet >= 10) {
            return -1;              /* nothing for ten reads: the main loop's watchdog */
        }
        if (r == 0) {
            continue;
        }
        quiet = 0;
        n -= r;
    }
    return 0;
}

/* the host pads every LR message to one max packet (512 bytes): the 16-byte header is
 * followed by pad bytes, so the payload (or the next message) starts on a packet boundary.
 * the receiver skips the pad here. it is normally already buffered (the header read is one
 * maxpacket), but if it is not, another packet is read. without the padding a frame ended
 * in a sub-packet remainder whose 512-byte read could not complete until the *next* frame
 * started arriving: that pinned every ack to the next frame (a frame period of latency)
 * and deadlocked the link when the host's window held only one frame. */
static int skip_pad(int n)
{
    while (n > 0) {
        int have = rxlen - rxpos;

        if (have > 0) {
            int take = have > n ? n : have;

            rxpos += take;
            n -= take;
            continue;
        }
        {
            int r = raw_usb_read(rxbuf, raw_usb_mps(), LINK_DEAD_US / 1000);

            if (r <= 0) {
                return -1;
            }
            rxpos = 0;
            rxlen = r;
        }
    }
    return 0;
}

/* a bulk-in transfer error leaves the stream mid-frame with no in-band resync: drop the
 * transport's buffered bytes, let it drain and reset its pipe, and let the main loop scan
 * for the next LR header. the frame that was in flight is acked stale by the caller so
 * the host's window moves on. */
static void resync_stream(void)
{
    raw_usb_resync();
    rxpos = rxlen = 0;
}

/* returns 1 on a header, 0 when no bytes are available yet, -1 on a dead link */
static int read_header(unsigned *type, unsigned *len, unsigned *seq, unsigned *ts)
{
    unsigned char h[LR_HEADER];
    unsigned char c, prev = 0;
    int i;

    for (;;) {
        int r = sget(&c);

        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            return 0;
        }
        if (c == LR_MAGIC1 && prev == LR_MAGIC0) {
            break;
        }
        prev = c;
    }
    h[0] = LR_MAGIC0;
    h[1] = LR_MAGIC1;
    for (i = 2; i < LR_HEADER; ) {
        int r = sget(&h[i]);

        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            return 0;               /* let the main loop run its watchdog */
        }
        i++;
    }
    *type = h[2];
    *len = h[4] | h[5] << 8 | h[6] << 16 | (unsigned)h[7] << 24;
    *seq = h[8] | h[9] << 8 | h[10] << 16 | (unsigned)h[11] << 24;
    *ts = h[12] | h[13] << 8 | h[14] << 16 | (unsigned)h[15] << 24;
    /* a raw picture has no mpeg start code to validate against, so the header is checked
     * against the protocol instead: reserved byte zero, a known type and a length that
     * matches the type. a false 'LR' inside picture data fails on the length. */
    if (h[3] != 0 ||
        (*type == LR_MODE && *len != MODE_PAYLOAD) ||
        (*type == LR_FRAME && (!expected_len || *len != (unsigned)expected_len)) ||
        (*type != LR_MODE && *type != LR_FRAME)) {
        return 2;                   /* not a header: keep scanning */
    }
    return 1;
}

static int read_mode(void)
{
    unsigned char p[MODE_PAYLOAD];
    unsigned w, h, stride, fmt;

    if (read_exact(p, MODE_PAYLOAD) < 0) {
        return -1;
    }
    if (skip_pad(LR_MSG - LR_HEADER - MODE_PAYLOAD) < 0) {
        return -1;
    }
    w = p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24;
    h = p[4] | p[5] << 8 | p[6] << 16 | (unsigned)p[7] << 24;
    stride = p[8] | p[9] << 8 | p[10] << 16 | (unsigned)p[11] << 24;
    fmt = p[12] | p[13] << 8 | p[14] << 16 | (unsigned)p[15] << 24;
    /* bit 8 of the format word is the transport hint: the host promises request-sized
     * bulk transfers (functionfs does; qemu's usb-livi chunked=on models it), which is
     * what lets direct mode place dma at fixed offsets. without it the staged ring is
     * used, because a short completion on the direct ring would misplace the frame. */
    if ((fmt & 0xff) != FMT_RGB565 || (int)w != fb.width || (int)h != fb.height ||
        (int)stride != (int)fb.stride) {
        printf("rawplay: host mode %ux%u stride %u fmt %u does not match panel %dx%d "
               "stride %u\n", w, h, stride, fmt, fb.width, fb.height, fb.stride);
        return -1;
    }
    expected_len = (int)(stride * h);
    raw_usb_set_direct(!opt_stage && (fmt & 0x100));
    printf("rawplay: host mode %ux%u rgb565, %d bytes/frame, path %s\n", w, h, expected_len,
           raw_usb_direct() ? "direct" : "staged");
    jlog("rawplay: host mode %ux%u rgb565, %d bytes/frame, path %s", w, h, expected_len,
         raw_usb_direct() ? "direct" : "staged");
    return 0;
}

/* ---- panel --------------------------------------------------------------------- */

static void panel_clear(pixel c)
{
    fb_fill(&fb, 0, 0, fb.width, fb.height, c);
}

static void ui_idle(void)
{
    con_init(&con, &fb);
    con_clear(&con);
    con_printf(&con, "rawplay: waiting for the host\n");
}

static void ui_button(void)
{
    pixel edge = fb_rgb(96, 116, 144);
    pixel fill = ui_btn_down ? fb_rgb(58, 18, 24) : fb_rgb(13, 19, 30);

    fb_fill(&fb, UI_BTN_X, UI_BTN_Y, UI_BTN_W, 2, edge);
    fb_fill(&fb, UI_BTN_X, UI_BTN_Y + UI_BTN_H - 2, UI_BTN_W, 2, edge);
    fb_fill(&fb, UI_BTN_X, UI_BTN_Y, 2, UI_BTN_H, edge);
    fb_fill(&fb, UI_BTN_X + UI_BTN_W - 2, UI_BTN_Y, 2, UI_BTN_H, edge);
    fb_fill(&fb, UI_BTN_X + 2, UI_BTN_Y + 2, UI_BTN_W - 4, UI_BTN_H - 4, fill);
    fb_text(&fb, UI_BTN_X + 12, UI_BTN_Y + 10, fb_rgb(228, 236, 247), "EXIT");
}

/* put the hmi and buttons back to the way the session pause holds them */
static void hmi_window_close(void)
{
    /* buttons first: it is the producer, and stopping the hmi before it would make any
     * event still generated queue in the stopped hmi for the next window or resume */
    if (hmi_buttons) {
        hmi_events_window_close();
        hmi_buttons = 0;
        printf("rawplay: buttons stopped with the hmi again\n");
        jlog("rawplay: buttons stopped with the hmi again");
    }
    hmi_signal(HMI_SIGSTOP);
    hmi_active = 0;
    printf("rawplay: climate window closed, hmi stopped\n");
    jlog("rawplay: climate window closed, hmi stopped");
}

/* while a climate window is open the main loop does not read: a frame would paint over the
 * bar the hmi is drawing (in direct mode the urbs land in the panel mapping itself). the
 * host is not acked, so it keeps only what its window holds and drops the rest, and after
 * the window the link picks up with the next frame it is given. */
static void hmi_window(void)
{
    unsigned gen = hmi_gen;

    if (!hmi_active) {
        /* buttons first, then the hmi: the session resume's order. the kill is what drops
         * the frames queued for the frozen connection, so the hmi cannot read a backlog
         * that was sitting in its own queue either. ham starts a clean buttons which then
         * feeds the live panel into the resumed hmi. */
        hmi_buttons = hmi_events_window_open();
        hmi_signal(HMI_SIGCONT);
        hmi_active = 1;
        printf("rawplay: climate key, hmi back for %d ms\n", HMI_WINDOW_MS);
        jlog("rawplay: climate key, hmi back for %d ms", HMI_WINDOW_MS);
        if (hmi_buttons) {
            printf("rawplay: buttons restarted for the hmi\n");
            jlog("rawplay: buttons restarted for the hmi");
        }
    }
    for (;;) {
        while (!ui_quit && hmi_gen == gen &&
               !((long)(now_ms() - hmi_until_ms) >= 0 && !climate_held)) {
            usleep(10000);
        }
        if (!ui_quit && hmi_gen != gen) {
            gen = hmi_gen;      /* a press arrived while the window was closing */
            continue;
        }
        break;
    }
    if (hmi_active) {
        hmi_window_close();
    }
}

static void usage(void)
{
    printf("usage: rawplay --usb [seconds] [--stats] [--direct] [--stage] [--hmi] [--urb BYTES]\n"
           "  --usb      the composite livi gadget's vendor interface (the only transport)\n"
           "  --direct   zero-copy: bulk-in urbs point straight at the panel mapping. the\n"
           "             default on the emulator, but on a real unit the first frame lands\n"
           "             and then the bulk-in pipe stops, so staged is the default there\n"
           "  --stage    read through the usbd_alloc staging ring and copy (the default)\n"
           "  --hmi      the hmi is stopped behind the video: a climate panel key pauses the\n"
           "             stream, resumes the hmi and its buttons so the climate bar shows the\n"
           "             press, and stops them again " HMI_WINDOW_STR(HMI_WINDOW_MS) " ms after\n"
           "             the last key\n"
           "  --urb      bulk-in urb size in bytes (default 262144, min 512, max 1048576)\n"
           "  --poll-us  input wake timeout in microseconds, the lost-wakeup net for the\n"
           "             event driven input thread (default 100000, min 250)\n");
}

int main(int argc, char **argv)
{
    long long last_ready = 0, last_stats = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--usb")) {
            opt_usb = 1;
        } else if (!strcmp(argv[i], "--stage")) {
            opt_stage = 1;
        } else if (!strcmp(argv[i], "--direct")) {
            opt_stage = 0;
        } else if (!strcmp(argv[i], "--hmi")) {
            opt_hmi = 1;
        } else if (!strcmp(argv[i], "--urb") && i + 1 < argc) {
            raw_usb_set_chunk((int)strtol(argv[++i], 0, 0));
        } else if (!strcmp(argv[i], "--poll-us") && i + 1 < argc) {
            opt_poll_us = (int)strtol(argv[++i], 0, 0);
            if (opt_poll_us < 250) {
                opt_poll_us = 250;
            }
            if (opt_poll_us > 1000000) {
                opt_poll_us = 1000000;
            }
        } else if (!strcmp(argv[i], "--stats")) {
            opt_stats = 1;
        } else if (argv[i][0] == '-' && argv[i][1] == '-') {
            printf("rawplay: unknown option %s\n", argv[i]);
            usage();
            return 1;
        } else {
            opt_seconds = (int)strtol(argv[i], 0, 0);
        }
    }
    if (!opt_usb) {
        printf("rawplay: the raw path runs over usb only (--usb)\n");
        usage();
        return 1;
    }
    if (fb_open(&fb) != 0) {
        printf("rawplay: can't map the framebuffer\n");
        jlog("rawplay: can't map the framebuffer");
        return 1;
    }
    panel_clear(fb_rgb(10, 14, 24));
    ui_idle();
    printf("rawplay: framebuffer %dx%d stride %u\n", fb.width, fb.height, fb.stride);
    jlog("rawplay: start, framebuffer %dx%d stride %u, urb %d", fb.width, fb.height,
         fb.stride, raw_usb_chunk());
    input_start();
    input_flush();      /* not this run's input; the rest of the driver's history arrives
                         * with the first touch, where the input thread forwards only the
                         * newest state and turns the replayed gestures away */

    if (raw_usb_start() != 0) {
        printf("rawplay: no livi usb gadget found\n");
        jlog("rawplay: no livi usb gadget found");
        return 1;
    }
    discard_buf = raw_usb_scratch();
    send_ready();
    t_start = now_us();
    last_rx_us = t_start;
    sem_init(&out_sem, 0, 0);

    {
        unsigned thread_id;

        if (pthread_create(&thread_id, 0, input_thread, 0)) {
            printf("rawplay: can't start the input thread\n");
            return 1;
        }
        if (pthread_create(&thread_id, 0, writer_thread, 0)) {
            printf("rawplay: can't start the writer thread\n");
            return 1;
        }
    }

    while (running && !ui_quit) {
        unsigned type, len, seq, ts;
        int r;
        long long now;

        /* only a deadline still in the future opens a window: after one closes its
         * deadline is stale and must not reopen it (races with the input thread are
         * caught by hmi_gen, and a press that extends it moves the deadline forward) */
        if (hmi_until_ms && !hmi_active && (long)(hmi_until_ms - now_ms()) > 0) {
            hmi_window();
            continue;
        }
        r = read_header(&type, &len, &seq, &ts);
        now = now_us();

        if (r < 0) {
            if (ui_quit) {
                break;
            }
            if (raw_usb_error()) {
                /* the transport lost part of a transfer while scanning: no in-band
                 * resync exists, so drain it and look for the next header */
                printf("rawplay: bulk-in error, resyncing\n");
                jlog("rawplay: bulk-in error, resyncing");
                resync_stream();
                continue;
            }
            /* the transport distinguishes a quiet link from a removed device; the unit
             * has no console, so this is also the line that lands in the stick log */
            exit_reason = raw_usb_quiet()
                              ? (rx_bytes ? "stream stopped" : "no video from the host")
                              : "link read failed";
            printf("rawplay: %s\n", exit_reason);
            jlog("rawplay: %s (%llu kb, %lu frames)", exit_reason, rx_bytes / 1024, frames);
            break;
        }
        if (r == 2) {
            /* a candidate that failed the header check; keep scanning */
        } else if (r == 0) {
            if (now - last_rx_us > (rx_bytes ? LINK_DEAD_US : STARTUP_DEAD_US)) {
                exit_reason = rx_bytes ? "stream stopped" : "no video from the host";
                printf("rawplay: %s\n", exit_reason);
                jlog("rawplay: %s (%llu kb, %lu frames)", exit_reason, rx_bytes / 1024,
                     frames);
                break;
            }
        } else if (type == LR_MODE) {
            if (read_mode() < 0) {
                exit_reason = "host mode does not match the panel";
                break;
            }
            ui_btn_down = 0;
        } else if (type == LR_FRAME) {
            long long t0 = now_us();

            /* the header's ts is the host's send time, so consecutive deltas are the
             * source's cadence. printing it next to the guest fps makes clear when the
             * panel is showing every frame the host sends under the icount clock. */
            if (have_ts) {
                unsigned dt = ts - last_ts;

                if (dt) {
                    src_dt_us += (unsigned long long)dt * 1000;
                    src_frames++;
                }
            }
            last_ts = ts;
            have_ts = 1;
            if (skip_pad(LR_MSG - LR_HEADER) < 0) {
                if (ui_quit) {
                    break;
                }
                if (raw_usb_error()) {
                    printf("rawplay: bulk-in error in frame padding, dropped frame %u\n", seq);
                    send_ack(seq, 2);
                    resync_stream();
                    continue;
                }
                exit_reason = "link read failed mid frame";
                printf("rawplay: %s\n", exit_reason);
                jlog("rawplay: %s (%llu kb, %lu frames)", exit_reason, rx_bytes / 1024,
                     frames);
                break;
            }
            if (len != (unsigned)expected_len) {
                printf("rawplay: frame %u bytes, expected %d, skipping\n", len, expected_len);
                if (discard(len) < 0) {
                    printf("rawplay: bulk-in error while discarding, resyncing\n");
                    resync_stream();
                    continue;
                }
                send_ack(seq, 2);
                continue;
            }
            /* a header already sitting in the refill buffer means this frame is already
             * old news: consume it without painting it (newest-wins) */
            if (rxlen - rxpos >= LR_HEADER && rxbuf[rxpos] == LR_MAGIC0 &&
                rxbuf[rxpos + 1] == LR_MAGIC1) {
                if (discard(len) < 0) {
                    printf("rawplay: bulk-in error while discarding, resyncing\n");
                    resync_stream();
                    continue;
                }
                superseded++;
                send_ack(seq, 2);
                continue;
            }
            if (read_exact((unsigned char *)fb.pixels, (int)len) < 0) {
                if (ui_quit) {
                    break;              /* the exit gesture, not a failure */
                }
                if (raw_usb_error()) {
                    /* the frame is lost mid-transfer. ack it stale so the host's window
                     * moves on, resync, and take the next header. the panel holds part
                     * of the frame; the next one overwrites it */
                    printf("rawplay: bulk-in error mid frame, dropped frame %u\n", seq);
                    jlog("rawplay: bulk-in error mid frame, dropped frame %u, %llu kb, "
                         "%lu frames", seq, rx_bytes / 1024, frames);
                    send_ack(seq, 2);
                    resync_stream();
                    continue;
                }
                exit_reason = "link read failed mid frame";
                printf("rawplay: %s\n", exit_reason);
                jlog("rawplay: %s (%llu kb, %lu frames)", exit_reason, rx_bytes / 1024,
                     frames);
                break;
            }
            rx_us += now_us() - t0;
            rx_bytes += len;
            last_rx_us = now_us();
            frames++;
            drawn++;
            send_ack(seq, 1);
        } else {
            discard((int)len);
        }

        if (opt_seconds && now_us() - t_start >= (long long)opt_seconds * 1000000) {
            break;
        }
        if (now_us() - last_ready >= READY_INTERVAL_US) {
            last_ready = now_us();
            send_ready();
        }
        if (opt_stats && now_us() - last_stats >= 5000000) {
            long long el = now_us() - t_start;

            last_stats = now_us();
            printf("rawplay: %lu frames, %lu drawn, %lu stale, %llu kb, %.1f fps "
                   "(source %.1f), %.1f ms/frame, %s%s\n", frames, drawn, superseded,
                   rx_bytes / 1024, frames * 1000000.0 / el,
                   src_dt_us ? src_frames * 1000000.0 / src_dt_us : 0.0,
                   frames ? rx_us / 1000.0 / frames : 0.0,
                   raw_usb_direct() ? "direct" : "staged",
                   raw_usb_compacted() ? " (compacted)" : "");
            src_dt_us = 0;
            src_frames = 0;
        }
    }
    running = 0;
    if (hmi_active) {
        /* the exit screen is drawn where the hmi was; a failure message would otherwise be
         * painted over by a bar the hmi still owns. the window close also stops any
         * buttons restarted for it before the hmi goes down */
        hmi_window_close();
    }
    {
        long long el = now_us() - t_start;

        panel_clear(fb_rgb(10, 14, 24));
        con_init(&con, &fb);
        if (exit_reason) {
            con_printf(&con, "rawplay: %s\n", exit_reason);
        }
        ui_button();
        printf("rawplay: %lu frames, %lu drawn, %lu stale, %llu kb in %lld ms (%.1f fps, "
               "%.1f ms/frame), %s%s, source %.1f fps, drop in/ack %lu/%lu%s%s\n", frames,
               drawn, superseded, rx_bytes / 1024, el / 1000, frames * 1000000.0 / el,
               frames ? rx_us / 1000.0 / frames : 0.0, raw_usb_direct() ? "direct" : "staged",
               raw_usb_compacted() ? " (compacted)" : "",
               src_dt_us ? src_frames * 1000000.0 / src_dt_us : 0.0,
               dropped_in, dropped_ack, exit_reason ? ", " : "", exit_reason ? exit_reason : "");
        jlog("rawplay: exit, %lu frames, %lu drawn, %llu kb, %s%s%s%s", frames, drawn,
             rx_bytes / 1024, raw_usb_direct() ? "direct" : "staged",
             raw_usb_compacted() ? " (compacted)" : "", exit_reason ? ", " : "",
             exit_reason ? exit_reason : "");
        /* the hmi is stopped behind the player and rawplay.sh resumes it as soon as this
         * returns: a failure would otherwise flash past unread on a unit with no console */
        if (exit_reason) {
            usleep(3000000);
        }
    }
    return 0;
}
