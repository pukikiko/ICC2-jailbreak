/*
 * jlog - the shared jailbreak log. see jlog.h.
 *
 * the fd is opened once and kept, so a line costs one write. no fsync: devb-umass writes its
 * cache back on its own and a sync per line would slow the hmi down while it loads its assets
 * (the shim logs every asset open). the lines are short, so appends from the several processes
 * that share the file do not interleave inside a line.
 *
 * open() is used rather than fopen() because the shim carries its own open() and logs from
 * inside it; the log path is never /usr/hmi/..., so the shim passes it straight through to
 * libc and this cannot recurse. if open() itself cannot be resolved (the shim on a unit whose
 * libc is not what dlsym expects), fopen() is tried before giving up.
 *
 * the log is capped. the appliance serves the stick read-only, so every line falls back to
 * /tmp (a ram disk) and on a car that runs for days a per-asset log can fill it; an
 * unbounded append-only file is not something a head unit can leave running. when a file
 * reaches its cap it is truncated in place and a marker line is written, so the newest
 * lines always survive and a runaway writer cannot take /tmp with it.
 */
#include "qnx.h"
#include "jlog.h"

#define JLOG_STICK "/fs/usb0/homebrew/jailbreak.log"
#define JLOG_RAM   "/tmp/jailbreak.log"
/* stickwatch creates this on the first failed probe. a missing/dead mass-storage mount can
 * sit in a devb-umass timeout for tens of seconds, so while the marker is there jlog goes
 * straight to /tmp instead of touching the stick path on every retry; the marker
 * disappearing means the mount is back and the next retry can move the log there. */
#define JLOG_DOWN  "/tmp/stick-down"
#define JLOG_MAX   600
/* while the stick is away every write goes to /tmp; try it again this often (lines) so the
 * log moves back when the stick is remounted */
#define JLOG_RETRY 32
/* keep the appliance's read-only-stick fallback bounded: 512 KiB is days of startup lines,
 * and the stick copy gets more room because pulling it is how a unit is debugged */
#define JLOG_STICK_CAP (2 * 1024 * 1024)
#define JLOG_RAM_CAP   (512 * 1024)
#define JLOG_CAPPED    "[jlog: log capped, oldest lines dropped]\n"

/* qnx.h is a hand-written subset; these two are only needed for the size cap and both are
 * in the stub. identical prototypes are harmless if qnx.h grows them later. */
int fseek(FILE *f, long offset, int whence);
long ftell(FILE *f);
#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif

static int log_fd = -1;
static int log_on_stick;
static int log_retry;
static const char *log_path;
static long log_cap;
static long log_size;

static int log_open(const char *path, long cap, int trunc)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | (trunc ? O_TRUNC : 0), 0666);
    FILE *f;

    if (fd < 0) {
        return -1;
    }
    log_path = path;
    log_cap = cap;
    log_size = 0;
    if (!trunc && (f = fopen(path, "r")) != 0) {
        if (fseek(f, 0, SEEK_END) == 0) {
            long n = ftell(f);

            if (n > 0) {
                log_size = n;
            }
        }
        fclose(f);
    }
    return fd;
}

/* zero the file and leave a line saying why. the fd was opened by this process; other
 * writers (ham, another shim process) hold their own O_APPEND fd and keep appending at the
 * new end, so truncating in place is safe and bounds every writer at once. */
static int log_rotate(void)
{
    int fd = log_open(log_path, log_cap, 1);

    if (fd < 0) {
        return -1;
    }
    if (write(fd, JLOG_CAPPED, sizeof JLOG_CAPPED - 1) == sizeof JLOG_CAPPED - 1) {
        log_size = sizeof JLOG_CAPPED - 1;
    }
    return fd;
}

/* the first line after a boot lands on the stick if it is there, else in /tmp. when the stick
 * goes away under the fd (a car wake can unmount it, see docs/homebrew.md) write() fails:
 * fall back to /tmp and keep retrying the stick, so a wake's lines are not lost to a dead fd
 * and the log returns to the stick once stickwatch has remounted it. */
static void log_write(const char *line, int n)
{
    if (log_fd < 0) {
        if (access(JLOG_DOWN, 0) != 0) {
            log_fd = log_open(JLOG_STICK, JLOG_STICK_CAP, 0);
        }
        log_on_stick = log_fd >= 0;
        if (log_fd < 0) {
            log_fd = log_open(JLOG_RAM, JLOG_RAM_CAP, 0);
        }
    } else if (!log_on_stick && ++log_retry >= JLOG_RETRY) {
        int fd;

        log_retry = 0;
        if (access(JLOG_DOWN, 0) == 0) {
            /* the watchdog already knows the mount is gone; do not walk into its timeout */
        } else if ((fd = log_open(JLOG_STICK, JLOG_STICK_CAP, 0)) >= 0) {
            close(log_fd);
            log_fd = fd;
            log_on_stick = 1;
        }
    }
    if (log_fd >= 0) {
        if (log_size + n > log_cap) {
            int fd = log_rotate();

            close(log_fd);
            log_fd = fd;
        }
        if (log_fd >= 0 && write(log_fd, line, n) == n) {
            log_size += n;
            return;
        }
        if (log_fd >= 0) {
            close(log_fd);
        }
        log_fd = log_open(JLOG_RAM, JLOG_RAM_CAP, 0);
        log_on_stick = 0;
        if (log_fd >= 0) {
            if (log_size + n > log_cap) {
                int fd = log_rotate();

                close(log_fd);
                log_fd = fd;
            }
            if (log_fd >= 0 && write(log_fd, line, n) == n) {
                log_size += n;
            }
            return;
        }
    }
    /* the shim's open() may have failed while fopen() still works */
    {
        FILE *f = fopen(JLOG_STICK, "a");

        if (!f) {
            f = fopen(JLOG_RAM, "a");
        }
        if (f) {
            fprintf(f, "%s", line);
            fclose(f);
        }
    }
}

void jlog(const char *fmt, ...)
{
    char msg[512];
    char line[JLOG_MAX];
    struct timespec ts;
    va_list ap;
    int n;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    /* uptime, not wall clock: the unit's clock is not set yet on the way up, and this is
     * what puts the hmi's asset loads and the scripts on one timeline */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    n = snprintf(line, sizeof line, "[%u.%03u] %s\n", ts.tv_sec,
                 (unsigned)(ts.tv_nsec / 1000000), msg);
    if (n <= 0) {
        return;
    }
    if (n > (int)sizeof line - 1) {
        n = sizeof line - 1;
    }
    log_write(line, n);
}
