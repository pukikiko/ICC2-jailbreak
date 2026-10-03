/*
 * rawlink - the host side of the raw livi link in one C binary: both the raw sender
 * (capture + LR framing + LI handling + touch injection) and the configfs composite
 * gadget + functionfs byte bridge. it replaced the python hosts (livi/rawstream.py,
 * livi/rawgadget.py and livi/usbgadget.py, since removed) and can run either role alone
 * or both in one process:
 *
 *     rawlink stream /tmp/livi-raw.sock --usb --wayland wayland-livi --stats
 *     rawlink gadget --stick usb.img --sock /tmp/livi-raw.sock
 *     sudo rawlink run --wayland wayland-livi --stats   # gadget + sender, the appliance mode
 *
 * the video source is the headless weston session: `--wayland` captures its output over
 * the wayland socket with the weston_capture_v1 protocol (shm buffer, converted to the
 * panel's rgb565 in-process), so there is no X server, no x11grab and no ffmpeg on that
 * path. `--test`, `--motion` and `--file` remain the synthetic sources (ffmpeg) for tests
 * and benchmarks. the wire protocol is LR MODE/FRAME downstream, LI touch/knob/ready/ack
 * upstream (rawplay/README.md section 2.2); the unit end is rawplay/rawplay.c.
 *
 * it is meant to sit on an orange pi wired to the car and not fall over: bad LI messages
 * are skipped, a lost client or a unix socket that disappears is retried, the wayland
 * capture reconnects when weston restarts, ffmpeg is respawned if its pipe closes (the
 * synthetic sources), the gadget is rebound if functionfs unbinds, SIGPIPE is ignored and
 * SIGHUP (a closed terminal) does not stop it; only SIGINT/SIGTERM exit cleanly, and a
 * real setup failure (no configfs, no udc) returns nonzero.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "weston-output-capture-client-protocol.h"

/* ---- wire protocol, rawplay/README.md section 2.2 -------------------------------------- */

#define LR_HDR        16
#define LR_MODE       1
#define LR_FRAME      2
#define FMT_RGB565    1
#define MODE_LEN      16
/* frame messages pad the 16-byte header to one max packet, and mode messages are padded
 * to one as well, so *every* LR message on the wire is a whole number of 512-byte packets
 * and the receiver's reads stay aligned (see queue_frame and rawplay.c's skip_pad). */
#define MSG_HDR       512

#define LI_TOUCH      1
#define LI_KNOB       2
#define LI_BUTTON     3
#define LI_READY      5
#define LI_RAW        8

/* linux evdev key codes (input-event-codes.h). the panel buttons with a livi binding are
 * tapped through the weston-touch module, which hands the compositor a real key event
 * (no x11 and no xtest anywhere on this path). */
#define KEY_BACKSPACE 14
#define KEY_H         35
#define KEY_V         47
#define KEY_B         48
#define KEY_N         49

/* functionfs abi, include/uapi/linux/usb/functionfs.h */
#define FFS_DESCRIPTORS_MAGIC 1
#define FFS_STRINGS_MAGIC     2
#define FFS_UNBIND            1
#define FFS_ENABLE            2
#define FFS_DISABLE           3

#define GADGET_ROOT   "/sys/kernel/config/usb_gadget"
#define DEFAULT_FFS   "/dev/ffs-liviraw"
#define DEFAULT_SOCK  "/tmp/livi-raw.sock"
#define DEFAULT_NAME  "liviraw"
#define TOUCH_SOCK    "/tmp/livi-touch.sock"
#define VID           0x1209
#define PID           0x1cc2
#define MPS           512            /* whole maxpackets only; 512 is also a multiple of 64 */
#define SENT_MAX      256
#define LAT_MAX       200
#define HDR_NO_SEQ    0xffffffffu

/* functionfs keeps exactly one bulk-in request per endpoint and a synchronous write()
 * waits for that request to complete (drivers/usb/gadget/function/f_fs.c, the !aio
 * branch), so the time between two writes is usb transfer + completion irq + the
 * writer's wakeup. with a 64 KB chunk and a phone that sits in deep cpu idle that gap is
 * milliseconds and the link trickles: the panel paints the frame line by line. a bigger
 * chunk divides the wakeup count, and a pipe that holds a whole 768 KB frame lets the
 * sender fill ep1 while the previous chunk is still moving. */
#define EP_BUF_LEN    (256 * 1024)
#define VID_PIPE_BYTES (1024 * 1024)

static const int li_needed[256] = { [LI_TOUCH] = 8, [LI_KNOB] = 4, [LI_BUTTON] = 5,
                                    [LI_READY] = 3, [LI_RAW] = 8 };

static volatile sig_atomic_t g_stop;

/* ---- logging, the rawstream:/usbgadget: role prefixes the python hosts used ---------- */

static void slogf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void tlogf(const char *fmt, ...)
{
    char stamp[16];
    time_t t = time(NULL);
    struct tm tm;
    va_list ap;

    localtime_r(&t, &tm);
    strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);
    fprintf(stderr, "%s ", stamp);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ---- small helpers -------------------------------------------------------------------- */

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static unsigned now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)(((unsigned long long)ts.tv_sec * 1000
                       + (unsigned long long)ts.tv_nsec / 1000000) & 0xffffffffu);
}

static void msleep(int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);

    if (fl < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int fd_write_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;

    while (len) {
        ssize_t n = write(fd, p, len);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void s_copy(char *dst, size_t n, const char *src)
{
    size_t l = strlen(src);

    if (n == 0) {
        return;
    }
    if (l >= n) {
        l = n - 1;
    }
    memcpy(dst, src, l);
    dst[l] = 0;
}

static void s_cat(char *dst, size_t n, const char *suffix)
{
    size_t l = strlen(dst);

    if (l >= n) {
        return;
    }
    s_copy(dst + l, n - l, suffix);
}

static void pjoin(char *dst, size_t n, const char *base, const char *suffix)
{
    s_copy(dst, n, base);
    s_cat(dst, n, suffix);
}

static int mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    char *p;

    if (len == 0 || len >= sizeof tmp) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(tmp, path, len + 1);
    for (p = tmp + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = 0;
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
            return -1;
        }
        *p = '/';
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static void unlink_quiet(const char *path)
{
    if (unlink(path) < 0 && errno != ENOENT) {
        /* best effort: a stale path is not an error in any caller here */
    }
}

static int write_file(const char *path, const char *data)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    int rc = 0;

    if (fd < 0) {
        tlogf("usbgadget: open %s: %s", path, strerror(errno));
        return -1;
    }
    if (fd_write_all(fd, data, strlen(data)) < 0) {
        tlogf("usbgadget: write %s: %s", path, strerror(errno));
        rc = -1;
    }
    close(fd);
    return rc;
}

static void put32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void build_hdr(unsigned char *h, int type, unsigned len, unsigned seq, unsigned ts)
{
    h[0] = 'L';
    h[1] = 'R';
    h[2] = (unsigned char)type;
    h[3] = 0;
    put32(h + 4, len);
    put32(h + 8, seq);
    put32(h + 12, ts);
}

/* ---- configuration ---------------------------------------------------------------------- */

struct cfg {
    const char *path;                /* stream: the unix socket (gadget or qemu chardev) */
    const char *file, *wayland, *events;
    int usb, use_test, use_motion, no_chunked, no_inject, stats, flip;
    int width, height, fps, window;
    double seconds;
    char stick[PATH_MAX], sock[PATH_MAX], name[64], udc[64], ffs[PATH_MAX];
    int rw, dry, dump_desc;
};

static void default_stick(char *buf, size_t n)
{
    struct stat st;

    if (stat("usb.img", &st) == 0) {
        snprintf(buf, n, "usb.img");
    } else if (stat("../usb.img", &st) == 0) {
        snprintf(buf, n, "../usb.img");
    } else {
        snprintf(buf, n, "usb.img");
    }
}

static void default_ffs(char *buf, size_t n)
{
    const char *env = getenv("LIVI_RAW_FFS");

    snprintf(buf, n, "%s", (env && *env) ? env : DEFAULT_FFS);
}

static const char *opt_arg(int argc, char **argv, int *i, const char *name)
{
    const char *a = argv[*i];
    size_t n = strlen(name);

    if (strcmp(a, name) == 0) {
        if (*i + 1 >= argc) {
            fprintf(stderr, "rawlink: %s wants a value\n", name);
            exit(2);
        }
        return argv[++(*i)];
    }
    if (strncmp(a, name, n) == 0 && a[n] == '=') {
        return a + n + 1;
    }
    return NULL;
}

static const char *stream_usage(void)
{
    return "usage: rawlink stream PATH [--usb] [--wayland W|--test|--motion|--file F]\n"
           "       [--width N] [--height N] [--fps N] [--window N] [--seconds S]\n"
           "       [--events FILE] [--stats] [--no-inject] [--no-chunked] [--flip]\n";
}

static const char *gadget_usage(void)
{
    return "usage: rawlink gadget [--stick IMG] [--sock PATH] [--udc NAME] [--name NAME]\n"
           "       [--ffs-mount PATH] [--rw] [--dry-run] [--dump-descriptors]\n";
}

static const char *run_usage(void)
{
    return "usage: rawlink run [gadget options] [stream options]\n"
           "       [--stick IMG] [--sock PATH] [--udc NAME] [--name NAME] [--rw]\n"
           "       [--wayland W|--file F|--test|--motion] [--width N] [--height N]\n"
           "       [--fps N] [--window N] [--seconds S] [--events FILE] [--stats]\n"
           "       [--no-inject] [--no-chunked] [--flip] [--dry-run] [--dump-descriptors]\n";
}

/* ---- input injection: the weston-touch module's socket (wayland, no x11) ----------------- */

struct inject {
    int enabled;
    int touch_fd;
    int touch_down;
    double touch_retry_at;
};

static const char *touch_path(void)
{
    const char *env = getenv("LIVI_TOUCH_SOCK");

    return (env && *env) ? env : TOUCH_SOCK;
}

static int touch_connect(struct inject *in)
{
    const char *path = touch_path();
    struct sockaddr_un addr;
    int fd;

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    in->touch_fd = fd;
    in->touch_down = 0;
    return 0;
}

static int touch_write(struct inject *in, const char *cmd)
{
    size_t len = strlen(cmd), off = 0;

    while (off < len) {
        ssize_t n = send(in->touch_fd, cmd + off, len - off, MSG_NOSIGNAL);

        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = { in->touch_fd, POLLOUT, 0 };

            if (poll(&p, 1, 50) > 0) {
                continue;
            }
        }
        return -1;
    }
    return 0;
}

/* reconnect the weston-touch socket on use. the module is loaded by weston, which is a
 * unit of its own and can restart (boot races, crashes), and its socket file can outlive
 * the listener; deciding at startup would leave touch dead for the whole process when
 * rawlink wins the boot race. */
static int touch_ensure(struct inject *in)
{
    if (in->touch_fd >= 0) {
        return 0;
    }
    if (in->touch_retry_at > now_sec()) {
        return -1;
    }
    in->touch_retry_at = now_sec() + 2.0;
    if (access(touch_path(), F_OK) == 0 && touch_connect(in) == 0) {
        slogf("inject: touch/key injection on %s", touch_path());
        return 0;
    }
    return -1;
}

static void inject_init(struct inject *in, int no_inject)
{
    memset(in, 0, sizeof *in);
    in->touch_fd = -1;
    if (no_inject) {
        return;
    }
    /* injection stays enabled even when the socket is not there yet; touch_ensure retries
     * on use, so a weston that comes up after rawlink still gets its input */
    in->enabled = 1;
    if (access(touch_path(), F_OK) == 0) {
        if (touch_connect(in) < 0) {
            slogf("rawstream: weston-touch socket unusable (%s), will retry",
                  strerror(errno));
        } else {
            slogf("inject: touch/key injection on %s", touch_path());
        }
    }
}

