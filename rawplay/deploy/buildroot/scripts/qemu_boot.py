#!/usr/bin/env python3
# Boot the qemu appliance image, drive its serial console, collect the boot
# milestone table and (with --checks) enforce the smoke criteria.
#
# This is the shared engine behind scripts/boottime.sh (table only) and
# tests/smoke.sh (table plus checks, non-zero on any missed milestone). The
# guest's /proc/uptime and the kernel's printk timestamps are the same clock,
# so T3..T6 read from /run/livi-boot.log map onto host wall time with one
# offset measured from the first kernel-timestamped serial line. T0 is qemu's
# process start, so the host column is directly comparable to a stopwatch.
import argparse
import os
import re
import select
import subprocess
import sys
import time
import tty

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                       # rawplay/deploy/buildroot
DEFAULT_IMAGES = os.path.join(ROOT, "build", "qemu", "images")

KERNEL_LINE = re.compile(rb"^\[\s*(\d+\.\d+)\]\s")
# a console line can carry the shell's "# " prompt in front of a kernel line
# when output races the typed check command; ui_event finds the timestamp
# anywhere in such a line
KERNEL_ANY = re.compile(rb"\[\s*(\d+\.\d+)\]\s")
INIT_LINE = re.compile(rb"Run /sbin/init as init process")

# the sentinel must not appear in the typed command, because the guest tty
# echoes the command back and read_until() would match the echo first: the
# literal text has POLL and END apart, only the output has LIVI_POLL_END.
POLL_CMD = "cat /run/livi-boot.log 2>/dev/null; printf 'LIVI_%s\\n' POLL_END"
CHECK_CMD = r"""
export SYSTEMD_PAGER=cat PAGER=cat
echo LIVI_BEGIN
echo --log--
cat /run/livi-boot.log 2>/dev/null
echo --unitsim--
grep -E 'mounted read-only|mass-storage|enumerated' /tmp/unit-sim.log 2>/dev/null | tail -n 5
echo --active--
for u in livi-persist livi-pulse livi-weston livi rawlink livi-markers unit-sim; do printf '%s=%s\n' "$u" "$(systemctl is-active $u)"; done
echo --helper--
ps 2>/dev/null | grep -v grep | grep livi-helper
echo --udc--
cat /sys/class/udc/*/state 2>/dev/null
echo --state--
systemctl is-system-running 2>&1
echo --jobs--
systemctl --no-pager list-jobs 2>&1
echo --analyze--
systemd-analyze --no-pager 2>&1
echo --blame--
systemd-analyze --no-pager blame 2>&1 | head -n 25
echo --critical--
systemd-analyze --no-pager critical-chain 2>&1
echo --initcalls--
dmesg 2>/dev/null | grep -E 'initcall .* returned' | head -n 20
printf 'LIVI_%s\n' CHECK_END
"""


