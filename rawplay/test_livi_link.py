#!/usr/bin/env python3
# state-machine and control-socket tests for deploy/livi-link-monitor + deploy/livi-cmd,
# no phone and no root needed:
#
#     make -C rawplay livi-cmd && python3 rawplay/test_livi_link.py
#
# what it proves:
#   * livi-cmd sends exactly "set-aa 1\n" to the helper socket and passes/fails on the
#     {"ok": true/false} reply and on a missing socket;
#   * with no charger the monitor is "unplugged": it rfkill-blocks the radios, unloads
#     the radio drivers and releases /dev/cpu_dma_latency (deep idle);
#   * with a charger and no player it is "idle": radios on (drivers reloaded), latency
#     released;
#   * livi is tied to the radios: unplugged stops livi.service, and charging starts it
#     again only after the driver reload and the rfkill unblock succeeded (a failed
#     reload leaves it stopped); a livi that appears behind the monitor while unplugged
#     (the multi-user boot job racing the first tick) is stopped on the next tick;
#   * a rawplay client ("reader ready", the ack-latency stats line, forwarded LI input)
#     makes it "connected" and holds /dev/cpu_dma_latency (visible in /proc/<pid>/fd);
#     unplugging the charger drops back to "unplugged" even while connected;
#   * it goes idle on rawlink's reader-silent watchdog ("no acks for 5 s"), on the usb
#     teardown lines and when rawlink (re)starts or crashes ("bound to"/systemd lines);
#   * it never talks to the helper control socket: wireless Android Auto is parked on in
#     LIVI's config and left alone, so no set-aa may be sent in any state;
#   * the state file tracks the state and the wake lock + sxmo flag are held throughout;
#   * it exits cleanly when the journal stream ends (stdin closed).
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MONITOR = os.path.join(HERE, 'deploy', 'livi-link-monitor')
LIVI_CMD = os.path.join(HERE, 'out', 'livi-cmd')
LIVI_CMD_SRC = os.path.join(HERE, 'deploy', 'livi-cmd.c')

TMP = tempfile.mkdtemp(prefix='livi-link-test-')
PASS, FAIL = [], []


def check(name, ok, detail=''):
    if ok:
        PASS.append(name)
        print(f'  PASS {name}')
    else:
        FAIL.append(name)
        print(f'  FAIL {name}: {detail}')


def build_livi_cmd():
    if not os.path.exists(LIVI_CMD) or \
            os.path.getmtime(LIVI_CMD) < os.path.getmtime(LIVI_CMD_SRC):
        os.makedirs(os.path.dirname(LIVI_CMD), exist_ok=True)
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-o', LIVI_CMD, LIVI_CMD_SRC],
                       check=True)


class FakeHelper(threading.Thread):
    """LIVI's helper control socket: one RPC per connection, {"ok": ...} reply."""

    def __init__(self, path, ok=True):
        super().__init__(daemon=True)
        self.path = path
        self.reply = '{"ok": true}\n' if ok else '{"ok": false, "error": "nope"}\n'
        self.lines = []
        self.lock = threading.Lock()
        self.stop_flag = threading.Event()
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(path)
        self.srv.listen(8)
        self.srv.settimeout(0.2)

    def run(self):
        while not self.stop_flag.is_set():
            try:
                conn, _ = self.srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with conn:
                data = b''
                conn.settimeout(2)
                while not data.endswith(b'\n'):
                    chunk = conn.recv(256)
                    if not chunk:
                        break
                    data += chunk
                if data:
                    with self.lock:
                        self.lines.append(data.decode(errors='replace').strip())
                    try:
                        conn.sendall(self.reply.encode())
                    except OSError:
                        pass

    def commands(self):
        with self.lock:
            return list(self.lines)

    def stop(self):
        self.stop_flag.set()
        self.srv.close()
        try:
            os.unlink(self.path)
        except OSError:
            pass


