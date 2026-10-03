#!/bin/sh
# shared post-build steps for both boards, invoked by Buildroot with
# TARGET_DIR/HOST_DIR/BINARIES_DIR in the environment.
set -eu

# systemd wants a machine-id before PID1 reads it, and the rootfs is
# read-only, so PID1 cannot generate one. livi-persist replaces it with a
# per-unit id from /data when a data partition exists; this placeholder keeps
# PID1 and early services quiet on a partitionless boot. it is not secret: the
# image ships no ssh, no logins and no network services.
if [ ! -s "$TARGET_DIR/etc/machine-id" ]; then
    printf 'b5a3f1c2d4e67890b5a3f1c2d4e67890\n' > "$TARGET_DIR/etc/machine-id"
fi

# an appliance does not need a login prompt on the release image. the debug
# defconfig sets a serial getty explicitly and this does not touch it.
rm -f "$TARGET_DIR/etc/systemd/system/multi-user.target.wants/serial-getty@"*.service

# busybox installs a tiny /etc/init.d skeleton even under systemd; keep it
# out of the way so nothing thinks sysv init is in charge
rm -rf "$TARGET_DIR/etc/init.d"

# the persistent-state directory and the home the services use
mkdir -p "$TARGET_DIR/home/user" "$TARGET_DIR/data"
chmod 0755 "$TARGET_DIR/home/user"