class Serial:
    """A pty the guest serial console talks through, with a rolling log and
    the host <-> guest clock anchor from the first kernel timestamp."""

    def __init__(self, log_path):
        self.master, self._slave = os.openpty()
        tty.setraw(self.master)
        tty.setraw(self._slave)
        self.slave_name = os.ttyname(self._slave)
        self.log_path = log_path
        self.log = open(log_path, "wb")
        self.buf = bytearray()
        self.host_offset = None
        self.first_host = None
        self.first_kernel = None

    def write_proc(self, text):
        os.write(self.master, (text + "\n").encode())

    def _record(self, data):
        self.log.write(data)
        self.log.flush()
        self.buf += data
        if self.host_offset is None and data:
            for line in bytes(data).replace(b"\r", b"").split(b"\n"):
                m = KERNEL_LINE.match(line.strip())
                if m:
                    # host_monotonic == kernel_time + offset; T0 (qemu start)
                    # is host_monotonic when we launched, so report times
                    # relative to it. qemu's reset-to-kernel gap is inside the
                    # first ~100 ms and is included in the offset.
                    self.first_host = time.monotonic()
                    self.first_kernel = float(m.group(1))
                    break

    def read_available(self, timeout):
        r, _, _ = select.select([self.master], [], [], timeout)
        if not r:
            return b""
        try:
            data = os.read(self.master, 65536)
        except OSError:
            return b""
        self._record(data)
        return data

    def read_until(self, needle, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.read_available(0.5)
            idx = self.buf.find(needle)
            if idx >= 0:
                text = self.buf[:idx].decode("utf-8", "replace")
                del self.buf[:idx + len(needle)]
                return text
        return None

    def log_bytes(self):
        with open(self.log_path, "rb") as f:
            return f.read()

    def close(self):
        self.log.close()
        os.close(self.master)
        os.close(self._slave)


def run_qemu(images, serial):
    disk = os.path.join(images, "disk.img")
    if not os.path.exists(disk):
        sys.exit("qemu-boot: no %s; run scripts/build.sh qemu first" % disk)
    cmd = [
        "qemu-system-aarch64",
        "-M", "virt",
        "-cpu", "max",
        "-smp", "4",
        "-m", "2048",
        "-kernel", os.path.join(images, "Image"),
        "-append",
        "root=/dev/vda1 ro rootwait rootfstype=squashfs console=ttyAMA0,115200 "
        "systemd.journald.forward_to_console=1 random.trust_cpu=on printk.time=1 "
        # the image ships its own no-device-unit getty for ttyAMA0; the stock
        # generator's serial-getty would wait on dev-ttyAMA0.device forever
        # with the udev coldplug masked (see board/common/post-build.sh)
        "systemd.getty_auto=0 "
        # qemu only, and ignored by LIVI-Lite: under TCG Chromium's llvmpipe GL
        # init times out and crash-loops the GPU process, so the old Electron
        # image gets --disable-gpu. The Rust stack does not read
        # LIVI_INNER_ARGS, so this is inert on the modified image and keeps the
        # unmodified image on its previously validated path.
        "systemd.setenv=LIVI_INNER_ARGS=--disable-gpu "
        # same escape hatch run-qemu.sh has, for measuring one image with
        # different kernel options (e.g. systemd.setenv=)
        + os.environ.get("LIVI_QEMU_APPEND", ""),
        "-drive", "if=none,id=disk,format=raw,file=%s" % disk,
        "-device", "virtio-blk-device,drive=disk",
        "-device", "virtio-rng-pci",
        "-display", "none",
        "-monitor", "none",
        "-no-reboot",
        "-serial", serial.slave_name,
    ]
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def parse_milestones(text):
    out = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0] in ("T3", "T4", "T5", "T6"):
            try:
                out[parts[0]] = float(parts[1])
            except ValueError:
                pass
    return out


def kernel_events(log_bytes):
    t1 = t2 = None
    for raw in log_bytes.split(b"\n"):
        line = raw.strip()
        m = KERNEL_LINE.match(line)
        if not m:
            continue
        ts = float(m.group(1))
        if t1 is None:
            t1 = ts
        if t2 is None and INIT_LINE.search(line):
            t2 = ts
    return t1, t2


# The UI milestones the wire cannot see. The rawplay/unit-sim link is the
# T5/T6 definition, but it is a test instrument (and a virtual USB controller
# on qemu); when it is silent, the guest's own console still says when LIVI's
# UI was presented. Both stacks log exactly one such line, with the kernel
# timestamp prefix printk.time=1 gives every console line:
#   Electron:  livi-compositor ... [kiosk] enter: screen=800x480 ...
#   LIVI-Lite: [core] UI started
UI_MARKERS = (
    (b"[kiosk] enter:", "Electron kiosk window presented"),
    (b"[core] UI started", "LIVI-Lite Slint UI started"),
)


def ui_event(log_bytes):
    """(guest_s, what) of the first UI-presented marker, or (None, None)."""
    for raw in log_bytes.split(b"\n"):
        line = raw.strip()
        for needle, what in UI_MARKERS:
            if needle in line:
                m = KERNEL_ANY.search(line)
                if m:
                    return float(m.group(1)), what
    return None, None


def parse_initcalls(block):
    out = []
    for line in block.splitlines():
        m = re.match(r"initcall (\S+) returned \d+ after (\d+) usecs", line)
        if m:
            out.append((int(m.group(2)), m.group(1)))
    out.sort(reverse=True)
    return out[:20]


