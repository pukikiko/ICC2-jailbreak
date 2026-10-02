#include "qnx.h"
#include "hmictl.h"
#include "jlog.h"

#define OVERLAY_MARK     "/tmp/hmi-overlay.loaded"
#define HMI_STOPPED_MARK "/tmp/hmi-stopped"
#define BUTTONS_PAUSED   "/tmp/buttons-paused"
#define CMD_MAX          2048
#define ARG_MAX          64

extern char **environ;

pid_t hmi_pid(void)
{
    char line[32];
    FILE *f = fopen(OVERLAY_MARK, "r");
    int pid = 0;

    if (f) {
        if (fgets(line, sizeof line, f)) {
            pid = (int)strtol(line, 0, 10);
        }
        fclose(f);
    }
    return pid;
}

/* the marker is missing or stale (an hmi without the shim, or one that restarted), so let
 * slay find it by name. slay is spawned rather than forked: the menu calls this too, and
 * fork() fails with ENOSYS in a process that has threads (its input readers). */
static void slay_by_name(const char *sig)
{
    char *argv[5];
    pid_t pid = 0;
    int status;

    argv[0] = "slay";
    argv[1] = "-s";
    argv[2] = (char *)sig;
    argv[3] = "hmi";
    argv[4] = 0;
    if (posix_spawn(&pid, "/proc/boot/slay", 0, 0, argv, environ) != 0) {
        return;
    }
    while (waitpid(pid, &status, 0) < 0) {
    }
}

/* ---------------------------------------------------------------- car guard */

/*
 * see hmictl.h. the watcher reads the ipc monitor channel, the same frames buswatch decodes
 * with bussignals.h: vehicle.state (ch 4, msg 0 v7, ignition is data byte 2), acm.tuner
 * (ch 2, msg 0x11 v4, mode is data byte 0) and the buttons bitmap (ch 6, type 0, power is
 * bit 0). only rx frames matter: buf[1] is set when the frame is the guest's own tx.
 */

#define IPC_MONITOR "/dev/ipc/0"
#define IG_OFF      1           /* vehicle.state ignition: 1 is off */
#define AUDIO_OFF   0           /* acm.tuner mode: 0 is audio off */
#define POWER_BIT   0           /* buttons bitmap bit 0, the power key */

static volatile int guard_active;       /* a homebrew session owns the panel */
static volatile int guard_fired;        /* the car asked for it back */
static volatile pid_t guard_pgrp;       /* command group to kill, 0 when none is running */
static volatile unsigned guard_grace;   /* ms to let an inner session close itself first */

static unsigned now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)ts.tv_sec * 1000u + (unsigned)(ts.tv_nsec / 1000000u);
}

/* ask the v850 for the current state the way vehicle_settings and acm do when they connect.
 * a watcher that starts after a long uptime has no ignition or mode baseline otherwise,
 * and the change that matters could not be told from the initial state. */
static void request_car_state(void)
{
    static const unsigned char acm_flow[] = { 0x0f, 0x03, 0x02 };
    static const unsigned char veh_flow[] = { 0x03, 0x02, 0x02 };
    int fd;

    fd = open("/dev/ipc/2", O_WRONLY);
    if (fd >= 0) {
        write(fd, acm_flow, sizeof acm_flow);
        close(fd);
    }
    fd = open("/dev/ipc/4", O_WRONLY);
    if (fd >= 0) {
        write(fd, veh_flow, sizeof veh_flow);
        close(fd);
    }
}

static void car_trigger(const char *why)
{
    pid_t pgrp;

    if (!guard_active) {
        return;
    }
    if (!guard_fired) {
        jlog("guard: %s while homebrew owns the panel, closing it", why);
    }
    guard_fired = 1;
    pgrp = guard_pgrp;
    if (pgrp > 0) {
        /* a guard-aware command (the homebrew menu) closes its own app and exits in
         * milliseconds. an outer session arms with a grace long enough for that to happen,
         * because killing the group from outside while its leader waits on the app stalls
         * the leader on qnx. the inner session arms with no grace. */
        if (guard_grace) {
            usleep(guard_grace * 1000);
        }
        if (!guard_active || guard_pgrp != pgrp) {
            return;                 /* the session ended while we waited */
        }
        killpg(pgrp, SIGKILL);
        kill(pgrp, SIGKILL);        /* in case the group never formed */
    }
}

