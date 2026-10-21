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
/* stickwatch creates this the moment a probe fails and removes it once the mount is back.
 * looking in a dead devb-umass mount can block for its 30 s timeout, which is what made the
 * hmi feel sluggish after the car woke without the stick; while the marker is there, skip
 * the stick root and serve the unit copy (or /tmp) immediately. */
#define STICK_DOWN      "/tmp/stick-down"
/* the buttons beep player carries the preload and opens this on every touch; it must not
 * be allowed to claim the marker (see marker()) */
#define AUDIO_PREFIX    "/usr/hmi/Audio/"
#define RTLD_NEXT       ((void *)-1)

static open_fn real_open;
static fopen_fn real_fopen;
static access_fn real_access;
static int resolved;
static volatile int resolving;

static void resolve(void)
{
    void *h;

    if (resolved) {
        return;
    }
    /* the hmi loads several assets from more than one thread. the old code set resolved
     * before filling the pointers, so a second thread could call a null real_open while the
     * first was still in dlsym and the overlay looked broken for that asset. serialise the
     * one-time lookup; every later call takes the fast path. */
    while (__sync_lock_test_and_set(&resolving, 1)) {
    }
    if (!resolved) {
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
        resolved = 1;
    }
    __sync_lock_release(&resolving);
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

/* qnx's /tmp is /dev/shmem and has no subdirectories, so the cache is a flat file per
 * asset: a hash of the path plus the original suffix, under /tmp. copy one overlay file
 * that was just found on the stick there, so a mount drop after it was loaded cannot take
 * it away again. the cache is written under a per-process temp name and renamed into
 * place, so a reader never sees a half file; the lock serialises copies inside one process
 * (the hmi opens assets from several threads). failure is not fatal: the caller keeps the
 * stick path. */
#define CACHE_PREFIX "/tmp/hmiovl"
static volatile int cache_lock;

static unsigned path_hash(const char *s)
{
    unsigned h = 2166136261u;

    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

static int cache_name(const char *rel, char *dst, size_t n)
{
    const char *dot = 0, *p;
    char ext[8] = "";
    int len;

    for (p = rel; *p; p++) {
        if (*p == '.') {
            dot = p;
        }
    }
    if (dot && strlen(dot) < sizeof ext) {
        snprintf(ext, sizeof ext, "%s", dot);
    }
    len = snprintf(dst, n, "%s-%08x%s", CACHE_PREFIX, path_hash(rel), ext);
    return (len < 0 || (size_t)len >= n) ? -1 : 0;
}

/* a crashed writer leaves the .lock behind; it carries its pid, so a later process can
 * tell a dead owner from a live one and take the lock over. */
static int stale_lock(const char *path)
{
    char line[24];
    int fd, n, pid = 0;

    fd = real_open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    n = (int)read(fd, line, sizeof line - 1);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    line[n] = 0;
    pid = (int)strtol(line, 0, 10);
    if (pid > 0 && kill(pid, 0) < 0) {
        unlink(path);
        return 1;
    }
    return 0;
}

static int cache_file(const char *src, const char *rel, char *dst, size_t n)
{
    char okpath[80], lockpath[80], pidbuf[16];
    unsigned char copybuf[4096];
    int in = -1, out = -1, lock = -1, ok = -1, plen;
    ssize_t got;

    if (!real_open || !real_access || cache_name(rel, dst, n) < 0) {
        return -1;
    }
    snprintf(okpath, sizeof okpath, "%s.ok", dst);
    snprintf(lockpath, sizeof lockpath, "%s.lock", dst);
    if (real_access(okpath, 0) == 0 && real_access(dst, 0) == 0) {
        return 0;
    }
    while (__sync_lock_test_and_set(&cache_lock, 1)) {
    }
    if (real_access(okpath, 0) == 0 && real_access(dst, 0) == 0) {
        __sync_lock_release(&cache_lock);
        return 0;
    }
    /* an exclusive lock file elects one writer. shmem has no directories, hard links or
     * rename (all return EXDEV), so the .ok sidecar is the publish step: a reader only
     * trusts the cached file after it exists complete. a stale lock from a crashed process
     * is left alone; that asset simply keeps coming from the stick. */
    lock = real_open(lockpath, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (lock < 0 && stale_lock(lockpath)) {
        lock = real_open(lockpath, O_WRONLY | O_CREAT | O_EXCL, 0644);
    }
    if (lock < 0) {
        __sync_lock_release(&cache_lock);
        return -1;
    }
    plen = snprintf(pidbuf, sizeof pidbuf, "%d\n", (int)getpid());
    if (plen > 0) {
        (void)!write(lock, pidbuf, (size_t)plen);
    }
    in = real_open(src, O_RDONLY);
    if (in >= 0) {
        out = real_open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (out >= 0) {
        ok = 0;
        while ((got = read(in, copybuf, sizeof copybuf)) > 0) {
            ssize_t off = 0;

            while (off < got) {
                ssize_t w = write(out, copybuf + off, (size_t)(got - off));

                if (w <= 0) {
                    ok = -1;
                    break;
                }
                off += w;
            }
            if (ok < 0) {
                break;
            }
        }
        if (got < 0) {
            ok = -1;
        }
        close(out);
        out = -1;
        if (ok == 0) {
            out = real_open(okpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (out >= 0) {
                close(out);
                out = -1;
            } else {
                ok = -1;
            }
        }
        if (ok < 0) {
            unlink(dst);
        }
    }
    if (out >= 0) {
        close(out);
    }
    if (in >= 0) {
        close(in);
    }
    close(lock);
    unlink(lockpath);
    __sync_lock_release(&cache_lock);
    return ok;
}

/* the first overlay root that has the file wins: $HMI_OVERLAY, the stick, then a ram one
 * (ram is where the test bench drops files; the stick is what a real unit would use).
 * the candidate is built in the caller's stack buffer, not a static one: the hmi opens
 * assets from several threads, and a shared buffer let one thread's path be overwritten
 * between the access() and the open() and read the wrong asset (or the unit copy). */
static const char *overlay_path(const char *path, char *buf, size_t n)
{
    if (!is_hmi(path)) {
        return path;
    }
    roots_init();
    for (unsigned i = 0; i < 3; i++) {
        if (!roots[i]) {
            continue;
        }
        if (i == 1 && real_access && real_access(STICK_DOWN, 0) == 0) {
            continue;
        }
        snprintf(buf, n, "%s/%s", roots[i], path + PREFIX_LEN);
        if (real_access && real_access(buf, 0) == 0) {
            if (i == 1) {
                char cached[512];

                if (cache_file(buf, path + PREFIX_LEN, cached, sizeof cached) == 0) {
                    snprintf(buf, n, "%s", cached);
                    return buf;
                }
            }
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
    char alt[512];
    const char *p;
    int fd;

    resolve();
    announce(path);
    if (!real_open) {
        return -1;
    }
    p = overlay_path(path, alt, sizeof alt);
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
    char alt[512];
    const char *p;

    resolve();
    announce(path);
    if (!real_fopen) {
        return 0;
    }
    p = overlay_path(path, alt, sizeof alt);
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
    char alt[512];
    const char *p;
    int hit;

    resolve();
    announce(path);
    if (!real_access) {
        return -1;
    }
    p = overlay_path(path, alt, sizeof alt);
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