def split_sections(block):
    sections = {}
    cur = None
    for line in block.splitlines():
        if line.startswith("--") and line.endswith("--"):
            cur = line.strip("-")
            sections[cur] = []
        elif cur is not None:
            sections[cur].append(line)
    return sections


def evaluate(block, milestones, ui_present=False):
    sections = split_sections(block)
    # T5/T6 come off the rawplay/unit-sim wire. That wire is a test
    # instrument on a virtual USB controller and has been seen to corrupt the
    # bulk stream under certain qemu/TCG hosts; the guest's own UI marker is
    # then the ground truth for "LIVI is up".
    checks = {
        "T3 gadget enumerated": "T3" in milestones,
        "T4 weston socket": "T4" in milestones,
        "T5 first frame or UI presented": "T5" in milestones or ui_present,
        "T6 non-blank frame or UI presented": "T6" in milestones or ui_present,
    }
    active = {}
    for line in sections.get("active", []):
        if "=" in line:
            unit, _, state = line.partition("=")
            active[unit.strip()] = state.strip()
    for unit in ("rawlink", "livi-weston", "livi-pulse"):
        checks["unit %s active" % unit] = active.get(unit) == "active"
    # qemu only: without a GLES3-capable software EGL, LIVI's app exits and
    # restarts; "activating" is a restart in flight, not a dead stack
    checks["unit livi up"] = active.get("livi") in ("active", "activating")
    checks["livi-helper running"] = bool(sections.get("helper"))
    checks["udc configured"] = any("configured" in l for l in sections.get("udc", []))
    # a masked/stock getty waiting on a device unit used to hold multi-user
    # for 90 s while every other check passed; systemd must be "running".
    checks["systemd finished booting"] = any(
        l.strip() == "running" for l in sections.get("state", []))
    logs = " ".join(sections.get("log", [])) + " " + " ".join(sections.get("unitsim", []))
    checks["stick rawplay.sh present"] = "homebrew/apps/rawplay.sh" in logs
    checks["no missing milestones"] = "missing" not in logs
    return checks


def boot_and_collect(images, timeout, log_path):
    print("qemu-boot: images %s" % images)
    print("qemu-boot: console log %s" % log_path)
    serial = Serial(log_path)
    t0 = time.monotonic()
    proc = run_qemu(images, serial)
    result = {"checks": {}, "log": log_path}
    try:
        # 1. login. the qemu image keeps a serial getty on purpose; the release
        #    board image does not.
        idx = serial.read_until(b"login:", timeout)
        if idx is None:
            result["error"] = "no login prompt"
            return result
        time.sleep(0.5)
        serial.write_proc("root")
        # busybox ash's root prompt is "~ # "; matching "# " avoids boot-log
        # lines that merely contain a '#'
        if serial.read_until(b"# ", 30) is None:
            result["error"] = "no root shell"
            return result


        # 2. wait for T6 by polling the milestone file; Electron on emulated
        #    aarch64 dominates, so the budget is minutes, not seconds
        deadline = time.monotonic() + timeout
        milestones = {}
        while time.monotonic() < deadline:
            serial.write_proc(POLL_CMD)
            text = serial.read_until(b"LIVI_POLL_END", 30)
            if text is None:
                continue
            milestones = parse_milestones(text)
            if "T6" in milestones or "missing" in text:
                break
            # the UI marker in the console is as good an end-of-boot signal
            # as the wire's T6 when the unit-sim stream is not landing
            if ui_event(serial.log_bytes())[0] is not None:
                break
            time.sleep(5)
        result["milestones"] = milestones

        # 3. the full check block
        serial.write_proc(CHECK_CMD)
        block = serial.read_until(b"LIVI_CHECK_END", 180)
        if block is None:
            result["error"] = "check block timed out"
            return result
        result["block"] = block
        result["t1"], result["t2"] = kernel_events(serial.log_bytes())
        result["t_ui"], result["ui_what"] = ui_event(serial.log_bytes())
        result["checks"] = evaluate(block, milestones, result["t_ui"] is not None)
        result["initcalls"] = parse_initcalls(block)
        if serial.first_host is not None:
            # host-relative-to-T0 for every guest timestamp: the host time at
            # the first kernel line minus its kernel timestamp, minus T0
            result["host_offset"] = (serial.first_host - t0) - serial.first_kernel
        return result
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        serial.close()