static void *car_watch(void *arg)
{
    static unsigned char buf[1002];
    int fd = -1, len;
    int have_ign = 0, have_mode = 0, have_buttons = 0;
    int ign = 0, mode = 0, power = 0;
    unsigned asked = 0;

    (void)arg;
    for (;;) {
        ssize_t n;

        if (fd < 0) {
            fd = open(IPC_MONITOR, O_RDONLY);
            if (fd < 0) {
                usleep(500000);
                continue;
            }
            asked = 0;
        }
        if (!have_ign || !have_mode) {
            unsigned now = now_ms();

            if (!asked || now - asked > 2000) {
                request_car_state();
                asked = now;
            }
        }
        n = read(fd, buf, sizeof buf);
        if (n < 0) {
            close(fd);
            fd = -1;
            usleep(100000);
            continue;
        }
        if (n < 5) {
            usleep(10000);
            continue;
        }
        len = (int)n - 2;
        if (buf[1]) {                   /* to the v850, not car state */
            usleep(10000);
            continue;
        }
        if (buf[0] == 4 && len >= 7 && buf[2] == 0x00 && buf[3] == 7) {
            int v = buf[6];             /* vehicle.state data byte 2 is ignition */

            if (!have_ign) {
                ign = v;
                have_ign = 1;
            } else if (v != ign) {
                ign = v;
                if (v == IG_OFF && have_mode && mode == AUDIO_OFF) {
                    car_trigger("ignition off with audio off");
                }
            }
        } else if (buf[0] == 2 && len >= 6 && buf[2] == 0x11 && buf[3] == 4) {
            int v = buf[4];             /* acm.tuner data byte 0 is the media mode */

            if (!have_mode) {
                mode = v;
                have_mode = 1;
            } else if (v != mode) {
                mode = v;
                if (v == AUDIO_OFF && have_ign && ign == IG_OFF) {
                    car_trigger("audio off with the ignition off");
                }
            }
        } else if (buf[0] == 6 && len >= 3 && buf[2] == 1 && buf[3] == 0) {
            int v = buf[4] & (1 << POWER_BIT);

            if (!have_buttons) {
                have_buttons = 1;
                power = v;
                /* the v850 only sends the bitmap when a bit moves (or on a state reload), so
                 * the first one seen can already be the press itself. take a down bit as a
                 * press rather than as a baseline, or a session that starts with no button
                 * history would never see the power button at all. */
                if (v) {
                    car_trigger("the power button");
                }
            } else if (v != power) {
                power = v;
                if (v) {
                    car_trigger("the power button");
                }
            }
        }
        usleep(10000);
    }
    return arg;
}

void hmi_sleep_guard_start(void)
{
    static int started;
    unsigned thread;
    int err;

    if (started) {
        return;
    }
    err = pthread_create(&thread, 0, car_watch, 0);
    if (err != 0) {
        jlog("guard: could not start the car watcher (error %d)", err);
        return;
    }
    pthread_detach(thread);
    started = 1;
}

void hmi_sleep_guard_arm(pid_t pgrp, unsigned grace_ms)
{
    guard_fired = 0;
    guard_pgrp = pgrp;
    guard_grace = grace_ms;
    guard_active = 1;
}

void hmi_sleep_guard_disarm(void)
{
    guard_active = 0;
    guard_pgrp = 0;
}

int hmi_sleep_guard_fired(void)
{
    return guard_fired;
}