static void inject_touch(struct inject *in, int down, int x, int y)
{
    char cmd[64];

    if (!in->enabled || touch_ensure(in) < 0) {
        return;
    }
    if (down) {
        snprintf(cmd, sizeof cmd, "%c %d %d\n", in->touch_down ? 'm' : 'd', x, y);
    } else if (in->touch_down) {
        snprintf(cmd, sizeof cmd, "u\n");
    } else {
        return;
    }
    if (touch_write(in, cmd) == 0) {
        in->touch_down = down;
        return;
    }
    /* the listener went away under the socket (a weston restart): reconnect and resend
     * the sample on the fresh connection, where a move was never a press */
    close(in->touch_fd);
    in->touch_fd = -1;
    in->touch_down = 0;
    if (touch_connect(in) == 0) {
        if (cmd[0] == 'm') {
            cmd[0] = 'd';
        }
        if (touch_write(in, cmd) == 0) {
            in->touch_down = down;
            return;
        }
        close(in->touch_fd);
        in->touch_fd = -1;
    }
    slogf("inject: touch socket lost (%s)", strerror(errno));
}

/* the panel keys are tapped on the weston-touch socket as evdev codes; the module injects
 * a real key press/release into the compositor (weston-touch.c 'k'), so the focused app
 * gets it with no XTest and no X server in the path. */
static void inject_key(struct inject *in, unsigned code)
{
    char cmd[32];

    if (!in->enabled || touch_ensure(in) < 0) {
        return;
    }
    snprintf(cmd, sizeof cmd, "k %u\n", code);
    if (touch_write(in, cmd) < 0) {
        close(in->touch_fd);
        in->touch_fd = -1;
        in->touch_down = 0;
        slogf("inject: touch socket lost (%s)", strerror(errno));
    }
}

/* the icc audio panel and steering wheel buttons livi has default bindings for, as evdev
 * key codes. the unit sends every panel button as LI_BUTTON (rawplay/rawplay.c send_input)
 * and the bit is the ipc channel 6 bitmap bit (docs/v850-ipc-protocol.md,
 * iccbuttons/iccbuttons.c). the key is tapped on the press edge only: the release is the
 * other half of the same tap, and tapping it again would fire the livi action twice. */
static const struct button_key {
    unsigned char bit;
    unsigned code;
} button_keys[] = {
    { 25, KEY_BACKSPACE },      /* back / home -> back */
    { 28, KEY_H },              /* menu -> home */
    { 29, KEY_B },              /* seek down -> previous */
    { 30, KEY_N },              /* seek up -> next */
    { 46, KEY_V },              /* swc phone -> voice assistant */
    { 48, KEY_N },              /* swc seek -> next */
};

static unsigned button_code(unsigned bit)
{
    size_t i;

    for (i = 0; i < sizeof button_keys / sizeof button_keys[0]; i++) {
        if (button_keys[i].bit == bit) {
            return button_keys[i].code;
        }
    }
    return 0;
}

/* ---- the sender ------------------------------------------------------------------------- */

struct txmsg {
    unsigned char *data;
    size_t len, off;
    unsigned seq;
    int owned;
    struct txmsg *next;
};

struct outlink {
    int fd;
    int is_pipe;
    double next_try;
    int announced_down;
};

/* --wayland: the capture thread owns the wayland connection and writes rgb565 frames into
 * the pipe whose read end is stream.ff_fd, so the sender treats it exactly like the ffmpeg
 * pipe. the thread never exits on a weston restart: it reconnects and keeps the pipe open,
 * so no respawn logic is needed for it. */
struct wlcap {
    pthread_t tid;
    int running;
    int out_fd;                 /* write end of the frame pipe (thread side) */
    int stop_r, stop_w;         /* wakes the thread out of poll/dispatch */
    volatile int stop;
    char display[128];
    unsigned char *dst;         /* rgb565 staging for one frame */
};

struct stream {
    struct cfg *cfg;
    int frame_len, window;
    unsigned char mode_msg[MSG_HDR];
    struct txmsg mode_tx;
    int mode_pending;
    unsigned seq, acked, lat_floor;
    struct txmsg *txq_head, *txq_tail, *tx;
    struct { unsigned seq; double t; } sent[SENT_MAX];
    unsigned sent_next;
    double lat_recent[LAT_MAX];
    int lat_count, lat_pos;
    double lat_max;
    unsigned frames, total_bytes, stale_src;
    double started, last_stats, last_ready, last_msg, stop_at;
    FILE *events;
    unsigned char *pending;
    size_t pending_cap, pending_len;
    unsigned char li_buf[65536 + 64];
    size_t li_len;
    int touch_pending;              /* a queued run of touch samples, newest wins */
    unsigned touch_x, touch_y, touch_down;
    struct inject inj;
    pid_t ff_pid;
    int ff_fd;
    double ff_retry_at;
    struct wlcap cap;
};

static void tx_free(struct txmsg *m)
{
    if (!m) {
        return;
    }
    if (m->owned) {
        free(m->data);
        free(m);
    }
}

static void txq_clear(struct stream *s)
{
    struct txmsg *m = s->txq_head;

    while (m) {
        struct txmsg *next = m->next;

        tx_free(m);
        m = next;
    }
    s->txq_head = s->txq_tail = NULL;
}

static void tx_clear(struct stream *s)
{
    if (s->tx) {
        tx_free(s->tx);
        s->tx = NULL;
    }
    txq_clear(s);
}

static void txq_push(struct stream *s, struct txmsg *m)
{
    m->next = NULL;
    if (s->txq_tail) {
        s->txq_tail->next = m;
    } else {
        s->txq_head = m;
    }
    s->txq_tail = m;
}

static ssize_t out_write(struct outlink *o, const unsigned char *buf, size_t len)
{
    for (;;) {
        ssize_t n;

        if (o->fd < 0) {
            return 0;
        }
        n = write(o->fd, buf, len);
        if (n >= 0) {
            return n;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return -1;
    }
}

static void sent_record(struct stream *s, unsigned seq, double t)
{
    unsigned slot;

    if (seq == HDR_NO_SEQ || seq < s->lat_floor) {
        return;
    }
    slot = s->sent_next++ % SENT_MAX;
    s->sent[slot].seq = seq;
    s->sent[slot].t = t;
}

/* every slot empty. seq 0 is a valid frame number, so a zeroed slot must never look like
 * a record of it: an ack for seq 0 after a reset used to match a cleared slot and report
 * the host's uptime as the latency (the ~1e6 ms max in the old stats). */
static void sent_clear(struct stream *s)
{
    int i;

    for (i = 0; i < SENT_MAX; i++) {
        s->sent[i].seq = HDR_NO_SEQ;
        s->sent[i].t = 0.0;
    }
    s->sent_next = 0;
}

static int sent_take(struct stream *s, unsigned seq, double *t)
{
    int i;

    for (i = 0; i < SENT_MAX; i++) {
        if (s->sent[i].seq == seq) {
            *t = s->sent[i].t;
            s->sent[i].seq = HDR_NO_SEQ;
            return 1;
        }
    }
    return 0;
}

static void lat_reset(struct stream *s)
{
    s->lat_count = 0;
    s->lat_pos = 0;
    s->lat_max = 0.0;
}

static void lat_add(struct stream *s, double ms)
{
    s->lat_recent[s->lat_pos] = ms;
    s->lat_pos = (s->lat_pos + 1) % LAT_MAX;
    if (s->lat_count < LAT_MAX) {
        s->lat_count++;
    }
    if (ms > s->lat_max) {
        s->lat_max = ms;
    }
}

static double lat_avg(const struct stream *s)
{
    double sum = 0.0;
    int i;

    if (s->lat_count == 0) {
        return 0.0;
    }
    for (i = 0; i < s->lat_count; i++) {
        sum += s->lat_recent[i];
    }
    return sum / s->lat_count;
}

static void stats_line(struct stream *s, int final)
{
    double el = now_sec() - s->started;

    if (el <= 0.0) {
        el = 1e-9;
    }
    if (final) {
        printf("rawstream: %u frames, %u kb, %u dropped, in %.1f s, %.1f fps, "
               "%.0f kbit/s, latency %.1f/%.1f ms avg/max\n",
               s->frames, s->total_bytes / 1024, s->stale_src, el, s->frames / el,
               s->total_bytes * 8.0 / el / 1000.0, lat_avg(s), s->lat_max);
    } else {
        printf("rawstream: %u frames, %u kb, %u dropped, %u in flight, %.1f fps, "
               "%.0f kbit/s, latency %.1f/%.1f ms avg/max\n",
               s->frames, s->total_bytes / 1024, s->stale_src, s->seq - s->acked,
               s->frames / el, s->total_bytes * 8.0 / el / 1000.0, lat_avg(s), s->lat_max);
    }
    fflush(stdout);
}

/* a fresh reader owns everything from here: drop the queued pictures and forget the acks
 * that belong to the old one, so a startup queue is not forever in the latency stats */
static void window_reset(struct stream *s, const char *why, unsigned seq)
{
    s->acked = s->seq;
    s->lat_floor = s->seq;
    sent_clear(s);
    lat_reset(s);
    txq_clear(s);
    s->mode_pending = 1;
    if (why) {
        slogf("rawstream: %s at seq %u", why, seq);
    }
}

/* the unit can hand a whole run of touch samples over at once: the driver reports one every
 * millisecond while a finger is down, and a fresh reader gets the queue from before it
 * opened as a burst with the next touch. only the newest sample of the run is meaningful,
 * so consecutive touches are held here and the last one is injected when the run ends (a
 * different message arrives, or the read buffer is drained). an isolated sample is
 * injected in the same pass, so live touch adds no latency. */
static void touch_flush(struct stream *s)
{
    if (!s->touch_pending) {
        return;
    }
    s->touch_pending = 0;
    if (s->events) {
        fprintf(s->events, "%.3f touch %u %u %u\n", now_sec(), s->touch_x, s->touch_y,
                s->touch_down);
    }
    if (s->inj.enabled) {
        inject_touch(&s->inj, (int)s->touch_down, (int)s->touch_x, (int)s->touch_y);
    }
}

static void handle_li(struct stream *s, const unsigned char *m, size_t n)
{
    unsigned t = m[2];
    double now = now_sec();

    s->last_msg = now;
    if (t == LI_TOUCH && n >= 8) {
        s->touch_pending = 1;
        s->touch_x = (unsigned)m[3] | (unsigned)m[4] << 8;
        s->touch_y = (unsigned)m[5] | (unsigned)m[6] << 8;
        s->touch_down = m[7];
        return;
    }
    touch_flush(s);             /* a touch never arrives out of order with the rest */
    /* LI_KNOB is consumed and dropped: livi has no default binding for the knob */
    if (t == LI_BUTTON && n >= 5) {
        /* the panel buttons: the mapped ones are tapped, the rest are logged and left
         * alone (the unit's own services see the same bitmap regardless) */
        unsigned bit = m[3], down = m[4];
        unsigned code = button_code(bit);

        if (s->events) {
            if (down && code) {
                fprintf(s->events, "%.3f button %u %u key %u\n", now, bit, down, code);
            } else {
                fprintf(s->events, "%.3f button %u %u\n", now, bit, down);
            }
        }
        if (down && code) {
            inject_key(&s->inj, code);
        }
    } else if (t == LI_READY) {
        if (s->last_ready == 0.0 || now - s->last_ready > 3.0) {
            window_reset(s, "reader ready", s->seq);
        }
        s->last_ready = now;
        /* the reader can have dropped the first mode message while flushing a stale
         * queue, so a heartbeat is always followed by the mode again */
        s->mode_pending = 1;
    } else if (t == LI_RAW && n >= 8) {
        unsigned rseq = (unsigned)m[3] | (unsigned)m[4] << 8 | (unsigned)m[5] << 16
                        | (unsigned)m[6] << 24;

        /* clamp: a stale or corrupt ack from a previous run must not make seq - acked
         * underflow and wedge the window until the watchdog notices */
        if (rseq >= s->seq) {
            s->acked = s->seq;
        } else if (rseq + 1 > s->acked) {
            s->acked = rseq + 1;
        }
        {
            double t0;

            if (sent_take(s, rseq, &t0)) {
                if (m[7] & 1) {
                    double lat = (now - t0) * 1000.0;

                    lat_add(s, lat);
                    if (s->events) {
                        fprintf(s->events, "%.3f drawn %u %.1f\n", now, rseq, lat);
                    }
                }
            }
        }
    }
}

static void li_feed(struct stream *s, const unsigned char *data, size_t n)
{
    while (n) {
        size_t room = sizeof(s->li_buf) - s->li_len;
        size_t take = n < room ? n : room;

        memcpy(s->li_buf + s->li_len, data, take);
        s->li_len += take;
        data += take;
        n -= take;
        for (;;) {
            size_t i = 0, need;

            while (i + 1 < s->li_len
                   && !(s->li_buf[i] == 'L' && s->li_buf[i + 1] == 'I')) {
                i++;
            }
            if (i + 1 >= s->li_len) {
                if (s->li_len && s->li_buf[s->li_len - 1] == 'L') {
                    s->li_buf[0] = 'L';
                    s->li_len = 1;
                } else {
                    s->li_len = 0;
                }
                break;
            }
            if (i) {
                memmove(s->li_buf, s->li_buf + i, s->li_len - i);
                s->li_len -= i;
            }
            if (s->li_len < 3) {
                break;
            }
            need = (size_t)li_needed[s->li_buf[2]];
            if (need == 0) {
                memmove(s->li_buf, s->li_buf + 2, s->li_len - 2);
                s->li_len -= 2;
                continue;
            }
            if (s->li_len < need) {
                break;
            }
            handle_li(s, s->li_buf, need);
            memmove(s->li_buf, s->li_buf + need, s->li_len - need);
            s->li_len -= need;
        }
        if (s->li_len == sizeof(s->li_buf)) {
            memmove(s->li_buf, s->li_buf + 1, --s->li_len);
        }
    }
    touch_flush(s);
}

static void queue_frame(struct stream *s, const unsigned char *payload)
{
    struct txmsg *m;
    unsigned char *buf;

    if (s->window && s->seq - s->acked >= (unsigned)s->window) {
        /* the guest has not caught up: drop this captured frame at the source and let
         * the panel show the next one instead of queueing stale pictures */
        s->stale_src++;
        return;
    }
    m = malloc(sizeof *m);
    buf = malloc(MSG_HDR + (size_t)s->frame_len);
    if (!m || !buf) {
        free(m);
        free(buf);
        s->stale_src++;
        slogf("rawstream: out of memory for a frame, dropping it");
        return;
    }
    /* the header is padded to one full max packet so the payload starts on a packet
     * boundary: the receiver then reads the frame as a whole number of max packets with
     * no sub-packet tail, and its ack does not have to wait for the first bytes of the
     * next frame (a 512-byte tail read has nothing to complete on until the next frame
     * arrives, which added a frame period to the reported latency and deadlocked a
     * one-frame window). */
    build_hdr(buf, LR_FRAME, (unsigned)s->frame_len, s->seq, now_ms());
    memset(buf + LR_HDR, 0, MSG_HDR - LR_HDR);
    memcpy(buf + MSG_HDR, payload, (size_t)s->frame_len);
    m->data = buf;
    m->len = MSG_HDR + (size_t)s->frame_len;
    m->off = 0;
    m->seq = s->seq;
    m->owned = 1;
    m->next = NULL;
    txq_push(s, m);
    s->seq++;
    s->frames++;
    s->total_bytes += (unsigned)s->frame_len;
}

/* returns 0 when the queue drained, -1 when the connection is gone */
static int flush_tx(struct stream *s, struct outlink *o)
{
    while (s->tx || s->mode_pending || s->txq_head) {
        ssize_t n;

        if (!s->tx) {
            if (s->mode_pending) {
                s->mode_tx.off = 0;
                s->tx = &s->mode_tx;
                s->mode_pending = 0;
            } else {
                s->tx = s->txq_head;
                s->txq_head = s->tx->next;
                if (!s->txq_head) {
                    s->txq_tail = NULL;
                }
                s->tx->next = NULL;
            }
        }
        n = out_write(o, s->tx->data + s->tx->off, s->tx->len - s->tx->off);
        if (n < 0) {
            if (o->is_pipe) {
                slogf("rawstream: video pipe write failed (%s)", strerror(errno));
                return 0;
            }
            return -1;
        }
        if (n == 0) {
            return 0;
        }
        s->tx->off += (size_t)n;
        if (s->tx->off == s->tx->len) {
            struct txmsg *done = s->tx;

            s->tx = NULL;
            if (done->seq != HDR_NO_SEQ) {
                sent_record(s, done->seq, now_sec());
            }
            tx_free(done);
        }
    }
    return 0;
}

static void link_lost(struct stream *s, struct outlink *o)
{
    if (o->fd >= 0) {
        close(o->fd);
    }
    o->fd = -1;
    o->announced_down = 0;
    o->next_try = now_sec() + 0.5;
    tx_clear(s);
    s->acked = s->seq;
    s->lat_floor = s->seq;
    sent_clear(s);
    lat_reset(s);
    s->touch_pending = 0;       /* a held touch from the old link is stale */
    s->mode_pending = 1;
    slogf("rawstream: %s went away, reconnecting", s->cfg->path);
}

static int try_connect(struct outlink *o, const char *path)
{
    struct sockaddr_un addr;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0) {
        return -1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof addr.sun_path) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    o->fd = fd;
    o->announced_down = 0;
    return 0;
}

