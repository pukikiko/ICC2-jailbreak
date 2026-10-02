/*
 * weston-touch - a weston module that gives the compositor a touchscreen and a way to
 * inject the unit's panel keys, so the raw video host can drive the app with real wayland
 * input and no X server anywhere.
 *
 * this module runs inside weston: it adds a touch device to the first seat (the headless
 * backend's fake seat, or whichever seat the backend creates) and listens on a unix socket
 * for one-line commands from rawplay/rawlink:
 *
 *     d X Y     touch down at panel pixels
 *     m X Y     touch move while down
 *     u         touch up
 *     c         touch cancel (the client went away mid touch)
 *     k CODE    tap the linux evdev key code CODE (press and release)
 *
 * a touch device created with no ops is exactly what weston's own test plugin does, so the
 * core routes the events the way it routes a real panel's; the key tap goes through
 * weston_keyboard_send_key, the same path a physical keyboard takes.
 *
 * loaded with weston --modules=/opt/livi/weston-touch.so, built by `make -C rawplay`
 * against weston-devel.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <libweston/libweston.h>

/* weston's input entry points are exported but live in internal headers the distro does
 * not install, so the few we need are declared here. a device with NULL ops has no
 * calibration, so the normalized point must stay NULL.
 *
 * weston 16 reworked the touch path: weston_touch_create_touch_device() takes a
 * set_output callback as a fifth argument, and notify_touch_normalized() takes a single
 * struct weston_touch_event (built by the public weston_touch_event_init()) instead of
 * loose arguments. building the old call against 16 aborts weston on the first touch, so
 * the build passes -DLIVI_WESTON_MAJOR=<pkg-config major> and both are supported. */
#ifndef LIVI_WESTON_MAJOR
#define LIVI_WESTON_MAJOR 14
#endif

struct weston_touch_device;
struct weston_touch_device_ops;

extern int weston_seat_init_touch(struct weston_seat *seat);
#if LIVI_WESTON_MAJOR >= 16
extern struct weston_touch_device *
weston_touch_create_touch_device(struct weston_touch *touch, const char *syspath,
                                 void *backend_data,
                                 const struct weston_touch_device_ops *ops,
                                 weston_touch_device_set_output_func_t set_output);
extern void
notify_touch_normalized(const struct weston_touch_event *event,
                        const struct weston_point2d_device_normalized *norm);
#else
extern struct weston_touch_device *
weston_touch_create_touch_device(struct weston_touch *touch, const char *syspath,
                                 void *backend_data,
                                 const struct weston_touch_device_ops *ops);
extern void
notify_touch_normalized(struct weston_touch_device *device, const struct timespec *time,
                        int touch_id, const struct weston_coord_global *pos,
                        const struct weston_point2d_device_normalized *norm,
                        int touch_type);
#endif
extern void notify_touch_frame(struct weston_touch_device *device);
extern void notify_touch_cancel(struct weston_touch_device *device);
extern void weston_touch_device_destroy(struct weston_touch_device *device);

#define DEFAULT_SOCK "/tmp/livi-touch.sock"
#define CMD_MAX      256