void hmi_signal(int sig)
{
    const char *name = sig == HMI_SIGSTOP ? "SIGSTOP" : "SIGCONT";
    pid_t pid = hmi_pid();

    if (pid <= 0 || kill(pid, sig) < 0) {
        jlog("hmi: no usable pid marker (%d, errno %d), slaying hmi with %s", pid, errno, name);
        slay_by_name(name);
    } else {
        jlog("hmi: %s to pid %d", name, pid);
    }
    /* commands started while the hmi is stopped can see this and not stop it again: a shell
     * waiting on slay is what used to leave it frozen (rawplay.sh reads it) */
    if (sig == HMI_SIGSTOP) {
        FILE *f = fopen(HMI_STOPPED_MARK, "w");

        if (f) {
            fclose(f);
        }
    } else {
        unlink(HMI_STOPPED_MARK);
    }
}

static int needs_shell(const char *cmd)
{
    static const char meta[] = ";|&<>()$`'\"*?[]{}~\\#";

    for (const char *p = cmd; *p; p++) {
        if (strchr(meta, *p)) {
            return 1;
        }
    }
    return 0;
}

/* split on whitespace, no quoting: anything with a quote is a shell command anyway */
static int split(char *buf, char **argv, int max)
{
    int n = 0;
    char *p = buf;

    while (*p && n < max - 1) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        argv[n++] = p;
        while (*p && *p != ' ' && *p != '\t') {
            p++;
        }
        if (*p) {
            *p++ = 0;
        }
    }
    argv[n] = 0;
    return n;
}

/* the pause marker's owner pid, and whether the file is there at all: an owner still around
 * means a live session pause, a missing or dead owner is a stale marker. rawplay.sh reads
 * and writes the same file. */
static int buttons_pause_owner(int *have)
{
    char line[32];
    FILE *f = fopen(BUTTONS_PAUSED, "r");
    int owner = 0;

    *have = f != 0;
    if (f) {
        if (fgets(line, sizeof line, f)) {
            owner = (int)strtol(line, 0, 10);
        }
        fclose(f);
    }
    return owner;
}

static int buttons_pause_live(void)
{
    int have;
    int owner = buttons_pause_owner(&have);

    return have && owner > 0 && kill(owner, 0) == 0;
}

int hmi_events_pause(void)
{
    FILE *f;
    int have;
    int owner = buttons_pause_owner(&have);

    if (have) {
        if (owner > 0 && kill(owner, 0) == 0) {
            return 0;   /* a live outer session (the launcher, a script) owns the pause */
        }
        /* the owner died without resuming: freeze buttons again for this session rather
         * than leave the panel deaf */
        unlink(BUTTONS_PAUSED);
        jlog("hmi: stale buttons pause (owner %d gone), taking it over", owner);
    }
    jlog("hmi: stopping buttons for the homebrew session");
    /* SIGSTOP, not a kill: buttons is guarded by ham (CONDDEATH + restart), so a death is
     * answered with an immediate respawn that would just start queuing again. the frozen
     * process cannot be signalled away, so the ipc frames for it pile up in the driver
     * until the resume kills it and lets ham start clean. */
    run_command("/proc/boot/slay -s SIGSTOP buttons");
    f = fopen(BUTTONS_PAUSED, "w");
    if (f) {
        fprintf(f, "%d\n", getpid());
        fclose(f);
    }
    return 1;
}

void hmi_events_resume(void)
{
    if (access(BUTTONS_PAUSED, 0) != 0) {
        return;         /* nothing paused, or something already put it back */
    }
    unlink(BUTTONS_PAUSED);
    jlog("hmi: dropping the frozen buttons service, ham restarts a clean one");
    /* SIGKILL: a SIGTERM would let buttons detach itself from ham (it imports
     * ham_detach_self) and leave nothing to restart. the death raises its CONDDEATH and
     * ham spawns a fresh instance; that start is also what drops every panel frame queued
     * for the old, frozen connection, so the resumed hmi sees the current state only. */
    run_command("/proc/boot/slay -f -s9 buttons");
}

