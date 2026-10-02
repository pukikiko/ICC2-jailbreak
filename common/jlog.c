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
 */
#include "qnx.h"
#include "jlog.h"

#define JLOG_STICK "/fs/usb0/homebrew/jailbreak.log"
#define JLOG_RAM   "/tmp/jailbreak.log"
#define JLOG_MAX   600
/* while the stick is away every write goes to /tmp; try it again this often (lines) so the
 * log moves back when the stick is remounted */
#define JLOG_RETRY 32

static int log_fd = -1;
static int log_on_stick;
static int log_retry;

static int log_open(const char *path)
{
    return open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
}

/* the first line after a boot lands on the stick if it is there, else in /tmp. when the stick
 * goes away under the fd (a car wake can unmount it, see docs/homebrew.md) write() fails:
 * fall back to /tmp and keep retrying the stick, so a wake's lines are not lost to a dead fd
 * and the log returns to the stick once stickwatch has remounted it. */
static void log_write(const char *line, int n)
{
    if (log_fd < 0) {
        log_fd = log_open(JLOG_STICK);
        log_on_stick = log_fd >= 0;
        if (log_fd < 0) {
            log_fd = log_open(JLOG_RAM);
        }
    } else if (!log_on_stick && ++log_retry >= JLOG_RETRY) {
        int fd;

        log_retry = 0;
        fd = log_open(JLOG_STICK);
        if (fd >= 0) {
            close(log_fd);
            log_fd = fd;
            log_on_stick = 1;
        }
    }
    if (log_fd >= 0) {
        if (write(log_fd, line, n) == n) {
            return;
        }
        close(log_fd);
        log_fd = log_open(JLOG_RAM);
        log_on_stick = 0;
        if (log_fd >= 0) {
            write(log_fd, line, n);
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
