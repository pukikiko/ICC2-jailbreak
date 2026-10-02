/*
 * jlog - one append-only log for the whole jailbreak chain, on the usb stick.
 *
 * the hook, boot.sh, the hmi-overlay shim, the launcher and hmictl all write to
 * /fs/usb0/homebrew/jailbreak.log, so a stick pulled after a boot holds every step of what
 * ran and what failed, in order. when the stick is not there (or is mounted read-only, as the
 * emulator attaches it) lines go to /tmp/jailbreak.log instead. see docs/homebrew.md.
 */
#ifndef JLOG_H
#define JLOG_H

void jlog(const char *fmt, ...);

#endif