static void wlcap_stop(struct stream *s);

static void ff_reap(struct stream *s)
{
    if (s->cap.running) {
        wlcap_stop(s);
    }
    if (s->ff_fd >= 0) {
        close(s->ff_fd);
        s->ff_fd = -1;
    }
    if (s->ff_pid > 0) {
        kill(s->ff_pid, SIGKILL);
        while (waitpid(s->ff_pid, NULL, 0) < 0 && errno == EINTR) {
        }
        s->ff_pid = -1;
    }
    s->pending_len = 0;
    s->ff_retry_at = now_sec() + 1.0;
}

static int ff_spawn(struct stream *s)
{
    static char args[32][512];
    static char *argv[40];
    struct cfg *c = s->cfg;
    char size[64];
    char draw[512];
    int n = 0, fds[2];
    pid_t pid;

    if (pipe2(fds, O_CLOEXEC) < 0) {
        return -1;
    }
    snprintf(size, sizeof size, "%dx%d", c->width, c->height);
#define ARG(...) do { snprintf(args[n], sizeof args[0], __VA_ARGS__); argv[n] = args[n]; n++; } while (0)
    ARG("ffmpeg");
    ARG("-hide_banner");
    ARG("-loglevel");
    ARG("error");
    if (c->file) {
        ARG("-re");
        ARG("-stream_loop");
        ARG("-1");
        ARG("-an");
        ARG("-i");
        ARG("%s", c->file);
    } else if (c->use_motion) {
        ARG("-f");
        ARG("lavfi");
        ARG("-re");
        ARG("-i");
        ARG("testsrc2=s=%s:r=%d", size, c->fps);
    } else {
        ARG("-f");
        ARG("lavfi");
        ARG("-re");
        ARG("-i");
        snprintf(draw, sizeof draw,
                 "color=c=0x202830:s=%s:r=%d,"
                 "drawbox=x='mod(t*160,720)':y=40:w=80:h=80:c=red@1:t=fill,"
                 "drawbox=x=20:y=380:w=60:h=60:c=blue@1:t=fill", size, c->fps);
        ARG("%s", draw);
    }
    ARG("-pix_fmt");
    ARG("rgb565le");
    ARG("-f");
    ARG("rawvideo");
    ARG("-");
#undef ARG
    argv[n] = NULL;

    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    set_nonblock(fds[0]);
    s->ff_fd = fds[0];
    s->ff_pid = pid;
    s->pending_len = 0;
    return 0;
}

/* ---- wayland capture: weston_capture_v1 + wl_shm ----------------------------------------- */

/* the capture protocol hands the client pixels in the renderer's own format. headless
 * weston with the pixman renderer captures XRGB8888 (an ARGB8888 output is possible too);
 * both are 4 bytes per pixel with the same rgb byte order, and the sender converts them to
 * the panel's rgb565le. no X server, no x11grab and no ffmpeg on this path. */
#define DRM_FORMAT_ARGB8888 0x34325241u
#define DRM_FORMAT_XRGB8888 0x34325258u
/* wl_shm takes the enum values, not the fourccs the capture source announces */
#define WL_SHM_FORMAT_ARGB8888 0
#define WL_SHM_FORMAT_XRGB8888 1

struct wlcap_state {
    struct wl_display *dpy;
    struct wl_registry *registry;
    struct wl_shm *shm;
    struct weston_capture_v1 *factory;
    struct wl_output *output;
    struct weston_capture_source_v1 *source;
    struct wl_shm_pool *pool;
    struct wl_buffer *buffer;
    unsigned char *map;
    size_t map_len;
    int fd;
    int have_format, have_size, complete, retry;
    uint32_t format;
    int width, height;
};

/* sleep up to ms, waking early when the stream stops; returns -1 when stopping */
static int wlcap_pause(struct wlcap *cap, int ms)
{
    struct pollfd p = { cap->stop_r, POLLIN, 0 };

    if (g_stop || cap->stop) {
        return -1;
    }
    if (ms <= 0) {
        return 0;
    }
    return poll(&p, 1, ms) > 0 ? -1 : 0;
}

/* dispatch queued and pending wayland events, waiting up to timeout_ms for more. returns
 * -1 on a display error or when the stream is stopping. */
static int wlcap_dispatch_wait(struct wlcap *cap, struct wl_display *dpy, int timeout_ms)
{
    struct pollfd pf[2];
    int r;

    if (g_stop || cap->stop) {
        return -1;
    }
    if (wl_display_prepare_read(dpy) != 0) {
        return wl_display_dispatch_pending(dpy) < 0 ? -1 : 0;
    }
    pf[0].fd = wl_display_get_fd(dpy);
    pf[0].events = POLLIN;
    pf[0].revents = 0;
    pf[1].fd = cap->stop_r;
    pf[1].events = POLLIN;
    pf[1].revents = 0;
    r = poll(pf, 2, timeout_ms);
    if (r < 0) {
        wl_display_cancel_read(dpy);
        return errno == EINTR ? 0 : -1;
    }
    if (pf[1].revents & POLLIN) {
        wl_display_cancel_read(dpy);
        return -1;
    }
    if (r == 0 || !(pf[0].revents & (POLLIN | POLLERR | POLLHUP))) {
        wl_display_cancel_read(dpy);
        return 0;
    }
    if (wl_display_read_events(dpy) < 0) {
        return -1;
    }
    return wl_display_dispatch_pending(dpy) < 0 ? -1 : 0;
}

static int wlcap_flush(struct wl_display *dpy)
{
    for (;;) {
        if (wl_display_flush(dpy) == 0) {
            return 0;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
        {
            struct pollfd p = { wl_display_get_fd(dpy), POLLOUT, 0 };

            poll(&p, 1, 100);
        }
    }
}

static void wlcap_registry_global(void *data, struct wl_registry *reg, uint32_t name,
                                  const char *iface, uint32_t version)
{
    struct wlcap_state *st = data;

    if (strcmp(iface, wl_shm_interface.name) == 0) {
        st->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, weston_capture_v1_interface.name) == 0) {
        st->factory = wl_registry_bind(reg, name, &weston_capture_v1_interface,
                                       version < 2 ? version : 2);
    } else if (strcmp(iface, wl_output_interface.name) == 0 && !st->output) {
        st->output = wl_registry_bind(reg, name, &wl_output_interface, 1);
    }
}