struct livi_touch {
    struct weston_compositor *compositor;
    struct weston_seat *seat;
    struct weston_touch_device *device;
    struct wl_listener seat_listener, destroy_listener;
    struct wl_event_source *listen_source, *client_source;
    int listen_fd, client_fd;
    int down;
    char buf[CMD_MAX];
    size_t len;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

static void touch_emit(struct livi_touch *t, int type, double x, double y)
{
    struct timespec ts;
    struct weston_coord_global pos = { .c = { .x = x, .y = y } };

    if (!t->device) {
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    switch (type) {
    case WL_TOUCH_DOWN:
    case WL_TOUCH_MOTION:
        t->down = 1;
#if LIVI_WESTON_MAJOR >= 16
        {
            struct weston_touch_event ev;

            weston_touch_event_init(&ev, &ts, t->seat, t->device, type, 0, &pos);
            notify_touch_normalized(&ev, NULL);
        }
#else
        notify_touch_normalized(t->device, &ts, 0, &pos, NULL, type);
#endif
        break;
    case WL_TOUCH_UP:
        t->down = 0;
#if LIVI_WESTON_MAJOR >= 16
        {
            struct weston_touch_event ev;

            weston_touch_event_init(&ev, &ts, t->seat, t->device, type, 0, NULL);
            notify_touch_normalized(&ev, NULL);
        }
#else
        notify_touch_normalized(t->device, &ts, 0, NULL, NULL, WL_TOUCH_UP);
#endif
        break;
    default:
        t->down = 0;
        notify_touch_cancel(t->device);
        return;
    }
    notify_touch_frame(t->device);
}

/* tap a linux evdev key code: press and release through the seat's keyboard, which the
 * focused client receives with the compositor's normal keymap. the panel buttons livi has
 * bindings for arrive here as codes from rawplay/rawlink.c; there is no XTest path. */
static void key_emit(struct livi_touch *t, uint32_t code)
{
    struct weston_keyboard *keyboard;
    struct timespec ts;

    if (!t->seat) {
        return;
    }
    keyboard = weston_seat_get_keyboard(t->seat);
    if (!keyboard) {
        weston_log("livi-touch: seat has no keyboard, key %u dropped\n", code);
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
#if LIVI_WESTON_MAJOR >= 16
    {
        struct weston_key_event ev;

        weston_key_event_init(&ev, &ts, t->seat, code,
                              WL_KEYBOARD_KEY_STATE_PRESSED, STATE_UPDATE_NONE);
        weston_keyboard_send_key(keyboard, &ev);
        weston_key_event_init(&ev, &ts, t->seat, code,
                              WL_KEYBOARD_KEY_STATE_RELEASED, STATE_UPDATE_NONE);
        weston_keyboard_send_key(keyboard, &ev);
    }
#else
    weston_keyboard_send_key(keyboard, &ts, code, WL_KEYBOARD_KEY_STATE_PRESSED);
    weston_keyboard_send_key(keyboard, &ts, code, WL_KEYBOARD_KEY_STATE_RELEASED);
#endif
}

static void handle_line(struct livi_touch *t, const char *line)
{
    double x, y;

    switch (line[0]) {
    case 'd':
        if (sscanf(line + 1, "%lf %lf", &x, &y) == 2)
            touch_emit(t, WL_TOUCH_DOWN, x, y);
        break;
    case 'm':
        if (sscanf(line + 1, "%lf %lf", &x, &y) == 2 && t->down)
            touch_emit(t, WL_TOUCH_MOTION, x, y);
        break;
    case 'u':
        if (t->down)
            touch_emit(t, WL_TOUCH_UP, 0, 0);
        break;
    case 'c':
        touch_emit(t, WL_TOUCH_CANCEL, 0, 0);
        break;
    case 'k':
        {
            unsigned code;

            if (sscanf(line + 1, "%u", &code) == 1)
                key_emit(t, code);
        }
        break;
    }
}

static void client_close(struct livi_touch *t)
{
    if (t->down) {
        touch_emit(t, WL_TOUCH_UP, 0, 0);
    }
    if (t->client_source) {
        wl_event_source_remove(t->client_source);
        t->client_source = NULL;
    }
    if (t->client_fd >= 0) {
        close(t->client_fd);
        t->client_fd = -1;
    }
    t->len = 0;
}

static int client_data(int fd, uint32_t mask, void *data)
{
    struct livi_touch *t = data;
    ssize_t n;

    if (!(mask & WL_EVENT_READABLE)) {
        return 0;
    }
    n = read(fd, t->buf + t->len, sizeof t->buf - t->len - 1);
    if (n <= 0) {
        client_close(t);
        return 0;
    }
    t->len += n;
    t->buf[t->len] = 0;
    for (;;) {
        char *nl = strchr(t->buf, '\n');
        size_t used;

        if (!nl) {
            break;
        }
        *nl = 0;
        handle_line(t, t->buf);
        used = nl + 1 - t->buf;
        memmove(t->buf, nl + 1, t->len - used);
        t->len -= used;
        t->buf[t->len] = 0;
    }
    if (t->len == sizeof t->buf - 1) {
        t->len = 0;         /* a line that never ended, drop it */
    }
    return 0;
}

static int listen_data(int fd, uint32_t mask, void *data)
{
    struct livi_touch *t = data;
    struct wl_event_loop *loop;
    int cfd;

    (void)mask;
    cfd = accept(fd, NULL, NULL);
    if (cfd < 0) {
        return 0;
    }
    if (t->client_fd >= 0) {
        /* one writer at a time: a restarted streamer replaces the old one */
        client_close(t);
    }
    t->client_fd = cfd;
    t->len = 0;
    loop = wl_display_get_event_loop(t->compositor->wl_display);
    t->client_source = wl_event_loop_add_fd(loop, cfd, WL_EVENT_READABLE, client_data, t);
    if (!t->client_source) {
        client_close(t);
    }
    return 0;
}

static void compositor_destroyed(struct wl_listener *listener, void *data)
{
    struct livi_touch *t = wl_container_of(listener, t, destroy_listener);

    (void)data;
    /* weston asserts the touch's device list is empty when it destroys the seat
     * (weston_touch_destroy) and this signal is emitted just before that, so a device
     * created here must be removed or every weston shutdown aborts */
    if (t->device) {
        weston_touch_device_destroy(t->device);
        t->device = NULL;
    }
    unlink(t->path);
}

static void seat_created(struct wl_listener *listener, void *data)
{
    struct livi_touch *t = wl_container_of(listener, t, seat_listener);

    if (t->seat) {
        return;
    }
    t->seat = data;
    if (weston_seat_init_touch(t->seat) < 0) {
        weston_log("livi-touch: cannot add touch to the seat\n");
        return;
    }
#if LIVI_WESTON_MAJOR >= 16
    t->device = weston_touch_create_touch_device(t->seat->touch_state, "livi-touch",
                                                 NULL, NULL, NULL);
#else
    t->device = weston_touch_create_touch_device(t->seat->touch_state, "livi-touch",
                                                 NULL, NULL);
#endif
    if (!t->device) {
        weston_log("livi-touch: cannot create the touch device\n");
    }
}

static int touch_socket(struct livi_touch *t)
{
    const char *path = getenv("LIVI_TOUCH_SOCK");
    struct sockaddr_un addr;
    struct wl_event_loop *loop;
    int fd;

    if (!path || !*path) {
        path = DEFAULT_SOCK;
    }
    if (strlen(path) >= sizeof addr.sun_path) {
        weston_log("livi-touch: socket path too long: %s\n", path);
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        weston_log("livi-touch: socket: %s\n", strerror(errno));
        return -1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);
    strcpy(t->path, path);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, 1) < 0) {
        weston_log("livi-touch: cannot bind %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    chmod(path, 0600);
    loop = wl_display_get_event_loop(t->compositor->wl_display);
    t->listen_source = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, listen_data, t);
    if (!t->listen_source) {
        weston_log("livi-touch: cannot add the socket to the event loop\n");
        close(fd);
        unlink(path);
        return -1;
    }
    t->listen_fd = fd;
    weston_log("livi-touch: touch device ready on %s\n", path);
    return 0;
}

WL_EXPORT int
wet_module_init(struct weston_compositor *compositor, int *argc, char *argv[])
{
    struct livi_touch *t;
    struct weston_seat *seat;

    (void)argc;
    (void)argv;
    t = calloc(1, sizeof *t);
    if (!t) {
        return -1;
    }
    t->compositor = compositor;
    t->listen_fd = t->client_fd = -1;
    t->seat_listener.notify = seat_created;
    t->destroy_listener.notify = compositor_destroyed;
    wl_list_init(&t->seat_listener.link);

    /* the headless backend's fake seat (and every other backend's) already exists by
     * module load time; a backend that creates it later is caught by the signal */
    wl_list_for_each(seat, &compositor->seat_list, link) {
        seat_created(&t->seat_listener, seat);
        break;
    }
    if (!t->seat) {
        wl_signal_add(&compositor->seat_created_signal, &t->seat_listener);
    }
    /* a module that cannot offer touch still leaves weston usable: the sender logs the
     * missing socket and touch is simply absent */
    if (touch_socket(t) < 0) {
        weston_log("livi-touch: touch injection disabled\n");
        return 0;
    }
    wl_signal_add(&compositor->destroy_signal, &t->destroy_listener);
    return 0;
}
