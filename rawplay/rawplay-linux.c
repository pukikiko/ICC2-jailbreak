/*
 * rawplay-linux - the generic-linux unit end of the raw link. it receives the same
 * uncompressed rgb565 LR MODE/FRAME stream rawplay/rawplay.c takes on the car
 * (rawplay/README.md section 2.2) and shows it in an SDL window, with mouse/keyboard
 * going back upstream as the same LI touch/knob/button messages plus the ready
 * heartbeat and the per-frame ack.
 *
 * two transports, because the two ends of a gadget test live in different places:
 *
 *   --usb         the pc is the usb host on a cable to a board running
 *                 `rawlink gadget`/`run`: opens 1209:1cc2 over libusb, claims the
 *                 0xff vendor interface and bulk-reads video on its IN endpoint
 *                 (0x83) while bulk-writing input on its OUT endpoint (0x04).
 *                 this is the pc-as-client path: the pc stands in for the car.
 *   --sock PATH   no usb hardware needed: listens on a unix socket the way
 *                 test_rawlink.py's FakeUnit does and `rawlink stream PATH`
 *                 connects to it. this tests the sender, the protocol and this
 *                 client on one machine.
 *
 * usage: rawplay-linux (--usb [--vid VID --pid PID] | --sock PATH) [--seconds S]
 *        [--stats] [--headless] [--dump DIR] [--no-input] [--scale N]
 *
 * keys in the window: mouse press/drag/release is the touch; Left/Right is the knob;
 * Backspace/h/b/n/v/m send the icc/swc button bits rawlink has livi bindings for
 * (25/28/29/30/46/48, press on keydown, release on keyup); Esc or closing the window
 * quits. --headless parses, acks and stats without opening a window (SDL dummy safe,
 * and what the automated loopback test uses).
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <libusb.h>
#include <SDL.h>

/* ---- wire protocol, rawplay/README.md section 2.2 --------------------------------- */

#define LR_MAGIC0   'L'
#define LR_MAGIC1   'R'
#define LR_HDR      16
#define LR_MSG      512    /* every LR message is padded to whole maxpackets on the wire */
#define LR_MODE     1
#define LR_FRAME    2
#define FMT_RGB565  1
#define MODE_LEN    16

#define LI_TOUCH    1
#define LI_KNOB     2
#define LI_BUTTON   3
#define LI_READY    5
#define LI_RAW      8

#define STARTUP_DEAD_MS 15000
#define LINK_DEAD_MS    5000
#define READY_MS        1000

/* sanity bounds for a MODE the sender announces; the panel is 800x480 but this client
 * takes whatever geometry the stream carries (the window follows it). */
#define MAX_DIM     2048
#define MAX_FRAME   (16 * 1024 * 1024)

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void put32le(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static unsigned get32le(const unsigned char *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16
           | (unsigned)p[3] << 24;
}

/* ---- configuration ----------------------------------------------------------------- */

struct cfg {
    int usb;
    int vid, pid;
    const char *sock;
    double seconds;
    int stats, headless, no_input, scale, list_usb;
    const char *dump;
    int init_w, init_h;
};

static void usage(void)
{
    printf("usage: rawplay-linux (--usb [--vid VID --pid PID] | --sock PATH) [--seconds S]\n"
           "       [--stats] [--headless] [--dump DIR] [--no-input] [--scale N]\n"
           "       [--list-usb]\n"
           "  --usb      open the gadget over libusb (default 1209:1cc2) as the car would\n"
           "  --sock     listen on PATH for `rawlink stream PATH` (no usb hardware needed)\n"
           "  --seconds  stop after S seconds\n"
           "  --stats    five second progress lines plus a totals line at exit\n"
           "  --headless no SDL window: parse, ack, stats (and --dump) only\n"
           "  --dump     save every displayed frame as DIR/frameNNNNNN.ppm\n"
           "  --no-input do not send mouse/keyboard upstream\n"
           "  --scale    integer window scale before the first MODE (default 1)\n");
}

static int parse_args(int argc, char **argv, struct cfg *c)
{
    int i;

    memset(c, 0, sizeof *c);
    c->vid = 0x1209;
    c->pid = 0x1cc2;
    c->scale = 1;
    c->init_w = 800;
    c->init_h = 480;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--usb")) {
            c->usb = 1;
        } else if (!strcmp(argv[i], "--sock") && i + 1 < argc) {
            c->sock = argv[++i];
        } else if (!strcmp(argv[i], "--vid") && i + 1 < argc) {
            c->vid = (int)strtol(argv[++i], 0, 0);
        } else if (!strcmp(argv[i], "--pid") && i + 1 < argc) {
            c->pid = (int)strtol(argv[++i], 0, 0);
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            c->seconds = strtod(argv[++i], 0);
        } else if (!strcmp(argv[i], "--stats")) {
            c->stats = 1;
        } else if (!strcmp(argv[i], "--headless")) {
            c->headless = 1;
        } else if (!strcmp(argv[i], "--dump") && i + 1 < argc) {
            c->dump = argv[++i];
        } else if (!strcmp(argv[i], "--no-input")) {
            c->no_input = 1;
        } else if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
            c->scale = (int)strtol(argv[++i], 0, 0);
        } else if (!strcmp(argv[i], "--list-usb")) {
            c->list_usb = 1;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage();
            exit(0);
        } else {
            fprintf(stderr, "rawplay-linux: unknown option %s\n", argv[i]);
            return -1;
        }
    }
    if (c->scale < 1) {
        c->scale = 1;
    }
    if (c->scale > 4) {
        c->scale = 4;
    }
    if (c->list_usb) {
        return 0;
    }
    if (c->usb == !!c->sock) {
        fprintf(stderr, "rawplay-linux: pick exactly one of --usb and --sock PATH\n");
        return -1;
    }
    return 0;
}