static void wlcap_registry_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data;
    (void)reg;
    (void)name;
}

static const struct wl_registry_listener wlcap_registry_listener = {
    .global = wlcap_registry_global,
    .global_remove = wlcap_registry_remove,
};

static void wlcap_source_format(void *data, struct weston_capture_source_v1 *src,
                                uint32_t drm_format)
{
    struct wlcap_state *st = data;

    (void)src;
    if (drm_format == DRM_FORMAT_XRGB8888 || drm_format == DRM_FORMAT_ARGB8888) {
        st->format = drm_format;
        st->have_format = 1;
    }
}

static void wlcap_source_size(void *data, struct weston_capture_source_v1 *src,
                              int32_t width, int32_t height)
{
    struct wlcap_state *st = data;

    (void)src;
    st->width = width;
    st->height = height;
    st->have_size = 1;
}

static void wlcap_source_formats_done(void *data, struct weston_capture_source_v1 *src)
{
    (void)data;
    (void)src;
    /* the first compatible format is enough; this only marks the end of the list, and the
     * listener entry must not be NULL (libwayland refuses to dispatch to it) */
}

static void wlcap_source_complete(void *data, struct weston_capture_source_v1 *src)
{
    struct wlcap_state *st = data;

    (void)src;
    st->complete = 1;
}

static void wlcap_source_retry(void *data, struct weston_capture_source_v1 *src)
{
    struct wlcap_state *st = data;

    (void)src;
    st->retry = 1;
}

static void wlcap_source_failed(void *data, struct weston_capture_source_v1 *src,
                                const char *msg)
{
    struct wlcap_state *st = data;

    (void)src;
    (void)st;
    slogf("rawstream: wayland capture failed: %s", msg ? msg : "unknown");
}

static const struct weston_capture_source_v1_listener wlcap_source_listener = {
    .format = wlcap_source_format,
    .formats_done = wlcap_source_formats_done,
    .size = wlcap_source_size,
    .complete = wlcap_source_complete,
    .retry = wlcap_source_retry,
    .failed = wlcap_source_failed,
};

static void wlcap_release(struct wlcap_state *st)
{
    if (st->source) {
        weston_capture_source_v1_destroy(st->source);
        st->source = NULL;
    }
    if (st->buffer) {
        wl_buffer_destroy(st->buffer);
        st->buffer = NULL;
    }
    if (st->pool) {
        wl_shm_pool_destroy(st->pool);
        st->pool = NULL;
    }
    if (st->map) {
        munmap(st->map, st->map_len);
        st->map = NULL;
    }
    if (st->fd >= 0) {
        close(st->fd);
        st->fd = -1;
    }
    if (st->output) {
        wl_output_destroy(st->output);
        st->output = NULL;
    }
    if (st->factory) {
        weston_capture_v1_destroy(st->factory);
        st->factory = NULL;
    }
    if (st->shm) {
        wl_shm_destroy(st->shm);
        st->shm = NULL;
    }
    if (st->registry) {
        wl_registry_destroy(st->registry);
        st->registry = NULL;
    }
    if (st->dpy) {
        wl_display_disconnect(st->dpy);
        st->dpy = NULL;
    }
}

static int wlcap_make_buffer(struct wlcap_state *st)
{
    int stride = st->width * 4;
    size_t len = (size_t)stride * (size_t)st->height;

    if (st->format != DRM_FORMAT_XRGB8888 && st->format != DRM_FORMAT_ARGB8888) {
        slogf("rawstream: unsupported wayland capture format 0x%x", st->format);
        return -1;
    }
    if (st->fd >= 0) {
        if (st->map) {
            munmap(st->map, st->map_len);
            st->map = NULL;
        }
        if (st->buffer) {
            wl_buffer_destroy(st->buffer);
            st->buffer = NULL;
        }
        if (st->pool) {
            wl_shm_pool_destroy(st->pool);
            st->pool = NULL;
        }
        close(st->fd);
        st->fd = -1;
    }
    st->fd = memfd_create("livi-capture", MFD_CLOEXEC);
    if (st->fd < 0 || ftruncate(st->fd, (off_t)len) < 0) {
        slogf("rawstream: wayland capture buffer: %s", strerror(errno));
        if (st->fd >= 0) {
            close(st->fd);
            st->fd = -1;
        }
        return -1;
    }
    st->map = mmap(NULL, len, PROT_READ, MAP_SHARED, st->fd, 0);
    if (st->map == MAP_FAILED) {
        st->map = NULL;
        slogf("rawstream: wayland capture mmap: %s", strerror(errno));
        return -1;
    }
    st->map_len = len;
    st->pool = wl_shm_create_pool(st->shm, st->fd, (int32_t)len);
    st->buffer = wl_shm_pool_create_buffer(st->pool, 0, st->width, st->height, stride,
                                           st->format == DRM_FORMAT_XRGB8888
                                               ? WL_SHM_FORMAT_XRGB8888
                                               : WL_SHM_FORMAT_ARGB8888);
    if (!st->pool || !st->buffer) {
        slogf("rawstream: wayland capture cannot create the shm buffer");
        return -1;
    }
    return 0;
}

static int wlcap_setup(struct stream *s, struct wlcap_state *st)
{
    st->registry = wl_display_get_registry(st->dpy);
    wl_registry_add_listener(st->registry, &wlcap_registry_listener, st);
    if (wl_display_roundtrip(st->dpy) < 0) {
        return -1;
    }
    if (!st->shm || !st->factory || !st->output) {
        slogf("rawstream: no weston_capture_v1/wl_shm/output on this display");
        return -1;
    }
    st->source = weston_capture_v1_create(st->factory, st->output,
                                          WESTON_CAPTURE_V1_SOURCE_FRAMEBUFFER);
    if (!st->source) {
        return -1;
    }
    weston_capture_source_v1_add_listener(st->source, &wlcap_source_listener, st);
    if (wlcap_flush(st->dpy) < 0) {
        return -1;
    }
    /* the source delivers format and size right after creation; wait for both (a compositor
     * can send several format events, so one roundtrip is not enough) */
    while (!(st->have_size && st->have_format)) {
        if (wlcap_dispatch_wait(&s->cap, st->dpy, 2000) < 0) {
            return -1;
        }
    }
    return wlcap_make_buffer(st);
}

/* convert one captured XRGB8888/ARGB8888 frame (bytes B,G,R,X) to the panel's rgb565le.
 * --flip reverses the row order: weston's asynchronous GL capture path flips whenever the
 * GL_ANGLE_pack_reverse_row_order extension is missing (NVIDIA), regardless of the
 * renderer's real y orientation, so a GL session on such a driver delivers bottom-up
 * frames. pixman sessions are always top-down and never need it. */
static void wlcap_convert(struct stream *s, const struct wlcap_state *st)
{
    unsigned char *dst = s->cap.dst;
    int x, y;

    for (y = 0; y < st->height; y++) {
        int sy = s->cfg->flip ? st->height - 1 - y : y;
        const unsigned char *p = st->map + (size_t)sy * (size_t)st->width * 4;

        for (x = 0; x < st->width; x++, p += 4, dst += 2) {
            dst[0] = (unsigned char)(((p[1] & 0x1c) << 3) | (p[0] >> 3));
            dst[1] = (unsigned char)((p[2] & 0xf8) | (p[1] >> 5));
        }
    }
}

static void *wlcap_thread(void *arg)
{
    struct stream *s = arg;
    struct wlcap *cap = &s->cap;
    struct cfg *c = s->cfg;

    while (!g_stop && !cap->stop) {
        unsigned long long n = 0;
        double start = now_sec();
        struct wlcap_state st;

        memset(&st, 0, sizeof st);
        st.fd = -1;
        st.dpy = wl_display_connect(cap->display);
        if (!st.dpy) {
            slogf("rawstream: wayland %s not up (%s), retrying", cap->display,
                  strerror(errno));
            if (wlcap_pause(cap, 1000) < 0) {
                break;
            }
            continue;
        }
        if (wlcap_setup(s, &st) < 0
            || st.width != c->width || st.height != c->height) {
            if (st.have_size) {
                slogf("rawstream: weston output is %dx%d, expected %dx%d", st.width,
                      st.height, c->width, c->height);
            }
            wlcap_release(&st);
            if (wlcap_pause(cap, 1000) < 0) {
                break;
            }
            continue;
        }
        slogf("rawstream: wayland capture on %s, %dx%d", cap->display, st.width, st.height);
        for (;;) {
            if (g_stop || cap->stop) {
                break;
            }
            st.complete = 0;
            if (st.retry) {
                st.retry = 0;
                if (wlcap_make_buffer(&st) < 0) {
                    break;
                }
            }
            weston_capture_source_v1_capture(st.source, st.buffer);
            if (wlcap_flush(st.dpy) < 0) {
                break;
            }
            while (!st.complete && !st.retry) {
                if (wlcap_dispatch_wait(cap, st.dpy, 1000) < 0) {
                    break;      /* display error; stop is checked below */
                }
            }
            if (g_stop || cap->stop) {
                break;
            }
            if (st.retry) {
                continue;
            }
            wlcap_convert(s, &st);
            if (fd_write_all(cap->out_fd, cap->dst, (size_t)s->frame_len) < 0) {
                break;          /* the sender is gone */
            }
            n++;
            /* the headless output repaints on capture only, so the rate is ours to set:
             * pace to --fps like the ffmpeg sources, instead of burning the phone cpu on
             * repaints and conversions the unit cannot take anyway */
            if (c->fps > 0) {
                double due = start + (double)n / (double)c->fps;
                double now = now_sec();

                if (due > now && wlcap_pause(cap, (int)((due - now) * 1000.0)) < 0) {
                    break;
                }
            }
        }
        wlcap_release(&st);
        if (g_stop || cap->stop) {
            break;
        }
        slogf("rawstream: wayland capture reconnecting");
        if (wlcap_pause(cap, 500) < 0) {
            break;
        }
    }
    return NULL;
}

