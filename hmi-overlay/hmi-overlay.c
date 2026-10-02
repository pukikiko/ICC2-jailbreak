/*
 * hmi-overlay - an LD_PRELOAD shim that lets the hmi's theme assets be overridden from a
 * writable overlay (a usb stick) without touching the unit's nand.
 *
 * the hmi opens /usr/hmi/<asset>; this interposes open/open64/fopen/fopen64/access and, when
 * the same relative path exists under an overlay root, opens that instead. the roots are
 * $HMI_OVERLAY if set, then /fs/usb0/hmi-overlay, then /tmp/hmi-overlay, so a stick with
 *
 *     hmi-overlay/HighSeries/Classic/Day/Backgrounds/Homescreen/nonNavigationVariant.png
 *
 * would change that one frame. missing files fall through to the unit's own copy.
 *
 *   export LD_PRELOAD=/fs/usb0/homebrew/hmi-overlay.so
 *   export HMI_OVERLAY=/fs/usb0/hmi-overlay
 *   hmi ...
 *
 * NOTE: at the time of writing the hmi opens and reads the redirected file (verified in the
 * emulator) but still paints the unit's own copy. treat this as experimental until that is
 * understood; the baked-in path qemu/mkhmiassets.py uses is the one that works today.
 *
 * every decision is appended to the jailbreak log on the stick (jlog.c), because on a real
 * unit there is no console to watch: which roots were found, which paths the hmi opened, and
 * whether each one was served from the overlay or from the unit. the first /usr/hmi open also
 * writes /tmp/hmi-overlay.loaded, the marker the launcher finds the hmi by.
 *
 * the real libc functions are found with dlsym: RTLD_NEXT first, then a handle on libc.
 */
#include "qnx.h"
#include "jlog.h"
#include <stdarg.h>

typedef int (*open_fn)(const char *, int, ...);
typedef FILE *(*fopen_fn)(const char *, const char *);
typedef int (*access_fn)(const char *, int);

void *dlopen(const char *path, int mode);
void *dlsym(void *handle, const char *name);
char *getenv(const char *name);
int access(const char *path, int mode);
int strncmp(const char *a, const char *b, size_t n);
pid_t getpid(void);

int open(const char *path, int flags, ...);
int open64(const char *path, int flags, ...);
FILE *fopen(const char *path, const char *mode);
FILE *fopen64(const char *path, const char *mode);

#define HMI_PREFIX      "/usr/hmi/"
#define PREFIX_LEN      9
#define OVERLAY_STICK   "/fs/usb0/hmi-overlay"
#define OVERLAY_RAM     "/tmp/hmi-overlay"
#define MARKER          "/tmp/hmi-overlay.loaded"
/* the buttons beep player carries the preload and opens this on every touch; it must not
 * be allowed to claim the marker (see marker()) */
#define AUDIO_PREFIX    "/usr/hmi/Audio/"
#define RTLD_NEXT       ((void *)-1)

static open_fn real_open;
static fopen_fn real_fopen;
static access_fn real_access;
static int resolved;

static void resolve(void)
{
    void *h;

    if (resolved) {
        return;
    }
    resolved = 1;
    real_open = (open_fn)dlsym(RTLD_NEXT, "open64");
    if (real_open == (open_fn)open64) {
        real_open = 0;
    }
    real_fopen = (fopen_fn)dlsym(RTLD_NEXT, "fopen");
    if (real_fopen == (fopen_fn)fopen) {
        real_fopen = 0;
    }
    real_access = (access_fn)dlsym(RTLD_NEXT, "access");
    if (real_access == (access_fn)access) {
        real_access = 0;
    }
    if (!real_open || !real_fopen || !real_access) {
        h = dlopen("libc.so.3", 0);
        if (h) {
            if (!real_open) real_open = (open_fn)dlsym(h, "open64");
            if (!real_fopen) real_fopen = (fopen_fn)dlsym(h, "fopen");
            if (!real_access) real_access = (access_fn)dlsym(h, "access");
        }
        if (!real_open) {
            printf("hmi-overlay: no real open64, overlay disabled\n");
        }
    }
}

static const char *roots[3];
static int announced;
static int marker_written;

static void roots_init(void)
{
    if (!roots[1]) {
        const char *env = getenv("HMI_OVERLAY");

        roots[0] = env && *env ? env : 0;
        roots[1] = OVERLAY_STICK;
        roots[2] = OVERLAY_RAM;
    }
}

static int is_hmi(const char *path)
{
    return path && strncmp(path, HMI_PREFIX, PREFIX_LEN) == 0;
}

/* the first overlay root that has the file wins: $HMI_OVERLAY, the stick, then a ram one
 * (ram is where the test bench drops files; the stick is what a real unit would use) */
static const char *overlay_path(const char *path)
{
    static char buf[512];

    if (!is_hmi(path)) {
        return path;
    }
    roots_init();
    for (unsigned i = 0; i < 3; i++) {
        if (!roots[i]) {
            continue;
        }
        snprintf(buf, sizeof buf, "%s/%s", roots[i], path + PREFIX_LEN);
        if (real_access && real_access(buf, 0) == 0) {
            return buf;
        }
    }
    return path;
}