/* ---- transport ----------------------------------------------------------------------
 *
 * a buffered byte stream over either libusb bulk endpoints (the pc as usb host,
 * standing in for the car) or an accepted unix-socket connection (the FakeUnit
 * shape, for testing the sender with no usb hardware at all). small reads go
 * through the buffer so the LR header scan never splits a message; frame payloads
 * read straight into the image buffer in chunks.
 */

#define RXBUF 65536

struct xport {
    int is_usb;
    libusb_context *ctx;
    libusb_device_handle *dev;
    uint8_t ep_in, ep_out;
    int iface;
    int sock_listen, sock_conn;
    unsigned char rx[RXBUF];
    size_t rxpos, rxlen;
    int conn_lost;          /* socket peer went away; the caller re-accepts */
    int dead;               /* usb device gone for good */
};

/* returns 1 with a byte buffered, 0 on timeout, -1 when the link is gone.-tox-:-
 * fills in at most quantum_ms so the SDL loop and the heartbeat stay live. */
static int xfill(struct xport *x, int quantum_ms)
{
    ssize_t n;

    if (x->rxpos < x->rxlen) {
        return 1;
    }
    x->rxpos = x->rxlen = 0;
    if (x->is_usb) {
        int got = 0;
        int rc = libusb_bulk_transfer(x->dev, x->ep_in, x->rx, RXBUF, &got,
                                      quantum_ms);
        if (rc == 0 && got > 0) {
            x->rxlen = (size_t)got;
            return 1;
        }
        if (rc == LIBUSB_ERROR_TIMEOUT) {
            return 0;
        }
        /* io errors and disconnects both end the run; the message says which */
        fprintf(stderr, "rawplay-linux: usb bulk-in: %s\n", libusb_strerror(rc));
        x->dead = 1;
        return -1;
    }
    {
        struct pollfd p = { x->sock_conn, POLLIN, 0 };
        int r = poll(&p, 1, quantum_ms);

        if (r < 0) {
            if (errno == EINTR) {
                return 0;
            }
            fprintf(stderr, "rawplay-linux: socket poll: %s\n", strerror(errno));
            x->conn_lost = 1;
            return -1;
        }
        if (r == 0 || !(p.revents & (POLLIN | POLLHUP | POLLERR))) {
            return 0;
        }
        n = recv(x->sock_conn, x->rx, RXBUF, 0);
        if (n > 0) {
            x->rxlen = (size_t)n;
            return 1;
        }
        x->conn_lost = 1;
        return -1;
    }
}

/* read exactly n bytes: buffered leftovers first, then direct chunks so a 768 KB
 * frame does not bounce through the 64 KB staging buffer. same return shape. */
static int xread_exact(struct xport *x, unsigned char *dst, size_t n, long long deadline)
{
    size_t done = 0;

    while (done < n) {
        if (x->rxpos < x->rxlen) {
            size_t take = x->rxlen - x->rxpos;

            if (take > n - done) {
                take = n - done;
            }
            memcpy(dst + done, x->rx + x->rxpos, take);
            x->rxpos += take;
            done += take;
            continue;
        }
        x->rxpos = x->rxlen = 0;
        if (now_ms() >= deadline) {
            return 0;
        }
        if (x->is_usb) {
            int got = 0;
            long long left = deadline - now_ms();
            int q = left > 500 ? 500 : (int)left;
            int rc;

            if (q <= 0) {
                return 0;
            }
            rc = libusb_bulk_transfer(x->dev, x->ep_in, dst + done,
                                      (int)(n - done), &got, q);
            if (rc == 0 && got > 0) {
                done += (size_t)got;
                continue;
            }
            if (rc == LIBUSB_ERROR_TIMEOUT) {
                return 0;
            }
            fprintf(stderr, "rawplay-linux: usb bulk-in: %s\n",
                    libusb_strerror(rc));
            x->dead = 1;
            return -1;
        } else {
            struct pollfd p = { x->sock_conn, POLLIN, 0 };
            long long left = deadline - now_ms();
            int q = left > 500 ? 500 : (int)left;
            int r;

            if (q <= 0) {
                return 0;
            }
            r = poll(&p, 1, q);
            if (r < 0 && errno == EINTR) {
                return 0;
            }
            if (r <= 0) {
                return 0;
            }
            {
                ssize_t m = recv(x->sock_conn, dst + done, n - done, 0);

                if (m > 0) {
                    done += (size_t)m;
                    continue;
                }
                x->conn_lost = 1;
                return -1;
            }
        }
    }
    return 1;
}