static int wlcap_spawn(struct stream *s)
{
    struct wlcap *cap = &s->cap;
    int p[2], q[2];

    if (pipe2(p, O_CLOEXEC) < 0) {
        return -1;
    }
    if (pipe2(q, O_CLOEXEC) < 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    cap->dst = malloc((size_t)s->frame_len);
    if (!cap->dst) {
        close(p[0]);
        close(p[1]);
        close(q[0]);
        close(q[1]);
        return -1;
    }
    s_copy(cap->display, sizeof cap->display, s->cfg->wayland);
    cap->out_fd = p[1];
    cap->stop_r = q[0];
    cap->stop_w = q[1];
    cap->stop = 0;
    s->ff_fd = p[0];
    set_nonblock(s->ff_fd);
    if (pthread_create(&cap->tid, NULL, wlcap_thread, s) != 0) {
        close(p[0]);
        close(p[1]);
        close(q[0]);
        close(q[1]);
        free(cap->dst);
        cap->dst = NULL;
        s->ff_fd = -1;
        errno = EAGAIN;
        return -1;
    }
    cap->running = 1;
    return 0;
}

static void wlcap_stop(struct stream *s)
{
    struct wlcap *cap = &s->cap;

    if (!cap->running) {
        return;
    }
    cap->stop = 1;
    (void)!write(cap->stop_w, "x", 1);
    if (s->ff_fd >= 0) {
        /* close the read end so a thread blocked writing a full pipe sees EPIPE and
         * exits; the stop pipe alone cannot wake a write */
        close(s->ff_fd);
        s->ff_fd = -1;
    }
    pthread_join(cap->tid, NULL);
    close(cap->stop_r);
    close(cap->stop_w);
    close(cap->out_fd);
    free(cap->dst);
    cap->dst = NULL;
    cap->running = 0;
}

static int stream_init(struct stream *s, struct cfg *c)
{
    long long fl = (long long)c->width * c->height * 2;
    unsigned fmt;

    memset(s, 0, sizeof *s);
    s->cfg = c;
    s->ff_pid = -1;
    s->ff_fd = -1;
    if (c->width <= 0 || c->height <= 0 || fl <= 0 || fl > (1LL << 30)) {
        slogf("rawstream: bad geometry %dx%d", c->width, c->height);
        return -1;
    }
    s->frame_len = (int)fl;
    s->window = c->window < 0 ? 0 : c->window;
    if (s->window > 64) {
        slogf("rawstream: window %d clamped to 64", s->window);
        s->window = 64;
    }
    s->pending_cap = (size_t)s->frame_len + 65536;
    s->pending = malloc(s->pending_cap);
    if (!s->pending) {
        slogf("rawstream: out of memory for the capture buffer");
        return -1;
    }
    fmt = FMT_RGB565 | (c->no_chunked ? 0 : 0x100);
    build_hdr(s->mode_msg, LR_MODE, MODE_LEN, 0, now_ms());
    put32(s->mode_msg + LR_HDR + 0, (unsigned)c->width);
    put32(s->mode_msg + LR_HDR + 4, (unsigned)c->height);
    put32(s->mode_msg + LR_HDR + 8, (unsigned)(c->width * 2));
    put32(s->mode_msg + LR_HDR + 12, fmt);
    memset(s->mode_msg + LR_HDR + MODE_LEN, 0, MSG_HDR - LR_HDR - MODE_LEN);
    s->mode_tx.data = s->mode_msg;
    s->mode_tx.len = MSG_HDR;
    s->mode_tx.off = 0;
    s->mode_tx.seq = HDR_NO_SEQ;
    s->mode_tx.owned = 0;
    sent_clear(s);
    s->started = now_sec();
    s->last_stats = s->started;
    s->last_msg = s->started;
    s->ff_retry_at = s->started;
    s->stop_at = c->seconds > 0.0 ? s->started + c->seconds : 0.0;
    s->mode_pending = 1;
    if (c->events) {
        s->events = fopen(c->events, "a");
        if (s->events) {
            setvbuf(s->events, NULL, _IOLBF, 0);
        } else {
            slogf("rawstream: cannot open %s: %s", c->events, strerror(errno));
        }
    }
    inject_init(&s->inj, c->no_inject);
    if (c->wayland) {
        if (wlcap_spawn(s) < 0) {
            slogf("rawstream: cannot start the wayland capture: %s", strerror(errno));
            s->ff_retry_at = now_sec() + 1.0;
        }
    } else if (ff_spawn(s) < 0) {
        slogf("rawstream: cannot start ffmpeg: %s", strerror(errno));
        s->ff_retry_at = now_sec() + 1.0;
    }
    return 0;
}

static void stream_cleanup(struct stream *s)
{
    ff_reap(s);
    if (s->events) {
        fclose(s->events);
    }
    tx_clear(s);
    free(s->pending);
    if (s->inj.touch_fd >= 0) {
        close(s->inj.touch_fd);
    }
}

static void capture_drain(struct stream *s, struct outlink *o)
{
    size_t drop = 0;

    if (!(s->tx || s->mode_pending || s->txq_head)) {
        /* a whole frame before anything else: the payload length is fixed */
        size_t need = (size_t)s->frame_len - s->pending_len;
        ssize_t n = read(s->ff_fd, s->pending + s->pending_len, need);

        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            ff_reap(s);
            return;
        }
        if (n == 0) {
            slogf("rawstream: capture pipe closed, restarting ffmpeg");
            ff_reap(s);
            return;
        }
        s->pending_len += (size_t)n;
        if (s->pending_len < (size_t)s->frame_len) {
            return;
        }
        s->pending_len = 0;
        queue_frame(s, s->pending);
        if (flush_tx(s, o) < 0) {
            link_lost(s, o);
        }
        return;
    }
    /* a frame is still going out: keep draining ffmpeg so it is at the live edge, but
     * do not queue what is captured, the panel takes the next one */
    while (drop == 0) {
        ssize_t n = read(s->ff_fd, s->pending + s->pending_len,
                         s->pending_cap - s->pending_len);

        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            ff_reap(s);
            return;
        }
        if (n == 0) {
            slogf("rawstream: capture pipe closed, restarting ffmpeg");
            ff_reap(s);
            return;
        }
        s->pending_len += (size_t)n;
        while (s->pending_len >= (size_t)s->frame_len) {
            memmove(s->pending, s->pending + s->frame_len,
                    s->pending_len - (size_t)s->frame_len);
            s->pending_len -= (size_t)s->frame_len;
            s->stale_src++;
        }
        drop = 1;
    }
}

static void maybe_rebind(void);             /* run mode: service a functionfs unbind */

/* the shared sender loop: rx_fd is -1 in stream mode (the socket is both directions) or
 * the run-mode upstream pipe. */
static void stream_loop(struct stream *s, struct outlink *o, int rx_fd)
{
    while (!g_stop) {
        double now = now_sec();
        int want_tx, rfd, i, nf = 0, ff_i = -1, rx_i = -1, tx_i = -1;
        struct pollfd pf[4];

        if (s->stop_at && now >= s->stop_at) {
            break;
        }
        if (!o->is_pipe && o->fd < 0 && now >= o->next_try) {
            if (try_connect(o, s->cfg->path) == 0) {
                slogf("rawstream: %s up", s->cfg->path);
                window_reset(s, NULL, 0);
            } else {
                o->next_try = now + 0.5;
                if (!o->announced_down) {
                    slogf("rawstream: waiting for %s", s->cfg->path);
                    o->announced_down = 1;
                }
            }
        }
        if (s->ff_fd < 0 && !s->cap.running && now >= s->ff_retry_at) {
            int rc = s->cfg->wayland ? wlcap_spawn(s) : ff_spawn(s);

            if (rc < 0) {
                s->ff_retry_at = now + 1.0;
            }
        }
        maybe_rebind();
        /* a reader that went quiet with frames in flight is either not reading yet (a
         * player started after the sender) or lost its place in a mid-stream start; drop
         * the window and send the mode again so it can resync */
        if (s->seq - s->acked >= (unsigned)(s->window ? s->window : 1)
            && now - s->last_msg > 5.0) {
            slogf("rawstream: no acks for 5 s, resending mode and resetting the window");
            window_reset(s, NULL, 0);
            s->last_msg = now;
        }
        if (s->cfg->stats && now - s->last_stats >= 5.0) {
            s->last_stats = now;
            stats_line(s, 0);
        }

        want_tx = s->tx || s->mode_pending || s->txq_head;
        if (s->ff_fd >= 0) {
            pf[nf].fd = s->ff_fd;
            pf[nf].events = POLLIN;
            pf[nf].revents = 0;
            ff_i = nf++;
        }
        rfd = rx_fd >= 0 ? rx_fd : o->fd;
        if (rfd >= 0) {
            pf[nf].fd = rfd;
            pf[nf].events = POLLIN;
            pf[nf].revents = 0;
            rx_i = nf++;
        }
        if (o->fd >= 0 && want_tx) {
            if (rx_i >= 0 && pf[rx_i].fd == o->fd) {
                pf[rx_i].events |= POLLOUT;
                tx_i = rx_i;
            } else {
                pf[nf].fd = o->fd;
                pf[nf].events = POLLOUT;
                pf[nf].revents = 0;
                tx_i = nf++;
            }
        }
        i = poll(pf, (nfds_t)nf, 50);
        if (g_stop) {
            break;
        }
        if (i < 0 && errno != EINTR) {
            slogf("rawstream: poll: %s", strerror(errno));
            msleep(50);
            continue;
        }

        /* writes and reads first, then the capture pipe: with an ack per frame the socket
         * can stay readable and starve the pipe (which then throttles ffmpeg) */
        if (tx_i >= 0 && (pf[tx_i].revents & POLLOUT)) {
            if (flush_tx(s, o) < 0) {
                link_lost(s, o);
            }
        }
        if (rx_i >= 0) {
            short re = pf[rx_i].revents;

            if (re & POLLIN) {
                unsigned char buf[65536];
                ssize_t n = read(rfd, buf, sizeof buf);

                if (n > 0) {
                    li_feed(s, buf, (size_t)n);
                } else if (n < 0 && errno != EINTR && errno != EAGAIN
                           && errno != EWOULDBLOCK) {
                    if (!o->is_pipe) {
                        link_lost(s, o);
                    }
                } else if (n == 0 && !o->is_pipe) {
                    link_lost(s, o);
                }
            } else if ((re & (POLLHUP | POLLERR | POLLNVAL)) && !o->is_pipe) {
                link_lost(s, o);
            }
        }
        if (ff_i >= 0 && (pf[ff_i].revents & (POLLIN | POLLHUP | POLLERR))) {
            capture_drain(s, o);
        }
    }
}

/* ---- the composite gadget ----------------------------------------------------------------- */

struct gadget {
    struct cfg *cfg;
    volatile int stop;
    volatile int enabled;
    volatile int gen;
    volatile int unbind;
    volatile double rebind_at;
    volatile int ep1_open, ep2_open;
    int ep1_dead_gen;           /* the generation whose ep1 write failed */
    double ep1_retry_at;        /* next allowed ep1 reopen after such a failure */
    int ep0, server_fd;
    int client_fd, client_dead;
    pthread_mutex_t cli_mu;
    int up_r, up_w;
    int vid_r, vid_w;
    char gdir[PATH_MAX], ffs[PATH_MAX], sock[PATH_MAX], stick[PATH_MAX];
    char name[64], udc[64];
    int rw;
    pthread_t ep0_tid, pump_tid, video_tid;
    int ep0_started, pump_started, video_started;
};

static size_t build_descriptors(unsigned char *buf)
{
    unsigned char *p = buf;

    put32(p, FFS_DESCRIPTORS_MAGIC);
    put32(p + 4, 0);
    put32(p + 8, 3);            /* fs_count: interface + two endpoints */
    put32(p + 12, 3);
    p += 16;
    /* full speed: vendor specific, two bulk endpoints, 64 byte max packets */
    *p++ = 9; *p++ = 4; *p++ = 0; *p++ = 0; *p++ = 2;
    *p++ = 0xff; *p++ = 0xff; *p++ = 0xff; *p++ = 0;
    *p++ = 7; *p++ = 5; *p++ = 0x83; *p++ = 2; *p++ = 64; *p++ = 0; *p++ = 0;
    *p++ = 7; *p++ = 5; *p++ = 0x04; *p++ = 2; *p++ = 64; *p++ = 0; *p++ = 0;
    /* high speed: same, 512 byte max packets */
    *p++ = 9; *p++ = 4; *p++ = 0; *p++ = 0; *p++ = 2;
    *p++ = 0xff; *p++ = 0xff; *p++ = 0xff; *p++ = 0;
    *p++ = 7; *p++ = 5; *p++ = 0x83; *p++ = 2; *p++ = 0; *p++ = 2; *p++ = 0;
    *p++ = 7; *p++ = 5; *p++ = 0x04; *p++ = 2; *p++ = 0; *p++ = 2; *p++ = 0;
    put32(buf + 4, (unsigned)(p - buf));
    return (size_t)(p - buf);
}

static size_t build_strings(unsigned char *buf)
{
    put32(buf, FFS_STRINGS_MAGIC);
    put32(buf + 4, 16);
    put32(buf + 8, 0);
    put32(buf + 12, 0);
    return 16;
}

static void dump_descriptors(void)
{
    unsigned char d[128], s[32];
    size_t dl = build_descriptors(d), sl = build_strings(s), i;

    printf("descriptors ");
    for (i = 0; i < dl; i++) {
        printf("%02x", d[i]);
    }
    printf("\nstrings ");
    for (i = 0; i < sl; i++) {
        printf("%02x", s[i]);
    }
    printf("\n");
    fflush(stdout);
}

