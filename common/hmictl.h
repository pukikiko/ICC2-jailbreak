/*
 * stop/resume of the hmi and running a button command, done without a shell in the loop.
 *
 * the launcher and the menu used to wrap each command as "slay -s SIGSTOP hmi; <command>;
 * slay -s SIGCONT hmi" and run it with system(). the hmi was found left stopped that way:
 * the shell was hung in sigsuspend with the slay child a zombie, so the resume at the end
 * of the line never ran. slay exits at once, which is when pdksh's wait is at its most
 * fragile.
 *
 * instead the pid from the shim's marker is signalled directly, and commands without shell
 * syntax are spawned and waited on here, where waitpid is the kernel's and cannot miss it.
 * they are posix_spawn()ed rather than fork()ed: fork() is not implemented in a process with
 * threads (ENOSYS), and the menu that runs the apps has input threads. commands with shell
 * syntax still go through /bin/sh -c, the way they always did. while
 * the hmi is stopped /tmp/hmi-stopped is left for scripts that stop it themselves
 * (rawplay.sh), so they do not have to touch it again.
 */
#ifndef HMICTL_H
#define HMICTL_H

#define HMI_SIGSTOP 23
#define HMI_SIGCONT 25

/* the hmi's pid from /tmp/hmi-overlay.loaded, 0 when the shim never ran */
pid_t hmi_pid(void);
/* stop/resume the hmi, falling back to slay by name when the marker is not there */
void hmi_signal(int sig);
/* run a "name args" command directly, or through the shell when it needs one */
int run_command(const char *cmd);

/*
 * the buttons service turns the panel bitmap into hmi events. with the hmi SIGSTOPped for a
 * homebrew run every one of those writes blocks in the stopped hmi's queue and the backlog
 * replays as one burst when the hmi comes back (the volume jumps, menus open on their own).
 * a session that owns the hmi SIGSTOPs buttons for its duration, so no further events are
 * generated; at resume the frozen process is SIGKILLed and its guard (buttons registers a
 * CONDDEATH restart with ham, like the hmi does) starts a clean one. the kill is what drops
 * the frames queued in the ipc driver for the old connection, so the hmi only ever sees the
 * newest state. pause() returns 1 when it did the pause, so nested callers (the menu inside
 * the launcher) do not resume what the outer session still holds.
 */
int hmi_events_pause(void);
void hmi_events_resume(void);

/*
 * a short hmi window inside a session (rawplay's --hmi climate bar) resumes the hmi for a
 * couple of seconds while buttons is still SIGSTOPped, so the panel goes dead exactly when
 * the hmi is watchable. window_open() brings buttons back for the window the way the session
 * resume does: kill the frozen process and let ham start a clean one, because a SIGCONT
 * would also replay every panel event queued during the video as one burst. window_close()
 * SIGSTOPs the fresh process again before the hmi stops, so nothing it generates queues.
 * they only act on a live session pause marker; the marker is left alone, so the owning
 * session still kills the frozen process at its own resume. window_close() is the matching
 * call and only valid after window_open() said 1.
 */
int hmi_events_window_open(void);
void hmi_events_window_close(void);

/*
 * the car can take the panel back while a homebrew session has the hmi stopped. two moments
 * mean the stock hmi should be driving again, and a stopped hmi never sees either:
 *
 * - acm.tuner.mode goes to 0 (audio off) while vehicle.state.ignition is 1 (off): this is
 *   what makes the hmi put the unit to sleep.
 * - the panel's power button (buttons bitmap bit 0) goes down: it is the audio power key,
 *   so it ends the session whatever the ignition is doing.
 *
 * a session arms the guard while it runs. the watcher reads the car on ipc's monitor
 * channel, latches the trigger and kills the running command's process group; the caller's
 * normal resume then brings the hmi and buttons back, so the stock logic runs again.
 */
void hmi_sleep_guard_start(void);
/* pgrp: the command group to kill, or 0 for a session with no command to kill (the menu
 * sitting on its list). grace_ms: how long the watcher waits before the kill, so an outer
 * session can give an inner one (the menu and its app) time to close itself first.
 * arming clears the latched trigger. */
void hmi_sleep_guard_arm(pid_t pgrp, unsigned grace_ms);
void hmi_sleep_guard_disarm(void);
int hmi_sleep_guard_fired(void);

/* run_command with the guard armed around it: the command runs in its own process group so
 * the whole tree goes down, and HMI_GUARD_STOP comes back when the car closed it */
#define HMI_GUARD_STOP (-4096)
int run_command_guarded(const char *cmd, unsigned grace_ms);

#endif