static int xwrite_all(struct xport *x, const void *buf, size_t len)
{
    const unsigned char *p = buf;

    if (x->is_usb) {
        while (len) {
            int got = 0;
            int rc = libusb_bulk_transfer(x->dev, x->ep_out, (unsigned char *)p,
                                          (int)len, &got, 500);
            if (rc != 0 || got <= 0) {
                return -1;
            }
            p += got;
            len -= (size_t)got;
        }
        return 0;
    }
    while (len) {
        ssize_t n = send(x->sock_conn, p, len, MSG_NOSIGNAL);

        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

/* ---- usb open ------------------------------------------------------------------------ */

static int list_usb(int vid, int pid)
{
    libusb_context *ctx = NULL;
    libusb_device **devs = NULL;
    ssize_t n, i;

    if (libusb_init(&ctx) != 0) {
        fprintf(stderr, "rawplay-linux: libusb_init failed\n");
        return 1;
    }
    n = libusb_get_device_list(ctx, &devs);
    printf("usb devices matching %04x:%04x:\n", vid, pid);
    for (i = 0; i < n; i++) {
        struct libusb_device_descriptor d;

        if (libusb_get_device_descriptor(devs[i], &d) != 0) {
            continue;
        }
        if (d.idVendor == vid && d.idProduct == pid) {
            printf("  bus %d addr %d: %04x:%04x\n",
                   libusb_get_bus_number(devs[i]),
                   libusb_get_device_address(devs[i]), d.idVendor,
                   d.idProduct);
        }
    }
    if (n >= 0) {
        libusb_free_device_list(devs, 1);
    }
    libusb_exit(ctx);
    return 0;
}

/* find the composite gadget's vendor interface the way common/raw-usb.c does: class
 * 0xff with two bulk endpoints. the IN endpoint carries the video (gadget writes
 * it), the OUT endpoint takes the input. waits up to ~20 s like the unit does. */
static int usb_open(struct xport *x, int vid, int pid)
{
    long long deadline = now_ms() + 20000;
    long long last_note = 0;
    int rc;

    memset(x, 0, sizeof *x);
    x->is_usb = 1;
    x->sock_listen = x->sock_conn = -1;
    if (libusb_init(&x->ctx) != 0) {
        fprintf(stderr, "rawplay-linux: libusb_init failed\n");
        return -1;
    }
    while (!g_stop) {
        libusb_device **devs = NULL;
        ssize_t n, i;

        n = libusb_get_device_list(x->ctx, &devs);
        for (i = 0; n >= 0 && i < n; i++) {
            struct libusb_device_descriptor dd;
            libusb_device_handle *h = NULL;

            if (libusb_get_device_descriptor(devs[i], &dd) != 0) {
                continue;
            }
            if (dd.idVendor != vid || dd.idProduct != pid) {
                continue;
            }
            if (libusb_open(devs[i], &h) != 0) {
                continue;
            }
            {
                struct libusb_config_descriptor *cfgd = NULL;
                int found = 0;

                if (libusb_get_active_config_descriptor(devs[i], &cfgd) == 0
                    && cfgd) {
                    uint8_t j;
                    for (j = 0; j < cfgd->bNumInterfaces && !found; j++) {
                        const struct libusb_interface *iface =
                            &cfgd->interface[j];
                        int k;

                        for (k = 0; k < iface->num_altsetting && !found;
                             k++) {
                            const struct libusb_interface_descriptor *ad =
                                &iface->altsetting[k];
                            uint8_t e;
                            uint8_t in = 0, out = 0;

                            if (ad->bInterfaceClass != 0xff
                                || ad->bNumEndpoints != 2) {
                                continue;
                            }
                            for (e = 0; e < ad->bNumEndpoints; e++) {
                                const struct libusb_endpoint_descriptor *ed =
                                    &ad->endpoint[e];

                                if ((ed->bmAttributes
                                     & LIBUSB_TRANSFER_TYPE_MASK)
                                    != LIBUSB_TRANSFER_TYPE_BULK) {
                                    in = out = 0;
                                    break;
                                }
                                if (ed->bEndpointAddress & 0x80) {
                                    in = ed->bEndpointAddress;
                                } else {
                                    out = ed->bEndpointAddress;
                                }
                            }
                            if (in && out) {
                                x->dev = h;
                                x->ep_in = in;
                                x->ep_out = out;
                                x->iface = ad->bInterfaceNumber;
                                found = 1;
                            }
                        }
                    }
                    libusb_free_config_descriptor(cfgd);
                }
                if (!found) {
                    libusb_close(h);
                    continue;
                }
            }
            if (libusb_kernel_driver_active(h, x->iface) == 1) {
                libusb_detach_kernel_driver(h, x->iface);
            }
            rc = libusb_claim_interface(h, x->iface);
            if (rc != 0) {
                /* busy usually means another client already holds the vendor
                 * interface (or a kernel driver grabbed it); throttle the note
                 * instead of spamming it at 5 Hz for the whole 20 s wait. */
                if (now_ms() - last_note >= 2000) {
                    last_note = now_ms();
                    fprintf(stderr, "rawplay-linux: claim interface %d: %s%s\n",
                            x->iface, libusb_strerror(rc),
                            rc == LIBUSB_ERROR_BUSY
                                ? " (another client holding it?)" : "");
                }
                libusb_close(h);
                x->dev = NULL;
                continue;
            }
            libusb_free_device_list(devs, 1);
            fprintf(stderr, "rawplay-linux: usb %04x:%04x up "
                    "(bulk in %#02x out %#02x, iface %d)\n", vid, pid,
                    x->ep_in, x->ep_out, x->iface);
            return 0;
        }
        if (n >= 0) {
            libusb_free_device_list(devs, 1);
        }
        if (now_ms() >= deadline) {
            fprintf(stderr, "rawplay-linux: no %04x:%04x gadget after 20 s "
                    "(is `rawlink gadget`/`run` up on the other end?)\n", vid,
                    pid);
            libusb_exit(x->ctx);
            x->ctx = NULL;
            return -1;
        }
        usleep(200000);
    }
    libusb_exit(x->ctx);
    x->ctx = NULL;
    return -1;
}

/* ---- socket listen (the FakeUnit shape) ------------------------------------------------ */

static int sock_listen(struct xport *x, const char *path)
{
    struct sockaddr_un addr;
    int fd;

    memset(x, 0, sizeof *x);
    x->sock_listen = x->sock_conn = -1;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        fprintf(stderr, "rawplay-linux: socket: %s\n", strerror(errno));
        return -1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0
        || listen(fd, 1) < 0) {
        fprintf(stderr, "rawplay-linux: listen on %s: %s\n", path,
                strerror(errno));
        close(fd);
        return -1;
    }
    chmod(path, 0666);
    x->sock_listen = fd;
    fprintf(stderr, "rawplay-linux: listening on %s for `rawlink stream %s`\n",
            path, path);
    return 0;
}

/* 0 = connected, -1 = error or g_stop, -2 = stop_at passed while waiting. the
 * deadline check lives inside the wait, not just before it: a sender that ends
 * (--seconds) before this client does would otherwise sit in accept forever. */
static int sock_accept(struct xport *x, long long stop_at, int has_deadline)
{
    while (!g_stop) {
        struct pollfd p = { x->sock_listen, POLLIN, 0 };
        int r = poll(&p, 1, 200);
        int c;

        if (has_deadline && now_ms() >= stop_at) {
            return -2;
        }
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            continue;
        }
        c = accept(x->sock_listen, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "rawplay-linux: accept: %s\n", strerror(errno));
            return -1;
        }
        x->sock_conn = c;
        x->rxpos = x->rxlen = 0;
        x->conn_lost = 0;
        fprintf(stderr, "rawplay-linux: stream connected\n");
        return 0;
    }
    return -1;
}