static void gadget_init(struct gadget *g, struct cfg *c)
{
    memset(g, 0, sizeof *g);
    g->cfg = c;
    g->ep0 = -1;
    g->server_fd = -1;
    g->client_fd = -1;
    g->up_r = g->up_w = -1;
    g->vid_r = g->vid_w = -1;
    g->rw = c->rw;
    pthread_mutex_init(&g->cli_mu, NULL);
    s_copy(g->name, sizeof g->name, c->name);
    s_copy(g->gdir, sizeof g->gdir, GADGET_ROOT);
    s_cat(g->gdir, sizeof g->gdir, "/");
    s_cat(g->gdir, sizeof g->gdir, c->name);
    s_copy(g->ffs, sizeof g->ffs, c->ffs);
    s_copy(g->sock, sizeof g->sock, c->sock);
    s_copy(g->stick, sizeof g->stick, c->stick);
    s_copy(g->udc, sizeof g->udc, c->udc);
}

static int gadget_udc_default(struct gadget *g)
{
    DIR *d;
    struct dirent *e;

    if (g->udc[0]) {
        return 0;
    }
    d = opendir("/sys/class/udc");
    if (!d) {
        tlogf("usbgadget: no usb device controller (/sys/class/udc is missing)");
        return -1;
    }
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') {
            continue;
        }
        s_copy(g->udc, sizeof g->udc, e->d_name);
        break;
    }
    closedir(d);
    if (!g->udc[0]) {
        tlogf("usbgadget: no usb device controller (/sys/class/udc is empty)");
        return -1;
    }
    return 0;
}

static void gadget_teardown(struct gadget *g)
{
    char path[PATH_MAX];

    pjoin(path, sizeof path, g->gdir, "/UDC");
    if (access(path, F_OK) == 0) {
        int fd = open(path, O_WRONLY | O_CLOEXEC);

        if (fd >= 0) {
            (void)!write(fd, "\n", 1);      /* configfs wants a store, not an empty write */
            close(fd);
        }
    }
    umount2(g->ffs, 0);
    umount2(g->ffs, MNT_DETACH);
    pjoin(path, sizeof path, g->gdir, "/configs/c.1/mass_storage.0");
    unlink_quiet(path);
    pjoin(path, sizeof path, g->gdir, "/configs/c.1/ffs.");
    s_cat(path, sizeof path, g->name);
    unlink_quiet(path);
    pjoin(path, sizeof path, g->gdir, "/configs/c.1/strings/0x409");
    rmdir(path);
    pjoin(path, sizeof path, g->gdir, "/configs/c.1");
    rmdir(path);
    pjoin(path, sizeof path, g->gdir, "/functions/mass_storage.0");
    rmdir(path);
    pjoin(path, sizeof path, g->gdir, "/functions/ffs.");
    s_cat(path, sizeof path, g->name);
    rmdir(path);
    pjoin(path, sizeof path, g->gdir, "/functions");
    rmdir(path);
    pjoin(path, sizeof path, g->gdir, "/strings/0x409");
    rmdir(path);
    pjoin(path, sizeof path, g->gdir, "/strings");
    rmdir(path);
    rmdir(g->gdir);
}

static int gadget_setup(struct gadget *g, int dry)
{
    char path[PATH_MAX], val[PATH_MAX];

    if (!dry && access(GADGET_ROOT, F_OK) < 0) {
        tlogf("usbgadget: %s is not there: no configfs gadget support?", GADGET_ROOT);
        return -1;
    }
    if (access(g->gdir, F_OK) == 0) {
        if (dry) {
            printf("would remove the existing %s and set it up again\n", g->gdir);
        } else {
            tlogf("usbgadget: %s already exists, removing the old instance", g->gdir);
            gadget_teardown(g);
        }
    }
    if (!dry && access(g->stick, R_OK) < 0) {
        tlogf("usbgadget: stick image %s not found, mkusb.py builds one", g->stick);
        return -1;
    }
    if (!dry && gadget_udc_default(g) < 0) {
        return -1;
    }
#define WRITE_ATTR(suffix) do { \
        pjoin(path, sizeof path, g->gdir, suffix); \
        if (dry) { \
            printf("would write: %s <- %s\n", path, val); \
        } else if (write_file(path, val) < 0) { \
            return -1; \
        } \
    } while (0)
    if (!dry) {
        if (mkdir_p(g->gdir) < 0) {
            tlogf("usbgadget: mkdir %s: %s", g->gdir, strerror(errno));
            return -1;
        }
        pjoin(path, sizeof path, g->gdir, "/strings/0x409");
        if (mkdir_p(path) < 0) {
            tlogf("usbgadget: mkdir %s: %s", path, strerror(errno));
            return -1;
        }
    }
    snprintf(val, sizeof val, "0x%04x", VID);
    WRITE_ATTR("/idVendor");
    snprintf(val, sizeof val, "0x%04x", PID);
    WRITE_ATTR("/idProduct");
    snprintf(val, sizeof val, "0x0100");
    WRITE_ATTR("/bcdDevice");
    snprintf(val, sizeof val, "0x0200");
    WRITE_ATTR("/bcdUSB");
    snprintf(val, sizeof val, "00");
    WRITE_ATTR("/bDeviceClass");       /* per interface, the unit needs this */
    WRITE_ATTR("/bDeviceSubClass");
    WRITE_ATTR("/bDeviceProtocol");
    snprintf(val, sizeof val, "ICC2");
    WRITE_ATTR("/strings/0x409/manufacturer");
    snprintf(val, sizeof val, "ICC2 livi link");
    WRITE_ATTR("/strings/0x409/product");
    snprintf(val, sizeof val, "1");
    WRITE_ATTR("/strings/0x409/serialnumber");

    /* the homebrew stick */
    if (!dry) {
        pjoin(path, sizeof path, g->gdir, "/functions/mass_storage.0");
        if (mkdir_p(path) < 0) {
            tlogf("usbgadget: mkdir %s: %s", path, strerror(errno));
            return -1;
        }
    }
    snprintf(val, sizeof val, "1");
    WRITE_ATTR("/functions/mass_storage.0/lun.0/removable");
    snprintf(val, sizeof val, "%s", g->rw ? "0" : "1");
    WRITE_ATTR("/functions/mass_storage.0/lun.0/ro");
    snprintf(val, sizeof val, "%s", g->stick);
    WRITE_ATTR("/functions/mass_storage.0/lun.0/file");

    if (!dry) {
        pjoin(path, sizeof path, g->gdir, "/configs/c.1/strings/0x409");
        if (mkdir_p(path) < 0) {
            tlogf("usbgadget: mkdir %s: %s", path, strerror(errno));
            return -1;
        }
    }
    snprintf(val, sizeof val, "livi");
    WRITE_ATTR("/configs/c.1/strings/0x409/configuration");
    snprintf(val, sizeof val, "250");
    WRITE_ATTR("/configs/c.1/MaxPower");

    /* ffs.<name> is the vendor function; the daemon mounts it and speaks ep0 */
    if (dry) {
        printf("would link: %s/functions/mass_storage.0 and ffs.%s into configs/c.1\n",
               g->gdir, g->name);
        printf("would run: mount --no-canonicalize -t functionfs %s %s\n", g->name, g->ffs);
        return 0;
    }
    pjoin(path, sizeof path, g->gdir, "/functions/ffs.");
    s_cat(path, sizeof path, g->name);
    if (mkdir(path, 0755) < 0 && errno != EEXIST) {
        tlogf("usbgadget: mkdir %s: %s", path, strerror(errno));
        return -1;
    }
    pjoin(path, sizeof path, g->gdir, "/configs/c.1/mass_storage.0");
    unlink_quiet(path);
    pjoin(val, sizeof val, g->gdir, "/functions/mass_storage.0");
    if (symlink(val, path) < 0) {
        tlogf("usbgadget: symlink %s: %s", path, strerror(errno));
        return -1;
    }
    pjoin(path, sizeof path, g->gdir, "/configs/c.1/ffs.");
    s_cat(path, sizeof path, g->name);
    unlink_quiet(path);
    pjoin(val, sizeof val, g->gdir, "/functions/ffs.");
    s_cat(val, sizeof val, g->name);
    if (symlink(val, path) < 0) {
        tlogf("usbgadget: symlink %s: %s", path, strerror(errno));
        return -1;
    }
    if (mkdir_p(g->ffs) < 0) {
        tlogf("usbgadget: mkdir %s: %s", g->ffs, strerror(errno));
        return -1;
    }
    if (mount(g->name, g->ffs, "functionfs", 0, NULL) < 0) {
        tlogf("usbgadget: mount functionfs on %s: %s", g->ffs, strerror(errno));
        return -1;
    }
#undef WRITE_ATTR
    return 0;
}

static int gadget_bind(struct gadget *g)
{
    unsigned char dbuf[128], sbuf[32];
    size_t dl = build_descriptors(dbuf), sl = build_strings(sbuf);
    char path[PATH_MAX];

    pjoin(path, sizeof path, g->ffs, "/ep0");
    g->ep0 = open(path, O_RDWR | O_CLOEXEC);
    if (g->ep0 < 0) {
        tlogf("usbgadget: open %s: %s", path, strerror(errno));
        return -1;
    }
    set_nonblock(g->ep0);
    if (fd_write_all(g->ep0, dbuf, dl) < 0 || fd_write_all(g->ep0, sbuf, sl) < 0) {
        tlogf("usbgadget: functionfs descriptors: %s", strerror(errno));
        close(g->ep0);
        g->ep0 = -1;
        return -1;
    }
    pjoin(path, sizeof path, g->gdir, "/UDC");
    if (write_file(path, g->udc) < 0) {
        close(g->ep0);
        g->ep0 = -1;
        return -1;
    }
    tlogf("usbgadget: bound to %s, listening on %s", g->udc, g->sock);
    return 0;
}

static void log_li_chunk(const unsigned char *data, size_t len)
{
    static const int n_of[8] = { 0, 8, 4, 5, 7, 3, 3, 4 };
    size_t off = 0;

    while (off + 3 <= len && data[off] == 'L' && data[off + 1] == 'I') {
        unsigned t = data[off + 2];
        int n = t < 8 ? n_of[t] : -1;

        if (n <= 0 || off + (size_t)n > len) {
            break;
        }
        if (t == LI_TOUCH) {
            unsigned x = (unsigned)data[off + 3] | (unsigned)data[off + 4] << 8;
            unsigned y = (unsigned)data[off + 5] | (unsigned)data[off + 6] << 8;

            tlogf("usbgadget: unit touch %u,%u %s", x, y, data[off + 7] ? "down" : "up");
        } else if (t == LI_KNOB) {
            tlogf("usbgadget: unit knob %d",
                  data[off + 3] > 127 ? (int)data[off + 3] - 256 : (int)data[off + 3]);
        } else if (t == LI_BUTTON || t == 6 || t == 7) {
            char hex[3 * 8 + 1];
            int i;

            for (i = 0; i < n; i++) {
                snprintf(hex + i * 3, 4, "%02x ", data[off + i]);
            }
            tlogf("usbgadget: unit msg %u %s", t, hex);
        }
        off += (size_t)n;
    }
    if (off == 0 && !(len >= 2 && data[0] == 'L' && data[1] == 'I')) {
        tlogf("usbgadget: unit sent unparsed data %02x%02x%02x%02x (%zu bytes)",
              len > 0 ? data[0] : 0, len > 1 ? data[1] : 0,
              len > 2 ? data[2] : 0, len > 3 ? data[3] : 0, len);
    }
}

static int send_all(int fd, const unsigned char *data, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = send(fd, data + off, len - off, MSG_NOSIGNAL);

        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = { fd, POLLOUT, 0 };

            if (poll(&p, 1, 200) > 0) {
                continue;
            }
        }
        return -1;
    }
    return 0;
}