def print_table(result):
    ms = result.get("milestones", {})
    offset = result.get("host_offset")
    rows = [("T0", "power on / qemu start", 0.0)]
    if result.get("t1") is not None:
        rows.append(("T1", "kernel entry", result["t1"]))
    if result.get("t2") is not None:
        rows.append(("T2", "rootfs mounted, init running", result["t2"]))
    for key, what in (("T3", "gadget bound to the UDC, rawlink running"),
                      ("T4", "weston wayland-livi socket exists"),
                      ("T5", "first FRAME sent"),
                      ("T6", "LIVI UI on the weston output")):
        if key in ms:
            rows.append((key, what, ms[key]))
    if result.get("t_ui") is not None:
        rows.append(("T6u", "LIVI UI presented (%s)" % result.get("ui_what", "guest log"),
                     result["t_ui"]))
    print()
    print("boottime (qemu, relative; not a hardware measurement):")
    print("  %-4s %-42s %8s %10s" % ("id", "milestone", "guest", "host"))
    for rid, what, t in rows:
        if rid == "T0":
            print("  %-4s %-42s %8s %10s" % (rid, what, "-", "0.000"))
            continue
        host = ("%.3f" % (t + offset)) if offset is not None else "n/a"
        print("  %-4s %-42s %8.3f %10s" % (rid, what, t, host))
    print()

    if result.get("initcalls"):
        print("slowest initcalls (initcall_debug; debug images only):")
        for us, name in result["initcalls"]:
            print("  %8d us  %s" % (us, name))
        print()

    if not result.get("block"):
        return
    sections = split_sections(result["block"])
    for key, title in (("state", "systemd state"),
                       ("jobs", "pending jobs (boot still finishing)"),
                       ("analyze", "systemd-analyze"),
                       ("critical", "systemd-analyze critical-chain"),
                       ("blame", "slowest userspace units (blame)")):
        if not sections.get(key):
            continue
        print("%s:" % title)
        for line in sections[key]:
            if line.strip():
                print("  %s" % line)
        print()


def parse_board_log(path):
    """Board mode: the debug image's serial log is all we have. T1/T2 come
    from kernel timestamps, T3/T4 from livi-marker lines; T5/T6 need the car
    (or a bench host) and are reported unknown when absent. There is no T0 in
    a captured log, so only guest times are shown."""
    with open(path, "rb") as f:
        data = f.read()
    t1, t2 = kernel_events(data)
    text = data.decode("utf-8", "replace")
    t_ui, ui_what = ui_event(data)
    result = {
        "t1": t1,
        "t2": t2,
        "milestones": parse_milestones(text),
        "t_ui": t_ui,
        "ui_what": ui_what,
        "initcalls": parse_initcalls(text),
        "block": text,
        "checks": {},
    }
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--images", default=DEFAULT_IMAGES)
    ap.add_argument("--timeout", type=float, default=1800)
    ap.add_argument("--log", default=os.path.join(ROOT, "build", "qemu-boot.log"))
    ap.add_argument("--checks", action="store_true")
    ap.add_argument("--parse-log", default=None,
                    help="parse a captured board serial log instead of booting")
    args = ap.parse_args()

    log_path = os.path.abspath(args.log)
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    if args.parse_log:
        result = parse_board_log(args.parse_log)
        print_table(result)
        return 0

    result = boot_and_collect(args.images, args.timeout, log_path)
    print_table(result)

    if result.get("error"):
        print("SMOKE FAIL: %s" % result["error"])
        print("log: %s" % log_path)
        return 1

    if args.checks:
        failed = [k for k, v in result["checks"].items() if not v]
        print()
        for k, v in result["checks"].items():
            print("  %-4s %s" % ("ok" if v else "FAIL", k))
        if failed:
            print("\nSMOKE FAIL: %s" % ", ".join(failed))
            print("last console lines:")
            try:
                with open(log_path, "rb") as f:
                    print(f.read()[-4000:].decode("utf-8", "replace"))
            except OSError:
                pass
            return 1
        print("\nSMOKE PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