/* ---- display -------------------------------------------------------------------------- */

struct view {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    int fw, fh;                 /* current frame geometry */
    int scale;
    int headless;
};

static int view_init(struct view *v, int w, int h, int scale, int headless)
{
    memset(v, 0, sizeof *v);
    v->fw = w;
    v->fh = h;
    v->scale = scale;
    v->headless = headless;
    if (headless) {
        return 0;
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "rawplay-linux: SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    v->win = SDL_CreateWindow("rawplay-linux", SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED, w * scale, h * scale,
                              SDL_WINDOW_RESIZABLE);
    if (!v->win) {
        fprintf(stderr, "rawplay-linux: SDL_CreateWindow: %s\n",
                SDL_GetError());
        return -1;
    }
    v->ren = SDL_CreateRenderer(v->win, -1, 0);
    if (!v->ren) {
        fprintf(stderr, "rawplay-linux: SDL_CreateRenderer: %s\n",
                SDL_GetError());
        return -1;
    }
    v->tex = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_RGB565,
                               SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!v->tex) {
        fprintf(stderr, "rawplay-linux: SDL_CreateTexture: %s\n",
                SDL_GetError());
        return -1;
    }
    return 0;
}

static int view_mode(struct view *v, int w, int h)
{
    if (w == v->fw && h == v->fh && v->tex) {
        return 0;
    }
    v->fw = w;
    v->fh = h;
    if (v->headless) {
        return 0;
    }
    SDL_SetWindowSize(v->win, w * v->scale, h * v->scale);
    if (v->tex) {
        SDL_DestroyTexture(v->tex);
    }
    v->tex = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_RGB565,
                               SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!v->tex) {
        fprintf(stderr, "rawplay-linux: SDL_CreateTexture %dx%d: %s\n", w, h,
                SDL_GetError());
        return -1;
    }
    return 0;
}

