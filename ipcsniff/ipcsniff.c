/*
 * ipcsniff: print every frame that crosses uart3, both ways, without touching the hmi.
 *
 * /dev/ipc/0 is ipc's monitor channel. each read returns one frame as
 * [client, direction (1 = to the v850), payload...]. the monitor is a passive tap:
 * opening it does not stop the hmi, buttons or any other reader, so this can run for a
 * whole session from the stick and log what the car and the unit say to each other.
 *
 * usage: ipcsniff [count] [--log path]
 *
 *   count       stop after count frames (default: run until killed)
 *   --log path  write the frame lines to path instead of stdout; if the path fails,
 *               fall back to /tmp/ipc.log
 *
 * the line format is:
 *   [uptime.seconds] unit -> ch 02: 10 03 05 00 00 00 09
 *   [uptime.seconds] v850 -> ch 02: 11 04 05 ...
 *
 * the pid is written to /tmp/ipc-sniff.pid so ipcsniff.sh can stop it by pid.
 */
#include "qnx.h"

#define MAX_FRAME 1002
#define LINE_MAX  4096
#define LOG_MAX   (16u << 20)

static int open_log(const char *path)
{
    int fd = -1;

    if (path) {
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    }
    return fd;
}

int main(int argc, char **argv)
{
    static unsigned char frame[MAX_FRAME];
    char line[LINE_MAX];
    const char *logpath = 0;
    long left = -1;
    unsigned long total = 0;
    int fd = -1, out = 1;
    unsigned silent = 0;
    FILE *pidf;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--log") && i + 1 < argc) {
            logpath = argv[++i];
        } else {
            long v = strtol(argv[i], 0, 0);
            if (v > 0) {
                left = v;
            }
        }
    }

    if (logpath) {
        int logfd = open_log(logpath);
        if (logfd >= 0) {
            out = logfd;
        } else {
            out = open("/tmp/ipc.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (out < 0) {
                out = 1;
            }
        }
    }

    pidf = fopen("/tmp/ipc-sniff.pid", "w");
    if (pidf) {
        fprintf(pidf, "%d\n", (int)getpid());
        fclose(pidf);
    }

    for (;;) {
        ssize_t n;

        if (fd < 0) {
            fd = open("/dev/ipc/0", O_RDONLY);
            if (fd < 0) {
                usleep(250000);
                continue;
            }
            silent = 0;
        }
        n = read(fd, frame, sizeof frame);
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
            } else {
                usleep(2000);
            }
            continue;
        }
        silent = 0;
        {
            struct timespec ts;
            int p;

            clock_gettime(CLOCK_MONOTONIC, &ts);
            p = snprintf(line, sizeof line, "[%u.%03u] %s ch %02x:",
                         ts.tv_sec, (unsigned)(ts.tv_nsec / 1000000),
                         frame[1] ? "unit ->" : "v850 ->", frame[0]);
            for (ssize_t j = 2; j < n && p < (int)sizeof line - 4; j++) {
                p += snprintf(line + p, sizeof line - p, " %02x", frame[j]);
            }
            line[p++] = '\n';
            if (write(out, line, p) < 0 && out != 1 && logpath) {
                /* the stick went away (a car wake remounts it): keep the tail in ram */
                close(out);
                out = open("/tmp/ipc.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
                if (out < 0) {
                    out = 1;
                }
                write(out, line, p);
            }
            total += p;
            if (out != 1 && total >= LOG_MAX) {
                static const char capped[] = "# ipcsniff: 16 MiB log reached, stopping\n";
                write(out, capped, sizeof capped - 1);
                break;
            }
        }
        if (left > 0 && --left == 0) {
            break;
        }
    }
    if (fd >= 0) {
        close(fd);
    }
    if (out != 1) {
        close(out);
    }
    unlink("/tmp/ipc-sniff.pid");
    return 0;
}