int hmi_events_window_open(void)
{
    if (!buttons_pause_live()) {
        return 0;   /* no session pause to borrow buttons from, it is not ours to stop */
    }
    jlog("hmi: hmi window, buttons restarted for the hmi");
    /* the same kill the session resume does, in place of a SIGCONT: the frozen process's
     * pending frames (every panel event of the video session) die with its connection and
     * ham starts a clean one, so the resumed hmi gets live events, not the backlog replayed
     * as one burst. */
    run_command("/proc/boot/slay -f -s9 buttons");
    return 1;
}

void hmi_events_window_close(void)
{
    jlog("hmi: hmi window over, buttons stopped again");
    run_command("/proc/boot/slay -s SIGSTOP buttons");
}

/* split the command line and spawn it; returns the pid, or -1 when it could not start */
static pid_t spawn_command(const char *cmd, int own_group)
{
    char buf[CMD_MAX];
    char *argv[ARG_MAX];
    char *sargv[4];
    char **exec_argv = argv;
    const char *path;
    int shell = needs_shell(cmd) || strlen(cmd) >= sizeof buf;
    pid_t pid = 0;
    int err;

    if (!shell) {
        snprintf(buf, sizeof buf, "%s", cmd);
        if (split(buf, argv, ARG_MAX) == 0) {
            shell = 1;
        }
    }
    if (shell) {
        sargv[0] = "sh";
        sargv[1] = "-c";
        sargv[2] = (char *)cmd;
        sargv[3] = 0;
        path = "/bin/sh";
        exec_argv = sargv;
    } else {
        path = argv[0];
    }
    /* spawn, not fork+exec: the menu that runs the apps has input threads, and fork() is
     * not implemented for a multithreaded process (ENOSYS), so every launch failed there.
     * posix_spawn still hands us the pid, and waitpid below reaps it as before. */
    if (!own_group) {
        err = posix_spawn(&pid, path, 0, 0, exec_argv, environ);
    } else {
        /* the guard kills a whole command tree, so the child has to lead its own process
         * group from the start. setpgid() after the spawn loses the race with the child's
         * exec, and the attr's layout is not in the hand written headers, so it gets a
         * generously sized aligned buffer that the libc's own init/setpgroup fill in. */
        union {
            long long align;
            unsigned char raw[512];
        } attr;

        memset(attr.raw, 0, sizeof attr.raw);
        err = posix_spawnattr_init(attr.raw);
        if (err == 0) {
            err = posix_spawnattr_setpgroup(attr.raw, 0);
        }
        if (err == 0) {
            err = posix_spawnattr_setflags(attr.raw, POSIX_SPAWN_SETPGROUP);
        }
        if (err == 0) {
            err = posix_spawn(&pid, path, 0, attr.raw, exec_argv, environ);
            posix_spawnattr_destroy(attr.raw);
        } else {
            jlog("guard: no spawnattr process group (error %d), trying setpgid after spawn",
                 err);
            err = posix_spawn(&pid, path, 0, 0, exec_argv, environ);
            if (err == 0 && setpgid(pid, pid) != 0) {
                jlog("guard: setpgid(%d) failed (errno %d)", pid, errno);
            }
        }
    }
    if (err != 0) {
        jlog("hmictl: could not run '%s' (errno %d)", cmd, errno);
        return -1;
    }
    return pid;
}

int run_command(const char *cmd)
{
    pid_t pid = spawn_command(cmd, 0);
    int status = 0;

    if (pid < 0) {
        return -1;
    }
    while (waitpid(pid, &status, 0) < 0) {
    }
    return status;
}

int run_command_guarded(const char *cmd, unsigned grace_ms)
{
    pid_t pid = spawn_command(cmd, 1);
    int status = 0, fired;

    if (pid < 0) {
        return -1;
    }
    hmi_sleep_guard_start();
    hmi_sleep_guard_arm(pid, grace_ms);
    while (waitpid(pid, &status, 0) < 0) {
    }
    fired = hmi_sleep_guard_fired();
    hmi_sleep_guard_disarm();
    if (fired) {
        jlog("guard: closed '%s' for the car", cmd);
        return HMI_GUARD_STOP;
    }
    return status;
}