static void view_show(struct view *v, const unsigned char *rgb565, int stride)
{
    if (v->headless) {
        return;
    }
    SDL_UpdateTexture(v->tex, NULL, rgb565, stride);
    SDL_RenderClear(v->ren);
    SDL_RenderCopy(v->ren, v->tex, NULL, NULL);
    SDL_RenderPresent(v->ren);
}

/* ---- ppm dump -------------------------------------------------------------------------- */

static void dump_ppm(const char *dir, unsigned long n, const unsigned char *f,
                     int w, int h)
{
    char path[1024];
    FILE *fp;
    int x, y;

    snprintf(path, sizeof path, "%s/frame%06lu.ppm", dir, n);
    fp = fopen(path, "wb");
    if (!fp) {
        return;
    }
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            unsigned v = f[(y * w + x) * 2] | f[(y * w + x) * 2 + 1] << 8;
            unsigned char px[3];

            /* rgb565le to rgb888 */
            px[0] = (unsigned char)(((v >> 11) & 0x1f) * 255 / 31);
            px[1] = (unsigned char)(((v >> 5) & 0x3f) * 255 / 63);
            px[2] = (unsigned char)((v & 0x1f) * 255 / 31);
            fwrite(px, 1, 3, fp);
        }
    }
    fclose(fp);
}

/* ---- input ------------------------------------------------------------------------------
 *
 * the window's mouse is the panel touch (scaled back to frame pixels); a few keys
 * send the icc/swc button bits rawlink has livi bindings for so the whole upstream
 * path can be exercised against --events. press on keydown, release on keyup.
 */

struct button_key {
    SDL_Keycode sym;
    unsigned char bit;
};

static const struct button_key button_keys[] = {
    { SDLK_BACKSPACE, 25 },
    { SDLK_h, 28 },
    { SDLK_b, 29 },
    { SDLK_n, 30 },
    { SDLK_v, 46 },
    { SDLK_m, 48 },
};

static unsigned button_bit(SDL_Keycode sym)
{
    size_t i;

    for (i = 0; i < sizeof button_keys / sizeof button_keys[0]; i++) {
        if (button_keys[i].sym == sym) {
            return button_keys[i].bit;
        }
    }
    return 0;
}

/* window pixels to frame pixels, for the touch coordinates the host injects */
static void win_to_frame(struct view *v, int wx, int wy, int *fx, int *fy)
{
    int ww, wh;

    SDL_GetWindowSize(v->win, &ww, &wh);
    if (ww <= 0 || wh <= 0) {
        *fx = *fy = 0;
        return;
    }
    *fx = wx * v->fw / ww;
    *fy = wy * v->fh / wh;
    if (*fx < 0) {
        *fx = 0;
    }
    if (*fy < 0) {
        *fy = 0;
    }
    if (*fx >= v->fw) {
        *fx = v->fw - 1;
    }
    if (*fy >= v->fh) {
        *fy = v->fh - 1;
    }
}

static void send_touch(struct xport *x, int tx, int ty, int down)
{
    unsigned char m[8];

    m[0] = 'L';
    m[1] = 'I';
    m[2] = LI_TOUCH;
    m[3] = (unsigned char)tx;
    m[4] = (unsigned char)(tx >> 8);
    m[5] = (unsigned char)ty;
    m[6] = (unsigned char)(ty >> 8);
    m[7] = (unsigned char)(down ? 1 : 0);
    xwrite_all(x, m, sizeof m);
}

static void send_button(struct xport *x, unsigned bit, int down)
{
    unsigned char m[5];

    m[0] = 'L';
    m[1] = 'I';
    m[2] = LI_BUTTON;
    m[3] = (unsigned char)bit;
    m[4] = (unsigned char)(down ? 1 : 0);
    xwrite_all(x, m, sizeof m);
}

static void send_knob(struct xport *x, int delta)
{
    unsigned char m[4];

    m[0] = 'L';
    m[1] = 'I';
    m[2] = LI_KNOB;
    m[3] = (unsigned char)delta;
    xwrite_all(x, m, sizeof m);
}

static void send_ready(struct xport *x)
{
    static const unsigned char m[3] = { 'L', 'I', LI_READY };

    xwrite_all(x, m, sizeof m);
}