/* the first interposed call says the shim is in the hmi at all: on a unit with no console this
 * line is the only proof. it logs the roots and whether the real libc calls were found, so a
 * real car that behaves differently from the emulator can be read off the stick. the stock
 * touch connector and buttons run with the same preload, so this fires in those too; that is
 * why the marker (which has to name the hmi) is left to the first /usr/hmi path below. */
static void announce(const char *path)
{
    if (announced) {
        return;
    }
    announced = 1;
    roots_init();
    jlog("hmi-overlay: active in pid %d, first call %s", getpid(), path ? path : "(null)");
    jlog("hmi-overlay: roots %s %s %s, real open64=%s fopen=%s access=%s",
         roots[0] ? roots[0] : "-", roots[1], roots[2],
         real_open ? "yes" : "no", real_fopen ? "yes" : "no", real_access ? "yes" : "no");
}

/* the marker boot.sh and the launcher find the hmi by. on a real unit the buttons beep player
 * also carries the preload and opens /usr/hmi/Audio/touchScreenBeep.wav on a touch, so "first
 * process to open a /usr/hmi path" is not the hmi: it used to overwrite the marker with a
 * short lived beep pid and the launcher then SIGSTOPped that dead pid instead of the hmi (the
 * "no usable pid marker ... slaying hmi with ..." lines in the jailbreak log). audio paths are
 * ignored, and an owner that is still alive is never taken over, so when the hmi restarts the
 * old pid is dead and its first theme asset claims the marker back. */
static int marker_pid(void)
{
    char line[32];
    FILE *f = fopen(MARKER, "r");
    int pid = 0;

    if (f) {
        if (fgets(line, sizeof line, f)) {
            pid = (int)strtol(line, 0, 10);
        }
        fclose(f);
    }
    return pid;
}

static void marker(const char *path)
{
    FILE *fp;
    int old;

    if (marker_written || !path || strncmp(path, AUDIO_PREFIX, sizeof AUDIO_PREFIX - 1) == 0) {
        return;
    }
    old = marker_pid();
    if (old > 0 && old != getpid() && kill(old, 0) == 0) {
        return;     /* a live owner, normally the hmi an earlier open already marked */
    }
    marker_written = 1;
    fp = fopen(MARKER, "w");
    if (fp) {
        fprintf(fp, "%d\n", getpid());
        fclose(fp);
    }
    jlog("hmi-overlay: marker %s %s for pid %d", MARKER,
         fp ? "written" : "not written", getpid());
}

static int do_open(const char *path, int flags, int mode)
{
    const char *p;
    int fd;

    resolve();
    announce(path);
    if (!real_open) {
        return -1;
    }
    p = overlay_path(path);
    if (is_hmi(path)) {
        marker(path);
    }
    if (p != path) {
        fd = real_open(p, flags, mode);
        jlog("hmi-overlay: open %s -> overlay %s: %s", path, p,
             fd >= 0 ? "hit" : "failed, using the unit copy");
        if (fd >= 0) {
            return fd;
        }
    } else if (is_hmi(path)) {
        jlog("hmi-overlay: open %s: no overlay copy", path);
    }
    return real_open(path, flags, mode);
}

int open(const char *path, int flags, ...)
{
    va_list ap;
    int mode;

    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
    return do_open(path, flags, mode);
}

int open64(const char *path, int flags, ...)
{
    va_list ap;
    int mode;

    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
    return do_open(path, flags, mode);
}

static FILE *do_fopen(const char *path, const char *mode)
{
    const char *p;

    resolve();
    announce(path);
    if (!real_fopen) {
        return 0;
    }
    p = overlay_path(path);
    if (is_hmi(path)) {
        marker(path);
    }
    if (p != path) {
        FILE *f = real_fopen(p, mode);

        jlog("hmi-overlay: fopen %s -> overlay %s: %s", path, p,
             f ? "hit" : "failed, using the unit copy");
        if (f) {
            return f;
        }
    } else if (is_hmi(path)) {
        jlog("hmi-overlay: fopen %s: no overlay copy", path);
    }
    return real_fopen(path, mode);
}

FILE *fopen(const char *path, const char *mode)
{
    return do_fopen(path, mode);
}

FILE *fopen64(const char *path, const char *mode)
{
    return do_fopen(path, mode);
}

int access(const char *path, int mode)
{
    const char *p;
    int hit;

    resolve();
    announce(path);
    if (!real_access) {
        return -1;
    }
    p = overlay_path(path);
    hit = p != path && real_access(p, mode) == 0;
    if (is_hmi(path)) {
        marker(path);
        jlog("hmi-overlay: access %s: %s", path,
             hit ? "overlay" : p != path ? "overlay missing" : "no overlay copy");
    }
    if (hit) {
        return 0;
    }
    return real_access(path, mode);
}