class Monitor:
    """the monitor under test, with a fake /sys, /dev, home, power supply and rfkill.
    the sock/cmd the constructor receives are still exported through the env on purpose:
    the monitor must not use them anymore (wireless AA is LIVI's own now), and the tests
    assert the fake helper stays silent."""

    def __init__(self, root, sock, cmd=LIVI_CMD, active_cmd='true', idle_secs=30):
        self.root = root
        self.env = dict(os.environ)
        self.env.update({
            'LIVI_LINK_SYS': os.path.join(root, 'sys'),
            'LIVI_LINK_DEV': os.path.join(root, 'dev'),
            'LIVI_LINK_HOME': os.path.join(root, 'home'),
            'LIVI_LINK_SOCK': sock,
            'LIVI_LINK_CMD': cmd,
            'LIVI_LINK_ACTIVE_CMD': active_cmd,
            'LIVI_LINK_JOURNAL': '-',
            'LIVI_LINK_IDLE_SECS': str(idle_secs),
            'LIVI_LINK_WAKE_REASSERT': '2',
            'LIVI_LINK_STATE': os.path.join(root, 'state'),
            'LIVI_LINK_POWER_SUPPLY': os.path.join(root, 'power_supply'),
            'LIVI_LINK_RFKILL': os.path.join(root, 'rfkill'),
            'LIVI_LINK_MODPROBE': os.path.join(root, 'modprobe'),
            'LIVI_LINK_SYSTEMCTL': os.path.join(root, 'systemctl'),
        })
        self.proc = None

    def start(self):
        self.proc = subprocess.Popen(
            [MONITOR], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, env=self.env, text=True, bufsize=1)
        return self.proc

    def send(self, line):
        self.proc.stdin.write(line + '\n')
        self.proc.stdin.flush()

    def state(self):
        try:
            with open(self.env['LIVI_LINK_STATE']) as f:
                return f.read().split()[0]
        except OSError:
            return ''

    def wait_state(self, want, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.state() == want:
                return True
            time.sleep(0.1)
        return False

    def set_charging(self, on):
        with open(os.path.join(self.root, 'power_supply', 'online'), 'w') as f:
            f.write('1' if on else '0')

    def rfkill_calls(self):
        try:
            with open(os.path.join(self.root, 'rfkill.log')) as f:
                return [line.strip() for line in f if line.strip()]
        except OSError:
            return []

    def wait_rfkill(self, want, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if want in self.rfkill_calls():
                return True
            time.sleep(0.1)
        return False

    def _calls(self, name):
        try:
            with open(os.path.join(self.root, name)) as f:
                return [line.strip() for line in f if line.strip()]
        except OSError:
            return []

    def modprobe_calls(self):
        return self._calls('modprobe.log')

    def systemctl_calls(self):
        return self._calls('systemctl.log')

    def wait_modprobe(self, want, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if want in self.modprobe_calls():
                return True
            time.sleep(0.1)
        return False

    def wait_systemctl(self, want, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if want in self.systemctl_calls():
                return True
            time.sleep(0.1)
        return False

    def livi_active(self):
        return os.path.exists(os.path.join(self.root, 'livi.active'))

    def wake_lock(self):
        try:
            with open(os.path.join(self.root, 'sys', 'power', 'wake_lock')) as f:
                return f.read()
        except OSError:
            return ''

    def nosuspend(self):
        return os.path.exists(
            os.path.join(self.root, 'home', '.cache', 'sxmo', 'sxmo.nosuspend'))

    def latency_fd(self):
        """True while the monitor holds /dev/cpu_dma_latency open."""
        if self.proc is None:
            return False
        d = f'/proc/{self.proc.pid}/fd'
        try:
            fds = os.listdir(d)
        except OSError:
            return False
        for fd in fds:
            try:
                if 'cpu_dma_latency' in os.readlink(os.path.join(d, fd)):
                    return True
            except OSError:
                pass
        return False

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()


def setup_root(name, charging=False):
    root = os.path.join(TMP, name)
    for d in ('sys/power', 'dev', 'home/.cache/sxmo', 'power_supply'):
        os.makedirs(os.path.join(root, d), exist_ok=True)
    for f in ('sys/power/wake_lock', 'sys/power/wake_unlock', 'dev/cpu_dma_latency'):
        open(os.path.join(root, f), 'w').close()
    with open(os.path.join(root, 'power_supply', 'online'), 'w') as f:
        f.write('1' if charging else '0')
    for tool in ('rfkill', 'modprobe'):
        path = os.path.join(root, tool)
        with open(path, 'w') as f:
            f.write('#!/bin/sh\necho "$@" >> %s/%s.log\n' % (root, tool))
        os.chmod(path, 0o755)
    # systemctl also models livi.service's active state, so the monitor's
    # is-active/start/stop calls behave like the real transactions across a test
    path = os.path.join(root, 'systemctl')
    with open(path, 'w') as f:
        f.write('''#!/bin/sh
root=%s
echo "$@" >> "$root/systemctl.log"
verb=$1
[ $# -ge 1 ] && shift
case "$verb" in
    is-active)
        [ "$1" = "--quiet" ] && shift
        case "$1" in
            livi.service) [ -e "$root/livi.active" ] && exit 0 || exit 3 ;;
            *) exit 3 ;;
        esac
        ;;
    start)
        case "$1" in livi.service) : > "$root/livi.active" ;; esac
        ;;
    stop)
        case "$1" in livi.service) rm -f "$root/livi.active" ;; esac
        ;;
esac
exit 0
''' % root)
    os.chmod(path, 0o755)
    return root


def test_livi_cmd():
    print('livi-cmd')
    sock = os.path.join(TMP, 'cmd-ok.sock')
    h = FakeHelper(sock)
    h.start()
    r = subprocess.run([LIVI_CMD, sock, 'set-aa 1'], capture_output=True, text=True,
                       timeout=5)
    check('sends the command line', h.commands() == ['set-aa 1'], h.commands())
    check('ok:true is exit 0', r.returncode == 0 and 'true' in r.stdout,
          (r.returncode, r.stdout, r.stderr))
    h.stop()

    sock = os.path.join(TMP, 'cmd-false.sock')
    h = FakeHelper(sock, ok=False)
    h.start()
    r = subprocess.run([LIVI_CMD, sock, 'set-aa 0'], capture_output=True, text=True,
                       timeout=5)
    check('ok:false is exit 1', r.returncode == 1, (r.returncode, r.stdout))
    h.stop()

    r = subprocess.run([LIVI_CMD, os.path.join(TMP, 'cmd-missing.sock'), 'set-aa 1'],
                       capture_output=True, text=True, timeout=5)
    check('missing socket is exit 1', r.returncode == 1, r.returncode)
    r = subprocess.run([LIVI_CMD], capture_output=True, text=True, timeout=5)
    check('usage error is exit 2', r.returncode == 2, r.returncode)


def test_monitor():
    print('livi-link-monitor')
    root = setup_root('monitor', charging=False)
    sock = os.path.join(root, 'cp-bt.sock')
    helper = FakeHelper(sock)
    helper.start()
    m = Monitor(root, sock)
    m.start()
    try:
        check('starts unplugged', m.wait_state('unplugged'), m.state())
        check('unplugged blocks the radios', m.wait_rfkill('block all'),
              m.rfkill_calls())
        check('unplugged unloads the radio drivers',
              m.wait_modprobe('-r btqcomsmd wcn36xx'), m.modprobe_calls())
        check('unplugged stops bluetooth', 'stop bluetooth' in m.systemctl_calls(),
              m.systemctl_calls())
        check('unplugged leaves livi stopped',
              'start livi.service' not in m.systemctl_calls(), m.systemctl_calls())
        check('unplugged livi not active', not m.livi_active())
        check('wake lock held', 'livi' in m.wake_lock(), m.wake_lock())
        check('sxmo nosuspend flag held', m.nosuspend())
        check('no latency hold while unplugged', not m.latency_fd())
        check('helper socket untouched while unplugged', helper.commands() == [],
              helper.commands())

        m.set_charging(True)
        check('charger -> idle', m.wait_state('idle'), m.state())
        check('charger unblocks the radios', m.wait_rfkill('unblock all'),
              m.rfkill_calls())
        check('charger reloads the radio drivers',
              m.wait_modprobe('btqcomsmd wcn36xx'), m.modprobe_calls())
        check('charger starts bluetooth', 'start bluetooth' in m.systemctl_calls(),
              m.systemctl_calls())
        check('charger confirmed -> livi started',
              m.wait_systemctl('start livi.service'), m.systemctl_calls())
        check('livi active after the charger', m.livi_active())
        check('idle still has no latency hold', not m.latency_fd())

        m.send('rawstream: reader ready at seq 7')
        check('reader ready -> connected', m.wait_state('connected'), m.state())
        check('connected holds the latency fd', m.latency_fd())

        m.send('rawstream: 10 frames, 100 kb, 0 dropped, 1 in flight, 2.0 fps, '
               '100 kbit/s, latency 8.0/20.0 ms avg/max')
        time.sleep(1.5)
        check('acking stats keep it connected', m.state() == 'connected', m.state())

        time.sleep(4.0)
        check('no set-aa while connected', helper.commands() == [], helper.commands())
        check('stays connected holds the latency fd', m.latency_fd())

        m.send('usbgadget: unit touch 100,200 down')
        time.sleep(1.0)
        check('LI input keeps it connected', m.state() == 'connected', m.state())

        m.send('rawstream: no acks for 5 s, resending mode and resetting the window')
        check('reader-silent watchdog -> idle', m.wait_state('idle'), m.state())
        check('idle releases the latency fd', not m.latency_fd())
        check('no set-aa while idle', helper.commands() == [], helper.commands())

        m.send('rawstream: reader ready at seq 8')
        m.wait_state('connected')
        m.send('rawstream: 1 frames, 1 kb, 0 dropped, 0 in flight, 1.0 fps, 1 kbit/s, '
               'latency 0.0/0.0 ms avg/max')
        time.sleep(1.5)
        check('zero-latency stats do not flap the state', m.state() == 'connected',
              m.state())
        check('zero-latency stats keep the latency fd', m.latency_fd())

        m.set_charging(False)
        check('unplug while connected -> unplugged', m.wait_state('unplugged'),
              m.state())
        check('unplug releases the latency fd', not m.latency_fd())
        check('unplug stops livi', m.wait_systemctl('stop livi.service'),
              m.systemctl_calls())
        check('livi stopped after the unplug', not m.livi_active())
        check('unplug blocks the radios again',
              m.rfkill_calls().count('block all') >= 2, m.rfkill_calls())
        check('unplug unloads the drivers again',
              m.modprobe_calls().count('-r btqcomsmd wcn36xx') >= 2,
              m.modprobe_calls())

        m.set_charging(True)
        m.wait_state('connected')
        check('charger again -> livi started again',
              m.systemctl_calls().count('start livi.service') >= 2,
              m.systemctl_calls())
        m.send('usbgadget: function unbound, will rebind')
        check('function unbound -> idle', m.wait_state('idle'), m.state())
        check('function unbound releases the latency fd', not m.latency_fd())

        m.send('rawstream: reader ready at seq 9')
        m.wait_state('connected')
        m.send('usbgadget: bound to 7000000.usb, listening on /tmp/livi-raw.sock')
        check('rawlink restart -> idle', m.wait_state('idle'), m.state())

        m.send('rawstream: reader ready at seq 10')
        m.wait_state('connected')
        m.send('rawlink.service: Main process exited, code=exited, status=1/FAILURE')
        check('rawlink crash -> idle', m.wait_state('idle'), m.state())

        m.send('rawstream: reader ready at seq 11')
        m.wait_state('connected')
        m.proc.stdin.close()
        try:
            m.proc.wait(timeout=5)
            exited = True
        except subprocess.TimeoutExpired:
            exited = False
        check('exits when the journal stream ends', exited, 'still running')
        check('leaves the state unplugged', m.state() == 'unplugged', m.state())
        check('helper socket never touched', helper.commands() == [], helper.commands())
    finally:
        m.stop()
        helper.stop()


def test_monitor_rawlink_gone():
    print('livi-link-monitor, rawlink gone')
    root = setup_root('monitor-gone', charging=True)
    sock = os.path.join(root, 'cp-bt.sock')
    helper = FakeHelper(sock)
    helper.start()
    # rawlink inactive and silent: the monitor must stop believing "connected"
    m = Monitor(root, sock, active_cmd='false', idle_secs=3)
    m.start()
    try:
        m.send('rawstream: reader ready at seq 1')
        check('connected first', m.wait_state('connected'), m.state())
        check('idle when rawlink is gone', m.wait_state('idle', timeout=8), m.state())
        check('rawlink gone releases the latency fd', not m.latency_fd())
    finally:
        m.stop()
        helper.stop()


def test_monitor_livi_behind_it():
    print('livi-link-monitor, livi started behind it')
    root = setup_root('monitor-livi-race', charging=False)
    # the multi-user boot job (or a manual start) can bring livi up right after the
    # monitor's first unplugged tick; the next tick must still take it down
    open(os.path.join(root, 'livi.active'), 'w').close()
    sock = os.path.join(root, 'cp-bt.sock')
    helper = FakeHelper(sock)
    helper.start()
    m = Monitor(root, sock)
    m.start()
    try:
        check('unplugged stops a livi that appeared behind the monitor',
              m.wait_systemctl('stop livi.service'), m.systemctl_calls())
        check('livi stays stopped', not m.livi_active())
        check('no livi start while unplugged',
              'start livi.service' not in m.systemctl_calls(), m.systemctl_calls())
    finally:
        m.stop()
        helper.stop()


def test_monitor_radios_unconfirmed():
    print('livi-link-monitor, radios not confirmed')
    root = setup_root('monitor-noradios', charging=True)
    # the wcn36xx reload failed: the charger is on but the radios are not back, so
    # livi must stay down (the half-up radio stack would only confuse its helper)
    with open(os.path.join(root, 'modprobe'), 'w') as f:
        f.write('#!/bin/sh\necho "$@" >> %s/modprobe.log\nexit 1\n' % root)
    os.chmod(os.path.join(root, 'modprobe'), 0o755)
    sock = os.path.join(root, 'cp-bt.sock')
    helper = FakeHelper(sock)
    helper.start()
    m = Monitor(root, sock)
    m.start()
    try:
        check('charging but unconfirmed is still idle', m.wait_state('idle'), m.state())
        check('rfkill unblock still attempted', m.wait_rfkill('unblock all'),
              m.rfkill_calls())
        check('unconfirmed radios do not start livi',
              'start livi.service' not in m.systemctl_calls(), m.systemctl_calls())
        check('livi not active with unconfirmed radios', not m.livi_active())
    finally:
        m.stop()
        helper.stop()


if __name__ == '__main__':
    build_livi_cmd()
    test_livi_cmd()
    test_monitor()
    test_monitor_rawlink_gone()
    test_monitor_livi_behind_it()
    test_monitor_radios_unconfirmed()
    print(f'{len(PASS)} passed, {len(FAIL)} failed')
    sys.exit(1 if FAIL else 0)