static void send_ack(struct xport *x, unsigned seq, int displayed)
{
    unsigned char m[8];

    m[0] = 'L';
    m[1] = 'I';
    m[2] = LI_RAW;
    put32le(m + 3, seq);
    m[7] = (unsigned char)(displayed ? 1 : 2);
    xwrite_all(x, m, sizeof m);
}

/* pump SDL events; returns 0 to keep running, 1 to quit. small input messages go
 * out immediately so a touch does not wait behind a frame read. */
static int pump_input(struct view *v, struct xport *x, int no_input,
                      int *mouse_down)
{
    SDL_Event ev;

    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            return 1;
        }
        if (ev.type == SDL_KEYDOWN && !ev.key.repeat
            && (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_q)) {
            return 1;
        }
        if (no_input) {
            continue;
        }
        if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
            int fx, fy;

            *mouse_down = 1;
            win_to_frame(v, ev.button.x, ev.button.y, &fx, &fy);
            send_touch(x, fx, fy, 1);
        } else if (ev.type == SDL_MOUSEMOTION && *mouse_down) {
            int fx, fy;

            win_to_frame(v, ev.motion.x, ev.motion.y, &fx, &fy);
            send_touch(x, fx, fy, 1);
        } else if (ev.type == SDL_MOUSEBUTTONUP
                   && ev.button.button == SDL_BUTTON_LEFT && *mouse_down) {
            int fx, fy;

            *mouse_down = 0;
            win_to_frame(v, ev.button.x, ev.button.y, &fx, &fy);
            send_touch(x, fx, fy, 0);
        } else if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
            unsigned bit = button_bit(ev.key.keysym.sym);

            if (bit) {
                send_button(x, bit, 1);
            } else if (ev.key.keysym.sym == SDLK_LEFT) {
                send_knob(x, -1);
            } else if (ev.key.keysym.sym == SDLK_RIGHT) {
                send_knob(x, 1);
            }
        } else if (ev.type == SDL_KEYUP) {
            unsigned bit = button_bit(ev.key.keysym.sym);

            if (bit) {
                send_button(x, bit, 0);
            }
        }
    }
    return 0;
}

/* ---- the stream --------------------------------------------------------------------------
 *
 * the LR framing mirrors rawplay.c's read_header/read_mode: scan for the magic,
 * validate the header against the protocol (a raw picture has no start code, so a
 * false 'LR' inside pixels must fail the length check), then take the payload.
 * MODE is 16 payload bytes plus pad to 512; FRAME is the header pad (496) then
 * stride*height bytes, displayed as-is: no decode, no conversion, no scaling.
 */

struct run {
    struct cfg *cfg;
    struct xport *xport;
    struct view *view;
    unsigned char *frame;
    size_t frame_cap;
    int fw, fh, stride, expected;
    unsigned long frames, drawn, rx_kb;
    unsigned long long rx_us;
    long long t_start, last_rx, last_ready, last_stats, stop_at;
    const char *exit_reason;
    int mouse_down;
};

/* scan for one LR header. 1 = header in *type/len/seq, 0 = need more bytes (the
 * caller runs its watchdog and pumps input), -1 = the link is gone. */
static int read_header(struct run *r, unsigned *type, unsigned *len,
                       unsigned *seq)
{
    unsigned char c, prev = 0;
    unsigned char h[LR_HDR];
    int i;

    for (;;) {
        int f = xfill(r->xport, 50);

        if (g_stop) {
            return -1;
        }
        if (f < 0) {
            return -1;
        }
        if (f == 0) {
            return 0;
        }
        c = r->xport->rx[r->xport->rxpos++];
        if (c == LR_MAGIC1 && prev == LR_MAGIC0) {
            break;
        }
        prev = c;
    }
    h[0] = LR_MAGIC0;
    h[1] = LR_MAGIC1;
    for (i = 2; i < LR_HDR;) {
        int f = xfill(r->xport, 50);

        if (g_stop) {
            return -1;
        }
        if (f < 0) {
            return -1;
        }
        if (f == 0) {
            return 0;
        }
        h[i++] = r->xport->rx[r->xport->rxpos++];
    }
    *type = h[2];
    *len = get32le(h + 4);
    *seq = get32le(h + 8);
    if (h[3] != 0 || (*type == LR_MODE && *len != MODE_LEN)
        || (*type == LR_FRAME
            && (r->expected == 0 ? *len > MAX_FRAME
                                : *len != (unsigned)r->expected))
        || (*type != LR_MODE && *type != LR_FRAME)) {
        return 2;               /* not a header: keep scanning past it */
    }
    return 1;
}

/* skip the header-to-512 pad every LR message carries so the payload (or the next
 * message) starts on a packet boundary. */