static void pump_deliver(struct gadget *g, const unsigned char *data, size_t len)
{
    if (g->up_w >= 0) {
        ssize_t n = write(g->up_w, data, len);

        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            tlogf("usbgadget: upstream pipe write failed (%s)", strerror(errno));
        }
        return;
    }
    pthread_mutex_lock(&g->cli_mu);
    if (g->client_fd >= 0) {
        double t0 = now_sec();

        if (send_all(g->client_fd, data, len) < 0) {
            /* broken client: the bridge loop notices and cleans up, we must not close
             * the fd under it (a double close could hit a reused fd) */
            g->client_dead = 1;
        } else if (now_sec() - t0 > 0.1) {
            tlogf("usbgadget: slow client forward %.0f ms (a unit write could time out)",
                  (now_sec() - t0) * 1000.0);
        }
    }
    pthread_mutex_unlock(&g->cli_mu);
}

static void *ep0_thread(void *arg)
{
    struct gadget *g = arg;
    unsigned char ev[12];

    while (!g->stop && !g_stop) {
        struct pollfd p = { g->ep0, POLLIN, 0 };
        ssize_t n;
        int r;

        if (g->ep0 < 0) {
            break;
        }
        r = poll(&p, 1, 200);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (r == 0) {
            continue;
        }
        n = read(g->ep0, ev, sizeof ev);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            tlogf("usbgadget: ep0 read failed (%s)", strerror(errno));
            g->unbind = 1;
            break;
        }
        if (n < 12) {
            continue;
        }
        if (ev[8] == FFS_ENABLE) {
            g->gen++;
            g->enabled = 1;
            tlogf("usbgadget: unit enabled the gadget");
        } else if (ev[8] == FFS_DISABLE) {
            g->enabled = 0;
            tlogf("usbgadget: unit disabled the gadget");
        } else if (ev[8] == FFS_UNBIND) {
            g->enabled = 0;
            g->unbind = 1;
            g->rebind_at = now_sec() + 1.0;
            tlogf("usbgadget: function unbound, will rebind");
            break;
        }
    }
    return NULL;
}

static void *pump_thread(void *arg)
{
    struct gadget *g = arg;
    unsigned char buf[16384];
    int ep = -1, gen = -1;

    while (!g->stop && !g_stop) {
        struct pollfd p;
        ssize_t n;

        if (!g->enabled) {
            if (ep >= 0) {
                close(ep);
                ep = -1;
                g->ep2_open = 0;
            }
            gen = -1;
            msleep(20);
            continue;
        }
        if (gen != g->gen) {
            char path[PATH_MAX];
            int fd;

            if (ep >= 0) {
                close(ep);
                g->ep2_open = 0;
            }
            pjoin(path, sizeof path, g->ffs, "/ep2");
            fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                msleep(20);
                continue;
            }
            ep = fd;
            gen = g->gen;
            g->ep2_open = 1;
            tlogf("usbgadget: ack/touch endpoint open");
        }
        p.fd = ep;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, 200) <= 0) {
            continue;
        }
        n = read(ep, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            tlogf("usbgadget: ack/touch read failed (%s), waiting for the next enable",
                  strerror(errno));
            close(ep);
            ep = -1;
            g->ep2_open = 0;
            gen = -1;
            continue;
        }
        if (n == 0) {
            continue;
        }
        log_li_chunk(buf, (size_t)n);
        pump_deliver(g, buf, (size_t)n);
    }
    if (ep >= 0) {
        close(ep);
        g->ep2_open = 0;
    }
    return NULL;
}

/* push the pending buffer to ep1 in whole maxpackets (a partial write becomes a short
 * packet on the wire and the host's multi-qtd urb leaves the rest unretired). when the
 * sender paused with less than a packet in flight, write it anyway: the unit can be
 * waiting for exactly those bytes while the sender waits for an ack, which is a
 * deadlock; the host compacts the short packet. */
static size_t ep1_forward(struct gadget *g, int ep1, int gen, unsigned char *pending,
                          size_t plen, int got, int *alive)
{
    size_t n = plen - plen % MPS;

    *alive = 1;
    if (!n) {
        if (!got && plen) {
            n = plen;
        } else {
            return plen;
        }
    }
    while (n) {
        ssize_t w;

        if (g_stop || g->stop || !g->enabled || g->gen != gen) {
            *alive = 0;
            return 0;
        }
        w = write(ep1, pending, n);
        if (w >= 0) {
            memmove(pending, pending + w, plen - (size_t)w);
            plen -= (size_t)w;
            n -= (size_t)w;
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd p = { ep1, POLLOUT, 0 };

            poll(&p, 1, 200);
            continue;
        }
        tlogf("usbgadget: video write failed (%s), waiting for the next enable",
              strerror(errno));
        *alive = 0;
        return 0;
    }
    return plen;
}

static int client_gone(int conn)
{
    struct pollfd p = { conn, POLLIN, 0 };
    char c;

    if (poll(&p, 1, 0) <= 0) {
        return 0;
    }
    if (recv(conn, &c, 1, MSG_PEEK) == 0) {
        return 1;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        return 1;
    }
    return 0;
}

/* open ep1 for the current enable generation. the caller closes any previous fd first
 * (musl has no reopen). a failed open arms the 1 s retry. returns the fd or -1. */
static int ep1_acquire(struct gadget *g, int *ep1, int *gen)
{
    char path[PATH_MAX];
    int fd;

    if (*ep1 >= 0) {
        close(*ep1);
        g->ep1_open = 0;
        *ep1 = -1;
    }
    pjoin(path, sizeof path, g->ffs, "/ep1");
    fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        g->ep1_dead_gen = g->gen;
        g->ep1_retry_at = now_sec() + 1.0;
        return -1;
    }
    *ep1 = fd;
    *gen = g->gen;
    g->ep1_dead_gen = -1;
    g->ep1_open = 1;
    tlogf("usbgadget: video endpoint open");
    return 0;
}

/* one client connection: socket -> ep1, until the client goes away. the unit -> host
 * direction belongs to the pump thread and keeps running across clients. the unit
 * re-selects the configuration as its class drivers and the player come and go and the
 * kernel tears the function down and back up under us; the client stays connected and
 * ep1 is reopened on the next enable. */
static void bridge_client(struct gadget *g, int conn)
{
    /* one request in flight at 256 KB instead of four at 64 KB: ffs completes a write
     * before accepting the next, so the per-frame wakeup count drops from twelve to three
     * on a phone whose cpus leave idle slowly. static, not on the stack: musl's default
     * thread stack is 128 KB. only one client is bridged at a time. */
    static unsigned char pending[EP_BUF_LEN];
    size_t plen = 0;
    int ep1 = -1, gen = -1;

    while (!g->stop && !g_stop && !g->client_dead) {
        if (!g->enabled) {
            if (ep1 >= 0) {
                close(ep1);
                ep1 = -1;
                g->ep1_open = 0;
            }
            gen = -1;
            plen = 0;
            if (client_gone(conn)) {
                break;
            }
            msleep(20);
            continue;
        }
        if (gen != g->gen
            || (ep1 < 0 && g->ep1_dead_gen == g->gen && now_sec() >= g->ep1_retry_at)) {
            if (ep1_acquire(g, &ep1, &gen) < 0) {
                msleep(20);
                continue;
            }
            plen = 0;
        }
        if (ep1 < 0) {
            /* the endpoint is shut down; wait for a fresh enable (gen bump) or the retry */
            plen = 0;
            msleep(20);
            continue;
        }
        {
            struct pollfd p = { conn, POLLIN, 0 };
            int got = 0, alive = 1;
            int r = poll(&p, 1, 50);

            if (r > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
                ssize_t n = recv(conn, pending + plen, sizeof pending - plen, 0);

                if (n > 0) {
                    plen += (size_t)n;
                    got = 1;
                } else if (n == 0) {
                    break;
                } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                    break;
                }
            }
            plen = ep1_forward(g, ep1, gen, pending, plen, got, &alive);
            if (!alive) {
                close(ep1);
                ep1 = -1;
                g->ep1_open = 0;
                plen = 0;
                if (gen != g->gen) {
                    gen = -1;       /* a new enable is already in: reopen at once */
                } else {
                    /* a dead ffs endpoint stays dead until the function is enabled
                     * again; reopen at most once a second instead of every 50 ms, which
                     * used to flood the log and keep the endpoint unusable */
                    g->ep1_dead_gen = g->gen;
                    g->ep1_retry_at = now_sec() + 1.0;
                    msleep(20);
                }
            }
        }
    }
    if (ep1 >= 0) {
        close(ep1);
        g->ep1_open = 0;
    }
}

/* run mode: the sender writes frames into vid_w; this thread moves them to ep1 */
static void *video_thread(void *arg)
{
    struct gadget *g = arg;
    /* same 256 KB request shape as bridge_client; static because a musl pthread stack is
     * only 128 KB and this thread is the only one that touches it */
    static unsigned char pending[EP_BUF_LEN];
    size_t plen = 0;
    int ep1 = -1, gen = -1;

    while (!g->stop && !g_stop) {
        if (!g->enabled) {
            if (ep1 >= 0) {
                close(ep1);
                ep1 = -1;
                g->ep1_open = 0;
            }
            gen = -1;
            plen = 0;
            msleep(20);
            continue;
        }
        if (gen != g->gen
            || (ep1 < 0 && g->ep1_dead_gen == g->gen && now_sec() >= g->ep1_retry_at)) {
            if (ep1_acquire(g, &ep1, &gen) < 0) {
                msleep(20);
                continue;
            }
            plen = 0;
        }
        if (ep1 < 0) {
            /* the endpoint is shut down; wait for a fresh enable (gen bump) or the retry */
            plen = 0;
            msleep(20);
            continue;
        }
        {
            struct pollfd p = { g->vid_r, POLLIN, 0 };
            int got = 0, alive = 1;

            if (poll(&p, 1, 50) > 0 && (p.revents & POLLIN)) {
                ssize_t n = read(g->vid_r, pending + plen, sizeof pending - plen);

                if (n > 0) {
                    plen += (size_t)n;
                    got = 1;
                }
            }
            plen = ep1_forward(g, ep1, gen, pending, plen, got, &alive);
            if (!alive) {
                close(ep1);
                ep1 = -1;
                g->ep1_open = 0;
                plen = 0;
                if (gen != g->gen) {
                    gen = -1;       /* a new enable is already in: reopen at once */
                } else {
                    /* a dead ffs endpoint stays dead until the function is enabled
                     * again; reopen at most once a second instead of every 50 ms, which
                     * used to flood the log and keep the endpoint unusable */
                    g->ep1_dead_gen = g->gen;
                    g->ep1_retry_at = now_sec() + 1.0;
                    msleep(20);
                }
            }
        }
    }
    if (ep1 >= 0) {
        close(ep1);
        g->ep1_open = 0;
    }
    return NULL;
}

static void gadget_wait_endpoints(struct gadget *g, int ms)
{
    int waited = 0;

    while (waited < ms && (g->ep1_open || g->ep2_open)) {
        msleep(10);
        waited += 10;
    }
}

/* the function can be unbound under us (the unit's stack re-selects configurations, or
 * somebody tears the gadget down): close the endpoints, write the descriptors and the
 * udc again. if the light path fails, rebuild the whole configfs gadget. */
static void gadget_rebind(struct gadget *g)
{
    tlogf("usbgadget: rebinding");
    g->enabled = 0;
    gadget_wait_endpoints(g, 1000);
    if (g->ep0 >= 0) {
        close(g->ep0);
        g->ep0 = -1;
    }
    if (g->ep0_started) {
        pthread_join(g->ep0_tid, NULL);
        g->ep0_started = 0;
    }
    if (gadget_bind(g) == 0) {
        g->unbind = 0;
        g->gen++;
        if (pthread_create(&g->ep0_tid, NULL, ep0_thread, g) == 0) {
            g->ep0_started = 1;
        }
        return;
    }
    tlogf("usbgadget: rebind failed, rebuilding the gadget");
    gadget_teardown(g);
    msleep(500);
    if (gadget_setup(g, 0) < 0 || gadget_bind(g) < 0) {
        tlogf("usbgadget: gadget setup failed, retrying in 2 s");
        g->rebind_at = now_sec() + 2.0;
        return;
    }
    g->unbind = 0;
    g->gen++;
    if (!g->ep0_started && pthread_create(&g->ep0_tid, NULL, ep0_thread, g) == 0) {
        g->ep0_started = 1;
    }
}

