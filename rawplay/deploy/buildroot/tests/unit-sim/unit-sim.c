/*
 * unit-sim - play the car's rawplay on the wire, inside a qemu guest.
 *
 * qemu virt has no i.MX31 and no panel, so the other end of rawlink's link is
 * this program: it enumerates the dummy_hcd gadget, finds the vendor interface
 * by class 0xff/0xff/0xff, reads whole maxpacket-aligned LR messages, checks
 * the MODE/FRAME headers and lengths, acks every frame with an LI type-8
 * message, and sends the 1 s ready heartbeat. It records T5 (first frame) and
 * T6 (first frame that is not a single colour, i.e. LIVI's UI is up) in the
 * guest uptime clock, and it checks that the mass-storage LUN appears, mounts
 * read-only and contains homebrew/apps/rawplay.sh.
 *
 * It is a test instrument. smoke.sh boots the qemu image, waits for the T6
 * line in /run/livi-boot.log and fails with the log tail otherwise.
 *
 * events (uptime seconds, from /proc/uptime, the same clock as kernel
 * printk timestamps): T5 <u> seq=<n> / T6 <u> seq=<n> colours=<n>.
 */
#include <errno.h>
#include <fcntl.h>
#include <libusb-1.0/libusb.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VID 0x1209
#define PID 0x1cc2

#define LR_HDR 16
#define LR_MODE 1
#define LR_FRAME 2
#define MSG_HDR 512
#define FMT_RGB565 1

#define LI_READY 5
#define LI_RAW 8

#define MAX_MSG (MSG_HDR + 1024 * 1024)
#define READ_CHUNK (256 * 1024)

struct sim {
    libusb_device_handle *dev;
    unsigned char *buf;
    size_t len, cap;
    int ep_in, ep_out;
    int have_mode;
    unsigned width, height, frame_len;
    unsigned frames, bad;
    int have_t5, have_t6;
    double last_ready;
    FILE *log;
};

static volatile sig_atomic_t g_stop;
static int g_fail;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static double uptime_seconds(void)
{
    FILE *f = fopen("/proc/uptime", "r");
    double up = 0.0;

    if (f) {
        if (fscanf(f, "%lf", &up) != 1) {
            up = 0.0;
        }
        fclose(f);
    }
    return up;
}