static int skip_pad(struct run *r, size_t n)
{
    while (n > 0) {
        if (r->xport->rxpos < r->xport->rxlen) {
            size_t take = r->xport->rxlen - r->xport->rxpos;

            if (take > n) {
                take = n;
            }
            r->xport->rxpos += take;
            n -= take;
            continue;
        }
        {
            int f = xfill(r->xport, 500);

            if (f <= 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int read_mode(struct run *r)
{
    unsigned char p[MODE_LEN];
    unsigned w, h, stride, fmt;
    long long deadline = now_ms() + LINK_DEAD_MS;
    int rc = xread_exact(r->xport, p, sizeof p, deadline);

    if (rc != 1) {
        return -1;
    }
    if (skip_pad(r, LR_MSG - LR_HDR - MODE_LEN) < 0) {
        return -1;
    }
    w = get32le(p);
    h = get32le(p + 4);
    stride = get32le(p + 8);
    fmt = get32le(p + 12);
    if ((fmt & 0xff) != FMT_RGB565 || w == 0 || h == 0 || w > MAX_DIM
        || h > MAX_DIM || stride != w * 2
        || (unsigned long long)stride * h > MAX_FRAME) {
        fprintf(stderr, "rawplay-linux: host mode %ux%u stride %u fmt %u "
                "rejected\n", w, h, stride, fmt);
        return -1;
    }
    r->fw = (int)w;
    r->fh = (int)h;
    r->stride = (int)stride;
    r->expected = (int)(stride * h);
    if ((size_t)r->expected > r->frame_cap) {
        free(r->frame);
        r->frame = malloc((size_t)r->expected);
        r->frame_cap = r->frame ? (size_t)r->expected : 0;
        if (!r->frame) {
            fprintf(stderr, "rawplay-linux: out of memory for a %d byte "
                    "frame\n", r->expected);
            return -1;
        }
    }
    if (view_mode(r->view, r->fw, r->fh) < 0) {
        return -1;
    }
    fprintf(stderr, "rawplay-linux: host mode %ux%u rgb565, %d bytes/frame\n",
            w, h, r->expected);
    return 0;
}

static int read_frame(struct run *r, unsigned len, unsigned seq)
{
    long long t0 = now_ms();
    long long deadline = t0 + LINK_DEAD_MS;
    int rc;

    if (skip_pad(r, LR_MSG - LR_HDR) < 0) {
        return -1;
    }
    if ((int)len != r->expected) {
        fprintf(stderr, "rawplay-linux: frame %u bytes, expected %d\n", len,
                r->expected);
        return -1;
    }
    rc = xread_exact(r->xport, r->frame, len, deadline);
    if (rc != 1) {
        return -1;
    }
    r->rx_us += (unsigned long long)(now_ms() - t0) * 1000;
    r->rx_kb += len / 1024;
    r->last_rx = now_ms();
    r->frames++;
    view_show(r->view, r->frame, r->stride);
    r->drawn++;
    send_ack(r->xport, seq, 1);
    if (r->cfg->dump) {
        dump_ppm(r->cfg->dump, r->drawn, r->frame, r->fw, r->fh);
    }
    return 0;
}

static void stats_line(struct run *r, int final)
{
    double el = (now_ms() - r->t_start) / 1000.0;

    if (el <= 0.0) {
        el = 1e-9;
    }
    printf("rawplay-linux: %lu frames, %lu drawn, %lu kb in %.1f s, %.1f fps%s\n",
           r->frames, r->drawn, r->rx_kb, el, r->frames / el,
           final ? " (total)" : "");
    fflush(stdout);
}

/* one connected session: heartbeats, headers, modes and frames until the link goes
 * quiet, the peer leaves, --seconds expires or the window closes. 0 = ran cleanly
 * (another accept may follow in socket mode), 1 = quit requested. */
static int run_session(struct run *r)
{
    r->last_rx = now_ms();
    send_ready(r->xport);
    r->last_ready = now_ms();
    while (!g_stop) {
        unsigned type, len, seq;
        long long now = now_ms();
        int h;

        if (!r->view->headless
            && pump_input(r->view, r->xport, r->cfg->no_input,
                          &r->mouse_down)) {
            return 1;
        }
        if (r->cfg->seconds > 0.0 && now >= r->stop_at) {
            return 0;
        }
        if (now - r->last_ready >= READY_MS) {
            r->last_ready = now;
            send_ready(r->xport);
        }
        if (r->cfg->stats && now - r->last_stats >= 5000) {
            r->last_stats = now;
            stats_line(r, 0);
        }
        h = read_header(r, &type, &len, &seq);
        if (h < 0) {
            if (r->xport->dead) {
                r->exit_reason = "link read failed";
                return 1;
            }
            if (r->xport->conn_lost) {
                fprintf(stderr, "rawplay-linux: stream gone, waiting\n");
                return 0;
            }
            if (g_stop) {
                return 1;
            }
            r->exit_reason = "link read failed";
            return 1;
        }
        if (h == 0) {
            long long quiet = now - r->last_rx;
            long long limit = r->frames ? LINK_DEAD_MS : STARTUP_DEAD_MS;

            if (quiet > limit) {
                r->exit_reason = r->frames ? "stream stopped"
                                           : "no video from the host";
                fprintf(stderr, "rawplay-linux: %s\n", r->exit_reason);
                return 1;
            }
            continue;
        }
        if (h == 2) {
            continue;           /* a false 'LR' inside pixels: keep scanning */
        }
        if (type == LR_MODE) {
            if (read_mode(r) < 0) {
                r->exit_reason = "bad host mode";
                return 1;
            }
            r->last_rx = now_ms();
        } else {
            if (!r->expected) {
                /* a frame before any mode: the sender always opens with a mode,
                 * so this is a resync artifact; drop the payload, keep scanning */
                unsigned char tmp[1024];
                size_t left = len;

                if (skip_pad(r, LR_MSG - LR_HDR) < 0) {
                    r->exit_reason = "link read failed mid frame";
                    return 1;
                }
                while (left > 0) {
                    size_t want = left > sizeof tmp ? sizeof tmp : left;
                    int rc = xread_exact(r->xport, tmp, want,
                                         now_ms() + LINK_DEAD_MS);

                    if (rc != 1) {
                        r->exit_reason = "link read failed mid frame";
                        return 1;
                    }
                    left -= want;
                }
                send_ack(r->xport, seq, 0);
                continue;
            }
            if (read_frame(r, len, seq) < 0) {
                if (r->xport->conn_lost || r->xport->dead || g_stop) {
                    return r->xport->conn_lost ? 0 : 1;
                }
                r->exit_reason = "link read failed mid frame";
                return 1;
            }
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    struct cfg cfg;
    struct xport xport;
    struct view view;
    struct run run;
    struct sigaction sa;
    int failed = 0;

    if (parse_args(argc, argv, &cfg) < 0) {
        usage();
        return 2;
    }
    if (cfg.list_usb) {
        return list_usb(cfg.vid, cfg.pid);
    }
    signal(SIGPIPE, SIG_IGN);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    memset(&xport, 0, sizeof xport);
    xport.sock_listen = xport.sock_conn = -1;
    if (cfg.usb) {
        if (usb_open(&xport, cfg.vid, cfg.pid) < 0) {
            return 1;
        }
    } else if (sock_listen(&xport, cfg.sock) < 0) {
        return 1;
    }
    if (cfg.dump && mkdir(cfg.dump, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "rawplay-linux: mkdir %s: %s\n", cfg.dump,
                strerror(errno));
        return 1;
    }
    if (view_init(&view, cfg.init_w, cfg.init_h, cfg.scale, cfg.headless) < 0) {
        return 1;
    }
    memset(&run, 0, sizeof run);
    run.cfg = &cfg;
    run.xport = &xport;
    run.view = &view;
    run.fw = cfg.init_w;
    run.fh = cfg.init_h;
    run.stride = cfg.init_w * 2;
    run.t_start = now_ms();
    run.last_stats = run.t_start;
    run.stop_at = cfg.seconds > 0.0
                      ? run.t_start + (long long)(cfg.seconds * 1000.0) : 0;

    if (cfg.usb) {
        run_session(&run);
    } else {
        /* the sender connects, streams, and may come back; each session ends on
         * disconnect or timeout, and only a quit or --seconds expiry leaves. */
        while (!g_stop) {
            int a;

            if (cfg.seconds > 0.0 && now_ms() >= run.stop_at) {
                break;
            }
            a = sock_accept(&xport, run.stop_at, cfg.seconds > 0.0);
            if (a < 0) {
                failed = a == -1 ? 1 : 0;
                break;
            }
            {
                int q = run_session(&run);

                close(xport.sock_conn);
                xport.sock_conn = -1;
                if (q == 1) {
                    break;
                }
                if (cfg.seconds > 0.0 && now_ms() >= run.stop_at) {
                    break;
                }
            }
        }
        unlink(cfg.sock);
    }

    if (cfg.stats || run.frames) {
        stats_line(&run, 1);
    }
    if (run.exit_reason) {
        fprintf(stderr, "rawplay-linux: %s\n", run.exit_reason);
    }
    free(run.frame);
    if (view.tex) {
        SDL_DestroyTexture(view.tex);
    }
    if (view.ren) {
        SDL_DestroyRenderer(view.ren);
    }
    if (view.win) {
        SDL_DestroyWindow(view.win);
        SDL_Quit();
    }
    if (xport.dev) {
        libusb_release_interface(xport.dev, xport.iface);
        libusb_close(xport.dev);
    }
    if (xport.ctx) {
        libusb_exit(xport.ctx);
    }
    if (xport.sock_listen >= 0) {
        close(xport.sock_listen);
    }
    return failed;
}