static int gadget_listen(struct gadget *g)
{
    struct sockaddr_un addr;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0) {
        return -1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(g->sock) >= sizeof addr.sun_path) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    s_copy(addr.sun_path, sizeof addr.sun_path, g->sock);
    unlink_quiet(g->sock);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    /* the daemon needs root for configfs but the sender does not, so a root-owned
     * 0755 socket would leave a plain sender connecting until its timeout */
    chmod(g->sock, 0666);
    g->server_fd = fd;
    return 0;
}

static struct gadget *g_run_gadget;         /* set in run mode: the sender services its rebind */

static void maybe_rebind(void)
{
    if (g_run_gadget && g_run_gadget->unbind && now_sec() >= g_run_gadget->rebind_at) {
        gadget_rebind(g_run_gadget);
    }
}

static int gadget_start_threads(struct gadget *g, int with_video)
{
    if (pthread_create(&g->ep0_tid, NULL, ep0_thread, g) != 0) {
        return -1;
    }
    g->ep0_started = 1;
    if (pthread_create(&g->pump_tid, NULL, pump_thread, g) != 0) {
        return -1;
    }
    g->pump_started = 1;
    if (with_video) {
        if (pthread_create(&g->video_tid, NULL, video_thread, g) != 0) {
            return -1;
        }
        g->video_started = 1;
    }
    return 0;
}

static void gadget_stop_threads(struct gadget *g)
{
    g->stop = 1;
    g->enabled = 0;
    if (g->ep0_started) {
        pthread_join(g->ep0_tid, NULL);
        g->ep0_started = 0;
    }
    if (g->pump_started) {
        pthread_join(g->pump_tid, NULL);
        g->pump_started = 0;
    }
    if (g->video_started) {
        pthread_join(g->video_tid, NULL);
        g->video_started = 0;
    }
    if (g->ep0 >= 0) {
        close(g->ep0);
        g->ep0 = -1;
    }
}

static void gadget_serve(struct gadget *g)
{
    while (!g_stop && !g->stop) {
        struct pollfd p = { g->server_fd, POLLIN, 0 };
        int r;

        if (g->unbind && now_sec() >= g->rebind_at) {
            gadget_rebind(g);
        }
        r = poll(&p, 1, 1000);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (r == 0 || !(p.revents & POLLIN)) {
            continue;
        }
        {
            int conn = accept(g->server_fd, NULL, NULL);

            if (conn < 0) {
                continue;
            }
            set_nonblock(conn);
            pthread_mutex_lock(&g->cli_mu);
            g->client_fd = conn;
            g->client_dead = 0;
            pthread_mutex_unlock(&g->cli_mu);
            tlogf("usbgadget: stream client connected");
            bridge_client(g, conn);
            tlogf("usbgadget: stream client gone");
            pthread_mutex_lock(&g->cli_mu);
            g->client_fd = -1;
            pthread_mutex_unlock(&g->cli_mu);
            close(conn);
        }
    }
}

/* ---- entry points ----------------------------------------------------------------------- */

static int stream_main(struct cfg *c)
{
    struct stream s;
    struct outlink o;

    if (stream_init(&s, c) < 0) {
        return 1;
    }
    memset(&o, 0, sizeof o);
    o.fd = -1;
    stream_loop(&s, &o, -1);
    stats_line(&s, 1);
    stream_cleanup(&s);
    return 0;
}

static int gadget_main(struct cfg *c)
{
    struct gadget g;

    gadget_init(&g, c);
    if (c->dump_desc) {
        dump_descriptors();
        return 0;
    }
    if (gadget_setup(&g, c->dry) < 0) {
        return 1;
    }
    if (c->dry) {
        printf("would mount functionfs at %s and bridge ep1/ep2 to %s\n", g.ffs, g.sock);
        return 0;
    }
    if (gadget_bind(&g) < 0) {
        gadget_teardown(&g);
        return 1;
    }
    if (gadget_listen(&g) < 0) {
        tlogf("usbgadget: listen on %s: %s", g.sock, strerror(errno));
        gadget_teardown(&g);
        return 1;
    }
    if (gadget_start_threads(&g, 0) < 0) {
        tlogf("usbgadget: cannot start the gadget threads");
        gadget_stop_threads(&g);
        gadget_teardown(&g);
        return 1;
    }
    gadget_serve(&g);
    gadget_stop_threads(&g);
    if (g.server_fd >= 0) {
        close(g.server_fd);
    }
    unlink_quiet(g.sock);
    gadget_teardown(&g);
    return 0;
}

static int run_main(struct cfg *c)
{
    struct gadget g;
    struct stream s;
    struct outlink o;
    int vid[2] = { -1, -1 }, up[2] = { -1, -1 };

    gadget_init(&g, c);
    if (c->dump_desc) {
        dump_descriptors();
        return 0;
    }
    if (gadget_setup(&g, c->dry) < 0) {
        return 1;
    }
    if (c->dry) {
        printf("would mount functionfs at %s and stream directly to ep1\n", g.ffs);
        return 0;
    }
    if (pipe2(vid, O_NONBLOCK | O_CLOEXEC) < 0
        || pipe2(up, O_NONBLOCK | O_CLOEXEC) < 0) {
        tlogf("usbgadget: pipe: %s", strerror(errno));
        return 1;
    }
    /* a whole 768 KB frame fits: the sender can finish a frame into the pipe without
     * waiting on the usb and start the next capture while ep1 still drains this one */
    (void)fcntl(vid[1], F_SETPIPE_SZ, VID_PIPE_BYTES);
    g.vid_r = vid[0];
    g.vid_w = vid[1];
    g.up_r = up[0];
    g.up_w = up[1];
    if (gadget_bind(&g) < 0) {
        gadget_teardown(&g);
        return 1;
    }
    if (gadget_start_threads(&g, 1) < 0) {
        tlogf("usbgadget: cannot start the gadget threads");
        gadget_stop_threads(&g);
        gadget_teardown(&g);
        return 1;
    }
    g_run_gadget = &g;
    if (stream_init(&s, c) < 0) {
        g_run_gadget = NULL;
        gadget_stop_threads(&g);
        gadget_teardown(&g);
        return 1;
    }
    memset(&o, 0, sizeof o);
    o.fd = g.vid_w;
    o.is_pipe = 1;
    stream_loop(&s, &o, g.up_r);
    stats_line(&s, 1);
    stream_cleanup(&s);
    g_run_gadget = NULL;
    gadget_stop_threads(&g);
    close(vid[0]);
    close(vid[1]);
    close(up[0]);
    close(up[1]);
    gadget_teardown(&g);
    return 0;
}

/* ---- arguments -------------------------------------------------------------------------- */

static void cfg_init(struct cfg *c)
{
    memset(c, 0, sizeof *c);
    c->width = 800;
    c->height = 480;
    c->fps = 30;
    c->window = 2;
    s_copy(c->sock, sizeof c->sock, DEFAULT_SOCK);
    s_copy(c->name, sizeof c->name, DEFAULT_NAME);
    default_stick(c->stick, sizeof c->stick);
    default_ffs(c->ffs, sizeof c->ffs);
}

static int parse_args(int argc, char **argv, struct cfg *c, int allow_gadget)
{
    int i;

    for (i = 0; i < argc; i++) {
        const char *v;

        if (!strcmp(argv[i], "--usb")) {
            c->usb = 1;
        } else if (!strcmp(argv[i], "--test")) {
            c->use_test = 1;
        } else if (!strcmp(argv[i], "--motion")) {
            c->use_motion = 1;
        } else if (!strcmp(argv[i], "--stats")) {
            c->stats = 1;
        } else if (!strcmp(argv[i], "--no-inject")) {
            c->no_inject = 1;
        } else if (!strcmp(argv[i], "--no-chunked")) {
            c->no_chunked = 1;
        } else if (!strcmp(argv[i], "--flip")) {
            c->flip = 1;
        } else if ((v = opt_arg(argc, argv, &i, "--file"))) {
            c->file = v;
        } else if ((v = opt_arg(argc, argv, &i, "--wayland"))) {
            c->wayland = v;
        } else if ((v = opt_arg(argc, argv, &i, "--events"))) {
            c->events = v;
        } else if ((v = opt_arg(argc, argv, &i, "--width"))) {
            c->width = (int)strtol(v, NULL, 0);
        } else if ((v = opt_arg(argc, argv, &i, "--height"))) {
            c->height = (int)strtol(v, NULL, 0);
        } else if ((v = opt_arg(argc, argv, &i, "--fps"))) {
            c->fps = (int)strtol(v, NULL, 0);
        } else if ((v = opt_arg(argc, argv, &i, "--window"))) {
            c->window = (int)strtol(v, NULL, 0);
        } else if ((v = opt_arg(argc, argv, &i, "--seconds"))) {
            c->seconds = strtod(v, NULL);
        } else if (allow_gadget && !strcmp(argv[i], "--rw")) {
            c->rw = 1;
        } else if (!strcmp(argv[i], "--dry-run")) {
            c->dry = 1;
        } else if (!strcmp(argv[i], "--dump-descriptors")) {
            c->dump_desc = 1;
        } else if (allow_gadget && (v = opt_arg(argc, argv, &i, "--stick"))) {
            s_copy(c->stick, sizeof c->stick, v);
        } else if (allow_gadget && (v = opt_arg(argc, argv, &i, "--sock"))) {
            s_copy(c->sock, sizeof c->sock, v);
        } else if (allow_gadget && (v = opt_arg(argc, argv, &i, "--udc"))) {
            s_copy(c->udc, sizeof c->udc, v);
        } else if (allow_gadget && (v = opt_arg(argc, argv, &i, "--name"))) {
            s_copy(c->name, sizeof c->name, v);
        } else if (allow_gadget && (v = opt_arg(argc, argv, &i, "--ffs-mount"))) {
            s_copy(c->ffs, sizeof c->ffs, v);
        } else if (argv[i][0] == '-' && argv[i][1]) {
            return -1;
        } else if (!c->path) {
            c->path = argv[i];
        } else {
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct cfg c;
    struct sigaction sa;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (argc < 2) {
        fputs(stream_usage(), stderr);
        fputs(gadget_usage(), stderr);
        fputs(run_usage(), stderr);
        return 2;
    }
    if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h") || !strcmp(argv[1], "help")) {
        fputs(stream_usage(), stdout);
        fputs(gadget_usage(), stdout);
        fputs(run_usage(), stdout);
        return 0;
    }
    cfg_init(&c);
    if (!strcmp(argv[1], "stream")) {
        if (parse_args(argc - 2, argv + 2, &c, 0) < 0 || !c.path) {
            fputs(stream_usage(), stderr);
            return 2;
        }
        return stream_main(&c);
    }
    if (!strcmp(argv[1], "gadget")) {
        if (parse_args(argc - 2, argv + 2, &c, 1) < 0) {
            fputs(gadget_usage(), stderr);
            return 2;
        }
        return gadget_main(&c);
    }
    if (!strcmp(argv[1], "run")) {
        if (parse_args(argc - 2, argv + 2, &c, 1) < 0) {
            fputs(run_usage(), stderr);
            return 2;
        }
        c.path = c.sock;                    /* only for log lines */
        return run_main(&c);
    }
    fputs(stream_usage(), stderr);
    fputs(gadget_usage(), stderr);
    fputs(run_usage(), stderr);
    return 2;
}