static void log_line(struct sim *s, const char *fmt, ...)
{
    char msg[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("unit-sim: %s\n", msg);
    fflush(stdout);
    if (s->log) {
        fprintf(s->log, "%s\n", msg);
        fflush(s->log);
    }
}

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static unsigned rd32(const unsigned char *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16
           | (unsigned)p[3] << 24;
}

/* the uptime clock the milestone lines and the kernel log share */
static double guest_uptime(void)
{
    return uptime_seconds();
}

static void milestone(struct sim *s, const char *id, const char *fmt, ...)
{
    char msg[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (s->log) {
        fprintf(s->log, "%s %.3f %s\n", id, guest_uptime(), msg);
        fflush(s->log);
    }
    log_line(s, "%s %.3f %s", id, guest_uptime(), msg);
}

static int send_li(struct sim *s, const unsigned char *msg, size_t n)
{
    int transferred = 0;
    int rc;

    rc = libusb_bulk_transfer(s->dev, (unsigned char)s->ep_out,
                              (unsigned char *)msg, (int)n, &transferred, 500);
    if (rc != 0 || transferred != (int)n) {
        log_line(s, "ep%02x write failed: %s", s->ep_out, libusb_error_name(rc));
        g_fail = 1;
        return -1;
    }
    return 0;
}

static void send_ready(struct sim *s)
{
    unsigned char msg[3] = { 'L', 'I', LI_READY };

    if (send_li(s, msg, sizeof msg) == 0) {
        s->last_ready = now_sec();
    }
}

static void send_ack(struct sim *s, unsigned seq, unsigned status)
{
    unsigned char msg[8];

    msg[0] = 'L';
    msg[1] = 'I';
    msg[2] = LI_RAW;
    msg[3] = (unsigned char)seq;
    msg[4] = (unsigned char)(seq >> 8);
    msg[5] = (unsigned char)(seq >> 16);
    msg[6] = (unsigned char)(seq >> 24);
    msg[7] = (unsigned char)status;
    send_li(s, msg, sizeof msg);
}

/* one frame's payload is RGB565; all-one-colour means blank (black, white,
 * a solid clear colour). sample 4096 pixel pairs: enough to catch the UI. */
static unsigned frame_colours(const unsigned char *p, unsigned len)
{
    unsigned seen = 1, i;
    unsigned char a = p[0], b = p[1];

    for (i = 2; i + 1 < len; i += 2) {
        if (p[i] != a || p[i + 1] != b) {
            seen++;
            a = p[i];
            b = p[i + 1];
            if (seen > 8) {
                break;
            }
        }
    }
    return seen;
}

static void handle_message(struct sim *s, const unsigned char *m, size_t n)
{
    unsigned type = m[2];
    unsigned len = rd32(m + 4);
    unsigned seq = rd32(m + 8);

    if (type == LR_MODE) {
        unsigned w = rd32(m + LR_HDR);
        unsigned h = rd32(m + LR_HDR + 4);
        unsigned stride = rd32(m + LR_HDR + 8);
        unsigned fmt = rd32(m + LR_HDR + 12) & 0xff;

        if (len != 16 || stride != w * 2 || fmt != FMT_RGB565
            || w == 0 || h == 0 || w > 4096 || h > 4096) {
            log_line(s, "bad MODE w=%u h=%u stride=%u fmt=%u", w, h, stride, fmt);
            s->bad++;
            return;
        }
        if (!s->have_mode || s->width != w || s->height != h) {
            log_line(s, "MODE %ux%u stride %u fmt RGB565", w, h, stride);
        }
        s->width = w;
        s->height = h;
        s->frame_len = w * h * 2;
        s->have_mode = 1;
        return;
    }

    if (type == LR_FRAME) {
        const unsigned char *payload;

        if (!s->have_mode) {
            log_line(s, "FRAME before MODE, ignored");
            s->bad++;
            return;
        }
        if (n != MSG_HDR + (size_t)s->frame_len || len != s->frame_len) {
            log_line(s, "bad FRAME n=%zu len=%u expected %u", n, len, s->frame_len);
            s->bad++;
            return;
        }
        payload = m + MSG_HDR;
        s->frames++;
        if (!s->have_t5) {
            s->have_t5 = 1;
            milestone(s, "T5", "seq=%u len=%u", seq, len);
        }
        if (!s->have_t6) {
            unsigned colours = frame_colours(payload, len);

            if (colours > 1) {
                s->have_t6 = 1;
                milestone(s, "T6", "seq=%u colours=%u", seq, colours);
            }
        }
        send_ack(s, seq, 1);
        return;
    }
    /* an unknown type is not fatal (the wire is allowed to grow), but count it */
    s->bad++;
}

/* consume whole messages from the front of buf. every message is a whole
 * number of 512-byte packets, so resync is a header scan at packet bounds. */
static void consume(struct sim *s)
{
    size_t off = 0;

    while (s->len - off >= LR_HDR) {
        const unsigned char *m = s->buf + off;
        unsigned type, len;
        size_t need;

        if (m[0] != 'L' || m[1] != 'R') {
            off++;
            continue;
        }
        type = m[2];
        len = rd32(m + 4);
        if (type == LR_MODE) {
            need = MSG_HDR;
        } else if (type == LR_FRAME) {
            need = MSG_HDR + (size_t)len;
        } else {
            off++;
            continue;
        }
        if (need > MAX_MSG || (type == LR_FRAME && len == 0)) {
            off++;
            continue;
        }
        if (s->len - off < need) {
            break;
        }
        handle_message(s, m, need);
        off += need;
    }
    if (off) {
        memmove(s->buf, s->buf + off, s->len - off);
        s->len -= off;
    }
    if (s->len == s->cap) {
        log_line(s, "input buffer full (%zu), dropping the oldest bytes", s->cap);
        memmove(s->buf, s->buf + 1, --s->len);
    }
}

static int check_stick(void)
{
    char dir[] = "/tmp/unit-sim-stick";
    char path[128];
    int i, rc = -1;

    /* The usb-storage probe needs a moment after enumeration, and the stick
     * image is MBR + one FAT partition (mkusb.py's layout), so the mountable
     * node is /dev/sdX1, not the whole disk. Prefer a partition node; fall
     * back to the whole disk for an image without a partition table. */
    for (i = 0; i < 100; i++) {
        FILE *f = popen("ls -1 /dev/sd*[0-9] 2>/dev/null | head -1; "
                        "ls -1 /dev/sd? 2>/dev/null | head -1", "r");
        char dev[64] = "";
        char node[128];

        if (f) {
            if (fgets(dev, sizeof dev, f)) {
                size_t l = strlen(dev);

                if (l && dev[l - 1] == '\n') {
                    dev[l - 1] = 0;
                }
            }
            pclose(f);
        }
        if (dev[0] == 0) {
            usleep(200 * 1000);
            continue;
        }
        snprintf(node, sizeof node, "%s", dev);
        mkdir(dir, 0755);
        /* MS_RDONLY is the flag under test: the gadget's LUN is ro=1 and the
         * unit mounts the stick read-only */
        if (mount(node, dir, "vfat", MS_RDONLY, NULL) != 0) {
            usleep(200 * 1000);
            continue;
        }
        snprintf(path, sizeof path, "%s/homebrew/apps/rawplay.sh", dir);
        if (access(path, R_OK) == 0) {
            printf("unit-sim: stick %s mounted read-only with homebrew/apps/rawplay.sh\n",
                   node);
            rc = 0;
        } else {
            printf("unit-sim: stick %s mounted read-only but %s is missing\n",
                   node, path);
            rc = -1;
        }
        umount(dir);
        rmdir(dir);
        break;
    }
    if (rc != 0) {
        printf("unit-sim: mass-storage LUN did not appear as a readable stick\n");
    }
    return rc;
}

static int find_endpoints(libusb_device *dev, int *iface, int *ep_in, int *ep_out)
{
    struct libusb_config_descriptor *cfg;
    int rc, i, j;

    rc = libusb_get_active_config_descriptor(dev, &cfg);
    if (rc != 0) {
        return -1;
    }
    for (i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *itf = &cfg->interface[i];
        const struct libusb_interface_descriptor *alt = &itf->altsetting[0];

        if (alt->bInterfaceClass == 0xff && alt->bInterfaceSubClass == 0xff
            && alt->bInterfaceProtocol == 0xff) {
            for (j = 0; j < alt->bNumEndpoints; j++) {
                const struct libusb_endpoint_descriptor *e = &alt->endpoint[j];

                if ((e->bmAttributes & 0x03) == LIBUSB_TRANSFER_TYPE_BULK) {
                    if (e->bEndpointAddress & 0x80) {
                        *ep_in = e->bEndpointAddress;
                    } else {
                        *ep_out = e->bEndpointAddress;
                    }
                }
            }
            *iface = alt->bInterfaceNumber;
            break;
        }
    }
    libusb_free_config_descriptor(cfg);
    return (*ep_in && *ep_out) ? 0 : -1;
}

int main(int argc, char **argv)
{
    struct sim s;
    libusb_device **list;
    libusb_device *target = NULL;
    const char *logpath = "/run/livi-boot.log";
    double seconds = 600.0, started;
    int want_stick = 1;
    int i, rc;

    memset(&s, 0, sizeof s);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--log") && i + 1 < argc) {
            logpath = argv[++i];
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            seconds = strtod(argv[++i], NULL);
        } else if (!strcmp(argv[i], "--no-stick")) {
            want_stick = 0;
        } else {
            fprintf(stderr, "usage: unit-sim [--log FILE] [--seconds S] [--no-stick]\n");
            return 2;
        }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    s.cap = 2 * 1024 * 1024;
    s.buf = malloc(s.cap);
    if (!s.buf) {
        return 1;
    }
    s.log = fopen(logpath, "a");
    if (!s.log) {
        fprintf(stderr, "unit-sim: cannot open %s: %s\n", logpath, strerror(errno));
    }
    printf("unit-sim: waiting for %04x:%04x\n", VID, PID);
    fflush(stdout);

    rc = libusb_init(NULL);
    if (rc != 0) {
        log_line(&s, "libusb_init: %s", libusb_error_name(rc));
        return 1;
    }

    /* wait for rawlink to bind the gadget and dummy_hcd to enumerate it */
    for (i = 0; i < 600 && !g_stop; i++) {
        ssize_t n = libusb_get_device_list(NULL, &list);
        ssize_t k;

        for (k = 0; k < n; k++) {
            struct libusb_device_descriptor d;

            if (libusb_get_device_descriptor(list[k], &d) == 0
                && d.idVendor == VID && d.idProduct == PID) {
                target = libusb_ref_device(list[k]);
                break;
            }
        }
        libusb_free_device_list(list, 1);
        if (target) {
            break;
        }
        usleep(200 * 1000);
    }
    if (!target || g_stop) {
        log_line(&s, "gadget %04x:%04x never appeared", VID, PID);
        return 1;
    }

    rc = libusb_open(target, &s.dev);
    libusb_unref_device(target);
    if (rc != 0) {
        log_line(&s, "cannot open the gadget: %s", libusb_error_name(rc));
        return 1;
    }
    libusb_set_auto_detach_kernel_driver(s.dev, 1);
    if (find_endpoints(libusb_get_device(s.dev), &i, &s.ep_in, &s.ep_out) < 0) {
        log_line(&s, "no vendor bulk endpoint pair");
        return 1;
    }
    rc = libusb_claim_interface(s.dev, i);
    if (rc != 0) {
        log_line(&s, "cannot claim interface %d: %s", i, libusb_error_name(rc));
        return 1;
    }
    log_line(&s, "enumerated %04x:%04x iface %d ep %02x/%02x", VID, PID, i,
             s.ep_in, s.ep_out);

    if (want_stick) {
        rc = check_stick();
        if (rc != 0) {
            g_fail = 1;
        }
    }

    /* the sender sends MODE when it sees the first ready; keep one coming */
    send_ready(&s);
    started = now_sec();
    while (!g_stop && now_sec() - started < seconds) {
        int transferred = 0;

        if (now_sec() - s.last_ready >= 1.0) {
            send_ready(&s);
        }
        if (s.cap - s.len < READ_CHUNK) {
            /* consume() only fails to drain when a message straddles the
             * buffer end; give it a chance, then make room */
            consume(&s);
            if (s.cap - s.len < READ_CHUNK) {
                memmove(s.buf, s.buf + s.len / 2, s.len - s.len / 2);
                s.len -= s.len / 2;
            }
        }
        rc = libusb_bulk_transfer(s.dev, (unsigned char)s.ep_in,
                                  s.buf + s.len, READ_CHUNK, &transferred, 200);
        if (rc == LIBUSB_ERROR_TIMEOUT) {
            continue;
        }
        if (rc == LIBUSB_ERROR_NO_DEVICE) {
            log_line(&s, "gadget went away");
            break;
        }
        if (rc != 0) {
            log_line(&s, "ep%02x read: %s", s.ep_in, libusb_error_name(rc));
            g_fail = 1;
            break;
        }
        if (transferred > 0) {
            s.len += (size_t)transferred;
            consume(&s);
        }
    }

    log_line(&s, "done: %u frames, %u protocol errors, T5=%d T6=%d",
             s.frames, s.bad, s.have_t5, s.have_t6);
    libusb_release_interface(s.dev, i);
    libusb_close(s.dev);
    libusb_exit(NULL);
    if (s.log) {
        fclose(s.log);
    }
    free(s.buf);
    if (!s.have_t6) {
        g_fail = 1;
    }
    return g_fail ? 1 : 0;
}
